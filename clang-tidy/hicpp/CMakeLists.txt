set(LLVM_LINK_COMPONENTS support)

add_clang_library(clangTidyHICPPModule
  ExceptionBaseclassCheck.cpp
  NoAssemblerCheck.cpp
  HICPPTidyModule.cpp

  LINK_LIBS
  clangAST
  clangASTMatchers
  clangBasic
  clangLex
  clangTidy
  clangTidyCppCoreGuidelinesModule
  clangTidyGoogleModule
  clangTidyMiscModule
  clangTidyModernizeModule
  clangTidyReadabilityModule
  clangTidyUtils
  )
