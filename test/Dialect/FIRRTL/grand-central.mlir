// RUN: circt-opt -pass-pipeline='firrtl.circuit(firrtl-grand-central)' -split-input-file %s | FileCheck %s

firrtl.circuit "InterfaceGroundType" attributes {annotations = [{class = "sifive.enterprise.grandcentral.AugmentedBundleType", defName = "Foo", elements = [{description = "description of foo", name = "foo", tpe = "sifive.enterprise.grandcentral.AugmentedGroundType"}, {name = "bar", tpe = "sifive.enterprise.grandcentral.AugmentedGroundType"}]}]} {
  firrtl.module @InterfaceGroundType() {
    %a = firrtl.wire {annotations = [{a}, {class = "sifive.enterprise.grandcentral.AugmentedGroundType", defName = "Foo", name = "foo", target = []}]} : !firrtl.uint<2>
    %b = firrtl.wire {annotations = [{a}, {class = "sifive.enterprise.grandcentral.AugmentedGroundType", defName = "Foo", name = "bar", target = []}]} : !firrtl.uint<4>
  }
}

// This block is checking that all annotations were removed.
// CHECK-LABEL: firrtl.circuit "InterfaceGroundType"
// CHECK-NOT: annotations
// CHECK-SAME: {

// All Grand Central annotations are removed from the wires.
// CHECK: firrtl.module @InterfaceGroundType
// CHECK: %a = firrtl.wire
// CHECK-SAME: annotations = [{a}]
// CHECK: %b = firrtl.wire
// CHECK-SAME: annotations = [{a}]

// CHECK: sv.interface @Foo
// CHECK-NEXT: sv.verbatim "\0A// description of foo"
// CHECK-NEXT: sv.interface.signal @foo : i2
// CHECK-NEXT: sv.interface.signal @bar : i4

// -----

firrtl.circuit "InterfaceVectorType" attributes {annotations = [{class = "sifive.enterprise.grandcentral.AugmentedBundleType", defName = "Foo", elements = [{description = "description of foo", name = "foo", tpe = "sifive.enterprise.grandcentral.AugmentedVectorType"}]}]} {
  firrtl.module @InterfaceVectorType(in %clock: !firrtl.clock, in %reset: !firrtl.uint<1>) {
    %a_0 = firrtl.reg %clock {annotations = [{a}, {class = "sifive.enterprise.grandcentral.AugmentedGroundType", defName = "Foo", name = "foo"}]} : (!firrtl.clock) -> !firrtl.uint<1>
    %c0_ui1 = firrtl.constant 0 : !firrtl.uint<1>
    %a_1 = firrtl.regreset %clock, %reset, %c0_ui1 {annotations = [{a}, {class = "sifive.enterprise.grandcentral.AugmentedGroundType", defName = "Foo", name = "foo"}]} : (!firrtl.clock, !firrtl.uint<1>, !firrtl.uint<1>) -> !firrtl.uint<1>
  }
}

// All annotations are removed from the circuit.
// CHECK-LABEL: firrtl.circuit "InterfaceVectorType"
// CHECK-NOT: annotations
// CHECK-SAME: {

// All Grand Central annotations are removed from the registers.
// CHECK: firrtl.module @InterfaceVectorType
// CHECK: %a_0 = firrtl.reg
// CHECK-SAME: annotations = [{a}]
// CHECK: %a_1 = firrtl.regreset
// CHECK-SAME: annotations = [{a}]

// CHECK: sv.interface @Foo
// CHECK-NEXT: sv.verbatim "\0A// description of foo"
// CHECK-NEXT: sv.interface.signal @foo : !hw.uarray<2xi1>

// -----

firrtl.circuit "InterfaceBundleType" attributes {annotations = [{class = "sifive.enterprise.grandcentral.AugmentedBundleType", defName = "Bar", elements = [{name = "b", tpe = "sifive.enterprise.grandcentral.AugmentedGroundType"}, {name = "a", tpe = "sifive.enterprise.grandcentral.AugmentedGroundType"}]}, {class = "sifive.enterprise.grandcentral.AugmentedBundleType", defName = "Foo", elements = [{description = "descripton of Bar", name = "Bar", tpe = "sifive.enterprise.grandcentral.AugmentedBundleType"}]}]}  {
  firrtl.module @InterfaceBundleType() {
    %x = firrtl.wire {annotations = [{a}, {class = "sifive.enterprise.grandcentral.AugmentedGroundType", defName = "Bar", name = "a"}]} : !firrtl.uint<1>
    %y = firrtl.wire {annotations = [{a}, {class = "sifive.enterprise.grandcentral.AugmentedGroundType", defName = "Bar", name = "b"}]} : !firrtl.uint<2>
  }
}

// All annotations are removed from the circuit.
// CHECK-LABEL: firrtl.circuit "InterfaceBundleType"
// CHECK-NOT: annotations
// CHECK-SAME: {

// All Grand Central annotations are removed from the wires.
// CHECK-LABEL: firrtl.module @InterfaceBundleType
// CHECK: %x = firrtl.wire
// CHECK-SAME: annotations = [{a}]
// CHECK: %y = firrtl.wire
// CHECK-SAME: annotations = [{a}]

// CHECK: sv.interface @Bar
// CHECK-NEXT: sv.interface.signal @b : i2
// CHECK-NEXT: sv.interface.signal @a : i1

// CHECK: sv.interface @Foo
// CHECK-NEXT: sv.verbatim "\0A// descripton of Bar"
// CHECK-NEXT: Bar Bar();

// -----

firrtl.circuit "InterfaceNode" attributes {annotations = [{class = "sifive.enterprise.grandcentral.AugmentedBundleType", defName = "Foo", elements = [{description = "some expression", name = "foo", tpe = "sifive.enterprise.grandcentral.AugmentedGroundType"}]}]} {
  firrtl.module @InterfaceNode() {
    %a = firrtl.wire : !firrtl.uint<2>
    %notA = firrtl.not %a : (!firrtl.uint<2>) -> !firrtl.uint<2>
    %b = firrtl.node %notA {annotations = [{a}, {class = "sifive.enterprise.grandcentral.AugmentedGroundType", defName = "Foo", name = "foo", target = []}]} : !firrtl.uint<2>
  }
}

// All annotations are removed from the circuit.
// CHECK-LABEL: firrtl.circuit "InterfaceNode"
// CHECK-NOT: annotations
// CHECK-SAME: {

// The Grand Central annotation is removed from the node.
// CHECK: firrtl.node
// CHECK-SAME: annotations = [{a}]

// CHECK: sv.interface @Foo
// CHECK-NEXT: sv.verbatim "\0A// some expression"
// CHECK-NEXT: sv.interface.signal @foo : i2

// -----

firrtl.circuit "InterfacePort" attributes {annotations = [{class = "sifive.enterprise.grandcentral.AugmentedBundleType", defName = "Foo", elements = [{description = "description of foo", name = "foo", tpe = "sifive.enterprise.grandcentral.AugmentedGroundType"}]}]} {
  firrtl.module @InterfacePort(in %a : !firrtl.uint<4> {firrtl.annotations = [{a}, {class = "sifive.enterprise.grandcentral.AugmentedGroundType", defName = "Foo", name = "foo", target = []}]}) {
  }
}

// All annotations are removed from the circuit.
// CHECK-LABEL: firrtl.circuit "InterfacePort"
// CHECK-NOT: annotations
// CHECK-SAME: {

// The Grand Central annotations are removed.
// CHECK: firrtl.module @InterfacePort
// CHECK-SAME: firrtl.annotations = [{a}]

// CHECK: sv.interface @Foo
// CHECK-NEXT: sv.verbatim "\0A// description of foo"
// CHECK-NEXT: sv.interface.signal @foo : i4

// -----

firrtl.circuit "UnsupportedTypes" attributes {annotations = [{a}, {class = "sifive.enterprise.grandcentral.AugmentedBundleType", defName = "Foo", elements = [{name = "string", tpe = "sifive.enterprise.grandcentral.AugmentedStringType"}, {name = "boolean", tpe = "sifive.enterprise.grandcentral.AugmentedBooleanType"}, {name = "integer", tpe = "sifive.enterprise.grandcentral.AugmentedIntegerType"}, {name = "double", tpe = "sifive.enterprise.grandcentral.AugmentedDoubleType"}]}]} {
  firrtl.module @UnsupportedTypes() {}
}

// All Grand Central annotations are removed from the circuit.
// CHECK-LABEL: firrtl.circuit "UnsupportedTypes"
// CHECK-SAME: annotations = [{a}]

// CHECK: sv.interface @Foo
// CHECK-NEXT: sv.verbatim "// string = <unsupported string type>;"
// CHECK-NEXT: sv.verbatim "// boolean = <unsupported boolean type>;"
// CHECK-NEXT: sv.verbatim "// integer = <unsupported integer type>;"
// CHECK-NEXT: sv.verbatim "// double = <unsupported double type>;"

// -----

firrtl.circuit "BindTest" attributes {annotations = [{class = "sifive.enterprise.grandcentral.AugmentedBundleType", defName = "Foo", elements = []}]} {
  firrtl.module @Companion() attributes {annotations = [{class = "sifive.enterprise.grandcentral.GrandCentralView$SerializedViewAnnotation", id = 42 : i64, type = "companion"}]} {}
  firrtl.module @BindTest() {
    firrtl.instance @Companion { name = "companion1" }
    firrtl.instance @Companion { name = "companion2" }
  }
}

// CHECK-LABEL: firrtl.circuit "BindTest"

// Annotations are remove from the companion module declaration.
// CHECK: firrtl.module @Companion()
// CHECK-NOT: annotations
// CHECK-SAME: {

// Each companion instance has "lowerToBind" set.
// CHECK: firrtl.module @BindTest
// CHECK-COUNT-2: firrtl.instance @Companion {lowerToBind = true

// -----

firrtl.circuit "BindInterfaceTest"  attributes {
  annotations = [{
    class = "sifive.enterprise.grandcentral.AugmentedBundleType",
    defName = "InterfaceName",
    elements = [{
      name = "_a",
      tpe = "sifive.enterprise.grandcentral.AugmentedGroundType"
    }]
  }]} {
  firrtl.module @BindInterfaceTest(
    in %a: !firrtl.uint<8> {
      firrtl.annotations = [
        #firrtl.subAnno<fieldID = [0, 0], {
          class = "sifive.enterprise.grandcentral.AugmentedGroundType",
          defName = "InterfaceName",
          name = "_a"}>
      ]},
    out %b: !firrtl.uint<8>) attributes {
      annotations = [{
        class = "sifive.enterprise.grandcentral.GrandCentralView$SerializedViewAnnotation",
        defName = "InterfaceName",
        id = 0 : i64,
        name = "instanceName",
        type = "parent"
      }]} {
    firrtl.connect %b, %a : !firrtl.uint<8>, !firrtl.uint<8>
  }
}

// The bind is dropped in the outer module, outside the circuit.
// CHECK: module {
// CHECK-NEXT: sv.bind.interface @[[INTERFACE_INSTANCE_SYMBOL:.+]] {output_file

// CHECK-LABEL: firrtl.circuit "BindInterfaceTest"

// Annotations are removed from the circuit.
// CHECK-NOT: annotations
// CHECK-SAME: {

// Annotations are removed from the module.
// CHECK-NEXT: firrtl.module @BindInterfaceTest
// CHECK-NOT: annotations
// CHECK-SAME: %a

// An instance of the interface was added to the module.
// CHECK: sv.interface.instance sym @[[INTERFACE_INSTANCE_SYMBOL]] {doNotPrint = true, name = "instanceName"} : !sv.interface<@InterfaceName>

// The interface is added.
// CHECK: sv.interface @InterfaceName {
// CHECK-NEXT: sv.interface.signal @_a : i8
