# D1：建仓与词法分析器（Kaleidoscope Ch.1）

这一天做了两件事：

1. 搭好一个独立的 CMake 工程，链接自己从源码构建的 LLVM 23.1.5。
2. 按教程第 1 章写出词法分析器 `gettok()`，并用一个临时的 `main` 把 token 逐个打印出来。

本篇只记两部分：CMake 如何找到并链接 LLVM，以及词法分析对应的编译原理概念。

---

## 1. CMake 如何找到并链接 LLVM

### 1.1 整体流程

```text
cmake -DLLVM_DIR=...            find_package(LLVM REQUIRED CONFIG)
        │                                   │
        ▼                                   ▼
 LLVM_DIR 指向的目录 ──────▶ LLVMConfig.cmake ──include──▶ LLVMExports*.cmake
                                   │                          （导入目标 LLVMSupport 等）
                                   ├─ 设置变量 LLVM_INCLUDE_DIRS / LLVM_DEFINITIONS / ...
                                   └─ include LLVM-Config.cmake
                                              （提供 llvm_map_components_to_libnames）
        │
        ▼
 include_directories / add_definitions / -fno-rtti     ← 编译阶段要用的
 llvm_map_components_to_libnames + target_link_libraries ← 链接阶段要用的
```

LLVM 不需要 `ninja install`。构建树本身就带着一套 CMake 包配置文件，位于 `~/work/llvm-project/build/lib/cmake/llvm/`，其中关键的几个文件是：

| 文件 | 作用 |
|---|---|
| `LLVMConfig.cmake` | 入口：设置版本号、头文件目录、宏定义、构建选项等变量，再 include 下面几个文件 |
| `LLVMConfigVersion.cmake` | 版本检查，供 `find_package(LLVM 23 ...)` 这类带版本号的调用使用 |
| `LLVMExports*.cmake` | 把每个 LLVM 库声明成 CMake 的**导入目标**（IMPORTED target），记录 `.a` 文件的位置及其依赖 |
| `LLVM-Config.cmake` | 提供函数 `llvm_map_components_to_libnames`，负责把组件名翻译成库名 |

### 1.2 `find_package(LLVM REQUIRED CONFIG)`：找到配置文件

- **CONFIG 模式**：表示去找 LLVM 自己提供的 `LLVMConfig.cmake` 或 `llvm-config.cmake`，而不是 CMake 自带的 `FindLLVM.cmake` 脚本（后者并不存在）。
- **查找顺序**（取最常用的几条）：
  1. 缓存变量 `LLVM_DIR`，也就是 `<包名>_DIR`。我们用 `-DLLVM_DIR=$HOME/work/llvm-project/build/lib/cmake/llvm` 直接指定，最确定。
  2. `CMAKE_PREFIX_PATH` 下的 `lib/cmake/llvm/` 等标准子目录。
  3. 系统路径，如 `/usr/lib/llvm-18/lib/cmake/llvm`。正因为有这一条，**不指定 `LLVM_DIR` 时可能悄悄用上系统的 LLVM 18**。所以要看 cmake 输出里 `Using LLVMConfig.cmake in:` 那一行，确认路径对不对。
- **REQUIRED**：找不到时直接报错终止，不会继续往下配置。
- **找到之后**，`LLVM_DIR` 会写进 `build/CMakeCache.txt`，之后重新运行 `cmake` 时就不用再指定了。要换一个 LLVM，就删掉 `build/` 重新配置。

### 1.3 `LLVMConfig.cmake` 给了哪些东西

在我们的 `CMakeLists.txt` 里用到的变量，以及打印出来的样子（下面是官方发行包上的实测值，你的构建树里路径不同，但形式相同）：

| 变量 | 实测值 | 用在哪 |
|---|---|---|
| `LLVM_PACKAGE_VERSION` | `23.1.5` | `message()` 打印，确认版本 |
| `LLVM_INCLUDE_DIRS` | `<LLVM>/include` | `include_directories(SYSTEM ...)` |
| `LLVM_DEFINITIONS` | `-D_GNU_SOURCE -D_GLIBCXX_USE_CXX11_ABI=1 -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS` | `add_definitions(...)` |
| `LLVM_ENABLE_RTTI` | `OFF` | 决定是否加 `-fno-rtti` |
| `LLVM_TOOLS_BINARY_DIR` | `<LLVM>/bin` | D3 用来找 `llvm-lit` 和 `FileCheck` |

构建树里的头文件其实分在两处：源码树的 `llvm/include`，以及构建时生成的 `build/include`（如 `llvm/Config/llvm-config.h`）。所以 `LLVM_INCLUDE_DIRS` 是一个列表，`include_directories` 会把两个目录都加上。

CMakeLists 里的三处编译相关写法：

```cmake
include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})
if(NOT LLVM_ENABLE_RTTI)
  add_compile_options(-fno-rtti)
endif()
```

- **`SYSTEM`**：把 LLVM 头文件当作系统头文件，以 `-isystem` 传给编译器。效果是 `-Wall -Wextra` 不会对 LLVM 头文件内部报警告，只检查我们自己的代码。
- **`separate_arguments`**：`LLVM_DEFINITIONS` 是一个空格分隔的字符串，要先拆成列表，否则会被当成一个整体参数传下去。
- **为什么必须对齐宏定义和 RTTI 设置**：我们的代码和 LLVM 的静态库最终会链接在一起，两边看到的类定义、ABI 必须一致。
  - `_GLIBCXX_USE_CXX11_ABI=1`：决定 `std::string` 等类型的内存布局。两边不一致时，链接会报 `undefined reference`，未定义的符号名里带着 `std::__cxx11`。
  - LLVM 默认不带 RTTI 构建，库里没有 `typeinfo for llvm::Xxx` 这些符号。我们的代码一旦派生 LLVM 的带虚函数的类，就会引用这些符号，链接时报 `undefined reference to typeinfo for ...`。加上 `-fno-rtti` 后两边一致。D11 还会讲 LLVM 用什么机制代替 RTTI（`isa`、`dyn_cast`）。

### 1.4 链接：组件名 → 库名 → 依赖闭包

```cmake
llvm_map_components_to_libnames(LLVM_LIBS support)
target_link_libraries(toy PRIVATE ${LLVM_LIBS})
```

**组件**是 LLVM 对外暴露的功能单元，名字是小写的，如 `support`、`core`、`orcjit`、`passes`。`llvm_map_components_to_libnames` 做两件事：

1. **翻译**：`support` → `LLVMSupport`。实测输出 `LLVM_LIBS=LLVMSupport`。
2. **展开伪组件**：`native` 展开成本机目标（X86）的那几个库，`all` 展开成全部库。

这里得到的 `LLVMSupport` 不是一个文件路径，而是 `LLVMExports*.cmake` 里定义的**导入目标**。导入目标带着两个关键属性：

| 属性 | 实测值 |
|---|---|
| 库文件位置（`IMPORTED_LOCATION_*`） | `<LLVM>/lib/libLLVMSupport.a` |
| 依赖（`INTERFACE_LINK_LIBRARIES`） | `rt;dl;m;ZLIB::ZLIB;zstd::libzstd_static;LLVMDemangle` |

`target_link_libraries` 拿到导入目标后，会沿着依赖关系递归展开，把整个**依赖闭包**按正确顺序放到链接命令行上。D1 实际的链接命令（`ninja -v` 可看）：

```text
clang++ -g CMakeFiles/toy.dir/src/toy.cpp.o -o toy \
  <LLVM>/lib/libLLVMSupport.a -lrt -ldl -lm \
  /usr/lib/x86_64-linux-gnu/libz.so .../libzstd.a <LLVM>/lib/libLLVMDemangle.a
```

我们只写了 `support`，`LLVMDemangle`、zlib、zstd 都是自动补上的。

这个顺序很重要：LLVM 默认构建的是**静态库**，GNU ld 按命令行顺序单遍扫描静态库，被依赖的库必须排在依赖它的库后面。手写 `-l` 参数很容易把顺序写错，导出的依赖关系替我们解决了这个问题。

后面几天组件列表会随章节增长。每加一个功能，就把它对应的组件加到这里（括号里是第几天加入）：

```text
support                    (D1)
core                       (D4)   IR：Module、Function、IRBuilder
analysis instcombine passes scalaropts   (D5)   优化管线
executionengine native object orcjit runtimedyld   (D6)   JIT
transformutils             (D10)  mem2reg
AllTargets*                (D12)  生成目标文件
```

### 1.5 与教程里 `llvm-config` 命令的对应关系

教程用的是手写命令：

```bash
clang++ -g -O3 toy.cpp `llvm-config --cxxflags --ldflags --system-libs --libs core` -o toy
```

两种方式提供的是同一组信息：

| `llvm-config` 选项 | CMake 中的对应物 |
|---|---|
| `--cxxflags`（`-I`、`-D`、`-std=c++17`、`-fno-exceptions -fno-rtti`） | `LLVM_INCLUDE_DIRS`、`LLVM_DEFINITIONS`、`LLVM_ENABLE_RTTI`，以及我们自己设的 `CMAKE_CXX_STANDARD 17` |
| `--ldflags`（`-L<LLVM>/lib`） | 导入目标里记录的绝对路径 |
| `--libs core` | `llvm_map_components_to_libnames(... core)` |
| `--system-libs`（`-lrt -ldl -lm -lz ...`） | 导入目标的 `INTERFACE_LINK_LIBRARIES` |

CMake 方式的好处有三点：

- **依赖传递是自动的**，库和系统库的顺序也由它排好。
- **生成 `compile_commands.json`**，coc-clangd 可以据此找到 LLVM 头文件。
- **和 LLVM 自身的构建方式一致**，以后读 LLVM 和 MLIR 的 `CMakeLists.txt` 不会陌生。

### 1.6 遇到的问题：`project()` 必须启用 C

最初写的是 `project(kaleidoscope LANGUAGES CXX)`，在本机配置时报错，改成 `LANGUAGES C CXX` 后解决。

原因：`find_package(LLVM)` 会按照 LLVM 构建时的设置，继续查找它的可选依赖。在 `LLVMConfig.cmake` 里是这样写的：

```cmake
set(LLVM_ENABLE_LIBEDIT ...)
if(LLVM_ENABLE_LIBEDIT)
  find_package(LibEdit)
endif()
```

LLVM 自带的 `FindLibEdit.cmake`、`FindFFI.cmake` 是用 **C 语言**做编译检查的，工程如果没启用 C 就会失败。本机构建 LLVM 时检测到了系统里的 libedit 并启用了它，于是本机必须启用 C。官方发行包关闭了这两项，所以在发行包上只写 CXX 也能配置成功。

由此得到的教训：`find_package` 的结果取决于被查找的那一份 LLVM 当初是怎么构建的，同一份 `CMakeLists.txt` 换一个 LLVM 可能表现不同。

### 1.7 另外两个配置项

- **`CMAKE_EXPORT_COMPILE_COMMANDS ON`**：生成 `build/compile_commands.json`。在仓库根目录软链接过去之后，clangd 就能拿到和真实编译完全相同的 `-isystem`、`-D`、`-std` 参数。
- **`-DCMAKE_CXX_COMPILER=/usr/bin/clang++`**：显式使用系统的 clang 18。PATH 里排在前面的 `clang++` 是自己构建的 23.1.5，它带断言，编译慢。

---

## 2. 编译原理概念

### 2.1 词法分析在编译流水线中的位置

```text
字符流 ──词法分析──▶ token 流 ──语法分析──▶ AST ──语义/IR 生成──▶ IR ──优化──▶ IR ──代码生成──▶ 机器码
         D1               D2–D3              D4                D5           D6 / D12
```

**词法分析器**（lexer / scanner）是编译器的第一道工序：把源程序的字符序列切分成有意义的单元（**token**），同时丢掉对后续阶段没有意义的东西（空白、换行、注释）。

**为什么要单独成一个阶段，而不让语法分析器直接读字符：**

- **简化文法**。如果语法分析器直接读字符，「空白可以出现在任意两个 token 之间」「注释可以出现在任意位置」这些规则就得写进每一条产生式里。
- **用合适的工具解决合适的问题**。token 的结构（标识符、数字）是**正则语言**，用有限自动机就能高效识别；程序的嵌套结构（括号、函数体）是**上下文无关语言**，需要更强的分析方法。分开处理，两边都简单。
- **效率**。逐字符的工作集中在一处，可以针对性地优化。在真实编译器里，词法分析常常是逐字节读输入的唯一一个阶段。

### 2.2 token、词素、模式、属性

| 概念 | 含义 | Kaleidoscope 中的例子 |
|---|---|---|
| **token**（词法单元） | 词法单元的**类别**，语法分析器只关心它 | `tok_identifier`、`tok_number`、`tok_def` |
| **lexeme**（词素） | 源码中实际匹配到的那段字符 | `foo`、`4.0`、`def` |
| **pattern**（模式） | 描述某一类 token 由哪些字符构成的规则，通常用正则表达式写 | 标识符：`[a-zA-Z][a-zA-Z0-9]*`；数字：`[0-9.]+` |
| **attribute**（属性） | token 附带的、语法分析器之后还需要的信息 | 标识符的名字存进 `IdentifierStr`，数字的值存进 `NumVal` |

`gettok()` 返回的是 token（一个 `int`），属性通过全局变量带出去。更规范的实现会把类别、属性、源码位置打包成一个 `Token` 结构体（Clang 的 `clang::Token` 就是这样），D13 我们会给它补上源码位置。

Kaleidoscope 全部的 token：

| token | 模式 | 属性 |
|---|---|---|
| `tok_def`、`tok_extern` | 与标识符相同，但词素恰好是 `def` / `extern` | 无 |
| `tok_identifier` | `[a-zA-Z][a-zA-Z0-9]*` | `IdentifierStr` |
| `tok_number` | `[0-9.]+` | `NumVal`（由 `strtod` 转换） |
| `tok_eof` | 输入结束 | 无 |
| 单字符 token | 其余任意一个字符，如 `(`、`+`、`;` | 字符本身就是返回值 |
| （不产生 token） | 空白：`isspace`；注释：`#` 到行尾 | — |

### 2.3 token 的编码：为什么返回 `int`

- 已知类别用**负数**枚举：`tok_eof = -1`、`tok_def = -2` …… `tok_number = -5`。
- 其余字符直接返回它的 **ASCII 值**（0–255，非负）。

两个值域不重叠，一个 `int` 就能表示全部 token。好处是：

- **词法器不需要为每个运算符单独定义 token**。语法分析器里可以直接写 `CurTok == '('`、`BinopPrecedence['+']`。
- **为 Ch.6 的自定义运算符留出了空间**。用户新定义的 `|`、`>` 也只是一个字符，词法器不用做任何修改，只需在语法分析器的优先级表里登记一下。

代价是类型不安全：`int` 能表示一个并不存在的 token。在 C 里这很常见（比如 `getchar()` 用 `int` 返回，`EOF` 是 -1），C++ 的规范写法是用 `enum class` 再加一个联合体或 `std::variant`。

### 2.4 正则表达式与有限自动机

词法分析的理论基础是**正则表达式 ⇔ 有限自动机**这一对等价关系：

```text
正则表达式 ──Thompson 构造──▶ NFA ──子集构造──▶ DFA ──最小化──▶ 最小 DFA ──▶ 状态转移表 / 代码
```

- **NFA**（非确定有限自动机）：一个状态在同一个输入字符下可以转移到多个状态，还可以有 ε 转移（不消耗输入）。从正则表达式机械地构造出来，构造简单，但不能直接高效执行。
- **DFA**（确定有限自动机）：每个状态在每个输入字符下只有唯一的下一个状态。通过**子集构造**（每个 DFA 状态对应 NFA 的一个状态集合）由 NFA 得到。执行时每读一个字符只做一次查表，时间与输入长度成正比。
- **flex / lex 这类工具**就是把上述流程自动化：输入每类 token 的正则表达式，生成 DFA 的状态转移表和驱动代码。

**`gettok()` 就是手写的 DFA**，代码结构和状态图一一对应：

```text
               isspace                  isalnum
             ┌────────┐              ┌──────────┐
             ▼        │              ▼          │
 ┌──────────────────────┐  isalpha  ┌────────────┐  其他字符  ┌──────────────────┐
 │ START（跳过空白）     │──────────▶│ IN_IDENT   │──────────▶│ 接受：标识符/关键字 │
 └──────────────────────┘           └────────────┘           └──────────────────┘
   │ isdigit 或 '.'
   │                   isdigit 或 '.'
   │                   ┌──────────┐
   │                   ▼          │
   ├────────────────▶┌────────────┐  其他字符  ┌────────────┐
   │                 │ IN_NUMBER  │──────────▶│ 接受：数字  │
   │                 └────────────┘           └────────────┘
   │ '#'
   ├────────────────▶ IN_COMMENT ──遇 '\n' '\r'──▶ 回到 START；遇 EOF──▶ 接受：tok_eof
   │ EOF
   ├────────────────▶ 接受：tok_eof
   │ 其他字符
   └────────────────▶ 接受：单字符 token
```

| 代码结构 | 自动机中的含义 |
|---|---|
| 依次判断 `isspace`、`isalpha`、`isdigit`、`'#'`、`EOF` 的 `if` 链 | START 状态根据第一个字符选择转移方向 |
| `while (isalnum(LastChar = getchar()))` | IN_IDENT 状态上的自环 |
| `do { ... } while (isdigit(LastChar) \|\| LastChar == '.')` | IN_NUMBER 状态上的自环 |
| 循环结束、`return` 一个 token | 到达接受状态 |
| 注释分支最后 `return gettok();` | 注释不产生 token，回到 START 继续扫描 |

手写和工具生成各有用处：工具生成的扫描器容易维护、规则一目了然；手写的扫描器速度快、报错信息好、容易处理特殊情况。Clang、GCC、Go、Rust 的编译器都用手写扫描器。

### 2.5 最长匹配（maximal munch）

**规则**：同一位置有多种切分方式时，总是取最长的那个词素。

- `define` 是一个标识符，而不是关键字 `def` 加标识符 `ine`。
- `x1` 是一个标识符，而不是 `x` 加 `1`。

**关键字的处理方式**：先按标识符的模式把整个词素读完，**然后**查它是不是保留字：

```cpp
if (IdentifierStr == "def")
  return tok_def;
if (IdentifierStr == "extern")
  return tok_extern;
return tok_identifier;
```

这样关键字就不需要出现在自动机里，状态数不会因为关键字增多而膨胀。保留字多了以后，通常换成一张哈希表来查。

**实验结果**：

```text
$ echo 'define def x1 1x' | ./build/toy
tok_identifier 'define'
tok_def
tok_identifier 'x1'
tok_number 1
tok_identifier 'x'
tok_eof
```

`1x` 被切成了数字 `1` 和标识符 `x`：读数字时遇到 `x`，它不属于 `[0-9.]`，数字到此结束，`x` 留给下一个 token。词法层面这完全合法。`1x` 是否构成错误，由语法分析器根据上下文来判断。

### 2.6 向前看一个字符（lookahead）

要知道一个词素在哪里**结束**，必须读到**下一个不属于它的字符**才行。例如读完 `foo` 后，必须读到后面的 `(` 才知道标识符结束了。这个多读出来的字符不属于当前 token，必须保留下来，作为下一个 token 的开头。

`gettok()` 用函数内的静态变量保存它：

```cpp
static int LastChar = ' ';
```

这一行有三个设计点：

- **`static`**：值要在多次调用之间保留，这就是「已读入、尚未消费」的那个字符。
- **初值是空格**：第一次调用时直接进入「跳过空白」的循环，再去读第一个真实字符，这样就不用为第一次调用单独写分支。
- **类型是 `int` 不是 `char`**：要能表示 `EOF`（-1），和 `getchar()` 的返回值类型一致。

注意两个分支读下一个字符的方式不同：

- 标识符分支是**先读后判断**：`while (isalnum((LastChar = getchar())))`，第一个字符在进入分支前已经存进 `IdentifierStr`。
- 数字分支是**先存后读**：`do { NumStr += LastChar; LastChar = getchar(); } while (...)`。

两种写法的效果都是：退出循环时，`LastChar` 恰好是词素之后的第一个字符。

**遇到 EOF 时不消费**：

```cpp
// Check for end of file.  Don't eat the EOF.
if (LastChar == EOF)
  return tok_eof;
```

这里没有再调用 `getchar()`，`LastChar` 一直保持 EOF，此后每次调用都会返回 `tok_eof`。语法分析器可以放心地反复请求 token，不会越过文件末尾。

这个思路到 D2 会在更高一层重复出现：语法分析器用 `CurTok` 保存「已读入、尚未消费」的一个 token，这就是 LL(1) 分析里的「1」。

### 2.7 词法错误

词法层能发现的错误很少，主要是「这段字符不属于任何模式」。Kaleidoscope 的处理方式是：

- **不认识的字符不报错**，原样作为单字符 token 返回，交给语法分析器去判断。实测 `a@b` 得到 `tok_identifier 'a'`、`'@'`、`tok_identifier 'b'`。
- **模式过于宽松**：数字的模式 `[0-9.]+` 会接受 `1.2.3`，而 `strtod` 只能转换出 `1.2`，剩下的 `.3` 被悄悄丢掉。这是教程自己承认的缺陷，D3 会修复：检查 `strtod` 的结束位置并报错。

```text
$ echo '1.2.3 .5 5.' | ./build/toy
tok_number 1.2
tok_number 0.5
tok_number 5
tok_eof
```

这个例子说明：**模式定义了「词素长什么样」，转换函数定义了「词素是什么意思」**，两者必须一致。宽松的模式会把错误推迟到更难定位的地方才暴露出来。

### 2.8 注释与空白

- **空白**：在 START 状态直接跳过，不产生任何 token。所以 `def foo` 和 `def   foo` 对语法分析器来说完全一样。
- **注释**：从 `#` 到行尾。注释分支读完后递归调用 `gettok()`，相当于「当作空白处理，再扫描下一个 token」。
- **EOF 的边界情况**：注释如果直到文件末尾都没有换行，循环会因 `LastChar != EOF` 这个条件结束，随后返回 `tok_eof`。少了这个条件就会死循环。实测 `printf 'x # no newline' | ./build/toy` 正常输出 `tok_identifier 'x'`、`tok_eof`。

因为这些东西都在词法层被丢弃了，后面的阶段永远看不到它们。代价是：源码的格式信息、注释内容都丢失了。像 clang-format 这类需要保留格式的工具，所用的词法器会把空白和注释也作为 token 保留下来。

### 2.9 与工业级词法分析器的对比

用 Clang 对 Day 2 的 `t1.c` 做词法分析：

```bash
clang -fsyntax-only -Xclang -dump-tokens ~/work/llvm-build/ir-lab/t1.c 2>&1 | head
```

| 方面 | Kaleidoscope `gettok()` | Clang |
|---|---|---|
| token 表示 | `int` + 全局变量 | `clang::Token` 结构体：类别、长度、标志位、源码位置 |
| 源码位置 | 没有 | 每个 token 都带 `Loc=<file:line:col>`，供报错和调试信息使用 |
| 关键字识别 | `if` 链比较字符串 | 标识符表（哈希），同时记录关键字信息 |
| 输入方式 | 每次 `getchar()` 读一个字符 | 整个文件读入内存缓冲区，用指针扫描 |
| 预处理 | 无 | 词法器和预处理器配合，处理宏展开、`#include` |

我们缺的「源码位置」在 D13 做调试信息时补上，到时会改造 `gettok()`，为每个 token 记录行号和列号。

---

## 3. 本日提交

| # | 提交 | 内容 |
|---|---|---|
| 1 | `[Build] Add CMake project skeleton linked against LLVM 23.1.5` | `.gitignore`、`.clang-format`、`LICENSE`、`README.md`、`CMakeLists.txt`、hello 版 `src/toy.cpp` |
| 2 | `[Lexer] Add lexer and a token-dumping driver` | `enum Token`、`gettok()`、临时 `main` |
| 3 | `[Docs] Add notes on the lexer` | 本文件 |

tag：`ch1`。

## 4. 遗留问题

- `[0-9.]+` 过于宽松，`1.2.3` 会悄悄丢掉一部分 → D3 修复。
- token 不带源码位置，报错信息无法给出行号 → D13 补上。
- 关键字用 `if` 链逐个比较，关键字多了以后要改成查表 → D7、D8 新增 `if`/`then`/`else`/`for`/`in` 时会更明显。