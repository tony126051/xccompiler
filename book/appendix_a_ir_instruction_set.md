# 附錄 A：XCC 三址碼 IR 與抗逆向強化指令集速查表

本附錄整理 XCC 編譯器在 [`include/xcc.h`](file:///Users/hcwtony/repo/projects/ccompiler/include/xcc.h) 中定義的完整中間表示（IR）指令集，包含標準三址碼指令與專屬抗逆向強化指令。

---

## A.1 核心資料結構定義

```c
typedef struct {
    IROp op;              /* 操作碼 (IROp) */
    int dst;              /* 目的虛擬暫存器編號 (%v0, %v1...)，若無則為 -1 */
    int src1;             /* 來源虛擬暫存器 1 編號，若無則為 -1 */
    int src2;             /* 來源虛擬暫存器 2 編號，若無則為 -1 */
    int64_t imm;          /* 立即數常數、插槽 ID、字串 ID 或引數索引 */
    int label;            /* 跳躍目標或定義的標籤 ID (.L<label>) */
    int mem_size;         /* 記憶體存取或運算寬度 (1, 2, 4, 8 位元組) */
    bool is_unsigned;     /* 是否為無號數運算 */
    const char *sym_name; /* 全域符號或呼叫函式名稱 */
    int call_arg_count;   /* 函式呼叫總引數個數 */
    int call_fixed_count; /* 固定參數個數 (專為 Apple ARM64 ABI 設計) */
    bool call_is_variadic;/* 是否為可變參數呼叫 (如 printf) */

    /* Custom Anti-Reversing 強化屬性 */
    uint64_t obf_key;     /* 多態混淆金鑰 (由 SplitMix64 隨機產生) */
    uint64_t obf_rot;     /* 二次掩碼加數 (用於 IR_OBF_IMM) */
    const char *obf_comment; /* 診斷註解 (--emit-ir 顯示) */
} IRInst;
```

---

## A.2 標準三址碼操作碼（Standard TAC Ops）

| 操作碼 (`IROp`) | 語意說明 | 運算元用法 | 典型發射範例 |
| :--- | :--- | :--- | :--- |
| `IR_NOP` | 空操作 | 無 | `nop` |
| `IR_IMM` | 載入 64 位元常數 | `dst = imm` | `%v0 = imm 42` |
| `IR_STR_ADDR` | 取得字串字面量唯讀位址 | `dst = &str[imm]` | `%v1 = str_addr #0` |
| `IR_LOCAL_ADDR` | 取得局部變數堆疊插槽位址 | `dst = &locals[imm]` | `%v2 = local_addr #0` |
| `IR_GLOBAL_ADDR`| 取得全域符號位址 | `dst = &sym_name` | `%v3 = global_addr @g_val` |
| `IR_MOV` | 暫存器賦值拷貝 | `dst = src1` | `%v4 = mov %v3` |
| `IR_LOAD` | 記憶體讀取 (支援 1/2/4/8 byte) | `dst = *[src1]` | `%v5 = load.4 [%v2]` |
| `IR_STORE` | 記憶體寫入 (支援 1/2/4/8 byte) | `*[src1] = src2` | `store.4 [%v2], %v5` |
| `IR_MEMCPY` | 結構體值拷貝 (固定位元組數) | `memcpy(src1, src2, imm)` | `memcpy [%v0], [%v1], #16` |
| `IR_NEG` | 算術取負數 | `dst = -src1` | `%v6 = neg %v5` |
| `IR_BITNOT` | 位元反相 (一補數) | `dst = ~src1` | `%v7 = not %v5` |
| `IR_LOGNOT` | 邏輯非 (轉為 0 或 1) | `dst = !src1` | `%v8 = lognot %v5` |
| `IR_CAST` | 型別大小與符號轉換 | `dst = (cast) src1` | `%v9 = cast.8.s %v5` |
| `IR_ADD` | 算術加法 (純量或已縮放指標) | `dst = src1 + src2` | `%v10 = add %v1, %v2` |
| `IR_SUB` | 算術減法 | `dst = src1 - src2` | `%v11 = sub %v1, %v2` |
| `IR_MUL` | 算術乘法 | `dst = src1 * src2` | `%v12 = mul %v1, %v2` |
| `IR_DIV` | 帶號／無號整數除法 | `dst = src1 / src2` | `%v13 = sdiv %v1, %v2` |
| `IR_MOD` | 帶號／無號整數取餘數 | `dst = src1 % src2` | `%v14 = smod %v1, %v2` |
| `IR_BITAND` | 位元與 (AND) | `dst = src1 & src2` | `%v15 = and %v1, %v2` |
| `IR_BITOR` | 位元或 (OR) | `dst = src1 \| src2` | `%v16 = or %v1, %v2` |
| `IR_BITXOR` | 位元異或 (XOR) | `dst = src1 ^ src2` | `%v17 = xor %v1, %v2` |
| `IR_SHL` | 邏輯左移 | `dst = src1 << src2` | `%v18 = shl %v1, %v2` |
| `IR_SHR` | 算術／邏輯右移 | `dst = src1 >> src2` | `%v19 = ashr %v1, %v2` |
| `IR_EQ` | 相等比較 | `dst = (src1 == src2)` | `%v20 = cmp.eq %v1, %v2` |
| `IR_NE` | 不相等比較 | `dst = (src1 != src2)` | `%v21 = cmp.ne %v1, %v2` |
| `IR_LT` | 小於比較 (帶號／無號) | `dst = (src1 < src2)` | `%v22 = cmp.slt %v1, %v2` |
| `IR_LE` | 小於等於比較 | `dst = (src1 <= src2)` | `%v23 = cmp.sle %v1, %v2` |
| `IR_GT` | 大於比較 | `dst = (src1 > src2)` | `%v24 = cmp.sgt %v1, %v2` |
| `IR_GE` | 大於等於比較 | `dst = (src1 >= src2)` | `%v25 = cmp.sge %v1, %v2` |
| `IR_LABEL` | 標籤定義 | `.L<label>:` | `.L1:` |
| `IR_JMP` | 無條件跳躍 | `goto .L<label>` | `jmp .L1` |
| `IR_JZ` | 為零跳躍 (條件假) | `if (src1 == 0) goto .L` | `jz %v20, .L2` |
| `IR_JNZ` | 非零跳躍 (條件真) | `if (src1 != 0) goto .L` | `jnz %v20, .L2` |
| `IR_ARG` | 函式引數排程準備 | `arg[imm] = src1` | `arg #0 = %v0` |
| `IR_CALL` | 函式呼叫與回傳接收 | `dst = call sym(args)` | `%v26 = call printf` |
| `IR_RET` | 函式回傳值並退出 | `return src1` | `ret %v26` |

---

## A.3 抗逆向專屬強化操作碼（Hardened Anti-Reversing Ops）

| 操作碼 (`IROp`) | 核心數學原理與展開語意 | 防禦目標 |
| :--- | :--- | :--- |
| **`IR_OBF_IMM`** | $dst = \Big( (imm \oplus (K \gg 17)) - K \Big) \oplus R$ | **消滅明文常數**。消除指令立即數特徵，執行期動態還原。 |
| **`IR_OBF_MBA_ADD`** | $dst = \Big( ((s1 \oplus s2) \oplus K) \oplus K \Big) + \Big( (s1 \land s2) \ll 1 \Big)$ | **粉碎代數化簡器**。以全加器原理結合多態金鑰 $K$ 展開加法。 |
| **`IR_OBF_MBA_SUB`** | $dst = \Big( ((s1 \land \neg s2) \oplus K) \oplus K \Big) - (\neg s1 \land s2)$ | **粉碎代數化簡器**。利用位元補數差恆等式展開減法。 |
| **`IR_OBF_MBA_XOR`** | $dst = \Big( (s1 \lor s2) + K \Big) - \Big( (s1 \land s2) + K \Big)$ | **破壞特徵識別**。利用容斥原理在被減數與減數同時注入抵消金鑰 $K$。 |
| **`IR_OBF_OPAQUE_TRUE`** | $dst = \Big( \big( (s1 \oplus K) \cdot ((s1 \oplus K) + 1) \big) \ \& \ 1 \Big) == 0 \implies \mathbf{1}$ | **數論恆真謂詞**。利用連續整數乘積必為偶數的性質，靜態分析無法化簡。 |
| **`IR_OBF_OPAQUE_FALSE`**| $dst = \Big( \big( (s1 \oplus K) \cdot ((s1 \oplus K) + 1) \big) \ \& \ 1 \Big) \neq 0 \implies \mathbf{0}$ | **數論恆假謂詞**。引導條件分支跳入幽靈基本塊，引發 SMT 路徑爆炸。 |
| **`IR_OBF_JUNK`** | $\big( (\text{rsp} \oplus K) \ggg 13 \big) \oplus \text{self} \implies 0$ | **暫存器熵值擾動**。干擾反編譯器的暫存器分配與變數活體分析。 |
