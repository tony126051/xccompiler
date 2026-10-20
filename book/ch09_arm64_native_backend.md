# 第 9 章：ARM64 (AArch64) 原生後端：Apple Darwin 與 Linux AAPCS64

> 「在 RISC 的世界裡，一切都是那麼規整——直到你遇到了 Apple Silicon 與可變參數函式呼叫。」

在過去十年間，ARM64（AArch64）架構經歷了爆發式的崛起。從智慧型手機、AWS Graviton 伺服器、到 Apple M 系列晶片的 MacBook，ARM64 已經成為現代軟體工程師最常接觸的硬體平台之一。

然而，許多人以為「ARM64 組合語言在 macOS 和 Linux 上是一模一樣的」。這是一個致命的誤解！
雖然二者擁有完全相同的 CPU 指令集（如 `ldr`, `str`, `add`, `bl`），但它們在**二進位檔案格式（Mach-O vs. ELF）、符號命名約定（Symbol Prefix）、PC-Relative 頁面尋址、以及最惡名昭彰的「可變參數呼叫慣例（Variadic ABI）」**上，有著巨大的差異。

在本章中，我們將深入剖析 XCC 的 ARM64 代碼生成器（[`src/codegen_arm64.c`](file:///Users/hcwtony/repo/projects/ccompiler/src/codegen_arm64.c)）。我們將探討堆疊框對齊鐵律、64 位元常數載入、大偏移量尋址技巧，並攻克 Apple Silicon 上最容易踩坑的 Variadic ABI 陷阱。

---

## 9.1 AArch64 硬體架構與暫存器規範

AArch64 是一種標準的精簡指令集（RISC）架構。它具備 31 個 64 位元通用暫存器（`x0–x30`，亦可作為 32 位元 `w0–w30` 存取），以及獨立的堆疊指標 `sp` 與零暫存器 `xzr`。

在標準 AAPCS64（ARM 64-bit Architecture Procedure Call Standard）中，暫存器約定如下表：

| 暫存器名稱 | 角色與用途 | 呼叫保存約定（Caller/Callee Saved） |
| :--- | :--- | :--- |
| `x0` – `x7` | 前 8 個引數傳遞 / `x0` 同時作為函式回傳值 | **Caller-saved**（呼叫者自行保存） |
| `x8` | 間接結果暫存器（XR）或暫存運算 | **Caller-saved** |
| `x9` – `x15` | 通用暫存運算暫存器 | **Caller-saved** |
| `x16`, `x17` | 程序內連結暫存器（IP0, IP1 / PLT Stubs） | **Caller-saved** |
| `x19` – `x28` | 被呼叫者保存暫存器（Callee-saved） | **Callee-saved**（若使用需在序言 push） |
| `x29` (FP) | 框架指標（Frame Pointer） | 記錄呼叫堆疊底端 |
| `x30` (LR) | 連結暫存器（Link Register） | 存放函式回傳跳躍位址 |
| `sp` | 堆疊指標（Stack Pointer） | **硬體強制 16-byte 對齊** |

---

## 9.2 函式序言（Prologue）、結尾（Epilogue）與 16-Byte 堆疊對齊

在 ARM64 硬體架構中，**任何透過 `sp` 進行的記憶體存取，`sp` 的值在硬體層面必須嚴格維持 16 位元組對齊（16-byte aligned）**，否則硬體會立即觸發 Alignment Exception。

### 1. 堆疊框空間規劃
函式堆疊框需要容納兩部分資料：
1. **區域變數插槽（Local Slots）**：儲存 C 語言的局部變數、陣列、結構體。
2. **虛擬暫存器溢出槽（VReg Spill Slots）**：儲存 TAC IR 中所有 `%v0, %v1...` 的數值。

在 [`src/codegen_arm64.c`](file:///Users/hcwtony/repo/projects/ccompiler/src/codegen_arm64.c#L120-L160) 中，我們計算總框架大小並對齊到 16 的倍數：
```c
int total_frame = align16(vreg_base + (fn->vreg_count + 1) * 8);
```

### 2. 序言與結尾組合語言發射
```arm64
# 函式序言 (Prologue)
_my_func:
  stp x29, x30, [sp, #-16]!   ; 將 FP 與 LR 成對壓入堆疊，sp 遞減 16
  mov x29, sp                 ; 設定當前 Frame Pointer 為堆疊頂端
  sub sp, sp, #total_frame    ; 配置局部變數與虛擬暫存器堆疊空間

  # ... 函式主體指令 ...

  # 函式結尾 (Epilogue)
  mov sp, x29                 ; 還原堆疊指標
  ldp x29, x30, [sp], #16     ; 彈出 FP 與 LR，sp 遞增 16
  ret                         ; 跳躍回 LR (x30)
```
這種 `stp`/`ldp` 搭配後變形（Post-index）與前變形（Pre-index）的寫法，是 ARM64 標準 ABI 的標準典範。

---

## 9.3 攻克 ARM64 立即數限制：64 位元常數與大偏移量堆疊存取

RISC 架構的所有指令長度均固定為 32 位元（4 bytes）。這帶來了一個物理限制：**一條指令根本不可能直接塞進一個 64 位元的常數或大於 4095 的記憶體偏移量！**

### 1. 任意 64 位元常數載入（`movz` + `movk`）
如何將一個 64 位元常數（例如 `0x1337BEEFC0FFEE11ULL`）載入暫存器 `x8`？
在 [`src/codegen_arm64.c`](file:///Users/hcwtony/repo/projects/ccompiler/src/codegen_arm64.c#L15-L36) 中，我們實作了 `emit_mov_imm64`：
```c
static void emit_mov_imm64(FILE *out, const char *reg, int64_t val) {
    uint64_t u = (uint64_t)val;
    // 小整數直接使用 mov
    if (val >= 0 && val <= 65535) {
        fprintf(out, "  mov %s, #%lld\n", reg, (long long)val);
        return;
    }
    // 大整數拆為 4 個 16-bit 區塊
    fprintf(out, "  movz %s, #%u\n", reg, (unsigned)(u & 0xFFFF));
    if ((u >> 16) & 0xFFFF)
        fprintf(out, "  movk %s, #%u, lsl #16\n", reg, (unsigned)((u >> 16) & 0xFFFF));
    if ((u >> 32) & 0xFFFF)
        fprintf(out, "  movk %s, #%u, lsl #32\n", reg, (unsigned)((u >> 32) & 0xFFFF));
    if ((u >> 48) & 0xFFFF)
        fprintf(out, "  movk %s, #%u, lsl #48\n", reg, (unsigned)((u >> 48) & 0xFFFF));
}
```
* `movz`（Move with Zero）：將 16 位元數值填入指定區塊，其他位元全部清為 0。
* `movk`（Move with Keep）：僅覆蓋指定的 16 位元區塊，保留其他 48 位元原值。

### 2. 突破 256 位元組堆疊負偏移量限制（`ldur` vs. `x15` 輔助暫存器）
在 ARM64 中，以 FP 為基準的負偏移讀寫 `[x29, #-offset]`：
* 標準非對齊指令 `ldur` / `stur` 支援的立即數範圍僅為 **-256 到 +255**！
* 當函式的局部變數或虛擬暫存器較多，`offset` 超過 256 時，直接寫 `ldur x8, [x29, #-300]` 會導致組譯器報錯。

在 [`src/codegen_arm64.c`](file:///Users/hcwtony/repo/projects/ccompiler/src/codegen_arm64.c#L38-L66) 中，我們巧妙利用暫存暫存器 `x15` 解決此限制：
```c
static void emit_load_slot(FILE *out, const char *reg, int offset) {
    if (offset <= 256) {
        fprintf(out, "  ldur %s, [x29, #-%d]\n", reg, offset);
    } else {
        // 先計算實際位址到暫存暫存器 x15
        emit_frame_addr(out, "x15", offset); // sub x15, x29, #offset
        fprintf(out, "  ldr %s, [x15]\n", reg);
    }
}
```

---

## 9.4 macOS Mach-O 與 Linux ELF 的跨平台差異

雖然都是 ARM64，但目標作業系統的不同直接反映在組合語言語法上：

### 1. 符號名稱前綴（Symbol Prefix）
* **macOS（Mach-O 格式）**：C 語言的函式 `main` 在組合語言中必須帶有下劃線前綴 `_main`。
* **Linux（ELF 格式）**：符號名稱直接為 `main`。
XCC 在發射符號與呼叫時，統一透過 `target->symbol_prefix` 自動處理。

### 2. PC-Relative 頁面位址尋址
在 64 位元環境中，為了支援 ASLR（位址空間隨機載入）與位置無關程式碼（PIC），存取全域變數或字串必須相對於當前程式計數器（PC）：
* **macOS Mach-O 語法**：
  ```arm64
  adrp x8, _global_var@GOTPAGE
  ldr  x8, [x8, _global_var@GOTPAGEOFF]
  ```
  對於局部字串標籤（`.L.str.0`）：
  ```arm64
  adrp x8, l_.str.0@PAGE
  add  x8, x8, l_.str.0@PAGEOFF
  ```
* **Linux ELF 語法**：
  ```arm64
  adrp x8, global_var
  add  x8, x8, :lo12:global_var
  ```

---

## 9.5 深入核心：Apple ARM64 最容易踩坑的 Variadic ABI

這是所有手寫 C 編譯器開發者在 Apple Silicon 上幾乎必定遭遇的「魔鬼細節」：

### 為什麼我的 `printf` 在 Linux ARM64 能跑，在 Mac M1/M2/M3 上卻印出亂碼？
* **標準 Linux AAPCS64 規範**：
  前 8 個整數或指標引數，無論該引數是固定參數（Fixed Argument）還是可變參數（Variadic Anonymous Argument，如 `printf("fmt", a, b)` 中的 `a` 和 `b`），**一律填入暫存器 `x0` 到 `x7`**。
* **Apple Silicon ARM64 ABI 規範**：
  Apple 為了簡化其 Darwin 核心的 `va_list` 實作，制定了一個特殊規則：**所有可變參數（位於 `...` 之後的引數），即使 `x0–x7` 還有空位，也一律強制放到堆疊（Stack）上傳遞！**

在 [`src/codegen_arm64.c`](file:///Users/hcwtony/repo/projects/ccompiler/src/codegen_arm64.c#L560-L586) 中，我們精確實作了這條規則：

```c
case IR_CALL: {
    int argc = in->call_arg_count;
    int reg_limit = 8;

    // 若目標為 macOS 且為可變參數呼叫，暫存器引數上限縮減為固定參數個數！
    if (target->is_macos && in->call_is_variadic && in->call_fixed_count < 8) {
        reg_limit = in->call_fixed_count;
    }

    int reg_args = argc < reg_limit ? argc : reg_limit;
    int stack_args = argc > reg_limit ? (argc - reg_limit) : 0;
    int stack_bytes = align16(stack_args * 8);

    // 1. 先將超出暫存器限制的引數壓入堆疊 (依 16-byte 對齊)
    if (stack_bytes > 0) {
        fprintf(out, "  sub sp, sp, #%d\n", stack_bytes);
        for (int a = 0; a < stack_args; a++) {
            int vreg = pending_args[reg_limit + a];
            emit_load_slot(out, "x8", vreg_offset(vreg_base, vreg));
            fprintf(out, "  str x8, [sp, #%d]\n", a * 8);
        }
    }

    // 2. 其餘引數放入 x0..x(reg_args - 1)
    for (int a = 0; a < reg_args; a++) {
        char rname[8];
        snprintf(rname, sizeof(rname), "x%d", a);
        emit_load_slot(out, rname, vreg_offset(vreg_base, pending_args[a]));
    }

    // 3. 呼叫目標函式
    fprintf(out, "  bl %s%s\n", target->symbol_prefix, in->sym_name);
    // ...
}
```

以 `printf("num: %d\n", 42)` 為例：
* 在 Linux ARM64 上：`x0` 放格式字串位址，`x1` 放數值 `42`。
* 在 macOS ARM64 上：`x0` 放格式字串位址（因為 format 是唯一的固定參數）；數值 `42` 則被壓入 `[sp]` 堆疊中傳遞！

這個精妙的條件分支讓 XCC 編譯出的二進位檔在 Linux 和 macOS 上都能完美、穩定地呼叫 C 標準程式庫。

---

## 9.6 小結

在本章中，我們掌握了：
1. AArch64 架構的暫存器規範與 `sp` 硬體 16-byte 對齊要求。
2. 如何使用 `movz`/`movk` 載入任意 64 位元常數，以及大堆疊框的位址計算技巧。
3. macOS Mach-O（`_` 前綴、`@PAGE`）與 Linux ELF（`:lo12:`）在組合語言層級的精準對應。
4. Apple Silicon ARM64 與 Linux AAPCS64 在可變參數函式呼叫上的本質差異與相容實作。

在征服了 ARM64 架構之後，在下一章中，我們將轉向伺服器與傳統 PC 領域的另一大霸主：**x86_64（AMD64）原生後端與 System V vs. Windows Win64 ABI 的正面對決**！
