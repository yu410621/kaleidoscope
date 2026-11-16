#!/usr/bin/env bash
# Compare src/toy.cpp with the tutorial's reference code for one chapter:
#   scripts/diff-ref.sh 2 | less      # against Chapter2/toy.cpp
# Both sides go through clang-format first so only real differences show up.
set -euo pipefail
chapter=${1:?usage: diff-ref.sh <chapter number, 2-9>}
llvm_src=${LLVM_SRC:-$HOME/work/llvm-project}
ref=$llvm_src/llvm/examples/Kaleidoscope/Chapter$chapter/toy.cpp
cd "$(dirname "$0")/.."
diff -u <(clang-format --style=LLVM "$ref") \
        <(clang-format --style=LLVM src/toy.cpp) || true