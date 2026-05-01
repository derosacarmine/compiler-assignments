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

    /*
    Per creare un DominatorTree mi devo basare sui dominatori immediati;
    il dominatore immediato di un nodo è il più basso che lo precede in cui 2 predecessori puntano
    Hai due nodi A e B nell'albero, e vuoi trovare il loro antenato comune più basso,
    basta che io risalga gli idom dei due nodi contemporaneamente finchè non trovo corrispondenza, in sto caso 
    risalgo pred e idomBB che sarebbe solo che per fare si che avvenga contemporaneamente la risalita devo controllare il livello;
    Poi i back edge li salto perchè sono gli archi che nel CFG vanno indietro verso un nodo già visitato, arrivando dall'alto rischio che 
    cerco di risalire l'idom di un nodo che non esiste ancora nell'albero e crashi.
     */

    void buildDomTree(Function &F, DominatorTree *DT) {
            
            // root
            BasicBlock* entry = &F.getEntryBlock();
            DT->addNewBlock(entry, nullptr);
            
            //cioè dall'alto verso il basso banalmente
            ReversePostOrderTraversal<Function*> RPOT(&F);
            int num_lvl = 0;
            std::map<BasicBlock*, int> post_order_num;
            for (BasicBlock* BB : RPOT) {
                post_order_num[BB] = num_lvl++;
            }

            for (BasicBlock* BB : RPOT) {

                if (BB == entry) continue;  // già aggiunto
                
                BasicBlock* idomBB = nullptr;
                
                for (BasicBlock* pred : predecessors(BB)) {
                    // se il predecessore non ha ancora un nodo nell'albero saltalo
                    // (back edge di un loop)
                    if (!DT->getNode(pred)) continue;
                    
                    if (idomBB == nullptr) {
                        idomBB = pred;  // primo predecessore processato
                    } else {
                        //trovo il dominatore immediato
                        
                        while(idomBB != pred){
                            if(post_order_num[idomBB] > post_order_num[pred])
                                idomBB = DT->getNode(idomBB)->getIDom()->getBlock();
                            else
                                pred = DT->getNode(pred)->getIDom()->getBlock();

                        }

                    }
                }
                
                DT->addNewBlock(BB, idomBB);
            }
            
            
        }

        /* Dato che devo trovare i back edge poichè sono quelli che formano i Loop, sfrutto il mio DT creato;
            un back edge si ha quando il terminatore di un basic block ha come successore un basic block che lo domina
         */
         /*
        std::map<BasicBlock*, std::vector<BasicBlock*>> buildLoop(Function &F, DominatorTree *DT){
            std::map<BasicBlock*, std::vector<BasicBlock*>> loop;
            
            for (BasicBlock *BB : F){
                for(BasicBlock *succ : successors(BB)){
                    if(DT->dominates(succ, BB)){
                        // succ è l'header, BB è il latch(ovvero i BB che hanno direttamente il back edge verso la header)
                        // visita a ritroso nel CFG per trovare tutti i BB del loop
                        //tutti i nodi raggiungibili risalendo i predecessori partendo dal latch, fino ad arrivare all'header, fanno parte del loop
                        //metti il latch nella worklist, poi per ogni nodo che estrai aggiungi i suoi predecessori 
                        // se non li hai già visitati e se non sono l'header
                        std::vector<BasicBlock*> worklist;
                        std::set<BasicBlock*> visited;
                        
                        worklist.push_back(BB);
                        visited.insert(BB);
                        visited.insert(succ); // l'header la aggiungiamo dopo
                        
                        while(!worklist.empty()){
                            BasicBlock *current = worklist.back();
                            worklist.pop_back();
                            loop[succ].push_back(current);
                            
                            for(BasicBlock *pred : predecessors(current)){
                                if(visited.find(pred) == visited.end()){
                                    visited.insert(pred);
                                    worklist.push_back(pred);
                                }
                            }
                        }
                        loop[succ].push_back(succ); // aggiungo l'header
                    }
                }
            }
            return loop;
        }
        */

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

        //LoopInfo &LI = AM.getResult<LoopAnalysis>(F);
        //auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
        DominatorTree DT;
        buildDomTree(F, &DT);

        LoopInfo &LI = LI.analyze(DT);  //i cerca nel CFG originale della funzione, usando il DT solo per verificare se un edge è effettivamente un back edge

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
