//===- SVExtractTestCode.cpp - SV Simulation Extraction Pass --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This transformation pass extracts simulation constructs to submodules.  It
// will take simulation operations, write, finish, assert, assume, and cover and
// extract them and the dataflow into them into a separate module.  This module
// is then instantiated in the original module.
//
//===----------------------------------------------------------------------===//

#include "PassDetail.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/SV/SVPasses.h"
#include "mlir/IR/BlockAndValueMapping.h"
#include "mlir/IR/Builders.h"

#include <set>

using namespace mlir;
using namespace circt;
using namespace sv;

//===----------------------------------------------------------------------===//
// StubExternalModules Helpers
//===----------------------------------------------------------------------===//

// Reimplemented from SliceAnalysis to use a worklist rather than recursion and
// non-insert ordered set.
static void getBackwardSliceSimple(Operation *rootOp,
                                   SmallPtrSetImpl<Operation *> &backwardSlice,
                                   std::function<bool(Operation *)> filter) {
  SmallVector<Operation *> worklist;
  worklist.push_back(rootOp);

  while (!worklist.empty()) {
    Operation *op = worklist.back();
    worklist.pop_back();

    if (!op || op->hasTrait<mlir::OpTrait::IsIsolatedFromAbove>())
      continue;

    // Evaluate whether we should keep this def.
    // This is useful in particular to implement scoping; i.e. return the
    // transitive backwardSlice in the current scope.
    if (filter && !filter(op))
      continue;

    for (auto en : llvm::enumerate(op->getOperands())) {
      auto operand = en.value();
      if (auto *definingOp = operand.getDefiningOp()) {
        if (backwardSlice.count(definingOp) == 0)
          worklist.push_back(definingOp);
      } else if (auto blockArg = operand.dyn_cast<BlockArgument>()) {
        Block *block = blockArg.getOwner();
        Operation *parentOp = block->getParentOp();
        // TODO: determine whether we want to recurse backward into the other
        // blocks of parentOp, which are not technically backward unless they
        // flow into us. For now, just bail.
        assert(parentOp->getNumRegions() == 1 &&
               parentOp->getRegion(0).getBlocks().size() == 1);
        if (backwardSlice.count(parentOp) == 0)
          worklist.push_back(parentOp);
      } else {
        llvm_unreachable("No definingOp and not a block argument.");
      }
    }

    backwardSlice.insert(op);
  }

  // Don't insert the top level operation, we just queried on it and don't
  // want it in the results.
  backwardSlice.erase(rootOp);
}

// Compute the dataflow for a set of ops.
static void dataflowSlice(SmallPtrSetImpl<Operation *> &ops,
                          SmallPtrSetImpl<Operation *> &results) {
  for (auto op : ops) {
    getBackwardSliceSimple(op, results, [](Operation *testOp) -> bool {
      return !isa<sv::ReadInOutOp>(testOp) && !isa<hw::InstanceOp>(testOp) &&
             !isa<sv::PAssignOp>(testOp) && !isa<sv::BPAssignOp>(testOp);
    });
  }
}

// Compute the ops defining the blocks a set of ops are in.
static void blockSlice(SmallPtrSetImpl<Operation *> &ops,
                       SmallPtrSetImpl<Operation *> &blocks) {
  for (auto op : ops) {
    while (!isa<hw::HWModuleOp>(op->getParentOp())) {
      op = op->getParentOp();
      blocks.insert(op);
    }
  }
}

// Aggressively mark operations to be moved to the new module.  This leaves
// maximum flexibility for optimization after removal of the nodes from the
// old module.
static SmallPtrSet<Operation *, 16>
computeCloneSet(SmallPtrSetImpl<Operation *> &roots) {
  SmallPtrSet<Operation *, 16> results;
  // Get Dataflow for roots
  dataflowSlice(roots, results);

  // Get Blocks
  SmallPtrSet<Operation *, 8> blocks;
  blockSlice(roots, blocks);
  blockSlice(results, blocks);

  // Make sure dataflow to block args (if conds, etc) is included
  dataflowSlice(blocks, results);

  // include the blocks and roots to clone
  results.insert(roots.begin(), roots.end());
  results.insert(blocks.begin(), blocks.end());

  return results;
}

StringRef getNameForPort(Value val, ArrayAttr modulePorts) {
  if (auto readinout = dyn_cast_or_null<ReadInOutOp>(val.getDefiningOp())) {
    if (auto wire = dyn_cast<WireOp>(readinout.input().getDefiningOp()))
      return wire.name();
    if (auto reg = dyn_cast<RegOp>(readinout.input().getDefiningOp()))
      return reg.name();
  } else if (auto bv = val.dyn_cast<BlockArgument>()) {
    return modulePorts[bv.getArgNumber()].cast<StringAttr>().getValue();
  }
  return "";
}

// Given a set of values, construct a module and bind instance of that module
// that passes those values through.  Returns the new module and the instance
// pointing to it.
static hw::HWModuleOp createModuleForCut(hw::HWModuleOp op,
                                         SetVector<Value> &inputs,
                                         BlockAndValueMapping &cutMap,
                                         StringRef suffix, StringRef path,
                                         StringRef fileName) {
  OpBuilder b(op->getParentOfType<mlir::ModuleOp>()->getRegion(0));

  // Construct the ports, this is just the input Values
  SmallVector<hw::ModulePortInfo> ports;
  {
    auto srcPorts = op.argNames();
    for (auto port : llvm::enumerate(inputs))
      ports.push_back({b.getStringAttr(getNameForPort(port.value(), srcPorts)),
                       hw::INPUT, port.value().getType(), port.index()});
  }

  // Create the module, setting the output path if indicated
  auto newMod = b.create<hw::HWModuleOp>(
      op.getLoc(),
      b.getStringAttr((getVerilogModuleNameAttr(op).getValue() + suffix).str()),
      ports);
  if (!path.empty())
    newMod->setAttr("output_file", hw::OutputFileAttr::get(
                                       b.getStringAttr(path),
                                       b.getStringAttr(""), b.getBoolAttr(true),
                                       b.getBoolAttr(true), op.getContext()));

  // Update the mapping from old values to cloned values
  for (auto port : llvm::enumerate(inputs))
    cutMap.map(port.value(), newMod.body().getArgument(port.index()));
  cutMap.map(op.getBodyBlock(), newMod.getBodyBlock());

  // Add an instance in the old module for the extracted module
  b = OpBuilder::atBlockTerminator(op.getBodyBlock());
  auto inst = b.create<hw::InstanceOp>(
      op.getLoc(), ArrayRef<Type>(), ("InvisibleBind" + suffix).str(),
      newMod.getName(), inputs.getArrayRef(), DictionaryAttr(),
      b.getStringAttr(
          ("__ETC_" + getVerilogModuleNameAttr(op).getValue() + suffix).str()));
  inst->setAttr("doNotPrint", b.getBoolAttr(true));
  b = OpBuilder::atBlockEnd(
      &op->getParentOfType<mlir::ModuleOp>()->getRegion(0).front());
  auto bindOp = b.create<sv::BindOp>(op.getLoc(), b.getSymbolRefAttr(inst));
  bindOp->setAttr(
      "output_file",
      hw::OutputFileAttr::get(b.getStringAttr(""), b.getStringAttr(fileName),
                              b.getBoolAttr(true), b.getBoolAttr(true),
                              op.getContext()));
  return newMod;
}

// Some blocks have terminators, some don't
static void setInsertPointToEndOrTerminator(OpBuilder &builder, Block *block) {
  if (!block->empty() && isa<hw::HWModuleOp>(block->getParentOp()))
    builder.setInsertionPoint(&block->back());
  else
    builder.setInsertionPointToEnd(block);
}

// Shallow clone, which we use to not clone the content of blocks, doesn't
// clone the regions, so create all the blocks we need and update the mapping.
static void addBlockMapping(BlockAndValueMapping &cutMap, Operation *oldOp,
                            Operation *newOp) {
  assert(oldOp->getNumRegions() == newOp->getNumRegions());
  for (size_t i = 0, e = oldOp->getNumRegions(); i != e; ++i) {
    auto &oldRegion = oldOp->getRegion(i);
    auto &newRegion = newOp->getRegion(i);
    for (auto oi = oldRegion.begin(), oe = oldRegion.end(); oi != oe; ++oi) {
      cutMap.map(&*oi, &newRegion.emplaceBlock());
    }
  }
}

static bool hasOoOArgs(hw::HWModuleOp newMod, Operation *op) {
  for (auto arg : op->getOperands()) {
    auto argOp = arg.getDefiningOp(); // may be null
    if (!argOp)
      continue;
    if (argOp->getParentOfType<hw::HWModuleOp>() != newMod)
      return true;
  }
  return false;
}

// Do the cloning, which is just a pre-order traversal over the module looking
// for marked ops.
static void migrateOps(hw::HWModuleOp oldMod, hw::HWModuleOp newMod,
                       SmallPtrSetImpl<Operation *> &depOps,
                       BlockAndValueMapping &cutMap) {
  SmallVector<Operation *, 16> lateBoundOps;
  OpBuilder b = OpBuilder::atBlockBegin(newMod.getBodyBlock());
  oldMod.walk<WalkOrder::PreOrder>([&](Operation *op) {
    if (depOps.count(op)) {
      setInsertPointToEndOrTerminator(b, cutMap.lookup(op->getBlock()));
      auto newOp = b.cloneWithoutRegions(*op, cutMap);
      addBlockMapping(cutMap, op, newOp);
      if (hasOoOArgs(newMod, newOp))
        lateBoundOps.push_back(newOp);
    }
  });
  // update any operand which was emitted before it's defining op was.
  for (auto op : lateBoundOps)
    for (unsigned argidx = 0, e = op->getNumOperands(); argidx < e; ++argidx) {
      Value arg = op->getOperand(argidx);
      if (cutMap.contains(arg))
        op->setOperand(argidx, cutMap.lookup(arg));
    }
}

//===----------------------------------------------------------------------===//
// StubExternalModules Pass
//===----------------------------------------------------------------------===//

namespace {

struct SVExtractTestCodeImplPass
    : public SVExtractTestCodeBase<SVExtractTestCodeImplPass> {
  void runOnOperation() override;

private:
  void doModule(hw::HWModuleOp module, std::function<bool(Operation *)> fn,
                StringRef suffix, StringRef path) {
    // Find Operations of interest.
    SmallPtrSet<Operation *, 8> roots;
    module->walk([&fn, &roots](Operation *op) {
      if (fn(op))
        roots.insert(op);
    });
    // No Ops?  No problem.
    if (roots.empty())
      return;
    StringRef fileName;
    // Check if the assert/assume/cover op has the output_file attribute.
    // How to handle different path attributes on multiple ops?
    for (auto extractOp : roots)
      if (extractOp->hasAttr("output_file")) {
        path = extractOp->getAttrOfType<hw::OutputFileAttr>("output_file")
                   .directory()
                   .getValue();
        fileName = extractOp->getAttrOfType<hw::OutputFileAttr>("output_file")
                       .name()
                       .getValue();
        break;
      }

    // Find the data-flow and structural ops to clone.  Result includes roots.
    auto opsToClone = computeCloneSet(roots);
    // Find the dataflow into the clone set
    SetVector<Value> inputs;
    for (auto op : opsToClone)
      for (auto arg : op->getOperands()) {
        auto argOp = arg.getDefiningOp(); // may be null
        if (!opsToClone.count(argOp))
          inputs.insert(arg);
      }

    // Make a module to contain the clone set, with arguments being the cut
    BlockAndValueMapping cutMap;
    auto bmod =
        createModuleForCut(module, inputs, cutMap, suffix, path, fileName);
    // do the clone
    migrateOps(module, bmod, opsToClone, cutMap);
    // erase old operations of interest
    for (auto op : roots)
      op->erase();
  }
};

} // end anonymous namespace

void SVExtractTestCodeImplPass::runOnOperation() {
  auto *topLevelModule = getOperation().getBody();
  for (auto &op : topLevelModule->getOperations())
    if (auto rtlmod = dyn_cast<hw::HWModuleOp>(op)) {
      // Extract two sets of ops to different modules
      auto isAssert = [](Operation *op) -> bool {
        return isa<AssertOp>(op) || isa<AssumeOp>(op) || isa<FinishOp>(op) ||
               isa<FWriteOp>(op);
      };
      auto isCover = [](Operation *op) -> bool { return isa<CoverOp>(op); };

      doModule(rtlmod, isAssert, "_assert", "generated/asserts");
      doModule(rtlmod, isCover, "_cover", "generated/covers");
    }
}

std::unique_ptr<Pass> circt::sv::createSVExtractTestCodePass() {
  return std::make_unique<SVExtractTestCodeImplPass>();
}
