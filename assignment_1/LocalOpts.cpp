#include "llvm/Analysis/TensorSpec.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"
#include <cstddef>
#include <cstdint>
#include <llvm-19/llvm/IR/Analysis.h>
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

namespace {



  /* struct for common methods */
struct Common {
    /*It iterates over each instruction and attempts to replace expensive operations with cheaper equivalents.*/
    virtual bool runOnBasicBlock(BasicBlock &B) = 0;
    
    PreservedAnalyses run(Function &F, FunctionAnalysisManager &) {
        if(runOnFunction(F))
            return  PreservedAnalyses::none();

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
std::function<Value*(ConstantInt* c, Value* v)> ifZeroReturnV = [](ConstantInt* c, Value* v) -> Value* { return c->isZero() ? v : nullptr;};
std::function<Value*(ConstantInt* c, Value* v)> ifOneReturnV = [](ConstantInt* c, Value* v) -> Value* { return c->isOne() ? v : nullptr;};
std::function<Value*(ConstantInt* c, Value* v)> ifOneReturnZero = [](ConstantInt* c, Value* v) -> Value* { return c->isOne() ? ConstantInt::get(c->getType(), 0) : nullptr;};
std::function<Value*(ConstantInt* c, Value* v)> ifZeroReturnZero = [](ConstantInt* c, Value* v) -> Value* { return c->isZero() ? ConstantInt::get(c->getType(), 0) : nullptr;};
std::function<Value*(ConstantInt* c, Value* v)> ifMinusOneReturnV = [](ConstantInt* c, Value* v) -> Value* { return c->isMinusOne() ? v : nullptr;};

//for variableMap
std::function<Value*(Value* op1, Value* op2)> ifOpsEqualReturnZero = [](Value* op1, Value* op2) -> Value* { return (op1 == op2) ? ConstantInt::get(op1->getType(), 0) : nullptr;};
std::function<Value*(Value* op1, Value* op2)> ifOpsEqualReturnOne = [](Value* op1, Value* op2) -> Value* { return (op1 == op2) ? ConstantInt::get(op1->getType(), 1) : nullptr;};
std::function<Value*(Value* op1, Value* op2)> ifOpsEqualReturnOp1 = [](Value* op1, Value* op2) -> Value* { return (op1 == op2) ? op1 : nullptr;};


/*Returns a function that takes the list of functions and tries them in order.*/
using Fn = std::function<Value*(ConstantInt*, Value*)>;
    
    static Fn firstOf(std::vector<Fn> fns) {
        return [fns](ConstantInt* c, Value* v) -> Value* {
            for (auto& fn : fns)
                if (auto* r = fn(c, v)) return r;
            return nullptr;
        };
    }
  
// map used to simplify identities which have a constant
std::map<unsigned, std::function<Value*(ConstantInt*, Value*)>> constantMap = {
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
std::map<unsigned, std::function<Value*(Value*, Value*)>> variablesMap = {
    {Instruction::Sub, ifOpsEqualReturnZero},
    {Instruction::SDiv, ifOpsEqualReturnOne},
    {Instruction::And, ifOpsEqualReturnOp1},
    {Instruction::Or, ifOpsEqualReturnOp1},
    {Instruction::Xor, ifOpsEqualReturnZero},
    {Instruction::URem, ifOpsEqualReturnZero},
    {Instruction::SRem, ifOpsEqualReturnZero},
};

std::set<unsigned> commutativeOps = {Instruction::Add, Instruction::Mul, Instruction::Or, Instruction::And, Instruction::Xor};

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

    std::vector<Value*> usableConstants;

    if(commutativeOps.count(opCode) > 0)
      usableConstants = {op1,op2};
    else
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

//create an instruction where we subract from 0 in case of an operation with a negative value (i.e.  x * -1 => 0 - x )
Instruction* createNegativeInstr(auto* type, Value* finalValue) {
    Value* zero = ConstantInt::get(type, 0);
    Instruction* neg = BinaryOperator::Create(Instruction::Sub, zero, finalValue);
    return neg;
}

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

    //checks if we multiplied by a negative value
    if (isNegative && finalValue) {
      Instruction* neg = createNegativeInstr(type, finalValue);
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
            //if it's a mul, checks which value is the constant and call tryMulReduction to optimize the instruction
            case Instruction::Mul:
                //auto* cst = cst1 ? cst1 : cst2;
                for (auto cst : {cst1, cst2}){
                    if (!cst) continue;
                    Value* var = cst == cst1 ? op2 : op1;
                    newInsts = tryMulReduction(var, cst);
                    if(!newInsts.empty()) break;
                }
                break;
            
            //if it's a div, do a shift right
            case Instruction::SDiv:
                if (cst2 && cst2->getValue().isPowerOf2()){
                    Instruction* ashr = BinaryOperator::Create(Instruction::AShr, op1,
                                ConstantInt::get(cst2->getType(), cst2->getValue().logBase2()));
                    newInsts.push_back(ashr);

                    const APInt& originalVal = cst2->getValue();
                    bool isNegative = originalVal.isNegative();
                    if(isNegative) {
                        auto* type = cst2->getType();
                        Instruction* neg = createNegativeInstr(type, ashr);
                        newInsts.push_back(neg);
                    }
                }
                break;

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

//we recursively check for values or instructions until we find one that matches our target (usually the neutral value for our operation)
Value* searchEquivalentAddSub(Value* v, int target, int currentOffset){

    //we found the the value we can use to replace the instruction
    if (currentOffset == target)
        return v;

    auto* instr = dyn_cast<Instruction>(v);

    //we reached the last possible value
    if (!instr) return nullptr;

    int opCode = instr->getOpcode();

    if(opCode != Instruction::Add && opCode != Instruction::Sub) return nullptr;

    auto [constant, var] = getConstAndVal(instr, commutativeOps.count(opCode) > 0);

    if(!constant) return nullptr;

    if (opCode == Instruction::Add)
        currentOffset = currentOffset + constant->getSExtValue();
    else
        currentOffset = currentOffset - constant->getSExtValue();

    return searchEquivalentAddSub(var, target, currentOffset);

}

// For mul and div we utilise fraction operands in order to avoid division approximation errors 
Value* searchEquivalentMulDiv(Value* v, int currentNum, int currentDen){

    //the target (1) is reached when numerator and denominator are the same
    if (currentNum == currentDen)
        return v;

    auto* instr = dyn_cast<Instruction>(v);

    if (!instr) return nullptr;

    int opCode = instr->getOpcode();

    if(opCode != Instruction::Mul && opCode != Instruction::SDiv) return nullptr;

    auto [constant, var] = getConstAndVal(instr, commutativeOps.count(opCode) > 0);

    if(!constant) return nullptr;

    int conValue = constant->getSExtValue();

    if(conValue == 0) return nullptr;

    if (opCode == Instruction::Mul)
        currentNum *= conValue;
    else
        currentDen *= conValue;
        
    return searchEquivalentMulDiv(var, currentNum, currentDen);
}

Value* searchEquivalentShift(Value* v, int target, int currentOffset){

    //we found the the value we can use to replace the instruction
    if (currentOffset == target)
        return v;

    auto* instr = dyn_cast<Instruction>(v);

    //we reached the last possible value
    if (!instr) return nullptr;

    int opCode = instr->getOpcode();

    if(opCode != Instruction::Shl && opCode != Instruction::AShr && opCode != Instruction::LShr) return nullptr;

    auto [constant, var] = getConstAndVal(instr, commutativeOps.count(opCode) > 0);

    if(!constant) return nullptr;

    if (opCode == Instruction::Shl)
        currentOffset = currentOffset + constant->getSExtValue();
    else
        currentOffset = currentOffset - constant->getSExtValue();

    return searchEquivalentShift(var, target, currentOffset);

}


std::set<unsigned> commutativeOps = {Instruction::Add, Instruction::Mul};

//returns the constant and variable value for the given instruction, if present, nullptr otherwise
std::pair<ConstantInt*, Value*> getConstAndVal(Instruction* instr, bool commutative){
    auto op1 = instr->getOperand(0);
    auto op2 = instr->getOperand(1);
    auto cst1 = dyn_cast<ConstantInt>(op1);
    auto cst2 = dyn_cast<ConstantInt>(op2);
    
    ConstantInt* constant = nullptr;
    if (!commutative) constant = cst2;
    else constant = (cst1 && !cst2) ? cst1 : (cst2 && !cst1) ? cst2 : nullptr;

    if(!constant) return {nullptr, nullptr};

    Value* var = (constant == cst1) ? op2 : op1;

    return {constant, var};
}

bool runOnBasicBlock(BasicBlock &B) override {
    bool transformed = false;
    for (auto it = B.begin(); it != B.end();) {
        Instruction& instr = *it++;

        int opCode = instr.getOpcode();

        if(instr.getNumOperands() != 2) continue;
        //auto it2 = instrTargets.find(opCode);

        //if (it2 == instrTargets.end()) continue;

        auto [constant, var] = getConstAndVal(&instr, commutativeOps.count(opCode) > 0);

        if(!constant) continue;

        //int target = it2->second;
        int startOffset = constant->getSExtValue();

        if(opCode == Instruction::Sub || opCode == Instruction::AShr || opCode == Instruction::LShr) 
            startOffset = -startOffset;
        
        Value* eqValue = nullptr;

        
        switch (opCode) {
            case Instruction::Add:
            case Instruction::Sub:
                eqValue = searchEquivalentAddSub(var, 0, startOffset);
                break;
            
            case Instruction::Mul:
                eqValue = searchEquivalentMulDiv(var, startOffset, 1);
                break;
            case Instruction::SDiv:
                eqValue = searchEquivalentMulDiv(var, 1, startOffset);
                break;

            case Instruction::Shl:
            case Instruction::AShr:
            case Instruction::LShr:
                eqValue = searchEquivalentShift(var, 0, startOffset);
                break;
            
            default:
                continue;
            
        }

        if(eqValue){
            instr.replaceAllUsesWith(eqValue);
            instr.eraseFromParent();
            transformed = true;
        }
    }
    return transformed;
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
