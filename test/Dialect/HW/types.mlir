// RUN: circt-opt %s | circt-opt | FileCheck %s

// CHECK-LABEL: module
module {
  // CHECK-LABEL: func @i20x5array(%{{.*}}: !hw.array<5xi20>)
  func @i20x5array(%A: !hw.array<5 x i20>) {
    return
  }

  // CHECK-LABEL: func @si20x5array(%{{.*}}: !hw.array<5xsi20>)
  func @si20x5array(%A: !hw.array<5 x si20>) {
    return
  }

  // CHECK-LABEL: func @inoutType(%arg0: !hw.inout<i42>) {
  func @inoutType(%arg0: !hw.inout<i42>) {
    return
  }

  // CHECK-LABEL: func @structType(%arg0: !hw.struct<>, %arg1: !hw.struct<foo: i32, bar: i4, baz: !hw.struct<foo: i7>>) {
  func @structType(%SE: !hw.struct<>, %SF: !hw.struct<foo: i32, bar: i4, baz: !hw.struct<foo: i7>>) {
    return
  }

  // CHECK-LABEL: func @unionType(%arg0: !hw.union<foo: i32, bar: i4, baz: !hw.struct<foo: i7>>) {
  func @unionType(%SF: !hw.union<foo: i32, bar: i4, baz: !hw.struct<foo: i7>>) {
    return
  }

  // CHECK-LABEL: nestedType
  func @nestedType(
    // CHECK: %arg0: !hw.inout<array<42xi8>>,
    %arg0: !hw.inout<!hw.array<42xi8>>,
     // CHECK: %arg1: !hw.inout<array<42xi8>>,
    %arg1: !hw.inout<array<42xi8>>,
    // CHECK: %arg2: !hw.inout<array<2xarray<42xi8>>>
    %arg2: !hw.inout<array<2xarray<42xi8>>>,

    // CHECK: %arg3: !hw.inout<uarray<42xi8>>,
    %arg3: !hw.inout<uarray<42xi8>>,
    // CHECK: %arg4: !hw.inout<uarray<2xarray<42xi8>>>
    %arg4: !hw.inout<uarray<2xarray<42xi8>>>) {
    return
  }
}
