//===- InstanceGraph.h - Instance graph -------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the FIRRTL InstanceGraph, which is similar to a CallGraph.
//
//===----------------------------------------------------------------------===//
#ifndef CIRCT_DIALECT_FIRRTL_INSTANCEGRAPH_H
#define CIRCT_DIALECT_FIRRTL_INSTANCEGRAPH_H

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "circt/Support/LLVM.h"
#include "mlir/IR/Operation.h"
#include "mlir/IR/OperationSupport.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Casting.h"

#include <memory>

// TODO: do i need these?
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/MapVector.h"

namespace circt {
namespace firrtl {

class InstanceGraphNode {
  using EdgeSetT = llvm::SmallVector<InstanceGraphNode *, 1>;

public:
  InstanceGraphNode(Operation *module) : module(module) {}
  InstanceGraphNode(InstanceGraphNode &&other)
      : module(other.module), instances(std::move(other.instances)) {
    other.module = nullptr;
  }

  /// Get the module this
  Operation *getModule() const { return module; }

  using iterator = EdgeSetT::const_iterator;
  iterator begin() const { return instances.begin(); }
  iterator end() const { return instances.end(); }

private:
  /// Record that a module instantiates this module.
  void addInstance(InstanceGraphNode *node) { instances.push_back(node); }

  /// The module.
  Operation *module;

  /// List of modules which instantiate this one.
  EdgeSetT instances;

  // Provide access to the constructor.
  friend class InstanceGraph;
};

/// This graph tracks modules and where they are instantiated. This is the
/// inverse of the regular instance graph.
class InstanceGraph {

  using NodeMapT = std::vector<std::unique_ptr<InstanceGraphNode>>;
  static InstanceGraphNode *unwrap(const NodeMapT::value_type &value) {
    return value.get();
  }

  /// Iterator that unwraps a unique_ptr to return a regular pointer.
  struct NodeIterator final
      : public llvm::mapped_iterator<NodeMapT::const_iterator,
                                     decltype(&unwrap)> {
    /// Initializes the result type iterator to the specified result iterator.
    NodeIterator(NodeMapT::const_iterator it)
        : llvm::mapped_iterator<NodeMapT::const_iterator, decltype(&unwrap)>(
              it, &unwrap) {}
  };

public:
  /// Create a new module graph of a circuit.  This must be called on a FIRRTL
  /// CircuitOp.
  InstanceGraph(Operation *operation);

  /// Get the node corresponding to the module.  If the node has does not exist
  /// yet, it will be created.
  InstanceGraphNode *getOrAddNode(Operation *module) {
    // Look up the node in the map. If it is a module we haven't seen before,
    // add it to the back up the vector, and update the map.
    auto itAndInserted = nodeMap.try_emplace(module, 0);
    auto &index = itAndInserted.first->second;
    if (itAndInserted.second) {
      nodes.emplace_back(new InstanceGraphNode(module));
      index = nodes.size() - 1;
    }
    return nodes[index].get();
  }

  /// Get the first node in the graph.
  using iterator = NodeIterator;
  iterator begin() const { return nodes.begin(); }
  iterator end() const { return nodes.end(); }

private:
  // This is not using a MapVector to save a bit of memory.  The
  // InstanceGraphNode holds a copy of the the Operation* used as a key, and
  // MapVector uses a vector of pair<Operation*,InstanceGraphNode>.

  /// The storage for graph nodes, with deterministic iteration.
  std::vector<std::unique_ptr<InstanceGraphNode>> nodes;

  /// This maps each operation to its graph node.
  DenseMap<Operation *, unsigned> nodeMap;
};

} // namespace firrtl
} // namespace circt

namespace llvm {
// Provide graph traits for traversing call graphs using standard graph
// traversals.
template <>
struct GraphTraits<const circt::firrtl::InstanceGraphNode *> {
  using NodeRef = circt::firrtl::InstanceGraphNode *;
  static NodeRef getEntryNode(NodeRef node) { return node; }

  // ChildIteratorType/begin/end - Allow iteration over all nodes in the graph.
  using ChildIteratorType = circt::firrtl::InstanceGraphNode::iterator;
  static ChildIteratorType child_begin(NodeRef node) { return node->begin(); }
  static ChildIteratorType child_end(NodeRef node) { return node->end(); }
};

template <>
struct GraphTraits<const circt::firrtl::InstanceGraph *>
    : public GraphTraits<const circt::firrtl::InstanceGraphNode *> {
  /// The entry node into the graph is the external node.
  static NodeRef
  getEntryNode(const circt::firrtl::InstanceGraph *instanceGraph) {
    return *instanceGraph->begin();
  }

  // nodes_iterator/begin/end - Allow iteration over all nodes in the graph
  using nodes_iterator = circt::firrtl::InstanceGraph::iterator;
  static nodes_iterator
  nodes_begin(circt::firrtl::InstanceGraph *instanceGraph) {
    return instanceGraph->begin();
  }
  static nodes_iterator nodes_end(circt::firrtl::InstanceGraph *instanceGraph) {
    return instanceGraph->end();
  }
};
} // end namespace llvm
#endif // CIRCT_DIALECT_FIRRTL_INSTANCEGRAPH_H
