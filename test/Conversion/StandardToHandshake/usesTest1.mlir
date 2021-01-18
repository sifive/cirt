// RUN: circt-opt -handshake-insert-fork-sink %s | FileCheck %s
//CHECK-LABEL:  handshake.func @simple_loop(%arg0: i32, %arg1: i32, %arg2: i32, %arg3: none, ...) -> (index, none) {
//CHECK:    %0 = "handshake.branch"(%arg0) {control = true} : (i32) -> i32
//CHECK:    %1 = index_cast %7#1 : i32 to index
//CHECK:    %2:2 = "handshake.control_merge"(%7#0) {control = true} : (i32) -> (i32, index)
//CHECK:    %3 = "handshake.constant"(%6#1) {value = 1 : index} : (none) -> index
//CHECK:    %4 = addi %1, %2#1 : index
//CHECK:    %5 = addi %8#1, %8#0 : index
//CHECK:    "handshake.sink"(%arg1) : (i32) -> ()
//CHECK:    "handshake.sink"(%arg2) : (i32) -> ()
//CHECK:    %6:2 = "handshake.fork"(%arg3) {control = false} : (none) -> (none, none)
//CHECK:    %7:2 = "handshake.fork"(%0) {control = false} : (i32) -> (i32, i32)
//CHECK:    "handshake.sink"(%2#0) : (i32) -> ()
//CHECK:    "handshake.sink"(%3) : (index) -> ()
//CHECK:    %8:2 = "handshake.fork"(%4) {control = false} : (index) -> (index, index)
//CHECK:    handshake.return %5, %6#0 : index, none
//CHECK:  }

module {
  handshake.func @simple_loop(%arg0: i32, %arg1: i32, %arg2: i32, %arg3: none, ...) -> (index, none) {
    %0 = "handshake.branch"(%arg0) {control = true} : (i32) -> i32
    %5 = "std.index_cast"(%0) : (i32) -> index
    %1:2 = "handshake.control_merge"(%0) {control = true} : (i32) -> (i32, index)
    %2 = "handshake.constant"(%arg3) {value = 1 : index} : (none) -> index
    %3 = addi %5, %1#1 : index
    %4 = addi %3, %3 : index
    handshake.return %4, %arg3 : index, none
  }
}
