//===- FIRParser.h - .fir to FIRRTL dialect parser --------------*- C++ -*-===//
//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Defines the interface to the .fir file parser.
//
//===----------------------------------------------------------------------===//

#ifndef CIRCT_DIALECT_FIRRTL_FIRPARSER_H
#define CIRCT_DIALECT_FIRRTL_FIRPARSER_H

namespace llvm {
class SourceMgr;
class StringRef;
} // namespace llvm

namespace mlir {
class MLIRContext;
class OwningModuleRef;
class OpBuilder;
class Value;
class Block;
class Location;
template <class T>
class FailureOr;
} // namespace mlir

namespace circt {
namespace firrtl {

struct FIRParserOptions {
  /// If this is set to true, the @info locators are ignored, and the locations
  /// are set to the location in the .fir file.
  bool ignoreInfoLocators = false;
};

mlir::OwningModuleRef importFIRRTL(llvm::SourceMgr &sourceMgr,
                                   mlir::MLIRContext *context,
                                   FIRParserOptions options = {});

void registerFromFIRRTLTranslation();

/// Parse a string as a UInt/SInt literal.
mlir::FailureOr<mlir::Value> parseIntegerLiteralExp(mlir::Block *into,
                                                    llvm::StringRef input,
                                                    mlir::Location loc);

} // namespace firrtl
} // namespace circt

#endif // CIRCT_DIALECT_FIRRTL_FIRPARSER_H
