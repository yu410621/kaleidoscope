# RUN: %toy -parse-only -dump-ast < %s 2>&1 | FileCheck --strict-whitespace %s

# '*' binds tighter than '+' and '-'; operators of equal precedence group to
# the left.
a+b*c-d;
# CHECK:      {{^}}Function
# CHECK-NEXT: {{^}}  Prototype __anon_expr()
# CHECK-NEXT: {{^}}  BinaryExpr '-'
# CHECK-NEXT: {{^}}    BinaryExpr '+'
# CHECK-NEXT: {{^}}      VariableExpr a
# CHECK-NEXT: {{^}}      BinaryExpr '*'
# CHECK-NEXT: {{^}}        VariableExpr b
# CHECK-NEXT: {{^}}        VariableExpr c
# CHECK-NEXT: {{^}}    VariableExpr d
# CHECK-NEXT: {{^}}Parsed a top-level expr

# Left associativity: a-b-c is (a-b)-c, not a-(b-c).
a-b-c;
# CHECK:      {{^}}  BinaryExpr '-'
# CHECK-NEXT: {{^}}    BinaryExpr '-'
# CHECK-NEXT: {{^}}      VariableExpr a
# CHECK-NEXT: {{^}}      VariableExpr b
# CHECK-NEXT: {{^}}    VariableExpr c

# '<' has the lowest precedence: a<b+c is a<(b+c).
a<b+c;
# CHECK:      {{^}}  BinaryExpr '<'
# CHECK-NEXT: {{^}}    VariableExpr a
# CHECK-NEXT: {{^}}    BinaryExpr '+'
# CHECK-NEXT: {{^}}      VariableExpr b
# CHECK-NEXT: {{^}}      VariableExpr c

# Parentheses override precedence and leave no node behind.
(a+b)*c;
# CHECK:      {{^}}  BinaryExpr '*'
# CHECK-NEXT: {{^}}    BinaryExpr '+'
# CHECK-NEXT: {{^}}      VariableExpr a
# CHECK-NEXT: {{^}}      VariableExpr b
# CHECK-NEXT: {{^}}    VariableExpr c
