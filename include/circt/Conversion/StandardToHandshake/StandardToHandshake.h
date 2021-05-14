//===- StandardToHandshake.h ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares passes which together will lower the Standard dialect to
// Handshake dialect.
//
//===----------------------------------------------------------------------===//

#ifndef CIRCT_CONVERSION_STANDARDTOHANDSHAKE_H_
#define CIRCT_CONVERSION_STANDARDTOHANDSHAKE_H_

#include <memory>

namespace mlir {
class ModuleOp;
template <typename T>
class OperationPass;
} // namespace mlir

namespace circt {

namespace handshake {
class FuncOp;
} // namespace handshake

std::unique_ptr<mlir::OperationPass<mlir::ModuleOp>>
createHandshakeCleanFuncArgPass();

std::unique_ptr<mlir::OperationPass<mlir::ModuleOp>>
createHandshakeAnalysisPass();

std::unique_ptr<mlir::OperationPass<mlir::ModuleOp>>
createHandshakeDataflowPass();

std::unique_ptr<mlir::OperationPass<handshake::FuncOp>>
createHandshakeCanonicalizePass();

std::unique_ptr<mlir::OperationPass<handshake::FuncOp>>
createHandshakeRemoveBlockPass();

std::unique_ptr<mlir::OperationPass<handshake::FuncOp>>
createHandshakeInsertBufferPass();

} // namespace circt

#endif // MLIR_CONVERSION_STANDARDTOHANDSHAKE_H_
