//===- PrettifyVerilog.cpp - Transformations to improve Verilog quality ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass contains elective transformations that improve the quality of
// SystemVerilog generated by the ExportVerilog library.  This pass is not
// compulsory: things that are required for ExportVerilog to be correct should
// be included as part of the ExportVerilog pass itself to make sure it is self
// contained.
//
//===----------------------------------------------------------------------===//

#include "PassDetail.h"
#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/SV/SVPasses.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Matchers.h"

using namespace circt;

//===----------------------------------------------------------------------===//
// PrettifyVerilogPass
//===----------------------------------------------------------------------===//

namespace {
struct PrettifyVerilogPass
    : public sv::PrettifyVerilogBase<PrettifyVerilogPass> {
  void runOnOperation() override;

private:
  void prettifyUnaryOperator(Operation *op);
  void sinkOpToUses(Operation *op);

  bool anythingChanged;
};
} // end anonymous namespace

/// Return true if this is something that will get printed as a unary operator
/// by the Verilog printer.
static bool isVerilogUnaryOperator(Operation *op) {
  if (isa<comb::ParityOp>(op))
    return true;

  if (auto xorOp = dyn_cast<comb::XorOp>(op))
    return xorOp.isBinaryNot();

  if (auto icmpOp = dyn_cast<comb::ICmpOp>(op))
    return icmpOp.isEqualAllOnes() || icmpOp.isNotEqualZero();

  return false;
}

/// Sink an operation into the same block where it is used.  This will clone the
/// operation so it can be sunk into multiple blocks. If there are no more uses
/// in the current block, the op will be removed.
void PrettifyVerilogPass::sinkOpToUses(Operation *op) {
  assert(mlir::MemoryEffectOpInterface::hasNoEffect(op) &&
         "Op with side effects cannot be sunk to its uses.");
  auto block = op->getBlock();
  for (auto it = op->use_begin(), end = op->use_end(); it != end;) {
    // If the current use is not in the same block as the operation, we are
    // going to clone the op into the use's block.
    auto &use = *it;
    auto useBlock = use.getOwner()->getBlock();
    if (block == useBlock) {
      ++it;
      continue;
    }
    // Clone the operation and insert it to the beginning of the block.
    auto *cloned = OpBuilder::atBlockBegin(useBlock).clone(*op);
    // Scan the use-list for any more uses in the local block and modify them to
    // use the newly cloned operation. This will only scan forward from the
    // current use list iterator.
    for (auto scanIt = std::next(it); scanIt != end;) {
      auto &scan = *scanIt;
      ++scanIt;
      if (useBlock == scan.getOwner()->getBlock()) {
        scan.set(cloned->getResult(0));
      }
    }
    // Advance the iterator to the next use. This has to happen before
    // removing the current use from the list, but after removing other uses.
    ++it;
    // Replace the current use, removing it from the use list.
    use.set(cloned->getResult(0));
    anythingChanged = true;
  }
  // If this op is no longer used, drop it.
  if (op->use_empty()) {
    op->erase();
    anythingChanged = true;
  }
}

/// This is called on unary operators.
void PrettifyVerilogPass::prettifyUnaryOperator(Operation *op) {
  // If this is a multiple use unary operator, duplicate it and move it into the
  // block corresponding to the user.  This avoids emitting a temporary just for
  // a unary operator.  Instead of:
  //
  //    tmp1 = ^(thing+thing);
  //         = tmp1 + 42
  //
  // we get:
  //
  //    tmp2 = thing+thing;
  //         = ^tmp2 + 42
  //
  // This is particularly helpful when the operand of the unary op has multiple
  // uses as well.
  if (op->use_empty() || op->hasOneUse())
    return;

  while (!op->hasOneUse()) {
    OpOperand &use = *op->use_begin();
    Operation *user = use.getOwner();

    // Clone the operation and insert before this user.
    auto *cloned = op->clone();
    user->getBlock()->getOperations().insert(Block::iterator(user), cloned);

    // Update user's operand to the new value.
    use.set(cloned->getResult(0));
  }

  // There is exactly one user left, so move this before it.
  Operation *user = *op->user_begin();
  op->moveBefore(user);
  anythingChanged = true;
}

void PrettifyVerilogPass::runOnOperation() {
  // Keeps track if anything changed during this pass, used to determine if
  // the analyses were preserved.
  anythingChanged = false;

  // Walk the operations in post-order, transforming any that are interesting.
  getOperation()->walk([&](Operation *op) {
    if (isVerilogUnaryOperator(op))
      return prettifyUnaryOperator(op);
    // Sink or duplicate constant ops into the same block as their use.  This
    // will allow the verilog emitter to inline constant expressions.
    if (matchPattern(op, mlir::m_Constant()))
      return sinkOpToUses(op);
  });

  // If we did not change anything in the graph mark all analysis as
  // preserved.
  if (!anythingChanged)
    markAllAnalysesPreserved();
}

std::unique_ptr<Pass> circt::sv::createPrettifyVerilogPass() {
  return std::make_unique<PrettifyVerilogPass>();
}
