# RUN: %toy -dump-tokens < %s | FileCheck %s

# Keywords, identifiers, numbers and single-character tokens.
def foo(x) x+4.5;
extern sin(a);

# CHECK:      tok_def
# CHECK-NEXT: tok_identifier 'foo'
# CHECK-NEXT: '('
# CHECK-NEXT: tok_identifier 'x'
# CHECK-NEXT: ')'
# CHECK-NEXT: tok_identifier 'x'
# CHECK-NEXT: '+'
# CHECK-NEXT: tok_number 4.500000e+00
# CHECK-NEXT: ';'
# CHECK-NEXT: tok_extern
# CHECK-NEXT: tok_identifier 'sin'
# CHECK-NEXT: '('
# CHECK-NEXT: tok_identifier 'a'
# CHECK-NEXT: ')'
# CHECK-NEXT: ';'

# A keyword is only a keyword when it is the whole identifier.
defx
# CHECK-NEXT: tok_identifier 'defx'
# CHECK-NEXT: tok_eof