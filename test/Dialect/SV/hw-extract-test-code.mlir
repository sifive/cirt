// RUN:  circt-opt --sv-extract-test-code %s | FileCheck %s
// CHECK-LABEL: hw.module @issue1246_assert(%clock: i1) attributes {output_file = {directory = "dir3", exclude_from_filelist = true, exclude_replicated_ops = true, name = ""}}
// CHECK: sv.assert
// CHECK: sv.assume
// CHECK: hw.module @issue1246
// CHECK: sv.bind @__ETC_issue1246_assert {output_file = {directory = "", exclude_from_filelist = true, exclude_replicated_ops = true, name = "./dir3/filename3"}}
  hw.module @issue1246(%clock: i1) -> () {
    sv.always posedge %clock  {
      sv.ifdef.procedural "SYNTHESIS"  {
      } else  {
        sv.if %2937  {
          sv.assert {output_file = {directory = "dir3", exclude_from_filelist = true, exclude_replicated_ops = true, name = "./dir3/filename3"}} %clock : i1
          sv.assume {output_file = {directory = "dir4", exclude_from_filelist = true, exclude_replicated_ops = true, name = "./dir4/filename4"}} %clock : i1
        }
      }
    }
    %2937 = hw.constant 0 : i1
    hw.output
  }
