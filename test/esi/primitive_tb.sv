//===- primitive_tb.sv - tests for ESI primitives -----------*- verilog -*-===//
//
// Testbenches for ESI primitives. Since these rely on an IEEE SystemVerilog
// simulator (Verilator is not a simulator by the IEEE definition) we don't have
// a way to run them as part of our PR gate. They're here for posterity.
//
//===----------------------------------------------------------------------===//

module ESI_PipelineStage_tb ();

  logic clk = 0;
  logic rstn = 0;

  logic a_valid = 0;
  logic [7:0] a = 0;
  logic a_ready;

  logic x_valid;
  logic [7:0] x;
  logic x_ready = 0;


  ESI_PipelineStage s1 (
      .*
  );

  always begin
    // A clock period is #4.
    clk = ~clk;
    #2;
  end

  // Increment the input every cycle.
  always begin
    @(posedge clk) #1;
    a++;
  end

  // Track the number of tokens currently in the stage for debugging.
  int balance = 0;
  always@(posedge clk) begin
    if (x_valid && x_ready)
      balance--;
    if (a_valid && a_ready)
      balance++;
  end

  initial begin
    @(posedge clk) #1;
    rstn = 1;
    @(posedge clk) #1;
    @(posedge clk) #1;
    @(posedge clk) #1;

    a_valid = 1;
    assert (a_ready);
    @(posedge clk) #1;
    assert (x_valid);
    assert (x == 8'h05);
    assert (a_ready);

    a_valid = 1;
    @(posedge clk) #1;
    assert (x_valid);
    assert (x == 8'h05);
    assert (~a_ready);
    a_valid = 1;

    @(posedge clk) #1;
    assert (x_valid);
    assert (x == 8'h05);
    assert (~a_ready);
    x_ready = 1;

    @(posedge clk) #1;
    assert (x_valid);
    assert (x == 8'h06);
    assert (a_ready);
    x_ready = 1;

    @(posedge clk) #1;
    assert (x_valid);
    assert (x == 8'h09);
    assert (a_ready);
    x_ready = 0;

    @(posedge clk) #1;
    assert (x_valid);
    assert (x == 8'h09);
    assert (~a_ready);
    x_ready = 1;

    @(posedge clk) #1;
    assert (x_valid);
    assert (x == 8'h0A);
    assert (a_ready);
    x_ready = 1;
    a_valid = 0;

    @(posedge clk) #1;
    assert (~x_valid);
    assert (a_ready);
    x_ready = 1;
    a_valid = 0;

    @(posedge clk) #1;
    assert (~x_valid);
    assert (a_ready);
    x_ready = 1;
    a_valid = 0;

    @(posedge clk) #1;
    assert (~x_valid);
    assert (a_ready);
    x_ready = 1;
    a_valid = 0;

    @(posedge clk) #1;
    assert (~x_valid);
    assert (a_ready);
    x_ready = 1;
    a_valid = 1;

    @(posedge clk) #1;
    assert (x_valid);
    assert (x == 8'h10);
    assert (a_ready);
    x_ready = 1;
    a_valid = 1;

    @(posedge clk) #1;
    assert (x_valid);
    assert (x == 8'h11);
    assert (a_ready);
    x_ready = 1;
    a_valid = 1;

    @(posedge clk) #1;
    assert (x_valid);
    assert (x == 8'h12);
    assert (a_ready);
    x_ready = 1;
    a_valid = 1;

    @(posedge clk) #1;
    @(posedge clk) #1;
    @(posedge clk) #1;
    @(posedge clk) #1;
    @(posedge clk) #1;
    @(posedge clk) #1;
    $stop();
  end

endmodule
