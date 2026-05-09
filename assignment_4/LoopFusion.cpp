#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Analysis/LoopInfo.h"
#include <algorithm>
#include <llvm-19/llvm/IR/Analysis.h>

#include <llvm-19/llvm/IR/BasicBlock.h>
#include <llvm-19/llvm/IR/Constant.h>
#include <llvm-19/llvm/IR/Constants.h>
#include <llvm-19/llvm/IR/Instruction.h>
#include <llvm-19/llvm/IR/IntrinsicInst.h>
#include <llvm-19/llvm/IR/Value.h>
#include <llvm-19/llvm/Support/Casting.h>
#include "llvm/IR/Dominators.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/Analysis/LoopIterator.h"
#include "llvm/Analysis/PostDominators.h"

#include <unordered_map>
#include <unordered_set>
#include <map>
#include <vector>

using namespace llvm;

namespace {

struct LoopFusion : PassInfoMixin<LoopFusion> {


    /**
     * @brief collects the loops present at each nest level that are candidates for loop fusion
     * 
     * @param LL current loop
     * @param loopsMap global map that tracks the candidates loop for each nest level
     * @param n current nest level
     */
    // void collectLoopsAtN(Loop* LL,  std::map<unsigned, std::vector<Loop*>>& loopsMap, int n){
    //     if(!LL->isLoopSimplifyForm())
    //             return;
    //     loopsMap[n].push_back(LL);
        
    //     for(auto& subLL : LL->getSubLoops()){
    //         collectLoopsAtN(subLL, loopsMap, n+1);
    //     }
    // }

    /**
     * @brief Check if two loops are control flow equivalent (CFE)
     * 
     * @param L1 first loop
     * @param L2 second loop
     * @param DT DomTree
     * @param PDT PostDomTree
     * @return true 
     * @return false 
     */

    bool areControlFlowEquivalent(Loop *L1, Loop *L2, DominatorTree &DT, PostDominatorTree &PDT) {
        BasicBlock *Pre1 = L1->getLoopPreheader();
        BasicBlock *Pre2 = L2->getLoopPreheader();

        if (!Pre1 || !Pre2) {
            return false;
        }

        return DT.dominates(Pre1, Pre2) && PDT.dominates(Pre2, Pre1);
    }
    
    void processNestLevelLoops(std::vector<Loop*> &siblings, DominatorTree &DT, PostDominatorTree &PDT) {
        std::vector<Loop*> candidateLoops;
    
        //filtering loops that are not candidate for LF
        for (Loop* L : siblings) {
            if (L->isLoopSimplifyForm()) {
                candidateLoops.push_back(L);
            }
        }
        
        std::vector<std::vector<Loop*>> cfeGroups;
        if (candidateLoops.size() >= 2){
            //reorder loops from first to last
            std::reverse(candidateLoops.begin(), candidateLoops.end());

            for(auto& loop: candidateLoops){
                bool addedToGroup = false;

                for (auto &group : cfeGroups) {
                    if (areControlFlowEquivalent(group.front(), loop, DT, PDT)) {
                        group.push_back(loop);
                        addedToGroup = true;
                        break;
                    }
                }
                    
                if (!addedToGroup) {
                    cfeGroups.push_back({loop});
                }

            }

            //debug output
            for (auto& group : cfeGroups) {
                if (group.size() >= 2) {
                    outs() << "found CFE with size " << group.size() << "\n";
                }
            }
        }
        

        // TODO: do checks for each pair of loops in each group (groups of only one loop are excluded) and fuse if possible

        //we explore the next nest level for each loop (in case of fusion both the domTree and LoopAnalysis must be updated)
        for (Loop* L : siblings) {
            std::vector<Loop*> children = L->getSubLoopsVector();
            if(children.size()>=2)
                processNestLevelLoops(children, DT, PDT);
        }
    }

    PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM) {
        LoopInfo &LI = AM.getResult<LoopAnalysis>(F);
        auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
        auto &PDT = AM.getResult<PostDominatorTreeAnalysis>(F);


        //getTopLevelLoops() iterates from the last loop to the first
        processNestLevelLoops(LI.getTopLevelLoopsVector(), DT, PDT);
        
        return PreservedAnalyses::all();
    }

    static bool isRequired() { return true; }

}; 
}

llvm::PassPluginLibraryInfo getLoopPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "LoopFusion", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &FPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "LF") {
                    FPM.addPass(LoopFusion());
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
