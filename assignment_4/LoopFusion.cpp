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
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/DependenceAnalysis.h"

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

    BasicBlock* getBlockToCheck(Loop &L) {
        return L.isGuarded() ? L.getLoopGuardBranch()->getParent : L.getLoopPreheader;
    }

    bool areConditionsEquivalent(BranchInst *l1GuardCond, BranchInst *l2GuardCond) {
        CmpInst *l1CmpInst = dyn_cast<CmpInst>(l1GuardCond->getCondition());
        CmpInst *l2CmpInst = dyn_cast<CmpInst>(l2GuardCond->getCondition());

        return l1CmpInst->isIdenticalTo(l2CmpInst);
    }

    /**
     * @brief checks if two loops are adjacent by checking that there are no
     * other basic blocks between the two loops, or in other words that the 
     * exit block of the first loop coincides with the preheader block of
     * the second loop
     * 
     * @param L1 
     * @param L2 
     * @return true 
     * @return false 
     */
    bool areAdjacent(Loop *L1, Loop *L2) {
        BasicBlock *L1Exit = nullptr;
        BasicBlock *L2Entry = getBlockToCheck(L2);
        
        if (L1.isGuarded) {
            if (L2.isGuarded && 
                !areConditionsEquivalent(L1.getLoopGuardBranch(), 
                                                        L2.getLoopGuardBranch())) return false;
            L1Exit = dyn_cast<BasicBlock>(L1.getLoopGuardBranch()->getOperand(1));
        } else {
            L1Exit = L1.getExitBlock();
        }
    }

    /**
     * @brief checks if two loops iterate the same number of times
     * 
     * @param L1 
     * @param L2 
     * @return true 
     * @return false 
     */
    bool hasSameTripCount(Loop *L1, Loop *L2, ScalarEvolution &SE) {
        // TODO: do we want to use getSmasslConstantTripCount or getBackedgeTakenCount ?
        const SCEV *TC1 = SE.getSmallConstantTripCount(&L1);
        const SCEV *TC2 = SE.getSmallConstantTripCount(L2);

        return (TC1 == TC2) && (TC1 != 0);
    }

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
        // TODO: we may need to add a different check for when both loops are guarded
        // also check that the code below is fine, are should we use getLoopHeader instead of Preheader?
        
        BasicBlock *Pre1 = L1->getLoopPreheader();
        BasicBlock *Pre2 = L2->getLoopPreheader();

        if (!Pre1 || !Pre2) {
            return false;
        }

        return DT.dominates(Pre1, Pre2) && PDT.dominates(Pre2, Pre1);
    }

    /**
     * @brief checks that there are no negative distance dependencies
     * between two loops, or in other words L2 can't have an instruction
     * at iteration m that uses a value computed by L1 at a future
     * iteration m+n (where n > 0) 
     * 
     * @param L1 
     * @param L2 
     * @return true 
     * @return false 
     */
    bool hasNegativeDependencies(Loop *L1, Loop *L2) {
        // TODO: I'm not even gonna read big dawg's code for ts, I'mma just hope ts works well enough :pray:

        // save operations that can change memory in some way to vectors
        std::vector<Instruction*> opsL1, opsL2;

        for (BasicBlock *BB : L1->getBlocks()) {
            for (Instruction &I : *BB)
                if (I.mayReadOrWriteMemory()) opsL1.push_back(&I);
        }

        for (BasicBlock *BB : L2->getBlocks()) {
            for (Instruction &I : *BB)
                if (I.mayReadOrWriteMemory()) opsL2.push_back(&I);
        }

        // if the vectors end up empty we can return without doing any check
        if (opsL1.empty() || opsL2.empty()) return false;

        for (Instruction *I1 : opsL1) {
            for (Instruction *I2 : opsL2) {
                // checks if there's a negative distance dependency between the first and second loops
                auto dep = DI.depends(&I1, &I2, true);
                if (!dep) continue;

                // if there's a conflict and the use of the instruction in L2
                // preceeds the use in L1 (checked with getDirection and GT (greater than))
                // then we return true (as in it's true that there's a negative distance dependency and the loops can't be fused)
                if (dep->isConflicting()) {
                    if (dep->getDirection(1) == Dependence::DVEntry::GT) {
                        return true;
                    }
                }
            }
        }

        return false;
    }
    
    void processNestLevelLoops(std::vector<Loop*> &siblings, ScalarEvolution &SE, DominatorTree &DT, PostDominatorTree &PDT, DependenceInfo &DI) {
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
                    // TODO: does this if check through each condition or is it only for the CF equivalence?
                    if (areControlFlowEquivalent(group.front(), loop, DT, PDT)) {

                        /*
                        for (size_t i = 0; i < group.size() - 1; ++i) {
                            Loop *L1 = group[i];
                            Loop *L2 = group[i+1];

                            if (!areAdjacent(L1, L2) || 
                                !hasSameTripCount(L1, L2, SE) || 
                                hasNegativeDependencies(L1, L2, DI)) continue;

                            //fuse loops L1 and L2; 
                        }
                        */

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

        ScalarEvolution &SE = AM.getResult<ScalarEvolutionAnalysis>(F);

        DominatorTree &DT = AM.getResult<DominatorTreeAnalysis>(F);
        PostDominatorTree &PDT = AM.getResult<PostDominatorTreeAnalysis>(F);

        DependenceInfo &DI = AM.getResult<DependenceAnalysis>(F);

        //getTopLevelLoops() iterates from the last loop to the first
        processNestLevelLoops(LI.getTopLevelLoopsVector(), SE, DT, PDT, DI);
        
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
