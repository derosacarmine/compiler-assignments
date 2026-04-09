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

class CustomDominatorAnalysis : public PassInfoMixin<CustomDominatorAnalysis> {
public:
    PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM) {
        // Mappa per memorizzare il set OUT per ogni BasicBlock
        std::map<BasicBlock*, std::set<BasicBlock*>> Out;
        
        // Inizializzazione
        BasicBlock *Entry = &F.getEntryBlock();
        
        // Il set universale U contiene tutti i blocchi della funzione
        std::set<BasicBlock*> UniversalSet;
        for (BasicBlock &BB : F) {
            UniversalSet.insert(&BB);
        }

        for (BasicBlock &BB : F) {
            if (&BB == Entry) {
                Out[&BB] = {Entry};
            } else {
                Out[&BB] = UniversalSet;
            }
        }

        // Iterazione (Fixed-point iteration)
        bool changes = true;
        while (changes) {
            changes = false;

            for (BasicBlock &B : F) {
                if (&B == Entry) continue;

                // Calcolo IN[B] = Intersection of OUT[p] for all predecessors p
                std::set<BasicBlock*> InB;
                bool firstPred = true;

                for (BasicBlock *P : predecessors(&B)) {
                    if (firstPred) {
                        InB = Out[P];
                        firstPred = false;
                    } else {
                        // Intersezione
                        std::set<BasicBlock*> intersect;
                        std::set_intersection(InB.begin(), InB.end(),
                                              Out[P].begin(), Out[P].end(),
                                              std::inserter(intersect, intersect.begin()));
                        InB = std::move(intersect);
                    }
                }

                // Se non ci sono predecessori (e non è l'entry), InB rimane vuoto o gestito
                // OUT_new = {B} U IN[B]
                std::set<BasicBlock*> OutNew = InB;
                OutNew.insert(&B);

                // Controllo se il set è cambiato
                if (OutNew != Out[&B]) {
                    Out[&B] = std::move(OutNew);
                    changes = true;
                }
            }
        }

        // Stampa dei risultati (Debug)
        errs() << "Dominator Analysis for function: " << F.getName() << "\n";
        for (BasicBlock &B : F) {
            errs() << "Node " << B.getName() << " is dominated by: { ";
            for (auto *Dom : Out[&B]) {
                errs() << Dom->getName() << " ";
            }
            errs() << "}\n";
        }
        


        return PreservedAnalyses::all();
    }
};