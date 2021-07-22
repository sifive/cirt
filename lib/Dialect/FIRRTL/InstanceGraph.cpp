//===- InstanceGraph.cpp - Instance Graph -----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/FIRRTL/InstanceGraph.h"

using namespace circt;
using namespace firrtl;

InstanceRecord *InstanceGraphNode::recordInstance(InstanceOp instance,
                                                  InstanceGraphNode *target) {
  moduleInstances.emplace_back(instance, this, target);
  return &moduleInstances.back();
}

void InstanceGraphNode::recordUse(InstanceRecord *record) {
  moduleUses.push_back(record);
}

InstanceGraph::InstanceGraph(Operation *operation) {
  auto circuitOp = cast<CircuitOp>(operation);

  // We insert the top level module first in to the node map.  Getting the node
  // here is enough to ensure that it is the first one added.
  getOrAddNode(circuitOp.name());

  for (auto &op : *circuitOp.getBody()) {
    if (auto extModule = dyn_cast<FExtModuleOp>(op)) {
      auto *currentNode = getOrAddNode(extModule.getName());
      currentNode->module = extModule;
    }
    if (auto module = dyn_cast<FModuleOp>(op)) {
      auto *currentNode = getOrAddNode(module.getName());
      currentNode->module = module;
      // Find all instance operations in the module body.
      module.body().walk([&](InstanceOp instanceOp) {
        // Add an edge to indicate that this module instantiates the target.
        auto *targetNode = getOrAddNode(instanceOp.moduleName());
        auto *instanceRecord =
            currentNode->recordInstance(instanceOp, targetNode);
        targetNode->recordUse(instanceRecord);
        targetNode->depth = std::max(targetNode->depth, currentNode->depth + 1);
      });
    }
  }
}

InstanceGraphNode *InstanceGraph::getTopLevelNode() {
  // The graph always puts the top level module in the array first.
  if (!nodes.size())
    return nullptr;
  return &nodes[0];
}

InstanceGraphNode *InstanceGraph::lookup(StringRef name) {
  auto it = nodeMap.find(name);
  assert(it != nodeMap.end() && "Module not in InstanceGraph!");
  return &nodes[it->second];
}

InstanceGraphNode *InstanceGraph::lookup(Operation *op) {
  if (auto extModule = dyn_cast<FExtModuleOp>(op)) {
    return lookup(extModule.getName());
  }
  if (auto module = dyn_cast<FModuleOp>(op)) {
    return lookup(module.getName());
  }
  llvm_unreachable("Can only look up module operations.");
}

InstanceGraphNode *InstanceGraph::getOrAddNode(StringRef name) {
  // Try to insert an InstanceGraphNode. If its not inserted, it returns
  // an iterator pointing to the node.
  auto itAndInserted = nodeMap.try_emplace(name, 0);
  auto &index = itAndInserted.first->second;
  if (itAndInserted.second) {
    // This is a new node, we have to add an element to the NodeVec.
    nodes.emplace_back();
    // Store the node storage index in to the map.
    index = nodes.size() - 1;
    return &nodes.back();
  }
  return &nodes[index];
}

Operation *InstanceGraph::getReferencedModule(InstanceOp op) {
  return lookup(op.moduleName())->getModule();
}

InstanceGraphNode *InstanceGraph::getLCA(InstanceGraphNode *node1,
                                         InstanceGraphNode *node2) {
  // Algorithm:
  // 1. Record all ancestors of node1 in set node1Ancestors.
  // 2. Visit all ancestors of node2.
  //  2.1. If node2 ancestor also exists in node1Ancestors set
  //    2.1.1 And its depth is largest of the common ancestors, then
  //      2.1.1.1 This is a candidate LCA.

  // Returns nullptr, if the two nodes donot have any common ancestor.
  InstanceGraphNode *lcaNode = nullptr;
  SmallVector<InstanceGraphNode *, 8> nodesQ;
  SmallPtrSet<InstanceGraphNode *, 16> node1Ancestors;
  nodesQ.push_back(node1);
  // Collect all the ancestors of the node1.
  while (!nodesQ.empty()) {
    InstanceGraphNode *node = nodesQ.back();
    nodesQ.pop_back();
    auto it = node1Ancestors.insert(node);
    if (!it.second)
      continue;
    for (auto instanceOp : node->uses())
      nodesQ.push_back(instanceOp->getParent());
  }
  // Reuse the Queue, to collect all ancestors of node2.
  nodesQ.push_back(node2);
  // Initialize the depth of the default LCA to 0.
  size_t lcaDepth = 0;
  // Record the set of ancestors already visited.
  SmallPtrSet<InstanceGraphNode *, 16> node2Ancestors;
  // Visit all ancestors of node2.
  while (!nodesQ.empty()) {
    InstanceGraphNode *node = nodesQ.back();
    nodesQ.pop_back();
    // Record the ancestors already visited.
    auto it = node2Ancestors.insert(node);
    // Ignore if node already visited.
    if (!it.second)
      continue;
    // If node2 ancestor is also an ancestor of node1, then it a candidate LCA.
    // A common ancestor with largest depth is the LCA. In case there are
    // multiple LCAs, this slects any one of them.
    if (node1Ancestors.contains(node) && node->depth >= lcaDepth) {
      lcaNode = node;
      lcaDepth = node->depth;
    }
    for (auto instanceOp : node->uses())
      nodesQ.push_back(instanceOp->getParent());
  }

  return lcaNode;
}
