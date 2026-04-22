#pragma once
// LLVM 22: DisassemblerTypes.h must precede TargetRegistry-pulling headers
#include <llvm-c/DisassemblerTypes.h>

#include "ast.h"
#include "symtable.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/Analysis/CGSCCPassManager.h>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <memory>
#include <cassert>
#include <cstring>

namespace AlphaTags {
    constexpr int NIL      = 0;
    constexpr int NUMBER   = 1;
    constexpr int STRING   = 2;
    constexpr int BOOL     = 3;
    constexpr int TABLE    = 4;
    constexpr int USERFUNC = 5;
}

class LLVMCodeGen {
public:
    llvm::LLVMContext ctx_;
    std::unique_ptr<llvm::Module> mod_;
    llvm::IRBuilder<> builder_;

    // Types
    llvm::StructType*  avTy_    = nullptr;
    llvm::PointerType* avPtrTy_ = nullptr;

    // Runtime functions
    llvm::Function* rtPrint_    = nullptr;
    llvm::Function* rtInput_    = nullptr;
    llvm::Function* rtTblNew_   = nullptr;
    llvm::Function* rtTblGet_   = nullptr;
    llvm::Function* rtTblSet_   = nullptr;
    llvm::Function* rtConcat_   = nullptr;
    llvm::Function* rtTypeof_   = nullptr;
    llvm::Function* rtTonum_    = nullptr;
    llvm::Function* rtSqrt_     = nullptr;
    llvm::Function* rtCos_      = nullptr;
    llvm::Function* rtSin_      = nullptr;
    llvm::Function* rtValEq_    = nullptr;
    llvm::Function* rtValNeq_   = nullptr;
    llvm::Function* rtObjKeys_  = nullptr;
    llvm::Function* rtObjTotal_ = nullptr;
    llvm::Function* rtObjCopy_  = nullptr;
    llvm::Function* rtToString_ = nullptr;
    llvm::Function* rtFloor_    = nullptr;
    llvm::Function* rtCeil_     = nullptr;
    llvm::Function* rtAbs_      = nullptr;
    llvm::Function* rtMax_      = nullptr;
    llvm::Function* rtMin_      = nullptr;
    llvm::Function* rtPow_      = nullptr;
    llvm::Function* rtMakeNil_  = nullptr;
    llvm::Function* rtMakeNum_  = nullptr;
    llvm::Function* rtMakeBool_ = nullptr;
    llvm::Function* rtMakeStr_  = nullptr;
    llvm::Function* rtMakeFunc_ = nullptr;
    llvm::Function* rtGetNum_   = nullptr;
    llvm::Function* rtIsTruthy_ = nullptr;
    llvm::Function* rtGetFnPtr_ = nullptr;

    // Variable environment
    struct EnvFrame {
        std::unordered_map<std::string, llvm::AllocaInst*> slots;
        int fnDepth = 0;
    };
    std::vector<EnvFrame> env_;
    int curFnDepth_ = 0;

    std::unordered_map<std::string, llvm::GlobalVariable*> globals_;
    std::unordered_map<std::string, llvm::Function*>       funcRegistry_;

    llvm::Function* curFn_ = nullptr;
    std::vector<llvm::BasicBlock*> breakStack_;
    std::vector<llvm::BasicBlock*> contStack_;
    int lambdaId_ = 0;

    // Public interface
    LLVMCodeGen();
    void generate(ASTNode* root, SymbolTable& st);
    void dumpIR(llvm::raw_ostream& os);
    bool verify();
    void optimize(int level = 1);

private:
    // Setup
    void buildTypes();
    void declareRT();
    llvm::Function* mkRT(const char* nm, llvm::Type* ret,
                         std::vector<llvm::Type*> ps, bool va = false);

    // Type shorthand
    llvm::Type* i1t();
    llvm::Type* i32t();
    llvm::Type* i64t();
    llvm::Type* dblt();
    llvm::Type* voidt();
    llvm::Type* i8pt();
    llvm::Constant* ci32(int v);
    llvm::Constant* ci64(int64_t v);
    llvm::Constant* cfp(double d);

    // Statements
    void genStmt(ASTNode* n);
    void genBlock(ASTNode* n);
    void genIf(ASTNode* n);
    void genWhile(ASTNode* n);
    void genFor(ASTNode* n);
    void genReturn(ASTNode* n);
    void genFuncDefStmt(ASTNode* n);
    void genLocalDecl(ASTNode* n);

    // Expressions
    llvm::Value* genExpr(ASTNode* n);
    llvm::Value* genAssign(ASTNode* n);
    void         writeLval(ASTNode* lhs, llvm::Value* val);
    llvm::Value* genBinOp(ASTNode* n);
    llvm::Value* genAnd(ASTNode* n);
    llvm::Value* genOr(ASTNode* n);
    llvm::Value* genUnary(ASTNode* n);
    llvm::Value* genPostfix(ASTNode* n);
    llvm::Value* genCall(ASTNode* n);
    llvm::Value* genMember(ASTNode* n);
    llvm::Value* genIndex(ASTNode* n);
    llvm::Value* genTable(ASTNode* n);

    // Function builder
    llvm::Function* buildFunc(const std::string& name,
                               ASTNode* formals, ASTNode* body);
    void preRegisterFunctions(const std::vector<ASTNodePtr>& stmts);

    // Upvalue scanning
    void collectNames(ASTNode* n, std::unordered_set<std::string>& names);
    void collectLocals(ASTNode* n, std::unordered_set<std::string>& locals);
    void scanUpvalues(ASTNode* body,
                      const std::unordered_set<std::string>& outerLocals);
    void scanUpvaluesNode(ASTNode* n,
                          const std::unordered_set<std::string>& outerLocals);

    // AlphaVal constructors (heap via runtime)
    llvm::Value* avNil();
    llvm::Value* avNum(double d);
    llvm::Value* avNum(llvm::Value* dblVal);
    llvm::Value* avStr(const std::string& s);
    llvm::Value* avBool(bool b);
    llvm::Value* avBool(llvm::Value* i1val);
    llvm::Value* avFunc(llvm::Function* fn);

    // Extractors
    llvm::Value* numBits(llvm::Value* av);
    llvm::Value* toBool(llvm::Value* av);

    // Variable environment
    llvm::GlobalVariable* getOrCreateGlobal(const std::string& name);
    llvm::Value*          loadVar(const std::string& name);
    void                  storeVar(const std::string& name, llvm::Value* val);

    // BB helpers
    llvm::BasicBlock* newBB(const char* nm);
    void setBB(llvm::BasicBlock* bb);
    void brIfOpen(llvm::BasicBlock* dest);
};
