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
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/ADT/DepthFirstIterator.h"

#include <unordered_set>
#include <map>
#include <vector>

using namespace llvm;

#define DEBUG true

namespace {

struct LoopInvariantCodeMotion : PassInfoMixin<LoopInvariantCodeMotion> {
    
    /**
     * @brief checks if the variable is dead after exiting the loop
     * 
     * @param I 
     * @param LL 
     * @return true 
     * @return false 
     */
    /*bool isDeadAfterLoop(Instruction* I, Loop* LL) {
        for (User* U : I->users()) {
            Instruction* userInst = cast<Instruction>(U);
            BasicBlock* userBB = userInst->getParent();


            if (!LL->contains(userBB)) {
                // non e' morta
                return false;
            }
        }

        //E" morta
        return true;
    }*/

    /**
     * @brief checks that the instruction is in a block that dominates every loop's exit
     * 
     * @param I 
     * @param LL 
     * @param DT 
     * @return true 
     * @return false 
     */
    bool dominatesExits(Instruction* I, Loop* LL, DominatorTree& DT){
        SmallVector<BasicBlock*> exitBlocks;
        LL->getExitBlocks(exitBlocks);

        bool dominates = true;
                        
        for(auto exitBlock : exitBlocks) {
            if(!DT.dominates(I->getParent(), exitBlock)){
                dominates = false;
                break; 
            }
        }

        return dominates;
    }

    PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM) {

        LoopInfo &LI = AM.getResult<LoopAnalysis>(F);
        auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
        bool modified = false;
        for (Loop *LL : LI.getLoopsInPreorder()) {
            if(!LL->isLoopSimplifyForm()) continue;
            
            std::unordered_set<Instruction*> invariantSet;
            std::vector<Instruction*> toMove;
            std::unordered_set<Instruction*> movedSet;

            bool changed = true;
            while (changed) {
                changed = false;
                
                //Loop Invariant
                for (BasicBlock *BB : LL->getBlocks()) {
                    for (Instruction &I : *BB) {
                        if (!isSafeToSpeculativelyExecute(&I)) continue;

                        if (I.getOpcode() == Instruction::PHI || invariantSet.count(&I) > 0) 
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
            
            
            //Code Motion
            for (BasicBlock* BB : LL->getBlocks()) {
                for (Instruction& I : *BB) {
                    if ((invariantSet.count(&I)>0) && (dominatesExits(&I, LL, DT) /*|| isDeadAfterLoop(&I, LL)*/)) {
                        toMove.push_back(&I);
                    }
                }
            }


            if(DEBUG){
                outs() << "Loop Invariant Instructions" << "\n";
                for (auto instr : invariantSet) {
                    instr->print(outs());
                    outs() << "\n";
                }
            
                outs() << "Code Motion Instructions" << "\n";
                for (auto instr : toMove) {
                    instr->print(outs());
                    outs() << "\n";
                }
            }

            auto preheader = LL->getLoopPreheader();
            auto lastInstr = preheader->getTerminator();

            for(auto instr : toMove){
                instr->moveBefore(lastInstr);
                modified = true;
            }

        }
          
        if(modified)
            return PreservedAnalyses::none();

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
