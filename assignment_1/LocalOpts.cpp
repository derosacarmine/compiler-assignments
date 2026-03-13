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
#include <functional>
#include <map>
#include <set>
#include <vector>
#include <utility>

using namespace llvm;
using namespace std;

// boolean fun
using Predicate = function<bool(const ConstantInt*)>;
using Builder = function<pair<Instruction*, Instruction*>(Value*, ConstantInt*)>;

namespace {

  /* struct for common methods */
struct Common {
    virtual bool runOnBasicBlock(BasicBlock &B) = 0;
    
    PreservedAnalyses run(Function &F, FunctionAnalysisManager &) {

  	runOnFunction(F);

  	return PreservedAnalyses::all();
}

  /*
  for each basic block of the fz it calls runOnBasickBlock
  */
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


struct AlgebraicIdentity: PassInfoMixin<AlgebraicIdentity>, Common {

// map used to simplify identities which depends on a single operand (the constant value) and can be replaced by other operand
map<unsigned, Predicate> singleOperandMap = {
    {Instruction::Add, [](const ConstantInt* c) -> bool { return c->isZero(); }},
    {Instruction::Sub, [](const ConstantInt* c) -> bool { return c->isZero(); }},
    {Instruction::AShr, [](const ConstantInt* c) -> bool { return c->isZero(); }}, // Arithmetic right shifts fills with 1s if the number is negative or 0s if positive
    {Instruction::LShr, [](const ConstantInt* c) -> bool { return c->isZero();}}, // Logical right shifts fill vacated positions with 0s
    {Instruction::Shl, [](const ConstantInt* c) -> bool { return c->isZero();}},
    {Instruction::Mul, [](const ConstantInt* c) -> bool { return c->isOne();}},
    {Instruction::SDiv, [](const ConstantInt* c) -> bool { return c->isOne();}},
    {Instruction::And, [](const ConstantInt* c) -> bool { return c->isMinusOne();}},
    {Instruction::Or, [](const ConstantInt* c) -> bool { return c->isZero();}},
    {Instruction::Xor, [](const ConstantInt* c) -> bool { return c->isZero();}}
};

// map used to simplify identities which depends on both operands or that can be replaced by a costant (or both)
map<unsigned, function<Value*(Value*, Value*)>> pairOperandMap = {
    {Instruction::Sub, [](Value* op1, Value* op2) -> Value* { if(op1 == op2) return ConstantInt::get(op1->getType(), 0); else return nullptr;}},
    {Instruction::SDiv, [](Value* op1, Value* op2) -> Value* { if(op1 == op2) return ConstantInt::get(op1->getType(), 1); else return nullptr; }},
    {Instruction::And, [](Value* op1, Value* op2) -> Value* { if(op1 == op2) return op1; else return nullptr; }},
    {Instruction::Or, [](Value* op1, Value* op2) -> Value* { if(op1 == op2) return op1; else return nullptr; }},
    {Instruction::Xor, [](Value* op1, Value* op2) -> Value* { if(op1 == op2) return ConstantInt::get(op1->getType(), 0); else return nullptr; }},
    {Instruction::URem, [](Value* op1, Value* op2) -> Value* { 
        if(op1 == op2) return ConstantInt::get(op1->getType(), 0); 
        if(auto c2 = dyn_cast<ConstantInt>(op2)) if(c2->isOne()) return ConstantInt::get(op1->getType(), 0);
        return nullptr;
    }},
    {Instruction::SRem, [](Value* op1, Value* op2) -> Value* { 
        if(op1 == op2) return ConstantInt::get(op1->getType(), 0); 
        if(auto c2 = dyn_cast<ConstantInt>(op2)) if(c2->isOne()) return ConstantInt::get(op1->getType(), 0);
        return nullptr;}},   
    {Instruction::Mul, [](Value* op1, Value* op2) -> Value* { 
    if(auto c2 = dyn_cast<ConstantInt>(op2)) if(c2->isZero()) return ConstantInt::get(op1->getType(), 0);
    if(auto c1 = dyn_cast<ConstantInt>(op1)) if(c1->isZero()) return ConstantInt::get(op2->getType(), 0);
    return nullptr;}}
};


/*
It takes a basic block, evaluates for each instruction whether it's adding or multiplying,
and checks whether the two operands are constant or variable.
If they're constant, it checks whether it's zero (in the case of adding) or
1 (in the case of multiplying) and replaces the result of the operation
with the variable operand.
 */
bool runOnBasicBlock(BasicBlock &B) override {
  
  for (auto instr_it = B.begin(); instr_it != B.end();) {
    Instruction& instr = *instr_it;
    instr_it++;
  
    if (instr.getNumOperands() != 2) continue;

    int opCode = instr.getOpcode();
    Value* op1 = instr.getOperand(0);
    Value* op2 = instr.getOperand(1);

    auto pairIt = pairOperandMap.find(opCode);
    if (pairIt != pairOperandMap.end()) {
      if(auto replacement = pairIt->second(op1, op2)){
        instr.replaceAllUsesWith(replacement);
        instr.eraseFromParent();
        continue;
      }
    }

    auto singleIt = singleOperandMap.find(opCode);
    if (singleIt == singleOperandMap.end()) continue;

    vector<int> usableOperands;

    if(opCode == Instruction::Add || opCode == Instruction::Mul || opCode == Instruction::Or || opCode == Instruction::And || 
                opCode == Instruction::Xor)
      usableOperands = {0,1};
    else if (opCode == Instruction::Sub || opCode == Instruction::SDiv || opCode == Instruction::AShr || opCode == Instruction::LShr ||
                opCode == Instruction::Shl)
      usableOperands = {1};

    for (int i : usableOperands) {
        if (auto* c = dyn_cast<ConstantInt>(instr.getOperand(i))) {
            if (singleIt->second(c)) {
                instr.replaceAllUsesWith(instr.getOperand(1 - i));
                instr.eraseFromParent();
                break;
            }
        }
      }  
  }
  
  return true;
}

};



//STRENGTH REDUCTION
struct StrengthReduction: PassInfoMixin<StrengthReduction>, Common {

// maps a check function for SR (Predicate) to a function 
// that receive returns a pair of instructions (Builder) 
// that replace the original "mul" instruction
/* vector<pair<predicate, builder>> */
vector<pair<Predicate, Builder>> mulReductions = {
    {
        [](const ConstantInt* c) -> bool { return c->getValue().isPowerOf2(); },
        [](Value* usableOperands, ConstantInt* c) -> std::pair<Instruction*, Instruction*> {
            auto* shl = BinaryOperator::Create(Instruction::Shl, usableOperands,
                ConstantInt::get(c->getType(), c->getValue().logBase2()));
            return {shl, nullptr};
        }
    },
    {
        [](const ConstantInt* c) -> bool { return (c->getValue()+1).isPowerOf2(); },
        [](Value* usableOperands, ConstantInt* c) -> std::pair<Instruction*, Instruction*> {
            auto* shl = BinaryOperator::Create(Instruction::Shl, usableOperands,
                ConstantInt::get(c->getType(), (c->getValue()+1).logBase2()));
            auto* sub = BinaryOperator::Create(Instruction::Sub, shl, usableOperands);
            return {shl, sub};
        }
    },
    {
        [](const ConstantInt* c) -> bool { return (c->getValue()-1).isPowerOf2(); },
        [](Value* usableOperands, ConstantInt* c) -> std::pair<Instruction*, Instruction*> {
            auto* shl = BinaryOperator::Create(Instruction::Shl, usableOperands,
                ConstantInt::get(c->getType(), (c->getValue()-1).logBase2()));
            auto* add = BinaryOperator::Create(Instruction::Add, shl, usableOperands);
            return {shl, add};
        }
    },
};


bool runOnBasicBlock(BasicBlock &B) override {
  
  for (auto instr_iter = B.begin(); instr_iter != B.end();)
  {
    Instruction &instr = *instr_iter;
    instr_iter++;

    if (instr.getNumOperands() != 2) continue;
  
    int opCode = instr.getOpcode();

    Value* operand1 = instr.getOperand(0);
    Value* operand2 = instr.getOperand(1);
    ConstantInt* const_value1 = dyn_cast<ConstantInt>(operand1);
    ConstantInt* const_value2 = dyn_cast<ConstantInt>(operand2);
    Instruction* first=nullptr, *second=nullptr; 

    if(opCode == Instruction::SDiv && const_value2 && const_value2->getValue().isPowerOf2()){
      first = BinaryOperator::Create(Instruction::AShr, operand1,
                ConstantInt::get(const_value2->getType(), const_value2->getValue().logBase2()));
    }
    else if(opCode == Instruction::Mul){
      
      ConstantInt* constantVal = const_value1 ? const_value1 : (const_value2 ? const_value2 : nullptr);
      if(constantVal == nullptr) continue;

      Value* variableValue = constantVal == const_value1 ? operand2 : operand1;

      for (auto& [pred, build] : mulReductions) {
        if (!pred(constantVal)) continue;
        
        //auto [first, second] = build(variableValue, constantVal);
        auto newInstructions = build(variableValue, constantVal);
        first = newInstructions.first;
        second = newInstructions.second;
        break;
      }
    }

    if(first == nullptr) continue;

    first->insertAfter(&instr);
    auto* replace = first;
    if (second) { second->insertAfter(first); replace = second; }
    instr.replaceAllUsesWith(replace);
    instr.eraseFromParent();
    
  }
  
  return true;
}

};

struct MultiInstruction : PassInfoMixin<MultiInstruction>, Common{
    bool runOnBasicBlock(BasicBlock &B) override {
      return false;
    }
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
                  else if(Name == "multi-instruction"){
                    FPM.addPass(MultiInstruction());
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
