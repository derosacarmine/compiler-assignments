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

namespace {

// cheat function ?
// loop->hasLoopInvariantOperands(const Instruction *I)

struct LoopInvariantCodeMotion : PassInfoMixin<LoopInvariantCodeMotion> {

    PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM) {

        LoopInfo &LI = AM.getResult<LoopAnalysis>(F);
        auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
        std::set<Instruction*> invariantSet;


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
        

        for(auto *Node : depth_first(DT.getRootNode())) {
                

        }

        //TODO: Code Motion Logic
    for (Loop *LL : LI.getLoopsInPreorder()) {
        for (auto instr : invariantSet) {

            if(BasicBlock *pre_header = LL->getLoopPreheader())
                instr->moveBefore(pre_header->getTerminator());
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
