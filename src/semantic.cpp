#include "semantic.h"
#include <cstdio>

bool SemanticAnalyzer::analyze(ASTNode* root) {
    visitNode(root);
    return errors.empty();
}

void SemanticAnalyzer::dumpSymbols() { symtab.dump(); }

void SemanticAnalyzer::error(int line, const std::string& msg) {
    errors.push_back({line, msg});
    fprintf(stderr, "Semantic error at line %d: %s\n", line, msg.c_str());
}

void SemanticAnalyzer::visitNode(ASTNode* n) {
    if (!n) return;
    switch (n->kind) {
        case NodeKind::Program:
        case NodeKind::Block:      visitBlock(n);    break;
        case NodeKind::FuncDef:    visitFuncDef(n);  break;
        case NodeKind::LambdaDef:  visitLambdaDef(n); break;
        case NodeKind::IfStmt:     visitChildren(n); break;
        case NodeKind::WhileStmt:
            loopDepth_++;
            visitChildren(n);
            loopDepth_--;
            break;
        case NodeKind::ForStmt:
            loopDepth_++;
            visitChildren(n);
            loopDepth_--;
            break;
        case NodeKind::BreakStmt:
            if (loopDepth_ == 0)
                error(n->line, "'break' used outside of loop");
            break;
        case NodeKind::ContinueStmt:
            if (loopDepth_ == 0)
                error(n->line, "'continue' used outside of loop");
            break;
        case NodeKind::ReturnStmt:
            if (funcDepth_ == 0)
                error(n->line, "'return' used outside of function");
            visitChildren(n);
            break;
        case NodeKind::AssignExpr: visitAssign(n);   break;
        case NodeKind::IdExpr:     visitIdExpr(n);   break;
        case NodeKind::LocalDecl:  visitLocalDecl(n); break;
        case NodeKind::CallExpr:   visitCall(n);     break;
        default:                   visitChildren(n); break;
    }
}

void SemanticAnalyzer::visitChildren(ASTNode* n) {
    for (auto& child : n->children)
        visitNode(child.get());
}

void SemanticAnalyzer::visitBlock(ASTNode* n) {
    bool isTopLevel = (n->kind == NodeKind::Program);
    if (!isTopLevel) symtab.enterScope(false);
    for (auto& child : n->children)
        visitNode(child.get());
    if (!isTopLevel) symtab.exitScope();
}

void SemanticAnalyzer::visitFuncDef(ASTNode* n) {
    const std::string& fname = n->sval;
    auto existLib = symtab.lookupGlobal(fname);
    if (existLib && existLib->kind == SymbolKind::LibraryFunction) {
        error(n->line, "Cannot shadow library function '" + fname + "'");
        return;
    }
    if (!fname.empty()) {
        auto existing = symtab.lookupCurrent(fname);
        if (existing && existing->kind != SymbolKind::UserFunction)
            error(n->line, "Symbol '" + fname + "' already declared in this scope");
        else
            symtab.insertFunction(fname, n->line);
    }
    symtab.enterScope(true);
    symtab.resetLocalOffset();
    funcDepth_++;

    ASTNode* formals = n->children[0].get();
    for (auto& fp : formals->children) {
        auto existG = symtab.lookupGlobal(fp->sval);
        if (existG && existG->kind == SymbolKind::LibraryFunction)
            error(fp->line, "Formal param '" + fp->sval + "' shadows library function");
        else
            symtab.insertFormal(fp->sval, fp->line);
    }
    ASTNode* body = n->children[1].get();
    for (auto& child : body->children)
        visitNode(child.get());

    funcDepth_--;
    symtab.exitScope();
}

void SemanticAnalyzer::visitLambdaDef(ASTNode* n) {
    symtab.enterScope(true);
    symtab.resetLocalOffset();
    funcDepth_++;
    ASTNode* formals = n->children[0].get();
    for (auto& fp : formals->children)
        symtab.insertFormal(fp->sval, fp->line);
    ASTNode* body = n->children[1].get();
    for (auto& child : body->children)
        visitNode(child.get());
    funcDepth_--;
    symtab.exitScope();
}

void SemanticAnalyzer::visitAssign(ASTNode* n) {
    ASTNode* lhs = n->children[0].get();
    ASTNode* rhs = n->children[1].get();
    if (lhs->kind == NodeKind::IdExpr && !lhs->isLocal && !lhs->isGlobal) {
        auto sym = symtab.lookup(lhs->sval);
        if (!sym)
            symtab.insertLocal(lhs->sval, lhs->line);
        else if (sym->kind == SymbolKind::LibraryFunction ||
                 sym->kind == SymbolKind::UserFunction)
            error(lhs->line, "Cannot assign to function '" + lhs->sval + "'");
    }
    visitNode(lhs);
    visitNode(rhs);
}

void SemanticAnalyzer::visitLocalDecl(ASTNode* n) {
    auto existLib = symtab.lookupGlobal(n->sval);
    if (existLib && existLib->kind == SymbolKind::LibraryFunction)
        error(n->line, "'local' declaration shadows library function '" + n->sval + "'");
    else
        symtab.insertLocal(n->sval, n->line);
    if (!n->children.empty()) visitNode(n->children[0].get());
}

void SemanticAnalyzer::visitIdExpr(ASTNode* n) {
    if (n->isGlobal) {
        auto sym = symtab.lookupGlobal(n->sval);
        if (!sym) error(n->line, "Global variable '" + n->sval + "' not found");
        return;
    }
    if (n->isLocal) {
        if (!symtab.lookupCurrent(n->sval))
            symtab.insertLocal(n->sval, n->line);
        return;
    }
    if (!symtab.lookup(n->sval))
        symtab.insertLocal(n->sval, n->line);
}

void SemanticAnalyzer::visitCall(ASTNode* n) {
    visitChildren(n);
}
