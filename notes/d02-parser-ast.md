# D2：语法分析器与 AST（Kaleidoscope Ch.2）

这一天把 D1 的 token 流变成抽象语法树（AST）：

- 定义了 7 个 AST 节点类。
- 写了递归下降语法分析器，其中二元表达式部分用算符优先解析。
- 用 `MainLoop` 替换了 D1 的临时 `main`，程序变成一个只做语法检查的 REPL。

本篇重点记三件事：用到的 C++ 语法、解析器的实现原理、AST 节点与所有权关系。最后解读一次实际的运行记录。

---

## 1. C++ 语法

### 1.1 类、继承与虚析构函数

```cpp
class ExprAST {
public:
  virtual ~ExprAST() = default;
};

class NumberExprAST : public ExprAST {
  double Val;
public:
  NumberExprAST(double Val) : Val(Val) {}
};
```

| 写法 | 含义 | 用 C 的概念来理解 |
|---|---|---|
| `class B : public A` | B 公有继承 A，B 对象里开头就是一个完整的 A | 结构体嵌套：`struct B { struct A base; ... }`，`B*` 可以当 `A*` 用 |
| `virtual` 成员函数 | 调用时按对象的**实际类型**分派 | 对象里藏一个函数指针表（vtable）指针，像内核的 `struct file_operations *f_op` |
| `virtual ~ExprAST() = default;` | 虚析构函数，函数体由编译器生成 | 释放时先查表找到派生类的析构函数，再逐层往上释放 |
| `class` 默认 `private` | 字段 `Val` 外部不可访问 | 无对应，C 结构体成员都是公开的 |

**为什么基类要有虚析构函数**：AST 节点全部通过 `ExprAST *`（或 `unique_ptr<ExprAST>`）持有。释放时执行的是 `delete (ExprAST*)p`：

- 析构函数不是虚函数：只调用 `ExprAST` 的析构，`BinaryExprAST` 里的 `LHS`、`RHS` 不会被释放，是**未定义行为**，实际表现通常是内存泄漏。
- 析构函数是虚函数：通过 vtable 找到 `BinaryExprAST` 的析构函数，它先释放 `LHS`、`RHS`，再执行基类部分。

规则：**一个类只要会被当作基类、通过基类指针删除，就必须有虚析构函数。**

今天的 `ExprAST` 只有析构函数是虚函数。D3 会加 `virtual void dump(...)`，D4 会加 `virtual Value *codegen()`，到时候「按实际类型分派」才真正派上用场。

### 1.2 构造函数与成员初始化列表

```cpp
BinaryExprAST(char Op, std::unique_ptr<ExprAST> LHS, std::unique_ptr<ExprAST> RHS)
    : Op(Op), LHS(std::move(LHS)), RHS(std::move(RHS)) {}
```

- 冒号后面是**成员初始化列表**，在进入函数体之前直接构造每个成员。等价的 C 写法是创建结构体时就给每个字段赋初值。
- `Op(Op)`：括号外是成员，括号里是参数。同名不冲突，LLVM 代码里普遍这样写。
- `unique_ptr` 成员**只能在初始化列表里初始化**，或者在函数体里移动赋值，不能拷贝赋值。所以这里必须写 `std::move`。
- 成员按**类中声明的顺序**初始化，与初始化列表里的书写顺序无关。两者不一致时 `-Wall` 会提示 `-Wreorder`。

### 1.3 `std::unique_ptr`：独占所有权的指针

```cpp
std::unique_ptr<ExprAST> LHS;
```

`unique_ptr<T>` 包着一个 `T*`，有两条规则：

1. **它是这块内存唯一的所有者**：析构时自动 `delete` 所指的对象。
2. **不能拷贝，只能移动**：拷贝构造函数被删除了。如果允许拷贝，就会有两个所有者，析构时 `delete` 两次。

```cpp
std::unique_ptr<ExprAST> a = std::make_unique<NumberExprAST>(1.0);
std::unique_ptr<ExprAST> b = a;             // 编译错误：call to deleted constructor
std::unique_ptr<ExprAST> b = std::move(a);  // 正确：所有权转给 b，a 变为空指针
```

这就是 D2 易错点里那条 `call to deleted constructor` 的来历：**你在拷贝一个不能拷贝的东西**。看到这个报错，就去找漏写 `std::move` 的地方。

**和 C 的对比**：C 里一个指针「该由谁 free」只能写在注释和约定里。`unique_ptr` 把这个约定写进了类型：函数参数或返回值是 `unique_ptr<T>`，就表示所有权随之转移；参数是 `T*` 或 `T&`，就表示只是借用。

**开销**：和裸指针一样大（8 字节），解引用没有额外开销，析构就是一次 `delete`。和带引用计数的 `shared_ptr` 不是一回事。

**常用操作**：

| 操作 | 作用 |
|---|---|
| `p->x`、`*p` | 和裸指针一样访问 |
| `if (!p)`、`return nullptr;` | 判空，以及用空指针构造。`return nullptr;` 能直接返回 `unique_ptr`，因为它有接受 `nullptr` 的构造函数 |
| `p.get()` | 取出裸指针，所有权不变（借用）。D11 会用到 |
| `p.reset()` | 提前释放 |
| `p.release()` | 放弃所有权，返回裸指针，之后要自己负责释放。很少用 |

### 1.4 移动语义与 `std::move`

**`std::move` 本身什么也不做**。它只是一个类型转换，把表达式变成「右值」（可以被拿走资源的值）。真正干活的是被调用的**移动构造函数**，或者**移动赋值运算符**。

对 `unique_ptr` 来说，「移动」就是：

```text
目标.ptr = 源.ptr;
源.ptr   = nullptr;
```

所以两个规则：

- **被移动过的变量不能再用**。`std::move(LHS)` 之后 `LHS` 是空指针，再解引用就是空指针访问。
- **一个变量只能 `std::move` 一次**。它交出去以后，手里就没有东西了。

`ParseBinOpRHS` 里的这一行是今天最典型的移动：

```cpp
LHS = std::make_unique<BinaryExprAST>(BinOp, std::move(LHS), std::move(RHS));
```

执行顺序是：

1. 先把旧的 `LHS`、`RHS` 移进新节点的构造函数参数，两个局部变量变为空。
2. 新节点构造完成，拥有这两棵子树。
3. 再把新节点移动赋值给 `LHS`。

整个过程没有拷贝任何节点，只是在搬指针。表达式树就这样一层层向上长。

**`std::string` 和 `std::vector` 的移动**：同样是「把内部缓冲区的指针搬过来，源对象变空」，不复制内容。

```cpp
ProtoTypeAST(const std::string &Name, std::vector<std::string> Args)
    : Name(Name), Args(std::move(Args)) {}
```

- `Name` 按 `const &` 传入，再拷贝一份：名字一般很短，拷贝也便宜。
- `Args` **按值传入，再 move 进成员**。这叫「sink 参数」写法：调用方 `std::move(ArgNames)` 时全程零拷贝；调用方不 move 时，按值传参那一次就是唯一的一次拷贝。

`vector<unique_ptr<T>>` 的元素也只能移动：

```cpp
Args.push_back(std::move(Arg));   // 不加 move 编译不过
```

### 1.5 `std::make_unique`

```cpp
auto Result = std::make_unique<NumberExprAST>(NumVal);
```

等价于 `std::unique_ptr<NumberExprAST>(new NumberExprAST(NumVal))`，把分配和构造一步做完，并立即交给 `unique_ptr` 管理。中间没有出现裸指针，所以也就不会有忘了释放的裸指针。

### 1.6 派生类指针到基类指针的转换

```cpp
static std::unique_ptr<ExprAST> ParseNumberExpr() {
  auto Result = std::make_unique<NumberExprAST>(NumVal);  // unique_ptr<NumberExprAST>
  getNextToken();
  return std::move(Result);                               // 转成 unique_ptr<ExprAST>
}
```

`unique_ptr<NumberExprAST>` 能转换成 `unique_ptr<ExprAST>`，规则与 `NumberExprAST*` 能转换成 `ExprAST*` 相同。转换过程中所有权一并转移。

这里的 `std::move` 是教程原有的写法。C++14 起，`return` 一个局部变量时编译器会自动尝试移动，所以这里不写 `std::move` 也能编译。

### 1.7 匿名命名空间（不是匿名函数）

```cpp
namespace {
class ExprAST { ... };
...
} // end anonymous namespace
```

- **作用**：里面声明的东西只在本文件可见，等价于给它们加了 C 的 `static`。C 的 `static` 只能修饰函数和变量；类型没有链接属性可言，但类的成员函数、vtable 是有链接属性的。放进匿名命名空间后，即使别的文件也有一个 `ExprAST` 类，链接时也不会冲突。
- **LLVM 的编码规范**：类型放进匿名命名空间，**函数用 `static`**。所以 `ParseXxx`、`getNextToken` 写成 `static`，而 AST 类放进 `namespace {}`。

今天的代码里**没有匿名函数**。C++ 的匿名函数叫 **lambda**，D6 拷贝进来的 `KaleidoscopeJIT.h` 里第一次出现：

```cpp
[](const MemoryBuffer &) { return std::make_unique<SectionMemoryManager>(); }
```

- `[]`：捕获列表，说明这个函数要用到外面的哪些变量。
- `(...)`：参数列表。
- `{...}`：函数体。

它生成一个可调用的对象，当回调传进去用，相当于 C 里「函数指针 + 一个 `void *ctx`」的组合。D6 再细讲。

### 1.8 其他

| 写法 | 含义 |
|---|---|
| `static int CurTok;`（文件作用域） | 和 C 一样：只在本文件可见的全局变量 |
| `static int LastChar = ' ';`（函数内） | 和 C 一样：跨调用保持值的局部静态变量 |
| `static std::unique_ptr<ExprAST> ParseExpression();` | **前置声明**。`ParseParenExpr` 要调用后面才定义的 `ParseExpression`，两者互相递归 |
| `auto LHS = ParsePrimary();` | 由右边推导类型，这里是 `std::unique_ptr<ExprAST>` |
| `if (auto Arg = ParseExpression()) {...}` | 在条件里声明变量：`Arg` 只在这个 `if/else` 里可见，条件是「`Arg` 非空」 |
| `std::map<char, int>` 的 `BinopPrecedence[CurTok]` | **查不到时会插入一个值为 0 的新项再返回**，不是只读查找。所以 `GetTokPrecedence` 要判断 `<= 0`；只想查而不插入时用 `find()` |
| `const std::string &getName() const` | 返回常量引用，不拷贝；末尾的 `const` 表示这个成员函数不修改对象 |
| `std::vector<std::string>()` | 构造一个空 vector 临时对象，给匿名函数当空参数列表 |

---

## 2. 解析器实现原理

### 2.1 文法

Kaleidoscope 的文法写在教程每个解析函数上方的注释里，汇总如下：

```text
top            ::= definition | external | expression | ';'
definition     ::= 'def' prototype expression
external       ::= 'extern' prototype
prototype      ::= identifier '(' identifier* ')'
expression     ::= primary binoprhs
binoprhs       ::= ( binop primary )*
primary        ::= identifierexpr | numberexpr | parenexpr
identifierexpr ::= identifier
                 | identifier '(' ( expression ( ',' expression )* )? ')'
numberexpr     ::= number
parenexpr      ::= '(' expression ')'
```

- 左边的名字叫**非终结符**，代表一类语法结构。
- 引号里的字符、`identifier`、`number` 叫**终结符**，就是 D1 产出的 token。
- 每一行叫**产生式**，说明一个非终结符可以展开成什么。
- 这种「左边只有一个非终结符」的文法叫**上下文无关文法**。和 D1 的正则表达式相比，它多了递归能力：`parenexpr` 里套 `expression`，`expression` 里又可以有 `parenexpr`，所以能描述任意深度的括号嵌套。正则表达式做不到这一点。

### 2.2 递归下降

**做法**：每个非终结符写成一个函数，函数体照着产生式的右边写。

- 遇到**终结符**：检查 `CurTok` 是不是它，是就 `getNextToken()` 吃掉，不是就报错。
- 遇到**非终结符**：调用它对应的函数。

逐条对应关系：

| 产生式 | 函数 | 代码的写法 |
|---|---|---|
| `numberexpr ::= number` | `ParseNumberExpr` | 用 `NumVal` 建节点，吃掉数字 |
| `parenexpr ::= '(' expression ')'` | `ParseParenExpr` | 吃 `(` → 调 `ParseExpression` → 检查并吃 `)` |
| `identifierexpr` | `ParseIdentifierExpr` | 吃标识符；后面不是 `(` 就是变量，否则循环解析参数 |
| `primary` | `ParsePrimary` | `switch (CurTok)` 选择分支 |
| `expression` | `ParseExpression` | 先 `ParsePrimary`，再 `ParseBinOpRHS(0, ...)` |
| `prototype` | `ParsePrototype` | 函数名、`(`、`while` 循环读参数名、`)` |
| `definition` | `ParseDefinition` | 吃 `def` → `ParsePrototype` → `ParseExpression` |
| `external` | `ParseExtern` | 吃 `extern` → `ParsePrototype` |
| `top` | `MainLoop` | `switch (CurTok)` 分派到三个 `HandleXxx` |

名字叫「递归下降」，是因为分析从 `top` 开始自顶向下展开，而且函数之间会递归调用：`ParseExpression` → `ParsePrimary` → `ParseParenExpr` → `ParseExpression`。

**所有 `ParseXxx` 共同遵守的约定**：

> 进入时，`CurTok` 是本结构的**第一个** token；返回时，`CurTok` 是本结构**之后的第一个** token。

检查每个函数是否写对，就拿这条约定逐个核对 `getNextToken()` 的调用。例如 `ParseIdentifierExpr` 末尾的 `getNextToken(); // eat )`：如果漏掉它，返回时 `CurTok` 还停在 `)` 上，违反约定，上一层就会把这个 `)` 当成下一个结构的开头。

### 2.3 向前看一个 token：LL(1) 与预测分析

`CurTok` 是「已读入、尚未消费」的那个 token，和 D1 的 `LastChar` 是同一个思路，只是高了一层：

| | 保存「多读的那一个」 | 读下一个 |
|---|---|---|
| 词法分析 | `LastChar`（字符） | `getchar()` |
| 语法分析 | `CurTok`（token） | `getNextToken()` |

**预测分析**：只看当前这一个 token，就能决定用哪条产生式，不需要试错，也不需要回退。`ParsePrimary` 就是典型：

```cpp
switch (CurTok) {
case tok_identifier: return ParseIdentifierExpr();
case tok_number:     return ParseNumberExpr();
case '(':            return ParseParenExpr();
default:             return LogError("unknown token when expecting an expression");
}
```

能这样做，是因为三条产生式的第一个 token 各不相同。用编译原理的术语说，就是三者的 **FIRST 集合**互不相交。满足这个条件、只需向前看 1 个 token 的文法叫 **LL(1)**：

- 第一个 L：从左（Left）到右扫描输入。
- 第二个 L：每一步展开最左（Leftmost）边的非终结符。
- 1：向前看 1 个 token。

**提取左因子**：`identifierexpr` 的两条产生式都以 `identifier` 开头，只看第一个 token 分不出来。代码的处理方法是**先把公共前缀吃掉，再看下一个 token**：

```cpp
std::string IdName = IdentifierStr;   // 先保存名字：getNextToken 会覆盖 IdentifierStr
getNextToken();                       // 吃掉公共前缀 identifier
if (CurTok != '(')                    // 再看下一个 token 决定分支
  return std::make_unique<VariableExprAST>(IdName);
// ... 否则是函数调用
```

这相当于把文法改写成 `identifierexpr ::= identifier ( ε | '(' args ')' )`。

### 2.4 表达式为什么不能照朴素文法直接写

描述二元表达式最自然的文法是：

```text
expr ::= expr '+' expr | expr '*' expr | primary
```

这个文法有两个问题：

1. **有二义性**：`a+b*c` 可以解析成 `(a+b)*c`，也可以解析成 `a+(b*c)`。文法本身没有规定哪个对。
2. **左递归**：`expr ::= expr '+' ...`，照着写成函数就是 `ParseExpr()` 一进来先调用 `ParseExpr()`，无限递归，栈溢出。

教科书的标准解法是**每个优先级一层非终结符**，再把左递归改写成循环：

```text
expr   ::= term   ( ('+' | '-') term )*
term   ::= factor ( '*' factor )*
factor ::= primary
```

这样可行，但有两个缺点：每个优先级就得写一个函数（C 有 15 级）；优先级写死在函数结构里，运行时无法添加新运算符。

### 2.5 算符优先解析（precedence climbing）

教程的做法是：把表达式看成 `primary (binop primary)*`，也就是「一个操作数，后面跟着若干个『运算符 + 操作数』对」，再用一张优先级表决定它们怎么结合。

```cpp
BinopPrecedence['<'] = 10;
BinopPrecedence['+'] = 20;
BinopPrecedence['-'] = 20;
BinopPrecedence['*'] = 40;   // 数字越大，结合越紧
```

核心函数：

```cpp
static std::unique_ptr<ExprAST> ParseBinOpRHS(int ExprPrec, std::unique_ptr<ExprAST> LHS)
```

**参数含义**：

- `LHS`：已经解析好的左操作数。
- `ExprPrec`：本次调用**有权吃掉的最低优先级**。遇到比它低的运算符就停下，交还给上层处理。

**循环体的逻辑**：

1. 看当前运算符的优先级 `TokPrec`。如果 `TokPrec < ExprPrec`，或者当前根本不是运算符（`GetTokPrecedence` 返回 -1），就返回 `LHS`。
2. 吃掉运算符，解析它右边的 primary，作为 `RHS`。
3. **偷看下一个运算符** `NextPrec`。如果它比当前运算符结合得更紧（`TokPrec < NextPrec`），说明 `RHS` 应该先和后面的部分结合：递归调用 `ParseBinOpRHS(TokPrec + 1, RHS)`，让它把 `RHS` 扩展成一棵更大的子树。
4. 把 `LHS 运算符 RHS` 合并成一个新节点，作为新的 `LHS`，回到第 1 步。

**完整跟踪 `a+b*c-d;`**：

```text
ParseExpression
  ParsePrimary → a
  ParseBinOpRHS(ExprPrec=0, LHS=a)
  │ 第 1 轮：CurTok='+'，TokPrec=20 ≥ 0，吃掉 '+'
  │         RHS = ParsePrimary → b
  │         NextPrec('*') = 40 > 20 → 递归
  │   ParseBinOpRHS(ExprPrec=21, LHS=b)
  │   │ 第 1 轮：CurTok='*'，TokPrec=40 ≥ 21，吃掉 '*'
  │   │         RHS = c，NextPrec('-') = 20，不大于 40 → 不递归
  │   │         LHS = (b*c)
  │   │ 第 2 轮：CurTok='-'，TokPrec=20 < 21 → 返回 (b*c)
  │   ←
  │         RHS = (b*c)，LHS = (a+(b*c))
  │ 第 2 轮：CurTok='-'，TokPrec=20 ≥ 0，吃掉 '-'
  │         RHS = d，NextPrec(';') = -1 → 不递归
  │         LHS = ((a+(b*c))-d)
  │ 第 3 轮：CurTok=';'，TokPrec=-1 < 0 → 返回
  ←
结果：((a+(b*c))-d)
```

```text
          '-'
         /   \
       '+'    d
      /   \
     a    '*'
         /   \
        b     c
```

**优先级是怎么实现的**：`*` 比 `+` 紧，所以在看到 `*` 时进入递归。递归层把 `b*c` 组装完，交回给上层当 `+` 的右操作数。

**结合性是怎么实现的**：递归时传的门槛是 `TokPrec + 1` = 21，而不是 `TokPrec`。递归层碰到同为 20 的 `-` 时，20 < 21，不能吃，只能返回。于是 `-` 由外层处理，左边是已经合并好的 `a+(b*c)`，得到左结合。

- 如果门槛写成 `TokPrec`，同级运算符会被递归层吃掉，`a-b-c` 就会变成 `a-(b-c)`，即右结合。D11 讲赋值运算符 `=` 时会再回到这一点：赋值需要右结合。

**这个方法的好处**：

- 一个函数处理所有二元运算符，优先级只是表里的数据。
- 运行时往表里加一项，就能扩展文法。Ch.6 的用户自定义运算符就是这样实现的，解析器一行都不用改。
- Clang 解析 C/C++ 表达式也用这个方法（`Parser::ParseRHSOfBinaryExpression`）。

### 2.6 错误处理与错误恢复

- **报错**：`LogError` 打印信息后返回 `nullptr`。调用者见到 `nullptr` 就继续返回 `nullptr`，一路传到顶层。这和 C 里逐层检查返回值是同一种做法，LLVM 不使用 C++ 异常。
- **恢复**：顶层的 `HandleXxx` 在失败时调用 `getNextToken()` 跳过一个 token，再回到 `MainLoop` 继续。这是最简单的**恐慌模式**（panic mode）恢复：丢弃输入，直到看起来可以重新开始为止。
- **副作用**：只跳过一个 token，剩下的残片会被当成新的结构来解析，于是一个错误常常引出第二条报错。

例子：`def foo(x y) x+y );`

1. `x+y` 之后的 `)` 不是运算符，表达式结束，定义解析成功，输出 `Parsed a function definition.`
2. 回到顶层，`)` 被当作一个新的顶层表达式的开头。`ParsePrimary` 不认识它，报 `unknown token when expecting an expression`。
3. 跳过 `)`，接着处理 `;`。

### 2.7 顶层循环

```text
MainLoop:
  ';'        → 吃掉，什么也不做
  tok_def    → HandleDefinition
  tok_extern → HandleExtern
  tok_eof    → 返回
  其他       → HandleTopLevelExpression（包装成 __anon_expr 函数）
```

顶层表达式被包装成一个**名为 `__anon_expr`、没有参数的函数**。这样后续阶段只需要处理「函数」这一种东西。D6 的 JIT 正是靠这个名字找到它、调用它，求出表达式的值。

注意：分号不是语句结束符。它只在顶层出现时被忽略。表达式在哪里结束，取决于 token 能不能再接下去，与分号无关。

---

## 3. AST 节点一览与所有权

### 3.1 节点表

| 类 | 继承自 | 字段 | 表示什么 | 由谁创建 |
|---|---|---|---|---|
| `ExprAST` | — | 无（只有虚析构函数） | 所有表达式的基类 | 不直接创建 |
| `NumberExprAST` | `ExprAST` | `double Val` | 数字字面量 `4.0` | `ParseNumberExpr` |
| `VariableExprAST` | `ExprAST` | `std::string Name` | 变量引用 `x` | `ParseIdentifierExpr`（标识符后面不是 `(`） |
| `BinaryExprAST` | `ExprAST` | `char Op`；`unique_ptr<ExprAST> LHS, RHS` | 二元运算 `a+b` | `ParseBinOpRHS` |
| `CallExprAST` | `ExprAST` | `std::string Callee`；`vector<unique_ptr<ExprAST>> Args` | 函数调用 `foo(1, x)` | `ParseIdentifierExpr`（标识符后面是 `(`） |
| `ProtoTypeAST` | — | `std::string Name`；`vector<string> Args` | 函数签名：名字和参数名 | `ParsePrototype`；`ParseTopLevelExpr` 直接构造 `__anon_expr` |
| `FunctionAST` | — | `unique_ptr<ProtoTypeAST> Proto`；`unique_ptr<ExprAST> Body` | 函数定义 = 签名 + 函数体 | `ParseDefinition`、`ParseTopLevelExpr` |

几点观察：

- **没有括号节点**。`ParseParenExpr` 直接返回括号里的表达式，括号的作用已经体现在树的形状里。这正是 AST 和**分析树**（parse tree）的区别：分析树为每条产生式、每个 token 都建节点，括号、逗号、分号都在里面；AST 只保留后续阶段需要的东西。
- **没有类型字段**。Kaleidoscope 只有 `double` 一种类型，所以节点里不需要记录类型。
- **`ProtoTypeAST` 和 `FunctionAST` 不是表达式**，不继承 `ExprAST`。它们只出现在顶层，不会出现在别的表达式内部。
- **参数只存名字**。`Args` 是 `vector<string>`，因为参数没有类型可写。参数个数由 `Args.size()` 隐含表示。

### 3.2 所有权关系

以 `def foo(x y) x+foo(y, 4.0);` 为例，图中每条实线都是一个 `unique_ptr`（独占所有权）：

```text
unique_ptr<FunctionAST>                    ← ParseDefinition 返回给 HandleDefinition
 ├─ Proto: unique_ptr<ProtoTypeAST>
 │    Name = "foo", Args = ["x", "y"]      ← string 和 vector 是值成员，随对象一起释放
 └─ Body: unique_ptr<ExprAST> → BinaryExprAST  Op='+'
           ├─ LHS → VariableExprAST  Name="x"
           └─ RHS → CallExprAST  Callee="foo"
                     └─ Args: vector<unique_ptr<ExprAST>>
                           ├─ [0] → VariableExprAST  Name="y"
                           └─ [1] → NumberExprAST  Val=4.0
```

**所有权规则**：

- 整棵树是一棵**严格的树**：每个节点只有一个父节点拥有它，没有共享，也没有环。这正是 `unique_ptr` 适用的场景。
- **父节点拥有子节点**。释放根节点时，析构函数逐层自动执行，整棵树释放干净。全程没有一行 `delete`。
- **字符串、向量是值成员**，不需要单独管理。

**所有权的流动**：节点在解析函数里被创建，然后一路向上移交，直到挂在父节点上：

```text
make_unique（在 ParseXxx 中创建）
   → return（移出函数）
   → 调用者的局部变量（auto LHS / RHS / Arg）
   → std::move 进父节点的构造函数或 vector
   → ……
   → 根节点 FunctionAST 返回给 HandleXxx
```

**根节点今天的去向**：

```cpp
if (ParseDefinition()) {               // 返回的 unique_ptr<FunctionAST> 是个临时对象
  fprintf(stderr, "Parsed a function definition.\n");
}                                      // if 条件求值结束时临时对象析构 → 整棵树被释放
```

解析完立刻就扔掉了，所以今天的 REPL 只能做语法检查。D3 会把它接住打印出来（`-dump-ast`），D4 会把它交给 `codegen()` 生成 IR。

---

## 4. 运行记录解读

```text
$ ./build/toy
ready> def foo(x y) x+y y;
ready> Parsed a function definition.
ready> Parsed a top-level expr
ready> def foo(x y) x+y;
ready> Parsed a function definition.
ready> y;
ready> Parsed a top-level expr
ready> a+b*c-d
ready> a+b*c-d;
Parsed a top-level expr
ready> Parsed a top-level expr
ready> ready>
```

**第 1 行：`def foo(x y) x+y y;` 被解析成两样东西。** `x+y` 之后的 `y` 不是运算符，表达式在这里结束，于是函数体是 `x+y`。剩下的 `y` 被当作一个新的顶层表达式。这说明表达式在哪结束，由「下一个 token 能不能接上」决定，与分号和换行都无关。

**提示符错位。** 每一行都是先打印 `ready> `，后打印结果，原因如下：

- `MainLoop` 每转一圈先打印一次 `ready> `，再根据 `CurTok` 分派。
- 终端是**行缓冲**的：敲回车后，整行一次性进入输入缓冲区。
- 语法分析器从缓冲区里逐个取 token，一行里有几个结构，就连续处理几圈，每圈都打印一次提示符。缓冲区读空以后，`getchar()` 才阻塞下来等你输入。
- 最后两个 `ready> ready>`：先处理掉末尾的 `;`，打印一次；按 Ctrl-D 之后，`CurTok` 变成 `tok_eof`，进入循环时又打印一次，然后退出。

**`a+b*c-d` 不加分号回车，没有任何反应。** 这是「向前看一个 token」最直观的体现：

1. 读到 `d` 以后，`ParseBinOpRHS` 必须再看一个 token，才知道 `d` 后面还有没有运算符。比如下一行可能是 `* 2`，那表达式就还要继续。
2. 缓冲区已经空了，`getchar()` 阻塞，于是什么也不输出。
3. 你接着输入第二行 `a+b*c-d;`，解析器拿到的第一个 token 是 `a`。它不是运算符，第一个表达式到此结束，输出 `Parsed a top-level expr`。
4. 第二行的 `a+b*c-d` 随后被解析，遇到 `;` 时立刻结束，紧接着输出第二个 `Parsed a top-level expr`。

所以 REPL 里在顶层表达式末尾加分号，并不是语法要求，而是为了让解析器**不用等下一行就能确定表达式已经结束**。

---

## 5. 与参考实现的差异

对照 `Chapter2/toy.cpp`（明天的 `scripts/diff-ref.sh 2` 也会显示这几处）：

| 差异 | 影响 | 建议 |
|---|---|---|
| `ProtoTypeAST` 的 T 是大写，教程是 `PrototypeAST` | 功能上没有区别；但之后每章对照参考代码时，每处出现都会成为一条差异，干扰 diff | 统一改名为 `PrototypeAST`，作为一个 NFC 提交：`[AST] Rename ProtoTypeAST to PrototypeAST (NFC)` |
| 没有 `#include <utility>` | `std::move` 定义在 `<utility>` 里。现在能编译，是因为别的头文件间接包含了它；换一个标准库实现就可能编译不过 | 补上 |
| `LogError`、`LogErrorP` 没有 `static` | 参考实现也没有，无实际影响 | 保持与教程一致 |

---

## 6. 本日提交

| # | 提交 |
|---|---|
| 1 | `[AST] Add expression, prototype and function nodes` |
| 2 | `[Parser] Add recursive descent and operator-precedence parsing for expressions` |
| 3 | `[Parser] Parse prototypes, definitions and externs` |
| 4 | `[Driver] Replace the token dumper with a REPL main loop` |
| 5 | `[Build] Add diff-ref.sh to compare toy.cpp with the tutorial sources` |
| 6 | `[Docs] Add notes on the parser and AST` |

tag：`ch2`。

## 7. 遗留问题

- 解析结果当场就被丢弃了，看不到树的样子 → D3 加 `-dump-ast`。
- `-x` 无法解析：语言里还没有一元运算符 → Ch.6 加入。
- 同级运算符一律左结合，`=` 需要右结合 → D11 遇到，D14 的练习 A 处理。
- 报错没有行号和列号 → D13 给 token 加上源码位置后补上。