//===-- Passes.td - FIRRTL pass definition file ------------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the passes that work on the FIRRTL dialect.
//
//===----------------------------------------------------------------------===//

#ifndef CIRCT_DIALECT_FIRRTL_PASSES_TD
#define CIRCT_DIALECT_FIRRTL_PASSES_TD

include "mlir/Pass/PassBase.td"

def LowerFIRRTLTypes : Pass<"firrtl-lower-types", "firrtl::CircuitOp"> {
  let summary = "Lower FIRRTL types to ground types";
  let description = [{
    Lower firrtl.module port types to ground types.
  }];
  let constructor = "circt::firrtl::createLowerFIRRTLTypesPass()";
}

def IMConstProp : Pass<"firrtl-imconstprop", "firrtl::CircuitOp"> {
  let summary = "Intermodule constant propagation and dead code elimination";
  let description = [{
    Use optimistic constant propagation to delete ports and unreachable IR.
  }];
  let constructor = "circt::firrtl::createIMConstPropPass()";
}

def Inliner : Pass<"firrtl-inliner", "firrtl::CircuitOp"> {
  let summary = "Performs inlining, flattening, and dead module elimination";
  let description = [{
    This inliner pass will inline any instance of module marked as inline, and
    recursively inline all instances inside of a module marked with flatten.
    This pass performs renaming of every entity with a name that is inlined by
    prefixing it with the instance name.  This pass also will remove any module
    which is not reachable from the top level module.

    The inline and flatten annotation attributes are attached to module
    definitions, and they are:
    ```mlir
      {class = "firrtl.passes.InlineAnnotation"}
      {class = "firrtl.transforms.FlattenAnnotation"}
    ```
  }];
  let constructor = "circt::firrtl::createInlinerPass()";
}

def BlackboxMemory : Pass<"firrtl-blackbox-memory", "firrtl::CircuitOp"> {
  let summary = "Replace all FIRRTL memories with an external module blackbox.";
  let description = [{
    This pass replaces all sequential memory operations with an external module
    blackbox.  For each memory operation, it creates a FModuleOp which
    replicates the return types of the memory operation, and replaces the
    MemoryOp with an instance of the module.  Inside the new module, an
    instance of an FExtModule blackbox is created.  The blackbox module must
    use the same parameter naming conventions used by the ReplaceSeqMemories
    pass in the Scala FIRRTL compiler.
  }];
  let constructor = "circt::firrtl::createBlackBoxMemoryPass()";
  let options = [
    Option<"emitWrapper", "emit-wrapper", "bool", "true",
           "Create a wrapper module around the blackbox external module.">
  ];
}

def ExpandWhens : Pass<"firrtl-expand-whens", "firrtl::FModuleOp"> {
  let summary = "Remove all when conditional blocks.";
  let description = [{
    This pass will:
    1. Resolve last connect semantics.
    2. Remove all when operations.

    When a wire has multiple connections, only the final connection is used,
    all previous connections are overwritten. When there is a conditional
    connect, the previous connect is only overwritten when the condition
    holds:

    ```firrtl
    w <= a
    when c :
      w <= b

    ; Equivalent to:
    w <= mux(c, b, a)
    ```

    This pass requires that all connects are expanded.
  }];
  let constructor = "circt::firrtl::createExpandWhensPass()";
}

def CheckWidths : Pass<"firrtl-check-widths", "firrtl::FModuleOp"> {
  let summary = "Verify that types have a well-known width";
  let description = [{
    This pass checks if the widths of all types throughout a FIRRTL module are
    well-known.
  }];
  let constructor = "circt::firrtl::createCheckWidthsPass()";
}

def InferWidths : Pass<"firrtl-infer-widths", "firrtl::CircuitOp"> {
  let summary = "Infer the width of types";
  let description = [{
    This pass infers the widths of all types throughout a FIRRTL module.
  }];
  let constructor = "circt::firrtl::createInferWidthsPass()";
}

def BlackBoxReader : Pass<"firrtl-blackbox-reader", "CircuitOp"> {
  let summary = "Load source files for black boxes into the IR";
  let description = [{
    This pass handles reads the Verilog source files for black boxes and adds
    them as `sv.verbatim.file` operations into the IR. Later passes can then
    write these files back to disk to ensure that they can be accessed by other
    tools down the line in a well-known location. Supports inline, resource, and
    path annotations for black box source files.

    The supported `firrtl.circuit` annotations are:

    - `{class = "firrtl.transforms.BlackBoxTargetDirAnno", targetDir = "..."}`
      Overrides the target directory into which black box source files are
      emitted.
    - `{class = "firrtl.transforms.BlackBoxResourceFileNameAnno", resourceFileName = "xyz.f"}`
      Specifies the output file name for the list of black box source files that
      is generated as a collateral of the pass.

    The supported `firrtl.extmodule` annotations are:

    - ```
      {
        class = "firrtl.transforms.BlackBoxInlineAnno",
        name = "myfile.v",
        text = "..."
      }
      ```
      Specifies the black box source code (`text`) inline. Generates a file with
      the given `name` in the target directory.
    - ```
      {
        class = "firrtl.transforms.BlackBoxPathAnno",
        path = "myfile.v"
      }
      ```
      Specifies the file `path` as source code for the module. Copies the file
      to the target directory.
    - ```
      {
        class = "firrtl.transforms.BlackBoxResourceAnno",
        resourceId = "myfile.v"
      }
      ```
      Specifies the file `path` as source code for the module. In contrast to
      the `BlackBoxPathAnno`, the file is searched for in the black box resource
      search path. This is a remnant of the Scala origins of FIRRTL. Copies the
      file to the target directory.
  }];

  let constructor = "circt::firrtl::createBlackBoxReaderPass()";
  let options = [
    Option<"inputPrefix", "input-prefix", "std::string", "",
      "Prefix for input paths in black box annotations. This should be the "
      "directory where the input file was located, to allow for annotations "
      "relative to the input file.">,
    Option<"resourcePrefix", "resource-prefix", "std::string",
      "\"src/main/resources\"",
      "Search path for black box sources specified via the "
      "`BlackBoxResourceAnno` annotation.">
  ];
  let dependentDialects = ["sv::SVDialect"];
}

def PrefixModules : Pass<"firrtl-prefix-modules", "firrtl::CircuitOp"> {
  let summary = "Prefixes names of modules and mems in a hiearchy";
  let description = [{
    This pass looks for modules annotated with the
    `NestedPrefixModulesAnnotation` and prefixes the names of all modules
    instantiated underneath it. This pass will duplicate modules as necessary
    to give submodules unique names. The annotation can be attached to module
    definitions, as well as specific instances.

    The supported annotation is:
    ```
      {
        class = "sifive.enterprise.firrtl.NestedPrefixModulesAnnotation",
        prefix = "MyPrefix_",
        inclusive = true
      }
    ```

    If `inclusive` is false, it will not attach the prefix to target module,
    only to modules instantiated underneath it.
  }];
  let constructor = "circt::firrtl::createPrefixModulesPass()";
}

#endif // CIRCT_DIALECT_FIRRTL_PASSES_TD
