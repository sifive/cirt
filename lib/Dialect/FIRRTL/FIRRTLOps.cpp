//===- FIRRTLOps.cpp - Implement the FIRRTL operations --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implement the FIRRTL ops.
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "circt/Dialect/FIRRTL/FIRRTLTypes.h"
#include "circt/Dialect/FIRRTL/FIRRTLVisitors.h"
#include "circt/Dialect/HW/HWTypes.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/DialectImplementation.h"
#include "mlir/IR/FunctionImplementation.h"
#include "mlir/IR/PatternMatch.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/TypeSwitch.h"

using mlir::RegionRange;
using namespace circt;
using namespace firrtl;

/// Remove elements at the specified indices from the input array, returning the
/// elements not mentioned.  The indices array is expected to be sorted and
/// unique.
template <typename T>
static SmallVector<T>
removeElementsAtIndices(ArrayRef<T> input, ArrayRef<unsigned> indicesToDrop) {
#ifndef NDEBUG // Check sortedness.
  if (!input.empty()) {
    for (size_t i = 1, e = indicesToDrop.size(); i != e; ++i)
      assert(indicesToDrop[i - 1] < indicesToDrop[i] &&
             "indicesToDrop isn't sorted and unique");
    assert(indicesToDrop.back() < input.size() && "index out of range");
  }
#endif

  // Copy over the live chunks.
  size_t lastCopied = 0;
  SmallVector<T> result;
  result.reserve(input.size() - indicesToDrop.size());

  for (unsigned indexToDrop : indicesToDrop) {
    // If we skipped over some valid elements, copy them over.
    if (indexToDrop > lastCopied) {
      result.append(input.begin() + lastCopied, input.begin() + indexToDrop);
      lastCopied = indexToDrop;
    }
    // Ignore this value so we don't copy it in the next iteration.
    ++lastCopied;
  }

  // If there are live elements at the end, copy them over.
  if (lastCopied < input.size())
    result.append(input.begin() + lastCopied, input.end());

  return result;
}

bool firrtl::isDuplexValue(Value val) {
  Operation *op = val.getDefiningOp();
  // Block arguments are not duplex values.
  if (!op)
    return false;
  return TypeSwitch<Operation *, bool>(op)
      .Case<SubfieldOp, SubindexOp, SubaccessOp>(
          [](auto op) { return isDuplexValue(op.input()); })
      .Case<RegOp, RegResetOp, WireOp>([](auto) { return true; })
      .Default([](auto) { return false; });
}

Flow firrtl::swapFlow(Flow flow) {
  switch (flow) {
  case Flow::Source:
    return Flow::Sink;
  case Flow::Sink:
    return Flow::Source;
  case Flow::Duplex:
    return Flow::Duplex;
  }
  llvm_unreachable("invalid flow");
}

Flow firrtl::foldFlow(Value val, Flow accumulatedFlow) {
  auto swap = [&accumulatedFlow]() -> Flow {
    return swapFlow(accumulatedFlow);
  };

  if (auto blockArg = val.dyn_cast<BlockArgument>()) {
    auto op = val.getParentBlock()->getParentOp();
    auto direction = (Direction)getModulePortDirections(op)
                         .getValue()[blockArg.getArgNumber()];
    if (direction == Direction::Output)
      return swap();
    return accumulatedFlow;
  }

  Operation *op = val.getDefiningOp();

  return TypeSwitch<Operation *, Flow>(op)
      .Case<SubfieldOp>([&](auto op) {
        return foldFlow(op.input(),
                        op.isFieldFlipped() ? swap() : accumulatedFlow);
      })
      .Case<SubindexOp, SubaccessOp>(
          [&](auto op) { return foldFlow(op.input(), accumulatedFlow); })
      // Registers, Wires, and behavioral memory ports are always Duplex.
      .Case<RegOp, RegResetOp, WireOp, MemoryPortOp>(
          [](auto) { return Flow::Duplex; })
      .Case<InstanceOp>([&](auto) {
        return val.getType().isa<FlipType>() ? swap() : accumulatedFlow;
      })
      .Case<MemOp>([&](auto op) { return swap(); })
      // Anything else acts like a universal source.
      .Default([&](auto) { return accumulatedFlow; });
}

// TODO: This is doing the same walk as foldFlow.  These two functions can be
// combined and return a (flow, kind) product.
DeclKind firrtl::getDeclarationKind(Value val) {
  Operation *op = val.getDefiningOp();
  if (!op)
    return DeclKind::Port;

  return TypeSwitch<Operation *, DeclKind>(op)
      .Case<InstanceOp>([](auto) { return DeclKind::Instance; })
      .Case<SubfieldOp, SubindexOp, SubaccessOp>(
          [](auto op) { return getDeclarationKind(op.input()); })
      .Default([](auto) { return DeclKind::Other; });
}

//===----------------------------------------------------------------------===//
// CircuitOp
//===----------------------------------------------------------------------===//

void CircuitOp::build(OpBuilder &builder, OperationState &result,
                      StringAttr name, ArrayAttr annotations) {
  // Add an attribute for the name.
  result.addAttribute(builder.getIdentifier("name"), name);

  if (annotations)
    result.addAttribute("annotations", annotations);

  // Create a region and a block for the body.
  Region *bodyRegion = result.addRegion();
  Block *body = new Block();
  bodyRegion->push_back(body);
}

// Return the main module that is the entry point of the circuit.
Operation *CircuitOp::getMainModule() { return lookupSymbol(name()); }

static LogicalResult verifyCircuitOp(CircuitOp circuit) {
  StringRef main = circuit.name();

  // Check that the circuit has a non-empty name.
  if (main.empty()) {
    circuit.emitOpError("must have a non-empty name");
    return failure();
  }

  // Check that a module matching the "main" module exists in the circuit.
  if (!circuit.getMainModule()) {
    circuit.emitOpError("must contain one module that matches main name '" +
                        main + "'");
    return failure();
  }

  // Store a mapping of defname to either the first external module
  // that defines it or, preferentially, the first external module
  // that defines it and has no parameters.
  llvm::DenseMap<Attribute, FExtModuleOp> defnameMap;

  // Verify external modules.
  for (auto &op : *circuit.getBody()) {
    auto extModule = dyn_cast<FExtModuleOp>(op);
    if (!extModule)
      continue;

    auto defname = extModule.defnameAttr();
    if (!defname)
      continue;

    // Check that this extmodule's defname does not conflict with
    // the symbol name of any module.
    auto collidingModule = circuit.lookupSymbol(defname.getValue());
    if (isa_and_nonnull<FModuleOp>(collidingModule)) {
      auto diag =
          op.emitOpError()
          << "attribute 'defname' with value " << defname
          << " conflicts with the name of another module in the circuit";
      diag.attachNote(collidingModule->getLoc())
          << "previous module declared here";
      return failure();
    }

    // Find an optional extmodule with a defname collision. Update
    // the defnameMap if this is the first extmodule with that
    // defname or if the current extmodule takes no parameters and
    // the collision does. The latter condition improves later
    // extmodule verification as checking against a parameterless
    // module is stricter.
    FExtModuleOp collidingExtModule;
    if (auto &value = defnameMap[defname]) {
      collidingExtModule = value;
      if (value.parameters() && !extModule.parameters())
        value = extModule;
    } else {
      value = extModule;
      // Go to the next extmodule if no extmodule with the same
      // defname was found.
      continue;
    }

    // Check that the number of ports is exactly the same.
    SmallVector<ModulePortInfo> ports = extModule.getPorts();
    SmallVector<ModulePortInfo> collidingPorts = collidingExtModule.getPorts();

    if (ports.size() != collidingPorts.size()) {
      auto diag = op.emitOpError()
                  << "with 'defname' attribute " << defname << " has "
                  << ports.size()
                  << " ports which is different from a previously defined "
                     "extmodule with the same 'defname' which has "
                  << collidingPorts.size() << " ports";
      diag.attachNote(collidingExtModule.getLoc())
          << "previous extmodule definition occurred here";
      return failure();
    }

    // Check that ports match for name and type. Since parameters
    // *might* affect widths, ignore widths if either module has
    // parameters. Note that this allows for misdetections, but
    // has zero false positives.
    for (auto p : llvm::zip(ports, collidingPorts)) {
      StringAttr aName = std::get<0>(p).name, bName = std::get<1>(p).name;
      FIRRTLType aType = std::get<0>(p).type, bType = std::get<1>(p).type;

      if (extModule.parameters() || collidingExtModule.parameters()) {
        aType = aType.getWidthlessType();
        bType = bType.getWidthlessType();
      }
      if (aName != bName) {
        auto diag = op.emitOpError()
                    << "with 'defname' attribute " << defname
                    << " has a port with name " << aName
                    << " which does not match the name of the port "
                    << "in the same position of a previously defined "
                    << "extmodule with the same 'defname', expected port "
                       "to have name "
                    << bName;
        diag.attachNote(collidingExtModule.getLoc())
            << "previous extmodule definition occurred here";
        return failure();
      }
      if (aType != bType) {
        auto diag = op.emitOpError()
                    << "with 'defname' attribute " << defname
                    << " has a port with name " << aName
                    << " which has a different type " << aType
                    << " which does not match the type of the port in "
                       "the same position of a previously defined "
                       "extmodule with the same 'defname', expected port "
                       "to have type "
                    << bType;
        diag.attachNote(collidingExtModule.getLoc())
            << "previous extmodule definition occurred here";
        return failure();
      }
    }
  }

  return success();
}

Region &CircuitOp::getBodyRegion() { return getOperation()->getRegion(0); }
Block *CircuitOp::getBody() { return &getBodyRegion().front(); }

//===----------------------------------------------------------------------===//
// FExtModuleOp and FModuleOp
//===----------------------------------------------------------------------===//

FunctionType firrtl::getModuleType(Operation *op) {
  auto typeAttr = op->getAttrOfType<TypeAttr>(FModuleOp::getTypeAttrName());
  return typeAttr.getValue().cast<FunctionType>();
}

/// This function can extract information about ports from a module and an
/// extmodule.
SmallVector<ModulePortInfo> firrtl::getModulePortInfo(Operation *op) {
  SmallVector<ModulePortInfo> results;
  auto argTypes = getModuleType(op).getInputs();

  auto portNamesAttr = getModulePortNames(op);
  auto portDirections = getModulePortDirections(op).getValue();
  auto portAnnotations = getModulePortAnnotations(op);
  for (unsigned i = 0, e = argTypes.size(); i < e; ++i) {
    auto type = argTypes[i].cast<FIRRTLType>();
    auto direction = direction::get(portDirections[i]);
    results.push_back({portNamesAttr[i].cast<StringAttr>(), type, direction,
                       portAnnotations[i]});
  }
  return results;
}

/// Given an FModule or ExtModule, return the name of the specified port number.
StringAttr firrtl::getModulePortName(Operation *op, size_t portIndex) {
  assert(isa<FModuleOp>(op) || isa<FExtModuleOp>(op));
  return getModulePortNames(op)[portIndex].cast<StringAttr>();
}

Direction firrtl::getModulePortDirection(Operation *op, size_t portIndex) {
  assert(isa<FModuleOp>(op) || isa<FExtModuleOp>(op));
  return direction::get(getModulePortDirections(op).getValue()[portIndex]);
}

ArrayAttr getModulePortAnnotation(Operation *op, size_t portIndex) {
  assert(isa<FModuleOp>(op) || isa<FExtModuleOp>(op));
  return getModulePortAnnotations(op)[portIndex].cast<ArrayAttr>();
}

// Return the port with the specified name.
BlockArgument FModuleOp::getPortArgument(size_t portNumber) {
  return getBodyBlock()->getArgument(portNumber);
}

/// Erases the ports listed in `portIndices`.  `portIndices` is expected to
/// be in order and unique.
void FModuleOp::erasePorts(ArrayRef<unsigned> portIndices) {
  if (portIndices.empty())
    return;

  // Drop the direction markers for dead ports.
  SmallVector<Direction> directions = direction::unpackAttribute(*this);
  ArrayRef<Attribute> portNames = this->portNames().getValue();
  assert(directions.size() == portNames.size());

  SmallVector<Direction> newDirections =
      removeElementsAtIndices<Direction>(directions, portIndices);
  SmallVector<Attribute> newPortNames =
      removeElementsAtIndices(portNames, portIndices);
  (*this)->setAttr(direction::attrKey,
                   direction::packAttribute(newDirections, getContext()));
  (*this)->setAttr("portNames", ArrayAttr::get(getContext(), newPortNames));

  // Erase the common function-like stuff, including the block arguments, and
  // argument attributes (incl port annotations).
  eraseArguments(portIndices);
}

static void buildModule(OpBuilder &builder, OperationState &result,
                        StringAttr name, ArrayRef<ModulePortInfo> ports) {
  using namespace mlir::function_like_impl;

  // Add an attribute for the name.
  result.addAttribute(::mlir::SymbolTable::getSymbolAttrName(), name);

  SmallVector<Type, 4> argTypes;
  for (auto elt : ports)
    argTypes.push_back(elt.type);

  // Record the argument and result types as an attribute.
  auto type = builder.getFunctionType(argTypes, /*resultTypes*/ {});
  result.addAttribute(getTypeAttrName(), TypeAttr::get(type));

  // Record the names of the arguments if present.
  SmallVector<Attribute, 4> argAttrs;
  SmallVector<Attribute, 4> portNames;
  SmallVector<Direction, 4> portDirections;
  for (size_t i = 0, e = ports.size(); i != e; ++i) {
    portNames.push_back(ports[i].name);
    portDirections.push_back(ports[i].direction);
    argAttrs.push_back(ports[i].annotations
                           ? builder.getDictionaryAttr(
                                 {{builder.getIdentifier("firrtl.annotations"),
                                   ports[i].annotations}})
                           : builder.getDictionaryAttr({}));
  }

  // Both attributes are added, even if the module has no ports.
  result.addAttribute(getArgDictAttrName(), builder.getArrayAttr(argAttrs));
  result.addAttribute("portNames", builder.getArrayAttr(portNames));
  result.addAttribute(
      direction::attrKey,
      direction::packAttribute(portDirections, builder.getContext()));

  result.addRegion();
}

void FModuleOp::build(OpBuilder &builder, OperationState &result,
                      StringAttr name, ArrayRef<ModulePortInfo> ports,
                      ArrayAttr annotations) {
  buildModule(builder, result, name, ports);

  // Create a region and a block for the body.
  auto *bodyRegion = result.regions[0].get();
  Block *body = new Block();
  bodyRegion->push_back(body);

  // Add arguments to the body block.
  for (auto elt : ports)
    body->addArgument(elt.type);

  if (annotations)
    result.addAttribute("annotations", annotations);
}

void FExtModuleOp::build(OpBuilder &builder, OperationState &result,
                         StringAttr name, ArrayRef<ModulePortInfo> ports,
                         StringRef defnameAttr, ArrayAttr annotations) {
  buildModule(builder, result, name, ports);
  if (!defnameAttr.empty())
    result.addAttribute("defname", builder.getStringAttr(defnameAttr));

  if (annotations)
    result.addAttribute("annotations", annotations);
}

// TODO: This ia a clone of mlir::impl::printFunctionSignature, refactor it to
// allow this customization.
static void printFunctionSignature2(OpAsmPrinter &p, Operation *op,
                                    ArrayRef<Type> argTypes, bool isVariadic,
                                    ArrayRef<Type> resultTypes,
                                    bool &needportNamesAttr, APInt directions) {
  Region &body = op->getRegion(0);
  bool isExternal = body.empty();
  SmallString<32> resultNameStr;

  p << '(';
  auto portNamesAttr = getModulePortNames(op);
  for (unsigned i = 0, e = argTypes.size(); i < e; ++i) {
    if (i > 0)
      p << ", ";

    p << (directions[i] ? "out " : "in ");

    auto portName = portNamesAttr[i].cast<StringAttr>().getValue();
    Value argumentValue;
    if (!isExternal) {
      // Get the printed format for the argument name.
      resultNameStr.clear();
      llvm::raw_svector_ostream tmpStream(resultNameStr);
      p.printOperand(body.front().getArgument(i), tmpStream);
      // If the name wasn't printable in a way that agreed with portName, make
      // sure to print out an explicit portNames attribute.
      if (!portName.empty() && tmpStream.str().drop_front() != portName)
        needportNamesAttr = true;
      p << tmpStream.str() << ": ";
    } else if (!portName.empty()) {
      p << '%' << portName << ": ";
    }

    p.printType(argTypes[i]);
    p.printOptionalAttrDict(::mlir::function_like_impl::getArgAttrs(op, i));
  }

  if (isVariadic) {
    if (!argTypes.empty())
      p << ", ";
    p << "...";
  }

  p << ')';
}

static ParseResult parseFunctionArgumentList2(
    OpAsmParser &parser, bool allowAttributes, bool allowVariadic,
    SmallVectorImpl<OpAsmParser::OperandType> &argNames,
    SmallVectorImpl<Type> &argTypes, SmallVectorImpl<Direction> &argDirections,
    SmallVectorImpl<NamedAttrList> &argAttrs, bool &isVariadic) {
  if (parser.parseLParen())
    return failure();

  // The argument list either has to consistently have ssa-id's followed by
  // types, or just be a type list.  It isn't ok to sometimes have SSA ID's and
  // sometimes not.
  auto parseArgument = [&]() -> ParseResult {
    llvm::SMLoc loc = parser.getCurrentLocation();

    // Parse argument name if present.
    OpAsmParser::OperandType argument;
    Type argumentType;
    // TODO: is this safe?
    SmallVector<StringRef, 2> directions({{"in"}, {"out"}});
    StringRef direction;
    if (succeeded(parser.parseOptionalKeyword(&direction, directions)) &&
        succeeded(parser.parseOptionalRegionArgument(argument)) &&
        !argument.name.empty()) {
      // Reject this if the preceding argument was missing a name.
      if (argNames.empty() && !argTypes.empty())
        return parser.emitError(loc, "expected type instead of SSA identifier");
      argNames.push_back(argument);
      argDirections.push_back(direction::get(direction == "out"));

      if (parser.parseColonType(argumentType))
        return failure();
    } else if (allowVariadic && succeeded(parser.parseOptionalEllipsis())) {
      isVariadic = true;
      return success();
    } else if (!argNames.empty()) {
      // Reject this if the preceding argument had a name.
      return parser.emitError(loc, "expected SSA identifier");
    } else if (parser.parseType(argumentType)) {
      return failure();
    }

    // Add the argument type.
    argTypes.push_back(argumentType);

    // Parse any argument attributes.
    NamedAttrList attrs;
    if (parser.parseOptionalAttrDict(attrs))
      return failure();
    if (!allowAttributes && !attrs.empty())
      return parser.emitError(loc, "expected arguments without attributes");
    argAttrs.push_back(attrs);
    return success();
  };

  // Parse the function arguments.
  isVariadic = false;
  if (failed(parser.parseOptionalRParen())) {
    do {
      unsigned numTypedArguments = argTypes.size();
      if (parseArgument())
        return failure();

      llvm::SMLoc loc = parser.getCurrentLocation();
      if (argTypes.size() == numTypedArguments &&
          succeeded(parser.parseOptionalComma()))
        return parser.emitError(
            loc, "variadic arguments must be in the end of the argument list");
    } while (succeeded(parser.parseOptionalComma()));
    parser.parseRParen();
  }

  return success();
}

static ParseResult
parseFunctionResultList2(OpAsmParser &parser,
                         SmallVectorImpl<Type> &resultTypes,
                         SmallVectorImpl<NamedAttrList> &resultAttrs) {
  if (failed(parser.parseOptionalLParen())) {
    // We already know that there is no `(`, so parse a type.
    // Because there is no `(`, it cannot be a function type.
    Type ty;
    if (parser.parseType(ty))
      return failure();
    resultTypes.push_back(ty);
    resultAttrs.emplace_back();
    return success();
  }

  // Special case for an empty set of parens.
  if (succeeded(parser.parseOptionalRParen()))
    return success();

  // Parse individual function results.
  do {
    resultTypes.emplace_back();
    resultAttrs.emplace_back();
    if (parser.parseType(resultTypes.back()) ||
        parser.parseOptionalAttrDict(resultAttrs.back())) {
      return failure();
    }
  } while (succeeded(parser.parseOptionalComma()));
  return parser.parseRParen();
}

static ParseResult
parseFunctionSignature2(OpAsmParser &parser, bool allowVariadic,
                        SmallVectorImpl<OpAsmParser::OperandType> &argNames,
                        SmallVectorImpl<Type> &argTypes,
                        SmallVectorImpl<Direction> &argDirections,
                        SmallVectorImpl<NamedAttrList> &argAttrs,
                        bool &isVariadic, SmallVectorImpl<Type> &resultTypes,
                        SmallVectorImpl<NamedAttrList> &resultAttrs) {
  bool allowArgAttrs = true;
  if (parseFunctionArgumentList2(parser, allowArgAttrs, allowVariadic, argNames,
                                 argTypes, argDirections, argAttrs, isVariadic))
    return failure();
  if (succeeded(parser.parseOptionalArrow()))
    return parseFunctionResultList2(parser, resultTypes, resultAttrs);
  return success();
}

static void printModuleLikeOp(OpAsmPrinter &p, Operation *op) {
  using namespace mlir::function_like_impl;

  FunctionType fnType = getModuleType(op);
  auto argTypes = fnType.getInputs();
  auto resultTypes = fnType.getResults();

  // TODO: Should refactor mlir::function_like_impl::printFunctionLikeOp to
  // allow these customizations.  Need to not print the terminator.

  // Print the operation and the function name.
  auto funcName =
      op->getAttrOfType<StringAttr>(::mlir::SymbolTable::getSymbolAttrName())
          .getValue();
  p << op->getName() << ' ';
  p.printSymbolName(funcName);

  bool needportNamesAttr = false;
  printFunctionSignature2(p, op, argTypes, /*isVariadic*/ false, resultTypes,
                          needportNamesAttr,
                          getModulePortDirections(op).getValue());
  SmallVector<StringRef, 3> omittedAttrs({direction::attrKey});
  if (!needportNamesAttr)
    omittedAttrs.push_back("portNames");
  printFunctionAttributes(p, op, argTypes.size(), resultTypes.size(),
                          omittedAttrs);
}

static void print(OpAsmPrinter &p, FExtModuleOp op) {
  printModuleLikeOp(p, op);
}

static void print(OpAsmPrinter &p, FModuleOp op) {
  printModuleLikeOp(p, op);

  // Print the body if this is not an external function. Since this block does
  // not have terminators, printing the terminator actually just prints the last
  // operation.
  Region &body = op.getBody();
  if (!body.empty())
    p.printRegion(body, /*printEntryBlockArgs=*/false,
                  /*printBlockTerminators=*/true);
}

static ParseResult parseFModuleOp(OpAsmParser &parser, OperationState &result,
                                  bool isExtModule = false) {
  using namespace mlir::function_like_impl;

  // TODO: Should refactor mlir::function_like_impl::parseFunctionLikeOp to
  // allow these customizations for implicit argument names.  Need to not print
  // the terminator.

  SmallVector<OpAsmParser::OperandType, 4> entryArgs;
  SmallVector<NamedAttrList, 4> portNamesAttrs;
  SmallVector<NamedAttrList, 4> resultAttrs;
  SmallVector<Type, 4> argTypes;
  SmallVector<Type, 4> resultTypes;
  SmallVector<Direction, 4> argDirections;
  auto &builder = parser.getBuilder();

  // Parse the name as a symbol.
  StringAttr nameAttr;
  if (parser.parseSymbolName(nameAttr, ::mlir::SymbolTable::getSymbolAttrName(),
                             result.attributes))
    return failure();

  // Parse the function signature.
  bool isVariadic = false;
  if (parseFunctionSignature2(parser, /*allowVariadic*/ false, entryArgs,
                              argTypes, argDirections, portNamesAttrs,
                              isVariadic, resultTypes, resultAttrs))
    return failure();

  // Record the argument and result types as an attribute.  This is necessary
  // for external modules.
  auto type = builder.getFunctionType(argTypes, resultTypes);
  result.addAttribute(getTypeAttrName(), TypeAttr::get(type));

  // If function attributes are present, parse them.
  if (parser.parseOptionalAttrDictWithKeyword(result.attributes))
    return failure();

  assert(portNamesAttrs.size() == argTypes.size());
  assert(resultAttrs.size() == resultTypes.size());

  auto *context = result.getContext();

  // Add the port directions attribute indiciating which port is.
  result.addAttribute(direction::attrKey,
                      direction::packAttribute(argDirections, context));

  SmallVector<Attribute> portNames;
  if (!result.attributes.get("portNames")) {
    // Postprocess each of the arguments.  If there was no portNames
    // attribute, and if the argument name was non-numeric, then add the
    // portNames attribute with the textual name from the IR.  The name in the
    // text file is a load-bearing part of the IR, but we don't want the
    // verbosity in dumps of including it explicitly in the attribute
    // dictionary.
    for (size_t i = 0, e = entryArgs.size(); i != e; ++i) {
      auto &arg = entryArgs[i];

      // The name of an argument is of the form "%42" or "%id", and since
      // parsing succeeded, we know it always has one character.
      assert(arg.name.size() > 1 && arg.name[0] == '%' && "Unknown MLIR name");
      if (isdigit(arg.name[1]))
        portNames.push_back(StringAttr::get(context, ""));
      else
        portNames.push_back(StringAttr::get(context, arg.name.drop_front()));
    }
    result.addAttribute("portNames", builder.getArrayAttr(portNames));
  }
  // Add the attributes to the function arguments.
  addArgAndResultAttrs(builder, result, portNamesAttrs, resultAttrs);

  // Parse the optional function body.
  auto *body = result.addRegion();
  if (!isExtModule) {
    if (parser.parseRegion(*body, entryArgs,
                           entryArgs.empty() ? ArrayRef<Type>() : argTypes))
      return failure();
    if (body->empty())
      body->push_back(new Block());
  }
  return success();
}

static ParseResult parseFExtModuleOp(OpAsmParser &parser,
                                     OperationState &result) {
  return parseFModuleOp(parser, result, /*isExtModule:*/ true);
}

static LogicalResult verifyModuleSignature(Operation *op) {
  for (auto argType : getModuleType(op).getInputs()) {
    if (!argType.isa<FIRRTLType>())
      return op->emitOpError("all module ports must be firrtl types");
    if (argType.isa<FlipType>())
      return op->emitOpError("module ports should not contain an outer flip");
  }
  return success();
}

static LogicalResult verifyFModuleOp(FModuleOp op) {
  // Verify the module signature.
  return verifyModuleSignature(op);
}

static LogicalResult verifyFExtModuleOp(FExtModuleOp op) {
  // Verify the module signature.
  if (failed(verifyModuleSignature(op)))
    return failure();

  auto paramDictOpt = op.parameters();
  if (!paramDictOpt)
    return success();

  DictionaryAttr paramDict = paramDictOpt.getValue();
  auto checkParmValue = [&](NamedAttribute elt) -> bool {
    auto value = elt.second;
    if (value.isa<IntegerAttr>() || value.isa<StringAttr>() ||
        value.isa<FloatAttr>())
      return true;
    op.emitError() << "has unknown extmodule parameter value '" << elt.first
                   << "' = " << value;
    return false;
  };

  if (!llvm::all_of(paramDict, checkParmValue))
    return failure();
  auto portNamesAttr = getModulePortNames(op);

  auto numPorts = op.getPorts().size();
  if (numPorts != portNamesAttr.size())
    return op.emitError("module ports does not match number of arguments");

  // Directions are stored in an APInt which cannot have zero bitwidth.  If the
  // module has no ports, then the APInt should be size one.  Otherwise, their
  // sizes should match.
  auto numDirections = getModulePortDirections(op).getValue().getBitWidth();
  if ((numPorts != numDirections) && (numPorts != 0 || numDirections != 1))
    return op.emitError()
           << "module ports size (" << numPorts
           << ") does not match number of bits in port direction ("
           << numDirections << ")";

  return success();
}

//===----------------------------------------------------------------------===//
// Declarations
//===----------------------------------------------------------------------===//

/// Lookup the module or extmodule for the symbol.  This returns null on
/// invalid IR.
Operation *InstanceOp::getReferencedModule() {
  auto circuit = (*this)->getParentOfType<CircuitOp>();
  if (!circuit)
    return nullptr;

  return circuit.lookupSymbol(moduleName());
}

/// Create a copy of the specified instance operation with some result removed.
void InstanceOp::build(OpBuilder &builder, OperationState &result,
                       InstanceOp existingInstance,
                       ArrayRef<unsigned> resultsToErase) {

  // Drop the direction markers for dead ports.
  auto resultTypes = SmallVector<Type>(existingInstance.getResultTypes());

  SmallVector<Type> newResultTypes =
      removeElementsAtIndices<Type>(resultTypes, resultsToErase);

  build(builder, result, newResultTypes, existingInstance.moduleNameAttr(),
        existingInstance.nameAttr(), existingInstance.annotationsAttr());
}

/// Verify the correctness of an InstanceOp.
static LogicalResult verifyInstanceOp(InstanceOp instance) {

  // Check that this instance is inside a module.
  auto module = instance->getParentOfType<FModuleOp>();
  if (!module) {
    instance.emitOpError("should be embedded in a 'firrtl.module'");
    return failure();
  }

  auto *referencedModule = instance.getReferencedModule();
  if (!referencedModule) {
    instance.emitOpError("invalid symbol reference");
    return failure();
  }

  // Check that this instance doesn't recursively instantiate its wrapping
  // module.
  if (referencedModule == module) {
    auto diag = instance.emitOpError()
                << "is a recursive instantiation of its containing module";
    diag.attachNote(module.getLoc()) << "containing module declared here";
    return failure();
  }

  SmallVector<ModulePortInfo> modulePorts = getModulePortInfo(referencedModule);

  // Check that result types are consistent with the referenced module's ports.
  size_t numResults = instance.getNumResults();
  if (numResults != modulePorts.size()) {
    auto diag = instance.emitOpError()
                << "has a wrong number of results; expected "
                << modulePorts.size() << " but got " << numResults;
    diag.attachNote(referencedModule->getLoc())
        << "original module declared here";
    return failure();
  }

  for (size_t i = 0; i != numResults; i++) {
    auto resultType = instance.getResult(i).getType();
    auto expectedType = modulePorts[i].type;
    if (modulePorts[i].direction == Direction::Input)
      expectedType = FlipType::get(expectedType);
    if (resultType != expectedType) {
      auto diag = instance.emitOpError()
                  << "result type for " << modulePorts[i].name << " must be "
                  << expectedType << ", but got " << resultType;

      diag.attachNote(referencedModule->getLoc())
          << "original module declared here";
      return failure();
    }
  }

  return success();
}

/// Verify the correctness of a MemOp.
static LogicalResult verifyMemOp(MemOp mem) {

  // Store the port names as we find them. This lets us check quickly
  // for uniqueneess.
  llvm::SmallDenseSet<Attribute, 8> portNamesSet;

  // Store the previous data type. This lets us check that the data
  // type is consistent across all ports.
  FIRRTLType oldDataType;

  for (size_t i = 0, e = mem.getNumResults(); i != e; ++i) {
    auto portName = mem.getPortName(i);

    // Get a bundle type representing this port, stripping an outer
    // flip if it exists.  If this is not a bundle<> or
    // flip<bundle<>>, then this is an error.
    BundleType portBundleType =
        TypeSwitch<FIRRTLType, BundleType>(
            mem.getResult(i).getType().cast<FIRRTLType>())
            .Case<BundleType>([](BundleType a) { return a; })
            .Case<FlipType>([](FlipType a) {
              return a.getElementType().dyn_cast<BundleType>();
            })
            .Default([](auto) { return nullptr; });
    if (!portBundleType) {
      mem.emitOpError() << "has an invalid type on port " << portName
                        << " (expected either '!firrtl.bundle<...>' or "
                           "'!firrtl.flip<bundle<...>>')";
      return failure();
    }

    // Require that all port names are unique.
    if (!portNamesSet.insert(portName).second) {
      mem.emitOpError() << "has non-unique port name " << portName;
      return failure();
    }

    // Determine the kind of the memory.  If the kind cannot be
    // determined, then it's indicative of the wrong number of fields
    // in the type (but we don't know any more just yet).
    MemOp::PortKind portKind;
    {
      auto elt = mem.getPortNamed(portName);
      if (!elt) {
        mem.emitOpError() << "could not get port with name " << portName;
        return failure();
      }
      auto firrtlType = elt.getType().cast<FIRRTLType>();
      auto portType = firrtlType.dyn_cast<BundleType>();
      if (!portType) {
        if (auto flipType = firrtlType.dyn_cast<FlipType>())
          portType = flipType.getElementType().dyn_cast<BundleType>();
      }
      switch (portType.getNumElements()) {
      case 4:
        portKind = MemOp::PortKind::Read;
        break;
      case 5:
        portKind = MemOp::PortKind::Write;
        break;
      case 7:
        portKind = MemOp::PortKind::ReadWrite;
        break;
      default:
        mem.emitOpError()
            << "has an invalid number of fields on port " << portName
            << " (expected 4 for read, 5 for write, or 7 for read/write)";
        return failure();
      }
    }

    // Safely search for the "data" field, erroring if it can't be
    // found.
    FIRRTLType dataType;
    {
      auto dataTypeOption = portBundleType.getElement("data");
      if (!dataTypeOption && portKind == MemOp::PortKind::ReadWrite)
        dataTypeOption = portBundleType.getElement("wdata");
      if (!dataTypeOption) {
        mem.emitOpError() << "has no data field on port " << portName
                          << " (expected to see \"data\" for a read or write "
                             "port or \"rdata\" for a read/write port)";
        return failure();
      }
      dataType = dataTypeOption.getValue().type;
      // Read data is expected to have an outer flip, so strip that.
      if (portKind == MemOp::PortKind::Read)
        dataType = FlipType::get(dataType);
    }

    // Error if the data type isn't passive.
    if (!dataType.isPassive()) {
      mem.emitOpError() << "has non-passive data type on port " << portName
                        << " (memory types must be passive)";
      return failure();
    }

    // Error if the data type contains analog types.
    if (dataType.containsAnalog()) {
      mem.emitOpError()
          << "has a data type that contains an analog type on port " << portName
          << " (memory types cannot contain analog types)";
      return failure();
    }

    // Compute the original port type as portBundleType may have
    // stripped outer flip information.
    auto originalType = mem.getResult(i).getType();
    size_t maskSize = 0;
    if (auto bType = originalType.cast<FIRRTLType>()
                         .getPassiveType()
                         .dyn_cast_or_null<BundleType>())
      for (auto elem : bType.getElements())
        if (elem.name.getValue().contains("mask") &&
            elem.type.cast<FIRRTLType>().isGround())
          maskSize = elem.type.cast<FIRRTLType>().getBitWidthOrSentinel();

    // Check that the port type matches the kind that we determined
    // for this port.  This catches situations of extraneous port
    // fields beind included or the fields being named incorrectly.
    FIRRTLType expectedType = FlipType::get(
        mem.getTypeForPort(mem.depth(), dataType, portKind, maskSize));
    if (originalType != expectedType) {
      StringRef portKindName;
      switch (portKind) {
      case MemOp::PortKind::Read:
        portKindName = "read";
        break;
      case MemOp::PortKind::Write:
        portKindName = "write";
        break;
      case MemOp::PortKind::ReadWrite:
        portKindName = "readwrite";
        break;
      }
      mem.emitOpError() << "has an invalid type for port " << portName
                        << " of determined kind \"" << portKindName
                        << "\" (expected " << expectedType << ", but got "
                        << originalType << ")";
      return failure();
    }

    // Error if the type of the current port was not the same as the
    // last port, but skip checking the first port.
    if (oldDataType && oldDataType != dataType) {
      mem.emitOpError() << "port " << mem.getPortName(i)
                        << " has a different type than port "
                        << mem.getPortName(i - 1) << " (expected "
                        << oldDataType << ", but got " << dataType << ")";
      return failure();
    }

    oldDataType = dataType;
  }

  return success();
}

BundleType MemOp::getTypeForPort(uint64_t depth, FIRRTLType dataType,
                                 PortKind portKind, size_t maskSize) {

  auto *context = dataType.getContext();

  auto getId = [&](StringRef name) -> StringAttr {
    return StringAttr::get(context, name);
  };

  SmallVector<BundleType::BundleElement, 7> portFields;

  auto addressType =
      UIntType::get(context, std::max(1U, llvm::Log2_64_Ceil(depth)));

  portFields.push_back({getId("addr"), addressType});
  portFields.push_back({getId("en"), UIntType::get(context, 1)});
  portFields.push_back({getId("clk"), ClockType::get(context)});

  switch (portKind) {
  case PortKind::Read:
    portFields.push_back({getId("data"), FlipType::get(dataType)});
    break;

  case PortKind::Write:
    portFields.push_back({getId("data"), dataType});
    portFields.push_back({getId("mask"), maskSize
                                             ? UIntType::get(context, maskSize)
                                             : dataType.getMaskType()});
    break;

  case PortKind::ReadWrite:
    portFields.push_back({getId("wmode"), UIntType::get(context, 1)});
    portFields.push_back({getId("rdata"), FlipType::get(dataType)});
    portFields.push_back({getId("wdata"), dataType});
    portFields.push_back({getId("wmask"), maskSize
                                              ? UIntType::get(context, maskSize)
                                              : dataType.getMaskType()});
    break;
  }

  return BundleType::get(portFields, context).cast<BundleType>();
}

/// Return the kind of port this is given the port type from a 'mem' decl.
static MemOp::PortKind getMemPortKindFromType(FIRRTLType type) {
  auto portType = type.dyn_cast<BundleType>();
  if (!portType) {
    if (auto flipType = type.dyn_cast<FlipType>())
      portType = flipType.getElementType().dyn_cast<BundleType>();
  }
  switch (portType.getNumElements()) {
  case 4:
    return MemOp::PortKind::Read;
  case 5:
    return MemOp::PortKind::Write;
  default:
    return MemOp::PortKind::ReadWrite;
  }
}

/// Return the name and kind of ports supported by this memory.
SmallVector<MemOp::NamedPort> MemOp::getPorts() {
  SmallVector<MemOp::NamedPort> result;
  // Each entry in the bundle is a port.
  for (size_t i = 0, e = getNumResults(); i != e; ++i) {
    // Each port is a bundle.
    auto portType = getResult(i).getType().cast<FIRRTLType>();
    result.push_back({getPortName(i), getMemPortKindFromType(portType)});
  }
  return result;
}

/// Return the kind of the specified port.
MemOp::PortKind MemOp::getPortKind(StringRef portName) {
  return getMemPortKindFromType(
      getPortNamed(portName).getType().cast<FIRRTLType>());
}

/// Return the kind of the specified port number.
MemOp::PortKind MemOp::getPortKind(size_t resultNo) {
  return getMemPortKindFromType(
      getResult(resultNo).getType().cast<FIRRTLType>());
}

/// Return the data-type field of the memory, the type of each element.
FIRRTLType MemOp::getDataType() {
  assert(getNumResults() != 0 && "Mems with no read/write ports are illegal");

  auto firstPortType = getResult(0).getType().cast<FIRRTLType>();

  StringRef dataFieldName = "data";
  if (getMemPortKindFromType(firstPortType) == PortKind::ReadWrite)
    dataFieldName = "rdata";

  return firstPortType.getPassiveType().cast<BundleType>().getElementType(
      dataFieldName);
}

/// Return the mask-size of the memory.
uint64_t MemOp::getMaskSize() {
  assert(getNumResults() != 0 && "Mems with no read/write ports are illegal");

  auto firstPortType = getResult(0).getType().cast<FIRRTLType>();

  StringRef dataFieldName = "mask";
  if (getMemPortKindFromType(firstPortType) == PortKind::ReadWrite)
    dataFieldName = "wmask";

  if (auto bType = firstPortType.getPassiveType().dyn_cast<BundleType>())
    if (auto maskField = bType.getElement(dataFieldName))
      return maskField.getValue().type.getBitWidthOrSentinel();
  return 1;
}

StringAttr MemOp::getPortName(size_t resultNo) {
  return portNames()[resultNo].cast<StringAttr>();
}

FIRRTLType MemOp::getPortType(size_t resultNo) {
  return results()[resultNo].getType().cast<FIRRTLType>();
}

Value MemOp::getPortNamed(StringAttr name) {
  auto namesArray = portNames();
  for (size_t i = 0, e = namesArray.size(); i != e; ++i) {
    if (namesArray[i] == name) {
      assert(i < getNumResults() && " names array out of sync with results");
      return getResult(i);
    }
  }
  return Value();
}

//===----------------------------------------------------------------------===//
// Statements
//===----------------------------------------------------------------------===//

static LogicalResult verifyConnectOp(ConnectOp connect) {
  FIRRTLType destType = connect.dest().getType().cast<FIRRTLType>();
  FIRRTLType srcType = connect.src().getType().cast<FIRRTLType>();

  auto isPortOrInstancePort = [](Value a) -> bool {
    auto op = a.getDefiningOp();
    return !op || isa<InstanceOp>(op);
  };

  // If the source or destination is a port or instance port, then an optional
  // outer flip, indicating the direction (input or output), should be stripped
  // for type checking.
  if (isPortOrInstancePort(connect.dest()))
    if (auto destTypeFlip = destType.dyn_cast<FlipType>())
      destType = destTypeFlip.getElementType();
  if (isPortOrInstancePort(connect.src()))
    if (auto srcTypeFlip = srcType.dyn_cast<FlipType>())
      srcType = srcTypeFlip.getElementType();

  // Analog types cannot be connected and must be attached.
  if (destType.isa<AnalogType>() || srcType.isa<AnalogType>())
    return connect.emitError("analog types may not be connected");

  // Destination and source types must be equivalent.
  if (!areTypesEquivalent(destType, srcType))
    return connect.emitError("type mismatch between destination ")
           << destType << " and source " << srcType;

  // Destination bitwidth must be greater than or equal to source bitwidth.
  int32_t destWidth = destType.getPassiveType().getBitWidthOrSentinel();
  int32_t srcWidth = srcType.getPassiveType().getBitWidthOrSentinel();
  if (destWidth > -1 && srcWidth > -1 && destWidth < srcWidth)
    return connect.emitError("destination width ")
           << destWidth << " is not greater than or equal to source width "
           << srcWidth;

  // TODO: Relax this to allow reads from output ports,
  // instance/memory input ports.
  if (foldFlow(connect.src()) == Flow::Sink) {
    // A sink that is a port output or instance input used as a source is okay.
    auto kind = getDeclarationKind(connect.src());
    if (kind != DeclKind::Port && kind != DeclKind::Instance) {
      auto diag =
          connect.emitOpError()
          << "has invalid flow: the right-hand-side has sink flow and "
             "is not an output port or instance input (expected source "
             "flow, duplex flow, an output port, or an instance input).";
      return diag.attachNote(connect.src().getLoc())
             << "the right-hand-side was defined here.";
    }
  }

  if (foldFlow(connect.dest()) == Flow::Source) {
    auto diag = connect.emitOpError()
                << "has invalid flow: the left-hand-side has source flow "
                   "(expected sink or duplex flow).";
    return diag.attachNote(connect.dest().getLoc())
           << "the left-hand-side was defined here.";
  }

  return success();
}

static LogicalResult verifyPartialConnectOp(PartialConnectOp partialConnect) {
  FIRRTLType destType =
      partialConnect.dest().getType().cast<FIRRTLType>().stripFlip().first;
  FIRRTLType srcType =
      partialConnect.src().getType().cast<FIRRTLType>().stripFlip().first;

  if (!areTypesWeaklyEquivalent(destType, srcType))
    return partialConnect.emitError("type mismatch between destination ")
           << destType << " and source " << srcType
           << ". Types are not weakly equivalent.";

  // Check that the flows make sense.
  if (foldFlow(partialConnect.src()) == Flow::Sink) {
    // A sink that is a port output or instance input used as a source is okay.
    auto kind = getDeclarationKind(partialConnect.src());
    if (kind != DeclKind::Port && kind != DeclKind::Instance) {
      auto diag =
          partialConnect.emitOpError()
          << "has invalid flow: the right-hand-side has sink flow and "
             "is not an output port or instance input (expected source "
             "flow, duplex flow, an output port, or an instance input).";
      return diag.attachNote(partialConnect.src().getLoc())
             << "the right-hand-side was defined here.";
    }
  }

  if (foldFlow(partialConnect.dest()) == Flow::Source) {
    auto diag = partialConnect.emitOpError()
                << "has invalid flow: the left-hand-side has source flow "
                   "(expected sink or duplex flow).";
    return diag.attachNote(partialConnect.dest().getLoc())
           << "the left-hand-side was defined here.";
  }

  return success();
}

void WhenOp::createElseRegion() {
  assert(!hasElseRegion() && "already has an else region");
  elseRegion().push_back(new Block());
}

void WhenOp::build(OpBuilder &builder, OperationState &result, Value condition,
                   bool withElseRegion, std::function<void()> thenCtor,
                   std::function<void()> elseCtor) {
  OpBuilder::InsertionGuard guard(builder);
  result.addOperands(condition);

  // Create "then" region.
  builder.createBlock(result.addRegion());
  if (thenCtor)
    thenCtor();

  // Create "else" region.
  Region *elseRegion = result.addRegion();
  if (withElseRegion) {
    builder.createBlock(elseRegion);
    if (elseCtor)
      elseCtor();
  }
}

//===----------------------------------------------------------------------===//
// Expressions
//===----------------------------------------------------------------------===//

/// Type inference adaptor that narrows from the very generic MLIR
/// `InferTypeOpInterface` to what we need in the FIRRTL dialect: just operands
/// and attributes, no context or regions. Also, we only ever produce a single
/// result value, so the FIRRTL-specific type inference ops directly return the
/// inferred type rather than pushing into the `results` vector.
LogicalResult impl::inferReturnTypes(
    MLIRContext *context, Optional<Location> loc, ValueRange operands,
    DictionaryAttr attrs, RegionRange regions, SmallVectorImpl<Type> &results,
    llvm::function_ref<FIRRTLType(ValueRange, ArrayRef<NamedAttribute>,
                                  Optional<Location>)>
        callback) {
  auto type = callback(
      operands, attrs ? attrs.getValue() : ArrayRef<NamedAttribute>{}, loc);
  if (type) {
    results.push_back(type);
    return success();
  }
  return failure();
}

/// Get an attribute by name from a list of named attributes. Aborts if the
/// attribute does not exist.
static Attribute getAttr(ArrayRef<NamedAttribute> attrs, StringRef name) {
  for (auto attr : attrs)
    if (attr.first == name)
      return attr.second;
  llvm::report_fatal_error("attribute '" + name + "' not found");
}

/// Same as above, but casts the attribute to a specific type.
template <typename AttrClass>
AttrClass getAttr(ArrayRef<NamedAttribute> attrs, StringRef name) {
  return getAttr(attrs, name).cast<AttrClass>();
}

/// Return true if the specified operation is a firrtl expression.
bool firrtl::isExpression(Operation *op) {
  struct IsExprClassifier : public ExprVisitor<IsExprClassifier, bool> {
    bool visitInvalidExpr(Operation *op) { return false; }
    bool visitUnhandledExpr(Operation *op) { return true; }
  };

  return IsExprClassifier().dispatchExprVisitor(op);
}

static void printConstantOp(OpAsmPrinter &p, ConstantOp &op) {
  p << "firrtl.constant ";
  p.printAttributeWithoutType(op.valueAttr());
  p << " : ";
  p.printType(op.getType());
  p.printOptionalAttrDict(op->getAttrs(), /*elidedAttrs=*/{"value"});
}

static ParseResult parseConstantOp(OpAsmParser &parser,
                                   OperationState &result) {
  // Parse the constant value, without knowing its width.
  APInt value;
  auto loc = parser.getCurrentLocation();
  auto valueResult = parser.parseOptionalInteger(value);
  if (!valueResult.hasValue())
    return parser.emitError(loc, "expected integer value");

  // Parse the result firrtl integer type.
  IntType resultType;
  if (failed(*valueResult) || parser.parseColonType(resultType) ||
      parser.parseOptionalAttrDict(result.attributes))
    return failure();
  result.addTypes(resultType);

  // Now that we know the width and sign of the result type, we can munge the
  // APInt as appropriate.
  if (resultType.hasWidth()) {
    auto width = (unsigned)resultType.getWidthOrSentinel();
    if (width == 0)
      return parser.emitError(loc, "zero bit constants aren't allowed");

    if (width > value.getBitWidth()) {
      // sext is always safe here, even for unsigned values, because the
      // parseOptionalInteger method will return something with a zero in the
      // top bits if it is a positive number.
      value = value.sext(width);
    } else if (width < value.getBitWidth()) {
      // The parser can return an unnecessarily wide result with leading zeros.
      // This isn't a problem, but truncating off bits is bad.
      if (value.getNumSignBits() < value.getBitWidth() - width)
        return parser.emitError(loc, "constant too large for result type ")
               << resultType;
      value = value.trunc(width);
    }
  }

  auto intType = parser.getBuilder().getIntegerType(value.getBitWidth(),
                                                    resultType.isSigned());
  auto valueAttr = parser.getBuilder().getIntegerAttr(intType, value);
  result.addAttribute("value", valueAttr);
  return success();
}

static LogicalResult verifyConstantOp(ConstantOp constant) {
  // If the result type has a bitwidth, then the attribute must match its width.
  auto intType = constant.getType().cast<IntType>();
  auto width = intType.getWidthOrSentinel();
  if (width != -1 && (int)constant.value().getBitWidth() != width)
    return constant.emitError(
        "firrtl.constant attribute bitwidth doesn't match return type");

  // The sign of the attribute's integer type must match our integer type sign.
  auto attrType = constant.valueAttr().getType().cast<IntegerType>();
  if (attrType.isSignless() ||
      attrType.isSigned() != constant.getType().isSigned())
    return constant.emitError("firrtl.constant attribute has wrong sign");

  return success();
}

/// Build a ConstantOp from an APInt and a FIRRTL type, handling the attribute
/// formation for the 'value' attribute.
void ConstantOp::build(OpBuilder &builder, OperationState &result, IntType type,
                       const APInt &value) {
  int32_t width = type.getWidthOrSentinel();
  (void)width;
  assert((width == -1 || (int32_t)value.getBitWidth() == width) &&
         "incorrect attribute bitwidth for firrtl.constant");

  auto attr =
      IntegerAttr::get(type.getContext(), APSInt(value, !type.isSigned()));
  return build(builder, result, type, attr);
}

/// Build a ConstantOp from an APSInt, handling the attribute formation for the
/// 'value' attribute and inferring the FIRRTL type.
void ConstantOp::build(OpBuilder &builder, OperationState &result,
                       const APSInt &value) {
  auto attr = IntegerAttr::get(builder.getContext(), value);
  auto type =
      IntType::get(builder.getContext(), value.isSigned(), value.getBitWidth());
  return build(builder, result, type, attr);
}

FIRRTLType SubfieldOp::inferReturnType(ValueRange operands,
                                       ArrayRef<NamedAttribute> attrs,
                                       Optional<Location> loc) {
  auto inType = operands[0].getType();
  auto fieldname = getAttr<StringAttr>(attrs, "fieldname");

  // Look through flips.
  while (auto flipType = inType.dyn_cast<FlipType>())
    inType = flipType.getElementType();

  if (auto bundleType = inType.dyn_cast<BundleType>()) {
    auto elt = bundleType.getElement(fieldname.getValue());
    if (!elt) {
      if (loc)
        mlir::emitError(*loc, "unknown field '")
            << fieldname.getValue() << "' in bundle type " << inType;
      return {};
    }
    // FIRRTL puts flips on element fields, not on the underlying
    // types.  The result type of a subfield should strip a flip
    // if one exists.
    if (auto flipped = elt->type.dyn_cast<FlipType>())
      return flipped.getElementType().cast<FIRRTLType>();
    return elt->type;
  }

  if (loc)
    mlir::emitError(*loc, "subfield requires bundle operand");
  return {};
}

bool SubfieldOp::isFieldFlipped() {
  auto fieldname = this->fieldname();

  auto handleBundle = [&](BundleType a) -> bool {
    auto b = a.getElement(fieldname);
    if (!b) {
      emitOpError() << "unknown field '" << fieldname << "' in bundle type "
                    << a;
      return false;
    };
    return b.getValue().type.isa<FlipType>();
  };

  return TypeSwitch<Type, bool>(this->input().getType())
      .Case<BundleType>([&](auto a) { return handleBundle(a); })
      .Case<FlipType>([&](auto a) {
        return handleBundle(a.getElementType().template dyn_cast<BundleType>());
      })
      .Default([&](auto) {
        emitOpError() << "subfield requires bundle operand";
        return false;
      });
}

FIRRTLType SubindexOp::inferReturnType(ValueRange operands,
                                       ArrayRef<NamedAttribute> attrs,
                                       Optional<Location> loc) {
  auto inType = operands[0].getType();
  auto fieldIdx =
      getAttr<IntegerAttr>(attrs, "index").getValue().getZExtValue();

  // Look through flips.
  while (auto flipType = inType.dyn_cast<FlipType>())
    inType = flipType.getElementType();

  if (auto vectorType = inType.dyn_cast<FVectorType>()) {
    if (fieldIdx < vectorType.getNumElements())
      return vectorType.getElementType();
    if (loc)
      mlir::emitError(*loc, "out of range index '")
          << fieldIdx << "' in vector type " << inType;
    return {};
  }

  if (loc)
    mlir::emitError(*loc, "subindex requires vector operand");
  return {};
}

FIRRTLType SubaccessOp::inferReturnType(ValueRange operands,
                                        ArrayRef<NamedAttribute> attrs,
                                        Optional<Location> loc) {
  auto inType = operands[0].getType();
  auto indexType = operands[1].getType();

  if (!indexType.isa<UIntType>()) {
    if (loc)
      mlir::emitError(*loc, "subaccess index must be UInt type, not ")
          << indexType;
    return {};
  }

  // Look through flips.
  while (auto flipType = inType.dyn_cast<FlipType>())
    inType = flipType.getElementType();

  if (auto vectorType = inType.dyn_cast<FVectorType>())
    return vectorType.getElementType();

  if (loc)
    mlir::emitError(*loc, "subaccess requires vector operand, not ") << inType;
  return {};
}

//===----------------------------------------------------------------------===//
// Binary Primitives
//===----------------------------------------------------------------------===//

/// If LHS and RHS are both UInt or SInt types, the return true and fill in the
/// width of them if known.  If unknown, return -1 for the widths.
///
/// On failure, this reports and error and returns false.  This function should
/// not be used if you don't want an error reported.
static bool isSameIntTypeKind(Type lhs, Type rhs, int32_t &lhsWidth,
                              int32_t &rhsWidth, Optional<Location> loc) {
  // Must have two integer types with the same signedness.
  auto lhsi = lhs.dyn_cast<IntType>();
  auto rhsi = rhs.dyn_cast<IntType>();
  if (!lhsi || !rhsi || lhsi.isSigned() != rhsi.isSigned()) {
    if (loc) {
      if (lhsi && !rhsi)
        mlir::emitError(*loc, "second operand must be an integer type, not ")
            << rhs;
      else if (!lhsi && rhsi)
        mlir::emitError(*loc, "first operand must be an integer type, not ")
            << lhs;
      else if (!lhsi && !rhsi)
        mlir::emitError(*loc, "operands must be integer types, not ")
            << lhs << " and " << rhs;
      else
        mlir::emitError(*loc, "operand signedness must match");
    }
    return false;
  }

  lhsWidth = lhsi.getWidthOrSentinel();
  rhsWidth = rhsi.getWidthOrSentinel();
  return true;
}

LogicalResult impl::verifySameOperandsIntTypeKind(Operation *op) {
  assert(op->getNumOperands() == 2 &&
         "SameOperandsIntTypeKind on non-binary op");
  int32_t lhsWidth, rhsWidth;
  return success(isSameIntTypeKind(op->getOperand(0).getType(),
                                   op->getOperand(1).getType(), lhsWidth,
                                   rhsWidth, op->getLoc()));
}

LogicalResult impl::validateBinaryOpArguments(ValueRange operands,
                                              ArrayRef<NamedAttribute> attrs,
                                              Location loc) {
  if (operands.size() != 2 || !attrs.empty()) {
    mlir::emitError(loc, "operation requires two operands and no constants");
    return failure();
  }
  return success();
}

FIRRTLType impl::inferAddSubResult(FIRRTLType lhs, FIRRTLType rhs,
                                   Optional<Location> loc) {
  int32_t lhsWidth, rhsWidth, resultWidth = -1;
  if (!isSameIntTypeKind(lhs, rhs, lhsWidth, rhsWidth, loc))
    return {};

  if (lhsWidth != -1 && rhsWidth != -1)
    resultWidth = std::max(lhsWidth, rhsWidth) + 1;
  return IntType::get(lhs.getContext(), lhs.isa<SIntType>(), resultWidth);
}

FIRRTLType MulPrimOp::inferBinaryReturnType(FIRRTLType lhs, FIRRTLType rhs,
                                            Optional<Location> loc) {
  int32_t lhsWidth, rhsWidth, resultWidth = -1;
  if (!isSameIntTypeKind(lhs, rhs, lhsWidth, rhsWidth, loc))
    return {};

  if (lhsWidth != -1 && rhsWidth != -1)
    resultWidth = lhsWidth + rhsWidth;

  return IntType::get(lhs.getContext(), lhs.isa<SIntType>(), resultWidth);
}

FIRRTLType DivPrimOp::inferBinaryReturnType(FIRRTLType lhs, FIRRTLType rhs,
                                            Optional<Location> loc) {
  int32_t lhsWidth, rhsWidth;
  if (!isSameIntTypeKind(lhs, rhs, lhsWidth, rhsWidth, loc))
    return {};

  // For unsigned, the width is the width of the numerator on the LHS.
  if (lhs.isa<UIntType>())
    return UIntType::get(lhs.getContext(), lhsWidth);

  // For signed, the width is the width of the numerator on the LHS, plus 1.
  int32_t resultWidth = lhsWidth != -1 ? lhsWidth + 1 : -1;
  return SIntType::get(lhs.getContext(), resultWidth);
}

FIRRTLType RemPrimOp::inferBinaryReturnType(FIRRTLType lhs, FIRRTLType rhs,
                                            Optional<Location> loc) {
  int32_t lhsWidth, rhsWidth, resultWidth = -1;
  if (!isSameIntTypeKind(lhs, rhs, lhsWidth, rhsWidth, loc))
    return {};

  if (lhsWidth != -1 && rhsWidth != -1)
    resultWidth = std::min(lhsWidth, rhsWidth);
  return IntType::get(lhs.getContext(), lhs.isa<SIntType>(), resultWidth);
}

FIRRTLType impl::inferBitwiseResult(FIRRTLType lhs, FIRRTLType rhs,
                                    Optional<Location> loc) {
  int32_t lhsWidth, rhsWidth, resultWidth = -1;
  if (!isSameIntTypeKind(lhs, rhs, lhsWidth, rhsWidth, loc))
    return {};

  if (lhsWidth != -1 && rhsWidth != -1)
    resultWidth = std::max(lhsWidth, rhsWidth);
  return UIntType::get(lhs.getContext(), resultWidth);
}

FIRRTLType impl::inferComparisonResult(FIRRTLType lhs, FIRRTLType rhs,
                                       Optional<Location> loc) {
  return UIntType::get(lhs.getContext(), 1);
}

FIRRTLType CatPrimOp::inferBinaryReturnType(FIRRTLType lhs, FIRRTLType rhs,
                                            Optional<Location> loc) {
  int32_t lhsWidth, rhsWidth, resultWidth = -1;
  if (!isSameIntTypeKind(lhs, rhs, lhsWidth, rhsWidth, loc))
    return {};

  if (lhsWidth != -1 && rhsWidth != -1)
    resultWidth = lhsWidth + rhsWidth;
  return UIntType::get(lhs.getContext(), resultWidth);
}

FIRRTLType DShlPrimOp::inferBinaryReturnType(FIRRTLType lhs, FIRRTLType rhs,
                                             Optional<Location> loc) {
  auto lhsi = lhs.dyn_cast<IntType>();
  auto rhsui = rhs.dyn_cast<UIntType>();
  if (!rhsui || !lhsi) {
    if (loc)
      mlir::emitError(*loc,
                      "first operand should be integer, second unsigned int");
    return {};
  }

  // If the left or right has unknown result type, then the operation does
  // too.
  auto width = lhsi.getWidthOrSentinel();
  if (width == -1 || !rhsui.getWidth().hasValue())
    width = -1;
  else
    width = width + (1 << rhsui.getWidth().getValue()) - 1;
  return IntType::get(lhs.getContext(), lhsi.isSigned(), width);
}

FIRRTLType DShlwPrimOp::inferBinaryReturnType(FIRRTLType lhs, FIRRTLType rhs,
                                              Optional<Location> loc) {
  if (!lhs.isa<IntType>() || !rhs.isa<UIntType>()) {
    if (loc)
      mlir::emitError(*loc,
                      "first operand should be integer, second unsigned int");
    return {};
  }
  return lhs;
}

FIRRTLType DShrPrimOp::inferBinaryReturnType(FIRRTLType lhs, FIRRTLType rhs,
                                             Optional<Location> loc) {
  if (!lhs.isa<IntType>() || !rhs.isa<UIntType>()) {
    if (loc)
      mlir::emitError(*loc,
                      "first operand should be integer, second unsigned int");
    return {};
  }
  return lhs;
}

//===----------------------------------------------------------------------===//
// Unary Primitives
//===----------------------------------------------------------------------===//

LogicalResult impl::validateUnaryOpArguments(ValueRange operands,
                                             ArrayRef<NamedAttribute> attrs,
                                             Location loc) {
  if (operands.size() != 1 || !attrs.empty()) {
    mlir::emitError(loc, "operation requires one operand and no constants");
    return failure();
  }
  return success();
}

FIRRTLType AsSIntPrimOp::inferUnaryReturnType(FIRRTLType input,
                                              Optional<Location> loc) {
  int32_t width = input.getBitWidthOrSentinel();
  if (width == -2) {
    if (loc)
      mlir::emitError(*loc, "operand must be a scalar type");
    return {};
  }
  return SIntType::get(input.getContext(), width);
}

FIRRTLType AsUIntPrimOp::inferUnaryReturnType(FIRRTLType input,
                                              Optional<Location> loc) {
  int32_t width = input.getBitWidthOrSentinel();
  if (width == -2) {
    if (loc)
      mlir::emitError(*loc, "operand must be a scalar type");
    return {};
  }
  return UIntType::get(input.getContext(), width);
}

FIRRTLType AsAsyncResetPrimOp::inferUnaryReturnType(FIRRTLType input,
                                                    Optional<Location> loc) {
  int32_t width = input.getBitWidthOrSentinel();
  if (width == -2 || width == 0 || width > 1) {
    if (loc)
      mlir::emitError(*loc, "operand must be single bit scalar type");
    return {};
  }
  return AsyncResetType::get(input.getContext());
}

FIRRTLType AsClockPrimOp::inferUnaryReturnType(FIRRTLType input,
                                               Optional<Location> loc) {
  return ClockType::get(input.getContext());
}

FIRRTLType CvtPrimOp::inferUnaryReturnType(FIRRTLType input,
                                           Optional<Location> loc) {
  if (auto uiType = input.dyn_cast<UIntType>()) {
    auto width = uiType.getWidthOrSentinel();
    if (width != -1)
      ++width;
    return SIntType::get(input.getContext(), width);
  }

  if (input.isa<SIntType>())
    return input;

  if (loc)
    mlir::emitError(*loc, "operand must have integer type");
  return {};
}

FIRRTLType NegPrimOp::inferUnaryReturnType(FIRRTLType input,
                                           Optional<Location> loc) {
  auto inputi = input.dyn_cast<IntType>();
  if (!inputi) {
    if (loc)
      mlir::emitError(*loc, "operand must have integer type");

    return {};
  }
  int32_t width = inputi.getWidthOrSentinel();
  if (width != -1)
    ++width;
  return SIntType::get(input.getContext(), width);
}

FIRRTLType NotPrimOp::inferUnaryReturnType(FIRRTLType input,
                                           Optional<Location> loc) {
  auto inputi = input.dyn_cast<IntType>();
  if (!inputi) {
    if (loc)
      mlir::emitError(*loc, "operand must have integer type");

    return {};
  }
  return UIntType::get(input.getContext(), inputi.getWidthOrSentinel());
}

FIRRTLType impl::inferReductionResult(FIRRTLType input,
                                      Optional<Location> loc) {
  return UIntType::get(input.getContext(), 1);
}

//===----------------------------------------------------------------------===//
// Other Operations
//===----------------------------------------------------------------------===//

LogicalResult BitsPrimOp::validateArguments(ValueRange operands,
                                            ArrayRef<NamedAttribute> attrs,
                                            Location loc) {
  if (operands.size() != 1 || attrs.size() != 2) {
    mlir::emitError(loc, "operation requires one operand and two constants");
    return failure();
  }
  return success();
}

FIRRTLType BitsPrimOp::inferReturnType(ValueRange operands,
                                       ArrayRef<NamedAttribute> attrs,
                                       Optional<Location> loc) {
  auto input = operands[0].getType();
  auto high = getAttr<IntegerAttr>(attrs, "hi").getValue().getSExtValue();
  auto low = getAttr<IntegerAttr>(attrs, "lo").getValue().getSExtValue();

  auto inputi = input.dyn_cast<IntType>();
  if (!inputi) {
    if (loc)
      mlir::emitError(*loc, "input type should be the int type but got ")
          << input;
    return {};
  }

  // High must be >= low and both most be non-negative.
  if (high < low) {
    if (loc)
      mlir::emitError(*loc,
                      "high must be equal or greater than low, but got high = ")
          << high << ", low = " << low;
    return {};
  }

  if (low < 0) {
    if (loc)
      mlir::emitError(*loc, "low must be non-negative but got ") << low;
    return {};
  }

  // If the input has staticly known width, check it.  Both and low must be
  // strictly less than width.
  int32_t width = inputi.getWidthOrSentinel();
  if (width != -1 && high >= width) {
    if (loc)
      mlir::emitError(*loc)
          << "high must be smaller than the width of input, but got high = "
          << high << ", width = " << width;
    return {};
  }

  return UIntType::get(input.getContext(), high - low + 1);
}

LogicalResult impl::validateOneOperandOneConst(ValueRange operands,
                                               ArrayRef<NamedAttribute> attrs,
                                               Location loc) {
  if (operands.size() != 1 || attrs.size() != 1) {
    mlir::emitError(loc, "operation requires one operand and one constant");
    return failure();
  }
  return success();
}

FIRRTLType HeadPrimOp::inferReturnType(ValueRange operands,
                                       ArrayRef<NamedAttribute> attrs,
                                       Optional<Location> loc) {
  auto input = operands[0].getType();
  auto amount = getAttr<IntegerAttr>(attrs, "amount").getValue().getSExtValue();

  auto inputi = input.dyn_cast<IntType>();
  if (amount < 0 || !inputi) {
    if (loc)
      mlir::emitError(*loc,
                      "operand must have integer type and amount must be >= 0");
    return {};
  }

  int32_t width = inputi.getWidthOrSentinel();
  if (width != -1 && amount > width) {
    if (loc)
      mlir::emitError(*loc, "amount larger than input width");
    return {};
  }

  width = std::max<int32_t>(width, amount);
  return UIntType::get(input.getContext(), amount);
}

LogicalResult MuxPrimOp::validateArguments(ValueRange operands,
                                           ArrayRef<NamedAttribute> attrs,
                                           Location loc) {
  if (operands.size() != 3 || attrs.size() != 0) {
    mlir::emitError(loc, "operation requires three operands and no constants");
    return failure();
  }
  return success();
}

FIRRTLType MuxPrimOp::inferReturnType(ValueRange operands,
                                      ArrayRef<NamedAttribute> attrs,
                                      Optional<Location> loc) {
  auto sel = operands[0].getType();
  auto high = operands[1].getType().cast<FIRRTLType>();
  auto low = operands[2].getType().cast<FIRRTLType>();

  // Sel needs to be a one bit uint or an unknown width uint.
  auto selui = sel.dyn_cast<UIntType>();
  int32_t selWidth = selui.getBitWidthOrSentinel();
  if (!selui || selWidth == 0 || selWidth > 1) {
    if (loc)
      mlir::emitError(*loc, "selector must be UInt or UInt<1>");
    return {};
  }

  // TODO: Should use a more general type equivalence operator.
  if (high == low)
    return low;

  // The base types need to be equivalent.
  if (high.getTypeID() != low.getTypeID()) {
    if (loc)
      mlir::emitError(*loc, "true and false value must have same type");
    return {};
  }

  if (low.isa<IntType>()) {
    // Two different Int types can be compatible.  If either has unknown
    // width, then return it.  If both are known but different width, then
    // return the larger one.
    int32_t highWidth = high.getBitWidthOrSentinel();
    int32_t lowWidth = low.getBitWidthOrSentinel();
    if (lowWidth == -1)
      return low;
    if (highWidth == -1)
      return high;
    return lowWidth > highWidth ? low : high;
  }

  // FIXME: Should handle bundles and other things.
  if (loc)
    mlir::emitError(*loc, "unknown types to mux");
  return {};
}

FIRRTLType PadPrimOp::inferReturnType(ValueRange operands,
                                      ArrayRef<NamedAttribute> attrs,
                                      Optional<Location> loc) {
  auto input = operands[0].getType();
  auto amount = getAttr<IntegerAttr>(attrs, "amount").getValue().getSExtValue();

  auto inputi = input.dyn_cast<IntType>();
  if (amount < 0 || !inputi) {
    if (loc)
      mlir::emitError(*loc, "input must be integer and amount must be >= 0");
    return {};
  }

  int32_t width = inputi.getWidthOrSentinel();
  if (width == -1)
    return inputi;

  width = std::max<int32_t>(width, amount);
  return IntType::get(input.getContext(), inputi.isSigned(), width);
}

FIRRTLType ShlPrimOp::inferReturnType(ValueRange operands,
                                      ArrayRef<NamedAttribute> attrs,
                                      Optional<Location> loc) {
  auto input = operands[0].getType();
  auto amount = getAttr<IntegerAttr>(attrs, "amount").getValue().getSExtValue();

  auto inputi = input.dyn_cast<IntType>();
  if (amount < 0 || !inputi) {
    if (loc)
      mlir::emitError(*loc, "input must be integer and amount must be >= 0");
    return {};
  }

  int32_t width = inputi.getWidthOrSentinel();
  if (width != -1)
    width += amount;

  return IntType::get(input.getContext(), inputi.isSigned(), width);
}

FIRRTLType ShrPrimOp::inferReturnType(ValueRange operands,
                                      ArrayRef<NamedAttribute> attrs,
                                      Optional<Location> loc) {
  auto input = operands[0].getType();
  auto amount = getAttr<IntegerAttr>(attrs, "amount").getValue().getSExtValue();

  auto inputi = input.dyn_cast<IntType>();
  if (amount < 0 || !inputi) {
    if (loc)
      mlir::emitError(*loc, "input must be integer and amount must be >= 0");
    return {};
  }

  int32_t width = inputi.getWidthOrSentinel();
  if (width != -1)
    width = std::max<int32_t>(1, width - amount);

  return IntType::get(input.getContext(), inputi.isSigned(), width);
}

FIRRTLType TailPrimOp::inferReturnType(ValueRange operands,
                                       ArrayRef<NamedAttribute> attrs,
                                       Optional<Location> loc) {
  auto input = operands[0].getType();
  auto amount = getAttr<IntegerAttr>(attrs, "amount").getValue().getSExtValue();

  auto inputi = input.dyn_cast<IntType>();
  if (amount < 0 || !inputi) {
    if (loc)
      mlir::emitError(*loc, "input must be integer and amount must be >= 0");
    return {};
  }

  int32_t width = inputi.getWidthOrSentinel();
  if (width != -1) {
    if (width < amount) {
      if (loc)
        mlir::emitError(*loc,
                        "amount must be less than or equal operand width");
      return {};
    }
    width -= amount;
  }

  return IntType::get(input.getContext(), false, width);
}

//===----------------------------------------------------------------------===//
// VerbatimExprOp
//===----------------------------------------------------------------------===//

void VerbatimExprOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  // If the text is macro like, then use a pretty name.  We only take the
  // text up to a weird character (like a paren) and currently ignore
  // parenthesized expressions.
  auto isOkCharacter = [](char c) { return llvm::isAlnum(c) || c == '_'; };
  auto name = text();
  // Ignore a leading ` in macro name.
  if (name.startswith("`"))
    name = name.drop_front();
  name = name.take_while(isOkCharacter);
  if (!name.empty())
    setNameFn(getResult(), name);
}

//===----------------------------------------------------------------------===//
// Conversions to/from fixed-width signless integer types in standard dialect.
//===----------------------------------------------------------------------===//

static LogicalResult verifyStdIntCastOp(StdIntCastOp cast) {
  // Either the input or result must have signless standard integer type, the
  // other must be a FIRRTL type that lowers to one, and their widths must
  // match.
  FIRRTLType firType;
  IntegerType integerType;
  if ((firType = cast.getOperand().getType().dyn_cast<FIRRTLType>())) {
    integerType = cast.getType().dyn_cast<IntegerType>();
    if (!integerType)
      return cast.emitError("result type must be a signless integer");
  } else if ((firType = cast.getType().dyn_cast<FIRRTLType>())) {
    integerType = cast.getOperand().getType().dyn_cast<IntegerType>();
    if (!integerType)
      return cast.emitError("operand type must be a signless integer");
  } else {
    return cast.emitError("either source or result type must be integer type");
  }

  int32_t intWidth = firType.getBitWidthOrSentinel();
  if (intWidth == -2)
    return cast.emitError("firrtl type isn't simple bit type");
  if (intWidth == -1)
    return cast.emitError("SInt/UInt type must have a width");
  if (!integerType.isSignless())
    return cast.emitError("standard integer type must be signless");
  if (unsigned(intWidth) != integerType.getWidth())
    return cast.emitError("source and result width must match");

  return success();
}

static LogicalResult verifyAnalogInOutCastOp(AnalogInOutCastOp cast) {
  AnalogType firType;
  hw::InOutType inoutType;

  if ((firType = cast.getOperand().getType().dyn_cast<AnalogType>())) {
    inoutType = cast.getType().dyn_cast<hw::InOutType>();
    if (!inoutType)
      return cast.emitError("result type must be an inout type");
  } else if ((firType = cast.getType().dyn_cast<AnalogType>())) {
    inoutType = cast.getOperand().getType().dyn_cast<hw::InOutType>();
    if (!inoutType)
      return cast.emitError("operand type must be an inout type");
  } else {
    return cast.emitError("either source or result type must be analog type");
  }

  // The inout type must wrap an integer.
  auto integerType = inoutType.getElementType().dyn_cast<IntegerType>();
  if (!integerType)
    return cast.emitError("inout type must wrap an integer");

  int32_t width = firType.getBitWidthOrSentinel();
  if (width == -2)
    return cast.emitError("firrtl type isn't simple bit type");
  if (width == -1)
    return cast.emitError("Analog type must have a width");
  if (!integerType.isSignless())
    return cast.emitError("standard integer type must be signless");
  if (unsigned(width) != integerType.getWidth())
    return cast.emitError("source and result width must match");

  return success();
}

//===----------------------------------------------------------------------===//
// Conversions to/from structs in the standard dialect.
//===----------------------------------------------------------------------===//

static LogicalResult verifyHWStructCastOp(HWStructCastOp cast) {
  // We must have a bundle and a struct, with matching pairwise fields
  BundleType bundleType;
  hw::StructType structType;
  if ((bundleType = cast.getOperand().getType().dyn_cast<BundleType>())) {
    structType = cast.getType().dyn_cast<hw::StructType>();
    if (!structType)
      return cast.emitError("result type must be a struct");
  } else if ((bundleType = cast.getType().dyn_cast<BundleType>())) {
    structType = cast.getOperand().getType().dyn_cast<hw::StructType>();
    if (!structType)
      return cast.emitError("operand type must be a struct");
  } else {
    return cast.emitError("either source or result type must be a bundle type");
  }

  auto firFields = bundleType.getElements();
  auto hwFields = structType.getElements();
  if (firFields.size() != hwFields.size())
    return cast.emitError("bundle and struct have different number of fields");

  for (size_t findex = 0, fend = firFields.size(); findex < fend; ++findex) {
    if (firFields[findex].name.getValue() != hwFields[findex].name)
      return cast.emitError("field names don't match '")
             << firFields[findex].name.getValue() << "', '"
             << hwFields[findex].name << "'";
    int64_t firWidth =
        FIRRTLType(firFields[findex].type).getBitWidthOrSentinel();
    int64_t hwWidth = hw::getBitWidth(hwFields[findex].type);
    if (firWidth > 0 && hwWidth > 0 && firWidth != hwWidth)
      return cast.emitError("size of field '")
             << hwFields[findex].name << "' don't match " << firWidth << ", "
             << hwWidth;
  }

  return success();
}

void AsPassivePrimOp::build(OpBuilder &builder, OperationState &result,
                            Value input) {
  result.addOperands(input);
  result.addTypes(input.getType().cast<FIRRTLType>().getPassiveType());
}

//===----------------------------------------------------------------------===//
// ImplicitSSAName Custom Directive
//===----------------------------------------------------------------------===//

static ParseResult parseImplicitSSAName(OpAsmParser &parser,
                                        NamedAttrList &resultAttrs) {

  if (parser.parseOptionalAttrDict(resultAttrs))
    return failure();

  // If the attribute dictionary contains no 'name' attribute, infer it from
  // the SSA name (if specified).
  bool hadName = llvm::any_of(
      resultAttrs, [](NamedAttribute attr) { return attr.first == "name"; });

  // If there was no name specified, check to see if there was a useful name
  // specified in the asm file.
  if (hadName)
    return success();

  auto resultName = parser.getResultName(0).first;
  if (!resultName.empty() && isdigit(resultName[0]))
    resultName = "";
  auto nameAttr = parser.getBuilder().getStringAttr(resultName);
  auto *context = parser.getBuilder().getContext();
  resultAttrs.push_back({Identifier::get("name", context), nameAttr});
  return success();
}

static void printImplicitSSAName(OpAsmPrinter &p, Operation *op,
                                 DictionaryAttr attr) {
  // List of attributes to elide when printing the dictionary.
  SmallVector<StringRef, 2> elides;

  // Note that we only need to print the "name" attribute if the asmprinter
  // result name disagrees with it.  This can happen in strange cases, e.g.
  // when there are conflicts.
  SmallString<32> resultNameStr;
  llvm::raw_svector_ostream tmpStream(resultNameStr);
  p.printOperand(op->getResult(0), tmpStream);
  auto actualName = tmpStream.str().drop_front();
  auto expectedName = op->getAttrOfType<StringAttr>("name").getValue();
  // Anonymous names are printed as digits, which is fine.
  if (actualName == expectedName ||
      (expectedName.empty() && isdigit(actualName[0])))
    elides.push_back("name");

  // Elide "annotations" if it doesn't exist or if it is empty
  auto annotationsAttr = op->getAttrOfType<ArrayAttr>("annotations");
  if (!annotationsAttr || annotationsAttr.empty())
    elides.push_back("annotations");

  p.printOptionalAttrDict(op->getAttrs(), elides);
}

//===----------------------------------------------------------------------===//
// Custom attr-dict Directive that Elides Annotations
//===----------------------------------------------------------------------===//

static ParseResult parseElideAnnotations(OpAsmParser &parser,
                                         NamedAttrList &resultAttrs) {
  return parser.parseOptionalAttrDict(resultAttrs);
}

static void printElideAnnotations(OpAsmPrinter &p, Operation *op,
                                  DictionaryAttr attr) {
  // Elide "annotations" if it doesn't exist or if it is empty
  auto annotationsAttr = op->getAttrOfType<ArrayAttr>("annotations");
  if (!annotationsAttr || annotationsAttr.empty())
    return p.printOptionalAttrDict(op->getAttrs(), {"annotations"});

  p.printOptionalAttrDict(op->getAttrs());
}

//===----------------------------------------------------------------------===//
// InstanceOp Custom attr-dict Directive
//===----------------------------------------------------------------------===//

/// No change from normal parsing.
static ParseResult parseInstanceOp(OpAsmParser &parser,
                                   NamedAttrList &resultAttrs) {
  return parser.parseOptionalAttrDict(resultAttrs);
}

/// Always elide "moduleName" and elide "annotations" if it exists or
/// if it is empty.
static void printInstanceOp(OpAsmPrinter &p, Operation *op,
                            DictionaryAttr attr) {

  // "moduleName" is always elided
  SmallVector<StringRef, 2> elides = {"moduleName"};

  // Elide "annotations" if it doesn't exist or if it is empty
  auto annotationsAttr = op->getAttrOfType<ArrayAttr>("annotations");
  if (!annotationsAttr || annotationsAttr.empty())
    elides.push_back("annotations");

  p.printOptionalAttrDict(op->getAttrs(), elides);
}

//===----------------------------------------------------------------------===//
// MemOp Custom attr-dict Directive
//===----------------------------------------------------------------------===//

/// No change from normal parsing.
static ParseResult parseMemOp(OpAsmParser &parser, NamedAttrList &resultAttrs) {
  return parser.parseOptionalAttrDict(resultAttrs);
}

/// Always elide "ruw" and elide "annotations" if it exists or if it is empty.
static void printMemOp(OpAsmPrinter &p, Operation *op, DictionaryAttr attr) {
  SmallVector<StringRef, 2> elides = {"ruw"};

  auto annotationsAttr = op->getAttrOfType<ArrayAttr>("annotations");
  if (!annotationsAttr || annotationsAttr.empty())
    elides.push_back("annotations");

  p.printOptionalAttrDict(op->getAttrs(), elides);
}

//===----------------------------------------------------------------------===//
// Utilities related to Direction
//===----------------------------------------------------------------------===//

Direction direction::get(bool a) { return (Direction)a; }

IntegerAttr direction::packAttribute(ArrayRef<Direction> directions,
                                     MLIRContext *ctx) {

  // If the module contaions no ports (parameter a is empty), then use an APInt
  // of size 1 with value 0 to store the ports.  This works around an issue
  // where APInt cannot be zero-sized.  This aligns with port name storage which
  // will use a zero-element array.
  auto size = directions.size();
  if (size == 0)
    size = 1;

  // Pack the array of directions into an APInt.  Input is zero, output is one.
  APInt portDirections(size, 0);
  for (size_t i = 0, e = directions.size(); i != e; ++i)
    if (directions[i] == Direction::Output)
      portDirections.setBit(i);

  return IntegerAttr::get(IntegerType::get(ctx, size), portDirections);
}

/// Turn a packed representation of port attributes into a vector that can be
/// worked with.
SmallVector<Direction> direction::unpackAttribute(Operation *module) {
  const APInt &value =
      module->getAttr(direction::attrKey).cast<IntegerAttr>().getValue();

  SmallVector<Direction> result;

  // The integer attribute will be a single bit in the case where the module has
  // no ports because APInt can't hold zero bits.
  if (getModuleType(module).getInputs().empty())
    return result;

  result.reserve(value.getBitWidth());
  for (size_t i = 0, e = value.getBitWidth(); i != e; ++i)
    result.push_back(direction::get(value[i]));
  return result;
}

//===----------------------------------------------------------------------===//
// TblGen Generated Logic.
//===----------------------------------------------------------------------===//

// Provide the autogenerated implementation guts for the Op classes.
#define GET_OP_CLASSES
#include "circt/Dialect/FIRRTL/FIRRTL.cpp.inc"
