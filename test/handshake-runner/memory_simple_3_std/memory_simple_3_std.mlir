// RUN: handshake-runner %s 1,0,0,0 | FileCheck %s
// BROKEN: circt-opt -create-dataflow -canonicalize -clean-memref-args %s | handshake-runner | FileCheck %s
// CHECK: 6 

module {
  func @main(%0: memref<4xi32>) -> i32{
    %c0 = constant 0 : index
    %c5 = constant 5 : i32
    %1 = load %0[%c0] : memref<4xi32>
    store %c5, %0[%c0] : memref<4xi32>
    %2 = load %0[%c0] : memref<4xi32>
    %3 = addi %1, %2 : i32
    return %3 : i32
    }

}
