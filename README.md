# AlphaLLVM

A scripting language compiler, implemented in modern C++17 with an LLVM backend. Supports both JIT execution via OrcJIT and ahead-of-time compilation to native object files.


---

## Features

* **OrcJIT execution** — `--run` compiles and executes Alpha programs in-process via LLVM OrcJIT; no intermediate files, no child processes, full native speed
* **AOT compilation** — `--emit-llvm` and `--emit-obj` produce LLVM IR or native object files for static linking and distribution
* **Dynamic typing** — all values are runtime-tagged (`nil`, `number`, `string`, `boolean`, `table`, `userfunc`) with automatic type coercion for string concatenation
* **First-class functions** — functions are values; can be stored in variables, passed as arguments, returned from functions, and stored in tables
* **Closures** — inner functions capture variables from outer scopes; captured variables are automatically promoted to shared storage across function boundaries
* **Mutual recursion** — functions are pre-registered before their bodies are compiled; `isEven` can call `isOdd` and vice versa without forward declarations
* **Prototype-based OOP** — objects are tables with function fields; inheritance and method dispatch implemented via closure-captured `self` references
* **Tables** — first-class hash maps supporting positional, named-key (`[name: value]`), and computed-key (`[expr: value]`) construction; used as arrays, dictionaries, and objects
* **Full control flow** — `if/else`, `while`, `for`, `break`, `continue`, `return` with correct short-circuit evaluation for `and`/`or`
* **String concatenation** — `..` operator with automatic coercion of any value to string
* **Math library** — built-in `sqrt`, `sin`, `cos`, `floor`, `ceil`, `abs`, `max`, `min`
* **Scoping** — `local` declarations, `::global` forced lookups, and lexical scoping with function nesting depth tracking
* **Optimization** — LLVM `PassBuilder` pipeline at `-O0`, `-O1`, `-O2`; `mem2reg` promotes stack slots to SSA registers eliminating redundant loads/stores
* **Symbol table** — full scoped symbol table with function nesting levels, formal parameter tracking, and library function shadow detection
* **Complete pipeline** - Flex lexer → Bison LALR(1) parser → semantic analyzer → LLVM IR codegen, each independently inspectable via `--dump-ast` and `--dump-symbols`

---

## Architecture

```
Source (.alpha)
    │
    ▼
Flex Lexer + Bison Parser
    │  → AST (ast.h)
    ▼
Semantic Analyzer + Symbol Table
    │  → Scoped SymbolTable with function nesting (symtable.h, semantic.h)
    ▼
LLVM IR Code Generator
    │  → LLVM Module in memory (codegen.h)
    ▼
    ├─── --run ──────────────────────────────────────────────────────┐
    │                                                                │
    │   OrcJIT (inside alphac)                                       │
    │     ├─ LLVM backend: instruction selection,                    │
    │     │  register allocation, machine code emission              │
    │     ├─ Symbol resolution: alpha_rt_* resolved from             │
    │     │  alphac's own address space (statically linked)          │
    │     └─ fn() ← native machine code runs in-process              │
    │                                                                │
    └─── --emit-llvm / --emit-obj ───────────────────────────────────┘
                                                                     │
        alphac writes .ll or .o to disk                              │
            │                                                        │
            ▼                                                        │
        llc + clang (external tools)                                 │
            │  Linked with alpha_runtime.c                           │
            ▼                                                        │
        Native Executable                                            │
            │                                                        │
            └────────────────────────────────────────────────────────┘
                                                                     │
                                                                     ▼
                                                             Program Output
```

---

## File Reference

| File | Role |
|---|---|
| `src/ast.h` | AST node definition — data structures and enums only, no method bodies |
| `src/symtable.h` | Symbol table class declarations |
| `src/symtable.cpp` | `SymbolTable`, `Scope`, `Symbol` implementations |
| `src/semantic.h` | Semantic analyzer class declaration |
| `src/semantic.cpp` | All `SemanticAnalyzer` method implementations |
| `src/codegen.h` | LLVM codegen class declaration, member variables, private method signatures |
| `src/codegen.cpp` | All `LLVMCodeGen` method implementations |
| `src/alpha_runtime.c` | C runtime — heap-allocated values, tables, print, math, equality |
| `src/main.cpp` | Compiler driver — CLI, phase orchestration, OrcJIT execution |
| `src/lexer.l` | Flex scanner — all tokens, nested `/* */` comments, string escapes |
| `src/parser.y` | Bison LALR(1) grammar — full Alpha syntax, builds typed AST |
| `CMakeLists.txt` | CMake build supporting LLVM 14–22+ |
| `Makefile` | GNU Make fallback |
| `Dockerfile` | Reproducible build container |
| `alpha-run.sh` | Shell script for JIT (`--run`) and AOT (`--aot`) pipelines |
| `examples/mathcomp.alpha` | A mathematical expression compiler built using AlphaLLVM (because why not?). |
| `examples/test.alpha` | 20-section test covering every language feature |
| `examples/linked_list.alpha` | Linked list with `map`, `filter`, `foldLeft` |
| `examples/mergesort.alpha` | In-place merge sort with randomized correctness check |
| `examples/oop.alpha` | Prototype-based OOP — shapes, method override, bounded counter |

---

## Language

Alpha is a dynamically typed scripting language. Every value is one of:

| Type | Example |
|---|---|
| `nil` | `nil` |
| `number` | `42`, `3.14` |
| `string` | `"hello"` |
| `boolean` | `true`, `false` |
| `table` | `[1, 2, 3]`, `[x: 10, y: 20]` |
| `userfunc` | `function f(x) { return x; }` |

### Syntax quick reference

```alpha
// Variables
x = 10;
local y = 20;     // local to current scope
::z = 30;         // force global scope

// Control flow
if (x > 5) { print("big"); } else { print("small"); }
while (x > 0) { x--; }
for (i = 0; i < 10; i++) { print(i); }

// Functions
function add(a, b) { return a + b; }
square = lambda(x) { return x * x; };

// Tables
arr = [1, 2, 3];
obj = [name: "Alice", age: 30];
obj.age = 31;
print(arr[0]);
print(obj.name);

// String concatenation (auto-coerces numbers)
print("value = " .. 42);

// Closures
function makeCounter(n) {
    function inc() { n = n + 1; return n; }
    return inc;
}
c = makeCounter(0);
print(c());   // 1
print(c());   // 2

// OOP via tables
function Animal(name) {
    local self = [name: name];
    function speak() { print(self.name .. " speaks"); }
    self.speak = speak;
    return self;
}
a = Animal("Dog");
a.speak();
```

### Built-in functions

| Function | Description |
|---|---|
| `print(v)` | Print value followed by newline |
| `input()` | Read line from stdin, returns string |
| `typeof(v)` | Returns type name as string |
| `tostring(v)` | Convert any value to string |
| `sqrt(n)` | Square root |
| `pow(n, p)` | Power |
| `sin(n)` / `cos(n)` | Trigonometry |
| `floor(n)` / `ceil(n)` | Rounding |
| `abs(n)` | Absolute value |
| `max(a,b)` / `min(a,b)` | Numeric min/max |
| `objectmemberkeys(t)` | Table of all keys |
| `objecttotalmembers(t)` | Number of entries |
| `objectcopy(t)` | Shallow copy of table |
| `strlen(s)` | Length of a string in characters |
| `strchar(s, i)` | Single character of string at index i |

---

## Runtime

The compiler uses a **custom C runtime** (`alpha_runtime.c`) statically linked into `alphac`. Every Alpha value is a heap-allocated `AlphaVal`:

```c
typedef struct AlphaVal {
    int32_t tag;   // 0=nil 1=number 2=string 3=bool 4=table 5=userfunc
    int64_t data;  // double bits / pointer cast to int64
} AlphaVal;
```

When running with `--run`, the OrcJIT engine resolves all `alpha_rt_*` symbols directly from `alphac`'s own address space — no dynamic linking or file I/O required.

Tables are implemented as open-addressing hash maps with 0.7 load factor and automatic resizing.

---

## Build

### Prerequisites

```bash
# Ubuntu / Debian
sudo apt install flex bison llvm-dev clang cmake build-essential

# macOS (Homebrew)
brew install llvm flex bison cmake
```

### CMake (recommended)

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DLLVM_DIR=$(brew --prefix llvm)/lib/cmake/llvm   # macOS only
make -j$(nproc)
```

### GNU Make (fallback)

```bash
make -j$(nproc)
```

---

## Usage

```bash
# JIT: compile and run immediately (no files written)
./alphac --run program.alpha

# Emit LLVM IR
./alphac --emit-llvm program.alpha        # writes program.ll

# Emit native object file
./alphac --emit-obj -o program.o program.alpha

# AOT full pipeline (requires llc + clang)
./alpha-run.sh --aot program.alpha

# Debug flags
./alphac --run --dump-ast program.alpha
./alphac --run --dump-symbols program.alpha

# Optimization level (default: -O1)
./alphac --run -O0 program.alpha
./alphac --run -O2 program.alpha
```

### Run the examples

```bash
./alphac --run test.alpha
./alphac --run examples/mergesort.alpha
./alphac --run examples/linked_list.alpha
./alphac --run examples/oop.alpha
```

### Docker (no local LLVM required)

```bash
docker build -t alphac .
docker run --rm -v $(pwd):/src alphac /src/test.alpha
```

---

## Lines of Code
```
-------------------------------------------------------------------------------
Language                     files          blank        comment           code
-------------------------------------------------------------------------------
C++                              4            152             71           1237
yacc                             1             35              2            432
C                                1             36             31            392
C/C++ Header                     4             48             21            274
Markdown                         1             41              0            133
CMake                            1             13             18            107
lex                              1             13              3             93
make                             1             22             17             53
-------------------------------------------------------------------------------
SUM:                            14            360            163           2721
-------------------------------------------------------------------------------
```

---

*Important Notes:*
* This software was produced using Claude Code (Sonnet 4.6). It has not been tested to production-level code.
* While I aim developing and improving the project, it is provided AS-IS, without guarantees.
* The concept of this project is inspired (although considerably changed) by the compilers project I had implemented at CSD - UoC.
* I used, to the best of my knowledge, only information that is already publicly available and without limitations (e.g., language syntax), as well as information from code that was my own.
* I am **against** any usage of the project for plagiarism purposes (e.g., copying it arbitrarily on university projects). However, if you want to use it as the base for a tool to LEGITIMATELY learn how to build a compiler, then you can do so.
