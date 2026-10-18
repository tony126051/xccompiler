#include "xcc.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Built-in Standard Headers for Cross-Platform Portability
 * ============================================================================ */

typedef struct {
    const char *name;
    const char *content;
} BuiltinHeader;

static const BuiltinHeader k_builtin_headers[] = {
    {
        "stddef.h",
        "#ifndef _XCC_STDDEF_H\n"
        "#define _XCC_STDDEF_H\n"
        "#define NULL ((void*)0)\n"
        "typedef unsigned long size_t;\n"
        "typedef long ptrdiff_t;\n"
        "#endif\n"
    },
    {
        "stdbool.h",
        "#ifndef _XCC_STDBOOL_H\n"
        "#define _XCC_STDBOOL_H\n"
        "#define bool _Bool\n"
        "#define true 1\n"
        "#define false 0\n"
        "#endif\n"
    },
    {
        "stdint.h",
        "#ifndef _XCC_STDINT_H\n"
        "#define _XCC_STDINT_H\n"
        "typedef char int8_t;\n"
        "typedef unsigned char uint8_t;\n"
        "typedef short int16_t;\n"
        "typedef unsigned short uint16_t;\n"
        "typedef int int32_t;\n"
        "typedef unsigned int uint32_t;\n"
        "typedef long int64_t;\n"
        "typedef unsigned long uint64_t;\n"
        "typedef long intptr_t;\n"
        "typedef unsigned long uintptr_t;\n"
        "#endif\n"
    },
    {
        "stdio.h",
        "#ifndef _XCC_STDIO_H\n"
        "#define _XCC_STDIO_H\n"
        "#include <stddef.h>\n"
        "#define EOF (-1)\n"
        "#define SEEK_SET 0\n"
        "#define SEEK_CUR 1\n"
        "#define SEEK_END 2\n"
        "typedef struct _FILE FILE;\n"
        "int printf(const char *fmt, ...);\n"
        "int fprintf(FILE *stream, const char *fmt, ...);\n"
        "int sprintf(char *str, const char *fmt, ...);\n"
        "int snprintf(char *str, size_t size, const char *fmt, ...);\n"
        "int scanf(const char *fmt, ...);\n"
        "int sscanf(const char *str, const char *fmt, ...);\n"
        "int puts(const char *s);\n"
        "int putchar(int c);\n"
        "int getchar(void);\n"
        "FILE *fopen(const char *path, const char *mode);\n"
        "int fclose(FILE *stream);\n"
        "size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);\n"
        "size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);\n"
        "int fseek(FILE *stream, long offset, int whence);\n"
        "long ftell(FILE *stream);\n"
        "void rewind(FILE *stream);\n"
        "int fflush(FILE *stream);\n"
        "void perror(const char *s);\n"
        "#endif\n"
    },
    {
        "stdlib.h",
        "#ifndef _XCC_STDLIB_H\n"
        "#define _XCC_STDLIB_H\n"
        "#include <stddef.h>\n"
        "#define EXIT_SUCCESS 0\n"
        "#define EXIT_FAILURE 1\n"
        "void *malloc(size_t size);\n"
        "void *calloc(size_t nmemb, size_t size);\n"
        "void *realloc(void *ptr, size_t size);\n"
        "void free(void *ptr);\n"
        "void exit(int status);\n"
        "void abort(void);\n"
        "int atoi(const char *nptr);\n"
        "long atol(const char *nptr);\n"
        "int abs(int j);\n"
        "long labs(long j);\n"
        "int rand(void);\n"
        "void srand(unsigned int seed);\n"
        "char *getenv(const char *name);\n"
        "int system(const char *command);\n"
        "#endif\n"
    },
    {
        "string.h",
        "#ifndef _XCC_STRING_H\n"
        "#define _XCC_STRING_H\n"
        "#include <stddef.h>\n"
        "size_t strlen(const char *s);\n"
        "int strcmp(const char *s1, const char *s2);\n"
        "int strncmp(const char *s1, const char *s2, size_t n);\n"
        "char *strcpy(char *dst, const char *src);\n"
        "char *strncpy(char *dst, const char *src, size_t n);\n"
        "char *strcat(char *dst, const char *src);\n"
        "char *strncat(char *dst, const char *src, size_t n);\n"
        "char *strchr(const char *s, int c);\n"
        "char *strrchr(const char *s, int c);\n"
        "char *strstr(const char *haystack, const char *needle);\n"
        "char *strdup(const char *s);\n"
        "void *memcpy(void *dst, const void *src, size_t n);\n"
        "void *memmove(void *dst, const void *src, size_t n);\n"
        "void *memset(void *s, int c, size_t n);\n"
        "int memcmp(const void *s1, const void *s2, size_t n);\n"
        "#endif\n"
    },
    {
        "ctype.h",
        "#ifndef _XCC_CTYPE_H\n"
        "#define _XCC_CTYPE_H\n"
        "int isalpha(int c);\n"
        "int isdigit(int c);\n"
        "int isalnum(int c);\n"
        "int isspace(int c);\n"
        "int isupper(int c);\n"
        "int islower(int c);\n"
        "int isxdigit(int c);\n"
        "int toupper(int c);\n"
        "int tolower(int c);\n"
        "#endif\n"
    },
    {
        "assert.h",
        "#ifndef _XCC_ASSERT_H\n"
        "#define _XCC_ASSERT_H\n"
        "void exit(int status);\n"
        "int printf(const char *fmt, ...);\n"
        "#define assert(cond) do { if (!(cond)) { printf(\"Assertion failed: %s\\n\", #cond); exit(1); } } while (0)\n"
        "#endif\n"
    },
    {
        "unistd.h",
        "#ifndef _XCC_UNISTD_H\n"
        "#define _XCC_UNISTD_H\n"
        "#include <stddef.h>\n"
        "typedef long ssize_t;\n"
        "ssize_t read(int fd, void *buf, size_t count);\n"
        "ssize_t write(int fd, const void *buf, size_t count);\n"
        "int close(int fd);\n"
        "int unlink(const char *pathname);\n"
        "#endif\n"
    },
};

/* ============================================================================
 * Dynamic String Builder
 * ============================================================================ */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

static void sb_init(StrBuf *sb) {
    sb->cap = 1024;
    sb->len = 0;
    sb->data = (char *)malloc(sb->cap);
    sb->data[0] = '\0';
}

static void sb_reserve(StrBuf *sb, size_t extra) {
    if (sb->len + extra + 1 > sb->cap) {
        while (sb->len + extra + 1 > sb->cap) {
            sb->cap *= 2;
        }
        sb->data = (char *)realloc(sb->data, sb->cap);
    }
}

static void sb_append_char(StrBuf *sb, char c) {
    sb_reserve(sb, 1);
    sb->data[sb->len++] = c;
    sb->data[sb->len] = '\0';
}

static void sb_append_n(StrBuf *sb, const char *s, size_t n) {
    sb_reserve(sb, n);
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

static void sb_append(StrBuf *sb, const char *s) {
    sb_append_n(sb, s, strlen(s));
}

/* ============================================================================
 * Macro Table
 * ============================================================================ */

typedef struct Macro {
    char *name;
    bool is_func;
    int param_count;
    char **params;
    char *body;
    struct Macro *next;
} Macro;

typedef struct {
    Macro *macros;
    const PreprocessorConfig *cfg;
    char *once_files[256];
    int once_count;
} PPContext;

static bool is_ident_start(char c) {
    return isalpha((unsigned char)c) || c == '_';
}

static bool is_ident_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

static Macro *find_macro(PPContext *ctx, const char *name, size_t len) {
    for (Macro *m = ctx->macros; m; m = m->next) {
        if (strlen(m->name) == len && strncmp(m->name, name, len) == 0) {
            return m;
        }
    }
    return NULL;
}

static void undef_macro(PPContext *ctx, const char *name, size_t len) {
    Macro **pp = &ctx->macros;
    while (*pp) {
        Macro *m = *pp;
        if (strlen(m->name) == len && strncmp(m->name, name, len) == 0) {
            *pp = m->next;
            return;
        }
        pp = &m->next;
    }
}

static void define_macro(PPContext *ctx, const char *name, const char *body,
                         bool is_func, int param_count, char **params) {
    undef_macro(ctx, name, strlen(name));
    Macro *m = (Macro *)calloc(1, sizeof(Macro));
    m->name = strdup(name);
    m->body = strdup(body ? body : "");
    m->is_func = is_func;
    m->param_count = param_count;
    m->params = params;
    m->next = ctx->macros;
    ctx->macros = m;
}

/* ============================================================================
 * Comment Removal & Line Continuation Joining
 * ============================================================================ */

static char *clean_comments_and_continuations(const char *src) {
    StrBuf sb;
    sb_init(&sb);
    size_t i = 0;
    while (src[i]) {
        /* Backslash-newline continuation */
        if (src[i] == '\\' && src[i + 1] == '\n') {
            i += 2;
            continue;
        }
        if (src[i] == '\\' && src[i + 1] == '\r' && src[i + 2] == '\n') {
            i += 3;
            continue;
        }
        /* String or char literal: copy verbatim */
        if (src[i] == '"' || src[i] == '\'') {
            char quote = src[i];
            sb_append_char(&sb, src[i++]);
            while (src[i] && src[i] != quote) {
                if (src[i] == '\\' && src[i + 1]) {
                    sb_append_char(&sb, src[i++]);
                }
                sb_append_char(&sb, src[i++]);
            }
            if (src[i] == quote) {
                sb_append_char(&sb, src[i++]);
            }
            continue;
        }
        /* Single-line comment */
        if (src[i] == '/' && src[i + 1] == '/') {
            i += 2;
            while (src[i] && src[i] != '\n') {
                i++;
            }
            sb_append_char(&sb, ' ');
            continue;
        }
        /* Multi-line comment: preserve newlines so line numbers stay accurate */
        if (src[i] == '/' && src[i + 1] == '*') {
            i += 2;
            sb_append_char(&sb, ' ');
            while (src[i] && !(src[i] == '*' && src[i + 1] == '/')) {
                if (src[i] == '\n') {
                    sb_append_char(&sb, '\n');
                }
                i++;
            }
            if (src[i]) i += 2;
            continue;
        }
        sb_append_char(&sb, src[i++]);
    }
    return sb.data;
}

/* ============================================================================
 * Macro Expansion on a Line
 * ============================================================================ */

static char *expand_line_macros(PPContext *ctx, const char *line, int depth);

static char *substitute_params(const char *body, int param_count, char **params, char **args) {
    StrBuf sb;
    sb_init(&sb);
    size_t i = 0;
    while (body[i]) {
        if (body[i] == '"' || body[i] == '\'') {
            char q = body[i];
            sb_append_char(&sb, body[i++]);
            while (body[i] && body[i] != q) {
                if (body[i] == '\\' && body[i + 1]) sb_append_char(&sb, body[i++]);
                sb_append_char(&sb, body[i++]);
            }
            if (body[i]) sb_append_char(&sb, body[i++]);
            continue;
        }
        /* Stringification #param */
        if (body[i] == '#' && body[i + 1] != '#') {
            size_t k = i + 1;
            while (body[k] == ' ' || body[k] == '\t') k++;
            if (is_ident_start(body[k])) {
                size_t start = k;
                while (is_ident_char(body[k])) k++;
                size_t len = k - start;
                int matched = -1;
                for (int p = 0; p < param_count; p++) {
                    if (strlen(params[p]) == len && strncmp(params[p], body + start, len) == 0) {
                        matched = p;
                        break;
                    }
                }
                if (matched >= 0) {
                    sb_append_char(&sb, '"');
                    for (const char *c = args[matched]; *c; c++) {
                        if (*c == '"' || *c == '\\') sb_append_char(&sb, '\\');
                        sb_append_char(&sb, *c);
                    }
                    sb_append_char(&sb, '"');
                    i = k;
                    continue;
                }
            }
        }
        if (is_ident_start(body[i])) {
            size_t start = i;
            while (is_ident_char(body[i])) i++;
            size_t len = i - start;
            int matched = -1;
            for (int p = 0; p < param_count; p++) {
                if (strlen(params[p]) == len && strncmp(params[p], body + start, len) == 0) {
                    matched = p;
                    break;
                }
            }
            if (matched >= 0) {
                sb_append(&sb, args[matched]);
            } else {
                sb_append_n(&sb, body + start, len);
            }
            continue;
        }
        sb_append_char(&sb, body[i++]);
    }
    return sb.data;
}

static char *expand_line_macros(PPContext *ctx, const char *line, int depth) {
    if (depth > 32) return strdup(line);
    StrBuf sb;
    sb_init(&sb);
    size_t i = 0;
    bool any_expanded = false;

    while (line[i]) {
        if (line[i] == '"' || line[i] == '\'') {
            char q = line[i];
            sb_append_char(&sb, line[i++]);
            while (line[i] && line[i] != q) {
                if (line[i] == '\\' && line[i + 1]) sb_append_char(&sb, line[i++]);
                sb_append_char(&sb, line[i++]);
            }
            if (line[i]) sb_append_char(&sb, line[i++]);
            continue;
        }
        if (is_ident_start(line[i])) {
            size_t start = i;
            while (is_ident_char(line[i])) i++;
            size_t len = i - start;
            Macro *m = find_macro(ctx, line + start, len);
            if (!m) {
                sb_append_n(&sb, line + start, len);
                continue;
            }
            if (!m->is_func) {
                sb_append(&sb, m->body);
                any_expanded = true;
                continue;
            }
            /* Function-like macro: look for '(' */
            size_t k = i;
            while (line[k] == ' ' || line[k] == '\t') k++;
            if (line[k] != '(') {
                sb_append_n(&sb, line + start, len);
                continue;
            }
            k++; /* skip '(' */
            char **args = (char **)calloc((size_t)(m->param_count > 0 ? m->param_count : 1), sizeof(char *));
            int arg_idx = 0;
            StrBuf arg_buf;
            sb_init(&arg_buf);
            int paren_depth = 0;
            while (line[k]) {
                if (line[k] == '"' || line[k] == '\'') {
                    char q = line[k];
                    sb_append_char(&arg_buf, line[k++]);
                    while (line[k] && line[k] != q) {
                        if (line[k] == '\\' && line[k + 1]) sb_append_char(&arg_buf, line[k++]);
                        sb_append_char(&arg_buf, line[k++]);
                    }
                    if (line[k]) sb_append_char(&arg_buf, line[k++]);
                    continue;
                }
                if (line[k] == '(') {
                    paren_depth++;
                    sb_append_char(&arg_buf, line[k++]);
                    continue;
                }
                if (line[k] == ')') {
                    if (paren_depth == 0) {
                        k++;
                        break;
                    }
                    paren_depth--;
                    sb_append_char(&arg_buf, line[k++]);
                    continue;
                }
                if (line[k] == ',' && paren_depth == 0 && arg_idx + 1 < m->param_count) {
                    args[arg_idx++] = arg_buf.data;
                    sb_init(&arg_buf);
                    k++;
                    while (line[k] == ' ' || line[k] == '\t') k++;
                    continue;
                }
                sb_append_char(&arg_buf, line[k++]);
            }
            if (m->param_count > 0 && arg_idx < m->param_count) {
                args[arg_idx++] = arg_buf.data;
            } else {
                free(arg_buf.data);
            }
            for (int p = arg_idx; p < m->param_count; p++) {
                args[p] = strdup("");
            }
            char *sub = substitute_params(m->body, m->param_count, m->params, args);
            sb_append(&sb, sub);
            free(sub);
            for (int p = 0; p < m->param_count; p++) free(args[p]);
            free(args);
            i = k;
            any_expanded = true;
            continue;
        }
        sb_append_char(&sb, line[i++]);
    }

    if (any_expanded) {
        char *res = expand_line_macros(ctx, sb.data, depth + 1);
        free(sb.data);
        return res;
    }
    return sb.data;
}

/* ============================================================================
 * #if / #elif Constant Expression Evaluator
 * ============================================================================ */

typedef struct {
    PPContext *ctx;
    const char *s;
} ExprEval;

static int64_t eval_pp_expr(ExprEval *e);

static void skip_ws(ExprEval *e) {
    while (*e->s == ' ' || *e->s == '\t') e->s++;
}

static int64_t eval_pp_primary(ExprEval *e) {
    skip_ws(e);
    if (*e->s == '(') {
        e->s++;
        int64_t v = eval_pp_expr(e);
        skip_ws(e);
        if (*e->s == ')') e->s++;
        return v;
    }
    if (*e->s == '!') {
        e->s++;
        return !eval_pp_primary(e);
    }
    if (*e->s == '~') {
        e->s++;
        return ~eval_pp_primary(e);
    }
    if (*e->s == '-') {
        e->s++;
        return -eval_pp_primary(e);
    }
    if (*e->s == '+') {
        e->s++;
        return eval_pp_primary(e);
    }
    if (strncmp(e->s, "defined", 7) == 0 && !is_ident_char(e->s[7])) {
        e->s += 7;
        skip_ws(e);
        bool has_paren = false;
        if (*e->s == '(') {
            has_paren = true;
            e->s++;
            skip_ws(e);
        }
        const char *start = e->s;
        while (is_ident_char(*e->s)) e->s++;
        size_t len = (size_t)(e->s - start);
        if (has_paren) {
            skip_ws(e);
            if (*e->s == ')') e->s++;
        }
        return find_macro(e->ctx, start, len) != NULL ? 1 : 0;
    }
    if (isdigit((unsigned char)*e->s)) {
        char *endptr = NULL;
        int64_t v = strtoll(e->s, &endptr, 0);
        e->s = endptr;
        while (*e->s == 'u' || *e->s == 'U' || *e->s == 'l' || *e->s == 'L') e->s++;
        return v;
    }
    if (is_ident_start(*e->s)) {
        /* Undefined identifier in #if evaluates to 0 in C preprocessor */
        while (is_ident_char(*e->s)) e->s++;
        return 0;
    }
    return 0;
}

static int64_t eval_pp_mul(ExprEval *e) {
    int64_t v = eval_pp_primary(e);
    for (;;) {
        skip_ws(e);
        if (*e->s == '*') { e->s++; v *= eval_pp_primary(e); }
        else if (*e->s == '/') { e->s++; int64_t r = eval_pp_primary(e); v = r ? v / r : 0; }
        else if (*e->s == '%') { e->s++; int64_t r = eval_pp_primary(e); v = r ? v % r : 0; }
        else break;
    }
    return v;
}

static int64_t eval_pp_add(ExprEval *e) {
    int64_t v = eval_pp_mul(e);
    for (;;) {
        skip_ws(e);
        if (*e->s == '+' && e->s[1] != '+') { e->s++; v += eval_pp_mul(e); }
        else if (*e->s == '-' && e->s[1] != '-') { e->s++; v -= eval_pp_mul(e); }
        else break;
    }
    return v;
}

static int64_t eval_pp_rel(ExprEval *e) {
    int64_t v = eval_pp_add(e);
    for (;;) {
        skip_ws(e);
        if (e->s[0] == '<' && e->s[1] == '=') { e->s += 2; v = (v <= eval_pp_add(e)); }
        else if (e->s[0] == '>' && e->s[1] == '=') { e->s += 2; v = (v >= eval_pp_add(e)); }
        else if (e->s[0] == '<' && e->s[1] != '<') { e->s += 1; v = (v < eval_pp_add(e)); }
        else if (e->s[0] == '>' && e->s[1] != '>') { e->s += 1; v = (v > eval_pp_add(e)); }
        else break;
    }
    return v;
}

static int64_t eval_pp_eq(ExprEval *e) {
    int64_t v = eval_pp_rel(e);
    for (;;) {
        skip_ws(e);
        if (e->s[0] == '=' && e->s[1] == '=') { e->s += 2; v = (v == eval_pp_rel(e)); }
        else if (e->s[0] == '!' && e->s[1] == '=') { e->s += 2; v = (v != eval_pp_rel(e)); }
        else break;
    }
    return v;
}

static int64_t eval_pp_and(ExprEval *e) {
    int64_t v = eval_pp_eq(e);
    for (;;) {
        skip_ws(e);
        if (e->s[0] == '&' && e->s[1] == '&') {
            e->s += 2;
            int64_t r = eval_pp_eq(e);
            v = (v && r);
        } else break;
    }
    return v;
}

static int64_t eval_pp_expr(ExprEval *e) {
    int64_t v = eval_pp_and(e);
    for (;;) {
        skip_ws(e);
        if (e->s[0] == '|' && e->s[1] == '|') {
            e->s += 2;
            int64_t r = eval_pp_and(e);
            v = (v || r);
        } else break;
    }
    return v;
}

static bool eval_if_condition(PPContext *ctx, const char *raw_expr) {
    /* Expand macros except inside `defined(...)` */
    StrBuf pre;
    sb_init(&pre);
    size_t i = 0;
    while (raw_expr[i]) {
        if (strncmp(raw_expr + i, "defined", 7) == 0 && !is_ident_char(raw_expr[i + 7])) {
            sb_append_n(&pre, raw_expr + i, 7);
            i += 7;
            while (raw_expr[i] == ' ' || raw_expr[i] == '\t') sb_append_char(&pre, raw_expr[i++]);
            if (raw_expr[i] == '(') {
                sb_append_char(&pre, raw_expr[i++]);
                while (raw_expr[i] && raw_expr[i] != ')') sb_append_char(&pre, raw_expr[i++]);
                if (raw_expr[i] == ')') sb_append_char(&pre, raw_expr[i++]);
            } else {
                while (is_ident_char(raw_expr[i])) sb_append_char(&pre, raw_expr[i++]);
            }
            continue;
        }
        if (is_ident_start(raw_expr[i])) {
            size_t start = i;
            while (is_ident_char(raw_expr[i])) i++;
            Macro *m = find_macro(ctx, raw_expr + start, i - start);
            if (m && !m->is_func) {
                sb_append(&pre, m->body);
            } else {
                sb_append_n(&pre, raw_expr + start, i - start);
            }
            continue;
        }
        sb_append_char(&pre, raw_expr[i++]);
    }
    ExprEval ev = { .ctx = ctx, .s = pre.data };
    int64_t res = eval_pp_expr(&ev);
    free(pre.data);
    return res != 0;
}

/* ============================================================================
 * File Processing & Directive Handling
 * ============================================================================ */

static void process_buffer(PPContext *ctx, const char *filename, const char *raw_src, StrBuf *out);

static const char *find_builtin_header(const char *name) {
    for (size_t i = 0; i < sizeof(k_builtin_headers) / sizeof(k_builtin_headers[0]); i++) {
        if (strcmp(k_builtin_headers[i].name, name) == 0) {
            return k_builtin_headers[i].content;
        }
    }
    return NULL;
}

static bool is_already_once(PPContext *ctx, const char *path) {
    for (int i = 0; i < ctx->once_count; i++) {
        if (strcmp(ctx->once_files[i], path) == 0) return true;
    }
    return false;
}

static void mark_once(PPContext *ctx, const char *path) {
    if (is_already_once(ctx, path)) return;
    if (ctx->once_count < 256) {
        ctx->once_files[ctx->once_count++] = strdup(path);
    }
}

static void handle_include(PPContext *ctx, const char *cur_file, const char *inc_name, bool is_system, StrBuf *out) {
    /* 1. If not system or if relative to current file, try relative path first */
    char path_buf[1024];
    if (!is_system && cur_file) {
        const char *slash = strrchr(cur_file, '/');
        if (slash) {
            size_t dir_len = (size_t)(slash - cur_file);
            snprintf(path_buf, sizeof(path_buf), "%.*s/%s", (int)dir_len, cur_file, inc_name);
        } else {
            snprintf(path_buf, sizeof(path_buf), "%s", inc_name);
        }
        char *text = read_file_text(path_buf);
        if (text) {
            if (!is_already_once(ctx, path_buf)) {
                process_buffer(ctx, path_buf, text, out);
            }
            free(text);
            return;
        }
    }

    /* 2. Check user -I include paths */
    if (ctx->cfg) {
        for (int i = 0; i < ctx->cfg->include_path_count; i++) {
            snprintf(path_buf, sizeof(path_buf), "%s/%s", ctx->cfg->include_paths[i], inc_name);
            char *text = read_file_text(path_buf);
            if (text) {
                if (!is_already_once(ctx, path_buf)) {
                    process_buffer(ctx, path_buf, text, out);
                }
                free(text);
                return;
            }
        }
    }

    /* 3. Check built-in standard headers */
    const char *builtin = find_builtin_header(inc_name);
    if (builtin) {
        process_buffer(ctx, inc_name, builtin, out);
        return;
    }

    /* 4. Try direct path */
    char *text = read_file_text(inc_name);
    if (text) {
        if (!is_already_once(ctx, inc_name)) {
            process_buffer(ctx, inc_name, text, out);
        }
        free(text);
        return;
    }

    fatal_error("cannot find include file '%s' (included from %s)", inc_name, cur_file ? cur_file : "<input>");
}

typedef struct {
    bool parent_active;
    bool this_branch_active;
    bool any_branch_taken;
} CondState;

static void process_buffer(PPContext *ctx, const char *filename, const char *raw_src, StrBuf *out) {
    char *cleaned = clean_comments_and_continuations(raw_src);
    CondState cond_stack[64];
    int cond_depth = 0;
    bool active = true;

    const char *p = cleaned;
    while (*p) {
        const char *line_start = p;
        while (*p && *p != '\n') p++;
        size_t line_len = (size_t)(p - line_start);
        if (*p == '\n') p++;

        char *line = (char *)malloc(line_len + 1);
        memcpy(line, line_start, line_len);
        line[line_len] = '\0';

        /* Trim trailing \r */
        if (line_len > 0 && line[line_len - 1] == '\r') {
            line[--line_len] = '\0';
        }

        const char *s = line;
        while (*s == ' ' || *s == '\t') s++;

        if (*s == '#') {
            s++;
            while (*s == ' ' || *s == '\t') s++;
            const char *dir_start = s;
            while (is_ident_char(*s)) s++;
            size_t dir_len = (size_t)(s - dir_start);
            while (*s == ' ' || *s == '\t') s++;

            if (dir_len == 5 && strncmp(dir_start, "ifdef", 5) == 0) {
                const char *id_start = s;
                while (is_ident_char(*s)) s++;
                bool cond = find_macro(ctx, id_start, (size_t)(s - id_start)) != NULL;
                cond_stack[cond_depth].parent_active = active;
                cond_stack[cond_depth].this_branch_active = active && cond;
                cond_stack[cond_depth].any_branch_taken = cond;
                active = cond_stack[cond_depth].this_branch_active;
                cond_depth++;
            } else if (dir_len == 6 && strncmp(dir_start, "ifndef", 6) == 0) {
                const char *id_start = s;
                while (is_ident_char(*s)) s++;
                bool cond = find_macro(ctx, id_start, (size_t)(s - id_start)) == NULL;
                cond_stack[cond_depth].parent_active = active;
                cond_stack[cond_depth].this_branch_active = active && cond;
                cond_stack[cond_depth].any_branch_taken = cond;
                active = cond_stack[cond_depth].this_branch_active;
                cond_depth++;
            } else if (dir_len == 2 && strncmp(dir_start, "if", 2) == 0) {
                bool cond = active ? eval_if_condition(ctx, s) : false;
                cond_stack[cond_depth].parent_active = active;
                cond_stack[cond_depth].this_branch_active = active && cond;
                cond_stack[cond_depth].any_branch_taken = cond;
                active = cond_stack[cond_depth].this_branch_active;
                cond_depth++;
            } else if (dir_len == 4 && strncmp(dir_start, "elif", 4) == 0) {
                if (cond_depth > 0) {
                    CondState *cs = &cond_stack[cond_depth - 1];
                    if (!cs->parent_active || cs->any_branch_taken) {
                        cs->this_branch_active = false;
                    } else {
                        bool cond = eval_if_condition(ctx, s);
                        cs->this_branch_active = cond;
                        if (cond) cs->any_branch_taken = true;
                    }
                    active = cs->this_branch_active;
                }
            } else if (dir_len == 4 && strncmp(dir_start, "else", 4) == 0) {
                if (cond_depth > 0) {
                    CondState *cs = &cond_stack[cond_depth - 1];
                    cs->this_branch_active = cs->parent_active && !cs->any_branch_taken;
                    cs->any_branch_taken = true;
                    active = cs->this_branch_active;
                }
            } else if (dir_len == 5 && strncmp(dir_start, "endif", 5) == 0) {
                if (cond_depth > 0) {
                    cond_depth--;
                    active = cond_stack[cond_depth].parent_active;
                }
            } else if (active) {
                if (dir_len == 6 && strncmp(dir_start, "define", 6) == 0) {
                    const char *id_start = s;
                    while (is_ident_char(*s)) s++;
                    size_t id_len = (size_t)(s - id_start);
                    if (id_len > 0) {
                        char *mname = (char *)malloc(id_len + 1);
                        memcpy(mname, id_start, id_len);
                        mname[id_len] = '\0';

                        bool is_func = false;
                        int param_count = 0;
                        char **params = NULL;
                        /* Note: function-like macro if '(' immediately follows macro name */
                        if (*s == '(') {
                            is_func = true;
                            s++;
                            params = (char **)calloc(16, sizeof(char *));
                            while (*s && *s != ')') {
                                while (*s == ' ' || *s == '\t') s++;
                                const char *ps = s;
                                while (is_ident_char(*s)) s++;
                                size_t plen = (size_t)(s - ps);
                                if (plen > 0 && param_count < 16) {
                                    char *pname = (char *)malloc(plen + 1);
                                    memcpy(pname, ps, plen);
                                    pname[plen] = '\0';
                                    params[param_count++] = pname;
                                }
                                while (*s == ' ' || *s == '\t') s++;
                                if (*s == ',') s++;
                            }
                            if (*s == ')') s++;
                        }
                        while (*s == ' ' || *s == '\t') s++;
                        define_macro(ctx, mname, s, is_func, param_count, params);
                        free(mname);
                    }
                } else if (dir_len == 5 && strncmp(dir_start, "undef", 5) == 0) {
                    const char *id_start = s;
                    while (is_ident_char(*s)) s++;
                    undef_macro(ctx, id_start, (size_t)(s - id_start));
                } else if (dir_len == 7 && strncmp(dir_start, "include", 7) == 0) {
                    if (*s == '<' || *s == '"') {
                        bool is_sys = (*s == '<');
                        char end_ch = is_sys ? '>' : '"';
                        s++;
                        const char *inc_start = s;
                        while (*s && *s != end_ch) s++;
                        size_t inc_len = (size_t)(s - inc_start);
                        char *inc_name = (char *)malloc(inc_len + 1);
                        memcpy(inc_name, inc_start, inc_len);
                        inc_name[inc_len] = '\0';
                        handle_include(ctx, filename, inc_name, is_sys, out);
                        free(inc_name);
                    }
                } else if (dir_len == 6 && strncmp(dir_start, "pragma", 6) == 0) {
                    if (strncmp(s, "once", 4) == 0 && filename) {
                        mark_once(ctx, filename);
                    }
                }
            }
            sb_append_char(out, '\n');
            free(line);
            continue;
        }

        if (active) {
            char *expanded = expand_line_macros(ctx, line, 0);
            sb_append(out, expanded);
            sb_append_char(out, '\n');
            free(expanded);
        } else {
            sb_append_char(out, '\n');
        }
        free(line);
    }

    free(cleaned);
}

char *preprocess_source(const char *filename, const char *source, const PreprocessorConfig *cfg) {
    PPContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.cfg = cfg;

    /* Predefined compiler & target macros */
    define_macro(&ctx, "__XCC__", "1", false, 0, NULL);
    define_macro(&ctx, "__STDC__", "1", false, 0, NULL);
    define_macro(&ctx, "__STDC_VERSION__", "201112L", false, 0, NULL);
    define_macro(&ctx, "__LP64__", "1", false, 0, NULL);

    TargetPlatform target = cfg ? cfg->target : get_host_target();
    const TargetInfo *ti = get_target_info(target);
    if (ti->is_arm64) {
        define_macro(&ctx, "__aarch64__", "1", false, 0, NULL);
        define_macro(&ctx, "__arm64__", "1", false, 0, NULL);
    } else if (ti->is_x86_64) {
        define_macro(&ctx, "__x86_64__", "1", false, 0, NULL);
        define_macro(&ctx, "__amd64__", "1", false, 0, NULL);
    }
    if (ti->is_macos) {
        define_macro(&ctx, "__APPLE__", "1", false, 0, NULL);
        define_macro(&ctx, "__MACH__", "1", false, 0, NULL);
    } else if (ti->is_linux) {
        define_macro(&ctx, "__linux__", "1", false, 0, NULL);
        define_macro(&ctx, "__unix__", "1", false, 0, NULL);
    } else if (ti->is_windows) {
        define_macro(&ctx, "_WIN32", "1", false, 0, NULL);
        define_macro(&ctx, "_WIN64", "1", false, 0, NULL);
    }

    /* Command-line -D macros */
    if (cfg) {
        for (int i = 0; i < cfg->cmd_define_count; i++) {
            const char *def = cfg->cmd_defines[i];
            const char *eq = strchr(def, '=');
            if (eq) {
                size_t nlen = (size_t)(eq - def);
                char *name = (char *)malloc(nlen + 1);
                memcpy(name, def, nlen);
                name[nlen] = '\0';
                define_macro(&ctx, name, eq + 1, false, 0, NULL);
                free(name);
            } else {
                define_macro(&ctx, def, "1", false, 0, NULL);
            }
        }
    }

    StrBuf out;
    sb_init(&out);
    process_buffer(&ctx, filename, source, &out);
    return out.data;
}
