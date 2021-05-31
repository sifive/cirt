// RUN: circt-opt --pass-pipeline="firrtl.circuit(firrtl-prefix-modules)" %s | FileCheck %s



// CHECK: asdasd
firrtl.circuit "Zebra" {
firrtl.module @Top() {
}

firrtl.module @Aardvark() {
  firrtl.instance @Top { name = "test" }
}

firrtl.module @Zebra() {
  firrtl.instance @Aardvark { name = "test" }

}
}

