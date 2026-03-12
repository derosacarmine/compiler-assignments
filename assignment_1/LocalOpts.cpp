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
#include <functional>  // std::function (per Predicate e Builder)
#include <map>         // std::map (per identityMap)
#include <set>
#include <vector>      // std::vector (per mulReductions)
#include <utility>     // std::pair (per la coppia Instruction*, Instruction*)

using namespace llvm;
using namespace std;

using Predicate = function<bool(const ConstantInt*)>; // boolean function
using Identity = map<unsigned, Predicate>; //maps an opcode to the needed boolean function to check for Algebraic identity
using Builder = function<
    pair<Instruction*, Instruction*>(Value*, ConstantInt*)>; //function that returns a pair of instructions



namespace {

struct AlgebraicIdentity: PassInfoMixin<AlgebraicIdentity> {

/*
AlgebraicIdentity --> map<opcode, predicate>
 */

Identity identityMap = {
    {Instruction::Add, [](const ConstantInt* c) -> bool { return c->isZero(); }},
    {Instruction::Mul, [](const ConstantInt* c) -> bool { return c->isOne();  }},
    {Instruction::Sub, [](const ConstantInt* c) -> bool { return c->isZero(); }},
    {Instruction::SDiv, [](const ConstantInt* c) -> bool { return c->isOne();  }},
};


PreservedAnalyses run(Function &F, FunctionAnalysisManager &) {

  	runOnFunction(F);

  	return PreservedAnalyses::all();
}


/*
It takes a basic block, evaluates for each instruction whether it's adding or multiplying,
and checks whether the two operands are constant or variable.
If they're constant, it checks whether it's zero (in the case of adding) or
1 (in the case of multiplying) and replaces the result of the operation
with the variable operand.
 */
bool runOnBasicBlock(BasicBlock &B) {
  
  for (auto instr_it = B.begin(); instr_it != B.end();) {
    Instruction& instr = *instr_it;
    instr_it++;
  
    auto it = identityMap.find(instr.getOpcode());
    if (it == identityMap.end()) continue;

    // Value* operand1 = instr.getOperand(0);
    // Value* operand2 = instr.getOperand(1);
    int opCode = it->first;
    set<int> var;

    if(opCode == Instruction::Add || opCode == Instruction::Mul)
      var = {0,1};
    else if (opCode == Instruction::Sub || opCode == Instruction::SDiv)
      var = {1};

    for (int i : var) {  //0 add e 1 Mul
        if (auto* c = dyn_cast<ConstantInt>(instr.getOperand(i))) {
            if (it->second(c)) {
                instr.replaceAllUsesWith(instr.getOperand(1 - i));
                instr.eraseFromParent();
                break;
            }
        }
      }
    
}
  
  return true;
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



//STRENGTH REDUCTION
struct StrengthReduction: PassInfoMixin<StrengthReduction> {

// maps a check function for SR (Predicate) to a function 
// that receive returns a pair of instructions (Builder) 
// that replace the original "mul" instruction
/* vector<pair<predicate, builder>> */
vector<pair<Predicate, Builder>> mulReductions = {
    {
        [](const ConstantInt* c) -> bool { return c->getValue().isPowerOf2(); },
        [](Value* var, ConstantInt* c) -> std::pair<Instruction*, Instruction*> {
            auto* shl = BinaryOperator::Create(Instruction::Shl, var,
                ConstantInt::get(c->getType(), c->getValue().logBase2()));
            return {shl, nullptr};
        }
    },
    {
        [](const ConstantInt* c) -> bool { return (c->getValue()+1).isPowerOf2(); },
        [](Value* var, ConstantInt* c) -> std::pair<Instruction*, Instruction*> {
            auto* shl = BinaryOperator::Create(Instruction::Shl, var,
                ConstantInt::get(c->getType(), (c->getValue()+1).logBase2()));
            auto* sub = BinaryOperator::Create(Instruction::Sub, shl, var);
            return {shl, sub};
        }
    },
    {
        [](const ConstantInt* c) -> bool { return (c->getValue()-1).isPowerOf2(); },
        [](Value* var, ConstantInt* c) -> std::pair<Instruction*, Instruction*> {
            auto* shl = BinaryOperator::Create(Instruction::Shl, var,
                ConstantInt::get(c->getType(), (c->getValue()-1).logBase2()));
            auto* add = BinaryOperator::Create(Instruction::Add, shl, var);
            return {shl, add};
        }
    },
};


PreservedAnalyses run(Function &F, FunctionAnalysisManager &) {

  	runOnFunction(F);

  	return PreservedAnalyses::all();
}

bool runOnBasicBlock(BasicBlock &B) {
  
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
