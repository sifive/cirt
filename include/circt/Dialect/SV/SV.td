//===- SV.td - SystemVerilog dialect definition ------------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This is the top level file for the SV dialect.
//
//===----------------------------------------------------------------------===//

#ifndef SV_TD
#define SV_TD

include "mlir/IR/OpBase.td"
include "mlir/IR/OpAsmInterface.td"
include "mlir/IR/SymbolInterfaces.td"
include "mlir/Interfaces/SideEffectInterfaces.td"

def SVDialect : Dialect {
  let name = "sv";

  let summary = "Types and operations for SV dialect";
  let description = [{
    This dialect defines the `sv` dialect, which represents various
    SystemVerilog-specific constructs in an AST-like representation.
  }];
  let dependentDialects = ["circt::comb::CombDialect"];
  let cppNamespace = "::circt::sv";
  let extraClassDeclaration = [{
    /// Register all SV types.
    void registerTypes();
    /// Register all SV attributes.
    void registerAttributes();
  }];
}

// Base class for the operation in this dialect.
class SVOp<string mnemonic, list<OpTrait> traits = []> :
    Op<SVDialect, mnemonic, traits>;

def ProceduralRegion : NativeOpTrait<"ProceduralRegion"> {
  let cppNamespace = "::circt::sv";
}

def ProceduralOp : NativeOpTrait<"ProceduralOp"> {
  let cppNamespace = "::circt::sv";
}

def NonProceduralOp : NativeOpTrait<"NonProceduralOp"> {
  let cppNamespace = "::circt::sv";
}

include "circt/Dialect/SV/SVEnums.td"
include "circt/Dialect/SV/SVTypes.td"
include "circt/Dialect/SV/SVAttributes.td"
include "circt/Dialect/SV/SVExpressions.td"
include "circt/Dialect/SV/SVInOutOps.td"
include "circt/Dialect/SV/SVStatements.td"
include "circt/Dialect/SV/SVTypeDecl.td"

#endif // SV_TD
