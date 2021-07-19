//===- FIRRTLOps.h - Declare FIRRTL dialect operations ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares the operation class for the FIRRTL IR.
//
//===----------------------------------------------------------------------===//

#ifndef CIRCT_DIALECT_FIRRTL_OPS_H
#define CIRCT_DIALECT_FIRRTL_OPS_H

#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "circt/Dialect/FIRRTL/FIRRTLDialect.h"
#include "circt/Dialect/FIRRTL/FIRRTLTypes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/FunctionSupport.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/IR/RegionKindInterface.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Interfaces/InferTypeOpInterface.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"

namespace circt {
namespace firrtl {

enum class Direction { Input = 0, Output };

template <typename T>
T &operator<<(T &os, const Direction &dir) {
  return os << (dir == Direction::Input ? "input" : "output");
}

namespace direction {

/// The key in a module's attribute dictionary used to find the direction.
static const char *const attrKey = "portDirections";

/// Return an output direction if \p isOutput is true, otherwise return an
/// input direction.
Direction get(bool isOutput);

/// Return a \p IntegerAttr containing the packed representation of an array of
/// directions.
IntegerAttr packAttribute(ArrayRef<Direction> a, MLIRContext *b);

/// Turn a packed representation of port attributes into a vector that can be
/// worked with.
SmallVector<Direction> unpackAttribute(Operation *module);

} // namespace direction

/// Return true if the specified operation is a firrtl expression.
bool isExpression(Operation *op);

/// Return true if the specified operation has a constant value. This trivially
/// checks for `firrtl.constant` and friends, but also looks through subaccesses
/// and correctly handles wires driven with only constant values.
bool isConstant(Operation *op);
bool isConstant(Value value);

/// This holds the name and type that describes the module's ports.
struct ModulePortInfo {
  StringAttr name;
  FIRRTLType type;
  Direction direction;
  Location loc;
  AnnotationSet annotations = AnnotationSet(type.getContext());

  StringRef getName() const { return name ? name.getValue() : ""; }

  /// Return true if this is a simple output-only port.  If you want the
  /// direction of the port, use the \p direction parameter.
  bool isOutput() {
    auto flags = type.getRecursiveTypeProperties();
    return flags.isPassive && !flags.containsAnalog &&
           direction == Direction::Output;
  }

  /// Return true if this is a simple input-only port.  If you want the
  /// direction of the port, use the \p direction parameter.
  bool isInput() {
    auto flags = type.getRecursiveTypeProperties();
    return flags.isPassive && !flags.containsAnalog &&
           direction == Direction::Input;
  }

  /// Return true if this is an inout port.  This will be true if the port
  /// contains either bi-directional signals or analog types.
  bool isInOut() { return !isOutput() && !isInput(); }
};

/// Return the function type that corresponds to a module.
FunctionType getModuleType(Operation *op);

/// This function can extract information about ports from a module and an
/// extmodule.
SmallVector<ModulePortInfo> getModulePortInfo(Operation *op);

/// Return the portNames attribute for the specified module, which contains the
/// name for each port.
ArrayAttr getModulePortNames(Operation *module);

/// Given an FModule or ExtModule, return the name of the specified port number.
StringAttr getModulePortName(Operation *op, size_t portIndex);

/// Return the portDirections attribute for the specified module, which contains
/// the direction for each port.
IntegerAttr getModulePortDirections(Operation *module);

/// Given an FModule or ExtModule, return the direction of the specified port
/// number.
Direction getModulePortDirection(Operation *op, size_t portIndex);

/// Returns true if the value results from an expression with duplex flow.
/// Duplex values have special treatment in bundle connect operations, and their
/// flip orientation is not used to determine the direction of each pairwise
/// connect.
bool isDuplexValue(Value val);

enum class Flow { Source, Sink, Duplex };

/// Get a flow's reverse.
Flow swapFlow(Flow flow);

/// Compute the flow for a Value, \p val, as determined by the FIRRTL
/// specification.  This recursively walks backwards from \p val to the
/// declaration.  The resulting flow is a combination of the declaration flow
/// (output ports and instance inputs are sinks, registers and wires are duplex,
/// anything else is a source) and the number of intermediary flips.  An even
/// number of flips will result in the same flow as the declaration.  An odd
/// number of flips will result in reversed flow being returned.  The reverse of
/// source is sink.  The reverse of sink is source.  The reverse of duplex is
/// duplex.  The \p accumulatedFlow parameter sets the initial flow.  A user
/// should normally \a not have to change this from its default of \p
/// Flow::Source.
Flow foldFlow(Value val, Flow accumulatedFlow = Flow::Source);

enum class DeclKind { Port, Instance, Other };

DeclKind getDeclarationKind(Value val);

// Out-of-line implementation of various trait verification methods and
// functions commonly used among operations.
namespace impl {
LogicalResult verifySameOperandsIntTypeKind(Operation *op);

// Type inference adaptor for FIRRTL operations.
LogicalResult inferReturnTypes(
    MLIRContext *context, Optional<Location> loc, ValueRange operands,
    DictionaryAttr attrs, mlir::RegionRange regions,
    SmallVectorImpl<Type> &results,
    llvm::function_ref<FIRRTLType(ValueRange, ArrayRef<NamedAttribute>,
                                  Optional<Location>)>
        callback);

// Common type inference functions.
FIRRTLType inferAddSubResult(FIRRTLType lhs, FIRRTLType rhs,
                             Optional<Location> loc);
FIRRTLType inferBitwiseResult(FIRRTLType lhs, FIRRTLType rhs,
                              Optional<Location> loc);
FIRRTLType inferComparisonResult(FIRRTLType lhs, FIRRTLType rhs,
                                 Optional<Location> loc);
FIRRTLType inferReductionResult(FIRRTLType arg, Optional<Location> loc);

// Common parsed argument validation functions.
LogicalResult validateBinaryOpArguments(ValueRange operands,
                                        ArrayRef<NamedAttribute> attrs,
                                        Location loc);
LogicalResult validateUnaryOpArguments(ValueRange operands,
                                       ArrayRef<NamedAttribute> attrs,
                                       Location loc);
LogicalResult validateOneOperandOneConst(ValueRange operands,
                                         ArrayRef<NamedAttribute> attrs,
                                         Location loc);
} // namespace impl

/// A binary operation where the operands have the same integer kind.
template <typename ConcreteOp>
class SameOperandsIntTypeKind
    : public OpTrait::TraitBase<ConcreteOp, SameOperandsIntTypeKind> {
public:
  static LogicalResult verifyTrait(Operation *op) {
    return impl::verifySameOperandsIntTypeKind(op);
  }
};

} // namespace firrtl
} // namespace circt

#define GET_OP_CLASSES
#include "circt/Dialect/FIRRTL/FIRRTL.h.inc"

#endif // CIRCT_DIALECT_FIRRTL_OPS_H
