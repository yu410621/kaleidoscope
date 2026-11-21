# RUN: %toy -emit-llvm < %s | FileCheck %s

def mul1(x) x*1;
# CHECK-LABEL: define double @mul1(double %x) {
# CHECK-NEXT:  entry:
# CHECK-NEXT:    ret double %x

def add0(x) x+0;
# CHECK-LABEL: define double @add0(double %x) {
# CHECK:         [[A:%[a-zA-Z0-9._]+]] = fadd double %x, 0.000000e+00
# CHECK-NEXT:    ret double [[A]]

def subself(x) x-x;
# CHECK-LABEL: define double @subself(double %x) {
# CHECK:         [[S:%[a-zA-Z0-9._]+]] = fsub double %x, %x
# CHECK-NEXT:    ret double [[S]]

def add12(x) x+1+2;
# CHECK-LABEL: define double @add12(double %x) {
# CHECK:         [[A1:%[a-zA-Z0-9._]+]] = fadd double %x, 1.000000e+00
# CHECK-NEXT:    [[A2:%[a-zA-Z0-9._]+]] = fadd double [[A1]], 2.000000e+00
# CHECK-NEXT:    ret double [[A2]]
