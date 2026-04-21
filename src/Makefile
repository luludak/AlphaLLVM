# ============================================================
# Alpha Compiler — GNU Makefile
# Requires: flex, bison, clang++ (or g++), llvm-config, clang
# ============================================================

CXX   := clang++
CC    := clang
FLEX  := flex
BISON := bison

LLVM_CXXFLAGS := $(shell llvm-config --cxxflags 2>/dev/null)
LLVM_LDFLAGS  := $(shell llvm-config --ldflags  2>/dev/null)

# Component set — includes OrcJIT for --run mode
LLVM_LIBS := $(shell llvm-config --libs \
    core support irreader target targetparser \
    mc mcparser asmprinter \
    passes transformutils scalaropts instcombine analysis \
    orcjit orctargetprocess orcshared \
    executionengine runtimedyld jitlink \
    native 2>/dev/null \
    || llvm-config --libs all 2>/dev/null \
    || echo "-lLLVM")

LLVM_SYSLIBS := $(shell llvm-config --system-libs 2>/dev/null || echo "-lm -ldl -lz")

CXXFLAGS := -std=c++17 -O2 -Isrc -I. $(LLVM_CXXFLAGS)
CFLAGS   := -std=c11 -O2 -Isrc

# -rdynamic: export all symbols from alphac so OrcJIT's
# DynamicLibrarySearchGenerator can find alpha_rt_* in-process.
LDFLAGS  := $(LLVM_LDFLAGS) -rdynamic

all: alphac libalpharuntime.a

# ── Lex / Bison ──────────────────────────────────────────────
lex.yy.cpp: src/lexer.l parser.tab.h
	$(FLEX) --outfile=lex.yy.cpp src/lexer.l

parser.tab.cpp parser.tab.h: src/parser.y
	$(BISON) -d -o parser.tab.cpp src/parser.y

# ── Compile ───────────────────────────────────────────────────
src/symtable.o: src/symtable.cpp src/symtable.h src/ast.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

src/semantic.o: src/semantic.cpp src/semantic.h src/symtable.h src/ast.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

src/codegen.o: src/codegen.cpp src/codegen.h src/ast.h src/symtable.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

src/main.o: src/main.cpp src/codegen.h src/semantic.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

lex.yy.o: lex.yy.cpp
	$(CXX) $(CXXFLAGS) -w -c $< -o $@

parser.tab.o: parser.tab.cpp
	$(CXX) $(CXXFLAGS) -w -c $< -o $@

src/alpha_runtime.o: src/alpha_runtime.c
	$(CC) $(CFLAGS) -c $< -o $@

# ── Link alphac ──────────────────────────────────────────────
# alpha_runtime.o is linked directly into alphac (not just as a
# separate .a) so that OrcJIT's in-process symbol lookup works.
alphac: src/main.o src/symtable.o src/semantic.o src/codegen.o lex.yy.o parser.tab.o src/alpha_runtime.o
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) $(LLVM_LIBS) $(LLVM_SYSLIBS) -o $@

# ── Runtime static lib (for external use / old-style linking) ─
libalpharuntime.a: src/alpha_runtime.o
	ar rcs $@ $^

# ── --run mode (JIT, no files written) ───────────────────────
# Usage: make jit SRC=myprogram.alpha
jit: alphac
	./alphac --run $(SRC)

# ── Old-style AOT pipeline ────────────────────────────────────
# Usage: make aot SRC=myprogram.alpha
aot: alphac libalpharuntime.a
	./alphac --emit-llvm $(SRC)
	llc -filetype=obj -o $(SRC:.alpha=.o) $(SRC:.alpha=.ll)
	$(CC) $(SRC:.alpha=.o) libalpharuntime.a -lm -o $(SRC:.alpha=)
	./$(SRC:.alpha=)

clean:
	rm -f alphac lex.yy.cpp lex.yy.h parser.tab.cpp parser.tab.h \
	      *.o src/*.o *.ll *.s libalpharuntime.a

.PHONY: all clean jit aot
