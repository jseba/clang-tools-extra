//===--- SuspiciousStringCompareCheck.cpp - clang-tidy---------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "SuspiciousStringCompareCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Lex/Lexer.h"
#include "../utils/Matchers.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace misc {

static const char KnownStringCompareFunctions[] = "__builtin_memcmp;"
                                                  "__builtin_strcasecmp;"
                                                  "__builtin_strcmp;"
                                                  "__builtin_strncasecmp;"
                                                  "__builtin_strncmp;"
                                                  "_mbscmp;"
                                                  "_mbscmp_l;"
                                                  "_mbsicmp;"
                                                  "_mbsicmp_l;"
                                                  "_mbsnbcmp;"
                                                  "_mbsnbcmp_l;"
                                                  "_mbsnbicmp;"
                                                  "_mbsnbicmp_l;"
                                                  "_mbsncmp;"
                                                  "_mbsncmp_l;"
                                                  "_mbsnicmp;"
                                                  "_mbsnicmp_l;"
                                                  "_memicmp;"
                                                  "_memicmp_l;"
                                                  "_stricmp;"
                                                  "_stricmp_l;"
                                                  "_strnicmp;"
                                                  "_strnicmp_l;"
                                                  "_wcsicmp;"
                                                  "_wcsicmp_l;"
                                                  "_wcsnicmp;"
                                                  "_wcsnicmp_l;"
                                                  "lstrcmp;"
                                                  "lstrcmpi;"
                                                  "memcmp;"
                                                  "memicmp;"
                                                  "strcasecmp;"
                                                  "strcmp;"
                                                  "strcmpi;"
                                                  "stricmp;"
                                                  "strncasecmp;"
                                                  "strncmp;"
                                                  "strnicmp;"
                                                  "wcscasecmp;"
                                                  "wcscmp;"
                                                  "wcsicmp;"
                                                  "wcsncmp;"
                                                  "wcsnicmp;"
                                                  "wmemcmp;";

static const char StringCompareLikeFunctionsDelimiter[] = ";";

static void ParseFunctionNames(StringRef Option,
                               std::vector<std::string> *Result) {
  SmallVector<StringRef, 4> Functions;
  Option.split(Functions, StringCompareLikeFunctionsDelimiter);
  for (StringRef &Function : Functions) {
    Function = Function.trim();
    if (!Function.empty())
      Result->push_back(Function);
  }
}

SuspiciousStringCompareCheck::SuspiciousStringCompareCheck(
    StringRef Name, ClangTidyContext *Context)
    : ClangTidyCheck(Name, Context),
      WarnOnImplicitComparison(Options.get("WarnOnImplicitComparison", 1)),
      WarnOnLogicalNotComparison(Options.get("WarnOnLogicalNotComparison", 0)),
      StringCompareLikeFunctions(
          Options.get("StringCompareLikeFunctions", "")) {}

void SuspiciousStringCompareCheck::storeOptions(
    ClangTidyOptions::OptionMap &Opts) {
  Options.store(Opts, "WarnOnImplicitComparison", WarnOnImplicitComparison);
  Options.store(Opts, "WarnOnLogicalNotComparison", WarnOnLogicalNotComparison);
  Options.store(Opts, "StringCompareLikeFunctions", StringCompareLikeFunctions);
}

void SuspiciousStringCompareCheck::registerMatchers(MatchFinder *Finder) {
  // Match relational operators.
  const auto ComparisonUnaryOperator = unaryOperator(hasOperatorName("!"));
  const auto ComparisonBinaryOperator =
      binaryOperator(matchers::isComparisonOperator());
  const auto ComparisonOperator =
      expr(anyOf(ComparisonUnaryOperator, ComparisonBinaryOperator));

  // Add the list of known string compare-like functions and add user-defined
  // functions.
  std::vector<std::string> FunctionNames;
  ParseFunctionNames(KnownStringCompareFunctions, &FunctionNames);
  ParseFunctionNames(StringCompareLikeFunctions, &FunctionNames);

  // Match a call to a string compare functions.
  const auto FunctionCompareDecl =
      functionDecl(hasAnyName(std::vector<StringRef>(FunctionNames.begin(),
                                                     FunctionNames.end())))
          .bind("decl");
  const auto DirectStringCompareCallExpr =
      callExpr(hasDeclaration(FunctionCompareDecl)).bind("call");
  const auto MacroStringCompareCallExpr =
      conditionalOperator(
        anyOf(hasTrueExpression(ignoringParenImpCasts(DirectStringCompareCallExpr)),
              hasFalseExpression(ignoringParenImpCasts(DirectStringCompareCallExpr))));
  // The implicit cast is not present in C.
  const auto StringCompareCallExpr = ignoringParenImpCasts(
        anyOf(DirectStringCompareCallExpr, MacroStringCompareCallExpr));

  if (WarnOnImplicitComparison) {
    // Detect suspicious calls to string compare:
    //     'if (strcmp())'  ->  'if (strcmp() != 0)'
    Finder->addMatcher(
        stmt(anyOf(ifStmt(hasCondition(StringCompareCallExpr)),
                   whileStmt(hasCondition(StringCompareCallExpr)),
                   doStmt(hasCondition(StringCompareCallExpr)),
                   forStmt(hasCondition(StringCompareCallExpr)),
                   binaryOperator(
                       anyOf(hasOperatorName("&&"), hasOperatorName("||")),
                       hasEitherOperand(StringCompareCallExpr))))
            .bind("missing-comparison"),
        this);
  }

  if (WarnOnLogicalNotComparison) {
    // Detect suspicious calls to string compared with '!' operator:
    //     'if (!strcmp())'  ->  'if (strcmp() == 0)'
    Finder->addMatcher(unaryOperator(hasOperatorName("!"),
                                     hasUnaryOperand(ignoringParenImpCasts(
                                         StringCompareCallExpr)))
                           .bind("logical-not-comparison"),
                       this);
  }

  // Detect suspicious cast to an inconsistant type (i.e. not integer type).
  Finder->addMatcher(
      implicitCastExpr(unless(hasType(isInteger())),
                       hasSourceExpression(StringCompareCallExpr))
          .bind("invalid-conversion"),
      this);

  // Detect suspicious operator with string compare function as operand.
  Finder->addMatcher(
      binaryOperator(
          unless(anyOf(matchers::isComparisonOperator(), hasOperatorName("&&"),
                       hasOperatorName("||"), hasOperatorName("="))),
          hasEitherOperand(StringCompareCallExpr))
          .bind("suspicious-operator"),
      this);

  // Detect comparison to invalid constant: 'strcmp() == -1'.
  const auto InvalidLiteral = ignoringParenImpCasts(
      anyOf(integerLiteral(unless(equals(0))),
            unaryOperator(hasOperatorName("-"),
                          has(integerLiteral(unless(equals(0))))),
            characterLiteral(), cxxBoolLiteral()));

  Finder->addMatcher(binaryOperator(matchers::isComparisonOperator(),
                                    hasEitherOperand(StringCompareCallExpr),
                                    hasEitherOperand(InvalidLiteral))
                         .bind("invalid-comparison"),
                     this);
}

void SuspiciousStringCompareCheck::check(
    const MatchFinder::MatchResult &Result) {
  const auto *Decl = Result.Nodes.getNodeAs<FunctionDecl>("decl");
  const auto *Call = Result.Nodes.getNodeAs<CallExpr>("call");
  assert(Decl != nullptr && Call != nullptr);

  if (Result.Nodes.getNodeAs<Stmt>("missing-comparison")) {
    SourceLocation EndLoc = Lexer::getLocForEndOfToken(
        Call->getRParenLoc(), 0, Result.Context->getSourceManager(),
        Result.Context->getLangOpts());

    diag(Call->getLocStart(),
         "function %0 is called without explicitly comparing result")
        << Decl << FixItHint::CreateInsertion(EndLoc, " != 0");
  }

  if (const auto *E = Result.Nodes.getNodeAs<Expr>("logical-not-comparison")) {
    SourceLocation EndLoc = Lexer::getLocForEndOfToken(
        Call->getRParenLoc(), 0, Result.Context->getSourceManager(),
        Result.Context->getLangOpts());
    SourceLocation NotLoc = E->getLocStart();

    diag(Call->getLocStart(),
         "function %0 is compared using logical not operator")
        << Decl << FixItHint::CreateRemoval(
                       CharSourceRange::getTokenRange(NotLoc, NotLoc))
        << FixItHint::CreateInsertion(EndLoc, " == 0");
  }

  if (Result.Nodes.getNodeAs<Stmt>("invalid-comparison")) {
    diag(Call->getLocStart(),
         "function %0 is compared to a suspicious constant")
        << Decl;
  }

  if (const auto* BinOp = Result.Nodes.getNodeAs<BinaryOperator>("suspicious-operator")) {
    diag(Call->getLocStart(), "results of function %0 used by operator '%1'")
        << Decl << BinOp->getOpcodeStr();
  }

  if (Result.Nodes.getNodeAs<Stmt>("invalid-conversion")) {
    diag(Call->getLocStart(), "function %0 has suspicious implicit cast")
        << Decl;
  }
}

} // namespace misc
} // namespace tidy
} // namespace clang
