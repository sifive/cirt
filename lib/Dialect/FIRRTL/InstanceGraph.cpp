//===- InstanceGraph.cpp - Instance Graph -----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "llvm/Support/Debug.h"
#include "circt/Dialect/FIRRTL/InstanceGraph.h"

using namespace mlir;
using namespace circt;
using namespace firrtl;

InstanceGraph::InstanceGraph(Operation *operation) {
  SymbolTable symbolTable(operation);
  auto circuitOp = cast<CircuitOp>(operation);
  // Find every instance operation in the circuit and add it as an edge in
  // the graph.
  for (auto module : circuitOp.getBody()->getOps<FModuleOp>()) {
    auto currentNode = getOrAddNode(module);
    module.body().walk([&](InstanceOp instanceOp) {
      auto targetModule = symbolTable.lookup(instanceOp.moduleName());
      if (!targetModule)
        return;
      // Add an edge to the target module to indicate that this module
      // instantiates the target module.
      auto targetNode = getOrAddNode(targetModule);
      targetNode->addInstance(currentNode);
    });
  }
}
