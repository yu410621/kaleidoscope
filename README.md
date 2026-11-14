# kaleidoscope

A step-by-step implementation of the Kaleidoscope language from the LLVM
tutorial "My First Language Frontend with LLVM", built against LLVM 23.1.5.

## Build

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DLLVM_DIR=$HOME/work/llvm-project/build/lib/cmake/llvm
ninja -C build
```

## Progress

- [x] Chapter 1: lexer

## License

The code follows the LLVM tutorial (`llvm/docs/tutorial/MyFirstLanguageFrontend`
and `llvm/examples/Kaleidoscope` at tag `llvmorg-23.1.5`) and is distributed
under the same license, Apache-2.0 WITH LLVM-exception. See `LICENSE`.
