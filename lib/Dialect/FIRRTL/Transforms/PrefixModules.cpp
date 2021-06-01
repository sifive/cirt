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
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "circt/Dialect/FIRRTL/InstanceGraph.h"
#include "circt/Dialect/FIRRTL/Passes.h"
#include "circt/Support/LLVM.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SmallSet.h"

using namespace circt;
using namespace firrtl;

namespace {

/// This is the prefix which will be applied to a module.
struct PrefixInfo {

  /// The string to prefix on to the module and all of its children.
  StringRef prefix;

  /// If true, this prefix applies to the module itself.  If false, the prefix
  /// only applies to it's children.
  bool inclusive;
};

/// This maps a FModuleOp to a list of all prefixes that need to be applied.
/// When a module has multiple prefixes, it will be cloned for each one.
using PrefixMap = DenseMap<StringRef, llvm::SmallSet<std::string, 1>>;

/// Get the PrefixInfo for a module. This reads the annotations on module.  If
/// the module is not annotated, the prefix returned will be empty.
PrefixInfo getPrefixInfo(FModuleOp module) {
  AnnotationSet annotations(module);

  // Get the annotation from the module.
  auto dict = annotations.getAnnotation(
      "sifive.enterprise.firrtl.NestedPrefixModulesAnnotation");
  if (!dict)
    return {"", false};
  Annotation anno(dict);

  // Get the prefix from the annotation.
  StringRef prefix = "";
  if (auto prefixAttr = anno.getMember<StringAttr>("prefix"))
    prefix = prefixAttr.getValue();

  // Get the inclusive flag from the annotation.
  bool inclusive = false;
  if (auto inclusiveAttr = anno.getMember<BoolAttr>("inclusive"))
    inclusive = inclusiveAttr.getValue();

  return {prefix, inclusive};
}

/// If there is an inclusive prefix attached to the module, return it.
StringRef getPrefix(FModuleOp module) {
  auto prefixInfo = getPrefixInfo(module);
  if (prefixInfo.inclusive)
    return prefixInfo.prefix;
  return "";
}

/// Applies the prefix to the module.  This will update the required prefixes of
/// any referenced module in the prefix map.
void renameModuleBody(PrefixMap &prefixMap, std::string prefix,
                      FModuleOp module) {
  module.body().walk([&](InstanceOp instance) {
    auto target = dyn_cast<FModuleOp>(instance.getReferencedModule());

    // Skip this rename if the instance is an external module.
    if (!target)
      return;

    // Record that we must prefix the target module with the current prefix.
    prefixMap[target.getName()].insert(prefix);

    // Fixup this instance op to use the prefixed module name.  Note that the
    // FModuleOp will be renamed later.
    auto targetName = (prefix + getPrefix(target) + target.getName()).str();
    auto targetSymbol =
        FlatSymbolRefAttr::get(target->getContext(), targetName);
    instance.moduleNameAttr(targetSymbol);
  });
}

/// Apply all required renames to the current module.  This will update the
/// prefix map for any referenced module.
void renameModule(PrefixMap &prefixMap, FModuleOp module) {
  // If the module is annotated to have a prefix, it will be applied after the
  // parent's prefix.
  auto prefixInfo = getPrefixInfo(module);
  auto innerPrefix = prefixInfo.prefix;

  // We only add the annotated prefix to the module name if it is inclusive.
  auto moduleName = module.getName().str();
  if (prefixInfo.inclusive) {
    moduleName = (innerPrefix + moduleName).str();
  }

  auto &prefixes = prefixMap[module.getName()];

  // If there are no required prefixes of this module, there is an implicit
  // requirement that it has an empty prefix. This empty prefix will be applied
  // to to all modules instantiated by this module.
  if (prefixes.empty())
    prefixes.insert("");

  // Rename the module for each required prefix. This will clone the module
  // once for each prefix but the last.
  auto outerPrefix = prefixes.begin();
  for (unsigned i = 1, e = prefixes.size(); i < e; ++i, ++outerPrefix) {
    auto moduleClone = cast<FModuleOp>(OpBuilder(module).clone(*module));
    moduleClone.setName(*outerPrefix + moduleName);
    renameModuleBody(prefixMap, (*outerPrefix + innerPrefix).str(),
                     moduleClone);
  }
  // The last prefix renames the module in place.
  if (outerPrefix != prefixes.end()) {
    module.setName(*outerPrefix + moduleName);
    renameModuleBody(prefixMap, (*outerPrefix + innerPrefix).str(), module);
  }
}

/// This pass finds modules annotated with NestedPrefixAnnotation and prefixes
/// module names using the string stored in the annotation.  This pass prefixes
/// every module instantiated under the annotated root module's hierarchy. If a
/// module is instantiated under two different prefix hierarchies, it will be
/// duplicated and each module will have one prefix applied.
class PrefixModulesPass : public PrefixModulesBase<PrefixModulesPass> {
  void runOnOperation() override;
};

} // namespace

void PrefixModulesPass::runOnOperation() {
  auto &instanceGraph = getAnalysis<InstanceGraph>();
  auto circuitOp = getOperation();

  // If the main module is prefixed, we have to update the CircuitOp.
  auto mainModule = cast<FModuleOp>(circuitOp.getMainModule());
  auto prefix = getPrefix(mainModule);
  if (!prefix.empty()) {
    auto newName = (prefix + circuitOp.name()).str();
    circuitOp.nameAttr(StringAttr::get(&getContext(), newName));
  }

  // This is a map from a module name to new prefixes to be applied.
  PrefixMap prefixMap;

  // Walk all Modules in a top-down order.  For each module, look at the list of
  // required prefixes to be applied.
  SmallPtrSet<InstanceGraphNode *, 16> visited;
  for (auto module : circuitOp.getOps<FModuleOp>()) {
    auto *current = instanceGraph.getOrAddNode(module);
    for (auto node : llvm::inverse_post_order_ext(current, visited)) {
      if (auto module = dyn_cast<FModuleOp>(node->getModule()))
        renameModule(prefixMap, module);
    }
  }
}

std::unique_ptr<mlir::Pass> circt::firrtl::createPrefixModulesPass() {
  return std::make_unique<PrefixModulesPass>();
}
