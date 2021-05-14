//===- StandardToHandshake.cpp - Convert standard MLIR into dataflow IR ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===----------------------------------------------------------------------===//
//
// This is the main Standard to Handshake Conversion Pass Implementation.
//
//===----------------------------------------------------------------------===//

#include "circt/Conversion/StandardToHandshake/StandardToHandshake.h"
#include "../PassDetail.h"
#include "circt/Dialect/Handshake/HandshakeOps.h"
#include "circt/Dialect/StaticLogic/StaticLogic.h"
#include "mlir/Analysis/AffineAnalysis.h"
#include "mlir/Analysis/AffineStructures.h"
#include "mlir/Conversion/AffineToStandard/AffineToStandard.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Affine/IR/AffineValueMap.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/Types.h"
#include "mlir/IR/Value.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Support/LLVM.h"
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/Transforms/Passes.h"
#include "mlir/Transforms/Utils.h"
#include "llvm/Support/raw_ostream.h"
#include <map>

using namespace mlir;
using namespace circt;
using namespace circt::handshake;
using namespace std;

typedef DenseMap<Block *, vector<Value>> BlockValues;
typedef DenseMap<Block *, vector<Operation *>> BlockOps;
typedef DenseMap<Value, Operation *> BlockArgPairs;

/// Remove basic blocks inside the given FuncOp. This allows the result to be
/// a valid graph region, since multi-basic block regions are not allowed to
/// be graph regions currently.
void removeBasicBlocks(handshake::FuncOp funcOp) {
  auto &entryBlock = funcOp.getBody().front().getOperations();

  // Erase all TerminatorOp, and move ReturnOp to the end of entry block.
  for (auto &block : funcOp) {
    Operation &termOp = block.back();
    if (isa<handshake::TerminatorOp>(termOp))
      termOp.erase();
    else if (isa<handshake::ReturnOp>(termOp))
      entryBlock.splice(entryBlock.end(), block.getOperations(), termOp);
  }

  // Move all operations to entry block and erase other blocks.
  for (auto &block : llvm::make_early_inc_range(llvm::drop_begin(funcOp, 1))) {
    entryBlock.splice(--entryBlock.end(), block.getOperations());
  }
  for (auto &block : llvm::make_early_inc_range(llvm::drop_begin(funcOp, 1))) {
    block.clear();
    block.dropAllDefinedValueUses();
    for (size_t i = 0; i < block.getNumArguments(); i++) {
      block.eraseArgument(i);
    }
    block.erase();
  }
}

template <typename FuncOp>
void dotPrint(FuncOp f, string name) {
  // Prints DOT representation of the dataflow graph, used for debugging.
  DenseMap<Block *, unsigned> blockIDs;
  DenseMap<Operation *, unsigned> opIDs;
  unsigned i = 0;
  unsigned j = 0;

  for (Block &block : f) {
    blockIDs[&block] = i++;
    for (Operation &op : block)
      opIDs[&op] = j++;
  }

  std::error_code ec;
  llvm::raw_fd_ostream outfile(name + ".dot", ec);

  outfile << "Digraph G {\n\tsplines=spline;\n";

  for (Block &block : f) {
    outfile << "\tsubgraph cluster_" + to_string(blockIDs[&block]) + " {\n";
    outfile << "\tcolor = \"darkgreen\";\n";
    outfile << "\t\tlabel = \" block " + to_string(blockIDs[&block]) + "\";\n";

    for (Operation &op : block) {
      outfile << "\t\t";
      outfile << "\"" + op.getName().getStringRef().str() + "_" +
                     to_string(opIDs[&op]) + "\"";
      outfile << "\n";
    }

    for (Operation &op : block) {
      if (op.getNumResults() > 0) {
        for (auto result : op.getResults()) {
          for (auto &u : result.getUses()) {
            Operation *useOp = u.getOwner();
            if (useOp->getBlock() == &block) {
              outfile << "\t\t";
              outfile << "\"" + op.getName().getStringRef().str() + "_" +
                             to_string(opIDs[&op]) + "\"";
              outfile << " -> ";
              outfile << "\"" + useOp->getName().getStringRef().str() + "_" +
                             to_string(opIDs[useOp]) + "\"";
              outfile << "\n";
            }
          }
        }
      }
    }

    outfile << "\t}\n";

    for (Operation &op : block) {
      if (op.getNumResults() > 0) {
        for (auto result : op.getResults()) {
          for (auto &u : result.getUses()) {
            Operation *useOp = u.getOwner();
            if (useOp->getBlock() != &block) {
              outfile << "\t\t";
              outfile << "\"" + op.getName().getStringRef().str() + "_" +
                             to_string(opIDs[&op]) + "\"";
              outfile << " -> ";
              outfile << "\"" + useOp->getName().getStringRef().str() + "_" +
                             to_string(opIDs[useOp]) + "\"";
              outfile << " [minlen = 3]\n";
            }
          }
        }
      }
    }
  }

  outfile << "\t}\n";
  outfile.close();
}

void setControlOnlyPath(handshake::FuncOp f,
                        ConversionPatternRewriter &rewriter) {
  // Creates start and end points of the control-only path

  // Temporary start node (removed in later steps) in entry block
  Block *entryBlock = &f.front();
  rewriter.setInsertionPointToStart(entryBlock);
  Operation *startOp = rewriter.create<StartOp>(entryBlock->front().getLoc());

  // Replace original return ops with new returns with additional control input
  for (Block &block : f) {
    Operation *termOp = block.getTerminator();
    if (isa<mlir::ReturnOp>(termOp)) {

      rewriter.setInsertionPoint(termOp);

      // Remove operands from old return op and add them to new op
      SmallVector<Value, 8> operands(termOp->getOperands());
      for (int i = 0, e = termOp->getNumOperands(); i < e; ++i)
        termOp->eraseOperand(0);
      assert(termOp->getNumOperands() == 0);
      operands.push_back(startOp->getResult(0));
      rewriter.replaceOpWithNewOp<handshake::ReturnOp>(termOp, operands);
    }
  }
}

BlockValues getBlockUses(handshake::FuncOp f) {
  // Returns map of values used in block but defined outside of block
  // For liveness analysis
  BlockValues uses;

  for (Block &block : f) {

    // Operands of operations in b which do not originate from operations or
    // arguments of b
    for (Operation &op : block) {
      for (int i = 0, e = op.getNumOperands(); i < e; ++i) {

        Block *operandBlock;

        if (op.getOperand(i).isa<BlockArgument>()) {
          // Operand is block argument, get its owner block
          operandBlock = op.getOperand(i).cast<BlockArgument>().getOwner();
        } else {
          // Operand comes from operation, get the block of its defining op
          auto *operand = op.getOperand(i).getDefiningOp();
          assert(operand != NULL);
          operandBlock = operand->getBlock();
        }

        // If operand defined in some other block, add to uses
        if (operandBlock != &block)
          // Add only unique uses
          if (std::find(uses[&block].begin(), uses[&block].end(),
                        op.getOperand(i)) == uses[&block].end())
            uses[&block].push_back(op.getOperand(i));
      }
    }
  }
  return uses;
}

BlockValues getBlockDefs(handshake::FuncOp f) {
  // Returns map of values defined in each block
  // For liveness analysis
  BlockValues defs;

  for (Block &block : f) {

    // Values produced by operations in b
    for (Operation &op : block) {
      if (op.getNumResults() > 0) {
        for (auto result : op.getResults())
          defs[&block].push_back(result);
      }
    }

    // Argument values of b
    for (auto &arg : block.getArguments())
      defs[&block].push_back(arg);
  }

  return defs;
}

vector<Value> vectorUnion(vector<Value> v1, vector<Value> v2) {
  // Returns v1 U v2
  // Assumes unique values in v1

  for (int i = 0, e = v2.size(); i < e; ++i) {
    Value val = v2[i];
    if (std::find(v1.begin(), v1.end(), val) == v1.end())
      v1.push_back(val);
  }
  return v1;
}

vector<Value> vectorDiff(vector<Value> v1, vector<Value> v2) {
  // Returns v1 - v2
  vector<Value> d;

  for (int i = 0, e = v1.size(); i < e; ++i) {
    Value val = v1[i];
    if (std::find(v2.begin(), v2.end(), val) == v2.end())
      d.push_back(val);
  }
  return d;
}

BlockValues livenessAnalysis(handshake::FuncOp f) {
  // Liveness analysis algorithm adapted from:
  // https://suif.stanford.edu/~courses/cs243/lectures/l2.pdf
  // See slide 19 (Liveness: Iterative Algorithm)

  // blockUses: values used in block but not defined in block
  BlockValues blockUses = getBlockUses(f);

  // blockDefs: values defined in block
  BlockValues blockDefs = getBlockDefs(f);

  BlockValues blockLiveIns;

  bool change = true;
  // Iterate while there are any changes to any of the livein sets
  while (change) {
    change = false;

    // liveOuts(b): all liveins of all successors of b
    // liveOuts(b) = U (blockLiveIns(s)) forall successors s of b
    for (Block &block : f) {
      vector<Value> liveOuts;
      for (int i = 0, e = block.getNumSuccessors(); i < e; ++i) {
        Block *succ = block.getSuccessor(i);
        liveOuts = vectorUnion(liveOuts, blockLiveIns[succ]);
      }

      // diff(b):  liveouts of b which are not defined in b
      // diff(b) = liveOuts(b) - blockDefs(b)
      vector<Value> diff = vectorDiff(liveOuts, blockDefs[&block]);

      // liveIns(b) = blockUses(b) U diff(b)
      vector<Value> tmpLiveIns = vectorUnion(blockUses[&block], diff);

      // Update blockLiveIns if new liveins found
      if (tmpLiveIns.size() > blockLiveIns[&block].size()) {
        blockLiveIns[&block] = tmpLiveIns;
        change = true;
      }
    }
  }

  return blockLiveIns;
}

unsigned getBlockPredecessorCount(Block *block) {
  // Returns number of block predecessors
  auto predecessors = block->getPredecessors();
  return std::distance(predecessors.begin(), predecessors.end());
}

// Insert appropriate type of Merge CMerge for control-only path,
// Merge for single-successor blocks, Mux otherwise
Operation *insertMerge(Block *block, Value val,
                       ConversionPatternRewriter &rewriter) {
  unsigned numPredecessors = getBlockPredecessorCount(block);

  // Control-only path originates from StartOp
  if (!val.isa<BlockArgument>()) {
    if (isa<StartOp>(val.getDefiningOp())) {
      return rewriter.create<handshake::ControlMergeOp>(block->front().getLoc(),
                                                        val, numPredecessors);
    }
  }

  // If there are no block predecessors (i.e., entry block), function argument
  // is set as single operand
  if (numPredecessors <= 1)
    return rewriter.create<handshake::MergeOp>(block->front().getLoc(), val, 1);

  return rewriter.create<handshake::MuxOp>(block->front().getLoc(), val,
                                           numPredecessors);
}

// Adds Merge for every live-in and block argument
// Returns DenseMap of all inserted operations
BlockOps insertMergeOps(handshake::FuncOp f, BlockValues blockLiveIns,
                        BlockArgPairs &mergePairs,
                        ConversionPatternRewriter &rewriter) {
  BlockOps blockMerges;

  for (Block &block : f) {
    // Live-ins identified by liveness analysis
    rewriter.setInsertionPointToStart(&block);

    for (auto &val : blockLiveIns[&block]) {
      Operation *newOp = insertMerge(&block, val, rewriter);
      blockMerges[&block].push_back(newOp);
      mergePairs[val] = newOp;
    }

    // Block arguments are not in livein list as they are defined inside the
    // block
    for (auto &arg : block.getArguments()) {
      Operation *newOp = insertMerge(&block, arg, rewriter);
      blockMerges[&block].push_back(newOp);
      mergePairs[arg] = newOp;
    }
  }

  return blockMerges;
}

// Check if block contains operation which produces val
bool blockHasSrcOp(Value val, Block *block) {
  // Arguments do not have an operation producer
  if (val.isa<BlockArgument>())
    return false;

  auto *op = val.getDefiningOp();
  assert(op != NULL);

  return (op->getBlock() == block);
}

// Get value from predBlock which will be set as operand of op (merge)
Value getMergeOperand(Operation *op, Block *predBlock, BlockOps blockMerges) {
  // Helper value (defining value of merge) to identify Merges which propagate
  // the same defining value
  Value srcVal = op->getOperand(0);

  Block *block = op->getBlock();

  // Value comes from predecessor block (i.e., not an argument of this block)
  if (std::find(block->getArguments().begin(), block->getArguments().end(),
                srcVal) == block->getArguments().end()) {
    // Value is not defined by operation in predBlock
    if (!blockHasSrcOp(srcVal, predBlock)) {
      // Find the corresponding Merge
      for (Operation *predOp : blockMerges[predBlock])
        if (predOp->getOperand(0) == srcVal)
          return predOp->getResult(0);
    } else
      return srcVal;
  }

  // Value is argument of this block
  // Operand of terminator in predecessor block should be input to Merge
  else {
    unsigned index = srcVal.cast<BlockArgument>().getArgNumber();
    Operation *termOp = predBlock->getTerminator();
    if (mlir::CondBranchOp br = dyn_cast<mlir::CondBranchOp>(termOp)) {
      if (block == br.getTrueDest())
        return br.getTrueOperand(index);
      else {
        assert(block == br.getFalseDest());
        return br.getFalseOperand(index);
      }
    } else if (isa<mlir::BranchOp>(termOp))
      return termOp->getOperand(index);
  }
  return nullptr;
}

void removeBlockOperands(handshake::FuncOp f) {
  // Remove all block arguments, they are no longer used
  // eraseArguments also removes corresponding branch operands
  for (Block &block : f) {
    if (!block.isEntryBlock()) {
      int x = block.getNumArguments() - 1;
      for (int i = x; i >= 0; --i)
        block.eraseArgument(i);
    }
  }
}

Operation *getControlMerge(Block *block) {
  // Returns CMerge of block
  for (Operation &op : *block) {
    if (isa<ControlMergeOp>(op)) {
      return &op;
    }
  }
  return nullptr;
}

Operation *getStartOp(Block *block) {
  // Returns CMerge of block
  for (Operation &op : *block) {
    if (isa<StartOp>(op)) {
      return &op;
    }
  }
  return nullptr;
}

void reconnectMergeOps(handshake::FuncOp f, BlockOps blockMerges,
                       BlockArgPairs &mergePairs) {
  // All merge operands are initially set to original (defining) value
  // We here replace defining value with appropriate value from predecessor
  // block The predecessor can either be a merge, the original defining value,
  // or a branch Operand Operand(0) is helper defining value for identifying
  // matching merges, it does not correspond to any predecessor block

  for (Block &block : f) {
    for (Operation *op : blockMerges[&block]) {

      int count = 1;

      // Set appropriate operand from predecessor block
      for (auto *predBlock : block.getPredecessors()) {
        Value mgOperand = getMergeOperand(op, predBlock, blockMerges);
        assert(mgOperand != nullptr);
        if (!mgOperand.getDefiningOp()) {
          assert(mergePairs.count(mgOperand));
          mgOperand = mergePairs[mgOperand]->getResult(0);
        }
        op->setOperand(count, mgOperand);
        count++;
      }
      // Reconnect all operands originating from livein defining value through
      // corresponding merge of that block
      for (Operation &opp : block)
        if (!isa<MergeLikeOpInterface>(opp))
          opp.replaceUsesOfWith(op->getOperand(0), op->getResult(0));
    }
  }

  // Disconnect original value (Operand(0), used as helper) from all merges
  // If the original value must be a merge operand, it is still set as some
  // subsequent operand
  // If block has multiple predecessors, connect Muxes to ControlMerge
  for (Block &block : f) {
    unsigned numPredecessors = getBlockPredecessorCount(&block);

    if (numPredecessors <= 1) {
      for (Operation *op : blockMerges[&block])
        op->eraseOperand(0);
    } else {
      Operation *cntrlMg = getControlMerge(&block);
      assert(cntrlMg != nullptr);
      cntrlMg->eraseOperand(0);

      for (Operation *op : blockMerges[&block])
        if (op != cntrlMg)
          op->setOperand(0, cntrlMg->getResult(1));
    }
  }

  removeBlockOperands(f);
}
void addMergeOps(handshake::FuncOp f, ConversionPatternRewriter &rewriter) {

  BlockArgPairs mergePairs;

  // blockLiveIns: live in variables of block
  BlockValues liveIns = livenessAnalysis(f);

  // Insert merge operations
  BlockOps mergeOps = insertMergeOps(f, liveIns, mergePairs, rewriter);

  // Set merge operands and uses
  reconnectMergeOps(f, mergeOps, mergePairs);
}

bool isLiveOut(Value val) {
  // Identifies liveout values after adding Merges
  for (auto &u : val.getUses())
    // Result is liveout if used by some Merge block
    if (isa<MergeLikeOpInterface>(u.getOwner()))
      return true;
  return false;
}

// A value can have multiple branches in a single successor block
// (for instance, there can be an SSA phi and a merge that we insert)
// This function determines the number of branches to insert based on the
// value uses in successor blocks
int getBranchCount(Value val, Block *block) {
  int uses = 0;
  for (int i = 0, e = block->getNumSuccessors(); i < e; ++i) {
    int curr = 0;
    Block *succ = block->getSuccessor(i);
    for (auto &u : val.getUses()) {
      if (u.getOwner()->getBlock() == succ)
        curr++;
    }
    uses = (curr > uses) ? curr : uses;
  }
  return uses;
}

// Return the appropriate branch result based on successor block which uses it
Value getSuccResult(Operation *termOp, Operation *newOp, Block *succBlock) {
  // For conditional block, check if result goes to true or to false successor
  if (auto condBranchOp = dyn_cast<mlir::CondBranchOp>(termOp)) {
    if (condBranchOp.getTrueDest() == succBlock)
      return dyn_cast<handshake::ConditionalBranchOp>(newOp).trueResult();
    else {
      assert(condBranchOp.getFalseDest() == succBlock);
      return dyn_cast<handshake::ConditionalBranchOp>(newOp).falseResult();
    }
  }
  // If the block is unconditional, newOp has only one result
  return newOp->getResult(0);
}

void addBranchOps(handshake::FuncOp f, ConversionPatternRewriter &rewriter) {

  BlockValues liveOuts;

  for (Block &block : f) {
    for (Operation &op : block) {
      for (auto result : op.getResults())
        if (isLiveOut(result))
          liveOuts[&block].push_back(result);
    }
  }

  for (Block &block : f) {
    Operation *termOp = block.getTerminator();
    rewriter.setInsertionPoint(termOp);

    for (Value val : liveOuts[&block]) {
      // Count the number of branches which the liveout needs
      int numBranches = getBranchCount(val, &block);

      // Instantiate branches and connect to Merges
      for (int i = 0, e = numBranches; i < e; ++i) {
        Operation *newOp = nullptr;

        if (auto condBranchOp = dyn_cast<mlir::CondBranchOp>(termOp))
          newOp = rewriter.create<handshake::ConditionalBranchOp>(
              termOp->getLoc(), condBranchOp.getCondition(), val);
        else if (isa<mlir::BranchOp>(termOp))
          newOp = rewriter.create<handshake::BranchOp>(termOp->getLoc(), val);

        if (newOp != nullptr) {
          for (int i = 0, e = block.getNumSuccessors(); i < e; ++i) {
            Block *succ = block.getSuccessor(i);
            Value res = getSuccResult(termOp, newOp, succ);

            for (auto &u : val.getUses()) {
              if (u.getOwner()->getBlock() == succ) {
                u.getOwner()->replaceUsesOfWith(val, res);
                break;
              }
            }
          }
        }
      }
    }
  }

  // Remove StandardOp branch terminators and place new terminator
  // Should be removed in some subsequent pass (we only need it to pass checks
  // in Verifier.cpp)
  for (Block &block : f) {
    Operation *termOp = block.getTerminator();
    if (isa<mlir::CondBranchOp>(termOp) || isa<mlir::BranchOp>(termOp)) {
      SmallVector<mlir::Block *, 8> results(block.getSuccessors());
      rewriter.setInsertionPointToEnd(&block);
      rewriter.create<handshake::TerminatorOp>(termOp->getLoc(), results);

      // Remove the Operands to keep the single-use rule.
      for (int i = 0, e = termOp->getNumOperands(); i < e; ++i)
        termOp->eraseOperand(0);
      assert(termOp->getNumOperands() == 0);
      rewriter.eraseOp(termOp);
    }
  }
}

// Create sink for every unused result
void addSinkOps(handshake::FuncOp f, OpBuilder &rewriter) {
  BlockValues liveOuts;

  for (Block &block : f) {
    for (Operation &op : block) {
      // Do not add sinks for unused MLIR operations which the rewriter will
      // later remove We have already replaced these ops with their handshake
      // equivalents
      // TODO: should we use other indicator for op that has been erased?
      if (!isa<mlir::CondBranchOp, mlir::BranchOp, memref::LoadOp,
               mlir::AffineReadOpInterface, mlir::AffineForOp>(op)) {

        if (op.getNumResults() > 0) {
          for (auto result : op.getResults())
            if (result.use_empty()) {
              rewriter.setInsertionPointAfter(&op);
              rewriter.create<SinkOp>(op.getLoc(), result);
            }
        }
      }
    }
  }
}

void connectConstantsToControl(handshake::FuncOp f,
                               ConversionPatternRewriter &rewriter) {
  // Create new constants which have a control-only input to trigger them
  // Connect input to ControlMerge (trigger const when its block is entered)

  for (Block &block : f) {
    Operation *cntrlMg =
        block.isEntryBlock() ? getStartOp(&block) : getControlMerge(&block);
    assert(cntrlMg != nullptr);
    vector<Operation *> cstOps;
    for (Operation &op : block) {
      if (auto constantOp = dyn_cast<mlir::ConstantOp>(op)) {
        rewriter.setInsertionPointAfter(&op);
        Operation *newOp = rewriter.create<handshake::ConstantOp>(
            op.getLoc(), constantOp.getValue(), cntrlMg->getResult(0));

        op.getResult(0).replaceAllUsesWith(newOp->getResult(0));
        cstOps.push_back(&op);
      }
    }

    // Erase StandardOp constants
    for (unsigned i = 0, e = cstOps.size(); i != e; ++i) {
      auto *op = cstOps[i];
      for (int i = 0, e = op->getNumOperands(); i < e; ++i)
        op->eraseOperand(0);
      assert(op->getNumOperands() == 0);
      rewriter.eraseOp(op);
    }
  }
}

void replaceFirstUse(Operation *op, Value oldVal, Value newVal) {
  for (int i = 0, e = op->getNumOperands(); i < e; ++i)
    if (op->getOperand(i) == oldVal) {
      op->setOperand(i, newVal);
      break;
    }
  return;
}

void insertFork(Operation *op, Value result, bool isLazy, OpBuilder &rewriter) {
  // Get successor operations
  vector<Operation *> opsToProcess;
  for (auto &u : result.getUses())
    opsToProcess.push_back(u.getOwner());

  // Insert fork after op
  rewriter.setInsertionPointAfter(op);
  Operation *newOp;
  if (isLazy)
    newOp =
        rewriter.create<LazyForkOp>(op->getLoc(), result, opsToProcess.size());
  else
    newOp = rewriter.create<ForkOp>(op->getLoc(), result, opsToProcess.size());

  // Modify operands of successor
  // opsToProcess may have multiple instances of same operand
  // Replace uses one by one to assign different fork outputs to them
  for (int i = 0, e = opsToProcess.size(); i < e; ++i)
    replaceFirstUse(opsToProcess[i], result, newOp->getResult(i));
}

// Insert Fork Operation for every operation with more than one successor
void addForkOps(handshake::FuncOp f, OpBuilder &rewriter) {
  for (Block &block : f) {
    for (Operation &op : block) {
      // Ignore terminators, and don't add Forks to Forks.
      if (op.getNumSuccessors() == 0 && !isa<ForkOp>(op)) {
        for (auto result : op.getResults()) {
          // If there is a result and it is used more than once
          if (!result.use_empty() && !result.hasOneUse())
            insertFork(&op, result, false, rewriter);
        }
      }
    }
  }
}

void checkUseCount(Operation *op, Value res) {
  // Checks if every result has single use
  if (!res.hasOneUse()) {
    int i = 0;
    for (auto *user : res.getUsers()) {
      user->emitWarning("user here");
      i++;
    }
    op->emitError("every result must have exactly one user, but had ") << i;
  }
  return;
}

// Checks if op successors are in appropriate blocks
void checkSuccessorBlocks(Operation *op, Value res) {
  for (auto &u : res.getUses()) {
    Operation *succOp = u.getOwner();
    // Non-branch ops: succesors must be in same block
    if (!(isa<handshake::ConditionalBranchOp>(op) ||
          isa<handshake::BranchOp>(op))) {
      if (op->getBlock() != succOp->getBlock())
        op->emitError("cannot be block live-out");
    } else {
      // Branch ops: must have successor per successor block
      if (op->getBlock()->getNumSuccessors() != op->getNumResults())
        op->emitError("incorrect successor count");
      bool found = false;
      for (int i = 0, e = op->getBlock()->getNumSuccessors(); i < e; ++i) {
        Block *succ = op->getBlock()->getSuccessor(i);
        if (succOp->getBlock() == succ || isa<SinkOp>(succOp))
          found = true;
      }
      if (!found)
        op->emitError("branch successor in incorrect block");
    }
  }
  return;
}

// Checks if merge predecessors are in appropriate block
void checkMergePredecessors(Operation *op) {
  auto mergeOp = dyn_cast<MergeLikeOpInterface>(op);
  Block *block = op->getBlock();
  unsigned operand_count = op->getNumOperands();

  // Merges in entry block have single predecessor (argument)
  if (block->isEntryBlock()) {
    if (operand_count != 1)
      op->emitError("merge operations in entry block must have a ")
          << "single predecessor";
  } else {
    if (operand_count > getBlockPredecessorCount(block))
      op->emitError("merge operation has ")
          << operand_count << " data inputs, but only "
          << getBlockPredecessorCount(block) << " predecessor blocks";
  }

  // There must be a predecessor from each predecessor block
  for (auto *predBlock : block->getPredecessors()) {
    bool found = false;
    for (auto operand : mergeOp.dataOperands()) {
      auto *operandOp = operand.getDefiningOp();
      if (operandOp->getBlock() == predBlock) {
        found = true;
        break;
      }
    }
    if (!found)
      op->emitError("missing predecessor from predecessor block");
  }

  // Select operand must come from same block
  if (auto muxOp = dyn_cast<MuxOp>(op)) {
    auto *operand = muxOp.selectOperand().getDefiningOp();
    if (operand->getBlock() != block)
      op->emitError("mux select operand must be from same block");
  }
  return;
}

void checkDataflowConversion(handshake::FuncOp f) {
  for (Block &block : f) {
    for (Operation &op : block) {
      if (!isa<mlir::CondBranchOp, mlir::BranchOp, memref::LoadOp,
               mlir::ConstantOp, mlir::AffineReadOpInterface,
               mlir::AffineForOp>(op)) {
        if (op.getNumResults() > 0) {
          for (auto result : op.getResults()) {
            checkUseCount(&op, result);
            checkSuccessorBlocks(&op, result);
          }
        }
        if (isa<MergeLikeOpInterface>(op))
          checkMergePredecessors(&op);
      }
    }
  }
}

Value getBlockControlValue(Block *block) {
  // Get control-only value sent to the block terminator
  for (Operation &op : *block) {
    if (auto BranchOp = dyn_cast<handshake::BranchOp>(op))
      if (BranchOp.isControl())
        return BranchOp.dataOperand();
    if (auto BranchOp = dyn_cast<handshake::ConditionalBranchOp>(op))
      if (BranchOp.isControl())
        return BranchOp.dataOperand();
    if (auto endOp = dyn_cast<handshake::ReturnOp>(op))
      return endOp.control();
  }
  return nullptr;
}

Value getOpMemRef(Operation *op) {
  if (auto memOp = dyn_cast<memref::LoadOp>(op))
    return memOp.getMemRef();
  if (auto memOp = dyn_cast<memref::StoreOp>(op))
    return memOp.getMemRef();
  if (isa<mlir::AffineReadOpInterface, mlir::AffineWriteOpInterface>(op)) {
    MemRefAccess access(op);
    return access.memref;
  }
  op->emitError("Unknown Op type");
  return Value();
}

bool isMemoryOp(Operation *op) {
  return isa<memref::LoadOp, memref::StoreOp, mlir::AffineReadOpInterface,
             mlir::AffineWriteOpInterface>(op);
}

typedef llvm::MapVector<Value, vector<Operation *>> MemRefToMemoryAccessOp;

// Replaces standard memory ops with their handshake version (i.e.,
// ops which connect to memory/LSQ). Returns a map with an ordered
// list of new ops corresponding to each memref. Later, we instantiate
// a memory node for each memref and connect it to its load/store ops
MemRefToMemoryAccessOp replaceMemoryOps(handshake::FuncOp f,
                                        ConversionPatternRewriter &rewriter) {
  // Map from original memref to new load/store operations.
  MemRefToMemoryAccessOp MemRefOps;

  vector<Operation *> opsToErase;

  // Replace load and store ops with the corresponding handshake ops
  // Need to traverse ops in blocks to store them in MemRefOps in program order
  for (Block &block : f)
    for (Operation &op : block) {
      if (isMemoryOp(&op)) {
        rewriter.setInsertionPoint(&op);
        Value memref = getOpMemRef(&op);
        Operation *newOp = nullptr;

        if (memref::LoadOp loadOp = dyn_cast<memref::LoadOp>(op)) {
          // Get operands which correspond to address indices
          // This will add all operands except alloc
          SmallVector<Value, 8> operands(loadOp.getIndices());

          newOp =
              rewriter.create<handshake::LoadOp>(op.getLoc(), memref, operands);
          op.getResult(0).replaceAllUsesWith(newOp->getResult(0));
        } else if (memref::StoreOp storeOp = dyn_cast<memref::StoreOp>(op)) {
          // Get operands which correspond to address indices
          // This will add all operands except alloc and data
          SmallVector<Value, 8> operands(storeOp.getIndices());

          // Create new op where operands are store data and address indices
          newOp = rewriter.create<handshake::StoreOp>(
              op.getLoc(), storeOp.getValueToStore(), operands);
        } else if (isa<mlir::AffineReadOpInterface,
                       mlir::AffineWriteOpInterface>(op)) {
          // Get essential memref access inforamtion.
          MemRefAccess access(&op);
          // The address of an affine load/store operation can be a result of an
          // affine map, which is a linear combination of constants and
          // parameters. Therefore, we should extract the affine map of each
          // address and expand it into proper expressions that calculate the
          // result.
          AffineMap map;
          if (auto loadOp = dyn_cast<mlir::AffineReadOpInterface>(op))
            map = loadOp.getAffineMap();
          else
            map = dyn_cast<AffineWriteOpInterface>(op).getAffineMap();

          // The returned object from expandAffineMap is an optional list of the
          // expansion results from the given affine map, which are the actual
          // address indices that can be used as operands for handshake
          // LoadOp/StoreOp. The following processing requires it to be a valid
          // result.
          auto operands =
              expandAffineMap(rewriter, op.getLoc(), map, access.indices);
          assert(operands &&
                 "Address operands of affine memref access cannot be reduced.");

          if (isa<mlir::AffineReadOpInterface>(op)) {
            newOp = rewriter.create<handshake::LoadOp>(
                op.getLoc(), access.memref, *operands);
            op.getResult(0).replaceAllUsesWith(newOp->getResult(0));
          } else {
            newOp = rewriter.create<handshake::StoreOp>(
                op.getLoc(), op.getOperand(0), *operands);
          }
        } else {
          op.emitError("Load/store operation cannot be handled.");
        }

        MemRefOps[memref].push_back(newOp);

        opsToErase.push_back(&op);
      }
    }

  // Erase old memory ops
  for (unsigned i = 0, e = opsToErase.size(); i != e; ++i) {
    auto *op = opsToErase[i];
    for (int i = 0, e = op->getNumOperands(); i < e; ++i)
      op->eraseOperand(0);
    assert(op->getNumOperands() == 0);

    rewriter.eraseOp(op);
  }

  return MemRefOps;
}

vector<Block *> getOperationBlocks(vector<Operation *> ops) {
  // Get list of (unique) blocks which ops belong to
  // Used to connect control network to memory
  vector<Block *> blocks;

  for (Operation *op : ops) {
    Block *b = op->getBlock();
    if (std::find(blocks.begin(), blocks.end(), b) == blocks.end())
      blocks.push_back(b);
  }
  return blocks;
}

SmallVector<Value, 8> getResultsToMemory(Operation *op) {
  // Get load/store results which are given as inputs to MemoryOp

  if (handshake::LoadOp loadOp = dyn_cast<handshake::LoadOp>(op)) {
    // For load, get all address outputs/indices
    // (load also has one data output which goes to successor operation)
    SmallVector<Value, 8> results(loadOp.addressResults());
    return results;

  } else {
    // For store, all outputs (data and address indices) go to memory
    assert(dyn_cast<handshake::StoreOp>(op));
    handshake::StoreOp storeOp = dyn_cast<handshake::StoreOp>(op);
    SmallVector<Value, 8> results(storeOp.getResults());
    return results;
  }
}

void addLazyForks(handshake::FuncOp f, ConversionPatternRewriter &rewriter) {

  for (Block &block : f) {
    Value res = getBlockControlValue(&block);
    Operation *op = res.getDefiningOp();
    if (!res.hasOneUse())
      insertFork(op, res, true, rewriter);
  }
}

void addMemOpForks(handshake::FuncOp f, ConversionPatternRewriter &rewriter) {

  for (Block &block : f) {
    for (Operation &op : block) {
      if (isa<MemoryOp, StartOp, ControlMergeOp>(op)) {
        for (auto result : op.getResults()) {
          // If there is a result and it is used more than once
          if (!result.use_empty() && !result.hasOneUse())
            insertFork(&op, result, false, rewriter);
        }
      }
    }
  }
}

void removeAllocOps(handshake::FuncOp f, ConversionPatternRewriter &rewriter) {
  vector<Operation *> allocOps;

  for (Block &block : f)
    for (Operation &op : block) {
      if (isa<memref::AllocOp>(op)) {
        assert(op.getResult(0).hasOneUse());
        for (auto &u : op.getResult(0).getUses()) {
          Operation *useOp = u.getOwner();
          if (auto mg = dyn_cast<SinkOp>(useOp))
            allocOps.push_back(&op);
        }
      }
    }
  for (unsigned i = 0, e = allocOps.size(); i != e; ++i) {
    auto *op = allocOps[i];
    rewriter.eraseOp(op);
  }
}

void removeRedundantSinks(handshake::FuncOp f,
                          ConversionPatternRewriter &rewriter) {
  vector<Operation *> redundantSinks;

  for (Block &block : f)
    for (Operation &op : block) {
      if (isa<SinkOp>(op))
        if (!op.getOperand(0).hasOneUse() ||
            isa<memref::AllocOp>(op.getOperand(0).getDefiningOp()))
          redundantSinks.push_back(&op);
    }
  for (unsigned i = 0, e = redundantSinks.size(); i != e; ++i) {
    auto *op = redundantSinks[i];
    rewriter.eraseOp(op);
    // op->erase();
  }
}

Value getMemRefOperand(Value val) {
  assert(val != nullptr);
  // If memref is function argument, connect to memory through
  // its successor merge (all other arguments are connected like that)
  if (val.isa<BlockArgument>()) {
    assert(val.hasOneUse());
    for (auto &u : val.getUses()) {
      Operation *useOp = u.getOwner();
      if (auto mg = dyn_cast<MergeOp>(useOp))
        return useOp->getResult(0);
    }
  }
  return val;
}

void addJoinOps(ConversionPatternRewriter &rewriter,
                vector<Value> controlVals) {
  for (auto val : controlVals) {
    assert(val.hasOneUse());
    auto srcOp = val.getDefiningOp();

    // Insert only single join per block
    if (!isa<JoinOp>(srcOp)) {
      rewriter.setInsertionPointAfter(srcOp);
      Operation *newOp = rewriter.create<JoinOp>(srcOp->getLoc(), val);
      for (auto &u : val.getUses())
        if (u.getOwner() != newOp)
          u.getOwner()->replaceUsesOfWith(val, newOp->getResult(0));
    }
  }
}

vector<Value> getControlValues(vector<Operation *> memOps) {
  vector<Value> vals;

  for (Operation *op : memOps) {
    // Get block from which the mem op originates
    Block *block = op->getBlock();
    // Add control signal from each block
    // Use result which goes to the branch
    Value res = getBlockControlValue(block);
    assert(res != nullptr);
    if (std::find(vals.begin(), vals.end(), res) == vals.end())
      vals.push_back(res);
  }
  return vals;
}

void addValueToOperands(Operation *op, Value val) {

  SmallVector<Value, 8> results(op->getOperands());
  results.push_back(val);
  op->setOperands(results);
}

void setLoadDataInputs(vector<Operation *> memOps, Operation *memOp) {
  // Set memory outputs as load input data
  int ld_count = 0;
  for (auto *op : memOps) {
    if (isa<handshake::LoadOp>(op))
      addValueToOperands(op, memOp->getResult(ld_count++));
  }
}

void setJoinControlInputs(vector<Operation *> memOps, Operation *memOp,
                          int offset, vector<int> cntrlInd) {
  // Connect all memory ops to the join of that block (ensures that all mem
  // ops terminate before a new block starts)
  for (int i = 0, e = memOps.size(); i < e; ++i) {
    auto *op = memOps[i];
    Value val = getBlockControlValue(op->getBlock());
    auto srcOp = val.getDefiningOp();
    if (!isa<JoinOp, StartOp>(srcOp)) {
      srcOp->emitError("Op expected to be a JoinOp or StartOp");
    }
    addValueToOperands(srcOp, memOp->getResult(offset + cntrlInd[i]));
  }
}

void setMemOpControlInputs(ConversionPatternRewriter &rewriter,
                           vector<Operation *> memOps, Operation *memOp,
                           int offset, vector<int> cntrlInd) {
  for (int i = 0, e = memOps.size(); i < e; ++i) {
    vector<Value> controlOperands;
    Operation *currOp = memOps[i];
    Block *currBlock = currOp->getBlock();

    // Set load/store control inputs from control merge
    Operation *cntrlMg = currBlock->isEntryBlock() ? getStartOp(currBlock)
                                                   : getControlMerge(currBlock);
    controlOperands.push_back(cntrlMg->getResult(0));

    // Set load/store control inputs from predecessors in block
    for (int j = 0, f = i; j < f; ++j) {
      Operation *predOp = memOps[j];
      Block *predBlock = predOp->getBlock();
      if (currBlock == predBlock)
        // Any dependency but RARs
        if (!(isa<handshake::LoadOp>(currOp) && isa<handshake::LoadOp>(predOp)))
          // cntrlInd maps memOps index to correct control output index
          controlOperands.push_back(memOp->getResult(offset + cntrlInd[j]));
    }

    // If there is only one control input, add directly to memory op
    if (controlOperands.size() == 1)
      addValueToOperands(currOp, controlOperands[0]);

    // If multiple, join them and connect join output to memory op
    else {
      rewriter.setInsertionPoint(currOp);
      Operation *joinOp =
          rewriter.create<JoinOp>(currOp->getLoc(), controlOperands);
      addValueToOperands(currOp, joinOp->getResult(0));
    }
  }
}

void connectToMemory(handshake::FuncOp f, MemRefToMemoryAccessOp MemRefOps,
                     bool lsq, ConversionPatternRewriter &rewriter) {
  // Add MemoryOps which represent the memory interface
  // Connect memory operations and control appropriately
  int mem_count = 0;
  for (auto memory : MemRefOps) {
    // First operand corresponds to memref (alloca or function argument)
    Value memrefOperand = getMemRefOperand(memory.first);

    vector<Value> operands;
    // operands.push_back(memrefOperand);

    // Get control values which need to connect to memory
    vector<Value> controlVals = getControlValues(memory.second);

    // In case of LSQ interface, set control values as inputs (used to trigger
    // allocation to LSQ)
    if (lsq)
      operands.insert(operands.end(), controlVals.begin(), controlVals.end());

    // Add load indices and store data+indices to memory operands
    // Count number of loads so that we can generate appropriate number of
    // memory outputs (data to load ops)

    // memory.second is in program order
    // Enforce MemoryOp port ordering as follows:
    // Operands: all stores then all loads (stdata1, staddr1, stdata2,...,
    // ldaddr1, ldaddr2,....) Outputs: all load outputs, ordered the same as
    // load addresses (lddata1, lddata2, ...), followed by all none outputs,
    // ordered as operands (stnone1, stnone2,...ldnone1, ldnone2,...)
    vector<int> newInd(memory.second.size(), 0);
    int ind = 0;
    for (int i = 0, e = memory.second.size(); i < e; ++i) {
      auto *op = memory.second[i];
      if (isa<handshake::StoreOp>(op)) {
        SmallVector<Value, 8> results = getResultsToMemory(op);
        operands.insert(operands.end(), results.begin(), results.end());
        newInd[i] = ind++;
      }
    }

    int ld_count = 0;

    for (int i = 0, e = memory.second.size(); i < e; ++i) {
      auto *op = memory.second[i];
      if (isa<handshake::LoadOp>(op)) {
        SmallVector<Value, 8> results = getResultsToMemory(op);
        operands.insert(operands.end(), results.begin(), results.end());

        ld_count++;
        newInd[i] = ind++;
      }
    }

    // control-only outputs for each access (indicate access completion)
    int cntrl_count = lsq ? 0 : memory.second.size();

    Block *entryBlock = &f.front();
    rewriter.setInsertionPointToStart(entryBlock);

    // Place memory op next to the alloc op
    Operation *newOp = rewriter.create<MemoryOp>(
        entryBlock->front().getLoc(), operands, ld_count, cntrl_count, lsq,
        mem_count++, memrefOperand);

    setLoadDataInputs(memory.second, newOp);

    if (!lsq) {
      // Create Joins which join done signals from memory with the
      // control-only network
      addJoinOps(rewriter, controlVals);

      // Connect all load/store done signals to the join of their block
      // Ensure that the block terminates only after all its accesses have
      // completed
      // True is default. When no sync needed, set to false (for now,
      // user-determined)
      bool control = true;

      if (control)
        setJoinControlInputs(memory.second, newOp, ld_count, newInd);
      else {
        for (int i = 0, e = cntrl_count; i < e; ++i) {
          rewriter.setInsertionPointAfter(newOp);
          rewriter.create<SinkOp>(newOp->getLoc(),
                                  newOp->getResult(ld_count + i));
        }
      }

      // Set control-only inputs to each memory op
      // Ensure that op starts only after prior blocks have completed
      // Ensure that op starts only after predecessor ops (with RAW, WAR, or
      // WAW) have completed
      setMemOpControlInputs(rewriter, memory.second, newOp, ld_count, newInd);
    }
  }

  if (lsq)
    addLazyForks(f, rewriter);
  else
    addMemOpForks(f, rewriter);

  removeAllocOps(f, rewriter);

  // Loads and stores have some sinks which are no longer needed now that they
  // connect to MemoryOp
  removeRedundantSinks(f, rewriter);
}

void replaceCallOps(handshake::FuncOp f, ConversionPatternRewriter &rewriter) {
  for (Block &block : f) {
    for (Operation &op : block) {
      if (auto callOp = dyn_cast<CallOp>(op)) {
        rewriter.setInsertionPointAfter(&op);
        rewriter.replaceOpWithNewOp<handshake::InstanceOp>(
            callOp, callOp.getCallee(), callOp.getResultTypes(),
            callOp.getOperands());
      }
    }
  }
}

/// Rewrite affine.for operations in a handshake.func into its representations
/// as a CFG in the standard dialect. Affine expressions in loop bounds will be
/// expanded to code in the standard dialect that actually computes them. We
/// combine the lowering of affine loops in the following two conversions:
/// [AffineToStandard](https://mlir.llvm.org/doxygen/AffineToStandard_8cpp.html),
/// [SCFToStandard](https://mlir.llvm.org/doxygen/SCFToStandard_8cpp_source.html)
/// into this function.
/// The affine memory operations will be preserved until other rewrite
/// functions, e.g.,`replaceMemoryOps`, are called. Any affine analysis, e.g.,
/// getting dependence information, should be carried out before calling this
/// function; otherwise, the affine for loops will be destructed and key
/// information will be missing.
LogicalResult rewriteAffineFor(handshake::FuncOp f,
                               ConversionPatternRewriter &rewriter) {
  // Get all affine.for operations in the function body.
  SmallVector<mlir::AffineForOp, 8> forOps;
  f.walk([&](mlir::AffineForOp op) { forOps.push_back(op); });

  // TODO: how to deal with nested loops?
  for (unsigned i = 0, e = forOps.size(); i < e; i++) {
    auto forOp = forOps[i];

    // Insert lower and upper bounds right at the position of the original
    // affine.for operation.
    rewriter.setInsertionPoint(forOp);
    auto loc = forOp.getLoc();
    auto lowerBound = expandAffineMap(rewriter, loc, forOp.getLowerBoundMap(),
                                      forOp.getLowerBoundOperands());
    auto upperBound = expandAffineMap(rewriter, loc, forOp.getUpperBoundMap(),
                                      forOp.getUpperBoundOperands());
    if (!lowerBound || !upperBound)
      return failure();
    auto step = rewriter.create<mlir::ConstantIndexOp>(loc, forOp.getStep());

    // Build blocks for a common for loop. initBlock and initPosition are the
    // block that contains the current forOp, and the position of the forOp.
    auto *initBlock = rewriter.getInsertionBlock();
    auto initPosition = rewriter.getInsertionPoint();

    // Split the current block into several parts. `endBlock` contains the code
    // starting from forOp. `conditionBlock` will have the condition branch.
    // `firstBodyBlock` is the loop body, and `lastBodyBlock` is about the loop
    // iterator stepping. Here we move the body region of the AffineForOp here
    // and split it into `conditionBlock`, `firstBodyBlock`, and
    // `lastBodyBlock`.
    // TODO: is there a simpler API for doing so?
    auto *endBlock = rewriter.splitBlock(initBlock, initPosition);
    // Split and get the references to different parts (blocks) of the original
    // loop body.
    auto *conditionBlock = &forOp.region().front();
    auto *firstBodyBlock =
        rewriter.splitBlock(conditionBlock, conditionBlock->begin());
    auto *lastBodyBlock =
        rewriter.splitBlock(firstBodyBlock, firstBodyBlock->end());
    rewriter.inlineRegionBefore(forOp.region(), endBlock);

    // The loop IV is the first argument of the conditionBlock.
    auto iv = conditionBlock->getArgument(0);

    // Get the loop terminator, which should be the last operation of the
    // original loop body. And `firstBodyBlock` points to that loop body.
    auto terminator = dyn_cast<mlir::AffineYieldOp>(firstBodyBlock->back());
    if (!terminator)
      return failure();

    // First, we fill the content of the lastBodyBlock with how the loop
    // iterator steps.
    rewriter.setInsertionPointToEnd(lastBodyBlock);
    auto stepped = rewriter.create<mlir::AddIOp>(loc, iv, step).getResult();

    // Next, we get the loop carried values, which are terminator operands.
    SmallVector<Value, 8> loopCarried;
    loopCarried.push_back(stepped);
    loopCarried.append(terminator.operand_begin(), terminator.operand_end());
    rewriter.create<mlir::BranchOp>(loc, conditionBlock, loopCarried);

    // Then we fill in the condition block.
    rewriter.setInsertionPointToEnd(conditionBlock);
    auto comparison = rewriter.create<mlir::CmpIOp>(loc, CmpIPredicate::slt, iv,
                                                    upperBound.getValue()[0]);

    rewriter.create<mlir::CondBranchOp>(loc, comparison, firstBodyBlock,
                                        ArrayRef<Value>(), endBlock,
                                        ArrayRef<Value>());

    // We also insert the branch operation at the end of the initBlock.
    rewriter.setInsertionPointToEnd(initBlock);
    // TODO: should we add more operands here?
    rewriter.create<mlir::BranchOp>(loc, conditionBlock,
                                    lowerBound.getValue()[0]);

    // Finally, setup the firstBodyBlock.
    rewriter.setInsertionPointToEnd(firstBodyBlock);
    // TODO: is it necessary to add this explicit branch operation?
    rewriter.create<mlir::BranchOp>(loc, lastBodyBlock);

    // Remove the original forOp and the terminator in the loop body.
    rewriter.eraseOp(terminator);
    rewriter.eraseOp(forOp);
  }

  return success();
}

struct HandshakeCanonicalizePattern : public ConversionPattern {
  using ConversionPattern::ConversionPattern;
  LogicalResult match(Operation *op) const override {
    // Ignore forks, ops without results, and branches (ops with succ blocks)
    op->emitWarning("checking...");
    return success();
    if (op->getNumSuccessors() == 0 && op->getNumResults() > 0 &&
        !isa<ForkOp>(op))
      return success();
    else
      return failure();
  }

  void rewrite(Operation *op, ArrayRef<Value> operands,
               ConversionPatternRewriter &rewriter) const override {
    op->emitWarning("skipping...");
    if (op->getNumSuccessors() == 0 && op->getNumResults() > 0 &&
        !isa<ForkOp>(op)) {
      op->emitWarning("skipping...");
    }
    for (auto result : op->getResults()) {
      // If there is a result and it is used more than once
      if (!result.use_empty() && !result.hasOneUse())
        insertFork(op, result, false, rewriter);
    }
  }
};

struct FuncOpLowering : public OpConversionPattern<mlir::FuncOp> {
  using OpConversionPattern<mlir::FuncOp>::OpConversionPattern;
  LogicalResult
  matchAndRewrite(mlir::FuncOp funcOp, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const override {

    // Only retain those attributes that are not constructed by build.
    SmallVector<NamedAttribute, 4> attributes;
    for (const auto &attr : funcOp->getAttrs()) {
      if (attr.first == SymbolTable::getSymbolAttrName() ||
          attr.first == function_like_impl::getTypeAttrName())
        continue;
      attributes.push_back(attr);
    }

    // Get function arguments
    llvm::SmallVector<mlir::Type, 8> argTypes;
    for (auto &arg : funcOp.getArguments()) {
      mlir::Type type = arg.getType();
      argTypes.push_back(type);
    }

    // Get function results
    llvm::SmallVector<mlir::Type, 8> resTypes;
    for (auto arg : funcOp.getType().getResults())
      resTypes.push_back(arg);

    // Add control input/output to function arguments/results
    auto noneType = rewriter.getNoneType();
    argTypes.push_back(noneType);
    resTypes.push_back(noneType);

    // Signature conversion (converts function arguments)
    int arg_count = funcOp.getNumArguments() + 1;
    TypeConverter::SignatureConversion result(arg_count);

    for (unsigned idx = 0, e = argTypes.size(); idx < e; ++idx)
      result.addInputs(idx, argTypes[idx]);

    // Create function of appropriate type
    auto func_type = rewriter.getFunctionType(argTypes, resTypes);
    auto newFuncOp = rewriter.create<handshake::FuncOp>(
        funcOp.getLoc(), funcOp.getName(), func_type, attributes);
    rewriter.inlineRegionBefore(funcOp.getBody(), newFuncOp.getBody(),
                                newFuncOp.end());

    // Rewrite affine.for operations.
    if (failed(rewriteAffineFor(newFuncOp, rewriter)))
      return newFuncOp.emitOpError("failed to rewrite Affine loops");

    // Perform dataflow conversion
    MemRefToMemoryAccessOp MemOps = replaceMemoryOps(newFuncOp, rewriter);

    replaceCallOps(newFuncOp, rewriter);

    setControlOnlyPath(newFuncOp, rewriter);

    addMergeOps(newFuncOp, rewriter);

    addBranchOps(newFuncOp, rewriter);

    addSinkOps(newFuncOp, rewriter);

    connectConstantsToControl(newFuncOp, rewriter);

    addForkOps(newFuncOp, rewriter);

    checkDataflowConversion(newFuncOp);

    bool lsq = false;
    connectToMemory(newFuncOp, MemOps, lsq, rewriter);

    // Apply signature conversion to set function arguments
    rewriter.applySignatureConversion(&newFuncOp.getBody(), result);

    // Remove redundant start op
    Block *entryBlock = &newFuncOp.front();
    // After signature conversion, control value is set as last entry block
    // argument
    Value val = entryBlock->getArguments().back();
    Operation *cntrlMg = getStartOp(entryBlock);
    cntrlMg->getResult(0).replaceAllUsesWith(val);
    rewriter.eraseOp(cntrlMg);

    rewriter.eraseOp(funcOp);

    return success();
  }
};

namespace {
struct HandshakeInsertBufferPass
    : public HandshakeInsertBufferBase<HandshakeInsertBufferPass> {

  // Perform a depth first search and insert buffers when cycles are detected.
  void bufferCyclesStrategy() {
    auto f = getOperation();
    DenseSet<Operation *> opVisited;
    DenseSet<Operation *> opInFlight;

    // Traverse each use of each argument of the entry block.
    auto builder = OpBuilder(f.getContext());
    for (auto &arg : f.getBody().front().getArguments()) {
      for (auto &operand : arg.getUses()) {
        if (opVisited.count(operand.getOwner()) == 0)
          insertBufferDFS(operand.getOwner(), builder, opVisited, opInFlight);
      }
    }
  }

  // Perform a depth first search and add a buffer to any un-buffered channel.
  void bufferAllStrategy() {
    auto f = getOperation();
    auto builder = OpBuilder(f.getContext());
    for (auto &arg : f.getArguments())
      for (auto &use : arg.getUses())
        insertBufferRecursive(use, builder, /*numSlots=*/2,
                              [](Operation *definingOp, Operation *usingOp) {
                                return !isa_and_nonnull<BufferOp>(definingOp) &&
                                       !isa<BufferOp>(usingOp);
                              });
  }

  /// DFS-based graph cycle detection and naive buffer insertion. Exactly one
  /// 2-slot non-transparent buffer will be inserted into each graph cycle.
  void insertBufferDFS(Operation *op, OpBuilder &builder,
                       DenseSet<Operation *> &opVisited,
                       DenseSet<Operation *> &opInFlight) {
    // Mark operation as visited and push into the stack.
    opVisited.insert(op);
    opInFlight.insert(op);

    // Traverse all uses of the current operation.
    for (auto &operand : op->getUses()) {
      auto *user = operand.getOwner();

      // If graph cycle detected, insert a BufferOp into the edge.
      if (opInFlight.count(user) != 0 && !isa<handshake::BufferOp>(op) &&
          !isa<handshake::BufferOp>(user)) {
        auto value = operand.get();

        builder.setInsertionPointAfter(op);
        auto bufferOp = builder.create<handshake::BufferOp>(
            op->getLoc(), value.getType(), value, /*sequential=*/true,
            /*control=*/value.getType().isa<NoneType>(),
            /*slots=*/2);
        value.replaceUsesWithIf(
            bufferOp,
            function_ref<bool(OpOperand &)>([](OpOperand &operand) -> bool {
              return !isa<handshake::BufferOp>(operand.getOwner());
            }));
      }
      // For unvisited operations, recursively call insertBufferDFS() method.
      else if (opVisited.count(user) == 0)
        insertBufferDFS(user, builder, opVisited, opInFlight);
    }
    // Pop operation out of the stack.
    opInFlight.erase(op);
  }

  void
  insertBufferRecursive(OpOperand &use, OpBuilder builder, size_t numSlots,
                        function_ref<bool(Operation *, Operation *)> callback) {
    auto oldValue = use.get();
    auto *definingOp = oldValue.getDefiningOp();
    auto *usingOp = use.getOwner();
    if (callback(definingOp, usingOp)) {
      builder.setInsertionPoint(usingOp);
      auto buffer = builder.create<handshake::BufferOp>(
          oldValue.getLoc(), oldValue.getType(), oldValue, /*sequential=*/true,
          /*control=*/oldValue.getType().isa<NoneType>(),
          /*slots=*/numSlots);
      use.getOwner()->setOperand(use.getOperandNumber(), buffer);
    }

    for (auto &childUse : usingOp->getUses())
      if (!isa<handshake::BufferOp>(childUse.getOwner()))
        insertBufferRecursive(childUse, builder, numSlots, callback);
  }

  void runOnOperation() override {
    if (strategies.empty())
      strategies = {"cycles"};

    for (auto strategy : strategies) {
      if (strategy == "cycles")
        bufferCyclesStrategy();
      else if (strategy == "all")
        bufferAllStrategy();
      else {
        emitError(getOperation().getLoc())
            << "Unknown buffer strategy: " << strategy;
        signalPassFailure();
        return;
      }
    }
  }
};

struct HandshakeRemoveBlockPass
    : HandshakeRemoveBlockBase<HandshakeRemoveBlockPass> {
  void runOnOperation() override { removeBasicBlocks(getOperation()); }
};

struct HandshakeDataflowPass
    : public HandshakeDataflowBase<HandshakeDataflowPass> {
  void runOnOperation() override {
    ModuleOp m = getOperation();

    ConversionTarget target(getContext());
    target.addLegalDialect<HandshakeOpsDialect, StandardOpsDialect>();

    RewritePatternSet patterns(&getContext());
    patterns.insert<FuncOpLowering>(m.getContext());

    if (failed(applyPartialConversion(m, target, std::move(patterns))))
      signalPassFailure();

    // Legalize the resulting regions, which can have no basic blocks.
    for (auto func : m.getOps<handshake::FuncOp>())
      removeBasicBlocks(func);
  }
};

struct HandshakeCanonicalizePass
    : public HandshakeCanonicalizeBase<HandshakeCanonicalizePass> {
  void runOnOperation() override {
    auto Op = getOperation();
    OpBuilder builder(Op);
    addForkOps(Op, builder);
    addSinkOps(Op, builder);

    for (auto &block : Op)
      for (auto &nestedOp : block)
        for (mlir::Value out : nestedOp.getResults())
          if (!out.hasOneUse()) {
            nestedOp.emitError("does not have exactly one use");
            signalPassFailure();
          }
    // ConversionTarget target(getContext());
    // // "Legal" function ops should have no interface variable ABI
    // attributes. target.addDynamicallyLegalDialect<HandshakeOpsDialect,
    // StandardOpsDialect>(
    //         Optional<ConversionTarget::DynamicLegalityCallbackFn>(
    //            [](Operation *op) { return op->hasOneUse(); }));

    // RewritePatternSet patterns;
    // patterns.insert<HandshakeCanonicalizePattern>("std.muli", 1,
    // m.getContext()); applyPatternsAndFoldGreedily(

    // if (failed(applyPartialConversion(m, target, patterns)))
    //   signalPassFailure();
  }
};
} // namespace

// Temporary: print resources (operation counts)
namespace {

struct HandshakeAnalysisPass
    : public HandshakeAnalysisBase<HandshakeAnalysisPass> {
  void runOnOperation() override {
    ModuleOp m = getOperation();

    for (auto func : m.getOps<handshake::FuncOp>()) {
      dotPrint(func, "output");

      int count = 0;
      int fork_count = 0;
      int merge_count = 0;
      int branch_count = 0;
      int join_count = 0;

      for (Block &block : func)
        for (Operation &op : block) {

          if (isa<ForkOp>(op))
            fork_count++;
          else if (isa<MergeLikeOpInterface>(op))
            merge_count++;
          else if (isa<ConditionalBranchOp>(op))
            branch_count++;
          else if (isa<JoinOp>(op))
            join_count++;
          else if (!isa<handshake::BranchOp>(op) && !isa<SinkOp>(op) &&
                   !isa<TerminatorOp>(op))
            count++;
        }

      llvm::outs() << "// Fork count: " << fork_count << "\n";
      llvm::outs() << "// Merge count: " << merge_count << "\n";
      llvm::outs() << "// Branch count: " << branch_count << "\n";
      llvm::outs() << "// Join count: " << join_count << "\n";
      int total = count + fork_count + merge_count + branch_count;

      llvm::outs() << "// Total op count: " << total << "\n";
    }
  }
};
} // namespace

// Temporary: remove unused function args
// The array arguements are impelemented as local memory ops in handshake
// dialect, and the original arguements have no use. This pass temporary removes
// these used args to let the simulator work properly by reading external data
// files.
namespace {

struct HandshakeCleanFuncArgPass
    : public HandshakeCleanFuncArgBase<HandshakeCleanFuncArgPass> {
  void runOnOperation() override {
    auto m = getOperation();
    for (auto funcOp : m.getOps<handshake::FuncOp>()) {
      OpBuilder rewriter(funcOp);

      // Get function arguments
      std::vector<unsigned int> argIndices;
      llvm::SmallVector<mlir::Type, 8> argTypes;
      auto i = 0;
      for (auto &arg : funcOp.getArguments()) {
        auto type = arg.getType();
        if (arg.use_empty())
          argIndices.push_back(i);
        else if (arg.hasOneUse()) {
          auto sinkOp =
              dyn_cast<handshake::SinkOp>(arg.use_begin()->getOwner());
          if (sinkOp) {
            sinkOp.erase();
            argIndices.push_back(i);
          } else
            argTypes.push_back(type);
        } else
          argTypes.push_back(type);
        i++;
      }

      // Get function results
      llvm::SmallVector<mlir::Type, 8> resTypes;
      for (auto arg : funcOp.getType().getResults())
        resTypes.push_back(arg);

      auto funcType = rewriter.getFunctionType(argTypes, resTypes);
      auto originalNumArgs = i;
      impl::eraseFunctionArguments(funcOp, argIndices, originalNumArgs,
                                   funcType);
    }
  }
};
} // namespace

std::unique_ptr<mlir::OperationPass<mlir::ModuleOp>>
circt::createHandshakeCleanFuncArgPass() {
  return std::make_unique<HandshakeCleanFuncArgPass>();
}

std::unique_ptr<mlir::OperationPass<mlir::ModuleOp>>
circt::createHandshakeAnalysisPass() {
  return std::make_unique<HandshakeAnalysisPass>();
}

std::unique_ptr<mlir::OperationPass<mlir::ModuleOp>>
circt::createHandshakeDataflowPass() {
  return std::make_unique<HandshakeDataflowPass>();
}

std::unique_ptr<mlir::OperationPass<handshake::FuncOp>>
circt::createHandshakeCanonicalizePass() {
  return std::make_unique<HandshakeCanonicalizePass>();
}

std::unique_ptr<mlir::OperationPass<handshake::FuncOp>>
circt::createHandshakeRemoveBlockPass() {
  return std::make_unique<HandshakeRemoveBlockPass>();
}

std::unique_ptr<mlir::OperationPass<handshake::FuncOp>>
circt::createHandshakeInsertBufferPass() {
  return std::make_unique<HandshakeInsertBufferPass>();
}
