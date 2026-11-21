# RUN: %toy -O0 < %s 2>&1 | FileCheck %s

def f(x) y;
# CHECK: Error: Unknown variable name

g(1);
# CHECK: Error: Unknown function referenced

def one(x) x;
one(1, 2);
# CHECK: Error: Incorrect # arguments passed

def one(x) x + 1;
# CHECK: Error: Function cannot be redefined.

extern arity(a);
def arity(a b) a+b;
# CHECK: Error: Definition conflicts with a declaration.
