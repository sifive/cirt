//===- SV/Types.cpp - Implement the SV types ------------------------------===//
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/SV/Types.h"

#include "mlir/IR/DialectImplementation.h"
#include "llvm/ADT/TypeSwitch.h"

using namespace circt::sv;
using namespace mlir;

//===----------------------------------------------------------------------===//
// SV Interface type logic.
//===----------------------------------------------------------------------===//

Type InterfaceType::parse(MLIRContext *ctxt, DialectAsmParser &p) {
  FlatSymbolRefAttr iface;
  if (p.parseLess() || p.parseAttribute(iface) || p.parseGreater())
    return Type();
  return get(ctxt, iface);
}

void InterfaceType::print(DialectAsmPrinter &p) const {
  p << "interface<" << getInterface() << ">";
}

Type InterfaceModportType::parse(MLIRContext *ctxt, DialectAsmParser &p) {
  SymbolRefAttr modPort;
  if (p.parseLess() || p.parseAttribute(modPort) || p.parseGreater())
    return Type();
  return get(ctxt, modPort);
}

void InterfaceModportType::print(DialectAsmPrinter &p) const {
  p << "modport<" << getModport() << ">";
}

//===----------------------------------------------------------------------===//
// TableGen generated logic.
//===----------------------------------------------------------------------===//

// Provide the autogenerated implementation guts for the Op classes.
#define GET_TYPEDEF_CLASSES
#include "circt/Dialect/SV/SVTypes.cpp.inc"
