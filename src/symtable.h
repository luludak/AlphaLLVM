#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
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
    SymbolKind  kind          = SymbolKind::LocalVar;
    int         scopeLevel    = 0;
    int         funcNestLevel = 0;
    int         offset        = 0;
    int         line          = 0;
    bool        isActive      = true;

    std::string kindStr() const;
};

using SymbolPtr = std::shared_ptr<Symbol>;

struct Scope {
    std::unordered_map<std::string, SymbolPtr> symbols;
    int  level;
    bool isFunctionScope = false;

    Scope(int l, bool funcScope = false);
    SymbolPtr lookup(const std::string& name) const;
    SymbolPtr insert(SymbolPtr sym);
};

class SymbolTable {
public:
    SymbolTable();

    void      enterScope(bool isFunctionScope = false);
    void      exitScope();
    int       currentScopeLevel() const;
    int       funcNestLevel() const;

    SymbolPtr insertLocal(const std::string& name, int line);
    SymbolPtr insertFormal(const std::string& name, int line);
    SymbolPtr insertFunction(const std::string& name, int line);

    SymbolPtr lookup(const std::string& name) const;
    SymbolPtr lookupCurrent(const std::string& name) const;
    SymbolPtr lookupGlobal(const std::string& name) const;

    void resetLocalOffset();
    void dump() const;

private:
    std::vector<std::unique_ptr<Scope>> scopes_;
    std::vector<int> scopeStack_ = {0};
    int funcNestLevel_ = 0;
    std::unordered_map<int,int> localOffsets_;
    std::unordered_map<int,int> formalOffsets_;

    Scope* currentScope() const;
};
