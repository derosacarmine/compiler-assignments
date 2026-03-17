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
  bool transformed = false;
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
        transformed = true;
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
                transformed = true;
                instr.replaceAllUsesWith(replacement);
                instr.eraseFromParent();
                break;
            }
        }
      }  
  }
  
  return transformed;
}

};


struct StrengthReduction: PassInfoMixin<StrengthReduction>, Common {

// Returns a vector of operations, or {} if no reduction applies
std::vector<Instruction*> tryMulReduction(Value* var, ConstantInt* c) {
    const APInt& originalVal = c->getValue();
    bool isNegative = originalVal.isNegative();
    
    APInt absVal = originalVal.abs();
    uint64_t z = absVal.getZExtValue();
    auto* type = c->getType();

    std::vector<Instruction*> results = {};
    Value* finalValue = nullptr;

    //case 1: multiply by 1 (if original value is -1), do nothing and subtract from 0 at the end
    //TODO: check if we can remove this if and if it's ok to have the extra shift (with 0) added be removed by the identities
    if (absVal.isOne() && isNegative) {
        finalValue = var;
    }

    //case 2: power of 2, just a shift
    else if (absVal.isPowerOf2()) {
        unsigned shift = absVal.logBase2();
        Instruction* shl = BinaryOperator::Create(Instruction::Shl, var, ConstantInt::get(type, shift));
        results.push_back(shl);
        finalValue = shl;
    }

    //case 3: not a power of 2
    //identify the surrounding powers of 2: 2^logLow <= constant <= 2^logHigh
    //compute the offset from both boundaries: distLow and distHigh
    //if either of these is a power of 2 the mul can be reduced to an add/sub between two shifts
    //unless the distLog results 0 (1 = 2^0), in which case the offset to the nearest power of 2 is only 1 and thus only a shift and ad add/sub is needed
    else{
      unsigned logLow = absVal.logBase2();
      unsigned logHigh = logLow + 1;
      uint64_t pLow = 1ULL << logLow;
      uint64_t pHigh = 1ULL << logHigh;

      uint64_t distLow = z - pLow;
      uint64_t distHigh = pHigh - z;

      unsigned mainLog = 0, distLog = 0;
      Instruction::BinaryOps finalOp;
      bool found = false;

      if (isPowerOf2_64(distLow)) {
          mainLog = logLow;
          distLog = APInt(64, distLow).logBase2();
          finalOp = Instruction::Add;
          found = true;
      } 
      else if (isPowerOf2_64(distHigh)) {
          mainLog = logHigh;
          distLog = APInt(64, distHigh).logBase2();
          finalOp = Instruction::Sub;
          found = true;
      }

      if (found) {
          auto* shl1 = BinaryOperator::Create(Instruction::Shl, var, ConstantInt::get(type, mainLog));
          
          //if distLog == 0, use var for the second operation (add/sub)
          Value* secondOperand = var;
          results.push_back(shl1);

          //if distLog is greater than 0 then we need a second shift before the add/sub 
          if (distLog > 0) {
              auto* shl2 = BinaryOperator::Create(Instruction::Shl, var, ConstantInt::get(type, distLog));
              results.push_back(shl2);
              secondOperand = shl2;
          }

          auto* finalRes = BinaryOperator::Create(finalOp, shl1, secondOperand);
          results.push_back(finalRes);
          finalValue = finalRes;
      }
    }

    //checks if we multiplied by a negative value, if so: 0 - the result
    if (isNegative && finalValue) {
      Value* zero = ConstantInt::get(type, 0);
      Instruction* neg = BinaryOperator::Create(Instruction::Sub, zero, finalValue);
      results.push_back(neg);
    }

    return results;
}

bool runOnBasicBlock(BasicBlock &B) override {
    //checks if we applied any optimizations
    bool transformed = false;
    for (auto it = B.begin(); it != B.end();) {
        Instruction& instr = *it++;

        //ignore instructions without 2 operands
        if (instr.getNumOperands() != 2) continue;

        Value* op1 = instr.getOperand(0);
        Value* op2 = instr.getOperand(1);
        auto* cst1 = dyn_cast<ConstantInt>(op1);
        auto* cst2 = dyn_cast<ConstantInt>(op2);

        //vector that takes the new instructions to replace the muls/divs
        std::vector<Instruction*> newInsts;

        switch (instr.getOpcode()) {
            //if it's a div, do a shift right
            case Instruction::SDiv:
                if (cst2 && cst2->getValue().isPowerOf2()){
                    Instruction* ashr = BinaryOperator::Create(Instruction::AShr, op1,
                                ConstantInt::get(cst2->getType(), cst2->getValue().logBase2()));
                    newInsts.push_back(ashr);
                }
                break;

            //if it's a mul, checks which value is the constant and call tryMulReduction to optimize the instruction
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

        //if there are new instructions, insert them after the old ones
        Instruction* replace = &instr;
        for(auto* newInst : newInsts) {
          newInst->insertAfter(replace);
          replace = newInst;
        }
        
        //replace all instances of the result of the old instructions with the new one and then delete the old one from the code
        instr.replaceAllUsesWith(newInsts.back());
        instr.eraseFromParent();

        transformed = true;
    }
    return transformed;
}



};

struct MultiInstruction : PassInfoMixin<MultiInstruction>, Common{
    
  /*Es: b = a +1 --> var = a e Constant = 1 
    b           →  (var=b, constant=0)
    a = b + 1   →  (var=b, constant=+1)
    c = a - 1   →  (var=b, constant=0)  -> constant=0 significa c == b !
    d = a + 1   →  (var=b, constant=+2)
    By recursively working your way up the chain, you accumulate the total constant. 
    If the constant is ultimately 0, it means the current statement is equivalent to var, and you can use 
    replaceAllUsesWith(var).
  */
std::pair<Value*, int64_t> getVarAndConstant(Value* v) {
    auto* instr = dyn_cast<Instruction>(v);
    if (!instr) return {v, 0};

    if (instr->getOpcode() == Instruction::Add) {
        if (auto* cst = dyn_cast<ConstantInt>(instr->getOperand(1))) {
            auto [var, constant] = getVarAndConstant(instr->getOperand(0));
            return {var, constant + cst->getSExtValue()};
        }
        if (auto* cst = dyn_cast<ConstantInt>(instr->getOperand(0))) {
            auto [var, constant] = getVarAndConstant(instr->getOperand(1));
            return {var, constant + cst->getSExtValue()};
        }
    }
    if (instr->getOpcode() == Instruction::Sub) {
        if (auto* cst = dyn_cast<ConstantInt>(instr->getOperand(1))) {
            auto [var, constant] = getVarAndConstant(instr->getOperand(0));
            return {var, constant - cst->getSExtValue()};
        }
    }

    return {v, 0};
}

bool runOnBasicBlock(BasicBlock &B) override {
    bool changed = false;
    for (auto it = B.begin(); it != B.end();) {
        Instruction& instr = *it++;

        if (instr.getOpcode() != Instruction::Add &&
            instr.getOpcode() != Instruction::Sub) continue;

        auto [var, constant] = getVarAndConstant(&instr);
        if (constant == 0 && var != &instr) {
            instr.replaceAllUsesWith(var);
            changed = true;
        }
    }
    return changed;
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
