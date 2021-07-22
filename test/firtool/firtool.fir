; RUN: firtool %s --format=fir -mlir    | circt-opt | FileCheck %s --check-prefix=MLIR
; RUN: firtool %s --format=fir -mlir --annotation-file %s.anno.json | circt-opt | FileCheck %s --check-prefix=ANNOTATIONS
; RUN: firtool %s --format=fir -mlir -lower-to-hw | circt-opt | FileCheck %s --check-prefix=MLIRLOWER
; RUN: firtool %s --format=fir -verilog |             FileCheck %s --check-prefix=VERILOG

circuit test_mod : %[[{"a": "a"}]]

; MLIR: firrtl.circuit "test_mod"

; ANNOTATIONS-LABEL: firrtl.circuit "test_mod"
; ANNOTATIONS-SAME: a = "a"
; ANNOTATIONS-SAME: info = "a NoTargetAnnotation"
; ANNOTATIONS-SAME: info = "a CircuitTarget Annotation
; ANNOTATIONS-SAME: info = "a CircuitName Annotation"

  module test_mod :
    input clock : Clock
    input a: UInt<1>
    input b: UInt<2>
    output c: UInt<1>

    inst cat of Cat
    cat.a <= b
    cat.b <= b
    cat.c <= b

    inst implicitTrunc of ImplicitTrunc
    implicitTrunc.inp_1 <= a
    implicitTrunc.inp_2 <= asSInt(cat.d)

    inst prettifyExample of PrettifyExample
    prettifyExample.inp_1 <= cat.d
    prettifyExample.inp_2 <= cat.d
    prettifyExample.inp_3 <= cat.d

    inst flipFlop of FlipFlop
    flipFlop.clock <= clock
    flipFlop.a_d <= a

    c <= flipFlop.a_q

; MLIR-LABEL: firrtl.module @test_mod(in %clock: !firrtl.clock, in %a: !firrtl.uint<1>, in %b: !firrtl.uint<2>, out %c: !firrtl.uint<1>) {
; MLIR-NEXT:    %cat_a, %cat_b, %cat_c, %cat_d = firrtl.instance @Cat  {name = "cat"} : !firrtl.uint<2>, !firrtl.uint<2>, !firrtl.uint<2>, !firrtl.uint<6>
; MLIR-NEXT:    firrtl.connect %cat_a, %b : !firrtl.uint<2>, !firrtl.uint<2>
; MLIR-NEXT:    firrtl.connect %cat_b, %b : !firrtl.uint<2>, !firrtl.uint<2>
; MLIR-NEXT:    firrtl.connect %cat_c, %b : !firrtl.uint<2>, !firrtl.uint<2>
; MLIR-NEXT:    %implicitTrunc_inp_1, %implicitTrunc_inp_2, %implicitTrunc_out1, %implicitTrunc_out2 = firrtl.instance @ImplicitTrunc  {name = "implicitTrunc"} : !firrtl.uint<1>, !firrtl.sint<5>, !firrtl.sint<3>, !firrtl.sint<3>
; MLIR-NEXT:    firrtl.connect %implicitTrunc_inp_1, %a : !firrtl.uint<1>, !firrtl.uint<1>
; MLIR-NEXT:    %0 = firrtl.asSInt %cat_d : (!firrtl.uint<6>) -> !firrtl.sint<6>
; MLIR-NEXT:    %1 = firrtl.bits %0 4 to 0 : (!firrtl.sint<6>) -> !firrtl.uint<5>
; MLIR-NEXT:    %2 = firrtl.asSInt %1 : (!firrtl.uint<5>) -> !firrtl.sint<5>
; MLIR-NEXT:    firrtl.connect %implicitTrunc_inp_2, %2 : !firrtl.sint<5>, !firrtl.sint<5>
; MLIR-NEXT:    %prettifyExample_inp_1, %prettifyExample_inp_2, %prettifyExample_inp_3, %prettifyExample_out1, %prettifyExample_out2 = firrtl.instance @PrettifyExample  {name = "prettifyExample"} : !firrtl.uint<5>, !firrtl.uint<5>, !firrtl.uint<5>, !firrtl.uint<10>, !firrtl.uint<10>
; MLIR-NEXT:    %3 = firrtl.bits %cat_d 4 to 0 : (!firrtl.uint<6>) -> !firrtl.uint<5>
; MLIR-NEXT:    firrtl.connect %prettifyExample_inp_1, %3 : !firrtl.uint<5>, !firrtl.uint<5>
; MLIR-NEXT:    %4 = firrtl.bits %cat_d 4 to 0 : (!firrtl.uint<6>) -> !firrtl.uint<5>
; MLIR-NEXT:    firrtl.connect %prettifyExample_inp_2, %4 : !firrtl.uint<5>, !firrtl.uint<5>
; MLIR-NEXT:    %5 = firrtl.bits %cat_d 4 to 0 : (!firrtl.uint<6>) -> !firrtl.uint<5>
; MLIR-NEXT:    firrtl.connect %prettifyExample_inp_3, %5 : !firrtl.uint<5>, !firrtl.uint<5>
; MLIR-NEXT:    %flipFlop_clock, %flipFlop_a_d, %flipFlop_a_q = firrtl.instance @FlipFlop  {name = "flipFlop"} : !firrtl.clock, !firrtl.uint<1>, !firrtl.uint<1>
; MLIR-NEXT:    firrtl.connect %flipFlop_clock, %clock : !firrtl.clock, !firrtl.clock
; MLIR-NEXT:    firrtl.connect %flipFlop_a_d, %a : !firrtl.uint<1>, !firrtl.uint<1>
; MLIR-NEXT:    firrtl.connect %c, %flipFlop_a_q : !firrtl.uint<1>, !firrtl.uint<1>
; MLIR-NEXT:  }

; ANNOTATIONS-LABEL: firrtl.module @test_mod
; ANNOTATIONS-SAME: info = "a ModuleTarget Annotation"
; ANNOTATIONS-SAME: info = "a ModuleName Annotation"

; VERILOG-LABEL: module test_mod(
; VERILOG-NEXT:    input        clock, a,
; VERILOG-NEXT:    input  [1:0] b,
; VERILOG-NEXT:    output       c);
; VERILOG-EMPTY:
; VERILOG-NEXT:    wire [4:0] _T;
; VERILOG-NEXT:    wire [9:0] prettifyExample_out1;
; VERILOG-NEXT:    wire [9:0] prettifyExample_out2;
; VERILOG-NEXT:    wire [2:0] implicitTrunc_out1;
; VERILOG-NEXT:    wire [2:0] implicitTrunc_out2;
; VERILOG-NEXT:    wire [5:0] cat_d;
; VERILOG-EMPTY:
; VERILOG-NEXT:    Cat cat (
; VERILOG-NEXT:      .a (b),
; VERILOG-NEXT:      .b (b),
; VERILOG-NEXT:      .c (b),
; VERILOG-NEXT:      .d (cat_d)
; VERILOG-NEXT:    );
; VERILOG-NEXT:    ImplicitTrunc implicitTrunc (
; VERILOG-NEXT:      .inp_1 (a),
; VERILOG-NEXT:      .inp_2 (_T),
; VERILOG-NEXT:      .out1  (implicitTrunc_out1),
; VERILOG-NEXT:      .out2  (implicitTrunc_out2)
; VERILOG-NEXT:    );
; VERILOG-NEXT:    wire [5:0] _T_0 = cat_d;
; VERILOG-NEXT:    assign _T = _T_0[4:0];
; VERILOG-NEXT:    PrettifyExample prettifyExample (
; VERILOG-NEXT:      .inp_1 (_T),
; VERILOG-NEXT:      .inp_2 (_T),
; VERILOG-NEXT:      .inp_3 (_T),
; VERILOG-NEXT:      .out1  (prettifyExample_out1),
; VERILOG-NEXT:      .out2  (prettifyExample_out2)
; VERILOG-NEXT:    );
; VERILOG-NEXT:    FlipFlop flipFlop (
; VERILOG-NEXT:      .clock (clock),
; VERILOG-NEXT:      .a_d   (a),
; VERILOG-NEXT:      .a_q   (c)
; VERILOG-NEXT:    );
; VERILOG-NEXT:  endmodule

; Check that we canonicalize the HW output of lowering.

  module Cat :
    input a: UInt<2>
    input b: UInt<2>
    input c: UInt<2>
    output d: UInt<6>
    d <= cat(cat(a, b), c)

; MLIRLOWER-LABEL: hw.module @Cat(%a: i2, %b: i2, %c: i2) -> (%d: i6) {
; MLIRLOWER-NEXT:    %0 = comb.concat %a, %b, %c : (i2, i2, i2) -> i6
; MLIRLOWER-NEXT:    hw.output %0 : i6
; MLIRLOWER-NEXT:  }


; Check that implicit truncation is working.

  module ImplicitTrunc :
    input inp_1: UInt<1>
    input inp_2: SInt<5>
    output out1: SInt<3>
    output out2: SInt<3>
    out1 <= dshl(inp_2, inp_1)
    out2 <= inp_2

; MLIRLOWER-LABEL: hw.module @ImplicitTrunc(%inp_1: i1, %inp_2: i5) -> (%out1: i3, %out2: i3) {
; MLIRLOWER-NEXT:    %c0_i5 = hw.constant 0 : i5
; MLIRLOWER-NEXT:    %0 = comb.sext %inp_2 : (i5) -> i6
; MLIRLOWER-NEXT:    %1 = comb.concat %c0_i5, %inp_1 : (i5, i1) -> i6
; MLIRLOWER-NEXT:    %2 = comb.shl %0, %1 : i6
; MLIRLOWER-NEXT:    %3 = comb.extract %2 from 0 : (i6) -> i3
; MLIRLOWER-NEXT:    %4 = comb.extract %inp_2 from 0 : (i5) -> i3
; MLIRLOWER-NEXT:    hw.output %3, %4 : i3, i3
; MLIRLOWER-NEXT:  }

; VERILOG-LABEL: module ImplicitTrunc(
; VERILOG-NEXT:   input        inp_1,
; VERILOG-NEXT:   input  [4:0] inp_2,
; VERILOG-NEXT:   output [2:0] out1, out2);
; VERILOG-EMPTY:
; VERILOG-NEXT:   wire [5:0] _T = {inp_2[4], inp_2} << {5'h0, inp_1};
; VERILOG-NEXT:   assign out1 = _T[2:0];
; VERILOG-NEXT:   assign out2 = inp_2[2:0];
; VERILOG-NEXT: endmodule


; Check that we prettify the IR before Verilog emission.

  module PrettifyExample :
    input inp_1: UInt<5>
    input inp_2: UInt<5>
    input inp_3: UInt<5>
    output out1: UInt<10>
    output out2: UInt<10>
    out1 <= cat(not(inp_1), inp_2)
    out2 <= cat(not(inp_1), inp_3)

; VERILOG-LABEL: module PrettifyExample(
; VERILOG:         assign out1 = {~inp_1, inp_2};
; VERILOG:         assign out2 = {~inp_1, inp_3};


; Check output of a simple flip-flop.

  module FlipFlop:
    input clock: Clock
    input a_d: UInt<1>
    output a_q: UInt<1>

    reg r: UInt<1>, clock

    r <= a_d
    a_q <= r

; VERILOG-LABEL: module FlipFlop(
; VERILOG-NEXT:    input clock, a_d,
; VERILOG-NEXT:    output a_q);
; VERILOG:         always @(posedge clock)
; VERILOG-NEXT:      r <= a_d;
; VERILOG-NEXT:    assign a_q = r;
