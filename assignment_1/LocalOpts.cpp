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
#include <vector>
#include <utility>

using namespace llvm;
using namespace std;

namespace {



  /* struct for common methods */
struct Common {
    /*It iterates over each instruction and attempts to replace expensive operations with cheaper equivalents.*/
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

//for constantMap
auto ifZeroReturnV = [](ConstantInt* c, Value* v) -> Value* { return c->isZero() ? v : nullptr;};
auto ifOneReturnV = [](ConstantInt* c, Value* v) -> Value* { return c->isOne() ? v : nullptr;};
auto ifOneReturnZero = [](ConstantInt* c, Value* v) -> Value* { return c->isOne() ? ConstantInt::get(c->getType(), 0) : nullptr;};
auto ifZeroReturnZero = [](ConstantInt* c, Value* v) -> Value* { return c->isZero() ? ConstantInt::get(c->getType(), 0) : nullptr;};
auto ifMinusOneReturnV = [](ConstantInt* c, Value* v) -> Value* { return c->isMinusOne() ? v : nullptr;};

//for variableMap
auto ifOpsEqualReturnZero = [](Value* op1, Value* op2) -> Value* { return (op1 == op2) ? ConstantInt::get(op1->getType(), 0) : nullptr;};
auto ifOpsEqualReturnOne = [](Value* op1, Value* op2) -> Value* { return (op1 == op2) ? ConstantInt::get(op1->getType(), 1) : nullptr;};
auto ifOpsEqualReturnOp1 = [](Value* op1, Value* op2) -> Value* { return (op1 == op2) ? op1 : nullptr;};


/*Returns a lambda that takes the list of functions and tries them in order.*/
auto firstOf = [](vector<function<Value*(ConstantInt*, Value*)>> fns) {
  return [fns](ConstantInt* c, Value* v) -> Value* {
        for (auto& fn : fns)
            if (auto* r = fn(c, v)) return r;
        return nullptr;
    };
};


struct AlgebraicIdentity: PassInfoMixin<AlgebraicIdentity>, Common {

// map used to simplify identities which have a constant
map<unsigned, function<Value*(ConstantInt*, Value*)>> constantMap = {
    {Instruction::Add, ifZeroReturnV},
    {Instruction::Sub, ifZeroReturnV},
    {Instruction::AShr, ifZeroReturnV}, // Arithmetic right shifts fills with 1s if the number is negative or 0s if positive
    {Instruction::LShr, ifZeroReturnV}, // Logical right shifts fill vacated positions with 0s
    {Instruction::Shl, ifZeroReturnV},
    {Instruction::Mul, firstOf({ifOneReturnV, ifZeroReturnZero})},

    {Instruction::SDiv,ifOneReturnV},
    {Instruction::And, firstOf( {ifMinusOneReturnV, ifZeroReturnZero})},

    {Instruction::Or, ifZeroReturnV},
    {Instruction::Xor, ifZeroReturnV},
    {Instruction::URem, ifOneReturnZero},
    {Instruction::SRem, ifOneReturnZero}
};

// map used to simplify identities which have two identical operands
map<unsigned, function<Value*(Value*, Value*)>> variablesMap = {
    {Instruction::Sub, ifOpsEqualReturnZero},
    {Instruction::SDiv, ifOpsEqualReturnOne},
    {Instruction::And, ifOpsEqualReturnOp1},
    {Instruction::Or, ifOpsEqualReturnOp1},
    {Instruction::Xor, ifOpsEqualReturnZero},
    {Instruction::URem, ifOpsEqualReturnZero},
    {Instruction::SRem, ifOpsEqualReturnZero},
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

    auto varIt = variablesMap.find(opCode);
    if (varIt != variablesMap.end()) {
      if(auto replacement = varIt->second(op1, op2)){
        instr.replaceAllUsesWith(replacement);
        instr.eraseFromParent();
        continue;
      }
    }

    auto constIt = constantMap.find(opCode);
    if (constIt == constantMap.end()) continue;

    vector<Value*> usableConstants;

    if(opCode == Instruction::Add || opCode == Instruction::Mul || opCode == Instruction::Or || opCode == Instruction::And || 
                opCode == Instruction::Xor)
      usableConstants = {op1,op2};
    else if (opCode == Instruction::Sub || opCode == Instruction::SDiv || opCode == Instruction::AShr || opCode == Instruction::LShr ||
                opCode == Instruction::Shl || opCode == Instruction::URem || opCode == Instruction::SRem)
      usableConstants = {op2};

    for (Value* operand : usableConstants) {
        Value* variable = op1 == operand ? op2 : op1;
        if (ConstantInt* constant = dyn_cast<ConstantInt>(operand)) {
            if (auto replacement = constIt->second(constant, variable)) {
                instr.replaceAllUsesWith(replacement);
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

struct mulReduction{
  function<bool(const ConstantInt*)> predicate;
  unsigned (*shiftAmount)(const ConstantInt*);
  std::optional<Instruction::BinaryOps> secondOp; // nullopt = solo shift

};


const vector<mulReduction> mulReductions = {
    { [](const ConstantInt* c) { return c->getValue().isPowerOf2(); },
      [](const ConstantInt* c) { return c->getValue().logBase2(); },
      std::nullopt },

    { [](const ConstantInt* c) { return (c->getValue()+1).isPowerOf2(); },  // <--
      [](const ConstantInt* c) { return (c->getValue()+1).logBase2(); },
      Instruction::Sub },

    { [](const ConstantInt* c) { return (c->getValue()-1).isPowerOf2(); },  // <--
      [](const ConstantInt* c) { return (c->getValue()-1).logBase2(); },
      Instruction::Add },
};

// Returns {first, second} or {nullptr, nullptr} if no reduction applies
std::pair<Instruction*, Instruction*> tryMulReduction(Value* var, ConstantInt* c) {
    for (auto& [pred, shift, op] : mulReductions) {
        if (!pred(c)) continue;
        auto* shl = BinaryOperator::Create(Instruction::Shl, var,
                        ConstantInt::get(c->getType(), shift(c)));
        if (!op) return {shl, nullptr};
        return {shl, BinaryOperator::Create(*op, shl, var)};
    }
    return {nullptr, nullptr};
}

bool runOnBasicBlock(BasicBlock &B) override {
    for (auto it = B.begin(); it != B.end();) {
        Instruction& instr = *it++;

        if (instr.getNumOperands() != 2) continue;

        Value*      op1 = instr.getOperand(0);
        Value*      op2 = instr.getOperand(1);
        auto* cst1 = dyn_cast<ConstantInt>(op1);
        auto* cst2 = dyn_cast<ConstantInt>(op2);

        Instruction* first  = nullptr;
        Instruction* second = nullptr;

        switch (instr.getOpcode()) {
            case Instruction::SDiv:
                if (cst2 && cst2->getValue().isPowerOf2())
                    first = BinaryOperator::Create(Instruction::AShr, op1,
                                ConstantInt::get(cst2->getType(), cst2->getValue().logBase2()));
                break;

            case Instruction::Mul: {
                auto* cst = cst1 ? cst1 : cst2;
                if (!cst) continue;
                Value* var = cst == cst1 ? op2 : op1;
                std::tie(first, second) = tryMulReduction(var, cst);
                break;
            }

            default: continue;
        }

        if (!first) continue;

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
