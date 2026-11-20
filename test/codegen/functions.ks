# RUN: %toy < %s 2>&1 | FileCheck %s

# Arguments keep their names; every instruction result is named after the
# operation that produced it.
def foo(a b) a*a + 2*a*b + b*b;
# CHECK-LABEL: define double @foo(double %a, double %b) {
# CHECK-NEXT:  entry:
# CHECK-NEXT:    %multmp = fmul double %a, %a
# CHECK-NEXT:    %multmp1 = fmul double 2.000000e+00, %a
# CHECK-NEXT:    %multmp2 = fmul double %multmp1, %b
# CHECK-NEXT:    %addtmp = fadd double %multmp, %multmp2
# CHECK-NEXT:    %multmp3 = fmul double %b, %b
# CHECK-NEXT:    %addtmp4 = fadd double %addtmp, %multmp3
# CHECK-NEXT:    ret double %addtmp4
# CHECK-NEXT:  }

# '<' compares as unordered-or-less-than and converts the i1 back to double.
def lt(x y) x < y;
# CHECK-LABEL: define double @lt(double %x, double %y) {
# CHECK:         %cmptmp = fcmp ult double %x, %y
# CHECK-NEXT:    %booltmp = uitofp i1 %cmptmp to double
# CHECK-NEXT:    ret double %booltmp

# An extern becomes a declaration, and calls refer to it.
extern cos(x);
# CHECK: Read extern: declare double @cos(double)
def callcos(x) cos(x) + cos(1.234);
# CHECK-LABEL: define double @callcos(double %x) {
# CHECK:         %calltmp = call double @cos(double %x)
# CHECK-NEXT:    %calltmp1 = call double @cos(double 1.234000e+00)

# IRBuilder folds constant operands while building, so no instruction is
# emitted for 4+5.
4+5;
# CHECK-LABEL: define double @__anon_expr() {
# CHECK-NEXT:  entry:
# CHECK-NEXT:    ret double 9.000000e+00
