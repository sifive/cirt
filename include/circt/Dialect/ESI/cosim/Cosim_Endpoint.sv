// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

// Copyright (c) Microsoft Corporation. All rights reserved.
// =============================================================================
// Package: CosimCore_EndpointBasePkg
//
// Authors:
// - John Demme (john.demme@microsoft.com)
//
// Based on code written by:
// - Andrew Lenharth (andrew.lenharth@microsoft.com)
//
// Description:
//   Main cosim <--> dpi bridge module
// =============================================================================

import Cosim_DpiPkg::*;

module Cosim_Endpoint
#(
  parameter int ENDPOINT_ID = -1,
  parameter longint SEND_TYPE_ID = -1,
  parameter int SEND_TYPE_SIZE_BITS = -1,
  parameter longint RECV_TYPE_ID = -1,
  parameter int RECV_TYPE_SIZE_BITS = -1
)
(
  input  logic clk,
  input  logic rstn,

  output logic DataOutValid,
  input  logic DataOutReady,
  output logic[SEND_TYPE_SIZE_BITS-1:0] DataOut,

  input  logic DataInValid,
  output logic DataInReady,
  input  logic [RECV_TYPE_SIZE_BITS-1:0] DataIn
);
  
  bit Initialized;

  // Handle initialization logic
  always@(posedge clk)
  begin
    // We've been instructed to start AND we're uninitialized
    if (!Initialized)
    begin
      int rc;
      rc = cosim_init();
      if (rc != 0)
        $error("Cosim init failed (%d)", rc);
      rc = cosim_ep_register(ENDPOINT_ID, SEND_TYPE_ID, SEND_TYPE_SIZE_BYTES, RECV_TYPE_ID, RECV_TYPE_SIZE_BYTES);
      if (rc != 0)
        $error("Cosim endpoint (%d) register failed: %d", ENDPOINT_ID, rc);
      Initialized = 1'b1;
    end
  end

  /// *******************
  /// Data out management
  ///

  localparam int SEND_TYPE_SIZE_BYTES = int'((SEND_TYPE_SIZE_BITS+7)/8);
  localparam int SEND_TYPE_SIZE_BITS_DIFF = SEND_TYPE_SIZE_BITS % 8; // The number of bits over a byte
  localparam int SEND_TYPE_SIZE_BYTES_FLOOR = int'(SEND_TYPE_SIZE_BITS/8);
  localparam int SEND_TYPE_SIZE_BYTES_FLOOR_IN_BITS = SEND_TYPE_SIZE_BYTES_FLOOR * 8;

  byte unsigned DataOutBuffer[SEND_TYPE_SIZE_BYTES-1:0];
  always@(posedge clk)
  begin
    if (rstn && Initialized)
    begin
      if (DataOutValid && DataOutReady) // A transfer occurred
      begin
        DataOutValid <= 1'b0;
      end

      if (!DataOutValid || DataOutReady)
      begin
        int data_limit;
        int rc;

        data_limit = SEND_TYPE_SIZE_BYTES;
        rc = cosim_ep_tryget(ENDPOINT_ID, DataOutBuffer, data_limit);
        if (rc < 0)
        begin
          $error("cosim_ep_tryget(%d, *, %d -> %d) returned an error (%d)",
            ENDPOINT_ID, SEND_TYPE_SIZE_BYTES, data_limit, rc);
        end
        else if (rc > 0)
        begin
          $error("cosim_ep_tryget(%d, *, %d -> %d) had data left over! (%d)",
            ENDPOINT_ID, SEND_TYPE_SIZE_BYTES, data_limit, rc);
        end
        else if (rc == 0)
        begin
          if (data_limit == SEND_TYPE_SIZE_BYTES)
          begin
            DataOutValid <= 1'b1;
          end
          else if (data_limit == 0)
          begin
            // No message
          end
          else
          begin
            $error("cosim_ep_tryget(%d, *, %d -> %d) did not load entire buffer!",
                ENDPOINT_ID, SEND_TYPE_SIZE_BYTES, data_limit);
          end
        end
      end
    end
    else
    begin
        DataOutValid <= 1'b0;
    end
  end

  // Assign packed output bit array from unpacked byte array
  genvar iOut;
  generate
    for (iOut=0; iOut<SEND_TYPE_SIZE_BYTES_FLOOR; iOut++)
    begin
      assign DataOut[((iOut+1)*8)-1:iOut*8] = DataOutBuffer[iOut];
    end
    if (SEND_TYPE_SIZE_BITS_DIFF != 0)
      assign DataOut[SEND_TYPE_SIZE_BYTES_FLOOR_IN_BITS + SEND_TYPE_SIZE_BITS_DIFF - 1 : SEND_TYPE_SIZE_BYTES_FLOOR_IN_BITS]
        = DataOutBuffer[SEND_TYPE_SIZE_BYTES-1][SEND_TYPE_SIZE_BITS_DIFF-1:0];
  endgenerate

  initial
  begin
    $display("SEND_TYPE_SIZE_BITS: %d", SEND_TYPE_SIZE_BITS);
    $display("SEND_TYPE_SIZE_BYTES: %d", SEND_TYPE_SIZE_BYTES);
    $display("SEND_TYPE_SIZE_BITS_DIFF: %d", SEND_TYPE_SIZE_BITS_DIFF);
    $display("SEND_TYPE_SIZE_BYTES_FLOOR: %d", SEND_TYPE_SIZE_BYTES_FLOOR);
    $display("SEND_TYPE_SIZE_BYTES_FLOOR_IN_BITS: %d", SEND_TYPE_SIZE_BYTES_FLOOR_IN_BITS);
  end


  /// **********************
  /// Data in management
  ///

  localparam int RECV_TYPE_SIZE_BYTES = int'((RECV_TYPE_SIZE_BITS+7)/8);
  localparam int RECV_TYPE_SIZE_BITS_DIFF = RECV_TYPE_SIZE_BITS % 8; // The number of bits over a byte
  localparam int RECV_TYPE_SIZE_BYTES_FLOOR = int'(RECV_TYPE_SIZE_BITS/8);
  localparam int RECV_TYPE_SIZE_BYTES_FLOOR_IN_BITS = RECV_TYPE_SIZE_BYTES_FLOOR * 8;

  assign DataInReady = 1'b1;
  byte unsigned DataInBuffer[RECV_TYPE_SIZE_BYTES-1:0];

  always@(posedge clk)
  begin
    if (rstn && Initialized)
    begin
      if (DataInValid)
      begin
        int rc;
        rc = cosim_ep_tryput(ENDPOINT_ID, DataInBuffer, RECV_TYPE_SIZE_BYTES);
        if (rc != 0)
        begin
          $error("cosim_ep_tryput(%d, *, %d) = %d Error! (Data lost)",
            ENDPOINT_ID, RECV_TYPE_SIZE_BYTES, rc);
        end
      end
    end
  end

  // Assign packed input bit array to unpacked byte array
  genvar iIn;
  generate
    for (iIn=0; iIn<RECV_TYPE_SIZE_BYTES_FLOOR; iIn++)
    begin
      assign DataInBuffer[iIn] = DataIn[((iIn+1)*8)-1:iIn*8];
    end
    if (RECV_TYPE_SIZE_BITS_DIFF != 0)
      assign DataInBuffer[RECV_TYPE_SIZE_BYTES-1][RECV_TYPE_SIZE_BITS_DIFF-1:0] =
        DataIn[RECV_TYPE_SIZE_BYTES_FLOOR_IN_BITS + RECV_TYPE_SIZE_BITS_DIFF - 1 : RECV_TYPE_SIZE_BYTES_FLOOR_IN_BITS];
  endgenerate

  initial
  begin
    $display("RECV_TYPE_SIZE_BITS: %d", RECV_TYPE_SIZE_BITS);
    $display("RECV_TYPE_SIZE_BYTES: %d", RECV_TYPE_SIZE_BYTES);
    $display("RECV_TYPE_SIZE_BITS_DIFF: %d", RECV_TYPE_SIZE_BITS_DIFF);
    $display("RECV_TYPE_SIZE_BYTES_FLOOR: %d", RECV_TYPE_SIZE_BYTES_FLOOR);
    $display("RECV_TYPE_SIZE_BYTES_FLOOR_IN_BITS: %d", RECV_TYPE_SIZE_BYTES_FLOOR_IN_BITS);
  end

endmodule
