#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <llvm-19/llvm/Analysis/LazyCallGraph.h>
#include <llvm-19/llvm/Analysis/LoopAnalysisManager.h>
#include <llvm-19/llvm/IR/Analysis.h>

#include "llvm/Analysis/DependenceAnalysis.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Dominators.h"
#include <llvm-19/llvm/IR/BasicBlock.h>
#include <llvm-19/llvm/IR/CFG.h>
#include <llvm-19/llvm/IR/Constant.h>
#include <llvm-19/llvm/IR/Constants.h>
#include <llvm-19/llvm/IR/Instruction.h>
#include <llvm-19/llvm/IR/Instructions.h>
#include <llvm-19/llvm/IR/IntrinsicInst.h>
#include <llvm-19/llvm/IR/Value.h>
#include <llvm-19/llvm/Support/Casting.h>

#include "llvm/Transforms/Utils/Local.h"

#include <map>
#include <vector>

using namespace llvm;

namespace {

struct LoopFusion : PassInfoMixin<LoopFusion> {

  std::map<Loop *, const SCEV *> loopsTripCountMap;

  BasicBlock *getLoopEntry(Loop *L) {
    return L->isGuarded() ? L->getLoopGuardBranch()->getParent()
                          : L->getLoopPreheader();
  }

  BasicBlock *getLoopExit(Loop *L) {
    if (L->isGuarded()) {
      BranchInst *GuardBr = L->getLoopGuardBranch();
      BasicBlock *Preheader = L->getLoopPreheader();

      // checks both successors of the guard block, one should be the preheader
      // of the loop while the other is the exit block.
      if (GuardBr->getSuccessor(0) == Preheader) {
        return GuardBr->getSuccessor(1);
      } else {
        return GuardBr->getSuccessor(0);
      }
    }

    return L->getExitBlock();
  }

  /**
   * @brief checks if the guard blocks of the two loops are equivalent
   *
   * @param l1GuardCond
   * @param l2GuardCond
   * @return true
   * @return false
   */
  bool areConditionsEquivalent(BranchInst *l1GuardCond,
                               BranchInst *l2GuardCond) {

    Value *Cond1 = l1GuardCond->getCondition();
    Value *Cond2 = l2GuardCond->getCondition();

    // in case they share the same variable
    if (Cond1 == Cond2)
      return true;

    if (auto *Inst1 = dyn_cast<Instruction>(Cond1)) {
      if (auto *Inst2 = dyn_cast<Instruction>(Cond2)) {
        return Inst1->isIdenticalTo(Inst2);
      }
    }

    return false;
  }

  /**
   * @brief checks if the block is empty (it contains only a branch or
   * if it has a comparison instruction for guarded loops
   *
   * @param BB
   * @param BI
   * @param isGuard
   * @return true
   * @return false
   */
  bool isBlockEmpty(BasicBlock *BB, BranchInst *BI, bool isGuard) {
    Instruction *FirstInst = BB->getFirstNonPHIOrDbg();

    // the block is empty
    if (FirstInst == BI) {
      return true;
    }

    // for guarded blocks, an additional comparison instruction might be present
    if (isGuard && BI->isConditional()) {
      if (FirstInst == dyn_cast<Instruction>(BI->getCondition())) {
        if (FirstInst->getNextNonDebugInstruction() == BI) {
          return true;
        }
      }
    }

    return false;
  }

  /**
   * @brief checks if two loops are adjacent by checking that there are no
   * other basic blocks between the two loops, or in other words that the
   * exit block of the first loop coincides with the entry block of
   * the second loop
   *
   * @param L1
   * @param L2
   * @return true
   * @return false
   */
  bool areAdjacent(Loop *L1, Loop *L2) {

    BasicBlock *ExitL1 = getLoopExit(L1);
    BasicBlock *EntryL2 = getLoopEntry(L2);
    bool isGuarded = L1->isGuarded();

    if (!ExitL1 || !EntryL2)
      return false;

    // both must be guarded or unguarded
    if (isGuarded != L2->isGuarded())
      return false;

    if (isGuarded && !areConditionsEquivalent(L1->getLoopGuardBranch(),
                                              L2->getLoopGuardBranch())) {
      return false;
    }

    BranchInst *BI1 = dyn_cast<BranchInst>(ExitL1->getTerminator());
    BranchInst *BI2 = dyn_cast<BranchInst>(EntryL2->getTerminator());

    if (!BI1 || !BI2)
      return false;

    // first case, exit and entry correspond
    if (ExitL1 == EntryL2) {

      if (!isBlockEmpty(ExitL1, BI1, isGuarded)) {
        outs() << " -> ERROR: there are instructions between the loops.\n";
        return false;
      }

      return true;
    }

    // make sure that two loops are considered adjacent even with a
    // "trampoline" block in the middle
    if (!BI1->isUnconditional() || BI1->getSuccessor(0) != EntryL2)
      return false;

    if (!isBlockEmpty(ExitL1, BI1, false) ||
        !isBlockEmpty(EntryL2, BI2, isGuarded)) {
      outs() << " -> ERROR: there are instructions between the loops.\n";
      return false;
    }

    return true;
  }

  /**
   * @brief checks if two loops iterate the same number of times
   *
   * @param L1
   * @param L2
   * @return true
   * @return false
   */
  bool hasSameTripCount(Loop *L1, Loop *L2) {
    const SCEV *TC1 = loopsTripCountMap[L1];
    const SCEV *TC2 = loopsTripCountMap[L2];

    if (isa<SCEVCouldNotCompute>(TC1) || isa<SCEVCouldNotCompute>(TC2)) {
      outs() << "couldnt compute\n";
      return false;
    }

    return TC1 == TC2;
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

  bool areControlFlowEquivalent(Loop *L1, Loop *L2, DominatorTree &DT,
                                PostDominatorTree &PDT) {
    // TODO: we may need to add a different check for when both loops are
    // guarded -> this could be right

    // BasicBlock *Pre1 = L1->getLoopPreheader();
    // BasicBlock *Pre2 = L2->getLoopPreheader();

    auto Pre1 = getLoopEntry(L1);
    auto Pre2 = getLoopEntry(L2);

    // outs() << "Pre1: ";
    // Pre1->printAsOperand(outs());
    // outs() << "\nPre2: ";
    // Pre2->printAsOperand(outs());
    // outs() << "\n";
    if (!Pre1 || !Pre2) {
      return false;
    }

    return (DT.dominates(Pre1, Pre2) && PDT.dominates(Pre2, Pre1));
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
  bool hasNegativeDependencies(Loop *L1, Loop *L2, DependenceInfo &DI) {
    // TODO: I'm not even gonna read big dawg's code for ts, I'mma just hope ts
    // works well enough :pray: I've added a function for scalar dependencies,
    // but i'm still unsure if it should be used here

    // save operations that can change memory in some way to vectors
    std::vector<Instruction *> opsL1, opsL2;

    for (BasicBlock *BB : L1->getBlocks()) {
      for (Instruction &I : *BB)
        if (I.mayReadOrWriteMemory())
          opsL1.push_back(&I);
    }

    for (BasicBlock *BB : L2->getBlocks()) {
      for (Instruction &I : *BB)
        if (I.mayReadOrWriteMemory())
          opsL2.push_back(&I);
    }

    // if the vectors end up empty we can return without doing any check
    if (opsL1.empty() || opsL2.empty())
      return false;

    for (Instruction *I1 : opsL1) {
      for (Instruction *I2 : opsL2) {
        // checks if there's a negative distance dependency between the first
        // and second loops
        auto dep = DI.depends(I1, I2, true);
        if (!dep)
          continue;

        // if there's a conflict and the use of the instruction in L2
        // preceeds the use in L1 (checked with getDirection and GT (greater
        // than)) then we return true (as in it's true that there's a negative
        // distance dependency and the loops can't be fused)
        // if (dep->isConflicting()) { isConflicting doesn't exist ??
        if (dep->getDirection(1) == Dependence::DVEntry::GT) {
          return true;
        }
        //}
      }
    }

    return false;
  }

  /**
   * @brief checks for scalar dependencies between two loops
   *
   * @param L1
   * @param L2
   * @return true
   * @return false
   */
  bool hasScalarDependencies(Loop *L1, Loop *L2) {
    for (BasicBlock *BB : L1->getBlocks()) {
      for (Instruction &I : *BB) {
        for (User *U : I.users()) {
          if (Instruction *UserInst = dyn_cast<Instruction>(U)) {

            if (L2->contains(UserInst->getParent())) {
              return true;
            }
          }
        }
      }
    }
    return false;
  }
  /**
   * @brief updates the phi nodes modifying the label of OldPred with the
   * NewPred label
   *
   * @param TargetBB
   * @param OldPred
   * @param NewPred
   */
  void updatePhiNodes(BasicBlock *TargetBB, BasicBlock *OldPred,
                      BasicBlock *NewPred) {
    for (PHINode &PN : TargetBB->phis()) {
      int blockIndex = PN.getBasicBlockIndex(OldPred);
      if (blockIndex >= 0) {
        PN.setIncomingBlock(blockIndex, NewPred);
      }
    }
  }

  bool fuseLoops(Loop *L1, Loop *L2, LoopInfo &LI) {
    auto L1Header = L1->getHeader();
    auto L1HeaderTerminator = L1Header->getTerminator();
    // auto L1InductionVar = L1->getInductionVariable(SE); for some reason it
    // doesn't work
    // auto L1InductionVar = findInductionVariable(L1);
    auto L1InductionVar = L1->getCanonicalInductionVariable();
    auto L1Latch = L1->getLoopLatch();

    auto L2Header = L2->getHeader();
    auto L2HeaderTerminator = L2Header->getTerminator();
    auto L2ExitBlock = getLoopExit(L2);
    auto L2EntryBlock = getLoopEntry(L2);
    // auto L2InductionVar = L2->getInductionVariable(SE);
    // auto L2InductionVar = findInductionVariable(L2);
    auto L2InductionVar = L2->getCanonicalInductionVariable();
    auto L2Latch = L2->getLoopLatch();

    if (!L1InductionVar || !L2InductionVar) {
      return false;
    }

    std::vector<BasicBlock *> L1LatchPreds(predecessors(L1Latch).begin(),
                                           predecessors(L1Latch).end());
    std::vector<BasicBlock *> L2LatchPreds(predecessors(L2Latch).begin(),
                                           predecessors(L2Latch).end());

    BasicBlock *L2BodyEntry =
        (L2HeaderTerminator->getSuccessor(0) == L2ExitBlock)
            ? L2HeaderTerminator->getSuccessor(1)
            : L2HeaderTerminator->getSuccessor(0);

    // we manage the phi nodes present in the second header and the induction
    // var make_early_inc_range increments before, so we are sure to not
    // invalidate the pointer
    for (PHINode &PN : llvm::make_early_inc_range(L2Header->phis())) {
      if (&PN == L2InductionVar) {
        // induction var is replace by the one in the first loop
        PN.replaceAllUsesWith(L1InductionVar);
        PN.eraseFromParent();
      } else {
        // other phi nodes are moved in L1Header after the others
        Instruction *InsertPt = L1Header->getFirstNonPHI();
        PN.moveBefore(InsertPt);

        int entryIdx = PN.getBasicBlockIndex(L2EntryBlock);
        if (entryIdx >= 0) {
          PN.setIncomingBlock(entryIdx, getLoopEntry(L1));
        }

        int latchIdx = PN.getBasicBlockIndex(L2Latch);
        if (latchIdx >= 0) {
          PN.setIncomingBlock(latchIdx, L1Latch);
        }
      }
    }

    // the exit block of L1 is now the exit block of L2
    if (L1HeaderTerminator->getSuccessor(0) == L2EntryBlock) {
      L1HeaderTerminator->setSuccessor(0, L2ExitBlock);
    } else {
      L1HeaderTerminator->setSuccessor(1, L2ExitBlock);
    }
    updatePhiNodes(L2ExitBlock, L2Header, L1Header);

    // the blocks in L1 that pointed to the L1 Latch now go to the body of L2
    for (BasicBlock *PredL1 : L1LatchPreds) {
      auto predTerminator = PredL1->getTerminator();
      for (unsigned i = 0; i < predTerminator->getNumSuccessors(); i++) {
        if (predTerminator->getSuccessor(i) == L1Latch) {
          predTerminator->setSuccessor(i, L2BodyEntry);
          updatePhiNodes(L2BodyEntry, L2Header, PredL1);
        }
      }
    }

    // the blocks in L2 that pointed to the L2 Latch now go to the Latch of L1
    for (BasicBlock *PredL2 : L2LatchPreds) {
      auto predTerminator = PredL2->getTerminator();
      for (unsigned i = 0; i < predTerminator->getNumSuccessors(); i++) {
        if (predTerminator->getSuccessor(i) == L2Latch) {
          predTerminator->setSuccessor(i, L1Latch);
          for (BasicBlock *OldPredL1 : L1LatchPreds) {
            updatePhiNodes(L1Latch, OldPredL1, PredL2);
          }
        }
      }
    }
    std::vector<Loop *> SubLoops = L2->getSubLoopsVector();

    // should manage the subloops
    for (Loop *SubLoop : SubLoops) {
      L2->removeChildLoop(SubLoop);
      L1->addChildLoop(SubLoop);
    }

    /* Should move blocks that belong to L2 to L1, except the header and the
     latch (maybe
     * there are other BBs idk)
     WARNING: I'm not sure it updates correctly, it's a nightmare to debug ts*/
    std::vector<BasicBlock *> blocksToMove(L2->block_begin(), L2->block_end());
    for (BasicBlock *BB : blocksToMove) {
      if (BB != L2Header && BB != L2Latch) {
        L2->removeBlockFromLoop(BB);
        L1->addBasicBlockToLoop(BB, LI);
      }
    }

    if (Loop *ParentLoop = L2->getParentLoop())
      ParentLoop->removeChildLoop(L2);

    LI.erase(L2);
    return true;
  }

  bool fuseGuardedLoops(Loop *L1, Loop *L2, LoopInfo &LI) {
    auto L1Header = L1->getHeader();
    auto L1Latch = L1->getLoopLatch();
    auto L1Preheader = L1->getLoopPreheader();
    auto L1InductionVar = L1->getCanonicalInductionVariable();
    auto L1GuardBr = L1->getLoopGuardBranch();
    auto L1GuardBB = L1GuardBr->getParent();

    auto L2Header = L2->getHeader();
    auto L2Latch = L2->getLoopLatch();
    auto L2Preheader = L2->getLoopPreheader();
    auto L2InductionVar = L2->getCanonicalInductionVariable();
    auto L2GuardBr = L2->getLoopGuardBranch();
    auto L2GuardBB = L2GuardBr->getParent();

    if (!L1InductionVar || !L2InductionVar) {
      outs() << "Induction not found\n";
      return false;
    }

    // L'uscita globale di L2 (il blocco bypass)
    BasicBlock *L2Bypass = (L2GuardBr->getSuccessor(0) == L2Preheader)
                               ? L2GuardBr->getSuccessor(1)
                               : L2GuardBr->getSuccessor(0);

    BasicBlock *L2BodyEntry = L2Header;

    if (!L2BodyEntry) {
      outs() << "l2 body entry not found\n";
      return false;
    }
    // -----------------------------------------------

    std::vector<BasicBlock *> L1LatchPreds(predecessors(L1Latch).begin(),
                                           predecessors(L1Latch).end());
    std::vector<BasicBlock *> L2LatchPreds(predecessors(L2Latch).begin(),
                                           predecessors(L2Latch).end());

    // 1. Spostamento Nodi PHI
    for (PHINode &PN : llvm::make_early_inc_range(L2Header->phis())) {
      if (&PN == L2InductionVar) {
        PN.replaceAllUsesWith(L1InductionVar);
        PN.eraseFromParent();
      } else {
        Instruction *InsertPt = L1Header->getFirstNonPHI();
        PN.moveBefore(InsertPt);

        int entryIdx = PN.getBasicBlockIndex(L2Preheader);
        if (entryIdx >= 0) {
          PN.setIncomingBlock(entryIdx, L1Preheader);
        }

        int latchIdx = PN.getBasicBlockIndex(L2Latch);
        if (latchIdx >= 0) {
          PN.setIncomingBlock(latchIdx, L1Latch);
        }
      }
    }

    // Qualsiasi blocco che prima puntava alla guardia di L2 (sia che si tratti
    // del fallimento della guardia di L1, sia l'uscita normale di L1), ora
    // scavalca L2 e va dritto al bypass globale.
    std::vector<BasicBlock *> L2GuardPreds(predecessors(L2GuardBB).begin(),
                                           predecessors(L2GuardBB).end());
    for (BasicBlock *Pred : L2GuardPreds) {
      auto *Term = Pred->getTerminator();
      for (unsigned i = 0; i < Term->getNumSuccessors(); i++) {
        if (Term->getSuccessor(i) == L2GuardBB) {
          Term->setSuccessor(i, L2Bypass);
          updatePhiNodes(L2Bypass, L2GuardBB, Pred);
        }
      }
    }

    for (BasicBlock *PredL1 : L1LatchPreds) {
      auto predTerminator = PredL1->getTerminator();
      for (unsigned i = 0; i < predTerminator->getNumSuccessors(); i++) {
        if (predTerminator->getSuccessor(i) == L1Latch) {
          predTerminator->setSuccessor(i, L2BodyEntry);
          updatePhiNodes(L2BodyEntry, L2Header, PredL1);
        }
      }
    }

    for (BasicBlock *PredL2 : L2LatchPreds) {
      auto predTerminator = PredL2->getTerminator();
      for (unsigned i = 0; i < predTerminator->getNumSuccessors(); i++) {
        if (predTerminator->getSuccessor(i) == L2Latch) {
          predTerminator->setSuccessor(i, L1Latch);
          for (BasicBlock *OldPredL1 : L1LatchPreds) {
            updatePhiNodes(L1Latch, OldPredL1, PredL2);
          }
        }
      }
    }

    // 4. Aggiornamento LoopInfo e SubLoops
    std::vector<Loop *> SubLoops = L2->getSubLoopsVector();
    for (Loop *SubLoop : SubLoops) {
      L2->removeChildLoop(SubLoop);
      L1->addChildLoop(SubLoop);
    }

    std::vector<BasicBlock *> blocksToMove;
    for (BasicBlock *BB : L2->getBlocks()) {
      if (LI.getLoopFor(BB) == L2) {
        blocksToMove.push_back(BB);
      }
    }

    for (BasicBlock *BB : blocksToMove) {
      if (BB != L2Latch && BB != L2Preheader && BB != L2GuardBB) {
        L2->removeBlockFromLoop(BB);
        L1->addBasicBlockToLoop(BB, LI);
      }
    }

    if (Loop *ParentLoop = L2->getParentLoop())
      ParentLoop->removeChildLoop(L2);

    LI.erase(L2);
    return true;
  }
  bool processNestLevelLoops(std::vector<Loop *> &siblings, DominatorTree &DT,
                             PostDominatorTree &PDT, DependenceInfo &DI,
                             LoopInfo &LI, Function &F) {
    std::vector<Loop *> candidateLoops;
    bool fused = false;

    // filtering loops that are not candidate for LF
    for (Loop *L : siblings) {
      if (L->isLoopSimplifyForm()) {
        outs() << "loop inserito\n";
        candidateLoops.push_back(L);
      }
    }

    std::vector<std::vector<Loop *>> cfeGroups;
    if (candidateLoops.size() >= 2) {
      std::sort(candidateLoops.begin(), candidateLoops.end(),
                [&](Loop *L1, Loop *L2) { // cattura 'this' per getLoopEntry
                  return DT.dominates(getLoopEntry(L1), getLoopEntry(L2));
                });

      for (auto &loop : candidateLoops) {
        bool addedToGroup = false;

        for (auto &group : cfeGroups) {
          if (areControlFlowEquivalent(group.front(), loop, DT, PDT)) {
            group.push_back(loop);
            addedToGroup = true;
            break;
          } else {
            outs() << "non cfe\n";
          }
        }

        if (!addedToGroup) {
          cfeGroups.push_back({loop});
        }
      }
    }

    // debug output
    for (auto &group : cfeGroups) {
      if (group.size() >= 2) {
        outs() << "found CFE with size " << group.size() << "\n";
      }
    }

    for (auto &group : cfeGroups) {
      int baseIndex = 0;
      auto &baseLoop = group[baseIndex];
      while (group.size() >= 2 && baseIndex < group.size() - 1) {
        auto nextLoop = group[baseIndex + 1];

        if (!areAdjacent(baseLoop, nextLoop)) {
          outs() << " -> FALLITO: Non sono adiacenti\n";
          baseIndex++;
          baseLoop = group[baseIndex];
          continue;
        }

        if (!hasSameTripCount(baseLoop, nextLoop)) {
          outs() << " -> FALLITO: Trip count diverso o SCEVCouldNotCompute\n";
          baseIndex++;
          baseLoop = group[baseIndex];
          continue;
        }

        if (hasNegativeDependencies(baseLoop, nextLoop, DI)) {
          outs() << " -> FALLITO: Dipendenze negative trovate\n";
          baseIndex++;
          baseLoop = group[baseIndex];
          continue;
        }

        if (hasScalarDependencies(baseLoop, nextLoop)) {
          outs() << " -> FALLITO: Dipendenze scalari trovate\n";
          baseIndex++;
          baseLoop = group[baseIndex];
          continue;
        }

        outs() << " -> Tutti i check passati! Tento la fusione...\n";
        bool fusionSuccess = false;
        if (baseLoop->isGuarded()) {
          outs() << "Guarded Fusion\n";
          fusionSuccess = fuseGuardedLoops(baseLoop, nextLoop, LI);
        } else {
          outs() << "Normal Fusion\n";
          fusionSuccess = fuseLoops(baseLoop, nextLoop, LI);
        }
        if (fusionSuccess) {
          outs() << " -> FUSIONE AVVENUTA CON SUCCESSO!\n";
          group.erase(group.begin() + baseIndex + 1);
          fused = true;
          removeUnreachableBlocks(F);

          siblings.erase(
              std::remove(siblings.begin(), siblings.end(), nextLoop),
              siblings.end());

          DT.recalculate(F);
          PDT.recalculate(F);
        } else {
          outs() << " -> FUSIONE ABORTITA\n";
          baseIndex++;
          baseLoop = group[baseIndex];
        }
      }
    }

    // TODO: vhat is ts commend bradar delet ts
    //  TODO: do checks for each pair of loops in each group (groups of only one
    //  loop are excluded) and fuse if possible

    // we explore the next nest level for each loop (in case of fusion both the
    // domTree and LoopAnalysis must be updated)
    // if (changed) {
    //   DT.recalculate(F);
    //   PDT.recalculate(F);
    //   SE.forgetAllLoops();
    // }

    bool childrenFused = false;
    for (Loop *L : siblings) {
      std::vector<Loop *> children = L->getSubLoopsVector();
      if (processNestLevelLoops(children, DT, PDT, DI, LI, F)) {
        childrenFused = true;
      }
    }

    return fused || childrenFused;
  }

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM) {
    LoopInfo &LI = AM.getResult<LoopAnalysis>(F);

    ScalarEvolution &SE = AM.getResult<ScalarEvolutionAnalysis>(F);

    DominatorTree &DT = AM.getResult<DominatorTreeAnalysis>(F);
    PostDominatorTree &PDT = AM.getResult<PostDominatorTreeAnalysis>(F);

    DependenceInfo &DI = AM.getResult<DependenceAnalysis>(F);

    for (auto L : LI.getLoopsInPreorder()) {
      auto backedgeLoop = SE.getBackedgeTakenCount(L);
      loopsTripCountMap[L] = backedgeLoop;
      // L->getCanonicalInductionVariable()->printAsOperand(outs());
      // outs() << "\n";
    }

    // getTopLevelLoops() iterates from the last loop to the first
    bool changed =
        processNestLevelLoops(LI.getTopLevelLoopsVector(), DT, PDT, DI, LI, F);

    return (changed ? PreservedAnalyses::none() : PreservedAnalyses::all());
  }

  static bool isRequired() { return true; }
};
} // namespace

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
