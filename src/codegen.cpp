#include "codegen.h"
#include <cstdio>
#include <cstring>

// ── Constructor ───────────────────────────────────────────────

LLVMCodeGen::LLVMCodeGen()
    : mod_(std::make_unique<llvm::Module>("alpha_module", ctx_))
    , builder_(ctx_)
{
    buildTypes();
    declareRT();
}

// ── Type helpers ──────────────────────────────────────────────

void LLVMCodeGen::buildTypes() {
    avTy_    = llvm::StructType::create(ctx_, {i32t(), i64t()}, "AlphaVal");
    avPtrTy_ = llvm::PointerType::get(ctx_, 0);
}

llvm::Type* LLVMCodeGen::i1t()   { return llvm::Type::getInt1Ty(ctx_);  }
llvm::Type* LLVMCodeGen::i32t()  { return llvm::Type::getInt32Ty(ctx_); }
llvm::Type* LLVMCodeGen::i64t()  { return llvm::Type::getInt64Ty(ctx_); }
llvm::Type* LLVMCodeGen::dblt()  { return llvm::Type::getDoubleTy(ctx_); }
llvm::Type* LLVMCodeGen::voidt() { return llvm::Type::getVoidTy(ctx_);  }
llvm::Type* LLVMCodeGen::i8pt()  { return llvm::PointerType::get(ctx_, 0); }

llvm::Constant* LLVMCodeGen::ci32(int v)     { return llvm::ConstantInt::get(i32t(), v); }
llvm::Constant* LLVMCodeGen::ci64(int64_t v) { return llvm::ConstantInt::get(i64t(), v); }
llvm::Constant* LLVMCodeGen::cfp(double d)   { return llvm::ConstantFP::get(dblt(), d); }

// ── Runtime declaration ───────────────────────────────────────

llvm::Function* LLVMCodeGen::mkRT(const char* nm, llvm::Type* ret,
                                   std::vector<llvm::Type*> ps, bool va) {
    auto* fty = llvm::FunctionType::get(ret, ps, va);
    return llvm::Function::Create(fty, llvm::Function::ExternalLinkage,
                                  nm, mod_.get());
}

void LLVMCodeGen::declareRT() {
    auto* avp = avPtrTy_;
    auto* vt  = voidt();
    auto* p   = i8pt();
    rtPrint_    = mkRT("alpha_rt_print",              vt,    {avp});
    rtInput_    = mkRT("alpha_rt_input",              avp,   {});
    rtTblNew_   = mkRT("alpha_rt_table_new",          avp,   {});
    rtTblGet_   = mkRT("alpha_rt_table_get",          avp,   {avp, avp});
    rtTblSet_   = mkRT("alpha_rt_table_set",          vt,    {avp, avp, avp});
    rtConcat_   = mkRT("alpha_rt_concat",             avp,   {avp, avp});
    rtTypeof_   = mkRT("alpha_rt_typeof",             avp,   {avp});
    rtTonum_    = mkRT("alpha_rt_strtonum",           avp,   {avp});
    rtSqrt_     = mkRT("alpha_rt_sqrt",               avp,   {avp});
    rtCos_      = mkRT("alpha_rt_cos",                avp,   {avp});
    rtSin_      = mkRT("alpha_rt_sin",                avp,   {avp});
    rtValEq_    = mkRT("alpha_rt_val_eq",             i1t(), {avp, avp});
    rtValNeq_   = mkRT("alpha_rt_val_neq",            i1t(), {avp, avp});
    rtObjKeys_  = mkRT("alpha_rt_objectmemberkeys",   avp,   {avp});
    rtObjTotal_ = mkRT("alpha_rt_objecttotalmembers", avp,   {avp});
    rtObjCopy_  = mkRT("alpha_rt_objectcopy",         avp,   {avp});
    rtToString_ = mkRT("alpha_rt_tostring",           avp,   {avp});
    rtFloor_    = mkRT("alpha_rt_floor",              avp,   {avp});
    rtCeil_     = mkRT("alpha_rt_ceil",               avp,   {avp});
    rtAbs_      = mkRT("alpha_rt_abs",                avp,   {avp});
    rtMax_      = mkRT("alpha_rt_max",                avp,   {avp, avp});
    rtMin_      = mkRT("alpha_rt_min",                avp,   {avp, avp});
    rtMakeNil_  = mkRT("alpha_rt_make_nil",           avp,   {});
    rtMakeNum_  = mkRT("alpha_rt_make_number",        avp,   {dblt()});
    rtMakeBool_ = mkRT("alpha_rt_make_bool",          avp,   {i32t()});
    rtMakeStr_  = mkRT("alpha_rt_make_string",        avp,   {p});
    rtMakeFunc_ = mkRT("alpha_rt_make_func",          avp,   {i64t()});
    rtGetNum_   = mkRT("alpha_rt_get_number",         dblt(), {avp});
    rtIsTruthy_ = mkRT("alpha_rt_is_truthy",          i32t(), {avp});
    rtGetFnPtr_ = mkRT("alpha_rt_get_funcptr",        i64t(), {avp});
}

// ── Top-level entry ───────────────────────────────────────────

void LLVMCodeGen::generate(ASTNode* root, SymbolTable& /*st*/) {
    auto* mainTy = llvm::FunctionType::get(i32t(), {}, false);
    curFn_ = llvm::Function::Create(mainTy, llvm::Function::ExternalLinkage,
                                    "main", mod_.get());
    auto* entry = llvm::BasicBlock::Create(ctx_, "entry", curFn_);
    builder_.SetInsertPoint(entry);
    env_.push_back({{}, 0});

    ASTNode* block = root->children[0].get();
    {
        std::unordered_set<std::string> topLocals;
        collectLocals(block, topLocals);
        scanUpvalues(block, topLocals);
    }
    preRegisterFunctions(block->children);

    for (auto& stmt : block->children)
        genStmt(stmt.get());

    if (!builder_.GetInsertBlock()->getTerminator())
        builder_.CreateRet(ci32(0));
    env_.pop_back();
}

void LLVMCodeGen::preRegisterFunctions(const std::vector<ASTNodePtr>& stmts) {
    for (auto& stmt : stmts) {
        if (!stmt) continue;
        ASTNode* n = stmt.get();
        if (n->kind == NodeKind::ExprStmt && !n->children.empty())
            n = n->children[0].get();
        if (n && n->kind == NodeKind::FuncDef) {
            size_t np = n->children[0]->children.size();
            std::vector<llvm::Type*> pts(np, avPtrTy_);
            auto* fty = llvm::FunctionType::get(avPtrTy_, pts, false);
            auto* fn  = llvm::Function::Create(
                fty, llvm::Function::InternalLinkage, n->sval, mod_.get());
            funcRegistry_[n->sval] = fn;
        }
        if (n && n->kind == NodeKind::Block)
            preRegisterFunctions(n->children);
    }
}

// ── Statements ────────────────────────────────────────────────

void LLVMCodeGen::genStmt(ASTNode* n) {
    if (!n) return;
    if (builder_.GetInsertBlock()->getTerminator()) return;
    switch (n->kind) {
        case NodeKind::ExprStmt:    genExpr(n->children[0].get()); break;
        case NodeKind::Block:       genBlock(n);       break;
        case NodeKind::IfStmt:      genIf(n);          break;
        case NodeKind::WhileStmt:   genWhile(n);       break;
        case NodeKind::ForStmt:     genFor(n);         break;
        case NodeKind::ReturnStmt:  genReturn(n);      break;
        case NodeKind::BreakStmt:
            if (!breakStack_.empty()) builder_.CreateBr(breakStack_.back());
            break;
        case NodeKind::ContinueStmt:
            if (!contStack_.empty())  builder_.CreateBr(contStack_.back());
            break;
        case NodeKind::FuncDef:     genFuncDefStmt(n); break;
        case NodeKind::LocalDecl:   genLocalDecl(n);   break;
        default:                    genExpr(n);        break;
    }
}

void LLVMCodeGen::genBlock(ASTNode* n) {
    env_.push_back({{}, curFnDepth_});
    for (auto& c : n->children) genStmt(c.get());
    env_.pop_back();
}

void LLVMCodeGen::genIf(ASTNode* n) {
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

void LLVMCodeGen::genWhile(ASTNode* n) {
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

void LLVMCodeGen::genFor(ASTNode* n) {
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

void LLVMCodeGen::genReturn(ASTNode* n) {
    if (!n->children.empty())
        builder_.CreateRet(genExpr(n->children[0].get()));
    else
        builder_.CreateRet(avNil());
}

void LLVMCodeGen::genFuncDefStmt(ASTNode* n) {
    llvm::Function* fn = buildFunc(n->sval, n->children[0].get(),
                                    n->children[1].get());
    storeVar(n->sval, avFunc(fn));
}

void LLVMCodeGen::genLocalDecl(ASTNode* n) {
    llvm::Value* initVal = n->children.empty()
                         ? avNil()
                         : genExpr(n->children[0].get());
    auto git = globals_.find(n->sval);
    if (git != globals_.end()) {
        builder_.CreateStore(initVal, git->second);
        return;
    }
    auto* slot = builder_.CreateAlloca(avPtrTy_, nullptr, n->sval + ".s");
    builder_.CreateStore(initVal, slot);
    env_.back().slots[n->sval] = slot;
}

// ── Expressions ───────────────────────────────────────────────

llvm::Value* LLVMCodeGen::genExpr(ASTNode* n) {
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

llvm::Value* LLVMCodeGen::genAssign(ASTNode* n) {
    llvm::Value* rhs = genExpr(n->children[1].get());
    writeLval(n->children[0].get(), rhs);
    return rhs;
}

void LLVMCodeGen::writeLval(ASTNode* lhs, llvm::Value* val) {
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

llvm::Value* LLVMCodeGen::genBinOp(ASTNode* n) {
    if (n->binop == BinOpKind::And) return genAnd(n);
    if (n->binop == BinOpKind::Or)  return genOr(n);
    if (n->binop == BinOpKind::Concat)
        return builder_.CreateCall(rtConcat_,
            {genExpr(n->children[0].get()), genExpr(n->children[1].get())});

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

llvm::Value* LLVMCodeGen::genAnd(ASTNode* n) {
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

llvm::Value* LLVMCodeGen::genOr(ASTNode* n) {
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

llvm::Value* LLVMCodeGen::genUnary(ASTNode* n) {
    switch (n->unop) {
        case UnaryOpKind::Minus:
            return avNum(builder_.CreateFNeg(
                numBits(genExpr(n->children[0].get())), "neg"));
        case UnaryOpKind::Not:
            return avBool(builder_.CreateNot(
                toBool(genExpr(n->children[0].get())), "not"));
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

llvm::Value* LLVMCodeGen::genPostfix(ASTNode* n) {
    auto* old = genExpr(n->children[0].get());
    auto* nv  = (n->postop == PostfixOpKind::PostInc)
              ? avNum(builder_.CreateFAdd(numBits(old), cfp(1.0), "pi"))
              : avNum(builder_.CreateFSub(numBits(old), cfp(1.0), "pd"));
    writeLval(n->children[0].get(), nv);
    return old;
}

llvm::Value* LLVMCodeGen::genCall(ASTNode* n) {
    ASTNode* cn = n->children[0].get();
    ASTNode* an = n->children[1].get();
    std::vector<llvm::Value*> args;
    for (auto& a : an->children)
        args.push_back(genExpr(a.get()));

    if (cn->kind == NodeKind::IdExpr) {
        const std::string& nm = cn->sval;
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
        auto fit = funcRegistry_.find(nm);
        if (fit != funcRegistry_.end())
            return builder_.CreateCall(fit->second, args);
    }

    // Indirect call via AlphaVal function pointer
    llvm::Value* fnAV  = genExpr(cn);
    llvm::Value* fnRaw = builder_.CreateCall(rtGetFnPtr_, {fnAV}, "fnraw");
    std::vector<llvm::Type*> paramTys(args.size(), avPtrTy_);
    auto* fnTy  = llvm::FunctionType::get(avPtrTy_, paramTys, false);
    auto* fnPtr = builder_.CreateIntToPtr(fnRaw, avPtrTy_, "fnptr");
    return builder_.CreateCall(fnTy, fnPtr, args);
}

llvm::Value* LLVMCodeGen::genMember(ASTNode* n) {
    return builder_.CreateCall(rtTblGet_,
        {genExpr(n->children[0].get()), avStr(n->sval)});
}

llvm::Value* LLVMCodeGen::genIndex(ASTNode* n) {
    return builder_.CreateCall(rtTblGet_,
        {genExpr(n->children[0].get()), genExpr(n->children[1].get())});
}

llvm::Value* LLVMCodeGen::genTable(ASTNode* n) {
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

// ── Function builder ──────────────────────────────────────────

llvm::Function* LLVMCodeGen::buildFunc(const std::string& name,
                                        ASTNode* formals, ASTNode* body) {
    size_t np = formals->children.size();
    std::vector<llvm::Type*> pts(np, avPtrTy_);
    auto* fty = llvm::FunctionType::get(avPtrTy_, pts, false);

    llvm::Function* fn = nullptr;
    auto fit = funcRegistry_.find(name);
    if (fit != funcRegistry_.end() && fit->second->empty()) {
        fn = fit->second;
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

    // Upvalue scan before binding formals
    {
        std::unordered_set<std::string> myLocals;
        for (auto& fp : formals->children) myLocals.insert(fp->sval);
        collectLocals(body, myLocals);
        scanUpvalues(body, myLocals);
    }

    // Bind formals — store to global if captured, else alloca
    auto it = fn->arg_begin();
    for (auto& fp : formals->children) {
        llvm::Value* arg = &*it++;
        arg->setName(fp->sval);
        auto git = globals_.find(fp->sval);
        if (git != globals_.end()) {
            builder_.CreateStore(arg, git->second);
        } else {
            auto* slot = builder_.CreateAlloca(avPtrTy_, nullptr, fp->sval + ".s");
            builder_.CreateStore(arg, slot);
            env_.back().slots[fp->sval] = slot;
        }
    }

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

// ── AlphaVal constructors (heap via runtime) ──────────────────

llvm::Value* LLVMCodeGen::avNil() {
    return builder_.CreateCall(rtMakeNil_, {}, "nil");
}
llvm::Value* LLVMCodeGen::avNum(double d) {
    return builder_.CreateCall(rtMakeNum_,
        {llvm::ConstantFP::get(dblt(), d)}, "num");
}
llvm::Value* LLVMCodeGen::avNum(llvm::Value* dblVal) {
    return builder_.CreateCall(rtMakeNum_, {dblVal}, "num");
}
llvm::Value* LLVMCodeGen::avStr(const std::string& s) {
    llvm::Value* ptr = builder_.CreateGlobalString(s, ".str");
    return builder_.CreateCall(rtMakeStr_, {ptr}, "strv");
}
llvm::Value* LLVMCodeGen::avBool(bool b) {
    return builder_.CreateCall(rtMakeBool_,
        {llvm::ConstantInt::get(i32t(), b ? 1 : 0)}, "bv");
}
llvm::Value* LLVMCodeGen::avBool(llvm::Value* i1val) {
    llvm::Value* ext = builder_.CreateZExt(i1val, i32t(), "b2i32");
    return builder_.CreateCall(rtMakeBool_, {ext}, "bv");
}
llvm::Value* LLVMCodeGen::avFunc(llvm::Function* fn) {
    llvm::Value* fptr = builder_.CreatePtrToInt(fn, i64t(), "fn2i64");
    return builder_.CreateCall(rtMakeFunc_, {fptr}, "fv");
}

// ── Extractors ────────────────────────────────────────────────

llvm::Value* LLVMCodeGen::numBits(llvm::Value* av) {
    return builder_.CreateCall(rtGetNum_, {av}, "num");
}
llvm::Value* LLVMCodeGen::toBool(llvm::Value* av) {
    llvm::Value* i32val = builder_.CreateCall(rtIsTruthy_, {av}, "truthy");
    return builder_.CreateICmpNE(i32val, ci32(0), "tobool");
}

// ── Variable environment ──────────────────────────────────────

llvm::GlobalVariable* LLVMCodeGen::getOrCreateGlobal(const std::string& name) {
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

void LLVMCodeGen::collectNames(ASTNode* n, std::unordered_set<std::string>& names) {
    if (!n) return;
    if (n->kind == NodeKind::IdExpr) { names.insert(n->sval); return; }
    if (n->kind == NodeKind::FuncDef || n->kind == NodeKind::LambdaDef) return;
    for (auto& c : n->children) collectNames(c.get(), names);
}

void LLVMCodeGen::collectLocals(ASTNode* n, std::unordered_set<std::string>& locals) {
    if (!n) return;
    if (n->kind == NodeKind::FuncDef || n->kind == NodeKind::LambdaDef) return;
    if (n->kind == NodeKind::LocalDecl)
        locals.insert(n->sval);
    if (n->kind == NodeKind::IdExpr && n->isLocal)
        locals.insert(n->sval);
    if (n->kind == NodeKind::AssignExpr && !n->children.empty()) {
        auto* lhs = n->children[0].get();
        if (lhs && lhs->kind == NodeKind::IdExpr && !lhs->isLocal && !lhs->isGlobal)
            locals.insert(lhs->sval);
    }
    for (auto& c : n->children) collectLocals(c.get(), locals);
}

void LLVMCodeGen::scanUpvalues(ASTNode* body,
                                const std::unordered_set<std::string>& outerLocals) {
    if (!body) return;
    for (auto& child : body->children)
        scanUpvaluesNode(child.get(), outerLocals);
}

void LLVMCodeGen::scanUpvaluesNode(ASTNode* n,
                                    const std::unordered_set<std::string>& outerLocals) {
    if (!n) return;
    if (n->kind == NodeKind::FuncDef || n->kind == NodeKind::LambdaDef) {
        std::unordered_set<std::string> innerNames;
        ASTNode* innerBody = n->children.back().get();
        collectNames(innerBody, innerNames);
        std::unordered_set<std::string> innerLocals;
        if (!n->children.empty())
            for (auto& fp : n->children[0]->children)
                innerLocals.insert(fp->sval);
        collectLocals(innerBody, innerLocals);
        for (auto& nm : innerNames)
            if (outerLocals.count(nm) && !innerLocals.count(nm))
                getOrCreateGlobal(nm);
        return;
    }
    for (auto& c : n->children) scanUpvaluesNode(c.get(), outerLocals);
}

llvm::Value* LLVMCodeGen::loadVar(const std::string& name) {
    auto git = globals_.find(name);
    if (git != globals_.end())
        return builder_.CreateLoad(avPtrTy_, git->second, name);

    for (int i = (int)env_.size()-1; i >= 0; i--) {
        auto sit = env_[i].slots.find(name);
        if (sit == env_[i].slots.end()) continue;
        if (env_[i].fnDepth == curFnDepth_)
            return builder_.CreateLoad(avPtrTy_, sit->second, name);
        else {
            llvm::GlobalVariable* gv = getOrCreateGlobal(name);
            env_[i].slots.erase(sit);
            return builder_.CreateLoad(avPtrTy_, gv, name);
        }
    }
    auto* slot = builder_.CreateAlloca(avPtrTy_, nullptr, name + ".s");
    auto* nil  = avNil();
    builder_.CreateStore(nil, slot);
    env_.back().slots[name] = slot;
    return nil;
}

void LLVMCodeGen::storeVar(const std::string& name, llvm::Value* val) {
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

// ── BB helpers ────────────────────────────────────────────────

llvm::BasicBlock* LLVMCodeGen::newBB(const char* nm) {
    return llvm::BasicBlock::Create(ctx_, nm, curFn_);
}
void LLVMCodeGen::setBB(llvm::BasicBlock* bb) { builder_.SetInsertPoint(bb); }
void LLVMCodeGen::brIfOpen(llvm::BasicBlock* dest) {
    if (!builder_.GetInsertBlock()->getTerminator())
        builder_.CreateBr(dest);
}

// ── Output & optimization ─────────────────────────────────────

void LLVMCodeGen::dumpIR(llvm::raw_ostream& os) { mod_->print(os, nullptr); }

bool LLVMCodeGen::verify() {
    std::string err;
    llvm::raw_string_ostream es(err);
    bool bad = llvm::verifyModule(*mod_, &es);
    if (bad) fprintf(stderr, "LLVM verify errors:\n%s\n", err.c_str());
    return !bad;
}

void LLVMCodeGen::optimize(int level) {
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
