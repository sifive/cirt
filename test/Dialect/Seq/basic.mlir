// RUN: circt-opt %s -verify-diagnostics | circt-opt -verify-diagnostics | FileCheck %s
// RUN: circt-opt %s -verify-diagnostics --lower-seq-to-sv | circt-opt -verify-diagnostics | FileCheck %s --check-prefix=SV
hw.module @top(%clk: i1, %rst: i1, %i: i32, %s: !hw.struct<foo: i32>) {
  %rv = hw.constant 0 : i32

  %r0 = seq.compreg %i, %clk, %rst, %rv : i32
  seq.compreg %i, %clk : i32
  // CHECK: %{{.+}} = seq.compreg %i, %clk, %rst, %c0_i32  : i32
  // CHECK: %{{.+}} = seq.compreg %i, %clk : i32
  // SV: [[REG0:%.+]] = sv.reg  : !hw.inout<i32>
  // SV: [[REG5:%.+]] = sv.read_inout [[REG0]] : !hw.inout<i32>
  // SV: sv.alwaysff(posedge %clk)  {
  // SV:   sv.passign [[REG0]], %i : i32
  // SV: }(syncreset : posedge %rst)  {
  // SV:   sv.passign [[REG0]], %c0_i32 : i32
  // SV: }
  // SV: [[REG1:%.+]] = sv.reg  : !hw.inout<i32>
  // SV: sv.alwaysff(posedge %clk)  {
  // SV:   sv.passign [[REG1]], %i : i32
  // SV: }

  %sv = hw.struct_create (%r0) : !hw.struct<foo: i32>

  seq.compreg %s, %clk, %rst, %sv : !hw.struct<foo: i32>
  seq.compreg %s, %clk : !hw.struct<foo: i32>
  // CHECK: %{{.+}} = seq.compreg %s, %clk, %rst, %{{.+}} : !hw.struct<foo: i32>
  // CHECK: %{{.+}} = seq.compreg %s, %clk : !hw.struct<foo: i32>

  // SV: [[REGST:%.+]] = hw.struct_create ([[REG5]]) : !hw.struct<foo: i32>
  // SV: [[REG3:%.+]] = sv.reg  : !hw.inout<struct<foo: i32>>
  // SV: sv.alwaysff(posedge %clk)  {
  // SV:   sv.passign [[REG3]], %s : !hw.struct<foo: i32>
  // SV: }(syncreset : posedge %rst)  {
  // SV:   sv.passign [[REG3]], [[REGST]] : !hw.struct<foo: i32>
  // SV: }
  // SV: [[REG4:%.+]] = sv.reg  : !hw.inout<struct<foo: i32>>
  // SV: sv.alwaysff(posedge %clk)  {
  // SV:   sv.passign [[REG4]], %s : !hw.struct<foo: i32>
  // SV: }
}

hw.module @testRegWithPosedgeSyncReset(%clk: i1, %d: !hw.struct<foo:i32>, %reset: i1, %resetValue: !hw.struct<foo:i32>) -> (%o : !hw.struct<foo:i32>) {
  // SV-LABEL: hw.module @testRegWithPosedgeSyncReset
  // SV-NEXT:    %true = hw.constant true
  // SV-NEXT:    [[REG:%.+]] = sv.reg : !hw.inout<struct<foo: i32>>
  // SV-NEXT:    [[OUTPUT:%.+]] = sv.read_inout %0 : !hw.inout<struct<foo: i32>>
  // SV-NEXT:    sv.alwaysff(posedge %clk) {
  // SV-NEXT:      sv.passign [[REG]], %d : !hw.struct<foo: i32>
  // SV-NEXT:    }(syncreset : posedge %reset) {
  // SV-NEXT:      sv.passign [[REG]], %resetValue : !hw.struct<foo: i32>
  // SV-NEXT:    }
  // SV-NEXT:    hw.output [[OUTPUT]] : !hw.struct<foo: i32>
  %vdd = hw.constant true
  %r0  = seq.reg %d, posedge %clk, enable %vdd, syncreset posedge %reset, %resetValue : !hw.struct<foo:i32>  : !hw.struct<foo:i32>
  hw.output %r0 : !hw.struct<foo:i32>
}

hw.module @testRegWithPosedgeAsyncReset(%clk: i1, %d: !hw.struct<foo:i32>, %reset: i1, %resetValue: !hw.struct<foo:i32>) -> (%o : !hw.struct<foo:i32>) {
  // SV-LABEL: hw.module @testRegWithPosedgeAsyncReset
  // SV-NEXT:    %true = hw.constant true
  // SV-NEXT:    [[REG:%.+]] = sv.reg : !hw.inout<struct<foo: i32>>
  // SV-NEXT:    [[OUTPUT:%.+]] = sv.read_inout %0 : !hw.inout<struct<foo: i32>>
  // SV-NEXT:    sv.alwaysff(posedge %clk) {
  // SV-NEXT:      sv.passign [[REG]], %d : !hw.struct<foo: i32>
  // SV-NEXT:    }(asyncreset : posedge %reset) {
  // SV-NEXT:      sv.passign [[REG]], %resetValue : !hw.struct<foo: i32>
  // SV-NEXT:    }
  // SV-NEXT:    hw.output [[OUTPUT]] : !hw.struct<foo: i32>
  %vdd = hw.constant true
  %r0  = seq.reg %d, posedge %clk, enable %vdd, asyncreset posedge %reset, %resetValue : !hw.struct<foo:i32>  : !hw.struct<foo:i32>
  hw.output %r0 : !hw.struct<foo:i32>
}

hw.module @testRegWithEnable(%clk: i1, %d: !hw.struct<foo:i32>, %enable: i1) -> (%o : !hw.struct<foo:i32>) {
  // SV-LABEL: hw.module @testRegWithEnable
  // SV: [[REG:%.+]] = sv.reg : !hw.inout<struct<foo: i32>>
  // SV: [[OUTPUT:%.+]] = sv.read_inout [[REG]] : !hw.inout<struct<foo: i32>>
  // SV: sv.alwaysff(posedge %clk) {
  // SV:   sv.if %enable {
  // SV:     sv.passign [[REG]], %d : !hw.struct<foo: i32>
  // SV:   }
  // SV: }
  // SV: hw.output [[OUTPUT]] : !hw.struct<foo: i32>
  %o = seq.reg %d, posedge %clk, enable %enable, noreset : !hw.struct<foo:i32>
  hw.output %o : !hw.struct<foo:i32>
}

hw.module @testRegWithEnableAndNegedgeSyncReset(%clk: i1, %d: !hw.struct<foo:i32>, %enable: i1, %reset: i1, %resetValue : !hw.struct<foo:i32>) -> (%o : !hw.struct<foo:i32>) {
  // SV-LABEL: hw.module @testRegWithEnableAndNegedgeSyncReset
  // SV-NEXT:    [[REG:%.+]] = sv.reg : !hw.inout<struct<foo: i32>>
  // SV-NEXT:    [[OUTPUT:%.+]] = sv.read_inout [[REG]] : !hw.inout<struct<foo: i32>>
  // SV-NEXT:    sv.alwaysff(posedge %clk) {
  // SV-NEXT:      sv.if %enable {
  // SV-NEXT:        sv.passign [[REG]], %d : !hw.struct<foo: i32>
  // SV-NEXT:      }
  // SV-NEXT:    }(syncreset : negedge %reset) {
  // SV-NEXT:      sv.passign [[REG]], %resetValue : !hw.struct<foo: i32>
  // SV-NEXT:    }
  // SV-NEXT:    hw.output [[OUTPUT]] : !hw.struct<foo: i32>
  %o = seq.reg %d, posedge %clk, enable %enable, syncreset negedge %reset, %resetValue : !hw.struct<foo:i32> : !hw.struct<foo:i32>
  hw.output %o : !hw.struct<foo:i32>
}
