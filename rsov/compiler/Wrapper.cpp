/*
 * Copyright 2017, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "Wrapper.h"

#include "Builtin.h"
#include "GlobalAllocSPIRITPass.h"
#include "RSAllocationUtils.h"
#include "bcinfo/MetadataExtractor.h"
#include "builder.h"
#include "instructions.h"
#include "module.h"
#include "word_stream.h"
#include "llvm/IR/Module.h"

using bcinfo::MetadataExtractor;
namespace android {
namespace spirit {

// Metadata buffer for global allocations
// struct metadata {
//  uint32_t element_size;
//  uint32_t x_size;
//  uint32_t y_size;
//  uint32_t ??
// };
VariableInst *AddGAMetadata(/*Instruction *elementType, uint32_t binding, */ Builder &b,
                        Module *m) {
  TypeIntInst *UInt32Ty = m->getUnsignedIntType(32);
  std::vector<Instruction *> metadata{
    UInt32Ty,
    UInt32Ty,
    UInt32Ty,
    UInt32Ty
  };
  auto MetadataStructTy = m->getStructType(metadata.data(), metadata.size());
  // FIXME: workaround on a weird OpAccessChain member offset problem. Somehow
  // when given constant indices, OpAccessChain returns pointers that are 4 bytes
  // less than what are supposed to be (at runtime).
  // For now workaround this with +4 the member offsets.
  MetadataStructTy->memberDecorate(0, Decoration::Offset)->addExtraOperand(4);
  MetadataStructTy->memberDecorate(1, Decoration::Offset)->addExtraOperand(8);
  MetadataStructTy->memberDecorate(2, Decoration::Offset)->addExtraOperand(12);
  MetadataStructTy->memberDecorate(3, Decoration::Offset)->addExtraOperand(16);
  // TBD: Implement getArrayType. RuntimeArray requires buffers and hence we
  // cannot use PushConstant underneath
  auto MetadataBufSTy = m->getRuntimeArrayType(MetadataStructTy);
  // Stride of metadata.
  MetadataBufSTy->decorate(Decoration::ArrayStride)->addExtraOperand(
      metadata.size()*sizeof(uint32_t));
  auto MetadataSSBO = m->getStructType(MetadataBufSTy);
  MetadataSSBO->decorate(Decoration::BufferBlock);
  auto MetadataPtrTy = m->getPointerType(StorageClass::Uniform, MetadataSSBO);


  VariableInst *MetadataVar = b.MakeVariable(MetadataPtrTy, StorageClass::Uniform);
  MetadataVar->decorate(Decoration::DescriptorSet)->addExtraOperand(0);
  MetadataVar->decorate(Decoration::Binding)->addExtraOperand(0);
  m->addVariable(MetadataVar);

  return MetadataVar;
}

VariableInst *AddBuffer(Instruction *elementType, uint32_t binding, Builder &b,
                        Module *m) {
  auto ArrTy = m->getRuntimeArrayType(elementType);
  const size_t stride = m->getSize(elementType);
  ArrTy->decorate(Decoration::ArrayStride)->addExtraOperand(stride);
  auto StructTy = m->getStructType(ArrTy);
  StructTy->decorate(Decoration::BufferBlock);
  StructTy->memberDecorate(0, Decoration::Offset)->addExtraOperand(0);

  auto StructPtrTy = m->getPointerType(StorageClass::Uniform, StructTy);

  VariableInst *bufferVar = b.MakeVariable(StructPtrTy, StorageClass::Uniform);
  bufferVar->decorate(Decoration::DescriptorSet)->addExtraOperand(0);
  bufferVar->decorate(Decoration::Binding)->addExtraOperand(binding);
  m->addVariable(bufferVar);

  return bufferVar;
}

bool AddWrapper(const char *name, const uint32_t signature,
                const uint32_t numInput, Builder &b, Module *m) {
  FunctionDefinition *kernel = m->lookupFunctionDefinitionByName(name);
  if (kernel == nullptr) {
    // In the metadata for RenderScript LLVM bitcode, the first foreach kernel
    // is always reserved for the root kernel, even though in the most recent RS
    // apps it does not exist. Simply bypass wrapper generation here, and return
    // true for this case.
    // Otherwise, if a non-root kernel function cannot be found, it is a
    // fatal internal error which is really unexpected.
    return (strncmp(name, "root", 4) == 0);
  }

  // The following three cases are not supported
  if (!MetadataExtractor::hasForEachSignatureKernel(signature)) {
    // Not handling old-style kernel
    return false;
  }

  if (MetadataExtractor::hasForEachSignatureUsrData(signature)) {
    // Not handling the user argument
    return false;
  }

  if (MetadataExtractor::hasForEachSignatureCtxt(signature)) {
    // Not handling the context argument
    return false;
  }

  TypeVoidInst *VoidTy = m->getVoidType();
  TypeFunctionInst *FuncTy = m->getFunctionType(VoidTy, nullptr, 0);
  FunctionDefinition *Func =
      b.MakeFunctionDefinition(VoidTy, FunctionControl::None, FuncTy);
  m->addFunctionDefinition(Func);

  Block *Blk = b.MakeBlock();
  Func->addBlock(Blk);

  Blk->addInstruction(b.MakeLabel());

  TypeIntInst *UIntTy = m->getUnsignedIntType(32);

  Instruction *XValue = nullptr;
  Instruction *YValue = nullptr;
  Instruction *ZValue = nullptr;
  Instruction *Index = nullptr;
  VariableInst *InvocationId = nullptr;
  VariableInst *NumWorkgroups = nullptr;

  if (MetadataExtractor::hasForEachSignatureIn(signature) ||
      MetadataExtractor::hasForEachSignatureOut(signature) ||
      MetadataExtractor::hasForEachSignatureX(signature) ||
      MetadataExtractor::hasForEachSignatureY(signature) ||
      MetadataExtractor::hasForEachSignatureZ(signature)) {
    TypeVectorInst *V3UIntTy = m->getVectorType(UIntTy, 3);
    InvocationId = m->getInvocationId();
    auto IID = b.MakeLoad(V3UIntTy, InvocationId);
    Blk->addInstruction(IID);

    XValue = b.MakeCompositeExtract(UIntTy, IID, {0});
    Blk->addInstruction(XValue);

    YValue = b.MakeCompositeExtract(UIntTy, IID, {1});
    Blk->addInstruction(YValue);

    ZValue = b.MakeCompositeExtract(UIntTy, IID, {2});
    Blk->addInstruction(ZValue);

    // TODO: Use SpecConstant for workgroup size
    auto ConstOne = m->getConstant(UIntTy, 1U);
    auto GroupSize =
        m->getConstantComposite(V3UIntTy, ConstOne, ConstOne, ConstOne);

    auto GroupSizeX = b.MakeCompositeExtract(UIntTy, GroupSize, {0});
    Blk->addInstruction(GroupSizeX);

    auto GroupSizeY = b.MakeCompositeExtract(UIntTy, GroupSize, {1});
    Blk->addInstruction(GroupSizeY);

    NumWorkgroups = m->getNumWorkgroups();
    auto NumGroup = b.MakeLoad(V3UIntTy, NumWorkgroups);
    Blk->addInstruction(NumGroup);

    auto NumGroupX = b.MakeCompositeExtract(UIntTy, NumGroup, {0});
    Blk->addInstruction(NumGroupX);

    auto NumGroupY = b.MakeCompositeExtract(UIntTy, NumGroup, {1});
    Blk->addInstruction(NumGroupY);

    auto GlobalSizeX = b.MakeIMul(UIntTy, GroupSizeX, NumGroupX);
    Blk->addInstruction(GlobalSizeX);

    auto GlobalSizeY = b.MakeIMul(UIntTy, GroupSizeY, NumGroupY);
    Blk->addInstruction(GlobalSizeY);

    auto RowsAlongZ = b.MakeIMul(UIntTy, GlobalSizeY, ZValue);
    Blk->addInstruction(RowsAlongZ);

    auto NumRows = b.MakeIAdd(UIntTy, YValue, RowsAlongZ);
    Blk->addInstruction(NumRows);

    auto NumCellsFromYZ = b.MakeIMul(UIntTy, GlobalSizeX, NumRows);
    Blk->addInstruction(NumCellsFromYZ);

    Index = b.MakeIAdd(UIntTy, NumCellsFromYZ, XValue);
    Blk->addInstruction(Index);
  }

  std::vector<IdRef> inputs;

  ConstantInst *ConstZero = m->getConstant(UIntTy, 0);

  for (uint32_t i = 0; i < numInput; i++) {
    FunctionParameterInst *param = kernel->getParameter(i);
    Instruction *elementType = param->mResultType.mInstruction;
    VariableInst *inputBuffer = AddBuffer(elementType, i + 2, b, m);

    TypePointerInst *PtrTy =
        m->getPointerType(StorageClass::Function, elementType);
    AccessChainInst *Ptr =
        b.MakeAccessChain(PtrTy, inputBuffer, {ConstZero, Index});
    Blk->addInstruction(Ptr);

    Instruction *input = b.MakeLoad(elementType, Ptr);
    Blk->addInstruction(input);

    inputs.push_back(IdRef(input));
  }

  // TODO: Convert from unsigned int to signed int if that is what the kernel
  // function takes for the coordinate parameters
  if (MetadataExtractor::hasForEachSignatureX(signature)) {
    inputs.push_back(XValue);
    if (MetadataExtractor::hasForEachSignatureY(signature)) {
      inputs.push_back(YValue);
      if (MetadataExtractor::hasForEachSignatureZ(signature)) {
        inputs.push_back(ZValue);
      }
    }
  }

  auto resultType = kernel->getReturnType();
  auto kernelCall =
      b.MakeFunctionCall(resultType, kernel->getInstruction(), inputs);
  Blk->addInstruction(kernelCall);

  if (MetadataExtractor::hasForEachSignatureOut(signature)) {
    VariableInst *OutputBuffer = AddBuffer(resultType, 1, b, m);
    auto resultPtrType = m->getPointerType(StorageClass::Function, resultType);
    AccessChainInst *OutPtr =
        b.MakeAccessChain(resultPtrType, OutputBuffer, {ConstZero, Index});
    Blk->addInstruction(OutPtr);
    Blk->addInstruction(b.MakeStore(OutPtr, kernelCall));
  }

  Blk->addInstruction(b.MakeReturn());

  std::string wrapperName("entry_");
  wrapperName.append(name);

  EntryPointDefinition *entry = b.MakeEntryPointDefinition(
      ExecutionModel::GLCompute, Func, wrapperName.c_str());

  entry->setLocalSize(1, 1, 1);

  if (Index != nullptr) {
    entry->addToInterface(InvocationId);
    entry->addToInterface(NumWorkgroups);
  }

  m->addEntryPoint(entry);

  return true;
}

bool DecorateGlobalBuffer(llvm::Module &LM, Builder &b, Module *m) {
  Instruction *inst = m->lookupByName("__GPUBlock");
  if (inst == nullptr) {
    return true;
  }

  VariableInst *bufferVar = static_cast<VariableInst *>(inst);
  bufferVar->decorate(Decoration::DescriptorSet)->addExtraOperand(0);
  bufferVar->decorate(Decoration::Binding)->addExtraOperand(0);

  TypePointerInst *StructPtrTy =
      static_cast<TypePointerInst *>(bufferVar->mResultType.mInstruction);
  TypeStructInst *StructTy =
      static_cast<TypeStructInst *>(StructPtrTy->mOperand2.mInstruction);
  StructTy->decorate(Decoration::BufferBlock);

  // Decorate each member with proper offsets

  const auto GlobalsB = LM.globals().begin();
  const auto GlobalsE = LM.globals().end();
  const auto Found =
      std::find_if(GlobalsB, GlobalsE, [](const llvm::GlobalVariable &GV) {
        return GV.getName() == "__GPUBlock";
      });

  if (Found == GlobalsE) {
    return true; // GPUBlock not found - not an error by itself.
  }

  const llvm::GlobalVariable &G = *Found;

  bool IsCorrectTy = false;
  if (const auto *LPtrTy = llvm::dyn_cast<llvm::PointerType>(G.getType())) {
    if (auto *LStructTy =
            llvm::dyn_cast<llvm::StructType>(LPtrTy->getElementType())) {
      IsCorrectTy = true;

      const auto &DLayout = LM.getDataLayout();
      const auto *SLayout = DLayout.getStructLayout(LStructTy);
      assert(SLayout);
      if (SLayout == nullptr) {
        std::cerr << "struct layout is null" << std::endl;
        return false;
      }

      for (uint32_t i = 0, e = LStructTy->getNumElements(); i != e; ++i) {
        auto decor = StructTy->memberDecorate(i, Decoration::Offset);
        if (!decor) {
          std::cerr << "failed creating member decoration for field " << i
                    << std::endl;
          return false;
        }
        decor->addExtraOperand((uint32_t)SLayout->getElementOffset(i));
      }
    }
  }

  if (!IsCorrectTy) {
    return false;
  }

  llvm::SmallVector<rs2spirv::RSAllocationInfo, 2> RSAllocs;
  if (!getRSAllocationInfo(LM, RSAllocs)) {
    // llvm::errs() << "Extracting rs_allocation info failed\n";
    return true;
  }

  // TODO: clean up the binding number assignment
  size_t BindingNum = 3;
  for (const auto &A : RSAllocs) {
    Instruction *inst = m->lookupByName(A.VarName.c_str());
    if (inst == nullptr) {
      return false;
    }
    VariableInst *bufferVar = static_cast<VariableInst *>(inst);
    bufferVar->decorate(Decoration::DescriptorSet)->addExtraOperand(0);
    bufferVar->decorate(Decoration::Binding)->addExtraOperand(BindingNum++);
  }

  return true;
}

void AddHeader(Module *m) {
  m->addCapability(Capability::Shader);
  // TODO: avoid duplicated capability
  // m->addCapability(Capability::Addresses);
  m->setMemoryModel(AddressingModel::Physical32, MemoryModel::GLSL450);

  m->addExtInstImport("GLSL.std.450");

  m->addSource(SourceLanguage::GLSL, 450);
  m->addSourceExtension("GL_ARB_separate_shader_objects");
  m->addSourceExtension("GL_ARB_shading_language_420pack");
  m->addSourceExtension("GL_GOOGLE_cpp_style_line_directive");
  m->addSourceExtension("GL_GOOGLE_include_directive");
}

namespace {

class StorageClassVisitor : public DoNothingVisitor {
public:
  void visit(TypePointerInst *inst) override {
    matchAndReplace(inst->mOperand1);
  }

  void visit(TypeForwardPointerInst *inst) override {
    matchAndReplace(inst->mOperand2);
  }

  void visit(VariableInst *inst) override { matchAndReplace(inst->mOperand1); }

private:
  void matchAndReplace(StorageClass &storage) {
    if (storage == StorageClass::Function) {
      storage = StorageClass::Uniform;
    }
  }
};

void FixGlobalStorageClass(Module *m) {
  StorageClassVisitor v;
  m->getGlobalSection()->accept(&v);
}

} // anonymous namespace

} // namespace spirit
} // namespace android

using android::spirit::AddHeader;
using android::spirit::AddWrapper;
using android::spirit::DecorateGlobalBuffer;
using android::spirit::InputWordStream;
using android::spirit::FixGlobalStorageClass;

namespace rs2spirv {

std::vector<uint32_t>
AddGLComputeWrappers(const std::vector<uint32_t> &kernel_spirv,
                     const bcinfo::MetadataExtractor &metadata,
                     llvm::Module &LM, int *error) {
  std::unique_ptr<InputWordStream> IS(
      InputWordStream::Create(std::move(kernel_spirv)));
  std::unique_ptr<android::spirit::Module> m(
      android::spirit::Deserialize<android::spirit::Module>(*IS));

  if (!m) {
    *error = -1;
    return std::vector<uint32_t>();
  }

  if (!m->resolveIds()) {
    *error = -2;
    return std::vector<uint32_t>();
  }

  android::spirit::Builder b;

  m->setBuilder(&b);

  FixGlobalStorageClass(m.get());

  AddHeader(m.get());

  DecorateGlobalBuffer(LM, b, m.get());

  const size_t numKernel = metadata.getExportForEachSignatureCount();
  const char **kernelName = metadata.getExportForEachNameList();
  const uint32_t *kernelSigature = metadata.getExportForEachSignatureList();
  const uint32_t *inputCount = metadata.getExportForEachInputCountList();

  for (size_t i = 0; i < numKernel; i++) {
    bool success =
        AddWrapper(kernelName[i], kernelSigature[i], inputCount[i], b, m.get());
    if (!success) {
      *error = -3;
      return std::vector<uint32_t>();
    }
  }

  m->consolidateAnnotations();
  auto words = rs2spirv::TranslateBuiltins(b, m.get(), error);

  // Recreate a module in known state after TranslateBuiltins
  std::unique_ptr<InputWordStream> IS1(
      InputWordStream::Create(std::move(words)));
  std::unique_ptr<android::spirit::Module> m1(
      android::spirit::Deserialize<android::spirit::Module>(*IS1));

  if (!m1) {
    *error = -1;
    return std::vector<uint32_t>();
  }

  if (!m1->resolveIds()) {
    *error = -2;
    return std::vector<uint32_t>();
  }

  // Builders can be reused
  m1->setBuilder(&b);

  // Create types and variable declarations for global allocation metadata
  android::spirit::VariableInst *GAmetadata = AddGAMetadata(b, m1.get());

  // Adding types on-the-fly inside a transformer is not well suported now;
  // creating them here before we enter transformer to avoid problems.
  // TODO: Fix the transformer
  android::spirit::TypeIntInst *UInt32Ty = m1->getUnsignedIntType(32);
  m1->getConstant(UInt32Ty, 0U);
  m1->getConstant(UInt32Ty, 1U);
  // TODO: Use constant memory for metadata
  m1->getPointerType(android::spirit::StorageClass::Uniform,
                     UInt32Ty);

  // Transform calls to lowered allocation accessors to use metadata
  // TODO: implement the lowering pass in LLVM
  m1->consolidateAnnotations();
  return rs2spirv::TranslateGAAccessors(b, m1.get(), GAmetadata, error);

}

} // namespace rs2spirv
