#pragma once
/*
 * codegen.h — LLVM IR backend for the Alpha compiler.
 *
 * AlphaVal layout (defined in alpha_runtime.c):
 *   struct AlphaVal { int32_t tag; int64_t data; }
 *
 * ALL AlphaVal construction is done via heap-allocating runtime calls
 * (alpha_rt_make_*). We never use alloca for AlphaVal because the
 * pointers escape into globals, tables, and return values.
 *
 * Named user functions are called directly via LLVM call instructions
 * (funcRegistry_). Only truly dynamic (first-class) calls go through
 * the AlphaVal function-pointer path.
 */

// LLVM 22: DisassemblerTypes.h must precede TargetRegistry.h
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
    // ── Core LLVM objects ─────────────────────────────────────
    llvm::LLVMContext ctx_;
    std::unique_ptr<llvm::Module> mod_;
    llvm::IRBuilder<> builder_;

    // ── Types ─────────────────────────────────────────────────
    llvm::StructType* avTy_    = nullptr;  // AlphaVal = {i32, i64}
    llvm::PointerType* avPtrTy_ = nullptr; // AlphaVal*

    // ── Runtime function declarations ─────────────────────────
    // Core
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
    // Heap constructors
    llvm::Function* rtMakeNil_  = nullptr;
    llvm::Function* rtMakeNum_  = nullptr;
    llvm::Function* rtMakeBool_ = nullptr;
    llvm::Function* rtMakeStr_  = nullptr;
    llvm::Function* rtMakeFunc_ = nullptr;
    // Extractors
    llvm::Function* rtGetNum_   = nullptr;
    llvm::Function* rtIsTruthy_ = nullptr;
    llvm::Function* rtGetFnPtr_ = nullptr;

    // ── Variable environment ──────────────────────────────────
    // Each frame maps name -> alloca of AlphaVal* slot.
    // fnDepth identifies which function owns the frame.
    struct EnvFrame {
        std::unordered_map<std::string, llvm::AllocaInst*> slots;
        int fnDepth = 0;
    };
    std::vector<EnvFrame> env_;
    int curFnDepth_ = 0;

    // Variables promoted to module globals due to cross-function capture
    std::unordered_map<std::string, llvm::GlobalVariable*> globals_;

    // Named user functions — for direct calls (avoids AlphaVal round-trip)
    std::unordered_map<std::string, llvm::Function*> funcRegistry_;

    // Current LLVM function being compiled
    llvm::Function* curFn_ = nullptr;

    // break / continue jump targets
    std::vector<llvm::BasicBlock*> breakStack_;
    std::vector<llvm::BasicBlock*> contStack_;

    int lambdaId_ = 0;

    // ── Constructor ───────────────────────────────────────────
    LLVMCodeGen()
        : mod_(std::make_unique<llvm::Module>("alpha_module", ctx_))
        , builder_(ctx_)
    {
        buildTypes();
        declareRT();
    }

    // ── Type helpers ──────────────────────────────────────────
    void buildTypes() {
        avTy_    = llvm::StructType::create(ctx_, {i32t(), i64t()}, "AlphaVal");
        // LLVM 22: use opaque pointer (context overload)
        avPtrTy_ = llvm::PointerType::get(ctx_, 0);
    }

    llvm::Type* i1t()   { return llvm::Type::getInt1Ty(ctx_);  }
    llvm::Type* i32t()  { return llvm::Type::getInt32Ty(ctx_); }
    llvm::Type* i64t()  { return llvm::Type::getInt64Ty(ctx_); }
    llvm::Type* dblt()  { return llvm::Type::getDoubleTy(ctx_); }
    llvm::Type* voidt() { return llvm::Type::getVoidTy(ctx_);  }
    llvm::Type* i8pt()  { return llvm::PointerType::get(ctx_, 0); }

    llvm::Constant* ci32(int v)     { return llvm::ConstantInt::get(i32t(), v); }
    llvm::Constant* ci64(int64_t v) { return llvm::ConstantInt::get(i64t(), v); }
    llvm::Constant* cfp(double d)   { return llvm::ConstantFP::get(dblt(), d); }

    // ── Runtime declaration helper ────────────────────────────
    llvm::Function* mkRT(const char* nm, llvm::Type* ret,
                         std::vector<llvm::Type*> ps, bool va = false) {
        auto* fty = llvm::FunctionType::get(ret, ps, va);
        return llvm::Function::Create(fty, llvm::Function::ExternalLinkage,
                                      nm, mod_.get());
    }

    void declareRT() {
        auto* avp = avPtrTy_;
        auto* vt  = voidt();
        auto* p   = i8pt();
        rtPrint_    = mkRT("alpha_rt_print",             vt,    {avp});
        rtInput_    = mkRT("alpha_rt_input",             avp,   {});
        rtTblNew_   = mkRT("alpha_rt_table_new",         avp,   {});
        rtTblGet_   = mkRT("alpha_rt_table_get",         avp,   {avp, avp});
        rtTblSet_   = mkRT("alpha_rt_table_set",         vt,    {avp, avp, avp});
        rtConcat_   = mkRT("alpha_rt_concat",            avp,   {avp, avp});
        rtTypeof_   = mkRT("alpha_rt_typeof",            avp,   {avp});
        rtTonum_    = mkRT("alpha_rt_strtonum",          avp,   {avp});
        rtSqrt_     = mkRT("alpha_rt_sqrt",              avp,   {avp});
        rtCos_      = mkRT("alpha_rt_cos",               avp,   {avp});
        rtSin_      = mkRT("alpha_rt_sin",               avp,   {avp});
        rtValEq_    = mkRT("alpha_rt_val_eq",            i1t(), {avp, avp});
        rtValNeq_   = mkRT("alpha_rt_val_neq",           i1t(), {avp, avp});
        rtObjKeys_  = mkRT("alpha_rt_objectmemberkeys",  avp,   {avp});
        rtObjTotal_ = mkRT("alpha_rt_objecttotalmembers",avp,   {avp});
        rtObjCopy_  = mkRT("alpha_rt_objectcopy",        avp,   {avp});
        rtToString_ = mkRT("alpha_rt_tostring",          avp,   {avp});
        rtFloor_    = mkRT("alpha_rt_floor",             avp,   {avp});
        rtCeil_     = mkRT("alpha_rt_ceil",              avp,   {avp});
        rtAbs_      = mkRT("alpha_rt_abs",               avp,   {avp});
        rtMax_      = mkRT("alpha_rt_max",               avp,   {avp, avp});
        rtMin_      = mkRT("alpha_rt_min",               avp,   {avp, avp});
        // Heap constructors
        rtMakeNil_  = mkRT("alpha_rt_make_nil",    avp,    {});
        rtMakeNum_  = mkRT("alpha_rt_make_number", avp,    {dblt()});
        rtMakeBool_ = mkRT("alpha_rt_make_bool",   avp,    {i32t()});
        rtMakeStr_  = mkRT("alpha_rt_make_string", avp,    {p});
        rtMakeFunc_ = mkRT("alpha_rt_make_func",   avp,    {i64t()});
        // Extractors
        rtGetNum_   = mkRT("alpha_rt_get_number",  dblt(), {avp});
        rtIsTruthy_ = mkRT("alpha_rt_is_truthy",   i32t(), {avp});
        rtGetFnPtr_ = mkRT("alpha_rt_get_funcptr", i64t(), {avp});
    }

    // ═════════════════════════════════════════════════════════
    // Top-level entry
    // ═════════════════════════════════════════════════════════
    void generate(ASTNode* root, SymbolTable& /*st*/) {
        auto* mainTy = llvm::FunctionType::get(i32t(), {}, false);
        curFn_ = llvm::Function::Create(mainTy, llvm::Function::ExternalLinkage,
                                        "main", mod_.get());
        auto* entry = llvm::BasicBlock::Create(ctx_, "entry", curFn_);
        builder_.SetInsertPoint(entry);
        env_.push_back({{}, 0});

        ASTNode* block = root->children[0].get();

        // Pre-scan upvalues
        {
            std::unordered_set<std::string> topLocals;
            collectLocals(block, topLocals);
            scanUpvalues(block, topLocals);
        }
        // Pre-register all functions so mutual recursion works
        preRegisterFunctions(block->children);

        for (auto& stmt : block->children)
            genStmt(stmt.get());
        if (!builder_.GetInsertBlock()->getTerminator())
            builder_.CreateRet(ci32(0));
        env_.pop_back();
    }

    // Forward-declare every FuncDef in a statement list into funcRegistry_.
    // This allows mutually recursive calls: isEven can call isOdd even though
    // isOdd's body hasn't been compiled yet, because the LLVM Function* stub
    // already exists with the right signature.
    void preRegisterFunctions(const std::vector<ASTNodePtr>& stmts) {
        for (auto& stmt : stmts) {
            if (!stmt) continue;
            ASTNode* n = stmt.get();
            // Unwrap ExprStmt
            if (n->kind == NodeKind::ExprStmt && !n->children.empty())
                n = n->children[0].get();
            if (n && n->kind == NodeKind::FuncDef) {
                size_t np = n->children[0]->children.size();
                std::vector<llvm::Type*> pts(np, avPtrTy_);
                auto* fty = llvm::FunctionType::get(avPtrTy_, pts, false);
                auto* fn  = llvm::Function::Create(
                    fty, llvm::Function::InternalLinkage,
                    n->sval, mod_.get());
                funcRegistry_[n->sval] = fn;
            }
            // Also pre-register inside blocks
            if (n && n->kind == NodeKind::Block)
                preRegisterFunctions(n->children);
        }
    }

    // ═════════════════════════════════════════════════════════
    // Statements
    // ═════════════════════════════════════════════════════════
    void genStmt(ASTNode* n) {
        if (!n) return;
        if (builder_.GetInsertBlock()->getTerminator()) return;
        switch (n->kind) {
            case NodeKind::ExprStmt:    genExpr(n->children[0].get()); break;
            case NodeKind::Block:       genBlock(n);   break;
            case NodeKind::IfStmt:      genIf(n);      break;
            case NodeKind::WhileStmt:   genWhile(n);   break;
            case NodeKind::ForStmt:     genFor(n);     break;
            case NodeKind::ReturnStmt:  genReturn(n);  break;
            case NodeKind::BreakStmt:
                if (!breakStack_.empty()) builder_.CreateBr(breakStack_.back());
                break;
            case NodeKind::ContinueStmt:
                if (!contStack_.empty())  builder_.CreateBr(contStack_.back());
                break;
            case NodeKind::FuncDef:     genFuncDefStmt(n); break;
            case NodeKind::LocalDecl:   genLocalDecl(n);   break;
            default:                    genExpr(n); break;
        }
    }

    void genBlock(ASTNode* n) {
        env_.push_back({{}, curFnDepth_});
        for (auto& c : n->children) genStmt(c.get());
        env_.pop_back();
    }

    void genIf(ASTNode* n) {
        llvm::Value* cond = toBool(genExpr(n->children[0].get()));
        bool hasElse = (n->children.size() == 3);
        auto* thenBB  = newBB("if.then");
        auto* mergeBB = newBB("if.end");
        auto* elseBB  = hasElse ? newBB("if.else") : mergeBB;
        builder_.CreateCondBr(cond, thenBB, elseBB);
        setBB(thenBB);
        genStmt(n->children[1].get());
        brIfOpen(mergeBB);
        if (hasElse) {
            setBB(elseBB);
            genStmt(n->children[2].get());
            brIfOpen(mergeBB);
        }
        setBB(mergeBB);
    }

    void genWhile(ASTNode* n) {
        auto* condBB  = newBB("while.cond");
        auto* bodyBB  = newBB("while.body");
        auto* afterBB = newBB("while.end");
        breakStack_.push_back(afterBB);
        contStack_.push_back(condBB);
        builder_.CreateBr(condBB);
        setBB(condBB);
        builder_.CreateCondBr(toBool(genExpr(n->children[0].get())), bodyBB, afterBB);
        setBB(bodyBB);
        genStmt(n->children[1].get());
        brIfOpen(condBB);
        breakStack_.pop_back();
        contStack_.pop_back();
        setBB(afterBB);
    }

    void genFor(ASTNode* n) {
        auto* condBB  = newBB("for.cond");
        auto* bodyBB  = newBB("for.body");
        auto* stepBB  = newBB("for.step");
        auto* afterBB = newBB("for.end");
        if (n->children[0]) genExpr(n->children[0].get());
        builder_.CreateBr(condBB);
        setBB(condBB);
        if (n->children[1])
            builder_.CreateCondBr(toBool(genExpr(n->children[1].get())), bodyBB, afterBB);
        else
            builder_.CreateBr(bodyBB);
        breakStack_.push_back(afterBB);
        contStack_.push_back(stepBB);
        setBB(bodyBB);
        genStmt(n->children[3].get());
        brIfOpen(stepBB);
        setBB(stepBB);
        if (n->children[2]) genExpr(n->children[2].get());
        builder_.CreateBr(condBB);
        breakStack_.pop_back();
        contStack_.pop_back();
        setBB(afterBB);
    }

    void genReturn(ASTNode* n) {
        if (!n->children.empty())
            builder_.CreateRet(genExpr(n->children[0].get()));
        else
            builder_.CreateRet(avNil());
    }

    void genFuncDefStmt(ASTNode* n) {
        llvm::Function* fn = buildFunc(n->sval, n->children[0].get(),
                                        n->children[1].get());
        storeVar(n->sval, avFunc(fn));
    }

    void genLocalDecl(ASTNode* n) {
        llvm::Value* initVal = n->children.empty()
                             ? avNil()
                             : genExpr(n->children[0].get());
        // If this name was pre-promoted to a global (it's captured by an
        // inner function), store there directly instead of creating an alloca.
        auto git = globals_.find(n->sval);
        if (git != globals_.end()) {
            builder_.CreateStore(initVal, git->second);
            return;
        }
        auto* slot = builder_.CreateAlloca(avPtrTy_, nullptr, n->sval + ".s");
        builder_.CreateStore(initVal, slot);
        env_.back().slots[n->sval] = slot;
    }

    // ═════════════════════════════════════════════════════════
    // Expressions
    // ═════════════════════════════════════════════════════════
    llvm::Value* genExpr(ASTNode* n) {
        if (!n) return avNil();
        switch (n->kind) {
            case NodeKind::IntLit:   return avNum((double)n->ival);
            case NodeKind::FloatLit: return avNum(n->fval);
            case NodeKind::StrLit:   return avStr(n->sval);
            case NodeKind::BoolLit:  return avBool(n->bval);
            case NodeKind::NilLit:   return avNil();
            case NodeKind::IdExpr:   return loadVar(n->sval);
            case NodeKind::AssignExpr:       return genAssign(n);
            case NodeKind::BinOp:            return genBinOp(n);
            case NodeKind::UnaryOp:          return genUnary(n);
            case NodeKind::PostfixOp:        return genPostfix(n);
            case NodeKind::CallExpr:         return genCall(n);
            case NodeKind::MemberExpr:       return genMember(n);
            case NodeKind::IndexExpr:        return genIndex(n);
            case NodeKind::TableConstructor: return genTable(n);
            case NodeKind::FuncDef:
                genFuncDefStmt(n);
                return loadVar(n->sval);
            case NodeKind::LambdaDef: {
                std::string nm = "__lam_" + std::to_string(lambdaId_++);
                return avFunc(buildFunc(nm, n->children[0].get(), n->children[1].get()));
            }
            case NodeKind::ExprStmt:
                return genExpr(n->children[0].get());
            case NodeKind::LocalDecl:
                genLocalDecl(n);
                return loadVar(n->sval);
            default:
                return avNil();
        }
    }

    llvm::Value* genAssign(ASTNode* n) {
        llvm::Value* rhs = genExpr(n->children[1].get());
        writeLval(n->children[0].get(), rhs);
        return rhs;
    }

    void writeLval(ASTNode* lhs, llvm::Value* val) {
        switch (lhs->kind) {
            case NodeKind::IdExpr:
                storeVar(lhs->sval, val); break;
            case NodeKind::MemberExpr:
                builder_.CreateCall(rtTblSet_,
                    {genExpr(lhs->children[0].get()), avStr(lhs->sval), val});
                break;
            case NodeKind::IndexExpr:
                builder_.CreateCall(rtTblSet_,
                    {genExpr(lhs->children[0].get()),
                     genExpr(lhs->children[1].get()), val});
                break;
            default: break;
        }
    }

    llvm::Value* genBinOp(ASTNode* n) {
        if (n->binop == BinOpKind::And) return genAnd(n);
        if (n->binop == BinOpKind::Or)  return genOr(n);
        if (n->binop == BinOpKind::Concat) {
            return builder_.CreateCall(rtConcat_,
                {genExpr(n->children[0].get()), genExpr(n->children[1].get())});
        }
        llvm::Value* lv = genExpr(n->children[0].get());
        llvm::Value* rv = genExpr(n->children[1].get());
        if (n->binop == BinOpKind::Eq)
            return avBool(builder_.CreateCall(rtValEq_,  {lv, rv}));
        if (n->binop == BinOpKind::Neq)
            return avBool(builder_.CreateCall(rtValNeq_, {lv, rv}));
        llvm::Value* ln = numBits(lv);
        llvm::Value* rn = numBits(rv);
        switch (n->binop) {
            case BinOpKind::Add: return avNum(builder_.CreateFAdd(ln, rn, "add"));
            case BinOpKind::Sub: return avNum(builder_.CreateFSub(ln, rn, "sub"));
            case BinOpKind::Mul: return avNum(builder_.CreateFMul(ln, rn, "mul"));
            case BinOpKind::Div: return avNum(builder_.CreateFDiv(ln, rn, "div"));
            case BinOpKind::Mod: return avNum(builder_.CreateFRem(ln, rn, "mod"));
            case BinOpKind::Lt:  return avBool(builder_.CreateFCmpOLT(ln, rn, "lt"));
            case BinOpKind::Gt:  return avBool(builder_.CreateFCmpOGT(ln, rn, "gt"));
            case BinOpKind::Leq: return avBool(builder_.CreateFCmpOLE(ln, rn, "le"));
            case BinOpKind::Geq: return avBool(builder_.CreateFCmpOGE(ln, rn, "ge"));
            default: return avNil();
        }
    }

    llvm::Value* genAnd(ASTNode* n) {
        auto* rhsBB = newBB("and.rhs"); auto* endBB = newBB("and.end");
        llvm::Value* lc = toBool(genExpr(n->children[0].get()));
        auto* lhsEnd = builder_.GetInsertBlock();
        builder_.CreateCondBr(lc, rhsBB, endBB);
        setBB(rhsBB);
        llvm::Value* rc = toBool(genExpr(n->children[1].get()));
        auto* rhsEnd = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);
        setBB(endBB);
        auto* phi = builder_.CreatePHI(i1t(), 2, "and");
        phi->addIncoming(llvm::ConstantInt::getFalse(ctx_), lhsEnd);
        phi->addIncoming(rc, rhsEnd);
        return avBool(phi);
    }

    llvm::Value* genOr(ASTNode* n) {
        auto* rhsBB = newBB("or.rhs"); auto* endBB = newBB("or.end");
        llvm::Value* lc = toBool(genExpr(n->children[0].get()));
        auto* lhsEnd = builder_.GetInsertBlock();
        builder_.CreateCondBr(lc, endBB, rhsBB);
        setBB(rhsBB);
        llvm::Value* rc = toBool(genExpr(n->children[1].get()));
        auto* rhsEnd = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);
        setBB(endBB);
        auto* phi = builder_.CreatePHI(i1t(), 2, "or");
        phi->addIncoming(llvm::ConstantInt::getTrue(ctx_), lhsEnd);
        phi->addIncoming(rc, rhsEnd);
        return avBool(phi);
    }

    llvm::Value* genUnary(ASTNode* n) {
        switch (n->unop) {
            case UnaryOpKind::Minus:
                return avNum(builder_.CreateFNeg(numBits(genExpr(n->children[0].get())), "neg"));
            case UnaryOpKind::Not:
                return avBool(builder_.CreateNot(toBool(genExpr(n->children[0].get())), "not"));
            case UnaryOpKind::PreInc: {
                auto* v  = genExpr(n->children[0].get());
                auto* nv = avNum(builder_.CreateFAdd(numBits(v), cfp(1.0), "inc"));
                writeLval(n->children[0].get(), nv); return nv;
            }
            case UnaryOpKind::PreDec: {
                auto* v  = genExpr(n->children[0].get());
                auto* nv = avNum(builder_.CreateFSub(numBits(v), cfp(1.0), "dec"));
                writeLval(n->children[0].get(), nv); return nv;
            }
        }
        return avNil();
    }

    llvm::Value* genPostfix(ASTNode* n) {
        auto* old = genExpr(n->children[0].get());
        auto* nv  = (n->postop == PostfixOpKind::PostInc)
                  ? avNum(builder_.CreateFAdd(numBits(old), cfp(1.0), "pi"))
                  : avNum(builder_.CreateFSub(numBits(old), cfp(1.0), "pd"));
        writeLval(n->children[0].get(), nv);
        return old;
    }

    llvm::Value* genCall(ASTNode* n) {
        ASTNode* cn = n->children[0].get();
        ASTNode* an = n->children[1].get();
        std::vector<llvm::Value*> args;
        for (auto& a : an->children)
            args.push_back(genExpr(a.get()));

        if (cn->kind == NodeKind::IdExpr) {
            const std::string& nm = cn->sval;
            // Built-ins
            if (nm == "print") {
                for (auto* a : args) builder_.CreateCall(rtPrint_, {a});
                return avNil();
            }
            if (nm == "input")                                return builder_.CreateCall(rtInput_,    {});
            if (nm == "typeof"      && args.size()==1)        return builder_.CreateCall(rtTypeof_,   {args[0]});
            if (nm == "tostring"    && args.size()==1)        return builder_.CreateCall(rtToString_, {args[0]});
            if (nm == "strtonum"    && args.size()==1)        return builder_.CreateCall(rtToString_, {args[0]});
            if (nm == "sqrt"        && args.size()==1)        return builder_.CreateCall(rtSqrt_,     {args[0]});
            if (nm == "cos"         && args.size()==1)        return builder_.CreateCall(rtCos_,      {args[0]});
            if (nm == "sin"         && args.size()==1)        return builder_.CreateCall(rtSin_,      {args[0]});
            if (nm == "floor"       && args.size()==1)        return builder_.CreateCall(rtFloor_,    {args[0]});
            if (nm == "ceil"        && args.size()==1)        return builder_.CreateCall(rtCeil_,     {args[0]});
            if (nm == "abs"         && args.size()==1)        return builder_.CreateCall(rtAbs_,      {args[0]});
            if (nm == "max"         && args.size()==2)        return builder_.CreateCall(rtMax_,      {args[0], args[1]});
            if (nm == "min"         && args.size()==2)        return builder_.CreateCall(rtMin_,      {args[0], args[1]});
            if (nm == "objectmemberkeys"   && args.size()==1) return builder_.CreateCall(rtObjKeys_,  {args[0]});
            if (nm == "objecttotalmembers" && args.size()==1) return builder_.CreateCall(rtObjTotal_, {args[0]});
            if (nm == "objectcopy"         && args.size()==1) return builder_.CreateCall(rtObjCopy_,  {args[0]});
            // Direct call to named user function
            auto fit = funcRegistry_.find(nm);
            if (fit != funcRegistry_.end())
                return builder_.CreateCall(fit->second, args);
        }

        // Indirect (first-class function) call via AlphaVal
        llvm::Value* fnAV  = genExpr(cn);
        llvm::Value* fnRaw = builder_.CreateCall(rtGetFnPtr_, {fnAV}, "fnraw");
        std::vector<llvm::Type*> paramTys(args.size(), avPtrTy_);
        auto* fnTy  = llvm::FunctionType::get(avPtrTy_, paramTys, false);
        // LLVM 22: opaque pointers — no need to specify pointee type
        auto* fnPtr = builder_.CreateIntToPtr(fnRaw, avPtrTy_, "fnptr");
        return builder_.CreateCall(fnTy, fnPtr, args);
    }

    llvm::Value* genMember(ASTNode* n) {
        return builder_.CreateCall(rtTblGet_,
            {genExpr(n->children[0].get()), avStr(n->sval)});
    }

    llvm::Value* genIndex(ASTNode* n) {
        return builder_.CreateCall(rtTblGet_,
            {genExpr(n->children[0].get()), genExpr(n->children[1].get())});
    }

    llvm::Value* genTable(ASTNode* n) {
        llvm::Value* tbl = builder_.CreateCall(rtTblNew_, {});
        int idx = 0;
        for (auto& elem : n->children) {
            if (elem->kind == NodeKind::TableElem) {
                builder_.CreateCall(rtTblSet_,
                    {tbl, avStr(elem->sval), genExpr(elem->children[0].get())});
            } else if (elem->kind == NodeKind::TableIndexElem) {
                builder_.CreateCall(rtTblSet_,
                    {tbl, genExpr(elem->children[0].get()),
                          genExpr(elem->children[1].get())});
            } else {
                builder_.CreateCall(rtTblSet_,
                    {tbl, avNum((double)idx++), genExpr(elem.get())});
            }
        }
        return tbl;
    }

    // ═════════════════════════════════════════════════════════
    // Function body builder
    // ═════════════════════════════════════════════════════════
    llvm::Function* buildFunc(const std::string& name,
                               ASTNode* formals, ASTNode* body) {
        size_t np = formals->children.size();
        std::vector<llvm::Type*> pts(np, avPtrTy_);
        auto* fty = llvm::FunctionType::get(avPtrTy_, pts, false);

        // Reuse pre-registered stub if available (avoids duplicate symbols
        // and ensures mutual recursion stubs are already in funcRegistry_).
        llvm::Function* fn = nullptr;
        auto fit = funcRegistry_.find(name);
        if (fit != funcRegistry_.end() && fit->second->empty()) {
            fn = fit->second;  // reuse the stub (it has no basic blocks yet)
        } else {
            fn = llvm::Function::Create(fty, llvm::Function::InternalLinkage,
                                        name, mod_.get());
            if (!name.empty() && name.substr(0,6) != "__lam_")
                funcRegistry_[name] = fn;
        }

        auto* outerFn    = curFn_;
        auto* outerBB    = builder_.GetInsertBlock();
        int   outerDepth = curFnDepth_;
        curFn_ = fn;
        curFnDepth_++;

        auto* entryBB = llvm::BasicBlock::Create(ctx_, "entry", fn);
        builder_.SetInsertPoint(entryBB);
        env_.push_back({{}, curFnDepth_});

        // Scan for upvalues FIRST, before binding formals.
        // This ensures that if a formal is captured by an inner function,
        // we know to write it to the global rather than an alloca.
        {
            std::unordered_set<std::string> myLocals;
            for (auto& fp : formals->children)
                myLocals.insert(fp->sval);
            collectLocals(body, myLocals);
            scanUpvalues(body, myLocals);
        }

        // Bind formals: if the name was promoted to a global (captured),
        // store the argument directly into the global.
        // Otherwise use a local alloca slot.
        auto it = fn->arg_begin();
        for (auto& fp : formals->children) {
            llvm::Value* arg = &*it++;
            arg->setName(fp->sval);
            auto git = globals_.find(fp->sval);
            if (git != globals_.end()) {
                // Upvalue: write arg directly to global, no alloca
                builder_.CreateStore(arg, git->second);
            } else {
                auto* slot = builder_.CreateAlloca(avPtrTy_, nullptr, fp->sval + ".s");
                builder_.CreateStore(arg, slot);
                env_.back().slots[fp->sval] = slot;
            }
        }

        // Pre-register all functions in this body scope for mutual recursion
        preRegisterFunctions(body->children);

        for (auto& stmt : body->children)
            genStmt(stmt.get());

        if (!builder_.GetInsertBlock()->getTerminator())
            builder_.CreateRet(avNil());

        env_.pop_back();
        curFn_      = outerFn;
        curFnDepth_ = outerDepth;
        if (outerBB) builder_.SetInsertPoint(outerBB);
        return fn;
    }

    // ═════════════════════════════════════════════════════════
    // AlphaVal constructors — heap-allocated via runtime calls
    // ═════════════════════════════════════════════════════════
    llvm::Value* avNil() {
        return builder_.CreateCall(rtMakeNil_, {}, "nil");
    }
    llvm::Value* avNum(double d) {
        return builder_.CreateCall(rtMakeNum_,
            {llvm::ConstantFP::get(dblt(), d)}, "num");
    }
    llvm::Value* avNum(llvm::Value* dblVal) {
        return builder_.CreateCall(rtMakeNum_, {dblVal}, "num");
    }
    llvm::Value* avStr(const std::string& s) {
        // LLVM 22: CreateGlobalString returns a pointer to the string data
        llvm::Value* ptr = builder_.CreateGlobalString(s, ".str");
        return builder_.CreateCall(rtMakeStr_, {ptr}, "strv");
    }
    llvm::Value* avBool(bool b) {
        return builder_.CreateCall(rtMakeBool_,
            {llvm::ConstantInt::get(i32t(), b ? 1 : 0)}, "bv");
    }
    llvm::Value* avBool(llvm::Value* i1val) {
        llvm::Value* ext = builder_.CreateZExt(i1val, i32t(), "b2i32");
        return builder_.CreateCall(rtMakeBool_, {ext}, "bv");
    }
    llvm::Value* avFunc(llvm::Function* fn) {
        llvm::Value* fptr = builder_.CreatePtrToInt(fn, i64t(), "fn2i64");
        return builder_.CreateCall(rtMakeFunc_, {fptr}, "fv");
    }

    // ═════════════════════════════════════════════════════════
    // Extractors — go through runtime helpers
    // ═════════════════════════════════════════════════════════
    llvm::Value* numBits(llvm::Value* av) {
        return builder_.CreateCall(rtGetNum_, {av}, "num");
    }
    llvm::Value* toBool(llvm::Value* av) {
        llvm::Value* i32val = builder_.CreateCall(rtIsTruthy_, {av}, "truthy");
        return builder_.CreateICmpNE(i32val, ci32(0), "tobool");
    }

    // ═════════════════════════════════════════════════════════
    // Variable environment
    // ═════════════════════════════════════════════════════════
    llvm::GlobalVariable* getOrCreateGlobal(const std::string& name) {
        auto it = globals_.find(name);
        if (it != globals_.end()) return it->second;
        auto* gv = new llvm::GlobalVariable(
            *mod_, avPtrTy_, false,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantPointerNull::get(avPtrTy_),
            "g_" + name);
        globals_[name] = gv;
        return gv;
    }

    // ── Upvalue scanning ──────────────────────────────────────
    // Before compiling a function body, scan the AST to find all
    // variable names that are referenced but not defined locally.
    // These are "upvalues" — captured from outer scopes.
    // We promote them to globals BEFORE compiling the outer function,
    // so that the outer function writes to the global from the start.

    void collectNames(ASTNode* n, std::unordered_set<std::string>& names) {
        if (!n) return;
        if (n->kind == NodeKind::IdExpr) { names.insert(n->sval); return; }
        // Don't descend into nested functions (they have their own scope)
        if (n->kind == NodeKind::FuncDef || n->kind == NodeKind::LambdaDef) return;
        for (auto& c : n->children) collectNames(c.get(), names);
    }

    void collectLocals(ASTNode* n, std::unordered_set<std::string>& locals) {
        if (!n) return;
        // Don't descend into nested functions
        if (n->kind == NodeKind::FuncDef || n->kind == NodeKind::LambdaDef) return;
        // explicit local declaration
        if (n->kind == NodeKind::LocalDecl) { locals.insert(n->sval); }
        // local x lvalue
        if (n->kind == NodeKind::IdExpr && n->isLocal) { locals.insert(n->sval); }
        // any assignment target that is a bare IdExpr is a local of this scope
        if (n->kind == NodeKind::AssignExpr && !n->children.empty()) {
            auto* lhs = n->children[0].get();
            if (lhs && lhs->kind == NodeKind::IdExpr
                    && !lhs->isLocal && !lhs->isGlobal)
                locals.insert(lhs->sval);
        }
        for (auto& c : n->children) collectLocals(c.get(), locals);
    }

    // Find all names used in inner functions that are defined in outer scope.
    // These must be promoted to globals so both levels share the same storage.
    void scanUpvalues(ASTNode* body, const std::unordered_set<std::string>& outerLocals) {
        if (!body) return;
        for (auto& child : body->children)
            scanUpvaluesNode(child.get(), outerLocals);
    }

    void scanUpvaluesNode(ASTNode* n, const std::unordered_set<std::string>& outerLocals) {
        if (!n) return;
        if (n->kind == NodeKind::FuncDef || n->kind == NodeKind::LambdaDef) {
            // This is an inner function — find names it uses from outer scope
            std::unordered_set<std::string> innerNames;
            ASTNode* innerBody = n->children.back().get();
            collectNames(innerBody, innerNames);
            // Collect inner function's own formals and locals
            std::unordered_set<std::string> innerLocals;
            if (!n->children.empty())
                for (auto& fp : n->children[0]->children)
                    innerLocals.insert(fp->sval);
            collectLocals(innerBody, innerLocals);
            // Any name used in inner that is defined in outer → upvalue
            for (auto& nm : innerNames) {
                if (outerLocals.count(nm) && !innerLocals.count(nm))
                    getOrCreateGlobal(nm);  // pre-promote to global
            }
            return;
        }
        for (auto& c : n->children) scanUpvaluesNode(c.get(), outerLocals);
    }

    llvm::Value* loadVar(const std::string& name) {
        // Check globals first (upvalue-promoted or previously captured)
        auto git = globals_.find(name);
        if (git != globals_.end())
            return builder_.CreateLoad(avPtrTy_, git->second, name);

        for (int i = (int)env_.size()-1; i >= 0; i--) {
            auto sit = env_[i].slots.find(name);
            if (sit == env_[i].slots.end()) continue;
            if (env_[i].fnDepth == curFnDepth_) {
                return builder_.CreateLoad(avPtrTy_, sit->second, name);
            } else {
                // Unexpected cross-function ref not caught by upvalue scan
                // Promote now (fallback)
                llvm::GlobalVariable* gv = getOrCreateGlobal(name);
                env_[i].slots.erase(sit);
                return builder_.CreateLoad(avPtrTy_, gv, name);
            }
        }
        // Auto-declare in current frame
        auto* slot = builder_.CreateAlloca(avPtrTy_, nullptr, name + ".s");
        auto* nil  = avNil();
        builder_.CreateStore(nil, slot);
        env_.back().slots[name] = slot;
        return nil;
    }

    void storeVar(const std::string& name, llvm::Value* val) {
        // Check globals first
        auto git = globals_.find(name);
        if (git != globals_.end()) {
            builder_.CreateStore(val, git->second); return;
        }
        for (int i = (int)env_.size()-1; i >= 0; i--) {
            auto sit = env_[i].slots.find(name);
            if (sit == env_[i].slots.end()) continue;
            if (env_[i].fnDepth == curFnDepth_) {
                builder_.CreateStore(val, sit->second); return;
            } else {
                llvm::GlobalVariable* gv = getOrCreateGlobal(name);
                env_[i].slots.erase(sit);
                builder_.CreateStore(val, gv); return;
            }
        }
        auto* slot = builder_.CreateAlloca(avPtrTy_, nullptr, name + ".s");
        builder_.CreateStore(val, slot);
        env_.back().slots[name] = slot;
    }

    // ═════════════════════════════════════════════════════════
    // BB helpers
    // ═════════════════════════════════════════════════════════
    llvm::BasicBlock* newBB(const char* nm) {
        return llvm::BasicBlock::Create(ctx_, nm, curFn_);
    }
    void setBB(llvm::BasicBlock* bb) { builder_.SetInsertPoint(bb); }
    void brIfOpen(llvm::BasicBlock* dest) {
        if (!builder_.GetInsertBlock()->getTerminator())
            builder_.CreateBr(dest);
    }

    // ═════════════════════════════════════════════════════════
    // Output & optimization
    // ═════════════════════════════════════════════════════════
    void dumpIR(llvm::raw_ostream& os) { mod_->print(os, nullptr); }

    bool verify() {
        std::string err;
        llvm::raw_string_ostream es(err);
        bool bad = llvm::verifyModule(*mod_, &es);
        if (bad) fprintf(stderr, "LLVM verify errors:\n%s\n", err.c_str());
        return !bad;
    }

    void optimize(int level = 1) {
        using namespace llvm;
        PassBuilder pb;
        LoopAnalysisManager     lam;
        FunctionAnalysisManager fam;
        CGSCCAnalysisManager    cgam;
        ModuleAnalysisManager   mam;
        pb.registerModuleAnalyses(mam);
        pb.registerCGSCCAnalyses(cgam);
        pb.registerFunctionAnalyses(fam);
        pb.registerLoopAnalyses(lam);
        pb.crossRegisterProxies(lam, fam, cgam, mam);
        OptimizationLevel ol = (level == 0) ? OptimizationLevel::O0
                             : (level == 1) ? OptimizationLevel::O1
                                            : OptimizationLevel::O2;
        ModulePassManager mpm = pb.buildPerModuleDefaultPipeline(ol);
        mpm.run(*mod_, mam);
    }
}; // end class LLVMCodeGen
