English | [繁體中文](README_TW.md)

# XCC — Cross-Platform C Compiler

`xcc` is a cross-platform C compiler written from scratch in pure C11 (zero external dependencies). It features a complete frontend, a platform-independent Three-Address Code intermediate representation (TAC IR), a built-in IR virtual machine, and multi-target native assembly code generators (supporting **ARM64** and **x86_64**, covering **macOS**, **Linux**, and **Windows** ABIs).

## Architecture

```
C Source (.c / .h)
   │
   ▼
[Preprocessor]   src/preprocessor.c   (#include, #define macro expansion, #if/#ifdef/#elif/#else, embedded standard headers)
   │
   ▼
[Lexer]          src/lexer.c          (Tokenization, literal parsing, adjacent string literal concatenation, line/column tracking)
   │
   ▼
[Parser & Type]  src/parser.c         (Recursive descent parsing, type checking, pointer arithmetic scaling, symbol tables & scopes)
   │             src/type.c
   ▼
[3-Address IR]   src/ir.c             (Platform-independent linear Three-Address Code IR)
   │
   ▼
[Custom Hardened IR & Polymorphic VM] src/ir_obf.c
   │  ├─ Control Flow Flattening (CFF) + Branchless State Selection + Basic Block Physical Shuffling
   │  ├─ Bogus Control Flow (BCF) + Number-Theoretic Opaque Predicates
   │  ├─ Mixed Boolean Arithmetic (MBA: obf.mba.add/sub/xor) + Dynamic Immediate Decomposition Encryption (obf.imm)
   │  ├─ Compile-Time String Stream Cipher Encryption + Runtime Dynamic Decryption (Eliminating plaintext strings & static Xrefs)
   │  └─ Embedded Polymorphic Self-Checksumming Bytecode Virtual Machine (Polymorphic Self-Checksumming VM Virtualizer)
   │
   ├──► [IR VM]          src/vm.c             (--run-ir direct cross-platform execution & verification)
   ├──► [ARM64 Backend]  src/codegen_arm64.c  (macOS Mach-O / Apple ABI & Linux ELF / AAPCS64)
   └──► [x86_64 Backend] src/codegen_x86_64.c (Linux ELF / macOS Mach-O System V ABI & Windows Win64 ABI)
```

## Supported Features

- **Data Types**: `void`, `_Bool`/`bool`, `char`, `short`, `int`, `long` (including `signed` / `unsigned`), enumerations `enum`, `typedef`
- **Composite Types**: Multi-level pointers (`T*`, `T**`), 1D and multi-dimensional arrays (`a[i][j]`), `struct`, `union`, member access via `.` and `->`, struct assignment/copy, `sizeof`
- **Operators**: Arithmetic, bitwise, shift, relational, logical short-circuit evaluation (`&&`, `||`), ternary operator (`?:`), compound assignments (`+=`, `-=`, `*=`, `/=`, etc.), prefix/postfix increment/decrement (`++`, `--`), pointer arithmetic
- **Control Flow**: `if`/`else`, `while`, `do-while`, `for`, `switch`/`case`/`default`, `break`, `continue`, `goto`/labels, `return`
- **Functions**: Function declarations and definitions, recursive calls, parameter passing exceeding register capacity, variadic external function calls (e.g., `printf`, `snprintf`)
- **Preprocessor**: `#include <...>` (embedded cross-platform standard library headers `<stdio.h>`, `<stdlib.h>`, `<string.h>`, `<stdint.h>`, `<stdbool.h>`, `<stddef.h>`, `<ctype.h>`, `<assert.h>`), `#include "..."`, `#define` (constant and function-like macros, stringification `#`), `#undef`, `#ifdef`/`#ifndef`/`#if`/`#elif`/`#else`/`#endif`, `#pragma once`

## Build and Test

```bash
# Compile the xcc compiler
make

# Run the complete test suite (native execution, IR VM, Custom IR obfuscation, polymorphic VM virtualization, anti-string-leakage, and cross-platform object file validation)
make test
```

## Usage

```bash
# 1. Compile and run directly as a native executable on the current platform
./xcc --run examples/demo.c

# 2. Compile and run with dual-track hardened protection (Custom IR obfuscation + embedded polymorphic self-checksumming Bytecode VM)
./xcc --obf-all --run examples/anti_crack_demo.c

# 3. Inspect custom hardened IR or polymorphic VM IR
./xcc --obf --emit-ir examples/anti_crack_demo.c
./xcc --obf-vm --emit-ir examples/anti_crack_demo.c

# 4. Individually configure anti-reverse engineering passes and set a polymorphic seed
./xcc --obf-cff --obf-bcf --obf-mba --obf-str --obf-seed=0x13372026 examples/anti_crack_demo.c -o hardened_app

# 5. Emit hardened assembly (.s) or object files (.o) for different operating systems and CPU architectures
./xcc --obf-all -target arm64-macos    -S examples/anti_crack_demo.c -o anti_arm64_macos.s
./xcc --obf-all -target arm64-linux    -S examples/anti_crack_demo.c -o anti_arm64_linux.s
./xcc --obf-all -target x86_64-macos   -S examples/anti_crack_demo.c -o anti_x86_64_macos.s
./xcc --obf-all -target x86_64-linux   -S examples/anti_crack_demo.c -o anti_x86_64_linux.s
./xcc --obf-all -target x86_64-windows -S examples/anti_crack_demo.c -o anti_x86_64_win.s
```
