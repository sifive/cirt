//===- InferResets.cpp - Infer resets and add async reset -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the InferResets pass.
//
//===----------------------------------------------------------------------===//

#include "PassDetails.h"
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "circt/Dialect/FIRRTL/FIRRTLTypes.h"
#include "circt/Dialect/FIRRTL/InstanceGraph.h"
#include "circt/Dialect/FIRRTL/Passes.h"
#include "circt/Support/FieldRef.h"
#include "mlir/IR/ImplicitLocOpBuilder.h"
#include "llvm/ADT/EquivalenceClasses.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "infer-resets"

using llvm::BumpPtrAllocator;
using llvm::MapVector;
using llvm::SmallDenseSet;
using llvm::SmallSetVector;
using mlir::FailureOr;
using mlir::InferTypeOpInterface;
using mlir::WalkOrder;

using namespace circt;
using namespace firrtl;

//===----------------------------------------------------------------------===//
// Traits
//===----------------------------------------------------------------------===//

namespace llvm {
template <>
struct DenseMapInfo<FModuleOp> {
  static inline FModuleOp getEmptyKey() {
    return FModuleOp::getFromOpaquePointer(DenseMapInfo<void *>::getEmptyKey());
  }
  static inline FModuleOp getTombstoneKey() {
    return FModuleOp::getFromOpaquePointer(
        DenseMapInfo<void *>::getTombstoneKey());
  }
  static unsigned getHashValue(const FModuleOp &val) {
    return DenseMapInfo<Operation *>::getHashValue(val);
  }
  static bool isEqual(const FModuleOp &lhs, const FModuleOp &rhs) {
    return lhs == rhs;
  }
};
} // end namespace llvm

//===----------------------------------------------------------------------===//
// Utilities
//===----------------------------------------------------------------------===//

/// An absolute instance path.
using InstancePathRef = ArrayRef<InstanceOp>;
using InstancePath = SmallVector<InstanceOp>;

template <typename T>
static T &operator<<(T &os, InstancePathRef path) {
  os << "$root";
  for (InstanceOp inst : path)
    os << "/" << inst.name() << ":" << inst.moduleName();
  return os;
}

static StringRef getTail(InstancePathRef path) {
  if (path.empty())
    return "$root";
  auto last = path.back();
  return last.name();
}

namespace {
/// A reset domain.
struct ResetDomain {
  /// Whether this is the root of the reset domain.
  bool isTop = false;
  /// The reset signal for this domain. A null value indicates that this domain
  /// explicitly has no reset.
  Value reset;

  // Implementation details for this domain.
  Value existingValue;
  Optional<unsigned> existingPort;
  StringAttr newPortName;

  ResetDomain(Value reset) : reset(reset) {}
};
} // namespace

inline bool operator==(const ResetDomain &a, const ResetDomain &b) {
  return (a.isTop == b.isTop && a.reset == b.reset);
}
inline bool operator!=(const ResetDomain &a, const ResetDomain &b) {
  return !(a == b);
}

/// Return the name and parent module of a reset. The reset value must either be
/// a module port or a wire/node operation.
static std::pair<StringAttr, FModuleOp> getResetNameAndModule(Value reset) {
  if (auto arg = reset.dyn_cast<BlockArgument>()) {
    auto module = cast<FModuleOp>(arg.getParentRegion()->getParentOp());
    return {getModulePortName(module, arg.getArgNumber()), module};
  } else {
    auto op = reset.getDefiningOp();
    return {op->getAttrOfType<StringAttr>("name"),
            op->getParentOfType<FModuleOp>()};
  }
}

/// Return the name of a reset. The reset value must either be a module port or
/// a wire/node operation.
static inline StringAttr getResetName(Value reset) {
  return getResetNameAndModule(reset).first;
}

/// Construct a zero value of the given type using the given builder.
static Value createZeroValue(ImplicitLocOpBuilder &builder, FIRRTLType type,
                             SmallDenseMap<FIRRTLType, Value> &cache) {
  auto it = cache.find(type);
  if (it != cache.end())
    return it->second;
  auto nullBit = [&]() {
    return createZeroValue(builder, UIntType::get(builder.getContext(), 1),
                           cache);
  };
  auto value =
      TypeSwitch<FIRRTLType, Value>(type)
          .Case<ClockType>([&](auto type) {
            return builder.create<AsClockPrimOp>(nullBit());
          })
          .Case<AsyncResetType>([&](auto type) {
            return builder.create<AsAsyncResetPrimOp>(nullBit());
          })
          .Case<SIntType, UIntType>([&](auto type) {
            return builder.create<ConstantOp>(
                type, APInt::getNullValue(type.getWidth().getValueOr(1)));
          })
          .Case<BundleType>([&](auto type) {
            auto wireOp = builder.create<WireOp>(type);
            for (auto &field : llvm::enumerate(type.getElements())) {
              auto zero = createZeroValue(builder, field.value().type, cache);
              auto acc = builder.create<SubfieldOp>(field.value().type, wireOp,
                                                    field.index());
              builder.create<ConnectOp>(acc, zero);
            }
            return wireOp;
          })
          .Case<FVectorType>([&](auto type) {
            auto wireOp = builder.create<WireOp>(type);
            auto zero = createZeroValue(builder, type.getElementType(), cache);
            for (unsigned i = 0, e = type.getNumElements(); i < e; ++i) {
              auto acc = builder.create<SubindexOp>(zero.getType(), wireOp, i);
              builder.create<ConnectOp>(acc, zero);
            }
            return wireOp;
          })
          .Case<ResetType, AnalogType>(
              [&](auto type) { return builder.create<InvalidValueOp>(type); })
          .Default([](auto) {
            llvm_unreachable("switch handles all types");
            return Value{};
          });
  cache.insert({type, value});
  return value;
}

/// Construct a null value of the given type using the given builder.
static Value createZeroValue(ImplicitLocOpBuilder &builder, FIRRTLType type) {
  SmallDenseMap<FIRRTLType, Value> cache;
  return createZeroValue(builder, type, cache);
}

/// Helper function that inserts reset multiplexer into all `ConnectOp`s and
/// `PartialConnectOp`s with the given target. Looks through `SubfieldOp`,
/// `SubindexOp`, and `SubaccessOp`, and inserts multiplexers into connects to
/// these subaccesses as well. Modifies the insertion location of the builder.
/// Returns true if the `resetValue` was used in any way, false otherwise.
static bool insertResetMux(ImplicitLocOpBuilder &builder, Value target,
                           Value reset, Value resetValue) {
  // Indicates whether the `resetValue` was assigned to in some way. We use this
  // to erase unused subfield/subindex/subaccess ops on the reset value if they
  // end up unused.
  bool resetValueUsed = false;

  for (auto &use : target.getUses()) {
    Operation *useOp = use.getOwner();
    builder.setInsertionPoint(useOp);
    TypeSwitch<Operation *>(useOp)
        // Insert a mux on the value connected to the target:
        // connect(dst, src) -> connect(dst, mux(reset, resetValue, src))
        .Case<ConnectOp, PartialConnectOp>([&](auto op) {
          if (op.dest() != target)
            return;
          LLVM_DEBUG(llvm::dbgs() << "  - Insert mux into " << op << "\n");
          auto muxOp = builder.create<MuxPrimOp>(reset, resetValue, op.src());
          op.srcMutable().assign(muxOp);
          resetValueUsed = true;
        })
        // Look through subfields.
        .Case<SubfieldOp>([&](auto op) {
          auto resetSubValue =
              builder.create<SubfieldOp>(resetValue, op.fieldIndexAttr());
          if (insertResetMux(builder, op, reset, resetSubValue))
            resetValueUsed = true;
          else
            resetSubValue.erase();
        })
        // Look through subindices.
        .Case<SubindexOp>([&](auto op) {
          auto resetSubValue =
              builder.create<SubindexOp>(resetValue, op.indexAttr());
          if (insertResetMux(builder, op, reset, resetSubValue))
            resetValueUsed = true;
          else
            resetSubValue.erase();
        })
        // Look through subaccesses.
        .Case<SubaccessOp>([&](auto op) {
          auto resetSubValue =
              builder.create<SubaccessOp>(resetValue, op.index());
          if (insertResetMux(builder, op, reset, resetSubValue))
            resetValueUsed = true;
          else
            resetSubValue.erase();
        });
  }
  return resetValueUsed;
}

//===----------------------------------------------------------------------===//
// Reset Network
//===----------------------------------------------------------------------===//

namespace {

/// A reset signal.
///
/// This essentially combines the exact `FieldRef` of the signal in question
/// with a type to be used for error reporting and inferring the reset kind.
struct ResetSignal {
  ResetSignal(FieldRef field, FIRRTLType type) : field(field), type(type) {}
  bool operator<(const ResetSignal &other) const { return field < other.field; }
  bool operator==(const ResetSignal &other) const {
    return field == other.field;
  }
  bool operator!=(const ResetSignal &other) const { return !(*this == other); }

  FieldRef field;
  FIRRTLType type;
};

/// A connection made to or from a reset network.
///
/// These drives are tracked for each reset network, and are used for error
/// reporting to the user.
struct ResetDrive {
  /// What's being driven.
  ResetSignal dst;
  /// What's driving.
  ResetSignal src;
  /// The location to use for diagnostics.
  Location loc;
};

/// A list of connections to a reset network.
using ResetDrives = SmallVector<ResetDrive, 1>;

/// All signals connected together into a reset network.
using ResetNetwork = llvm::iterator_range<
    llvm::EquivalenceClasses<ResetSignal>::member_iterator>;

/// Whether a reset is sync or async.
enum class ResetKind { Async, Sync };

} // namespace

namespace llvm {
template <>
struct DenseMapInfo<ResetSignal> {
  static inline ResetSignal getEmptyKey() {
    return ResetSignal{DenseMapInfo<FieldRef>::getEmptyKey(), {}};
  }
  static inline ResetSignal getTombstoneKey() {
    return ResetSignal{DenseMapInfo<FieldRef>::getTombstoneKey(), {}};
  }
  static unsigned getHashValue(const ResetSignal &x) {
    return circt::hash_value(x.field);
  }
  static bool isEqual(const ResetSignal &lhs, const ResetSignal &rhs) {
    return lhs == rhs;
  }
};
} // namespace llvm

template <typename T>
static T &operator<<(T &os, const ResetKind &kind) {
  switch (kind) {
  case ResetKind::Async:
    return os << "async";
  case ResetKind::Sync:
    return os << "sync";
  }
}

//===----------------------------------------------------------------------===//
// Pass Infrastructure
//===----------------------------------------------------------------------===//

namespace {
/// Infer concrete reset types and insert full async reset.
///
/// This pass replaces `reset` types in the IR with a concrete `asyncreset` or
/// `uint<1>` depending on how the reset is used, and adds async resets to
/// registers in modules marked with the corresponding
/// `FullAsyncResetAnnotation`. On a high level, the pass operates as follows:
///
/// 1. Build a global graph of the resets in the design by tracing reset signals
///    through instances. This uses the `ResetNetwork` utilities and boils down
///    to finding  groups of values in the IR that are part of the same reset
///    network (i.e., somehow attached together through ports, wires, instances,
///    and connects). We use LLVM's `EquivalenceClasses` data structure to do
///    this efficiently.
///
/// 2. Infer the type of each reset network found in step 1 by looking at the
///    type of values connected to the network. This results in the network
///    being declared a sync (`uint<1>`) or async (`asyncreset`) network. If the
///    reset is never driven by a concrete type, an error is emitted.
///
/// 3. Walk the IR and update the type of wires and ports with the reset types
///    found in step 2. This will replace all `reset` types in the IR with
///    a concrete type.
///
/// 4. Visit every module in the design and determine if it has an explicit
///    reset domain annotated. Ports on and wires in the module can have a
///    `FullAsyncResetAnnotation`, which marks that port or wire as the async
///    reset for the module. A module may also carry a
///    `IgnoreFullAsyncResetAnnotation`, which marks it as being explicitly not
///    in a reset domain. These annotations are sparse; it is very much possible
///    that just the top-level module in the design has a full async reset
///    annotation. A module can only ever carry one of these annotations, which
///    puts it into one of three categories from an async reset inference
///    perspective:
///
///      a. unambiguously marks a port or wire as the module's async reset
///      b. explicitly marks it as not to have any async resets added
///      c. inherit reset
///
/// 5. For every module in the design, determine the reset domain it is in. If a
///    module carries one of the annotations, that is used as its reset domain.
///    otherwise, a module inherits the reset domain from parent modules. This
///    conceptually involves looking at all the places where a module is
///    instantiated, and recursively determining the reset domain at the
///    instantiation site. A module can only ever be in one reset domain. In
///    case it is inferred to lie in multiple ones, e.g., if it is instantiated
///    in different reset domains, an error is emitted. If successful, every
///    module is associated with a reset signal, either one of its local ports
///    or wires, or a port or wire within one of its parent modules.
///
/// 6. For every module in the design, determine how async resets shall be
///    implemented. This step handles the following distinct cases:
///
///      a. Skip a module because it is marked as having no reset domain.
///      b. Use a port or wire in the module itself as reset. This is possible
///         if the module is at the "top" of its reset domain, which means that
///         it itself carried a reset annotation, and the reset value is either
///         a port or wire of the module itself.
///      c. Route a parent module's reset through a module port and use that
///         port as the reset. This happens if the module is *not* at the "top"
///         of its reset domain, but rather refers to a value in a parent module
///         as its reset.
///
///    As a result, a module's reset domain is annotated with the existing local
///    value to reuse (port or wire), the index of an existing port to reuse,
///    and the name of an additional port to insert into its port list.
///
/// 7. For every module in the design, async resets are implemented. This
///    determines the local value to use as the reset signal and updates the
///    `reg` and `regreset` operations in the design. If the register already
///    has an async reset, it is left unchanged. If it has a sync reset, the
///    sync reset is moved into a `mux` operation on all `connect`s to the
///    register (which the Scala code base called the `RemoveResets` pass).
///    Finally the register is replaced with a `regreset` operation, with the
///    reset signal determined earlier, and a "zero" value constructed for the
///    register's type.
///
///    Determining the local reset value is trivial if step 6 found a module to
///    be of case a or b. Case c is the non-trivial one, because it requires
///    modifying the port list of the module. This is done by first determining
///    the name of the reset signal in the parent module, which is either the
///    name of the port or wire declaration. We then look for an existing
///    `asyncreset` port in the port list and reuse that as reset. If no port
///    with that name was found, or the existing port is of the wrong type, a
///    new port is inserted into the port list.
///
struct InferResetsPass : public InferResetsBase<InferResetsPass> {
  void runOnOperation() override;
  void runOnOperationInner();

  // Copy creates a new empty pass (because ResetMap has no copy constructor).
  using InferResetsBase::InferResetsBase;
  InferResetsPass(const InferResetsPass &) {}

  //===--------------------------------------------------------------------===//
  // Reset type inference

  void traceResets(CircuitOp circuit);
  void traceResets(InstanceOp inst);
  void traceResets(Value dst, Value src, Location loc);
  void traceResets(Value value);
  void traceResets(FIRRTLType dstType, Value dst, unsigned dstID,
                   FIRRTLType srcType, Value src, unsigned srcID, Location loc);

  LogicalResult inferAndUpdateResets();
  FailureOr<ResetKind> inferReset(ResetNetwork net);
  LogicalResult updateReset(ResetNetwork net, ResetKind kind);
  bool updateReset(FieldRef field, FIRRTLType resetType);

  //===--------------------------------------------------------------------===//
  // Async reset implementation

  LogicalResult collectAnnos(CircuitOp circuit);
  LogicalResult collectAnnos(FModuleOp module);

  LogicalResult buildDomains(CircuitOp circuit);
  void buildDomains(FModuleOp module, const InstancePath &instPath,
                    Value parentReset, InstanceGraph &instGraph,
                    unsigned indent = 0);

  void determineImpl();
  void determineImpl(FModuleOp module, ResetDomain &domain);

  LogicalResult implementAsyncReset();
  LogicalResult implementAsyncReset(FModuleOp module, ResetDomain &domain);
  void implementAsyncReset(Operation *op, FModuleOp module, Value actualReset,
                           SmallVectorImpl<Operation *> &deleteOps,
                           SmallVectorImpl<std::pair<Value, Value>> &connects);

  //===--------------------------------------------------------------------===//
  // Utilities

  /// Get the reset network a signal belongs to.
  ResetNetwork getResetNetwork(ResetSignal signal) {
    return llvm::make_range(resetClasses.findLeader(signal),
                            resetClasses.member_end());
  }

  /// Get the drives of a reset network.
  ResetDrives &getResetDrives(ResetNetwork net) {
    return resetDrives[*net.begin()];
  }

  /// Guess the root node of a reset network, such that we have something for
  /// the user to make sense of.
  ResetSignal guessRoot(ResetNetwork net);
  ResetSignal guessRoot(ResetSignal signal) {
    return guessRoot(getResetNetwork(signal));
  }

  //===--------------------------------------------------------------------===//
  // Analysis data

  /// A map of all traced reset networks in the circuit.
  llvm::EquivalenceClasses<ResetSignal> resetClasses;

  /// A map of all connects to and from a reset.
  DenseMap<ResetSignal, ResetDrives> resetDrives;

  /// The annotated reset for a module. A null value indicates that the module
  /// is explicitly annotated with `ignore`. Otherwise the port/wire/node
  /// annotated as reset within the module is stored.
  DenseMap<Operation *, Value> annotatedResets;

  /// The reset domain for a module. In case of conflicting domain membership,
  /// the vector for a module contains multiple elements.
  MapVector<FModuleOp, SmallVector<std::pair<ResetDomain, InstancePath>, 1>>
      domains;
};
} // namespace

void InferResetsPass::runOnOperation() {
  runOnOperationInner();
  resetClasses = llvm::EquivalenceClasses<ResetSignal>();
  resetDrives.clear();
  annotatedResets.clear();
  domains.clear();
}

void InferResetsPass::runOnOperationInner() {
  // Trace the uninferred reset networks throughout the design.
  traceResets(getOperation());

  // Infer the type of the traced resets and update the IR.
  if (failed(inferAndUpdateResets()))
    return signalPassFailure();

  // Gather the reset annotations throughout the modules.
  if (failed(collectAnnos(getOperation())))
    return signalPassFailure();

  // Build the reset domains in the design.
  if (failed(buildDomains(getOperation())))
    return signalPassFailure();

  // Determine how each reset shall be implemented.
  determineImpl();

  // Implement the async resets.
  if (failed(implementAsyncReset()))
    return signalPassFailure();
}

std::unique_ptr<mlir::Pass> circt::firrtl::createInferResetsPass() {
  return std::make_unique<InferResetsPass>();
}

ResetSignal InferResetsPass::guessRoot(ResetNetwork net) {
  ResetDrives &drives = getResetDrives(net);
  ResetSignal bestSignal = *net.begin();
  unsigned bestNumDrives = -1;

  for (auto signal : net) {
    // Don't consider `invalidvalue` for reporting as a root.
    if (isa_and_nonnull<InvalidValueOp>(
            signal.field.getValue().getDefiningOp()))
      continue;

    // Count the number of times this particular signal in the reset network is
    // assigned to.
    unsigned numDrives = 0;
    for (auto &drive : drives)
      if (drive.dst == signal)
        ++numDrives;

    // Keep track of the signal with the lowest number of assigns. These tend to
    // be the signals further up the reset tree. This will usually resolve to
    // the root of the reset tree far up in the design hierarchy.
    if (numDrives < bestNumDrives) {
      bestNumDrives = numDrives;
      bestSignal = signal;
    }
  }
  return bestSignal;
}

//===----------------------------------------------------------------------===//
// Reset Tracing
//===----------------------------------------------------------------------===//

/// Iterate over a circuit and follow all signals with `ResetType`, aggregating
/// them into reset nets. After this function returns, the `resetMap` is
/// populated with the reset networks in the circuit, alongside information on
/// drivers and their types that contribute to the reset.
void InferResetsPass::traceResets(CircuitOp circuit) {
  LLVM_DEBUG(
      llvm::dbgs() << "\n===----- Tracing uninferred resets -----===\n\n");
  circuit.walk([&](Operation *op) {
    TypeSwitch<Operation *>(op)
        .Case<ConnectOp, PartialConnectOp>(
            [&](auto op) { traceResets(op.dest(), op.src(), op.getLoc()); })
        .Case<InstanceOp>([&](auto op) { traceResets(op); });
  });
}

/// Trace reset signals through an instance. This essentially associates the
/// instance's port values with the target module's port values.
void InferResetsPass::traceResets(InstanceOp inst) {
  // Lookup the referenced module. Nothing to do if its an extmodule.
  auto module = dyn_cast<FModuleOp>(inst.getReferencedModule());
  if (!module)
    return;
  LLVM_DEBUG(llvm::dbgs() << "Visiting instance " << inst.name() << "\n");

  // Establish a connection between the instance ports and module ports.
  auto dirs = getModulePortDirections(module);
  for (auto it : llvm::enumerate(inst.getResults())) {
    auto dir = direction::get(dirs.getValue()[it.index()]);
    Value dstPort = module.getArgument(it.index());
    Value srcPort = it.value();
    if (dir == Direction::Output)
      std::swap(dstPort, srcPort);
    traceResets(dstPort, srcPort, it.value().getLoc());
  }
}

/// Analyze a connect or partial connect of one (possibly aggregate) value to
/// another. Each drive involving a `ResetType` is recorded.
void InferResetsPass::traceResets(Value dst, Value src, Location loc) {
  // Trace through any subfield/subindex/subaccess ops on both sides of the
  // connect.
  traceResets(dst);
  traceResets(src);

  // Analyze the actual connection.
  auto dstType = dst.getType().cast<FIRRTLType>();
  auto srcType = src.getType().cast<FIRRTLType>();
  traceResets(dstType, dst, 0, srcType, src, 0, loc);
}

/// Trace a value through a possible subfield/subindex/subaccess op. This is
/// used when analyzing connects and partial connects, to ensure we actually
/// track down which subfields of larger aggregate values these drives refer to.
void InferResetsPass::traceResets(Value value) {
  auto op = value.getDefiningOp();
  if (!op)
    return;
  TypeSwitch<Operation *>(op)
      .Case<SubfieldOp>([&](auto op) {
        auto bundleType = op.input().getType().template cast<BundleType>();
        auto index = op.fieldIndex();
        traceResets(op.getType(), op.getResult(), 0,
                    bundleType.getElements()[index].type, op.input(),
                    bundleType.getFieldID(index), value.getLoc());
      })
      .Case<SubindexOp, SubaccessOp>([&](auto op) {
        // Collapse all elements in vectors into one shared element which will
        // ensure that reset inference provides a uniform result for all
        // elements.
        //
        // CAVEAT: This may infer reset networks that are too big, since
        // unrelated resets in the same vector end up looking as if they were
        // connected. However for the sake of type inference, this is
        // indistinguishable from them having to share the same type (namely the
        // vector element type).
        auto vectorType = op.input().getType().template cast<FVectorType>();
        traceResets(op.getType(), op.getResult(), 0,
                    vectorType.getElementType(), op.input(),
                    vectorType.getFieldID(0), value.getLoc());
      });
}

/// Analyze a connect or partial connect of one (possibly aggregate) value to
/// another. Each drive involving a `ResetType` is recorded.
void InferResetsPass::traceResets(FIRRTLType dstType, Value dst, unsigned dstID,
                                  FIRRTLType srcType, Value src, unsigned srcID,
                                  Location loc) {
  if (auto dstBundle = dstType.dyn_cast<BundleType>()) {
    auto srcBundle = srcType.cast<BundleType>();
    for (unsigned dstIdx = 0, e = dstBundle.getNumElements(); dstIdx < e;
         ++dstIdx) {
      auto dstField = dstBundle.getElements()[dstIdx].name.getValue();
      auto srcIdx = srcBundle.getElementIndex(dstField);
      if (!srcIdx)
        continue;
      auto &dstElt = dstBundle.getElements()[dstIdx];
      auto &srcElt = srcBundle.getElements()[*srcIdx];
      if (dstElt.isFlip) {
        traceResets(srcElt.type, src, srcID + srcBundle.getFieldID(*srcIdx),
                    dstElt.type, dst, dstID + dstBundle.getFieldID(dstIdx),
                    loc);
      } else {
        traceResets(dstElt.type, dst, dstID + dstBundle.getFieldID(dstIdx),
                    srcElt.type, src, srcID + srcBundle.getFieldID(*srcIdx),
                    loc);
      }
    }
  } else if (auto dstVector = dstType.dyn_cast<FVectorType>()) {
    auto srcVector = srcType.cast<FVectorType>();
    auto srcElType = srcVector.getElementType();
    auto dstElType = dstVector.getElementType();
    // Collapse all elements into one shared element. See comment in traceResets
    // above for some context.
    traceResets(dstElType, dst, dstID + dstVector.getFieldID(0), srcElType, src,
                srcID + srcVector.getFieldID(0), loc);
  } else if (dstType.isGround()) {
    if (dstType.isa<ResetType>() || srcType.isa<ResetType>()) {
      FieldRef dstField(dst, dstID);
      FieldRef srcField(src, srcID);
      LLVM_DEBUG(llvm::dbgs() << "Visiting driver '" << getFieldName(dstField)
                              << "' = '" << getFieldName(srcField) << "' ("
                              << dstType << " = " << srcType << ")\n");

      // Determine the leaders for the dst and src reset networks before we make
      // the connection. This will allow us to later detect if dst got merged
      // into src, or src into dst.
      ResetSignal dstLeader =
          *resetClasses.findLeader(resetClasses.insert({dstField, dstType}));
      ResetSignal srcLeader =
          *resetClasses.findLeader(resetClasses.insert({srcField, srcType}));

      // Unify the two reset networks.
      ResetSignal unionLeader = *resetClasses.unionSets(dstLeader, srcLeader);
      assert(unionLeader == dstLeader || unionLeader == srcLeader);

      // If dst got merged into src, append dst's drives to src's, or vice
      // versa.
      auto &unionDrives = resetDrives[unionLeader];
      if (dstLeader != srcLeader) {
        if (unionLeader == dstLeader)
          unionDrives.append(std::move(resetDrives[srcLeader]));
        else
          unionDrives.append(std::move(resetDrives[dstLeader]));
      }

      // Keep note of this drive so we can point the user at the right location
      // in case something goes wrong.
      unionDrives.push_back({{dstField, dstType}, {srcField, srcType}, loc});
    }
  } else {
    llvm_unreachable("unknown type");
  }
}

//===----------------------------------------------------------------------===//
// Reset Inference
//===----------------------------------------------------------------------===//

LogicalResult InferResetsPass::inferAndUpdateResets() {
  LLVM_DEBUG(llvm::dbgs() << "\n===----- Infer reset types -----===\n\n");
  for (auto it = resetClasses.begin(), end = resetClasses.end(); it != end;
       ++it) {
    if (!it->isLeader())
      continue;
    ResetNetwork net = llvm::make_range(resetClasses.member_begin(it),
                                        resetClasses.member_end());

    // Infer whether this should be a sync or async reset.
    auto kind = inferReset(net);
    if (failed(kind))
      return failure();

    // Update the types in the IR to match the inferred kind.
    if (failed(updateReset(net, *kind)))
      return failure();
  }
  return success();
}

FailureOr<ResetKind> InferResetsPass::inferReset(ResetNetwork net) {
  LLVM_DEBUG(llvm::dbgs() << "Inferring reset network with "
                          << std::distance(net.begin(), net.end())
                          << " nodes\n");

  // Go through the nodes and track the involved types.
  unsigned asyncDrives = 0;
  unsigned syncDrives = 0;
  unsigned invalidDrives = 0;
  for (ResetSignal signal : net) {
    // Keep track of whether this signal contributes a vote for async or sync.
    if (signal.type.isa<AsyncResetType>())
      ++asyncDrives;
    else if (signal.type.isa<UIntType>())
      ++syncDrives;
    else if (isa_and_nonnull<InvalidValueOp>(
                 signal.field.getValue().getDefiningOp()))
      ++invalidDrives;
  }
  LLVM_DEBUG(llvm::dbgs() << "- Found " << asyncDrives << " async, "
                          << syncDrives << " sync, " << invalidDrives
                          << " invalid drives\n");

  // Handle the case where we have no votes for either kind.
  if (asyncDrives == 0 && syncDrives == 0 && invalidDrives == 0) {
    ResetSignal root = guessRoot(net);
    mlir::emitError(root.field.getValue().getLoc())
        << "reset network never driven with concrete type";
    return failure();
  }

  // Handle the case where we have votes for both kinds.
  if (asyncDrives > 0 && syncDrives > 0) {
    ResetSignal root = guessRoot(net);
    bool majorityAsync = asyncDrives >= syncDrives;
    auto diag =
        mlir::emitError(root.field.getValue().getLoc())
        << "reset network simultaneously connected to async and sync resets";
    diag.attachNote(root.field.getValue().getLoc())
        << "did you intend for the reset to be "
        << (majorityAsync ? "async?" : "sync?");
    for (auto &drive : getResetDrives(net)) {
      if ((drive.dst.type.isa<AsyncResetType>() && !majorityAsync) ||
          (drive.src.type.isa<AsyncResetType>() && !majorityAsync) ||
          (drive.dst.type.isa<UIntType>() && majorityAsync) ||
          (drive.src.type.isa<UIntType>() && majorityAsync))
        diag.attachNote(drive.loc)
            << "offending " << (majorityAsync ? "sync" : "async")
            << " drive here:";
    }
    return failure();
  }

  // At this point we know that the type of the reset is unambiguous. If there
  // are any votes for async, we make the reset async. Otherwise we make it
  // sync.
  auto kind = (asyncDrives ? ResetKind::Async : ResetKind::Sync);
  LLVM_DEBUG(llvm::dbgs() << "- Inferred as " << kind << "\n");
  return kind;
}

//===----------------------------------------------------------------------===//
// Reset Updating
//===----------------------------------------------------------------------===//

LogicalResult InferResetsPass::updateReset(ResetNetwork net, ResetKind kind) {
  LLVM_DEBUG(llvm::dbgs() << "Updating reset network with "
                          << std::distance(net.begin(), net.end())
                          << " nodes to " << kind << "\n");

  // Determine the final type the reset should have.
  FIRRTLType resetType;
  if (kind == ResetKind::Async)
    resetType = AsyncResetType::get(&getContext());
  else
    resetType = UIntType::get(&getContext(), 1);

  // Update all those values in the network that cannot be inferred from
  // operands. If we change the type of a module port (i.e. BlockArgument), add
  // the module to a module worklist since we need to update its function type.
  SmallSetVector<Operation *, 16> worklist;
  SmallDenseSet<Operation *> moduleWorklist;
  for (auto signal : net) {
    Value value = signal.field.getValue();
    if (!value.isa<BlockArgument>() &&
        !isa_and_nonnull<WireOp, RegOp, RegResetOp, InstanceOp, InvalidValueOp>(
            value.getDefiningOp()))
      continue;
    if (updateReset(signal.field, resetType)) {
      for (auto user : value.getUsers())
        worklist.insert(user);
      if (auto blockArg = value.dyn_cast<BlockArgument>())
        moduleWorklist.insert(blockArg.getOwner()->getParentOp());
    }
  }

  // Work dat list.
  while (!worklist.empty()) {
    auto op = dyn_cast_or_null<InferTypeOpInterface>(worklist.pop_back_val());
    if (!op)
      continue;

    // Determine the new result types.
    SmallVector<Type, 2> types;
    if (failed(op.inferReturnTypes(op->getContext(), op->getLoc(),
                                   op->getOperands(), op->getAttrDictionary(),
                                   op->getRegions(), types)))
      return failure();
    assert(types.size() == op->getNumResults());

    // Update the results and add the changed ones to the worklist.
    for (auto it : llvm::zip(op->getResults(), types)) {
      auto newType = std::get<1>(it);
      if (std::get<0>(it).getType() == newType)
        continue;
      std::get<0>(it).setType(newType);
      for (auto user : std::get<0>(it).getUsers())
        worklist.insert(user);
    }

    LLVM_DEBUG(llvm::dbgs() << "- Inferred " << *op << "\n");
  }

  // Update module types based on the type of the block arguments.
  for (auto *op : moduleWorklist) {
    auto module = dyn_cast<FModuleOp>(op);
    if (!module)
      continue;

    SmallVector<Type> argTypes;
    argTypes.reserve(module.getArguments().size());
    for (auto arg : module.getArguments())
      argTypes.push_back(arg.getType());

    auto type =
        FunctionType::get(op->getContext(), argTypes, /*resultTypes*/ {});
    module->setAttr(FModuleOp::getTypeAttrName(), TypeAttr::get(type));
    LLVM_DEBUG(llvm::dbgs()
               << "- Updated type of module '" << module.getName() << "'\n");
  }

  return success();
}

/// Update the type of a single field within a type.
static FIRRTLType updateType(FIRRTLType oldType, unsigned fieldID,
                             FIRRTLType fieldType) {
  // If this is a ground type, simply replace it.
  if (oldType.isGround()) {
    assert(fieldID == 0);
    return fieldType;
  }

  // If this is a bundle type, update the corresponding field.
  if (auto bundleType = oldType.dyn_cast<BundleType>()) {
    unsigned index = bundleType.getIndexForFieldID(fieldID);
    SmallVector<BundleType::BundleElement> fields(
        bundleType.getElements().begin(), bundleType.getElements().end());
    fields[index].type = updateType(
        fields[index].type, fieldID - bundleType.getFieldID(index), fieldType);
    return BundleType::get(fields, oldType.getContext());
  }

  // If this is a vector type, update the element type.
  if (auto vectorType = oldType.dyn_cast<FVectorType>()) {
    unsigned index = vectorType.getIndexForFieldID(fieldID);
    auto newType =
        updateType(vectorType.getElementType(),
                   fieldID - vectorType.getFieldID(index), fieldType);
    return FVectorType::get(newType, vectorType.getNumElements());
  }

  llvm_unreachable("unknown aggregate type");
  return oldType;
}

/// Update the reset type of a specific field.
bool InferResetsPass::updateReset(FieldRef field, FIRRTLType resetType) {
  // Compute the updated type.
  auto oldType = field.getValue().getType().cast<FIRRTLType>();
  auto newType = updateType(oldType, field.getFieldID(), resetType);

  // Update the type if necessary.
  if (oldType == newType)
    return false;
  LLVM_DEBUG(llvm::dbgs() << "- Updating '" << getFieldName(field) << "' from "
                          << oldType << " to " << newType << "\n");
  field.getValue().setType(newType);
  return true;
}

//===----------------------------------------------------------------------===//
// Reset Annotations
//===----------------------------------------------------------------------===//

/// Annotation that marks a reset (port or wire) and domain.
static constexpr const char *resetAnno =
    "sifive.enterprise.firrtl.FullAsyncResetAnnotation";

/// Annotation that marks a module as not belonging to any reset domain.
static constexpr const char *ignoreAnno =
    "sifive.enterprise.firrtl.IgnoreFullAsyncResetAnnotation";

LogicalResult InferResetsPass::collectAnnos(CircuitOp circuit) {
  LLVM_DEBUG(
      llvm::dbgs() << "\n===----- Gather async reset annotations -----===\n\n");
  circuit.walk<WalkOrder::PreOrder>([&](FModuleOp module) {
    if (failed(collectAnnos(module)))
      return WalkResult::interrupt();
    return WalkResult::skip();
  });
  return success();
}

LogicalResult InferResetsPass::collectAnnos(FModuleOp module) {
  bool anyFailed = false;
  SmallSetVector<std::pair<Annotation, Location>, 4> conflictingAnnos;

  // Consume a possible "ignore" annotation on the module itself, which
  // explicitly assigns it no reset domain.
  bool ignore = false;
  AnnotationSet moduleAnnos(module);
  if (!moduleAnnos.empty()) {
    moduleAnnos.removeAnnotations([&](Annotation anno) {
      if (anno.isClass(ignoreAnno)) {
        ignore = true;
        conflictingAnnos.insert({anno, module.getLoc()});
        return true;
      }
      if (anno.isClass(resetAnno)) {
        anyFailed = true;
        module.emitError("'FullAsyncResetAnnotation' cannot target module; "
                         "must target port or wire/node instead");
        return true;
      }
      return false;
    });
    moduleAnnos.applyToOperation(module);
  }
  if (anyFailed)
    return failure();

  // Consume any reset annotations on module ports.
  Value reset;
  AnnotationSet::removePortAnnotations(module, [&](unsigned argNum,
                                                   Annotation anno) {
    Value arg = module.getArgument(argNum);
    if (anno.isClass(resetAnno)) {
      reset = arg;
      conflictingAnnos.insert({anno, reset.getLoc()});
      return true;
    }
    if (anno.isClass(ignoreAnno)) {
      anyFailed = true;
      mlir::emitError(arg.getLoc(),
                      "'IgnoreFullAsyncResetAnnotation' cannot target port; "
                      "must target module instead");
      return true;
    }
    return false;
  });
  if (anyFailed)
    return failure();

  // Consume any reset annotations on wires in the module body.
  module.walk([&](Operation *op) {
    AnnotationSet::removeAnnotations(op, [&](Annotation anno) {
      // Reset annotations must target wire/node ops.
      if (!isa<WireOp, NodeOp>(op)) {
        if (anno.isClass(resetAnno, ignoreAnno)) {
          anyFailed = true;
          op->emitError(
              "reset annotations must target module, port, or wire/node");
          return true;
        }
        return false;
      }

      // At this point we know that we have a WireOp/NodeOp. Process the reset
      // annotations.
      if (anno.isClass(resetAnno)) {
        reset = op->getResult(0);
        conflictingAnnos.insert({anno, reset.getLoc()});
        return true;
      }
      if (anno.isClass(ignoreAnno)) {
        anyFailed = true;
        op->emitError(
            "'IgnoreFullAsyncResetAnnotation' cannot target wire/node; must "
            "target module instead");
        return true;
      }
      return false;
    });
  });
  if (anyFailed)
    return failure();

  // If we have found no annotations, there is nothing to do. We just leave this
  // module unannotated, which will cause it to inherit a reset domain from its
  // instantiation sites.
  if (!ignore && !reset) {
    LLVM_DEBUG(llvm::dbgs()
               << "No reset annotation for " << module.getName() << "\n");
    return success();
  }

  // If we have found multiple annotations, emit an error and abort.
  if (conflictingAnnos.size() > 1) {
    auto diag = module.emitError("multiple reset annotations on module '")
                << module.getName() << "'";
    for (auto &annoAndLoc : conflictingAnnos)
      diag.attachNote(annoAndLoc.second)
          << "conflicting " << annoAndLoc.first.getClassAttr() << ":";
    return failure();
  }

  // Dump some information in debug builds.
  LLVM_DEBUG({
    llvm::dbgs() << "Annotated reset for " << module.getName() << ": ";
    if (ignore)
      llvm::dbgs() << "no domain\n";
    else if (auto arg = reset.dyn_cast<BlockArgument>())
      llvm::dbgs() << "port " << getModulePortName(module, arg.getArgNumber())
                   << "\n";
    else
      llvm::dbgs() << "wire "
                   << reset.getDefiningOp()->getAttrOfType<StringAttr>("name")
                   << "\n";
  });

  // Store the annotated reset for this module.
  assert(ignore || reset);
  annotatedResets.insert({module, reset});
  return success();
}

//===----------------------------------------------------------------------===//
// Domain Construction
//===----------------------------------------------------------------------===//

/// Gather the reset domains present in a circuit. This traverses the instance
/// hierarchy of the design, making instances either live in a new reset domain
/// if so annotated, or inherit their parent's domain. This can go wrong in some
/// cases, mainly when a module is instantiated multiple times within different
/// reset domains.
LogicalResult InferResetsPass::buildDomains(CircuitOp circuit) {
  LLVM_DEBUG(
      llvm::dbgs() << "\n===----- Build async reset domains -----===\n\n");

  // Gather the domains.
  auto instGraph = getAnalysis<InstanceGraph>();
  auto module = dyn_cast<FModuleOp>(instGraph.getTopLevelNode()->getModule());
  if (!module) {
    LLVM_DEBUG(llvm::dbgs()
               << "Skipping circuit because main module is no `firrtl.module`");
    return success();
  }
  buildDomains(module, InstancePath{}, Value{}, instGraph);

  // Report any domain conflicts among the modules.
  bool anyFailed = false;
  for (auto &it : domains) {
    auto module = cast<FModuleOp>(it.first);
    auto &domainConflicts = it.second;
    if (domainConflicts.size() <= 1)
      continue;

    anyFailed = true;
    SmallDenseSet<Value> printedDomainResets;
    auto diag = module.emitError("module '")
                << module.getName()
                << "' instantiated in different reset domains";
    for (auto &it : domainConflicts) {
      ResetDomain &domain = it.first;
      InstancePathRef path = it.second;
      auto inst = path.back();
      auto loc = path.empty() ? module.getLoc() : inst.getLoc();
      auto &note = diag.attachNote(loc);

      // Describe the instance itself.
      if (path.empty())
        note << "root instance";
      else {
        note << "instance '";
        llvm::interleave(
            path, [&](InstanceOp inst) { note << inst.name(); },
            [&]() { note << "/"; });
        note << "'";
      }

      // Describe the reset domain the instance is in.
      note << " is in";
      if (domain.reset) {
        auto nameAndModule = getResetNameAndModule(domain.reset);
        note << " reset domain rooted at '" << nameAndModule.first.getValue()
             << "' of module '" << nameAndModule.second.getName() << "'";

        // Show where the domain reset is declared (once per reset).
        if (printedDomainResets.insert(domain.reset).second) {
          diag.attachNote(domain.reset.getLoc())
              << "reset domain '" << nameAndModule.first.getValue()
              << "' of module '" << nameAndModule.second.getName()
              << "' declared here:";
        }
      } else
        note << " no reset domain";
    }
  }
  return failure(anyFailed);
}

void InferResetsPass::buildDomains(FModuleOp module,
                                   const InstancePath &instPath,
                                   Value parentReset, InstanceGraph &instGraph,
                                   unsigned indent) {
  LLVM_DEBUG(llvm::dbgs().indent(indent * 2)
             << "Visiting " << getTail(instPath) << " (" << module.getName()
             << ")\n");

  // Assemble the domain for this module.
  ResetDomain domain(parentReset);
  auto it = annotatedResets.find(module);
  if (it != annotatedResets.end()) {
    domain.isTop = true;
    domain.reset = it->second;
  }

  // Associate the domain with this module. If the module already has an
  // associated domain, it must be identical. Otherwise we'll have to report the
  // conflicting domains to the user.
  auto &entries = domains[module];
  if (llvm::all_of(entries,
                   [&](const auto &entry) { return entry.first != domain; }))
    entries.push_back({domain, instPath});

  // Traverse the child instances.
  InstancePath childPath = instPath;
  for (auto record : instGraph[module]->instances()) {
    auto submodule = dyn_cast<FModuleOp>(record->getTarget()->getModule());
    if (!submodule)
      continue;
    childPath.push_back(record->getInstance());
    buildDomains(submodule, childPath, domain.reset, instGraph, indent + 1);
    childPath.pop_back();
  }
}

/// Determine how the reset for each module shall be implemented.
void InferResetsPass::determineImpl() {
  LLVM_DEBUG(
      llvm::dbgs() << "\n===----- Determine implementation -----===\n\n");
  for (auto &it : domains) {
    auto module = cast<FModuleOp>(it.first);
    auto &domain = it.second.back().first;
    determineImpl(module, domain);
  }
}

/// Determine how the reset for a module shall be implemented. This function
/// fills in the `existingValue`, `existingPort`, and `newPortName` fields of
/// the given reset domain.
///
/// Generally it does the following:
/// - If the domain has explicitly no reset ("ignore"), leaves everything empty.
/// - If the domain is the place where the reset is defined ("top"), fills in
///   the existing port/wire/node as reset.
/// - If the module already has a port with the reset's name:
///   - If the type is `asyncreset`, reuses that port.
///   - Otherwise appends a `_N` suffix with increasing N to create a yet-unused
///     port name, and marks that as to be created.
/// - Otherwise indicates that a port with the reset's name should be created.
///
void InferResetsPass::determineImpl(FModuleOp module, ResetDomain &domain) {
  if (!domain.reset)
    return; // nothing to do if the module needs no reset
  LLVM_DEBUG(llvm::dbgs() << "Planning reset for " << module.getName() << "\n");

  // If this is the root of a reset domain, we don't need to add any ports
  // and can just simply reuse the existing values.
  if (domain.isTop) {
    LLVM_DEBUG(llvm::dbgs() << "- Rooting at local value "
                            << getResetName(domain.reset) << "\n");
    domain.existingValue = domain.reset;
    if (auto blockArg = domain.reset.dyn_cast<BlockArgument>())
      domain.existingPort = blockArg.getArgNumber();
    return;
  }

  // Otherwise, check if a port with this name and type already exists and
  // reuse that where possible.
  auto neededName = getResetName(domain.reset);
  auto neededType = domain.reset.getType();
  LLVM_DEBUG(llvm::dbgs() << "- Looking for existing port " << neededName
                          << "\n");
  auto portNames = getModulePortNames(module);
  auto ports = llvm::zip(portNames, module.getArguments());
  auto portIt = llvm::find_if(
      ports, [&](auto port) { return std::get<0>(port) == neededName; });
  if (portIt != ports.end() && std::get<1>(*portIt).getType() == neededType) {
    LLVM_DEBUG(llvm::dbgs()
               << "- Reusing existing port " << neededName << "\n");
    domain.existingValue = std::get<1>(*portIt);
    domain.existingPort = std::distance(ports.begin(), portIt);
    return;
  }

  // If we have found a port but the types don't match, pick a new name for
  // the reset port.
  //
  // CAVEAT: The Scala FIRRTL compiler just throws an error in this case. This
  // seems unnecessary though, since the compiler can just insert a new reset
  // signal as needed.
  if (portIt != ports.end()) {
    LLVM_DEBUG(llvm::dbgs()
               << "- Existing " << neededName << " has incompatible type "
               << std::get<1>(*portIt).getType() << "\n");
    StringAttr newName;
    unsigned suffix = 0;
    do {
      newName =
          StringAttr::get(&getContext(), Twine(neededName.getValue()) +
                                             Twine("_") + Twine(suffix++));
    } while (llvm::is_contained(portNames, newName));
    LLVM_DEBUG(llvm::dbgs()
               << "- Creating uniquified port " << newName << "\n");
    domain.newPortName = newName;
    return;
  }

  // At this point we know that there is no such port, and we can safely
  // create one as needed.
  LLVM_DEBUG(llvm::dbgs() << "- Creating new port " << neededName << "\n");
  domain.newPortName = neededName;
}

//===----------------------------------------------------------------------===//
// Async Reset Implementation
//===----------------------------------------------------------------------===//

/// Implement the async resets gathered in the pass' `domains` map.
LogicalResult InferResetsPass::implementAsyncReset() {
  LLVM_DEBUG(llvm::dbgs() << "\n===----- Implement async resets -----===\n\n");
  for (auto &it : domains)
    if (failed(implementAsyncReset(cast<FModuleOp>(it.first),
                                   it.second.back().first)))
      return failure();
  return success();
}

/// Implement the async resets for a specific module.
///
/// This will add ports to the module as appropriate, update the register ops in
/// the module, and update any instantiated submodules with their corresponding
/// reset implementation details.
LogicalResult InferResetsPass::implementAsyncReset(FModuleOp module,
                                                   ResetDomain &domain) {
  LLVM_DEBUG(llvm::dbgs() << "Implementing async reset for " << module.getName()
                          << "\n");

  // Nothing to do if the module was marked explicitly with no reset domain.
  if (!domain.reset) {
    LLVM_DEBUG(llvm::dbgs()
               << "- Skipping because module explicitly has no domain\n");
    return success();
  }

  // If needed, add a reset port to the module.
  Value actualReset = domain.existingValue;
  if (domain.newPortName) {
    ModulePortInfo portInfo{.name = domain.newPortName,
                            .type = AsyncResetType::get(&getContext()),
                            .direction = Direction::Input,
                            .loc = domain.reset.getLoc()};
    module.insertPorts({{0, portInfo}});
    actualReset = module.getArgument(0);
    LLVM_DEBUG(llvm::dbgs()
               << "- Inserted port " << domain.newPortName << "\n");
  }
  assert(actualReset);
  LLVM_DEBUG({
    llvm::dbgs() << "- Using ";
    if (auto blockArg = actualReset.dyn_cast<BlockArgument>())
      llvm::dbgs() << "port #" << blockArg.getArgNumber() << " ";
    else
      llvm::dbgs() << "wire/node ";
    llvm::dbgs() << getResetName(actualReset) << "\n";
  });

  // Update the operations in the module.
  SmallVector<Operation *> deleteOps;
  SmallVector<std::pair<Value, Value>> connects;
  module.walk([&](Operation *op) {
    implementAsyncReset(op, module, actualReset, deleteOps, connects);
  });

  // Remove the obsolete instances.
  for (auto op : deleteOps)
    op->erase();

  // Add the necessary connects.
  OpBuilder builder(module);
  builder.setInsertionPointToEnd(module.getBodyBlock());
  for (auto con : connects)
    builder.create<ConnectOp>(con.first.getLoc(), con.first, con.second);

  return success();
}

/// Modify an operation in a module to implement an async reset for that module.
void InferResetsPass::implementAsyncReset(
    Operation *op, FModuleOp module, Value actualReset,
    SmallVectorImpl<Operation *> &deleteOps,
    SmallVectorImpl<std::pair<Value, Value>> &connects) {
  ImplicitLocOpBuilder builder(op->getLoc(), op);

  // Handle instances.
  if (auto instOp = dyn_cast<InstanceOp>(op)) {
    // Lookup the reset domain of the instantiated module. If there is no reset
    // domain associated with that module, or the module is explicitly marked as
    // being in no domain, simply skip.
    auto refModule = dyn_cast<FModuleOp>(instOp.getReferencedModule());
    if (!refModule)
      return;
    auto domainIt = domains.find(refModule);
    if (domainIt == domains.end())
      return;
    auto &domain = domainIt->second.back().first;
    if (!domain.reset)
      return;
    LLVM_DEBUG(llvm::dbgs() << "- Update instance '" << instOp.name() << "'\n");

    // If needed, add a reset port to the instance.
    Value instReset;
    if (domain.newPortName) {
      LLVM_DEBUG(llvm::dbgs() << "  - Adding new result as reset\n");

      // Determine the new result types.
      SmallVector<Type> resultTypes;
      resultTypes.reserve(instOp.getNumResults() + 1);
      resultTypes.push_back(actualReset.getType());
      resultTypes.append(instOp.getResultTypes().begin(),
                         instOp.getResultTypes().end());

      // Create a new list of port annotations.
      SmallVector<Attribute> newPortAnnos;
      if (auto oldPortAnnos = instOp.portAnnotations()) {
        newPortAnnos.reserve(oldPortAnnos.size() + 1);
        newPortAnnos.push_back(builder.getArrayAttr({}));
        newPortAnnos.append(oldPortAnnos.begin(), oldPortAnnos.end());
      }
      LLVM_DEBUG(llvm::dbgs()
                 << "  - Result types: " << resultTypes.size() << "\n");
      LLVM_DEBUG(llvm::dbgs()
                 << "  - Port annos: " << newPortAnnos.size() << "\n");

      // Create a new instance op with the reset inserted.
      auto newInstOp = builder.create<InstanceOp>(
          resultTypes, instOp.moduleName(), instOp.name(),
          instOp.annotations().getValue(), newPortAnnos);
      instReset = newInstOp.getResult(0);

      // Update the uses over to the new instance and drop the old instance.
      instOp.replaceAllUsesWith(newInstOp.getResults().drop_front());
      deleteOps.push_back(instOp);
    } else if (domain.existingPort.hasValue()) {
      auto idx = domain.existingPort.getValue();
      instReset = instOp.getResult(idx);
      LLVM_DEBUG(llvm::dbgs() << "  - Using result #" << idx << " as reset\n");
    }

    // If there's no reset port on the instance to connect, we're done. This can
    // happen if the instantiated module has a reset domain, but that domain is
    // e.g. rooted at an internal wire.
    if (!instReset)
      return;

    // Connect the instance's reset to the actual reset.
    assert(instReset && actualReset);
    connects.push_back(std::make_pair(instReset, actualReset));
    return;
  }

  // Handle reset-less registers.
  if (auto regOp = dyn_cast<RegOp>(op)) {
    LLVM_DEBUG(llvm::dbgs() << "- Adding async reset to " << regOp << "\n");
    auto zero = createZeroValue(builder, regOp.getType());
    auto newRegOp = builder.create<RegResetOp>(
        regOp.getType(), regOp.clockVal(), actualReset, zero, regOp.nameAttr(),
        regOp.annotations());
    regOp.getResult().replaceAllUsesWith(newRegOp);
    deleteOps.push_back(regOp);
    return;
  }

  // Handle registers with reset.
  if (auto regOp = dyn_cast<RegResetOp>(op)) {
    // If the register already has an async reset, leave it untouched.
    if (regOp.resetSignal().getType().isa<AsyncResetType>()) {
      LLVM_DEBUG(llvm::dbgs()
                 << "- Skipping (has async reset) " << regOp << "\n");
      // The following performs the logic of `CheckResets` in the original Scala
      // source code.
      if (failed(regOp.verify()))
        signalPassFailure();
      return;
    }
    LLVM_DEBUG(llvm::dbgs() << "- Updating reset of " << regOp << "\n");

    // If we arrive here, the register has a sync reset. In order to add an
    // async reset, we have to move the sync reset into a mux in front of the
    // register.
    insertResetMux(builder, regOp, regOp.resetSignal(), regOp.resetValue());
    builder.setInsertionPoint(regOp);

    // Replace the existing reset with the async reset.
    auto zero = createZeroValue(builder, regOp.getType());
    regOp.resetSignalMutable().assign(actualReset);
    regOp.resetValueMutable().assign(zero);
  }
}
