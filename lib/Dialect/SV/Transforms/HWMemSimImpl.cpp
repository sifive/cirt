//===- HWMemSimImpl.cpp - HW Memory Implementation Pass -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This transformation pass converts generated FIRRTL memory modules to
// simulation models.
//
//===----------------------------------------------------------------------===//

#include "SVPassDetail.h"
#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/SV/SVPasses.h"
#include "mlir/IR/ImplicitLocOpBuilder.h"

using namespace circt;

//===----------------------------------------------------------------------===//
// HWMemSimImplPass Pass
//===----------------------------------------------------------------------===//

namespace {
struct FirMemory {
  size_t numReadPorts;
  size_t numWritePorts;
  size_t numReadWritePorts;
  size_t dataWidth;
  size_t depth;
  size_t readLatency;
  size_t writeLatency;
  size_t readUnderWrite;
};
} // end anonymous namespace

namespace {
struct HWMemSimImplPass : public sv::HWMemSimImplBase<HWMemSimImplPass> {
  void runOnOperation() override;

private:
  void generateMemory(hw::HWModuleOp op, FirMemory mem);
};
} // end anonymous namespace

static FirMemory analyzeMemOp(hw::HWModuleGeneratedOp op) {
  FirMemory mem;
  mem.depth = op->getAttrOfType<IntegerAttr>("depth").getInt();
  mem.numReadPorts = op->getAttrOfType<IntegerAttr>("numReadPorts").getUInt();
  mem.numWritePorts = op->getAttrOfType<IntegerAttr>("numWritePorts").getUInt();
  mem.numReadWritePorts =
      op->getAttrOfType<IntegerAttr>("numReadWritePorts").getUInt();
  mem.readLatency = op->getAttrOfType<IntegerAttr>("readLatency").getUInt();
  mem.writeLatency = op->getAttrOfType<IntegerAttr>("writeLatency").getUInt();
  mem.dataWidth = op->getAttrOfType<IntegerAttr>("width").getUInt();
  mem.readUnderWrite =
      op->getAttrOfType<IntegerAttr>("readUnderWrite").getUInt();
  return mem;
}

static Value addPipelineStages(ImplicitLocOpBuilder &b, size_t stages,
                               Value clock, Value data) {
  if (!stages)
    return data;

  while (stages--) {
    auto reg = b.create<sv::RegOp>(data.getType());

    // pipeline stage
    b.create<sv::AlwaysFFOp>(sv::EventControl::AtPosEdge, clock,
                             [&]() { b.create<sv::PAssignOp>(reg, data); });
    data = b.create<sv::ReadInOutOp>(reg);
  }

  return data;
}

void HWMemSimImplPass::generateMemory(hw::HWModuleOp op, FirMemory mem) {
  ImplicitLocOpBuilder b(UnknownLoc::get(&getContext()), op.getBody());

  // Create a register for the memory.
  auto dataType = b.getIntegerType(mem.dataWidth);
  Value reg =
      b.create<sv::RegOp>(hw::UnpackedArrayType::get(dataType, mem.depth),
                          b.getStringAttr("Memory"));

  SmallVector<Value, 4> outputs;

  size_t inArg = 0;
  for (size_t i = 0; i < mem.numReadPorts; ++i) {
    Value clock = op.body().getArgument(inArg++);
    Value en = op.body().getArgument(inArg++);
    Value addr = op.body().getArgument(inArg++);
    // Add pipeline stages
    en = addPipelineStages(b, mem.readLatency, clock, en);
    addr = addPipelineStages(b, mem.readLatency, clock, addr);

    // Read Logic
    Value ren =
        b.create<sv::ReadInOutOp>(b.create<sv::ArrayIndexInOutOp>(reg, addr));
    Value x = b.create<sv::ConstantXOp>(dataType);

    Value rdata = b.create<comb::MuxOp>(en, ren, x);
    outputs.push_back(rdata);
  }

  for (size_t i = 0; i < mem.numReadWritePorts; ++i) {
    auto numStages = std::max(mem.readLatency, mem.writeLatency) - 1;
    Value clock = op.body().getArgument(inArg++);
    Value en = op.body().getArgument(inArg++);
    Value addr = op.body().getArgument(inArg++);
    Value wmode = op.body().getArgument(inArg++);
    Value wmask = op.body().getArgument(inArg++);
    Value wdata = op.body().getArgument(inArg++);

    // Add pipeline stages
    en = addPipelineStages(b, numStages, clock, en);
    addr = addPipelineStages(b, numStages, clock, addr);
    wmode = addPipelineStages(b, numStages, clock, wmode);
    wmask = addPipelineStages(b, numStages, clock, wmask);
    wdata = addPipelineStages(b, numStages, clock, wdata);

    // wire to store read result
    auto rWire = b.create<sv::WireOp>(wdata.getType());
    Value rdata = b.create<sv::ReadInOutOp>(rWire);

    // RW logic
    b.create<sv::AlwaysFFOp>(sv::EventControl::AtPosEdge, clock, [&]() {
      auto slot = b.create<sv::ArrayIndexInOutOp>(reg, addr);
      auto rcond = b.createOrFold<comb::AndOp>(
          en, b.createOrFold<comb::ICmpOp>(
                  ICmpPredicate::eq, wmode,
                  b.createOrFold<hw::ConstantOp>(wmode.getType(), 0)));
      auto wcond = b.createOrFold<comb::AndOp>(
          en, b.createOrFold<comb::AndOp>(
                  b.createOrFold<comb::ICmpOp>(
                      ICmpPredicate::eq, wmask,
                      b.createOrFold<hw::ConstantOp>(wmask.getType(), 1)),
                  wmode));

      b.create<sv::PAssignOp>(rWire, b.create<sv::ConstantXOp>(dataType));
      b.create<sv::IfOp>(
          wcond, [&]() { b.create<sv::PAssignOp>(slot, wdata); },
          [&]() {
            b.create<sv::IfOp>(rcond, [&]() {
              b.create<sv::PAssignOp>(rWire, b.create<sv::ReadInOutOp>(slot));
            });
          });
    });
    outputs.push_back(rdata);
  }

  for (size_t i = 0; i < mem.numWritePorts; ++i) {
    auto numStages = mem.writeLatency - 1;
    Value clock = op.body().getArgument(inArg++);
    Value en = op.body().getArgument(inArg++);
    Value addr = op.body().getArgument(inArg++);
    Value wmask = op.body().getArgument(inArg++);
    Value wdata = op.body().getArgument(inArg++);
    // Add pipeline stages
    en = addPipelineStages(b, numStages, clock, en);
    addr = addPipelineStages(b, numStages, clock, addr);
    wmask = addPipelineStages(b, numStages, clock, wmask);
    wdata = addPipelineStages(b, numStages, clock, wdata);

    // Write logic
    b.create<sv::AlwaysFFOp>(sv::EventControl::AtPosEdge, clock, [&]() {
      auto wcond = b.createOrFold<comb::AndOp>(
          en, b.createOrFold<comb::ICmpOp>(
                  ICmpPredicate::eq, wmask,
                  b.createOrFold<hw::ConstantOp>(wmask.getType(), 1)));
      b.create<sv::IfOp>(wcond, [&]() {
        auto slot = b.create<sv::ArrayIndexInOutOp>(reg, addr);
        b.create<sv::PAssignOp>(slot, wdata);
      });
    });
  }

  auto outputOp = op.getBodyBlock()->getTerminator();
  outputOp->setOperands(outputs);
}

void HWMemSimImplPass::runOnOperation() {
  auto topModule = getOperation().getBody();

  SmallVector<hw::HWModuleGeneratedOp> toErase;
  bool anythingChanged = false;

  for (auto op : llvm::make_early_inc_range(
           topModule->getOps<hw::HWModuleGeneratedOp>())) {
    auto oldModule = cast<hw::HWModuleGeneratedOp>(op);
    auto gen = oldModule.generatorKind();
    auto genOp = cast<hw::HWGeneratorSchemaOp>(
        SymbolTable::lookupSymbolIn(getOperation(), gen));

    if (genOp.descriptor() == "FIRRTL_Memory") {
      auto mem = analyzeMemOp(oldModule);

      OpBuilder builder(oldModule);
      auto nameAttr = builder.getStringAttr(oldModule.getName());
      auto newModule = builder.create<hw::HWModuleOp>(
          oldModule.getLoc(), nameAttr, oldModule.getPorts());
      generateMemory(newModule, mem);
      oldModule.erase();
      anythingChanged = true;
    }
  }

  if (!anythingChanged)
    markAllAnalysesPreserved();
}

std::unique_ptr<Pass> circt::sv::createHWMemSimImplPass() {
  return std::make_unique<HWMemSimImplPass>();
}
