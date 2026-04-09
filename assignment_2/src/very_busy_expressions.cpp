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
#include <map>
#include <set>
#include <vector>

using namespace llvm;

// Struttura per identificare univocamente un'espressione binaria (es. a - b)
struct Expression {
    unsigned opcode;
    Value *lhs, *rhs;

    bool operator<(const Expression& other) const {
        return std::tie(opcode, lhs, rhs) < std::tie(other.opcode, other.lhs, other.rhs);
    }
    bool operator==(const Expression& other) const {
        return opcode == other.opcode && lhs == other.lhs && rhs == other.rhs;
    }
};

using ExpressionSet = std::set<Expression>;

class VeryBusyExpressions : public PassInfoMixin<VeryBusyExpressions> {
public:
    PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM) {
        std::map<BasicBlock*, ExpressionSet> In, Out;
        std::map<BasicBlock*, ExpressionSet> Gen;
        std::map<BasicBlock*, std::set<Value*>> KillVars;

        // 1. Pre-calcolo di Gen e Kill
        // Gen[B]: espressioni calcolate in B prima che i loro operandi siano ridefiniti.
        // KillVars[B]: variabili (Value*) scritte in B che invalidano espressioni.
       for (BasicBlock &BB : F) {
        for (Instruction &I : BB) {
            if (auto *BinOp = dyn_cast<BinaryOperator>(&I)) {
                Value *lhsPtr = nullptr, *rhsPtr = nullptr;

                if (auto *LLoad = dyn_cast<LoadInst>(BinOp->getOperand(0)))
                    lhsPtr = LLoad->getPointerOperand();
                if (auto *RLoad = dyn_cast<LoadInst>(BinOp->getOperand(1)))
                    rhsPtr = RLoad->getPointerOperand();

                if (lhsPtr && rhsPtr) {
                    Expression expr = {BinOp->getOpcode(), lhsPtr, rhsPtr};
                    if (KillVars[&BB].find(lhsPtr) == KillVars[&BB].end() &&
                        KillVars[&BB].find(rhsPtr) == KillVars[&BB].end()) {
                        Gen[&BB].insert(expr);
                    }
                }
            }
            if (auto *Store = dyn_cast<StoreInst>(&I)) {
                KillVars[&BB].insert(Store->getPointerOperand());
            }
        }
    }
        // 2. Inizializzazione (Boundary e Interior Points)
        ExpressionSet UniversalSet;
        for (auto &BB : F) {
            for (auto &expr : Gen[&BB]) UniversalSet.insert(expr);
        }

        for (BasicBlock &BB : F) {
            // Inizializziamo In[B] al set universale (tranne Exit)
            if (succ_empty(&BB)) { // Exit block
                In[&BB] = {};
            } else {
                In[&BB] = UniversalSet;
            }
        }

        // 3. Iterazione del Punto Fisso (Backward)
        bool changes = true;
        while (changes) {
            changes = false;

            // Iteriamo all'indietro per efficienza (opzionale, ma consigliato)
            for (BasicBlock &BB : reverse(F)) {
                if (succ_empty(&BB)) continue;

                // Meet Operation: Out[B] = Intersection of In[Succ]
                ExpressionSet currentOut;
                bool first = true;
                for (BasicBlock *S : successors(&BB)) {
                    if (first) {
                        currentOut = In[S];
                        first = false;
                    } else {
                        ExpressionSet intersect;
                        std::set_intersection(currentOut.begin(), currentOut.end(),
                                              In[S].begin(), In[S].end(),
                                              std::inserter(intersect, intersect.begin()));
                        currentOut = std::move(intersect);
                    }
                }
                Out[&BB] = currentOut;

                // Transfer Function: In[B] = Gen[B] U (Out[B] - Kill[B])
                ExpressionSet currentIn = Gen[&BB];
                for (const auto &expr : Out[&BB]) {
                    // Un'espressione sopravvive se nessuno dei suoi operandi è in KillVars[B] -> controlla se i puntatori sorgente degli operandi sono stati storati
                    if (KillVars[&BB].find(expr.lhs) == KillVars[&BB].end() &&
                        KillVars[&BB].find(expr.rhs) == KillVars[&BB].end()) {
                        currentIn.insert(expr);
                    }
                }

                if (currentIn != In[&BB]) {
                    In[&BB] = std::move(currentIn);
                    changes = true;
                }
            }
        }

        // Stampa Risultati
        printResults(F, In, Out);
        return PreservedAnalyses::all();
    }

 

private:
   void printResults(Function &F, std::map<BasicBlock*, ExpressionSet> &In, std::map<BasicBlock*, ExpressionSet> &Out) {
    errs() << "--- Very Busy Expressions Analysis: " << F.getName() << " ---\n";
    for (BasicBlock &BB : F) {
        errs() << "BB " << BB.getName() << ":\n";
        errs() << "  IN:  ";
        for (auto &e : In[&BB]) {
            errs() << "(";
            e.lhs->printAsOperand(errs(), false);
            errs() << " " << Instruction::getOpcodeName(e.opcode) << " ";
            e.rhs->printAsOperand(errs(), false);
            errs() << ") ";
        }
        errs() << "\n  OUT: ";
        for (auto &e : Out[&BB]) {
            errs() << "(";
            e.lhs->printAsOperand(errs(), false);
            errs() << " " << Instruction::getOpcodeName(e.opcode) << " ";
            e.rhs->printAsOperand(errs(), false);
            errs() << ") ";
        }
        errs() << "\n";
    }
}
};

   llvm::PassPluginLibraryInfo getVeryBusyExpressionsPluginInfo() {
    return {LLVM_PLUGIN_API_VERSION, "VeryBusyExpressions", LLVM_VERSION_STRING,
            [](PassBuilder &PB) {
                PB.registerPipelineParsingCallback(
                    [](StringRef Name, FunctionPassManager &FPM,
                    ArrayRef<PassBuilder::PipelineElement>) {
                    if (Name == "very-busy-expressions") {
                        FPM.addPass(VeryBusyExpressions());
                        return true;
                    }
                    return false;
                    });
            }};
    }

    extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
    llvmGetPassPluginInfo() {
    return getVeryBusyExpressionsPluginInfo();
    }