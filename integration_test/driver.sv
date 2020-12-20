//===- driver.sv - Standard SystemVerilog testbench driver ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Contains the top module driver for most simulations. It makes the following
// assumptions/demands:
//
// - The testbench module is called 'top'.
// - It exposes a pin named 'clk' (for the clock).
// - It exposes a pin named 'rstn' (for the reset signel).
//
//===----------------------------------------------------------------------===//

// Make sure this file compiles.
// REQUIRES: questa
// RUN: vlog -sv %s

module driver();

  logic clk = 0;
  logic rstn = 0;

  top top (
    .clk(clk),
    .rstn(rstn)
  );

  always begin
    // A clock period is #4.
    clk = ~clk;
    #2;
  end

  initial begin
    int cycles;

    $display("[driver] Starting simulation");

    rstn = 0;
    // Hold in reset for 4 cycles.
    @(posedge clk);
    @(posedge clk);
    @(posedge clk);
    @(posedge clk);
    rstn = 1;

    if ($value$plusargs ("cycles=%d", cycles)) begin
      int i;
      for (i = 0; i < cycles; i++) begin
        @(posedge clk);
      end
      $display("[driver] Ending simulation at tick #%0d", $time);
      $finish();
    end
  end

endmodule
