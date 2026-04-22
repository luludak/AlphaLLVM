#include "ast.h"
#include "semantic.h"
#include "codegen.h"

// llvm-c/DisassemblerTypes.h must come before TargetRegistry.h in LLVM 22
// to provide LLVMOpInfoCallback / LLVMSymbolLookupCallback typedefs.
#include <llvm-c/DisassemblerTypes.h>

// ── LLVM emit / target ─────────────────────────────────────────
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/Analysis/CGSCCPassManager.h>

// ── OrcJIT ────────────────────────────────────────────────────
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/JITSymbol.h>
#include <llvm/Support/Error.h>

#include <cstdio>
#include <cstring>
#include <string>

// ── Parser interface ──────────────────────────────────────────
extern ASTNodePtr g_root;
extern int yyparse();
extern FILE* yyin;

// ── Alpha runtime symbols (compiled into alphac when --run is used) ──
// Declared so the linker keeps them in the alphac binary and OrcJIT
// can find them via in-process symbol lookup.
extern "C" {
    void      alpha_rt_print(void*);
    void*     alpha_rt_input();
    void*     alpha_rt_table_new();
    void*     alpha_rt_table_get(void*, void*);
    void      alpha_rt_table_set(void*, void*, void*);
    void*     alpha_rt_concat(void*, void*);
    void*     alpha_rt_typeof(void*);
    void*     alpha_rt_strtonum(void*);
    void*     alpha_rt_tostring(void*);
    void*     alpha_rt_sqrt(void*);
    void*     alpha_rt_cos(void*);
    void*     alpha_rt_sin(void*);
    void*     alpha_rt_floor(void*);
    void*     alpha_rt_ceil(void*);
    void*     alpha_rt_abs(void*);
    void*     alpha_rt_max(void*, void*);
    void*     alpha_rt_min(void*, void*);
    void*     alpha_rt_pow(void*, void*);
    void*     alpha_rt_strlen(void*);
    void*     alpha_rt_strchar(void*, void*);
    int       alpha_rt_val_eq(void*, void*);
    int       alpha_rt_val_neq(void*, void*);
    void*     alpha_rt_objectmemberkeys(void*);
    void*     alpha_rt_objecttotalmembers(void*);
    void*     alpha_rt_objectcopy(void*);
    // Heap constructors
    void*     alpha_rt_make_nil();
    void*     alpha_rt_make_number(double);
    void*     alpha_rt_make_bool(int);
    void*     alpha_rt_make_string(const char*);
    void*     alpha_rt_make_func(long long);
    // Extractors
    double    alpha_rt_get_number(void*);
    int       alpha_rt_is_truthy(void*);
    long long alpha_rt_get_funcptr(void*);
}

// ─────────────────────────────────────────────────────────────
static void usage(const char* prog) {
    fprintf(stderr,
        "Usage: %s [options] <input.alpha>\n"
        "\n"
        "Modes (pick one):\n"
        "  --run            Compile and execute immediately via OrcJIT [no files written]\n"
        "  --emit-llvm      Write LLVM IR to <input>.ll  [default]\n"
        "  --emit-obj       Write native object file to <input>.o\n"
        "\n"
        "Options:\n"
        "  --dump-ast       Dump AST to stdout\n"
        "  --dump-symbols   Dump symbol table to stdout\n"
        "  -O0 / -O1 / -O2  Optimization level (default: -O1)\n"
        "  -o <file>        Output filename (not used with --run)\n",
        prog);
}

// ── AST dump helper ───────────────────────────────────────────
static void dumpASTNode(const ASTNode* n, int depth) {
    if (!n) { printf("%*s<null>\n", depth*2, ""); return; }
    static const char* kinds[] = {
        "Program","Block","IfStmt","WhileStmt","ForStmt","ReturnStmt",
        "BreakStmt","ContinueStmt","ExprStmt","FuncDef","LambdaDef",
        "AssignExpr","BinOp","UnaryOp","PostfixOp",
        "CallExpr","IndexExpr","MemberExpr",
        "IdExpr","IntLit","FloatLit","StrLit","BoolLit","NilLit",
        "TableConstructor","TableElem","TableIndexElem",
        "LocalDecl","FormalParam","ArgList","ElemList"
    };
    int ki = (int)n->kind;
    const char* kname = (ki >= 0 && ki < (int)(sizeof(kinds)/sizeof(*kinds))) ? kinds[ki] : "?";
    printf("%*s[%s", depth*2, "", kname);
    if (!n->sval.empty())           printf(" '%s'",  n->sval.c_str());
    if (n->kind == NodeKind::IntLit)   printf(" %d",  n->ival);
    if (n->kind == NodeKind::FloatLit) printf(" %g",  n->fval);
    if (n->kind == NodeKind::BoolLit)  printf(" %s",  n->bval ? "true" : "false");
    if (n->line)                    printf(" @%d", n->line);
    printf("]\n");
    for (auto& c : n->children) dumpASTNode(c.get(), depth+1);
}

// ── OrcJIT execution ──────────────────────────────────────────
//
// How it works:
//   1. Wrap the LLVM Module in a ThreadSafeModule (OrcJIT requires this).
//   2. Build an LLJIT instance targeting the current process's CPU.
//   3. Add a DynamicLibrarySearchGenerator so OrcJIT can resolve
//      alpha_rt_* symbols by looking them up in the current process
//      (alphac is linked with alpha_runtime.a, so all symbols are in-process).
//   4. Add the module to the JIT's main JITDylib.
//   5. Look up "main" → get a function pointer → call it.
//
// No files are written. No external processes are spawned.
// Compilation to machine code happens inside this process via MCJIT.
//
static int runWithOrcJIT(LLVMCodeGen& cg, int /*optLevel*/) {

    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    // ── Build LLJIT ──────────────────────────────────────────
    auto jitExpected = llvm::orc::LLJITBuilder().create();
    if (!jitExpected) {
        llvm::errs() << "Failed to create LLJIT: "
                     << llvm::toString(jitExpected.takeError()) << "\n";
        return 1;
    }
    auto& jit = *jitExpected;
    auto& mainDylib = jit->getMainJITDylib();

    // ── Explicitly register every alpha_rt_* symbol ──────────
    // DynamicLibrarySearchGenerator is unreliable on macOS because
    // the linker strips unexported symbols from the binary.
    // We register each function's address directly — this always works.
    auto& es  = jit->getExecutionSession();
    auto  mangle = llvm::orc::MangleAndInterner(es, jit->getDataLayout());

    llvm::orc::SymbolMap rtSymbols;

    #define REG_SYM(name) \
        rtSymbols[mangle(#name)] = { \
            llvm::orc::ExecutorAddr::fromPtr((void*)&name), \
            llvm::JITSymbolFlags::Exported };

    REG_SYM(alpha_rt_print)
    REG_SYM(alpha_rt_input)
    REG_SYM(alpha_rt_table_new)
    REG_SYM(alpha_rt_table_get)
    REG_SYM(alpha_rt_table_set)
    REG_SYM(alpha_rt_concat)
    REG_SYM(alpha_rt_typeof)
    REG_SYM(alpha_rt_strtonum)
    REG_SYM(alpha_rt_tostring)
    REG_SYM(alpha_rt_sqrt)
    REG_SYM(alpha_rt_cos)
    REG_SYM(alpha_rt_sin)
    REG_SYM(alpha_rt_floor)
    REG_SYM(alpha_rt_ceil)
    REG_SYM(alpha_rt_abs)
    REG_SYM(alpha_rt_max)
    REG_SYM(alpha_rt_min)
    REG_SYM(alpha_rt_pow)
    REG_SYM(alpha_rt_strlen)
    REG_SYM(alpha_rt_strchar)
    REG_SYM(alpha_rt_val_eq)
    REG_SYM(alpha_rt_val_neq)
    REG_SYM(alpha_rt_objectmemberkeys)
    REG_SYM(alpha_rt_objecttotalmembers)
    REG_SYM(alpha_rt_objectcopy)
    REG_SYM(alpha_rt_make_nil)
    REG_SYM(alpha_rt_make_number)
    REG_SYM(alpha_rt_make_bool)
    REG_SYM(alpha_rt_make_string)
    REG_SYM(alpha_rt_make_func)
    REG_SYM(alpha_rt_get_number)
    REG_SYM(alpha_rt_is_truthy)
    REG_SYM(alpha_rt_get_funcptr)

    #undef REG_SYM

    if (auto err = mainDylib.define(
            llvm::orc::absoluteSymbols(std::move(rtSymbols)))) {
        llvm::errs() << "Symbol registration failed: "
                     << llvm::toString(std::move(err)) << "\n";
        return 1;
    }

    // ── Add IR module ────────────────────────────────────────
    auto tsm = llvm::orc::ThreadSafeModule(
                   std::move(cg.mod_),
                   std::make_unique<llvm::LLVMContext>());

    if (auto err = jit->addIRModule(std::move(tsm))) {
        llvm::errs() << "addIRModule failed: "
                     << llvm::toString(std::move(err)) << "\n";
        return 1;
    }

    // ── Look up and call main ────────────────────────────────
    auto mainSym = jit->lookup("main");
    if (!mainSym) {
        llvm::errs() << "Cannot find 'main' symbol: "
                     << llvm::toString(mainSym.takeError()) << "\n";
        return 1;
    }

    using MainFn = int (*)();
    auto* fn = mainSym->toPtr<MainFn>();

    printf("[JIT] Running...\n");
    printf("────────────────────────────────\n");
    int exitCode = fn();
    printf("────────────────────────────────\n");
    printf("[JIT] Exited with code %d\n", exitCode);

    return exitCode;
}

// ── main ──────────────────────────────────────────────────────
int main(int argc, char** argv) {
    bool dumpAST    = false;
    bool dumpSyms   = false;
    bool doRun      = false;
    bool emitLLVM   = false;
    bool emitObj    = false;
    int  optLevel   = 1;
    std::string outFile;
    std::string inFile;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--run"))          doRun     = true;
        else if (!strcmp(argv[i], "--dump-ast"))     dumpAST   = true;
        else if (!strcmp(argv[i], "--dump-symbols")) dumpSyms  = true;
        else if (!strcmp(argv[i], "--emit-llvm"))    emitLLVM  = true;
        else if (!strcmp(argv[i], "--emit-obj"))     emitObj   = true;
        else if (!strcmp(argv[i], "-O0"))            optLevel  = 0;
        else if (!strcmp(argv[i], "-O1"))            optLevel  = 1;
        else if (!strcmp(argv[i], "-O2"))            optLevel  = 2;
        else if (!strcmp(argv[i], "-o") && i+1 < argc) outFile = argv[++i];
        else if (argv[i][0] != '-')                  inFile   = argv[i];
        else { usage(argv[0]); return 1; }
    }

    if (inFile.empty()) { usage(argv[0]); return 1; }

    // ── Phase 1+2: Lex + Parse ───────────────────────────────
    FILE* f = fopen(inFile.c_str(), "r");
    if (!f) { perror(inFile.c_str()); return 1; }
    yyin = f;
    if (yyparse() != 0) {
        fclose(f);
        fprintf(stderr, "Compilation failed: parse error.\n");
        return 1;
    }
    fclose(f);
    printf("[1/4] Parsed.\n");

    if (dumpAST) {
        printf("\n=== AST ===\n");
        dumpASTNode(g_root.get(), 0);
        printf("===========\n\n");
    }

    // ── Phase 3: Semantic analysis ───────────────────────────
    SemanticAnalyzer sema;
    if (!sema.analyze(g_root.get())) {
        fprintf(stderr, "Compilation failed: semantic errors.\n");
        return 1;
    }
    printf("[2/4] Semantic analysis OK.\n");
    if (dumpSyms) sema.dumpSymbols();

    // ── Phase 4: LLVM IR codegen ─────────────────────────────
    LLVMCodeGen cg;
    cg.generate(g_root.get(), sema.symtab);
    printf("[3/4] LLVM IR generated.\n");

    if (!cg.verify()) {
        fprintf(stderr, "Compilation failed: LLVM IR verification.\n");
        return 1;
    }

    // ── Optimization ─────────────────────────────────────────
    if (optLevel > 0) {
        cg.optimize(optLevel);
        printf("[opt] -O%d complete.\n", optLevel);
        if (!cg.verify()) {
            fprintf(stderr, "Post-optimization verification failed.\n");
            return 1;
        }
    }
    printf("[4/4] Ready.\n\n");

    // ══════════════════════════════════════════════════════════
    // --run: JIT compile + execute in-process, no files written
    // ══════════════════════════════════════════════════════════
    if (doRun) {
        return runWithOrcJIT(cg, optLevel);
    }

    // ── Emit mode (default: LLVM IR) ─────────────────────────
    if (!emitLLVM && !emitObj) emitLLVM = true;

    if (outFile.empty()) {
        outFile = inFile;
        size_t dot = outFile.rfind('.');
        if (dot != std::string::npos) outFile = outFile.substr(0, dot);
        outFile += emitObj ? ".o" : ".ll";
    }

    if (emitLLVM) {
        std::string llFile = emitObj
            ? outFile.substr(0, outFile.rfind('.')) + ".ll"
            : outFile;
        std::error_code ec;
        llvm::raw_fd_ostream os(llFile, ec, llvm::sys::fs::OF_None);
        if (ec) {
            fprintf(stderr, "Cannot open %s: %s\n",
                    llFile.c_str(), ec.message().c_str());
            return 1;
        }
        cg.dumpIR(os);
        printf("[out] LLVM IR -> %s\n", llFile.c_str());
    }

    if (emitObj) {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmParser();
        llvm::InitializeNativeTargetAsmPrinter();

        llvm::Triple triple(llvm::sys::getDefaultTargetTriple());
        cg.mod_->setTargetTriple(triple);

        std::string err;
        auto* target = llvm::TargetRegistry::lookupTarget(triple, err);
        if (!target) {
            fprintf(stderr, "Target lookup failed: %s\n", err.c_str());
            return 1;
        }
        auto* tm = target->createTargetMachine(
            triple, "generic", "",
            llvm::TargetOptions{},
            std::optional<llvm::Reloc::Model>());
        cg.mod_->setDataLayout(tm->createDataLayout());

        std::error_code ec;
        llvm::raw_fd_ostream dest(outFile, ec, llvm::sys::fs::OF_None);
        if (ec) {
            fprintf(stderr, "Cannot open %s: %s\n",
                    outFile.c_str(), ec.message().c_str());
            return 1;
        }
        llvm::legacy::PassManager pm;
        if (tm->addPassesToEmitFile(pm, dest, nullptr,
                llvm::CodeGenFileType::ObjectFile)) {
            fprintf(stderr, "Target cannot emit object file.\n");
            return 1;
        }
        pm.run(*cg.mod_);
        dest.flush();
        printf("[out] Object -> %s\n", outFile.c_str());
    }

    return 0;
}
