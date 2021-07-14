//===- GrandCentralTaps.cpp -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the GrandCentralTaps pass.
//
//===----------------------------------------------------------------------===//

#include "PassDetails.h"
#include "circt/Dialect/FIRRTL/FIRParser.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "circt/Dialect/FIRRTL/FIRRTLTypes.h"
#include "circt/Dialect/FIRRTL/FIRRTLVisitors.h"
#include "circt/Dialect/FIRRTL/InstanceGraph.h"
#include "circt/Dialect/FIRRTL/Passes.h"
#include "circt/Dialect/SV/SVOps.h"
#include "mlir/IR/ImplicitLocOpBuilder.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FormatVariadic.h"

#define DEBUG_TYPE "gct"

using namespace circt;
using namespace firrtl;
using mlir::function_like_impl::getArgAttrDict;
using mlir::function_like_impl::setAllArgAttrDicts;

//===----------------------------------------------------------------------===//
// PointerLikeTypeTraits
//===----------------------------------------------------------------------===//

namespace llvm {
template <>
struct PointerLikeTypeTraits<InstanceOp> : PointerLikeTypeTraits<Operation *> {
public:
  static inline void *getAsVoidPointer(InstanceOp val) {
    return const_cast<void *>(val.getAsOpaquePointer());
  }
  static inline InstanceOp getFromVoidPointer(void *p) {
    return InstanceOp::getFromOpaquePointer(p);
  }
};
} // end namespace llvm

//===----------------------------------------------------------------------===//
// Static class names
//===----------------------------------------------------------------------===//

static constexpr const char *dataTapsClass =
    "sifive.enterprise.grandcentral.DataTapsAnnotation";
static constexpr const char *memTapClass =
    "sifive.enterprise.grandcentral.MemTapAnnotation";
static constexpr const char *deletedKeyClass =
    "sifive.enterprise.grandcentral.DeletedDataTapKey";
static constexpr const char *literalKeyClass =
    "sifive.enterprise.grandcentral.LiteralDataTapKey";
static constexpr const char *referenceKeyClass =
    "sifive.enterprise.grandcentral.ReferenceDataTapKey";
static constexpr const char *internalKeyClass =
    "sifive.enterprise.grandcentral.DataTapModuleSignalKey";

//===----------------------------------------------------------------------===//
// Utilities
//===----------------------------------------------------------------------===//

/// An absolute instance path.
using InstancePath = ArrayRef<InstanceOp>;

template <typename T>
static T &operator<<(T &os, const InstancePath &path) {
  os << "$root";
  for (auto inst : path)
    os << "/" << inst.name() << ":" << inst.moduleName();
  return os;
}

namespace {

/// A port annotated with a data tap key or mem tap.
struct AnnotatedPort {
  unsigned portNum;
  Annotation anno;
};

/// An extmodule that has annotated ports.
struct AnnotatedExtModule {
  FExtModuleOp extModule;
  SmallVector<AnnotatedPort, 4> portAnnos;
  // Module annotations without data tap stuff.
  ArrayAttr filteredModuleAnnos;
  /// Port annotations without data tap stuff.
  SmallVector<AnnotationSet, 8> filteredPortAnnos;
};

/// A value annotated to be tapped.
struct TappedValue {
  Value value;
  Annotation anno;
};

/// A data structure that caches and provides absolute paths to module instances
/// in the IR.
struct InstancePaths {
  /// The instance graph of the IR.
  InstanceGraph &instanceGraph;

  explicit InstancePaths(InstanceGraph &instanceGraph)
      : instanceGraph(instanceGraph) {}
  ArrayRef<InstancePath> getAbsolutePaths(Operation *op);

private:
  /// An allocator for individual instance paths and entire path lists.
  llvm::BumpPtrAllocator allocator;

  /// Cached absolute instance paths.
  DenseMap<Operation *, ArrayRef<InstancePath>> absolutePathsCache;

  /// Append an instance to a path.
  InstancePath appendInstance(InstancePath path, InstanceOp inst);
};

/// Necessary information to wire up a port with tapped data or memory location.
struct PortWiring {
  unsigned portNum;
  ArrayRef<InstancePath> prefices;
  SmallString<16> suffix;
  /// If set, the port should output a constant literal.
  StringAttr literal;
};

} // namespace

ArrayRef<InstancePath> InstancePaths::getAbsolutePaths(Operation *op) {
  assert((isa<FModuleOp, FExtModuleOp>(op))); // extra parens makes parser smile

  // If we have reached the circuit root, we're done.
  if (op == instanceGraph.getTopLevelNode()->getModule()) {
    static InstancePath empty{};
    return empty; // array with single empty path
  }

  // Fast path: hit the cache.
  auto cached = absolutePathsCache.find(op);
  if (cached != absolutePathsCache.end())
    return cached->second;

  // For each instance, collect the instance paths to its parent and append the
  // instance itself to each.
  SmallVector<InstancePath, 8> extendedPaths;
  for (auto inst : instanceGraph[op]->uses()) {
    auto instPaths = getAbsolutePaths(inst->getParent()->getModule());
    extendedPaths.reserve(instPaths.size());
    for (auto path : instPaths) {
      extendedPaths.push_back(appendInstance(path, inst->getInstance()));
    }
  }

  // Move the list of paths into the bump allocator for later quick retrieval.
  ArrayRef<InstancePath> pathList;
  if (!extendedPaths.empty()) {
    auto paths = allocator.Allocate<InstancePath>(extendedPaths.size());
    std::copy(extendedPaths.begin(), extendedPaths.end(), paths);
    pathList = ArrayRef<InstancePath>(paths, extendedPaths.size());
  }

  absolutePathsCache.insert({op, pathList});
  return pathList;
}

InstancePath InstancePaths::appendInstance(InstancePath path, InstanceOp inst) {
  size_t n = path.size() + 1;
  auto newPath = allocator.Allocate<InstanceOp>(n);
  std::copy(path.begin(), path.end(), newPath);
  newPath[path.size()] = inst;
  return InstancePath(newPath, n);
}

/// Return a version of `path` that skips all front instances it has in common
/// with `other`.
static InstancePath stripCommonPrefix(InstancePath path, InstancePath other) {
  while (!path.empty() && !other.empty() && path.front() == other.front()) {
    path = path.drop_front();
    other = other.drop_front();
  }
  return path;
}

/// A reference data tap key, identified by the annotation id and port id.
using Key = std::pair<Attribute, Attribute>;

/// A tapped port, described as the module/extmodule operation and the port
/// number.
struct Port : std::pair<Operation *, unsigned> {
  using std::pair<Operation *, unsigned>::pair;
  operator bool() const { return bool(first); }
};

// Allow printing of `Key` through `<<`.
template <typename T>
static T &operator<<(T &os, Key key) {
  return os << "[" << key.first << ", " << key.second << "]";
}

/// Check if an annotation is a `ReferenceDataTapKey`, and that it has a `type`
/// field with a given content.
static bool isReferenceDataTapOfType(Annotation anno, StringRef type) {
  if (!anno.isClass(referenceKeyClass))
    return false;
  auto typeAttr = anno.getMember<StringAttr>("type");
  if (!typeAttr)
    return false;
  return typeAttr.getValue() == type;
}

/// Check if an annotation is a `ReferenceDataTapKey` with `source` type.
static bool isReferenceDataTapSource(Annotation anno) {
  return isReferenceDataTapOfType(anno, "source");
}

/// Check if an annotation is a `ReferenceDataTapKey` with `portName` type.
static bool isReferenceDataTapPortName(Annotation anno) {
  return isReferenceDataTapOfType(anno, "portName");
}

/// Map an annotation to a `Key`.
static Key getKey(Annotation anno) {
  auto id = anno.getMember("id");
  auto portID = anno.getMember("portID");
  return {id, portID};
}

//===----------------------------------------------------------------------===//
// Pass Infrastructure
//===----------------------------------------------------------------------===//

class GrandCentralTapsPass : public GrandCentralTapsBase<GrandCentralTapsPass> {
  void runOnOperation() override;
  void gatherAnnotations(Operation *op);
  void processAnnotation(AnnotatedPort &portAnno, AnnotatedExtModule &blackBox,
                         InstancePaths &instancePaths);

  // Helpers to simplify collecting taps on the various things.
  void gatherTap(Annotation anno, Port port) {
    auto it = tappedPorts.insert({getKey(anno), port});
    assert(it.second && "ambiguous tap annotation");
  }
  void gatherTap(Annotation anno, Operation *op) {
    auto it = tappedOps.insert({getKey(anno), op});
    assert(it.second && "ambiguous tap annotation");
  }

  DenseMap<Key, Operation *> tappedOps;
  DenseMap<Key, Port> tappedPorts;
  SmallDenseMap<Attribute, unsigned, 2> memPortIdx;
  SmallVector<PortWiring, 8> portWiring;
};

void GrandCentralTapsPass::runOnOperation() {
  auto circuitOp = getOperation();
  LLVM_DEBUG(llvm::dbgs() << "Running the GCT Data Taps pass\n");

  // Here's a rough idea of what the Scala code is doing:
  // - Gather the `source` of all `keys` of all `DataTapsAnnotation`s throughout
  //   the design.
  // - Convert the sources, which are specified on modules, to the absolute
  //   paths in all instances. E.g. module M tap x will produce a.x and b.x if
  //   there are two instances a and b of module M in the design.
  // - All data tap keys are specified on black box ports.
  // - The code then processes every DataTapsAnnotation separately as follows
  //   (with the targeted blackbox and keys):
  // - Check for collisions between SV keywords and key port names (skip this).
  // - Find all instances of the blackbox, but then just pick the first one (we
  //   should probably do this for each?)
  // - Process each key independently as follows:
  // - Look up the absolute path of the source in the map. Ensure it exists and
  //   is unambiguous. Make it relative to the blackbox instance path.
  // - Look up the port on the black box.
  // - Create a hierarchical SV name and store this as an assignment to the
  //   blackbox port.
  //   - DeletedDataTapKey: skip and don't create a wiring
  //   - LiteralDataTapKey: just the literal
  //   - ReferenceDataTapKey: relative path with "." + source name
  //   - DataTapModuleSignalKey: relative path with "." + internal path
  // - Generate a body for the blackbox module with the signal mapping

  memPortIdx.clear();

  // Gather a list of extmodules that have data or mem tap annotations to be
  // expanded.
  SmallVector<AnnotatedExtModule, 4> modules;
  for (auto &op : *circuitOp.getBody()) {
    auto extModule = dyn_cast<FExtModuleOp>(&op);
    if (!extModule)
      continue;

    // Go through the module ports and collect the annotated ones.
    AnnotatedExtModule result{extModule, {}, {}, {}};
    result.filteredPortAnnos.reserve(extModule.getNumArguments());
    for (unsigned argNum = 0, e = extModule.getNumArguments(); argNum < e;
         ++argNum) {
      // Go through all annotations on this port and add the data tap key and
      // mem tap ones to the list.
      auto annos = AnnotationSet::forPort(extModule, argNum);
      annos.removeAnnotations([&](Annotation anno) {
        if (anno.isClass(memTapClass, deletedKeyClass, literalKeyClass,
                         internalKeyClass) ||
            isReferenceDataTapPortName(anno)) {
          result.portAnnos.push_back({argNum, anno});
          return true;
        }
        return false;
      });
      result.filteredPortAnnos.push_back(annos);
    }

    // If there are data tap annotations on the module, which is likely the
    // case, create a filtered array of annotations with them removed.
    AnnotationSet annos(extModule.getOperation());
    annos.removeAnnotations(
        [&](Annotation anno) { return anno.isClass(dataTapsClass); });
    result.filteredModuleAnnos = annos.getArrayAttr();

    if (!result.portAnnos.empty())
      modules.push_back(std::move(result));
  }

  LLVM_DEBUG({
    for (auto m : modules) {
      llvm::dbgs() << "Extmodule " << m.extModule.getName() << " has "
                   << m.portAnnos.size() << " port annotations\n";
    }
  });

  // Fast path if there's nothing to do.
  if (modules.empty()) {
    markAllAnalysesPreserved();
    return;
  }

  // Build a generator for absolute module and instance paths in the design.
  InstancePaths instancePaths(getAnalysis<InstanceGraph>());

  // Gather the annotated ports and operations throughout the design that we are
  // supposed to tap in one way or another.
  tappedPorts.clear();
  tappedOps.clear();
  circuitOp.walk([&](Operation *op) { gatherAnnotations(op); });

  LLVM_DEBUG({
    llvm::dbgs() << "Tapped ports:\n";
    for (auto it : tappedPorts)
      llvm::dbgs() << "- " << it.first << ": "
                   << it.second.first->getAttr("sym_name") << " port #"
                   << it.second.second << "\n";
    llvm::dbgs() << "Tapped ops:\n";
    for (auto it : tappedOps)
      llvm::dbgs() << "- " << it.first << ": " << *it.second << "\n";
  });

  // Process each black box independently.
  for (auto blackBox : modules) {
    LLVM_DEBUG(llvm::dbgs() << "Generating impls for "
                            << blackBox.extModule.getName() << "\n");

    // As a first step, gather a list of all absolute paths to instances of
    // this black box.
    auto paths = instancePaths.getAbsolutePaths(blackBox.extModule);
    LLVM_DEBUG({
      for (auto path : paths)
        llvm::dbgs() << "- " << path << "\n";
    });

    // Go through the port annotations of the tap module and generate a
    // hierarchical path for each.
    portWiring.clear();
    portWiring.reserve(blackBox.portAnnos.size());
    for (auto portAnno : blackBox.portAnnos) {
      processAnnotation(portAnno, blackBox, instancePaths);
    }

    LLVM_DEBUG({
      llvm::dbgs() << "- Wire up as follows:\n";
      for (auto wiring : portWiring) {
        llvm::dbgs() << "- Port " << wiring.portNum << ":\n";
        for (auto path : wiring.prefices) {
          llvm::dbgs() << "  - " << path << "." << wiring.suffix << "\n";
        }
      }
    });

    // Now we have an awkward mapping problem. We have multiple data tap
    // module instances, which reference things in modules that in turn have
    // multiple instances. This is a side-effect of how Grand Central
    // annotates things on modules rather than instances. (However in practice
    // these will have a one-to-one correspondence due to CHIRRTL having fully
    // uniquified instances.) To solve this issue, create a dedicated
    // implementation for every data tap instance, and among the possible
    // targets for the data taps choose the one with the shortest relative
    // path to the data tap instance.
    ImplicitLocOpBuilder builder(blackBox.extModule->getLoc(),
                                 blackBox.extModule);
    unsigned implIdx = 0;
    for (auto path : paths) {
      builder.setInsertionPointAfter(blackBox.extModule);

      // Get the list of ports from the original extmodule, and update the
      // annotations such that they no longer contain any data/mem taps.
      auto ports = blackBox.extModule.getPorts();
      for (auto port : llvm::zip(ports, blackBox.filteredPortAnnos)) {
        std::get<0>(port).annotations = std::get<1>(port);
      }

      // Create a new firrtl.module that implements the data tap.
      auto name =
          StringAttr::get(&getContext(), Twine(blackBox.extModule.getName()) +
                                             "_impl_" + Twine(implIdx++));
      LLVM_DEBUG(llvm::dbgs()
                 << "Implementing " << name << " ("
                 << blackBox.extModule.getName() << " for " << path << ")\n");
      auto impl =
          builder.create<FModuleOp>(name, ports, blackBox.filteredModuleAnnos);
      builder.setInsertionPointToEnd(impl.getBodyBlock());

      // Connect the output ports to the appropriate tapped object.
      for (auto port : portWiring) {
        LLVM_DEBUG(llvm::dbgs() << "- Wiring up port " << port.portNum << "\n");

        // Handle literals. We send the literal string off to the FIRParser to
        // translate into whatever ops are necessary. This yields a handle on
        // value we're supposed to drive.
        if (port.literal) {
          auto parsed = circt::firrtl::parseIntegerLiteralExp(
              builder.getInsertionBlock(), port.literal.getValue(),
              blackBox.extModule.getLoc());

          if (failed(parsed)) {
            blackBox.extModule.emitError("invalid literal \"")
                << port.literal.getValue() << "\" in LiteralDataTapKey on port "
                << getModulePortName(blackBox.extModule, port.portNum);
            signalPassFailure();
            continue;
          }

          LLVM_DEBUG(llvm::dbgs()
                     << "  - Connecting literal " << parsed << "\n");
          auto arg = impl.getArgument(port.portNum);
          builder.create<ConnectOp>(arg, *parsed);
          continue;
        }

        // Determine the shortest hierarchical prefix from this black box
        // instance to the tapped object.
        Optional<InstancePath> shortestPrefix;
        for (auto prefix : port.prefices) {
          auto relative = stripCommonPrefix(prefix, path);
          if (!shortestPrefix.hasValue() ||
              relative.size() < shortestPrefix->size())
            shortestPrefix = relative;
        }
        if (!shortestPrefix.hasValue()) {
          LLVM_DEBUG(llvm::dbgs() << "  - Has no prefix, skipping\n");
          continue;
        }
        LLVM_DEBUG(llvm::dbgs()
                   << "  - Shortest prefix " << *shortestPrefix << "\n");

        // Concatenate the prefix into a proper full hierarchical name.
        SmallString<128> hname;
        for (auto inst : shortestPrefix.getValue()) {
          if (!hname.empty())
            hname += '.';
          hname += inst.name();
        }
        if (!hname.empty())
          hname += '.';
        hname += port.suffix;
        LLVM_DEBUG(llvm::dbgs() << "  - Connecting as " << hname << "\n");

        // Add a verbatim op that assigns this module port.
        auto arg = impl.getArgument(port.portNum);
        auto hnameExpr = builder.create<VerbatimExprOp>(
            arg.getType().cast<FIRRTLType>(), hname);
        builder.create<ConnectOp>(arg, hnameExpr);
      }

      // Switch the instance from the original extmodule to this implementation.
      // CAVEAT: If the same black box data tap module is instantiated in a
      // parent module that itself is instantiated in different locations, this
      // will pretty arbitrarily pick one of those locations.
      if (!path.empty())
        path.back()->setAttr("moduleName",
                             builder.getSymbolRefAttr(name.getValue()));
    }

    // Drop the original black box module.
    blackBox.extModule.erase();
  }
}

/// Gather the annotations on ports and operations into the `tappedPorts` and
/// `tappedOps` maps.
void GrandCentralTapsPass::gatherAnnotations(Operation *op) {
  if (isa<FModuleOp, FExtModuleOp>(op)) {
    // Handle port annotations on module/extmodule ops.
    auto gather = [&](unsigned argNum, Annotation anno) {
      if (isReferenceDataTapSource(anno)) {
        gatherTap(anno, Port{op, argNum});
        return true;
      }
      return false;
    };
    AnnotationSet::removePortAnnotations(op, gather);

    // Handle internal data taps on extmodule ops.
    if (isa<FExtModuleOp>(op)) {
      auto gather = [&](Annotation anno) {
        if (anno.isClass(internalKeyClass)) {
          gatherTap(anno, op);
          return true;
        }
        return false;
      };
      AnnotationSet::removeAnnotations(op, gather);
    }

    return;
  }

  // Go through all annotations on this op and extract the interesting
  // ones. Note that the way tap annotations are scattered to their
  // targets, we should never see multiple values or memories annotated
  // with the exact same annotation (hence the asserts).
  AnnotationSet::removeAnnotations(op, [&](Annotation anno) {
    if (anno.isClass(memTapClass) || isReferenceDataTapSource(anno)) {
      gatherTap(anno, op);
      return true;
    }
    return false;
  });
}

void GrandCentralTapsPass::processAnnotation(AnnotatedPort &portAnno,
                                             AnnotatedExtModule &blackBox,
                                             InstancePaths &instancePaths) {
  LLVM_DEBUG(llvm::dbgs() << "- Processing port " << portAnno.portNum
                          << " anno " << portAnno.anno.getDict() << "\n");
  auto key = getKey(portAnno.anno);
  auto portName = getModulePortName(blackBox.extModule, portAnno.portNum);
  PortWiring wiring;
  wiring.portNum = portAnno.portNum;

  // Handle data taps on signals and ports.
  if (portAnno.anno.isClass(referenceKeyClass)) {
    // Handle ports.
    if (auto port = tappedPorts.lookup(key)) {
      wiring.prefices = instancePaths.getAbsolutePaths(port.first);
      wiring.suffix = getModulePortName(port.first, port.second).getValue();
      portWiring.push_back(std::move(wiring));
      return;
    }

    // Handle operations.
    if (auto op = tappedOps.lookup(key)) {
      // We require the target to be a wire or node, such that it gets a name
      // during Verilog emission.
      if (!isa<WireOp, NodeOp, RegOp, RegResetOp>(op)) {
        auto diag = blackBox.extModule.emitError("ReferenceDataTapKey on port ")
                    << portName << " must be a wire, node, or reg";
        diag.attachNote(op->getLoc()) << "referenced operation is here:";
        signalPassFailure();
        return;
      }

      // We currently require the target to be named.
      // TODO: If we were to use proper cross-module reference ops in the IR
      // then this could be anonymous, with ExportVerilog resolving the name
      // at the last moment.
      auto name = op->getAttrOfType<StringAttr>("name");
      if (!name) {
        auto diag =
            op->emitError("declaration targeted by data tap must have a name");
        diag.attachNote(blackBox.extModule->getLoc())
            << "used by ReferenceDataTapKey on port " << portName << " here:";
        signalPassFailure();
        return;
      }

      wiring.prefices =
          instancePaths.getAbsolutePaths(op->getParentOfType<FModuleOp>());
      wiring.suffix = name.getValue();
      portWiring.push_back(std::move(wiring));
      return;
    }

    // The annotation scattering must have placed this annotation on some
    // target operation or block argument, which we should have picked up in
    // the tapped args or ops maps.
    blackBox.extModule.emitOpError(
        "ReferenceDataTapKey annotation was not scattered to "
        "an operation: ")
        << portAnno.anno.getDict();
    signalPassFailure();
    return;
  }

  // Handle data taps on black boxes.
  if (portAnno.anno.isClass(internalKeyClass)) {
    auto op = tappedOps.lookup(key);
    if (!op) {
      blackBox.extModule.emitOpError(
          "DataTapModuleSignalKey annotation was not scattered to "
          "an operation: ")
          << portAnno.anno.getDict();
      signalPassFailure();
      return;
    }

    // Extract the internal path we're supposed to append.
    auto internalPath = portAnno.anno.getMember<StringAttr>("internalPath");
    if (!internalPath) {
      blackBox.extModule.emitError("DataTapModuleSignalKey annotation on port ")
          << portName << " missing \"internalPath\" attribute";
      signalPassFailure();
      return;
    }

    wiring.prefices = instancePaths.getAbsolutePaths(op);
    wiring.suffix = internalPath.getValue();
    portWiring.push_back(std::move(wiring));
    return;
  }

  // Handle data taps with literals.
  if (portAnno.anno.isClass(literalKeyClass)) {
    auto literal = portAnno.anno.getMember<StringAttr>("literal");
    if (!literal) {
      blackBox.extModule.emitError("LiteralDataTapKey annotation on port ")
          << portName << " missing \"literal\" attribute";
      signalPassFailure();
      return;
    }

    wiring.literal = literal;
    portWiring.push_back(std::move(wiring));
    return;
  }

  // Handle memory taps.
  if (portAnno.anno.isClass(memTapClass)) {
    auto op = tappedOps.lookup(key);
    if (!op) {
      blackBox.extModule.emitOpError(
          "MemTapAnnotation annotation was not scattered to "
          "an operation: ")
          << portAnno.anno.getDict();
      signalPassFailure();
      return;
    }

    // Extract the name of the memory.
    // TODO: This would be better handled through a proper cross-module
    // reference preserved in the IR, such that ExportVerilog can insert a
    // proper name here at the last moment.
    auto name = op->getAttrOfType<StringAttr>("name");
    if (!name) {
      auto diag = op->emitError("target of memory tap must have a name");
      diag.attachNote(blackBox.extModule->getLoc())
          << "used by MemTapAnnotation on port " << portName << " here:";
      signalPassFailure();
      return;
    }

    // Formulate a hierarchical reference into the memory.
    // CAVEAT: This just assumes that the memory will map to something that
    // can be indexed in the final Verilog. If the memory gets turned into
    // an instance of some sort, we lack the information necessary to go in
    // and access individual elements of it. This will break horribly since
    // we generate memory impls out-of-line already, and memories coming
    // from an external generator are even worse. This needs a special node
    // in the IR that can properly inject the memory array on emission.
    wiring.prefices =
        instancePaths.getAbsolutePaths(op->getParentOfType<FModuleOp>());
    (Twine(name.getValue()) + ".Memory[" +
     llvm::utostr(memPortIdx[portAnno.anno.getDict()]++) + "]")
        .toVector(wiring.suffix);
    portWiring.push_back(std::move(wiring));
    return;
  }

  // We never arrive here since the above code that populates the portAnnos
  // list only adds annotations that we handle in one of the if statements
  // above.
  llvm_unreachable("portAnnos is never populated with unsupported annos");
}

std::unique_ptr<mlir::Pass> circt::firrtl::createGrandCentralTapsPass() {
  return std::make_unique<GrandCentralTapsPass>();
}
