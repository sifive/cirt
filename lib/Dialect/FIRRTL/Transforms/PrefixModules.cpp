//===- PrefixModules.cpp - Prefix module names pass -------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the PrefixModules pass.
//
//===----------------------------------------------------------------------===//

#include "./PassDetails.h"
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "circt/Dialect/FIRRTL/FIRRTLTypes.h"
#include "circt/Dialect/FIRRTL/FIRRTLVisitors.h"
#include "circt/Dialect/FIRRTL/InstanceGraph.h"
#include "circt/Dialect/FIRRTL/Passes.h"
#include "circt/Support/LLVM.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/Support/Debug.h"

using namespace circt;
using namespace firrtl;

//===----------------------------------------------------------------------===//
// Pass Infrastructure
//===----------------------------------------------------------------------===//

namespace {

/// This pass finds modules annotated with NestedPrefixAnnotation and prefixes
/// module names using the string stored in the annotation.
/// This pass prefixes every module instantiated under the annotated root
/// module's hierarchy. If a module is instantiated under two different prefix
/// hierarchies, it will be duplicated and each module will have one prefix
/// applied.
///
/// This pass works in two phases:
///  1. Create a list of prefixes for each module. This works by walking the
///     the instance graph in reverse post-order, and adding any instantiator's
///     prefixes to the instantiatee's prefixes.
///  2. Clone every module for each prefix it must have.
///
class PrefixModulesPass : public PrefixModulesBase<PrefixModulesPass> {
  void runOnOperation() override;
};

} // namespace

void PrefixModulesPass::runOnOperation() {
  //   auto annoClassID = NamedAttribute(
  //       Identifier::get("class", context),
  //       StringAttr::get(context, "firrtl.transforms.FlattenAnnotation"));
  //   auto classAttr = DictionaryAttr::getWithSorted(context, {flattenId});
  //   auto prefixID =
  //

  auto &instanceGraph = getAnalysis<InstanceGraph>();
  auto circuitOp = getOperation();
  for (auto n :
       llvm::post_order(const_cast<const InstanceGraph *>(&instanceGraph))) {
    llvm::errs() << "visit\n";
    n->getModule()->dump();
  }

  //   auto topModule =
  //       circuitOp.get
  //
  //       // This maps a module name to the name of each clone of this module.
  //       using RenameMap =
  //           llvm::DenseMap<SymbolAttr, llvm::DenseSet<SymbolRefAttr>>;
  //
  //   RenameMap moduleRenames;
  //
  //   // Maps a ModuleOp to all list of all prefixes that must be applied to
  //   it. using PrefixMap = llvm::DenseMap<ModuleOp, std::vector>;
  //
  //   // This is the prefix to apply to any module name.
  //   std::string prefix = "";
  //
  //   SmallVector<FModuleOp, 16> worklist;
  //
  //   // Calculate the required prefixes of each module. A module may be
  //   required
  //   // to have multiple prefixes, and it have to be clones for each unique
  //   // prefix.
  //   for (FModuleOp module : circuitOp.getOps<FModuleOp>()) {
  //     // If the module has a required prefix
  //     if (auto prefixAttr = getPrefix(module)) {
  //     }
  //   }
}

std::unique_ptr<mlir::Pass> circt::firrtl::createPrefixModulesPass() {
  return std::make_unique<PrefixModulesPass>();
}
