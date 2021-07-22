// RUN: circt-opt -firrtl-print-instance-graph=print-LCA %s -o %t 2>&1 | FileCheck %s

// CHECK: digraph "Top"
// CHECK:   label="Top";
// CHECK:   [[TOP:.*]] [shape=record,label="{Top(0)}"];
// CHECK:   [[TOP]] -> [[ALLIGATOR:.*]][label=alligator];
// CHECK:   [[TOP]] -> [[CAT:.*]][label=cat];
// CHECK:   [[ALLIGATOR]] [shape=record,label="{Alligator(1)}"];
// CHECK:   [[ALLIGATOR]] -> [[BEAR:.*]][label=bear];
// CHECK:   [[CAT]] [shape=record,label="{Cat(3)}"];
// CHECK:   [[BEAR]] [shape=record,label="{Bear(2)}"];
// CHECK:   [[BEAR]] -> [[CAT]][label=cat];
// CHECK:  LCA of (Top(0),Alligator(1))=Top(0)
// CHECK:  LCA of (Top(0),Cat(3))=Top(0)
// CHECK:  LCA of (Top(0),Bear(2))=Top(0)
// CHECK:  LCA of (Top(0),Deer(2))=Top(0)
// CHECK:  LCA of (Alligator(1),Top(0))=Top(0)
// CHECK:  LCA of (Alligator(1),Cat(3))=Alligator(1)
// CHECK:  LCA of (Alligator(1),Bear(2))=Alligator(1)
// CHECK:  LCA of (Alligator(1),Deer(2))=Alligator(1)
// CHECK:  LCA of (Cat(3),Top(0))=Top(0)
// CHECK:  LCA of (Cat(3),Alligator(1))=Alligator(1)
// CHECK:  LCA of (Cat(3),Bear(2))=Bear(2)
// CHECK:  LCA of (Cat(3),Deer(2))=Alligator(1)
// CHECK:  LCA of (Bear(2),Top(0))=Top(0)
// CHECK:  LCA of (Bear(2),Alligator(1))=Alligator(1)
// CHECK:  LCA of (Bear(2),Cat(3))=Bear(2)
// CHECK:  LCA of (Bear(2),Deer(2))=Alligator(1)
// CHECK:  LCA of (Deer(2),Top(0))=Top(0)
// CHECK:  LCA of (Deer(2),Alligator(1))=Alligator(1)
// CHECK:  LCA of (Deer(2),Cat(3))=Alligator(1)
// CHECK:  LCA of (Deer(2),Bear(2))=Alligator(1)

firrtl.circuit "Top" {

firrtl.module @Top() {
  firrtl.instance @Alligator {name = "alligator" }
  firrtl.instance @Cat {name = "cat"}
}

firrtl.module @Alligator() {
  firrtl.instance @Bear {name = "bear"}
  firrtl.instance @Deer {name = "bear"}
}

firrtl.module @Bear() {
  firrtl.instance @Cat {name = "cat" }
}

firrtl.module @Cat() { }

firrtl.module @Deer() { }

firrtl.module @Bat() { }

}
