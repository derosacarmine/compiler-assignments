#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"
#include <cstddef>
#include <cstdint>
#include <llvm-19/llvm/IR/Constant.h>
#include <llvm-19/llvm/IR/Constants.h>
#include <llvm-19/llvm/IR/InstrTypes.h>
#include <llvm-19/llvm/IR/Instruction.h>
#include <llvm-19/llvm/IR/Operator.h>
#include <llvm-19/llvm/IR/Value.h>
#include <llvm-19/llvm/Support/Casting.h>

using namespace llvm;

namespace {

struct AlgebraicIdentity: PassInfoMixin<AlgebraicIdentity> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &) {

  	runOnFunction(F);

  	return PreservedAnalyses::all();
}

bool runOnBasicBlock(BasicBlock &B) {
  
  for (auto iter = B.begin(); iter != B.end();)
  {
    Instruction& instr = *iter;
    ++iter; //make sure that it iterates correctly even if we eliminate the instruction

    if (instr.getOpcode() == Instruction::Add) {
      Value* op1 = instr.getOperand(0);
      Value* op2 = instr.getOperand(1);
      Value* id_value = nullptr;

      if (ConstantInt* const_value = dyn_cast<ConstantInt>(op1))
      {
        if(const_value->isZero())
          id_value = op2;
      }
      else if (ConstantInt* const_value = dyn_cast<ConstantInt>(op2))
      {
        if(const_value->isZero())
          id_value = op1;
      }

      if (id_value != nullptr)
      {
        instr.replaceAllUsesWith(id_value);
        instr.eraseFromParent();
      }

    }
    else if (instr.getOpcode() == Instruction::Mul)
    {
      Value* op1 = instr.getOperand(0);
      Value* op2 = instr.getOperand(1);
      Value* id_value = nullptr;

      if (ConstantInt* const_value = dyn_cast<ConstantInt>(op1))
      {
        if(const_value->isOne())
          id_value = op2;
      }
      else if (ConstantInt* const_value = dyn_cast<ConstantInt>(op2))
      {
        if(const_value->isOne())
          id_value = op1;
      }

      if (id_value != nullptr)
      {
        instr.replaceAllUsesWith(id_value);
        instr.eraseFromParent();
      }

    }
  }

  return true;
}

bool runOnFunction(Function &F) {
  bool Transformed = false;

  for (auto Iter = F.begin(); Iter != F.end(); ++Iter) {
    if (runOnBasicBlock(*Iter)) {
      Transformed = true;
    }
  }

  return Transformed;
}
  static bool isRequired() { return true; }
};



//STRENGTH REDUCTION
struct StrengthReduction: PassInfoMixin<StrengthReduction> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &) {

  	runOnFunction(F);

  	return PreservedAnalyses::all();
}

bool runOnBasicBlock(BasicBlock &B) {
  
  for (auto iter = B.begin(); iter != B.end();)
    {
      Instruction& instr = *iter;
      ++iter;
      auto opCode = instr.getOpcode();
      if (opCode == Instruction::Mul){
        Value* first_op = instr.getOperand(0);
        Value* second_op = instr.getOperand(1);
        ConstantInt* const_value = nullptr;
        Value* variable_value = nullptr;

        if ((const_value = dyn_cast<ConstantInt>(first_op)))
        {
          variable_value = second_op;
        }
        else if ((const_value = dyn_cast<ConstantInt>(second_op)))
        {
          variable_value = first_op;
        }

        if (const_value != nullptr)
        {
          Instruction* secondInstr = nullptr;
          Instruction *firstInstr = nullptr;
          uint64_t const_val = const_value->getValue().getSExtValue();
          if (const_val == 0) continue;

          if (const_value->getValue().isPowerOf2()){
            uint64_t n = const_value->getValue().logBase2();
            firstInstr = BinaryOperator::Create(
          Instruction::Shl, variable_value, ConstantInt::get(const_value->getType(), n));
          }
          else if ((const_value->getValue()+1).isPowerOf2()) //next value is a power of 2
          {
            uint64_t n = (const_value->getValue()+1).logBase2();
            firstInstr = BinaryOperator::Create(
          Instruction::Shl, variable_value, ConstantInt::get(const_value->getType(), n));
            secondInstr = BinaryOperator::Create(
          Instruction::Sub, firstInstr, variable_value);
          }
          else if ((const_value->getValue()-1).isPowerOf2()) // previous value is a power of 2
          {
            uint64_t n = (const_value->getValue()-1).logBase2();
            firstInstr = BinaryOperator::Create(
          Instruction::Shl, variable_value, ConstantInt::get(const_value->getType(), n));
            secondInstr = BinaryOperator::Create(
          Instruction::Add, firstInstr, variable_value);
          }
          else continue;
          
          firstInstr->insertAfter(&instr);
          auto replaceInstr = firstInstr;
          if(secondInstr != nullptr){
            secondInstr->insertAfter(firstInstr);
            replaceInstr = secondInstr;
          }
          instr.replaceAllUsesWith(replaceInstr);
          instr.eraseFromParent();
        }
      }
      else if (opCode == Instruction::SDiv){
        auto divisor = dyn_cast<ConstantInt>(instr.getOperand(1));
        if(divisor && divisor->getValue().isPowerOf2())
        {
          auto shiftRInstruction = BinaryOperator::Create(
          Instruction::AShr, instr.getOperand(0), ConstantInt::get(divisor->getType(), divisor->getValue().logBase2()));
          shiftRInstruction->insertAfter(&instr);
          instr.replaceAllUsesWith(shiftRInstruction);
          instr.eraseFromParent();
        }
      }
    }

  return true;
}


bool runOnFunction(Function &F) {
  bool Transformed = false;

  for (auto Iter = F.begin(); Iter != F.end(); ++Iter) {
    if (runOnBasicBlock(*Iter)) {
      Transformed = true;
    }
  }

  return Transformed;
}

  static bool isRequired() { return true; }
};

} 


llvm::PassPluginLibraryInfo getLocalOptsPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "LocalOpts", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &FPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "algebraic-identity") {
                    FPM.addPass(AlgebraicIdentity());
                    return true;
                  }
                  else if (Name == "strength-reduction"){
                    FPM.addPass(StrengthReduction());
                    return true;
                  }
                  return false;
                });
          }};
}

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getLocalOptsPluginInfo();
}
