# 第 3 章：詞法分析器（Lexer）與精準錯誤診斷

> 「一個優秀編譯器的價值，一半在於它能產出高效的機械碼，另一半在於它能在開發者犯錯時，給出清晰、一針見血的錯誤提示。」

在前置處理器將 `#include`、巨集展開與條件編譯處理完畢後，進入編譯管線的將是一長串平坦的 C 原始碼文字。此時，電腦看到的只是一個個連續的字元（ASCII 位元組流），它尚不知道哪裡代表關鍵字 `int`、哪裡是變數名稱、哪裡是常數數值。

本章將帶領讀者實作編譯器的前端核心之一：**詞法分析器（Lexer，又稱 Tokenizer）**。我們將探討如何用單純的 C 語言字串指標走訪，切分出詞法單元（Token）、解析多進位數值與跳脫字元、處理相鄰字串合併，並建立**精確到檔名、行號、欄號（Row & Column）的原始碼診斷系統**。

---

## 3.1 Token 的結構設計與鏈結串列

詞法分析的目標是將字元流轉換為符號流。在 [`include/xcc.h`](file:///Users/hcwtony/repo/projects/ccompiler/include/xcc.h#L62-L180) 中，我們定義了所有可能的詞法單元種類 [`TokenKind`](file:///Users/hcwtony/repo/projects/ccompiler/include/xcc.h#L62-L165)：

```c
typedef enum {
    TOK_EOF = 0,
    
    /* 關鍵字 */
    TOK_VOID, TOK_CHAR, TOK_SHORT, TOK_INT, TOK_LONG,
    TOK_SIGNED, TOK_UNSIGNED, TOK_BOOL,
    TOK_STRUCT, TOK_UNION, TOK_ENUM, TOK_TYPEDEF,
    TOK_IF, TOK_ELSE, TOK_WHILE, TOK_DO, TOK_FOR,
    TOK_SWITCH, TOK_CASE, TOK_DEFAULT, TOK_BREAK, TOK_CONTINUE,
    TOK_RETURN, TOK_GOTO, TOK_SIZEOF,
    
    /* 識別字與字面量 */
    TOK_IDENT,      /* 變數名、函式名 */
    TOK_INT_LIT,    /* 42, 0x10, 0b101 */
    TOK_CHAR_LIT,   /* 'a', '\n' */
    TOK_STR_LIT,    /* "hello\n" */
    
    /* 運算子與標點符號 */
    TOK_PLUS,       /* + */
    TOK_MINUS,      /* - */
    TOK_STAR,       /* * */
    TOK_SLASH,      /* / */
    TOK_EQ,         /* == */
    TOK_NE,         /* != */
    TOK_ARROW,      /* -> */
    // ...
} TokenKind;
```

每個 Token 節點則封裝了種類、在原始文字中的指標與長度、數值載荷，以及指向下一個 Token 的指標：

```c
typedef struct Token Token;
struct Token {
    TokenKind kind;
    SourceLoc loc;        /* 記錄來源檔名、行號、欄號 */
    const char *str;      /* 指向原始碼文字片段的起點 */
    int len;              /* Token 文字長度 */
    
    int64_t int_val;      /* 數值字面量的值（TOK_INT_LIT / TOK_CHAR_LIT） */
    char *str_val;        /* 字串字面量解碼後的緩衝區（TOK_STR_LIT） */
    int str_len;          /* 字串長度（位元組數） */
    
    Token *next;          /* 單向鏈結串列指標 */
};
```

使用單向鏈結串列（Linked List）來組織 Token 是手寫編譯器的經典設計：語法分析器（Parser）只需透過一個指標 `tok = tok->next` 就能流暢地向前爬梳，且若需要回溯或 Lookahead，也能輕鬆暫存指標。

---

## 3.2 關鍵字與識別字的高效識別

在掃描時，識別字（Identifier）通常符合正規表達式 `[a-zA-Z_$][a-zA-Z0-9_$]*`。然而，像 `int`、`while`、`return` 雖然形式上也是識別字，但它們是保留關鍵字。

傳統做法常使用 `gperf` 或雜湊表，但在 XCC 這樣小巧的編譯器中，使用靜態比對陣列速度極快且代碼極度直觀：

在 [`src/lexer.c`](file:///Users/hcwtony/repo/projects/ccompiler/src/lexer.c#L65-L115) 中：
```c
static TokenKind check_keyword(const char *s, int len) {
    static const struct {
        const char *kw;
        TokenKind kind;
    } kws[] = {
        {"void",     TOK_VOID},     {"char",     TOK_CHAR},
        {"int",      TOK_INT},      {"long",     TOK_LONG},
        {"struct",   TOK_STRUCT},   {"union",    TOK_UNION},
        {"if",       TOK_IF},       {"else",     TOK_ELSE},
        {"while",    TOK_WHILE},    {"for",      TOK_FOR},
        {"return",   TOK_RETURN},   {"sizeof",   TOK_SIZEOF},
        // ...
    };
    for (size_t i = 0; i < sizeof(kws) / sizeof(kws[0]); i++) {
        if ((int)strlen(kws[i].kw) == len && memcmp(kws[i].kw, s, len) == 0) {
            return kws[i].kind;
        }
    }
    return TOK_IDENT; // 找不到則視為一般識別字
}
```

---

## 3.3 數值字面量：十進位、十六進位、二進位與八進位

現代 C 語言（特別是系統程式設計與加密演算法）大量使用非十進位整數。XCC 的詞法分析器完整支援了四種進位制解析：

在 [`src/lexer.c`](file:///Users/hcwtony/repo/projects/ccompiler/src/lexer.c#L140-L177) 中：
* **十六進位（`0x` / `0X`）**：
  若開頭為 `0x`，走訪後續十六進位字元，使用 `val = (val << 4) | hex_val(*p++)`。
* **二進位（`0b` / `0B`）**：
  C23/GCC 擴充常數，若開頭為 `0b`，走訪後續 `'0'` 與 `'1'`，使用 `val = (val << 1) | (*p++ - '0')`。
* **八進位（`0` 開頭）**：
  若首字為 `'0'` 且後方接續數字，按照八進位位移 `val = (val << 3) | (*p++ - '0')`。
* **十進位**：
  標準累加 `val = val * 10 + (*p++ - '0')`。
* **後綴過濾（Suffixes）**：
  解析完畢後，主動吃掉結尾的 `u`、`U`、`l`、`L`、`ll`、`LL` 等整數修飾後綴。

---

## 3.4 字元與字串：跳脫序列與相鄰字串自動合併

字元與字串的解析是詞法分析最容易出錯的地方，主要是因為 C 語言支援多種跳脫序列（Escape Sequences），如 `\n`、`\t`、`\x1b`、八進位 `\033` 等。

### 跳脫字元解析函式
在 [`src/lexer.c`](file:///Users/hcwtony/repo/projects/ccompiler/src/lexer.c#L21-L63) 中，我們實作了 `read_escape_char`：
```c
static char read_escape_char(const char **p, SourceLoc loc) {
    const char *s = *p;
    char c = *s++;
    switch (c) {
        case 'n':  *p = s; return '\n';
        case 't':  *p = s; return '\t';
        case 'r':  *p = s; return '\r';
        case '0':  *p = s; return '\0';
        case '\\': *p = s; return '\\';
        case '\'': *p = s; return '\'';
        case '\"': *p = s; return '\"';
        case 'x': { // 十六進位跳脫: \x41 -> 'A'
            int v = 0;
            while (isxdigit((unsigned char)*s)) {
                v = (v << 4) | hex_val(*s++);
            }
            *p = s;
            return (char)v;
        }
        default: // 八進位跳脫 \123 或未定義字元
            // ...
            *p = s;
            return c;
    }
}
```

### C 語言經典特性：相鄰字串自動合併（Adjacent Concatenation）
在 C 語言中，以下寫法是合法的，且在編譯後必須合併為單一字串：
```c
const char *msg = "Hello, " "World! "
                  "Welcome to XCC.";
```
這在巨集展開與格式化輸出時極為常見。XCC 的詞法分析器巧妙地在讀取字串時加入了 **Lookahead 迴圈**：
```c
while (*p == '"') {
    // 1. 讀取雙引號內的字串並填入動態緩衝區 buf
    // 2. 略過後續空白、定位字元與換行
    const char *look = p;
    while (*look && isspace((unsigned char)*look)) look++;
    if (*look == '"') {
        p = look; // 發現下一個相鄰字串！繼續讀取並附加到 buf
    } else {
        break;    // 沒有相鄰字串，結束解析
    }
}
```
這個優雅的設計直接在詞法階段消滅了字串碎片的困擾，讓後續的語法分析器只需將其視為單一的 `TOK_STR_LIT`。

---

## 3.5 精準錯誤診斷系統（Error Diagnostics）

許多手寫編譯器最為人詬病的是：一旦語法出錯，只會印出冷冰冰的 `syntax error`，甚至連在哪個檔案、第幾行都沒寫。

在 XCC 中，精準診斷是一等公民（First-Class Citizen）。我們在 [`include/xcc.h`](file:///Users/hcwtony/repo/projects/ccompiler/include/xcc.h#L41-L48) 中定義了 `SourceLoc`，並在 [`src/type.c`](file:///Users/hcwtony/repo/projects/ccompiler/src/type.c#L96-L115) 實作了統一的格式化報錯：

```c
typedef struct {
    const char *filename;
    int line;
    int col;
} SourceLoc;

void error_at(SourceLoc loc, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s:%d:%d: error: ",
            loc.filename ? loc.filename : "<input>",
            loc.line, loc.col);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}
```

當 Lexer 在掃描時，每遇到換行字元 `\n`，便將 `line` 遞增並重設 `col = 1`；其餘字元則依長度累加 `col`。無論是詞法階段的「未閉合的字串字面量」、語法階段的「未宣告的識別字」、還是型別階段的「無效指標運算」，都會統一印出如同 GCC/Clang 般清爽標準的錯誤資訊：
```
examples/demo.c:42:15: error: invalid hex escape sequence
```

---

## 3.6 第一篇總結與完整管線串連

至此，我們已經完成了編譯器的第一篇：**前端基礎建設**！

讓我們回顧這三章所構建的堅固地基：
1. **全域視野（第 1 章）**：確立了 XCC 純 C11、零依賴、單一標頭檔的模組化架構，並定義了跨越 ARM64/x86_64 與 macOS/Linux/Windows 的目標矩陣。
2. **前置處理（第 2 章）**：透過虛擬內建標準標頭檔（`<stdio.h>`, `<stdlib.h>` 等），徹底擺脫了宿主系統標頭檔的語法污染，同時支援了巨集展開與條件編譯。
3. **詞法分析（第 3 章）**：成功將文字流化為帶有精準座標的 Token 鏈結串列，並完整支援了多進位整數、跳脫字元與相鄰字串合併。

輸入是一串原始的 C 原始碼檔案，現在輸出已經變成整齊劃一、帶有豐富型別與位置屬性的 Token 串列。

在**第二篇（第 4～6 章）**中，我們將跨入編譯器最核心的語義大腦——**型別系統（Type System）與遞迴下降語法分析器（Parser）**，探討如何把 Token 構築成抽象語法樹（AST），並處理複雜的結構體對齊與指標縮放算術！
