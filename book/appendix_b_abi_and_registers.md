# 附錄 B：五大目標平台 ABI 呼叫慣例與暫存器對照表

本附錄詳細比對 XCC 編譯器原生支援的五大目標平台架構（ARM64 macOS、ARM64 Linux、x86_64 macOS、x86_64 Linux、x86_64 Windows）之二進位介面（ABI）規格與硬體暫存器配置。

---

## B.1 目標平台矩陣速查

| 目標名稱 (`-target`) | 架構 / 格式 | Triple 字串 | 符號前綴 | 巨集定義 | 呼叫慣例標準 |
| :--- | :--- | :--- | :---: | :--- | :--- |
| `arm64-macos` | ARM64 Mach-O | `arm64-apple-darwin` | `_` | `__APPLE__`, `__aarch64__` | Apple Silicon ARM64 ABI |
| `arm64-linux` | ARM64 ELF | `aarch64-unknown-linux-gnu` | `""` | `__linux__`, `__aarch64__` | Standard AAPCS64 |
| `x86_64-macos` | x86_64 Mach-O | `x86_64-apple-darwin` | `_` | `__APPLE__`, `__x86_64__` | System V AMD64 ABI |
| `x86_64-linux` | x86_64 ELF | `x86_64-unknown-linux-gnu` | `""` | `__linux__`, `__x86_64__` | System V AMD64 ABI |
| `x86_64-windows` | x86_64 PE/COFF | `x86_64-w64-mingw32` | `""` | `_WIN32`, `_M_X64` | Microsoft x64 (Win64) ABI |

---

## B.2 函式呼叫暫存器分配比對

### 1. 整數與指標引數傳遞順序

| 引數序號 | ARM64 Linux | ARM64 macOS (非可變) | ARM64 macOS (可變參數) | x86_64 Linux / macOS | x86_64 Windows (Win64) |
| :---: | :---: | :---: | :---: | :---: | :---: |
| **第 1 引數** | `x0` | `x0` | `x0`（若為固定參數） | `%rdi` | `%rcx` |
| **第 2 引數** | `x1` | `x1` | **堆疊傳遞**（若為可變） | `%rsi` | `%rdx` |
| **第 3 引數** | `x2` | `x2` | **堆疊傳遞** | `%rdx` | `%r8` |
| **第 4 引數** | `x3` | `x3` | **堆疊傳遞** | `%rcx` | `%r9` |
| **第 5 引數** | `x4` | `x4` | **堆疊傳遞** | `%r8` | **堆疊傳遞** |
| **第 6 引數** | `x5` | `x5` | **堆疊傳遞** | `%r9` | **堆疊傳遞** |
| **第 7 引數** | `x6` | `x6` | **堆疊傳遞** | **堆疊傳遞** | **堆疊傳遞** |
| **第 8 引數** | `x7` | `x7` | **堆疊傳遞** | **堆疊傳遞** | **堆疊傳遞** |
| **第 9+ 引數** | **堆疊傳遞** | **堆疊傳遞** | **堆疊傳遞** | **堆疊傳遞** | **堆疊傳遞** |

---

## B.3 關鍵 ABI 差異特性總結

### 1. 堆疊對齊要求（Stack Alignment）
* **全平台通用鐵律**：無論是 ARM64 還是 x86_64，在執行函式呼叫指令（`bl` 或 `call`）之前，堆疊指標（`sp` / `%rsp`）**必須維持 16 位元組（16-byte aligned）對齊**！
* 在 ARM64 上，若 `sp` 未對齊即執行讀寫，CPU 會觸發硬體對齊異常；在 x86_64 上，若堆疊未對齊，呼叫含有 SSE/AVX 向量指令的標準 C 函式（如 `printf`）會觸發 `SIGSEGV`。

### 2. Apple Silicon ARM64 可變參數特殊規定
* **Linux AAPCS64**：固定參數與可變參數一律優先填滿 `x0–x7`。
* **Apple Silicon ARM64**：所有屬於可變參數（`...`）的匿名引數，**一律禁止使用暫存器，強制壓入堆疊傳遞**！

### 3. Windows Win64 陰影空間（Shadow Space / Home Space）
* **微軟規範**：在發起 `callq` 之前，呼叫者必須在 `%rsp` 之上**無條件預留至少 32 位元組（4 個 64-bit slot）的空間**。
* 超過 4 個引數的堆疊參數，必須放在這 32 位元組陰影空間的上方（`32(%rsp), 40(%rsp)...`）。

### 4. System V 可變參數 `%al` 約定
* 在 Linux 與 macOS x86_64 上呼叫可變參數函式（如 `printf`）之前，**必須執行 `movb $0, %al`**，告知被呼叫者使用了 0 個 SSE 浮點向量暫存器。Windows 則無此要求。
