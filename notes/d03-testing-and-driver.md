# D3：测试框架、命令行选项与 AST 打印

D3 不跟教程，给项目补了三样基础设施：

1. **命令行选项**：用 `llvm::cl` 加上 `-dump-tokens`，并接入 `InitLLVM`。
2. **AST 打印**：给每个节点加 `dump()`，加上 `-dump-ast` 选项。
3. **回归测试**：`test/lit.cfg.py`、`ninja -C build check` 目标、第一个测试 `test/lexer/tokens.ks`。

本篇按三个主题整理：lit 与 FileCheck 的原理以及本项目怎么接入、LLVM 命令行库的用法、`dump()` 虚函数的设计。

---

## 1. lit 与 FileCheck

### 1.1 两个工具的分工

LLVM 的回归测试由两个工具配合完成：

| 工具 | 角色 | 一句话 |
|---|---|---|
| **lit**（LLVM Integrated Tester） | 测试运行器 | 找到测试文件，执行其中的 `RUN:` 命令，看退出码判定通过与否 |
| **FileCheck** | 输出匹配器 | 从标准输入读被测程序的输出，按测试文件里的 `CHECK:` 行逐条匹配，不符就以非 0 退出 |

lit 本身完全不理解「期望输出」。它只知道「`RUN:` 行里的命令全部返回 0 就算通过」。判断输出对不对，靠的是 `RUN:` 行把输出通过管道交给 FileCheck，FileCheck 匹配失败就返回 1，lit 于是判定失败。

一个测试就是一个文件，**输入、要执行的命令、期望的输出三者写在同一个文件里**：

```text
# RUN: %toy -dump-tokens < %s | FileCheck %s      ← 要执行的命令
                                                   
def foo(x) x+4.5;                                  ← 被测程序的输入
extern sin(a);

# CHECK:      tok_def                              ← 期望的输出
# CHECK-NEXT: tok_identifier 'foo'
...
```

同一个文件 `%s` 被用了两次：

- `toy -dump-tokens < %s`：它作为 toy 的**输入**。toy 把 `# RUN`、`# CHECK` 当作 Kaleidoscope 的注释，在词法层直接跳过，只看到 `def foo(x) x+4.5;` 这些代码。
- `FileCheck %s`：它作为 FileCheck 的**检查文件**。FileCheck 只关心其中以 `CHECK` 开头的指令行，其余内容都忽略。

这个设计能成立，前提是被测语言的注释符号能把这些指令藏起来：Kaleidoscope 用 `#`，LLVM IR 用 `;`，C 用 `//`。你在 `~/work/llvm-project/llvm/test/` 下看到的几万个测试都是这个形式。

### 1.2 lit 的运行机制

执行 `llvm-lit -v test` 时，lit 依次做这几步：

**1. 找测试套件。** 从给出的路径开始向上逐级查找 `lit.cfg.py`（或 `lit.site.cfg.py`），找到的那个目录就是一个测试套件的根。我们的 `test/lit.cfg.py` 就是这个根配置。

**2. 执行配置文件。** `lit.cfg.py` 是一段普通的 Python 脚本，lit 在执行它之前预先放进去两个对象：

- `config`：本测试套件的配置，脚本通过给它的字段赋值来定制行为。
- `lit_config`：lit 全局的配置，其中 `lit_config.params` 装着命令行上 `-Dkey=value` 传进来的参数。

我们的配置做了这些事：

| 配置 | 作用 |
|---|---|
| `config.name = "Kaleidoscope"` | 测试名的前缀，输出里显示为 `Kaleidoscope :: lexer/tokens.ks` |
| `config.test_format = lit.formats.ShTest(execute_external=False)` | 测试格式：「文件里的 `RUN:` 行是 shell 命令」，并用 lit **内置的 shell** 来执行 |
| `config.suffixes = [".ks"]` | 只有 `.ks` 文件算测试，其余文件忽略 |
| `config.test_source_root` | 测试源文件所在的目录，即 `test/` |
| `config.test_exec_root` | 执行目录，`%t` 临时文件放在这里。设为 `build/test`，避免往源码树里写东西 |
| `config.substitutions.append(("%toy", ...))` | 自定义替换：`%toy` 展开成 `build/toy` 的绝对路径 |
| `config.environment["PATH"]` | 测试运行时的 PATH，把 LLVM 工具目录排在最前，保证用到的是 23.1.5 的 FileCheck |

**3. 发现测试。** 递归扫描 `test_source_root`，后缀匹配的每个文件就是一个测试。

**4. 逐个执行。** 对每个测试文件：

- **收集 `RUN:` 行**。可以有多行，行尾的 `\` 表示续行。
- **做替换**。内置替换和自定义替换都在这一步展开：

| 替换 | 展开为 |
|---|---|
| `%s` | 测试文件本身的绝对路径 |
| `%S` | 测试文件所在的目录 |
| `%t` | 本测试专用的临时文件路径（在 `test_exec_root` 下，每个测试各不相同） |
| `%toy` | 我们在 `lit.cfg.py` 里定义的，指向 `build/toy` |

- **用内置 shell 执行**。lit 自己实现了一个小 shell，支持管道 `|`、重定向 `<` `>` `2>&1`、`&&`、`not`（把退出码取反，用来测试「程序应该报错」）等常用语法。用内置 shell 的好处是不依赖系统 shell，Windows 上也能跑。LLVM 23 的 lit 已经废弃了改用外部 shell 的模式，网上旧示例里的 `ShTest(True)` 在新版本会直接报错。
- **判定结果**。所有命令都返回 0 就是 `PASS`，否则是 `FAIL`。还有 `XFAIL`（预期失败）、`UNSUPPORTED`（条件不满足而跳过）等状态，以后写 pass 测试时会用到。

**5. 汇总报告。** 打印每个测试的状态和统计。加 `-v` 时会打印失败测试的完整命令和输出，加 `-a` 时通过的测试也打印。

用 `-a` 看一次通过的测试，就能清楚看到替换后的真实命令：

```text
$ llvm-lit -a test
PASS: Kaleidoscope :: lexer/tokens.ks (1 of 1)
...
# RUN: at line 1
/home/li/work/kaleidoscope/build/toy -dump-tokens < /home/li/work/kaleidoscope/test/lexer/tokens.ks | FileCheck /home/li/work/kaleidoscope/test/lexer/tokens.ks
# executed command: /home/li/work/kaleidoscope/build/toy -dump-tokens
# executed command: FileCheck /home/li/work/kaleidoscope/test/lexer/tokens.ks
```

### 1.3 本项目中测试是怎么接入的

```text
ninja -C build check
   │
   ▼
CMake 生成的 check 目标
   │  1. DEPENDS toy → 先重新构建 toy
   │  2. 执行 llvm-lit -v <源码>/test -Dbuild_dir=<build> -Dllvm_tools_dir=<LLVM>/bin
   ▼
llvm-lit
   │  向上找到 test/lit.cfg.py 并执行
   │     build_dir      ← lit_config.params["build_dir"]      → 用来拼出 %toy 的路径
   │     llvm_tools_dir ← lit_config.params["llvm_tools_dir"] → 加到 PATH 最前面
   ▼
对每个 test/**/*.ks：替换 → 内置 shell 执行 RUN 行 → 看退出码
   │
   ▼
toy ... | FileCheck %s    ← FileCheck 退出码决定 PASS / FAIL
```

`CMakeLists.txt` 里的这一段：

```cmake
find_program(LLVM_LIT NAMES llvm-lit lit HINTS ${LLVM_TOOLS_BINARY_DIR})
if(LLVM_LIT)
  add_custom_target(check
    COMMAND ${LLVM_LIT} -v ${CMAKE_SOURCE_DIR}/test
            -Dbuild_dir=${CMAKE_BINARY_DIR}
            -Dllvm_tools_dir=${LLVM_TOOLS_BINARY_DIR}
    DEPENDS toy
    USES_TERMINAL
    COMMENT "Running the Kaleidoscope regression tests")
endif()
```

| 写法 | 作用 |
|---|---|
| `find_program(... HINTS ${LLVM_TOOLS_BINARY_DIR})` | 优先在 LLVM 构建树的 `bin/` 下找 `llvm-lit`，找不到再试 PATH 里的 `lit`（pip 安装的版本） |
| `add_custom_target(check ...)` | 定义一个不产生文件的目标，`ninja check` 就是执行它的命令 |
| `DEPENDS toy` | 先确保 toy 是最新的。直接运行 `llvm-lit` 不会重新构建，测的可能是旧的二进制 |
| `-Dbuild_dir=...`、`-Dllvm_tools_dir=...` | 通过 lit 参数把 CMake 知道的路径传给 `lit.cfg.py` |
| `USES_TERMINAL` | 让 ninja 把 lit 的输出直接送到终端，否则会被缓冲到最后才显示 |

**两条路径信息分别从哪来**：

- **`build_dir`**：CMake 知道构建目录在哪，lit 不知道。`lit.cfg.py` 里给了默认值 `<test>/../build`，所以在仓库根目录直接运行 `llvm-lit test` 也能找到 `toy`。
- **`llvm_tools_dir`**：保证 FileCheck 用的是 23.1.5 构建树里的版本，而不是系统里可能存在的其他版本。

LLVM 自己的测试套件分成两层：CMake 配置时由 `lit.site.cfg.py.in` 生成 `build/test/lit.site.cfg.py`，里面写死构建目录等路径；它再去加载源码树里的 `lit.cfg.py`，后者负责通用逻辑。我们用 `-D` 参数代替了生成文件那一层，机制相同，只是简化了。

**以后加一个测试只需要一步**：在 `test/` 下任意子目录新建一个 `.ks` 文件，第一行写 `# RUN:`。不用改 CMake，也不用改 `lit.cfg.py`，lit 会自动发现它。

### 1.4 FileCheck 的匹配机制

FileCheck 做的事情分三步：

1. 读检查文件，取出所有以检查前缀（默认 `CHECK`）开头的指令。前面的 `#` 注释符号不影响识别。
2. 从标准输入读入被测程序的全部输出。
3. 从输出开头开始，**按顺序**逐条匹配指令。每条指令从上一条匹配结束的位置往后找。全部匹配成功就返回 0，任何一条失败就打印诊断信息并返回 1。

**常用指令**：

| 指令 | 含义 |
|---|---|
| `CHECK: text` | 在上一个匹配之后的某处出现 `text`。中间隔多少行都可以；`text` 只需是行的子串 |
| `CHECK-NEXT: text` | 必须出现在上一个匹配的**紧邻下一行** |
| `CHECK-SAME: text` | 必须出现在上一个匹配的**同一行**，位于它的后面 |
| `CHECK-NOT: text` | 在前后两个匹配之间**不得**出现 |
| `CHECK-DAG: text` | 一组相邻的 DAG 指令可以按任意顺序匹配 |
| `CHECK-LABEL: text` | 先用所有 LABEL 把输出切成若干段，其他指令只在各自所在的段里匹配。一个函数的检查失败，不会连累后面的函数也错位 |
| `CHECK-EMPTY:` | 下一行必须是空行 |

**模式语法**：

| 写法 | 含义 |
|---|---|
| 普通文本 | 按字面匹配，正则元字符无需转义 |
| `{{regex}}` | 嵌入一段正则表达式，例如 `{{^}}` 表示行首，`{{[0-9]+}}` 匹配一串数字 |
| `[[NAME:regex]]` | 匹配一段文本，并把它保存到变量 `NAME` |
| `[[NAME]]` | 引用前面保存的变量，要求出现完全相同的文本 |

变量在测 IR 时非常重要：`%addtmp1` 这类临时名的编号会随代码的小改动而变化。用 `[[X:%.+]]` 捕获之后再引用，测试就只依赖「是同一个值」，不依赖它叫什么名字。D10 会遇到这种情况。

**常用选项**：

| 选项 | 作用 |
|---|---|
| `--check-prefix=X` | 改用 `X:` 作为前缀。一个文件里配几条 RUN，各用一个前缀，就能测同一份输入的不同模式（D5 用来对比 `-O0` 和优化后的输出） |
| `--strict-whitespace` | 关闭空白归一化（见下文） |
| `--match-full-lines` | 要求整行匹配，而不是子串匹配 |
| `--implicit-check-not=text` | 输出里任何地方都不许出现 `text`，常用来确保没有多余的报错 |
| `--dump-input=always` | 无论成败都把输入打印出来，调试测试本身时有用 |

**空白归一化**：FileCheck 默认把连续的空格和制表符当成一个空格来比较，所以 `CHECK:  a   b` 能匹配 `a b`。这对测 IR 很方便，但测 `-dump-ast` 时就成了漏洞，因为 AST 的层次完全靠缩进表示，缩进错了也照样能通过。所以 AST 测试要这样写：

```text
# RUN: %toy -dump-ast < %s 2>&1 | FileCheck --strict-whitespace %s
# CHECK: {{^}}    BinaryExpr '+'
```

`--strict-whitespace` 让空格个数必须一致，`{{^}}` 把匹配钉在行首。两者缺一不可：只有 `--strict-whitespace` 而没有行首锚点，多缩进了的那一行照样包含「4 个空格 + `BinaryExpr`」这个子串，仍会匹配成功。

### 1.5 读懂失败输出

把 `tokens.ks` 里的 `tok_identifier 'sin'` 故意改成 `'cos'` 再运行：

```text
FAIL: Kaleidoscope :: lexer/tokens.ks (1 of 1)
...
tokens.ks:17:15: error: CHECK-NEXT: expected string not found in input
# CHECK-NEXT: tok_identifier 'cos'
              ^
<stdin>:10:11: note: scanning from here
tok_extern
          ^
<stdin>:11:1: note: possible intended match here
tok_identifier 'sin'
^
```

| 诊断行 | 含义 |
|---|---|
| `tokens.ks:17:15: error: ...` | 检查文件第 17 行的这条 `CHECK-NEXT` 没有匹配上 |
| `<stdin>:10:11: note: scanning from here` | 它从输出第 10 行末尾，即上一个匹配 `tok_extern` 之后开始找 |
| `<stdin>:11:1: note: possible intended match here` | FileCheck 用模糊匹配猜测你想匹配的是第 11 行，并把它列出来 |

后面还会附上整段输入，并用标记注明每条指令匹配到了哪一行（`-dump-input=help` 解释这些标记的含义）。

排查顺序：先看 `error` 那一行确认是哪条指令；再看 `possible intended match` 判断是输出错了还是期望写错了；必要时手动执行 `RUN:` 行里的命令，直接看完整输出。

**写测试的正确流程**：先手动执行一遍命令，确认输出是对的，再把它写进 `CHECK`。然后**故意改坏一次**，确认测试真的会失败。从没见过失败的测试，可能什么都没在检查，比如 RUN 行写错、前缀拼错，FileCheck 没找到任何指令也会报错，但别的错误不一定会暴露。

### 1.6 常用命令

```bash
ninja -C build check                         # 重新构建 toy 并运行全部测试
llvm-lit -v test                             # 只运行测试，失败时显示详情
llvm-lit -v test/lexer/tokens.ks             # 只运行一个测试
llvm-lit -a test/lexer                       # 通过的测试也显示命令和输出
llvm-lit -v --filter=parser test             # 只运行路径匹配 parser 的测试
```

### 1.7 当前状态与后续

现在的测试只有一个 `test/lexer/tokens.ks`。规划里 D3 还有 `test/parser/precedence.ks`、`definitions.ks`、`errors.ks` 三个语法测试，以及提交 4 的 `test/lexer/numbers.ks`，这次跳过了。从 D4 起，每个功能提交都会带自己的测试，到时候语法这一层没有测试覆盖，`ParseBinOpRHS` 一类的改动出错就不容易发现。建议在 D4 开始前，至少把 `precedence.ks` 补上：它只需要把 D2 笔记里 `a+b*c-d` 那棵树写成 `CHECK` 行，用不了十分钟。

---

## 2. LLVM 命令行工具

### 2.1 `llvm::cl`：声明即注册

LLVM 的命令行库 `llvm::cl`（`llvm/Support/CommandLine.h`）用起来和 getopt 完全不同：**每个选项是一个全局对象，声明它就完成了注册**。

```cpp
static cl::OptionCategory ToyCategory("toy options");

static cl::opt<bool> DumpTokens("dump-tokens",
                                cl::desc("Print the token stream and exit"),
                                cl::cat(ToyCategory));
```

- `cl::opt<bool>` 的构造函数在**程序启动、进入 `main` 之前**执行（C++ 全局对象的静态初始化），它把自己登记到一张进程级的全局选项表里。
- `cl::ParseCommandLineOptions(argc, argv, ...)` 只负责扫描 `argv`，到表里查名字，再把值写回对应的对象。
- 之后 `DumpTokens` 就当作 `bool` 用：`if (DumpTokens)`。`cl::opt<T>` 能隐式转换成 `T`。

与 getopt 的对比：

| | getopt | `llvm::cl` |
|---|---|---|
| 选项定义在哪 | `main` 里集中写一个选项表 | 分散在各个源文件，谁用谁声明 |
| 解析结果存哪 | `switch` 里手动写进变量 | 自动存进选项对象本身 |
| `-help` | 自己写 | 根据 `cl::desc` 自动生成 |
| 跨模块 | 每加一个选项都要改 `main` | 库里声明的选项，链接进来就生效 |

最后一点是关键：LLVM 的每个库都可以在自己的 `.cpp` 里声明选项，比如某个 pass 的调优阈值、调试开关。它们链接进哪个可执行文件，哪个程序就有这些选项。这就是 `opt` 有几百个选项的来历，也是 D5 之后 toy 能直接使用 `-print-changed`、`-debug-pass-manager` 这些选项的原因。以后自己写 pass 时，阈值参数也照这个方式声明。

### 2.2 选项的修饰符

`cl::opt` 的构造参数是一串修饰符，顺序随意：

| 修饰符 | 作用 | 本项目用在哪 |
|---|---|---|
| 字符串字面量 | 选项名，如 `"dump-tokens"`。区分大小写 | D3 |
| `cl::desc("...")` | `-help` 里显示的说明 | D3 |
| `cl::cat(Category)` | 归入某个类别，`-help` 按类别分组显示 | D3 |
| `cl::init(value)` | 默认值 | D4 起 |
| `cl::values(clEnumValN(...), ...)` | 枚举型选项的可选值 | D4 的 `-parse-only` / `-emit-llvm` |
| `cl::Positional` | 位置参数（不带 `-`） | D12 的输入文件名 |
| `cl::value_desc("filename")` | `-help` 里值的占位名 | D12 的 `-o <filename>` |
| `cl::Hidden` | 只在 `-help-hidden` 里显示 | 调试用的开关 |

除了 `cl::opt`，还有 `cl::list<T>`（可以重复出现的选项，如多个 `-I`）和 `cl::alias`（给选项起别名）。

**命令行写法**：一个横线和两个横线都接受（`-dump-tokens` 与 `--dump-tokens` 等价）；带值的选项写成 `-o file` 或 `-o=file`；布尔选项可以显式写 `-dump-ast=false`。

### 2.3 `HideUnrelatedOptions`

```cpp
cl::HideUnrelatedOptions(ToyCategory);
cl::ParseCommandLineOptions(argc, argv, "Kaleidoscope toy compiler\n");
```

- `HideUnrelatedOptions` 把**不属于给定类别**的选项都标记为隐藏，`-help` 里只剩我们自己的选项和 Generic Options。
- 隐藏只影响显示，不影响使用：被隐藏的选项照样能用，`-help-hidden` 也仍然能列出它们。
- 现在只链接了 Support 库，隐藏的选项还不多。D6 链接了优化器和 JIT 之后，不调用这一行的话，`-help` 会有 200 行左右。
- `ParseCommandLineOptions` 的第三个参数就是 `-help` 第一行 `OVERVIEW:` 后面的说明文字。

### 2.4 `InitLLVM`

```cpp
int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
```

它是一个 RAII 对象：构造时完成初始化，`main` 返回时在析构函数里做清理。主要做三件事：

- **安装信号处理函数**。程序收到 `SIGSEGV`、`SIGABRT`（`assert` 失败时触发）等信号时，打印带函数名和行号的调用栈：

```text
PLEASE submit a bug report to https://github.com/llvm/llvm-project/issues/ ...
Stack dump:
0.	Program arguments: ./build/toy
 #0 0x... llvm::sys::PrintStackTrace(llvm::raw_ostream&, int)
 ...
 #6 0x... gettok() .../src/toy.cpp:120:14
```

  符号化依赖 PATH 里的 `llvm-symbolizer`。你自己的 LLVM 带断言，D4 以后 API 用错时，第一手线索就是这个调用栈：从栈顶往下找到第一个 `src/toy.cpp` 的帧，就是出错的调用点。
- **记录程序参数**，即调用栈里的 `Program arguments:` 那一行。
- **退出时调用 `llvm_shutdown()`**，释放 LLVM 内部的全局对象。

`opt`、`llc`、`clang` 的 `main` 都以 `InitLLVM` 开头。

### 2.5 `raw_ostream`：`outs()` 和 `errs()`

LLVM 不用 `<iostream>`，而是用自己的输出流 `raw_ostream`（`llvm/Support/raw_ostream.h`）：

| 函数 | 对应 | 缓冲 |
|---|---|---|
| `outs()` | stdout | 有缓冲 |
| `errs()` | stderr | 无缓冲，写入后立即出现 |
| `dbgs()` | 调试输出（默认 stderr） | 配合 `-debug` / `LLVM_DEBUG` 使用 |

用法和 iostream 一样，用 `<<` 串起来：`outs() << "tok_number " << NumVal << '\n';`。

LLVM 不用 iostream 的原因：没有 locale 和格式化状态的开销，速度更快；不会引入 iostream 的全局静态构造函数；能直接输出 LLVM 自己的类型（`StringRef`、`Value`、`Type` 等）。LLVM 的编码规范明确禁止在库代码里 `#include <iostream>`。

**本项目的分流约定**：

- **成块的产物走 stdout**：`-dump-tokens` 的 token 列表，D4 以后 `-emit-llvm` 输出的 IR。这样它们可以直接接管道，交给 `opt`、`FileCheck` 处理。
- **诊断和交互信息走 stderr**：报错、`Parsed ...`、`ready>` 提示符、`-dump-ast` 的输出。

两者混在一起检查时（`RUN` 行里写 `2>&1`），要注意 stdout 有缓冲而 stderr 没有，两边的内容不会按代码执行的顺序交错出现。这就是为什么 REPL 相关的输出都走 stderr，以保证它们之间的先后顺序；D3 提交 4 测数字报错时，也因此要用 `CHECK-DAG`。

`double` 通过 `raw_ostream` 输出时用的是 `%e` 格式（`4.500000e+00`），和 D1 里 `printf("%g")` 的输出（`4.5`）不同。这正好和 LLVM IR 里浮点常量的写法一致。

### 2.6 `sys::Process::StandardInIsUserInput()`

```cpp
ShowPrompt = sys::Process::StandardInIsUserInput();
```

判断标准输入是不是终端，在 Linux 上就是 `isatty(0)`。终端输入时显示 `ready>` 提示符；管道或文件重定向时不显示，测试输出里就不会混进提示符。`llvm/Support/` 下有一组这类跨平台封装（`sys::fs` 文件系统、`sys::path` 路径处理、`sys::Process` 进程信息），LLVM 代码里不直接调用 POSIX 接口，而是用这些封装。

---

## 3. `dump()` 虚函数

### 3.1 实现回顾

```cpp
class ExprAST {
public:
  virtual ~ExprAST() = default;

  /// Print this node and its children, one node per line, indented by
  /// \p Indent spaces.
  virtual void dump(raw_ostream &OS, unsigned Indent) const = 0;
};

class BinaryExprAST : public ExprAST {
  ...
  void dump(raw_ostream &OS, unsigned Indent) const override {
    OS.indent(Indent) << "BinaryExpr '" << Op << "'\n";
    LHS->dump(OS, Indent + 2);
    RHS->dump(OS, Indent + 2);
  }
};
```

### 3.2 纯虚函数与抽象类

`= 0` 让 `dump` 成为**纯虚函数**：基类只声明接口，不提供实现。

- 含有纯虚函数的类是**抽象类**，不能直接创建对象。`ExprAST` 本来就不该被实例化，它只代表「某种表达式」。
- 派生类**必须**实现所有纯虚函数，否则它自己也是抽象类。漏写时，报错出现在**创建对象**的地方，而不是类定义处：

```text
unique_ptr.h:1070:34: error: allocating an object of abstract class type 'NumberExprAST'
toy.cpp:225:22: note: in instantiation of function template specialization
                'std::make_unique<NumberExprAST, double &>' requested here
toy.cpp:108:16: note: unimplemented pure virtual method 'dump' in 'NumberExprAST'
```

  第一行报在标准库头文件 `unique_ptr.h` 里，容易让人摸不着头脑。真正的原因在第三行的 note：`NumberExprAST` 漏了 `dump`。模板代码里的报错都是这样，要往下看 note，找到落在自己代码里的那一行。

这正是选择纯虚函数而不是给一个空的默认实现的理由：以后新增节点类时（D7 的 `IfExprAST`，D8 的 `ForExprAST`……）如果忘了写 `dump`，编译就过不去，而不会默默打印出一片空白。D4 的 `codegen()` 也是纯虚函数，道理相同。

### 3.3 `override` 与 `const`

- **`override`**：告诉编译器「这个函数是要覆盖基类的虚函数」。签名有任何不一致，编译器都会报错。例如漏写了 `const`：

```text
error: non-virtual member function marked 'override' hides virtual member function
```

  不写 `override` 时，签名不一致的函数会被当成一个**新的**普通函数，与基类的虚函数无关。编译能通过，但通过基类指针调用 `dump` 时走不到它。如果基类是纯虚函数，结果就是上面那个「抽象类不能实例化」的错误；如果基类有默认实现，就会悄悄调用错误的版本。所以 LLVM 的编码规范要求覆盖虚函数时一律写 `override`。

- **`const`**（参数列表后面那个）：表示 `dump` 不修改对象。打印是只读操作，标成 `const` 后，持有 `const ExprAST &` 的代码也能调用它。基类和派生类的 `const` 必须一致，否则就不是同一个函数签名。

### 3.4 动态分派

`LHS` 的静态类型是 `unique_ptr<ExprAST>`，`LHS->dump(...)` 这一行编译时并不知道 `LHS` 实际是哪种节点。运行时通过对象里的 vtable 指针找到实际类型的 `dump`：

```text
LHS->dump(OS, 2)
   │
   ├─ 读 LHS 所指对象开头的 vptr
   ├─ 在 vtable 里取 dump 槽位的函数地址
   └─ 实际是 VariableExprAST → 调用 VariableExprAST::dump
```

用 C 的写法表达就是：

```c
struct expr_ops { void (*dump)(const struct expr *, FILE *, unsigned); /* ... */ };
struct expr     { const struct expr_ops *ops; };
e->ops->dump(e, out, indent + 2);
```

内核的 `struct file_operations` 是同一回事。C++ 只是让编译器替你生成函数指针表、填好表项、在调用处插入查表代码。

### 3.5 递归结构：先序遍历

每个节点的 `dump` 都是先打印自己，再让子节点打印，这就是树的**先序遍历**：

```text
BinaryExprAST::dump(Indent=2)          打印 "  BinaryExpr '+'"
 ├─ VariableExprAST::dump(Indent=4)    打印 "    VariableExpr a"
 └─ BinaryExprAST::dump(Indent=4)      打印 "    BinaryExpr '*'"
     ├─ NumberExprAST::dump(Indent=6)  打印 "      NumberExpr 4.000000e+00"
     └─ VariableExprAST::dump(Indent=6) 打印 "      VariableExpr b"
```

几个设计点：

- **缩进作为参数往下传**，每深一层加 2。没有用全局变量记录当前深度，所以函数没有副作用，可以从任意一个节点开始打印。
- **输出流作为参数传入**（`raw_ostream &OS`），而不是写死 `errs()`。调用方决定打到哪里：现在传 `errs()`，测试时也可以传一个 `raw_string_ostream`，把结果写进字符串。
- **`OS.indent(n)` 返回流的引用**，所以可以接着写 `<< "BinaryExpr '"`，整行一个表达式写完。
- **递归调用 `dump` 的时候，调用方不需要知道子节点的类型**，这正是动态分派的价值所在。

这个「对每种节点做一件事，并递归处理子节点」的结构，到 D4 会再用一次：`codegen()` 也是纯虚函数、也是递归，只不过改成了**后序遍历**，先为子节点生成代码，拿到它们的值，再为自己生成指令。

### 3.6 为什么 `PrototypeAST` 和 `FunctionAST` 的 `dump` 不是虚函数

它们不继承 `ExprAST`，也没有派生类，不存在「通过基类指针调用、需要按实际类型分派」的情况，所以写成普通成员函数就够了，也就没有 `override`。

两者的参数列表处理方式也不同：`CallExprAST` 的参数是实参，每个都是一棵表达式子树，要递归调用 `dump` 打印；`PrototypeAST` 的参数是形参，只是一串名字，用 `interleave(Args, OS, " ")` 在一行里打印完。

### 3.7 LLVM 中的 `dump()` 惯例

LLVM 里几乎每个核心类都有一对方法：

```cpp
void print(raw_ostream &OS) const;   // 打印到指定的流
void dump() const;                   // 打印到 dbgs()，专供调试器调用
```

`dump()` 通常带 `LLVM_DUMP_METHOD` 标记，防止它在没有被调用时被链接器删除，从而在 gdb 里可以 `call V->dump()`。D4 会写一个同样用途的 `dumpValue()`。我们的 `dump(OS, Indent)` 实际上相当于 LLVM 的 `print`，在命名上做了简化。

### 3.8 另一种做法：Visitor 模式

给每个节点类加一个虚函数，是「新增一种操作，就要改动所有节点类」的做法。另一种做法是 **Visitor 模式**：节点类只提供一个 `accept`，每种操作写成一个独立的 Visitor 类，里面为每种节点各写一个 `visit` 函数。

| | 虚函数（本项目） | Visitor |
|---|---|---|
| 新增一种操作（如 `dump`、`codegen`） | 要改每个节点类 | 新写一个 Visitor 类，节点类不动 |
| 新增一种节点（如 `IfExprAST`） | 新写一个类即可 | 要改每个 Visitor |
| 代码组织 | 一种节点的所有行为写在一处 | 一种操作的所有分支写在一处 |

这就是所谓的「表达式问题」（expression problem）：两种做法分别让一个方向的扩展变得容易。Kaleidoscope 的节点种类会不断增加（每章都加新节点），而操作只有 `dump`、`codegen` 两种，所以虚函数更合适。Clang 的 AST 节点多达数百种，操作也很多，于是用了 Visitor 一类的模板机制（`RecursiveASTVisitor`、`StmtVisitor`）。LLVM IR 层也有 `InstVisitor`，写 pass 时可以用到。

---

## 4. 本日提交

| # | 提交 | 状态 |
|---|---|---|
| 1 | `[Driver] Parse command-line options with llvm::cl` | 已完成 |
| 2 | `[AST] Add dump() methods and a -dump-ast option` | 已完成 |
| 3 | `[Test] Add lit configuration and lexer tests` | 已完成 |
| 4 | `[Test] Add lit configuration and lexer tests` | 已完成 |
| 5 | `[Docs] Add notes on testing and the driver` | 本文件 |

