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

}
