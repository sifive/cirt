//===- LowerToHW.cpp - FIRRTL to HW/SV Lowering Pass ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This is the main FIRRTL to HW/SV Lowering Pass Implementation.
//
//===----------------------------------------------------------------------===//

#include "../PassDetail.h"
#include "circt/Conversion/FIRRTLToHW/FIRRTLToHW.h"
#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "circt/Dialect/FIRRTL/FIRRTLVisitors.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/HW/HWTypes.h"
#include "circt/Dialect/SV/SVOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/ImplicitLocOpBuilder.h"
#include "mlir/IR/Threading.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Parallel.h"

using namespace circt;
using namespace firrtl;
using circt::comb::ICmpPredicate;

/// Given a FIRRTL type, return the corresponding type for the HW dialect.
/// This returns a null type if it cannot be lowered.
static Type lowerType(Type type) {
  auto firType = type.dyn_cast<FIRRTLType>();
  if (!firType)
    return {};

  // Ignore flip types.
  firType = firType.getPassiveType();

  if (BundleType bundle = firType.dyn_cast<BundleType>()) {
    mlir::SmallVector<hw::StructType::FieldInfo, 8> hwfields;
    for (auto element : bundle.getElements()) {
      Type etype = lowerType(element.type);
      if (!etype)
        return {};
      // TODO: make hw::StructType contain StringAttrs.
      auto name = Identifier::get(element.name.getValue(), type.getContext());
      hwfields.push_back(hw::StructType::FieldInfo{name, etype});
    }
    return hw::StructType::get(type.getContext(), hwfields);
  }

  auto width = firType.getBitWidthOrSentinel();
  if (width >= 0) // IntType, analog with known width, clock, etc.
    return IntegerType::get(type.getContext(), width);

  return {};
}

/// Given two FIRRTL integer types, return the widest one.
static IntType getWidestIntType(Type t1, Type t2) {
  auto t1c = t1.cast<IntType>(), t2c = t2.cast<IntType>();
  return t2c.getWidth() > t1c.getWidth() ? t2c : t1c;
}

/// Cast from a standard type to a FIRRTL type, potentially with a flip.
static Value castToFIRRTLType(Value val, Type type,
                              ImplicitLocOpBuilder &builder) {
  auto firType = type.cast<FIRRTLType>();

  // If this was an Analog type, it will be converted to an InOut type.
  if (type.isa<AnalogType>())
    return builder.createOrFold<AnalogInOutCastOp>(firType, val);

  if (BundleType bundle = type.dyn_cast<BundleType>()) {
    val = builder.createOrFold<HWStructCastOp>(firType.getPassiveType(), val);
  } else {
    val = builder.createOrFold<StdIntCastOp>(firType.getPassiveType(), val);
  }

  // Handle the flip type if needed.
  if (type != val.getType())
    val = builder.createOrFold<AsNonPassivePrimOp>(firType, val);
  return val;
}

/// Cast from a FIRRTL type (potentially with a flip) to a standard type.
static Value castFromFIRRTLType(Value val, Type type,
                                ImplicitLocOpBuilder &builder) {
  if (type.isa<hw::InOutType>() && val.getType().isa<AnalogType>())
    return builder.createOrFold<AnalogInOutCastOp>(type, val);

  // Strip off Flip type if needed.
  val = builder.createOrFold<AsPassivePrimOp>(val);
  if (hw::StructType structTy = type.dyn_cast<hw::StructType>())
    return builder.createOrFold<HWStructCastOp>(type, val);
  return builder.createOrFold<StdIntCastOp>(type, val);
}

/// Return true if the specified FIRRTL type is a sized type (Int or Analog)
/// with zero bits.
static bool isZeroBitFIRRTLType(Type type) {
  return type.cast<FIRRTLType>().getPassiveType().getBitWidthOrSentinel() == 0;
}

namespace {
struct FirMemory {
  size_t numReadPorts;
  size_t numWritePorts;
  size_t numReadWritePorts;
  size_t dataWidth;
  size_t depth;
  size_t readLatency;
  size_t writeLatency;
  size_t readUnderWrite;
  bool operator<(const FirMemory &rhs) const {
#define cmp3way(name)                                                          \
  if (name < rhs.name)                                                         \
    return true;                                                               \
  if (name > rhs.name)                                                         \
    return false;
    cmp3way(numReadPorts);
    cmp3way(numWritePorts);
    cmp3way(numReadWritePorts);
    cmp3way(dataWidth);
    cmp3way(depth);
    cmp3way(readLatency);
    cmp3way(writeLatency);
    cmp3way(readUnderWrite);
    return false;
#undef cmp3way
  }
  bool operator==(const FirMemory &rhs) const {
    return numReadPorts == rhs.numReadPorts &&
           numWritePorts == rhs.numWritePorts &&
           numReadWritePorts == rhs.numReadWritePorts &&
           dataWidth == rhs.dataWidth && depth == rhs.depth &&
           readLatency == rhs.readLatency && writeLatency == rhs.writeLatency &&
           readUnderWrite == rhs.readUnderWrite;
  }
};
} // namespace

static std::string getFirMemoryName(const FirMemory &mem) {
  return llvm::formatv("FIRRTLMem_{0}_{1}_{2}_{3}_{4}_{5}_{6}_{7}",
                       mem.numReadPorts, mem.numWritePorts,
                       mem.numReadWritePorts, mem.dataWidth, mem.depth,
                       mem.readLatency, mem.writeLatency, mem.readUnderWrite);
}

static FirMemory analyzeMemOp(MemOp op) {
  size_t numReadPorts = 0;
  size_t numWritePorts = 0;
  size_t numReadWritePorts = 0;

  for (size_t i = 0, e = op.getNumResults(); i != e; ++i) {
    auto portKind = op.getPortKind(i);
    if (portKind == MemOp::PortKind::Read)
      ++numReadPorts;
    else if (portKind == MemOp::PortKind::Write)
      ++numWritePorts;
    else
      ++numReadWritePorts;
  }
  auto width = op.getDataType().getBitWidthOrSentinel();
  if (width <= 0) {
    op.emitError("'firrtl.mem' should have simple type and known width");
    width = 0;
  }

  return {numReadPorts, numWritePorts,    numReadWritePorts, (size_t)width,
          op.depth(),   op.readLatency(), op.writeLatency(), (size_t)op.ruw()};
}

static SmallVector<FirMemory> collectFIRRTLMemories(Operation *op) {
  auto module = cast<FModuleOp>(op);
  SmallVector<FirMemory> retval;
  for (auto op : module.getBody().getOps<MemOp>())
    retval.push_back(analyzeMemOp(op));
  return retval;
}

static SmallVector<FirMemory>
mergeFIRRTLMemories(const SmallVector<FirMemory> &lhs,
                    SmallVector<FirMemory> rhs) {
  if (rhs.empty())
    return lhs;
  // lhs is always sorted and uniqued
  llvm::sort(rhs);
  rhs.erase(std::unique(rhs.begin(), rhs.end()), rhs.end());
  SmallVector<FirMemory> retval;
  std::set_union(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
                 std::back_inserter(retval));
  return retval;
}

//===----------------------------------------------------------------------===//
// firrtl.module Lowering Pass
//===----------------------------------------------------------------------===//
namespace {

struct FIRRTLModuleLowering;

/// This is state shared across the parallel module lowering logic.
struct CircuitLoweringState {
  std::atomic<bool> used_PRINTF_COND{false};
  std::atomic<bool> used_STOP_COND{false};

  std::atomic<bool> used_RANDOMIZE_REG_INIT{false},
      used_RANDOMIZE_MEM_INIT{false};
  std::atomic<bool> used_RANDOMIZE_GARBAGE_ASSIGN{false};

  CircuitLoweringState(CircuitOp circuitOp, bool warn)
      : circuitOp(circuitOp), enableAnnotationWarning(warn) {}

  Operation *getNewModule(Operation *oldModule) {
    auto it = oldToNewModuleMap.find(oldModule);
    return it != oldToNewModuleMap.end() ? it->second : nullptr;
  }

  // Emit warnings on unprocessed annotations still remaining in the annoSet.
  void warnOnRemainingAnnotations(Operation *op, const AnnotationSet &annoSet);

  CircuitOp circuitOp;

  // Safely add a BindOp to global mutable state.  This will acquire a lock to
  // do this safely.
  void addBind(sv::BindOp op) {
    std::lock_guard<std::mutex> lock(bindsMutex);
    binds.push_back(op);
  }

private:
  friend struct FIRRTLModuleLowering;
  CircuitLoweringState(const CircuitLoweringState &) = delete;
  void operator=(const CircuitLoweringState &) = delete;

  DenseMap<Operation *, Operation *> oldToNewModuleMap;

  /// These are ops that are just copied as they go through.  This is intended
  /// to be used for ops that are from other dialects and are expected.
  SmallVector<Operation *> passThroughOps;

  // Record the set of remaining annotation classes. This is used to warn only
  // once about any annotation class.
  StringSet<> alreadyPrinted;
  const bool enableAnnotationWarning;
  std::mutex annotationPrintingMtx;

  // Records any sv::BindOps that are found during the course of execution.
  // This is unsafe to access directly and should only be used through addBind.
  SmallVector<sv::BindOp> binds;

  // Control access to binds.
  std::mutex bindsMutex;
};

void CircuitLoweringState::warnOnRemainingAnnotations(
    Operation *op, const AnnotationSet &annoSet) {
  if (!enableAnnotationWarning || annoSet.empty())
    return;
  std::lock_guard<std::mutex> lock(annotationPrintingMtx);

  for (auto a : annoSet) {
    auto inserted = alreadyPrinted.insert(a.getClass());
    if (inserted.second)
      mlir::emitWarning(op->getLoc(), "unprocessed annotation:'" +
                                          a.getClass() +
                                          "' still remaining after LowerToHW");
  }
}
} // end anonymous namespace

namespace {
struct FIRRTLModuleLowering : public LowerFIRRTLToHWBase<FIRRTLModuleLowering> {

  void runOnOperation() override;
  void setEnableAnnotationWarning() { enableAnnotationWarning = true; }

private:
  void lowerFileHeader(CircuitOp op, CircuitLoweringState &loweringState);
  LogicalResult lowerPorts(ArrayRef<ModulePortInfo> firrtlPorts,
                           SmallVectorImpl<hw::ModulePortInfo> &ports,
                           Operation *moduleOp,
                           CircuitLoweringState &loweringState);
  hw::HWModuleOp lowerModule(FModuleOp oldModule, Block *topLevelModule,
                             CircuitLoweringState &loweringState);
  hw::HWModuleExternOp lowerExtModule(FExtModuleOp oldModule,
                                      Block *topLevelModule,
                                      CircuitLoweringState &loweringState);

  void lowerModuleBody(FModuleOp oldModule,
                       CircuitLoweringState &loweringState);
  void lowerModuleOperations(hw::HWModuleOp module,
                             CircuitLoweringState &loweringState);

  void lowerMemoryDecls(const SmallVector<FirMemory> &mems, Block *top,
                        CircuitLoweringState &loweringState);
};

} // end anonymous namespace

/// This is the pass constructor.
std::unique_ptr<mlir::Pass>
circt::createLowerFIRRTLToHWPass(llvm::Optional<bool> enableAnnotationWarning) {
  auto pass = std::make_unique<FIRRTLModuleLowering>();
  if (enableAnnotationWarning.hasValue() && enableAnnotationWarning.getValue())
    pass->setEnableAnnotationWarning();
  return pass;
}

/// Run on the firrtl.circuit operation, lowering any firrtl.module operations
/// it contains.
void FIRRTLModuleLowering::runOnOperation() {
  // We run on the top level modules in the IR blob.  Start by finding the
  // firrtl.circuit within it.  If there is none, then there is nothing to do.
  auto *topLevelModule = getOperation().getBody();

  // Find the single firrtl.circuit in the module.
  CircuitOp circuit;
  for (auto &op : *topLevelModule) {
    if ((circuit = dyn_cast<CircuitOp>(&op)))
      break;
  }

  if (!circuit)
    return;

  auto *circuitBody = circuit.getBody();

  // Keep track of the mapping from old to new modules.  The result may be null
  // if lowering failed.
  CircuitLoweringState state(circuit, enableAnnotationWarning);

  SmallVector<FModuleOp, 32> modulesToProcess;

  state.warnOnRemainingAnnotations(circuit, AnnotationSet(circuit));
  // Iterate through each operation in the circuit body, transforming any
  // FModule's we come across.
  for (auto &op : circuitBody->getOperations()) {
    if (auto module = dyn_cast<FModuleOp>(op)) {
      state.oldToNewModuleMap[&op] = lowerModule(module, topLevelModule, state);
      modulesToProcess.push_back(module);
      continue;
    }

    if (auto extModule = dyn_cast<FExtModuleOp>(op)) {
      state.oldToNewModuleMap[&op] =
          lowerExtModule(extModule, topLevelModule, state);
      continue;
    }

    // Anything which is _not_ a module or an extmoulde is treated carefully.
    // By default, this produces an error.
    TypeSwitch<Operation *>(&op)
        // GrandCentral can generate interfaces.  These need to be let through.
        .Case<sv::InterfaceOp>(
            [&](auto op) { state.passThroughOps.push_back(op); })
        .Default([](auto op) {
          // Otherwise we don't know what this is.  We are just going to drop
          // it, but emit an error so the client has some chance to know that
          // this is going to happen.
          op->emitError("unexpected operation '")
              << op->getName() << "' in a firrtl.circuit";
        });
  }

  SmallVector<FirMemory> memories;
  if (getContext().isMultithreadingEnabled()) {
    // TODO: Update this to use a mlir::parallelTransformReduce once it exists.
    memories = llvm::parallelTransformReduce(
        modulesToProcess.begin(), modulesToProcess.end(),
        SmallVector<FirMemory>(), mergeFIRRTLMemories, collectFIRRTLMemories);
  } else {
    for (auto m : modulesToProcess)
      memories = mergeFIRRTLMemories(memories, collectFIRRTLMemories(m));
  }
  if (!memories.empty())
    lowerMemoryDecls(memories, topLevelModule, state);

  // Now that we've lowered all of the modules, move the bodies over and update
  // any instances that refer to the old modules.
  mlir::parallelForEachN(
      &getContext(), 0, modulesToProcess.size(),
      [&](auto index) { lowerModuleBody(modulesToProcess[index], state); });

  // Move binds from inside modules to outside modules.
  for (auto bind : state.binds) {
    bind->moveBefore(bind->getParentOfType<hw::HWModuleOp>());
  }

  // Move any pass through ops into the top of the new module.
  for (auto *passThrough : state.passThroughOps)
    passThrough->moveAfter(topLevelModule, topLevelModule->begin());

  // Finally delete all the old modules.
  for (auto oldNew : state.oldToNewModuleMap)
    oldNew.first->erase();

  // Now that the modules are moved over, remove the Circuit.  We pop the 'main
  // module' specified in the Circuit into an attribute on the top level module.
  getOperation()->setAttr(
      "firrtl.mainModule",
      StringAttr::get(circuit.getContext(), circuit.name()));

  // Emit all the macros and preprocessor gunk at the start of the file.
  lowerFileHeader(circuit, state);

  circuit.erase();
}

void FIRRTLModuleLowering::lowerMemoryDecls(const SmallVector<FirMemory> &mems,
                                            Block *top,
                                            CircuitLoweringState &state) {
  auto b =
      ImplicitLocOpBuilder::atBlockBegin(UnknownLoc::get(&getContext()), top);
  std::array<StringRef, 8> schemaFields = {
      "depth",       "numReadPorts", "numWritePorts", "numReadWritePorts",
      "readLatency", "writeLatency", "width",         "readUnderWrite"};
  auto schemaFieldsAttr = b.getStrArrayAttr(schemaFields);
  auto schema = b.create<hw::HWGeneratorSchemaOp>("FIRRTLMem", "FIRRTL_Memory",
                                                  schemaFieldsAttr);
  auto memorySchema = b.getSymbolRefAttr(schema);

  Type b1Type = IntegerType::get(&getContext(), 1);

  for (auto &mem : mems) {
    SmallVector<hw::ModulePortInfo> ports;
    size_t inputPin = 0;
    size_t outputPin = 0;

    auto makePortCommon = [&](StringRef prefix, size_t idx, Type bAddrType) {
      ports.push_back({b.getStringAttr(prefix + "_clock_" + Twine(idx)),
                       hw::INPUT, b1Type, inputPin++});
      ports.push_back({b.getStringAttr(prefix + "_en_" + Twine(idx)), hw::INPUT,
                       b1Type, inputPin++});
      ports.push_back({b.getStringAttr(prefix + "_addr_" + Twine(idx)),
                       hw::INPUT, bAddrType, inputPin++});
    };

    Type bDataType =
        IntegerType::get(&getContext(), std::max((size_t)1, mem.dataWidth));

    Type bAddrType = IntegerType::get(
        &getContext(), std::max(1U, llvm::Log2_64_Ceil(mem.depth)));

    for (size_t i = 0; i < mem.numReadPorts; ++i) {
      makePortCommon("ro", i, bAddrType);
      ports.push_back({b.getStringAttr(("ro_data_" + Twine(i)).str()),
                       hw::OUTPUT, bDataType, outputPin++});
    }
    for (size_t i = 0; i < mem.numReadWritePorts; ++i) {
      makePortCommon("rw", i, bAddrType);
      ports.push_back({b.getStringAttr("rw_wmode_" + Twine(i)), hw::INPUT,
                       b1Type, inputPin++});
      ports.push_back({b.getStringAttr("rw_wmask_" + Twine(i)), hw::INPUT,
                       b1Type, inputPin++});
      ports.push_back({b.getStringAttr("rw_wdata_" + Twine(i)), hw::INPUT,
                       bDataType, inputPin++});
      ports.push_back({b.getStringAttr("rw_rdata_" + Twine(i)), hw::OUTPUT,
                       bDataType, outputPin++});
    }

    for (size_t i = 0; i < mem.numWritePorts; ++i) {
      makePortCommon("wo", i, bAddrType);
      ports.push_back({b.getStringAttr("wo_mask_" + Twine(i)), hw::INPUT,
                       b1Type, inputPin++});
      ports.push_back({b.getStringAttr("wo_data_" + Twine(i)), hw::INPUT,
                       bDataType, inputPin++});
    }
    std::array<NamedAttribute, 8> genAttrs = {
        b.getNamedAttr("depth", b.getI64IntegerAttr(mem.depth)),
        b.getNamedAttr("numReadPorts", b.getUI32IntegerAttr(mem.numReadPorts)),
        b.getNamedAttr("numWritePorts",
                       b.getUI32IntegerAttr(mem.numWritePorts)),
        b.getNamedAttr("numReadWritePorts",
                       b.getUI32IntegerAttr(mem.numReadWritePorts)),
        b.getNamedAttr("readLatency", b.getUI32IntegerAttr(mem.readLatency)),
        b.getNamedAttr("writeLatency", b.getUI32IntegerAttr(mem.writeLatency)),
        b.getNamedAttr("width", b.getUI32IntegerAttr(mem.dataWidth)),
        b.getNamedAttr("readUnderWrite",
                       b.getUI32IntegerAttr(mem.readUnderWrite))};

    // Make the global module for the memory
    auto memoryName = b.getStringAttr(getFirMemoryName(mem));
    b.create<hw::HWModuleGeneratedOp>(memorySchema, memoryName, ports,
                                      StringRef(), genAttrs);
  }
}

/// Emit the file header that defines a bunch of macros.
void FIRRTLModuleLowering::lowerFileHeader(CircuitOp op,
                                           CircuitLoweringState &state) {
  // Intentionally pass an UnknownLoc here so we don't get line number comments
  // on the output of this boilerplate in generated Verilog.
  ImplicitLocOpBuilder b(UnknownLoc::get(&getContext()), op);

  // TODO: We could have an operation for macros and uses of them, and
  // even turn them into symbols so we can DCE unused macro definitions.
  auto emitString = [&](StringRef verilogString) {
    b.create<sv::VerbatimOp>(verilogString);
  };

  // Helper function to emit a "#ifdef guard" with a `define in the then and
  // optionally in the else branch.
  auto emitGuardedDefine = [&](const char *guard, const char *defineTrue,
                               const char *defineFalse = nullptr) {
    std::string define = "`define ";
    if (!defineFalse) {
      assert(defineTrue && "didn't define anything");
      b.create<sv::IfDefOp>(guard, [&]() { emitString(define + defineTrue); });
    } else {
      b.create<sv::IfDefOp>(
          guard,
          [&]() {
            if (defineTrue)
              emitString(define + defineTrue);
          },
          [&]() { emitString(define + defineFalse); });
    }
  };

  // If none of the macros are needed, then don't emit any header at all, not
  // even the header comment.
  if (!state.used_RANDOMIZE_GARBAGE_ASSIGN && !state.used_RANDOMIZE_REG_INIT &&
      !state.used_RANDOMIZE_MEM_INIT && !state.used_PRINTF_COND &&
      !state.used_STOP_COND)
    return;

  emitString("// Standard header to adapt well known macros to our needs.");

  bool needRandom = false;
  if (state.used_RANDOMIZE_GARBAGE_ASSIGN) {
    emitGuardedDefine("RANDOMIZE_GARBAGE_ASSIGN", "RANDOMIZE");
    needRandom = true;
  }
  if (state.used_RANDOMIZE_REG_INIT) {
    emitGuardedDefine("RANDOMIZE_REG_INIT", "RANDOMIZE");
    needRandom = true;
  }
  if (state.used_RANDOMIZE_MEM_INIT) {
    emitGuardedDefine("RANDOMIZE_MEM_INIT", "RANDOMIZE");
    needRandom = true;
  }

  if (needRandom) {
    emitString("\n// RANDOM may be set to an expression that produces a 32-bit "
               "random unsigned value.");
    emitGuardedDefine("RANDOM", nullptr, "RANDOM {$random}");
  }

  if (state.used_PRINTF_COND) {
    emitString(
        "\n// Users can define 'PRINTF_COND' to add an extra gate to prints.");
    emitGuardedDefine("PRINTF_COND", "PRINTF_COND_ (`PRINTF_COND)",
                      "PRINTF_COND_ 1");
  }

  if (state.used_STOP_COND) {
    emitString("\n// Users can define 'STOP_COND' to add an extra gate "
               "to stop conditions.");
    emitGuardedDefine("STOP_COND", "STOP_COND_ (`STOP_COND)", "STOP_COND_ 1");
  }

  if (needRandom) {
    emitString(
        "\n// Users can define INIT_RANDOM as general code that gets injected "
        "into the\n// initializer block for modules with registers.");
    emitGuardedDefine("INIT_RANDOM", nullptr, "INIT_RANDOM");

    emitString(
        "\n// If using random initialization, you can also define "
        "RANDOMIZE_DELAY to\n// customize the delay used, otherwise 0.002 "
        "is used.");
    emitGuardedDefine("RANDOMIZE_DELAY", nullptr, "RANDOMIZE_DELAY 0.002");

    emitString("\n// Define INIT_RANDOM_PROLOG_ for use in our modules below.");
    b.create<sv::IfDefOp>(
        "RANDOMIZE",
        [&]() {
          emitGuardedDefine(
              "VERILATOR", "INIT_RANDOM_PROLOG_ `INIT_RANDOM",
              "INIT_RANDOM_PROLOG_ `INIT_RANDOM #`RANDOMIZE_DELAY begin end");
        },
        [&]() { emitString("`define INIT_RANDOM_PROLOG_"); });
  }

  if (state.used_RANDOMIZE_GARBAGE_ASSIGN) {
    emitString("\n// RANDOMIZE_GARBAGE_ASSIGN enable range checks for mem "
               "assignments.");
    b.create<sv::IfDefOp>(
        "RANDOMIZE_GARBAGE_ASSIGN",
        [&]() {
          emitString(
              "`define RANDOMIZE_GARBAGE_ASSIGN_BOUND_CHECK(INDEX, VALUE, "
              "SIZE) \\");
          emitString("  ((INDEX) < (SIZE) ? (VALUE) : `RANDOM)");
        },
        [&]() {
          emitString("`define RANDOMIZE_GARBAGE_ASSIGN_BOUND_CHECK(INDEX, "
                     "VALUE, SIZE) (VALUE)");
        });
  }

  // Blank line to separate the header from the modules.
  emitString("");
}

LogicalResult
FIRRTLModuleLowering::lowerPorts(ArrayRef<ModulePortInfo> firrtlPorts,
                                 SmallVectorImpl<hw::ModulePortInfo> &ports,
                                 Operation *moduleOp,
                                 CircuitLoweringState &loweringState) {
  ports.reserve(firrtlPorts.size());
  size_t numArgs = 0;
  size_t numResults = 0;
  for (auto firrtlPort : firrtlPorts) {
    hw::ModulePortInfo hwPort;
    hwPort.name = firrtlPort.name;
    hwPort.type = lowerType(firrtlPort.type);

    // We can't lower all types, so make sure to cleanly reject them.
    if (!hwPort.type) {
      moduleOp->emitError("cannot lower this port type to HW");
      return failure();
    }

    // If this is a zero bit port, just drop it.  It doesn't matter if it is
    // input, output, or inout.  We don't want these at the HW level.
    if (hwPort.type.isInteger(0))
      continue;

    // Figure out the direction of the port.
    if (firrtlPort.isOutput()) {
      hwPort.direction = hw::PortDirection::OUTPUT;
      hwPort.argNum = numResults++;
    } else if (firrtlPort.isInput()) {
      hwPort.direction = hw::PortDirection::INPUT;
      hwPort.argNum = numArgs++;
    } else {
      // If the port is an inout bundle or contains an analog type, then it is
      // implicitly inout.
      hwPort.type = hw::InOutType::get(hwPort.type);
      hwPort.direction = hw::PortDirection::INOUT;
      hwPort.argNum = numArgs++;
    }
    ports.push_back(hwPort);
    loweringState.warnOnRemainingAnnotations(moduleOp, firrtlPort.annotations);
  }
  return success();
}

hw::HWModuleExternOp
FIRRTLModuleLowering::lowerExtModule(FExtModuleOp oldModule,
                                     Block *topLevelModule,
                                     CircuitLoweringState &loweringState) {
  // Map the ports over, lowering their types as we go.
  SmallVector<ModulePortInfo> firrtlPorts = oldModule.getPorts();
  SmallVector<hw::ModulePortInfo, 8> ports;
  if (failed(lowerPorts(firrtlPorts, ports, oldModule, loweringState)))
    return {};

  StringRef verilogName;
  if (auto defName = oldModule.defname())
    verilogName = defName.getValue();

  loweringState.warnOnRemainingAnnotations(oldModule, AnnotationSet(oldModule));
  // Build the new hw.module op.
  OpBuilder builder(topLevelModule->getParent()->getContext());
  builder.setInsertionPointToEnd(topLevelModule);
  auto nameAttr = builder.getStringAttr(oldModule.getName());
  return builder.create<hw::HWModuleExternOp>(oldModule.getLoc(), nameAttr,
                                              ports, verilogName);
}

/// Run on each firrtl.module, transforming it from an firrtl.module into an
/// hw.module, then deleting the old one.
hw::HWModuleOp
FIRRTLModuleLowering::lowerModule(FModuleOp oldModule, Block *topLevelModule,
                                  CircuitLoweringState &loweringState) {
  // Map the ports over, lowering their types as we go.
  SmallVector<ModulePortInfo> firrtlPorts = oldModule.getPorts();
  SmallVector<hw::ModulePortInfo, 8> ports;
  if (failed(lowerPorts(firrtlPorts, ports, oldModule, loweringState)))
    return {};

  loweringState.warnOnRemainingAnnotations(oldModule, AnnotationSet(oldModule));
  // Build the new hw.module op.
  OpBuilder builder(topLevelModule->getParent()->getContext());
  builder.setInsertionPointToEnd(topLevelModule);
  auto nameAttr = builder.getStringAttr(oldModule.getName());
  return builder.create<hw::HWModuleOp>(oldModule.getLoc(), nameAttr, ports);
}

/// Given a value of analog type, check to see the only use of it is an attach.
/// If so, remove the attach and return the value being attached to it,
/// converted to an HW inout type.  If this isn't a situation we can handle,
/// just return null.
static Value tryEliminatingAttachesToAnalogValue(Value value,
                                                 Operation *insertPoint) {
  if (!value.hasOneUse())
    return {};

  auto attach = dyn_cast<AttachOp>(*value.user_begin());
  if (!attach || attach.getNumOperands() != 2)
    return {};

  // Don't optimize zero bit analogs.
  auto loweredType = lowerType(value.getType());
  if (loweredType.isInteger(0))
    return {};

  // Check to see if the attached value dominates the insertion point.  If
  // not, just fail.
  auto attachedValue = attach.getOperand(attach.getOperand(0) == value);
  auto *op = attachedValue.getDefiningOp();
  if (op && op->getBlock() == insertPoint->getBlock() &&
      !op->isBeforeInBlock(insertPoint))
    return {};

  attach.erase();

  ImplicitLocOpBuilder builder(insertPoint->getLoc(), insertPoint);
  return castFromFIRRTLType(attachedValue, hw::InOutType::get(loweredType),
                            builder);
}

/// Given a value of flip type, check to see if all of the uses of it are
/// connects.  If so, remove the connects and return the value being connected
/// to it, converted to an HW type.  If this isn't a situation we can handle,
/// just return null.
///
/// This can happen when there are no connects to the value.  The 'mergePoint'
/// location is where a 'hw.merge' operation should be inserted if needed.
static Value tryEliminatingConnectsToValue(Value flipValue,
                                           Operation *insertPoint) {
  // Handle analog's separately.
  if (flipValue.getType().isa<AnalogType>())
    return tryEliminatingAttachesToAnalogValue(flipValue, insertPoint);

  SmallVector<ConnectOp, 2> connects;
  for (auto *use : flipValue.getUsers()) {
    // We only know about 'connect' uses, where this is the destination.
    auto connect = dyn_cast<ConnectOp>(use);
    if (!connect || connect.src() == flipValue)
      return {};

    connects.push_back(connect);
  }

  // We don't have an HW equivalent of "poison" so just don't special case
  // the case where there are no connects other uses of an output.
  if (connects.empty())
    return {}; // TODO: Emit an sv.constantz here since it is unconnected.

  // Don't special case zero-bit results.
  auto loweredType = lowerType(flipValue.getType());
  if (loweredType.isInteger(0))
    return {};

  // Convert each connect into an extended version of its operand being
  // output.
  SmallVector<Value, 2> results;
  ImplicitLocOpBuilder builder(insertPoint->getLoc(), insertPoint);

  for (auto connect : connects) {
    auto connectSrc = connect.src();

    // Convert fliped sources to passive sources.
    if (!connectSrc.getType().cast<FIRRTLType>().isPassive())
      connectSrc = builder.createOrFold<AsPassivePrimOp>(connectSrc);

    // We know it must be the destination operand due to the types, but the
    // source may not match the destination width.
    auto destTy = flipValue.getType().cast<FIRRTLType>().getPassiveType();
    if (destTy.getBitWidthOrSentinel() !=
        connectSrc.getType().cast<FIRRTLType>().getBitWidthOrSentinel()) {
      // The only type mismatchs we care about is due to integer width
      // differences.
      auto destWidth = destTy.getBitWidthOrSentinel();
      assert(destWidth != -1 && "must know integer widths");
      connectSrc =
          builder.createOrFold<PadPrimOp>(destTy, connectSrc, destWidth);
    }

    // Remove the connect and use its source as the value for the output.
    connect.erase();
    results.push_back(connectSrc);
  }

  // Convert from FIRRTL type to builtin type to do the merge.
  for (auto &result : results)
    result = castFromFIRRTLType(result, loweredType, builder);

  // Folding merge of one value just returns the value.
  return builder.createOrFold<comb::MergeOp>(results);
}

static SmallVector<SubfieldOp> getAllFieldAccesses(Value structValue,
                                                   StringRef field) {
  SmallVector<SubfieldOp> accesses;
  for (auto op : structValue.getUsers()) {
    assert(isa<SubfieldOp>(op));
    auto fieldAccess = cast<SubfieldOp>(op);
    auto elemIndex =
        fieldAccess.input().getType().cast<BundleType>().getElementIndex(field);
    if (elemIndex.hasValue() &&
        fieldAccess.fieldIndex() == elemIndex.getValue()) {
      accesses.push_back(fieldAccess);
    }
  }
  return accesses;
}

/// Now that we have the operations for the hw.module's corresponding to the
/// firrtl.module's, we can go through and move the bodies over, updating the
/// ports and instances.
void FIRRTLModuleLowering::lowerModuleBody(
    FModuleOp oldModule, CircuitLoweringState &loweringState) {
  auto newModule =
      dyn_cast_or_null<hw::HWModuleOp>(loweringState.getNewModule(oldModule));
  // Don't touch modules if we failed to lower ports.
  if (!newModule)
    return;

  ImplicitLocOpBuilder bodyBuilder(oldModule.getLoc(), newModule.body());

  // Use a placeholder instruction be a cursor that indicates where we want to
  // move the new function body to.  This is important because we insert some
  // ops at the start of the function and some at the end, and the body is
  // currently empty to avoid iterator invalidation.
  auto cursor = bodyBuilder.create<hw::ConstantOp>(APInt(1, 1));
  bodyBuilder.setInsertionPoint(cursor);

  // Insert argument casts, and re-vector users in the old body to use them.
  SmallVector<ModulePortInfo> ports = oldModule.getPorts();
  assert(oldModule.body().getNumArguments() == ports.size() &&
         "port count mismatch");

  size_t nextNewArg = 0;
  size_t firrtlArg = 0;
  SmallVector<Value, 4> outputs;

  // This is the terminator in the new module.
  auto outputOp = newModule.getBodyBlock()->getTerminator();
  ImplicitLocOpBuilder outputBuilder(oldModule.getLoc(), outputOp);

  for (auto &port : ports) {
    // Inputs and outputs are both modeled as arguments in the FIRRTL level.
    auto oldArg = oldModule.body().getArgument(firrtlArg++);

    bool isZeroWidth =
        port.type.cast<FIRRTLType>().getBitWidthOrSentinel() == 0;

    if (!port.isOutput() && !isZeroWidth) {
      // Inputs and InOuts are modeled as arguments in the result, so we can
      // just map them over.  We model zero bit outputs as inouts.
      Value newArg = newModule.body().getArgument(nextNewArg++);

      // Cast the argument to the old type, reintroducing sign information in
      // the hw.module body.
      newArg = castToFIRRTLType(newArg, oldArg.getType(), bodyBuilder);
      // Switch all uses of the old operands to the new ones.
      oldArg.replaceAllUsesWith(newArg);
      continue;
    }

    // We lower zero width inout and outputs to a wire that isn't connected to
    // anything outside the module.  Inputs are lowered to zero.
    if (isZeroWidth && port.isInput()) {
      Value newArg = bodyBuilder.create<WireOp>(
          port.type, "." + port.getName().str() + ".0width_input");
      oldArg.replaceAllUsesWith(newArg);
      continue;
    }

    if (auto value = tryEliminatingConnectsToValue(oldArg, outputOp)) {
      // If we were able to find the value being connected to the output,
      // directly use it!
      outputs.push_back(value);
      assert(oldArg.use_empty() && "should have removed all uses of oldArg");
      continue;
    }

    // Outputs need a temporary wire so they can be connect'd to, which we
    // then return.
    Value newArg = bodyBuilder.create<WireOp>(
        port.type, "." + port.getName().str() + ".output");
    // Switch all uses of the old operands to the new ones.
    oldArg.replaceAllUsesWith(newArg);

    // Don't output zero bit results or inouts.
    auto resultHWType = lowerType(port.type);
    if (!resultHWType.isInteger(0)) {
      auto output = castFromFIRRTLType(newArg, resultHWType, outputBuilder);
      outputs.push_back(output);
    }
  }

  // Update the hw.output terminator with the list of outputs we have.
  outputOp->setOperands(outputs);

  // Finally splice the body over, don't move the old terminator over though.
  auto &oldBlockInstList = oldModule.getBodyBlock()->getOperations();
  auto &newBlockInstList = newModule.getBodyBlock()->getOperations();
  newBlockInstList.splice(Block::iterator(cursor), oldBlockInstList,
                          oldBlockInstList.begin(), oldBlockInstList.end());

  // We are done with our cursor op.
  cursor.erase();

  // Lower all of the other operations.
  lowerModuleOperations(newModule, loweringState);
}

//===----------------------------------------------------------------------===//
// Module Body Lowering Pass
//===----------------------------------------------------------------------===//

namespace {
struct FIRRTLLowering : public FIRRTLVisitor<FIRRTLLowering, LogicalResult> {

  FIRRTLLowering(hw::HWModuleOp module, CircuitLoweringState &circuitState)
      : theModule(module), circuitState(circuitState),
        builder(module.getLoc(), module.getContext()) {}

  void run();

  void optimizeTemporaryWire(sv::WireOp wire);

  // Helpers.
  Value getOrCreateIntConstant(const APInt &value);
  Value getOrCreateIntConstant(unsigned numBits, uint64_t val,
                               bool isSigned = false) {
    return getOrCreateIntConstant(APInt(numBits, val, isSigned));
  }
  Value getPossiblyInoutLoweredValue(Value value);
  Value getLoweredValue(Value value);
  Value getLoweredAndExtendedValue(Value value, Type destType);
  Value getLoweredAndExtOrTruncValue(Value value, Type destType);
  LogicalResult setLowering(Value orig, Value result);
  LogicalResult setPossiblyFoldedLowering(Value orig, Value result);
  template <typename ResultOpType, typename... CtorArgTypes>
  LogicalResult setLoweringTo(Operation *orig, CtorArgTypes... args);
  void emitRandomizePrologIfNeeded();
  void initializeRegister(Value reg, Value resetSignal);

  void runWithInsertionPointAtEndOfBlock(std::function<void(void)> fn,
                                         Region &region);
  void addToAlwaysBlock(Value clock, std::function<void(void)> fn);
  void addToAlwaysFFBlock(sv::EventControl clockEdge, Value clock,
                          ::ResetType resetStyle, sv::EventControl resetEdge,
                          Value reset, std::function<void(void)> body = {},
                          std::function<void(void)> resetBody = {});
  void addToAlwaysFFBlock(sv::EventControl clockEdge, Value clock,
                          std::function<void(void)> body = {}) {
    addToAlwaysFFBlock(clockEdge, clock, ::ResetType(), sv::EventControl(),
                       Value(), body, std::function<void(void)>());
  }
  void addToIfDefBlock(StringRef cond, std::function<void(void)> thenCtor,
                       std::function<void(void)> elseCtor = {});
  void addToInitialBlock(std::function<void(void)> body);
  void addToIfDefProceduralBlock(StringRef cond,
                                 std::function<void(void)> thenCtor,
                                 std::function<void(void)> elseCtor = {});
  void addIfProceduralBlock(Value cond, std::function<void(void)> thenCtor,
                            std::function<void(void)> elseCtor = {});

  // Create a temporary wire at the current insertion point, and try to
  // eliminate it later as part of lowering post processing.
  sv::WireOp createTmpWireOp(Type type, StringRef name) {
    // This is a locally visible, private wire created by the compiler, so do
    // not attach a symbol name.
    auto result = builder.create<sv::WireOp>(type, name);
    tmpWiresToOptimize.push_back(result);
    return result;
  }

  using FIRRTLVisitor<FIRRTLLowering, LogicalResult>::visitExpr;
  using FIRRTLVisitor<FIRRTLLowering, LogicalResult>::visitDecl;
  using FIRRTLVisitor<FIRRTLLowering, LogicalResult>::visitStmt;

  // Lowering hooks.
  enum UnloweredOpResult { AlreadyLowered, NowLowered, LoweringFailure };
  UnloweredOpResult handleUnloweredOp(Operation *op);
  LogicalResult visitExpr(ConstantOp op);
  LogicalResult visitExpr(SpecialConstantOp op);
  LogicalResult visitExpr(SubfieldOp op);
  LogicalResult visitUnhandledOp(Operation *op) { return failure(); }
  LogicalResult visitInvalidOp(Operation *op) { return failure(); }

  // Declarations.
  LogicalResult visitDecl(WireOp op);
  LogicalResult visitDecl(NodeOp op);
  LogicalResult visitDecl(RegOp op);
  LogicalResult visitDecl(RegResetOp op);
  LogicalResult visitDecl(MemOp op);
  LogicalResult visitDecl(InstanceOp op);

  // Unary Ops.
  LogicalResult lowerNoopCast(Operation *op);
  LogicalResult visitExpr(AsSIntPrimOp op) { return lowerNoopCast(op); }
  LogicalResult visitExpr(AsUIntPrimOp op) { return lowerNoopCast(op); }
  LogicalResult visitExpr(AsPassivePrimOp op) { return lowerNoopCast(op); }
  LogicalResult visitExpr(AsNonPassivePrimOp op) { return lowerNoopCast(op); }
  LogicalResult visitExpr(AsClockPrimOp op) { return lowerNoopCast(op); }
  LogicalResult visitExpr(AsAsyncResetPrimOp op) { return lowerNoopCast(op); }

  LogicalResult visitExpr(StdIntCastOp op);
  LogicalResult visitExpr(HWStructCastOp op);
  LogicalResult visitExpr(AnalogInOutCastOp op);
  LogicalResult visitExpr(CvtPrimOp op);
  LogicalResult visitExpr(NotPrimOp op);
  LogicalResult visitExpr(NegPrimOp op);
  LogicalResult visitExpr(PadPrimOp op);
  LogicalResult visitExpr(XorRPrimOp op);
  LogicalResult visitExpr(AndRPrimOp op);
  LogicalResult visitExpr(OrRPrimOp op);

  // Binary Ops.
  template <typename ResultUnsignedOpType,
            typename ResultSignedOpType = ResultUnsignedOpType>
  LogicalResult lowerBinOp(Operation *op);
  template <typename ResultOpType>
  LogicalResult lowerBinOpToVariadic(Operation *op);
  LogicalResult lowerCmpOp(Operation *op, ICmpPredicate signedOp,
                           ICmpPredicate unsignedOp);
  template <typename SignedOp, typename UnsignedOp>
  LogicalResult lowerDivLikeOp(Operation *op);
  template <typename AOpTy, typename BOpTy>
  LogicalResult lowerVerificationStatement(AOpTy op);

  LogicalResult visitExpr(CatPrimOp op);

  LogicalResult visitExpr(AndPrimOp op) {
    return lowerBinOpToVariadic<comb::AndOp>(op);
  }
  LogicalResult visitExpr(OrPrimOp op) {
    return lowerBinOpToVariadic<comb::OrOp>(op);
  }
  LogicalResult visitExpr(XorPrimOp op) {
    return lowerBinOpToVariadic<comb::XorOp>(op);
  }
  LogicalResult visitExpr(AddPrimOp op) {
    return lowerBinOpToVariadic<comb::AddOp>(op);
  }
  LogicalResult visitExpr(EQPrimOp op) {
    return lowerCmpOp(op, ICmpPredicate::eq, ICmpPredicate::eq);
  }
  LogicalResult visitExpr(NEQPrimOp op) {
    return lowerCmpOp(op, ICmpPredicate::ne, ICmpPredicate::ne);
  }
  LogicalResult visitExpr(LTPrimOp op) {
    return lowerCmpOp(op, ICmpPredicate::slt, ICmpPredicate::ult);
  }
  LogicalResult visitExpr(LEQPrimOp op) {
    return lowerCmpOp(op, ICmpPredicate::sle, ICmpPredicate::ule);
  }
  LogicalResult visitExpr(GTPrimOp op) {
    return lowerCmpOp(op, ICmpPredicate::sgt, ICmpPredicate::ugt);
  }
  LogicalResult visitExpr(GEQPrimOp op) {
    return lowerCmpOp(op, ICmpPredicate::sge, ICmpPredicate::uge);
  }

  LogicalResult visitExpr(SubPrimOp op) { return lowerBinOp<comb::SubOp>(op); }
  LogicalResult visitExpr(MulPrimOp op) {
    return lowerBinOpToVariadic<comb::MulOp>(op);
  }
  LogicalResult visitExpr(DivPrimOp op) {
    return lowerDivLikeOp<comb::DivSOp, comb::DivUOp>(op);
  }
  LogicalResult visitExpr(RemPrimOp op) {
    return lowerDivLikeOp<comb::ModSOp, comb::ModUOp>(op);
  }

  // Other Operations
  LogicalResult visitExpr(BitsPrimOp op);
  LogicalResult visitExpr(InvalidValueOp op);
  LogicalResult visitExpr(HeadPrimOp op);
  LogicalResult visitExpr(ShlPrimOp op);
  LogicalResult visitExpr(ShrPrimOp op);
  LogicalResult visitExpr(DShlPrimOp op) {
    return lowerDivLikeOp<comb::ShlOp, comb::ShlOp>(op);
  }
  LogicalResult visitExpr(DShrPrimOp op) {
    return lowerDivLikeOp<comb::ShrSOp, comb::ShrUOp>(op);
  }
  LogicalResult visitExpr(DShlwPrimOp op) {
    return lowerDivLikeOp<comb::ShlOp, comb::ShlOp>(op);
  }
  LogicalResult visitExpr(TailPrimOp op);
  LogicalResult visitExpr(MuxPrimOp op);
  LogicalResult visitExpr(VerbatimExprOp op);

  // Statements
  LogicalResult visitStmt(SkipOp op);
  LogicalResult visitStmt(ConnectOp op);
  LogicalResult visitStmt(PartialConnectOp op);
  LogicalResult visitStmt(PrintFOp op);
  LogicalResult visitStmt(StopOp op);
  LogicalResult visitStmt(AssertOp op);
  LogicalResult visitStmt(AssumeOp op);
  LogicalResult visitStmt(CoverOp op);
  LogicalResult visitStmt(AttachOp op);

private:
  /// The module we're lowering into.
  hw::HWModuleOp theModule;

  /// Global state.
  CircuitLoweringState &circuitState;

  /// This builder is set to the right location for each visit call.
  ImplicitLocOpBuilder builder;

  /// Each value lowered (e.g. operation result) is kept track in this map.
  /// The key should have a FIRRTL type, the result will have an HW dialect
  /// type.
  DenseMap<Value, Value> valueMapping;

  /// This keeps track of constants that we have created so we can reuse them.
  /// This is populated by the getOrCreateIntConstant method.
  DenseMap<Attribute, Value> hwConstantMap;

  // We auto-unique graph-level blocks to reduce the amount of generated
  // code and ensure that side effects are properly ordered in FIRRTL.
  llvm::SmallDenseMap<Value, sv::AlwaysOp> alwaysBlocks;

  using AlwaysFFKeyType = std::tuple<Block *, sv::EventControl, Value,
                                     ::ResetType, sv::EventControl, Value>;
  llvm::SmallDenseMap<AlwaysFFKeyType, sv::AlwaysFFOp> alwaysFFBlocks;
  llvm::SmallDenseMap<std::pair<Block *, Attribute>, sv::IfDefOp> ifdefBlocks;
  llvm::SmallDenseMap<Block *, sv::InitialOp> initialBlocks;

  /// This is a set of wires that get inserted as an artifact of the
  /// lowering process.  LowerToHW should attempt to clean these up after
  /// lowering.
  SmallVector<sv::WireOp> tmpWiresToOptimize;

  /// This is true if we've emitted `INIT_RANDOM_PROLOG_ into an initial
  /// block in this module already.
  bool randomizePrologEmitted;
};
} // end anonymous namespace

void FIRRTLModuleLowering::lowerModuleOperations(
    hw::HWModuleOp module, CircuitLoweringState &loweringState) {
  FIRRTLLowering(module, loweringState).run();
}

// This is the main entrypoint for the lowering pass.
void FIRRTLLowering::run() {
  // FIRRTL FModule is a single block because FIRRTL ops are a DAG.  Walk
  // through each operation, lowering each in turn if we can, introducing
  // casts if we cannot.
  auto *body = theModule.getBodyBlock();
  randomizePrologEmitted = false;

  SmallVector<Operation *, 16> opsToRemove;

  // Iterate through each operation in the module body, attempting to lower
  // each of them.  We maintain 'builder' for each invocation.
  for (auto &op : body->getOperations()) {
    builder.setInsertionPoint(&op);
    builder.setLoc(op.getLoc());
    auto done = succeeded(dispatchVisitor(&op));
    circuitState.warnOnRemainingAnnotations(&op, AnnotationSet(&op));
    if (done)
      opsToRemove.push_back(&op);
    else {
      switch (handleUnloweredOp(&op)) {
      case AlreadyLowered:
        break;         // Something like hw.output, which is already lowered.
      case NowLowered: // Something handleUnloweredOp removed.
        opsToRemove.push_back(&op);
        break;
      case LoweringFailure:
        // If lowering failed, don't remove *anything* we've lowered so far,
        // there may be uses, and the pass will fail anyway.
        opsToRemove.clear();
      }
    }
  }

  // Now that all of the operations that can be lowered are, remove the
  // original values.  We know that any lowered operations will be dead (if
  // removed in reverse order) at this point - any users of them from
  // unremapped operations will be changed to use the newly lowered ops.
  while (!opsToRemove.empty()) {
    assert(opsToRemove.back()->use_empty() &&
           "Should remove ops in reverse order of visitation");
    opsToRemove.pop_back_val()->erase();
  }

  // Now that the IR is in a stable form, try to eliminate temporary wires
  // inserted by MemOp insertions.
  for (auto wire : tmpWiresToOptimize)
    optimizeTemporaryWire(wire);
}

// Try to optimize out temporary wires introduced during lowering.
void FIRRTLLowering::optimizeTemporaryWire(sv::WireOp wire) {
  // Wires have inout type, so they'll have connects and read_inout operations
  // that work on them.  If anything unexpected is found then leave it alone.
  SmallVector<sv::ReadInOutOp> reads;
  sv::AssignOp write;

  for (auto *user : wire->getUsers()) {
    if (auto read = dyn_cast<sv::ReadInOutOp>(user)) {
      reads.push_back(read);
      continue;
    }

    // Otherwise must be a connect, and we must not have seen a write yet.
    auto assign = dyn_cast<sv::AssignOp>(user);
    if (!assign || write)
      return;
    write = assign;
  }

  // Must have found the write!
  if (!write)
    return;

  // If the write is happening at the module level then we don't have any
  // use-before-def checking to do, so we only handle that for now.
  if (!isa<hw::HWModuleOp>(write->getParentOp()))
    return;

  auto connected = write.src();

  // Ok, we can do this.  Replace all the reads with the connected value.
  for (auto read : reads) {
    read.replaceAllUsesWith(connected);
    read.erase();
  }
  // And remove the write and wire itself.
  write.erase();
  wire.erase();
}

//===----------------------------------------------------------------------===//
// Helpers
//===----------------------------------------------------------------------===//

/// Check to see if we've already lowered the specified constant.  If so, return
/// it.  Otherwise create it and put it in the entry block for reuse.
Value FIRRTLLowering::getOrCreateIntConstant(const APInt &value) {
  auto attr = builder.getIntegerAttr(
      builder.getIntegerType(value.getBitWidth()), value);

  auto &entry = hwConstantMap[attr];
  if (entry)
    return entry;

  OpBuilder entryBuilder(&theModule.getBodyBlock()->front());
  entry = entryBuilder.create<hw::ConstantOp>(builder.getLoc(), attr);
  return entry;
}

/// Zero bit operands end up looking like failures from getLoweredValue.  This
/// helper function invokes the closure specified if the operand was actually
/// zero bit, or returns failure() if it was some other kind of failure.
static LogicalResult handleZeroBit(Value failedOperand,
                                   std::function<LogicalResult()> fn) {
  assert(failedOperand && failedOperand.getType().isa<FIRRTLType>() &&
         "Should be called on the failed FIRRTL operand");
  if (!isZeroBitFIRRTLType(failedOperand.getType()))
    return failure();
  return fn();
}

/// Return the lowered HW value corresponding to the specified original value.
/// This returns a null value for FIRRTL values that haven't be lowered, e.g.
/// unknown width integers.  This returns hw::inout type values if present, it
/// does not implicitly read from them.
Value FIRRTLLowering::getPossiblyInoutLoweredValue(Value value) {
  assert(value.getType().isa<FIRRTLType>() &&
         "Should only lower FIRRTL operands");
  // If we lowered this value, then return the lowered value, otherwise fail.
  auto it = valueMapping.find(value);
  return it != valueMapping.end() ? it->second : Value();
}

/// Return the lowered value corresponding to the specified original value.
/// This returns a null value for FIRRTL values that cannot be lowered, e.g.
/// unknown width integers.
Value FIRRTLLowering::getLoweredValue(Value value) {
  auto result = getPossiblyInoutLoweredValue(value);
  if (!result)
    return result;

  // If we got an inout value, implicitly read it.  FIRRTL allows direct use
  // of wires and other things that lower to inout type.
  if (result.getType().isa<hw::InOutType>())
    return builder.createOrFold<sv::ReadInOutOp>(result);

  return result;
}

/// Return the lowered value corresponding to the specified original value and
/// then extend it to match the width of destType if needed.
///
/// This returns a null value for FIRRTL values that cannot be lowered, e.g.
/// unknown width integers.
Value FIRRTLLowering::getLoweredAndExtendedValue(Value value, Type destType) {
  assert(value.getType().isa<FIRRTLType>() && destType.isa<FIRRTLType>() &&
         "input/output value should be FIRRTL");

  // We only know how to extend integer types with known width.
  auto destWidth = destType.cast<FIRRTLType>().getBitWidthOrSentinel();
  if (destWidth == -1)
    return {};

  auto result = getLoweredValue(value);
  if (!result) {
    // If this was a zero bit operand being extended, then produce a zero of
    // the right result type.  If it is just a failure, fail.
    if (!isZeroBitFIRRTLType(value.getType()))
      return {};
    // Zero bit results have to be returned as null.  The caller can handle
    // this if they want to.
    if (destWidth == 0)
      return {};
    // Otherwise, FIRRTL semantics is that an extension from a zero bit value
    // always produces a zero value in the destination width.
    return getOrCreateIntConstant(destWidth, 0);
  }

  auto srcWidth = result.getType().cast<IntegerType>().getWidth();
  if (srcWidth == unsigned(destWidth))
    return result;

  if (srcWidth > unsigned(destWidth)) {
    builder.emitError("operand should not be a truncation");
    return {};
  }

  auto resultType = builder.getIntegerType(destWidth);

  // Extension follows the sign of the source value, not the destination.
  auto valueFIRType = value.getType().cast<FIRRTLType>().getPassiveType();
  if (valueFIRType.cast<IntType>().isSigned())
    return builder.createOrFold<comb::SExtOp>(resultType, result);

  auto zero = getOrCreateIntConstant(destWidth - srcWidth, 0);
  return builder.createOrFold<comb::ConcatOp>(zero, result);
}

/// Return the lowered value corresponding to the specified original value and
/// then extended or truncated to match the width of destType if needed.
///
/// This returns a null value for FIRRTL values that cannot be lowered, e.g.
/// unknown width integers.
Value FIRRTLLowering::getLoweredAndExtOrTruncValue(Value value, Type destType) {
  assert(value.getType().isa<FIRRTLType>() && destType.isa<FIRRTLType>() &&
         "input/output value should be FIRRTL");

  // We only know how to adjust integer types with known width.
  auto destWidth = destType.cast<FIRRTLType>().getBitWidthOrSentinel();
  if (destWidth == -1)
    return {};

  auto result = getLoweredValue(value);
  if (!result) {
    // If this was a zero bit operand being extended, then produce a zero of
    // the right result type.  If it is just a failure, fail.
    if (!isZeroBitFIRRTLType(value.getType()))
      return {};
    // Zero bit results have to be returned as null.  The caller can handle
    // this if they want to.
    if (destWidth == 0)
      return {};
    // Otherwise, FIRRTL semantics is that an extension from a zero bit value
    // always produces a zero value in the destination width.
    return getOrCreateIntConstant(destWidth, 0);
  }

  auto srcWidth = result.getType().cast<IntegerType>().getWidth();
  if (srcWidth == unsigned(destWidth))
    return result;

  if (destWidth == 0)
    return {};

  if (srcWidth > unsigned(destWidth)) {
    auto resultType = builder.getIntegerType(destWidth);
    return builder.createOrFold<comb::ExtractOp>(resultType, result, 0);
  }

  auto resultType = builder.getIntegerType(destWidth);

  // Extension follows the sign of the source value, not the destination.
  auto valueFIRType = value.getType().cast<FIRRTLType>().getPassiveType();
  if (valueFIRType.cast<IntType>().isSigned())
    return builder.createOrFold<comb::SExtOp>(resultType, result);

  auto zero = getOrCreateIntConstant(destWidth - srcWidth, 0);
  return builder.createOrFold<comb::ConcatOp>(zero, result);
}

/// Set the lowered value of 'orig' to 'result', remembering this in a map.
/// This always returns success() to make it more convenient in lowering code.
///
/// Note that result may be null here if we're lowering orig to a zero-bit
/// value.
///
LogicalResult FIRRTLLowering::setLowering(Value orig, Value result) {
  assert(orig.getType().isa<FIRRTLType>() &&
         (!result || !result.getType().isa<FIRRTLType>()) &&
         "Lowering didn't turn a FIRRTL value into a non-FIRRTL value");

#ifndef NDEBUG
  auto srcWidth = orig.getType()
                      .cast<FIRRTLType>()
                      .getPassiveType()
                      .getBitWidthOrSentinel();

  // Caller should pass null value iff this was a zero bit value.
  if (srcWidth != -1) {
    if (result)
      assert((srcWidth != 0) &&
             "Lowering produced value for zero width source");
    else
      assert((srcWidth == 0) &&
             "Lowering produced null value but source wasn't zero width");
  }
#endif

  assert(!valueMapping.count(orig) && "value lowered multiple times");
  valueMapping[orig] = result;
  return success();
}

/// Set the lowering for a value to the specified result.  This came from a
/// possible folding, so check to see if we need to handle a constant.
LogicalResult FIRRTLLowering::setPossiblyFoldedLowering(Value orig,
                                                        Value result) {
  // If this is a constant, check to see if we have it in our unique mapping:
  // it could have come from folding an operation.
  if (auto cst = dyn_cast_or_null<hw::ConstantOp>(result.getDefiningOp())) {
    auto &entry = hwConstantMap[cst.valueAttr()];
    if (entry == cst) {
      // We're already using an entry in the constant map, nothing to do.
    } else if (entry) {
      // We already had this constant, reuse the one we have instead of the
      // one we just folded.
      result = entry;
      cst->erase();
    } else {
      // This is a new constant.  Remember it!
      entry = cst;
      cst->moveBefore(&theModule.getBodyBlock()->front());
    }
  }

  return setLowering(orig, result);
}

/// Create a new operation with type ResultOpType and arguments CtorArgTypes,
/// then call setLowering with its result.
template <typename ResultOpType, typename... CtorArgTypes>
LogicalResult FIRRTLLowering::setLoweringTo(Operation *orig,
                                            CtorArgTypes... args) {
  auto result = builder.createOrFold<ResultOpType>(args...);
  return setPossiblyFoldedLowering(orig->getResult(0), result);
}

/// Switch the insertion point of the current builder to the end of the
/// specified block and run the closure.  This correctly handles the case where
/// the closure is null, but the caller needs to make sure the block exists.
void FIRRTLLowering::runWithInsertionPointAtEndOfBlock(
    std::function<void(void)> fn, Region &region) {
  if (!fn)
    return;

  auto oldIP = builder.saveInsertionPoint();

  builder.setInsertionPointToEnd(&region.front());
  fn();
  builder.restoreInsertionPoint(oldIP);
}

void FIRRTLLowering::addToAlwaysBlock(Value clock,
                                      std::function<void(void)> fn) {
  // This isn't uniquing the parent region in.  This can be added as a key to
  // the alwaysBlocks set if needed.
  assert(isa<hw::HWModuleOp>(builder.getBlock()->getParentOp()) &&
         "only support inserting into the top level of a module so far");

  auto &op = alwaysBlocks[clock];
  if (op) {
    runWithInsertionPointAtEndOfBlock(fn, op.body());
  } else {
    op = builder.create<sv::AlwaysOp>(sv::EventControl::AtPosEdge, clock, fn);
  }
}

void FIRRTLLowering::addToAlwaysFFBlock(sv::EventControl clockEdge, Value clock,
                                        ::ResetType resetStyle,
                                        sv::EventControl resetEdge, Value reset,
                                        std::function<void(void)> body,
                                        std::function<void(void)> resetBody) {
  auto &op = alwaysFFBlocks[std::make_tuple(
      builder.getBlock(), clockEdge, clock, resetStyle, resetEdge, reset)];
  if (op) {
    runWithInsertionPointAtEndOfBlock(body, op.bodyBlk());
    runWithInsertionPointAtEndOfBlock(resetBody, op.resetBlk());
  } else {
    if (reset) {
      op = builder.create<sv::AlwaysFFOp>(clockEdge, clock, resetStyle,
                                          resetEdge, reset, body, resetBody);
    } else {
      assert(!resetBody);
      op = builder.create<sv::AlwaysFFOp>(clockEdge, clock, body);
    }
  }
}

void FIRRTLLowering::addToIfDefBlock(StringRef cond,
                                     std::function<void(void)> thenCtor,
                                     std::function<void(void)> elseCtor) {
  auto condAttr = builder.getStringAttr(cond);
  auto &op = ifdefBlocks[{builder.getBlock(), condAttr}];
  if (op) {
    runWithInsertionPointAtEndOfBlock(thenCtor, op.thenRegion());
    runWithInsertionPointAtEndOfBlock(elseCtor, op.elseRegion());
  } else {
    op = builder.create<sv::IfDefOp>(condAttr, thenCtor, elseCtor);
  }
}

void FIRRTLLowering::addToInitialBlock(std::function<void(void)> body) {
  auto &op = initialBlocks[builder.getBlock()];
  if (op) {
    runWithInsertionPointAtEndOfBlock(body, op.body());
  } else {
    op = builder.create<sv::InitialOp>(body);
  }
}

void FIRRTLLowering::addToIfDefProceduralBlock(
    StringRef cond, std::function<void(void)> thenCtor,
    std::function<void(void)> elseCtor) {
  // Check to see if we already have an ifdef on this condition immediately
  // before the insertion point.  If so, extend it.
  auto insertIt = builder.getInsertionPoint();
  if (insertIt != builder.getBlock()->begin())
    if (auto ifdef = dyn_cast<sv::IfDefProceduralOp>(*--insertIt)) {
      if (ifdef.cond() == cond) {
        runWithInsertionPointAtEndOfBlock(thenCtor, ifdef.thenRegion());
        runWithInsertionPointAtEndOfBlock(elseCtor, ifdef.elseRegion());
        return;
      }
    }

  builder.create<sv::IfDefProceduralOp>(cond, thenCtor, elseCtor);
}

void FIRRTLLowering::addIfProceduralBlock(Value cond,
                                          std::function<void(void)> thenCtor,
                                          std::function<void(void)> elseCtor) {
  // Check to see if we already have an if on this condition immediately
  // before the insertion point.  If so, extend it.
  auto insertIt = builder.getInsertionPoint();
  if (insertIt != builder.getBlock()->begin())
    if (auto ifOp = dyn_cast<sv::IfOp>(*--insertIt)) {
      if (ifOp.cond() == cond) {
        runWithInsertionPointAtEndOfBlock(thenCtor, ifOp.thenRegion());
        runWithInsertionPointAtEndOfBlock(elseCtor, ifOp.elseRegion());
        return;
      }
    }

  builder.create<sv::IfOp>(cond, thenCtor, elseCtor);
}

//===----------------------------------------------------------------------===//
// Special Operations
//===----------------------------------------------------------------------===//

/// Handle the case where an operation wasn't lowered.  When this happens, the
/// operands should just be unlowered non-FIRRTL values.  If the operand was
/// not lowered then leave it alone, otherwise we have a problem with lowering.
///
FIRRTLLowering::UnloweredOpResult
FIRRTLLowering::handleUnloweredOp(Operation *op) {
  // Scan the operand list for the operation to see if none were lowered.  In
  // that case the operation must be something lowered to HW already, e.g.
  // the hw.output operation.  This is success for us because it is already
  // lowered.
  if (llvm::all_of(op->getOpOperands(), [&](auto &operand) -> bool {
        return !valueMapping.count(operand.get());
      })) {
    return AlreadyLowered;
  }

  // Ok, at least one operand got lowered, so this operation is using a FIRRTL
  // value, but wasn't itself lowered.  This is because the lowering is
  // incomplete. This is either a bug or incomplete implementation.
  //
  // There is one aspect of incompleteness we intentionally expect: we allow
  // primitive operations that produce a zero bit result to be ignored by the
  // lowering logic.  They don't have side effects, and handling this corner
  // case just complicates each of the lowering hooks. Instead, we just handle
  // them all right here.
  if (op->getNumResults() == 1) {
    auto resultType = op->getResult(0).getType();
    if (resultType.isa<FIRRTLType>() && isZeroBitFIRRTLType(resultType) &&
        (isExpression(op) || isa<AsPassivePrimOp>(op) ||
         isa<AsNonPassivePrimOp>(op))) {
      // Zero bit values lower to the null Value.
      (void)setLowering(op->getResult(0), Value());
      return NowLowered;
    }
  }
  op->emitOpError("LowerToHW couldn't handle this operation");
  return LoweringFailure;
}

LogicalResult FIRRTLLowering::visitExpr(ConstantOp op) {
  return setLowering(op, getOrCreateIntConstant(op.value()));
}

LogicalResult FIRRTLLowering::visitExpr(SpecialConstantOp op) {
  return setLowering(op,
                     getOrCreateIntConstant(APInt(/*bitWidth*/ 1, op.value())));
}

LogicalResult FIRRTLLowering::visitExpr(SubfieldOp op) {
  // firrtl.mem lowering lowers some SubfieldOps.  Zero-width can leave invalid
  // subfield accesses
  if (getLoweredValue(op) || !op.input())
    return success();

  // Extracting a zero bit value from a struct is defined but doesn't do
  // anything.
  if (isZeroBitFIRRTLType(op->getResult(0).getType()))
    return setLowering(op, Value());

  auto resultType = lowerType(op->getResult(0).getType());
  Value value = getLoweredValue(op.input());
  assert(resultType && value && "subfield type lowering failed");

  return setLoweringTo<hw::StructExtractOp>(
      op, resultType, value,
      op.input().getType().cast<BundleType>().getElementName(op.fieldIndex()));
}

//===----------------------------------------------------------------------===//
// Declarations
//===----------------------------------------------------------------------===//

LogicalResult FIRRTLLowering::visitDecl(WireOp op) {
  auto resultType = lowerType(op.result().getType());
  if (!resultType)
    return failure();

  if (resultType.isInteger(0))
    return setLowering(op, Value());

  // Name attr is required on sv.wire but optional on firrtl.wire.
  auto nameAttr = op.nameAttr() ? op.nameAttr() : builder.getStringAttr("");

  if (!AnnotationSet::removeAnnotations(
          op, "firrtl.transforms.DontTouchAnnotation"))
    return setLoweringTo<sv::WireOp>(op, resultType, nameAttr);
  auto moduleName = cast<hw::HWModuleOp>(op->getParentOp()).getName();
  auto symName = op.nameAttr();
  // Prepend the name of the module to make the symbol name unique in the symbol
  // table, it is already unique in the module. Checking if the name is unique
  // in the SymbolTable is non-trivial.
  if (symName && !symName.getValue().empty())
    symName = builder.getStringAttr(Twine("__") + moduleName + Twine("__") +
                                    symName.getValue());
  else
    // If marked with DontTouch but does not have a name, then add a symbol
    // name. Note: Same symbol name for all such wires in the module.
    symName = builder.getStringAttr(Twine("__") + moduleName + Twine("__"));

  // This is not a temporary wire created by the compiler, so attach a symbol
  // name.
  return setLoweringTo<sv::WireOp>(op, resultType, nameAttr, symName);
}

LogicalResult FIRRTLLowering::visitDecl(NodeOp op) {
  auto operand = getLoweredValue(op.input());
  if (!operand)
    return handleZeroBit(op.input(),
                         [&]() { return setLowering(op, Value()); });

  // Node operations are logical noops, but may carry annotations.  Don't touch
  // indicates we should keep it as a wire.
  if (AnnotationSet::removeAnnotations(
          op, "firrtl.transforms.DontTouchAnnotation")) {
    // name may be empty
    auto name = op->getAttrOfType<StringAttr>("name");
    auto moduleName = cast<hw::HWModuleOp>(op->getParentOp()).getName();
    auto symName = builder.getStringAttr(Twine("__") + moduleName +
                                         Twine("__") + name.getValue());

    auto wire = builder.create<sv::WireOp>(operand.getType(), name, symName);
    builder.create<sv::AssignOp>(wire, operand);
  }

  return setLowering(op, operand);
}

/// Emit a `INIT_RANDOM_PROLOG_ statement into the current block.  This should
/// already be within an `ifndef SYNTHESIS + initial block.
void FIRRTLLowering::emitRandomizePrologIfNeeded() {
  if (randomizePrologEmitted)
    return;

  builder.create<sv::VerbatimOp>("`INIT_RANDOM_PROLOG_");
  randomizePrologEmitted = true;
}

void FIRRTLLowering::initializeRegister(Value reg, Value resetSignal) {
  // Construct and return a new reference to `RANDOM.  It is always a 32-bit
  // unsigned expression.  Calls to $random have side effects, so we use
  // VerbatimExprSEOp.
  auto getRandom32Val = [&]() -> Value {
    return builder.create<sv::VerbatimExprSEOp>(builder.getIntegerType(32),
                                                "`RANDOM");
  };

  // Return an expression containing random bits of the specified width.
  // An explicit std::function is required here due to recursion.
  std::function<Value(IntegerType)> getRandomValue =
      [&](IntegerType type) -> Value {
    assert(type.getWidth() != 0 && "zero bit width's not supported");
    auto rand32 = getRandom32Val();
    if (type.getWidth() <= 32)
      return builder.createOrFold<comb::ExtractOp>(type, rand32, 0);

    // Get the top part.
    auto rest = getRandomValue(builder.getIntegerType(type.getWidth() - 32));
    return builder.createOrFold<comb::ConcatOp>(rand32, rest);
  };

  // Get a random value with the specified width, combining or truncating
  // 32-bit units as necessary.
  auto emitRandomInit = [&](Value dest, Type type) {
    auto intType = type.cast<IntegerType>();
    if (intType.getWidth() == 0)
      return;
    builder.create<sv::BPAssignOp>(dest, getRandomValue(intType));
  };

  // Randomly initialize everything in the register. If the register
  // is an aggregate type, then assign random values to all its
  // constituent ground types.
  // TODO: Extend this so it recursively initializes everything.
  auto randomInit = [&]() {
    auto type = reg.getType().dyn_cast<hw::InOutType>().getElementType();
    TypeSwitch<Type>(type)
        .Case<hw::UnpackedArrayType>([&](auto a) {
          for (size_t i = 0, e = a.getSize(); i != e; ++i) {
            auto iIdx = getOrCreateIntConstant(log2(e + 1), i);
            auto arrayIndex = builder.create<sv::ArrayIndexInOutOp>(reg, iIdx);
            emitRandomInit(arrayIndex, a.getElementType());
          }
        })
        .Default([&](auto type) { emitRandomInit(reg, type); });
  };

  // Emit the initializer expression for simulation that fills it with random
  // value.
  addToIfDefBlock("SYNTHESIS", std::function<void()>(), [&]() {
    addToInitialBlock([&]() {
      emitRandomizePrologIfNeeded();
      circuitState.used_RANDOMIZE_REG_INIT = 1;
      addToIfDefProceduralBlock("RANDOMIZE_REG_INIT", [&]() {
        if (resetSignal) {
          addIfProceduralBlock(resetSignal, {}, [&]() { randomInit(); });
        } else {
          randomInit();
        }
      });
    });
  });
}

LogicalResult FIRRTLLowering::visitDecl(RegOp op) {
  auto resultType = lowerType(op.result().getType());
  if (!resultType)
    return failure();
  if (resultType.isInteger(0))
    return setLowering(op, Value());

  // Add symbol if DontTouch annotation present.
  auto regResult =
      AnnotationSet::removeAnnotations(op,
                                       "firrtl.transforms.DontTouchAnnotation")
          ? builder.create<sv::RegOp>(resultType, op.nameAttr(), op.nameAttr())
          : builder.create<sv::RegOp>(resultType, op.nameAttr());
  (void)setLowering(op, regResult);

  initializeRegister(regResult, Value());

  return success();
}

LogicalResult FIRRTLLowering::visitDecl(RegResetOp op) {
  auto resultType = lowerType(op.result().getType());
  if (!resultType)
    return failure();
  if (resultType.isInteger(0))
    return setLowering(op, Value());

  Value clockVal = getLoweredValue(op.clockVal());
  Value resetSignal = getLoweredValue(op.resetSignal());
  // Reset values may be narrower than the register.  Extend appropriately.
  Value resetValue = getLoweredAndExtOrTruncValue(
      op.resetValue(), op.getType().cast<FIRRTLType>());

  if (!clockVal || !resetSignal || !resetValue)
    return failure();

  auto regResult = builder.create<sv::RegOp>(resultType, op.nameAttr());
  (void)setLowering(op, regResult);

  auto resetFn = [&]() {
    builder.create<sv::PAssignOp>(regResult, resetValue);
  };

  if (op.resetSignal().getType().isa<AsyncResetType>()) {
    addToAlwaysFFBlock(sv::EventControl::AtPosEdge, clockVal,
                       ::ResetType::AsyncReset, sv::EventControl::AtPosEdge,
                       resetSignal, std::function<void()>(), resetFn);
  } else { // sync reset
    addToAlwaysFFBlock(sv::EventControl::AtPosEdge, clockVal,
                       ::ResetType::SyncReset, sv::EventControl::AtPosEdge,
                       resetSignal, std::function<void()>(), resetFn);
  }

  initializeRegister(regResult, resetSignal);
  return success();
}

LogicalResult FIRRTLLowering::visitDecl(MemOp op) {
  auto memName = op.name();
  if (memName.empty())
    memName = "mem";

  // TODO: Remove this restriction and preserve aggregates in
  // memories.
  if (op.getDataType().cast<FIRRTLType>().getPassiveType().isa<BundleType>())
    return op.emitOpError(
        "should have already been lowered from a ground type to an aggregate "
        "type using the LowerTypes pass. Use "
        "'firtool --lower-types' or 'circt-opt "
        "--pass-pipeline='firrtl.circuit(firrtl-lower-types)' "
        "to run this.");

  FirMemory memSummary = analyzeMemOp(op);

  // Process each port in turn.
  SmallVector<Type, 8> resultTypes;
  SmallVector<Value, 8> readOperands;
  SmallVector<Value, 8> rwOperands;
  SmallVector<Value, 8> writeOperands;
  DenseMap<Operation *, size_t> returnHolder;

  size_t readCount = 0;
  size_t writeCount = 0;
  size_t readwriteCount = 0;

  // Memories return multiple structs, one for each port, which means we have
  // two layers of type to split appart.
  for (size_t i = 0, e = op.getNumResults(); i != e; ++i) {
    auto portName = op.getPortName(i).getValue();
    auto portKind = op.getPortKind(i);

    auto &portKindNum =
        portKind == MemOp::PortKind::Read
            ? readCount
            : portKind == MemOp::PortKind::Write ? writeCount : readwriteCount;

    auto addInput = [&](SmallVectorImpl<Value> &operands, StringRef field,
                        size_t width) {
      auto portType =
          IntegerType::get(op.getContext(), std::max((size_t)1, width));
      auto accesses = getAllFieldAccesses(op.getResult(i), field);

      Value wire = createTmpWireOp(
          portType, ("." + portName + "." + field + ".wire").str());

      for (auto a : accesses) {
        if (a.getType()
                .cast<FIRRTLType>()
                .getPassiveType()
                .getBitWidthOrSentinel() > 0)
          (void)setLowering(a, wire);
        else
          a->eraseOperand(0);
      }

      wire = builder.create<sv::ReadInOutOp>(wire);
      operands.push_back(wire);
    };
    auto addOutput = [&](StringRef field, size_t width) {
      auto portType =
          IntegerType::get(op.getContext(), std::max((size_t)1, width));
      resultTypes.push_back(portType);

      // Now collect the data for the instance.  A op produces multiple
      // structures, so we need to look through SubfieldOps to see the true
      // inputs and outputs.
      auto accesses = getAllFieldAccesses(op.getResult(i), field);
      // Read data ports are tracked to be updated later
      for (auto &a : accesses)
        if (width)
          returnHolder[a] = resultTypes.size() - 1;
        else
          a->eraseOperand(0);
    };

    if (portKind == MemOp::PortKind::Read) {
      addInput(readOperands, "clk", 1);
      addInput(readOperands, "en", 1);
      addInput(readOperands, "addr", llvm::Log2_64_Ceil(memSummary.depth));
      addOutput("data", memSummary.dataWidth);
    } else if (portKind == MemOp::PortKind::ReadWrite) {
      addInput(readOperands, "clk", 1);
      addInput(readOperands, "en", 1);
      addInput(readOperands, "addr", llvm::Log2_64_Ceil(memSummary.depth));
      addInput(readOperands, "wmode", 1);
      addInput(writeOperands, "wmask", 1);
      addInput(writeOperands, "wdata", memSummary.dataWidth);
      addOutput("rdata", memSummary.dataWidth);
    } else {
      addInput(writeOperands, "clk", 1);
      addInput(writeOperands, "en", 1);
      addInput(writeOperands, "addr", llvm::Log2_64_Ceil(memSummary.depth));
      addInput(writeOperands, "mask", 1);
      addInput(writeOperands, "data", memSummary.dataWidth);
    }
    ++portKindNum;
  }

  auto memModuleAttr = builder.getSymbolRefAttr(getFirMemoryName(memSummary));

  // FIRRTL ports are arbitrary in order, ours are not
  readOperands.append(writeOperands.begin(), writeOperands.end());
  // Create the instance to replace the memop
  auto inst = builder.create<hw::InstanceOp>(
      resultTypes, builder.getStringAttr(memName), memModuleAttr, readOperands,
      DictionaryAttr(), StringAttr());

  // Update all users of the result of read ports
  for (auto &ret : returnHolder)
    (void)setLowering(ret.first->getResult(0), inst.getResult(ret.second));
  return success();
}

LogicalResult FIRRTLLowering::visitDecl(InstanceOp oldInstance) {
  auto *oldModule =
      circuitState.circuitOp.lookupSymbol(oldInstance.moduleName());
  auto newModule = circuitState.getNewModule(oldModule);
  if (!newModule) {
    oldInstance->emitOpError("could not find module referenced by instance");
    return failure();
  }

  // If this is a referenced to a parameterized extmodule, then bring the
  // parameters over to this instance.
  DictionaryAttr parameters;
  if (auto oldExtModule = dyn_cast<FExtModuleOp>(oldModule))
    if (auto paramsOptional = oldExtModule.parameters())
      parameters = paramsOptional.getValue();

  // Decode information about the input and output ports on the referenced
  // module.
  SmallVector<ModulePortInfo, 8> portInfo = getModulePortInfo(oldModule);

  // Build an index from the name attribute to an index into portInfo, so we
  // can do efficient lookups.
  llvm::SmallDenseMap<Attribute, unsigned> portIndicesByName;
  for (unsigned portIdx = 0, e = portInfo.size(); portIdx != e; ++portIdx)
    portIndicesByName[portInfo[portIdx].name] = portIdx;

  // Ok, get ready to create the new instance operation.  We need to prepare
  // input operands and results.
  SmallVector<Type, 8> resultTypes;
  SmallVector<Value, 8> operands;
  for (size_t portIndex = 0, e = portInfo.size(); portIndex != e; ++portIndex) {
    auto &port = portInfo[portIndex];
    auto portType = lowerType(port.type);
    if (!portType) {
      oldInstance->emitOpError("could not lower type of port ") << port.name;
      return failure();
    }

    // Drop zero bit input/inout ports.
    if (portType.isInteger(0))
      continue;

    // Just remember outputs, we'll wire them up after creating the instance.
    if (port.isOutput()) {
      resultTypes.push_back(portType);
      continue;
    }

    // If we can find the connects to this port, then we can directly
    // materialize it.
    auto portResult = oldInstance.getResult(portIndex);
    assert(portResult && "invalid IR, couldn't find port");

    // Create a wire for each input/inout operand, so there is
    // something to connect to.
    Value wire =
        createTmpWireOp(portType, "." + port.getName().str() + ".wire");

    // Know that the argument FIRRTL value is equal to this wire, allowing
    // connects to it to be lowered.
    (void)setLowering(portResult, wire);

    // inout ports directly use the wire, but normal inputs read it.
    if (!port.isInOut())
      wire = builder.create<sv::ReadInOutOp>(wire);

    operands.push_back(wire);
  }

  // Use the symbol from the module we are referencing.
  FlatSymbolRefAttr symbolAttr = builder.getSymbolRefAttr(newModule);

  // If this instance is destined to be lowered to a bind, generate a symbol for
  // it and generate a bind op.  Enter the bind into global CircuitLoweringState
  // so that this can be moved outside of module once we're guaranteed to not be
  // a parallel context.
  StringAttr symbol({});
  if (oldInstance->getAttrOfType<BoolAttr>("lowerToBind").getValue()) {
    symbol = builder.getStringAttr("__" + oldInstance.name() + "__");
    auto bindOp =
        builder.create<sv::BindOp>(builder.getSymbolRefAttr(symbol.getValue()));
    bindOp->setAttr("output_file",
                    hw::OutputFileAttr::get(
                        builder.getStringAttr(""),
                        builder.getStringAttr("bindings.sv"),
                        /*exclude_from_filelist=*/builder.getBoolAttr(true),
                        /*exclude_replicated_ops=*/builder.getBoolAttr(true),
                        bindOp.getContext()));
    // Add the bind to the circuit state.  This will be moved outside of the
    // encapsulating module after all modules have been processed in parallel.
    circuitState.addBind(bindOp);
  }

  // Create the new hw.instance operation.
  auto newInstance =
      builder.create<hw::InstanceOp>(resultTypes, oldInstance.nameAttr(),
                                     symbolAttr, operands, parameters, symbol);

  if (symbol)
    newInstance->setAttr("doNotPrint", builder.getBoolAttr(true));

  // Now that we have the new hw.instance, we need to remap all of the users
  // of the outputs/results to the values returned by the instance.
  unsigned resultNo = 0;
  for (size_t portIndex = 0, e = portInfo.size(); portIndex != e; ++portIndex) {
    auto &port = portInfo[portIndex];
    if (!port.isOutput() || isZeroBitFIRRTLType(port.type))
      continue;

    Value resultVal = newInstance.getResult(resultNo);

    auto oldPortResult = oldInstance.getResult(portIndex);
    (void)setLowering(oldPortResult, resultVal);
    ++resultNo;
  }
  return success();
}

//===----------------------------------------------------------------------===//
// Unary Operations
//===----------------------------------------------------------------------===//

// Lower a cast that is a noop at the HW level.
LogicalResult FIRRTLLowering::lowerNoopCast(Operation *op) {
  auto operand = getPossiblyInoutLoweredValue(op->getOperand(0));
  if (!operand)
    return failure();

  // Noop cast.
  return setLowering(op->getResult(0), operand);
}

LogicalResult FIRRTLLowering::visitExpr(StdIntCastOp op) {
  // Conversions from standard integer types to FIRRTL types are lowered as
  // the input operand.
  if (auto opIntType = op.getOperand().getType().dyn_cast<IntegerType>()) {
    if (opIntType.getWidth() != 0)
      return setLowering(op, op.getOperand());
    else
      return setLowering(op, Value());
  }

  // Otherwise must be a conversion from FIRRTL type to standard int type.
  auto result = getLoweredValue(op.getOperand());
  if (!result) {
    // If this is a conversion from a zero bit HW type to firrtl value, then
    // we want to successfully lower this to a null Value.
    if (op.getOperand().getType().isSignlessInteger(0)) {
      return setLowering(op, Value());
    }
    return failure();
  }

  // We lower firrtl.stdIntCast converting from a firrtl type to a standard
  // type into the lowered operand.
  op.replaceAllUsesWith(result);
  return success();
}

LogicalResult FIRRTLLowering::visitExpr(HWStructCastOp op) {
  // Conversions from hw struct types to FIRRTL types are lowered as the
  // input operand.
  if (auto opStructType = op.getOperand().getType().dyn_cast<hw::StructType>())
    return setLowering(op, op.getOperand());

  // Otherwise must be a conversion from FIRRTL bundle type to hw struct
  // type.
  auto result = getLoweredValue(op.getOperand());
  if (!result)
    return failure();

  // We lower firrtl.stdStructCast converting from a firrtl bundle to an hw
  // struct type into the lowered operand.
  op.replaceAllUsesWith(result);
  return success();
}

LogicalResult FIRRTLLowering::visitExpr(AnalogInOutCastOp op) {
  // Standard -> FIRRTL.
  if (!op.getOperand().getType().isa<FIRRTLType>())
    return setLowering(op, op.getOperand());

  // FIRRTL -> Standard.
  auto result = getPossiblyInoutLoweredValue(op.getOperand());
  if (!result)
    return failure();

  if (!result.getType().isa<hw::InOutType>())
    return op.emitOpError("operand didn't lower to inout type correctly");

  op.replaceAllUsesWith(result);
  return success();
}

LogicalResult FIRRTLLowering::visitExpr(CvtPrimOp op) {
  auto operand = getLoweredValue(op.getOperand());
  if (!operand) {
    return handleZeroBit(op.getOperand(), [&]() {
      // Unsigned zero bit to Signed is 1b0.
      if (op.getOperand().getType().cast<IntType>().isUnsigned())
        return setLowering(op, getOrCreateIntConstant(1, 0));
      // Signed->Signed is a zero bit value.
      return setLowering(op, Value());
    });
  }

  // Signed to signed is a noop.
  if (op.getOperand().getType().cast<IntType>().isSigned())
    return setLowering(op, operand);

  // Otherwise prepend a zero bit.
  auto zero = getOrCreateIntConstant(1, 0);
  return setLoweringTo<comb::ConcatOp>(op, zero, operand);
}

LogicalResult FIRRTLLowering::visitExpr(NotPrimOp op) {
  auto operand = getLoweredValue(op.input());
  if (!operand)
    return failure();
  // ~x  ---> x ^ 0xFF
  auto allOnes = getOrCreateIntConstant(
      APInt::getAllOnesValue(operand.getType().getIntOrFloatBitWidth()));
  return setLoweringTo<comb::XorOp>(op, operand, allOnes);
}

LogicalResult FIRRTLLowering::visitExpr(NegPrimOp op) {
  // FIRRTL negate always adds a bit.
  // -x ---> 0-sext(x) or 0-zext(x)
  auto operand = getLoweredAndExtendedValue(op.input(), op.getType());
  if (!operand)
    return failure();

  auto resultType = lowerType(op.getType());

  auto zero = getOrCreateIntConstant(resultType.getIntOrFloatBitWidth(), 0);
  return setLoweringTo<comb::SubOp>(op, zero, operand);
}

// Pad is a noop or extension operation.
LogicalResult FIRRTLLowering::visitExpr(PadPrimOp op) {
  auto operand = getLoweredAndExtendedValue(op.input(), op.getType());
  if (!operand)
    return failure();
  return setLowering(op, operand);
}

LogicalResult FIRRTLLowering::visitExpr(XorRPrimOp op) {
  auto operand = getLoweredValue(op.input());
  if (!operand) {
    return handleZeroBit(op.input(), [&]() {
      return setLowering(op, getOrCreateIntConstant(1, 0));
    });
    return failure();
  }

  return setLoweringTo<comb::ParityOp>(op, builder.getIntegerType(1), operand);
}

LogicalResult FIRRTLLowering::visitExpr(AndRPrimOp op) {
  auto operand = getLoweredValue(op.input());
  if (!operand) {
    return handleZeroBit(op.input(), [&]() {
      return setLowering(op, getOrCreateIntConstant(1, 1));
    });
  }

  // Lower AndR to == -1
  return setLoweringTo<comb::ICmpOp>(
      op, ICmpPredicate::eq, operand,
      getOrCreateIntConstant(
          APInt::getAllOnesValue(operand.getType().getIntOrFloatBitWidth())));
}

LogicalResult FIRRTLLowering::visitExpr(OrRPrimOp op) {
  auto operand = getLoweredValue(op.input());
  if (!operand) {
    return handleZeroBit(op.input(), [&]() {
      return setLowering(op, getOrCreateIntConstant(1, 0));
    });
    return failure();
  }

  // Lower OrR to != 0
  return setLoweringTo<comb::ICmpOp>(
      op, ICmpPredicate::ne, operand,
      getOrCreateIntConstant(operand.getType().getIntOrFloatBitWidth(), 0));
}

//===----------------------------------------------------------------------===//
// Binary Operations
//===----------------------------------------------------------------------===//

template <typename ResultOpType>
LogicalResult FIRRTLLowering::lowerBinOpToVariadic(Operation *op) {
  auto resultType = op->getResult(0).getType();
  auto lhs = getLoweredAndExtendedValue(op->getOperand(0), resultType);
  auto rhs = getLoweredAndExtendedValue(op->getOperand(1), resultType);
  if (!lhs || !rhs)
    return failure();

  return setLoweringTo<ResultOpType>(op, lhs, rhs);
}

/// lowerBinOp extends each operand to the destination type, then performs the
/// specified binary operator.
template <typename ResultUnsignedOpType, typename ResultSignedOpType>
LogicalResult FIRRTLLowering::lowerBinOp(Operation *op) {
  // Extend the two operands to match the destination type.
  auto resultType = op->getResult(0).getType();
  auto lhs = getLoweredAndExtendedValue(op->getOperand(0), resultType);
  auto rhs = getLoweredAndExtendedValue(op->getOperand(1), resultType);
  if (!lhs || !rhs)
    return failure();

  // Emit the result operation.
  if (resultType.cast<IntType>().isSigned())
    return setLoweringTo<ResultSignedOpType>(op, lhs, rhs);
  return setLoweringTo<ResultUnsignedOpType>(op, lhs, rhs);
}

/// lowerCmpOp extends each operand to the longest type, then performs the
/// specified binary operator.
LogicalResult FIRRTLLowering::lowerCmpOp(Operation *op, ICmpPredicate signedOp,
                                         ICmpPredicate unsignedOp) {
  // Extend the two operands to match the longest type.
  auto lhsIntType = op->getOperand(0).getType().cast<IntType>();
  auto rhsIntType = op->getOperand(1).getType().cast<IntType>();
  if (!lhsIntType.hasWidth() || !rhsIntType.hasWidth())
    return failure();

  auto cmpType = getWidestIntType(lhsIntType, rhsIntType);
  if (cmpType.getWidth() == 0) // Handle 0-width inputs by promoting to 1 bit.
    cmpType = UIntType::get(builder.getContext(), 1);
  auto lhs = getLoweredAndExtendedValue(op->getOperand(0), cmpType);
  auto rhs = getLoweredAndExtendedValue(op->getOperand(1), cmpType);
  if (!lhs || !rhs)
    return failure();

  // Emit the result operation.
  Type resultType = builder.getIntegerType(1);
  return setLoweringTo<comb::ICmpOp>(
      op, resultType, lhsIntType.isSigned() ? signedOp : unsignedOp, lhs, rhs);
}

/// Lower a divide or dynamic shift, where the operation has to be performed in
/// the widest type of the result and two inputs then truncated down.
template <typename SignedOp, typename UnsignedOp>
LogicalResult FIRRTLLowering::lowerDivLikeOp(Operation *op) {
  // hw has equal types for these, firrtl doesn't.  The type of the firrtl
  // RHS may be wider than the LHS, and we cannot truncate off the high bits
  // (because an overlarge amount is supposed to shift in sign or zero bits).
  auto opType = op->getResult(0).getType().cast<IntType>();
  if (opType.getWidth() == 0)
    return setLowering(op->getResult(0), Value());

  auto resultType = getWidestIntType(opType, op->getOperand(1).getType());
  resultType = getWidestIntType(resultType, op->getOperand(0).getType());
  auto lhs = getLoweredAndExtendedValue(op->getOperand(0), resultType);
  auto rhs = getLoweredAndExtendedValue(op->getOperand(1), resultType);
  if (!lhs || !rhs)
    return failure();

  Value result;
  if (opType.isSigned())
    result = builder.createOrFold<SignedOp>(lhs, rhs);
  else
    result = builder.createOrFold<UnsignedOp>(lhs, rhs);

  if (resultType == opType)
    return setLowering(op->getResult(0), result);
  return setLoweringTo<comb::ExtractOp>(op, lowerType(opType), result, 0);
}

LogicalResult FIRRTLLowering::visitExpr(CatPrimOp op) {
  auto lhs = getLoweredValue(op.lhs());
  auto rhs = getLoweredValue(op.rhs());
  if (!lhs) {
    return handleZeroBit(op.lhs(), [&]() {
      if (rhs) // cat(0bit, x) --> x
        return setLowering(op, rhs);
      // cat(0bit, 0bit) --> 0bit
      return handleZeroBit(op.rhs(),
                           [&]() { return setLowering(op, Value()); });
    });
  }

  if (!rhs) // cat(x, 0bit) --> x
    return handleZeroBit(op.rhs(), [&]() { return setLowering(op, lhs); });

  return setLoweringTo<comb::ConcatOp>(op, lhs, rhs);
}

//===----------------------------------------------------------------------===//
// Other Operations
//===----------------------------------------------------------------------===//

LogicalResult FIRRTLLowering::visitExpr(BitsPrimOp op) {
  auto input = getLoweredValue(op.input());
  if (!input)
    return failure();

  Type resultType = builder.getIntegerType(op.hi() - op.lo() + 1);
  return setLoweringTo<comb::ExtractOp>(op, resultType, input, op.lo());
}

LogicalResult FIRRTLLowering::visitExpr(InvalidValueOp op) {
  auto resultTy = lowerType(op.getType());
  if (!resultTy)
    return failure();

  // Values of analog type always need to be lowered to something with inout
  // type.  We do that by lowering to a wire and return that.  As with the
  // SFC, we do not connect anything to this, because it is bidirectional.
  if (op.getType().isa<AnalogType>())
    // This is a locally visible, private wire created by the compiler, so do
    // not attach a symbol name.
    return setLoweringTo<sv::WireOp>(op, resultTy, ".invalid_analog");

  // We lower invalid to 0.  TODO: the FIRRTL spec mentions something about
  // lowering it to a random value, we should see if this is what we need to
  // do.
  if (auto intType = resultTy.dyn_cast<IntegerType>()) {
    if (intType.getWidth() == 0) // Let the caller handle zero width values.
      return failure();
    return setLowering(
        op, getOrCreateIntConstant(resultTy.getIntOrFloatBitWidth(), 0));
  }

  // Invalid for bundles isn't supported.
  op.emitOpError("unsupported type");
  return failure();
}

LogicalResult FIRRTLLowering::visitExpr(HeadPrimOp op) {
  auto input = getLoweredValue(op.input());
  if (!input)
    return failure();
  auto inWidth = input.getType().cast<IntegerType>().getWidth();
  if (op.amount() == 0)
    return setLowering(op, Value());
  Type resultType = builder.getIntegerType(op.amount());
  return setLoweringTo<comb::ExtractOp>(op, resultType, input,
                                        inWidth - op.amount());
}

LogicalResult FIRRTLLowering::visitExpr(ShlPrimOp op) {
  auto input = getLoweredValue(op.input());
  if (!input) {
    return handleZeroBit(op.input(), [&]() {
      if (op.amount() == 0)
        return failure();
      return setLowering(op, getOrCreateIntConstant(op.amount(), 0));
    });
  }

  // Handle the degenerate case.
  if (op.amount() == 0)
    return setLowering(op, input);

  auto zero = getOrCreateIntConstant(op.amount(), 0);
  return setLoweringTo<comb::ConcatOp>(op, input, zero);
}

LogicalResult FIRRTLLowering::visitExpr(ShrPrimOp op) {
  auto input = getLoweredValue(op.input());
  if (!input)
    return failure();

  // Handle the special degenerate cases.
  auto inWidth = input.getType().cast<IntegerType>().getWidth();
  auto shiftAmount = op.amount();
  if (shiftAmount >= inWidth) {
    // Unsigned shift by full width returns a single-bit zero.
    if (op.input().getType().cast<IntType>().isUnsigned())
      return setLowering(op, getOrCreateIntConstant(1, 0));

    // Signed shift by full width is equivalent to extracting the sign bit.
    shiftAmount = inWidth - 1;
  }

  Type resultType = builder.getIntegerType(inWidth - shiftAmount);
  return setLoweringTo<comb::ExtractOp>(op, resultType, input, shiftAmount);
}

LogicalResult FIRRTLLowering::visitExpr(TailPrimOp op) {
  auto input = getLoweredValue(op.input());
  if (!input)
    return failure();

  auto inWidth = input.getType().cast<IntegerType>().getWidth();
  if (inWidth == op.amount())
    return setLowering(op, Value());
  Type resultType = builder.getIntegerType(inWidth - op.amount());
  return setLoweringTo<comb::ExtractOp>(op, resultType, input, 0);
}

LogicalResult FIRRTLLowering::visitExpr(MuxPrimOp op) {
  auto cond = getLoweredValue(op.sel());
  auto ifTrue = getLoweredAndExtendedValue(op.high(), op.getType());
  auto ifFalse = getLoweredAndExtendedValue(op.low(), op.getType());
  if (!ifTrue || !ifFalse)
    return failure();

  // Lower mux(0-bit, x, y) -> y
  if (!cond) {
    return handleZeroBit(op.sel(), [&]() { return setLowering(op, ifFalse); });
  }

  return setLoweringTo<comb::MuxOp>(op, ifTrue.getType(), cond, ifTrue,
                                    ifFalse);
}

LogicalResult FIRRTLLowering::visitExpr(VerbatimExprOp op) {
  auto resultTy = lowerType(op.getType());
  if (!resultTy)
    return failure();

  SmallVector<Value, 4> operands;
  operands.reserve(op.operands().size());
  for (auto operand : op.operands()) {
    auto lowered = getLoweredValue(operand);
    if (!lowered)
      return failure();
    operands.push_back(lowered);
  }

  return setLoweringTo<sv::VerbatimExprOp>(op, resultTy, op.textAttr(),
                                           operands);
}

//===----------------------------------------------------------------------===//
// Statements
//===----------------------------------------------------------------------===//

LogicalResult FIRRTLLowering::visitStmt(SkipOp op) {
  // Nothing!  We could emit an comment as a verbatim op if there were a
  // reason to.
  return success();
}

LogicalResult FIRRTLLowering::visitStmt(ConnectOp op) {
  auto dest = op.dest();
  // The source can be a smaller integer, extend it as appropriate if so.
  auto destType = dest.getType().cast<FIRRTLType>().getPassiveType();
  auto srcVal = getLoweredAndExtendedValue(op.src(), destType);
  if (!srcVal)
    return handleZeroBit(op.src(), []() { return success(); });

  auto destVal = getPossiblyInoutLoweredValue(dest);
  if (!destVal)
    return failure();

  if (!destVal.getType().isa<hw::InOutType>())
    return op.emitError("destination isn't an inout type");

  // If this is an assignment to a register, then the connect implicitly
  // happens under the clock that gates the register.
  if (auto regOp = dyn_cast_or_null<RegOp>(dest.getDefiningOp())) {
    Value clockVal = getLoweredValue(regOp.clockVal());
    if (!clockVal)
      return failure();

    addToAlwaysFFBlock(sv::EventControl::AtPosEdge, clockVal, [&]() {
      builder.create<sv::PAssignOp>(destVal, srcVal);
    });
    return success();
  }

  // If this is an assignment to a RegReset, then the connect implicitly
  // happens under the clock and reset that gate the register.
  if (auto regResetOp = dyn_cast_or_null<RegResetOp>(dest.getDefiningOp())) {
    Value clockVal = getLoweredValue(regResetOp.clockVal());
    Value resetSignal = getLoweredValue(regResetOp.resetSignal());
    if (!clockVal || !resetSignal)
      return failure();

    addToAlwaysFFBlock(sv::EventControl::AtPosEdge, clockVal,
                       regResetOp.resetSignal().getType().isa<AsyncResetType>()
                           ? ::ResetType::AsyncReset
                           : ::ResetType::SyncReset,
                       sv::EventControl::AtPosEdge, resetSignal, [&]() {
                         builder.create<sv::PAssignOp>(destVal, srcVal);
                       });
    return success();
  }

  builder.create<sv::AssignOp>(destVal, srcVal);
  return success();
}

// This will have to handle struct connects at some point.
LogicalResult FIRRTLLowering::visitStmt(PartialConnectOp op) {
  auto dest = op.dest();
  // The source can be a different size integer, adjust it as appropriate if
  // so.
  auto destType = dest.getType().cast<FIRRTLType>().getPassiveType();
  auto srcVal = getLoweredAndExtOrTruncValue(op.src(), destType);
  if (!srcVal)
    return success(isZeroBitFIRRTLType(op.src().getType()) ||
                   isZeroBitFIRRTLType(destType));

  auto destVal = getPossiblyInoutLoweredValue(dest);
  if (!destVal)
    return failure();

  if (!destVal.getType().isa<hw::InOutType>())
    return op.emitError("destination isn't an inout type");

  // If this is an assignment to a register, then the connect implicitly
  // happens under the clock that gates the register.
  if (auto regOp = dyn_cast_or_null<RegOp>(dest.getDefiningOp())) {
    Value clockVal = getLoweredValue(regOp.clockVal());
    if (!clockVal)
      return failure();

    addToAlwaysFFBlock(sv::EventControl::AtPosEdge, clockVal, [&]() {
      builder.create<sv::PAssignOp>(destVal, srcVal);
    });
    return success();
  }

  // If this is an assignment to a RegReset, then the connect implicitly
  // happens under the clock and reset that gate the register.
  if (auto regResetOp = dyn_cast_or_null<RegResetOp>(dest.getDefiningOp())) {
    Value clockVal = getLoweredValue(regResetOp.clockVal());
    Value resetSignal = getLoweredValue(regResetOp.resetSignal());
    if (!clockVal || !resetSignal)
      return failure();

    auto resetStyle = regResetOp.resetSignal().getType().isa<AsyncResetType>()
                          ? ::ResetType::AsyncReset
                          : ::ResetType::SyncReset;
    addToAlwaysFFBlock(sv::EventControl::AtPosEdge, clockVal, resetStyle,
                       sv::EventControl::AtPosEdge, resetSignal, [&]() {
                         builder.create<sv::PAssignOp>(destVal, srcVal);
                       });
    return success();
  }

  builder.create<sv::AssignOp>(destVal, srcVal);
  return success();
}

// Printf is a macro op that lowers to an sv.ifdef.procedural, an sv.if,
// and an sv.fwrite all nested together.
LogicalResult FIRRTLLowering::visitStmt(PrintFOp op) {
  auto clock = getLoweredValue(op.clock());
  auto cond = getLoweredValue(op.cond());
  if (!clock || !cond)
    return failure();

  SmallVector<Value, 4> operands;
  operands.reserve(op.operands().size());
  for (auto operand : op.operands()) {
    operands.push_back(getLoweredValue(operand));
    if (!operands.back()) {
      // If this is a zero bit operand, just pass a one bit zero.
      if (!isZeroBitFIRRTLType(operand.getType()))
        return failure();
      operands.back() = getOrCreateIntConstant(1, 0);
    }
  }

  addToAlwaysBlock(clock, [&]() {
    // Emit an "#ifndef SYNTHESIS" guard into the always block.
    addToIfDefProceduralBlock("SYNTHESIS", std::function<void()>(), [&]() {
      circuitState.used_PRINTF_COND = true;

      // Emit an "sv.if '`PRINTF_COND_ & cond' into the #ifndef.
      Value ifCond =
          builder.create<sv::VerbatimExprOp>(cond.getType(), "`PRINTF_COND_");
      ifCond = builder.createOrFold<comb::AndOp>(ifCond, cond);

      addIfProceduralBlock(ifCond, [&]() {
        // Emit the sv.fwrite.
        builder.create<sv::FWriteOp>(op.formatString(), operands);
      });
    });
  });

  return success();
}

// Stop lowers into a nested series of behavioral statements plus $fatal or
// $finish.
LogicalResult FIRRTLLowering::visitStmt(StopOp op) {
  auto clock = getLoweredValue(op.clock());
  auto cond = getLoweredValue(op.cond());
  if (!clock || !cond)
    return failure();

  // Emit this into an "sv.always posedge" body.
  addToAlwaysBlock(clock, [&]() {
    // Emit an "#ifndef SYNTHESIS" guard into the always block.
    addToIfDefProceduralBlock("SYNTHESIS", std::function<void()>(), [&]() {
      circuitState.used_STOP_COND = true;

      // Emit an "sv.if '`STOP_COND_ & cond' into the #ifndef.
      Value ifCond =
          builder.create<sv::VerbatimExprOp>(cond.getType(), "`STOP_COND_");
      ifCond = builder.createOrFold<comb::AndOp>(ifCond, cond);
      addIfProceduralBlock(ifCond, [&]() {
        // Emit the sv.fatal or sv.finish.
        if (op.exitCode())
          builder.create<sv::FatalOp>();
        else
          builder.create<sv::FinishOp>();
      });
    });
  });

  return success();
}

/// Template for lowering verification statements from type A to
/// type B.
///
/// For example, lowering the "foo" op to the "bar" op would start
/// with:
///
///     foo(clock, condition, enable, "message")
///
/// This becomes a Verilog clocking block with the "bar" op guarded
/// by an if enable:
///
///     always @(posedge clock) begin
///       if (enable) begin
///         bar(condition);
///       end
///     end
template <typename AOpTy, typename BOpTy>
LogicalResult FIRRTLLowering::lowerVerificationStatement(AOpTy op) {
  auto clock = getLoweredValue(op.clock());
  auto enable = getLoweredValue(op.enable());
  auto predicate = getLoweredValue(op.predicate());
  if (!clock || !enable || !predicate)
    return failure();

  addToAlwaysBlock(clock, [&]() {
    addIfProceduralBlock(enable, [&]() {
      // Create BOpTy inside the always/if.
      StringAttr label;
      if (op.nameAttr())
        label = op.nameAttr();
      else
        label = builder.getStringAttr("");
      builder.create<BOpTy>(predicate, label);
    });
  });

  return success();
}

// Lower an assert to SystemVerilog.
LogicalResult FIRRTLLowering::visitStmt(AssertOp op) {
  return lowerVerificationStatement<AssertOp, sv::AssertOp>(op);
}

// Lower an assume to SystemVerilog.
LogicalResult FIRRTLLowering::visitStmt(AssumeOp op) {
  return lowerVerificationStatement<AssumeOp, sv::AssumeOp>(op);
}

// Lower a cover to SystemVerilog.
LogicalResult FIRRTLLowering::visitStmt(CoverOp op) {
  return lowerVerificationStatement<CoverOp, sv::CoverOp>(op);
}

LogicalResult FIRRTLLowering::visitStmt(AttachOp op) {
  // Don't emit anything for a zero or one operand attach.
  if (op.operands().size() < 2)
    return success();

  SmallVector<Value, 4> inoutValues;
  for (auto v : op.operands()) {
    inoutValues.push_back(getPossiblyInoutLoweredValue(v));
    if (!inoutValues.back()) {
      // Ignore zero bit values.
      if (!isZeroBitFIRRTLType(v.getType()))
        return failure();
      inoutValues.pop_back();
      continue;
    }

    if (!inoutValues.back().getType().isa<hw::InOutType>())
      return op.emitError("operand isn't an inout type");
  }

  if (inoutValues.size() < 2)
    return success();

  addToIfDefBlock(
      "SYNTHESIS",
      // If we're doing synthesis, we emit an all-pairs assign complex.
      [&]() {
        SmallVector<Value, 4> values;
        for (size_t i = 0, e = inoutValues.size(); i != e; ++i)
          values.push_back(
              builder.createOrFold<sv::ReadInOutOp>(inoutValues[i]));

        for (size_t i1 = 0, e = inoutValues.size(); i1 != e; ++i1) {
          for (size_t i2 = 0; i2 != e; ++i2)
            if (i1 != i2)
              builder.create<sv::AssignOp>(inoutValues[i1], values[i2]);
        }
      },
      // In the non-synthesis case, we emit a SystemVerilog alias statement.
      [&]() {
        builder.create<sv::IfDefOp>(
            "verilator",
            [&]() {
              builder.create<sv::VerbatimOp>(
                  "`error \"Verilator does not support alias and thus cannot "
                  "arbitrarily connect bidirectional wires and ports\"");
            },
            [&]() { builder.create<sv::AliasOp>(inoutValues); });
      });

  return success();
}
