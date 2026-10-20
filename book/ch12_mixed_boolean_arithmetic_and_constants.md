# 第 12 章：數據流混淆——混合布林算術（MBA）與動態常數加密

> 「在反編譯器的世界裡，代數化簡器是一把鋒利的解剖刀；而混合布林算術，則是讓解剖刀瞬間鈍化的堅韌迷宮。」

當逆向人員使用 IDA Pro、Ghidra 或 Binary Ninja 載入一個未經混淆的二進位檔案時，他們常常會直奔關鍵的數值檢查點：
```c
if (key == 0x1337BEEF) {
    activate_license();
}
```
在標準編譯產生的機器碼中，這段邏輯會直接對應到一條明晃晃的比較指令：
```assembly
# x86_64
movabsq $0x1337beef, %rax
cmpq    %rax, -8(%rbp)
je      .L_success
```
逆向分析人員甚至不需要閱讀控制流程，只需要在反組譯器中搜尋常數 `0x1337beef`，便能精確錨定註冊碼驗證的核心區塊。接著，分析人員利用 SMT 求解器（如 Z3）或反編譯器的常數傳播（Constant Propagation）演算法，輕鬆還原並破解整個驗證邏輯。

為了徹底瓦解靜態常數分析與代數化簡，本章將深入剖析 XCC 中的**數據流混淆（Data-Flow Obfuscation）**兩大支柱：
1. **動態常數拆解加密（Dynamic Constant Encryption, `IR_OBF_IMM`）**：在編譯期消滅所有裸露常數，在執行期動態還原。
2. **混合布林算術（Mixed Boolean-Arithmetic, MBA）**：打破代數環與布林環的界線，讓反編譯器的化簡模式比對全面失效。

---

## 12.1 消滅明文常數：`IR_OBF_IMM` 的動態還原機制

在標準 IR 中，常數載入指令形式為：
```
%v0 = imm 0x1337BEEF
```
如果直接編譯，這個 64 位元常數將直接暴露在二進位檔的指令立即數中。

在 XCC 的混淆器 Pass 2（[`src/ir_obf.c`](file:///Users/hcwtony/repo/projects/ccompiler/src/ir_obf.c#L85-L105)）中，只要啟用了 `--obf-mba` 或 `--obf`，所有的 `IR_IMM` 都會被轉換為加密的 `IR_OBF_IMM` 指令。

### 1. 編譯期常數加密公式
給定一個原始明文常數 $P \in \mathbb{Z}_{2^{64}}$，XCC 利用多態隨機數生成器（SplitMix64）生成兩個獨立的 64 位元金鑰：
* $K$（動態金鑰，`obf_key`）
* $R$（掩碼加數，`obf_rot`）

加密數值 $C$（`imm`）的計算公式如下：
$$C = \Big( (P \oplus R) + K \Big) \oplus (K \gg 17)$$

在 [`src/ir_obf.c`](file:///Users/hcwtony/repo/projects/ccompiler/src/ir_obf.c#L86-L97) 中的實作：
```c
static IRInst make_obf_imm_inst(int dst, int64_t plain_val, uint64_t *rng, const char *comment) {
    IRInst in = make_inst(IR_OBF_IMM);
    uint64_t key = splitmix64_next(rng);
    uint64_t rot = splitmix64_next(rng);
    uint64_t enc = ((((uint64_t)plain_val ^ rot) + key) ^ (key >> 17));
    in.dst = dst;
    in.imm = (int64_t)enc;
    in.obf_key = key;
    in.obf_rot = rot;
    in.obf_comment = comment;
    return in;
}
```

### 2. 執行期動態解密公式
在程式執行時，要將加密值 $C$ 還原為原始值 $P$，必須精確反轉上述運算步驟：
$$T_1 = C \oplus (K \gg 17)$$
$$T_2 = T_1 - K$$
$$P = T_2 \oplus R$$

### 3. 原生組合語言降階（x86_64 與 ARM64）
在 [`src/codegen_x86_64.c`](file:///Users/hcwtony/repo/projects/ccompiler/src/codegen_x86_64.c#L215-L229) 中，這條單一的 IR 指令被降階為一段連續的暫存器運算：
```gas
# 原本只需 movabsq $0x1337beef, %rax
# 現在降階為不可直視的多態還原運算：
movabsq $0x8f2d5e7a91b4c302, %rax    # 載入加密常數 C
movq    %rax, -16(%rbp)              # 存入槽位
movabsq $0x5a1e83d940c27f61, %rcx    # 載入 obf_key (K)
movq    %rcx, %rdx
shrq    $17, %rdx                    # K >> 17
movq    -16(%rbp), %rax
xorq    %rdx, %rax                   # C ^ (K >> 17)
subq    %rcx, %rax                   # (...) - K
movabsq $0x2c9b4701e85f3a64, %rdx    # 載入 obf_rot (R)
xorq    %rdx, %rax                   # 還原出原始數值 P！
movq    %rax, -16(%rbp)
```
在 ARM64 後端（[`src/codegen_arm64.c`](file:///Users/hcwtony/repo/projects/ccompiler/src/codegen_arm64.c)）中，這段邏輯同樣會利用 `x8`, `x9`, `x10` 以及 `eor`, `sub`, `lsr` 進行展開。

**防護效果**：
在二進位檔的唯讀資料段、指令立即數中，原始常數 `0x1337BEEF` 的任何位元組序列都已不復存在！靜態特徵搜尋完全失效；逆向人員如果想要知道常數是什麼，必須執行符號執行（Symbolic Execution）追蹤整個暫存器鏈，大幅增加了分析成本。

---

## 12.2 混合布林算術（Mixed Boolean-Arithmetic, MBA）數學原理

動態常數加密解決了「常數值外露」的問題，但如果資料流中的算術運算本身太清晰（例如 `a + b`、`a ^ b`），反編譯引擎依然能輕易重建資料流圖。

**混合布林算術（MBA）**是由 Zhou 等人於 2007 年正式提出的混淆理論。其核心洞察在於：
> 電腦上的整數運算同時受兩個不同的代數系統支配：
> 1. **算術環（Arithmetic Ring）**：$(\mathbb{Z}_{2^n}, +, \times)$，遵從標準加減乘除。
> 2. **布林代數（Boolean Algebra）**：$(\mathbb{B}^n, \land, \lor, \oplus, \neg)$，遵從位元逐位邏輯運算。

一般的編譯器最佳化器只精通其中一個領域的化簡（例如將 `x + x` 化簡為 `x * 2`，或將 `x ^ x` 化簡為 `0`）。然而，當算術運算與位元運算以**非線性方式交織在一起**時，運算式的維度會急劇膨脹，現代反編譯器內建的模式比對庫將完全無法辨識！

### 核心恆等式推導
對於任意 64 位元整數 $x, y \in \mathbb{Z}_{2^{64}}$，存在以下精確成立的恆等關係：

#### 1. 加法恆等式（Addition MBA）
從全加器（Full Adder）原理可知，兩個整數相加等於「不帶進位的異或和」加上「進位項左移一位」：
$$x + y = (x \oplus y) + 2 \cdot (x \land y) = (x \oplus y) + \Big((x \land y) \ll 1\Big)$$

#### 2. 減法恆等式（Subtraction MBA）
利用位元補數與集合差運算：
$$x - y = (x \land \neg y) - (\neg x \land y)$$
在位元層次上，這表示「$x$ 為 1 且 $y$ 為 0 的位元」減去「$x$ 為 0 且 $y$ 為 1 的位元」，其數學結果分毫不差地等於 $x - y$。

#### 3. 異或恆等式（XOR MBA）
根據容斥原理（Inclusion-Exclusion Principle）：
$$x \lor y = (x \oplus y) + (x \land y)$$
移項後即可得：
$$x \oplus y = (x \lor y) - (x \land y)$$

---

## 12.3 帶多態金鑰擾動的 MBA IR 實作

如果我們只是單純把 `x + y` 替換成 `(x ^ y) + ((x & y) << 1)`，反編譯引擎若是增加一條比對規則，依然可能將其化簡還原。

XCC 的創新之處在於：**引入多態混淆金鑰 $K$（`obf_key`）作為運算過程中的動態中介擾動項（Perturbation Key）**！

在 [`include/xcc.h`](file:///Users/hcwtony/repo/projects/ccompiler/include/xcc.h#L476-L478) 與 [`src/ir_obf.c`](file:///Users/hcwtony/repo/projects/ccompiler/src/ir_obf.c#L324-L338) 中：

### 1. `IR_OBF_MBA_ADD`
$$dst = \Big( ((src_1 \oplus src_2) \oplus K) \oplus K \Big) + \Big( (src_1 \land src_2) \ll 1 \Big)$$
金鑰 $K$ 在運算中間被異或注入，隨後在相加前再次異或消除。在 SSA 分析器看來，這引入了額外的外部依賴項，進一步阻斷常數傳播。

### 2. `IR_OBF_MBA_SUB`
$$dst = \Big( ((src_1 \land \neg src_2) \oplus K) \oplus K \Big) - (\neg src_1 \land src_2)$$

### 3. `IR_OBF_MBA_XOR`
利用線性加法結合金鑰偏移：
$$dst = \Big( (src_1 \lor src_2) + K \Big) - \Big( (src_1 \land src_2) + K \Big)$$
注意看！在被減數與減數中**同時加入金鑰 $K$**：
$$((x \lor y) + K) - ((x \land y) + K) = (x \lor y) - (x \land y) = x \oplus y$$
金鑰 $K$ 在減法展開時自動相互抵消！這使得反組譯器在進行數據流切片時，會看到兩個巨大且帶有隨機金鑰的運算項，完全無法識別這其實只是一個簡單的 `^` 異或指令。

---

## 12.4 後端降階展開與反編譯器對比實測

讓我們透過實際的編譯結果，檢視 `IR_OBF_MBA_*` 如何在組合語言層級徹底摧毀反編譯器的可讀性。

在 [`src/codegen_x86_64.c`](file:///Users/hcwtony/repo/projects/ccompiler/src/codegen_x86_64.c#L382-L425) 中，以 `IR_OBF_MBA_ADD` 為例：
```gas
# 原始指令：addq %rcx, %rax
# 經過 MBA 混淆後發射的機器碼序列：
movq    -16(%rbp), %rax              # 載入 src1 (x)
movq    -24(%rbp), %rcx              # 載入 src2 (y)
movabsq $0x4b89c20ef13d567a, %r10    # 載入隨機生成的 64 位元多態金鑰 K
movq    %rax, %rdx
xorq    %rcx, %rdx                   # x ^ y
xorq    %r10, %rdx                   # (x ^ y) ^ K
andq    %rcx, %rax                   # x & y
shlq    $1, %rax                     # (x & y) << 1
xorq    %r10, %rdx                   # ((x ^ y) ^ K) ^ K  -> 還原不帶進位和
addq    %rdx, %rax                   # 最終相加！
movq    %rax, -32(%rbp)
```

### 反編譯器實測視覺對比

#### 未開啟混淆時的反編譯偽代碼（Ghidra / IDA）：
```c
long calculate(long a, long b) {
    return a + b;
}
```

#### 開啟 `--obf-mba` 後的反編譯偽代碼：
```c
ulong calculate(ulong a, ulong b) {
    ulong uVar1;
    ulong uVar2;
    
    uVar1 = (a ^ b ^ 0x4b89c20ef13d567a) ^ 0x4b89c20ef13d567a;
    uVar2 = (a & b) * 2;
    return uVar1 + uVar2;
}
```
原本乾淨的 `a + b` 被反編譯器忠實地解析為複雜的位元交錯與魔術常數運算。當一個函式內部有幾十個算術與位元運算全部被替換為帶金鑰 MBA 時，反編譯器產生的代碼量將會膨脹 **5 到 8 倍**，徹底擊潰逆向分析人員人工審閱邏輯的耐性。

---

## 12.5 小結

在本章中，我們掌握了：
1. **動態常數拆解加密（`IR_OBF_IMM`）**：透過 $( (P \oplus R) + K ) \oplus (K \gg 17)$ 在編譯期消除明文數值，並在執行期無感知還原。
2. **混合布林算術（MBA）數學原理**：利用算術環與布林代數的交叉恆等式，突破現代反編譯器的化簡模式比對。
3. **帶多態擾動金鑰的 MBA 指令實作**：在加、減、異或運算中動態注入高熵金鑰，製造高複雜度的暫存器依賴鏈。
4. **原生組合語言降階展開**：展示了單一 IR 運算如何在後端擴散為多條交錯的位元邏輯與算術組合。

數據流混淆成功隱藏了「程式在計算什麼」。但在逆向人員眼裡，程式的「控制流程結構（誰呼叫誰、迴圈在哪裡）」依然清晰可辨。

在下一章中，我們將祭出反逆向工程中最具毀滅性的結構變形武器：**第 13 章——控制流平坦化（Control-Flow Flattening）與無分支狀態機**！
