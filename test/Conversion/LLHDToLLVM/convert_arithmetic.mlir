// NOTE: Assertions have been autogenerated by utils/generate-test-checks.py
// RUN: circt-opt %s --convert-llhd-to-llvm | FileCheck %s

// CHECK-LABEL:   llvm.func @convert_neg(
// CHECK-SAME:                           %[[VAL_0:.*]]: !llvm.i1,
// CHECK-SAME:                           %[[VAL_1:.*]]: !llvm.i32) {
// CHECK:           %[[VAL_2:.*]] = llvm.mlir.constant(-1 : i32) : !llvm.i1
// CHECK:           %[[VAL_3:.*]] = llvm.mul %[[VAL_2]], %[[VAL_0]] : !llvm.i1
// CHECK:           %[[VAL_4:.*]] = llvm.mlir.constant(-1 : i32) : !llvm.i32
// CHECK:           %[[VAL_5:.*]] = llvm.mul %[[VAL_4]], %[[VAL_1]] : !llvm.i32
// CHECK:           llvm.return
// CHECK:         }
func @convert_neg(%i1 : i1, %i32 : i32) {
    %0 = llhd.neg %i1 : i1
    %1 = llhd.neg %i32 : i32

    return
}

// CHECK-LABEL:   llvm.func @convert_eq(
// CHECK-SAME:                          %[[VAL_0:.*]]: !llvm.i32,
// CHECK-SAME:                          %[[VAL_1:.*]]: !llvm.i32) -> !llvm.i1 {
// CHECK:           %[[VAL_2:.*]] = llvm.icmp "eq" %[[VAL_0]], %[[VAL_1]] : !llvm.i32
// CHECK:           llvm.return %[[VAL_2]] : !llvm.i1
// CHECK:         }
func @convert_eq(%lhs : i32, %rhs: i32) -> i1 {
    %0 = llhd.eq %lhs, %rhs: i32

    return %0 : i1
}


// CHECK-LABEL:   llvm.func @convert_neq(
// CHECK-SAME:                           %[[VAL_0:.*]]: !llvm.i32,
// CHECK-SAME:                           %[[VAL_1:.*]]: !llvm.i32) -> !llvm.i1 {
// CHECK:           %[[VAL_2:.*]] = llvm.icmp "ne" %[[VAL_0]], %[[VAL_1]] : !llvm.i32
// CHECK:           llvm.return %[[VAL_2]] : !llvm.i1
// CHECK:         }
func @convert_neq(%lhs : i32, %rhs: i32) -> i1 {
    %0 = llhd.neq %lhs, %rhs: i32

    return %0 : i1
}
