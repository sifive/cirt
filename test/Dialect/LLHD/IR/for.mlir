// RUN: circt-opt %s -mlir-print-op-generic -split-input-file -verify-diagnostics | circt-opt | circt-opt | FileCheck %s

// CHECK-LABEL: @check_simple_i32_for
func @check_simple_i32_for() {
  // CHECK: %[[VAL_0:.*]] = llhd.const 0 : i32
  %zero = llhd.const 0 : i32
  // CHECK: %[[VAL_1:.*]] = llhd.const 1 : i32
  %one = llhd.const 1 : i32
  // CHECK-NEXT: llhd.for (%[[VAL_2:.*]] = %[[VAL_0]] : i32) to %[[VAL_1]] step %[[VAL_1]] {
  // CHECK-NEXT:   llhd.yield
  // CHECK-NEXT: }
  llhd.for (%i = %zero : i32) to %one step %one {
    llhd.yield
  }
  return
}

// CHECK-LABEL: @check_simple_i8_for
func @check_simple_i8_for() {
  // CHECK-NEXT: %[[VAL_0:.*]] = llhd.const 0 : i8
  %zero = llhd.const 0 : i8
  // CHECK-NEXT: %[[VAL_1:.*]] = llhd.const 1 : i8
  %one = llhd.const 1 : i8
  // CHECK-NEXT: llhd.for (%[[VAL_2:.*]] = %[[VAL_0]] : i8) to %[[VAL_1]] step %[[VAL_1]] {
  // CHECK-NEXT:   llhd.yield
  // CHECK-NEXT: }
  llhd.for (%i = %zero : i8) to %one step %one {
    llhd.yield
  }
  return
}

// CHECK-LABEL: @check_carried_for
// CHECK-SAME: %[[VAL_0:.*]]: i32,
// CHECK-SAME: %[[VAL_1:.*]]: !llhd.array<3xi1>,
// CHECK-SAME: %[[VAL_2:.*]]: tuple<i1, i2, i3>
func @check_carried_for(%a : i32, %b : !llhd.array<3xi1>, %c : tuple<i1, i2, i3>) {
  // CHECK-NEXT: %[[VAL_3:.*]] = llhd.const 0 : i32
  %zero = llhd.const 0 : i32
  // CHECK-NEXT: %[[VAL_4:.*]] = llhd.const 1 : i32
  %one = llhd.const 1 : i32
  // CHECK-NEXT: %{{.*}}:3 = llhd.for (%[[VAL_6:.*]] = %[[VAL_3]] : i32) to %[[VAL_4]] step %[[VAL_4]] iter_args(%[[VAL_7:.*]] = %[[VAL_0]], %[[VAL_8:.*]] = %[[VAL_1]], %[[VAL_9:.*]] = %[[VAL_2]]) -> (i32, !llhd.array<3xi1>, tuple<i1, i2, i3>) {
  // CHECK-NEXT:   %[[VAL_10:.*]] = llhd.and %[[VAL_7]], %[[VAL_6]] : i32
  // CHECK-NEXT:   llhd.yield %[[VAL_10]], %[[VAL_8]], %[[VAL_9]] : i32, !llhd.array<3xi1>, tuple<i1, i2, i3>
  // CHECK-NEXT: }
  %res:3 = llhd.for (%i = %zero : i32) to %one step %one iter_args(%an = %a, %bn = %b, %cn = %c) -> (i32, !llhd.array<3xi1>, tuple<i1, i2, i3>) {
    %0 = llhd.and %an, %i : i32
    llhd.yield %0, %bn, %cn: i32, !llhd.array<3xi1>, tuple<i1, i2, i3>
  }
  return
}

// CHECK-LABEL: @check_bb_for
func @check_bb_for() {
  // CHECK-NEXT: %[[VAL_0:.*]] = llhd.const 0 : i8
  %zero = llhd.const 0 : i8
  // CHECK-NEXT: %[[VAL_1:.*]] = llhd.const 1 : i8
  %one = llhd.const 1 : i8
  // CHECK-NEXT: llhd.for (%[[VAL_2:.*]] = %[[VAL_0]] : i8) to %[[VAL_1]] step %[[VAL_1]] {
  // CHECK-NEXT:   %[[VAL_3:.*]] = llhd.eq %[[VAL_2]], %[[VAL_0]] : i8
  // CHECK-NEXT:   cond_br %[[VAL_3]], ^bb1, ^bb2
  // CHECK-NEXT: ^bb1:
  // CHECK-NEXT:   llhd.yield
  // CHECK-NEXT: ^bb2:
  // CHECK-NEXT:   llhd.yield
  // CHECK-NEXT: }
  llhd.for (%i = %zero : i8) to %one step %one {
    %cond = llhd.eq %i, %zero : i8
    cond_br %cond, ^bb1, ^bb2
  ^bb1:
    llhd.yield
  ^bb2:
    llhd.yield
  }
  return
}

// CHECK-LABEL: @check_nested_for
func @check_nested_for() {
  // CHECK-NEXT: %[[VAL_0:.*]] = llhd.const 0 : i8
  %zero = llhd.const 0 : i8
  // CHECK-NEXT: %[[VAL_1:.*]] = llhd.const 1 : i8
  %one = llhd.const 1 : i8
  // CHECK-NEXT: llhd.for (%[[VAL_2:.*]] = %[[VAL_0]] : i8) to %[[VAL_1]] step %[[VAL_1]] {
  // CHECK-NEXT:   %[[VAL_3:.*]] = llhd.eq %[[VAL_2]], %[[VAL_0]] : i8
  // CHECK-NEXT:   cond_br %[[VAL_3]], ^bb1, ^bb2
  // CHECK-NEXT: ^bb1:
  // CHECK-NEXT:   llhd.for (%[[VAL_4:.*]] = %[[VAL_2]] : i8) to %[[VAL_1]] step %[[VAL_1]] {
  // CHECK-NEXT:     br ^bb1
  // CHECK-NEXT:   ^bb1:
  // CHECK-NEXT:     llhd.yield
  // CHECK-NEXT:   }
  // CHECK-NEXT:   llhd.yield
  // CHECK-NEXT: ^bb2:
  // CHECK-NEXT:   llhd.yield
  // CHECK-NEXT: }
  llhd.for (%i = %zero : i8) to %one step %one {
    %cond = llhd.eq %i, %zero : i8
    cond_br %cond, ^bb1, ^bb2
  ^bb1:
    llhd.for (%j = %i : i8) to %one step %one {
      br ^bb1
    ^bb1:
      llhd.yield
    }
    llhd.yield
  ^bb2:
    llhd.yield
  }
  return
}

// -----

func @illegal_wait_for() {
  %zero = llhd.const 0 : i8
  %one = llhd.const 1 : i8
  llhd.for (%i = %zero : i8) to %one step %one {
    br ^bb1
  ^bb1:
    // expected-error @+1 {{'llhd.wait' op expects parent op 'llhd.proc'}}
    llhd.wait ^bb2
  ^bb2:
    llhd.yield
  }
  return
}

// -----

func @illegal_halt_for() {
  %zero = llhd.const 0 : i8
  %one = llhd.const 1 : i8
  llhd.for (%i = %zero : i8) to %one step %one {
    %cond = llhd.eq %i, %zero : i8
    cond_br %cond, ^bb1, ^bb2
  ^bb1:
    // expected-error @+1 {{'llhd.halt' op expects parent op 'llhd.proc'}}
    llhd.halt
  ^bb2:
    llhd.yield
  }
  return
}

// -----

func @illegal_empty_for() {
  %zero = llhd.const 0 : i8
  %one = llhd.const 1 : i8
  // expected-error @+1 {{every block in the loop body must have a terminator}}
  llhd.for (%i = %zero : i8) to %one step %one {
  }
  return
}

// -----

func @illegal_cyclic_for() {
  %zero = llhd.const 0 : i8
  %one = llhd.const 1 : i8
  // expected-error @+1 {{CFG of loop body must be a DAG}}
  llhd.for (%i = %zero : i8) to %one step %one {
    br ^bb1
  ^bb1:
    br ^bb1
  }
  return
}

// -----

func @yield_number_of_results_mismatch(%a : i32) {
  %zero = llhd.const 0 : i32
  %one = llhd.const 1 : i32
  %res = llhd.for (%i = %zero : i32) to %one step %one iter_args(%an = %a) -> (i32) {
    // expected-error @+1 {{number of results do not match number of parent ForOp results}}
    llhd.yield %an, %an: i32, i32
  }
  return
}

// -----

func @yield_result_type_mismatch(%a : i32) {
  %zero = llhd.const 0 : i32
  %one = llhd.const 1 : i32
  %res = llhd.for (%i = %zero : i32) to %one step %one iter_args(%an = %a) -> (i32) {
    %0 = llhd.const 0 : i8
    // expected-error @+1 {{result types do not match parent ForOp result types.}}
    llhd.yield %0: i8
  }
  return
}
