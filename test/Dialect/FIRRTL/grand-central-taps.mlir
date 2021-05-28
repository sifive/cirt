// RUN: firtool %s --sifive-grand-central --verilog | FileCheck %s

firrtl.circuit "TestHarness" attributes {
  annotations = [{
    class = "sifive.enterprise.grandcentral.ExtractGrandCentralAnnotation",
    directory = "builds/sandbox/dataTaps/firrtl",
    filename = "builds/sandbox/dataTaps/firrtl/bindings.sv"
  }]
} {
  firrtl.module @Bar(
    in %clock: !firrtl.clock {firrtl.annotations = [{
      class = "sifive.enterprise.grandcentral.ReferenceDataTapKey",
      id = 0 : i64,
      portID = 2 : i64
    }]},
    in %reset: !firrtl.reset {firrtl.annotations = [{
      class = "sifive.enterprise.grandcentral.ReferenceDataTapKey",
      id = 0 : i64,
      portID = 3 : i64
    }]},
    in %in: !firrtl.uint<1>,
    out %out: !firrtl.uint<1>
  ) {
    %wire = firrtl.wire {annotations = [{
      class = "sifive.enterprise.grandcentral.ReferenceDataTapKey",
      id = 0 : i64,
      portID = 1 : i64
    }, {
      class = "firrtl.transforms.DontTouchAnnotation"
    }]} : !firrtl.uint<1>

    %mem = firrtl.mem Undefined {
      annotations = [{
        class = "sifive.enterprise.grandcentral.MemTapAnnotation",
        id = 4 : i64
      }, {
        class = "firrtl.transforms.DontTouchAnnotation"
      }],
      name = "mem",
      depth = 2 : i64,
      portNames = ["MPORT"],
      readLatency = 0 : i32,
      writeLatency = 1 : i32
    } : !firrtl.flip<bundle<addr: uint<1>, en: uint<1>, clk: clock, data: flip<uint<1>>>>

    %0 = firrtl.not %in : (!firrtl.uint<1>) -> !firrtl.uint<1>
    firrtl.connect %wire, %0  : !firrtl.uint<1>, !firrtl.uint<1>
    firrtl.connect %out, %wire : !firrtl.uint<1>, !firrtl.uint<1>
  }

  firrtl.module @Foo(
    in %clock: !firrtl.clock,
    in %reset: !firrtl.reset,
    in %in: !firrtl.uint<1>,
    out %out: !firrtl.uint<1>
  ) {
    %bar_clock, %bar_reset, %bar_in, %bar_out = firrtl.instance @Bar  {name = "bar"} : !firrtl.flip<clock>, !firrtl.flip<reset>, !firrtl.flip<uint<1>>, !firrtl.uint<1>
    firrtl.connect %bar_clock, %clock : !firrtl.flip<clock>, !firrtl.clock
    firrtl.connect %bar_reset, %reset : !firrtl.flip<reset>, !firrtl.reset
    firrtl.connect %bar_in, %in : !firrtl.flip<uint<1>>, !firrtl.uint<1>
    firrtl.connect %out, %bar_out : !firrtl.uint<1>, !firrtl.uint<1>
  }

  // CHECK: module [[DT:DataTap.*]](
  // CHECK: assign _3 = bigScary.schwarzschild.no.more;
  // CHECK: assign _2 = foo.bar.reset;
  // CHECK: assign _1 = foo.bar.clock;
  // CHECK: assign _0 = foo.bar.wire;
  firrtl.extmodule @DataTap(
    out %_3: !firrtl.uint<1> {firrtl.annotations = [{
      class = "sifive.enterprise.grandcentral.DataTapModuleSignalKey",
      internalPath = "schwarzschild.no.more",
      id = 0 : i64,
      portID = 4 : i64 }]},
    out %_2: !firrtl.uint<1> {firrtl.annotations = [{
      class = "sifive.enterprise.grandcentral.ReferenceDataTapKey",
      id = 0 : i64,
      portID = 3 : i64 }]},
    out %_1: !firrtl.clock {firrtl.annotations = [{
      class = "sifive.enterprise.grandcentral.ReferenceDataTapKey",
      id = 0 : i64,
      portID = 2 : i64 }]},
    out %_0: !firrtl.uint<1> {firrtl.annotations = [{
      class = "sifive.enterprise.grandcentral.ReferenceDataTapKey",
      id = 0 : i64,
      portID = 1 : i64 }]}
  ) attributes {
    annotations = [
      { class = "sifive.enterprise.grandcentral.DataTapsAnnotation" },
      { class = "firrtl.transforms.NoDedupAnnotation" }
    ],
    defname = "DataTap"
  }

  // CHECK: module [[MT:MemTap.*]](
  // CHECK: assign mem_0 = foo.bar.mem[0];
  // CHECK: assign mem_1 = foo.bar.mem[1];
  firrtl.extmodule @MemTap(
    out %mem_0: !firrtl.uint<1> {firrtl.annotations = [{
      class = "sifive.enterprise.grandcentral.MemTapAnnotation",
      id = 4 : i64 }]},
    out %mem_1: !firrtl.uint<1> {firrtl.annotations = [{
      class = "sifive.enterprise.grandcentral.MemTapAnnotation",
      id = 4 : i64 }]}
  ) attributes {
    annotations = [
      {class = "firrtl.transforms.NoDedupAnnotation"}
    ],
    defname = "MemTap"
  }

  firrtl.extmodule @BlackHole() attributes {
    annotations = [{
      class = "sifive.enterprise.grandcentral.DataTapModuleSignalKey",
      internalPath = "schwarzschild.no.more",
      id = 0 : i64,
      portID = 4 : i64 }]
  }

  // CHECK-LABEL: module TestHarness
  firrtl.module @TestHarness(in %clock: !firrtl.clock, in %reset: !firrtl.uint<1>, in %in: !firrtl.uint<1>, out %out: !firrtl.uint<1>) {
    %foo_clock, %foo_reset, %foo_in, %foo_out = firrtl.instance @Foo {name = "foo"} : !firrtl.flip<clock>, !firrtl.flip<reset>, !firrtl.flip<uint<1>>, !firrtl.uint<1>
    firrtl.connect %foo_clock, %clock : !firrtl.flip<clock>, !firrtl.clock
    firrtl.connect %foo_reset, %reset : !firrtl.flip<reset>, !firrtl.uint<1>
    firrtl.connect %foo_in, %in : !firrtl.flip<uint<1>>, !firrtl.uint<1>
    firrtl.connect %out, %foo_out : !firrtl.uint<1>, !firrtl.uint<1>
    firrtl.instance @BlackHole {name = "bigScary"}
    // CHECK: [[DT]] dataTap (
    %DataTap_3, %DataTap_2, %DataTap_1, %DataTap_0 = firrtl.instance @DataTap {name = "dataTap"} : !firrtl.uint<1>, !firrtl.uint<1>, !firrtl.clock, !firrtl.uint<1>
    // CHECK: [[MT]] memTap (
    %MemTap_mem_0, %MemTap_mem_1 = firrtl.instance @MemTap {name = "memTap"} : !firrtl.uint<1>, !firrtl.uint<1>
  }
}