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

using namespace llvm;

// Rappresentiamo l'ambiente come una mappa di coppie <Variabile, Valore>
using ConstantEnv = std::map<Value*, ConstantInt*>;

class ExplicitConstantPropagation : public PassInfoMixin<ExplicitConstantPropagation> {
public:
    PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM) {
        std::map<BasicBlock*, ConstantEnv> In, Out;
        std::map<BasicBlock*, ConstantEnv> Gen;
        std::map<BasicBlock*, std::set<Value*>> Kill;

        // --- FASE 1: Calcolo statico di Gen[B] e Kill[B] ---
        for (BasicBlock &BB : F) {
            for (Instruction &I : BB) {
                if (auto *Store = dyn_cast<StoreInst>(&I)) {
                    Value *Var = Store->getPointerOperand();
                    Value *Val = Store->getValueOperand();

                    if (auto *CI = dyn_cast<ConstantInt>(Val)) {
                        // Se è una costante: Gen[B] = Gen[B] U {<Var, Val>}
                        Gen[&BB][Var] = CI;
                    } else {
                        // Se non è costante, rimuovila da Gen se c'era prima (shadowing)
                        Gen[&BB].erase(Var);
                    }
                    // Ogni store "uccide" le definizioni precedenti della variabile
                    Kill[&BB].insert(Var);
                }
            }
        }

        // --- FASE 2: Inizializzazione ---
        // In[Entry] = Out[Entry] = empty
        BasicBlock *Entry = &F.getEntryBlock();
        In[Entry] = {};
        Out[Entry] = Gen[Entry]; // f_Entry({}) = Gen_Entry

        // Inizializzazione punti interni: In[B] = U (Set Universale)
        // In questo contesto, U è rappresentato da un set che contiene tutte le variabili
        // possibili con un valore speciale "Unknown". Per semplicità, gestiamo l'intersezione.
        for (BasicBlock &BB : F) {
            if (&BB == Entry) continue;
            Out[&BB] = {}; 
        }

        // --- FASE 3: Iterazione del Punto Fisso ---
        bool changes = true;
        while (changes) {
            changes = false;

            for (BasicBlock &BB : F) {
                if (&BB == Entry) continue;

                // Meet Operation: In[B] = Intersection of Out[Preds]
                ConstantEnv currentIn;
                bool first = true;
                for (BasicBlock *P : predecessors(&BB)) {
                    if (first) {
                        currentIn = Out[P];
                        first = false;
                    } else {
                        // Intersezione rigorosa: <var, val> deve essere identico in tutti i pred
                        for (auto it = currentIn.begin(); it != currentIn.end(); ) {
                            if (Out[P].find(it->first) == Out[P].end() || 
                                Out[P].at(it->first) != it->second) {
                                it = currentIn.erase(it);
                            } else {
                                ++it;
                            }
                        }
                    }
                }
                In[&BB] = currentIn;

                // Transfer Function: Out[B] = Gen[B] U (In[B] - Kill[B])
                ConstantEnv currentOut = Gen[&BB]; // Gen_B
                for (auto const& [var, val] : In[&BB]) {
                    // Se la variabile non è in Kill[B], sopravvive
                    if (Kill[&BB].find(var) == Kill[&BB].end()) {
                        // Gen ha la precedenza se la stessa variabile fosse in entrambi
                        if (currentOut.find(var) == currentOut.end()) {
                            currentOut[var] = val;
                        }
                    }
                }

                if (currentOut != Out[&BB]) {
                    Out[&BB] = std::move(currentOut);
                    changes = true;
                }
            }
        }

        // Stampa finale
        printTable(F, In, Out);
        return PreservedAnalyses::all();
    }

   

private:
    void printTable(Function &F, std::map<BasicBlock*, ConstantEnv> &In, std::map<BasicBlock*, ConstantEnv> &Out) {
        errs() << "\nConstant Propagation Table:\n";
        errs() << "--------------------------------------------------\n";
        for (BasicBlock &BB : F) {
            errs() << "Block " << BB.getName() << ":\n";
            errs() << "  IN:  "; dumpEnv(In[&BB]);
            errs() << "  OUT: "; dumpEnv(Out[&BB]);
            errs() << "--------------------------------------------------\n";
        }
    }

    void dumpEnv(ConstantEnv &E) {
    if (E.empty()) { errs() << "empty\n"; return; }
    for (auto const& [var, val] : E) {
        errs() << "<";
        // Usa printAsOperand per stampare il nome/riferimento SSA
        var->printAsOperand(errs(), false);
        errs() << ", " << val->getValue() << "> ";
    }
    errs() << "\n";
}
};

 llvm::PassPluginLibraryInfo getConstantPropagationPluginInfo() {
    return {LLVM_PLUGIN_API_VERSION, "ConstantPropagation", LLVM_VERSION_STRING,
            [](PassBuilder &PB) {
                PB.registerPipelineParsingCallback(
                    [](StringRef Name, FunctionPassManager &FPM,
                    ArrayRef<PassBuilder::PipelineElement>) {
                    if (Name == "constant-propagation") {
                        FPM.addPass(ExplicitConstantPropagation());
                        return true;
                    }
                    return false;
                    });
            }};
    }

    extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
    llvmGetPassPluginInfo() {
    return getConstantPropagationPluginInfo();
    }