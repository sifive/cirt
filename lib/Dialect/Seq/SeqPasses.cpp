//===- SeqPasses.cpp - Implement Seq passes -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/SV/SVOps.h"
#include "circt/Dialect/Seq/SeqOps.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/DialectImplementation.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"

using namespace mlir;
using namespace circt;
using namespace seq;

namespace circt {
namespace seq {
#define GEN_PASS_CLASSES
#include "circt/Dialect/Seq/SeqPasses.h.inc"
} // namespace seq
} // namespace circt

namespace {
struct SeqToSVPass : public LowerSeqToSVBase<SeqToSVPass> {
  void runOnOperation() override;
};
} // anonymous namespace

namespace {
/// Lower CompRegOp to `sv.reg` and `sv.alwaysff`. Use a posedge clock and
/// synchronous reset.
struct CompRegLower : public OpConversionPattern<CompRegOp> {
public:
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(CompRegOp reg, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const final;
};

} // anonymous namespace

namespace {
class RegLower : public OpConversionPattern<RegOp> {
public:
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(RegOp reg, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const final;
};

} // anonymous namespace

struct RegOperandsAndAttrs {
  Location loc;
  DictionaryAttr regAttrs;
  Type resultType;
  sv::EventControl clockEdge;
  Value input;
  Value clk;
  Value enable;
  Value reset;
  Value resetValue;
  ::ResetType resetType;
  ::llvm::Optional<sv::EventControl> resetEdge;
};

static bool isI1Constant(::mlir::Value value, size_t n) {
  assert(value.getType().isSignlessInteger(1));
  if (auto constantOp = value.getDefiningOp<::circt::hw::ConstantOp>()) {
    return constantOp.value() == n;
  }

  return false;
}

static bool isI1Gnd(::mlir::Value value) { return isI1Constant(value, 0); }
static bool isI1Vdd(::mlir::Value value) { return isI1Constant(value, 1); }

static sv::ReadInOutOp
lowerRegCommon(const RegOperandsAndAttrs &regOperandsAndAttrs,
               ConversionPatternRewriter &rewriter) {
  Location loc = regOperandsAndAttrs.loc;

  auto svReg = rewriter.create<sv::RegOp>(loc, regOperandsAndAttrs.resultType);
  auto svRegValue = rewriter.create<sv::ReadInOutOp>(loc, svReg);

  DictionaryAttr regAttrs = regOperandsAndAttrs.regAttrs;
  for (auto &attr : regAttrs) {
    if (attr.first != "clockEdge" && attr.first != "resetEdge" &&
        attr.first != "resetType") {
      svReg->setAttr(attr.first, attr.second);
    }
  }
  if (!svReg->hasAttrOfType<StringAttr>("name"))
    // sv.reg requires a name attribute.
    svReg->setAttr("name", rewriter.getStringAttr(""));

  auto onAssignInput = [&]() {
    rewriter.create<sv::PAssignOp>(loc, svReg, regOperandsAndAttrs.input);
  };

  auto onReset = [&]() {
    rewriter.create<sv::PAssignOp>(loc, svReg, regOperandsAndAttrs.resetValue);
  };

  auto onRegularOperation = [&]() {
    if (!regOperandsAndAttrs.enable || isI1Vdd(regOperandsAndAttrs.enable)) {
      return onAssignInput();
    }

    rewriter.create<sv::IfOp>(loc, regOperandsAndAttrs.enable, onAssignInput,
                              nullptr);
  };

  if (!regOperandsAndAttrs.reset || isI1Gnd(regOperandsAndAttrs.reset)) {
    rewriter.create<sv::AlwaysFFOp>(loc, regOperandsAndAttrs.clockEdge,
                                    regOperandsAndAttrs.clk,
                                    onRegularOperation);
  } else {
    // reg.resetEdge() will always be defined, when reg.reset() is set.
    assert(regOperandsAndAttrs.resetEdge.hasValue() &&
           "resetEdge must be set when reset is set!");
    rewriter.create<sv::AlwaysFFOp>(
        loc, regOperandsAndAttrs.clockEdge, regOperandsAndAttrs.clk,
        regOperandsAndAttrs.resetType, regOperandsAndAttrs.resetEdge.getValue(),
        regOperandsAndAttrs.reset, onRegularOperation, onReset);
  }

  return svRegValue;
}

LogicalResult
CompRegLower::matchAndRewrite(CompRegOp reg, ArrayRef<Value> operands,
                              ConversionPatternRewriter &rewriter) const {
  RegOperandsAndAttrs regOperandsAndAttrs{
      .loc = reg.getLoc(),
      .regAttrs = reg->getAttrDictionary(),
      .resultType = reg.getResult().getType(),
      .clockEdge = sv::EventControl::AtPosEdge,
      .input = reg.input(),
      .clk = reg.clk(),
      .enable = ::mlir::Value(),
      .reset = reg.reset(),
      .resetValue = reg.resetValue(),
      .resetType =
          (reg.reset() ? ::ResetType::SyncReset : ::ResetType::NoReset),
      .resetEdge = sv::EventControl::AtPosEdge};

  auto svRegValue = lowerRegCommon(regOperandsAndAttrs, rewriter);
  rewriter.replaceOp(reg, {svRegValue});

  return success();
}

LogicalResult
RegLower::matchAndRewrite(RegOp reg, ArrayRef<Value> operands,
                          ConversionPatternRewriter &rewriter) const {

  RegOperandsAndAttrs regOperandsAndAttrs{.loc = reg.getLoc(),
                                          .regAttrs = reg->getAttrDictionary(),
                                          .resultType =
                                              reg.getResult().getType(),
                                          .clockEdge = reg.clockEdge(),
                                          .input = reg.input(),
                                          .clk = reg.clk(),
                                          .enable = reg.enable(),
                                          .reset = reg.reset(),
                                          .resetValue = reg.resetValue(),
                                          .resetType = reg.resetType(),
                                          .resetEdge = reg.resetEdge()};

  auto svRegValue = lowerRegCommon(regOperandsAndAttrs, rewriter);
  rewriter.replaceOp(reg, {svRegValue});

  return success();
}

void SeqToSVPass::runOnOperation() {
  ModuleOp top = getOperation();
  MLIRContext &ctxt = getContext();

  ConversionTarget target(ctxt);
  target.addIllegalDialect<SeqDialect>();
  target.addLegalDialect<sv::SVDialect>();
  RewritePatternSet patterns(&ctxt);
  patterns.add<CompRegLower>(&ctxt);
  patterns.add<RegLower>(&ctxt);

  if (failed(applyPartialConversion(top, target, std::move(patterns))))
    signalPassFailure();
}

namespace circt {
namespace seq {
std::unique_ptr<OperationPass<ModuleOp>> createSeqLowerToSVPass() {
  return std::make_unique<SeqToSVPass>();
}
} // namespace seq
} // namespace circt

namespace {
#define GEN_PASS_REGISTRATION
#include "circt/Dialect/Seq/SeqPasses.h.inc"
} // namespace

void circt::seq::registerSeqPasses() { registerPasses(); }
