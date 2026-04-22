#include "symtable.h"
#include <cstdio>
#include <string>

// ── Symbol ────────────────────────────────────────────────────

std::string Symbol::kindStr() const {
    switch(kind) {
        case SymbolKind::GlobalVar:       return "global";
        case SymbolKind::LocalVar:        return "local";
        case SymbolKind::FormalParam:     return "formal";
        case SymbolKind::UserFunction:    return "userfunc";
        case SymbolKind::LibraryFunction: return "libfunc";
    }
    return "?";
}

// ── Scope ─────────────────────────────────────────────────────

Scope::Scope(int l, bool funcScope)
    : level(l), isFunctionScope(funcScope) {}

SymbolPtr Scope::lookup(const std::string& name) const {
    auto it = symbols.find(name);
    if (it != symbols.end() && it->second->isActive)
        return it->second;
    return nullptr;
}

SymbolPtr Scope::insert(SymbolPtr sym) {
    symbols[sym->name] = sym;
    return sym;
}

// ── SymbolTable ───────────────────────────────────────────────

SymbolTable::SymbolTable() {
    scopes_.push_back(std::make_unique<Scope>(0, false));
    for (auto lib : {"print", "input", "objectmemberkeys", "objecttotalmembers",
                     "objectcopy", "totalarguments", "argument", "typeof",
                     "strtonum", "tostring",
                     "sqrt", "cos", "sin", "pow",
                     "floor", "ceil", "abs", "max", "min",
                     "strlen", "strchar"}) {
        auto sym = std::make_shared<Symbol>();
        sym->name = lib;
        sym->kind = SymbolKind::LibraryFunction;
        scopes_[0]->insert(sym);
    }
}

void SymbolTable::enterScope(bool isFunctionScope) {
    int newLevel = static_cast<int>(scopes_.size());
    scopes_.push_back(std::make_unique<Scope>(newLevel, isFunctionScope));
    if (isFunctionScope) funcNestLevel_++;
    scopeStack_.push_back(newLevel);
}

void SymbolTable::exitScope() {
    assert(!scopeStack_.empty());
    int idx = scopeStack_.back();
    if (scopes_[idx]->isFunctionScope) funcNestLevel_--;
    for (auto& [name, sym] : scopes_[idx]->symbols)
        sym->isActive = false;
    scopeStack_.pop_back();
}

int SymbolTable::currentScopeLevel() const {
    return scopeStack_.empty() ? 0 : scopeStack_.back();
}

int SymbolTable::funcNestLevel() const { return funcNestLevel_; }

SymbolPtr SymbolTable::insertLocal(const std::string& name, int line) {
    auto sym = std::make_shared<Symbol>();
    sym->name = name;
    sym->kind = (currentScopeLevel() == 0) ? SymbolKind::GlobalVar : SymbolKind::LocalVar;
    sym->scopeLevel    = currentScopeLevel();
    sym->funcNestLevel = funcNestLevel_;
    sym->offset        = localOffsets_[funcNestLevel_]++;
    sym->line          = line;
    currentScope()->insert(sym);
    return sym;
}

SymbolPtr SymbolTable::insertFormal(const std::string& name, int line) {
    auto sym = std::make_shared<Symbol>();
    sym->name          = name;
    sym->kind          = SymbolKind::FormalParam;
    sym->scopeLevel    = currentScopeLevel();
    sym->funcNestLevel = funcNestLevel_;
    sym->offset        = formalOffsets_[funcNestLevel_]++;
    sym->line          = line;
    currentScope()->insert(sym);
    return sym;
}

SymbolPtr SymbolTable::insertFunction(const std::string& name, int line) {
    auto sym = std::make_shared<Symbol>();
    sym->name          = name;
    sym->kind          = SymbolKind::UserFunction;
    sym->scopeLevel    = currentScopeLevel();
    sym->funcNestLevel = funcNestLevel_;
    sym->offset        = 0;
    sym->line          = line;
    currentScope()->insert(sym);
    return sym;
}

SymbolPtr SymbolTable::lookup(const std::string& name) const {
    for (int i = static_cast<int>(scopeStack_.size()) - 1; i >= 0; i--) {
        int idx = scopeStack_[i];
        auto sym = scopes_[idx]->lookup(name);
        if (sym) return sym;
    }
    return scopes_[0]->lookup(name);
}

SymbolPtr SymbolTable::lookupCurrent(const std::string& name) const {
    return currentScope()->lookup(name);
}

SymbolPtr SymbolTable::lookupGlobal(const std::string& name) const {
    return scopes_[0]->lookup(name);
}

void SymbolTable::resetLocalOffset() {
    localOffsets_[funcNestLevel_]  = 0;
    formalOffsets_[funcNestLevel_] = 0;
}

void SymbolTable::dump() const {
    printf("\n=== Symbol Table Dump ===\n");
    printf("%-20s %-12s %-6s %-6s %-6s %s\n",
           "Name", "Kind", "Scope", "Func", "Offset", "Line");
    printf("%s\n", std::string(65, '-').c_str());
    for (auto& scope : scopes_) {
        for (auto& [name, sym] : scope->symbols) {
            printf("%-20s %-12s %-6d %-6d %-6d %d\n",
                   name.c_str(), sym->kindStr().c_str(),
                   sym->scopeLevel, sym->funcNestLevel,
                   sym->offset, sym->line);
        }
    }
    printf("========================\n\n");
}

Scope* SymbolTable::currentScope() const {
    return scopes_[scopeStack_.back()].get();
}
