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
        std::set<BasicBlock*> visited; // Fondamentale per evitare il bug dell'intersezione con blocchi non visitati

        BasicBlock *Entry = &F.getEntryBlock();

        bool changes = true;
        while (changes) {
            changes = false;

            for (BasicBlock &BB : F) {
                ConstantEnv currentIn;

                // --- MEET OPERATION ---
                if (&BB != Entry) {
                    bool first = true;
                    for (BasicBlock *P : predecessors(&BB)) {
                        // TRUCCO VITALE: Ignoriamo i predecessori non ancora calcolati.
                        // Questo simula l'inizializzazione al set "Universale" o "Top".
                        if (visited.find(P) == visited.end()) continue;

                        if (first) {
                            currentIn = Out[P];
                            first = false;
                        } else {
                            // Intersezione: tieni la variabile solo se esiste in entrambi con lo STESSO valore
                            for (auto it = currentIn.begin(); it != currentIn.end(); ) {
                                if (Out[P].find(it->first) == Out[P].end() || 
                                    Out[P].at(it->first)->getValue() != it->second->getValue()) {
                                    it = currentIn.erase(it);
                                } else {
                                    ++it;
                                }
                            }
                        }
                    }
                }
                In[&BB] = currentIn;

                // --- TRANSFER FUNCTION ---
                // In SSA non c'è Kill. Tutto ciò che entra, esce (OUT parte identico a IN).
                ConstantEnv currentOut = currentIn; 
                
                for (Instruction &I : BB) {
                    // 1. Valutazione Matematica (Add, Sub, Mul, SDiv)
                    if (auto *BinOp = dyn_cast<BinaryOperator>(&I)) {
                        Value *op1 = BinOp->getOperand(0);
                        Value *op2 = BinOp->getOperand(1);

                        ConstantInt *c1 = dyn_cast<ConstantInt>(op1);
                        if (!c1 && currentOut.count(op1)) c1 = currentOut[op1];

                        ConstantInt *c2 = dyn_cast<ConstantInt>(op2);
                        if (!c2 && currentOut.count(op2)) c2 = currentOut[op2];

                        if (c1 && c2) {
                            if (BinOp->getOpcode() == Instruction::Add) {
                                currentOut[&I] = ConstantInt::get(I.getContext(), c1->getValue() + c2->getValue());
                            } else if (BinOp->getOpcode() == Instruction::Sub) {
                                currentOut[&I] = ConstantInt::get(I.getContext(), c1->getValue() - c2->getValue());
                            } else if (BinOp->getOpcode() == Instruction::Mul) {
                                // AGGIUNTO: Supporto per la moltiplicazione!
                                currentOut[&I] = ConstantInt::get(I.getContext(), c1->getValue() * c2->getValue());
                            } else if (BinOp->getOpcode() == Instruction::SDiv) {
                                // AGGIUNTO: Divisione con controllo per evitare crash (divisione per zero)
                                if (c2->getValue() != 0) {
                                    currentOut[&I] = ConstantInt::get(I.getContext(), c1->getValue().sdiv(c2->getValue()));
                                }
                            }
                        }
                    }
                    // 1.5 Valutazione delle Comparazioni (es. icmp eq i32 4, 4)
                    else if (auto *Cmp = dyn_cast<ICmpInst>(&I)) {
                        Value *op1 = Cmp->getOperand(0);
                        Value *op2 = Cmp->getOperand(1);

                        ConstantInt *c1 = dyn_cast<ConstantInt>(op1);
                        if (!c1 && currentOut.count(op1)) c1 = currentOut[op1];

                        ConstantInt *c2 = dyn_cast<ConstantInt>(op2);
                        if (!c2 && currentOut.count(op2)) c2 = currentOut[op2];

                        if (c1 && c2) {
                            bool isTrue = false;
                            switch (Cmp->getPredicate()) {
                                case CmpInst::ICMP_EQ:  isTrue = (c1->getValue() == c2->getValue()); break;
                                case CmpInst::ICMP_NE:  isTrue = (c1->getValue() != c2->getValue()); break;
                                case CmpInst::ICMP_SGT: isTrue = (c1->getValue().sgt(c2->getValue())); break;
                                case CmpInst::ICMP_SLT: isTrue = (c1->getValue().slt(c2->getValue())); break;
                                default: break; 
                            }
                            // i1 è il tipo booleano in LLVM (intero a 1 bit)
                            currentOut[&I] = ConstantInt::get(Type::getInt1Ty(I.getContext()), isTrue);
                        }
                    }
                    // 2. Valutazione dei Nodi PHI
                    else if (auto *Phi = dyn_cast<PHINode>(&I)) {
                        ConstantInt *commonConst = nullptr;
                        bool isConstant = true;
                        
                        for (Value *incVal : Phi->incoming_values()) {
                            ConstantInt *c = dyn_cast<ConstantInt>(incVal);
                            if (!c && currentOut.count(incVal)) c = currentOut[incVal];
                            
                            if (!c) { 
                                isConstant = false; 
                                break; 
                            }
                            
                            if (!commonConst) {
                                commonConst = c;
                            } else if (commonConst->getValue() != c->getValue()) { 
                                isConstant = false; 
                                break; 
                            }
                        }
                        
                        if (isConstant && commonConst) {
                            currentOut[&I] = commonConst;
                        }
                    }
                }

                // Controllo di convergenza
                if (currentOut != Out[&BB]) {
                    Out[&BB] = std::move(currentOut);
                    changes = true;
                }
                
                visited.insert(&BB);
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