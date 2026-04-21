# AlphaLLVM Compiler — An Alpha PL implementation with LLVM Backend

A complete compiler for the **AlphaLLVM** language, implemented in modern C++17 with an **LLVM IR** backend.

---

## Architecture

```
Source (.alpha)
    │
    ▼
Phase 1+2: Flex Lexer + Bison Parser
    │  → AST (ast.h)
    ▼
Phase 3: Semantic Analyzer + Symbol Table
    │  → Scoped SymbolTable with function nesting (symtable.h, semantic.h)
    ▼
Phase 4: LLVM IR Code Generator
    │  → LLVM Module (codegen.h)
    ▼
Phase 5: LLVM → Native (via llc + clang)
    │  Linked with alpha_runtime.c
    ▼
Native Executable
```

---

## Language Features

Alpha is a dynamically typed scripting language:

| Feature         | Syntax example                          |
|-----------------|----------------------------------------|
| Types           | `nil`, `number`, `string`, `bool`, `table`, `function` |
| Variables       | `x = 10;`  `local x = 5;`  `::globalX` |
| Arithmetic      | `+ - * / %`                             |
| Comparison      | `== != < > <= >=`                       |
| Logic           | `and or not`                            |
| Increment       | `++x  x++  --x  x--`                   |
| Conditionals    | `if (cond) { } else { }`               |
| Loops           | `while (c) {}` `for (i=0; i<n; i++) {}` |
| Functions       | `function f(a, b) { return a+b; }`     |
| Lambdas         | `lambda(x) { return x*x; }`           |
| Tables          | `[1,2,3]`  `[k: v, ...]`  `t.field`   |
| Globals         | `::x` — forced global lookup           |
| Locals          | `local x` — force local scope          |
| Library         | `print input typeof strtonum sqrt`     |

---

## Runtime Value Representation (LLVM)

Every Alpha value is a heap-allocated `AlphaVal`:

```c
struct AlphaVal {
    int32_t tag;    // 0=nil 1=number 2=string 3=bool 4=table 5=userfunc
    int64_t data;   // double bits / ptr / bool
};
```

All generated LLVM functions take and return `AlphaVal*`.

---

## Build

### Prerequisites

```bash
# Ubuntu/Debian
sudo apt install flex bison llvm-dev clang cmake build-essential

# macOS (Homebrew)
brew install flex bison llvm cmake
```

### CMake (recommended)

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### GNU Make (fallback)

```bash
make -j$(nproc)
```

---

## Usage

```bash
# Compile Alpha source to LLVM IR
./alphac --emit-llvm program.alpha

# Compile to object file
./alphac --emit-obj -o program.o program.alpha

# Dump AST
./alphac --dump-ast program.alpha

# Dump symbol table
./alphac --dump-symbols program.alpha

# Full pipeline: Alpha → native binary
./alphac --emit-llvm program.alpha
llc -filetype=obj -o program.o program.ll
clang program.o libalpharuntime.a -lm -o program
./program

# Or use the Makefile shortcut:
make run SRC=test.alpha
```

---

## Compiler Phases

### Phase 1 — Lexer (`lexer.l`)
Flex-based scanner. Tokens: keywords, identifiers, integers, floats, strings (with escape sequences), operators, nested block comments `/* ... */`, line comments `//`.

### Phase 2 — Parser (`parser.y`)
LALR(1) Bison grammar. Produces a typed AST (`ast.h`). Handles:
- Full operator precedence
- `if/else`, `while`, `for`, `break`, `continue`, `return`
- Named and anonymous functions, lambdas
- Table constructors (positional, named, computed-key)
- Scoped identifiers (`local`, `::`)

### Phase 3 — Semantic Analysis (`semantic.h`, `symtable.h`)
- Scoped symbol table (hash map per scope frame, scope stack)
- Function nesting level tracking
- Formal parameter binding
- `break`/`continue` outside loop detection
- `return` outside function detection
- Library function shadow prevention
- Auto-declaration of undeclared variables (Alpha is dynamic)

### Phase 4 — LLVM IR Codegen (`codegen.h`)
- `AlphaVal` struct type with tag-union representation
- All variables as `alloca`'d `AlphaVal**` with flat scope environments
- Full control flow: `if`, `while`, `for`, short-circuit `and`/`or`
- Function definitions → LLVM `Function` objects with `AlphaVal*` ABI
- Lambda closures → anonymous LLVM functions
- Table operations → calls to `alpha_rt_table_*`
- Arithmetic/comparison on extracted double values
- Pre/post increment/decrement

### Phase 5 — Native Execution
The LLVM IR module is:
1. Written to `.ll` (or compiled to `.o` directly via the LLVM `PassManager`)
2. Linked with `alpha_runtime.c` (the C runtime: `print`, `input`, table GC, etc.)
3. Executed natively

---

## Symbol Table Design

```
Scope 0 (global): {lib funcs, global vars}
  └─ Scope 1 (block): {local vars}
       └─ Scope 2 (function): {formals, locals}  ← funcNestLevel++
            └─ Scope 3 (inner block): ...
```

- Each `Symbol` carries: name, kind, scopeLevel, funcNestLevel, offset, line
- `offset` = slot index in the function's local/formal array (useful for VM codegen)
- On scope exit, symbols are **deactivated** (not deleted) for post-analysis dumps
