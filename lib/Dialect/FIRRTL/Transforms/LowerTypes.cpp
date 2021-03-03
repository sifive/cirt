//===- LowerTypes.cpp - Lower FIRRTL types to ground types ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===----------------------------------------------------------------------===//
//
// This file implements the lowering of FIRRTL types to ground types.
//
//===----------------------------------------------------------------------===//

#include "./PassDetails.h"
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "circt/Dialect/FIRRTL/FIRRTLTypes.h"
#include "circt/Dialect/FIRRTL/FIRRTLVisitors.h"
#include "circt/Dialect/FIRRTL/Passes.h"
#include "circt/Support/ImplicitLocOpBuilder.h"
#include "circt/Support/LLVM.h"
#include "mlir/IR/FunctionSupport.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/StringSwitch.h"
#include <algorithm>

using namespace circt;
using namespace firrtl;

namespace {
// This represents a flattened bundle field element.
struct FlatBundleFieldEntry {
  // This is the underlying ground type of the field.
  FIRRTLType type;
  // This is a suffix to add to the field name to make it unique.
  std::string suffix;
  // This indicates whether the field was flipped to be an output.
  bool isOutput;

  // Helper to determine if a fully flattened type needs to be flipped.
  FIRRTLType getPortType() { return isOutput ? FlipType::get(type) : type; }
};
} // end anonymous namespace

// Convert an aggregate type into a flat list of fields.  This is used
// when working with instances and mems to flatten them.
static void flattenType(FIRRTLType type, StringRef suffixSoFar, bool isFlipped,
                        SmallVectorImpl<FlatBundleFieldEntry> &results) {
  if (auto flip = type.dyn_cast<FlipType>())
    return flattenType(flip.getElementType(), suffixSoFar, !isFlipped, results);

  TypeSwitch<FIRRTLType>(type)
      .Case<BundleType>([&](auto bundle) {
        SmallString<16> tmpSuffix(suffixSoFar);

        // Otherwise, we have a bundle type.  Break it down.
        for (auto &elt : bundle.getElements()) {
          // Construct the suffix to pass down.
          tmpSuffix.resize(suffixSoFar.size());
          tmpSuffix.push_back('_');
          tmpSuffix.append(elt.name.strref());
          // Recursively process subelements.
          flattenType(elt.type, tmpSuffix, isFlipped, results);
        }
        return;
      })
      .Case<FVectorType>([&](auto vector) {
        for (size_t i = 0, e = vector.getNumElements(); i != e; ++i)
          flattenType(vector.getElementType(),
                      (suffixSoFar + "_" + std::to_string(i)).str(), isFlipped,
                      results);
        return;
      })
      .Default([&](auto) {
        results.push_back({type, suffixSoFar.str(), isFlipped});
        return;
      });

  return;
}

// Helper to peel off the outer most flip type from an aggregate type that has
// all flips canonicalized to the outer level, or just return the bundle
// directly. For any ground type, returns null.
static FIRRTLType getCanonicalAggregateType(Type originalType) {
  FIRRTLType unflipped = originalType.dyn_cast<FIRRTLType>();
  if (auto flipType = originalType.dyn_cast<FlipType>())
    unflipped = flipType.getElementType();

  return TypeSwitch<FIRRTLType, FIRRTLType>(unflipped)
      .Case<BundleType, FVectorType>([](auto a) { return a; })
      .Default([](auto) { return nullptr; });
}

//===----------------------------------------------------------------------===//
// Pass Infrastructure
//===----------------------------------------------------------------------===//

namespace {
struct FIRRTLTypesLowering : public LowerFIRRTLTypesBase<FIRRTLTypesLowering>,
                             public FIRRTLVisitor<FIRRTLTypesLowering> {

  using ValueIdentifier = std::pair<Value, Identifier>;
  using FIRRTLVisitor<FIRRTLTypesLowering>::visitDecl;
  using FIRRTLVisitor<FIRRTLTypesLowering>::visitExpr;
  using FIRRTLVisitor<FIRRTLTypesLowering>::visitStmt;

  void runOnOperation() override;

  // Lowering operations.
  void visitDecl(InstanceOp op);
  void visitDecl(MemOp op);
  void visitDecl(RegOp op);
  void visitDecl(WireOp op);
  void visitExpr(InvalidValuePrimOp op);
  void visitExpr(SubfieldOp op);
  void visitExpr(SubindexOp op);
  void visitStmt(ConnectOp op);

private:
  // Lowering module block arguments.
  void lowerArg(BlockArgument arg, FIRRTLType type);

  // Helpers to manage state.
  Value addArg(Type type, unsigned oldArgNumber, StringRef nameSuffix = "");

  void setBundleLowering(Value oldValue, StringRef flatField, Value newValue);
  Value getBundleLowering(Value oldValue, StringRef flatField);
  void getAllBundleLowerings(Value oldValue, SmallVectorImpl<Value> &results);

  Identifier getArgAttrName(unsigned newArgNumber);

  // The builder is set and maintained in the main loop.
  ImplicitLocOpBuilder *builder;

  // State to keep track of arguments and operations to clean up at the end.
  SmallVector<unsigned, 8> argsToRemove;
  SmallVector<Operation *, 16> opsToRemove;

  // State to keep a mapping from (Value, Identifier) pairs to flattened values.
  DenseMap<ValueIdentifier, Value> loweredBundleValues;

  // State to track the new attributes for the module.
  SmallVector<NamedAttribute, 8> newAttrs;
  size_t originalNumArgs;
};
} // end anonymous namespace

/// This is the pass constructor.
std::unique_ptr<mlir::Pass> circt::firrtl::createLowerFIRRTLTypesPass() {
  return std::make_unique<FIRRTLTypesLowering>();
}

// This is the main entrypoint for the lowering pass.
void FIRRTLTypesLowering::runOnOperation() {
  auto module = getOperation();
  auto *body = module.getBodyBlock();

  ImplicitLocOpBuilder theBuilder(module.getLoc(), &getContext());
  builder = &theBuilder;

  // Remember the original argument attributess.
  SmallVector<NamedAttribute, 8> originalArgAttrs;
  DictionaryAttr originalAttrs = module->getAttrDictionary();
  for (size_t i = 0, e = module.getNumArguments(); i < e; ++i)
    originalArgAttrs.push_back(
        originalAttrs.getNamed(getArgAttrName(i)).getValue());

  // Lower the module block arguments.
  SmallVector<BlockArgument, 8> args(body->args_begin(), body->args_end());
  originalNumArgs = args.size();
  for (auto arg : args)
    if (auto type = arg.getType().dyn_cast<FIRRTLType>())
      lowerArg(arg, type);

  // Lower the operations.
  for (auto &op : body->getOperations()) {
    builder->setInsertionPoint(&op);
    builder->setLoc(op.getLoc());
    dispatchVisitor(&op);
  }

  // Remove ops that have been lowered.
  while (!opsToRemove.empty())
    opsToRemove.pop_back_val()->erase();

  if (argsToRemove.empty())
    return;

  // Remove block args that have been lowered.
  body->eraseArguments(argsToRemove);
  argsToRemove.clear();

  // Copy over any attributes that weren't original argument attributes.
  auto *argAttrBegin = originalArgAttrs.begin();
  auto *argAttrEnd = originalArgAttrs.end();
  std::sort(argAttrBegin, argAttrEnd);
  for (auto attr : originalAttrs)
    if (std::lower_bound(argAttrBegin, argAttrEnd, attr) == argAttrEnd)
      newAttrs.push_back(attr);

  // Update the module's attributes.
  module->setAttrs(newAttrs);
  newAttrs.clear();

  // Keep the module's type up-to-date.
  auto moduleType = builder->getFunctionType(body->getArgumentTypes(), {});
  module->setAttr(module.getTypeAttrName(), TypeAttr::get(moduleType));

  // Reset lowered state.
  loweredBundleValues.clear();
}

//===----------------------------------------------------------------------===//
// Lowering module block arguments.
//===----------------------------------------------------------------------===//

// Lower arguments with bundle type by flattening them.
void FIRRTLTypesLowering::lowerArg(BlockArgument arg, FIRRTLType type) {
  unsigned argNumber = arg.getArgNumber();

  // Flatten any bundle types.
  SmallVector<FlatBundleFieldEntry, 8> fieldTypes;
  flattenType(type, "", false, fieldTypes);

  for (auto field : fieldTypes) {

    // Create new block arguments.
    auto type = field.getPortType();
    auto newValue = addArg(type, argNumber, field.suffix);

    // If this field was flattened from a bundle.
    if (!field.suffix.empty()) {
      // Remove field separator prefix for consitency with the rest of the pass.
      auto fieldName = StringRef(field.suffix).drop_front(1);

      // Map the flattened suffix for the original bundle to the new value.
      setBundleLowering(arg, fieldName, newValue);
    } else {
      // Lower any other arguments by copying them to keep the relative order.
      arg.replaceAllUsesWith(newValue);
    }
  }

  // Remember to remove the original block argument.
  argsToRemove.push_back(argNumber);
}

//===----------------------------------------------------------------------===//
// Lowering operations.
//===----------------------------------------------------------------------===//

// Lower instance operations in the same way as module block arguments. Bundles
// are flattened, and other arguments are copied to keep the relative order. By
// ensuring both lowerings are the same, we can process every module in the
// circuit in parallel, and every instance will have the correct ports.
void FIRRTLTypesLowering::visitDecl(InstanceOp op) {
  // Create a new, flat bundle type for the new result
  SmallVector<Type, 8> resultTypes;
  SmallVector<Attribute, 8> resultNames;
  SmallVector<size_t, 8> numFieldsPerResult;
  for (size_t i = 0, e = op.getNumResults(); i != e; ++i) {
    // Flatten any nested bundle types the usual way.
    SmallVector<FlatBundleFieldEntry, 8> fieldTypes;
    flattenType(op.getType(i).cast<FIRRTLType>(), op.getPortNameStr(i),
                /*isFlip*/ false, fieldTypes);

    for (auto field : fieldTypes) {
      // Store the flat type for the new bundle type.
      resultNames.push_back(builder->getStringAttr(field.suffix));
      resultTypes.push_back(field.getPortType());
    }
    numFieldsPerResult.push_back(fieldTypes.size());
  }

  auto newInstance = builder->create<InstanceOp>(
      resultTypes, op.moduleNameAttr(), builder->getArrayAttr(resultNames),
      op.nameAttr());

  // Record the mapping of each old result to each new result.
  size_t nextResult = 0;
  for (size_t i = 0, e = op.getNumResults(); i != e; ++i) {
    // If this result was a non-bundle value, just RAUW it.
    auto origPortName = op.getPortNameStr(i);
    if (numFieldsPerResult[i] == 1 &&
        newInstance.getPortNameStr(nextResult) == origPortName) {
      op.getResult(i).replaceAllUsesWith(newInstance.getResult(nextResult));
      ++nextResult;
      continue;
    }

    // Otherwise lower bundles.
    for (size_t j = 0, e = numFieldsPerResult[i]; j != e; ++j) {
      auto newPortName = newInstance.getPortNameStr(nextResult);
      // Drop the original port name and the underscore.
      newPortName = newPortName.drop_front(origPortName.size() + 1);

      // Map the flattened suffix for the original bundle to the new value.
      setBundleLowering(op.getResult(i), newPortName,
                        newInstance.getResult(nextResult));
      ++nextResult;
    }
  }

  // Remember to remove the original op.
  opsToRemove.push_back(op);
}

/// Lower memory operations. A new memory is created for every leaf
/// element in a memory's data type.
void FIRRTLTypesLowering::visitDecl(MemOp op) {
  auto type = op.getDataType();
  auto depth = op.depth();

  SmallVector<FlatBundleFieldEntry, 8> fieldTypes;
  flattenType(type, "", false, fieldTypes);

  // Mutable store of the types of the ports of a new memory. This is
  // cleared and re-used.
  SmallVector<Type, 4> resultPortTypes;
  llvm::SmallSetVector<Attribute, 4> resultPortNames;

  // Insert a unique port into resultPortNames with base name nameStr.
  auto uniquePortName = [&](StringRef baseName) {
    size_t i = 0;
    std::string suffix = "";
    while (!resultPortNames.insert(
        builder->getStringAttr(baseName.str() + suffix)))
      suffix = std::to_string(i++);
  };

  // Store any new wires created during lowering. This ensures that
  // wires are re-used if they already exist.
  llvm::StringMap<Value> newWires;

  // Loop over the leaf aggregates.
  for (auto field : fieldTypes) {

    // Determine the new port type for this memory. New ports are
    // constructed by checking the kind of the memory.
    resultPortTypes.clear();
    resultPortNames.clear();
    for (size_t i = 0, e = op.getNumResults(); i != e; ++i) {
      auto kind = op.getPortKind(i).getValue();
      auto name = op.getPortName(i);

      // Any read or write ports are just added.
      if (kind != MemOp::PortKind::ReadWrite) {
        resultPortTypes.push_back(
            FlipType::get(op.getTypeForPort(depth, field.type, kind)));
        uniquePortName(name.getValue());
        continue;
      }

      // Any readwrite ports are lowered to 1x read and 1x write.
      resultPortTypes.push_back(FlipType::get(
          op.getTypeForPort(depth, field.type, MemOp::PortKind::Read)));
      resultPortTypes.push_back(FlipType::get(
          op.getTypeForPort(depth, field.type, MemOp::PortKind::Write)));

      auto nameStr = name.getValue().str();
      uniquePortName(nameStr + "_r");
      uniquePortName(nameStr + "_w");
    }

    // Construct the new memory for this flattened field.
    auto newName = op.name().getValue().str() + field.suffix;
    auto newMem = builder->create<MemOp>(
        resultPortTypes, op.readLatency(), op.writeLatency(), depth, op.ruw(),
        builder->getArrayAttr(resultPortNames.getArrayRef()),
        builder->getStringAttr(newName));

    // Setup the lowering to the new memory. We need to track both the
    // new memory index ("i") and the old memory index ("j") to deal
    // with the situation where readwrite ports have been split into
    // separate ports.
    for (size_t i = 0, j = 0, e = newMem.getNumResults(); i != e; ++i, ++j) {

      BundleType underlying = newMem.getResult(i)
                                  .getType()
                                  .cast<FIRRTLType>()
                                  .getPassiveType()
                                  .cast<BundleType>();

      auto kind =
          newMem.getPortKind(newMem.getPortName(i).getValue()).getValue();
      auto oldKind = op.getPortKind(op.getPortName(j).getValue()).getValue();

      auto skip = kind == MemOp::PortKind::Write &&
                  oldKind == MemOp::PortKind::ReadWrite;

      // Loop over all elements in the port. Because readwrite ports
      // have been split, this only needs to deal with the fields of
      // read or write ports. If the port is replacing a readwrite
      // port, then this is linked against the old field.
      for (auto elt : underlying.getElements()) {

        auto oldName = elt.name.str();
        if (oldKind == MemOp::PortKind::ReadWrite) {
          if (oldName == "mask")
            oldName = "wmask";
          if (oldName == "data" && kind == MemOp::PortKind::Read)
            oldName = "rdata";
          if (oldName == "data" && kind == MemOp::PortKind::Write)
            oldName = "wdata";
        }

        auto getWire = [&](FIRRTLType type, StringRef wireName) -> Value {
          auto wire = newWires[wireName];
          if (!wire) {
            wire = builder->create<WireOp>(type.getPassiveType(),
                                           newMem.name().getValue().str() +
                                               "_" + wireName.str());
            newWires[wireName] = wire;
          }
          return wire;
        };

        // These ports ("addr", "clk", "en") require special
        // handling. When these are lowered, they result in multiple
        // new connections. E.g., an assignment to a clock needs to be
        // split into an assignment to all clocks. This is handled by
        // creating a dummy wire, setting the dummy wire as the
        // lowering target, and then connecting every new port
        // subfield to that.
        if ((elt.name == "clk") || (elt.name == "en") || (elt.name == "addr")) {
          FIRRTLType theType = FlipType::get(elt.type);

          // Construct a new wire if needed.
          auto wireName = op.getPortName(j).getValue().str() + "_" + oldName;
          auto wire = getWire(theType, wireName);

          if (!(oldKind == MemOp::PortKind::ReadWrite &&
                kind == MemOp::PortKind::Write))
            setBundleLowering(op.getResult(j), oldName, wire);

          // Handle "en" specially if this used to be a readwrite port.
          if (oldKind == MemOp::PortKind::ReadWrite && elt.name == "en") {
            auto wmode =
                getWire(theType, op.getPortName(j).getValue().str() + "_wmode");
            if (!skip)
              setBundleLowering(op.getResult(j), "wmode", wmode);
            Value gate;
            if (kind == MemOp::PortKind::Read)
              gate = builder->create<NotPrimOp>(wmode.getType(), wmode);
            else
              gate = wmode;
            wire = builder->create<AndPrimOp>(wire.getType(), wire, gate);
          }

          builder->create<ConnectOp>(
              builder->create<SubfieldOp>(theType, newMem.getResult(i),
                                          elt.name),
              wire);
          continue;
        }

        // Data ports ("data", "mask") are trivially lowered because
        // each data leaf winds up in a new, separate memory. No wire
        // creation is needed.
        FIRRTLType theType = elt.type;
        if (kind == MemOp::PortKind::Write)
          theType = FlipType::get(theType);

        setBundleLowering(op.getResult(j), oldName + field.suffix,
                          builder->create<SubfieldOp>(
                              theType, newMem.getResult(i), elt.name));
      }

      // Don't increment the index of the old memory if this is the
      // first, new port (the read port).
      if (kind == MemOp::PortKind::Read &&
          oldKind == MemOp::PortKind::ReadWrite)
        --j;
    }
  }

  opsToRemove.push_back(op);
}

/// Lower a wire op with a bundle to mutliple non-bundled wires.
void FIRRTLTypesLowering::visitDecl(WireOp op) {
  Value result = op.result();

  // Attempt to get the bundle types, potentially unwrapping an outer flip type
  // that wraps the whole bundle.
  FIRRTLType resultType = getCanonicalAggregateType(result.getType());

  // If the wire is not a bundle, there is nothing to do.
  if (!resultType)
    return;

  SmallVector<FlatBundleFieldEntry, 8> fieldTypes;
  flattenType(resultType, "", false, fieldTypes);

  // Create the prefix of the lowered name
  StringRef wireName = "";
  if (op.name().hasValue())
    wireName = op.name().getValue();

  // Loop over the leaf aggregates.
  SmallString<16> loweredName(wireName);
  for (auto field : fieldTypes) {
    loweredName.resize(wireName.size());
    loweredName += field.suffix;
    auto wire = builder->create<WireOp>(field.getPortType(),
                                        builder->getStringAttr(loweredName));
    setBundleLowering(result, StringRef(field.suffix).drop_front(1), wire);
  }

  // Remember to remove the original op.
  opsToRemove.push_back(op);
}

/// Lower a reg op with a bundle to multiple non-bundled regs.
void FIRRTLTypesLowering::visitDecl(RegOp op) {
  Value result = op.result();

  // Attempt to get the bundle types, potentially unwrapping an outer flip type
  // that wraps the whole bundle.
  FIRRTLType resultType = getCanonicalAggregateType(result.getType());

  // If the reg is not a bundle, there is nothing to do.
  if (!resultType)
    return;

  SmallVector<FlatBundleFieldEntry, 8> fieldTypes;
  flattenType(resultType, "", false, fieldTypes);

  // Loop over the leaf aggregates.
  for (auto field : fieldTypes) {
    SmallString<16> loweredName(op.nameAttr().getValue());
    loweredName += field.suffix;
    setBundleLowering(
        result, StringRef(field.suffix).drop_front(1),
        builder->create<RegOp>(field.getPortType(), op.clockVal(),
                               builder->getStringAttr(loweredName)));
  }

  // Remember to remove the original op.
  opsToRemove.push_back(op);
}

// Lowering subfield operations has to deal with three different cases:
//   a) the input value is from a module block argument
//   b) the input value is from another subfield operation's result
//   c) the input value is from an instance
//   d) the input value is from a register
//
// This is accomplished by storing value and suffix mappings that point to the
// flattened value. If the subfield op is accessing the leaf field of a bundle,
// it replaces all uses with the flattened value. Otherwise, it flattens the
// rest of the bundle and adds the flattened values to the mapping for each
// partial suffix.
void FIRRTLTypesLowering::visitExpr(SubfieldOp op) {
  Value input = op.input();
  StringRef fieldname = op.fieldname();
  FIRRTLType resultType = op.getType();

  // Flatten any nested bundle types the usual way.
  SmallVector<FlatBundleFieldEntry, 8> fieldTypes;
  flattenType(resultType, fieldname, false, fieldTypes);

  for (auto field : fieldTypes) {
    // Look up the mapping for this suffix.
    auto newValue = getBundleLowering(input, field.suffix);

    // The prefix is the field name and possibly field separator.
    auto prefixSize = fieldname.size();
    if (field.suffix.size() > fieldname.size())
      prefixSize += 1;

    // Get the remaining field suffix by removing the prefix.
    auto partialSuffix = StringRef(field.suffix).drop_front(prefixSize);

    // If we are at the leaf of a bundle.
    if (partialSuffix.empty())
      // Replace the result with the flattened value.
      op.replaceAllUsesWith(newValue);
    else
      // Map the partial suffix for the result value to the flattened value.
      setBundleLowering(op, partialSuffix, newValue);
  }

  // Remember to remove the original op.
  opsToRemove.push_back(op);
}

// This is currently the same lowering as SubfieldOp, but using a fieldname
// derived from the fixed index.
//
// TODO: Unify this and SubfieldOp handling.
void FIRRTLTypesLowering::visitExpr(SubindexOp op) {
  Value input = op.input();
  std::string fieldname = std::to_string(op.index());
  FIRRTLType resultType = op.getType();

  // Flatten any nested bundle types the usual way.
  SmallVector<FlatBundleFieldEntry, 8> fieldTypes;
  flattenType(resultType, fieldname, false, fieldTypes);

  for (auto field : fieldTypes) {
    // Look up the mapping for this suffix.
    auto newValue = getBundleLowering(input, field.suffix);

    // The prefix is the field name and possibly field separator.
    auto prefixSize = fieldname.size();
    if (field.suffix.size() > fieldname.size())
      prefixSize += 1;

    // Get the remaining field suffix by removing the prefix.
    auto partialSuffix = StringRef(field.suffix).drop_front(prefixSize);

    // If we are at the leaf of a bundle.
    if (partialSuffix.empty())
      // Replace the result with the flattened value.
      op.replaceAllUsesWith(newValue);
    else
      // Map the partial suffix for the result value to the flattened value.
      setBundleLowering(op, partialSuffix, newValue);
  }

  // Remember to remove the original op.
  opsToRemove.push_back(op);
}

// Lowering connects only has to deal with one special case: connecting two
// bundles. This situation should only arise when both of the arguments are a
// bundle that was:
//   a) originally a block argument
//   b) originally an instance's port
//
// When two such bundles are connected, none of the subfield visits have a
// chance to lower them, so we must ensure they have the same number of
// flattened values and flatten out this connect into multiple connects.
void FIRRTLTypesLowering::visitStmt(ConnectOp op) {
  Value dest = op.dest();
  Value src = op.src();

  // Attempt to get the bundle types, potentially unwrapping an outer flip type
  // that wraps the whole bundle.
  FIRRTLType destType = getCanonicalAggregateType(dest.getType());
  FIRRTLType srcType = getCanonicalAggregateType(src.getType());

  // If we aren't connecting two bundles, there is nothing to do.
  if (!destType || !srcType)
    return;

  // Get the lowered values for each side.
  SmallVector<Value, 8> destValues;
  getAllBundleLowerings(dest, destValues);

  SmallVector<Value, 8> srcValues;
  getAllBundleLowerings(src, srcValues);

  // Check that we got out the same number of values from each bundle.
  assert(destValues.size() == srcValues.size() &&
         "connected bundles don't match");

  for (auto tuple : llvm::zip_first(destValues, srcValues)) {
    Value newDest = std::get<0>(tuple);
    Value newSrc = std::get<1>(tuple);
    if (newDest.getType().isa<FlipType>())
      builder->create<ConnectOp>(newDest, newSrc);
    else
      builder->create<ConnectOp>(newSrc, newDest);
  }

  // Remember to remove the original op.
  opsToRemove.push_back(op);
}

// Lowering invalid may need to create a new invalid for each field
void FIRRTLTypesLowering::visitExpr(InvalidValuePrimOp op) {
  Value result = op.result();

  // Attempt to get the bundle types, potentially unwrapping an outer flip type
  // that wraps the whole bundle.
  FIRRTLType resultType = getCanonicalAggregateType(result.getType());

  // If we aren't connecting two bundles, there is nothing to do.
  if (!resultType)
    return;

  SmallVector<FlatBundleFieldEntry, 8> fieldTypes;
  flattenType(resultType, "", false, fieldTypes);

  // Loop over the leaf aggregates.
  for (auto field : fieldTypes) {
    setBundleLowering(result, StringRef(field.suffix).drop_front(1),
                      builder->create<InvalidValuePrimOp>(field.getPortType()));
  }

  // Remember to remove the original op.
  opsToRemove.push_back(op);
}

//===----------------------------------------------------------------------===//
// Helpers to manage state.
//===----------------------------------------------------------------------===//

static DictionaryAttr getArgAttrs(StringAttr nameAttr, StringRef suffix,
                                  OpBuilder *builder) {
  SmallString<16> newName(nameAttr.getValue());
  newName += suffix;

  StringAttr newNameAttr = builder->getStringAttr(newName);
  auto attr = builder->getNamedAttr("firrtl.name", newNameAttr);

  return builder->getDictionaryAttr(attr);
}

// Creates and returns a new block argument of the specified type to the module.
// This also maintains the name attribute for the new argument, possibly with a
// new suffix appended.
Value FIRRTLTypesLowering::addArg(Type type, unsigned oldArgNumber,
                                  StringRef nameSuffix) {
  FModuleOp module = getOperation();
  Block *body = module.getBodyBlock();

  // Append the new argument.
  auto newValue = body->addArgument(type);

  // Save the name attribute for the new argument.
  StringAttr nameAttr = getFIRRTLNameAttr(module.getArgAttrs(oldArgNumber));
  if (nameAttr) {
    auto newArgAttrs = getArgAttrs(nameAttr, nameSuffix, builder);
    auto newArgNumber = newValue.getArgNumber() - originalNumArgs;
    auto newArgAttrName = getArgAttrName(newArgNumber);
    newAttrs.push_back(NamedAttribute(newArgAttrName, newArgAttrs));
  }

  return newValue;
}

// Store the mapping from a bundle typed value to a mapping from its field names
// to flat values.
void FIRRTLTypesLowering::setBundleLowering(Value oldValue, StringRef flatField,
                                            Value newValue) {
  auto flatFieldId = builder->getIdentifier(flatField);
  auto &entry = loweredBundleValues[ValueIdentifier(oldValue, flatFieldId)];
  if (entry == newValue)
    return;
  assert(!entry && "bundle lowering has already been set");
  entry = newValue;
}

// For a mapped bundle typed value and a flat subfield name, retrieve and return
// the flat value if it exists.
Value FIRRTLTypesLowering::getBundleLowering(Value oldValue,
                                             StringRef flatField) {
  auto flatFieldId = builder->getIdentifier(flatField);
  auto &entry = loweredBundleValues[ValueIdentifier(oldValue, flatFieldId)];
  assert(entry && "bundle lowering was not set");
  return entry;
}

// For a mapped aggregate typed value, retrieve and return the flat values for
// each field.
void FIRRTLTypesLowering::getAllBundleLowerings(
    Value value, SmallVectorImpl<Value> &results) {

  TypeSwitch<FIRRTLType>(getCanonicalAggregateType(value.getType()))
      .Case<BundleType, FVectorType>([&](auto aggregateType) {
        // Flatten the original value's bundle type.
        SmallVector<FlatBundleFieldEntry, 8> fieldTypes;
        flattenType(aggregateType, "", false, fieldTypes);

        for (auto element : fieldTypes) {
          // Remove the field separator prefix.
          auto name = StringRef(element.suffix).drop_front(1);

          // Store the resulting lowering for this flat value.
          results.push_back(getBundleLowering(value, name));
        }
      })
      .Default([&](auto) {});
}

Identifier FIRRTLTypesLowering::getArgAttrName(unsigned newArgNumber) {
  SmallString<16> argNameBuffer;
  mlir::impl::getArgAttrName(newArgNumber, argNameBuffer);
  return Identifier::get(argNameBuffer, &getContext());
}
