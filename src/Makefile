# ============================================================
# Alpha Compiler — fallback GNU Makefile
# Requires: flex, bison, clang++ (or g++), llvm-config
# ============================================================

CXX       := clang++
CC        := clang
FLEX      := flex
BISON     := bison

LLVM_CXXFLAGS := $(shell llvm-config --cxxflags 2>/dev/null || echo "")
LLVM_LDFLAGS  := $(shell llvm-config --ldflags  2>/dev/null || echo "")
LLVM_LIBS     := $(shell llvm-config --libs core support irreader target mc mcparser \
                           x86codegen x86asmparser x86desc x86info 2>/dev/null || \
                           llvm-config --libs all 2>/dev/null || echo "-lLLVM")

CXXFLAGS  := -std=c++17 -O2 -Isrc -I. $(LLVM_CXXFLAGS)
CFLAGS    := -std=c11 -O2 -Isrc

SRCS_CXX  := src/main.cpp parser.tab.cpp lex.yy.cpp
SRCS_C    := src/alpha_runtime.c

OBJ_CXX   := $(SRCS_CXX:.cpp=.o)
OBJ_CXX   := $(OBJ_CXX:.cpp=.o)
OBJ_C     := src/alpha_runtime.o

all: alphac libalpharuntime.a

# ---- Lex / Bison ----
lex.yy.cpp: src/lexer.l parser.tab.h
	$(FLEX) --outfile=lex.yy.cpp src/lexer.l

parser.tab.cpp parser.tab.h: src/parser.y
	$(BISON) -d -o parser.tab.cpp src/parser.y

# ---- Compile ----
%.o: %.cpp parser.tab.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

lex.yy.o: lex.yy.cpp
	$(CXX) $(CXXFLAGS) -Wno-unused-function -c $< -o $@

parser.tab.o: parser.tab.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

src/main.o: src/main.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

src/alpha_runtime.o: src/alpha_runtime.c
	$(CC) $(CFLAGS) -c $< -o $@

# ---- Link compiler ----
alphac: src/main.o lex.yy.o parser.tab.o src/alpha_runtime.o
	$(CXX) $(CXXFLAGS) $^ $(LLVM_LDFLAGS) $(LLVM_LIBS) -lm -o $@

# ---- Runtime static lib (to link against generated .o) ----
libalpharuntime.a: src/alpha_runtime.o
	ar rcs $@ $^

# ---- Helper: compile an Alpha program end-to-end ----
# Usage: make run SRC=myprogram.alpha
run: alphac libalpharuntime.a
	./alphac --emit-llvm $(SRC)
	llc -filetype=obj -o $(SRC:.alpha=.o) $(SRC:.alpha=.ll)
	$(CC) $(SRC:.alpha=.o) libalpharuntime.a -lm -o $(SRC:.alpha=)
	./$(SRC:.alpha=)

clean:
	rm -f alphac lex.yy.cpp lex.yy.h parser.tab.cpp parser.tab.h \
	      *.o src/*.o *.ll *.s libalpharuntime.a

.PHONY: all clean run
