//===- ModuleInliner.cpp - FIRRTL module inlining ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements FIRRTL module instance inlining.
//
//===----------------------------------------------------------------------===//

#include "PassDetails.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "circt/Dialect/FIRRTL/FIRRTLTypes.h"
#include "circt/Dialect/FIRRTL/FIRRTLVisitors.h"
#include "circt/Dialect/FIRRTL/Passes.h"
#include "circt/Support/LLVM.h"
#include "mlir/IR/BlockAndValueMapping.h"

using namespace circt;
using namespace firrtl;

//===----------------------------------------------------------------------===//
// Module Inlining Support
//===----------------------------------------------------------------------===//

/// If this operation or any child operation has a name, add the prefix to that
/// operation's name.
static void rename(StringRef prefix, Operation *op) {
  llvm::TypeSwitch<Operation *>(op)
      .Case<CMemOp, InstanceOp, MemOp, MemoryPortOp, NodeOp, RegOp, RegResetOp,
            SMemOp, WireOp>([&](auto op) {
        op.nameAttr(
            StringAttr::get(op.getContext(), (prefix + op.name()).str()));
      });
  // Recursively rename any child operations.
  for (auto &region : op->getRegions())
    for (auto &block : region)
      for (auto &op : block)
        rename(prefix, &op);
}

/// Clone an operation, mapping used values and results with the mapper, and
/// apply the prefix to the name of the operation. This will clone to the
/// insert point of the builder.
static void cloneAndRename(StringRef prefix, OpBuilder &b,
                           BlockAndValueMapping &mapper, Operation &op) {
  auto newOp = b.clone(op, mapper);
  rename(prefix, newOp);
}

/// This function is used before inlining a module, to handle the conversion
/// between module ports and instance results. For every port in the target
/// module, create a wire, and assign a mapping from each module port to the
/// wire. When the body of the module is cloned, the value of the wire will be
/// used instead of the module's ports.
static SmallVector<Value> mapPortsToWires(StringRef prefix, OpBuilder &b,
                                          BlockAndValueMapping &mapper,
                                          FModuleOp target) {
  SmallVector<Value> wires;
  auto portInfo = target.getPorts();
  for (unsigned i = 0, e = target.getNumArguments(); i < e; ++i) {
    auto arg = target.getArgument(i);
    // Get the type of the wire.
    auto type = arg.getType().cast<FIRRTLType>();
    auto wire = b.create<WireOp>(target.getLoc(), type,
                                 (prefix + portInfo[i].getName()).str());
    wires.push_back(wire);
    mapper.map(arg, wire.getResult());
  }
  return wires;
}

/// This function is used after inlining a module, to handle the conversion
/// between module ports and instance results. This maps each wire to the
/// result of the instance operation.  When future operations are cloned from
/// the current block, they will use the value of the wire instead of the
/// instance results.
static void mapResultsToWires(BlockAndValueMapping &mapper,
                              SmallVectorImpl<Value> &wires,
                              InstanceOp instance) {
  for (unsigned i = 0, e = instance.getNumResults(); i < e; ++i) {
    auto result = instance.getResult(i);
    auto wire = wires[i];
    mapper.map(result, wire);
  }
}

/// Inlines, flattens, and removes dead modules in a circuit.
///
/// The inliner works in a top down fashion, starting from the top level module,
/// and inlines every possible instance. With this method of recursive top-down
/// inlining, each operation will be cloned directly to its final location.
///
/// The inliner uses a worklist to track which modules need to be processed.
/// When an instance op is not inlined, the referenced module is added to the
/// worklist. When the inliner is complete, it deletes every un-processed
/// module: either all instances of the module were inlined, or it was not
/// reachable from the top level module.
///
/// During the inlining process, every cloned operation with a name must be
/// prefixed with the instance's name. The top-down process means that we know
/// the entire desired prefix when we clone an operation, and can set the name
/// attribute once. This means that we will not create any intermediate name
/// attributes (which will be interned by the compiler), and helps keep down the
/// total memory usage.
namespace {
class Inliner {
public:
  /// Initialize the inliner to run on this circuit.
  Inliner(CircuitOp circuit);

  /// Run the inliner.
  void run();

private:
  /// Returns true if the operation is annotated to be flattened.
  bool shouldFlatten(Operation *op);

  /// Returns true if the operation is annotated to be inlined.
  bool shouldInline(Operation *op);

  /// Flattens a target module in to the insertion point of the builder,
  /// renaming all operations using the prefix.  This clones all operations from
  /// the target, and does not trigger inlining on the target itself.
  void flattenInto(StringRef prefix, OpBuilder &b, BlockAndValueMapping &mapper,
                   FModuleOp target);

  /// Inlines a target module in to the location of the build, prefixing all
  /// operations with prefix.  This clones all operations from the target, and
  /// does not trigger inlining on the target itself.
  void inlineInto(StringRef prefix, OpBuilder &b, BlockAndValueMapping &mapper,
                  FModuleOp target);

  /// Recursively flatten all instances in a module.
  void flattenInstances(FModuleOp module);

  /// Inline any instances in the module which were marked for inlining.
  void inlineInstances(FModuleOp module);

  CircuitOp circuit;
  MLIRContext *context;

  // A symbol table with references to each module in a circuit.
  SymbolTable symbolTable;

  /// The set of live modules.  Anything not recorded in this set will be
  /// removed by dead code elimination.
  DenseSet<Operation *> liveModules;

  /// Worklist of modules to process for inlining or flattening.
  SmallVector<FModuleOp, 16> worklist;
};
} // namespace

bool Inliner::shouldFlatten(Operation *op) {
  return AnnotationSet(op).hasAnnotation("firrtl.transforms.FlattenAnnotation");
}

bool Inliner::shouldInline(Operation *op) {
  return AnnotationSet(op).hasAnnotation("firrtl.passes.InlineAnnotation");
}

void Inliner::flattenInto(StringRef prefix, OpBuilder &b,
                          BlockAndValueMapping &mapper, FModuleOp target) {
  for (auto &op : *target.getBodyBlock()) {
    // If its not an instance op, clone it and continue.
    auto instance = dyn_cast<InstanceOp>(op);
    if (!instance) {
      cloneAndRename(prefix, b, mapper, op);
      continue;
    }

    // If its not a regular module we can't inline it. Mark is as live.
    auto module = symbolTable.lookup(instance.moduleName());
    auto target = dyn_cast<FModuleOp>(module);
    if (!target) {
      liveModules.insert(module);
      cloneAndRename(prefix, b, mapper, op);
      continue;
    }

    // Create the wire mapping for results + ports.
    auto nestedPrefix = (prefix + instance.name() + "_").str();
    auto wires = mapPortsToWires(nestedPrefix, b, mapper, target);
    mapResultsToWires(mapper, wires, instance);

    // Unconditionally flatten all instance operations.
    flattenInto(nestedPrefix, b, mapper, target);
  }
}

void Inliner::flattenInstances(FModuleOp module) {
  for (auto &op : llvm::make_early_inc_range(*module.getBodyBlock())) {
    // If its not an instance op, skip it.
    auto instance = dyn_cast<InstanceOp>(op);
    if (!instance)
      continue;

    // If its not a regular module we can't inline it. Mark is as live.
    auto module = symbolTable.lookup(instance.moduleName());
    auto target = dyn_cast<FModuleOp>(module);
    if (!target) {
      liveModules.insert(module);
      continue;
    }

    // Create the wire mapping for results + ports. We RAUW the results instead
    // of mapping them.
    BlockAndValueMapping mapper;
    OpBuilder b(instance);
    auto nestedPrefix = (instance.name() + "_").str();
    auto wires = mapPortsToWires(nestedPrefix, b, mapper, target);
    for (unsigned i = 0, e = instance.getNumResults(); i < e; ++i)
      instance.getResult(i).replaceAllUsesWith(wires[i]);

    // Recursively flatten the target module.
    flattenInto(nestedPrefix, b, mapper, target);

    // Erase the replaced instance.
    instance.erase();
  }
}

void Inliner::inlineInto(StringRef prefix, OpBuilder &b,
                         BlockAndValueMapping &mapper, FModuleOp target) {
  for (auto &op : *target.getBodyBlock()) {
    // If its not an instance op, clone it and continue.
    auto instance = dyn_cast<InstanceOp>(op);
    if (!instance) {
      cloneAndRename(prefix, b, mapper, op);
      continue;
    }

    // If its not a regular module we can't inline it. Mark is as live.
    auto module = symbolTable.lookup(instance.moduleName());
    auto target = dyn_cast<FModuleOp>(module);
    if (!target) {
      liveModules.insert(module);
      cloneAndRename(prefix, b, mapper, op);
      continue;
    }

    // If we aren't inlining the target, add it to the work list.
    if (!shouldInline(target)) {
      if (liveModules.insert(target).second) {
        worklist.push_back(target);
      }
      cloneAndRename(prefix, b, mapper, op);
      continue;
    }

    // Create the wire mapping for results + ports.
    auto nestedPrefix = (prefix + instance.name() + "_").str();
    auto wires = mapPortsToWires(nestedPrefix, b, mapper, target);
    mapResultsToWires(mapper, wires, instance);

    // Inline the module, it can be marked as flatten and inline.
    if (shouldFlatten(target)) {
      flattenInto(nestedPrefix, b, mapper, target);
    } else {
      inlineInto(nestedPrefix, b, mapper, target);
    }
  }
}

void Inliner::inlineInstances(FModuleOp module) {
  for (auto &op : llvm::make_early_inc_range(*module.getBodyBlock())) {
    // If its not an instance op, skip it.
    auto instance = dyn_cast<InstanceOp>(op);
    if (!instance)
      continue;

    // If its not a regular module we can't inline it. Mark is as live.
    auto module = symbolTable.lookup(instance.moduleName());
    auto target = dyn_cast<FModuleOp>(module);
    if (!target) {
      liveModules.insert(module);
      continue;
    }

    // If we aren't inlining the target, add it to the work list.
    if (!shouldInline(target)) {
      if (liveModules.insert(target).second) {
        worklist.push_back(target);
      }
      continue;
    }

    // Create the wire mapping for results + ports. We RAUW the results instead
    // of mapping them.
    BlockAndValueMapping mapper;
    OpBuilder b(instance);
    auto nestedPrefix = (instance.name() + "_").str();
    auto wires = mapPortsToWires(nestedPrefix, b, mapper, target);
    for (unsigned i = 0, e = instance.getNumResults(); i < e; ++i)
      instance.getResult(i).replaceAllUsesWith(wires[i]);

    // Inline the module, it can be marked as flatten and inline.
    if (shouldFlatten(target)) {
      flattenInto(nestedPrefix, b, mapper, target);
    } else {
      inlineInto(nestedPrefix, b, mapper, target);
    }

    // Erase the replaced instance.
    instance.erase();
  }
}

Inliner::Inliner(CircuitOp circuit)
    : circuit(circuit), context(circuit.getContext()), symbolTable(circuit) {}

void Inliner::run() {
  auto topModule = circuit.getMainModule();
  // Mark the top module as live, so it doesn't get deleted.
  liveModules.insert(topModule);

  // If the top module is not a regular module, there is nothing to do.
  if (auto fmodule = dyn_cast<FModuleOp>(topModule))
    worklist.push_back(fmodule);

  // If the module is marked for flattening, flatten it. Otherwise, inline
  // every instance marked to be inlined.
  while (!worklist.empty()) {
    auto module = worklist.pop_back_val();
    if (shouldFlatten(module)) {
      flattenInstances(module);
      continue;
    }
    inlineInstances(module);

    // Delete the flatten annotations. Any module with the inline annotation
    // will be deleted, as there won't be any remaining instances of it.
    AnnotationSet(module).removeAnnotationsWithClass(
        "firrtl.transforms.FlattenAnnotation");
  }

  // Delete all unreferenced modules.
  for (auto &op : llvm::make_early_inc_range(*circuit.getBody())) {
    if (!liveModules.count(&op))
      op.erase();
  }
}

//===----------------------------------------------------------------------===//
// Pass Infrastructure
//===----------------------------------------------------------------------===//

namespace {
class InlinerPass : public InlinerBase<InlinerPass> {
  void runOnOperation() override {
    Inliner inliner(getOperation());
    inliner.run();
  }
};
} // namespace

std::unique_ptr<mlir::Pass> circt::firrtl::createInlinerPass() {
  return std::make_unique<InlinerPass>();
}
