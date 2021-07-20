// RUN: circt-translate %s --export-verilog --verify-diagnostics --lowering-options=alwaysFF,wireInEventControl | FileCheck %s --strict-whitespace

// CHECK-LABEL: module AlwaysSpill(
hw.module @AlwaysSpill(%port: i1) {
  // CHECK: localparam [[FALSE:.+]] = 1'h0;
  // CHECK: localparam [[TRUE:.+]] = 1'h1;
  %false = hw.constant false
  %true = hw.constant true

  // Existing simple names should not cause additional spill.
  // CHECK: always @(posedge port)
  sv.always posedge %port {}
  // CHECK: always_ff @(posedge port)
  sv.alwaysff(posedge %port) {}

  // Constant values should cause a spill.
  // CHECK: wire [[TMP:.+]] = [[FALSE]];
  // CHECK=NEXT: always @(posedge [[TMP]])
  sv.always posedge %false {}
  // CHECK: wire [[TMP:.+]] = [[TRUE]];
  // CHECK=NEXT: always_ff @(posedge [[TMP]])
  sv.alwaysff(posedge %true) {}
}
