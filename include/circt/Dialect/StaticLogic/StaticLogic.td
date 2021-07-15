//===- StaticLogic.td - StaticLogic Definitions ------------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// StaticLogic Ops are defined in tablegen.
//
//===----------------------------------------------------------------------===//

#ifdef STATICLOGIC_OPS
#else
#define STATICLOGIC_OPS

#ifdef OP_BASE
#else
include "mlir/IR/OpBase.td"
#endif // OP_BASE

include "mlir/IR/SymbolInterfaces.td"
include "mlir/IR/RegionKindInterface.td"
include "mlir/Interfaces/CallInterfaces.td"
include "mlir/Interfaces/SideEffectInterfaces.td"

def StaticLogic_Dialect : Dialect {
  let name = "staticlogic";
  let cppNamespace = "::circt::staticlogic";
}

def PipelineOp : Op<StaticLogic_Dialect, "pipeline", [NoSideEffect]> {
  let summary = "pipeline operation";
  let description = [{
    The "staticlogic.pipeline" operation represents a statically scheduled
    pipeline stucture which contains several MLIR blocks. Each MLIR block is
    corresponding to a pipeline stage.
  }];

  let arguments = (ins Variadic<AnyType>);
  let results = (outs Variadic<AnyType>);
  let regions = (region AnyRegion: $body);

  let hasCanonicalizer = 1;

  let skipDefaultBuilders = 1;

  let builders = [OpBuilder<(ins "ValueRange":$operands, "ValueRange":$results), [{
    SmallVector<Type, 4> argTypes;
    for (auto value : operands)
      argTypes.push_back(value.getType());

    SmallVector<Type, 4> resultTypes;
    for (auto value : results)
      resultTypes.push_back(value.getType());

    Region *bodyRegion = odsState.addRegion();
    Block *body = new Block();
    bodyRegion->push_back(body);
    body->addArguments(argTypes);

    odsState.addOperands(operands);
    odsState.addTypes(resultTypes);
  }]>];
}

def RegisterOp : Op<StaticLogic_Dialect, "register", 
    [NoSideEffect, Terminator]> {
  let summary = "register operation";
  let description = [{
    The "staticlogic.register" operation is the terminator of blocks inside of a
    pipeline region. In the further lowering, operands of this operation will be
    lowered to registers between pipeline stages.
  }];

  let arguments = (ins Variadic<AnyType>:$destOperands);
  let successors = (successor AnySuccessor:$dest);

  let builders = [OpBuilder<"OperandRange destOperands, Block *dest", [{
    odsState.addOperands(destOperands);
    odsState.addSuccessors(dest);
  }]>];

  let assemblyFormat = [{
    $dest (`(` $destOperands^ `:` type($destOperands) `)`)? attr-dict
  }];
}

def ReturnOp : Op<StaticLogic_Dialect, "return", [Terminator]> {
  let summary = "StaticLogic dialect return";
  let description = [{
    The "staticlogic.return" operation represents a terminator of a statically
    scheduled module, which is similar to a standard return operation.
  }];

  let arguments = (ins Variadic<AnyType>: $operands);
}

#endif // STATICLOGIC_OPS
