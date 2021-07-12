//===- Passes.h - FIRRTL pass entry points ----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This header file defines prototypes that expose pass constructors.
//
//===----------------------------------------------------------------------===//

#ifndef CIRCT_DIALECT_FIRRTL_PASSES_H
#define CIRCT_DIALECT_FIRRTL_PASSES_H

#include "llvm/ADT/Optional.h"
#include <memory>

namespace mlir {
class Pass;
} // namespace mlir

namespace circt {
namespace firrtl {

std::unique_ptr<mlir::Pass> createLowerFIRRTLTypesPass();

std::unique_ptr<mlir::Pass> createLowerBundleVectorTypesPass();

std::unique_ptr<mlir::Pass> createIMConstPropPass();

std::unique_ptr<mlir::Pass> createInlinerPass();

std::unique_ptr<mlir::Pass> createBlackBoxMemoryPass();

std::unique_ptr<mlir::Pass> createExpandWhensPass();

std::unique_ptr<mlir::Pass> createInferWidthsPass();

std::unique_ptr<mlir::Pass> createPrintInstanceGraphPass();

std::unique_ptr<mlir::Pass>
createBlackBoxReaderPass(llvm::Optional<StringRef> inputPrefix = {},
                         llvm::Optional<StringRef> resourcePrefix = {});

std::unique_ptr<mlir::Pass> createGrandCentralPass();

std::unique_ptr<mlir::Pass> createGrandCentralTapsPass();

std::unique_ptr<mlir::Pass> createCheckCombCyclesPass();

/// Generate the code for registering passes.
#define GEN_PASS_REGISTRATION
#include "circt/Dialect/FIRRTL/Passes.h.inc"

} // namespace firrtl
} // namespace circt

#endif // CIRCT_DIALECT_FIRRTL_PASSES_H
