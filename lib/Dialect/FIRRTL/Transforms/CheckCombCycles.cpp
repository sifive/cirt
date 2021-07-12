//===- CheckCombCycles.cpp - FIRRTL check combinational cycles --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the FIRRTL combintational cycles detection pass.
//
//===----------------------------------------------------------------------===//

#include "PassDetails.h"
#include "circt/Dialect/FIRRTL/FIRRTLVisitors.h"
#include "circt/Dialect/FIRRTL/InstanceGraph.h"
#include "circt/Dialect/FIRRTL/Passes.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SCCIterator.h"

using namespace circt;
using namespace firrtl;

namespace {
class CombNodeImpl;
}

using CombNode = CombNodeImpl *;
using ValueOrOpType = PointerUnion<Value, Operation *>;

//===----------------------------------------------------------------------===//
// CombNodeImpl class
//===----------------------------------------------------------------------===//

namespace {
/// This is the implementation of a combinational node.
class CombNodeImpl {
public:
  CombNodeImpl(ValueOrOpType valOrOp, unsigned fieldID = 0)
      : valOrOp(valOrOp), fieldID(fieldID) {}

  bool operator==(const CombNodeImpl &other) const {
    return valOrOp == other.valOrOp && fieldID == other.fieldID;
  }

  CombNode *child_begin() { return children.begin(); }
  CombNode *child_end() { return children.end(); }

  void addChild(CombNode child) { children.emplace_back(child); }
  void clearChilds() { children.clear(); }

  ValueOrOpType getValOrOp() const { return valOrOp; }
  unsigned getFieldID() const { return fieldID; }

  llvm::hash_code hash_value() const {
    return llvm::hash_combine(valOrOp.getOpaqueValue(), fieldID);
  }

private:
  /// The underneath value or operation of this node.
  ValueOrOpType valOrOp;

  /// The field ID this node is referencing to.
  unsigned fieldID;

  /// Store all children of this node in the graph.
  SmallVector<CombNode, 4> children;
};
} // namespace

//===----------------------------------------------------------------------===//
// CombGraph class
//===----------------------------------------------------------------------===//

namespace {
// This is a combinational graph contains a set of nodes.
class CombGraph {
public:
  /// Get a node from the graph if it exists. Otherwise, allocate a new node and
  /// add to the graph.
  CombNode getOrAddNode(ValueOrOpType valOrOp, unsigned fieldID = 0) {
    auto node = CombNodeImpl(valOrOp, fieldID);
    auto it = nodes.find(&node);
    if (it != nodes.end())
      return *it;
    auto newNode = new (allocator) CombNodeImpl(std::move(node));
    nodes.insert(newNode);
    return newNode;
  }

  /// Get a node from the graph if it exists.
  CombNode getNode(ValueOrOpType valOrOp, unsigned fieldID = 0) {
    auto node = CombNodeImpl(valOrOp, fieldID);
    auto it = nodes.find(&node);
    if (it != nodes.end())
      return *it;
    return nullptr;
  }

private:
  /// For allocating memories for combinational nodes.
  llvm::BumpPtrAllocator allocator;

  /// A set of combinational nodes.
  DenseSet<CombNode> nodes;
};
} // namespace

//===----------------------------------------------------------------------===//
// GraphTraits on CombNode
//===----------------------------------------------------------------------===//

namespace llvm {
template <>
struct GraphTraits<CombNode> {
  using NodeRef = CombNode;
  using ChildIteratorType = NodeRef *;

  static NodeRef getEntryNode(NodeRef node) { return node; }

  static inline ChildIteratorType child_begin(NodeRef node) {
    return node->child_begin();
  }
  static inline ChildIteratorType child_end(NodeRef node) {
    return node->child_end();
  }
};
} // namespace llvm

//===----------------------------------------------------------------------===//
// Detector class
//===----------------------------------------------------------------------===//

using CombPaths = SmallDenseMap<int64_t, SmallVector<int64_t>>;

namespace {
class Detector : public FIRRTLVisitor<Detector, bool> {
public:
  Detector(FModuleOp module, DenseMap<Operation *, CombPaths> &combPathsMap,
           InstanceGraph &instanceGraph)
      : module(module), combPathsMap(combPathsMap),
        instanceGraph(instanceGraph) {}

  /// Each operation visitor is supposed to finish the following tasks:
  ///   1. Create a node for each of its results if the result is not a
  ///   sequential value, such as register and memory write port;
  ///   2. Create edges between its arguments and results if applicable.
  using FIRRTLVisitor<Detector, bool>::visitStmt;
  using FIRRTLVisitor<Detector, bool>::visitDecl;
  using FIRRTLVisitor<Detector, bool>::visitExpr;

  /// Statement handlers.
  template <typename ConnectOpType>
  bool handleConnectOp(ConnectOpType op);
  bool visitStmt(ConnectOp op) { return handleConnectOp(op); }
  bool visitStmt(PartialConnectOp op) { return handleConnectOp(op); }
  bool visitStmt(WhenOp op) {
    op->emitOpError("unsupported, please run -firrtl-expand-whens first");
    return false;
  }

  /// Declaration handlers.
  bool visitDecl(InstanceOp op);
  bool visitDecl(MemOp op);
  bool visitDecl(RegOp op) { return true; }
  bool visitDecl(RegResetOp op) { return true; }

  /// Expression handlers.
  bool visitExpr(SubfieldOp op);
  bool visitExpr(SubindexOp op) {
    op->emitOpError("unsupported, please run -firrtl-lower-types first");
    return false;
  };
  bool visitExpr(SubaccessOp op) {
    op->emitOpError("unsupported, please run -firrtl-lower-types first");
    return false;
  };

  /// Handle other operations that don't have concrete visitors.
  bool visitUnhandledOp(Operation *op);

  /// Build combinational graph of the module with the visitor dispatcher.
  void buildCombGraph();

  /// Detect combinational cycles in the module. Also detect combinational paths
  /// between IOs of the module and record these paths in the `combPathsMap`.
  void detect();

private:
  /// The module detected on.
  FModuleOp module;

  /// The global combinational paths map.
  DenseMap<Operation *, CombPaths> &combPathsMap;

  /// The global instance graph.
  InstanceGraph &instanceGraph;

  /// The local combinational graph.
  CombGraph combGraph;
};
} // namespace

template <typename ConnectOpType>
bool Detector::handleConnectOp(ConnectOpType op) {
  // Create an edge between the `source` value and `destination` value if both
  // exist in the graph.
  auto srcNode = combGraph.getNode(op.src());
  auto dstNode = combGraph.getNode(op.dest());
  if (srcNode && dstNode) {
    auto connectNode = combGraph.getOrAddNode(op.getOperation());
    srcNode->addChild(connectNode);
    connectNode->addChild(dstNode);
  }
  return true;
}

/// Declaration handlers.
bool Detector::visitDecl(InstanceOp op) {
  SmallVector<CombNode, 8> nodes;
  for (auto result : op.getResults())
    nodes.push_back(combGraph.getOrAddNode(result));

  // Query the combinational paths between IOs and create edges.
  for (auto combPath :
       combPathsMap.lookup(instanceGraph.getReferencedModule(op))) {
    auto srcNode = nodes[combPath.first];
    for (auto i : combPath.second)
      srcNode->addChild(nodes[i]);
  }
  return true;
}

bool Detector::visitDecl(MemOp op) {
  // Combinational paths only exist when reading latency is zero.
  if (op.readLatency() != 0)
    return true;

  // Traverse every ports of the memory.
  for (unsigned i = 0, e = op.getNumResults(); i < e; ++i) {
    // Combinational paths only exist in `read` or `readwrite` ports.
    auto portKind = op.getPortKind(i);
    if (portKind == MemOp::PortKind::Write)
      continue;

    auto portType = op.getPortType(i).cast<BundleType>();

    auto enIndex = portType.getElementIndex("en");
    auto addrIndex = portType.getElementIndex("addr");
    auto dataIndex = portKind == MemOp::PortKind::ReadWrite
                         ? portType.getElementIndex("rdata")
                         : portType.getElementIndex("data");

    // Travers all subfields of the memory port. Combinational paths can only
    // start from `en` or `addr`, and propagate to `rdata` (for `readwrite`
    // port) or `data` (for `read` port).
    SmallVector<CombNode, 2> srcNodes;
    CombNode dstNode = nullptr;
    for (auto &use : op.getResult(i).getUses()) {
      auto subField = dyn_cast<SubfieldOp>(use.getOwner());
      if (!subField) {
        use.getOwner()->emitOpError(
            "should only be used by SubFieldOp, please run "
            "-firrtl-lower-types first");
        return false;
      }

      // Record source nodes and destination node of combinational paths.
      auto fieldIndex = subField.fieldIndex();
      if (fieldIndex == enIndex || fieldIndex == addrIndex)
        srcNodes.push_back(combGraph.getOrAddNode(use.get(), fieldIndex + 1));
      else if (fieldIndex == dataIndex)
        dstNode = combGraph.getOrAddNode(use.get(), fieldIndex + 1);
    }

    // Now, we can create edges.
    if (dstNode) {
      for (auto srcNode : srcNodes)
        srcNode->addChild(dstNode);
    }
  }
  return true;
}

/// Expression handlers.
bool Detector::visitExpr(SubfieldOp op) {
  auto definingOp = op.input().getDefiningOp();
  if (!definingOp || !isa<MemOp>(definingOp)) {
    op->emitOpError("input must be a port of a MemOp, please run "
                    "-firrtl-lower-types first");
    return false;
  }

  auto portType = op.input().getType().cast<BundleType>();
  auto fieldIndex = op.fieldIndex();
  // If the memory port is not on a combinational path, it was not be inserted
  // into the graph as a node in the MemOp handler.
  if (auto memPortNode = combGraph.getNode(op.input(), fieldIndex + 1)) {
    auto node = combGraph.getOrAddNode(op.result());
    // We use `isFlip` to determine the direction of the edge.
    if (portType.getElement(fieldIndex).isFlip)
      memPortNode->addChild(node);
    else
      node->addChild(memPortNode);
  }
  return true;
}

/// Handle other operations that don't have concrete visitors.
bool Detector::visitUnhandledOp(Operation *op) {
  // Create an edge between each exist input node and each output node.
  SmallVector<CombNode, 4> dstNodes;
  for (auto result : op->getResults())
    dstNodes.push_back(combGraph.getOrAddNode(result));

  for (auto operand : op->getOperands()) {
    if (auto srcNode = combGraph.getNode(operand)) {
      for (auto dstNode : dstNodes)
        srcNode->addChild(dstNode);
    }
  }
  return true;
}

/// Build combinational graph of the module with the visitor dispatcher.
void Detector::buildCombGraph() {
  // Each module port should be a node in the graph.
  for (auto arg : module.getArguments())
    combGraph.getOrAddNode(arg);

  // Traverse all operations in the module and create combinational nodes and
  // edges.
  module.walk([&](Operation *op) {
    if (!dispatchVisitor(op))
      return WalkResult::interrupt();
    return WalkResult::advance();
  });
}

/// Detect combinational cycles in the module. Also detect combinational paths
/// between IOs of the module and record these paths in the `combPathsMap`.
void Detector::detect() {
  buildCombGraph();

  // Add a dummy node in the begining to launch a Tarjan's SCC algorithm.
  auto dummyNode = combGraph.getOrAddNode(nullptr);
  for (auto connect : module.getOps<ConnectOp>()) {
    if (auto node = combGraph.getNode(connect.dest()))
      dummyNode->addChild(node);
  }

  // Lauch SCC iteration and report diagnostics.
  using SCCIterator = llvm::scc_iterator<CombNode>;
  for (auto SCC = SCCIterator::begin(dummyNode); !SCC.isAtEnd(); ++SCC) {
    if (SCC.hasCycle()) {
      auto errorDiag = mlir::emitError(
          module.getLoc(), "detected combinational SCC in the module");
      for (auto it = SCC->rbegin(); it < SCC->rend(); ++it) {
        auto node = *it;
        auto nodeVal = node->getValOrOp().dyn_cast<Value>();
        auto nodeOp = node->getValOrOp().dyn_cast<Operation *>();

        auto &noteDiag =
            errorDiag.attachNote(nodeVal ? nodeVal.getLoc() : nodeOp->getLoc());

        noteDiag << "see node in the combinational SCC";
        // Indicate field ID if it is not zero.
        if (auto fieldID = node->getFieldID())
          noteDiag << ", field ID is " << fieldID;
        // Indicate result number if the definining op has multiple results.
        if (nodeVal) {
          auto definingOp = nodeVal.getDefiningOp();
          if (definingOp && definingOp->getNumResults() > 1) {
            auto resultNumber = nodeVal.dyn_cast<OpResult>().getResultNumber();
            noteDiag << ", result number is " << resultNumber;
          }
        }
      }
    }
  }
  dummyNode->clearChilds();

  // Collect all inputs of the module and a port direction list.
  SmallVector<BlockArgument, 4> inputs;
  SmallVector<bool, 8> directionList;
  unsigned index = 0;
  for (auto port : module.getPorts()) {
    directionList.push_back(port.isOutput());
    if (!port.isOutput())
      inputs.push_back(module.getPortArgument(index));
    ++index;
  }

  // Launch a DFS for each input to find outputs that are combinationally
  // connected to it.
  for (auto input : inputs) {
    auto inputIndex = input.getArgNumber();
    for (auto node : llvm::depth_first<CombNode>(combGraph.getNode(input))) {
      if (auto nodeVal = node->getValOrOp().dyn_cast<Value>()) {
        if (auto output = nodeVal.dyn_cast<BlockArgument>()) {
          auto outputIndex = output.getArgNumber();
          if (directionList[outputIndex])
            combPathsMap[module][inputIndex].push_back(outputIndex);
        }
      }
    }
  }
}

//===----------------------------------------------------------------------===//
// Hashing CombNode
//===----------------------------------------------------------------------===//

namespace llvm {
template <>
struct DenseMapInfo<CombNode> {
  static CombNode getEmptyKey() {
    auto pointer = llvm::DenseMapInfo<void *>::getEmptyKey();
    return static_cast<CombNode>(pointer);
  }
  static CombNode getTombstoneKey() {
    auto pointer = llvm::DenseMapInfo<void *>::getTombstoneKey();
    return static_cast<CombNode>(pointer);
  }
  static unsigned getHashValue(const CombNodeImpl *node) {
    return node->hash_value();
  }
  static bool isEqual(const CombNodeImpl *lhs, const CombNodeImpl *rhs) {
    auto empty = getEmptyKey();
    auto tombstone = getTombstoneKey();
    if (lhs == empty || lhs == tombstone || rhs == empty || rhs == tombstone)
      return lhs == rhs;
    return *lhs == *rhs;
  }
};
} // namespace llvm

//===----------------------------------------------------------------------===//
// Pass Infrastructure
//===----------------------------------------------------------------------===//

namespace {
/// This pass constructs a local graph for each module to detect combinational
/// cycles. To capture the cross-module combinational cycles, this pass inlines
/// the combinational paths between IOs of its subinstances into a subgraph and
/// encodes them in a `combPathsMap`.
class CheckCombCyclesPass : public CheckCombCyclesBase<CheckCombCyclesPass> {
  void runOnOperation() override {
    auto &instanceGraph = getAnalysis<InstanceGraph>();
    // Traverse modules in a post order to make sure the combinational paths
    // between IOs of a module have been detected and recorded in `combPathsMap`
    // before we handle its parent modules.
    for (auto node : llvm::post_order<InstanceGraph *>(&instanceGraph)) {
      // TODO: Handle FExtModuleOp with annotations.
      if (auto module = dyn_cast<FModuleOp>(node->getModule()))
        Detector(module, combPathsMap, instanceGraph).detect();
    }
    markAllAnalysesPreserved();
  }

private:
  // Global combinational paths map. Used to store the combinational paths
  // between IOs of modules in the circuit.
  DenseMap<Operation *, CombPaths> combPathsMap;
};
} // namespace

std::unique_ptr<mlir::Pass> circt::firrtl::createCheckCombCyclesPass() {
  return std::make_unique<CheckCombCyclesPass>();
}
