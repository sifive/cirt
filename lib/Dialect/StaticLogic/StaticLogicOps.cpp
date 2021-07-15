//===- StaticLogicOps.h - StaticLogic MLIR Operations -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implement the StaticLogic ops.
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/StaticLogic/StaticLogic.h"
#include "mlir/IR/FunctionImplementation.h"
#include "mlir/IR/PatternMatch.h"

using namespace circt;
using namespace circt::staticlogic;

#include "circt/Dialect/StaticLogic/StaticLogicDialect.cpp.inc"

#define GET_OP_CLASSES
#include "circt/Dialect/StaticLogic/StaticLogic.cpp.inc"

void StaticLogicDialect::initialize() {
  addOperations<
#define GET_OP_LIST
#include "circt/Dialect/StaticLogic/StaticLogic.cpp.inc"
      >();
}

//===----------------------------------------------------------------------===//
// PipelineOp
//===----------------------------------------------------------------------===//

namespace {
/// This canonicalizer will ensure all values passed between pipeline stages are
/// going through the register operation.
struct CanonicalizePipeline : public OpRewritePattern<PipelineOp> {
  using OpRewritePattern<PipelineOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(PipelineOp pipeline,
                                PatternRewriter &rewriter) const override {
    for (auto &block : pipeline.getRegion()) {
      auto registerOp = dyn_cast<RegisterOp>(block.getTerminator());
      if (!registerOp)
        continue;

      auto dest = registerOp.dest();
      SmallVector<Value, 8> registers;

      // Walk through all block arguments. If an argument is used by other
      // blocks, it needs to be registered.
      for (auto arg : block.getArguments()) {
        for (auto &use : arg.getUses()) {
          if (use.getOwner()->getBlock() != &block) {
            registers.push_back(arg);
            break;
          }
        }
      }

      // Walk through all results of all operations in the block. If a result is
      // used by other blocks, it needs to be regitered.
      for (auto &op : block) {
        for (auto result : op.getResults()) {
          for (auto &use : result.getUses()) {
            if (use.getOwner()->getBlock() != &block) {
              // Only push back unique operands.
              if (std::find(registers.begin(), registers.end(), result) ==
                  registers.end())
                registers.push_back(result);
              break;
            }
          }
        }
      }

      // Update register operation's operands list and the successor's arguments
      // list.
      auto destOperands = registerOp.destOperandsMutable();

      for (auto value : registers) {
        destOperands.append(value);
        auto arg = dest->addArgument(value.getType());
        value.replaceUsesWithIf(arg, [&block](OpOperand &use) {
          return use.getOwner()->getBlock() != &block;
        });
      }

      rewriter.setInsertionPoint(registerOp);
      rewriter.create<RegisterOp>(registerOp.getLoc(), destOperands, dest);
      rewriter.eraseOp(registerOp);
    }
    return success();
  }
};
} // end anonymous namespace.

void PipelineOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                             MLIRContext *context) {
  results.insert<CanonicalizePipeline>(context);
}
