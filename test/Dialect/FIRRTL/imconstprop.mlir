// RUN: circt-opt -pass-pipeline='firrtl.circuit(firrtl-imconstprop)' %s | FileCheck %s

firrtl.circuit "Test" {

  // CHECK-LABEL: @PassThrough
  // CHECK: (in %source: !firrtl.uint<1>, out %dest: !firrtl.uint<1>)
  firrtl.module @PassThrough(in %source: !firrtl.uint<1>, out %dest: !firrtl.uint<1>) {
    // CHECK-NEXT: %c0_ui1 = firrtl.constant 0 : !firrtl.uint<1>
    // CHECK-NEXT: firrtl.connect %dest, %c0_ui1
    firrtl.connect %dest, %source : !firrtl.uint<1>, !firrtl.uint<1>
    // CHECK-NEXT: }
  }

  // CHECK-LABEL: @Test
  firrtl.module @Test(in %clock: !firrtl.clock, in %reset: !firrtl.uint<1>,
                      out %result1: !firrtl.uint<1>,
                      out %result2: !firrtl.uint<1>,
                      out %result3: !firrtl.uint<1>,
                      out %result4: !firrtl.uint<2>,
                      out %result5: !firrtl.uint<2>,
                      out %result6: !firrtl.uint<4>,
                      out %result7: !firrtl.uint<4>,
                      out %result8: !firrtl.uint<4>) {
    %c0_ui1 = firrtl.constant 0 : !firrtl.uint<1>
    %c1_ui1 = firrtl.constant 1 : !firrtl.uint<1>

    // Trivial wire constant propagation.
    %someWire = firrtl.wire : !firrtl.uint<1>
    firrtl.connect %someWire, %c0_ui1 : !firrtl.uint<1>, !firrtl.uint<1>

    // CHECK-NOT: firrtl.wire
    // CHECK: firrtl.connect %result1, %c0_ui1_0
    firrtl.connect %result1, %someWire : !firrtl.uint<1>, !firrtl.uint<1>

    // Not a constant.
    %nonconstWire = firrtl.wire : !firrtl.uint<1>
    firrtl.connect %nonconstWire, %c0_ui1 : !firrtl.uint<1>, !firrtl.uint<1>
    firrtl.connect %nonconstWire, %c1_ui1 : !firrtl.uint<1>, !firrtl.uint<1>

    // CHECK: firrtl.connect %result2, %nonconstWire
    firrtl.connect %result2, %nonconstWire : !firrtl.uint<1>, !firrtl.uint<1>


    // Constant propagation through instance.
    %source, %dest = firrtl.instance @PassThrough {name = "", portNames = ["source", "dest"]} : !firrtl.flip<uint<1>>, !firrtl.uint<1>

    // CHECK: firrtl.connect %inst_source, %c0_ui1
    firrtl.connect %source, %c0_ui1 : !firrtl.flip<uint<1>>, !firrtl.uint<1>
    // CHECK: firrtl.connect %result3, %c0_ui1_1
    firrtl.connect %result3, %dest : !firrtl.uint<1>, !firrtl.uint<1>

    // Check connect extensions.
    %extWire = firrtl.wire : !firrtl.uint<2>
    firrtl.connect %extWire, %c0_ui1 : !firrtl.uint<2>, !firrtl.uint<1>

    // Connects of invalid values shouldn't hurt.
    %invalid = firrtl.invalidvalue : !firrtl.uint<2>
    firrtl.connect %extWire, %invalid : !firrtl.uint<2>, !firrtl.uint<2>

    // CHECK: firrtl.connect %result4, %c0_ui2
    firrtl.connect %result4, %extWire: !firrtl.uint<2>, !firrtl.uint<2>

    // regreset
    %c0_ui20 = firrtl.constant 0 : !firrtl.uint<20>
    %regreset = firrtl.regreset %clock, %reset, %c0_ui20  : (!firrtl.clock, !firrtl.uint<1>, !firrtl.uint<20>) -> !firrtl.uint<2>

    %c0_ui2 = firrtl.constant 0 : !firrtl.uint<2>
    firrtl.connect %regreset, %c0_ui2 : !firrtl.uint<2>, !firrtl.uint<2>

    // CHECK: firrtl.connect %result5, %c0_ui2
    firrtl.connect %result5, %regreset: !firrtl.uint<2>, !firrtl.uint<2>

    // reg
    %reg = firrtl.reg %clock  : (!firrtl.clock) -> !firrtl.uint<4>
    firrtl.connect %reg, %c0_ui2 : !firrtl.uint<4>, !firrtl.uint<2>
    // CHECK: firrtl.connect %result6, %c0_ui4
    firrtl.connect %result6, %reg: !firrtl.uint<4>, !firrtl.uint<4>

    // Wire without connects to it should turn into 'invalid'.
    %unconnectedWire = firrtl.wire : !firrtl.uint<2>
    // CHECK: firrtl.connect %result7, %invalid_ui2
    firrtl.connect %result7, %unconnectedWire: !firrtl.uint<4>, !firrtl.uint<2>

    // CHECK-NEXT: firrtl.constant 1
    %c1_ui2 = firrtl.constant 1 : !firrtl.uint<2>
    // CHECK-NEXT: firrtl.constant 2
    %c2_ui2 = firrtl.constant 2 : !firrtl.uint<2>

    // Multiple operations that fold to constants shouldn't leave dead constants
    // around.
    // CHECK-NEXT: firrtl.constant 0
    %a = firrtl.and %extWire, %c2_ui2 : (!firrtl.uint<2>, !firrtl.uint<2>) -> !firrtl.uint<2>
    // CHECK-NEXT: firrtl.constant 1
    %b = firrtl.or %a, %c1_ui2 : (!firrtl.uint<2>, !firrtl.uint<2>) -> !firrtl.uint<2>
    // CHECK-NEXT: firrtl.constant 3
    %c = firrtl.xor %b, %c2_ui2 : (!firrtl.uint<2>, !firrtl.uint<2>) -> !firrtl.uint<2>

    // CHECK-NEXT: firrtl.connect %result8, %c3_ui2
    firrtl.connect %result8, %c: !firrtl.uint<4>, !firrtl.uint<2>


    // Constant propagation through instance.
    firrtl.instance @ReadMem {name = "ReadMem"}
  }

  // Unused modules should be completely dropped.

  // CHECK-LABEL: @UnusedModule(in %source: !firrtl.uint<1>, out %dest: !firrtl.uint<1>)
  firrtl.module @UnusedModule(in %source: !firrtl.uint<1>, out %dest: !firrtl.uint<1>) {
    firrtl.connect %dest, %source : !firrtl.uint<1>, !firrtl.uint<1>
    // CHECK-NEXT: }
  }


  // CHECK-LABEL: ReadMem
  firrtl.module @ReadMem() {
    %c0_ui1 = firrtl.constant 0 : !firrtl.uint<1>
    %c1_ui1 = firrtl.constant 1 : !firrtl.uint<1>

    %0 = firrtl.mem Undefined {depth = 16 : i64, name = "ReadMemory", portNames = ["read0"], readLatency = 1 : i32, writeLatency = 1 : i32} : !firrtl.flip<bundle<addr: uint<4>, en: uint<1>, clk: clock, data: flip<sint<8>>>>

    %1 = firrtl.subfield %0("data") : (!firrtl.flip<bundle<addr: uint<4>, en: uint<1>, clk: clock, data: flip<sint<8>>>>) -> !firrtl.sint<8>
    %2 = firrtl.subfield %0("addr") : (!firrtl.flip<bundle<addr: uint<4>, en: uint<1>, clk: clock, data: flip<sint<8>>>>) -> !firrtl.uint<4>
    firrtl.connect %2, %c0_ui1 : !firrtl.uint<4>, !firrtl.uint<1>
    %3 = firrtl.subfield %0("en") : (!firrtl.flip<bundle<addr: uint<4>, en: uint<1>, clk: clock, data: flip<sint<8>>>>) -> !firrtl.uint<1>
    firrtl.connect %3, %c1_ui1 : !firrtl.uint<1>, !firrtl.uint<1>
    %4 = firrtl.subfield %0("clk") : (!firrtl.flip<bundle<addr: uint<4>, en: uint<1>, clk: clock, data: flip<sint<8>>>>) -> !firrtl.clock
  }
}
