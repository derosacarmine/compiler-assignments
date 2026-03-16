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




struct AlgebraicIdentity: PassInfoMixin<AlgebraicIdentity>, Common {


//for constantMap
function<Value*(ConstantInt* c, Value* v)> ifZeroReturnV = [](ConstantInt* c, Value* v) -> Value* { return c->isZero() ? v : nullptr;};
function<Value*(ConstantInt* c, Value* v)> ifOneReturnV = [](ConstantInt* c, Value* v) -> Value* { return c->isOne() ? v : nullptr;};
function<Value*(ConstantInt* c, Value* v)> ifOneReturnZero = [](ConstantInt* c, Value* v) -> Value* { return c->isOne() ? ConstantInt::get(c->getType(), 0) : nullptr;};
function<Value*(ConstantInt* c, Value* v)> ifZeroReturnZero = [](ConstantInt* c, Value* v) -> Value* { return c->isZero() ? ConstantInt::get(c->getType(), 0) : nullptr;};
function<Value*(ConstantInt* c, Value* v)> ifMinusOneReturnV = [](ConstantInt* c, Value* v) -> Value* { return c->isMinusOne() ? v : nullptr;};

//for variableMap
function<Value*(Value* op1, Value* op2)> ifOpsEqualReturnZero = [](Value* op1, Value* op2) -> Value* { return (op1 == op2) ? ConstantInt::get(op1->getType(), 0) : nullptr;};
function<Value*(Value* op1, Value* op2)> ifOpsEqualReturnOne = [](Value* op1, Value* op2) -> Value* { return (op1 == op2) ? ConstantInt::get(op1->getType(), 1) : nullptr;};
function<Value*(Value* op1, Value* op2)> ifOpsEqualReturnOp1 = [](Value* op1, Value* op2) -> Value* { return (op1 == op2) ? op1 : nullptr;};


/*Returns a function that takes the list of functions and tries them in order.*/
using Fn = function<Value*(ConstantInt*, Value*)>;
    
    static Fn firstOf(vector<Fn> fns) {
        return [fns](ConstantInt* c, Value* v) -> Value* {
            for (auto& fn : fns)
                if (auto* r = fn(c, v)) return r;
            return nullptr;
        };
    }
  
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


struct StrengthReduction: PassInfoMixin<StrengthReduction>, Common {

struct mulReduction{
  function<bool(const ConstantInt*)> predicate;
  function<unsigned(const ConstantInt*)> shiftAmount;
  std::vector<Instruction::BinaryOps> ops;
};


const vector<mulReduction> mulReductions = {
    { [](const ConstantInt* c) { return c->getValue().isPowerOf2(); },
      [](const ConstantInt* c) { return c->getValue().logBase2(); },
      {} },

    { [](const ConstantInt* c) { return (c->getValue()+1).isPowerOf2(); },
      [](const ConstantInt* c) { return (c->getValue()+1).logBase2(); },
      { Instruction::Sub } },

    { [](const ConstantInt* c) { return (c->getValue()-1).isPowerOf2(); },
      [](const ConstantInt* c) { return (c->getValue()-1).logBase2(); },
      { Instruction::Add } },
    
    { [](const ConstantInt* c) { return (c->getValue()+2).isPowerOf2(); },
      [](const ConstantInt* c) { return (c->getValue()+2).logBase2(); },
      { Instruction::Sub, Instruction::Sub } },

    { [](const ConstantInt* c) { return (c->getValue()-2).isPowerOf2(); },
      [](const ConstantInt* c) { return (c->getValue()-2).logBase2(); },
      { Instruction::Add, Instruction::Add } },
};

// Returns a vector of operations, or {} if no reduction applies
std::vector<Instruction*> tryMulReduction(Value* var, ConstantInt* c) {
    // pred -> condition to verify, shift -> shift value for a constant (if pred is true), ops -> extra operations, if needed
    for (auto& [pred, shift, ops] : mulReductions) {
        if (!pred(c)) continue;

        std::vector<Instruction*> results;

        auto* shl = BinaryOperator::Create(Instruction::Shl, var,
                        ConstantInt::get(c->getType(), shift(c)));
        results.push_back(shl);
        Value* lastValue = shl;

        for(auto opCode : ops) {
          auto* next = BinaryOperator::Create(opCode, lastValue, var);
          results.push_back(next);
          lastValue = next;
        }
        return results;
    }

    //check if we can use two shifts
    const APInt& val = c->getValue();
    unsigned width = val.getBitWidth();

    uint64_t z = val.getZExtValue();
    //floor
    unsigned logLow = val.logBase2();
    //ceiling
    unsigned logHigh = logLow + 1;

    uint64_t pLow = 1ULL << logLow;
    uint64_t pHigh = 1ULL << logHigh;

    //difference from floor value
    uint64_t distLow = z - pLow;
    //difference from ceiling value
    uint64_t distHigh = pHigh - z;

    unsigned mainLog = 0;
    unsigned distLog = 0;
    Instruction::BinaryOps finalOp;
    bool found = false;

    if (isPowerOf2_64(distLow)) {
        mainLog = logLow;
        distLog = APInt(width, distLow).logBase2();
        finalOp = Instruction::Add;
        found = true;
    } else if (isPowerOf2_64(distHigh)) {
        mainLog = logHigh;
        distLog = APInt(width, distHigh).logBase2();
        finalOp = Instruction::Sub;
        found = true;
    }

    if (found) {
        auto* type = c->getType();
        auto* shl1 = BinaryOperator::Create(Instruction::Shl, var, ConstantInt::get(type, mainLog));
        auto* shl2 = BinaryOperator::Create(Instruction::Shl, var, ConstantInt::get(type, distLog));
        auto* finalRes = BinaryOperator::Create(finalOp, shl1, shl2);
        
        return {shl1, shl2, finalRes};
    }

    return {};
}

bool runOnBasicBlock(BasicBlock &B) override {
    bool Transformed = false;
    for (auto it = B.begin(); it != B.end();) {
        Instruction& instr = *it++;

        if (instr.getNumOperands() != 2) continue;

        Value*      op1 = instr.getOperand(0);
        Value*      op2 = instr.getOperand(1);
        auto* cst1 = dyn_cast<ConstantInt>(op1);
        auto* cst2 = dyn_cast<ConstantInt>(op2);

        std::vector<Instruction*> newInsts;

        switch (instr.getOpcode()) {
            case Instruction::SDiv:
                if (cst2 && cst2->getValue().isPowerOf2()){
                    Instruction* ashr = BinaryOperator::Create(Instruction::AShr, op1,
                                ConstantInt::get(cst2->getType(), cst2->getValue().logBase2()));
                    newInsts.push_back(ashr);
                }
                break;

            case Instruction::Mul: {
                auto* cst = cst1 ? cst1 : cst2;
                if (!cst) continue;
                Value* var = cst == cst1 ? op2 : op1;
                newInsts = tryMulReduction(var, cst);
                break;
            }

            default: continue;
        }

        if (newInsts.empty()) continue;

        Instruction* replace = &instr;
        for(auto* newInst : newInsts) {
          newInst->insertAfter(replace);
          replace = newInst;
        }
        
        instr.replaceAllUsesWith(newInsts.back());
        instr.eraseFromParent();

        Transformed = true;
    }
    return Transformed;
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
