// RUN: circt-opt -canonicalize %s | FileCheck %s

// CHECK: ^bb0(%[[ARG_3:.*]]: index, %[[ARG_4:.*]]: index, %[[ARG_5:.*]]: index):  // no predecessors
// CHECK:   %[[VAL_1:.*]] = addi %[[ARG_3:.*]], %[[ARG_4:.*]] : index
// CHECK:   staticlogic.register ^bb1(%[[ARG_5:.*]], %[[VAL_1:.*]] : index, index)
// CHECK: ^bb1(%[[VAL_2:.*]]: index, %[[VAL_3:.*]]: index):  // pred: ^bb0
// CHECK:   %[[VAL_4:.*]] = addi %[[VAL_3:.*]], %[[VAL_2:.*]] : index
// CHECK:   staticlogic.register ^bb2(%[[VAL_3:.*]], %[[VAL_4:.*]] : index, index)
// CHECK: ^bb2(%[[VAL_5:.*]]: index, %[[VAL_6:.*]]: index):  // pred: ^bb1
// CHECK:   %[[VAL_7:.*]] = addi %[[VAL_5:.*]], %[[VAL_6:.*]] : index
// CHECK:   "staticlogic.return"(%[[VAL_7:.*]]) : (index) -> ()

func @ops(%arg0: index, %arg1: index, %arg2: index) -> index {
  %0 = "staticlogic.pipeline"(%arg0, %arg1, %arg2) ( {
  ^bb0(%arg3: index, %arg4: index, %arg5: index):  // no predecessors
    %1 = addi %arg3, %arg4 : index
    staticlogic.register ^bb1
  ^bb1:  // pred: ^bb0
    %2 = addi %1, %arg5 : index
    staticlogic.register ^bb2
  ^bb2:  // pred: ^bb1
    %3 = addi %1, %2 : index
    "staticlogic.return"(%3) : (index) -> ()
  }) : (index, index, index) -> index
  return %0 : index
}
