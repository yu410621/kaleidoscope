# RUN: %toy < %s 2>&1 | FileCheck %s --check-prefix=OPT
# RUN: %toy -O0 < %s 2>&1 | FileCheck %s --check-prefix=O0

def test(x) (1+2+x)*(x+(1+2));

# OPT-LABEL: define double @test(double %x) {
# OPT-NEXT:  entry:
# OPT-NEXT:    [[SUM:%[a-zA-Z0-9._]+]] = fadd double %x, 3.000000e+00
# OPT-NEXT:    [[PROD:%[a-zA-Z0-9._]+]] = fmul double [[SUM]], [[SUM]]
# OPT-NEXT:    ret double [[PROD]]

# O0-LABEL: define double @test(double %x) {
# O0-NEXT:  entry:
# O0-NEXT:    [[LEFT:%[a-zA-Z0-9._]+]] = fadd double 3.000000e+00, %x
# O0-NEXT:    [[RIGHT:%[a-zA-Z0-9._]+]] = fadd double %x, 3.000000e+00
# O0-NEXT:    [[PROD:%[a-zA-Z0-9._]+]] = fmul double [[LEFT]], [[RIGHT]]
# O0-NEXT:    ret double [[PROD]]
