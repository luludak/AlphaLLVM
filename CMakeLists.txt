cmake_minimum_required(VERSION 3.16)
project(alpha-compiler CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── LLVM ──────────────────────────────────────────────────────
# Supports LLVM 14-20+.
# macOS/Homebrew:  cmake .. -DLLVM_DIR=$(brew --prefix llvm)/lib/cmake/llvm
# Ubuntu:          apt install llvm-dev  (auto-detected)
find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} (${LLVM_DIR})")

include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")
add_definitions(${LLVM_DEFINITIONS_LIST})

# Component list — includes OrcJIT + ExecutionEngine for --run mode
llvm_map_components_to_libnames(LLVM_LIBS
    Core
    Support
    IRReader
    Target
    TargetParser
    MC
    MCParser
    AsmPrinter
    Passes
    Analysis
    OrcJIT
    OrcTargetProcess
    OrcShared
    ExecutionEngine
    RuntimeDyld
    JITLink
    native
)
# ── Flex & Bison ──────────────────────────────────────────────
# On macOS the system bison is 2.3 (too old). Prefer Homebrew's.
if(APPLE)
    find_program(BISON_EXECUTABLE
        NAMES bison
        PATHS /usr/local/opt/bison/bin /opt/homebrew/opt/bison/bin
        NO_DEFAULT_PATH)
    if(NOT BISON_EXECUTABLE)
        find_program(BISON_EXECUTABLE NAMES bison)
    endif()
    find_program(FLEX_EXECUTABLE
        NAMES flex
        PATHS /usr/local/opt/flex/bin /opt/homebrew/opt/flex/bin
        NO_DEFAULT_PATH)
    if(NOT FLEX_EXECUTABLE)
        find_program(FLEX_EXECUTABLE NAMES flex)
    endif()
endif()
find_package(FLEX  REQUIRED)
find_package(BISON REQUIRED)

FLEX_TARGET(AlphaLexer
    src/lexer.l
    ${CMAKE_CURRENT_BINARY_DIR}/lex.yy.c
    DEFINES_FILE ${CMAKE_CURRENT_BINARY_DIR}/lex.yy.h
)
BISON_TARGET(AlphaParser
    src/parser.y
    ${CMAKE_CURRENT_BINARY_DIR}/parser.tab.cpp
    DEFINES_FILE  ${CMAKE_CURRENT_BINARY_DIR}/parser.tab.h
    COMPILE_FLAGS "-d"
)
ADD_FLEX_BISON_DEPENDENCY(AlphaLexer AlphaParser)

include_directories(${CMAKE_CURRENT_BINARY_DIR} src)

# The flex output is C but included in a C++ project — compile it as C
set_source_files_properties(
    ${CMAKE_CURRENT_BINARY_DIR}/lex.yy.c
    PROPERTIES LANGUAGE C COMPILE_FLAGS "-w"
)
set_source_files_properties(
    ${CMAKE_CURRENT_BINARY_DIR}/parser.tab.cpp
    PROPERTIES COMPILE_FLAGS "-w"
)

# ── Alpha runtime (C) ─────────────────────────────────────────
# Built as a static library AND linked directly into alphac.
# This is what makes --run work: when OrcJIT does in-process symbol
# lookup via DynamicLibrarySearchGenerator, it finds alpha_rt_*
# because they are part of the alphac executable itself.
add_library(alpha_runtime STATIC src/alpha_runtime.c)
target_compile_options(alpha_runtime PRIVATE -Wall -Wextra)
target_link_libraries(alpha_runtime PUBLIC m)

# ── alphac executable ─────────────────────────────────────────
add_executable(alphac
    src/main.cpp
    ${BISON_AlphaParser_OUTPUTS}
    ${FLEX_AlphaLexer_OUTPUTS}
)
target_compile_options(alphac PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(alphac PRIVATE
    alpha_runtime   # <- runtime linked IN so OrcJIT finds alpha_rt_* in-process
    ${LLVM_LIBS}
)

# On Linux: also need dl (dlopen) and z (zlib, LLVM dep)
if(UNIX AND NOT APPLE)
    target_link_libraries(alphac PRIVATE dl z)
endif()

# ── Install ───────────────────────────────────────────────────
include(GNUInstallDirs)
install(TARGETS alphac alpha_runtime
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

# ── Tests ─────────────────────────────────────────────────────
enable_testing()
add_test(NAME EmitLLVM
    COMMAND alphac --emit-llvm -O0 -o /dev/null ${CMAKE_SOURCE_DIR}/test.alpha
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
add_test(NAME JITRun
    COMMAND alphac --run -O1 ${CMAKE_SOURCE_DIR}/test.alpha
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# ── Summary ───────────────────────────────────────────────────
message(STATUS "")
message(STATUS "  LLVM version : ${LLVM_PACKAGE_VERSION}")
message(STATUS "  Build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Flex         : ${FLEX_EXECUTABLE}")
message(STATUS "  Bison        : ${BISON_EXECUTABLE}")
message(STATUS "  JIT mode     : alphac --run <file.alpha>")
message(STATUS "")
