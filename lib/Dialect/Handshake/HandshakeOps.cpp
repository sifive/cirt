//===- HandshakeOps.cpp - Handshake MLIR Operations -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the declaration of the SlotMapping struct.
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/Handshake/HandshakeOps.h"
#include "circt/Support/LLVM.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/FunctionImplementation.h"
#include "mlir/IR/IntegerSet.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/Value.h"
#include "mlir/Transforms/InliningUtils.h"
#include "llvm/ADT/Any.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/Support/Debug.h"

#include <fstream>

using namespace circt;
using namespace circt::handshake;

#define INDEX_WIDTH 32

namespace circt {
namespace handshake {
#include "circt/Dialect/Handshake/HandshakeOps.inc"
}
} // namespace circt

// Convert ValueRange to vectors
std::vector<mlir::Value> toVector(mlir::ValueRange range) {
  return std::vector<mlir::Value>(range.begin(), range.end());
}

// Returns whether the precondition holds for a general op to execute
bool isReadyToExecute(ArrayRef<mlir::Value> ins, ArrayRef<mlir::Value> outs,
                      llvm::DenseMap<mlir::Value, llvm::Any> &valueMap) {
  for (auto in : ins)
    if (valueMap.count(in) == 0)
      return false;

  for (auto out : outs)
    if (valueMap.count(out) > 0)
      return false;

  return true;
}

// Fetch values from the value map and consume them
std::vector<llvm::Any>
fetchValues(ArrayRef<mlir::Value> values,
            llvm::DenseMap<mlir::Value, llvm::Any> &valueMap) {
  std::vector<llvm::Any> ins;
  for (auto &value : values) {
    assert(valueMap[value].hasValue());
    ins.push_back(valueMap[value]);
    valueMap.erase(value);
  }
  return ins;
}

// Store values to the value map
void storeValues(std::vector<llvm::Any> &values, ArrayRef<mlir::Value> outs,
                 llvm::DenseMap<mlir::Value, llvm::Any> &valueMap) {
  assert(values.size() == outs.size());
  for (unsigned long i = 0; i < outs.size(); ++i)
    valueMap[outs[i]] = values[i];
}

// Update the time map after the execution
void updateTime(ArrayRef<mlir::Value> ins, ArrayRef<mlir::Value> outs,
                llvm::DenseMap<mlir::Value, double> &timeMap, double latency) {
  double time = 0;
  for (auto &in : ins)
    time = std::max(time, timeMap[in]);
  time += latency;
  for (auto &out : outs)
    timeMap[out] = time;
}

bool tryToExecute(Operation *op,
                  llvm::DenseMap<mlir::Value, llvm::Any> &valueMap,
                  llvm::DenseMap<mlir::Value, double> &timeMap,
                  std::vector<mlir::Value> &scheduleList, double latency) {
  auto ins = toVector(op->getOperands());
  auto outs = toVector(op->getResults());

  if (isReadyToExecute(ins, outs, valueMap)) {
    auto in = fetchValues(ins, valueMap);
    std::vector<llvm::Any> out(outs.size());
    auto generalOp = dyn_cast<handshake::GeneralOpInterface>(op);
    if (!generalOp)
      op->emitError("Undefined execution for the current op");
    generalOp.execute(in, out);
    storeValues(out, outs, valueMap);
    updateTime(ins, outs, timeMap, latency);
    scheduleList = outs;
    return true;
  } else
    return false;
}

//===----------------------------------------------------------------------===//
// HandshakeOpsDialect
//===----------------------------------------------------------------------===//

void HandshakeOpsDialect::initialize() {
  addOperations<
#define GET_OP_LIST
#include "circt/Dialect/Handshake/HandshakeOps.cpp.inc"
      >();
}

void ForkOp::build(OpBuilder &builder, OperationState &result, Value operand,
                   int outputs) {

  auto type = operand.getType();

  // Fork has results as many as there are successor ops
  result.types.append(outputs, type);

  // Single operand
  result.addOperands(operand);

  // Fork is control-only if it has NoneType. This includes the no-data output
  // of a ControlMerge or a StartOp, as well as control values from MemoryOps.
  bool isControl = operand.getType().isa<NoneType>() ? true : false;
  result.addAttribute("control", builder.getBoolAttr(isControl));
}

void handshake::ForkOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                                    MLIRContext *context) {
  results.insert<circt::handshake::EliminateSimpleForksPattern>(context);
}

void handshake::ForkOp::execute(std::vector<llvm::Any> &ins,
                                std::vector<llvm::Any> &outs) {
  for (auto &out : outs)
    out = ins[0];
}

bool handshake::ForkOp::tryExecute(
    llvm::DenseMap<mlir::Value, llvm::Any> &valueMap,
    llvm::DenseMap<unsigned, unsigned> &memoryMap,
    llvm::DenseMap<mlir::Value, double> &timeMap,
    std::vector<std::vector<llvm::Any>> &store,
    std::vector<mlir::Value> &scheduleList) {
  return tryToExecute(getOperation(), valueMap, timeMap, scheduleList, 1);
}

void LazyForkOp::build(OpBuilder &builder, OperationState &result,
                       Value operand, int outputs) {

  auto type = operand.getType();

  // Fork has results as many as there are successor ops
  result.types.append(outputs, type);

  // Single operand
  result.addOperands(operand);

  // Fork is control-only if it is the no-data output of a ControlMerge or a
  // StartOp
  auto *op = operand.getDefiningOp();
  bool isControl = ((dyn_cast<ControlMergeOp>(op) || dyn_cast<StartOp>(op)) &&
                    operand == op->getResult(0))
                       ? true
                       : false;
  result.addAttribute("control", builder.getBoolAttr(isControl));
}

void MergeOp::build(OpBuilder &builder, OperationState &result, Value operand,
                    int inputs) {

  auto type = operand.getType();
  result.types.push_back(type);

  // Operand to keep defining value (used when connecting merges)
  // Removed afterwards
  result.addOperands(operand);

  // Operands from predecessor blocks
  for (int i = 0, e = inputs; i < e; ++i)
    result.addOperands(operand);
}

void MergeOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                          MLIRContext *context) {
  results.insert<circt::handshake::EliminateSimpleMergesPattern>(context);
}

bool handshake::MergeOp::tryExecute(
    llvm::DenseMap<mlir::Value, llvm::Any> &valueMap,
    llvm::DenseMap<unsigned, unsigned> &memoryMap,
    llvm::DenseMap<mlir::Value, double> &timeMap,
    std::vector<std::vector<llvm::Any>> &store,
    std::vector<mlir::Value> &scheduleList) {
  auto op = getOperation();
  bool found = false;
  int i = 0;
  for (mlir::Value in : op->getOperands()) {
    if (valueMap.count(in) == 1) {
      if (found)
        op->emitError("More than one valid input to Merge!");
      auto t = valueMap[in];
      valueMap[op->getResult(0)] = t;
      timeMap[op->getResult(0)] = timeMap[in];
      // Consume the inputs.
      valueMap.erase(in);
      found = true;
    }
    i++;
  }
  if (!found)
    op->emitError("No valid input to Merge!");
  scheduleList.push_back(getResult());
  return true;
}

void MuxOp::build(OpBuilder &builder, OperationState &result, Value operand,
                  int inputs) {

  auto type = operand.getType();
  result.types.push_back(type);

  // Operand connected to ControlMerge from same block
  result.addOperands(operand);

  // Operands from predecessor blocks
  for (int i = 0, e = inputs; i < e; ++i)
    result.addOperands(operand);
}

bool handshake::MuxOp::tryExecute(
    llvm::DenseMap<mlir::Value, llvm::Any> &valueMap,
    llvm::DenseMap<unsigned, unsigned> &memoryMap,
    llvm::DenseMap<mlir::Value, double> &timeMap,
    std::vector<std::vector<llvm::Any>> &store,
    std::vector<mlir::Value> &scheduleList) {
  auto op = getOperation();
  mlir::Value control = op->getOperand(0);
  if (valueMap.count(control) == 0)
    return false;
  auto controlValue = valueMap[control];
  auto controlTime = timeMap[control];
  mlir::Value in = llvm::any_cast<APInt>(controlValue) == 0 ? op->getOperand(1)
                                                            : op->getOperand(2);
  if (valueMap.count(in) == 0)
    return false;
  auto inValue = valueMap[in];
  auto inTime = timeMap[in];
  double time = std::max(controlTime, inTime);
  valueMap[op->getResult(0)] = inValue;
  timeMap[op->getResult(0)] = time;

  // Consume the inputs.
  valueMap.erase(control);
  valueMap.erase(in);
  scheduleList.push_back(getResult());
  return true;
}

static LogicalResult verify(MuxOp op) {
  unsigned numDataOperands = static_cast<int>(op.dataOperands().size());
  if (numDataOperands < 2)
    return op.emitError("need at least two inputs to mux");

  auto selectType = op.selectOperand().getType();

  unsigned selectBits;
  if (auto integerType = selectType.dyn_cast<IntegerType>())
    selectBits = integerType.getWidth();
  else if (selectType.isIndex())
    selectBits = IndexType::kInternalStorageBitWidth;
  else
    return op.emitError("unsupported type for select operand: ") << selectType;

  double maxDataOperands = std::pow(2, selectBits);
  if (numDataOperands > maxDataOperands)
    return op.emitError("select bitwidth was ")
           << selectBits << ", which can mux "
           << static_cast<int64_t>(maxDataOperands) << " operands, but found "
           << numDataOperands << " operands";

  return success();
}

void ControlMergeOp::build(OpBuilder &builder, OperationState &result,
                           Value operand, int inputs) {

  auto type = operand.getType();
  result.types.push_back(type);
  // Second result gives the input index to the muxes
  // Number of bits depends on encoding (log2/1-hot)
  result.types.push_back(builder.getIndexType());

  // Operand to keep defining value (used when connecting merges)
  // Removed afterwards
  result.addOperands(operand);

  // Operands from predecessor blocks
  for (int i = 0, e = inputs; i < e; ++i)
    result.addOperands(operand);

  result.addAttribute("control", builder.getBoolAttr(true));
}

namespace {
struct EliminateSimpleControlMergesPattern
    : mlir::OpRewritePattern<ControlMergeOp> {
  using mlir::OpRewritePattern<ControlMergeOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(ControlMergeOp op,
                                PatternRewriter &rewriter) const override;
};
} // namespace

LogicalResult EliminateSimpleControlMergesPattern::matchAndRewrite(
    ControlMergeOp op, PatternRewriter &rewriter) const {
  auto dataResult = op.getResult(0);
  auto choiceResult = op.getResult(1);
  auto choiceUnused = choiceResult.use_empty();
  if (!choiceUnused && !choiceResult.hasOneUse())
    return failure();

  Operation *choiceUser;
  if (choiceResult.hasOneUse()) {
    choiceUser = choiceResult.getUses().begin().getUser();
    if (!isa<SinkOp>(choiceUser))
      return failure();
  }

  auto merge = rewriter.create<MergeOp>(op.getLoc(), dataResult.getType(),
                                        op.dataOperands());

  for (auto &use : dataResult.getUses()) {
    auto *user = use.getOwner();
    rewriter.updateRootInPlace(
        user, [&]() { user->setOperand(use.getOperandNumber(), merge); });
  }

  if (choiceUnused) {
    rewriter.eraseOp(op);
    return success();
  }

  rewriter.eraseOp(choiceUser);
  rewriter.eraseOp(op);
  return success();
}

void ControlMergeOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                                 MLIRContext *context) {
  results.insert<EliminateSimpleControlMergesPattern>(context);
}

bool handshake::ControlMergeOp::tryExecute(
    llvm::DenseMap<mlir::Value, llvm::Any> &valueMap,
    llvm::DenseMap<unsigned, unsigned> &memoryMap,
    llvm::DenseMap<mlir::Value, double> &timeMap,
    std::vector<std::vector<llvm::Any>> &store,
    std::vector<mlir::Value> &scheduleList) {
  auto op = getOperation();
  bool found = false;
  int i = 0;
  for (mlir::Value in : op->getOperands()) {
    if (valueMap.count(in) == 1) {
      if (found)
        op->emitError("More than one valid input to CMerge!");
      auto t = valueMap[in];
      valueMap[op->getResult(0)] = t;
      timeMap[op->getResult(0)] = timeMap[in];

      valueMap[op->getResult(1)] = APInt(INDEX_WIDTH, i);
      timeMap[op->getResult(1)] = timeMap[in];

      // Consume the inputs.
      valueMap.erase(in);

      found = true;
    }
    i++;
  }
  if (!found)
    op->emitError("No valid input to CMerge!");
  scheduleList = toVector(op->getResults());
  return true;
}

void handshake::BranchOp::build(OpBuilder &builder, OperationState &result,
                                Value dataOperand) {

  auto type = dataOperand.getType();
  result.types.push_back(type);
  result.addOperands(dataOperand);

  // Branch is control-only if it is the no-data output of a ControlMerge or a
  // StartOp This holds because Branches are inserted before Forks
  auto *op = dataOperand.getDefiningOp();
  bool isControl = ((dyn_cast<ControlMergeOp>(op) || dyn_cast<StartOp>(op)) &&
                    dataOperand == op->getResult(0))
                       ? true
                       : false;
  result.addAttribute("control", builder.getBoolAttr(isControl));
}

void handshake::BranchOp::getCanonicalizationPatterns(
    RewritePatternSet &results, MLIRContext *context) {
  results.insert<circt::handshake::EliminateSimpleBranchesPattern>(context);
}

void handshake::BranchOp::execute(std::vector<llvm::Any> &ins,
                                  std::vector<llvm::Any> &outs) {
  outs[0] = ins[0];
}

bool handshake::BranchOp::tryExecute(
    llvm::DenseMap<mlir::Value, llvm::Any> &valueMap,
    llvm::DenseMap<unsigned, unsigned> &memoryMap,
    llvm::DenseMap<mlir::Value, double> &timeMap,
    std::vector<std::vector<llvm::Any>> &store,
    std::vector<mlir::Value> &scheduleList) {
  return tryToExecute(getOperation(), valueMap, timeMap, scheduleList, 0);
}

void handshake::ConditionalBranchOp::build(OpBuilder &builder,
                                           OperationState &result,
                                           Value condOperand,
                                           Value dataOperand) {

  auto type = dataOperand.getType();
  result.types.append(2, type);
  result.addOperands(condOperand);
  result.addOperands(dataOperand);

  // Branch is control-only if it is the no-data output of a ControlMerge or a
  // StartOp This holds because Branches are inserted before Forks
  auto *op = dataOperand.getDefiningOp();
  bool isControl = ((dyn_cast<ControlMergeOp>(op) || dyn_cast<StartOp>(op)) &&
                    dataOperand == op->getResult(0))
                       ? true
                       : false;
  result.addAttribute("control", builder.getBoolAttr(isControl));
}

bool handshake::ConditionalBranchOp::tryExecute(
    llvm::DenseMap<mlir::Value, llvm::Any> &valueMap,
    llvm::DenseMap<unsigned, unsigned> &memoryMap,
    llvm::DenseMap<mlir::Value, double> &timeMap,
    std::vector<std::vector<llvm::Any>> &store,
    std::vector<mlir::Value> &scheduleList) {
  auto op = getOperation();
  mlir::Value control = op->getOperand(0);
  if (valueMap.count(control) == 0)
    return false;
  auto controlValue = valueMap[control];
  auto controlTime = timeMap[control];
  mlir::Value in = op->getOperand(1);
  if (valueMap.count(in) == 0)
    return false;
  auto inValue = valueMap[in];
  auto inTime = timeMap[in];
  mlir::Value out = llvm::any_cast<APInt>(controlValue) != 0 ? op->getResult(0)
                                                             : op->getResult(1);
  double time = std::max(controlTime, inTime);
  valueMap[out] = inValue;
  timeMap[out] = time;
  scheduleList.push_back(out);

  // Consume the inputs.
  valueMap.erase(control);
  valueMap.erase(in);
  return true;
}

void StartOp::build(OpBuilder &builder, OperationState &result) {
  // Control-only output, has no type
  auto type = builder.getNoneType();
  result.types.push_back(type);
  result.addAttribute("control", builder.getBoolAttr(true));
}

bool handshake::StartOp::tryExecute(
    llvm::DenseMap<mlir::Value, llvm::Any> &valueMap,
    llvm::DenseMap<unsigned, unsigned> &memoryMap,
    llvm::DenseMap<mlir::Value, double> &timeMap,
    std::vector<std::vector<llvm::Any>> &store,
    std::vector<mlir::Value> &scheduleList) {
  return true;
}

void EndOp::build(OpBuilder &builder, OperationState &result, Value operand) {

  result.addOperands(operand);
}

bool handshake::EndOp::tryExecute(
    llvm::DenseMap<mlir::Value, llvm::Any> &valueMap,
    llvm::DenseMap<unsigned, unsigned> &memoryMap,
    llvm::DenseMap<mlir::Value, double> &timeMap,
    std::vector<std::vector<llvm::Any>> &store,
    std::vector<mlir::Value> &scheduleList) {
  return true;
}

void handshake::ReturnOp::build(OpBuilder &builder, OperationState &result,
                                ArrayRef<Value> operands) {

  result.addOperands(operands);
}

void SinkOp::build(OpBuilder &builder, OperationState &result, Value operand) {

  result.addOperands(operand);
}

bool handshake::SinkOp::tryExecute(
    llvm::DenseMap<mlir::Value, llvm::Any> &valueMap,
    llvm::DenseMap<unsigned, unsigned> &memoryMap,
    llvm::DenseMap<mlir::Value, double> &timeMap,
    std::vector<std::vector<llvm::Any>> &store,
    std::vector<mlir::Value> &scheduleList) {
  valueMap.erase(getOperand());
  return true;
}

void handshake::ConstantOp::build(OpBuilder &builder, OperationState &result,
                                  Attribute value, Value operand) {

  result.addOperands(operand);

  auto type = value.getType();
  result.types.push_back(type);

  result.addAttribute("value", value);
}

void handshake::ConstantOp::execute(std::vector<llvm::Any> &ins,
                                    std::vector<llvm::Any> &outs) {
  auto attr = (*this)->getAttrOfType<mlir::IntegerAttr>("value");
  outs[0] = attr.getValue();
}

bool handshake::ConstantOp::tryExecute(
    llvm::DenseMap<mlir::Value, llvm::Any> &valueMap,
    llvm::DenseMap<unsigned, unsigned> &memoryMap,
    llvm::DenseMap<mlir::Value, double> &timeMap,
    std::vector<std::vector<llvm::Any>> &store,
    std::vector<mlir::Value> &scheduleList) {
  return tryToExecute(getOperation(), valueMap, timeMap, scheduleList, 0);
}

void handshake::ConstantOp::getCanonicalizationPatterns(
    RewritePatternSet &results, MLIRContext *context) {
  results.insert<circt::handshake::EliminateSunkConstantsPattern>(context);
}

void handshake::TerminatorOp::build(OpBuilder &builder, OperationState &result,
                                    ArrayRef<Block *> successors) {
  // Add all the successor blocks of the block which contains this terminator
  result.addSuccessors(successors);
  // for (auto &succ : successors)
  //   result.addSuccessor(succ, {});
}

void MemoryOp::build(OpBuilder &builder, OperationState &result,
                     ArrayRef<Value> operands, int outputs, int control_outputs,
                     bool lsq, int id, Value memref) {

  result.addOperands(operands);

  auto memrefType = memref.getType().cast<MemRefType>();

  // Data outputs (get their type from memref)
  result.types.append(outputs, memrefType.getElementType());

  // Control outputs
  result.types.append(control_outputs, builder.getNoneType());

  // Indicates whether a memory is an LSQ
  result.addAttribute("lsq", builder.getBoolAttr(lsq));

  // Memref info
  result.addAttribute("type", TypeAttr::get(memrefType));

  // Memory ID (individual ID for each MemoryOp)
  Type i32Type = builder.getIntegerType(32);
  result.addAttribute("id", builder.getIntegerAttr(i32Type, id));

  if (!lsq) {

    result.addAttribute("ld_count", builder.getIntegerAttr(i32Type, outputs));
    result.addAttribute(
        "st_count", builder.getIntegerAttr(i32Type, control_outputs - outputs));
  }
}

bool handshake::MemoryOp::allocateMemory(
    llvm::DenseMap<unsigned, unsigned> &memoryMap,
    std::vector<std::vector<llvm::Any>> &store,
    std::vector<double> &storeTimes) {
  unsigned id = getID();
  if (memoryMap.count(id))
    return false;

  auto type = getMemRefType();
  std::vector<llvm::Any> in;

  ArrayRef<int64_t> shape = type.getShape();
  int allocationSize = 1;
  unsigned count = 0;
  for (int64_t dim : shape) {
    if (dim > 0)
      allocationSize *= dim;
    else {
      assert(count < in.size());
      allocationSize *= llvm::any_cast<APInt>(in[count++]).getSExtValue();
    }
  }
  unsigned ptr = store.size();
  store.resize(ptr + 1);
  storeTimes.resize(ptr + 1);
  store[ptr].resize(allocationSize);
  storeTimes[ptr] = 0.0;
  mlir::Type elementType = type.getElementType();
  int width = elementType.getIntOrFloatBitWidth();
  for (int i = 0; i < allocationSize; i++) {
    if (elementType.isa<mlir::IntegerType>()) {
      store[ptr][i] = APInt(width, 0);
    } else if (elementType.isa<mlir::FloatType>()) {
      store[ptr][i] = APFloat(0.0);
    } else {
      llvm_unreachable("Unknown result type!\n");
    }
  }

  std::ifstream file;
  file.open("datain" + std::to_string(id) + ".dat");
  if (file.is_open()) {
    llvm::errs() << "Loaded the initialized data for memory " << id << ".\n";
    int i = 0;
    while (!file.eof() && i < allocationSize) {
      int num;
      file >> num;
      store[ptr][i] = APInt(width, num);
      i++;
    }
  }

  memoryMap[id] = ptr;
  return true;
}

bool handshake::MemoryOp::tryExecute(
    llvm::DenseMap<mlir::Value, llvm::Any> &valueMap,
    llvm::DenseMap<unsigned, unsigned> &memoryMap,
    llvm::DenseMap<mlir::Value, double> &timeMap,
    std::vector<std::vector<llvm::Any>> &store,
    std::vector<mlir::Value> &scheduleList) {
  auto op = getOperation();
  int opIndex = 0;
  bool notReady = false;
  unsigned id = getID(); // The ID of this memory.
  unsigned buffer = memoryMap[id];

  for (unsigned i = 0; i < getStCount().getZExtValue(); i++) {
    mlir::Value data = op->getOperand(opIndex++);
    mlir::Value address = op->getOperand(opIndex++);
    mlir::Value nonceOut = op->getResult(getLdCount().getZExtValue() + i);
    if ((!valueMap.count(data) || !valueMap.count(address))) {
      notReady = true;
      continue;
    }
    auto addressValue = valueMap[address];
    auto addressTime = timeMap[address];
    auto dataValue = valueMap[data];
    auto dataTime = timeMap[data];

    assert(buffer < store.size());
    auto &ref = store[buffer];
    unsigned offset = llvm::any_cast<APInt>(addressValue).getZExtValue();
    assert(offset < ref.size());
    ref[offset] = dataValue;

    // Implicit none argument
    APInt apnonearg(1, 0);
    valueMap[nonceOut] = apnonearg;
    double time = std::max(addressTime, dataTime);
    timeMap[nonceOut] = time;
    scheduleList.push_back(nonceOut);
    // Consume the inputs.
    valueMap.erase(data);
    valueMap.erase(address);
  }

  for (unsigned i = 0; i < getLdCount().getZExtValue(); i++) {
    mlir::Value address = op->getOperand(opIndex++);
    mlir::Value dataOut = op->getResult(i);
    mlir::Value nonceOut = op->getResult(getLdCount().getZExtValue() +
                                         getStCount().getZExtValue() + i);
    if (!valueMap.count(address)) {
      notReady = true;
      continue;
    }
    auto addressValue = valueMap[address];
    auto addressTime = timeMap[address];
    assert(buffer < store.size());
    auto &ref = store[buffer];
    unsigned offset = llvm::any_cast<APInt>(addressValue).getZExtValue();
    assert(offset < ref.size());

    valueMap[dataOut] = ref[offset];
    timeMap[dataOut] = addressTime;
    // Implicit none argument
    APInt apnonearg(1, 0);
    valueMap[nonceOut] = apnonearg;
    timeMap[nonceOut] = addressTime;
    scheduleList.push_back(dataOut);
    scheduleList.push_back(nonceOut);
    // Consume the inputs.
    valueMap.erase(address);
  }
  return (notReady) ? false : true;
}

void handshake::LoadOp::build(OpBuilder &builder, OperationState &result,
                              Value memref, ArrayRef<Value> indices) {

  // Address indices
  // result.addOperands(memref);
  result.addOperands(indices);

  // Data type
  auto memrefType = memref.getType().cast<MemRefType>();

  // Data output (from load to successor ops)
  result.types.push_back(memrefType.getElementType());

  // Address outputs (to lsq)
  result.types.append(indices.size(), builder.getIndexType());
}

bool handshake::LoadOp::tryExecute(
    llvm::DenseMap<mlir::Value, llvm::Any> &valueMap,
    llvm::DenseMap<unsigned, unsigned> &memoryMap,
    llvm::DenseMap<mlir::Value, double> &timeMap,
    std::vector<std::vector<llvm::Any>> &store,
    std::vector<mlir::Value> &scheduleList) {
  auto op = getOperation();
  mlir::Value address = op->getOperand(0);
  mlir::Value data = op->getOperand(1);
  mlir::Value nonce = op->getOperand(2);
  mlir::Value addressOut = op->getResult(1);
  mlir::Value dataOut = op->getResult(0);
  if ((valueMap.count(address) && !valueMap.count(nonce)) ||
      (!valueMap.count(address) && valueMap.count(nonce)) ||
      (!valueMap.count(address) && !valueMap.count(nonce) &&
       !valueMap.count(data)))
    return false;
  if (valueMap.count(address) && valueMap.count(nonce)) {
    auto addressValue = valueMap[address];
    auto addressTime = timeMap[address];
    auto nonceValue = valueMap[nonce];
    auto nonceTime = timeMap[nonce];
    valueMap[addressOut] = addressValue;
    double time = std::max(addressTime, nonceTime);
    timeMap[addressOut] = time;
    scheduleList.push_back(addressOut);
    // Consume the inputs.
    valueMap.erase(address);
    valueMap.erase(nonce);
  } else if (valueMap.count(data)) {
    auto dataValue = valueMap[data];
    auto dataTime = timeMap[data];
    valueMap[dataOut] = dataValue;
    timeMap[dataOut] = dataTime;
    scheduleList.push_back(dataOut);
    // Consume the inputs.
    valueMap.erase(data);
  } else {
    llvm_unreachable("why?");
  }
  return true;
}

void handshake::StoreOp::build(OpBuilder &builder, OperationState &result,
                               Value valueToStore, ArrayRef<Value> indices) {

  // Data
  result.addOperands(valueToStore);

  // Address indices
  result.addOperands(indices);

  // Data output (from store to LSQ)
  result.types.push_back(valueToStore.getType());

  // Address outputs (from store to lsq)
  result.types.append(indices.size(), builder.getIndexType());
}

void handshake::StoreOp::execute(std::vector<llvm::Any> &ins,
                                 std::vector<llvm::Any> &outs) {
  // Forward the address and data to the memory op.
  outs[0] = ins[0];
  outs[1] = ins[1];
}

bool handshake::StoreOp::tryExecute(
    llvm::DenseMap<mlir::Value, llvm::Any> &valueMap,
    llvm::DenseMap<unsigned, unsigned> &memoryMap,
    llvm::DenseMap<mlir::Value, double> &timeMap,
    std::vector<std::vector<llvm::Any>> &store,
    std::vector<mlir::Value> &scheduleList) {
  return tryToExecute(getOperation(), valueMap, timeMap, scheduleList, 1);
}

void JoinOp::build(OpBuilder &builder, OperationState &result,
                   ArrayRef<Value> operands) {

  auto type = builder.getNoneType();
  result.types.push_back(type);

  result.addOperands(operands);

  result.addAttribute("control", builder.getBoolAttr(true));
}

void handshake::JoinOp::execute(std::vector<llvm::Any> &ins,
                                std::vector<llvm::Any> &outs) {
  outs[0] = ins[0];
}

bool handshake::JoinOp::tryExecute(
    llvm::DenseMap<mlir::Value, llvm::Any> &valueMap,
    llvm::DenseMap<unsigned, unsigned> &memoryMap,
    llvm::DenseMap<mlir::Value, double> &timeMap,
    std::vector<std::vector<llvm::Any>> &store,
    std::vector<mlir::Value> &scheduleList) {
  return tryToExecute(getOperation(), valueMap, timeMap, scheduleList, 1);
}

// for let printer/parser/verifier in Handshake_Op class
/*static LogicalResult verify(ForkOp op) {
  return success();
}
void print(OpAsmPrinter &p, ForkOp op) {
  p << "handshake.fork ";
  p.printOperands(op.getOperands());
 // p << " : " << op.getType();
}
ParseResult parseForkOp(OpAsmParser &parser, OperationState &result) {
  return success();
}*/

//===----------------------------------------------------------------------===//
// TableGen'd op method definitions
//===----------------------------------------------------------------------===//

// Code below is largely duplicated from Standard/Ops.cpp
static ParseResult parseReturnOp(OpAsmParser &parser, OperationState &result) {
  SmallVector<OpAsmParser::OperandType, 2> opInfo;
  SmallVector<Type, 2> types;
  llvm::SMLoc loc = parser.getCurrentLocation();
  return failure(parser.parseOperandList(opInfo) ||
                 (!opInfo.empty() && parser.parseColonTypeList(types)) ||
                 parser.resolveOperands(opInfo, types, loc, result.operands));
}

static void print(OpAsmPrinter &p, handshake::ReturnOp op) {
  p << "handshake.return";
  if (op.getNumOperands() != 0) {
    p << ' ';
    p.printOperands(op.getOperands());
    p << " : ";
    interleaveComma(op.getOperandTypes(), p);
  }
}

static LogicalResult verify(handshake::ReturnOp op) {
  auto *parent = op->getParentOp();
  auto function = dyn_cast<handshake::FuncOp>(parent);
  if (!function)
    return op.emitOpError("must have a handshake.func parent");

  // The operand number and types must match the function signature.
  const auto &results = function.getType().getResults();
  if (op.getNumOperands() != results.size())
    return op.emitOpError("has ")
           << op.getNumOperands()
           << " operands, but enclosing function returns " << results.size();

  for (unsigned i = 0, e = results.size(); i != e; ++i)
    if (op.getOperand(i).getType() != results[i])
      return op.emitError()
             << "type of return operand " << i << " ("
             << op.getOperand(i).getType()
             << ") doesn't match function result type (" << results[i] << ")";

  return success();
}

namespace circt {
namespace handshake {

#include "circt/Dialect/Handshake/HandshakeInterfaces.cpp.inc"

} // namespace handshake
} // namespace circt

#define GET_OP_CLASSES
#include "circt/Dialect/Handshake/HandshakeOps.cpp.inc"
