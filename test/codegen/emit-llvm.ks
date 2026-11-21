# RUN: %toy -O0 -emit-llvm < %s | FileCheck %s
# RUN: %toy -O0 -emit-llvm < %s | opt -passes=verify -disable-output

extern cos(x);
def square(x) x*x;
def wave(x) cos(square(x));
4+5;

# CHECK:       declare double @cos(double)
# CHECK-LABEL: define double @square(double %x)
# CHECK-LABEL: define double @wave(double %x)
# CHECK:       call double @square(double %x)
# CHECK:       call double @cos(
# CHECK-NOT:   __anon_expr
