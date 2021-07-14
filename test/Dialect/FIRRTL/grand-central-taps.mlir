// RUN: circt-opt %s --firrtl-grand-central-taps | FileCheck %s

firrtl.circuit "TestHarness" attributes {
  annotations = [{
    class = "sifive.enterprise.grandcentral.ExtractGrandCentralAnnotation",
    directory = "builds/sandbox/dataTaps/firrtl",
    filename = "builds/sandbox/dataTaps/firrtl/bindings.sv"
  }]
} {
  // CHECK-LABEL: firrtl.module @Bar
  // CHECK-NOT: class = "sifive.enterprise.grandcentral.ReferenceDataTapKey"
  firrtl.module @Bar(
    in %clock: !firrtl.clock {firrtl.annotations = [{
      class = "sifive.enterprise.grandcentral.ReferenceDataTapKey",
      id = 0 : i64,
      portID = 2 : i64,
      type = "source"
    }]},
    in %reset: !firrtl.reset {firrtl.annotations = [{
      class = "sifive.enterprise.grandcentral.ReferenceDataTapKey",
      id = 0 : i64,
      portID = 3 : i64,
      type = "source"
    }]},
    in %in: !firrtl.uint<1>,
    out %out: !firrtl.uint<1>
  ) {
    // CHECK-LABEL: %wire = firrtl.wire
    // CHECK-NOT: class = "sifive.enterprise.grandcentral.ReferenceDataTapKey"
    // CHECK-SAME: class = "firrtl.transforms.DontTouchAnnotation"
    %wire = firrtl.wire {annotations = [{
      class = "sifive.enterprise.grandcentral.ReferenceDataTapKey",
      id = 0 : i64,
      portID = 1 : i64,
      type = "source"
    }, {
      class = "firrtl.transforms.DontTouchAnnotation"
    }]} : !firrtl.uint<1>

    // CHECK-LABEL: %node = firrtl.node
    // CHECK-NOT: class = "sifive.enterprise.grandcentral.ReferenceDataTapKey"
    // CHECK-SAME: class = "firrtl.transforms.DontTouchAnnotation"
    %node = firrtl.node %in {annotations = [{
      class = "sifive.enterprise.grandcentral.ReferenceDataTapKey",
      id = 0 : i64,
      portID = 5 : i64,
      type = "source"
    }, {
      class = "firrtl.transforms.DontTouchAnnotation"
    }]} : !firrtl.uint<1>

    // CHECK-LABEL: %reg = firrtl.reg
    // CHECK-NOT: class = "sifive.enterprise.grandcentral.ReferenceDataTapKey"
    // CHECK-SAME: class = "firrtl.transforms.DontTouchAnnotation"
    %reg = firrtl.reg %clock {annotations = [{
      class = "sifive.enterprise.grandcentral.ReferenceDataTapKey",
      id = 0 : i64,
      portID = 6 : i64,
      type = "source"
    }, {
      class = "firrtl.transforms.DontTouchAnnotation"
    }]} : (!firrtl.clock) -> !firrtl.uint<1>

    // CHECK-LABEL: %regreset = firrtl.regreset
    // CHECK-NOT: class = "sifive.enterprise.grandcentral.ReferenceDataTapKey"
    // CHECK-SAME: class = "firrtl.transforms.DontTouchAnnotation"
    %regreset = firrtl.regreset %clock, %reset, %in {annotations = [{
      class = "sifive.enterprise.grandcentral.ReferenceDataTapKey",
      id = 0 : i64,
      portID = 7 : i64,
      type = "source"
    }, {
      class = "firrtl.transforms.DontTouchAnnotation"
    }]} : (!firrtl.clock, !firrtl.reset, !firrtl.uint<1>) -> !firrtl.uint<1>

    // CHECK-LABEL: firrtl.mem Undefined
    // CHECK-NOT: class = "sifive.enterprise.grandcentral.MemTapAnnotation"
    // CHECK-SAME: class = "firrtl.transforms.DontTouchAnnotation"
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
    } : !firrtl.bundle<addr: uint<1>, en: uint<1>, clk: clock, data flip: uint<1>>
    %mem_addr = firrtl.subfield %mem(0) : (!firrtl.bundle<addr: uint<1>, en: uint<1>, clk: clock, data flip: uint<1>>) -> !firrtl.uint<1>
    %mem_en = firrtl.subfield %mem(1) : (!firrtl.bundle<addr: uint<1>, en: uint<1>, clk: clock, data flip: uint<1>>) -> !firrtl.uint<1>
    %mem_clk = firrtl.subfield %mem(2) : (!firrtl.bundle<addr: uint<1>, en: uint<1>, clk: clock, data flip: uint<1>>) -> !firrtl.clock
    firrtl.connect %mem_addr, %in : !firrtl.uint<1>, !firrtl.uint<1>
    firrtl.connect %mem_en, %in : !firrtl.uint<1>, !firrtl.uint<1>
    firrtl.connect %mem_clk, %clock : !firrtl.clock, !firrtl.clock

    %42 = firrtl.not %in : (!firrtl.uint<1>) -> !firrtl.uint<1>
    firrtl.connect %wire, %42  : !firrtl.uint<1>, !firrtl.uint<1>
    firrtl.connect %out, %wire : !firrtl.uint<1>, !firrtl.uint<1>
  }

  firrtl.module @Foo(
    in %clock: !firrtl.clock,
    in %reset: !firrtl.reset,
    in %in: !firrtl.uint<1>,
    out %out: !firrtl.uint<1>
  ) {
    %bar_clock, %bar_reset, %bar_in, %bar_out = firrtl.instance @Bar  {name = "bar"} : !firrtl.clock, !firrtl.reset, !firrtl.uint<1>, !firrtl.uint<1>
    firrtl.connect %bar_clock, %clock : !firrtl.clock, !firrtl.clock
    firrtl.connect %bar_reset, %reset : !firrtl.reset, !firrtl.reset
    firrtl.connect %bar_in, %in : !firrtl.uint<1>, !firrtl.uint<1>
    firrtl.connect %out, %bar_out : !firrtl.uint<1>, !firrtl.uint<1>
  }

  // CHECK: firrtl.module [[DT:@DataTap.*]](
  // CHECK-SAME: out %_9: !firrtl.uint<1>
  // CHECK-SAME: out %_8: !firrtl.sint<8>
  // CHECK-SAME: out %_7: !firrtl.uint<1>
  // CHECK-SAME: out %_6: !firrtl.uint<1>
  // CHECK-SAME: out %_5: !firrtl.uint<1>
  // CHECK-SAME: out %_4: !firrtl.uint<1>
  // CHECK-SAME: out %_3: !firrtl.uint<1>
  // CHECK-SAME: out %_2: !firrtl.uint<1>
  // CHECK-SAME: out %_1: !firrtl.clock
  // CHECK-SAME: out %_0: !firrtl.uint<1>
  // CHECK-NEXT: [[V9:%.+]] = firrtl.constant 0 : !firrtl.uint<1>
  // CHECK-NEXT: firrtl.connect %_9, [[V9]]
  // CHECK-NEXT: [[V8:%.+]] = firrtl.constant -42 : !firrtl.sint<8>
  // CHECK-NEXT: firrtl.connect %_8, [[V8]]
  // CHECK-NEXT: [[V7:%.+]] = firrtl.verbatim.expr "extmoduleWithTappedPort.out"
  // CHECK-NEXT: firrtl.connect %_7, [[V7]]
  // CHECK-NEXT: [[V6:%.+]] = firrtl.verbatim.expr "foo.bar.regreset"
  // CHECK-NEXT: firrtl.connect %_6, [[V6]]
  // CHECK-NEXT: [[V5:%.+]] = firrtl.verbatim.expr "foo.bar.reg"
  // CHECK-NEXT: firrtl.connect %_5, [[V5]]
  // CHECK-NEXT: [[V4:%.+]] = firrtl.verbatim.expr "foo.bar.node"
  // CHECK-NEXT: firrtl.connect %_4, [[V4]]
  // CHECK-NEXT: [[V3:%.+]] = firrtl.verbatim.expr "bigScary.schwarzschild.no.more"
  // CHECK-NEXT: firrtl.connect %_3, [[V3]]
  // CHECK-NEXT: [[V2:%.+]] = firrtl.verbatim.expr "foo.bar.reset"
  // CHECK-NEXT: firrtl.connect %_2, [[V2]]
  // CHECK-NEXT: [[V1:%.+]] = firrtl.verbatim.expr "foo.bar.clock"
  // CHECK-NEXT: firrtl.connect %_1, [[V1]]
  // CHECK-NEXT: [[V0:%.+]] = firrtl.verbatim.expr "foo.bar.wire"
  // CHECK-NEXT: firrtl.connect %_0, [[V0]]
  firrtl.extmodule @DataTap(
    out %_9: !firrtl.uint<1> {firrtl.annotations = [{
        class = "sifive.enterprise.grandcentral.LiteralDataTapKey",
        literal = "UInt<1>(\"h0\")",
        id = 0 : i64,
        portID = 10 : i64 }]},
    out %_8: !firrtl.sint<8> {firrtl.annotations = [{
        class = "sifive.enterprise.grandcentral.LiteralDataTapKey",
        literal = "SInt<8>(-42)",
        id = 0 : i64,
        portID = 9 : i64 }]},
    out %_7: !firrtl.uint<1> {firrtl.annotations = [{
      class = "sifive.enterprise.grandcentral.ReferenceDataTapKey",
      id = 0 : i64,
      portID = 8 : i64,
      type = "portName" }]},
    out %_6: !firrtl.uint<1> {firrtl.annotations = [{
      class = "sifive.enterprise.grandcentral.ReferenceDataTapKey",
      id = 0 : i64,
      portID = 7 : i64,
      type = "portName" }]},
    out %_5: !firrtl.uint<1> {firrtl.annotations = [{
      class = "sifive.enterprise.grandcentral.ReferenceDataTapKey",
      id = 0 : i64,
      portID = 6 : i64,
      type = "portName" }]},
    out %_4: !firrtl.uint<1> {firrtl.annotations = [{
      class = "sifive.enterprise.grandcentral.ReferenceDataTapKey",
      id = 0 : i64,
      portID = 5 : i64,
      type = "portName" }]},
    out %_3: !firrtl.uint<1> {firrtl.annotations = [{
      class = "sifive.enterprise.grandcentral.DataTapModuleSignalKey",
      internalPath = "schwarzschild.no.more",
      id = 0 : i64,
      portID = 4 : i64 }]},
    out %_2: !firrtl.uint<1> {firrtl.annotations = [{
      class = "sifive.enterprise.grandcentral.ReferenceDataTapKey",
      id = 0 : i64,
      portID = 3 : i64,
      type = "portName" }]},
    out %_1: !firrtl.clock {firrtl.annotations = [{
      class = "sifive.enterprise.grandcentral.ReferenceDataTapKey",
      id = 0 : i64,
      portID = 2 : i64,
      type = "portName" }]},
    out %_0: !firrtl.uint<1> {firrtl.annotations = [{
      class = "sifive.enterprise.grandcentral.ReferenceDataTapKey",
      id = 0 : i64,
      portID = 1 : i64,
      type = "portName" }]}
  ) attributes {
    annotations = [
      { class = "sifive.enterprise.grandcentral.DataTapsAnnotation" },
      { class = "firrtl.transforms.NoDedupAnnotation" }
    ],
    defname = "DataTap"
  }

  // CHECK: firrtl.module [[MT:@MemTap.*]](
  // CHECK-NOT: class = "sifive.enterprise.grandcentral.MemTapAnnotation"
  // CHECK-SAME: out %mem_0: !firrtl.uint<1>
  // CHECK-SAME: out %mem_1: !firrtl.uint<1>
  // CHECK-SAME: class = "firrtl.transforms.NoDedupAnnotation"
  // CHECK-NEXT: [[V0:%.+]] = firrtl.verbatim.expr "foo.bar.mem.Memory[0]"
  // CHECK-NEXT: firrtl.connect %mem_0, [[V0:%.+]]
  // CHECK-NEXT: [[V1:%.+]] = firrtl.verbatim.expr "foo.bar.mem.Memory[1]"
  // CHECK-NEXT: firrtl.connect %mem_1, [[V1:%.+]]
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

  // CHECK-LABEL: firrtl.extmodule @BlackHole()
  // CHECK-NOT: class = "sifive.enterprise.grandcentral.DataTapModuleSignalKey"
  firrtl.extmodule @BlackHole() attributes {
    annotations = [{
      class = "sifive.enterprise.grandcentral.DataTapModuleSignalKey",
      internalPath = "schwarzschild.no.more",
      id = 0 : i64,
      portID = 4 : i64 }]
  }

  firrtl.extmodule @ExtmoduleWithTappedPort(
    out %out: !firrtl.uint<1> {firrtl.annotations = [{
      class = "sifive.enterprise.grandcentral.ReferenceDataTapKey",
      id = 0 : i64,
      portID = 8 : i64,
      type = "source" }]}
  )

  // CHECK: firrtl.module @TestHarness
  firrtl.module @TestHarness(in %clock: !firrtl.clock, in %reset: !firrtl.uint<1>, in %in: !firrtl.uint<1>, out %out: !firrtl.uint<1>) {
    %foo_clock, %foo_reset, %foo_in, %foo_out = firrtl.instance @Foo {name = "foo"} : !firrtl.clock, !firrtl.reset, !firrtl.uint<1>, !firrtl.uint<1>
    firrtl.connect %foo_clock, %clock : !firrtl.clock, !firrtl.clock
    firrtl.connect %foo_reset, %reset : !firrtl.reset, !firrtl.uint<1>
    firrtl.connect %foo_in, %in : !firrtl.uint<1>, !firrtl.uint<1>
    firrtl.connect %out, %foo_out : !firrtl.uint<1>, !firrtl.uint<1>
    firrtl.instance @BlackHole {name = "bigScary"}
    %0 = firrtl.instance @ExtmoduleWithTappedPort {name = "extmoduleWithTappedPort"} : !firrtl.uint<1>
    // CHECK: firrtl.instance [[DT]] {name = "dataTap"}
    %DataTap_9, %DataTap_8, %DataTap_7, %DataTap_6, %DataTap_5, %DataTap_4, %DataTap_3, %DataTap_2, %DataTap_1, %DataTap_0 = firrtl.instance @DataTap {name = "dataTap"} : !firrtl.uint<1>, !firrtl.sint<8>, !firrtl.uint<1>, !firrtl.uint<1>, !firrtl.uint<1>, !firrtl.uint<1>, !firrtl.uint<1>, !firrtl.uint<1>, !firrtl.clock, !firrtl.uint<1>
    // CHECK: firrtl.instance [[MT]] {name = "memTap"}
    %MemTap_mem_0, %MemTap_mem_1 = firrtl.instance @MemTap {name = "memTap"} : !firrtl.uint<1>, !firrtl.uint<1>
  }
}
