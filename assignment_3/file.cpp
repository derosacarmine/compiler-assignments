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

#include <llvm-19/llvm/IR/DominatorTree.h>
#include <llvm-19/llvm/Analysis/LoopInfo.h>

#include <functional>
#include <map>
#include <set>
#include <vector>
#include <utility>

