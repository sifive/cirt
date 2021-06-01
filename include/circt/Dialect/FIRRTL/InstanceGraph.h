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
#include "llvm/ADT/SmallVector.h"
#include <memory>

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

  /// Get the module that this node is tracking.
  Operation *getModule() const { return module; }

  using iterator = EdgeSetT::iterator;
  iterator begin() { return instances.begin(); }
  iterator end() { return instances.end(); }

  using const_iterator = EdgeSetT::const_iterator;
  const_iterator begin() const { return instances.begin(); }
  const_iterator end() const { return instances.end(); }

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
    auto itAndInserted = nodeMap.try_emplace(module, 0);
    auto &index = itAndInserted.first->second;
    if (itAndInserted.second) {
      nodes.emplace_back(new InstanceGraphNode(module));
      index = nodes.size() - 1;
    }
    return nodes[index].get();
  }

  using iterator = NodeIterator;
  iterator begin() const { return nodes.begin(); }
  iterator end() const { return nodes.end(); }

private:
  // Implementation note: This is not using a MapVector to save a bit of memory.
  // The InstanceGraphNode holds a copy of the the Operation* used as a key, and
  // MapVector would duplicate this information in a
  // pair<Operation*, InstanceGraphNode>.

  /// The storage for graph nodes, with deterministic iteration.
  std::vector<std::unique_ptr<InstanceGraphNode>> nodes;

  /// This maps each operation to its graph node.
  DenseMap<Operation *, unsigned> nodeMap;
};

} // namespace firrtl
} // namespace circt

namespace llvm {

// Provide graph traits for iterating the modules in inverse order.
template <>
struct GraphTraits<Inverse<circt::firrtl::InstanceGraphNode *>> {
  using NodeType = circt::firrtl::InstanceGraphNode;
  using NodeRef = NodeType *;
  using ChildIteratorType = NodeType::iterator;

  static NodeRef getEntryNode(Inverse<NodeRef> inverse) {
    return inverse.Graph;
  }
  static ChildIteratorType child_begin(NodeRef node) { return node->begin(); }
  static ChildIteratorType child_end(NodeRef node) { return node->end(); }
};

// Provide constant graph traits for iterating the modules in inverse order.
template <>
struct GraphTraits<Inverse<const circt::firrtl::InstanceGraphNode *>> {
  using NodeType = const circt::firrtl::InstanceGraphNode;
  using NodeRef = NodeType *;
  using ChildIteratorType = NodeType::const_iterator;

  static NodeRef getEntryNode(Inverse<NodeRef> inverse) {
    return inverse.Graph;
  }
  static ChildIteratorType child_begin(NodeRef node) { return node->begin(); }
  static ChildIteratorType child_end(NodeRef node) { return node->end(); }
};

} // end namespace llvm
#endif // CIRCT_DIALECT_FIRRTL_INSTANCEGRAPH_H
