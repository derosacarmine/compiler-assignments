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

    /**
     * @brief struct for common methods
     * 
     */
struct Common {
    /**
     * @brief It iterates over each instruction and attempts to replace expensive operations with cheaper equivalents.
     * 
     * @param B 
     * @return true 
     * @return false 
     */
    virtual bool runOnBasicBlock(BasicBlock &B) = 0; // forces to implement method in subclasses
    
    /**
     * @brief the starting point, it calls runOnFunction with the given function,
     * which will in turn iterate over each of its basic blocks calling runOnBasicBlock
     * for each of them. eventually returns whether there were changes ("preserved" "none") or not ("all" preserved as is) 
     * 
     * @param F 
     * @return PreservedAnalyses 
     */
    PreservedAnalyses run(Function &F, FunctionAnalysisManager &) {
        if(runOnFunction(F))
            return  PreservedAnalyses::none();

        return PreservedAnalyses::all();
    }

  /**
   * @brief for each basic block of the fz it calls runOnBasicBlock
   * 
   * @param F 
   * @return true 
   * @return false 
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

  /**
   * @brief required
   * 
   * @return true 
   * @return false 
   */
  static bool isRequired() { return true; }



};




struct AlgebraicIdentity: PassInfoMixin<AlgebraicIdentity>, Common {


/**
 * @brief for constantMap
 */
std::function<Value*(ConstantInt* c, Value* v)> ifZeroReturnV = [](ConstantInt* c, Value* v) -> Value* { return c->isZero() ? v : nullptr;};
std::function<Value*(ConstantInt* c, Value* v)> ifOneReturnV = [](ConstantInt* c, Value* v) -> Value* { return c->isOne() ? v : nullptr;};
std::function<Value*(ConstantInt* c, Value* v)> ifOneReturnZero = [](ConstantInt* c, Value* v) -> Value* { return c->isOne() ? ConstantInt::get(c->getType(), 0) : nullptr;};
std::function<Value*(ConstantInt* c, Value* v)> ifZeroReturnZero = [](ConstantInt* c, Value* v) -> Value* { return c->isZero() ? ConstantInt::get(c->getType(), 0) : nullptr;};
std::function<Value*(ConstantInt* c, Value* v)> ifMinusOneReturnV = [](ConstantInt* c, Value* v) -> Value* { return c->isMinusOne() ? v : nullptr;};

/**
 * @brief for variableMap
 */
std::function<Value*(Value* op1, Value* op2)> ifOpsEqualReturnZero = [](Value* op1, Value* op2) -> Value* { return (op1 == op2) ? ConstantInt::get(op1->getType(), 0) : nullptr;};
std::function<Value*(Value* op1, Value* op2)> ifOpsEqualReturnOne = [](Value* op1, Value* op2) -> Value* { return (op1 == op2) ? ConstantInt::get(op1->getType(), 1) : nullptr;};
std::function<Value*(Value* op1, Value* op2)> ifOpsEqualReturnOp1 = [](Value* op1, Value* op2) -> Value* { return (op1 == op2) ? op1 : nullptr;};


/**
 * @brief Returns a function that takes the list of functions and tries them in order
 */
using Fn = std::function<Value*(ConstantInt*, Value*)>;
    
    /**
     * @brief creates a lambda function that iterates on a vector of functions until it finds an applicable one
     * 
     * @param fns vector of functions
     * @return Fn the function that iterates on the vector of functions
     */
    static Fn firstOf(std::vector<Fn> fns) {
        return [fns](ConstantInt* c, Value* v) -> Value* {
            for (auto& fn : fns)
                if (auto* r = fn(c, v)) return r;
            return nullptr;
        };
    }
  
/**
 * @brief map used to simplify identities which have a constant
 */
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

/**
 * @brief map used to simplify identities which have two identical operands
 */
std::map<unsigned, std::function<Value*(Value*, Value*)>> variablesMap = {
    {Instruction::Sub, ifOpsEqualReturnZero},
    {Instruction::SDiv, ifOpsEqualReturnOne},
    {Instruction::And, ifOpsEqualReturnOp1},
    {Instruction::Or, ifOpsEqualReturnOp1},
    {Instruction::Xor, ifOpsEqualReturnZero},
    {Instruction::URem, ifOpsEqualReturnZero},
    {Instruction::SRem, ifOpsEqualReturnZero},
};

/**
 * @brief set of commutative instructions
 */
std::set<unsigned> commutativeOps = {Instruction::Add, Instruction::Mul, Instruction::Or, Instruction::And, Instruction::Xor};

/**
 * @brief performs algebraic simplification on instructions within a basic block.
 * it identifies binary operations (which can have either constant operands or also a variable and a constant)
 * and checks if they represent an identity element (e.g. adding 0, multiplying or dividing by 1).
 * it a simplification is possible, the instruction is replaced by it's variable operand which is propagated
 * in place of all future uses of that instruction which is then removed
 * 
 * @param B 
 * @return true 
 * @return false 
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

/**
 * @brief generates an instruction to negate the resulting value by subtracting it from 0.
 * this handles operations involgin negative coefficients (e.g.  x * -1 => 0 - x )
 * 
 * @param type 
 * @param finalValue 
 * @return Instruction* 
 */
Instruction* createNegativeInstr(Type* type, Value* finalValue) {
    Value* zero = ConstantInt::get(type, 0);
    Instruction* neg = BinaryOperator::Create(Instruction::Sub, zero, finalValue);
    return neg;
}

/**
 * @brief Returns a vector of instructions for the reduction, or an empty vector if no optimization is applicable.
 * differentiates between three cases:
 * negation (multiplication by -1)
 * power of two scaling
 * general constant multiplication
 * 
 * @param var 
 * @param c 
 * @return std::vector<Instruction*> 
 */
std::vector<Instruction*> tryMulReduction(Value* var, ConstantInt* c) {
    const APInt& originalVal = c->getValue();
    bool isNegative = originalVal.isNegative();
    
    APInt absVal = originalVal.abs();
    uint64_t z = absVal.getZExtValue();
    auto* type = c->getType();

    std::vector<Instruction*> results = {};
    Value* finalValue = nullptr;

    //case 1: multiply by 1 (if original value is -1), do nothing and subtract from 0 at the end
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
          //if both are true then we'd be reducing the mul to 4 operations, not optimizing anything
          //a first shift, a second shift, an add/sub, a sub for the negative constant
          if (distLog > 0 && isNegative) return {};
          
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

/**
 * @brief only works for powers of 2, because of that it just needs to check for negative values (if SDiv) and then shift by the result of the log in base 2
 * if signed and negative then it adds a 0-x sub at the end
 * 
 * @param op1 
 * @param c 
 * @return std::vector<Instruction*> 
 */
/*std::vector<Instruction*> tryDivReduction(Value* op1, ConstantInt* c) {
    std::vector<Instruction*> results;
    Value* finalValue = nullptr;
    
    const APInt& originalVal = c->getValue();
    bool isNegative = originalVal.isNegative();
    
    APInt abs_c = isNegative ? originalVal.abs() : originalVal;
    
    // reduce only if the constant is a power of 2
    if (abs_c.isPowerOf2()) {
        finalValue = op1;

        if (abs_c.logBase2() > 0) {
            Instruction* ashr = BinaryOperator::Create(
                Instruction::AShr, op1,
                ConstantInt::get(c->getType(), abs_c.logBase2())
            );

            results.push_back(ashr);
            finalValue = ashr;
        }

        if (isNegative && finalValue) {
            Instruction* neg = createNegativeInstr(c->getType(), finalValue);
            results.push_back(neg);
        }
    }
    
    return results;
}
*/
/**
 * Ottimizzazione della divisione intera con segno tramite moltiplicazione per costante magica.
 * Implementa l'algoritmo di Granlund-Montgomery.
 * * L'idea è sostituire n / d con (n * M) >> k, dove M è una "Magic Constant" e k è lo shift.
 * * 1. Calcolo dei parametri (M, precision):
 * - Si cerca la precisione minima 'precision' tale che l'approssimazione di 1/d sia corretta.
 * - 'm_low' e 'm_high' definiscono l'intervallo di confidenza per la costante magica.
 * - Il loop di riduzione ottimizza 'precision' per minimizzare il valore del moltiplicatore.
 * * 2. Generazione del codice IR :
 * - Moltiplicazione a 64 bit (per ottenere i 32 bit alti del prodotto).
 * - Correzione additiva/sottrattiva: necessaria se il magic number è >= 2^31.
 * - Correzione del segno finale: aggiunge il bit di segno per arrotondare verso zero.
 */
std::vector<Instruction*> getDivisor(Value* v, ConstantInt* c) {
    std::vector<Instruction*> insts;
    
    int32_t d = (int32_t)c->getSExtValue();
    
    if (d == 0 || d == 1 || d == -1) return {};

    uint32_t abs_d = (d < 0) ? -d : d;

    if( (abs_d  & (abs_d-1)) == 0){ //potenza di 2

        //conta gli zeri da destra verso sinistra finchè non incontra 1
        uint32_t log_d = __builtin_ctz(abs_d);
        Type* i32 = v->getType();

        // Se il val di v è negativo:
        // dobbiamo aggiungere (distanza - 1) prima dello shift
        // per arrotondare correttamente verso lo zero.
        
        // 1. Estrai il segno: sra x, 31
        auto* ashr_sign = BinaryOperator::Create(Instruction::AShr, v, 
                          ConstantInt::get(i32, 31), "sign_mask");
        insts.push_back(ashr_sign);

        // 2. Crea l'offset per isolare log_d bit (matematicamente per ottenere 2^log_d - 1, ma lo facciamo così poichè è branchless): lshr sign_mask, (32 - log_d)
        auto* lshr_offset = BinaryOperator::Create(Instruction::LShr, ashr_sign, 
                           ConstantInt::get(i32, 32 - log_d), "offset");
        insts.push_back(lshr_offset);

        // 3. Aggiungi l'offset al valore originale
        auto* add_off = BinaryOperator::Create(Instruction::Add, v, lshr_offset, "add_offset");
        insts.push_back(add_off);

        // 4. Shift finale vero e proprio: ashr result, log_d
        auto* final_ashr = BinaryOperator::Create(Instruction::AShr, add_off, 
                           ConstantInt::get(i32, log_d), "final_res");
        insts.push_back(final_ashr);

        if(d < 0){
            Instruction* neg = createNegativeInstr(c->getType(), final_ashr);
            insts.push_back(neg);
        }


        return insts;
    }

    // Calcoliamo la potenza di 2 logaritmica --> clz
    //Conta il numero di zeri consecutivi prima del primo bit 
    // impostato a 1 (il bit più significativo), 
    // partendo da sinistra verso destra nella rappresentazione 
    // binaria di un intero.
    uint32_t precision = 31 - __builtin_clz(abs_d - 1) + 1; 
    uint64_t m_low = (1ULL << (32 + precision)) / abs_d;
    uint64_t m_high = ((1ULL << (32 + precision)) + (1ULL << precision)) / abs_d;
    
    // Riduzione della precisione per trovare il magic number più piccolo
    while ((m_low >> 1) < (m_high >> 1) && precision > 0) {
        m_low >>= 1;
        m_high >>= 1;
        precision--;
    }
    
    int64_t magic_number = (int64_t)m_high;
    int32_t magic_32 = (int32_t)magic_number;
    int shift_k = precision;

    // --- GENERAZIONE ISTRUZIONI ---
    Type* i32 = v->getType();
    Type* i64 = Type::getInt64Ty(v->getContext());

    // 1. Moltiplicazione Wide (64-bit)
    auto* x64 = new SExtInst(v, i64, "sext_v");
    insts.push_back(x64);

    auto* m64 = ConstantInt::get(i64, magic_number);
    auto* prod = BinaryOperator::Create(Instruction::Mul, x64, m64, "mul_tmp");
    insts.push_back(prod);

    // 2. Estrazione della parte alta (il risultato della moltiplicazione magica)
    auto* high64 = BinaryOperator::Create(Instruction::AShr, prod, ConstantInt::get(i64, 32), "ashr_high");
    insts.push_back(high64);

    auto* high = new TruncInst(high64, i32, "trunc_high");
    insts.push_back(high);

    Value* current_v = high;

    // 3. Correzione Additiva/Sottrattiva (necessaria se il magic number eccede 2^31)
    if (magic_32 < 0 /*&& d > 0*/) {
        auto* fix = BinaryOperator::Create(Instruction::Add, current_v, v, "fix_pos");
        insts.push_back(fix);
        current_v = fix;
    }

    // 4. Shift Aritmetico finale per scalare il risultato
    if (shift_k > 0) {
        auto* sar = BinaryOperator::Create(Instruction::AShr, current_v, ConstantInt::get(i32, shift_k), "final_sar");
        insts.push_back(sar);
        current_v = sar;
    }

    // 5. Correzione Segno (Arrotondamento verso zero)
    // Estraiamo il bit di segno (1 se negativo, 0 se positivo)
    auto* sign = BinaryOperator::Create(Instruction::LShr, current_v, ConstantInt::get(i32, 31), "sign_corr");
    insts.push_back(sign);

    // Aggiungiamo il bit di segno al risultato per correggere il troncamento di ashr
    auto* div_abs = BinaryOperator::Create(Instruction::Add, current_v, sign, "div_abs");
    insts.push_back(div_abs);
    current_v = div_abs;

    if (d < 0) {
        auto* final_neg = BinaryOperator::Create(Instruction::Sub, 
                        ConstantInt::get(i32, 0), current_v, "final_div_neg");
        insts.push_back(final_neg);
        current_v = final_neg;
    }

    return insts;
}

/**
 * formula: x - ((x >> k) << k)
 * this function works with the signed rem, which would otherwise give wrong results for negative variables if not treated differently from positive ones
 */
/*
std::vector<Instruction*> trySRemReduction(Value* op1, Type* type, unsigned k) {
    std::vector<Instruction*> results;
    Instruction* ashr = BinaryOperator::Create(Instruction::AShr, op1, ConstantInt::get(type, k));
    results.push_back(ashr);
    Instruction* shl = BinaryOperator::Create(Instruction::Shl, ashr, ConstantInt::get(type, k));
    results.push_back(shl);
    Instruction* sub = BinaryOperator::Create(Instruction::Sub, op1, shl);
    results.push_back(sub);
    return results;
}
*/

/**
 * @brief logic for the reduction of the signed remainder
 * 
 * @param op1 
 * @param type 
 * @param k 
 * @return std::vector<Instruction*> 
 */
std::vector<Instruction*> trySRemReduction(Value* op1, Type* type, unsigned k) {
    std::vector<Instruction*> results;
    
    // Ensure 'type' is an integer to retrieve bitwidth
    unsigned bitwidth = type->getIntegerBitWidth();

    // 1. Create a sign mask: x >> (bitwidth - 1)
    // If x is negative, it becomes all 1s (-1). If positive, all 0s.
    Instruction* signMask = BinaryOperator::Create(Instruction::AShr, op1, ConstantInt::get(type, bitwidth - 1));
    results.push_back(signMask);

    // 2. Create the offset: logical shift (LShr) the signMask by (bitwidth - k)
    // If negative: (-1) LShr (32 - k) = (2^k - 1)
    // If positive: 0 LShr ... = 0
    Instruction* offset = BinaryOperator::Create(Instruction::LShr, signMask, ConstantInt::get(type, bitwidth - k));
    results.push_back(offset);

    // 3. Add the offset to the original dividend to handle rounding towards zero
    Instruction* adjusted = BinaryOperator::Create(Instruction::Add, op1, offset);
    results.push_back(adjusted);

    //push 
    Instruction* ashr = BinaryOperator::Create(Instruction::AShr, adjusted, ConstantInt::get(type, k));
    results.push_back(ashr);
    Instruction* shl = BinaryOperator::Create(Instruction::Shl, ashr, ConstantInt::get(type, k));
    results.push_back(shl);

    Instruction* sub = BinaryOperator::Create(Instruction::Sub, op1, shl);
    results.push_back(sub);

    return results;
}

/**
 * @brief we just need to to an AND operation between the variable x and the constant-1
 * since this only works for constants that are powers of 2 bitwise they're going to be a 1 followed by 0s, so remove one and it's a 0 followed by 1s
 * this deletes from the result the most significant bit of the variable and only leaves a sum between the other active bits from the variable
 * 
 * @param op1 
 * @param type 
 * @param absVal 
 * @return std::vector<Instruction*> 
 */
std::vector<Instruction*> tryURemReduction(Value* op1, Type* type, APInt absVal) {
    std::vector<Instruction*> results;

    uint64_t maskValue = absVal.getZExtValue() - 1;
    Instruction* andInst = BinaryOperator::Create(Instruction::And, op1, ConstantInt::get(type, maskValue));
    results.push_back(andInst);

    return results;
}

/**
 * @brief calls the appropriate Rem reduction function based on the isSigned boolean as the signed one 
 * needs a different optimization in case of a negative variable, this is at the cost
 * of a worse optimization for the SRem in the case of a positive variable which could be optimized
 * the same as the URem.
 * 
 * @param op1 
 * @param cst2 
 * @param isSigned 
 * @return std::vector<Instruction*> 
 */
std::vector<Instruction*> tryRemReduction(Value* op1, ConstantInt* cst2, bool isSigned) {
    std::vector<Instruction*> results;
    const APInt& val = cst2->getValue();
    
    APInt absVal = isSigned ? val.abs() : val;

    if (absVal.isPowerOf2()) {
        Type* type = cst2->getType();
        unsigned k = absVal.logBase2();

        if (isSigned) {
            results = trySRemReduction(op1, type, k);
        } else {
            results = tryURemReduction(op1, type, absVal);
        }
    }

    return results;
}

/**
 * @brief iterates through a basic block to analyze each instruction's operation.
 * it verifies that the instruction has exactly two operands, a variable and a constant,
 * otherwise it skips optimization. it then dispatched the appropriate reduction function based on
 * the opcode. if optimized, the new instructio(s) are inserted right after the original,
 * which is then removed.
 * 
 * @param B 
 * @return true 
 * @return false 
 */
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
            case Instruction::Mul:{
                for (auto cst : {cst1, cst2}){
                    if (!cst) continue;
                    Value* var = cst == cst1 ? op2 : op1;
                    newInsts = tryMulReduction(var, cst);
                    if(!newInsts.empty()) break;
                }
                break;
            }

            case Instruction::SDiv:
            case Instruction::UDiv:{
                // divisore non costante -> non ottimizzabile
                if (!cst2) continue;

                newInsts = getDivisor(op1, cst2);
                if (newInsts.empty()) continue;
                break;
            }

            case Instruction::SRem:{
                if (cst2) newInsts = tryRemReduction(op1, cst2, true);
                break;
            }

            case Instruction::URem:{
                if (cst2) newInsts = tryRemReduction(op1, cst2, false);
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

/**
 * @brief we recursively check for instructions with either one of the two opcodes
 * and keep applying that operation on our constant operands until we either
 * find an instruction with a different opcode, reach the end of the chain of
 * instructions, or find one that matches our target.
 * in the last case: if a constant yields the desired offset, the instruction is replaced with the
 * operand variable of the matching instruction
 * 
 * @param v 
 * @param currentOffset 
 * @param target 
 * @return Value* 
 */
Value* searchEquivalentAddSub(Value* v, int currentOffset, int target = 0){

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

    return searchEquivalentAddSub(var, currentOffset);

}

/**
 * @brief For mul and div we utilise fraction operands in order to avoid division approximation errors.
 * we keep computing until we find an instruction with a different operation,
 * reach the end of the instructions,
 * or find a constant that yields the desired offset (in this case it's when numerator and denominator are equal, aka 1),
 * if we do the instruction is replaced with the operand variable of the matching instruction
 * 
 * @param v 
 * @param currentNum 
 * @param currentDen 
 * @return Value* 
 */
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

/**
 * we recursively check for instructions with either one of the two opcodes
 * and keep summing(left-shift)/subtracting(right-shift) on our constant operands until we either
 * find an instruction with a different opcode, reach the end of the chain of
 * instructions, or find one that matches our target.
 * in the last case: if a constant yields the desired offset, the instruction is replaced with the
 * operand variable of the matching instruction
 */
/*
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
*/

 /**
  * @brief if the previous instruction doesn't share the same opcode and costant, we recursively XOR each constant.
  * if a constant yields the desired target offset the instruction can be, and is, replaced with the operand variable
  * of the matching instruction.
  * @param v 
  * @param currentOffset 
  * @param target 
  * @return Value* 
  */
Value* searchEquivalentXor(Value* v, int currentOffset, int target = 0) {
    if(target == currentOffset) return v;
    
    auto* instr = dyn_cast<Instruction>(v);
    if (!instr || instr->getOpcode() != Instruction::Xor) return nullptr;

    auto [constant, var] = getConstAndVal(instr, commutativeOps.count(instr->getOpcode()) > 0);
    if(!constant) return nullptr;

    currentOffset = currentOffset ^ constant->getZExtValue();

    return searchEquivalentXor(var, currentOffset);
}

/**
 * @brief if the previous instruction doesn't share the same opcode and costant, we recursively AND each constant.
 * if a constant yields the desired target offset the instruction can be, and is, replaced with 0.
 * this only works with 0 as that means that the current instruction will yield a 0 result no matter what,
 * if it was any other value it wouldn't work as and AND with a 0 is akin to multiplying by 0,
 * the result remains 0, so this optimization can be applied only with a target 0, if it was any
 * other value then an and with that value wouldn't guarantee that the value would remain unchanged like a 0/
 * 
 * @param v 
 * @param currentOffset 
 * @param target 
 * @return true 
 * @return false 
 */
bool searchEquivalentAnd(Value* v, unsigned currentOffset, int target = 0) {
    if(target == currentOffset) return true;

    auto* instr = dyn_cast<Instruction>(v);
    if (!instr || instr->getOpcode() != Instruction::And) return false;

    auto [constant, var] = getConstAndVal(instr, commutativeOps.count(instr->getOpcode()) > 0);
    if(!constant) return false;

    currentOffset = currentOffset & constant->getZExtValue();

    return searchEquivalentAnd(var, currentOffset);
}

/**
 * @brief we check that the previous instruction shares the same opcode and constant,
 * if so we can remove the current instruction and replace any future use with the previous one.
 * this is because any of the three boolean operations below with the same repeated constant keeps
 * yielding the same result: e.g. x & cst & cst & ... & cst == x & cst
 * 
 * @param var 
 * @param cst 
 * @param opCode 
 * @return Value* 
 */
Value* searchEquivalentBool(Value* var, ConstantInt* cst, unsigned opCode) {
    auto* prevInstr = dyn_cast<Instruction>(var);
    if (!prevInstr || prevInstr->getOpcode() != opCode) return nullptr;

    auto [prevCst, prevVar] = getConstAndVal(prevInstr, commutativeOps.count(opCode) > 0);
    if (!prevCst) return nullptr;

    // a = x ^ 5; b = a ^ 5; => b = x
    if (opCode == Instruction::Xor) {
        // k1 XOR k2, where k1 == k2, => 0
        if (cst->getZExtValue() == prevCst->getZExtValue())
            return prevVar;
        else
            return searchEquivalentXor(prevVar, cst->getZExtValue() ^ prevCst->getZExtValue());
    }
    // a = x & 5; b = a & 5 => b = a; same for OR
    else if (opCode == Instruction::And || opCode == Instruction::Or) {
        if (cst->getZExtValue() == prevCst->getZExtValue())
            return var;
        else if (opCode == Instruction::And)
            return searchEquivalentAnd(prevVar, cst->getZExtValue() & prevCst->getZExtValue()) ? ConstantInt::get(cst->getType(), 0) : nullptr;
    }

    return nullptr;
}

/**
 * @brief set of commutative instructions
 * 
 */
std::set<unsigned> commutativeOps = {Instruction::Add, Instruction::Mul, Instruction::And, Instruction::Or, Instruction::Xor};

/**
 * @brief returns the constant and variable values for the given instruction if present, nullptr otherwise
 * 
 * @param instr 
 * @param commutative 
 * @return std::pair<ConstantInt*, Value*> 
 */
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

/**
 * @brief iterates through a basic block to analyze each instruction's for exactly two operands.
 * it identifies the roles of the variable and constant, computes a baseline offset, and normalizes
 * the sign for subtraction or right-shift operations.
 * dispathced the relevant optimization based on the opcode; if successful, it propagates the new value to
 * replace the old redundant instruction which is then removed 
 * 
 * @param B 
 * @return true 
 * @return false 
 */
bool runOnBasicBlock(BasicBlock &B) override {
    bool transformed = false;
    for (auto it = B.begin(); it != B.end();) {
        Instruction& instr = *it++;

        int opCode = instr.getOpcode();

        if(instr.getNumOperands() != 2) continue;

        auto [constant, var] = getConstAndVal(&instr, commutativeOps.count(opCode) > 0);

        if(!constant) continue;

        int startOffset = constant->getSExtValue();

        if(opCode == Instruction::Sub || opCode == Instruction::AShr || opCode == Instruction::LShr) 
            startOffset = -startOffset;
        
        Value* eqValue = nullptr;

        
        switch (opCode) {
            case Instruction::Add:
            case Instruction::Sub:
                eqValue = searchEquivalentAddSub(var, startOffset);
                break;
            
            case Instruction::Mul:
                eqValue = searchEquivalentMulDiv(var, startOffset, 1);
                break;
            case Instruction::SDiv:
                eqValue = searchEquivalentMulDiv(var, 1, startOffset);
                break;

            /*
            case Instruction::Shl:
            case Instruction::AShr:
            case Instruction::LShr:
                eqValue = searchEquivalentShift(var, 0, startOffset);
                break;
            */
            
            case Instruction::And:
            case Instruction::Or:
            case Instruction::Xor:
                eqValue = searchEquivalentBool(var, constant, opCode);
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
