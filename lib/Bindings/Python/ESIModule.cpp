//===- ESIModule.cpp - ESI API pybind module ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "DialectModules.h"

#include "circt-c/Dialect/ESI.h"
#include "circt/Dialect/ESI/ESIDialect.h"
#include "circt/Support/LLVM.h"
#include "mlir-c/Bindings/Python/Interop.h"

#include "mlir/CAPI/IR.h"
#include "mlir/CAPI/Support.h"
#include "mlir/IR/Builders.h"

#include "llvm/ADT/SmallVector.h"

#include "PybindUtils.h"
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
namespace py = pybind11;

using namespace circt;
using namespace circt::esi;

//===----------------------------------------------------------------------===//
// Functions which should be part of the C API, but which are too difficult be
// bind to C. E.g. Functions which need complex data structures (like
// ArrayRefs). Pybind11 has conversions to/from the STL so it's easier to write
// a C++ wrapper.)
//===----------------------------------------------------------------------===//

static MlirOperation pyWrapModule(MlirOperation cModOp,
                                  std::vector<std::string> portNames) {
  mlir::Operation *modOp = unwrap(cModOp);
  SmallVector<StringRef, 8> portNamesRefs;
  for (auto name : portNames)
    portNamesRefs.push_back(name);
  SmallVector<ESIPortValidReadyMapping, 8> portTriples;
  resolvePortNames(modOp, portNamesRefs, portTriples);
  OpBuilder b(modOp);
  Operation *wrapper = buildESIWrapper(b, modOp, portTriples);
  return wrap(wrapper);
}

void circt::python::populateDialectESISubmodule(py::module &m) {
  m.doc() = "ESI Python Native Extension";

  m.def("buildWrapper", &pyWrapModule);
}