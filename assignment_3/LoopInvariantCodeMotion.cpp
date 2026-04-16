#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Analysis/LoopInfo.h"
#include <llvm-19/llvm/IR/Analysis.h>

#include <llvm-19/llvm/IR/BasicBlock.h>
#include <llvm-19/llvm/IR/Constant.h>
#include <llvm-19/llvm/IR/Constants.h>
#include <llvm-19/llvm/IR/Instruction.h>
#include <llvm-19/llvm/IR/Value.h>
#include <llvm-19/llvm/Support/Casting.h>
#include "llvm/IR/Dominators.h"

#include <set>
#include <map>

using namespace llvm;

#define DEBUG true

//TODO: Code Motion Logic
        /* # = fatto
        Algoritmo per la Code Motion
        Dato un insieme di nodi in un loop

        # Calcolare le reaching definitions     da vedere

        # Trovare le istruzioni loop-invariant      controllare
        all’uscita del loop
        # Calcolare i dominatori (dominance tree)
        # Trovare le uscite del loop (i successori fuori dal loop)

         Le istruzioni candidate alla code motion:
        # Sono loop invariant
        # Si trovano in blocchi che dominano tutte le uscite del loop
        
        • Oppure la variabile definita dall’istruzione è dead
          all’uscita del loop
        
        # Assegnano un valore a variabili non assegnate altrove nel loop
        • Si trovano in blocchi che dominano tutti i blocchi nel loop che usano la
        variabile a cui si sta assegnando un valore

         Eseguire una ricerca depth-first dei blocchi
        • Spostare l’istruzione candidata nel preheader se tutte le istruzioni
        invarianti da cui questa dipende sono state spostate
        */

        /*
        x=y+z

        q=...x
        a = getUses instr
        b = getParent instr
        
        for (qualcosa in a)
            check b dominates getParent(qualcosa)
        */

namespace {

// cheat function ?
// loop->hasLoopInvariantOperands(const Instruction *I)

struct LoopInvariantCodeMotion : PassInfoMixin<LoopInvariantCodeMotion> {

    PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM) {

        LoopInfo &LI = AM.getResult<LoopAnalysis>(F);
        auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
        std::set<Instruction*> invariantSet;
        
        std::set<BasicBlock*> dominatorSet;

        //Loop Invariant logic
        for (Loop *LL : LI.getLoopsInPreorder()) {
            if(!LL->isLoopSimplifyForm()) continue;
            

            bool changed = true;
            while (changed) {
                changed = false;
                
                for (BasicBlock *BB : LL->getBlocks()) {
                    for (Instruction &I : *BB) {
                        if (I.getOpcode() == Instruction::PHI || invariantSet.count(&I) > 0) 
                            continue;        
                        
                        // TODO: check if there are other instructions that can be used
                        if(I.getNumOperands() != 2)
                            continue;

                        bool isInvariant = true;
                        
                        for (Use &Op : I.operands()) {
                            Value *operandValue = Op.get();
                            
                            if (auto *op_instr = dyn_cast<Instruction>(operandValue)) {

                                if (LL->contains(op_instr->getParent()) && invariantSet.count(op_instr) == 0) {
                                    isInvariant = false;
                                    break;
                                }
                            }
                        }

                        if (isInvariant) {
                            invariantSet.insert(&I);
                            changed = true;
                        }
                    }
                }
            }
        }

        //Debug output for loop invariant instructions
        if(DEBUG){
            for (auto instr : invariantSet) {
                instr->print(outs());
                outs() << "\n";
            }
        }

    for (Loop *LL : LI.getLoopsInPreorder()) {
        for (auto instr : invariantSet) {
            if(BasicBlock *pre_header = LL->getLoopPreheader())
                instr->moveBefore(pre_header->getTerminator());
        }
        for (auto block : dominatorSet) {
            Instruction* instr = block->getTerminator();
            if(BasicBlock *pre_header = LL->getLoopPreheader())
                instr->moveBefore(pre_header->getTerminator());
        }
    }


    /*RIFERIMENTO: ESERCIZIO DELL'ASS 2 DOMINATOR ANALYSIS
        */ 
    for (Loop *LL : LI.getLoopsInPreorder()) {
        if(!LL->isLoopSimplifyForm()) continue;

        SmallVector<BasicBlock*, 8> exitBlocks;

        //1. trovo i blocchi di uscita del loop
        // blocchi DENTRO il loop che hanno
        // almeno un successore FUORI dal loop
        LL->getExitingBlocks(exitBlocks);

        if(exitBlocks.empty()) continue;

        // 2. Dominatori
        /*
         Un nodo d domina un nodo n in un grafo (d dom n) se
        ogni percorso dall’ENTRY node a n passa per d;
        ogni nodo d domina solo i suoi discendenti nell’albero
    
        */
        for(auto *node : depth_first(DT.getRootNode())) {
            BasicBlock *BB = node->getBlock();    

            if(!LL->contains(BB))   continue;

            bool dominatesAllExits = true;
            for (BasicBlock *exitBB : exitBlocks) {
                //se A dom B
                // internamente risale l'albero da B verso la root
                // e controlla se trova A 
                if (!DT.dominates(BB, exitBB)) {
                    dominatesAllExits = false;
                    break;
                }
            }

            if (dominatesAllExits)
                dominatorSet.insert(BB);
        }
    }
        
        return PreservedAnalyses::all();
    }



    static bool isRequired() { return true; }
}; 
}

//-----------------------------------------------------------------------------
// New PM Registration
//-----------------------------------------------------------------------------
llvm::PassPluginLibraryInfo getLoopPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "LoopInvariantCodeMotion", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &FPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "LI-CM") {
                    FPM.addPass(LoopInvariantCodeMotion());
                    return true;
                  }
                  return false;
                });
          }};
}

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getLoopPassPluginInfo();
}
