//===- LLVM.h - Import and forward declare core LLVM types ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file forward declares and imports various common LLVM and MLIR datatypes
// that we want to use unqualified.
//
// Note that most of these are forward declared and then imported into the circt
// namespace with using decls, rather than being #included.  This is because we
// want clients to explicitly #include the files they need.
//
//===----------------------------------------------------------------------===//

#ifndef CIRCT_SUPPORT_LLVM_H
#define CIRCT_SUPPORT_LLVM_H

// MLIR includes a lot of forward declarations of LLVM types, use them.
#include "mlir/Support/LLVM.h"

// Can not forward declare inline functions with default arguments, so we
// include the header directly.
#include "mlir/Support/LogicalResult.h"

// Import classes from the `mlir` namespace into the `circt` namespace.  All of
// the following classes have been already forward declared and imported from
// `llvm` in to the `mlir` namespace. For classes with default template
// arguments, MLIR does not import the type directly, it creates a templated
// using statement. This is due to the limitiation that only one declaration of
// a type can have default arguments. For those types, it is important to import
// the MLIR version, and not the LLVM version. To keep things simple, all
// classes here should be imported from the `mlir` namespace, not the `llvm`
// namespace.
namespace circt {
using mlir::APFloat;
using mlir::APInt;
using mlir::APSInt;
using mlir::ArrayRef;
using mlir::cast;
using mlir::cast_or_null;
using mlir::DenseMap;
using mlir::DenseMapInfo;
using mlir::DenseSet;
using mlir::dyn_cast;
using mlir::dyn_cast_or_null;
using mlir::function_ref;
using mlir::isa;
using mlir::isa_and_nonnull;
using mlir::iterator_range;
using mlir::MutableArrayRef;
using mlir::None;
using mlir::Optional;
using mlir::PointerUnion;
using mlir::raw_ostream;
using mlir::SmallPtrSet;
using mlir::SmallPtrSetImpl;
using mlir::SmallString;
using mlir::SmallVector;
using mlir::SmallVectorImpl;
using mlir::StringLiteral;
using mlir::StringRef;
using mlir::StringSet;
using mlir::TinyPtrVector;
using mlir::Twine;
using mlir::TypeSwitch;
} // namespace circt

// Forward declarations of classes to be imported in to the circt namespace.
namespace mlir {
class ArrayAttr;
class Attribute;
class Block;
class BlockAndValueMapping;
class BlockArgument;
class BoolAttr;
class Builder;
class NamedAttrList;
class ConversionPattern;
class ConversionPatternRewriter;
class ConversionTarget;
class DenseElementsAttr;
class Diagnostic;
class Dialect;
class DialectAsmParser;
class DialectAsmPrinter;
class DictionaryAttr;
class ElementsAttr;
class FileLineColLoc;
class FlatSymbolRefAttr;
class FloatAttr;
class FunctionType;
class FusedLoc;
class Identifier;
class ImplicitLocOpBuilder;
class IndexType;
class InFlightDiagnostic;
class IntegerAttr;
class IntegerType;
class Location;
class MemRefType;
class MLIRContext;
class ModuleOp;
class MutableOperandRange;
class NamedAttrList;
class NoneType;
class OpAsmDialectInterface;
class OpAsmParser;
class OpAsmPrinter;
class OpBuilder;
class OperandRange;
class Operation;
class OpFoldResult;
class OpOperand;
class OpResult;
class OwningModuleRef;
class ParseResult;
class Pass;
class PatternRewriter;
class Region;
class RewritePatternSet;
class ShapedType;
class SplatElementsAttr;
class StringAttr;
class SymbolRefAttr;
class SymbolTable;
class TupleType;
class Type;
class TypeAttr;
class TypeConverter;
class TypeID;
class TypeRange;
class TypeStorage;
class UnknownLoc;
class Value;
class ValueRange;
class VectorType;
class WalkResult;
enum class RegionKind;
struct CallInterfaceCallable;
struct LogicalResult;
struct MemRefAccess;
struct OperationState;

template <typename SourceOp>
struct OpConversionPattern;
template <typename T>
class OperationPass;
template <typename SourceOp>
struct OpRewritePattern;

using DefaultTypeStorage = TypeStorage;
using OpAsmSetValueNameFn = function_ref<void(Value, StringRef)>;
using NamedAttribute = std::pair<Identifier, Attribute>;

namespace OpTrait {}

} // namespace mlir

// Import things we want into our namespace.
namespace circt {
using mlir::ArrayAttr;
using mlir::Attribute;
using mlir::Block;
using mlir::BlockAndValueMapping;
using mlir::BlockArgument;
using mlir::BoolAttr;
using mlir::Builder;
using mlir::CallInterfaceCallable;
using mlir::ConversionPattern;
using mlir::ConversionPatternRewriter;
using mlir::ConversionTarget;
using mlir::DefaultTypeStorage;
using mlir::DenseElementsAttr;
using mlir::Diagnostic;
using mlir::Dialect;
using mlir::DialectAsmParser;
using mlir::DialectAsmPrinter;
using mlir::DictionaryAttr;
using mlir::ElementsAttr;
using mlir::failed;
using mlir::failure;
using mlir::FileLineColLoc;
using mlir::FlatSymbolRefAttr;
using mlir::FloatAttr;
using mlir::FunctionType;
using mlir::FusedLoc;
using mlir::Identifier;
using mlir::ImplicitLocOpBuilder;
using mlir::IndexType;
using mlir::InFlightDiagnostic;
using mlir::IntegerAttr;
using mlir::IntegerType;
using mlir::Location;
using mlir::LogicalResult;
using mlir::MemRefAccess;
using mlir::MemRefType;
using mlir::MLIRContext;
using mlir::ModuleOp;
using mlir::MutableOperandRange;
using mlir::NamedAttribute;
using mlir::NamedAttrList;
using mlir::NoneType;
using mlir::OpAsmDialectInterface;
using mlir::OpAsmParser;
using mlir::OpAsmPrinter;
using mlir::OpAsmSetValueNameFn;
using mlir::OpBuilder;
using mlir::OpConversionPattern;
using mlir::OperandRange;
using mlir::Operation;
using mlir::OperationPass;
using mlir::OperationState;
using mlir::OpFoldResult;
using mlir::OpOperand;
using mlir::OpResult;
using mlir::OpRewritePattern;
using mlir::OwningModuleRef;
using mlir::ParseResult;
using mlir::Pass;
using mlir::PatternRewriter;
using mlir::Region;
using mlir::RegionKind;
using mlir::RewritePatternSet;
using mlir::ShapedType;
using mlir::SplatElementsAttr;
using mlir::StringAttr;
using mlir::succeeded;
using mlir::success;
using mlir::SymbolRefAttr;
using mlir::SymbolTable;
using mlir::TupleType;
using mlir::Type;
using mlir::TypeAttr;
using mlir::TypeConverter;
using mlir::TypeID;
using mlir::TypeRange;
using mlir::TypeStorage;
using mlir::UnknownLoc;
using mlir::Value;
using mlir::ValueRange;
using mlir::VectorType;
using mlir::WalkResult;
namespace OpTrait = mlir::OpTrait;
} // namespace circt

#endif // CIRCT_SUPPORT_LLVM_H
