//===-- Passes.td - Conversion pass definitions ------------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains definitions for all dialect conversions.
//
//===----------------------------------------------------------------------===//

#ifndef CIRCT_CONVERSION_PASSES_TD
#define CIRCT_CONVERSION_PASSES_TD

include "mlir/Pass/PassBase.td"

//===----------------------------------------------------------------------===//
// FIRRTLToLLHD
//===----------------------------------------------------------------------===//

def ConvertFIRRTLToLLHD : Pass<"convert-firrtl-to-llhd", "mlir::ModuleOp"> {
  let summary = "Convert FIRRTL to LLHD";
  let description = [{
    This pass translates a FIRRTL design into an equivalent structural LLHD
    description.
  }];
  let constructor = "circt::createConvertFIRRTLToLLHDPass()";
  let dependentDialects = ["llhd::LLHDDialect"];
}

//===----------------------------------------------------------------------===//
// FIRRTLToRTL
//===----------------------------------------------------------------------===//

def LowerFIRRTLToRTL : Pass<"lower-firrtl-to-rtl", "mlir::ModuleOp"> {
  let summary = "Lower FIRRTL to RTL";
  let description = [{
    Lower a module of FIRRTL dialect to the RTL dialect family.
  }];
  let constructor = "circt::createLowerFIRRTLToRTLPass()";
  let dependentDialects = ["comb::CombDialect", "rtl::RTLDialect",
                           "sv::SVDialect"];
}

//===----------------------------------------------------------------------===//
// HandshakeToFIRRTL
//===----------------------------------------------------------------------===//

def HandshakeToFIRRTL : Pass<"lower-handshake-to-firrtl", "mlir::ModuleOp"> {
  let summary = "Lower Handshake to FIRRTL";
  let description = [{
    Lower Handshake to FIRRTL.
  }];
  let constructor = "circt::createHandshakeToFIRRTLPass()";
  let dependentDialects = ["firrtl::FIRRTLDialect"];
}

//===----------------------------------------------------------------------===//
// LLHDToLLVM
//===----------------------------------------------------------------------===//

def ConvertLLHDToLLVM : Pass<"convert-llhd-to-llvm", "mlir::ModuleOp"> {
    let summary = "Convert LLHD to LLVM";
    let description = [{
        This pass translates LLHD to LLVM. 
    }];

    let constructor = "circt::createConvertLLHDToLLVMPass()";
    let dependentDialects = ["mlir::LLVM::LLVMDialect"];
}

//===----------------------------------------------------------------------===//
// RTLToLLHD
//===----------------------------------------------------------------------===//

def ConvertRTLToLLHD : Pass<"convert-rtl-to-llhd", "mlir::ModuleOp"> {
  let summary = "Convert RTL to LLHD";
  let description = [{
    This pass translates a RTL design into an equivalent structural LLHD
    description.
  }];
  let constructor = "circt::createConvertRTLToLLHDPass()";
  let dependentDialects = ["llhd::LLHDDialect"];
}

//===----------------------------------------------------------------------===//
// StandardToHandshake
//===----------------------------------------------------------------------===//

def HandshakeCleanFuncArg : Pass<"clean-memref-args", "mlir::ModuleOp"> {
  let summary = "Remove unused function args";
  let constructor = "circt::createHandshakeCleanFuncArgPass()";
}

def HandshakeAnalysis : Pass<"analyze-dataflow", "mlir::ModuleOp"> {
  let summary = "Print resource (operation) statistics";
  let constructor = "circt::createHandshakeAnalysisPass()";
}

def HandshakeDataflow : Pass<"create-dataflow", "mlir::ModuleOp"> {
  let summary = "Convert standard MLIR into dataflow IR";
  let constructor = "circt::createHandshakeDataflowPass()";
  let dependentDialects = ["handshake::HandshakeOpsDialect"];
}

def HandshakeCanonicalize : Pass<"canonicalize-dataflow", "handshake::FuncOp"> {
  let summary = "Canonicalize handshake IR";
  let constructor = "circt::createHandshakeCanonicalizePass()";
}

def HandshakeRemoveBlock : Pass<"remove-block-structure", "handshake::FuncOp"> {
  let summary = "Remove block structure in handshake IR";
  let constructor = "circt::createHandshakeRemoveBlockPass()";
}

def HandshakeInsertBuffer
  : Pass<"handshake-insert-buffer", "handshake::FuncOp"> {
  let summary = "Insert buffers to break graph cycles";
  let constructor = "circt::createHandshakeInsertBufferPass()";
  let options = [
    ListOption<"strategies", "strategies", "std::string",
               "List of strategies to apply. Possible values are: cycles",
               "llvm::cl::ZeroOrMore, llvm::cl::MiscFlags::CommaSeparated">
  ];
}

//===----------------------------------------------------------------------===//
// StandardToStaticLogic
//===----------------------------------------------------------------------===//

def CreatePipeline : Pass<"create-pipeline", "mlir::FuncOp"> {
  let summary = "Create StaticLogic pipeline operations";
  let constructor = "circt::createCreatePipelinePass()";
  let dependentDialects = ["staticlogic::StaticLogicDialect"];
}

#endif // CIRCT_CONVERSION_PASSES_TD
