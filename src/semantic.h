#pragma once
#include "ast.h"
#include "symtable.h"
#include <stdexcept>
#include <string>
#include <vector>
#include <functional>

struct SemanticError {
    int line;
    std::string msg;
};

class SemanticAnalyzer {
public:
    SymbolTable symtab;
    std::vector<SemanticError> errors;

    // Returns false if there were errors
    bool analyze(ASTNode* root) {
        visitNode(root);
        return errors.empty();
    }

    void dumpSymbols() { symtab.dump(); }

private:
    int loopDepth_ = 0;
    int funcDepth_ = 0;

    void error(int line, const std::string& msg) {
        errors.push_back({line, msg});
        fprintf(stderr, "Semantic error at line %d: %s\n", line, msg.c_str());
    }

    void visitNode(ASTNode* n) {
        if (!n) return;
        switch (n->kind) {
            case NodeKind::Program:
            case NodeKind::Block:
                visitBlock(n);
                break;
            case NodeKind::FuncDef:
                visitFuncDef(n);
                break;
            case NodeKind::LambdaDef:
                visitLambdaDef(n);
                break;
            case NodeKind::IfStmt:
                visitChildren(n);
                break;
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
            case NodeKind::AssignExpr:
                visitAssign(n);
                break;
            case NodeKind::IdExpr:
                visitIdExpr(n);
                break;
            case NodeKind::LocalDecl:
                visitLocalDecl(n);
                break;
            case NodeKind::CallExpr:
                visitCall(n);
                break;
            default:
                visitChildren(n);
                break;
        }
    }

    void visitChildren(ASTNode* n) {
        for (auto& child : n->children)
            visitNode(child.get());
    }

    void visitBlock(ASTNode* n) {
        bool isTopLevel = (n->kind == NodeKind::Program);
        if (!isTopLevel)
            symtab.enterScope(false);
        for (auto& child : n->children)
            visitNode(child.get());
        if (!isTopLevel)
            symtab.exitScope();
    }

    void visitFuncDef(ASTNode* n) {
        // n->sval = function name, children[0] = formals, children[1] = body
        const std::string& fname = n->sval;

        // Check for library function shadow
        auto existLib = symtab.lookupGlobal(fname);
        if (existLib && existLib->kind == SymbolKind::LibraryFunction) {
            error(n->line, "Cannot shadow library function '" + fname + "'");
            return;
        }

        // Insert function symbol in current scope
        if (!fname.empty()) {
            auto existing = symtab.lookupCurrent(fname);
            if (existing && existing->kind != SymbolKind::UserFunction)
                error(n->line, "Symbol '" + fname + "' already declared in this scope");
            else
                symtab.insertFunction(fname, n->line);
        }

        // Enter function scope
        symtab.enterScope(true);
        symtab.resetLocalOffset();
        funcDepth_++;

        // Install formals (children[0] is the ElemList of FormalParam)
        ASTNode* formals = n->children[0].get();
        for (auto& fp : formals->children) {
            const std::string& pname = fp->sval;
            auto existG = symtab.lookupGlobal(pname);
            if (existG && existG->kind == SymbolKind::LibraryFunction)
                error(fp->line, "Formal param '" + pname + "' shadows library function");
            else
                symtab.insertFormal(pname, fp->line);
        }

        // Visit body
        ASTNode* body = n->children[1].get();
        // Body is a Block, visit its children in current (function) scope
        for (auto& child : body->children)
            visitNode(child.get());

        funcDepth_--;
        symtab.exitScope();
    }

    void visitLambdaDef(ASTNode* n) {
        // Same as FuncDef but anonymous
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

    void visitAssign(ASTNode* n) {
        // LHS
        ASTNode* lhs = n->children[0].get();
        // RHS
        ASTNode* rhs = n->children[1].get();

        // If LHS is a bare IdExpr and not yet declared, auto-declare it
        if (lhs->kind == NodeKind::IdExpr && !lhs->isLocal && !lhs->isGlobal) {
            auto sym = symtab.lookup(lhs->sval);
            if (!sym) {
                // implicit declaration in current scope
                symtab.insertLocal(lhs->sval, lhs->line);
            } else if (sym->kind == SymbolKind::LibraryFunction ||
                       sym->kind == SymbolKind::UserFunction) {
                error(lhs->line, "Cannot assign to function '" + lhs->sval + "'");
            }
        }
        visitNode(lhs);
        visitNode(rhs);
    }

    void visitLocalDecl(ASTNode* n) {
        const std::string& name = n->sval;
        auto existLib = symtab.lookupGlobal(name);
        if (existLib && existLib->kind == SymbolKind::LibraryFunction)
            error(n->line, "'local' declaration shadows library function '" + name + "'");
        else
            symtab.insertLocal(name, n->line);
        if (!n->children.empty()) visitNode(n->children[0].get());
    }

    void visitIdExpr(ASTNode* n) {
        const std::string& name = n->sval;
        if (n->isGlobal) {
            // ::x — lookup only in global scope
            auto sym = symtab.lookupGlobal(name);
            if (!sym)
                error(n->line, "Global variable '" + name + "' not found");
            return;
        }
        if (n->isLocal) {
            // local x — declare in current scope
            auto sym = symtab.lookupCurrent(name);
            if (!sym) symtab.insertLocal(name, n->line);
            return;
        }
        // Plain id — lookup
        auto sym = symtab.lookup(name);
        if (!sym) {
            // Auto-declare as local — this is valid in Alpha (dynamic language)
            // But warn if inside a function that it's implicitly global
            if (symtab.funcNestLevel() > 0)
                symtab.insertLocal(name, n->line);
            else
                symtab.insertLocal(name, n->line);
        }
    }

    void visitCall(ASTNode* n) {
        visitChildren(n);
    }
};
