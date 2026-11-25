
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <llvm/IR/PassInstrumentation.h>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/StandardInstrumentations.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Scalar/Reassociate.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"

using namespace llvm;

static cl::OptionCategory ToyCategory("toy options");

/// What toy does with the code it parses.
enum ActionKind {
  RunInteractive, ///< REPL: print the IR of every top-level item as it is read.
  ParseOnly,      ///< Stop after parsing; nothing is generated.
  EmitLLVMIR,     ///< Print the complete module after processing all input.
};

static cl::opt<ActionKind> Action(
    cl::desc("Action to perform (default: interactive REPL):"),
    cl::init(RunInteractive),
    cl::values(clEnumValN(ParseOnly, "parse-only",
                          "Parse the input and stop; combine with "
                          "-dump-ast to inspect the result"),
               clEnumValN(EmitLLVMIR, "emit-llvm",
                          "Compile the whole input and print its LLVM IR "
                          "to stdout")),
    cl::cat(ToyCategory));

static cl::opt<bool> DumpTokens("dump-tokens",
                                cl::desc("Print the token stream and exit"),
                                cl::cat(ToyCategory));
static cl::opt<bool>
    DumpAST("dump-ast",
            cl::desc("Print the AST of every top-level item after parsing"),
            cl::cat(ToyCategory));

static cl::opt<bool>
    NoOpt("O0", cl::desc("Do not run the function optimization pipeline"), cl::cat(ToyCategory));

static cl::opt<bool> DebugPM(
    "debug-pass-manager",
    cl::desc("Print pass manager debugging information"),
    cl::cat(ToyCategory));

enum Token {
  tok_eof = -1,

  // commands
  tok_def = -2,
  tok_extern = -3,

  // primary
  tok_identifier = -4,
  tok_number = -5
};

static std::string IdentifierStr; // Filled in if tok_identifier
static double NumVal;             // Filed in if tok_number

/// gettok - Return the next token from standard input.
static int gettok() {
  static int LastChar = ' ';

  // Skip any whitespace.
  while (isspace(LastChar)) {
    LastChar = getchar();
  }

  if (isalpha(LastChar)) { // identifier: [a-zA-Z][a-zA-Z0-9]*
    IdentifierStr = LastChar;
    while (isalnum((LastChar = getchar()))) {
      IdentifierStr += LastChar;
    }

    if (IdentifierStr == "def") {
      return tok_def;
    }
    if (IdentifierStr == "extern") {
      return tok_extern;
    }
    return tok_identifier;
  }

  if (isdigit(LastChar) || LastChar == '.') { // Number: [0-9.]+
    std::string NumStr;
    do {
      NumStr += LastChar;
      LastChar = getchar();
    } while (isdigit(LastChar) || LastChar == '.');

    NumVal = strtod(NumStr.c_str(), nullptr);
    return tok_number;
  }

  if (LastChar == '#') {
    // Comment until end of line.
    do {
      LastChar = getchar();
    } while (LastChar != EOF && LastChar != '\n' && LastChar != '\r');

    if (LastChar != EOF) {
      return gettok();
    }
  }

  // Check for end of file. Don't eat the EOF.
  if (LastChar == EOF) {
    return tok_eof;
  }

  // Otherwise, just return the character as its ascii value.
  int ThisChar = LastChar;
  LastChar = getchar();
  return ThisChar;
}

namespace {

class ExprAST {
public:
  virtual ~ExprAST() = default;
  virtual Value *codegen() = 0;

  /// Print this node and its children, one node per line, indented by
  /// \p Indent spaces.
  virtual void dump(raw_ostream &OS, unsigned Indent) const = 0;
};

class NumberExprAST : public ExprAST {
  double Val;

public:
  NumberExprAST(double Val) : Val(Val) {}
  void dump(raw_ostream &OS, unsigned Indent) const override {
    OS.indent(Indent) << "NumberExpr " << Val << '\n';
  }
  Value *codegen() override;
};

class VariableExprAST : public ExprAST {
  std::string Name;

public:
  VariableExprAST(const std::string &Name) : Name(Name) {}
  void dump(raw_ostream &OS, unsigned Indent) const override {
    OS.indent(Indent) << "VariableExpr " << Name << '\n';
  }
  Value *codegen() override;
};

class BinaryExprAST : public ExprAST {
  char Op;
  std::unique_ptr<ExprAST> LHS, RHS;

public:
  BinaryExprAST(char Op, std::unique_ptr<ExprAST> LHS,
                std::unique_ptr<ExprAST> RHS)
      : Op(Op), LHS(std::move(LHS)), RHS(std::move(RHS)) {}

  void dump(raw_ostream &OS, unsigned Indent) const override {
    OS.indent(Indent) << "BinaryExpr '" << Op << "'\n";
    LHS->dump(OS, Indent + 2);
    RHS->dump(OS, Indent + 2);
  }
  Value *codegen() override;
};

class CallExprAST : public ExprAST {
  std::string Callee;
  std::vector<std::unique_ptr<ExprAST>> Args;

public:
  CallExprAST(const std::string &Callee,
              std::vector<std::unique_ptr<ExprAST>> Args)
      : Callee(Callee), Args(std::move(Args)) {}
  void dump(raw_ostream &OS, unsigned Indent) const override {
    OS.indent(Indent) << "CallExpr " << Callee << '\n';
    for (const auto &Arg : Args)
      Arg->dump(OS, Indent + 2);
  }
  Value *codegen() override;
};

class ProtoTypeAST {
  std::string Name;
  std::vector<std::string> Args;

public:
  ProtoTypeAST(const std::string &Name, std::vector<std::string> Args)
      : Name(Name), Args(std::move(Args)) {}

  const std::string &getName() const { return Name; }
  const std::vector<std::string> &getArgs() const { return Args; }

  void dump(raw_ostream &OS, unsigned Indent) const {
    OS.indent(Indent) << "Prototype " << Name << '(';
    interleave(Args, OS, " ");
    OS << ")\n";
  }
  Function *codegen();
};

class FunctionAST {
  std::unique_ptr<ProtoTypeAST> Proto;
  std::unique_ptr<ExprAST> Body;

public:
  FunctionAST(std::unique_ptr<ProtoTypeAST> Proto,
              std::unique_ptr<ExprAST> Body)
      : Proto(std::move(Proto)), Body(std::move(Body)) {}

  void dump(raw_ostream &OS, unsigned Indent) const {
    OS.indent(Indent) << "Function\n";
    Proto->dump(OS, Indent + 2);
    Body->dump(OS, Indent + 2);
  }
  Function *codegen();
};

} // end anonymous namespace

static int CurTok;
static int getNextToken() { return CurTok = gettok(); }

static std::map<char, int> BinopPrecedence;

static int GetTokPrecedence() {
  if (!isascii(CurTok)) {
    return -1;
  }
  int TokPrec = BinopPrecedence[CurTok];
  if (TokPrec <= 0)
    return -1;
  return TokPrec;
}

std::unique_ptr<ExprAST> LogError(const char *Str) {
  fprintf(stderr, "Error: %s\n", Str);
  return nullptr;
}

std::unique_ptr<ProtoTypeAST> LogErrorP(const char *Str) {
  LogError(Str);
  return nullptr;
}

static std::unique_ptr<ExprAST> ParseExpression();

static std::unique_ptr<ExprAST> ParseNumberExpr() {
  auto Result = std::make_unique<NumberExprAST>(NumVal);
  getNextToken(); // consume the number
  return std::move(Result);
}

static std::unique_ptr<ExprAST> ParseParenExpr() {
  getNextToken(); // eat (.
  auto V = ParseExpression();
  if (!V) {
    return nullptr;
  }

  if (CurTok != ')') {
    return LogError("expected ')'");
  }
  getNextToken(); // eat ).
  return V;
}

static std::unique_ptr<ExprAST> ParseIdentifierExpr() {
  std::string IdName = IdentifierStr;

  getNextToken();

  if (CurTok != '(')
    return std::make_unique<VariableExprAST>(IdName);

  getNextToken(); // eat (
  std::vector<std::unique_ptr<ExprAST>> Args;
  if (CurTok != ')') {
    while (true) {
      if (auto Arg = ParseExpression()) {
        Args.push_back(std::move(Arg));
      } else {
        return nullptr;
      }

      if (CurTok == ')') {
        break;
      }

      if (CurTok != ',') {
        return LogError("Expected ')' or ',' in argument list");
      }
      getNextToken();
    }
  }

  getNextToken(); // eat ).
  return std::make_unique<CallExprAST>(IdName, std::move(Args));
}

static std::unique_ptr<ExprAST> ParsePrimary() {
  switch (CurTok) {
  case tok_identifier:
    return ParseIdentifierExpr();
  case tok_number:
    return ParseNumberExpr();
  case '(':
    return ParseParenExpr();
  default:
    return LogError("unknown token when expecting an expression");
  }
}

static std::unique_ptr<ExprAST> ParseBinOpRHS(int ExprPrec,
                                              std::unique_ptr<ExprAST> LHS) {
  while (true) {
    int TokPrec = GetTokPrecedence();

    if (TokPrec < ExprPrec) {
      return LHS;
    }

    int BinOp = CurTok;
    getNextToken(); // eat binop

    auto RHS = ParsePrimary();
    if (!RHS) {
      return nullptr;
    }

    int NextPrec = GetTokPrecedence();
    if (TokPrec < NextPrec) {
      RHS = ParseBinOpRHS(TokPrec + 1, std::move(RHS));
      if (!RHS) {
        return nullptr;
      }
    }

    LHS =
        std::make_unique<BinaryExprAST>(BinOp, std::move(LHS), std::move(RHS));
  }
}

static std::unique_ptr<ExprAST> ParseExpression() {
  auto LHS = ParsePrimary();
  if (!LHS) {
    return nullptr;
  }

  return ParseBinOpRHS(0, std::move(LHS));
}

static std::unique_ptr<ProtoTypeAST> ParsePrototype() {
  if (CurTok != tok_identifier) {
    return LogErrorP("Expected function name in prototype");
  }

  std::string FnName = IdentifierStr;
  getNextToken();

  if (CurTok != '(') {
    return LogErrorP("Expected '(' in prototype");
  }

  std::vector<std::string> ArgNames;
  while (getNextToken() == tok_identifier) {
    ArgNames.push_back(IdentifierStr);
  }
  if (CurTok != ')') {
    return LogErrorP("Expected ')' in prototype");
  }

  getNextToken(); // eat ')'.

  return std::make_unique<ProtoTypeAST>(FnName, std::move(ArgNames));
}

static std::unique_ptr<FunctionAST> ParseDefinition() {
  getNextToken(); // eat def.
  auto Proto = ParsePrototype();
  if (!Proto) {
    return nullptr;
  }

  if (auto E = ParseExpression()) {
    return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
  }
  return nullptr;
}

static std::unique_ptr<FunctionAST> ParseTopLevelExpr() {
  if (auto E = ParseExpression()) {
    auto Proto = std::make_unique<ProtoTypeAST>("__anon_expr",
                                                std::vector<std::string>());
    return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
  }
  return nullptr;
}

static std::unique_ptr<ProtoTypeAST> ParseExtern() {
  getNextToken(); // eat extern.
  return ParsePrototype();
}

//===----------------------------------------------------------------------===//
// Code Generation
//===----------------------------------------------------------------------===//

static std::unique_ptr<LLVMContext> TheContext;
static std::unique_ptr<Module> TheModule;
static std::unique_ptr<IRBuilder<>> Builder;
static std::map<std::string, Value *> NamedValues;

static std::unique_ptr<PassInstrumentationCallbacks> ThePIC;
static std::unique_ptr<StandardInstrumentations> TheSI;
static std::unique_ptr<LoopAnalysisManager> TheLAM;
static std::unique_ptr<FunctionAnalysisManager> TheFAM;
static std::unique_ptr<CGSCCAnalysisManager> TheCGAM;
static std::unique_ptr<ModuleAnalysisManager> TheMAM;
static std::unique_ptr<FunctionPassManager> TheFPM;

Value *LogErrorV(const char *Str) {
  LogError(Str);
  return nullptr;
}

Value *NumberExprAST::codegen() {
  return ConstantFP::get(*TheContext, APFloat(Val));
}

Value *VariableExprAST::codegen() {
  Value *V = NamedValues[Name];
  if (!V) {
    return LogErrorV("Unknown variable name");
  }
  return V;
}

Value *BinaryExprAST::codegen() {
  Value *L = LHS->codegen();
  Value *R = RHS->codegen();
  if (!L || !R) {
    return nullptr;
  }

  switch (Op) {
  case '+':
    return Builder->CreateFAdd(L, R, "addtmp");
  case '-':
    return Builder->CreateFSub(L, R, "subtmp");
  case '*':
    return Builder->CreateFMul(L, R, "multmp");
  case '<':
    L = Builder->CreateFCmpULT(L, R, "cmptmp");
    return Builder->CreateUIToFP(L, Type::getDoubleTy(*TheContext), "booltmp");
  default:
    return LogErrorV("invalid binary operator");
  }
}

Value *CallExprAST::codegen() {
  Function *CalleeF = TheModule->getFunction(Callee);
  if (!CalleeF) {
    return LogErrorV("Unknown function referenced");
  }

  if (CalleeF->arg_size() != Args.size()) {
    return LogErrorV("Incorrect # arguments passed");
  }

  std::vector<Value *> ArgsV;
  for (unsigned i = 0, e = Args.size(); i != e; ++i) {
    ArgsV.push_back(Args[i]->codegen());
    if (!ArgsV.back()) {
      return nullptr;
    }
  }

  return Builder->CreateCall(CalleeF, ArgsV, "calltmp");
}

Function *ProtoTypeAST::codegen() {
  std::vector<Type *> Doubles(Args.size(), Type::getDoubleTy(*TheContext));
  FunctionType *FT =
      FunctionType::get(Type::getDoubleTy(*TheContext), Doubles, false);

  Function *F =
      Function::Create(FT, Function::ExternalLinkage, Name, TheModule.get());

  unsigned Idx = 0;
  for (auto &Arg : F->args()) {
    Arg.setName(Args[Idx++]);
  }

  return F;
}

Function *FunctionAST::codegen() {
  Function *TheFunction = TheModule->getFunction(Proto->getName());

  if (!TheFunction) {
    TheFunction = Proto->codegen();
  }

  if (!TheFunction) {
    return nullptr;
  }

  if (!TheFunction->empty()) {
    return (Function *)LogErrorV("Function cannot be redefined.");
  }

  const auto &Params = Proto->getArgs();
  if (TheFunction->arg_size() != Params.size()) {
    return (Function *)LogErrorV("Definition conflicts with a declaration.");
  }

  // Drop the old names first so that two parameters can change names.
  for (auto &Arg : TheFunction->args()) {
    Arg.setName("");
  }
  for (auto &Arg : TheFunction->args()) {
    Arg.setName(Params[Arg.getArgNo()]);
  }

  BasicBlock *BB = BasicBlock::Create(*TheContext, "entry", TheFunction);
  Builder->SetInsertPoint(BB);

  NamedValues.clear();
  for (auto &Arg : TheFunction->args()) {
    NamedValues[std::string(Arg.getName())] = &Arg;
  }

  if (Value *RetVal = Body->codegen()) {
    Builder->CreateRet(RetVal);
    if (!verifyFunction(*TheFunction, &errs()) && !NoOpt) {
      TheFPM->run(*TheFunction, *TheFAM);
    }
    return TheFunction;
  }

  TheFunction->eraseFromParent();
  return nullptr;
}

/// Print an LLVM IR value from the debugger.
LLVM_DUMP_METHOD void dumpValue(const Value *V) {
  V->print(errs());
  errs() << '\n';
}

//===----------------------------------------------------------------------===//
// Top-Level parsing and driver
//===----------------------------------------------------------------------===//

/// Set up IR construction and the function optimization pipeline.
static void InitializeModuleAndManagers() {
  // Open a new context and module.
  TheContext = std::make_unique<LLVMContext>();
  TheModule = std::make_unique<Module>("KaleidoscopeJit", *TheContext);

  // Create a new builder for the module.
  Builder = std::make_unique<IRBuilder<>>(*TheContext);

  // Create a new pass manager and analysis managers.
  
  TheLAM = std::make_unique<LoopAnalysisManager>();
  TheFAM = std::make_unique<FunctionAnalysisManager>();
  TheCGAM = std::make_unique<CGSCCAnalysisManager>();
  TheMAM = std::make_unique<ModuleAnalysisManager>();

  ThePIC = std::make_unique<PassInstrumentationCallbacks>();
  TheSI = std::make_unique<StandardInstrumentations>(*TheContext, DebugPM);
  TheSI->registerCallbacks(*ThePIC, TheMAM.get());

  TheFPM = std::make_unique<FunctionPassManager>();
  TheFPM->addPass(InstCombinePass());
  TheFPM->addPass(ReassociatePass());
  TheFPM->addPass(GVNPass());
  TheFPM->addPass(SimplifyCFGPass());

  PassBuilder PB(/*TM=*/nullptr, PipelineTuningOptions(), std::nullopt, ThePIC.get());
  PB.registerModuleAnalyses(*TheMAM);
  PB.registerCGSCCAnalyses(*TheCGAM);
  PB.registerFunctionAnalyses(*TheFAM);
  PB.registerLoopAnalyses(*TheLAM);
  PB.crossRegisterProxies(*TheLAM, *TheFAM, *TheCGAM, *TheMAM);
}

static void HandleDefinition() {
  if (auto FnAST = ParseDefinition()) {
    if (DumpAST)
      FnAST->dump(errs(), 0);
    if (Action == ParseOnly) {
      fprintf(stderr, "Parsed a function definition.\n");
      return;
    }
    if (auto *FnIR = FnAST->codegen()) {
      if (Action == RunInteractive) {
        fprintf(stderr, "Read function definition:");
        FnIR->print(errs());
        fprintf(stderr, "\n");
      }
    }
  } else {
    getNextToken();
  }
}

static void HandleExtern() {
  if (auto ProtoAST = ParseExtern()) {
    if (DumpAST)
      ProtoAST->dump(errs(), 0);
    if (Action == ParseOnly) {
      fprintf(stderr, "Parsed an extern\n");
      return;
    }
    if (auto *FnIR = ProtoAST->codegen()) {
      if (Action == RunInteractive) {
        fprintf(stderr, "Read extern: ");
        FnIR->print(errs());
        fprintf(stderr, "\n");
      }
    }
  } else {
    getNextToken();
  }
}

static void HandleTopLevelExpression() {
  if (auto FnAST = ParseTopLevelExpr()) {
    if (DumpAST)
      FnAST->dump(errs(), 0);
    if (Action == ParseOnly) {
      fprintf(stderr, "Parsed a top-level expr\n");
      return;
    }
    if (auto *FnIR = FnAST->codegen()) {
      if (Action == RunInteractive) {
        fprintf(stderr, "Read top-level expression:");
        FnIR->print(errs());
        fprintf(stderr, "\n");
      }

      // Discard cached analyses before deleting the anonymous function.
      TheFAM->clear(*FnIR, FnIR->getName());
      FnIR->eraseFromParent();
    }
  } else {
    getNextToken();
  }
}

static void DumpTokenStream() {
  while (true) {
    int Tok = gettok();
    switch (Tok) {
    case tok_eof:
      outs() << "tok_eof" << '\n';
      return;
    case tok_def:
      outs() << "tok_def" << '\n';
      break;
    case tok_extern:
      outs() << "tok_extern" << '\n';
      break;
    case tok_identifier:
      outs() << "tok_identifier '" << IdentifierStr << "'\n";
      break;
    case tok_number:
      outs() << "tok_number " << NumVal << '\n';
      break;
    default:
      outs() << '\'' << (char)Tok << "'\n";
      break;
    }
  }
}

static bool ShowPrompt = false;

static void Prompt() {
  if (ShowPrompt) {
    fprintf(stderr, "ready> ");
  }
}

static void MainLoop() {
  while (true) {
    Prompt();
    switch (CurTok) {
    case tok_eof:
      return;
    case ';': // ignore top-level semicolons.
      getNextToken();
      break;
    case tok_def:
      HandleDefinition();
      break;
    case tok_extern:
      HandleExtern();
      break;
    default:
      HandleTopLevelExpression();
      break;
    }
  }
}

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);

  cl::HideUnrelatedOptions(ToyCategory);
  cl::ParseCommandLineOptions(argc, argv, "Kaleidoscope toy compiler\n");

  if (DumpTokens) {
    DumpTokenStream();
    return 0;
  }

  ShowPrompt = sys::Process::StandardInIsUserInput();

  BinopPrecedence['<'] = 10;
  BinopPrecedence['+'] = 20;
  BinopPrecedence['-'] = 20;
  BinopPrecedence['*'] = 40; // highest.

  Prompt();
  getNextToken();

  InitializeModuleAndManagers();

  MainLoop();
  if (Action == EmitLLVMIR)
    TheModule->print(outs(), nullptr);

  return 0;
}