#pragma once
#include "ast.h"
#include "symtable.h"
#include <string>
#include <vector>

struct SemanticError {
    int line;
    std::string msg;
};

class SemanticAnalyzer {
public:
    SymbolTable symtab;
    std::vector<SemanticError> errors;

    bool analyze(ASTNode* root);
    void dumpSymbols();

private:
    int loopDepth_ = 0;
    int funcDepth_ = 0;

    void error(int line, const std::string& msg);
    void visitNode(ASTNode* n);
    void visitChildren(ASTNode* n);
    void visitBlock(ASTNode* n);
    void visitFuncDef(ASTNode* n);
    void visitLambdaDef(ASTNode* n);
    void visitAssign(ASTNode* n);
    void visitLocalDecl(ASTNode* n);
    void visitIdExpr(ASTNode* n);
    void visitCall(ASTNode* n);
};
