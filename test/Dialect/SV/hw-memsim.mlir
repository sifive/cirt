// RUN: circt-opt -hw-memory-sim %s | FileCheck %s

hw.generator.schema @FIRRTLMem, "FIRRTL_Memory", ["depth", "numReadPorts", "numWritePorts", "numReadWritePorts", "readLatency", "writeLatency", "width", "readUnderWrite"]

//CHECK-LABEL: @complex
hw.module @complex(%clock: i1, %reset: i1, %r0en: i1, %mode: i1, %data0: i16) -> (%data1: i16, %data2: i16) {
  %true = hw.constant true
  %c0_i4 = hw.constant 0 : i4
  %tmp41.ro_data_0, %tmp41.rw_rdata_0 = hw.instance "tmp41" @FIRRTLMem_1_1_1_16_10_2_4_0(%clock, %r0en, %c0_i4, %clock, %r0en, %c0_i4, %mode, %true, %data0, %clock, %r0en, %c0_i4, %true, %data0) : (i1, i1, i4, i1, i1, i4, i1, i1, i16, i1, i1, i4, i1, i16) -> (i16, i16)
  hw.output %tmp41.ro_data_0, %tmp41.rw_rdata_0 : i16, i16
}

//CHECK-LABEL: @simple
hw.module @simple(%clock: i1, %reset: i1, %r0en: i1, %mode: i1, %data0: i16) -> (%data1: i16, %data2: i16) {
  %true = hw.constant true
  %c0_i4 = hw.constant 0 : i4
  %tmp41.ro_data_0, %tmp41.rw_rdata_0 = hw.instance "tmp41" @FIRRTLMem_1_1_1_16_10_0_1_0(%clock, %r0en, %c0_i4, %clock, %r0en, %c0_i4, %mode, %true, %data0, %clock, %r0en, %c0_i4, %true, %data0) : (i1, i1, i4, i1, i1, i4, i1, i1, i16, i1, i1, i4, i1, i16) -> (i16, i16)
  hw.output %tmp41.ro_data_0, %tmp41.rw_rdata_0 : i16, i16
}

hw.module.generated @FIRRTLMem_1_1_1_16_10_0_1_0, @FIRRTLMem(%ro_clock_0: i1, %ro_en_0: i1, %ro_addr_0: i4, %rw_clock_0: i1, %rw_en_0: i1, %rw_addr_0: i4, %rw_wmode_0: i1, %rw_wmask_0: i1, %rw_wdata_0: i16, %wo_clock_0: i1, %wo_en_0: i1, %wo_addr_0: i4, %wo_mask_0: i1, %wo_data_0: i16) -> (%ro_data_0: i16, %rw_rdata_0: i16) attributes {depth = 10 : i64, numReadPorts = 1 : ui32, numReadWritePorts = 1 : ui32, numWritePorts = 1 : ui32, readLatency = 0 : ui32, readUnderWrite = 0 : ui32, width = 16 : ui32, writeLatency = 1 : ui32}

//CHECK-LABEL: @FIRRTLMem_1_1_1_16_10_0_1_0
//CHECK:       %Memory = sv.reg  : !hw.inout<uarray<10xi16>>
//CHECK-NEXT:  %[[rslot:.+]] = sv.array_index_inout %Memory[%ro_addr_0]
//CHECK-NEXT:  %[[read:.+]] = sv.read_inout %[[rslot]]
//CHECK-NEXT:  %[[x:.+]] = sv.constantX
//CHECK-NEXT:  %[[readres:.+]] = comb.mux %ro_en_0, %[[read]], %[[x]]
//CHECK-NEXT:  %[[rwtmp:.+]] = sv.wire
//CHECK-NEXT:  %[[rwres:.+]] = sv.read_inout %[[rwtmp]]
//CHECK-NEXT:    sv.alwaysff(posedge %rw_clock_0)  {
//CHECK-NEXT:      %[[rwslot:.+]] = sv.array_index_inout %Memory[%rw_addr_0] 
//CHECK-NEXT:      %false = hw.constant false
//CHECK-NEXT:      %[[rwrcondpre:.+]] = comb.icmp eq %rw_wmode_0, %false
//CHECK-NEXT:      %[[rwrcond:.+]] = comb.and %rw_en_0, %[[rwrcondpre]]
//CHECK-NEXT:      %true = hw.constant true
//CHECK-NEXT:      %[[rwMaskT:.+]]= comb.icmp eq %rw_wmask_0, %true : i1
//CHECK-NEXT:      %[[rwMaskM:.+]] = comb.and %[[rwMaskT:.+]], %rw_wmode_0 : i1
//CHECK-NEXT:      %[[rwwcond:.+]] = comb.and %rw_en_0, %[[rwMaskM:.+]] : i1
//CHECK-NEXT:      %[[x2:.+]] = sv.constantX
//CHECK-NEXT:      sv.passign %[[rwtmp]], %[[x2]]
//CHECK-NEXT:      sv.if %[[rwwcond]]  {
//CHECK-NEXT:        sv.passign %[[rwslot]], %rw_wdata_0
//CHECK-NEXT:      } else  {
//CHECK-NEXT:        sv.if %[[rwrcond]]  {
//CHECK-NEXT:          %[[rwdata:.+]] = sv.read_inout %[[rwslot]]
//CHECK-NEXT:          sv.passign %[[rwtmp]], %[[rwdata]]
//CHECK-NEXT:        }
//CHECK-NEXT:      }
//CHECK-NEXT:    }
//CHECK-NEXT:  sv.alwaysff(posedge %wo_clock_0)  {
//CHECK-NEXT:      %true = hw.constant true
//CHECK-NEXT:      %[[maskS:.+]] = comb.icmp eq %wo_mask_0, %true : i1
//CHECK-NEXT:      %[[wcond:.+]] = comb.and %wo_en_0, %[[maskS:.+]] : i1
//CHECK-NEXT:    sv.if %[[wcond]]  {
//CHECK-NEXT:      %[[wslot:.+]] = sv.array_index_inout %Memory[%wo_addr_0]
//CHECK-NEXT:      sv.passign %[[wslot]], %wo_data_0
//CHECK-NEXT:    }
//CHECK-NEXT:  }
//CHECK-NEXT:  hw.output %[[readres]], %[[rwres]]

hw.module.generated @FIRRTLMem_1_1_1_16_10_2_4_0, @FIRRTLMem(%ro_clock_0: i1, %ro_en_0: i1, %ro_addr_0: i4, %rw_clock_0: i1, %rw_en_0: i1, %rw_addr_0: i4, %rw_wmode_0: i1, %rw_wmask_0: i1, %rw_wdata_0: i16, %wo_clock_0: i1, %wo_en_0: i1, %wo_addr_0: i4, %wo_mask_0: i1, %wo_data_0: i16) -> (%ro_data_0: i16, %rw_rdata_0: i16) attributes {depth = 10 : i64, numReadPorts = 1 : ui32, numReadWritePorts = 1 : ui32, numWritePorts = 1 : ui32, readLatency = 2 : ui32, readUnderWrite = 0 : ui32, width = 16 : ui32, writeLatency = 4 : ui32}

//CHECK-LABEL: @FIRRTLMem_1_1_1_16_10_2_4_0
//COM: This produces a lot of output, we check one field's pipeline
//CHECK:         %Memory = sv.reg  : !hw.inout<uarray<10xi16>>
//CHECK:         sv.alwaysff(posedge %ro_clock_0)  {
//CHECK-NEXT:      sv.passign %0, %ro_en_0 : i1
//CHECK-NEXT:    }
//CHECK-NEXT:    %1 = sv.read_inout %0 : !hw.inout<i1>
//CHECK-NEXT:    %2 = sv.reg  : !hw.inout<i1>
//CHECK-NEXT:    sv.alwaysff(posedge %ro_clock_0)  {
//CHECK-NEXT:      sv.passign %2, %1 : i1
//CHECK-NEXT:    }
//CHECK-NEXT:    %3 = sv.read_inout %2 : !hw.inout<i1>
//CHECK-NEXT:    %4 = sv.reg  : !hw.inout<i4>
//CHECK-NEXT:    sv.alwaysff(posedge %ro_clock_0)  {
//CHECK-NEXT:      sv.passign %4, %ro_addr_0 : i4
//CHECK-NEXT:    }
//CHECK-NEXT:    %5 = sv.read_inout %4 : !hw.inout<i4>
//CHECK-NEXT:    %6 = sv.reg  : !hw.inout<i4>
//CHECK-NEXT:    sv.alwaysff(posedge %ro_clock_0)  {
//CHECK-NEXT:      sv.passign %6, %5 : i4
//CHECK-NEXT:    }
//CHECK-NEXT:    %7 = sv.read_inout %6 : !hw.inout<i4>
//CHECK-NEXT:    %8 = sv.array_index_inout %Memory[%7] : !hw.inout<uarray<10xi16>>, i4
