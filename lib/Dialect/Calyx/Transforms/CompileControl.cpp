//===- CompileControl.cpp - Compile Control Pass ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Contains the definitions of the Compile Control pass.
//
//===----------------------------------------------------------------------===//

#include "PassDetails.h"
#include "circt/Dialect/Calyx/CalyxOps.h"
#include "circt/Dialect/Calyx/CalyxPasses.h"
#include "circt/Support/LLVM.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/OperationSupport.h"

using namespace circt;
using namespace calyx;
using namespace mlir;

/// A helper function to create constants in the HW dialect.
static hw::ConstantOp createConstant(OpBuilder &builder, Location loc,
                                     size_t bitWidth, size_t value) {
  return builder.create<hw::ConstantOp>(
      loc, APInt(bitWidth, value, /*unsigned=*/true));
}

/// Given some number of states, returns the necessary bit width
/// TODO(Calyx): Probably a better built-in operation?
static size_t getNecessaryBitWidth(size_t numStates) {
  APInt apNumStates(64, numStates);
  size_t log2 = apNumStates.ceilLogBase2();
  return log2 > 1 ? log2 : 1;
}

/// Creates a RegisterOp, with input and output port bit widths defined by
/// `width`.
static RegisterOp createRegister(OpBuilder &builder, ComponentOp &component,
                                 size_t width, StringRef name) {
  auto *context = builder.getContext();
  builder.setInsertionPointToStart(component.getBody());
  return builder.create<RegisterOp>(component->getLoc(),
                                    StringAttr::get(context, name), width);
}

/// Generates a latency-insensitive FSM to realize a sequential operation. This
/// is done by initializing GroupGoOp values for the enabled groups in the
/// SeqOp, and then creating a new Seq GroupOp with the given FSM. Each step in
/// the FSM is guarded by the done operation of the group currently being
/// executed. After the group is complete, the FSM is incremented. This SeqOp is
/// then replaced in the control with an Enable statement referring to the new
/// Seq GroupOp.
static void visitSeqOp(SeqOp &seq, ComponentOp &component) {
  auto wires = component.getWiresOp();
  Block *wiresBody = wires.getBody();

  auto &seqOps = seq.getBody()->getOperations();
  assert(llvm::all_of(seqOps, [](auto &&op) { return isa<EnableOp>(op); }) &&
         "The SeqOp should only contain EnableOps in this pass.");
  // This should be the number of enable statements + 1 since this is the
  // maximum value the FSM register will reach.
  size_t fsmBitWidth = getNecessaryBitWidth(seqOps.size() + 1);

  OpBuilder builder(component->getRegion(0));
  auto fsmRegister = createRegister(builder, component, fsmBitWidth, "fsm");
  auto fsmIn = fsmRegister.getResult(0);
  auto fsmWriteEn = fsmRegister.getResult(1);
  auto fsmOut = fsmRegister.getResult(4);

  builder.setInsertionPointToStart(wiresBody);
  auto oneConstant = createConstant(builder, wires->getLoc(), 1, 1);

  // Create the new compilation group to replace this SeqOp.
  builder.setInsertionPointToEnd(wiresBody);
  auto seqGroup =
      builder.create<GroupOp>(wires->getLoc(), builder.getStringAttr("seq"));
  Block *seqGroupBody = new Block();
  seqGroup->getRegion(0).push_back(seqGroupBody);

  // Guarantees a unique SymbolName for the group.
  SymbolTable symTable(wires);
  symTable.insert(seqGroup);

  size_t fsmIndex = 0;
  SmallVector<Attribute, 8> groupNames;
  Value fsmNextState;
  seq.walk([&](EnableOp enable) {
    StringRef groupName = enable.groupName();
    groupNames.push_back(SymbolRefAttr::get(builder.getContext(), groupName));
    auto groupOp = cast<GroupOp>(symTable.lookup(groupName));

    builder.setInsertionPoint(groupOp);
    auto fsmCurrentState =
        createConstant(builder, wires->getLoc(), fsmBitWidth, fsmIndex);

    // TODO(Calyx): Eventually, we should canonicalize the GroupDoneOp's guard
    // and source.
    auto guard = groupOp.getDoneOp().guard();
    auto source = groupOp.getDoneOp().src();
    auto doneOpValue =
        !guard ? source
               : builder.create<comb::AndOp>(wires->getLoc(), guard, source);

    // Build the Guard for the `go` signal of the current group being walked.
    // The group should begin when:
    // (1) the current step in the fsm is reached, and
    // (2) the done signal of this group is not high.
    auto eqCmp = builder.create<comb::ICmpOp>(
        wires->getLoc(), comb::ICmpPredicate::eq, fsmOut, fsmCurrentState);
    auto notDone =
        builder.create<comb::XorOp>(wires->getLoc(), doneOpValue, oneConstant);
    auto groupGoGuard =
        builder.create<comb::AndOp>(wires->getLoc(), eqCmp, notDone);

    // Guard for the `in` and `write_en` signal of the fsm register. These are
    // driven when the group has completed.
    builder.setInsertionPoint(seqGroup);
    auto groupDoneGuard =
        builder.create<comb::AndOp>(wires->getLoc(), eqCmp, doneOpValue);

    // Directly update the GroupGoOp of the current group being walked.
    auto goOp = groupOp.getGoOp();
    assert(goOp && "The Go Insertion pass should be run before this.");
    goOp->setOperand(0, oneConstant);
    goOp->insertOperands(1, {groupGoGuard});

    // Add guarded assignments to the fsm register `in` and `write_en` ports.
    fsmNextState =
        createConstant(builder, wires->getLoc(), fsmBitWidth, fsmIndex + 1);
    builder.setInsertionPointToEnd(seqGroupBody);
    builder.create<AssignOp>(wires->getLoc(), fsmIn, fsmNextState,
                             groupDoneGuard);
    builder.create<AssignOp>(wires->getLoc(), fsmWriteEn, oneConstant,
                             groupDoneGuard);
    // Increment the fsm index for the next group.
    ++fsmIndex;
  });

  // Build the final guard for the new Seq group's GroupDoneOp. This is defined
  // by the fsm's final state.
  builder.setInsertionPoint(seqGroup);
  auto isFinalState = builder.create<comb::ICmpOp>(
      wires->getLoc(), comb::ICmpPredicate::eq, fsmOut, fsmNextState);

  // Insert the respective GroupDoneOp.
  builder.setInsertionPointToEnd(seqGroupBody);
  builder.create<GroupDoneOp>(seqGroup->getLoc(), oneConstant, isFinalState);

  // Add continuous wires to reset the `in` and `write_en` ports of the fsm
  // when the SeqGroup is finished executing.
  builder.setInsertionPointToEnd(wiresBody);
  auto zeroConstant = createConstant(builder, wires->getLoc(), fsmBitWidth, 0);
  builder.create<AssignOp>(wires->getLoc(), fsmIn, zeroConstant, isFinalState);
  builder.create<AssignOp>(wires->getLoc(), fsmWriteEn, oneConstant,
                           isFinalState);

  // Replace the SeqOp with an EnableOp.
  builder.setInsertionPoint(seq);
  auto newEnableOp =
      builder.create<EnableOp>(seq->getLoc(), seqGroup.sym_name());
  seq->erase();

  // Keep a list of compiled groups associated with the new EnableOp.
  newEnableOp->setAttr("groups",
                       ArrayAttr::get(builder.getContext(), groupNames));
}

namespace {

struct CompileControlPass : public CompileControlBase<CompileControlPass> {
  void runOnOperation() override;
};

} // end anonymous namespace

void CompileControlPass::runOnOperation() {
  ComponentOp component = getOperation();
  component.getControlOp().walk([&](SeqOp seq) { visitSeqOp(seq, component); });

  // A post-condition of this pass is that all undefined GroupGoOps, created in
  // the Go Insertion pass, are now defined.
  component.getWiresOp().walk([&](UndefinedOp op) { op->erase(); });
}

std::unique_ptr<mlir::Pass> circt::calyx::createCompileControlPass() {
  return std::make_unique<CompileControlPass>();
}
