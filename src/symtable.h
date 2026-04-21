#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <optional>
#include <cassert>

enum class SymbolKind {
    GlobalVar,
    LocalVar,
    FormalParam,
    UserFunction,
    LibraryFunction,
};

struct Symbol {
    std::string name;
    SymbolKind  kind;
    int         scopeLevel;
    int         funcNestLevel;  // how deeply nested in functions
    int         offset;         // stack/closure slot
    int         line;

    bool isActive = true;

    std::string kindStr() const {
        switch(kind) {
            case SymbolKind::GlobalVar:       return "global";
            case SymbolKind::LocalVar:        return "local";
            case SymbolKind::FormalParam:     return "formal";
            case SymbolKind::UserFunction:    return "userfunc";
            case SymbolKind::LibraryFunction: return "libfunc";
        }
        return "?";
    }
};

using SymbolPtr = std::shared_ptr<Symbol>;

// A single scope frame
struct Scope {
    std::unordered_map<std::string, SymbolPtr> symbols;
    int level;
    bool isFunctionScope = false;

    Scope(int l, bool funcScope = false) : level(l), isFunctionScope(funcScope) {}

    SymbolPtr lookup(const std::string& name) const {
        auto it = symbols.find(name);
        if (it != symbols.end() && it->second->isActive)
            return it->second;
        return nullptr;
    }

    SymbolPtr insert(SymbolPtr sym) {
        symbols[sym->name] = sym;
        return sym;
    }
};

class SymbolTable {
public:
    SymbolTable() {
        // Global scope (level 0)
        scopes_.push_back(std::make_unique<Scope>(0, false));
        // Register Alpha standard library functions
        for (auto& lib : {"print", "input", "objectmemberkeys", "objecttotalmembers",
                          "objectcopy", "totalarguments", "argument", "typeof",
                          "strtonum", "sqrt", "cos", "sin"}) {
            auto sym = std::make_shared<Symbol>();
            sym->name = lib;
            sym->kind = SymbolKind::LibraryFunction;
            sym->scopeLevel = 0;
            sym->funcNestLevel = 0;
            sym->offset = 0;
            sym->line = 0;
            scopes_[0]->insert(sym);
        }
    }

    // --- Scope management ---
    void enterScope(bool isFunctionScope = false) {
        int newLevel = static_cast<int>(scopes_.size());
        scopes_.push_back(std::make_unique<Scope>(newLevel, isFunctionScope));
        if (isFunctionScope) funcNestLevel_++;
        scopeStack_.push_back(newLevel);
    }

    void exitScope() {
        assert(!scopeStack_.empty());
        int idx = scopeStack_.back();
        if (scopes_[idx]->isFunctionScope) funcNestLevel_--;
        // Deactivate symbols (hide them) but keep for dump
        for (auto& [name, sym] : scopes_[idx]->symbols)
            sym->isActive = false;
        scopeStack_.pop_back();
    }

    int currentScopeLevel() const {
        return scopeStack_.empty() ? 0 : scopeStack_.back();
    }

    int funcNestLevel() const { return funcNestLevel_; }

    // --- Symbol insertion ---
    SymbolPtr insertLocal(const std::string& name, int line) {
        auto sym = std::make_shared<Symbol>();
        sym->name = name;
        sym->kind = (currentScopeLevel() == 0) ? SymbolKind::GlobalVar : SymbolKind::LocalVar;
        sym->scopeLevel = currentScopeLevel();
        sym->funcNestLevel = funcNestLevel_;
        sym->offset = localOffsets_[funcNestLevel_]++;
        sym->line = line;
        currentScope()->insert(sym);
        return sym;
    }

    SymbolPtr insertFormal(const std::string& name, int line) {
        auto sym = std::make_shared<Symbol>();
        sym->name = name;
        sym->kind = SymbolKind::FormalParam;
        sym->scopeLevel = currentScopeLevel();
        sym->funcNestLevel = funcNestLevel_;
        sym->offset = formalOffsets_[funcNestLevel_]++;
        sym->line = line;
        currentScope()->insert(sym);
        return sym;
    }

    SymbolPtr insertFunction(const std::string& name, int line) {
        // Functions are inserted in the *current* scope before entering function scope
        auto sym = std::make_shared<Symbol>();
        sym->name = name;
        sym->kind = SymbolKind::UserFunction;
        sym->scopeLevel = currentScopeLevel();
        sym->funcNestLevel = funcNestLevel_;
        sym->offset = 0;
        sym->line = line;
        currentScope()->insert(sym);
        return sym;
    }

    // --- Lookup ---
    // Lookup visible symbol: walk scope stack outward
    SymbolPtr lookup(const std::string& name) const {
        for (int i = static_cast<int>(scopeStack_.size()) - 1; i >= 0; i--) {
            int idx = scopeStack_[i];
            auto sym = scopes_[idx]->lookup(name);
            if (sym) return sym;
        }
        // also check global scope if not in stack
        auto sym = scopes_[0]->lookup(name);
        return sym;
    }

    // Lookup only in current scope (for collision detection)
    SymbolPtr lookupCurrent(const std::string& name) const {
        return currentScope()->lookup(name);
    }

    // Lookup only global scope
    SymbolPtr lookupGlobal(const std::string& name) const {
        return scopes_[0]->lookup(name);
    }

    void resetLocalOffset() {
        localOffsets_[funcNestLevel_] = 0;
        formalOffsets_[funcNestLevel_] = 0;
    }

    // Print symbol table (for debugging / phase3 output)
    void dump() const {
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

private:
    std::vector<std::unique_ptr<Scope>> scopes_;
    std::vector<int> scopeStack_ = {0}; // start with global scope active
    int funcNestLevel_ = 0;
    std::unordered_map<int,int> localOffsets_;
    std::unordered_map<int,int> formalOffsets_;

    Scope* currentScope() const {
        return scopes_[scopeStack_.back()].get();
    }
};
