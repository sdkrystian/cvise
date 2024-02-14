//===----------------------------------------------------------------------===//
//
// Copyright (c) 2016, 2017, 2018, 2019 The University of Utah
// All rights reserved.
//
// This file is distributed under the University of Illinois Open Source
// License.  See the file COPYING for details.
//
//===----------------------------------------------------------------------===//

#if HAVE_CONFIG_H
#  include <config.h>
#endif

#include "ExpressionDetector.h"

#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/ASTContext.h"
#if LLVM_VERSION_MAJOR >= 15
#include "clang/Basic/FileEntry.h"
#endif
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Preprocessor.h"

#include "CommonStatementVisitor.h"
#include "TransformationManager.h"

using namespace clang;

static const char *DescriptionMsg =
"Insert a printf statement to print out the value of an expression. \
Currently, only expressions of type integer and floating point are \
considered valid. The transformation also injects a static control \
variable to ensure that the expression of interest will be printed \
only once.\n";

// Some known issues:
// (1) Because we don't have any array-bound analysis, this pass will
//     turn the following code into one that would coredump:
//    if (argc == 2 && !strcmp(argv[1], "xxx"))
//  ==>
//    int __cvise_expr_tmp_xxx = !strcmp(argv[1], "xxx");
//    ...
// (2) we don't perform pointer analysis, the transformed program
//     will produce different result from the original one, e.g.,
//    int *x = &g;
//    foo((*x) += 1 || g);
//  ==>
//    int *x = &g;
//    int __cvise_expr_tmp_xxx = g;
//    ...
//    foo((*x) += 1 || __cvise_expr_tmp_xxx);

static RegisterTransformation<ExpressionDetector>
         Trans("expression-detector", DescriptionMsg);

namespace {
class IncludesPPCallbacks : public PPCallbacks {
public:
  IncludesPPCallbacks(SourceManager &M, const std::string &Name,
                      bool &H, SourceLocation &Loc)
    : SrcManager(M), HeaderName(Name), HasHeader(H), HeaderLoc(Loc)
  { }

  virtual void InclusionDirective(SourceLocation HashLoc,
                          const Token &IncludeTok,
                          StringRef FileName, bool IsAngled,
                          CharSourceRange FilenameRange,
#if LLVM_VERSION_MAJOR < 15
                          const FileEntry *File,
#elif LLVM_VERSION_MAJOR < 16
                          Optional<FileEntryRef> File,
#else
                          OptionalFileEntryRef File,
#endif
                          StringRef SearchPath, StringRef RelativePath,
#if LLVM_VERSION_MAJOR < 19
                          const Module *Imported,
#else
                          const Module *SuggestedModule, bool ModuleImported,
#endif
                          SrcMgr::CharacteristicKind FileType) override;

private:
  SourceManager &SrcManager;

  const std::string &HeaderName;

  bool &HasHeader;

  SourceLocation &HeaderLoc;
};

void IncludesPPCallbacks::InclusionDirective(SourceLocation HashLoc,
                                             const Token &/*IncludeTok*/,
                                             StringRef FileName,
                                             bool /*IsAngled*/,
                                             CharSourceRange /*FilenameRange*/,
#if LLVM_VERSION_MAJOR < 15
                                             const FileEntry * /*File*/,
#elif LLVM_VERSION_MAJOR < 16
                                             Optional<FileEntryRef> /*File*/,
#else
                                             OptionalFileEntryRef /*File*/,
#endif
                                             StringRef /*SearchPath*/,
                                             StringRef /*RelativePath*/,
#if LLVM_VERSION_MAJOR < 19
                                            const Module *Imported,
#else
                                            const Module *SuggestedModule, bool ModuleImported,
#endif
                                             SrcMgr::CharacteristicKind /*FileType*/)
{
  if (!SrcManager.isInMainFile(HashLoc))
    return;
  // We may have multiple "#include <stdio.h>". Only handle the first one.
  if (!HasHeader && FileName == HeaderName) {
    HasHeader = true;
    HeaderLoc = HashLoc;
  }
}

// Collecting all tmp vars __cvise_expr_tmp_xxx within a statement
// generated by C-Vise
class LocalTmpVarCollector : public RecursiveASTVisitor<LocalTmpVarCollector> {
public:
  LocalTmpVarCollector(SmallVector<const VarDecl *, 4> &V, const std::string &P)
    : TmpVars(V), Prefix(P)
  { }

  bool VisitDeclRefExpr(DeclRefExpr *DRE);

private:
  SmallVector<const VarDecl *, 4> &TmpVars;

  const std::string &Prefix;
};

bool LocalTmpVarCollector::VisitDeclRefExpr(DeclRefExpr *DRE)
{
  const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl());
  if (!VD)
    return true;
  if (VD->getName().startswith(Prefix))
    TmpVars.push_back(VD);
  return true;
}

// For a given statement, collect all expressions, each of which is
// (1) either the sub-expression of an inc, dec or addrof operator; or
// (2) the LHS of a binary operator;
class LocalUOBOVisitor : public RecursiveASTVisitor<LocalUOBOVisitor> {
public:
  explicit LocalUOBOVisitor(llvm::SmallPtrSet<const Expr *, 10> &ES)
    : InvalidExprs(ES)
  { }

  bool VisitUnaryOperator(UnaryOperator *UO);

  bool VisitBinaryOperator(BinaryOperator *BO);

private:
  llvm::SmallPtrSet<const Expr *, 10> &InvalidExprs;
};

bool LocalUOBOVisitor::VisitUnaryOperator(UnaryOperator *UO)
{
  if (!UO->isIncrementDecrementOp() && UO->getOpcode() != UO_AddrOf)
    return true;
  
  const Expr *E = UO->getSubExpr();
  InvalidExprs.insert(E->IgnoreParenImpCasts());
  return true;
}

bool LocalUOBOVisitor::VisitBinaryOperator(BinaryOperator *BO)
{
  if (!BO->isAssignmentOp())
    return true;
  const Expr *E = BO->getLHS();
  InvalidExprs.insert(E->IgnoreParenImpCasts());
  return true;
}

} // End anonymous namespace

class ExprDetectorTempVarVisitor :
        public RecursiveASTVisitor<ExprDetectorTempVarVisitor> {
public:

  explicit ExprDetectorTempVarVisitor(ExpressionDetector *Instance)
    : ConsumerInstance(Instance)
  { }

  bool VisitDeclStmt(DeclStmt *DS);

private:

  ExpressionDetector *ConsumerInstance;
};

bool ExprDetectorTempVarVisitor::VisitDeclStmt(DeclStmt *DS)
{
  for (auto I : DS->decls()) {
    ConsumerInstance->addOneTempVar(dyn_cast<VarDecl>(I));
  }
  return true;
}

class ExprDetectorCollectionVisitor :
        public RecursiveASTVisitor<ExprDetectorCollectionVisitor> {
public:

  explicit ExprDetectorCollectionVisitor(ExpressionDetector *Instance)
    : ConsumerInstance(Instance)
  { }

  bool VisitFunctionDecl(FunctionDecl *FD);

private:

  ExpressionDetector *ConsumerInstance;
};

class ExprDetectorStmtVisitor :
        public CommonStatementVisitor<ExprDetectorStmtVisitor> {
public:

  explicit ExprDetectorStmtVisitor(ExpressionDetector *Instance)
    : ConsumerInstance(Instance)
  { }

  bool VisitExpr(Expr *E);

private:
  ExpressionDetector *ConsumerInstance;
};

bool ExprDetectorCollectionVisitor::VisitFunctionDecl(FunctionDecl *FD)
{
  if (!ConsumerInstance->HFInfo.HasFunction &&
      FD->getNameAsString() == ConsumerInstance->HFInfo.FunctionName) {
    ConsumerInstance->HFInfo.HasFunction = true;
    ConsumerInstance->HFInfo.FunctionLoc = FD->getSourceRange().getBegin();
  }

  if (ConsumerInstance->isInIncludedFile(FD) ||
      !FD->isThisDeclarationADefinition())
    return true;

  ExprDetectorTempVarVisitor VarVisitor(ConsumerInstance);
  VarVisitor.TraverseDecl(FD);

  ExprDetectorStmtVisitor StmtVisitor(ConsumerInstance);
  StmtVisitor.setCurrentFunctionDecl(FD);
  StmtVisitor.TraverseDecl(FD);
  StmtVisitor.setCurrentFunctionDecl(NULL);
  ConsumerInstance->UniqueExprs.clear();
  ConsumerInstance->ProcessedExprs.clear();
  return true;
}

bool ExprDetectorStmtVisitor::VisitExpr(Expr *E)
{
  if (ConsumerInstance->isInIncludedFile(E))
    return true;

  switch(E->getStmtClass()) {
  default: return true;

  case Expr::ArraySubscriptExprClass:
  case Expr::BinaryOperatorClass:
  case Expr::CallExprClass:
  case Expr::DeclRefExprClass:
  case Expr::MemberExprClass:
  case Expr::UnaryOperatorClass: // Fall-through
    break;
  }

  const Type *Ty = E->getType().getTypePtr();
  // Currently, integer and floating point only.
  if (!Ty->isIntegerType() && !Ty->isFloatingType())
    return true;

  if (!ConsumerInstance->isValidExpr(CurrentStmt, E))
    return true;

  ConsumerInstance->ValidInstanceNum++;
  if (ConsumerInstance->ValidInstanceNum ==
      ConsumerInstance->TransformationCounter) {
    ConsumerInstance->TheFunc = CurrentFuncDecl;
    ConsumerInstance->TheStmt = CurrentStmt;
    ConsumerInstance->TheExpr = E;
  }
  return true;
}

void ExpressionDetector::Initialize(ASTContext &context) 
{
  Transformation::Initialize(context);
  CollectionVisitor = new ExprDetectorCollectionVisitor(this);
  if (CheckReference) {
    ControlVarNamePrefix = CheckedVarNamePrefix;
    HFInfo.HeaderName = "stdlib.h";
    HFInfo.FunctionName = "abort";
    HFInfo.FunctionDeclStr = "void abort(void)";
  }
  else {
    ControlVarNamePrefix = PrintedVarNamePrefix;
    HFInfo.HeaderName = "stdio.h";
    HFInfo.FunctionName = "printf";
    HFInfo.FunctionDeclStr = "int printf(const char *format, ...)";
  }

  ControlVarNameQueryWrap = 
    new TransNameQueryWrap(ControlVarNamePrefix);
  TmpVarNameQueryWrap =
    new TransNameQueryWrap(TmpVarNamePrefix);
  Preprocessor &PP = TransformationManager::getPreprocessor();
  IncludesPPCallbacks *C =
    new IncludesPPCallbacks(PP.getSourceManager(),
                            HFInfo.HeaderName,
                            HFInfo.HasHeader,
                            HFInfo.HeaderLoc);
  PP.addPPCallbacks(std::unique_ptr<PPCallbacks>(C));
}

bool ExpressionDetector::HandleTopLevelDecl(DeclGroupRef D) 
{
  // Skip C++ programs for now.
  if (TransformationManager::isCXXLangOpt()) {
    ValidInstanceNum = 0;
    return true;
  }

  for (DeclGroupRef::iterator I = D.begin(), E = D.end(); I != E; ++I) {
    CollectionVisitor->TraverseDecl(*I);
  }
  return true;
}
 
void ExpressionDetector::HandleTranslationUnit(ASTContext &Ctx)
{
  if (QueryInstanceOnly)
    return;

  if (TransformationCounter > ValidInstanceNum) {
    TransError = TransMaxInstanceError;
    return;
  }

  Ctx.getDiagnostics().setSuppressAllDiagnostics(false);

  TransAssert(TheFunc && "NULL TheFunc!");
  TransAssert(TheStmt && "NULL TheStmt!");
  TransAssert(TheExpr && "NULL TheExpr");

  if (DoReplacement) {
    RewriteHelper->replaceExpr(TheExpr, Replacement);
  }
  else {
    ControlVarNameQueryWrap->TraverseDecl(TheFunc);
    TmpVarNameQueryWrap->TraverseDecl(TheFunc);
    doRewrite();
  }

  if (Ctx.getDiagnostics().hasErrorOccurred() ||
      Ctx.getDiagnostics().hasFatalErrorOccurred())
    TransError = TransInternalError;
}

// Return true if
// (1) we don't have the function declaration or the decl appears after
//     the recorded FunctionLoc; and
// (2) header file is not included in the main file or header file
//     appears after the Loc.
bool ExpressionDetector::shouldAddFunctionDecl(SourceLocation Loc)
{
  return (!HFInfo.HasFunction ||
          SrcManager->isBeforeInSLocAddrSpace(Loc, HFInfo.FunctionLoc)) &&
         (!HFInfo.HasHeader ||
          SrcManager->isBeforeInSLocAddrSpace(Loc, HFInfo.HeaderLoc));
}

void ExpressionDetector::addOneTempVar(const VarDecl *VD)
{
  if (!VD)
    return;
  if (!VD->getName().startswith(TmpVarNamePrefix))
    return;
  if (const Expr *E = VD->getInit())
    ProcessedExprs[VD] = E->IgnoreParenImpCasts();
}

bool ExpressionDetector::refToTmpVar(const NamedDecl *ND)
{
  StringRef Name = ND->getName();
  // We don't want to repeatly replace temporary variables
  // __cvise_expr_tmp_xxx, __cvise_printed_yy and __cvise_checked_zzz.
  return Name.startswith(TmpVarNamePrefix) ||
         Name.startswith(PrintedVarNamePrefix) ||
         Name.startswith(CheckedVarNamePrefix);
}

// Reference: IdenticalExprChecker.cpp from Clang
bool ExpressionDetector::isIdenticalExpr(const Expr *E1, const Expr *E2)
{
  if (!E1 || !E2)
    return !E1 && !E2;

  E1 = E1->IgnoreParenImpCasts();
  E2 = E2->IgnoreParenImpCasts();
  Stmt::StmtClass SC1 = E1->getStmtClass();
  Stmt::StmtClass SC2 = E2->getStmtClass();
  if (SC1 != SC2)
    return false;

  // If either of E1 or E2 has side-effects, treat them as non-identical
  // expressions.
  if (E1->HasSideEffects(*Context) || E2->HasSideEffects(*Context))
    return false;
  
  Expr::const_child_iterator I1 = E1->child_begin(), I2 = E2->child_begin();
  while (I1 != E1->child_end() && I2 != E2->child_end()) {
    if (!isIdenticalExpr(dyn_cast<Expr>(*I1), dyn_cast<Expr>(*I2)))
      return false;
    ++I1;
    ++I2;
  }
  if (I1 != E1->child_end() || I2 != E2->child_end())
    return false;

  switch (SC1) {
  default:
    return false;
  case Stmt::ArraySubscriptExprClass:
  case Stmt::CallExprClass: // Fall-through
    return true;

  case Stmt::CStyleCastExprClass: {
    const CStyleCastExpr *Cast1 = cast<CStyleCastExpr>(E1);
    const CStyleCastExpr *Cast2 = cast<CStyleCastExpr>(E2);
    return Cast1->getTypeAsWritten() == Cast2->getTypeAsWritten();
  }

  case Stmt::MemberExprClass: {
    const MemberExpr *ME1 = cast<MemberExpr>(E1);
    const MemberExpr *ME2 = cast<MemberExpr>(E2);
    return ME1->getMemberDecl() == ME2->getMemberDecl();
  }

  case Stmt::DeclRefExprClass: {
    const DeclRefExpr *DRE1 = cast<DeclRefExpr>(E1);
    const DeclRefExpr *DRE2 = cast<DeclRefExpr>(E2);
    return DRE1->getDecl() == DRE2->getDecl();
  }

  case Stmt::CompoundAssignOperatorClass:
  case Stmt::BinaryOperatorClass: // Fall-through
  {
    const BinaryOperator *BO1 = cast<BinaryOperator>(E1);
    const BinaryOperator *BO2 = cast<BinaryOperator>(E2);
    return BO1->getOpcode() == BO2->getOpcode();
  }

  case Stmt::UnaryOperatorClass:
  {
    const UnaryOperator *UO1 = cast<UnaryOperator>(E1);
    const UnaryOperator *UO2 = cast<UnaryOperator>(E2);
    return UO1->getOpcode() == UO2->getOpcode();
  }

  case Stmt::CharacterLiteralClass: {
    const CharacterLiteral *Lit1 = cast<CharacterLiteral>(E1);
    const CharacterLiteral *Lit2 = cast<CharacterLiteral>(E2);
    return Lit1->getValue() == Lit2->getValue();
  }

  case Stmt::StringLiteralClass: {
    const clang::StringLiteral *Lit1 = cast<clang::StringLiteral>(E1);
    const clang::StringLiteral *Lit2 = cast<clang::StringLiteral>(E2);
    return Lit1->getBytes() == Lit2->getBytes();
  }

  case Stmt::IntegerLiteralClass: {
    const IntegerLiteral *Lit1 = cast<IntegerLiteral>(E1);
    const IntegerLiteral *Lit2 = cast<IntegerLiteral>(E2);
    llvm::APInt I1 = Lit1->getValue();
    llvm::APInt I2 = Lit2->getValue();
    return I1.getBitWidth() == I2.getBitWidth() && I1 == I2;
  }

  case Stmt::FloatingLiteralClass: {
    const FloatingLiteral *Lit1 = cast<FloatingLiteral>(E1);
    const FloatingLiteral *Lit2 = cast<FloatingLiteral>(E2);
    return Lit1->getValue().bitwiseIsEqual(Lit2->getValue());
  }

  }

  return true;
}

bool ExpressionDetector::hasIdenticalExpr(
       const SmallVector<const VarDecl *, 4> &TmpVars, const Expr *E)
{
  for (auto V : TmpVars) {
    auto I = ProcessedExprs.find(V);
    if (I != ProcessedExprs.end() && isIdenticalExpr(I->second, E))
      return true;
  }
  return false;
}

bool ExpressionDetector::isValidExpr(Stmt *S, const Expr *E)
{
  Stmt::StmtClass SC = S->getStmtClass();
  // Don't mess up with init/condition/inc exprs of loops.
  if (SC == Stmt::ForStmtClass ||
      SC == Stmt::DoStmtClass ||
      SC == Stmt::WhileStmtClass) {
    return false;
  }

  if (const Expr *SE = dyn_cast<Expr>(S)) {
    // Don't do self-replacement. Note that E can't be of paren
    // or cast expr (See ExprDetectorStmtVisitor::VisitExpr).
    if (SE->IgnoreParenCasts() == E)
      return false;
  }

  if (const DeclStmt *DS = dyn_cast<DeclStmt>(S)) {
    if (DS->isSingleDecl()) {
      const NamedDecl *ND = dyn_cast<NamedDecl>(DS->getSingleDecl());
      if (ND == NULL || refToTmpVar(ND))
        return false;
    }
    else {
      // Skip group decls for now.
      return false;
    }
  }

  // don't handle !__cvise_printed and !__cvise_checked
  if (const UnaryOperator *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->getOpcode() == UO_LNot) {
      if (const DeclRefExpr *SubE =
          dyn_cast<DeclRefExpr>(UO->getSubExpr()->IgnoreParenCasts())) {
        StringRef SubEName = SubE->getDecl()->getName();
        if (SubEName.startswith(PrintedVarNamePrefix) ||
            SubEName.startswith(CheckedVarNamePrefix))
          return false;
      }
    }
  }

  // skip if (__cvise_expr_tmp != xxx)
  if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(E)) {
    if (BO->getOpcode() == BO_NE) {
      const Expr *Lhs = BO->getLHS()->IgnoreParenCasts();
      const Expr *Rhs = BO->getRHS()->IgnoreParenCasts();
      const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(Lhs);
      Stmt::StmtClass SC = Rhs->getStmtClass();
      bool IsLit = SC == Stmt::IntegerLiteralClass ||
                   SC == Stmt::FloatingLiteralClass;
      if (IsLit && DRE &&
          DRE->getDecl()->getName().startswith(TmpVarNamePrefix) &&
          S->getStmtClass() == Stmt::IfStmtClass) {
        return false;
      }
    }
  }

  if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E)) {
    // Don't repeatly process temp vars.
    if (refToTmpVar(DRE->getDecl()))
      return false;
    // Skip cases such as printf("%d", a);
    if (const CallExpr *CE = dyn_cast<CallExpr>(S)) {
      const FunctionDecl *FD = CE->getDirectCallee();
      if (FD && FD->getNameAsString() == "printf")
        return false;
    }
  }

  // If the Expr is the LHS of a binary operator, we don't
  // need to process it, e.g., given
  //   a = x + y;
  // we don't need to replace "a" with "__cvise_expr_tmp_xxx"
  // 
  // We also skip x++, --x and &x, because we don't want to make
  // the following transformation:
  //   x++;
  // ==>
  //   __cvise_expr_tmp_xxx = x;
  //   ++__cvise_expr_tmp_xxx;
  // In the original code, we pre-increment x, but after the "transformation",
  // we would end up doing that for __cvise_expr_tmp_xxx.
  // Note that we cache the result, because we don't want to re-visit the same
  // statement many times. Gain a log of performance improvement.
  auto EI = InvalidExprsInUOBO.find(S);
  if (EI == InvalidExprsInUOBO.end()) {
    llvm::SmallPtrSet<const Expr *, 10> InvalidExprs;
    LocalUOBOVisitor UOBOVisitor(InvalidExprs);
    UOBOVisitor.TraverseStmt(S);
    InvalidExprsInUOBO[S] = InvalidExprs;
    if (InvalidExprs.count(E))
      return false;
  }
  else if (EI->second.count(E)) {
    return false;
  }

  // Skip identical expression. For example, for
  // the given statement below:
  //   x = y[1] + y[1] + y[1];
  // we only need to print one y[1] before the assignment statement.
  // This optimization is able to avoid a lots of dups.
  auto &Vec = UniqueExprs[S];
  for (auto I : Vec) {
    if (isIdenticalExpr(I, E))
      return false;
  }
  // update UniqueExprs
  Vec.push_back(E);

  // The optimization above only works for single iteration. The code
  // below handles the following patterns:
  //   int __cvise_expr_tmp_1 = y[1];
  //   ...printf("%d\n", __cvise_expr_tmp_1);
  //   x = __cvise_expr_tmp_1 + y[1] + y[1]
  // We don't need to generate another tmp var for y[1] in this case.
  auto VI = TmpVarsInStmt.find(S);
  if (VI == TmpVarsInStmt.end()) {
    SmallVector<const VarDecl *, 4> TmpVars;
    LocalTmpVarCollector TmpCollector(TmpVars, TmpVarNamePrefix);
    TmpCollector.TraverseStmt(S);
    TmpVarsInStmt[S] = TmpVars;
    if (hasIdenticalExpr(TmpVars, E))
      return false;
  }
  else if (hasIdenticalExpr(VI->second, E)) {
    return false;
  }

  return true;
}

static std::string getFormatString(const BuiltinType *BT)
{
  switch(BT->getKind()) {
  default:
    TransAssert(0 && "Bad BuiltinType!");
    return "";

  case BuiltinType::Bool:
  case BuiltinType::Char_U:
  case BuiltinType::WChar_U:
  case BuiltinType::UChar:
  case BuiltinType::UShort:
  case BuiltinType::UInt:
    return "u";

  case BuiltinType::Char_S:
  case BuiltinType::SChar:
  case BuiltinType::WChar_S:
  case BuiltinType::Short:
  case BuiltinType::Int:
  case BuiltinType::Char16:
  case BuiltinType::Char32:
    return "d";

  case BuiltinType::ULong:
    return "lu";

  case BuiltinType::Long:
    return "ld";

  case BuiltinType::ULongLong:
    return "llu";

  case BuiltinType::LongLong:
    return "lld";

  case BuiltinType::Float:
  case BuiltinType::Double:
    return "f";

  case BuiltinType::LongDouble:
    return "Lf";
  }

  return "";
}

void ExpressionDetector::doRewrite()
{
  SourceLocation LocStart = TheStmt->getBeginLoc();
  if (shouldAddFunctionDecl(LocStart)) {
    SourceLocation Loc =
      SrcManager->getLocForStartOfFile(SrcManager->getMainFileID());
    TheRewriter.InsertText(Loc, HFInfo.FunctionDeclStr+";\n");
  }

  std::string Str, ExprStr, TmpVarName;
  RewriteHelper->getExprString(TheExpr, ExprStr);

  std::string TyStr;
  TheExpr->getType().getAsStringInternal(TyStr, getPrintingPolicy());
  TmpVarName = TmpVarNamePrefix +
               std::to_string(TmpVarNameQueryWrap->getMaxNamePostfix()+1);
  Str += TyStr + " " + TmpVarName + " = " + ExprStr + ";\n";

  std::string ControlVarName = ControlVarNamePrefix +
    std::to_string(ControlVarNameQueryWrap->getMaxNamePostfix()+1);
  Str += "static int " + ControlVarName + " = 0;\n";
  Str += "if (" + ControlVarName + " == __CVISE_INSTANCE_NUMBER) {\n";
  if (CheckReference) {
    Str += "  if (" + TmpVarName + " != " + ReferenceValue + ") ";
    Str +=  HFInfo.FunctionName + "();\n";
  }
  else {
    const Type *Ty =
      TheExpr->getType().getTypePtr()->getUnqualifiedDesugaredType();
    std::string FormatStr = getFormatString(dyn_cast<BuiltinType>(Ty));
    Str += "  " + HFInfo.FunctionName;
    Str += "(\"cvise_value(%" + FormatStr + ")\\n\", ";
    Str += TmpVarName + ");\n";
  }
  Str += "}\n";
  Str += "++" + ControlVarName + ";";

  bool NeedParen = TheStmt->getStmtClass() != Stmt::DeclStmtClass;
  RewriteHelper->addStringBeforeStmtAndReplaceExpr(TheStmt, Str,
                                                   TheExpr, TmpVarName,
                                                   NeedParen);
}

ExpressionDetector::~ExpressionDetector(void)
{
  delete CollectionVisitor;
  delete ControlVarNameQueryWrap;
  delete TmpVarNameQueryWrap;
}
