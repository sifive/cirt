//===- ExportVerilog.cpp - Verilog Emitter --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This is the main Verilog emitter implementation.
//
//===----------------------------------------------------------------------===//

#include "circt/Translation/ExportVerilog.h"
#include "circt/Dialect/Comb/CombDialect.h"
#include "circt/Dialect/Comb/CombVisitors.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/HW/HWTypes.h"
#include "circt/Dialect/HW/HWVisitors.h"
#include "circt/Dialect/SV/SVAttributes.h"
#include "circt/Dialect/SV/SVOps.h"
#include "circt/Dialect/SV/SVVisitors.h"
#include "circt/Support/LLVM.h"
#include "circt/Support/LoweringOptions.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/ImplicitLocOpBuilder.h"
#include "mlir/Support/FileUtilities.h"
#include "mlir/Translation.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Parallel.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"

using namespace circt;

using namespace comb;
using namespace hw;
using namespace sv;

#define DEBUG_TYPE "export-verilog"

//===----------------------------------------------------------------------===//
// Helper routines
//===----------------------------------------------------------------------===//

/// Return true for operations that are always inlined into a containing
/// expression.
static bool isExpressionAlwaysInline(Operation *op) {
  // We need to emit array indexes inline per verilog "lvalue" semantics.
  if (isa<ArrayIndexInOutOp>(op))
    return true;

  // An SV interface modport is a symbolic name that is always inlined.
  if (isa<GetModportOp>(op) || isa<ReadInterfaceSignalOp>(op))
    return true;

  return false;
}

/// Return whether an operation is a constant.
static bool isConstantExpression(Operation *op) {
  return isa<ConstantOp>(op) || isa<ConstantXOp>(op) || isa<ConstantZOp>(op);
}

/// Return true for nullary operations that are better emitted multiple
/// times as inline expression (when they have multiple uses) rather than having
/// a temporary wire.
///
/// This can only handle nullary expressions, because we don't want to replicate
/// subtrees arbitrarily.
static bool isDuplicatableNullaryExpression(Operation *op) {
  // We don't want wires that are just constants aesthetically.
  if (isConstantExpression(op))
    return true;

  // If this is a small verbatim expression, keep it inline.
  if (isa<VerbatimExprOp, VerbatimExprSEOp>(op)) {
    if (op->getNumOperands() == 0 &&
        op->getAttrOfType<StringAttr>("string").getValue().size() <= 16)
      return true;
  }

  return false;
}

static bool isVerilogExpression(Operation *op) {
  // These are SV dialect expressions.
  if (isa<ReadInOutOp>(op) || isa<ArrayIndexInOutOp>(op))
    return true;

  // All HW combinatorial logic ops and SV expression ops are Verilog
  // expressions.
  return isCombinatorial(op) || isExpression(op);
}

/// Return the width of the specified type in bits or -1 if it isn't
/// supported.
static int getBitWidthOrSentinel(Type type) {
  return TypeSwitch<Type, int>(type)
      .Case<IntegerType>([](IntegerType integerType) {
        // Verilog doesn't support zero bit integers.  We only support them in
        // limited cases.
        return integerType.getWidth();
      })
      .Case<InOutType>([](InOutType inoutType) {
        return getBitWidthOrSentinel(inoutType.getElementType());
      })
      .Case<TypeAliasType>([](TypeAliasType alias) {
        return getBitWidthOrSentinel(alias.getInnerType());
      })
      .Default([](Type) { return -1; });
}

/// Push this type's dimension into a vector.
static void getTypeDims(SmallVectorImpl<int64_t> &dims, Type type,
                        Location loc) {
  if (auto inout = type.dyn_cast<hw::InOutType>())
    return getTypeDims(dims, inout.getElementType(), loc);
  if (auto uarray = type.dyn_cast<hw::UnpackedArrayType>())
    return getTypeDims(dims, uarray.getElementType(), loc);
  if (type.isa<InterfaceType>())
    return;
  if (hw::type_isa<StructType>(type))
    return;

  int width;
  if (auto arrayType = hw::type_dyn_cast<hw::ArrayType>(type)) {
    width = arrayType.getSize();
  } else {
    width = getBitWidthOrSentinel(type);
  }
  if (width == -1)
    mlir::emitError(loc, "value has an unsupported verilog type ") << type;

  if (width != 1) // Width 1 is implicit.
    dims.push_back(width);

  if (auto arrayType = type.dyn_cast<hw::ArrayType>()) {
    getTypeDims(dims, arrayType.getElementType(), loc);
  }
}

/// Emit a list of dimensions.
static void emitDims(ArrayRef<int64_t> dims, raw_ostream &os) {
  for (int64_t width : dims)
    switch (width) {
    case -1: // -1 is an invalid type.
      os << "<<invalid type>>";
      return;
    case 0:
      os << "/*Zero Width*/";
      break;
    default:
      os << '[' << (width - 1) << ":0]";
      break;
    }
}

/// Emit a type's packed dimensions, returning whether or not text was emitted.
static void emitTypeDims(Type type, Location loc, raw_ostream &os) {
  SmallVector<int64_t, 4> dims;
  getTypeDims(dims, type, loc);
  emitDims(dims, os);
}

/// True iff 'a' and 'b' have the same wire dims.
static bool haveMatchingDims(Type a, Type b, Location loc) {
  SmallVector<int64_t, 4> aDims;
  getTypeDims(aDims, a, loc);

  SmallVector<int64_t, 4> bDims;
  getTypeDims(bDims, b, loc);

  return aDims == bDims;
}

/// Return true if this is a zero bit type, e.g. a zero bit integer or array
/// thereof.
static bool isZeroBitType(Type type) {
  if (auto intType = type.dyn_cast<IntegerType>())
    return intType.getWidth() == 0;
  if (auto inout = type.dyn_cast<hw::InOutType>())
    return isZeroBitType(inout.getElementType());
  if (auto uarray = type.dyn_cast<hw::UnpackedArrayType>())
    return isZeroBitType(uarray.getElementType());
  if (auto array = type.dyn_cast<hw::ArrayType>())
    return isZeroBitType(array.getElementType());

  // We have an open type system, so assume it is ok.
  return false;
}

/// Given a set of known nested types (those supported by this pass), strip off
/// leading unpacked types.  This strips off portions of the type that are
/// printed to the right of the name in verilog.
static Type stripUnpackedTypes(Type type) {
  return TypeSwitch<Type, Type>(type)
      .Case<InOutType>([](InOutType inoutType) {
        return stripUnpackedTypes(inoutType.getElementType());
      })
      .Case<UnpackedArrayType>([](UnpackedArrayType arrayType) {
        return stripUnpackedTypes(arrayType.getElementType());
      })
      .Default([](Type type) { return type; });
}

/// Output the basic type that consists of packed and primitive types.  This is
/// those to the left of the name in verilog. implicitIntType controls whether
/// to print a base type for (logic) for inteters or whether the caller will
/// have handled this (with logic, wire, reg, etc).
/// Returns whether anything was printed out
static bool printPackedTypeImpl(Type type, raw_ostream &os, Operation *op,
                                SmallVectorImpl<int64_t> &dims,
                                bool implicitIntType) {
  return TypeSwitch<Type, bool>(type)
      .Case<IntegerType>([&](IntegerType integerType) {
        if (!implicitIntType)
          os << "logic";
        if (integerType.getWidth() != 1)
          dims.push_back(integerType.getWidth());
        if (!dims.empty() && !implicitIntType)
          os << ' ';

        emitDims(dims, os);
        return !dims.empty() || !implicitIntType;
      })
      .Case<InOutType>([&](InOutType inoutType) {
        return printPackedTypeImpl(inoutType.getElementType(), os, op, dims,
                                   implicitIntType);
      })
      .Case<StructType>([&](StructType structType) {
        os << "struct packed {";
        for (auto &element : structType.getElements()) {
          SmallVector<int64_t, 8> structDims;
          printPackedTypeImpl(stripUnpackedTypes(element.type), os, op,
                              structDims, /*implicitIntType=*/false);
          os << ' ' << element.name << "; ";
        }
        os << '}';
        emitDims(dims, os);
        return true;
      })
      .Case<ArrayType>([&](ArrayType arrayType) {
        dims.push_back(arrayType.getSize());
        return printPackedTypeImpl(arrayType.getElementType(), os, op, dims,
                                   implicitIntType);
      })
      .Case<InterfaceType>([](InterfaceType ifaceType) { return false; })
      .Case<UnpackedArrayType>([&](UnpackedArrayType arrayType) {
        os << "<<unexpected unpacked array>>";
        op->emitError("Unexpected unpacked array in packed type ") << arrayType;
        return true;
      })
      .Case<TypeAliasType>([&](TypeAliasType typeRef) {
        auto typedecl = typeRef.getDecl(op);
        if (!typedecl.hasValue())
          return false;

        os << typedecl.getValue().getPreferredName();
        emitDims(dims, os);
        return true;
      })
      .Default([&](Type type) {
        os << "<<invalid type>>";
        op->emitError("value has an unsupported verilog type ") << type;
        return true;
      });
}

static bool printPackedType(Type type, raw_ostream &os, Operation *op,
                            bool implicitIntType = true) {
  SmallVector<int64_t, 8> packedDimensions;
  return printPackedTypeImpl(type, os, op, packedDimensions, implicitIntType);
}

/// Output the unpacked array dimensions.  This is the part of the type that is
/// to the right of the name.
static void printUnpackedTypePostfix(Type type, raw_ostream &os) {
  TypeSwitch<Type, void>(type)
      .Case<InOutType>([&](InOutType inoutType) {
        printUnpackedTypePostfix(inoutType.getElementType(), os);
      })
      .Case<UnpackedArrayType>([&](UnpackedArrayType arrayType) {
        printUnpackedTypePostfix(arrayType.getElementType(), os);
        os << "[0:" << (arrayType.getSize() - 1) << "]";
      });
}

/// Return the word (e.g. "reg") in Verilog to declare the specified thing.
static StringRef getVerilogDeclWord(Operation *op) {
  if (isa<RegOp>(op)) {
    // Check if the type stored in this register is a struct or array of
    // structs. In this case, according to spec section 6.8, the "reg" prefix
    // should be left off.
    auto elementType =
        op->getResult(0).getType().cast<InOutType>().getElementType();
    if (elementType.isa<StructType>())
      return "";
    if (auto innerType = elementType.dyn_cast<ArrayType>()) {
      while (innerType.getElementType().isa<ArrayType>())
        innerType = innerType.getElementType().cast<ArrayType>();
      if (innerType.getElementType().isa<StructType>() ||
          innerType.getElementType().isa<TypeAliasType>())
        return "";
    }
    if (elementType.isa<TypeAliasType>())
      return "";

    return "reg";
  }
  if (isa<WireOp>(op))
    return "wire";
  if (isa<ConstantOp>(op))
    return "localparam";

  // Interfaces instances use the name of the declared interface.
  if (auto interface = dyn_cast<InterfaceInstanceOp>(op))
    return interface.getInterfaceType().getInterface().getValue();

  // If 'op' is in a module, output 'wire'. If 'op' is in a procedural block,
  // fall through to default.
  bool isProcedural = op->getParentOp()->hasTrait<ProceduralRegion>();
  return isProcedural ? "automatic logic" : "wire";
}
/// Return the name of a value without using the name map.  This is needed when
/// looking into an instance from a different module as happens with bind.  It
/// may return "" when unable to determine a name.  This works in situations
/// where names are pre-legalized during prepare.
static StringRef getNameRemotely(Value &value,
                                 ArrayRef<ModulePortInfo> modulePorts) {
  if (auto barg = value.dyn_cast<BlockArgument>()) {
    return modulePorts[barg.getArgNumber()].getName();
  }
  if (auto readinout = dyn_cast<ReadInOutOp>(value.getDefiningOp())) {
    if (auto wire = dyn_cast<WireOp>(readinout.input().getDefiningOp())) {
      return wire.name();
    }
    if (auto reg = dyn_cast<RegOp>(readinout.input().getDefiningOp())) {
      return reg.name();
    }
  }
  return {};
}

namespace {
/// This enum keeps track of the precedence level of various binary operators,
/// where a lower number binds tighter.
enum VerilogPrecedence {
  // Normal precedence levels.
  Symbol,          // Atomic symbol like "foo" and {a,b}
  Selection,       // () , [] , :: , .
  Unary,           // Unary operators like ~foo
  Multiply,        // * , / , %
  Addition,        // + , -
  Shift,           // << , >>
  Comparison,      // > , >= , < , <=
  Equality,        // == , !=
  And,             // &
  Xor,             // ^ , ^~
  Or,              // |
  AndShortCircuit, // &&
  Conditional,     // ? :

  LowestPrecedence,  // Sentinel which is always the lowest precedence.
  ForceEmitMultiUse, // Sentinel saying to recursively emit a multi-used expr.
};
} // end anonymous namespace

/// Return the location information as a (potentially empty) string.
static std::string
getLocationInfoAsString(const SmallPtrSet<Operation *, 8> &ops) {
  std::string resultStr;
  llvm::raw_string_ostream sstr(resultStr);

  // Multiple operations may come from the same location or may not have useful
  // location info.  Unique it now.
  SmallPtrSet<Attribute, 8> locations;
  for (auto *op : ops) {
    if (auto loc = op->getLoc().dyn_cast<FileLineColLoc>())
      locations.insert(loc);
  }

  auto printLoc = [&](FileLineColLoc loc) {
    sstr << loc.getFilename();
    if (auto line = loc.getLine()) {
      sstr << ':' << line;
      if (auto col = loc.getColumn())
        sstr << ':' << col;
    }
  };

  switch (locations.size()) {
  case 1:
    printLoc((*locations.begin()).cast<FileLineColLoc>());
    LLVM_FALLTHROUGH;
  case 0:
    return sstr.str();
  default:
    break;
  }

  // Sort the entries.
  SmallVector<FileLineColLoc, 8> locVector;
  locVector.reserve(locations.size());
  for (auto loc : locations)
    locVector.push_back(loc.cast<FileLineColLoc>());

  llvm::array_pod_sort(
      locVector.begin(), locVector.end(),
      [](const FileLineColLoc *lhs, const FileLineColLoc *rhs) -> int {
        if (auto fn = lhs->getFilename().compare(rhs->getFilename()))
          return fn;
        if (lhs->getLine() != rhs->getLine())
          return lhs->getLine() < rhs->getLine() ? -1 : 1;
        return lhs->getColumn() < rhs->getColumn() ? -1 : 1;
      });

  // The entries are sorted by filename, line, col.  Try to merge together
  // entries to reduce verbosity on the column info.
  StringRef lastFileName;
  for (size_t i = 0, e = locVector.size(); i != e;) {
    if (i != 0)
      sstr << ", ";

    // Print the filename if it changed.
    auto first = locVector[i];
    if (first.getFilename() != lastFileName) {
      lastFileName = first.getFilename();
      sstr << lastFileName;
    }

    // Scan for entries with the same file/line.
    size_t end = i + 1;
    while (end != e && first.getFilename() == locVector[end].getFilename() &&
           first.getLine() == locVector[end].getLine())
      ++end;

    // If we have one entry, print it normally.
    if (end == i + 1) {
      if (auto line = first.getLine()) {
        sstr << ':' << line;
        if (auto col = first.getColumn())
          sstr << ':' << col;
      }
      ++i;
      continue;
    }

    // Otherwise print a brace enclosed list.
    sstr << ':' << first.getLine() << ":{";
    while (i != end) {
      sstr << locVector[i++].getColumn();

      if (i != end)
        sstr << ',';
    }
    sstr << '}';
  }

  return sstr.str();
}

/// Append a path to an existing path, replacing it if the other path is
/// absolute. This mimicks the behaviour of `foo/bar` and `/foo/bar` being used
/// in a working directory `/home`, resulting in `/home/foo/bar` and `/foo/bar`,
/// respectively.
// TODO: This also exists in BlackBoxReader.cpp. Maybe we should move this to
// some CIRCT-wide file system utility source file?
static void appendPossiblyAbsolutePath(SmallVectorImpl<char> &base,
                                       const Twine &suffix) {
  if (llvm::sys::path::is_absolute(suffix)) {
    base.clear();
    suffix.toVector(base);
  } else {
    llvm::sys::path::append(base, suffix);
  }
}

//===----------------------------------------------------------------------===//
// VerilogEmitter
//===----------------------------------------------------------------------===//

namespace {

/// This class maintains the mutable state that cross-cuts and is shared by the
/// various emitters.
class VerilogEmitterState {
public:
  explicit VerilogEmitterState(raw_ostream &os) : os(os) {}

  /// The emitter options which control verilog emission.
  LoweringOptions options;

  /// The stream to emit to.
  raw_ostream &os;

  bool encounteredError = false;
  unsigned currentIndent = 0;

private:
  VerilogEmitterState(const VerilogEmitterState &) = delete;
  void operator=(const VerilogEmitterState &) = delete;
};
} // namespace

//===----------------------------------------------------------------------===//
// NameManager
//===----------------------------------------------------------------------===//

namespace {
struct ModuleNameManager {
  ModuleNameManager() : encounteredError(false) {}

  /// Add the specified name to the name table, auto-uniquing the name if
  /// required.  If the name is empty, then this creates a unique temp name.
  StringRef addName(StringRef name) {
    return legalizeName(name, usedNames, nextGeneratedNameID);
  }

  StringRef addName(Value value, StringRef name) {
    return addName(ValueOrOp(value), name);
  }
  StringRef addName(Operation *op, StringRef name) {
    return addName(ValueOrOp(op), name);
  }
  StringRef addName(Value value, StringAttr name) {
    return addName(ValueOrOp(value), name);
  }
  StringRef addName(Operation *op, StringAttr name) {
    return addName(ValueOrOp(op), name);
  }

  StringRef addLegalName(Value value, StringRef name, Operation *errOp) {
    return addLegalName(ValueOrOp(value), name, errOp);
  }
  StringRef addLegalName(Operation *op, StringRef name, Operation *errOp) {
    return addLegalName(ValueOrOp(op), name, errOp);
  }

  StringRef getName(Value value) { return getName(ValueOrOp(value)); }
  StringRef getName(Operation *op) { return getName(ValueOrOp(op)); }

  bool hasName(Value value) { return nameTable.count(ValueOrOp(value)); }

  void addOutputNames(StringRef name, Operation *module) {
    outputNames.push_back(addLegalName(nullptr, name, module));
  }

  StringRef getOutputName(size_t portNum) { return outputNames[portNum]; }

  bool hadError() { return encounteredError; }

private:
  using ValueOrOp = PointerUnion<Value, Operation *>;

  /// Retrieve a name from the name table.  The name must already have been
  /// added.
  StringRef getName(ValueOrOp valueOrOp) {
    auto entry = nameTable.find(valueOrOp);
    assert(entry != nameTable.end() &&
           "value expected a name but doesn't have one");
    return entry->getSecond();
  }

  /// Add the specified name to the name table, auto-uniquing the name if
  /// required.  If the name is empty, then this creates a unique temp name.
  ///
  /// "valueOrOp" is typically the Value for an intermediate wire etc, but it
  /// can also be an op for an instance, since we want the instances op uniqued
  /// and tracked.  It can also be null for things like outputs which are not
  /// tracked in the nameTable.
  StringRef addName(ValueOrOp valueOrOp, StringRef name) {
    auto updatedName = addName(name);
    if (valueOrOp)
      nameTable[valueOrOp] = updatedName;
    return updatedName;
  }

  StringRef addName(ValueOrOp valueOrOp, StringAttr nameAttr) {
    return addName(valueOrOp, nameAttr ? nameAttr.getValue() : "");
  }
  /// Add the specified name to the name table, emitting an error message if the
  /// name empty or is changed by uniqing.
  StringRef addLegalName(ValueOrOp valueOrOp, StringRef name, Operation *op) {
    auto updatedName = addName(valueOrOp, name);
    if (name.empty())
      emitOpError(op, "should have non-empty name");
    else if (updatedName != name)
      emitOpError(op, "name '") << name << "' changed during emission";
    return updatedName;
  }

  InFlightDiagnostic emitOpError(Operation *op, const Twine &message) {
    encounteredError = true;
    return op->emitOpError(message);
  }

  // Track whether a name error ocurred
  bool encounteredError;

  /// nameTable keeps track of mappings from Value's and operations (for
  /// instances) to their string table entry.
  llvm::DenseMap<ValueOrOp, StringRef> nameTable;

  /// outputNames tracks the uniquified names for output ports, which don't
  /// have
  /// a Value or Op representation.
  SmallVector<StringRef> outputNames;

  llvm::StringSet<> usedNames;

  size_t nextGeneratedNameID = 0;
};
} // namespace

//===----------------------------------------------------------------------===//
// EmitterBase
//===----------------------------------------------------------------------===//
namespace {

class EmitterBase {
public:
  // All of the mutable state we are maintaining.
  VerilogEmitterState &state;

  /// The stream to emit to.
  raw_ostream &os;

  EmitterBase(VerilogEmitterState &state, raw_ostream &os)
      : state(state), os(os) {}
  explicit EmitterBase(VerilogEmitterState &state)
      : EmitterBase(state, state.os) {}

  InFlightDiagnostic emitError(Operation *op, const Twine &message) {
    state.encounteredError = true;
    return op->emitError(message);
  }

  InFlightDiagnostic emitOpError(Operation *op, const Twine &message) {
    state.encounteredError = true;
    return op->emitOpError(message);
  }

  raw_ostream &indent() { return os.indent(state.currentIndent); }

  void addIndent() { state.currentIndent += 2; }
  void reduceIndent() { state.currentIndent -= 2; }

  /// If we have location information for any of the specified operations,
  /// aggregate it together and print a pretty comment specifying where the
  /// operations came from.  In any case, print a newline.
  void emitLocationInfoAndNewLine(const SmallPtrSet<Operation *, 8> &ops) {
    auto locInfo = getLocationInfoAsString(ops);
    if (!locInfo.empty())
      os << "\t// " << locInfo;
    os << '\n';
  }

  void emitTextWithSubstitutions(StringRef string, Operation *op,
                                 std::function<void(Value)> operandEmitter);

private:
  void operator=(const EmitterBase &) = delete;
  EmitterBase(const EmitterBase &) = delete;
};
} // end anonymous namespace

void EmitterBase::emitTextWithSubstitutions(
    StringRef string, Operation *op,
    std::function<void(Value)> operandEmitter) {
  // Perform operand substitions as we emit the line string.  We turn {{42}}
  // into the value of operand 42.

  // Scan 'line' for a substitution, emitting any non-substitution prefix,
  // then the mentioned operand, chopping the relevant text off 'line' and
  // returning true.  This returns false if no substitution is found.
  auto emitUntilSubstitution = [&](size_t next = 0) -> bool {
    size_t start = 0;
    while (1) {
      next = string.find("{{", next);
      if (next == StringRef::npos)
        return false;

      // Check to make sure we have a number followed by }}.  If not, we
      // ignore the {{ sequence as something that could happen in Verilog.
      next += 2;
      start = next;
      while (next < string.size() && isdigit(string[next]))
        ++next;
      // We need at least one digit.
      if (start == next)
        continue;

      // We must have a }} right after the digits.
      if (!string.substr(next).startswith("}}"))
        continue;

      // We must be able to decode the integer into an unsigned.
      unsigned operandNo = 0;
      if (string.drop_front(start)
              .take_front(next - start)
              .getAsInteger(10, operandNo)) {
        emitError(op, "operand substitution too large");
        continue;
      }
      next += 2;

      if (operandNo >= op->getNumOperands()) {
        emitError(op, "operand " + llvm::utostr(operandNo) + " isn't valid");
        continue;
      }

      // Emit any text before the substitution.
      os << string.take_front(start - 2);

      // Emit the operand.
      operandEmitter(op->getOperand(operandNo));

      // Forget about the part we emitted.
      string = string.drop_front(next);
      return true;
    }
  };

  // Emit all the substitutions.
  while (emitUntilSubstitution())
    ;

  // Emit any text after the last substitution.
  os << string;
}

//===----------------------------------------------------------------------===//
// ModuleEmitter
//===----------------------------------------------------------------------===//

namespace {

class ModuleEmitter : public EmitterBase {
public:
  explicit ModuleEmitter(VerilogEmitterState &state) : EmitterBase(state) {}

  void emitHWModule(HWModuleOp module, ModuleNameManager &names);
  void emitHWExternModule(HWModuleExternOp module);
  void emitHWGeneratedModule(HWModuleGeneratedOp module);

  // Statements.
  void emitStatement(Operation *op, ModuleNameManager &names);
  void emitStatementBlock(Block &block, ModuleNameManager &names);
  void emitBind(BindOp op);

public:
  void verifyModuleName(Operation *, StringAttr nameAttr);

  /// This set keeps track of all of the expression nodes that need to be
  /// emitted as standalone wire declarations.  This can happen because they are
  /// multiply-used or because the user requires a name to reference.
  SmallPtrSet<Operation *, 16> outOfLineExpressions;
};

} // end anonymous namespace

/// Check if the given module name \p nameAttr is a valid SV name (does not
/// contain any illegal characters). If invalid, calls \c emitOpError.
void ModuleEmitter::verifyModuleName(Operation *op, StringAttr nameAttr) {
  if (!isNameValid(nameAttr.getValue()))
    emitOpError(op, "name \"" + nameAttr.getValue() +
                        "\" is not allowed in Verilog output");
}

//===----------------------------------------------------------------------===//
// Expression Emission
//===----------------------------------------------------------------------===//

namespace {

/// This enum keeps track of whether the emitted subexpression is signed or
/// unsigned as seen from the Verilog language perspective.
enum SubExprSignResult { IsSigned, IsUnsigned };

/// This enum keeps track of whether the emitted subexpression was a constant,
/// a wire, or anything more complex (like a parenthesized expression).
enum class SubExprKind { Const, Wire, Other };

/// This is information precomputed about each subexpression in the tree we
/// are emitting as a unit.
struct SubExprInfo {
  /// The precedence of this expression.
  VerilogPrecedence precedence;

  /// The signedness of the expression.
  SubExprSignResult signedness;

  /// The kind of this expression.
  SubExprKind kind = SubExprKind::Other;

  SubExprInfo(VerilogPrecedence precedence, SubExprSignResult signedness,
              SubExprKind kind = SubExprKind::Other)
      : precedence(precedence), signedness(signedness), kind(kind) {}
};

} // namespace

namespace {
/// This builds a recursively nested expression from an SSA use-def graph.  This
/// uses a post-order walk, but it needs to obey precedence and signedness
/// constraints that depend on the behavior of the child nodes.  To handle this,
/// we emit the characters to a SmallVector which allows us to emit a bunch of
/// stuff, then pre-insert parentheses and other things if we find out that it
/// was needed later.
class ExprEmitter : public EmitterBase,
                    public TypeOpVisitor<ExprEmitter, SubExprInfo>,
                    public CombinationalVisitor<ExprEmitter, SubExprInfo>,
                    public Visitor<ExprEmitter, SubExprInfo> {
public:
  /// Create an ExprEmitter for the specified module emitter, and keeping track
  /// of any emitted expressions in the specified set.  If any subexpressions
  /// are too large to emit, then they are added into tooLargeSubExpressions to
  /// be emitted independently by the caller.
  ExprEmitter(ModuleEmitter &emitter, SmallVectorImpl<char> &outBuffer,
              SmallPtrSet<Operation *, 8> &emittedExprs,
              SmallVectorImpl<Operation *> &tooLargeSubExpressions,
              ModuleNameManager &names)
      : EmitterBase(emitter.state, os), emitter(emitter),
        emittedExprs(emittedExprs),
        tooLargeSubExpressions(tooLargeSubExpressions), outBuffer(outBuffer),
        os(outBuffer), names(names) {}

  /// Emit the specified value as an expression.  If this is an inline-emitted
  /// expression, we emit that expression, otherwise we emit a reference to the
  /// already computed name.
  ///
  SubExprInfo emitExpression(Value exp,
                             VerilogPrecedence parenthesizeIfLooserThan) {
    // Emit the expression.
    return emitSubExpr(exp, parenthesizeIfLooserThan, OOLTopLevel,
                       /*signRequirement*/ NoRequirement);
  }

private:
  friend class TypeOpVisitor<ExprEmitter, SubExprInfo>;
  friend class CombinationalVisitor<ExprEmitter, SubExprInfo>;
  friend class Visitor<ExprEmitter, SubExprInfo>;

  enum SubExprSignRequirement { NoRequirement, RequireSigned, RequireUnsigned };
  enum SubExprOutOfLineBehavior {
    OOLTopLevel, //< Top level expressions shouldn't be emitted out of line.
    OOLUnary,    //< Unary expressions are more generous on line lengths.
    OOLBinary    //< Binary expressions split easily.
  };

  /// Emit the specified value as a subexpression to the stream.
  SubExprInfo emitSubExpr(Value exp, VerilogPrecedence parenthesizeIfLooserThan,
                          SubExprOutOfLineBehavior outOfLineBehavior,
                          SubExprSignRequirement signReq = NoRequirement);

  void retroactivelyEmitExpressionIntoTemporary(Operation *op);

  SubExprInfo visitUnhandledExpr(Operation *op);
  SubExprInfo visitInvalidComb(Operation *op) {
    return dispatchTypeOpVisitor(op);
  }
  SubExprInfo visitUnhandledComb(Operation *op) {
    return visitUnhandledExpr(op);
  }
  SubExprInfo visitInvalidTypeOp(Operation *op) {
    return dispatchSVVisitor(op);
  }
  SubExprInfo visitUnhandledTypeOp(Operation *op) {
    return visitUnhandledExpr(op);
  }
  SubExprInfo visitUnhandledSV(Operation *op) { return visitUnhandledExpr(op); }

  using Visitor::visitSV;

  SubExprInfo emitBinary(Operation *op, VerilogPrecedence prec,
                         const char *syntax,
                         SubExprSignRequirement operandSignReq = NoRequirement);

  SubExprInfo emitVariadic(Operation *op, VerilogPrecedence prec,
                           const char *syntax);

  SubExprInfo emitUnary(Operation *op, const char *syntax,
                        bool resultAlwaysUnsigned = false);

  SubExprInfo visitSV(GetModportOp op);
  SubExprInfo visitSV(ReadInterfaceSignalOp op);
  SubExprInfo visitVerbatimExprOp(Operation *op);
  SubExprInfo visitSV(VerbatimExprOp op) { return visitVerbatimExprOp(op); }
  SubExprInfo visitSV(VerbatimExprSEOp op) { return visitVerbatimExprOp(op); }
  SubExprInfo visitSV(ConstantXOp op);
  SubExprInfo visitSV(ConstantZOp op);

  // Noop cast operators.
  SubExprInfo visitSV(ReadInOutOp op) {
    return emitSubExpr(op->getOperand(0), LowestPrecedence, OOLUnary);
  }
  SubExprInfo visitSV(ArrayIndexInOutOp op);

  // Other
  using TypeOpVisitor::visitTypeOp;
  SubExprInfo visitTypeOp(ConstantOp op);
  SubExprInfo visitTypeOp(BitcastOp op);
  SubExprInfo visitTypeOp(ArraySliceOp op);
  SubExprInfo visitTypeOp(ArrayGetOp op);
  SubExprInfo visitTypeOp(ArrayCreateOp op);
  SubExprInfo visitTypeOp(ArrayConcatOp op);
  SubExprInfo visitTypeOp(StructCreateOp op);
  SubExprInfo visitTypeOp(StructExtractOp op);
  SubExprInfo visitTypeOp(StructInjectOp op);

  // Comb Dialect Operations
  using CombinationalVisitor::visitComb;
  SubExprInfo visitComb(MuxOp op);
  SubExprInfo visitComb(AddOp op) { return emitVariadic(op, Addition, "+"); }
  SubExprInfo visitComb(SubOp op) { return emitBinary(op, Addition, "-"); }
  SubExprInfo visitComb(MulOp op) { return emitVariadic(op, Multiply, "*"); }
  SubExprInfo visitComb(DivUOp op) {
    return emitBinary(op, Multiply, "/", RequireUnsigned);
  }
  SubExprInfo visitComb(DivSOp op) {
    return emitBinary(op, Multiply, "/", RequireSigned);
  }
  SubExprInfo visitComb(ModUOp op) {
    return emitBinary(op, Multiply, "%", RequireUnsigned);
  }
  SubExprInfo visitComb(ModSOp op) {
    return emitBinary(op, Multiply, "%", RequireSigned);
  }
  SubExprInfo visitComb(ShlOp op) { return emitBinary(op, Shift, "<<"); }
  SubExprInfo visitComb(ShrUOp op) {
    // >> in Verilog is always an unsigned right shift.
    return emitBinary(op, Shift, ">>");
  }
  SubExprInfo visitComb(ShrSOp op) {
    // >>> is only an arithmetic shift right when both operands are signed.
    // Otherwise it does a logical shift.
    return emitBinary(op, Shift, ">>>", RequireSigned);
  }
  SubExprInfo visitComb(AndOp op) { return emitVariadic(op, And, "&"); }
  SubExprInfo visitComb(OrOp op) { return emitVariadic(op, Or, "|"); }
  SubExprInfo visitComb(XorOp op) {
    if (op.isBinaryNot())
      return emitUnary(op, "~");

    return emitVariadic(op, Xor, "^");
  }

  // SystemVerilog spec 11.8.1: "Reduction operator results are unsigned,
  // regardless of the operands."
  SubExprInfo visitComb(ParityOp op) { return emitUnary(op, "^", true); }

  SubExprInfo visitComb(SExtOp op);
  SubExprInfo visitComb(ConcatOp op);
  SubExprInfo visitComb(ExtractOp op);
  SubExprInfo visitComb(ICmpOp op);

public:
  ModuleEmitter &emitter;

private:
  /// This is set (before a visit method is called) if emitSubExpr would
  /// prefer to get an output of a specific sign.  This is a hint to cause the
  /// visitor to change its emission strategy, but the visit method can ignore
  /// it without a correctness problem.
  SubExprSignRequirement signPreference = NoRequirement;

  /// Keep track of all operations emitted within this subexpression for
  /// location information tracking.
  SmallPtrSet<Operation *, 8> &emittedExprs;

  /// If any subexpressions would result in too large of a line, report it back
  /// to the caller in this vector.
  SmallVectorImpl<Operation *> &tooLargeSubExpressions;
  SmallVectorImpl<char> &outBuffer;
  llvm::raw_svector_ostream os;
  // Track legalized names.
  ModuleNameManager &names;
};
} // end anonymous namespace

SubExprInfo ExprEmitter::emitBinary(Operation *op, VerilogPrecedence prec,
                                    const char *syntax,
                                    SubExprSignRequirement operandSignReq) {
  auto lhsInfo =
      emitSubExpr(op->getOperand(0), prec, OOLBinary, operandSignReq);
  os << ' ' << syntax << ' ';

  // Right associative operators are already generally variadic, we need to
  // handle things like: (a<4> == b<4>) == (c<3> == d<3>).
  // When processing the top operation of the tree, the rhs needs parens.
  auto rhsPrec = VerilogPrecedence(prec - 1);
  auto rhsInfo =
      emitSubExpr(op->getOperand(1), rhsPrec, OOLBinary, operandSignReq);

  // SystemVerilog 11.8.1 says that the result of a binary expression is signed
  // only if both operands are signed.
  SubExprSignResult signedness = IsUnsigned;
  if (lhsInfo.signedness == IsSigned && rhsInfo.signedness == IsSigned)
    signedness = IsSigned;

  return {prec, signedness};
}

SubExprInfo ExprEmitter::emitVariadic(Operation *op, VerilogPrecedence prec,
                                      const char *syntax) {
  // The result is signed if all the subexpressions are signed.
  SubExprSignResult sign = IsSigned;
  interleave(
      op->getOperands().begin(), op->getOperands().end(),
      [&](Value v1) {
        if (emitSubExpr(v1, prec, OOLBinary).signedness != IsSigned)
          sign = IsUnsigned;
      },
      [&] { os << ' ' << syntax << ' '; });

  return {prec, sign};
}

SubExprInfo ExprEmitter::emitUnary(Operation *op, const char *syntax,
                                   bool resultAlwaysUnsigned) {
  os << syntax;
  auto signedness =
      emitSubExpr(op->getOperand(0), Selection, OOLUnary).signedness;
  return {Unary, resultAlwaysUnsigned ? IsUnsigned : signedness};
}

/// We eagerly emit single-use expressions inline into big expression trees...
/// up to the point where they turn into massively long source lines of Verilog.
/// At that point, we retroactively break the huge expression by inserting
/// temporaries.  This handles the bookkeeping.
void ExprEmitter::retroactivelyEmitExpressionIntoTemporary(Operation *op) {
  assert(isVerilogExpression(op) && !emitter.outOfLineExpressions.count(op) &&
         "Should only be called on expressions though to be inlined");

  emitter.outOfLineExpressions.insert(op);
  names.addName(op->getResult(0), "_tmp");

  // Remember that this subexpr needs to be emitted independently.
  tooLargeSubExpressions.push_back(op);
}

/// Emit the specified value as a subexpression to the stream.
SubExprInfo ExprEmitter::emitSubExpr(Value exp,
                                     VerilogPrecedence parenthesizeIfLooserThan,
                                     SubExprOutOfLineBehavior outOfLineBehavior,
                                     SubExprSignRequirement signRequirement) {
  auto *op = exp.getDefiningOp();
  bool shouldEmitInlineExpr = op && isVerilogExpression(op);

  // Don't emit this expression inline if it has multiple uses.
  if (shouldEmitInlineExpr && parenthesizeIfLooserThan != ForceEmitMultiUse &&
      emitter.outOfLineExpressions.count(op))
    shouldEmitInlineExpr = false;

  // If this is a non-expr or shouldn't be done inline, just refer to its name.
  if (!shouldEmitInlineExpr) {
    // All wires are declared as unsigned, so if the client needed it signed,
    // emit a conversion.
    if (signRequirement == RequireSigned) {
      os << "$signed(" << names.getName(exp) << ')';
      return {Symbol, IsSigned};
    }

    os << names.getName(exp);
    return {Symbol, IsUnsigned,
            op && isConstantExpression(op) ? SubExprKind::Const
                                           : SubExprKind::Wire};
  }

  unsigned subExprStartIndex = outBuffer.size();

  // Inform the visit method about the preferred sign we want from the result.
  // It may choose to ignore this, but some emitters can change behavior based
  // on contextual desired sign.
  signPreference = signRequirement;

  // Okay, this is an expression we should emit inline.  Do this through our
  // visitor.
  auto expInfo = dispatchCombinationalVisitor(exp.getDefiningOp());

  // Check cases where we have to insert things before the expression now that
  // we know things about it.
  auto addPrefix = [&](StringRef prefix) {
    outBuffer.insert(outBuffer.begin() + subExprStartIndex, prefix.begin(),
                     prefix.end());
  };
  if (signRequirement == RequireSigned && expInfo.signedness == IsUnsigned) {
    addPrefix("$signed(");
    os << ')';
    expInfo.signedness = IsSigned;
  } else if (signRequirement == RequireUnsigned &&
             expInfo.signedness == IsSigned) {
    addPrefix("$unsigned(");
    os << ')';
    expInfo.signedness = IsUnsigned;
  } else if (expInfo.precedence > parenthesizeIfLooserThan) {
    // If this subexpression would bind looser than the expression it is bound
    // into, then we need to parenthesize it.  Insert the parentheses
    // retroactively.
    addPrefix("(");
    os << ')';
    // Reset the precedence to the () level.
    expInfo.precedence = Selection;
  }

  // If we emitted this subexpression and it resulted in something very large,
  // then we may be in the process of making super huge lines.  Back off to
  // emitting this as its own temporary on its own line.
  unsigned threshold;
  switch (outOfLineBehavior) {
  case OOLTopLevel:
    threshold = ~0U;
    break;
  case OOLUnary:
    threshold = std::max(state.options.emittedLineLength - 20, 10U);
    break;
  case OOLBinary:
    threshold = std::max(state.options.emittedLineLength / 2, 10U);
    break;
  }

  if (outBuffer.size() - subExprStartIndex > threshold &&
      parenthesizeIfLooserThan != ForceEmitMultiUse &&
      !isExpressionAlwaysInline(op)) {
    // Inform the module emitter that this expression needs a temporary
    // wire/logic declaration and set it up so it will be referenced instead of
    // emitted inline.
    retroactivelyEmitExpressionIntoTemporary(op);

    // Lop this off the buffer we emitted.
    outBuffer.resize(subExprStartIndex);

    // Try again, now it will get emitted as a out-of-line leaf.
    return emitSubExpr(exp, parenthesizeIfLooserThan, outOfLineBehavior,
                       signRequirement);
  }

  // Remember that we emitted this.
  emittedExprs.insert(exp.getDefiningOp());
  return expInfo;
}

SubExprInfo ExprEmitter::visitComb(SExtOp op) {
  auto inWidth = op.getOperand().getType().getIntOrFloatBitWidth();
  auto destWidth = op.getType().getIntOrFloatBitWidth();

  // Handle sign extend from a single bit in a pretty way.
  if (inWidth == 1) {
    os << '{' << destWidth << '{';
    emitSubExpr(op.getOperand(), LowestPrecedence, OOLUnary);
    os << "}}";
    return {Symbol, IsUnsigned};
  }

  // Otherwise, this is a sign extension of a general expression.
  os << '{';
  if (destWidth - inWidth == 1) {
    // Special pattern for single bit extension, where we just need the bit.
    emitSubExpr(op.getOperand(), Unary, OOLUnary);
    os << '[' << (inWidth - 1) << ']';
  } else {
    // General pattern for multi-bit extension: splat the bit.
    os << '{' << (destWidth - inWidth) << '{';
    emitSubExpr(op.getOperand(), Unary, OOLUnary);
    os << '[' << (inWidth - 1) << "]}}";
  }
  os << ", ";
  emitSubExpr(op.getOperand(), LowestPrecedence, OOLUnary);
  os << '}';
  return {Symbol, IsUnsigned};
}

SubExprInfo ExprEmitter::visitComb(ConcatOp op) {
  // If all of the operands are the same, we emit this as a SystemVerilog
  // replicate operation, ala SV Spec 11.4.12.1.
  auto firstOperand = op.getOperand(0);
  bool allSame = llvm::all_of(op.getOperands(), [&firstOperand](auto operand) {
    return operand == firstOperand;
  });

  if (allSame) {
    os << '{' << op.getNumOperands() << '{';
    emitSubExpr(firstOperand, LowestPrecedence, OOLUnary);
    os << "}}";
    return {Symbol, IsUnsigned};
  }

  os << '{';
  llvm::interleaveComma(op.getOperands(), os, [&](Value v) {
    emitSubExpr(v, LowestPrecedence, OOLBinary);
  });

  os << '}';
  return {Symbol, IsUnsigned};
}

SubExprInfo ExprEmitter::visitTypeOp(BitcastOp op) {
  // NOTE: Bitcasts are emitted out-of-line with their own wire declaration when
  // their dimensions don't match. SystemVerilog uses the wire declaration to
  // know what type this value is being casted to.
  Type toType = op.getType();
  if (!haveMatchingDims(toType, op.input().getType(), op.getLoc())) {
    os << "/*cast(bit";
    emitTypeDims(toType, op.getLoc(), os);
    os << ")*/";
  }
  return emitSubExpr(op.input(), LowestPrecedence, OOLUnary);
}

SubExprInfo ExprEmitter::visitComb(ICmpOp op) {
  const char *symop[] = {"==", "!=", "<",  "<=", ">",
                         ">=", "<",  "<=", ">",  ">="};
  SubExprSignRequirement signop[] = {
      // Equality
      NoRequirement, NoRequirement,
      // Signed Comparisons
      RequireSigned, RequireSigned, RequireSigned, RequireSigned,
      // Unsigned Comparisons
      RequireUnsigned, RequireUnsigned, RequireUnsigned, RequireUnsigned};

  auto pred = static_cast<uint64_t>(op.predicate());
  assert(pred < sizeof(symop) / sizeof(symop[0]));

  // Lower "== -1" to Reduction And.
  if (op.isEqualAllOnes())
    return emitUnary(op, "&", true);

  // Lower "!= 0" to Reduction Or.
  if (op.isNotEqualZero())
    return emitUnary(op, "|", true);

  auto result = emitBinary(op, Comparison, symop[pred], signop[pred]);

  // SystemVerilog 11.8.1: "Comparison... operator results are unsigned,
  // regardless of the operands".
  result.signedness = IsUnsigned;
  return result;
}

SubExprInfo ExprEmitter::visitComb(ExtractOp op) {
  unsigned loBit = op.lowBit();
  unsigned hiBit = loBit + op.getType().getWidth() - 1;

  auto x = emitSubExpr(op.input(), LowestPrecedence, OOLUnary);
  assert(x.precedence == Symbol &&
         "should be handled by isExpressionUnableToInline");

  // If we're extracting the whole input, just return it.  This is valid but
  // non-canonical IR, and we don't want to generate invalid Verilog.
  if (loBit == 0 && op.input().getType().getIntOrFloatBitWidth() == hiBit + 1)
    return x;

  os << '[' << hiBit;
  if (hiBit != loBit) // Emit x[4] instead of x[4:4].
    os << ':' << loBit;
  os << ']';
  return {Unary, IsUnsigned};
}

SubExprInfo ExprEmitter::visitSV(GetModportOp op) {
  os << names.getName(op.iface()) + "." + op.field();
  return {Selection, IsUnsigned};
}

SubExprInfo ExprEmitter::visitSV(ReadInterfaceSignalOp op) {
  os << names.getName(op.iface()) + "." + op.signalName();
  return {Selection, IsUnsigned};
}

SubExprInfo ExprEmitter::visitVerbatimExprOp(Operation *op) {
  emitTextWithSubstitutions(op->getAttrOfType<StringAttr>("string").getValue(),
                            op, [&](Value operand) {
                              emitSubExpr(operand, LowestPrecedence, OOLBinary);
                            });

  return {Unary, IsUnsigned};
}

SubExprInfo ExprEmitter::visitSV(ConstantXOp op) {
  os << op.getType().getWidth() << "'bx";
  return {Unary, IsUnsigned};
}

SubExprInfo ExprEmitter::visitSV(ConstantZOp op) {
  os << op.getType().getWidth() << "'bz";
  return {Unary, IsUnsigned};
}

SubExprInfo ExprEmitter::visitTypeOp(ConstantOp op) {
  bool isNegated = false;
  const APInt &value = op.getValue();
  // If this is a negative signed number and not MININT (e.g. -128), then print
  // it as a negated positive number.
  if (signPreference == RequireSigned && value.isNegative() &&
      !value.isMinSignedValue()) {
    os << '-';
    isNegated = true;
  }

  os << op.getType().getWidth() << '\'';

  // Emit this as a signed constant if the caller would prefer that.
  if (signPreference == RequireSigned)
    os << 's';
  os << 'h';

  // Print negated if required.
  SmallString<32> valueStr;
  if (isNegated) {
    (-value).toStringUnsigned(valueStr, 16);
  } else {
    value.toStringUnsigned(valueStr, 16);
  }
  os << valueStr;
  return {Unary, signPreference == RequireSigned ? IsSigned : IsUnsigned};
}

// 11.5.1 "Vector bit-select and part-select addressing" allows a '+:' syntax
// for slicing operations.
SubExprInfo ExprEmitter::visitTypeOp(ArraySliceOp op) {
  auto arrayPrec = emitSubExpr(op.input(), Selection, OOLUnary);

  unsigned dstWidth = type_cast<ArrayType>(op.getType()).getSize();
  os << '[';
  emitSubExpr(op.lowIndex(), LowestPrecedence, OOLBinary);
  os << "+:" << dstWidth << ']';
  return {Selection, arrayPrec.signedness};
}

SubExprInfo ExprEmitter::visitTypeOp(ArrayGetOp op) {
  emitSubExpr(op.input(), Selection, OOLUnary);
  os << '[';
  emitSubExpr(op.index(), LowestPrecedence, OOLBinary);
  os << ']';
  return {Selection, IsUnsigned};
}

// Syntax from: section 5.11 "Array literals".
SubExprInfo ExprEmitter::visitTypeOp(ArrayCreateOp op) {
  os << '{';
  llvm::interleaveComma(op.inputs(), os, [&](Value operand) {
    os << "{";
    emitSubExpr(operand, LowestPrecedence, OOLBinary);
    os << "}";
  });
  os << '}';
  return {Unary, IsUnsigned};
}

SubExprInfo ExprEmitter::visitTypeOp(ArrayConcatOp op) {
  os << '{';
  llvm::interleaveComma(op.getOperands(), os, [&](Value v) {
    emitSubExpr(v, LowestPrecedence, OOLBinary);
  });
  os << '}';
  return {Unary, IsUnsigned};
}

SubExprInfo ExprEmitter::visitSV(ArrayIndexInOutOp op) {
  auto arrayPrec = emitSubExpr(op.input(), Selection, OOLUnary);
  os << '[';
  emitSubExpr(op.index(), LowestPrecedence, OOLBinary);
  os << ']';
  return {Selection, arrayPrec.signedness};
}

SubExprInfo ExprEmitter::visitComb(MuxOp op) {
  // The ?: operator is right associative.
  emitSubExpr(op.cond(), VerilogPrecedence(Conditional - 1), OOLBinary);
  os << " ? ";
  auto lhsInfo = emitSubExpr(op.trueValue(), VerilogPrecedence(Conditional - 1),
                             OOLBinary);
  os << " : ";
  auto rhsInfo = emitSubExpr(op.falseValue(), Conditional, OOLBinary);

  SubExprSignResult signedness = IsUnsigned;
  if (lhsInfo.signedness == IsSigned && rhsInfo.signedness == IsSigned)
    signedness = IsSigned;

  return {Conditional, signedness};
}

SubExprInfo ExprEmitter::visitTypeOp(StructCreateOp op) {
  StructType stype = op.getType();
  os << "'{";
  size_t i = 0;
  llvm::interleaveComma(stype.getElements(), os,
                        [&](const StructType::FieldInfo &field) {
                          os << field.name << ": ";
                          emitSubExpr(op.getOperand(i++), Selection, OOLBinary);
                        });
  os << '}';
  return {Unary, IsUnsigned};
}

SubExprInfo ExprEmitter::visitTypeOp(StructExtractOp op) {
  emitSubExpr(op.input(), Selection, OOLUnary);
  os << '.' << op.field();
  return {Selection, IsUnsigned};
}

SubExprInfo ExprEmitter::visitTypeOp(StructInjectOp op) {
  StructType stype = op.getType().cast<StructType>();
  os << "'{";
  llvm::interleaveComma(stype.getElements(), os,
                        [&](const StructType::FieldInfo &field) {
                          os << field.name << ": ";
                          if (field.name == op.field()) {
                            emitSubExpr(op.newValue(), Selection, OOLBinary);
                          } else {
                            emitSubExpr(op.input(), Selection, OOLBinary);
                            os << '.' << field.name;
                          }
                        });
  os << '}';
  return {Selection, IsUnsigned};
}

SubExprInfo ExprEmitter::visitUnhandledExpr(Operation *op) {
  emitOpError(op, "cannot emit this expression to Verilog");
  os << "<<unsupported expr: " << op->getName().getStringRef() << ">>";
  return {Symbol, IsUnsigned};
}

//===----------------------------------------------------------------------===//
// NameCollector
//===----------------------------------------------------------------------===//

/// Most expressions are invalid to bit-select from in Verilog, but some
/// things are ok.  Return true if it is ok to inline bitselect from the
/// result of this expression.  It is conservatively correct to return false.
static bool isOkToBitSelectFrom(Value v) {
  // Module ports are always ok to bit select from.
  if (v.getDefiningOp())
    // TODO: We could handle concat and other operators here.
    return false;

  return true;
}

/// Return true if we are unable to ever inline the specified operation.  This
/// happens because not all Verilog expressions are composable, notably you
/// can only use bit selects like x[4:6] on simple expressions, you cannot use
/// expressions in the sensitivity list of always blocks, etc.
static bool isExpressionUnableToInline(Operation *op) {
  if (auto cast = dyn_cast<BitcastOp>(op))
    if (!haveMatchingDims(cast.input().getType(), cast.result().getType(),
                          op->getLoc()))
      // Bitcasts rely on the type being assigned to, so we cannot inline.
      return true;

  // StructCreateOp needs to be assigning to a named temporary so that types
  // are inferred properly by verilog
  if (isa<StructCreateOp>(op))
    return true;

  auto *opBlock = op->getBlock();

  // Scan the users of the operation to see if any of them need this to be
  // emitted out-of-line.
  for (auto user : op->getUsers()) {
    // If the user is in a different block and the op shouldn't be inlined, then
    // we emit this as an out-of-line declaration into its block and the user
    // can refer to it.
    if (user->getBlock() != opBlock)
      return true;

    // Verilog bit selection is required by the standard to be:
    // "a vector, packed array, packed structure, parameter or concatenation".
    // It cannot be an arbitrary expression.
    if (isa<ExtractOp>(user))
      if (!isOkToBitSelectFrom(op->getResult(0)))
        return true;

    // Sign extend (when the operand isn't a single bit) requires a bitselect
    // syntactically.
    if (auto sext = dyn_cast<SExtOp>(user)) {
      auto sextOperandType = sext.getOperand().getType().cast<IntegerType>();
      if (sextOperandType.getWidth() != 1 &&
          !isOkToBitSelectFrom(op->getResult(0)))
        return true;
    }
    // ArraySliceOp uses its operand twice, so we want to assign it first then
    // use that variable in the ArraySliceOp expression.
    if (isa<ArraySliceOp>(user) && !isa<ConstantOp>(op))
      return true;

    // Always blocks must have a name in their sensitivity list, not an expr.
    if (isa<AlwaysOp>(user) || isa<AlwaysFFOp>(user))
      return true;
  }
  return false;
}

/// Return true if this expression should be emitted inline into any statement
/// that uses it.
static bool isExpressionEmittedInline(Operation *op) {
  // If it isn't structurally possible to inline this expression, emit it out
  // of line.
  if (isExpressionUnableToInline(op))
    return false;

  // If it has a single use, emit it inline.
  if (op->getResult(0).hasOneUse())
    return true;

  // If it is nullary and duplicable, then we can emit it inline.
  return op->getNumOperands() == 0 && isDuplicatableNullaryExpression(op);
}

namespace {
class NameCollector {
public:
  // This is information we keep track of for each wire/reg/interface
  // declaration we're going to emit.
  struct ValuesToEmitRecord {
    Value value;
    SmallString<8> typeString;
  };

  NameCollector(ModuleEmitter &moduleEmitter, ModuleNameManager &names)
      : moduleEmitter(moduleEmitter), names(names) {}

  // Scan operations in the specified block, collecting information about
  // those that need to be emitted out of line.
  void collectNames(Block &block);

  size_t getMaxDeclNameWidth() const { return maxDeclNameWidth; }
  size_t getMaxTypeWidth() const { return maxTypeWidth; }
  const SmallVectorImpl<ValuesToEmitRecord> &getValuesToEmit() const {
    return valuesToEmit;
  }

private:
  size_t maxDeclNameWidth = 0, maxTypeWidth = 0;
  SmallVector<ValuesToEmitRecord, 16> valuesToEmit;
  ModuleEmitter &moduleEmitter;
  ModuleNameManager &names;
};
} // namespace

void NameCollector::collectNames(Block &block) {
  bool isBlockProcedural = block.getParentOp()->hasTrait<ProceduralRegion>();

  SmallString<32> nameTmp;

  // Loop over all of the results of all of the ops.  Anything that defines a
  // value needs to be noticed.
  for (auto &op : block) {
    bool isExpr = isVerilogExpression(&op);

    // Instances are handled in prepareHWModule
    if (isa<InstanceOp>(op))
      continue;

    for (auto result : op.getResults()) {
      // If this is an expression emitted inline or unused, it doesn't need a
      // name.
      if (isExpr) {
        // If this expression is dead, or can be emitted inline, ignore it.
        if (result.use_empty() || isExpressionEmittedInline(&op))
          continue;

        // Remember that this expression should be emitted out of line.
        moduleEmitter.outOfLineExpressions.insert(&op);
      }

      // Otherwise, it must be an expression or a declaration like a
      // RegOp/WireOp.
      if (!names.hasName(result))
        names.addName(result, op.getAttrOfType<StringAttr>("name"));

      // Don't measure or emit wires that are emitted inline (i.e. the wire
      // definition is emitted on the line of the expression instead of a
      // block at the top of the module).
      if (isExpr) {
        // Procedural blocks always emit out of line variable declarations,
        // because Verilog requires that they all be at the top of a block.
        // TODO: Improve this, at least in the simple cases.
        if (!isBlockProcedural)
          continue;
      }

      // Emit this value.
      valuesToEmit.push_back(ValuesToEmitRecord{result, {}});
      auto &typeString = valuesToEmit.back().typeString;

      maxDeclNameWidth =
          std::max(getVerilogDeclWord(&op).size(), maxDeclNameWidth);

      // Convert the port's type to a string and measure it.
      {
        llvm::raw_svector_ostream stringStream(typeString);
        printPackedType(stripUnpackedTypes(result.getType()), stringStream,
                        &op);
      }
      maxTypeWidth = std::max(typeString.size(), maxTypeWidth);
    }

    // Recursively process any regions under the op iff this is a procedural
    // #ifdef region: we need to emit automatic logic values at the top of the
    // enclosing region.
    if (isa<IfDefProceduralOp>(op)) {
      for (auto &region : op.getRegions()) {
        if (!region.empty())
          collectNames(region.front());
      }
    }
  }
}

//===----------------------------------------------------------------------===//
// TypeScopeEmitter
//===----------------------------------------------------------------------===//

namespace {
/// This emits typescope-related operations.
class TypeScopeEmitter
    : public EmitterBase,
      public hw::TypeScopeVisitor<TypeScopeEmitter, LogicalResult> {
public:
  /// Create a TypeScopeEmitter for the specified module emitter.
  TypeScopeEmitter(VerilogEmitterState &state) : EmitterBase(state) {}

  void emitTypeScopeBlock(Block &body);

private:
  friend class TypeScopeVisitor<TypeScopeEmitter, LogicalResult>;

  LogicalResult visitTypeScope(TypedeclOp op);
};

} // end anonymous namespace

void TypeScopeEmitter::emitTypeScopeBlock(Block &body) {
  for (auto &op : body) {
    if (failed(dispatchTypeScopeVisitor(&op))) {
      op.emitOpError("cannot emit this type scope op to Verilog");
      os << "<<unsupported op: " << op.getName().getStringRef() << ">>\n";
    }
  }
}

LogicalResult TypeScopeEmitter::visitTypeScope(TypedeclOp op) {
  indent() << "typedef ";
  printPackedType(stripUnpackedTypes(op.type()), os, op, false);
  printUnpackedTypePostfix(op.type(), os);
  os << ' ' << op.getPreferredName();
  os << ";\n";
  return success();
}

//===----------------------------------------------------------------------===//
// StmtEmitter
//===----------------------------------------------------------------------===//

namespace {
/// This emits statement-related operations.
class StmtEmitter : public EmitterBase,
                    public hw::StmtVisitor<StmtEmitter, LogicalResult>,
                    public sv::Visitor<StmtEmitter, LogicalResult> {
public:
  /// Create an ExprEmitter for the specified module emitter, and keeping track
  /// of any emitted expressions in the specified set.
  StmtEmitter(ModuleEmitter &emitter, SmallVectorImpl<char> &outBuffer,
              ModuleNameManager &names)
      : EmitterBase(emitter.state, stringStream), emitter(emitter),
        stringStream(outBuffer), outBuffer(outBuffer), names(names) {}

  void emitStatement(Operation *op);
  void emitStatementBlock(Block &body);
  size_t getNumStatementsEmitted() const { return numStatementsEmitted; }

  /// Emit the declaration for the temporary operation. If the operation is not
  /// a constant, emit no initializer and no semicolon, e.g. `wire foo`, and
  /// return false. If the operation *is* a constant, also emit the initializer
  /// and semicolon, e.g. `localparam K = 1'h0`, and return true.
  bool emitDeclarationForTemporary(Operation *op) {
    StringRef declWord = getVerilogDeclWord(op);
    indent() << declWord;
    if (!declWord.empty())
      os << ' ';
    if (printPackedType(stripUnpackedTypes(op->getResult(0).getType()), os, op))
      os << ' ';
    os << names.getName(op->getResult(0));

    if (!isConstantExpression(op))
      return false;

    // If this is a constant, we have to immediately assign its value as
    // required by the `localparam` construct.
    os << " = ";
    SmallPtrSet<Operation *, 8> emittedExprs;
    emitExpression(op->getResult(0), emittedExprs, ForceEmitMultiUse);
    os << ';';
    emitLocationInfoAndNewLine(emittedExprs);
    return true;
  }

private:
  void collectNamesEmitDecls(Block &block);

  SubExprInfo
  emitExpression(Value exp, SmallPtrSet<Operation *, 8> &emittedExprs,
                 VerilogPrecedence parenthesizeIfLooserThan = LowestPrecedence);

  void emitEventControlExpression(
      Value exp, SmallPtrSet<Operation *, 8> &emittedExprs,
      VerilogPrecedence parenthesizeIfLooserThan = LowestPrecedence);

  using StmtVisitor::visitStmt;
  using Visitor::visitSV;
  friend class hw::StmtVisitor<StmtEmitter, LogicalResult>;
  friend class sv::Visitor<StmtEmitter, LogicalResult>;

  // Visitor methods.
  LogicalResult visitUnhandledStmt(Operation *op) { return failure(); }
  LogicalResult visitInvalidStmt(Operation *op) { return failure(); }
  LogicalResult visitUnhandledSV(Operation *op) { return failure(); }
  LogicalResult visitInvalidSV(Operation *op) { return failure(); }

  LogicalResult emitNoop() {
    --numStatementsEmitted;
    return success();
  }

  LogicalResult visitSV(WireOp op) { return emitNoop(); }
  LogicalResult visitSV(RegOp op) { return emitNoop(); }
  LogicalResult visitSV(InterfaceInstanceOp op) { return emitNoop(); }
  LogicalResult visitSV(AssignOp op);
  LogicalResult visitSV(BPAssignOp op);
  LogicalResult visitSV(PAssignOp op);
  LogicalResult visitSV(ForceOp op);
  LogicalResult visitSV(ReleaseOp op);
  LogicalResult visitSV(AliasOp op);
  LogicalResult visitStmt(OutputOp op);
  LogicalResult visitStmt(InstanceOp op);

  LogicalResult emitIfDef(Operation *op, StringRef cond);
  LogicalResult visitSV(IfDefOp op) { return emitIfDef(op, op.cond()); }
  LogicalResult visitSV(IfDefProceduralOp op) {
    return emitIfDef(op, op.cond());
  }
  LogicalResult visitSV(IfOp op);
  LogicalResult visitSV(AlwaysOp op);
  LogicalResult visitSV(AlwaysCombOp op);
  LogicalResult visitSV(AlwaysFFOp op);
  LogicalResult visitSV(InitialOp op);
  LogicalResult visitSV(CaseZOp op);
  LogicalResult visitSV(FWriteOp op);
  LogicalResult visitSV(FatalOp op);
  LogicalResult visitSV(FinishOp op);
  LogicalResult visitSV(VerbatimOp op);
  LogicalResult emitImmediateAssertion(Operation *op, Twine name,
                                       StringRef label, Value expression);
  LogicalResult visitSV(AssertOp op);
  LogicalResult visitSV(AssumeOp op);
  LogicalResult visitSV(CoverOp op);
  LogicalResult visitSV(BindOp op);
  LogicalResult emitConcurrentAssertion(Operation *op, Twine name,
                                        StringRef label, EventControl event,
                                        Value clock, Value property);
  LogicalResult visitSV(AssertConcurrentOp op);
  LogicalResult visitSV(AssumeConcurrentOp op);
  LogicalResult visitSV(CoverConcurrentOp op);
  LogicalResult visitSV(InterfaceOp op);
  LogicalResult visitSV(InterfaceSignalOp op);
  LogicalResult visitSV(InterfaceModportOp op);
  LogicalResult visitSV(AssignInterfaceSignalOp op);
  void emitStatementExpression(Operation *op);

  void emitBlockAsStatement(Block *block,
                            SmallPtrSet<Operation *, 8> &locationOps,
                            StringRef multiLineComment = StringRef());

public:
  ModuleEmitter &emitter;

private:
  llvm::raw_svector_ostream stringStream;
  // All statements are emitted into a temporary buffer, this is it.
  SmallVectorImpl<char> &outBuffer;

  // Track the legalized names.
  ModuleNameManager &names;

  // This is the index of the start of the current statement being emitted.
  size_t statementBeginningIndex = 0;

  /// This is the index of the end of the declaration region of the current
  /// 'begin' block, used to emit variable declarations.
  size_t blockDeclarationInsertPointIndex = 0;
  size_t numStatementsEmitted = 0;
};

} // end anonymous namespace

/// Emit the specified value as an expression.  If this is an inline-emitted
/// expression, we emit that expression, otherwise we emit a reference to the
/// already computed name.
///
SubExprInfo
StmtEmitter::emitExpression(Value exp,
                            SmallPtrSet<Operation *, 8> &emittedExprs,
                            VerilogPrecedence parenthesizeIfLooserThan) {
  SmallVector<Operation *> tooLargeSubExpressions;
  auto info = ExprEmitter(emitter, outBuffer, emittedExprs,
                          tooLargeSubExpressions, names)
                  .emitExpression(exp, parenthesizeIfLooserThan);

  // It is possible that the emitted expression was too large to fit on a line
  // and needs to be split.  If so, the new subexpressions that need emitting
  // are put out into the the 'tooLargeSubExpressions' list.  Re-emit these at
  // the start of the current statement as their own stmt expressions.
  if (tooLargeSubExpressions.empty())
    return info;

  // Pop this statement off and save it to the side.
  std::string thisStmt(outBuffer.begin() + statementBeginningIndex,
                       outBuffer.end());
  outBuffer.resize(statementBeginningIndex);

  // Emit each stmt expression in turn.
  for (auto *expr : tooLargeSubExpressions) {
    statementBeginningIndex = outBuffer.size();
    ++numStatementsEmitted;
    emitStatementExpression(expr);
  }

  // Re-add this statement now that all the preceeding ones are out.
  outBuffer.append(thisStmt.begin(), thisStmt.end());

  // If we are working on a procedural statement, we need to emit the
  // declarations for each variable separately from the assignments to them.
  // We do this after inserting the assign statements because we don't want to
  // invalidate the index.
  if (tooLargeSubExpressions[0]->getParentOp()->hasTrait<ProceduralRegion>()) {
    thisStmt.assign(outBuffer.begin() + blockDeclarationInsertPointIndex,
                    outBuffer.end());
    outBuffer.resize(blockDeclarationInsertPointIndex);
    for (auto *expr : tooLargeSubExpressions) {
      if (!emitDeclarationForTemporary(expr))
        os << ";\n";
    }
    blockDeclarationInsertPointIndex = outBuffer.size();
    outBuffer.append(thisStmt.begin(), thisStmt.end());
  }

  return info;
}

/// Emit the specified value as an expression inside a process' event control
/// part (`@(...)`). This optionally enforces spilling the expression into a
/// wire.
void StmtEmitter::emitEventControlExpression(
    Value exp, SmallPtrSet<Operation *, 8> &emittedExprs,
    VerilogPrecedence parenthesizeIfLooserThan) {
  // Remember where the current statement started (so we can insert before that
  // if needed), and remember where the current expression started (which is
  // where `emitExpression` will write whatever should go in the event control).
  auto originalStatementBeginningIndex = statementBeginningIndex;
  auto expressionBeginningIndex = outBuffer.size();

  // Actually emit the expression.
  SubExprInfo info =
      emitExpression(exp, emittedExprs, parenthesizeIfLooserThan);
  if (!state.options.useWireInEventControl || info.kind == SubExprKind::Wire)
    return;

  // Adjust the expression beginning to account for any additional statements
  // the above might have emitted.
  expressionBeginningIndex +=
      statementBeginningIndex - originalStatementBeginningIndex;

  // Stow away the current statement (e.g. the `always`) up to the beginning of
  // the expression in the event control. Then stow that expression away
  // separately.
  std::string thisStmt(outBuffer.begin() + statementBeginningIndex,
                       outBuffer.begin() + expressionBeginningIndex);
  std::string thisExpr(outBuffer.begin() + expressionBeginningIndex,
                       outBuffer.end());
  outBuffer.resize(statementBeginningIndex);

  // Decide on a name for the spilled value. If the original expression already
  // had a name, reuse that as a hint.
  SmallString<16> suggestedName(names.hasName(exp) ? names.getName(exp) : "");
  suggestedName.append("_event");
  auto name = names.addName(suggestedName);

  // Spill the expression into a wire.
  indent() << "wire ";
  if (printPackedType(stripUnpackedTypes(exp.getType()), os,
                      exp.getDefiningOp()))
    os << ' ';
  os << name << " = " << thisExpr << ";\n";
  outBuffer.append(thisStmt.begin(), thisStmt.end());
  os << name;
}

void StmtEmitter::emitStatementExpression(Operation *op) {
  // This is invoked for expressions that have a non-single use.  This could
  // either be because they are dead or because they have multiple uses.
  if (op->getResult(0).use_empty()) {
    indent() << "// Unused: ";
    --numStatementsEmitted;
  } else if (isZeroBitType(op->getResult(0).getType())) {
    indent() << "// Zero width: ";
    --numStatementsEmitted;
  } else if (op->getParentOp()->hasTrait<ProceduralRegion>()) {
    // Constants have their value emitted directly in the corresponding
    // `localparam` declaration. Don't try to reassign these.
    if (isConstantExpression(op)) {
      --numStatementsEmitted;
      return;
    }
    indent() << names.getName(op->getResult(0)) << " = ";
  } else {
    if (emitDeclarationForTemporary(op))
      return;
    os << " = ";
  }

  // Emit the expression with a special precedence level so it knows to do a
  // "deep" emission even though there are multiple uses, not just emitting the
  // name.
  SmallPtrSet<Operation *, 8> emittedExprs;
  emitExpression(op->getResult(0), emittedExprs, ForceEmitMultiUse);
  os << ';';
  emitLocationInfoAndNewLine(emittedExprs);
}

LogicalResult StmtEmitter::visitSV(AssignOp op) {
  // prepare assigns wires to instance outputs, but these are logically handled
  // in the port binding list when outputing an instance.
  if (dyn_cast_or_null<InstanceOp>(op.src().getDefiningOp()))
    return success();

  SmallPtrSet<Operation *, 8> ops;
  ops.insert(op);

  indent() << "assign ";
  emitExpression(op.dest(), ops);
  os << " = ";
  emitExpression(op.src(), ops);
  os << ';';
  emitLocationInfoAndNewLine(ops);
  return success();
}

LogicalResult StmtEmitter::visitSV(BPAssignOp op) {
  SmallPtrSet<Operation *, 8> ops;
  ops.insert(op);

  indent();
  emitExpression(op.dest(), ops);
  os << " = ";
  emitExpression(op.src(), ops);
  os << ';';
  emitLocationInfoAndNewLine(ops);
  return success();
}

LogicalResult StmtEmitter::visitSV(PAssignOp op) {
  SmallPtrSet<Operation *, 8> ops;
  ops.insert(op);

  indent();
  emitExpression(op.dest(), ops);
  os << " <= ";
  emitExpression(op.src(), ops);
  os << ';';
  emitLocationInfoAndNewLine(ops);
  return success();
}

LogicalResult StmtEmitter::visitSV(ForceOp op) {
  SmallPtrSet<Operation *, 8> ops;
  ops.insert(op);

  indent() << "force ";
  emitExpression(op.dest(), ops);
  os << " = ";
  emitExpression(op.src(), ops);
  os << ';';
  emitLocationInfoAndNewLine(ops);
  return success();
}

LogicalResult StmtEmitter::visitSV(ReleaseOp op) {
  SmallPtrSet<Operation *, 8> ops;
  ops.insert(op);

  indent() << "release ";
  emitExpression(op.dest(), ops);
  os << ';';
  emitLocationInfoAndNewLine(ops);
  return success();
}

LogicalResult StmtEmitter::visitSV(AliasOp op) {
  SmallPtrSet<Operation *, 8> ops;
  ops.insert(op);

  indent() << "alias ";
  llvm::interleave(
      op.getOperands(), os, [&](Value v) { emitExpression(v, ops); }, " = ");
  os << ';';
  emitLocationInfoAndNewLine(ops);
  return success();
}

/// For OutputOp we put "assign" statements at the end of the Verilog module to
/// assign the module outputs to intermediate wires.
LogicalResult StmtEmitter::visitStmt(OutputOp op) {
  --numStatementsEmitted; // Count emitted statements manually.

  SmallPtrSet<Operation *, 8> ops;
  HWModuleOp parent = op->getParentOfType<HWModuleOp>();

  size_t operandIndex = 0;
  for (ModulePortInfo port : parent.getPorts()) {
    if (!port.isOutput())
      continue;

    auto operand = op.getOperand(operandIndex);
    if (operand.hasOneUse() &&
        dyn_cast_or_null<InstanceOp>(operand.getDefiningOp())) {
      ++operandIndex;
      continue;
    }

    ops.clear();
    ops.insert(op);
    indent();
    if (isZeroBitType(port.type))
      os << "// Zero width: ";
    os << "assign " << names.getOutputName(port.argNum) << " = ";
    emitExpression(operand, ops);
    os << ';';
    emitLocationInfoAndNewLine(ops);
    ++operandIndex;
    ++numStatementsEmitted;
  }
  return success();
}

LogicalResult StmtEmitter::visitSV(FWriteOp op) {
  SmallPtrSet<Operation *, 8> ops;
  ops.insert(op);

  indent() << "$fwrite(32'h80000002, \"";
  os.write_escaped(op.string());
  os << '"';

  for (auto operand : op.operands()) {
    os << ", ";
    emitExpression(operand, ops);
  }
  os << ");";
  emitLocationInfoAndNewLine(ops);
  return success();
}

LogicalResult StmtEmitter::visitSV(FatalOp op) {
  SmallPtrSet<Operation *, 8> ops;
  ops.insert(op);
  indent() << "$fatal;";
  emitLocationInfoAndNewLine(ops);
  return success();
}

LogicalResult StmtEmitter::visitSV(VerbatimOp op) {
  SmallPtrSet<Operation *, 8> ops;
  ops.insert(op);

  // Drop an extraneous \n off the end of the string if present.
  StringRef string = op.string();
  if (string.endswith("\n"))
    string = string.drop_back();

  // Emit each \n separated piece of the string with each piece properly
  // indented.  The convention is to not emit the \n so
  // emitLocationInfoAndNewLine can do that for the last line.
  bool isFirst = true;
  indent();

  // Emit each line of the string at a time.
  while (!string.empty()) {
    auto lhsRhs = string.split('\n');
    if (isFirst)
      isFirst = false;
    else {
      os << '\n';
      indent();
    }

    // Emit each chunk of the line.
    emitTextWithSubstitutions(
        lhsRhs.first, op, [&](Value operand) { emitExpression(operand, ops); });
    string = lhsRhs.second;
  }

  emitLocationInfoAndNewLine(ops);

  // We don't know how many statements we emitted, so assume conservatively
  // that a lot got put out. This will make sure we get a begin/end block around
  // this.
  numStatementsEmitted += 2;
  return success();
}

LogicalResult StmtEmitter::visitSV(FinishOp op) {
  SmallPtrSet<Operation *, 8> ops;
  ops.insert(op);
  indent() << "$finish;";
  emitLocationInfoAndNewLine(ops);
  return success();
}

LogicalResult StmtEmitter::emitImmediateAssertion(Operation *op, Twine name,
                                                  StringRef label,
                                                  Value expression) {
  SmallPtrSet<Operation *, 8> ops;
  ops.insert(op);
  indent();
  if (!label.empty())
    os << label << ": ";
  os << name << "(";
  emitExpression(expression, ops);
  os << ");";
  emitLocationInfoAndNewLine(ops);
  return success();
}

LogicalResult StmtEmitter::visitSV(AssertOp op) {
  return emitImmediateAssertion(op, "assert", op.label(), op.expression());
}

LogicalResult StmtEmitter::visitSV(AssumeOp op) {
  return emitImmediateAssertion(op, "assume", op.label(), op.expression());
}

LogicalResult StmtEmitter::visitSV(CoverOp op) {
  return emitImmediateAssertion(op, "cover", op.label(), op.expression());
}

LogicalResult StmtEmitter::emitConcurrentAssertion(Operation *op, Twine name,
                                                   StringRef label,
                                                   EventControl event,
                                                   Value clock,
                                                   Value property) {
  SmallPtrSet<Operation *, 8> ops;
  ops.insert(op);
  if (!label.empty())
    os << label << ": ";
  os << name << " property (@(" << stringifyEventControl(event) << " ";
  emitExpression(clock, ops);
  os << ") ";
  emitExpression(property, ops);
  os << ");";
  emitLocationInfoAndNewLine(ops);
  return success();
}

LogicalResult StmtEmitter::visitSV(AssertConcurrentOp op) {
  return emitConcurrentAssertion(op, "assert", op.label(), op.event(),
                                 op.clock(), op.property());
}

LogicalResult StmtEmitter::visitSV(AssumeConcurrentOp op) {
  return emitConcurrentAssertion(op, "assume", op.label(), op.event(),
                                 op.clock(), op.property());
}

LogicalResult StmtEmitter::visitSV(CoverConcurrentOp op) {
  return emitConcurrentAssertion(op, "cover", op.label(), op.event(),
                                 op.clock(), op.property());
}

LogicalResult StmtEmitter::emitIfDef(Operation *op, StringRef cond) {
  bool hasEmptyThen = op->getRegion(0).front().empty();
  if (hasEmptyThen)
    indent() << "`ifndef " << cond;
  else
    indent() << "`ifdef " << cond;

  SmallPtrSet<Operation *, 8> ops;
  ops.insert(op);
  emitLocationInfoAndNewLine(ops);

  if (!hasEmptyThen)
    emitStatementBlock(op->getRegion(0).front());

  if (!op->getRegion(1).empty()) {
    if (!hasEmptyThen)
      indent() << "`else\n";
    emitStatementBlock(op->getRegion(1).front());
  }

  indent() << "`endif\n";

  // We don't know how many statements we emitted, so assume conservatively
  // that a lot got put out. This will make sure we get a begin/end block around
  // this.
  numStatementsEmitted += 2;
  return success();
}

/// Emit the body of a control flow statement that is surrounded by begin/end
/// markers if non-singular.  If the control flow construct is multi-line and
/// if multiLineComment is non-null, the string is included in a comment after
/// the 'end' to make it easier to associate.
void StmtEmitter::emitBlockAsStatement(Block *block,
                                       SmallPtrSet<Operation *, 8> &locationOps,
                                       StringRef multiLineComment) {

  // We don't know if we need to emit the begin until after we emit the body of
  // the block.  We can have multiple ops that fold together into one statement
  // (common in nested expressions feeding into a connect) or one apparently
  // simple set of operations that gets broken across multiple lines because
  // they are too long.
  //
  // Solve this by emitting the statements, determining if we need to
  // emit the begin, and if so, emit the begin retroactively.
  size_t beginInsertPoint = outBuffer.size();
  emitLocationInfoAndNewLine(locationOps);

  // Change the blockDeclarationInsertPointIndex for the statements in this
  // block.
  llvm::SaveAndRestore<size_t> X(blockDeclarationInsertPointIndex,
                                 outBuffer.size());
  auto numEmittedBefore = getNumStatementsEmitted();
  emitStatementBlock(*block);

  // If we emitted exactly one statement, then we are done.
  if (getNumStatementsEmitted() - numEmittedBefore == 1)
    return;

  // Otherwise we emit the begin and end logic.
  StringRef beginStr = " begin";
  outBuffer.insert(outBuffer.begin() + beginInsertPoint, beginStr.begin(),
                   beginStr.end());

  indent() << "end";
  if (!multiLineComment.empty())
    os << " // " << multiLineComment;
  os << '\n';
}

LogicalResult StmtEmitter::visitSV(IfOp op) {
  SmallPtrSet<Operation *, 8> ops;
  ops.insert(op);

  indent() << "if (";

  // If we have an else and and empty then block, emit an inverted condition.
  if (!op.hasElse() || !op.getThenBlock()->empty()) {
    // Normal emission.
    emitExpression(op.cond(), ops);
    os << ')';
    emitBlockAsStatement(op.getThenBlock(), ops);
    if (op.hasElse()) {
      indent() << "else";
      emitBlockAsStatement(op.getElseBlock(), ops);
    }
  } else {
    // inverted condition.
    os << '!';
    emitExpression(op.cond(), ops, Unary);
    os << ')';
    emitBlockAsStatement(op.getElseBlock(), ops);
  }

  // We count if as multiple statements to make sure it is always surrounded by
  // a begin/end so we don't get if/else confusion in cases like this:
  // if (cond)
  //   if (otherCond)    // This should force a begin!
  //     stmt
  // else                // Goes with the outer if!
  //   thing;
  ++numStatementsEmitted;
  return success();
}

LogicalResult StmtEmitter::visitSV(AlwaysOp op) {
  SmallPtrSet<Operation *, 8> ops;
  ops.insert(op);

  auto printEvent = [&](AlwaysOp::Condition cond) {
    os << stringifyEventControl(cond.event) << ' ';
    emitEventControlExpression(cond.value, ops);
  };

  switch (op.getNumConditions()) {
  case 0:
    indent() << "always @*";
    break;
  case 1:
    indent() << "always @(";
    printEvent(op.getCondition(0));
    os << ')';
    break;
  default:
    indent() << "always @(";
    printEvent(op.getCondition(0));
    for (size_t i = 1, e = op.getNumConditions(); i != e; ++i) {
      os << " or ";
      printEvent(op.getCondition(i));
    }
    os << ')';
    break;
  }

  // Build the comment string, leave out the signal expressions (since they
  // can be large).
  std::string comment;
  if (op.getNumConditions() == 0) {
    comment = "always @*";
  } else {
    comment = "always @(";
    llvm::interleave(
        op.events(),
        [&](Attribute eventAttr) {
          auto event = EventControl(eventAttr.cast<IntegerAttr>().getInt());
          comment += stringifyEventControl(event);
        },
        [&]() { comment += ", "; });
    comment += ')';
  }

  emitBlockAsStatement(op.getBodyBlock(), ops, comment);
  return success();
}

LogicalResult StmtEmitter::visitSV(AlwaysCombOp op) {
  SmallPtrSet<Operation *, 8> ops;
  ops.insert(op);

  indent() << "always_comb";
  emitBlockAsStatement(op.getBodyBlock(), ops, "always_comb");
  return success();
}

LogicalResult StmtEmitter::visitSV(AlwaysFFOp op) {
  SmallPtrSet<Operation *, 8> ops;
  ops.insert(op);

  StringRef opString = "always";
  if (state.options.useAlwaysFF)
    opString = "always_ff";

  indent() << opString << " @(" << stringifyEventControl(op.clockEdge()) << " ";
  emitEventControlExpression(op.clock(), ops);
  if (op.resetStyle() == ResetType::AsyncReset) {
    os << " or " << stringifyEventControl(*op.resetEdge()) << " ";
    emitEventControlExpression(op.reset(), ops);
  }
  os << ')';

  // Build the comment string, leave out the signal expressions (since they
  // can be large).
  std::string comment;
  comment += opString.str() + " @(";
  comment += stringifyEventControl(op.clockEdge());
  if (op.resetStyle() == ResetType::AsyncReset) {
    comment += " or ";
    comment += stringifyEventControl(*op.resetEdge());
  }
  comment += ')';

  if (op.resetStyle() == ResetType::NoReset)
    emitBlockAsStatement(op.getBodyBlock(), ops, comment);
  else {
    os << " begin";
    emitLocationInfoAndNewLine(ops);
    addIndent();

    indent() << "if (";
    // Negative edge async resets need to invert the reset condition.  This is
    // noted in the op description.
    if (op.resetStyle() == ResetType::AsyncReset &&
        *op.resetEdge() == EventControl::AtNegEdge)
      os << "!";
    emitExpression(op.reset(), ops);
    os << ')';
    emitBlockAsStatement(op.getResetBlock(), ops);
    indent() << "else";
    emitBlockAsStatement(op.getBodyBlock(), ops);
    reduceIndent();

    indent() << "end";
    os << " // " << comment;
    os << '\n';
  }
  return success();
}

LogicalResult StmtEmitter::visitSV(InitialOp op) {
  SmallPtrSet<Operation *, 8> ops;
  ops.insert(op);

  indent() << "initial";
  emitBlockAsStatement(op.getBodyBlock(), ops, "initial");
  return success();
}

LogicalResult StmtEmitter::visitSV(CaseZOp op) {
  SmallPtrSet<Operation *, 8> ops, emptyOps;
  ops.insert(op);

  indent() << "casez (";
  emitExpression(op.cond(), ops);
  os << ')';
  emitLocationInfoAndNewLine(ops);

  addIndent();
  for (auto caseInfo : op.getCases()) {
    auto pattern = caseInfo.pattern;

    if (pattern.isDefault())
      indent() << "default";
    else {
      // TODO: We could emit in hex if/when the size is a multiple of 4 and
      // there are no x's crossing nibble boundaries.
      indent() << pattern.getWidth() << "'b";
      for (size_t bit = 0, e = pattern.getWidth(); bit != e; ++bit)
        os << CaseZOp::getLetter(pattern.getBit(e - bit - 1),
                                 /*isVerilog*/ true);
    }
    os << ":";
    emitBlockAsStatement(caseInfo.block, emptyOps);
  }

  reduceIndent();
  indent() << "endcase";
  emitLocationInfoAndNewLine(ops);
  return success();
}

LogicalResult StmtEmitter::visitStmt(InstanceOp op) {
  StringRef prefix = "";
  if (op->hasAttr("doNotPrint")) {
    prefix = "// ";
    indent() << "// This instance is elsewhere emitted as a bind statement.\n";
  }

  SmallPtrSet<Operation *, 8> ops;
  ops.insert(op);

  auto *moduleOp = op.getReferencedModule();
  assert(moduleOp && "Invalid IR");

  // Use the specified name or the symbol name as appropriate.
  auto verilogName = getVerilogModuleNameAttr(moduleOp);
  emitter.verifyModuleName(op, verilogName);
  indent() << prefix << verilogName.getValue();

  // Helper that prints a parameter constant value in a Verilog compatible way.
  auto printParmValue = [&](Identifier paramName, Attribute value) {
    if (auto intAttr = value.dyn_cast<IntegerAttr>()) {
      IntegerType intTy = intAttr.getType().cast<IntegerType>();
      APInt value = intAttr.getValue();

      // We omit the width specifier if the value is <= 32-bits in size, which
      // makes this more compatible with unknown width extmodules.
      if (intTy.getWidth() > 32) {
        // Sign comes out before any width specifier.
        if (intTy.isSigned() && value.isNegative()) {
          os << '-';
          value = -value;
        }
        if (intTy.isSigned())
          os << intTy.getWidth() << "'sd";
        else
          os << intTy.getWidth() << "'d";
      }
      value.print(os, intTy.isSigned());
    } else if (auto strAttr = value.dyn_cast<StringAttr>()) {
      os << '"';
      os.write_escaped(strAttr.getValue());
      os << '"';
    } else if (auto fpAttr = value.dyn_cast<FloatAttr>()) {
      // TODO: relying on float printing to be precise is not a good idea.
      os << fpAttr.getValueAsDouble();
    } else if (auto verbatimParam = value.dyn_cast<VerbatimParameterAttr>()) {
      os << verbatimParam.getValue().getValue();
    } else {
      os << "<<UNKNOWN MLIRATTR: " << value << ">>";
      emitOpError(op, "unknown extmodule parameter value '")
          << paramName << "' = " << value;
    }
  };

  // If this is a parameterized module, then emit the parameters.
  if (auto paramDictOpt = op.parameters()) {
    DictionaryAttr paramDict = paramDictOpt.getValue();
    if (!paramDict.empty()) {
      os << " #(\n";
      llvm::interleave(
          paramDict, os,
          [&](NamedAttribute elt) {
            os.indent(state.currentIndent + 2)
                << prefix << '.' << elt.first << '(';
            printParmValue(elt.first, elt.second);
            os << ')';
          },
          ",\n");
      os << '\n';
      indent() << prefix << ')';
    }
  }

  os << ' ' << names.getName(op) << " (";

  SmallVector<ModulePortInfo> portInfo = getModulePortInfo(moduleOp);

  // Get the max port name length so we can align the '('.
  size_t maxNameLength = 0;
  for (auto &elt : portInfo) {
    maxNameLength = std::max(maxNameLength, elt.getName().size());
  }

  auto getWireForValue = [&](Value result) {
    return result.getUsers().begin()->getOperand(0);
  };

  // Emit the argument and result ports.
  auto opArgs = op.inputs();
  auto opResults = op.getResults();
  bool isFirst = true; // True until we print a port.
  SmallVector<Value, 32> portValues;
  for (auto &elt : portInfo) {
    // Figure out which value we are emitting.
    portValues.push_back(elt.isOutput() ? opResults[elt.argNum]
                                        : opArgs[elt.argNum]);
  }

  for (size_t portNum = 0, e = portValues.size(); portNum < e; ++portNum) {
    // Figure out which value we are emitting.
    auto &elt = portInfo[portNum];
    Value portVal = portValues[portNum];
    bool isZeroWidth = isZeroBitType(portVal.getType());

    // Decide if we should print a comma.  We can't do this if we're the first
    // port or if all the subsequent ports are zero width.
    if (!isFirst) {
      bool shouldPrintComma = true;
      if (isZeroWidth) {
        shouldPrintComma = false;
        for (size_t i = (&elt - portInfo.data()) + 1, e = portInfo.size();
             i != e; ++i)
          if (!isZeroBitType(portValues[i].getType())) {
            shouldPrintComma = true;
            break;
          }
      }

      if (shouldPrintComma)
        os << ',';
    }
    emitLocationInfoAndNewLine(ops);

    // Emit the port's name.
    indent() << prefix;
    if (!isZeroWidth) {
      // If this is a real port we're printing, then it isn't the first one. Any
      // subsequent ones will need a comma.
      isFirst = false;
      os << "  ";
    } else {
      // We comment out zero width ports, so their presence and initializer
      // expressions are still emitted textually.
      os << "//";
    }

    os << '.' << elt.getName();
    os.indent(maxNameLength - elt.getName().size()) << " (";

    // Emit the value as an expression.
    ops.clear();

    // Output ports that are not connected to single use output ports were
    // lowered to wire.
    OutputOp output;
    if (!elt.isOutput()) {
      emitExpression(portVal, ops);
    } else if (portVal.hasOneUse() &&
               (output = dyn_cast_or_null<OutputOp>(
                    portVal.getUses().begin()->getOwner()))) {
      auto module = output->getParentOfType<HWModuleOp>();
      auto name = getModuleResultNameAttr(
          module, portVal.getUses().begin()->getOperandNumber());
      os << name.getValue().str();
    } else {
      portVal = getWireForValue(portVal);
      emitExpression(portVal, ops);
    }
    os << ')';
  }
  if (!isFirst) {
    emitLocationInfoAndNewLine(ops);
    ops.clear();
    indent() << prefix;
  }
  os << ");";
  emitLocationInfoAndNewLine(ops);
  return success();
}

// This may be called in the top-level, not just in an hw.module.  Thus we can't
// use the name map to find expression names for arguments to the instance, nor
// do we need to emit subexpressions.  Prepare pass, which has run for all
// modules prior to this, has ensured that all arguments are bound to wires,
// regs, or ports, with legalized names, so we can lookup up the names through
// the IR.
LogicalResult StmtEmitter::visitSV(BindOp op) {
  emitter.emitBind(op);
  return success();
}

LogicalResult StmtEmitter::visitSV(InterfaceOp op) {
  os << "interface " << op.sym_name() << ";\n";
  emitStatementBlock(*op.getBodyBlock());
  os << "endinterface\n\n";
  return success();
}

LogicalResult StmtEmitter::visitSV(InterfaceSignalOp op) {
  indent();
  printPackedType(stripUnpackedTypes(op.type()), os, op, false);
  os << ' ' << op.sym_name();
  printUnpackedTypePostfix(op.type(), os);
  os << ";\n";
  return success();
}

LogicalResult StmtEmitter::visitSV(InterfaceModportOp op) {
  indent() << "modport " << op.sym_name() << '(';

  llvm::interleaveComma(op.ports(), os, [&](const Attribute &portAttr) {
    auto port = portAttr.cast<ModportStructAttr>();
    os << port.direction().getValue() << ' ' << port.signal().getValue();
  });

  os << ");\n";
  return success();
}

LogicalResult StmtEmitter::visitSV(AssignInterfaceSignalOp op) {
  SmallPtrSet<Operation *, 8> emitted;
  indent() << "assign ";
  emitExpression(op.iface(), emitted);
  os << '.' << op.signalName() << " = ";
  emitExpression(op.rhs(), emitted);
  os << ";\n";
  return success();
}

void StmtEmitter::emitStatement(Operation *op) {
  // Know where the start of this statement is in case any out of band precuror
  // statements need to be emitted.
  statementBeginningIndex = outBuffer.size();

  // Expressions may either be ignored or emitted as an expression statements.
  if (isVerilogExpression(op)) {
    if (emitter.outOfLineExpressions.count(op)) {
      ++numStatementsEmitted;
      emitStatementExpression(op);
    }
    return;
  }

  ++numStatementsEmitted;

  // Handle HW statements.
  if (succeeded(dispatchStmtVisitor(op)))
    return;

  // Handle SV Statements.
  if (succeeded(dispatchSVVisitor(op)))
    return;

  emitOpError(op, "cannot emit this operation to Verilog");
  indent() << "unknown MLIR operation " << op->getName().getStringRef() << "\n";
}

void StmtEmitter::collectNamesEmitDecls(Block &block) {
  // In the first pass, we fill in the symbol table, calculate the max width
  // of the declaration words and the max type width.
  NameCollector collector(emitter, names);
  collector.collectNames(block);

  auto &valuesToEmit = collector.getValuesToEmit();
  size_t maxDeclNameWidth = collector.getMaxDeclNameWidth();
  size_t maxTypeWidth = collector.getMaxTypeWidth();

  if (maxTypeWidth > 0) // add a space if any type exists
    maxTypeWidth += 1;

  SmallPtrSet<Operation *, 8> ops;

  // Okay, now that we have measured the things to emit, emit the things.
  for (const auto &record : valuesToEmit) {
    auto *decl = record.value.getDefiningOp();
    ops.clear();
    ops.insert(decl);

    // Emit the leading word, like 'wire' or 'reg'.
    auto type = record.value.getType();
    auto word = getVerilogDeclWord(decl);
    if (!isZeroBitType(type)) {
      indent() << word;
      auto extraIndent = word.empty() ? 0 : 1;
      os.indent(maxDeclNameWidth - word.size() + extraIndent);
    } else {
      indent() << "// Zero width: " << word << ' ';
    }

    // Emit the type.
    os << record.typeString;
    if (record.typeString.size() < maxTypeWidth)
      os.indent(maxTypeWidth - record.typeString.size());

    // Emit the name.
    os << names.getName(record.value);

    // Interface instantiations have parentheses like a module with no ports.
    if (type.isa<InterfaceType>()) {
      os << "()";
    } else {
      // Print out any array subscripts.
      printUnpackedTypePostfix(type, os);
    }

    // Constants carry their assignment directly in the declaration.
    if (isConstantExpression(decl)) {
      os << " = ";
      emitExpression(decl->getResult(0), ops, ForceEmitMultiUse);
    }

    os << ';';
    emitLocationInfoAndNewLine(ops);
  }

  if (!valuesToEmit.empty())
    os << '\n';
}

void StmtEmitter::emitStatementBlock(Block &body) {
  addIndent();

  // Build up the symbol table for all of the values that need names in the
  // module.  #ifdef's in procedural regions are special because local variables
  // are all emitted at the top of their enclosing blocks.
  if (!isa<IfDefProceduralOp>(body.getParentOp()))
    collectNamesEmitDecls(body);

  // Emit the body.
  for (auto &op : body) {
    emitStatement(&op);
  }

  reduceIndent();
}

void ModuleEmitter::emitStatement(Operation *op, ModuleNameManager &names) {
  SmallString<128> outputBuffer;
  StmtEmitter(*this, outputBuffer, names).emitStatement(op);
  os << outputBuffer;
}

void ModuleEmitter::emitStatementBlock(Block &body, ModuleNameManager &names) {
  SmallString<128> outputBuffer;
  StmtEmitter(*this, outputBuffer, names).emitStatementBlock(body);
  os << outputBuffer;
}

//===----------------------------------------------------------------------===//
// Module Driver
//===----------------------------------------------------------------------===//

void ModuleEmitter::emitHWExternModule(HWModuleExternOp module) {
  auto verilogName = module.getVerilogModuleNameAttr();
  verifyModuleName(module, verilogName);
  os << "// external module " << verilogName.getValue() << "\n\n";
}

void ModuleEmitter::emitHWGeneratedModule(HWModuleGeneratedOp module) {
  auto verilogName = module.getVerilogModuleNameAttr();
  verifyModuleName(module, verilogName);
  os << "// external generated module " << verilogName.getValue() << "\n\n";
}

// This may be called in the top-level, not just in an hw.module.  Thus we can't
// use the name map to find expression names for arguments to the instance, nor
// do we need to emit subexpressions.  Prepare pass, which has run for all
// modules prior to this, has ensured that all arguments are bound to wires,
// regs, or ports, with legalized names, so we can lookup up the names through
// the IR.
void ModuleEmitter::emitBind(BindOp op) {
  InstanceOp inst = op.getReferencedInstance();

  HWModuleOp parentMod = inst->getParentOfType<hw::HWModuleOp>();
  auto parentVerilogName = getVerilogModuleNameAttr(parentMod);
  verifyModuleName(op, parentVerilogName);

  Operation *childMod = inst.getReferencedModule();
  auto childVerilogName = getVerilogModuleNameAttr(childMod);
  verifyModuleName(op, childVerilogName);

  indent() << "bind " << parentVerilogName.getValue() << " "
           << childVerilogName.getValue() << ' ' << inst.getName() << " (";

  SmallVector<ModulePortInfo> parentPortInfo = parentMod.getPorts();
  SmallVector<ModulePortInfo> childPortInfo = getModulePortInfo(childMod);

  // Get the max port name length so we can align the '('.
  size_t maxNameLength = 0;
  for (auto &elt : childPortInfo) {
    maxNameLength = std::max(maxNameLength, elt.getName().size());
  }

  // Emit the argument and result ports.
  auto opArgs = inst.inputs();
  auto opResults = inst.getResults();
  bool isFirst = true; // True until we print a port.
  for (auto &elt : childPortInfo) {
    // Figure out which value we are emitting.
    Value portVal = elt.isOutput() ? opResults[elt.argNum] : opArgs[elt.argNum];
    bool isZeroWidth = isZeroBitType(elt.type);

    // Decide if we should print a comma.  We can't do this if we're the first
    // port or if all the subsequent ports are zero width.
    if (!isFirst) {
      bool shouldPrintComma = true;
      if (isZeroWidth) {
        shouldPrintComma = false;
        for (size_t i = (&elt - childPortInfo.data()) + 1,
                    e = childPortInfo.size();
             i != e; ++i)
          if (!isZeroBitType(childPortInfo[i].type)) {
            shouldPrintComma = true;
            break;
          }
      }

      if (shouldPrintComma)
        os << ',';
    }
    os << "\n";

    // Emit the port's name.
    indent();
    if (!isZeroWidth) {
      // If this is a real port we're printing, then it isn't the first one. Any
      // subsequent ones will need a comma.
      isFirst = false;
      os << "  ";
    } else {
      // We comment out zero width ports, so their presence and initializer
      // expressions are still emitted textually.
      os << "//";
    }

    os << '.' << elt.getName();
    os.indent(maxNameLength - elt.getName().size()) << " (";

    // Emit the value as an expression.
    auto name = getNameRemotely(portVal, parentPortInfo);
    if (name.empty()) {
      // Non stable names will come from expressions.  Since we are lowering the
      // instance also, we can ensure that expressions feeding bound instances
      // will be lowered consistently to verilog-namable entities.
      os << childVerilogName.getValue() << '_' << inst.getName() << '_'
         << elt.getName() << ')';
    } else {
      os << name << ')';
    }
  }
  if (!isFirst) {
    os << "\n";
    indent();
  }
  os << ");\n";
}

// Check if the value is from read of a wire or reg or is a port.
static bool isSimpleReadOrPort(Value v) {
  if (v.isa<BlockArgument>())
    return true;
  auto vOp = v.getDefiningOp();
  if (!vOp)
    return false;
  auto read = dyn_cast<ReadInOutOp>(vOp);
  if (!read)
    return false;
  auto readSrc = read.input().getDefiningOp();
  if (!readSrc)
    return false;
  return isa<WireOp, RegOp>(readSrc);
}

// Given an invisible instance, make sure all inputs are driven from
// wires or ports.
static void lowerBoundInstance(InstanceOp op) {
  Block *block = op->getParentOfType<HWModuleOp>().getBodyBlock();
  auto builder = ImplicitLocOpBuilder::atBlockBegin(op.getLoc(), block);

  SmallString<32> nameTmp;
  nameTmp = (op.instanceName() + "_").str();
  auto namePrefixSize = nameTmp.size();

  size_t nextOpNo = 0;
  for (auto &port : getModulePortInfo(op.getReferencedModule())) {
    if (port.isOutput())
      continue;

    auto src = op.getOperand(nextOpNo);
    ++nextOpNo;

    if (isSimpleReadOrPort(src))
      continue;

    nameTmp.resize(namePrefixSize);
    if (port.name)
      nameTmp += port.name.getValue().str();
    else
      nameTmp += std::to_string(nextOpNo - 1);

    auto newWire = builder.create<WireOp>(src.getType(), nameTmp);
    auto newWireRead = builder.create<ReadInOutOp>(newWire);
    auto connect = builder.create<AssignOp>(newWire, src);
    newWireRead->moveBefore(op);
    connect->moveBefore(op);
    op.setOperand(nextOpNo - 1, newWireRead);
  }
}

static bool onlyUseIsAssign(Value v) {
  if (!v.hasOneUse())
    return false;
  if (!dyn_cast_or_null<AssignOp>(v.getDefiningOp()))
    return false;
  return true;
}

// Ensure that each output of an instance are used only by a wire
static void lowerInstanceResults(InstanceOp op) {
  Block *block = op->getParentOfType<HWModuleOp>().getBodyBlock();
  auto builder = ImplicitLocOpBuilder::atBlockBegin(op.getLoc(), block);

  SmallString<32> nameTmp;
  nameTmp = (op.instanceName() + "_").str();
  auto namePrefixSize = nameTmp.size();

  size_t nextResultNo = 0;
  for (auto &port : getModulePortInfo(op.getReferencedModule())) {
    if (!port.isOutput())
      continue;

    auto result = op.getResult(nextResultNo);
    ++nextResultNo;

    if (onlyUseIsAssign(result))
      continue;

    bool isOneUseOutput = false;
    if (result.hasOneUse()) {
      OpOperand &use = *result.getUses().begin();
      isOneUseOutput = dyn_cast_or_null<OutputOp>(use.getOwner()) != nullptr;
    }

    if (!isOneUseOutput) {
      nameTmp.resize(namePrefixSize);
      if (port.name)
        nameTmp += port.name.getValue().str();
      else
        nameTmp += std::to_string(nextResultNo - 1);

      auto newWire = builder.create<WireOp>(result.getType(), nameTmp);
      while (!result.use_empty()) {
        auto newWireRead = builder.create<ReadInOutOp>(newWire);
        OpOperand &use = *result.getUses().begin();
        use.set(newWireRead);
        newWireRead->moveBefore(use.getOwner());
      }

      auto connect = builder.create<AssignOp>(newWire, result);
      connect->moveAfter(op);
    }
  }
}

// Given a side effect free "always inline" operation, make sure that it
// exists in the same block as its users and that it has one use for each one.
static void lowerAlwaysInlineOperation(Operation *op) {
  assert(op->getNumResults() == 1 &&
         "only support 'always inline' ops with one result");

  // Nuke use-less operations.
  if (op->use_empty()) {
    op->erase();
    return;
  }

  // Moving/cloning an op should pull along its operand tree with it if they
  // are always inline.  This happens when an array index has a constant
  // operand for example.
  auto recursivelyHandleOperands = [](Operation *op) {
    for (auto operand : op->getOperands()) {
      if (auto *operandOp = operand.getDefiningOp())
        if (isExpressionAlwaysInline(operandOp))
          lowerAlwaysInlineOperation(operandOp);
    }
  };

  // If this operation has multiple uses, duplicate it into N-1 of them in
  // turn.
  while (!op->hasOneUse()) {
    OpOperand &use = *op->getUses().begin();
    Operation *user = use.getOwner();

    // Clone the op before the user.
    auto *newOp = op->clone();
    user->getBlock()->getOperations().insert(Block::iterator(user), newOp);
    // Change the user to use the new op.
    use.set(newOp->getResult(0));

    // If any of the operations of the moved op are always inline, recursively
    // handle them too.
    recursivelyHandleOperands(newOp);
  }

  // Finally, ensures the op is in the same block as its user so it can be
  // inlined.
  Operation *user = *op->getUsers().begin();
  if (op->getBlock() != user->getBlock()) {
    op->moveBefore(user);

    // If any of the operations of the moved op are always inline, recursively
    // move/clone them too.
    recursivelyHandleOperands(op);
  }
  return;
}

/// We lower the Merge operation to a wire at the top level along with
/// assigns to it and a ReadInOut.
static Value lowerMergeOp(MergeOp merge) {
  auto module = merge->getParentOfType<HWModuleOp>();
  assert(module && "merges should only be in a module");

  // Start with the wire at the top level.
  ImplicitLocOpBuilder b(merge.getLoc(), &module.getBodyBlock()->front());
  auto wire = b.create<WireOp>(merge.getType());

  // Each of the operands is an assign or passign into the wire.
  b.setInsertionPoint(merge);
  if (merge->getParentOp()->hasTrait<sv::ProceduralRegion>()) {
    for (auto op : merge.getOperands())
      b.create<PAssignOp>(wire, op);
  } else {
    for (auto op : merge.getOperands())
      b.create<AssignOp>(wire, op);
  }

  return b.create<ReadInOutOp>(wire);
}

/// Lower a commutative operation into an expression tree.  This enables
/// long-line splitting to work with them.
static Value lowerVariadicCommutativeOp(Operation &op, OperandRange operands) {
  Value lhs, rhs;
  switch (operands.size()) {
  case 0:
    assert(0 && "cannot be called with empty operand range");
    break;
  case 1:
    return operands[0];
  case 2:
    lhs = operands[0];
    rhs = operands[1];
    break;
  default:
    auto firstHalf = operands.size() / 2;
    lhs = lowerVariadicCommutativeOp(op, operands.take_front(firstHalf));
    rhs = lowerVariadicCommutativeOp(op, operands.drop_front(firstHalf));
    break;
  }

  OperationState state(op.getLoc(), op.getName());
  state.addOperands(ValueRange{lhs, rhs});
  state.addTypes(op.getResult(0).getType());
  auto *newOp = Operation::create(state);
  op.getBlock()->getOperations().insert(Block::iterator(&op), newOp);
  return newOp->getResult(0);
}

/// When we find that an operation is used before it is defined in a graph
/// region, we emit an explicit wire to resolve the issue.
static void lowerUsersToTemporaryWire(Operation &op) {
  Block *block = op.getBlock();
  auto builder = ImplicitLocOpBuilder::atBlockBegin(op.getLoc(), block);

  for (auto result : op.getResults()) {
    auto newWire = builder.create<WireOp>(result.getType());

    while (!result.use_empty()) {
      auto newWireRead = builder.create<ReadInOutOp>(newWire);
      OpOperand &use = *result.getUses().begin();
      use.set(newWireRead);
      newWireRead->moveBefore(use.getOwner());
    }

    auto connect = builder.create<AssignOp>(newWire, result);
    connect->moveAfter(&op);
  }
}

void ModuleEmitter::emitHWModule(HWModuleOp module, ModuleNameManager &names) {
  // Add all the ports to the name table.
  SmallVector<ModulePortInfo> portInfo = module.getPorts();
  for (auto &port : portInfo) {
    StringRef name = port.getName();
    if (name.empty()) {
      emitOpError(module,
                  "Found port without a name. Port names are required for "
                  "Verilog synthesis.\n");
      name = "<<NO-NAME-FOUND>>";
    }
    if (port.isOutput())
      names.addOutputNames(name, module);
    else
      names.addLegalName(module.getArgument(port.argNum), name, module);
  }

  auto moduleNameAttr = module.getNameAttr();
  verifyModuleName(module, moduleNameAttr);
  os << "module " << moduleNameAttr.getValue() << '(';
  if (!portInfo.empty())
    os << '\n';

  // Determine the width of the widest type we have to print so everything
  // lines up nicely.
  bool hasOutputs = false, hasZeroWidth = false;
  size_t maxTypeWidth = 0;
  SmallVector<SmallString<8>, 16> portTypeStrings;

  for (auto &port : portInfo) {
    hasOutputs |= port.isOutput();
    hasZeroWidth |= isZeroBitType(port.type);

    // Convert the port's type to a string and measure it.
    portTypeStrings.push_back({});
    {
      llvm::raw_svector_ostream stringStream(portTypeStrings.back());
      printPackedType(stripUnpackedTypes(port.type), stringStream, module);
    }

    maxTypeWidth = std::max(portTypeStrings.back().size(), maxTypeWidth);
  }

  if (maxTypeWidth > 0) // add a space if any type exists
    maxTypeWidth += 1;

  addIndent();

  for (size_t portIdx = 0, e = portInfo.size(); portIdx != e;) {
    size_t startOfLinePos = os.tell();

    indent();
    // Emit the arguments.
    auto portType = portInfo[portIdx].type;
    bool isZeroWidth = false;
    if (hasZeroWidth) {
      isZeroWidth = isZeroBitType(portType);
      os << (isZeroWidth ? "// " : "   ");
    }

    PortDirection thisPortDirection = portInfo[portIdx].direction;
    switch (thisPortDirection) {
    case PortDirection::OUTPUT:
      os << "output ";
      break;
    case PortDirection::INPUT:
      os << (hasOutputs ? "input  " : "input ");
      break;
    case PortDirection::INOUT:
      os << (hasOutputs ? "inout  " : "inout ");
      break;
    }

    // Emit the type.
    os << portTypeStrings[portIdx];
    if (portTypeStrings[portIdx].size() < maxTypeWidth)
      os.indent(maxTypeWidth - portTypeStrings[portIdx].size());

    auto getPortName = [&](size_t portIdx) -> StringRef {
      if (portInfo[portIdx].isOutput())
        return names.getOutputName(portInfo[portIdx].argNum);
      else
        return names.getName(module.getArgument(portInfo[portIdx].argNum));
    };

    // Emit the name.
    os << getPortName(portIdx);
    printUnpackedTypePostfix(portType, os);
    ++portIdx;

    auto lineLength = state.options.emittedLineLength;

    // If we have any more ports with the same types and the same direction,
    // emit them in a list on the same line.
    while (portIdx != e && portInfo[portIdx].direction == thisPortDirection &&
           stripUnpackedTypes(portType) ==
               stripUnpackedTypes(portInfo[portIdx].type)) {
      // Don't exceed our preferred line length.
      StringRef name = getPortName(portIdx);
      if (os.tell() + 2 + name.size() - startOfLinePos >
          // We use "-2" here because we need a trailing comma or ); for the
          // decl.
          lineLength - 2)
        break;

      // Append this to the running port decl.
      os << ", " << name;
      printUnpackedTypePostfix(portInfo[portIdx].type, os);
      ++portIdx;
    }

    if (portIdx != e)
      os << ',';
    else if (isZeroWidth)
      os << "\n   );\n";
    else
      os << ");\n";
    os << '\n';
  }

  if (portInfo.empty())
    os << ");\n";
  reduceIndent();

  // Emit the body of the module.
  emitStatementBlock(*module.getBodyBlock(), names);

  os << "endmodule\n\n";
}

//===----------------------------------------------------------------------===//
// Common functionality for root module emitters
//===----------------------------------------------------------------------===//

namespace {

/// Information to control the emission of a single operation into a file.
struct OpFileInfo {
  /// The operation to be emitted.
  Operation *op;

  /// Where among the replicated per-file operations the `op` above should be
  /// emitted.
  size_t position = 0;
};

/// Information to control the emission of a list of operations into a file.
struct FileInfo {
  /// The operations to be emitted into a separate file, and where among the
  /// replicated per-file operations the operation should be emitted.
  SmallVector<OpFileInfo, 1> ops;

  /// Whether to emit the replicated per-file operations.
  bool emitReplicatedOps = true;

  /// Whether to include this file as part of the emitted file list.
  bool addToFilelist = true;
};

/// A base class for all MLIR module emitters.
struct RootEmitterBase {
  /// The MLIR module to emit.
  ModuleOp rootOp;

  /// The main file that collects all operations that are neither replicated
  /// per-file ops nor specifically assigned to a file.
  FileInfo rootFile;

  /// The additional files to emit, with the output file name as the key into
  /// the map.
  llvm::MapVector<Identifier, FileInfo> files;

  /// A list of operations replicated in each output file (e.g., `sv.verbatim`
  /// or `sv.ifdef` without dedicated output file).
  SmallVector<Operation *, 0> replicatedOps;

  /// Whether any error has been encountered during emission.
  std::atomic<bool> encounteredError = {};

  /// Legalized names for each module
  llvm::DenseMap<Operation *, ModuleNameManager> legalizedNames;

  explicit RootEmitterBase(ModuleOp rootOp) : rootOp(rootOp) {}
  void prepareAllModules();
  void gatherFiles(bool separateModules);
  void emitFile(const FileInfo &fileInfo, VerilogEmitterState &state);
  void emitOperation(VerilogEmitterState &state, Operation *op);
};

} // namespace

/// For each module we emit, do a prepass over the structure, pre-lowering and
/// otherwise rewriting operations we don't want to emit.
static void prepareHWModule(Block &block, ModuleNameManager &names) {
  for (auto &op : llvm::make_early_inc_range(block)) {
    // If the operations has regions, lower each of the regions.
    for (auto &region : op.getRegions()) {
      if (!region.empty())
        prepareHWModule(region.front(), names);
    }

    // Duplicate "always inline" expression for each of their users and move
    // them to be next to their users.
    if (isExpressionAlwaysInline(&op)) {
      lowerAlwaysInlineOperation(&op);
      continue;
    }

    // Lower 'merge' operations to wires and connects.
    if (auto merge = dyn_cast<MergeOp>(op)) {
      auto result = lowerMergeOp(merge);
      op.getResult(0).replaceAllUsesWith(result);
      op.erase();
      continue;
    }

    // Lower commutative variadic operations with more than two operands into
    // balanced operand trees so we can split long lines across multiple
    // statements.
    if (op.getNumOperands() > 2 && op.getNumResults() == 1 &&
        op.hasTrait<mlir::OpTrait::IsCommutative>() &&
        mlir::MemoryEffectOpInterface::hasNoEffect(&op) &&
        op.getNumRegions() == 0 && op.getNumSuccessors() == 0 &&
        op.getAttrs().empty()) {
      // Lower this operation to a balanced binary tree of the same operation.
      auto result = lowerVariadicCommutativeOp(op, op.getOperands());
      op.getResult(0).replaceAllUsesWith(result);
      op.erase();
      continue;
    }

    // Name legalization should have happened in a different pass for these sv
    // elements and we don't want to change their name through re-legalization
    // (e.g. letting a temporary take the name of an unvisited wire). Adding
    // them now ensures any temporary generated will not use one of the names
    // previously declared.
    if (auto instance = dyn_cast<InstanceOp>(op)) {
      // Anchor return values to wires early
      lowerInstanceResults(instance);
      // Anchor ports of bound instances
      if (instance->hasAttr("doNotPrint"))
        lowerBoundInstance(instance);
      names.addLegalName(&op, instance.instanceName(), &op);
    } else if (auto wire = dyn_cast<WireOp>(op))
      names.addLegalName(op.getResult(0), wire.name(), &op);
    else if (auto regOp = dyn_cast<RegOp>(op))
      names.addLegalName(op.getResult(0), regOp.name(), &op);
  }

  // Now that all the basic ops are settled, check for any use-before def issues
  // in graph regions.  Lower these into explicit wires to keep the emitter
  // simple.
  if (!block.getParentOp()->hasTrait<ProceduralRegion>()) {
    SmallPtrSet<Operation *, 32> seenOperations;

    for (auto &op : llvm::make_early_inc_range(block)) {
      // Check the users of any expressions to see if they are
      // lexically below the operation itself.  If so, it is being used out
      // of order.
      bool haveAnyOutOfOrderUses = false;
      for (auto *userOp : op.getUsers()) {
        // If the user is in a suboperation like an always block, then zip up
        // to the operation that uses it.
        while (&block != &userOp->getParentRegion()->front())
          userOp = userOp->getParentOp();

        if (seenOperations.count(userOp)) {
          haveAnyOutOfOrderUses = true;
          break;
        }
      }

      // Remember that we've seen this operation.
      seenOperations.insert(&op);

      // If all the uses of the operation are below this, then we're ok.
      if (!haveAnyOutOfOrderUses)
        continue;

      // If this is a reg/wire declaration, then we move it to the top of the
      // block.  We can't abstract the inout result.
      if (op.getNumResults() == 1 &&
          op.getResult(0).getType().isa<InOutType>() &&
          op.getNumOperands() == 0) {
        op.moveBefore(&block.front());
        continue;
      }

      // If this is a constant, then we move it to the top of the block.
      if (isConstantExpression(&op)) {
        op.moveBefore(&block.front());
        continue;
      }

      // Otherwise, we need to lower this to a wire to resolve this.
      lowerUsersToTemporaryWire(op);
    }
  }
}

void RootEmitterBase::prepareAllModules() {
  bool hasError = false;
  for (auto op : rootOp.getBody()->getOps<HWModuleOp>()) {
    auto &names = legalizedNames[op];
    prepareHWModule(*op.getBodyBlock(), names);
    hasError |= names.hadError();
  }
  if (hasError)
    encounteredError = true;
}

/// Organize the operations in the root MLIR module into output files to be
/// generated. If `separateModules` is true, a handful of top-level
/// declarations will be split into separate output files even in the absence
/// of an explicit output file attribute.
void RootEmitterBase::gatherFiles(bool separateModules) {
  for (auto &op : *rootOp.getBody()) {
    auto info = OpFileInfo{&op, replicatedOps.size()};
    auto attr = op.getAttrOfType<hw::OutputFileAttr>("output_file");

    SmallString<32> outputPath;
    bool hasFileName = false;
    bool emitReplicatedOps = true;
    bool addToFilelist = true;

    // Check if the operation has an explicit `output_file` attribute set. If
    // it does, extract the information from the attribute.
    if (attr) {
      LLVM_DEBUG(llvm::dbgs() << "Found output_file attribute " << attr
                              << " on " << op << "\n";);

      if (auto directory = attr.directory())
        appendPossiblyAbsolutePath(outputPath, directory.getValue());

      if (auto name = attr.name()) {
        appendPossiblyAbsolutePath(outputPath, name.getValue());
        hasFileName = true;
      }

      emitReplicatedOps = !attr.exclude_replicated_ops().getValue();
      addToFilelist = !attr.exclude_from_filelist().getValue();
    }

    auto separateFile = [&](Operation *op, Twine fileName = "") {
      if (!hasFileName)
        llvm::sys::path::append(outputPath, fileName);

      auto &file = files[Identifier::get(outputPath, op->getContext())];
      file.ops.push_back(info);
      file.emitReplicatedOps = emitReplicatedOps;
      file.addToFilelist = addToFilelist;
    };

    // Separate the operation into dedicated output file, or emit into the
    // root file, or replicate in all output files.
    TypeSwitch<Operation *>(&op)
        .Case<HWModuleOp>([&](auto &mod) {
          // Emit into a separate file named after the module.
          if (attr || separateModules)
            separateFile(mod, mod.getName() + ".sv");
          else
            rootFile.ops.push_back(info);
        })
        .Case<InterfaceOp>([&](auto &intf) {
          // Emit into a separate file named after the interface.
          if (attr || separateModules)
            separateFile(intf, intf.sym_name() + ".sv");
          else
            rootFile.ops.push_back(info);
        })
        .Case<VerbatimOp, IfDefOp, TypeScopeOp, HWModuleExternOp>([&](auto &) {
          // Emit into a separate file using the specified file name or
          // replicate the operation in each outputfile.
          if (attr) {
            if (!hasFileName) {
              op.emitError("file name unspecified");
              encounteredError = true;
            } else
              separateFile(&op);
          } else
            replicatedOps.push_back(&op);
        })
        .Case<HWGeneratorSchemaOp>([&](auto &) {
          // Empty.
        })
        .Case<BindOp>([&](auto &op) {
          if (attr) {
            if (!hasFileName) {
              op.emitError("file name unspecified");
              encounteredError = true;
            } else
              separateFile(op);
          } else
            separateFile(op, "bindfile");
        })
        .Default([&](auto *) {
          op.emitError("unknown operation");
          encounteredError = true;
        });
  }
}

/// Emit the operations in a `FileInfo` to an output stream. This handles the
/// correct interpolation of replicated operations.
void RootEmitterBase::emitFile(const FileInfo &file,
                               VerilogEmitterState &state) {
  size_t lastReplicatedOp = 0;

  // Emit each operation in the file preceded by the replicated ops not yet
  // printed.
  for (const auto &opInfo : file.ops) {
    // Emit the replicated per-file operations before the main operation's
    // position (if enabled).
    if (file.emitReplicatedOps)
      for (; lastReplicatedOp < std::min(opInfo.position, replicatedOps.size());
           ++lastReplicatedOp)
        emitOperation(state, replicatedOps[lastReplicatedOp]);

    // Emit the operation itself.
    emitOperation(state, opInfo.op);
  }

  // Emit the replicated per-file operations after the last operation (if
  // enabled).
  if (file.emitReplicatedOps)
    for (; lastReplicatedOp < replicatedOps.size(); lastReplicatedOp++)
      emitOperation(state, replicatedOps[lastReplicatedOp]);

  if (state.encounteredError)
    encounteredError = true;
}

void RootEmitterBase::emitOperation(VerilogEmitterState &state, Operation *op) {
  TypeSwitch<Operation *>(op)
      .Case<HWModuleOp>([&](auto op) {
        ModuleEmitter(state).emitHWModule(op, legalizedNames[op]);
      })
      .Case<HWModuleExternOp>(
          [&](auto op) { ModuleEmitter(state).emitHWExternModule(op); })
      .Case<HWModuleGeneratedOp>(
          [&](auto op) { ModuleEmitter(state).emitHWGeneratedModule(op); })
      .Case<HWGeneratorSchemaOp>([&](auto op) { /* Empty */ })
      .Case<BindOp>([&](auto op) { ModuleEmitter(state).emitBind(op); })
      .Case<InterfaceOp, VerbatimOp, IfDefOp>([&](auto op) {
        ModuleNameManager emptyNames;
        ModuleEmitter(state).emitStatement(op, emptyNames);
      })
      .Case<TypeScopeOp>([&](auto typedecls) {
        TypeScopeEmitter(state).emitTypeScopeBlock(*typedecls.getBodyBlock());
      })
      .Default([&](auto *op) {
        encounteredError = true;
        op->emitError("unknown operation");
      });
}

//===----------------------------------------------------------------------===//
// Unified Emitter
//===----------------------------------------------------------------------===//

namespace {

/// A Verilog emitter that emits all modules into a single output stream.
struct UnifiedEmitter : public RootEmitterBase {
  explicit UnifiedEmitter(llvm::raw_ostream &os, ModuleOp rootOp)
      : RootEmitterBase(rootOp), os(os) {}

  /// The output stream to emit into.
  llvm::raw_ostream &os;

  void emitMLIRModule();
};

} // namespace

void UnifiedEmitter::emitMLIRModule() {
  VerilogEmitterState state(os);
  gatherFiles(false);

  // Read the emitter options out of the module.
  state.options.parseFromAttribute(rootOp);

  // Emit the main file. This is a container for anything not explicitly split
  // out into a separate file.
  emitFile(rootFile, state);

  // Emit the separate files.
  for (const auto &it : files) {
    state.os << "\n// ----- 8< ----- FILE \"" << it.first
             << "\" ----- 8< -----\n\n";
    emitFile(it.second, state);
  }
}

//===----------------------------------------------------------------------===//
// Split Emitter
//===----------------------------------------------------------------------===//

namespace {

/// A Verilog emitter that separates modules into individual output files.
struct SplitEmitter : public RootEmitterBase {
  explicit SplitEmitter(StringRef dirname, ModuleOp rootOp)
      : RootEmitterBase(rootOp), dirname(dirname) {}

  /// The directory to emit files into.
  StringRef dirname;

  void emitMLIRModule();
  void createFile(const LoweringOptions &options, Identifier fileName,
                  FileInfo &file);
};

} // namespace

void SplitEmitter::emitMLIRModule() {
  gatherFiles(true);

  // Load any emitter options from the top-level module.
  LoweringOptions options(rootOp);

  // Emit operations to separate files in parallel if enabled.
  if (rootOp.getContext()->isMultithreadingEnabled())
    llvm::parallelForEach(files.begin(), files.end(), [&](auto &it) {
      createFile(options, it.first, it.second);
    });
  else
    for (auto &it : files)
      createFile(options, it.first, it.second);
}

void SplitEmitter::createFile(const LoweringOptions &options,
                              Identifier fileName, FileInfo &file) {
  // Determine the output path from the output directory and filename.
  SmallString<128> outputFilename(dirname);
  appendPossiblyAbsolutePath(outputFilename, fileName.strref());
  auto outputDir = llvm::sys::path::parent_path(outputFilename);

  // Create the output directory if needed.
  std::error_code error = llvm::sys::fs::create_directory(outputDir);
  if (error) {
    mlir::emitError(file.ops[0].op->getLoc(),
                    "cannot create output directory \"" + outputDir +
                        "\": " + error.message());
    encounteredError = true;
  }

  // Open the output file.
  std::string errorMessage;
  auto output = mlir::openOutputFile(outputFilename, &errorMessage);
  if (!output) {
    encounteredError = true;
    llvm::errs() << errorMessage << "\n";
    return;
  }

  // Emit the file, copying the global options into the individual module
  // state.
  VerilogEmitterState state(output->os());
  state.options = options;
  emitFile(file, state);
  output->keep();
}

//===----------------------------------------------------------------------===//
// MLIRModuleEmitter
//===----------------------------------------------------------------------===//

LogicalResult circt::exportVerilog(ModuleOp module, llvm::raw_ostream &os) {
  UnifiedEmitter emitter(os, module);
  emitter.prepareAllModules();
  emitter.emitMLIRModule();
  return failure(emitter.encounteredError);
}

LogicalResult circt::exportSplitVerilog(ModuleOp module, StringRef dirname) {
  SplitEmitter emitter(dirname, module);
  emitter.prepareAllModules();
  emitter.emitMLIRModule();

  // Write the file list.
  SmallString<128> filelistPath(dirname);
  llvm::sys::path::append(filelistPath, "filelist.f");

  std::string errorMessage;
  auto output = mlir::openOutputFile(filelistPath, &errorMessage);
  if (!output) {
    module->emitError(errorMessage);
    return failure();
  }

  for (const auto &it : emitter.files) {
    if (it.second.addToFilelist)
      output->os() << it.first << "\n";
  }
  output->keep();

  return failure(emitter.encounteredError);
}

void circt::registerToVerilogTranslation() {
  // Register the circt emitter command line options.
  registerLoweringCLOptions();
  // Register the circt emitter translation.
  mlir::TranslateFromMLIRRegistration toVerilog(
      "export-verilog",
      [](ModuleOp module, llvm::raw_ostream &os) {
        // ExportVerilog requires that the SV dialect be loaded in order to
        // create WireOps. It may not have been  loaded by the MLIR parser,
        // which can happen if the input IR has no SV operations.
        module->getContext()->loadDialect<sv::SVDialect>();
        applyLoweringCLOptions(module);
        return exportVerilog(module, os);
      },
      [](mlir::DialectRegistry &registry) {
        registry.insert<CombDialect, HWDialect, SVDialect>();
      });
}
