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
        std::map<BasicBlock*, ExpressionSet> In, Out, Gen;

        // HELPER: Controlla se un Value (operando) è stato definito (creato) in questo blocco.
        // In SSA, un'istruzione coincide con il valore che produce.
        auto isDefinedIn = [](Value *V, BasicBlock *BB) {
            if (auto *Inst = dyn_cast<Instruction>(V)) {
                return Inst->getParent() == BB;
            }
            return false; // Parametri della funzione (%a, %b) o Costanti (5) non sono definiti in nessun blocco
        };

        // 1. Pre-calcolo dell'Universal Set (tutte le espressioni del programma)
        ExpressionSet UniversalSet;
        for (BasicBlock &BB : F) {
            for (Instruction &I : BB) {
                if (auto *BinOp = dyn_cast<BinaryOperator>(&I)) {
                    UniversalSet.insert({BinOp->getOpcode(), BinOp->getOperand(0), BinOp->getOperand(1)});
                }
            }
        }

        // 2. Pre-calcolo di Gen e Inizializzazione di In
        for (BasicBlock &BB : F) {
            for (Instruction &I : BB) {
                if (auto *BinOp = dyn_cast<BinaryOperator>(&I)) {
                    Value *lhs = BinOp->getOperand(0);
                    Value *rhs = BinOp->getOperand(1);
                    Expression expr = {BinOp->getOpcode(), lhs, rhs};

                    // GEN: Un'espressione entra in Gen[B] SOLO SE viene usata in B e 
                    // i suoi operandi NON sono stati appena definiti all'interno di B stesso.
                    // (Se fossero definiti in B, all'ingresso del blocco l'espressione non sarebbe valida!)
                    if (!isDefinedIn(lhs, &BB) && !isDefinedIn(rhs, &BB)) {
                        Gen[&BB].insert(expr);
                    }
                }
            }
            // Inizializziamo In al set universale per l'algoritmo
            In[&BB] = UniversalSet;
        }

        // 3. Iterazione del Punto Fisso (Backward)
        bool changes = true;
        while (changes) {
            changes = false;

            for (BasicBlock &BB : reverse(F)) {
                ExpressionSet currentOut;
                
                // MEET OPERATION e BOUNDARY CONDITION
                if (succ_empty(&BB)) {
                    currentOut = {};
                } else {
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
                }
                Out[&BB] = currentOut;

                // TRANSFER FUNCTION: In[B] = Gen[B] U (Out[B] - Kill[B])
                ExpressionSet currentIn = Gen[&BB];
                for (const auto &expr : Out[&BB]) {
                    // KILL IN SSA: Un'espressione che arriva da Out NON sopravvive in In se 
                    // uno dei suoi operandi è stato definito in questo blocco (viene "Killata" andando a monte).
                    if (!isDefinedIn(expr.lhs, &BB) && !isDefinedIn(expr.rhs, &BB)) {
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