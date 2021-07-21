//===- Seq.td - Seq dialect definition ---------------------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This is the top level file for the Seq dialect. It contains the one op and
// pass. Once we add more than one, we should break it out like the other
// dialects.
//
//===----------------------------------------------------------------------===//

#ifndef SEQ_TD
#define SEQ_TD

include "mlir/IR/OpBase.td"
include "mlir/Interfaces/SideEffectInterfaces.td"
include "mlir/Pass/PassBase.td"

// Include this file to pick up the enumerations exposed by the SystemVerilog dialect.
include "circt/Dialect/SV/SVEnums.td"

def SeqDialect : Dialect {
  let name = "seq";

  let summary = "Types and operations for seq dialect";
  let description = [{
    The `seq` dialect is intended to model digital sequential logic.
  }];

  let hasConstantMaterializer = 1;
  let cppNamespace = "::circt::seq";
}

// Base class for the operation in this dialect.
class SeqOp<string mnemonic, list<OpTrait> traits = []> :
    Op<SeqDialect, mnemonic, traits>;

def CompRegOp : SeqOp<"compreg",
    [NoSideEffect, AllTypesMatch<["input", "data"/*, "resetValue"*/]>,
     SameVariadicOperandSize]> {
       // AllTypesMatch doesn't work with Optional types yet.

  let summary = "Register a value, storing it for one cycle";
  let description = "See the Seq dialect rationale for a longer description";

  let arguments = (ins AnyType:$input, I1:$clk,
    Optional<I1>:$reset, Optional<AnyType>:$resetValue);
  let results = (outs AnyType:$data);

  let printer = "return ::print$cppClass(p, *this);";
  let parser = "return ::parse$cppClass(parser, result);";
}

def RegOp : SeqOp<"reg",
    [AllTypesMatch<["input", "data"]>, SameVariadicOperandSize]> {

  let summary = "A complete implementation of registers.";
  let description = "See the Seq dialect rationale for a longer description";

  let arguments = (ins AnyType:$input, I1:$clk,
    I1:$enable,
    Optional<I1>:$reset, Optional<AnyType>:$resetValue,
    EventControlAttr:$clockEdge,
    OptionalAttr<EventControlAttr>:$resetEdge,
    ResetTypeAttr:$resetType
    );

  let results = (outs AnyType:$data);

  let assemblyFormat = "$input `,` $clockEdge $clk `,` `enable` $enable `,` $resetType ($resetEdge^ $reset `,` $resetValue `:` type($resetValue))? attr-dict `:`  type($input)";

  let verifier = "return ::verifyReg(*this);";
}

def LowerSeqToSV: Pass<"lower-seq-to-sv", "mlir::ModuleOp"> {
  let summary = "Lower sequential ops to SV.";
  let constructor = "circt::seq::createSeqLowerToSVPass()";
  let dependentDialects = ["circt::sv::SVDialect"];
}

#endif // SEQ_TD
