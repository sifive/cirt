// RUN: circt-translate %s -export-verilog -verify-diagnostics

hw.type_scope @__hw_typedecls {
  hw.typedecl @foo : i1
}

// expected-error @+1 {{unable to resolve declaration for type reference}}
hw.module @testTypeAlias1(%arg0: !hw.typealias<@__hw_typedecls::@bar,i1>) {}

// expected-error @+1 {{unable to resolve scope for type reference}}
hw.module @testTypeAlias2(%arg0: !hw.typealias<@_other_scope::@foo,i1>) {}

// expected-error @+1 {{declared type did not match aliased type}}
hw.module @testTypeAlias3(%arg0: !hw.typealias<@__hw_typedecls::@foo,i2>) {}
