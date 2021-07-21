//===- SVStatements.td - SV statements ops -----------------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This describes the ops for System Verilog statements.
//
//===----------------------------------------------------------------------===//

/// Ensure symbol is one of the hw module.* types
def isModuleSymbol : AttrConstraint<
  CPred<
    "hw::isAnyModule(::mlir::SymbolTable::lookupNearestSymbolFrom("
      "&$_op, $_self.cast<::mlir::FlatSymbolRefAttr>().getValue()))"
  >, "is module like">;

//===----------------------------------------------------------------------===//
// Control flow like-operations
//===----------------------------------------------------------------------===//

def IfDefOp : SVOp<"ifdef", [SingleBlock, NoTerminator, NoRegionArguments,
                             NonProceduralOp]> {
  let summary = "'ifdef MACRO' block";

  let description = [{
    This operation is an #ifdef block, which has a "then" and "else" region.
    This operation is for non-procedural regions and its body is non-procedural.
  }];

  let regions = (region SizedRegion<1>:$thenRegion, AnyRegion:$elseRegion);
  let arguments = (ins StrAttr:$cond);
  let results = (outs);

  let hasCanonicalizeMethod = true;
  let assemblyFormat = "$cond $thenRegion (`else` $elseRegion^)? attr-dict";

  // TODO: ODS forces using a custom builder just to get the region terminator
  // implicitly installed.
  let skipDefaultBuilders = 1;
  let builders = [
    OpBuilder<(ins "StringRef":$cond,
                      CArg<"std::function<void()>", "{}">:$thenCtor,
                      CArg<"std::function<void()>", "{}">:$elseCtor)>,
    OpBuilder<(ins "StringAttr":$cond,
                      CArg<"std::function<void()>", "{}">:$thenCtor,
                      CArg<"std::function<void()>", "{}">:$elseCtor)>
  ];

  let extraClassDeclaration = [{
    /// Returns true if an else block exists.
    bool hasElse() { return !elseRegion().empty(); }

    Block *getThenBlock() {
      assert(!thenRegion().empty() && "Unexpected empty 'then' region.");
      return &thenRegion().front();
    }

    Block *getElseBlock() {
      assert(hasElse() && "Empty 'else' region.");
      return &elseRegion().front();
    }
  }];
}

def IfDefProceduralOp
  : SVOp<"ifdef.procedural", [SingleBlock, NoTerminator, NoRegionArguments,
                              ProceduralRegion, ProceduralOp]> {
  let summary = "'ifdef MACRO' block for procedural regions";

  let description = [{
    This operation is an #ifdef block, which has a "then" and "else" region.
    This operation is for procedural regions and its body is procedural.
  }];

  let regions = (region SizedRegion<1>:$thenRegion, AnyRegion:$elseRegion);
  let arguments = (ins StrAttr:$cond);
  let results = (outs);

  let assemblyFormat = "$cond $thenRegion (`else` $elseRegion^)? attr-dict";

  // TODO: ODS forces using a custom builder just to get the region terminator
  // implicitly installed.
  let skipDefaultBuilders = 1;
  let builders = [
    OpBuilder<(ins "StringRef":$cond,
                      CArg<"std::function<void()>", "{}">:$thenCtor,
                      CArg<"std::function<void()>", "{}">:$elseCtor)>
  ];

  let extraClassDeclaration = [{
    /// Returns true if an else block exists.
    bool hasElse() { return !elseRegion().empty(); }

    Block *getThenBlock() {
      assert(!thenRegion().empty() && "Unexpected empty 'then' region.");
      return &thenRegion().front();
    }

    Block *getElseBlock() {
      assert(hasElse() && "Empty 'else' region.");
      return &elseRegion().front();
    }
  }];
}

def IfOp : SVOp<"if", [SingleBlock, NoTerminator, NoRegionArguments,
                       ProceduralRegion, ProceduralOp]> {
  let summary = "'if (cond)' block";

  let regions = (region SizedRegion<1>:$thenRegion, AnyRegion:$elseRegion);
  let arguments = (ins I1:$cond);
  let results = (outs);

  let hasCanonicalizeMethod = true;
  let assemblyFormat = "$cond $thenRegion (`else` $elseRegion^)? attr-dict";

  // TODO: ODS forces using a custom builder just to get the region terminator
  // implicitly installed.
  let skipDefaultBuilders = 1;
  let builders = [
    OpBuilder<(ins "Value":$cond,
                      CArg<"std::function<void()>", "{}">:$thenCtor,
                      CArg<"std::function<void()>", "{}">:$elseCtor)>
  ];

  let extraClassDeclaration = [{
    /// Returns true if an else block exists.
    bool hasElse() { return !elseRegion().empty(); }

    Block *getThenBlock() {
      assert(!thenRegion().empty() && "Unexpected empty 'then' region.");
      return &thenRegion().front();
    }

    Block *getElseBlock() {
      assert(hasElse() && "Empty 'else' region.");
      return &elseRegion().front();
    }
  }];
}

def AlwaysOp : SVOp<"always", [SingleBlock, NoTerminator, NoRegionArguments,
                               RecursiveSideEffects, ProceduralRegion,
                               NonProceduralOp]> {
  let summary = "'always @' block";
  let description = "See SV Spec 9.2, and 9.4.2.2.";

  let regions = (region SizedRegion<1>:$body);
  let arguments = (ins TypedArrayAttrBase<EventControlAttr, "events">:$events,
                       Variadic<I1>:$clocks);
  let results = (outs);

  let assemblyFormat = "custom<EventList>($events, $clocks) $body attr-dict";

  let skipDefaultBuilders = 1;
  let builders = [
    OpBuilder<(ins "ArrayRef<EventControl>":$event, "ArrayRef<Value>":$cond,
                      CArg<"std::function<void()>", "{}">:$bodyCtor)>
  ];

  let extraClassDeclaration = [{
    Block *getBodyBlock() { return &body().front(); }

    struct Condition {
      EventControl event;
      Value value;
    };

    size_t getNumConditions() { return events().size(); }
    Condition getCondition(size_t idx);
  }];

  // Check that we have the same number of events and conditions.
  let verifier = "return ::verifyAlwaysOp(*this);";
}

def AlwaysCombOp : SVOp<"alwayscomb", [SingleBlock, NoTerminator,
                                       NoRegionArguments, RecursiveSideEffects,
                                       ProceduralRegion, NonProceduralOp]> {
  let summary = "'alwayscomb block";
  let description = "See SV Spec 9.2, and 9.2.2.2.";

  let regions = (region SizedRegion<1>:$body);
  let arguments = (ins);
  let results = (outs);

  let assemblyFormat = "$body attr-dict";

  let skipDefaultBuilders = 1;
  let builders = [
    OpBuilder<(ins CArg<"std::function<void()>", "{}">:$bodyCtor)>
  ];

  let extraClassDeclaration = [{
    Block *getBodyBlock() { return &body().front(); }
  }];
}


def AlwaysFFOp : SVOp<"alwaysff", [SingleBlock, NoTerminator, NoRegionArguments,
                                   RecursiveSideEffects, ProceduralRegion,
                                   NonProceduralOp]> {
  let summary = "'alwaysff @' block with optional reset";
  let description = [{
    alwaysff blocks represent always_ff verilog nodes, which enforce inference
    of registers.  This block takes a clock signal and edge sensitivity and
    reset type.  If the reset type is anything but 'noreset', the block takes a
    reset signal, reset sensitivity, and reset block.  Appropriate if conditions
    are generated in the output code based on the reset type.  A negative-edge,
    asynchronous reset will check the inverse of the reset condition
    (if (!reset) begin resetblock end) to match the sensitivity.
  }];

  let regions = (region SizedRegion<1>:$bodyBlk, AnyRegion:$resetBlk);
  let arguments = (ins EventControlAttr:$clockEdge, I1:$clock,
                       DefaultValuedAttr<ResetTypeAttr,
                                         "ResetType::NoReset">:$resetStyle,
                       OptionalAttr<EventControlAttr>:$resetEdge,
                       Optional<I1>:$reset);
  let results = (outs);

  let assemblyFormat = [{
    `(` $clockEdge $clock `)` $bodyBlk
    ( `(` $resetStyle `:` $resetEdge^ $reset `)` $resetBlk )? attr-dict
  }];

  let skipDefaultBuilders = 1;
  let builders = [
    OpBuilder<(ins "EventControl":$clockEdge, "Value":$clock,
                      CArg<"std::function<void()>", "{}">:$bodyCtor)>,
    OpBuilder<(ins "EventControl":$clockEdge, "Value":$clock,
                      "ResetType":$resetStyle,
                      "EventControl":$resetEdge, "Value":$reset,
                      CArg<"std::function<void()>", "{}">:$bodyCtor,
                      CArg<"std::function<void()>", "{}">:$resetCtor)>
  ];

  let extraClassDeclaration = [{
    Block *getBodyBlock() { return &bodyBlk().front(); }
    Block *getResetBlock() { return &resetBlk().front(); }
  }];
}


def InitialOp : SVOp<"initial", [SingleBlock, NoTerminator, NoRegionArguments,
                                 RecursiveSideEffects, ProceduralRegion,
                                 NonProceduralOp]> {
  let summary = "'initial' block";
  let description = "See SV Spec 9.2.1.";

  let regions = (region SizedRegion<1>:$body);
  let arguments = (ins);
  let results = (outs);

  let assemblyFormat = "$body attr-dict";

  // TODO: ODS forces using a custom builder just to get the region terminator
  // implicitly installed.
  let skipDefaultBuilders = 1;
  let builders = [
    OpBuilder<(ins CArg<"std::function<void()>", "{}">:$bodyCtor)>
  ];

  let extraClassDeclaration = [{
    Block *getBodyBlock() { return &body().front(); }
  }];
}

def CaseZOp : SVOp<"casez", [SingleBlock, NoTerminator, NoRegionArguments,
                             ProceduralRegion, ProceduralOp]> {
  let summary = "'casez (cond)' block";
  let description = "See SystemVerilog 2017 12.5.1.";

  let regions = (region VariadicRegion<SizedRegion<1>>:$caseRegions);
  let arguments = (ins HWIntegerType:$cond, ArrayAttr:$casePatterns);
  let results = (outs);

  let parser = "return parseCaseZOp(parser, result);";
  let printer = "printCaseZOp(p, *this);";
  let verifier = "return ::verifyCaseZOp(*this);";

  let extraClassDeclaration = [{
    /// This describes the bit in a pattern, 0/1/x.
    enum PatternBit { PatternZero = 0, PatternOne = 1, PatternAny = 2 };

    static char getLetter(PatternBit bit, bool isVerilog) {
      switch (bit) {
      case PatternZero: return '0';
      case PatternOne: return '1';
      case PatternAny: return isVerilog ? '?' : 'x';
      }
      llvm_unreachable("invalid casez PatternBit");
    }

    // This is provides convenient access to encode and decode a pattern.
    struct CasePattern {
      IntegerAttr attr;

      // Return the number of bits in the pattern.
      size_t getWidth() const {
        return attr.getValue().getBitWidth()/2;
      }

      /// Return the specified bit, bit 0 is the least significant bit.
      PatternBit getBit(size_t bitNumber) const;

      /// Return true if this pattern always matches.
      bool isDefault() const;

      // Get a CasePattern from a specified list of PatternBits.  Bits are
      // specified in most least significant order - element zero is the least
      // significant bit.
      CasePattern(ArrayRef<PatternBit> bits, MLIRContext *context);
      CasePattern(IntegerAttr attr) : attr(attr) {}
    };
    // This provides information about one case.
    struct CaseInfo {
      CasePattern pattern;
      Block *block;
    };

    SmallVector<CaseInfo, 4> getCases();

    // void addCase(...)
  }];
}

//===----------------------------------------------------------------------===//
// Assignment Related Statements
//===----------------------------------------------------------------------===//

def AssignOp : SVOp<"assign", [InOutTypeConstraint<"src", "dest">,
                                 NonProceduralOp]> {
  let summary = "Continuous assignment";
  let description = [{
    A SystemVerilog assignment statement 'x = y;'.
    These occur in module scope.  See SV Spec 10.3.2.
  }];
  let arguments = (ins InOutType:$dest, InOutElementType:$src);
  let results = (outs);
  let assemblyFormat = "$dest `,` $src  attr-dict `:` type($src)";
}

def BPAssignOp : SVOp<"bpassign", [InOutTypeConstraint<"src", "dest">,
                                   ProceduralOp]> {
  let summary = "Blocking procedural assignment";
  let description = [{
    A SystemVerilog blocking procedural assignment statement 'x = y;'.  These
    occur in initial, always, task, and function blocks.  The statement is
    executed before any following statements are. See SV Spec 10.4.1.
  }];
  let arguments = (ins InOutType:$dest, InOutElementType:$src);
  let results = (outs);
  let assemblyFormat = "$dest `,` $src  attr-dict `:` type($src)";
}

def PAssignOp : SVOp<"passign", [InOutTypeConstraint<"src", "dest">,
                                 ProceduralOp]> {
  let summary = "Nonblocking procedural assignment";
  let description = [{
    A SystemVerilog nonblocking procedural assignment statement 'x <= y;'.
    These occur in initial, always, task, and function blocks.  The statement
    can be scheduled without blocking procedural flow.  See SV Spec 10.4.2.
  }];
  let hasCanonicalizeMethod = true;
  let arguments = (ins InOutType:$dest, InOutElementType:$src);
  let results = (outs);
  let assemblyFormat = "$dest `,` $src  attr-dict `:` type($src)";
}



//===----------------------------------------------------------------------===//
// Other Statements
//===----------------------------------------------------------------------===//

def ForceOp : SVOp<"force", [InOutTypeConstraint<"src", "dest">,
                                   ProceduralOp]> {
  let summary = "Force procedural statement";
  let description = [{
    A SystemVerilog force procedural statement 'force x = y;'.  These
    occur in initial, always, task, and function blocks.
    A force statement shall override a procedural assignment until
    a release statement is executed on the variable.
    The left-hand side of the assignment can be avariable, a net,
    a constant bit-select of a vector net, a part-select of a vector
    net or a concatenation. It cannot be a memory word or a bit-select
    or part-select of a vector variable. See SV Spec 10.6.2.
  }];
  let arguments = (ins InOutType:$dest, InOutElementType:$src);
  let results = (outs);
  let assemblyFormat = "$dest `,` $src  attr-dict `:` type($src)";
}

def ReleaseOp : SVOp<"release", [ProceduralOp]> {
  let summary = "Release procedural statement";
  let description = [{
    Release is used in conjunction with force. When released,
    then if the variable does not currently have an active assign
    procedural continuous assignment, the variable shall not immediately
    change value. The variable shall maintain its current value until
    the next procedural assignment or procedural continuous assignment
    to the variable. Releasing a variable that currently has an
    active assign procedural continuous assignment shall immediately
    reestablish that assignment. See SV Spec 10.6.2.
  }];
  let arguments = (ins InOutType:$dest);
  let results = (outs);
  let assemblyFormat = "$dest attr-dict `:` type($dest)";
}

def AliasOp : SVOp<"alias"> {
  let summary = "SystemVerilog 'alias' statement";
  let description = [{
    An alias statement declares multiple names for the same physical net, or
    bits within a net.  Aliases always have at least two operands.
  }];

  let arguments = (ins Variadic<InOutType>:$operands);
  let results = (outs);

  let assemblyFormat = "$operands attr-dict `:` type($operands)";

  // Verify that we have at least two operands.
  let verifier = "return ::verifyAliasOp(*this);";
}

// TODO: This needs to model the file descriptor to write on.  This is currently
// hard coded to 32'h80000002.
def FWriteOp : SVOp<"fwrite", [ProceduralOp]> {
  let summary = "'$fwrite' statement";

  let arguments = (ins StrAttr:$string, Variadic<AnyType>:$operands);
  let results = (outs);

  let assemblyFormat =
    "$string attr-dict (`(` $operands^ `)` `:` type($operands))?";
}

def FinishOp : SVOp<"finish", [ProceduralOp]> {
  let summary = "'$finish' statement";
  let description = [{
    Finishes a simulation and exits the simulation process.
  }];

  let arguments = (ins);
  let results = (outs);

  let assemblyFormat = "attr-dict";
}

def FatalOp : SVOp<"fatal", [ProceduralOp]> {
  let summary = "'$fatal' statement";
  let description = [{
    Run-time fatal assertion error.
  }];

  let arguments = (ins);
  let results = (outs);

  let assemblyFormat = "attr-dict";
}

def VerbatimOp : SVOp<"verbatim"> {
  let summary = "Verbatim opaque text emitted inline.";
  let description = [{
    This operation produces opaque text inline in the SystemVerilog output.

    sv.verbatim allows operand substitutions with {{0}} syntax.
  }];
  let arguments = (ins StrAttr:$string, Variadic<AnyType>:$operands);

  let assemblyFormat =
    "$string attr-dict (`(` $operands^ `)` `:` type($operands))?";

  let builders = [
    OpBuilder<(ins "Twine":$string),
                 "build(odsBuilder, odsState, "
                 "odsBuilder.getStringAttr(string), ValueRange{});">,
    OpBuilder<(ins "StringAttr":$string),
                 "build(odsBuilder, odsState, string, ValueRange{});">
  ];
}

//===----------------------------------------------------------------------===//
// Verification Statements
//===----------------------------------------------------------------------===//

class ImmediateAssertionOp<string mnemonic, list<OpTrait> traits = []> :
    SVOp<mnemonic, traits> {
  let arguments = (ins I1:$expression, StrAttr:$label);
  let results = (outs);
  let assemblyFormat =
    "custom<OmitEmptyStringAttr>($label) attr-dict $expression `:` type($expression)";
}

def AssertOp : ImmediateAssertionOp<"assert", [ProceduralOp]> {
  let summary = "immediate assertion statement";
  let description = [{
    Specify that a Boolean expression is always true.  This can be used to both
    document the behavior of the design and to test that the design behaves as
    expected.  See Section 16.3 of the SystemVerilog 2017 specification.
  }];
}

def AssumeOp : ImmediateAssertionOp<"assume", [ProceduralOp]> {
  let summary = "immediate assume statement";
  let description = [{
    Specify that a Boolean expression is assumed to always be true.  This can
    either be used as an assertion-like check that the expression is, in fact,
    always true or to bound legal input values during testing.  See Section 16.3
    of the SystemVerilog 2017 specification.
  }];
}

def CoverOp : ImmediateAssertionOp<"cover", [ProceduralOp]> {
  let summary = "immediate cover statement";
  let description = [{
    Specify that a Boolean expression should be monitored for coverage, i.e., a
    simulator will watch if it occurs and how many times it occurs.  See section
    16.3 of the SystemVerilog 2017 specification.
  }];
}

class ConcurrentAssertionOp<string mnemonic, list<OpTrait> traits = []> :
    SVOp<mnemonic, traits> {
  let arguments =
    (ins EventControlAttr:$event, I1:$clock, I1:$property, StrAttr:$label);
  let results = (outs);
  let assemblyFormat =
    "custom<OmitEmptyStringAttr>($label) $event $clock $property attr-dict `:`"
    " type($property)";
}

def AssertConcurrentOp : ConcurrentAssertionOp<"assert.concurrent"> {
  let summary = "concurrent assertion statement, i.e., assert property";
  let description = [{
    Specify that a property of the hardware design is true whenever the property
    is evaluated.  This can be used to both document the behavior of the design
    and to test that the design behaves as expected.  See section 16.5 of the
    SystemVerilog 2017 specification.
  }];
}

def AssumeConcurrentOp : ConcurrentAssertionOp<"assume.concurrent"> {
  let summary = "concurrent assume statement, i.e., assume property";
  let description = [{
    Specify that a property is assumed to be true whenever the property is
    evaluated.  This can be used to both document the behavior of the design and
    to test that the design behaves as expected.  See section 16.5 of the
    SystemVerilog 2017 specification.
  }];
}

def CoverConcurrentOp : ConcurrentAssertionOp<"cover.concurrent"> {
  let summary = "concurrent cover statement, i.e., cover property";
  let description = [{
    Specify that a specific property should be monitored for coverage, i.e., a
    simulation will watch if it occurrs and how many times it occurs.  See
    section 16.5 of the SystemVerilog 2017 specification.
  }];
}

def BindOp : SVOp<"bind", []> {
  let summary = "indirect instantiation statement";
  let description = [{
    Indirectly instantiate a module in the context of another module. This
    operation pairs with rtl.instance which tracks all information except the
    emission point for the bind.  This requires that the parent module of the
    bind exist in the IR. See 23.11 of SV 2017 spec.
  }];

  let arguments = (ins FlatSymbolRefAttr:$bind);
  let results = (outs);
  let verifier = "return ::verify$cppClass(*this);";

  let assemblyFormat = [{ $bind attr-dict }];

  let extraClassDeclaration = [{
    /// Lookup the instance for the bind.  This returns null on
    /// invalid IR.
    hw::InstanceOp getReferencedInstance();
  }];
}
