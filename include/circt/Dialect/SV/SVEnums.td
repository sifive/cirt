//===- SVEnums.td - Enumerations used in SV Dialect  ------------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This is the top level file for the SV dialect.
//
//===----------------------------------------------------------------------===//

#ifndef SV_ENUMS_TD
#define SV_ENUMS_TD

// Edge behavior enum for always block.  See SV Spec 9.4.2.

/// AtPosEdge triggers on a rise from 0 to 1/X/Z, or X/Z to 1.
def AtPosEdge: I32EnumAttrCase<"AtPosEdge", 0, "posedge">;
/// AtNegEdge triggers on a drop from 1 to 0/X/Z, or X/Z to 0.
def AtNegEdge: I32EnumAttrCase<"AtNegEdge", 1, "negedge">;
/// AtEdge(v) is syntactic sugar for "AtPosEdge(v) or AtNegEdge(v)".
def AtEdge   : I32EnumAttrCase<"AtEdge", 2, "edge">;

def EventControlAttr : I32EnumAttr<"EventControl", "edge control trigger",
                                   [AtPosEdge, AtNegEdge, AtEdge]> {
  let cppNamespace = "circt::sv";
}

def NoReset: I32EnumAttrCase<"NoReset", 0, "noreset">;
def SyncReset: I32EnumAttrCase<"SyncReset", 1, "syncreset">;
def AsyncReset: I32EnumAttrCase<"AsyncReset", 2, "asyncreset">;

def ResetTypeAttr : I32EnumAttr<"ResetType", "reset type",
                                   [NoReset, SyncReset, AsyncReset]>;

#endif
