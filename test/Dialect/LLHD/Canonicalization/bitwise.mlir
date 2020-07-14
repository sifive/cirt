// RUN: circt-opt -canonicalize %s | FileCheck %s

// CHECK-LABEL: @check_not_folding
// CHECK-SAME: %[[A:.*]]: i64
func @check_not_folding(%a : i64) -> (i64, i64) {
  %c0 = llhd.const 0 : i64
  // CHECK-NEXT: %[[CN1:.*]] = llhd.const -1 : i64
  %cn1 = llhd.const -1 : i64
  %0 = llhd.not %c0 : i64
  %na = llhd.not %a : i64
  %1 = llhd.not %na : i64

  // CHECK-NEXT: return %[[CN1]], %[[A]] : i64, i64
  return %0, %1 : i64, i64
}

// CHECK-LABEL: @check_and_folding
// CHECK-SAME: %[[A:.*]]: i64,
// CHECK-SAME: %[[B:.*]]: i64
func @check_and_folding(%a : i64, %b : i64) -> (i64, i64, i64, i64, i64, i64, i64) {
  // CHECK-NEXT: %[[C0:.*]] = llhd.const 0 : i64
  %c0 = llhd.const 0 : i64
  %cn1 = llhd.const -1 : i64
  %na = llhd.not %a : i64
  // CHECK-NEXT: %[[NB:.*]] = llhd.not %[[B]] : i64
  %nb = llhd.not %b : i64
  %0 = llhd.and %a, %a : i64
  %1 = llhd.and %c0, %a : i64
  %2 = llhd.and %cn1, %a : i64
  %3 = llhd.and %na, %a : i64
  %4 = llhd.and %a, %na : i64
  // CHECK-NEXT: %[[AND1:.*]] = llhd.and %[[NB]], %[[A]] : i64
  %5 = llhd.and %nb, %a : i64
  // CHECK-NEXT: %[[AND2:.*]] = llhd.and %[[A]], %[[NB]] : i64
  %6 = llhd.and %a, %nb : i64

  // CHECK-NEXT: return %[[A]], %[[C0]], %[[A]], %[[C0]], %[[C0]], %[[AND1]], %[[AND2]] : i64, i64, i64, i64, i64, i64, i64
  return %0, %1, %2, %3, %4, %5, %6 : i64, i64, i64, i64, i64, i64, i64
}

// CHECK-LABEL: @check_or_folding
// CHECK-SAME: %[[A:.*]]: i64,
// CHECK-SAME: %[[B:.*]]: i64
func @check_or_folding(%a : i64, %b : i64) -> (i64, i64, i64, i64, i64, i64, i64) {
  %c0 = llhd.const 0 : i64
  // CHECK-NEXT: %[[CN1:.*]] = llhd.const -1 : i64
  %cn1 = llhd.const -1 : i64
  %na = llhd.not %a : i64
  // CHECK-NEXT: %[[NB:.*]] = llhd.not %[[B]] : i64
  %nb = llhd.not %b : i64
  %0 = llhd.or %a, %a : i64
  %1 = llhd.or %c0, %a : i64
  %2 = llhd.or %cn1, %a : i64
  %3 = llhd.or %na, %a : i64
  %4 = llhd.or %a, %na : i64
  // CHECK-NEXT: %[[OR1:.*]] = llhd.or %[[NB]], %[[A]] : i64
  %5 = llhd.or %nb, %a : i64
  // CHECK-NEXT: %[[OR2:.*]] = llhd.or %[[A]], %[[NB]] : i64
  %6 = llhd.or %a, %nb : i64

  // CHECK-NEXT: return %[[A]], %[[A]], %[[CN1]], %[[CN1]], %[[CN1]], %[[OR1]], %[[OR2]] : i64, i64, i64, i64, i64, i64, i64
  return %0, %1, %2, %3, %4, %5, %6 : i64, i64, i64, i64, i64, i64, i64
}

// CHECK-LABEL: @check_xor_folding
// CHECK-SAME: %[[A:.*]]: i64,
// CHECK-SAME: %[[B:.*]]: i64
func @check_xor_folding(%a : i64, %b : i64) -> (i64, i64, i64, i64, i64, i64, i64) {
  // CHECK-NEXT: %[[C0:.*]] = llhd.const 0 : i64
  %c0 = llhd.const 0 : i64
  // CHECK-NEXT: %[[CN1:.*]] = llhd.const -1 : i64
  %cn1 = llhd.const -1 : i64
  %na = llhd.not %a : i64
  // CHECK-NEXT: %[[NB:.*]] = llhd.not %[[B]] : i64
  %nb = llhd.not %b : i64
  %0 = llhd.xor %a, %a : i64
  %1 = llhd.xor %c0, %a : i64
  // CHECK-NEXT: %[[NA:.*]] = llhd.not %[[A]] : i64
  %2 = llhd.xor %cn1, %a : i64
  %3 = llhd.xor %na, %a : i64
  %4 = llhd.xor %a, %na : i64
  // CHECK-NEXT: %[[XOR1:.*]] = llhd.xor %[[NB]], %[[A]] : i64
  %5 = llhd.xor %nb, %a : i64
  // CHECK-NEXT: %[[XOR2:.*]] = llhd.xor %[[A]], %[[NB]] : i64
  %6 = llhd.xor %a, %nb : i64

  // CHECK-NEXT: return %[[C0]], %[[A]], %[[NA]], %[[CN1]], %[[CN1]], %[[XOR1]], %[[XOR2]] : i64, i64, i64, i64, i64, i64, i64
  return %0, %1, %2, %3, %4, %5, %6 : i64, i64, i64, i64, i64, i64, i64
}
