//===-- circt-c/RTLDialect.h - C API for RTL dialect --------------*- C -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This header declares the C interface for registering and accessing the
// RTL dialect. A dialect should be registered with a context to make it
// available to users of the context. These users must load the dialect
// before using any of its attributes, operations or types. Parser and pass
// manager can load registered dialects automatically.
//
//===----------------------------------------------------------------------===//

#ifndef CIRCT_C_DIALECT_RTL_H
#define CIRCT_C_DIALECT_RTL_H

#include "mlir-c/Registration.h"

#ifdef __cplusplus
extern "C" {
#endif

MLIR_DECLARE_CAPI_DIALECT_REGISTRATION(RTL, rtl);

#ifdef __cplusplus
}
#endif

#endif // CIRCT_C_DIALECT_RTL_H