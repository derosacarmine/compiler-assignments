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
#include <vector>

using namespace llvm;

#define DEBUG true

namespace {

struct LoopInvariantCodeMotion : PassInfoMixin<LoopInvariantCodeMotion> {

    //this function return that is a dead variable
    bool isDeadAfterLoop(Instruction* I, Loop* LL) {

        for (User* U : I->users()) {    //mi scorre tutti gli usi dell'istruzione tramite la def use chain
            Instruction* userInst = cast<Instruction>(U);
            BasicBlock* userBB = userInst->getParent();

            // se l'uso è fuori dal loop -> non è dead
            if (!LL->contains(userBB)) {
                return false;
            }
        }
        return true;
    }

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
            
            std::set<Instruction*> invariantSet;
            std::vector<Instruction*> toMove;

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
            
            

            for (BasicBlock* BB : LL->getBlocks()) {
                for (Instruction& I : *BB) {
                    if ((invariantSet.count(&I)>0) && (dominatesExits(&I, LL, DT) || isDeadAfterLoop(&I, LL))) {
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
