//===- GrandCentral.cpp - Ingest black box sources --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===----------------------------------------------------------------------===//
//
// Implement SiFive's Grand Central transform.  Currently, this supports
// SystemVerilog Interface generation.
//
//===----------------------------------------------------------------------===//

#include "PassDetails.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "circt/Dialect/FIRRTL/FIRRTLVisitors.h"
#include "circt/Dialect/FIRRTL/Passes.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "circt/Dialect/SV/SVOps.h"
#include "mlir/IR/ImplicitLocOpBuilder.h"
#include "llvm/ADT/StringSwitch.h"

using namespace circt;
using namespace firrtl;

//===----------------------------------------------------------------------===//
// Pass Implementation
//===----------------------------------------------------------------------===//

namespace {
/// Mutable store of information about an Element in an interface.  This is
/// derived from information stored in the "elements" field of an
/// "AugmentedBundleType".  This is updated as more information is known about
/// an Element.
struct ElementInfo {
  /// Encodes the "tpe" of an element.  This is called "Kind" to avoid
  /// overloading the meaning of "Type" (which also conflicts with mlir::Type).
  enum Kind {
    Error = -1,
    Ground,
    Vector,
    Bundle,
    String,
    Boolean,
    Integer,
    Double
  };
  /// The "tpe" field indicating if this element of the interface is a ground
  /// type, a vector type, or a bundle type.  Bundle types are nested
  /// interfaces.
  Kind tpe;
  /// A string description that will show up as a comment in the output Verilog.
  StringRef description;
  /// The width of this interface.  This is only non-negative for ground or
  /// vector types.
  int32_t width = -1;
  /// The depth of the interface.  This is one for ground types and greater
  /// than one for vector types.
  uint32_t depth = 0;
  /// Indicate if this element was found in the circuit.
  bool found = false;
  /// Trakcs location information about what was used to build this element.
  SmallVector<Location> locations = {};
  /// True if this is a ground or vector type and it was not (statefully) found.
  /// This indicates that an interface element, which is composed of ground and
  /// vector types, found no matching, annotated components in the circuit.
  bool isMissing() { return !found && (tpe == Ground || tpe == Vector); }
};

/// Stores a decoded Grand Central AugmentedField
struct AugmentedField {
  /// The name of the field.
  StringRef name;
  /// An optional descripton that the user provided for the field.  This should
  /// become a comment in the Verilog.
  StringRef description;
  /// The "type" of the field.
  ElementInfo::Kind tpe;
};

/// Stores a decoded Grand Central AugmentedBundleType.
struct AugmentedBundleType {
  /// The name of the interface.
  StringRef defName;
  /// The elements that make up the body of the interface.
  SmallVector<AugmentedField> elements;
};

/// Convert an arbitrary attributes into an optional AugmentedField.  Returns
/// None if the attribute is an invalid AugmentedField.
static Optional<AugmentedField> decodeField(Attribute maybeField) {
  auto field = maybeField.dyn_cast_or_null<DictionaryAttr>();
  if (!field)
    return {};
  auto tpeString = field.getAs<StringAttr>("tpe");
  auto name = field.getAs<StringAttr>("name");
  if (!name || !tpeString)
    return {};
  auto tpe = llvm::StringSwitch<ElementInfo::Kind>(tpeString.getValue())
                 .Case("sifive.enterprise.grandcentral.AugmentedBundleType",
                       ElementInfo::Bundle)
                 .Case("sifive.enterprise.grandcentral.AugmentedVectorType",
                       ElementInfo::Vector)
                 .Case("sifive.enterprise.grandcentral.AugmentedGroundType",
                       ElementInfo::Ground)
                 .Case("sifive.enterprise.grandcentral.AugmentedStringType",
                       ElementInfo::String)
                 .Case("sifive.enterprise.grandcentral.AugmentedBooleanType",
                       ElementInfo::Boolean)
                 .Case("sifive.enterprise.grandcentral.AugmentedIntegerType",
                       ElementInfo::Integer)
                 .Case("sifive.enterprise.grandcentral.AugmentedDoubleType",
                       ElementInfo::Double)
                 .Default(ElementInfo::Error);
  if (tpe == ElementInfo::Error)
    return {};

  StringRef description = {};
  if (auto maybeDescription = field.getAs<StringAttr>("description"))
    description = maybeDescription.getValue();
  return Optional<AugmentedField>({name.getValue(), description, tpe});
};

/// Convert an Annotation into an optional AugmentedBundleType.  Returns None if
/// the annotation is not an AugmentedBundleType.
static Optional<AugmentedBundleType> decodeBundleType(Annotation anno) {
  auto defName = anno.getMember<StringAttr>("defName");
  auto elements = anno.getMember<ArrayAttr>("elements");
  if (!defName || !elements)
    return {};
  AugmentedBundleType bundle(
      {defName.getValue(), SmallVector<AugmentedField>()});
  for (auto element : elements) {
    auto field = decodeField(element);
    if (!field)
      return {};
    bundle.elements.push_back(field.getValue());
  }
  return Optional<AugmentedBundleType>(bundle);
};

/// Remove Grand Central Annotations associated with SystemVerilog interfaces
/// that should emitted.  This pass works in three major phases:
///
/// 1. The circuit's annotations are examnined to figure out _what_ interfaces
///    there are.  This includes information about the name of the interface
///    ("defName") and each of the elements (sv::InterfaceSignalOp) that make up
///    the interface.  However, no information about the _type_ of the elements
///    is known.
///
/// 2. With this, information, walk through the circuit to find scattered
///    information about the types of the interface elements.  Annotations are
///    scattered during FIRRTL parsing to attach all the annotations associated
///    with elements on the right components.
///
/// 3. Add interface ops and populate the elements.
///
/// Grand Central supports three "normal" element types and four "weird" element
/// types.  The normal ones are ground types (SystemVerilog logic), vector types
/// (SystemVerilog unpacked arrays), and nested interface types (another
/// SystemVerilog interface).  The Chisel API provides "weird" elements that
/// include: Boolean, Integer, String, and Double.  The SFC implementation
/// currently drops these, but this pass emits them as commented out strings.
struct GrandCentralPass : public GrandCentralBase<GrandCentralPass> {
  void runOnOperation() override;

  // A map storing mutable information about an element in an interface.  This
  // is keyed using a (defName, name) tuple where defname is the name of the
  // interface and name is the name of the element.
  typedef DenseMap<std::pair<StringRef, StringRef>, ElementInfo> InterfaceMap;

private:
  // Store a mapping of interface name to InterfaceOp.
  llvm::StringMap<sv::InterfaceOp> interfaces;

  // Discovered interfaces that need to be constructed.
  InterfaceMap interfaceMap;

  // Track the order that interfaces should be emitted in.
  SmallVector<std::pair<StringRef, StringRef>> interfaceKeys;
};

class GrandCentralVisitor : public FIRRTLVisitor<GrandCentralVisitor> {
public:
  GrandCentralVisitor(GrandCentralPass::InterfaceMap &interfaceMap)
      : interfaceMap(interfaceMap) {}

private:
  /// Mutable store tracking each element in an interface.  This is indexed by a
  /// "defName" -> "name" tuple.
  GrandCentralPass::InterfaceMap &interfaceMap;

  /// Helper to handle wires, registers, and nodes.
  void handleRef(Operation *op);

  /// A helper used by handleRef that can also be used to process ports.
  void handleRefLike(Operation *op, AnnotationSet &annotations,
                     FIRRTLType type);

  // Helper to handle ports of modules that may have Grand Central annotations.
  void handlePorts(Operation *op);

  // If true, then some error occurred while the visitor was running.  This
  // indicates that pass failure should occur.
  bool failed = false;

public:
  using FIRRTLVisitor<GrandCentralVisitor>::visitDecl;

  /// Visit FModuleOp and FExtModuleOp
  void visitModule(Operation *op);

  /// Visit ops that can make up an interface element.
  void visitDecl(RegOp op) { handleRef(op); }
  void visitDecl(RegResetOp op) { handleRef(op); }
  void visitDecl(WireOp op) { handleRef(op); }
  void visitDecl(NodeOp op) { handleRef(op); }
  void visitDecl(InstanceOp op);

  /// Process all other ops.  Error if any of these ops contain annotations that
  /// indicate it as being part of an interface.
  void visitUnhandledDecl(Operation *op);

  /// Returns true if an error condition occurred while visiting ops.
  bool hasFailed() { return failed; };
};

} // namespace

void GrandCentralVisitor::visitModule(Operation *op) {
  handlePorts(op);

  if (isa<FModuleOp>(op))
    for (auto &stmt : op->getRegion(0).front())
      dispatchVisitor(&stmt);
}

/// Process all other operations.  This will throw an error if the operation
/// contains any annotations that indicates that this should be included in an
/// interface.  Otherwise, this is a valid nop.
void GrandCentralVisitor::visitUnhandledDecl(Operation *op) {
  AnnotationSet annotations(op);
  auto anno = annotations.getAnnotation(
      "sifive.enterprise.grandcentral.AugmentedGroundType");
  if (!anno)
    return;
  auto diag =
      op->emitOpError()
      << "is marked as a an interface element, but this op or its ports are "
         "not supposed to be interface elements (Are your annotations "
         "malformed? Is this a missing feature that should be supported?)";
  diag.attachNote()
      << "this annotation marked the op as an interface element: '" << anno
      << "'";
  failed = true;
}

/// Process annotations associated with an operation and having some type.
/// Return the annotations with the processed annotations removed.  If all
/// annotations are removed, this returns an empty ArrayAttr.
void GrandCentralVisitor::handleRefLike(mlir::Operation *op,
                                        AnnotationSet &annotations,
                                        FIRRTLType type) {
  if (annotations.empty())
    return;

  bool foundAnnotations = false;
  for (auto anno : annotations) {
    if (!anno.isClass("sifive.enterprise.grandcentral.AugmentedGroundType"))
      continue;

    auto defName = anno.getMember<StringAttr>("defName");
    auto name = anno.getMember<StringAttr>("name");
    if (!defName || !name) {
      op->emitOpError(
            "is marked as part of an interface, but is missing 'defName' or "
            "'name' fields (did you forget to add these?)")
              .attachNote()
          << "the full annotation is: " << anno.getDict();
      failed = true;
      continue;
    }

    // TODO: This is ignoring situations where the leaves of and interface are
    // not ground types.  This enforces the requirement that this runs after
    // LowerTypes.  However, this could eventually be relaxed.
    if (!type.isGround()) {
      auto diag = op->emitOpError()
                  << "cannot be added to interface '" << defName.getValue()
                  << "', component '" << name.getValue()
                  << "' because it is not a ground type. (Got type '" << type
                  << "'.) This will be dropped from the interface. (Did you "
                     "forget to run LowerTypes?)";
      diag.attachNote()
          << "The annotation indicating that this should be added was: '"
          << anno.getDict();
      failed = true;
      continue;
    }

    auto &component = interfaceMap[{defName.getValue(), name.getValue()}];
    component.found = true;

    switch (component.tpe) {
    case ElementInfo::Vector:
      component.width = type.getBitWidthOrSentinel();
      component.depth++;
      component.locations.push_back(op->getLoc());
      break;
    case ElementInfo::Ground:
      component.width = type.getBitWidthOrSentinel();
      component.locations.push_back(op->getLoc());
      break;
    case ElementInfo::Bundle:
    case ElementInfo::String:
    case ElementInfo::Boolean:
    case ElementInfo::Integer:
    case ElementInfo::Double:
      break;
    case ElementInfo::Error:
      llvm_unreachable("Shouldn't be here");
      break;
    }
    annotations.removeAnnotation(anno);
    foundAnnotations = true;
  }
}

/// Combined logic to handle Wires, Registers, and Nodes because these all use
/// the same approach.
void GrandCentralVisitor::handleRef(mlir::Operation *op) {
  auto annotations = AnnotationSet(op);
  handleRefLike(op, annotations, op->getResult(0).getType().cast<FIRRTLType>());
  annotations.applyToOperation(op);
}

/// Remove Grand Central Annotations from ports of modules or external modules.
/// Return argument attributes with annotations removed.
void GrandCentralVisitor::handlePorts(Operation *op) {

  SmallVector<Attribute> newArgAttrs;
  auto ports = getModulePortInfo(op);
  for (size_t i = 0, e = ports.size(); i != e; ++i) {
    auto port = ports[i];
    handleRefLike(op, port.annotations, port.type);

    newArgAttrs.push_back(port.annotations.applyToPortDictionaryAttr(
        DictionaryAttr::get(op->getContext(), {})));
  }

  mlir::function_like_impl::setAllArgAttrDicts(op, newArgAttrs);
}

void GrandCentralVisitor::visitDecl(InstanceOp op) {

  // If this instance's underlying module has a "companion" annotation, then
  // move this onto the actual instance op.
  AnnotationSet annotations(op.getReferencedModule());
  if (auto anno = annotations.getAnnotation(
          "sifive.enterprise.grandcentral.GrandCentralView$"
          "SerializedViewAnnotation")) {
    auto tpe = anno.getAs<StringAttr>("type");
    if (!tpe) {
      op.getReferencedModule()->emitOpError(
          "contains a GrandcCentralView$SerializedViewAnnotation that does not "
          "contain a \"type\" field");
      failed = true;
      return;
    }
    if (tpe.getValue() == "companion")
      op->setAttr("lowerToBind", BoolAttr::get(op.getContext(), true));
  }

  visitUnhandledDecl(op);
}

void GrandCentralPass::runOnOperation() {
  CircuitOp circuitOp = getOperation();

  AnnotationSet annotations(circuitOp);
  if (annotations.empty())
    return;

  // Setup the builder to create ops _inside the FIRRTL circuit_.  This is
  // necessary because interfaces and interface instances are created.
  // Instances link to their definitions via symbols and we don't want to break
  // this.
  auto builder = OpBuilder::atBlockEnd(circuitOp.getBody());

  // Utility that acts like emitOpError, but does _not_ include a note.  The
  // note in emitOpError includes the entire op which means the **ENTIRE**
  // FIRRTL circuit.  This doesn't communicate anything useful to the user other
  // than flooding their terminal.
  auto emitCircuitError = [&circuitOp](StringRef message = {}) {
    return emitError(circuitOp.getLoc(), message);
  };

  // Examine the Circuit's Annotations doing work to remove Grand Central
  // Annotations.  Ignore any unprocesssed annotations and rewrite the Circuit's
  // Annotations with these when done.
  bool removalError = false;
  annotations.removeAnnotations([&](auto anno) {
    if (!anno.isClass("sifive.enterprise.grandcentral.AugmentedBundleType"))
      return false;

    AugmentedBundleType bundle;
    if (auto maybeBundle = decodeBundleType(anno))
      bundle = maybeBundle.getValue();
    else {
      emitCircuitError(
          "'firrtl.circuit' op contained an 'AugmentedBundleType' "
          "Annotation which did not conform to the expected format")
              .attachNote()
          << "the problematic 'AugmentedBundleType' is: '" << anno.getDict()
          << "'";
      removalError = true;
      return false;
    }

    for (auto elt : bundle.elements) {
      std::pair<StringRef, StringRef> key = {bundle.defName, elt.name};
      interfaceMap[key] = {elt.tpe, elt.description};
      interfaceKeys.push_back(key);
    }

    // If the interface already exists, don't create it.
    if (interfaces.count(bundle.defName))
      return true;

    // Create the interface.  This will be populated later.
    interfaces[bundle.defName] =
        builder.create<sv::InterfaceOp>(circuitOp->getLoc(), bundle.defName);

    return true;
  });

  if (removalError)
    return signalPassFailure();

  // Remove the processed annotations.
  circuitOp->setAttr("annotations", annotations.getArrayAttr());

  // Walk through the circuit to collect additional information.  If this fails,
  // signal pass failure.  Walk in reverse order so that annotations can be
  // removed from modules after all referring instances have consumed their
  // annotations.
  for (auto &op : llvm::reverse(circuitOp.getBody()->getOperations())) {
    if (isa<FModuleOp, FExtModuleOp>(op)) {
      GrandCentralVisitor visitor(interfaceMap);
      visitor.visitModule(&op);
      if (visitor.hasFailed())
        return signalPassFailure();
      AnnotationSet annotations(&op);
      // Insert an instantiated interface
      if (auto viewAnnotation = annotations.getAnnotation(
              "sifive.enterprise.grandcentral.GrandCentralView$"
              "SerializedViewAnnotation")) {
        auto tpe = viewAnnotation.getAs<StringAttr>("type");
        if (tpe && tpe.getValue() == "parent") {
          auto name = viewAnnotation.getAs<StringAttr>("name");
          auto defName = viewAnnotation.getAs<StringAttr>("defName");
          auto guard = OpBuilder::InsertionGuard(builder);
          builder.setInsertionPointToEnd(cast<FModuleOp>(op).getBodyBlock());
          auto instance = builder.create<sv::InterfaceInstanceOp>(
              circuitOp->getLoc(),
              interfaces.lookup(defName.getValue()).getInterfaceType(), name,
              builder.getStringAttr("__" + defName.getValue() + "__"));
          instance->setAttr("doNotPrint", builder.getBoolAttr(true));
          builder.setInsertionPointToStart(
              op.getParentOfType<ModuleOp>().getBody());
          auto bind = builder.create<sv::BindInterfaceOp>(
              circuitOp->getLoc(),
              builder.getSymbolRefAttr(
                  ("__" + defName.getValue() + "__").str()));
          bind->setAttr(
              "output_file",
              hw::OutputFileAttr::get(
                  builder.getStringAttr(""),
                  builder.getStringAttr("bindings.sv"),
                  /*exclude_from_filelist=*/builder.getBoolAttr(true),
                  /*exclude_replicated_ops=*/builder.getBoolAttr(true),
                  bind.getContext()));
        }
      }
      annotations.removeAnnotations([](auto anno) {
        return anno.isClass("sifive.enterprise.grandcentral.GrandCentralView$"
                            "SerializedViewAnnotation");
      });
      annotations.applyToOperation(&op);
    }
  }

  // Populate interfaces.
  for (auto &a : interfaceKeys) {
    auto defName = a.first;
    auto name = a.second;

    auto &info = interfaceMap[{defName, name}];
    if (info.isMissing()) {
      emitCircuitError()
          << "'firrtl.circuit' op contained a Grand Central Interface '"
          << defName << "' that had an element '" << name
          << "' which did not have a scattered companion annotation (is there "
             "an invalid target in your annotation file?)";
      continue;
    }

    builder.setInsertionPointToEnd(interfaces[defName].getBodyBlock());

    auto loc = builder.getFusedLoc(info.locations);
    auto description = info.description;
    if (!description.empty())
      builder.create<sv::VerbatimOp>(loc, "\n// " + description);

    switch (info.tpe) {
    case ElementInfo::Bundle:
      // TODO: Change this to actually use an interface type.  This currently
      // does not work because: (1) interfaces don't have a defined way to get
      // their bit width and (2) interfaces have a symbol table that is used to
      // verify internal ops, but this requires looking arbitrarily far upwards
      // to find other symbols.
      builder.create<sv::VerbatimOp>(loc, name + " " + name + "();");
      break;
    case ElementInfo::Vector: {
      auto type = hw::UnpackedArrayType::get(builder.getIntegerType(info.width),
                                             info.depth);
      builder.create<sv::InterfaceSignalOp>(loc, name, type);
      break;
    }
    case ElementInfo::Ground: {
      auto type = builder.getIntegerType(info.width);
      builder.create<sv::InterfaceSignalOp>(loc, name, type);
      break;
    }
    case ElementInfo::String:
      builder.create<sv::VerbatimOp>(loc, "// " + name +
                                              " = <unsupported string type>;");
      break;
    case ElementInfo::Boolean:
      builder.create<sv::VerbatimOp>(loc, "// " + name +
                                              " = <unsupported boolean type>;");
      break;
    case ElementInfo::Integer:
      builder.create<sv::VerbatimOp>(loc, "// " + name +
                                              " = <unsupported integer type>;");
      break;
    case ElementInfo::Double:
      builder.create<sv::VerbatimOp>(loc, "// " + name +
                                              " = <unsupported double type>;");
      break;
    case ElementInfo::Error:
      llvm_unreachable("Shouldn't be here");
      break;
    }
  }

  interfaces.clear();
  interfaceMap.clear();
  interfaceKeys.clear();
}

//===----------------------------------------------------------------------===//
// Pass Creation
//===----------------------------------------------------------------------===//

std::unique_ptr<mlir::Pass> circt::firrtl::createGrandCentralPass() {
  return std::make_unique<GrandCentralPass>();
}
