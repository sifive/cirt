//===- PrintInstanceGraph.cpp - Print the instance graph --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===----------------------------------------------------------------------===//
//
// Print the module hierarchy.
//
//===----------------------------------------------------------------------===//

#include "PassDetails.h"
#include "circt/Dialect/FIRRTL/InstanceGraph.h"
#include "circt/Dialect/FIRRTL/Passes.h"
#include "llvm/Support/DOTGraphTraits.h"
#include "llvm/Support/GraphWriter.h"
#include "llvm/Support/raw_ostream.h"

using namespace circt;
using namespace firrtl;

template <>
struct llvm::DOTGraphTraits<InstanceGraph *>
    : public llvm::DefaultDOTGraphTraits {
  using DefaultDOTGraphTraits::DefaultDOTGraphTraits;

  static std::string getNodeLabel(InstanceGraphNode *node, InstanceGraph *) {
    // The name of the graph node is the module name.
    auto *op = node->getModule();
    auto depth = "(" + std::to_string(node->getDepth()) + ")";
    if (auto module = dyn_cast<FModuleOp>(op)) {
      return module.getName().str() + depth;
    }
    if (auto extModule = dyn_cast<FExtModuleOp>(op)) {
      return extModule.getName().str() + depth;
    }
    return "<<unknown>>";
  }
  template <typename Iterator>
  static std::string getEdgeAttributes(const InstanceGraphNode *node,
                                       Iterator it, InstanceGraph *) {
    // Set an edge label that is the name of the instance.
    auto *instanceRecord = *it.getCurrent();
    auto instanceOp = instanceRecord->getInstance();
    return ("label=" + instanceOp.name()).str();
  }
};

namespace {
struct PrintInstanceGraphPass
    : public PrintInstanceGraphBase<PrintInstanceGraphPass> {
  PrintInstanceGraphPass(raw_ostream &os) : os(os) {}
  void runOnOperation() override {
    auto circuitOp = getOperation();
    auto &instanceGraph = getAnalysis<InstanceGraph>();
    llvm::WriteGraph(os, &instanceGraph, /*ShortNames=*/false,
                     circuitOp.name());
    markAllAnalysesPreserved();

    if (printLCA)
      for (InstanceGraphNode *node1 : instanceGraph) {
        for (InstanceGraphNode *node2 : instanceGraph) {
          if (node1 == node2)
            continue;
          auto lcaNode = instanceGraph.getLCA(node1, node2);
          if (lcaNode != nullptr)
            os << "\n LCA of ("
               << llvm::DOTGraphTraits<InstanceGraph *>::getNodeLabel(
                      node1, &instanceGraph)
               << ","
               << llvm::DOTGraphTraits<InstanceGraph *>::getNodeLabel(
                      node2, &instanceGraph)
               << ")="
               << llvm::DOTGraphTraits<InstanceGraph *>::getNodeLabel(
                      lcaNode, &instanceGraph);
        }
      }
  }
  raw_ostream &os;
};
} // end anonymous namespace

std::unique_ptr<mlir::Pass> circt::firrtl::createPrintInstanceGraphPass() {
  return std::make_unique<PrintInstanceGraphPass>(llvm::errs());
}
