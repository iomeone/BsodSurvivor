; NOTE: Assertions have been autogenerated by utils/update_test_checks.py
; XFAIL: *
; RUN: opt < %s -basicaa -dse -enable-dse-memoryssa -S | FileCheck %s
; RUN: opt < %s -aa-pipeline=basic-aa -passes=dse -enable-dse-memoryssa -S | FileCheck %s
target datalayout = "E-p:64:64:64-a0:0:8-f32:32:32-f64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:32:64-v64:64:64-v128:128:128"

declare void @llvm.memset.p0i8.i64(i8* nocapture, i8, i64, i1) nounwind
declare void @llvm.memset.element.unordered.atomic.p0i8.i64(i8* nocapture, i8, i64, i32) nounwind
declare void @llvm.memcpy.p0i8.p0i8.i64(i8* nocapture, i8* nocapture, i64, i1) nounwind
declare void @llvm.memcpy.element.unordered.atomic.p0i8.p0i8.i64(i8* nocapture, i8* nocapture, i64, i32) nounwind
declare void @llvm.init.trampoline(i8*, i8*, i8*)

; Test for byval handling.
%struct.x = type { i32, i32, i32, i32 }
define void @test9(%struct.x* byval  %a) nounwind  {
; CHECK-LABEL: @test9(
; CHECK-NEXT:    ret void
;
  %tmp2 = getelementptr %struct.x, %struct.x* %a, i32 0, i32 0
  store i32 1, i32* %tmp2, align 4
  ret void
}

; Test for inalloca handling.
define void @test9_2(%struct.x* inalloca  %a) nounwind  {
; CHECK-LABEL: @test9_2(
; CHECK-NEXT:    ret void
;
  %tmp2 = getelementptr %struct.x, %struct.x* %a, i32 0, i32 0
  store i32 1, i32* %tmp2, align 4
  ret void
}

; Test for preallocated handling.
define void @test9_3(%struct.x* preallocated(%struct.x)  %a) nounwind  {
; CHECK-LABEL: @test9_3(
; CHECK-NEXT:    ret void
;
  %tmp2 = getelementptr %struct.x, %struct.x* %a, i32 0, i32 0
  store i32 1, i32* %tmp2, align 4
  ret void
}

; DSE should delete the dead trampoline.
declare void @test11f()
define void @test11() {
; CHECK-LABEL: @test11(
; CHECK-NEXT:    ret void
;
  %storage = alloca [10 x i8], align 16		; <[10 x i8]*> [#uses=1]
  %cast = getelementptr [10 x i8], [10 x i8]* %storage, i32 0, i32 0		; <i8*> [#uses=1]
  call void @llvm.init.trampoline( i8* %cast, i8* bitcast (void ()* @test11f to i8*), i8* null )		; <i8*> [#uses=1]
  ret void
}


declare noalias i8* @malloc(i32)
declare noalias i8* @calloc(i32, i32)

define void @test14(i32* %Q) {
; CHECK-LABEL: @test14(
; CHECK-NEXT:    ret void
;
  %P = alloca i32
  %DEAD = load i32, i32* %Q
  store i32 %DEAD, i32* %P
  ret void

}

define void @test20() {
; CHECK-LABEL: @test20(
; CHECK-NEXT:    ret void
;
  %m = call i8* @malloc(i32 24)
  store i8 0, i8* %m
  ret void
}

define void @test21() {
; CHECK-LABEL: @test21(
; CHECK-NEXT:    ret void
;
  %m = call i8* @calloc(i32 9, i32 7)
  store i8 0, i8* %m
  ret void
}

define void @test22(i1 %i, i32 %k, i32 %m) nounwind {
; CHECK-LABEL: @test22(
; CHECK-NEXT:    ret void
;
  %k.addr = alloca i32
  %m.addr = alloca i32
  %k.addr.m.addr = select i1 %i, i32* %k.addr, i32* %m.addr
  store i32 0, i32* %k.addr.m.addr, align 4
  ret void
}

declare void @unknown_func()

; Remove redundant store if loaded value is in another block inside a loop.
define i32 @test31(i1 %c, i32* %p, i32 %i) {
; CHECK-LABEL: @test31(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    br label [[BB1:%.*]]
; CHECK:       bb1:
; CHECK-NEXT:    br i1 undef, label [[BB1]], label [[BB2:%.*]]
; CHECK:       bb2:
; CHECK-NEXT:    ret i32 0
;
entry:
  %v = load i32, i32* %p, align 4
  br label %bb1
bb1:
  store i32 %v, i32* %p, align 4
  br i1 undef, label %bb1, label %bb2
bb2:
  ret i32 0
}
