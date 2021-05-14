//===- Simulation.cpp - Handshake MLIR Operations -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains functions used to execute a restricted form of the
// standard dialect, and the handshake dialect.
//
//===----------------------------------------------------------------------===//

#include <fstream>
#include <list>

#include "mlir/Dialect/StandardOps/IR/Ops.h"

#include "circt/Dialect/Handshake/HandshakeOps.h"
#include "circt/Dialect/Handshake/Simulation.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "runner"

#define INDEX_WIDTH 32

using namespace llvm;

using namespace circt;

STATISTIC(instructionsExecuted, "Instructions Executed");
STATISTIC(simulatedTime, "Simulated Time");

void executeOp(mlir::ConstantIndexOp op, std::vector<Any> &in,
               std::vector<Any> &out) {
  auto attr = op->getAttrOfType<mlir::IntegerAttr>("value");
  out[0] = attr.getValue().sextOrTrunc(INDEX_WIDTH);
}

void executeOp(mlir::ConstantIntOp op, std::vector<Any> &in,
               std::vector<Any> &out) {
  auto attr = op->getAttrOfType<mlir::IntegerAttr>("value");
  out[0] = attr.getValue();
}

void executeOp(mlir::AddIOp op, std::vector<Any> &in, std::vector<Any> &out) {
  out[0] = any_cast<APInt>(in[0]) + any_cast<APInt>(in[1]);
}

void executeOp(mlir::AddFOp op, std::vector<Any> &in, std::vector<Any> &out) {
  out[0] = any_cast<APFloat>(in[0]) + any_cast<APFloat>(in[1]);
}

void executeOp(mlir::CmpIOp op, std::vector<Any> &in, std::vector<Any> &out) {
  APInt in0 = any_cast<APInt>(in[0]);
  APInt in1 = any_cast<APInt>(in[1]);
  APInt out0(1, mlir::applyCmpPredicate(op.getPredicate(), in0, in1));
  out[0] = out0;
}

void executeOp(mlir::CmpFOp op, std::vector<Any> &in, std::vector<Any> &out) {
  APFloat in0 = any_cast<APFloat>(in[0]);
  APFloat in1 = any_cast<APFloat>(in[1]);
  APInt out0(1, mlir::applyCmpPredicate(op.getPredicate(), in0, in1));
  out[0] = out0;
}

void executeOp(mlir::SubIOp op, std::vector<Any> &in, std::vector<Any> &out) {
  out[0] = any_cast<APInt>(in[0]) - any_cast<APInt>(in[1]);
}

void executeOp(mlir::SubFOp op, std::vector<Any> &in, std::vector<Any> &out) {
  out[0] = any_cast<APFloat>(in[0]) + any_cast<APFloat>(in[1]);
}

void executeOp(mlir::MulIOp op, std::vector<Any> &in, std::vector<Any> &out) {
  out[0] = any_cast<APInt>(in[0]) * any_cast<APInt>(in[1]);
}

void executeOp(mlir::MulFOp op, std::vector<Any> &in, std::vector<Any> &out) {
  out[0] = any_cast<APFloat>(in[0]) * any_cast<APFloat>(in[1]);
}

void executeOp(mlir::SignedDivIOp op, std::vector<Any> &in,
               std::vector<Any> &out) {
  assert(any_cast<APInt>(in[1]).getZExtValue() && "Division By Zero!");
  out[0] = any_cast<APInt>(in[0]).sdiv(any_cast<APInt>(in[1]));
}

void executeOp(mlir::UnsignedDivIOp op, std::vector<Any> &in,
               std::vector<Any> &out) {
  assert(any_cast<APInt>(in[1]).getZExtValue() && "Division By Zero!");
  out[0] = any_cast<APInt>(in[0]).udiv(any_cast<APInt>(in[1]));
}

void executeOp(mlir::DivFOp op, std::vector<Any> &in, std::vector<Any> &out) {
  out[0] = any_cast<APFloat>(in[0]) / any_cast<APFloat>(in[1]);
}

void executeOp(mlir::IndexCastOp op, std::vector<Any> &in,
               std::vector<Any> &out) {
  out[0] = in[0];
}

void executeOp(mlir::SignExtendIOp op, std::vector<Any> &in,
               std::vector<Any> &out) {
  int64_t width = op.getType().getIntOrFloatBitWidth();
  out[0] = any_cast<APInt>(in[0]).sext(width);
}

void executeOp(mlir::ZeroExtendIOp op, std::vector<Any> &in,
               std::vector<Any> &out) {
  int64_t width = op.getType().getIntOrFloatBitWidth();
  out[0] = any_cast<APInt>(in[0]).zext(width);
}

// Allocate a new matrix with dimensions given by the type, in the
// given store.  Return the pseuddo-pointer to the new matrix in the
// store (i.e. the first dimension index)
unsigned allocateMemRef(mlir::MemRefType type, std::vector<Any> &in,
                        std::vector<std::vector<Any>> &store,
                        std::vector<double> &storeTimes) {
  ArrayRef<int64_t> shape = type.getShape();
  int64_t allocationSize = 1;
  unsigned count = 0;
  for (int64_t dim : shape) {
    if (dim > 0)
      allocationSize *= dim;
    else {
      assert(count < in.size());
      allocationSize *= any_cast<APInt>(in[count++]).getSExtValue();
    }
  }
  unsigned ptr = store.size();
  store.resize(ptr + 1);
  storeTimes.resize(ptr + 1);
  store[ptr].resize(allocationSize);
  storeTimes[ptr] = 0.0;
  mlir::Type elementType = type.getElementType();
  int64_t width = elementType.getIntOrFloatBitWidth();
  for (int i = 0; i < allocationSize; i++) {
    if (elementType.isa<mlir::IntegerType>()) {
      store[ptr][i] = APInt(width, 0);
    } else if (elementType.isa<mlir::FloatType>()) {
      store[ptr][i] = APFloat(0.0);
    } else {
      llvm_unreachable("Unknown result type!\n");
    }
  }
  return ptr;
}

void executeOp(mlir::memref::LoadOp op, std::vector<Any> &in,
               std::vector<Any> &out, std::vector<std::vector<Any>> &store) {
  ArrayRef<int64_t> shape = op.getMemRefType().getShape();
  unsigned address = 0;
  for (unsigned i = 0; i < shape.size(); i++) {
    address = address * shape[i] + any_cast<APInt>(in[i + 1]).getZExtValue();
  }
  unsigned ptr = any_cast<unsigned>(in[0]);
  assert(ptr < store.size());
  auto &ref = store[ptr];
  assert(address < ref.size());
  //  LLVM_DEBUG(dbgs() << "Load " << ref[address] << " from " << ptr << "[" <<
  //  address << "]\n");
  Any result = ref[address];
  out[0] = result;
}

void executeOp(mlir::memref::StoreOp op, std::vector<Any> &in,
               std::vector<Any> &out, std::vector<std::vector<Any>> &store) {
  ArrayRef<int64_t> shape = op.getMemRefType().getShape();
  unsigned address = 0;
  for (unsigned i = 0; i < shape.size(); i++) {
    address = address * shape[i] + any_cast<APInt>(in[i + 2]).getZExtValue();
  }
  unsigned ptr = any_cast<unsigned>(in[1]);
  assert(ptr < store.size());
  auto &ref = store[ptr];
  //  LLVM_DEBUG(dbgs() << "Store " << in[0] << " to " << ptr << "[" << address
  //  << "]\n");
  assert(address < ref.size());
  ref[address] = in[0];
}

void debugArg(const std::string &head, mlir::Value op, const APInt &value,
              double time) {
  LLVM_DEBUG(dbgs() << "  " << head << ":  " << op << " = " << value
                    << " (APInt<" << value.getBitWidth() << ">) @" << time
                    << "\n");
}

void debugArg(const std::string &head, mlir::Value op, const APFloat &value,
              double time) {
  LLVM_DEBUG(dbgs() << "  " << head << ":  " << op << " = ";
             value.print(dbgs()); dbgs() << " ("
                                         << "float"
                                         << ") @" << time << "\n");
}

void debugArg(const std::string &head, mlir::Value op, const Any &value,
              double time) {
  if (any_isa<APInt>(value)) {
    debugArg(head, op, any_cast<APInt>(value), time);
  } else if (any_isa<APFloat>(value)) {
    debugArg(head, op, any_cast<APFloat>(value), time);
  } else if (any_isa<unsigned>(value)) {
    // Represents an allocated buffer.
    LLVM_DEBUG(dbgs() << "  " << head << ":  " << op << " = Buffer "
                      << any_cast<unsigned>(value) << "\n");
  } else {
    llvm_unreachable("unknown type");
  }
}

Any readValueWithType(mlir::Type type, std::string in) {
  std::stringstream arg(in);
  if (type.isIndex()) {
    int64_t x;
    arg >> x;
    int64_t width = INDEX_WIDTH;
    APInt aparg(width, x);
    return aparg;
  } else if (type.isa<mlir::IntegerType>()) {
    int64_t x;
    arg >> x;
    int64_t width = type.getIntOrFloatBitWidth();
    APInt aparg(width, x);
    return aparg;
    // } else if (type.isF16()) {
    //   half x;
    //   arg >> x;
    //   APFloat aparg(x);
    //   return aparg;
  } else if (type.isF32()) {
    float x;
    arg >> x;
    APFloat aparg(x);
    return aparg;
  } else if (type.isF64()) {
    double x;
    arg >> x;
    APFloat aparg(x);
    return aparg;
  } else {
    assert(0 && "unknown argument type!\n");
    return {};
  }
  return nullptr;
}

std::string printAnyValueWithType(mlir::Type type, Any &value) {
  std::stringstream out;
  if (type.isa<mlir::IntegerType>() || type.isa<mlir::IndexType>()) {
    out << any_cast<APInt>(value).getSExtValue();
    return out.str();
  } else if (type.isa<mlir::FloatType>()) {
    out << any_cast<APFloat>(value).convertToDouble();
    return out.str();
  } else if (type.isa<mlir::NoneType>()) {
    return "none";
  } else {
    llvm_unreachable("Unknown result type!");
  }
}

void scheduleIfNeeded(std::list<mlir::Operation *> &readyList,
                      llvm::DenseMap<mlir::Value, Any> &valueMap,
                      mlir::Operation *op) {
  if (std::find(readyList.begin(), readyList.end(), op) == readyList.end()) {
    readyList.push_back(op);
  }
}
void scheduleUses(std::list<mlir::Operation *> &readyList,
                  llvm::DenseMap<mlir::Value, Any> &valueMap,
                  mlir::Value value) {
  for (auto &use : value.getUses()) {
    scheduleIfNeeded(readyList, valueMap, use.getOwner());
  }
}

bool executeStdOp(mlir::Operation &op, std::vector<Any> &inValues,
                  std::vector<Any> &outValues) {
  if (auto stdOp = dyn_cast<mlir::ConstantIndexOp>(op))
    executeOp(stdOp, inValues, outValues);
  else if (auto stdOp = dyn_cast<mlir::ConstantIntOp>(op))
    executeOp(stdOp, inValues, outValues);
  else if (auto stdOp = dyn_cast<mlir::AddIOp>(op))
    executeOp(stdOp, inValues, outValues);
  else if (auto stdOp = dyn_cast<mlir::AddFOp>(op))
    executeOp(stdOp, inValues, outValues);
  else if (auto stdOp = dyn_cast<mlir::SubIOp>(op))
    executeOp(stdOp, inValues, outValues);
  else if (auto stdOp = dyn_cast<mlir::SubFOp>(op))
    executeOp(stdOp, inValues, outValues);
  else if (auto stdOp = dyn_cast<mlir::CmpIOp>(op))
    executeOp(stdOp, inValues, outValues);
  else if (auto stdOp = dyn_cast<mlir::CmpFOp>(op))
    executeOp(stdOp, inValues, outValues);
  else if (auto stdOp = dyn_cast<mlir::MulIOp>(op))
    executeOp(stdOp, inValues, outValues);
  else if (auto stdOp = dyn_cast<mlir::MulFOp>(op))
    executeOp(stdOp, inValues, outValues);
  else if (auto stdOp = dyn_cast<mlir::UnsignedDivIOp>(op))
    executeOp(stdOp, inValues, outValues);
  else if (auto stdOp = dyn_cast<mlir::SignedDivIOp>(op))
    executeOp(stdOp, inValues, outValues);
  else if (auto stdOp = dyn_cast<mlir::DivFOp>(op))
    executeOp(stdOp, inValues, outValues);
  else if (auto stdOp = dyn_cast<mlir::IndexCastOp>(op))
    executeOp(stdOp, inValues, outValues);
  else if (auto stdOp = dyn_cast<mlir::SignExtendIOp>(op))
    executeOp(stdOp, inValues, outValues);
  else if (auto stdOp = dyn_cast<mlir::ZeroExtendIOp>(op))
    executeOp(stdOp, inValues, outValues);
  else
    return false;
  return true;
}

void executeFunction(mlir::FuncOp &toplevel,
                     llvm::DenseMap<mlir::Value, Any> &valueMap,
                     llvm::DenseMap<mlir::Value, double> &timeMap,
                     std::vector<Any> &results,
                     std::vector<double> &resultTimes,
                     std::vector<std::vector<Any>> &store,
                     std::vector<double> &storeTimes) {
  mlir::Block &entryBlock = toplevel.getBody().front();
  // An iterator which walks over the instructions.
  mlir::Block::iterator instIter = entryBlock.begin();

  // Main executive loop.  Start at the first instruction of the entry
  // block.  Fetch and execute instructions until we hit a terminator.
  while (true) {
    mlir::Operation &op = *instIter;
    int64_t i = 0;
    std::vector<Any> inValues(op.getNumOperands());
    std::vector<Any> outValues(op.getNumResults());
    LLVM_DEBUG(dbgs() << "OP:  " << op.getName() << "\n");
    double time = 0.0;
    for (mlir::Value in : op.getOperands()) {
      inValues[i] = valueMap[in];
      time = std::max(time, timeMap[in]);
      LLVM_DEBUG(debugArg("IN", in, inValues[i], timeMap[in]));
      i++;
    }
    if (executeStdOp(op, inValues, outValues)) {
    } else if (auto allocOp = dyn_cast<mlir::memref::AllocOp>(op)) {
      outValues[0] =
          allocateMemRef(allocOp.getType(), inValues, store, storeTimes);
      unsigned ptr = any_cast<unsigned>(outValues[0]);
      storeTimes[ptr] = time;
    } else if (auto loadOp = dyn_cast<mlir::memref::LoadOp>(op)) {
      executeOp(loadOp, inValues, outValues, store);
      unsigned ptr = any_cast<unsigned>(inValues[0]);
      double storeTime = storeTimes[ptr];
      LLVM_DEBUG(dbgs() << "STORE: " << storeTime << "\n");
      time = std::max(time, storeTime);
      storeTimes[ptr] = time;
    } else if (auto storeOp = dyn_cast<mlir::memref::StoreOp>(op)) {
      executeOp(storeOp, inValues, outValues, store);
      unsigned ptr = any_cast<unsigned>(inValues[1]);
      double storeTime = storeTimes[ptr];
      LLVM_DEBUG(dbgs() << "STORE: " << storeTime << "\n");
      time = std::max(time, storeTime);
      storeTimes[ptr] = time;
    } else if (auto branchOp = dyn_cast<mlir::BranchOp>(op)) {
      mlir::Block *dest = branchOp.getDest();
      unsigned arg = 0;
      for (mlir::Value out : dest->getArguments()) {
        LLVM_DEBUG(debugArg("ARG", out, inValues[arg], time));
        valueMap[out] = inValues[arg];
        timeMap[out] = time;
        arg++;
      }
      instIter = dest->begin();
      continue;
    } else if (auto condBranchOp = dyn_cast<mlir::CondBranchOp>(op)) {
      APInt condition = any_cast<APInt>(inValues[0]);
      mlir::Block *dest;
      std::vector<Any> inArgs;
      double time = 0.0;
      if (condition != 0) {
        dest = condBranchOp.getTrueDest();
        inArgs.resize(condBranchOp.getNumTrueOperands());
        for (mlir::Value in : condBranchOp.getTrueOperands()) {
          inArgs[i] = valueMap[in];
          time = std::max(time, timeMap[in]);
          LLVM_DEBUG(debugArg("IN", in, inArgs[i], timeMap[in]));
          i++;
        }
      } else {
        dest = condBranchOp.getFalseDest();
        inArgs.resize(condBranchOp.getNumFalseOperands());
        for (mlir::Value in : condBranchOp.getFalseOperands()) {
          inArgs[i] = valueMap[in];
          time = std::max(time, timeMap[in]);
          LLVM_DEBUG(debugArg("IN", in, inArgs[i], timeMap[in]));
          i++;
        }
      }
      int64_t arg = 0;
      for (mlir::Value out : dest->getArguments()) {
        LLVM_DEBUG(debugArg("ARG", out, inArgs[arg], time));
        valueMap[out] = inArgs[arg];
        timeMap[out] = time;
        arg++;
      }
      instIter = dest->begin();
      continue;
    } else if (auto returnOp = dyn_cast<mlir::ReturnOp>(op)) {
      for (unsigned i = 0; i < results.size(); i++) {
        results[i] = inValues[i];
        resultTimes[i] = timeMap[returnOp.getOperand(i)];
      }
      return;
    } else if (auto callOp = dyn_cast<mlir::CallOpInterface>(op)) {
      // implement function calls.
      mlir::Operation *calledOp = callOp.resolveCallable();
      if (auto funcOp = dyn_cast<mlir::FuncOp>(calledOp)) {
        mlir::FunctionType ftype = funcOp.getType();
        unsigned inputs = ftype.getNumInputs();
        unsigned outputs = ftype.getNumResults();
        llvm::DenseMap<mlir::Value, Any> newValueMap;
        llvm::DenseMap<mlir::Value, double> newTimeMap;
        std::vector<Any> results(outputs);
        std::vector<double> resultTimes(outputs);
        std::vector<std::vector<Any>> store;
        std::vector<double> storeTimes;
        mlir::Block &entryBlock = funcOp.getBody().front();
        mlir::Block::BlockArgListType blockArgs = entryBlock.getArguments();

        for (unsigned i = 0; i < inputs; i++) {
          newValueMap[blockArgs[i]] = inValues[i];
          newTimeMap[blockArgs[i]] = timeMap[op.getOperand(i)];
        }
        executeFunction(funcOp, newValueMap, newTimeMap, results, resultTimes,
                        store, storeTimes);
        i = 0;
        for (mlir::Value out : op.getResults()) {
          valueMap[out] = results[i];
          timeMap[out] = resultTimes[i];
          i++;
        }
        instIter++;
        continue;
      } else {
        llvm_unreachable("Callable was not a Function!\n");
      }
    } else {
      llvm_unreachable("Unknown operation!\n");
    }
    i = 0;
    for (mlir::Value out : op.getResults()) {
      LLVM_DEBUG(debugArg("OUT", out, outValues[i], time));
      valueMap[out] = outValues[i];
      timeMap[out] = time + 1;
      i++;
    }
    instIter++;
    instructionsExecuted++;
  }
}

static void exportData(llvm::DenseMap<unsigned, unsigned> &memoryMap,
                       std::vector<std::vector<llvm::Any>> &store) {
  // Export memory data into .dat
  for (auto &mem : memoryMap) {
    std::ofstream file("dataout" + std::to_string(mem.first) + ".dat");
    for (auto element : store[mem.second])
      file << any_cast<APInt>(element).getSExtValue() << "\n";
    file.close();
  }
}

void executeHandshakeFunction(handshake::FuncOp &toplevel,
                              llvm::DenseMap<mlir::Value, Any> &valueMap,
                              llvm::DenseMap<mlir::Value, double> &timeMap,
                              std::vector<Any> &results,
                              std::vector<double> &resultTimes,
                              std::vector<std::vector<Any>> &store,
                              std::vector<double> &storeTimes) {
  mlir::Block &entryBlock = toplevel.getBody().front();
  // The arguments of the entry block.
  mlir::Block::BlockArgListType blockArgs = entryBlock.getArguments();
  // A list of operations which might be ready to execute.
  std::list<mlir::Operation *> readyList;
  // A map of memory ops
  llvm::DenseMap<unsigned, unsigned> memoryMap;

  // Pre-allocate memory
  toplevel.walk([&](Operation *op) {
    if (auto handshakeMemoryOp = dyn_cast<handshake::MemoryOpInterface>(op))
      if (!handshakeMemoryOp.allocateMemory(memoryMap, store, storeTimes))
        llvm_unreachable("Memory op does not have unique ID!\n");
  });

  for (unsigned i = 0; i < blockArgs.size(); i++)
    scheduleUses(readyList, valueMap, blockArgs[i]);

#define EXTRA_DEBUG
  while (true) {
#ifdef EXTRA_DEBUG
    LLVM_DEBUG(
        for (auto t
             : readyList) { dbgs() << "READY: " << *t << "\n"; } dbgs()
            << "Live: " << valueMap.size() << "\n";
        for (auto t
             : valueMap) {
          debugArg("Value:", t.first, t.second, 0.0);
          //        dbgs() << "Value: " << *(t.first) << " " << t.second <<
          //        "\n";
        });
#endif
    assert(readyList.size() > 0);
    mlir::Operation &op = *readyList.front();
    readyList.pop_front();

    /*    for(mlir::Value out : op.getResults()) {
      if(valueMap.count(out) != 0) {
        LLVM_DEBUG(dbgs() << "OP:  " << op << "\n");
        for(auto t : valueMap) {
          dbgs() << "Value: " << *(t.first) << " " << t.second << "\n";
        }
        assert(false);
      }
      }*/

    // Execute handshake ops through ExecutableOpInterface
    if (auto handshakeOp = dyn_cast<handshake::ExecutableOpInterface>(op)) {
      std::vector<mlir::Value> scheduleList;
      if (!handshakeOp.tryExecute(valueMap, memoryMap, timeMap, store,
                                  scheduleList))
        readyList.push_back(&op);
      for (mlir::Value out : scheduleList)
        scheduleUses(readyList, valueMap, out);
      continue;
    }

    int64_t i = 0;
    std::vector<Any> inValues(op.getNumOperands());
    std::vector<Any> outValues(op.getNumResults());
    bool reschedule = false;
    LLVM_DEBUG(dbgs() << "OP: (" << op.getNumOperands() << "->"
                      << op.getNumResults() << ")" << op << "\n");
    double time;
    for (mlir::Value in : op.getOperands()) {
      if (valueMap.count(in) == 0) {
        reschedule = true;
        continue;
      }
      inValues[i] = valueMap[in];
      time = std::max(time, timeMap[in]);
      LLVM_DEBUG(debugArg("IN", in, inValues[i], timeMap[in]));
      i++;
    }
    if (reschedule) {
      LLVM_DEBUG(dbgs() << "Rescheduling data...\n");
      readyList.push_back(&op);
      continue;
    }
    // Consume the inputs.
    for (mlir::Value in : op.getOperands()) {
      valueMap.erase(in);
    }
    if (executeStdOp(op, inValues, outValues)) {
    } else if (auto returnOp = dyn_cast<handshake::ReturnOp>(op)) {
      for (unsigned i = 0; i < results.size(); i++) {
        results[i] = inValues[i];
        resultTimes[i] = timeMap[returnOp.getOperand(i)];
      }
      exportData(memoryMap, store);
      return;
      //} else {
      // implement function calls.
    } else {
      llvm_unreachable("Unknown operation!\n");
    }
    i = 0;
    for (mlir::Value out : op.getResults()) {
      LLVM_DEBUG(debugArg("OUT", out, outValues[i], time));
      assert(outValues[i].hasValue());
      valueMap[out] = outValues[i];
      timeMap[out] = time + 1;
      scheduleUses(readyList, valueMap, out);

      i++;
    }
    instructionsExecuted++;
  }
}

bool simulate(StringRef toplevelFunction, ArrayRef<std::string> inputArgs,
              mlir::OwningModuleRef &module, mlir::MLIRContext &context) {
  // The store associates each allocation in the program
  // (represented by a int) with a vector of values which can be
  // accessed by it.  Currently values are assumed to be an integer.
  std::vector<std::vector<Any>> store;
  std::vector<double> storeTimes;

  // The valueMap associates each SSA statement in the program
  // (represented by a Value*) with it's corresponding value.
  // Currently the value is assumed to be an integer.
  llvm::DenseMap<mlir::Value, Any> valueMap;

  // The timeMap associates each value with the time it was created.
  llvm::DenseMap<mlir::Value, double> timeMap;

  // We need three things in a function-type independent way.
  // The type signature of the function.
  mlir::FunctionType ftype;
  // The arguments of the entry block.
  mlir::Block::BlockArgListType blockArgs;

  // The number of inputs to the function in the IR.
  unsigned inputs;
  unsigned outputs;
  // The number of 'real' inputs.  This avoids the dummy input
  // associated with the handshake control logic for handshake
  // functions.
  unsigned realInputs;
  unsigned realOutputs;

  if (mlir::FuncOp toplevel =
          module->lookupSymbol<mlir::FuncOp>(toplevelFunction)) {
    ftype = toplevel.getType();
    mlir::Block &entryBlock = toplevel.getBody().front();
    blockArgs = entryBlock.getArguments();

    // Get the primary inputs of toplevel off the command line.
    inputs = ftype.getNumInputs();
    realInputs = inputs;
    outputs = ftype.getNumResults();
    realOutputs = outputs;
  } else if (handshake::FuncOp toplevel =
                 module->lookupSymbol<handshake::FuncOp>(toplevelFunction)) {
    ftype = toplevel.getType();
    mlir::Block &entryBlock = toplevel.getBody().front();
    blockArgs = entryBlock.getArguments();

    // Get the primary inputs of toplevel off the command line.
    inputs = ftype.getNumInputs();
    realInputs = inputs - 1;
    outputs = ftype.getNumResults();
    realOutputs = outputs - 1;
    if (inputs == 0) {
      errs() << "Function " << toplevelFunction << " is expected to have "
             << "at least one dummy argument.\n";
      return 1;
    }
    if (outputs == 0) {
      errs() << "Function " << toplevelFunction << " is expected to have "
             << "at least one dummy result.\n";
      return 1;
    }
    // Implicit none argument
    APInt apnonearg(1, 0);
    valueMap[blockArgs[blockArgs.size() - 1]] = apnonearg;
  } else {
    llvm_unreachable("Function not supported.\n");
  }

  if (inputArgs.size() != realInputs) {
    errs() << "Toplevel function " << toplevelFunction << " has " << realInputs
           << " actual arguments, but " << inputArgs.size()
           << " arguments were provided on the command line.\n";
    return 1;
  }

  for (unsigned i = 0; i < realInputs; i++) {
    mlir::Type type = ftype.getInput(i);
    if (type.isa<mlir::MemRefType>()) {
      // We require this memref type to be fully specified.
      auto memreftype = type.dyn_cast<mlir::MemRefType>();
      std::vector<Any> nothing;
      std::string x;
      unsigned buffer = allocateMemRef(memreftype, nothing, store, storeTimes);
      valueMap[blockArgs[i]] = buffer;
      timeMap[blockArgs[i]] = 0.0;
      int64_t i = 0;
      std::stringstream arg(inputArgs[i]);
      while (!arg.eof()) {
        getline(arg, x, ',');
        store[buffer][i++] = readValueWithType(memreftype.getElementType(), x);
      }
    } else {
      Any value = readValueWithType(type, inputArgs[i]);
      valueMap[blockArgs[i]] = value;
      timeMap[blockArgs[i]] = 0.0;
    }
  }

  std::vector<Any> results(realOutputs);
  std::vector<double> resultTimes(realOutputs);
  if (mlir::FuncOp toplevel =
          module->lookupSymbol<mlir::FuncOp>(toplevelFunction)) {
    executeFunction(toplevel, valueMap, timeMap, results, resultTimes, store,
                    storeTimes);
  } else if (handshake::FuncOp toplevel =
                 module->lookupSymbol<handshake::FuncOp>(toplevelFunction)) {
    executeHandshakeFunction(toplevel, valueMap, timeMap, results, resultTimes,
                             store, storeTimes);
  }
  double time = 0.0;
  for (unsigned i = 0; i < results.size(); i++) {
    mlir::Type t = ftype.getResult(i);
    outs() << printAnyValueWithType(t, results[i]) << " ";
    time = std::max(resultTimes[i], time);
  }
  // Return time if there is no result
  if (!results.size()) {
    for (auto t : timeMap)
      time = std::max(t.second, time);
  }
  // Go back through the arguments and output any memrefs.
  for (unsigned i = 0; i < realInputs; i++) {
    mlir::Type type = ftype.getInput(i);
    if (type.isa<mlir::MemRefType>()) {
      // We require this memref type to be fully specified.
      auto memreftype = type.dyn_cast<mlir::MemRefType>();
      unsigned buffer = any_cast<unsigned>(valueMap[blockArgs[i]]);
      auto elementType = memreftype.getElementType();
      for (int j = 0; j < memreftype.getNumElements(); j++) {
        if (j != 0)
          outs() << ",";
        outs() << printAnyValueWithType(elementType, store[buffer][j]);
      }
      outs() << " ";
    }
  }
  outs() << "\nTime: " << time << "\n";

  simulatedTime += (int)time;

  return 0;
}
