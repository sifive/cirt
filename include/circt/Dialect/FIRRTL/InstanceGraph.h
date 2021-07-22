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
#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/iterator.h"
#include <deque>

namespace circt {
namespace firrtl {

class InstanceGraphNode;

/// This is an edge in the InstanceGraph. This tracks a specific instantiation
/// of a module.
class InstanceRecord {
public:
  InstanceRecord(InstanceOp instance, InstanceGraphNode *parent,
                 InstanceGraphNode *target)
      : instance(instance), parent(parent), target(target) {}

  /// Get the InstanceOp that this is tracking.
  InstanceOp getInstance() const { return instance; }

  /// Get the module where the InstanceOp lives.
  InstanceGraphNode *getParent() const { return parent; }

  /// Get the module which the InstanceOp is instantiating.
  InstanceGraphNode *getTarget() const { return target; }

private:
  /// The InstanceOp that this is tracking.
  InstanceOp instance;

  /// This is the module where the InstanceOp lives.
  InstanceGraphNode *parent;

  /// This is the module which the InstanceOp is instantiating.
  InstanceGraphNode *target;
};

/// This is a Node in the InstanceGraph.  Each node represents a Module in a
/// Circuit.  Both external modules and regular modules can be represented by
/// this class. It is possible to efficiently iterate all modules instantiated
/// by this module, as well as all instantiations of this module.
class InstanceGraphNode {
  using EdgeVec = std::deque<InstanceRecord>;
  using UseVec = std::vector<InstanceRecord *>;

  static InstanceRecord *unwrap(EdgeVec::value_type &value) {
    return &value;
  }
  class InstanceIterator final
      : public llvm::mapped_iterator<EdgeVec::iterator, decltype(&unwrap)> {
  public:
    /// Initializes the result type iterator to the specified result iterator.
    InstanceIterator(EdgeVec::iterator it)
        : llvm::mapped_iterator<EdgeVec::iterator, decltype(&unwrap)>(
              it, &unwrap) {}
  };

public:
  InstanceGraphNode() : module(nullptr), depth(0) {}

  /// Get the module that this node is tracking.
  Operation *getModule() const { return module; }

  /// Iterate the instance records in this module.
  using iterator = InstanceIterator;
  iterator begin() { return iterator(moduleInstances.begin()); }
  iterator end() { return iterator(moduleInstances.end()); }
  llvm::iterator_range<iterator> instances() {
    return llvm::make_range(begin(), end());
  }

  /// Iterate the instance records which instantiate this module.
  using use_iterator = UseVec::iterator;
  use_iterator uses_begin() { return moduleUses.begin(); }
  use_iterator uses_end() { return moduleUses.end(); }
  llvm::iterator_range<use_iterator> uses() {
    return llvm::make_range(uses_begin(), uses_end());
  }

  size_t getDepth() { return depth; }

private:
  /// Record a new instance op in the body of this module. Returns a newly
  /// allocated InstanceRecord which will be owned by this node.
  InstanceRecord *recordInstance(InstanceOp instance,
                                 InstanceGraphNode *target);

  /// Record that a module instantiates this module.
  void recordUse(InstanceRecord *record);

  /// The module.
  Operation *module;

  /// List of instance operations in this module.  This member owns the
  /// InstanceRecords, which may be pointed to by other InstanceGraohNode's use
  /// lists.
  EdgeVec moduleInstances;

  /// List of instances which instantiate this module.
  UseVec moduleUses;

  ///  The depth of a node in a DAG is the length of the longest path from a
  ///  source to the node.
  size_t depth;

  // Provide access to the constructor.
  friend class InstanceGraph;
};

/// This graph tracks modules and where they are instantiated. This is intended
/// to be used as a cached analysis on FIRRTL circuits.  This class can be used
/// to walk the modules efficiently in a bottom-up or top-down order.
///
/// To use this class, retrieve a cached copy from the analysis manager:
///   auto &instanceGraph = getAnalysis<InstanceGraph>(getOperation());
class InstanceGraph {

  /// Storage for InstanceGraphNodes.
  using NodeVec = std::deque<InstanceGraphNode>;

  /// Iterator that unwraps a unique_ptr to return a regular pointer.
  static InstanceGraphNode *unwrap(NodeVec::value_type &value) {
    return &value;
  }
  struct NodeIterator final
      : public llvm::mapped_iterator<NodeVec::iterator,
                                     decltype(&unwrap)> {
    /// Initializes the result type iterator to the specified result iterator.
    NodeIterator(NodeVec::iterator it)
        : llvm::mapped_iterator<NodeVec::iterator, decltype(&unwrap)>(
              it, &unwrap) {}
  };

public:
  /// Create a new module graph of a circuit.  This must be called on a FIRRTL
  /// CircuitOp.
  explicit InstanceGraph(Operation *operation);

  /// Get the node corresponding to the top-level module of a circuit.
  InstanceGraphNode *getTopLevelNode();

  /// Look up an InstanceGraphNode for a module. Operation must be an FModuleOp
  /// or an FExtModuleOp.
  InstanceGraphNode *lookup(Operation *op);

  /// Lookup an InstanceGraphNode for a module. Operation must be an FModuleOp
  /// or an FExtModuleOp.
  InstanceGraphNode *operator[](Operation *op) { return lookup(op); }

  /// Look up the referenced module from an InstanceOp. This will use a
  /// hashtable lookup to find the module, where
  /// InstanceOp.getReferencedModule() will be a linear search through the IR.
  Operation *getReferencedModule(InstanceOp op);

  /// Iterate through all modules.
  using iterator = NodeIterator;
  iterator begin() { return nodes.begin(); }
  iterator end() { return nodes.end(); }

  /// Return the node of greatest depth in the graph that is an ancestor of both
  /// node1 and node2, which is a lowest common ancestor of node1 and node2.
  /// If there are multiple LCAs, then this will return any one of them.
  InstanceGraphNode *getLCA(InstanceGraphNode *node1, InstanceGraphNode *node2);

private:
  /// Get the node corresponding to the module.  If the node has does not exist
  /// yet, it will be created.
  InstanceGraphNode *getOrAddNode(StringRef name);

  /// Lookup an module by name.
  InstanceGraphNode *lookup(StringRef name);

  /// The storage for graph nodes, with deterministic iteration.
  NodeVec nodes;

  /// This maps each operation to its graph node.
  llvm::StringMap<unsigned> nodeMap;
};

} // namespace firrtl
} // namespace circt

namespace llvm {
template <>
struct GraphTraits<circt::firrtl::InstanceGraphNode> {
  using NodeType = circt::firrtl::InstanceGraphNode;
  using NodeRef = NodeType *;

  // Helper for getting the module referenced by the instance op.
  static NodeRef getChild(const circt::firrtl::InstanceRecord *record) {
    return record->getTarget();
  }

  using ChildIteratorType =
      llvm::mapped_iterator<NodeType::iterator, decltype(&getChild)>;

  static NodeRef getEntryNode(NodeRef node) { return node; }
  static ChildIteratorType child_begin(NodeRef node) {
    return {node->begin(), &getChild};
  }
  static ChildIteratorType child_end(NodeRef node) {
    return {node->end(), &getChild};
  }
};

template <>
struct GraphTraits<circt::firrtl::InstanceGraph *>
    : public GraphTraits<circt::firrtl::InstanceGraphNode> {
  using nodes_iterator = circt::firrtl::InstanceGraph::iterator;

  static NodeRef getEntryNode(circt::firrtl::InstanceGraph *instanceGraph) {
    return instanceGraph->getTopLevelNode();
  }
  static nodes_iterator
  nodes_begin(circt::firrtl::InstanceGraph *instanceGraph) {
    return instanceGraph->begin();
  }
  static nodes_iterator nodes_end(circt::firrtl::InstanceGraph *instanceGraph) {
    return instanceGraph->end();
  }
};

// Provide graph traits for iterating the modules in inverse order.
template <>
struct GraphTraits<Inverse<circt::firrtl::InstanceGraphNode *>> {
  using NodeType = circt::firrtl::InstanceGraphNode;
  using NodeRef = NodeType *;

  // Helper for getting the module containing the instance op.
  static NodeRef getParent(const circt::firrtl::InstanceRecord *record) {
    return record->getParent();
  }

  using ChildIteratorType =
      llvm::mapped_iterator<NodeType::use_iterator, decltype(&getParent)>;

  static NodeRef getEntryNode(Inverse<NodeRef> inverse) {
    return inverse.Graph;
  }
  static ChildIteratorType child_begin(NodeRef node) {
    return {node->uses_begin(), &getParent};
  }
  static ChildIteratorType child_end(NodeRef node) {
    return {node->uses_end(), &getParent};
  }
};

} // end namespace llvm
#endif // CIRCT_DIALECT_FIRRTL_INSTANCEGRAPH_H
