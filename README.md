# XCC — 跨平台 C 語言編譯器 (Cross-Platform C Compiler)

`xcc` 是一個使用純 C11（零外部依賴）從頭實作的跨平台 C 語言編譯器。採用完整的前端、平台無關三址碼中間表示（Three-Address Code IR）、內建 IR 虛擬機，以及多目標原生組合語言後端（支援 **ARM64** 與 **x86_64**，涵蓋 **macOS**、**Linux**、**Windows** ABI）。

## 架構設計

```
C Source (.c / .h)
   │
   ▼
[Preprocessor]   src/preprocessor.c   (#include, #define 巨集展開, #if/#ifdef/#elif/#else, 內建標準標頭檔)
   │
   ▼
[Lexer]          src/lexer.c          (Token 切分、字面量解析、鄰接字串合併、行列號追蹤)
   │
   ▼
[Parser & Type]  src/parser.c         (遞迴下降語法分析、型別檢查、指標算術縮放、符號表與作用域)
   │             src/type.c
   ▼
[3-Address IR]   src/ir.c             (平台無關線性三址碼 IR)
   │
   ▼
[Custom Hardened IR & Polymorphic VM] src/ir_obf.c
   │  ├─ 控制流平坦化 (CFF) + 無分支狀態轉移 (Branchless State Selection) + 基本塊實體洗牌
   │  ├─ 虛假控制流 (BCF) + 數論不透明謂詞 (Number-Theoretic Opaque Predicates)
   │  ├─ 混合布林算術 (MBA: obf.mba.add/sub/xor) + 動態常數拆解加密 (obf.imm)
   │  ├─ 編譯期字串流密碼加密 + 執行期動態還原 (消滅明文字串與靜態 Xrefs)
   │  └─ 內嵌多態自校驗 Bytecode 虛擬機 (Polymorphic Self-Checksumming VM Virtualizer)
   │
   ├──► [IR VM]          src/vm.c             (--run-ir 直接跨平台執行與驗證)
   ├──► [ARM64 Backend]  src/codegen_arm64.c  (macOS Mach-O / Apple ABI & Linux ELF / AAPCS64)
   └──► [x86_64 Backend] src/codegen_x86_64.c (Linux ELF / macOS Mach-O System V ABI & Windows Win64 ABI)
```

## 支援的語法特性

- **資料型別**：`void`, `_Bool`/`bool`, `char`, `short`, `int`, `long`（含 `signed` / `unsigned`）、列舉 `enum`、`typedef`
- **複合型別**：多級指標（`T*`, `T**`）、一維與多維陣列（`a[i][j]`）、結構體（`struct`）、聯合體（`union`）、`.` 與 `->` 成員存取、結構體賦值拷貝、`sizeof`
- **運算子**：算術、位元、移位、關係、邏輯短路求值（`&&`, `||`）、三元運算子（`?:`）、複合賦值（`+=`, `-=`, `*=`, `/=` 等）、前置/後置遞增遞減（`++`, `--`）、指標算術
- **控制流程**：`if`/`else`, `while`, `do-while`, `for`, `switch`/`case`/`default`, `break`, `continue`, `goto`/標籤、`return`
- **函式**：函式宣告與定義、遞迴呼叫、超過暫存器數量的多參數傳遞、可變參數外部函式呼叫（如 `printf`, `snprintf`）
- **前置處理器**：`#include <...>`（內建跨平台標準庫標頭 `<stdio.h>`, `<stdlib.h>`, `<string.h>`, `<stdint.h>`, `<stdbool.h>`, `<stddef.h>`, `<ctype.h>`, `<assert.h>`）、`#include "..."`、`#define`（常數與函數式巨集、字串化 `#`）、`#undef`、`#ifdef`/`#ifndef`/`#if`/`#elif`/`#else`/`#endif`、`#pragma once`

## 建置與測試

```bash
# 編譯 xcc 編譯器
make

# 執行完整測試套件（包含原生執行、IR VM、Custom IR 混淆、多態 VM 虛擬化、反字串洩漏與跨平台目標檔驗證）
make test
```

## 使用方式

```bash
# 1. 直接編譯成目前平台的原生執行檔並執行
./xcc --run examples/demo.c

# 2. 啟用雙軌最高強度防破解（Custom IR 混淆 + 內嵌多態自校驗 Bytecode VM）編譯並執行
./xcc --obf-all --run examples/anti_crack_demo.c

# 3. 檢視特製防破解 Custom Hardened IR 或多態 VM IR
./xcc --obf --emit-ir examples/anti_crack_demo.c
./xcc --obf-vm --emit-ir examples/anti_crack_demo.c

# 4. 個別控制各項防逆向機制與指定多態亂數種子
./xcc --obf-cff --obf-bcf --obf-mba --obf-str --obf-seed=0x13372026 examples/anti_crack_demo.c -o hardened_app

# 5. 輸出不同作業系統與處理器架構的防逆向組合語言 (.s) 或目標檔 (.o)
./xcc --obf-all -target arm64-macos    -S examples/anti_crack_demo.c -o anti_arm64_macos.s
./xcc --obf-all -target arm64-linux    -S examples/anti_crack_demo.c -o anti_arm64_linux.s
./xcc --obf-all -target x86_64-macos   -S examples/anti_crack_demo.c -o anti_x86_64_macos.s
./xcc --obf-all -target x86_64-linux   -S examples/anti_crack_demo.c -o anti_x86_64_linux.s
./xcc --obf-all -target x86_64-windows -S examples/anti_crack_demo.c -o anti_x86_64_win.s
```
