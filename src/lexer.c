#include "xcc.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static bool is_ident1(char c) {
    return isalpha((unsigned char)c) || c == '_' || c == '$';
}

static bool is_ident2(char c) {
    return isalnum((unsigned char)c) || c == '_' || c == '$';
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char read_escape_char(const char **p, SourceLoc loc) {
    const char *s = *p;
    char c = *s++;
    switch (c) {
        case 'a':  *p = s; return '\a';
        case 'b':  *p = s; return '\b';
        case 't':  *p = s; return '\t';
        case 'n':  *p = s; return '\n';
        case 'v':  *p = s; return '\v';
        case 'f':  *p = s; return '\f';
        case 'r':  *p = s; return '\r';
        case 'e':  *p = s; return 27;
        case '\\': *p = s; return '\\';
        case '\'': *p = s; return '\'';
        case '"':  *p = s; return '"';
        case '?':  *p = s; return '?';
        case 'x': {
            if (!isxdigit((unsigned char)*s)) {
                error_at(loc, "invalid hex escape sequence");
            }
            int v = 0;
            while (isxdigit((unsigned char)*s)) {
                v = (v << 4) | hex_val(*s++);
            }
            *p = s;
            return (char)v;
        }
        default:
            if (c >= '0' && c <= '7') {
                int v = c - '0';
                if (*s >= '0' && *s <= '7') {
                    v = (v << 3) | (*s++ - '0');
                    if (*s >= '0' && *s <= '7') {
                        v = (v << 3) | (*s++ - '0');
                    }
                }
                *p = s;
                return (char)v;
            }
            *p = s;
            return c;
    }
}

static TokenKind check_keyword(const char *s, int len) {
    static const struct {
        const char *kw;
        TokenKind kind;
    } kws[] = {
        {"void",     TOK_VOID},
        {"char",     TOK_CHAR},
        {"short",    TOK_SHORT},
        {"int",      TOK_INT},
        {"long",     TOK_LONG},
        {"signed",   TOK_SIGNED},
        {"unsigned", TOK_UNSIGNED},
        {"_Bool",    TOK_BOOL},
        {"bool",     TOK_BOOL},
        {"struct",   TOK_STRUCT},
        {"union",    TOK_UNION},
        {"enum",     TOK_ENUM},
        {"typedef",  TOK_TYPEDEF},
        {"static",   TOK_STATIC},
        {"extern",   TOK_EXTERN},
        {"const",    TOK_CONST},
        {"sizeof",   TOK_SIZEOF},
        {"if",       TOK_IF},
        {"else",     TOK_ELSE},
        {"while",    TOK_WHILE},
        {"do",       TOK_DO},
        {"for",      TOK_FOR},
        {"switch",   TOK_SWITCH},
        {"case",     TOK_CASE},
        {"default",  TOK_DEFAULT},
        {"break",    TOK_BREAK},
        {"continue", TOK_CONTINUE},
        {"return",   TOK_RETURN},
        {"goto",     TOK_GOTO},
    };
    for (size_t i = 0; i < sizeof(kws) / sizeof(kws[0]); i++) {
        if ((int)strlen(kws[i].kw) == len && strncmp(kws[i].kw, s, (size_t)len) == 0) {
            return kws[i].kind;
        }
    }
    return TOK_IDENT;
}

static Token *new_token(TokenKind kind, SourceLoc loc, const char *lexeme, int len) {
    Token *tok = (Token *)calloc(1, sizeof(Token));
    tok->kind = kind;
    tok->loc = loc;
    tok->lexeme = lexeme;
    tok->len = len;
    return tok;
}

Token *tokenize(const char *filename, const char *source) {
    Token head = {0};
    Token *cur = &head;
    const char *p = source;
    int line = 1;
    int col = 1;

    while (*p) {
        if (*p == '\n') {
            p++;
            line++;
            col = 1;
            continue;
        }
        if (isspace((unsigned char)*p)) {
            p++;
            col++;
            continue;
        }

        SourceLoc loc = { .filename = filename, .line = line, .col = col };

        /* Numeric literal */
        if (isdigit((unsigned char)*p)) {
            const char *start = p;
            int64_t val = 0;
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
                p += 2;
                if (!isxdigit((unsigned char)*p)) {
                    error_at(loc, "invalid hexadecimal literal");
                }
                while (isxdigit((unsigned char)*p)) {
                    val = (val << 4) | hex_val(*p++);
                }
            } else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) {
                p += 2;
                if (*p != '0' && *p != '1') {
                    error_at(loc, "invalid binary literal");
                }
                while (*p == '0' || *p == '1') {
                    val = (val << 1) | (*p++ - '0');
                }
            } else if (p[0] == '0') {
                while (*p >= '0' && *p <= '7') {
                    val = (val << 3) | (*p++ - '0');
                }
            } else {
                while (isdigit((unsigned char)*p)) {
                    val = val * 10 + (*p++ - '0');
                }
            }
            while (*p == 'u' || *p == 'U' || *p == 'l' || *p == 'L') {
                p++;
            }
            int len = (int)(p - start);
            Token *tok = new_token(TOK_INT_LIT, loc, start, len);
            tok->int_val = val;
            cur = cur->next = tok;
            col += len;
            continue;
        }

        /* Character literal */
        if (*p == '\'') {
            const char *start = p++;
            if (*p == '\0' || *p == '\n') {
                error_at(loc, "unclosed character literal");
            }
            char c;
            if (*p == '\\') {
                p++;
                c = read_escape_char(&p, loc);
            } else {
                c = *p++;
            }
            if (*p != '\'') {
                error_at(loc, "multi-character or unclosed character literal");
            }
            p++;
            int len = (int)(p - start);
            Token *tok = new_token(TOK_CHAR_LIT, loc, start, len);
            tok->int_val = (int64_t)(unsigned char)c;
            cur = cur->next = tok;
            col += len;
            continue;
        }

        /* String literal (with automatic concatenation of adjacent string literals) */
        if (*p == '"') {
            const char *start = p;
            size_t cap = 64;
            size_t slen = 0;
            char *buf = (char *)malloc(cap);

            while (*p == '"') {
                p++;
                col++;
                while (*p && *p != '"') {
                    if (*p == '\n') {
                        error_at(loc, "unclosed string literal");
                    }
                    char c;
                    if (*p == '\\') {
                        const char *esc_start = p;
                        p++;
                        c = read_escape_char(&p, loc);
                        col += (int)(p - esc_start);
                    } else {
                        c = *p++;
                        col++;
                    }
                    if (slen + 2 > cap) {
                        cap *= 2;
                        buf = (char *)realloc(buf, cap);
                    }
                    buf[slen++] = c;
                }
                if (*p != '"') {
                    error_at(loc, "unclosed string literal");
                }
                p++;
                col++;

                /* Look ahead for adjacent string literal */
                const char *look = p;
                int tmp_line = line;
                int tmp_col = col;
                while (*look && isspace((unsigned char)*look)) {
                    if (*look == '\n') {
                        tmp_line++;
                        tmp_col = 1;
                    } else {
                        tmp_col++;
                    }
                    look++;
                }
                if (*look == '"') {
                    p = look;
                    line = tmp_line;
                    col = tmp_col;
                }
            }
            buf[slen] = '\0';
            Token *tok = new_token(TOK_STR_LIT, loc, start, (int)(p - start));
            tok->str_val = buf;
            tok->str_len = (int)slen;
            cur = cur->next = tok;
            continue;
        }

        /* Identifier or Keyword */
        if (is_ident1(*p)) {
            const char *start = p;
            while (is_ident2(*p)) p++;
            int len = (int)(p - start);
            /* Ignore GCC/Clang __attribute__((...)) or restrict/inline keywords gracefully */
            if ((len == 8 && strncmp(start, "restrict", 8) == 0) ||
                (len == 10 && strncmp(start, "__restrict", 10) == 0) ||
                (len == 6 && strncmp(start, "inline", 6) == 0) ||
                (len == 8 && strncmp(start, "__inline", 8) == 0) ||
                (len == 8 && strncmp(start, "volatile", 8) == 0) ||
                (len == 8 && strncmp(start, "register", 8) == 0)) {
                col += len;
                continue;
            }
            TokenKind kind = check_keyword(start, len);
            Token *tok = new_token(kind, loc, start, len);
            if (kind == TOK_IDENT) {
                tok->str_val = (char *)malloc((size_t)len + 1);
                memcpy(tok->str_val, start, (size_t)len);
                tok->str_val[len] = '\0';
            }
            cur = cur->next = tok;
            col += len;
            continue;
        }

        /* 3-character operators */
        if (strncmp(p, "...", 3) == 0) {
            cur = cur->next = new_token(TOK_ELLIPSIS, loc, p, 3);
            p += 3; col += 3; continue;
        }
        if (strncmp(p, "<<=", 3) == 0) {
            cur = cur->next = new_token(TOK_SHL_ASSIGN, loc, p, 3);
            p += 3; col += 3; continue;
        }
        if (strncmp(p, ">>=", 3) == 0) {
            cur = cur->next = new_token(TOK_SHR_ASSIGN, loc, p, 3);
            p += 3; col += 3; continue;
        }

        /* 2-character operators */
        static const struct {
            const char *op;
            TokenKind kind;
        } ops2[] = {
            {"->", TOK_ARROW},
            {"++", TOK_INC},
            {"--", TOK_DEC},
            {"<<", TOK_LSHIFT},
            {">>", TOK_RSHIFT},
            {"<=", TOK_LE},
            {">=", TOK_GE},
            {"==", TOK_EQ},
            {"!=", TOK_NE},
            {"&&", TOK_LOGAND},
            {"||", TOK_LOGOR},
            {"+=", TOK_ADD_ASSIGN},
            {"-=", TOK_SUB_ASSIGN},
            {"*=", TOK_MUL_ASSIGN},
            {"/=", TOK_DIV_ASSIGN},
            {"%=", TOK_MOD_ASSIGN},
            {"&=", TOK_AND_ASSIGN},
            {"|=", TOK_OR_ASSIGN},
            {"^=", TOK_XOR_ASSIGN},
        };
        bool matched2 = false;
        for (size_t i = 0; i < sizeof(ops2) / sizeof(ops2[0]); i++) {
            if (p[0] == ops2[i].op[0] && p[1] == ops2[i].op[1]) {
                cur = cur->next = new_token(ops2[i].kind, loc, p, 2);
                p += 2;
                col += 2;
                matched2 = true;
                break;
            }
        }
        if (matched2) continue;

        /* 1-character operators */
        TokenKind single = TOK_EOF;
        switch (*p) {
            case '(': single = TOK_LPAREN; break;
            case ')': single = TOK_RPAREN; break;
            case '{': single = TOK_LBRACE; break;
            case '}': single = TOK_RBRACE; break;
            case '[': single = TOK_LBRACKET; break;
            case ']': single = TOK_RBRACKET; break;
            case ';': single = TOK_SEMI; break;
            case ',': single = TOK_COMMA; break;
            case '.': single = TOK_DOT; break;
            case '?': single = TOK_QUESTION; break;
            case ':': single = TOK_COLON; break;
            case '+': single = TOK_PLUS; break;
            case '-': single = TOK_MINUS; break;
            case '*': single = TOK_STAR; break;
            case '/': single = TOK_SLASH; break;
            case '%': single = TOK_PERCENT; break;
            case '&': single = TOK_AMP; break;
            case '|': single = TOK_PIPE; break;
            case '^': single = TOK_CARET; break;
            case '~': single = TOK_TILDE; break;
            case '!': single = TOK_BANG; break;
            case '<': single = TOK_LT; break;
            case '>': single = TOK_GT; break;
            case '=': single = TOK_ASSIGN; break;
            default:
                error_at(loc, "unexpected character '%c' (0x%02x)", *p, (unsigned char)*p);
        }
        cur = cur->next = new_token(single, loc, p, 1);
        p++;
        col++;
    }

    SourceLoc eof_loc = { .filename = filename, .line = line, .col = col };
    cur->next = new_token(TOK_EOF, eof_loc, p, 0);
    return head.next;
}

const char *token_kind_name(TokenKind kind) {
    switch (kind) {
        case TOK_EOF: return "EOF";
        case TOK_IDENT: return "identifier";
        case TOK_INT_LIT: return "integer literal";
        case TOK_CHAR_LIT: return "char literal";
        case TOK_STR_LIT: return "string literal";
        case TOK_LPAREN: return "(";
        case TOK_RPAREN: return ")";
        case TOK_LBRACE: return "{";
        case TOK_RBRACE: return "}";
        case TOK_LBRACKET: return "[";
        case TOK_RBRACKET: return "]";
        case TOK_SEMI: return ";";
        case TOK_COMMA: return ",";
        default: return "token";
    }
}
