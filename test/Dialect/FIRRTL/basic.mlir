// RUN: circt-opt %s --verify-diagnostics | FileCheck %s

firrtl.circuit "Foo" {
firrtl.module @Foo() {}

// CHECK-LABEL: firrtl.module @AsyncResetConst
// The following should not error.
firrtl.module @AsyncResetConst(in %clock: !firrtl.clock, in %reset: !firrtl.asyncreset) {
  // Constant check should handle trivial cases.
  %c0_ui = firrtl.constant 0 : !firrtl.uint<8>
  %0 = firrtl.regreset %clock, %reset, %c0_ui : (!firrtl.clock, !firrtl.asyncreset, !firrtl.uint<8>) -> !firrtl.uint<8>

  // Constant check should see through nodes.
  %node = firrtl.node %c0_ui : !firrtl.uint<8>
  %1 = firrtl.regreset %clock, %reset, %node : (!firrtl.clock, !firrtl.asyncreset, !firrtl.uint<8>) -> !firrtl.uint<8>

  // Constant check should see through subfield connects.
  %bundle0 = firrtl.wire : !firrtl.bundle<a: uint<8>>
  %bundle0.a = firrtl.subfield %bundle0(0) : (!firrtl.bundle<a: uint<8>>) -> !firrtl.uint<8>
  firrtl.connect %bundle0.a, %c0_ui : !firrtl.uint<8>, !firrtl.uint<8>
  %2 = firrtl.regreset %clock, %reset, %bundle0 : (!firrtl.clock, !firrtl.asyncreset, !firrtl.bundle<a: uint<8>>) -> !firrtl.bundle<a: uint<8>>

  // Constant check should see through multiple connect hops.
  %bundle1 = firrtl.wire : !firrtl.bundle<a: uint<8>>
  firrtl.connect %bundle1, %bundle0 : !firrtl.bundle<a: uint<8>>, !firrtl.bundle<a: uint<8>>
  %3 = firrtl.regreset %clock, %reset, %bundle1 : (!firrtl.clock, !firrtl.asyncreset, !firrtl.bundle<a: uint<8>>) -> !firrtl.bundle<a: uint<8>>

  // Constant check should see through subindex connects.
  %vector0 = firrtl.wire : !firrtl.vector<uint<8>, 1>
  %vector0.a = firrtl.subindex %vector0[0] : !firrtl.vector<uint<8>, 1>
  firrtl.connect %vector0.a, %c0_ui : !firrtl.uint<8>, !firrtl.uint<8>
  %4 = firrtl.regreset %clock, %reset, %vector0 : (!firrtl.clock, !firrtl.asyncreset, !firrtl.vector<uint<8>, 1>) -> !firrtl.vector<uint<8>, 1>

  // Constant check should see through multiple connect hops.
  %vector1 = firrtl.wire : !firrtl.vector<uint<8>, 1>
  firrtl.connect %vector1, %vector0 : !firrtl.vector<uint<8>, 1>, !firrtl.vector<uint<8>, 1>
  %5 = firrtl.regreset %clock, %reset, %vector1 : (!firrtl.clock, !firrtl.asyncreset, !firrtl.vector<uint<8>, 1>) -> !firrtl.vector<uint<8>, 1>
}

// Complex literals should be allowed as reset values for AsyncReset
firrtl.module @AsyncResetComplexLiterals(in %clock: !firrtl.clock, in %reset: !firrtl.asyncreset, in %x: !firrtl.vector<uint<1>, 4>, out %z: !firrtl.vector<uint<1>, 4>) {
  %literal = firrtl.wire  : !firrtl.vector<uint<1>, 4>
  %0 = firrtl.subindex %literal[0] : !firrtl.vector<uint<1>, 4>
  %c0_ui1 = firrtl.constant 0 : !firrtl.uint<1>
  firrtl.connect %0, %c0_ui1 : !firrtl.uint<1>, !firrtl.uint<1>
  %1 = firrtl.subindex %literal[1] : !firrtl.vector<uint<1>, 4>
  firrtl.connect %1, %c0_ui1 : !firrtl.uint<1>, !firrtl.uint<1>
  %2 = firrtl.subindex %literal[2] : !firrtl.vector<uint<1>, 4>
  firrtl.connect %2, %c0_ui1 : !firrtl.uint<1>, !firrtl.uint<1>
  %3 = firrtl.subindex %literal[3] : !firrtl.vector<uint<1>, 4>
  firrtl.connect %3, %c0_ui1 : !firrtl.uint<1>, !firrtl.uint<1>
  %r = firrtl.regreset %clock, %reset, %literal  : (!firrtl.clock, !firrtl.asyncreset, !firrtl.vector<uint<1>, 4>) -> !firrtl.vector<uint<1>, 4>
  firrtl.connect %r, %x : !firrtl.vector<uint<1>, 4>, !firrtl.vector<uint<1>, 4>
  firrtl.connect %z, %r : !firrtl.vector<uint<1>, 4>, !firrtl.vector<uint<1>, 4>
}

// Complex literals of complex literals should be allowed as reset values for AsyncReset
firrtl.module @AsyncResetComplexNestedLiterals(in %clock: !firrtl.clock, in %reset: !firrtl.asyncreset, in %x: !firrtl.vector<uint<1>, 4>, out %z: !firrtl.vector<uint<1>, 4>) {
  %literal = firrtl.wire  : !firrtl.vector<uint<1>, 2>
  %0 = firrtl.subindex %literal[0] : !firrtl.vector<uint<1>, 2>
  %c1_ui1 = firrtl.constant 1 : !firrtl.uint<1>
  firrtl.connect %0, %c1_ui1 : !firrtl.uint<1>, !firrtl.uint<1>
  %1 = firrtl.subindex %literal[1] : !firrtl.vector<uint<1>, 2>
  firrtl.connect %1, %c1_ui1 : !firrtl.uint<1>, !firrtl.uint<1>
  %complex_literal = firrtl.wire  : !firrtl.vector<uint<1>, 4>
  %2 = firrtl.subindex %complex_literal[0] : !firrtl.vector<uint<1>, 4>
  %3 = firrtl.subindex %literal[0] : !firrtl.vector<uint<1>, 2>
  firrtl.connect %2, %3 : !firrtl.uint<1>, !firrtl.uint<1>
  %4 = firrtl.subindex %complex_literal[1] : !firrtl.vector<uint<1>, 4>
  %5 = firrtl.subindex %literal[1] : !firrtl.vector<uint<1>, 2>
  firrtl.connect %4, %5 : !firrtl.uint<1>, !firrtl.uint<1>
  %6 = firrtl.subindex %complex_literal[2] : !firrtl.vector<uint<1>, 4>
  %c0_ui1 = firrtl.constant 0 : !firrtl.uint<1>
  firrtl.connect %6, %c0_ui1 : !firrtl.uint<1>, !firrtl.uint<1>
  %7 = firrtl.subindex %complex_literal[3] : !firrtl.vector<uint<1>, 4>
  firrtl.connect %7, %c0_ui1 : !firrtl.uint<1>, !firrtl.uint<1>
  %r = firrtl.regreset %clock, %reset, %complex_literal  : (!firrtl.clock, !firrtl.asyncreset, !firrtl.vector<uint<1>, 4>) -> !firrtl.vector<uint<1>, 4>
  firrtl.connect %r, %x : !firrtl.vector<uint<1>, 4>, !firrtl.vector<uint<1>, 4>
  firrtl.connect %z, %r : !firrtl.vector<uint<1>, 4>, !firrtl.vector<uint<1>, 4>
}

// Literals of bundle literals should be allowed as reset values for AsyncReset
firrtl.module @AsyncResetBundleLiterals(in %clock: !firrtl.clock, in %reset: !firrtl.asyncreset, in %x: !firrtl.vector<uint<1>, 4>, out %z: !firrtl.vector<uint<1>, 4>) {
  %bundle = firrtl.wire  : !firrtl.bundle<a: uint<1>, b: uint<1>>
  %0 = firrtl.subfield %bundle(0) : (!firrtl.bundle<a: uint<1>, b: uint<1>>) -> !firrtl.uint<1>
  %c1_ui1 = firrtl.constant 1 : !firrtl.uint<1>
  firrtl.connect %0, %c1_ui1 : !firrtl.uint<1>, !firrtl.uint<1>
  %1 = firrtl.subfield %bundle(1) : (!firrtl.bundle<a: uint<1>, b: uint<1>>) -> !firrtl.uint<1>
  firrtl.connect %1, %c1_ui1 : !firrtl.uint<1>, !firrtl.uint<1>
  %complex_literal = firrtl.wire  : !firrtl.vector<uint<1>, 4>
  %2 = firrtl.subindex %complex_literal[0] : !firrtl.vector<uint<1>, 4>
  %3 = firrtl.subfield %bundle(0) : (!firrtl.bundle<a: uint<1>, b: uint<1>>) -> !firrtl.uint<1>
  firrtl.connect %2, %3 : !firrtl.uint<1>, !firrtl.uint<1>
  %4 = firrtl.subindex %complex_literal[1] : !firrtl.vector<uint<1>, 4>
  %5 = firrtl.subfield %bundle(1) : (!firrtl.bundle<a: uint<1>, b: uint<1>>) -> !firrtl.uint<1>
  firrtl.connect %4, %5 : !firrtl.uint<1>, !firrtl.uint<1>
  %6 = firrtl.subindex %complex_literal[2] : !firrtl.vector<uint<1>, 4>
  %c0_ui1 = firrtl.constant 0 : !firrtl.uint<1>
  firrtl.connect %6, %c0_ui1 : !firrtl.uint<1>, !firrtl.uint<1>
  %7 = firrtl.subindex %complex_literal[3] : !firrtl.vector<uint<1>, 4>
  firrtl.connect %7, %c0_ui1 : !firrtl.uint<1>, !firrtl.uint<1>
  %r = firrtl.regreset %clock, %reset, %complex_literal  : (!firrtl.clock, !firrtl.asyncreset, !firrtl.vector<uint<1>, 4>) -> !firrtl.vector<uint<1>, 4>
  firrtl.connect %r, %x : !firrtl.vector<uint<1>, 4>, !firrtl.vector<uint<1>, 4>
  firrtl.connect %z, %r : !firrtl.vector<uint<1>, 4>, !firrtl.vector<uint<1>, 4>
}

// Cast literals should be allowed as reset values for AsyncReset
firrtl.module @AsyncResetCastLiterals(in %clock: !firrtl.clock, in %reset: !firrtl.asyncreset, in %x: !firrtl.sint<4>, out %y: !firrtl.sint<4>, out %z: !firrtl.sint<4>) {
  %c0_ui = firrtl.constant 0 : !firrtl.uint
  %0 = firrtl.asSInt %c0_ui : (!firrtl.uint) -> !firrtl.sint
  %r = firrtl.regreset %clock, %reset, %0  : (!firrtl.clock, !firrtl.asyncreset, !firrtl.sint) -> !firrtl.sint<4>
  firrtl.connect %r, %x : !firrtl.sint<4>, !firrtl.sint<4>
  %w = firrtl.wire  : !firrtl.sint<4>
  %r2 = firrtl.regreset %clock, %reset, %w  : (!firrtl.clock, !firrtl.asyncreset, !firrtl.sint<4>) -> !firrtl.sint<4>
  firrtl.connect %r2, %x : !firrtl.sint<4>, !firrtl.sint<4>
  %c15_ui = firrtl.constant 15 : !firrtl.uint
  %n = firrtl.node %c15_ui  : !firrtl.uint
  %1 = firrtl.asSInt %n : (!firrtl.uint) -> !firrtl.sint
  firrtl.connect %w, %1 : !firrtl.sint<4>, !firrtl.sint
  firrtl.connect %y, %r2 : !firrtl.sint<4>, !firrtl.sint<4>
  firrtl.connect %z, %r : !firrtl.sint<4>, !firrtl.sint<4>
}

}
