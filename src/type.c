#include "xcc.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Target Registry
 * ============================================================================ */

static const TargetInfo k_targets[] = {
    {
        .platform = TARGET_ARM64_MACOS,
        .name = "arm64-macos",
        .triple = "arm64-apple-darwin",
        .is_macos = true,
        .is_linux = false,
        .is_windows = false,
        .is_arm64 = true,
        .is_x86_64 = false,
        .symbol_prefix = "_",
    },
    {
        .platform = TARGET_ARM64_LINUX,
        .name = "arm64-linux",
        .triple = "aarch64-unknown-linux-gnu",
        .is_macos = false,
        .is_linux = true,
        .is_windows = false,
        .is_arm64 = true,
        .is_x86_64 = false,
        .symbol_prefix = "",
    },
    {
        .platform = TARGET_X86_64_MACOS,
        .name = "x86_64-macos",
        .triple = "x86_64-apple-darwin",
        .is_macos = true,
        .is_linux = false,
        .is_windows = false,
        .is_arm64 = false,
        .is_x86_64 = true,
        .symbol_prefix = "_",
    },
    {
        .platform = TARGET_X86_64_LINUX,
        .name = "x86_64-linux",
        .triple = "x86_64-unknown-linux-gnu",
        .is_macos = false,
        .is_linux = true,
        .is_windows = false,
        .is_arm64 = false,
        .is_x86_64 = true,
        .symbol_prefix = "",
    },
    {
        .platform = TARGET_X86_64_WINDOWS,
        .name = "x86_64-windows",
        .triple = "x86_64-w64-mingw32",
        .is_macos = false,
        .is_linux = false,
        .is_windows = true,
        .is_arm64 = false,
        .is_x86_64 = true,
        .symbol_prefix = "",
    },
};

const TargetInfo *get_target_info(TargetPlatform platform) {
    for (size_t i = 0; i < sizeof(k_targets) / sizeof(k_targets[0]); i++) {
        if (k_targets[i].platform == platform) {
            return &k_targets[i];
        }
    }
    return &k_targets[0];
}

TargetPlatform get_host_target(void) {
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
    return TARGET_ARM64_MACOS;
#elif defined(__APPLE__) && defined(__x86_64__)
    return TARGET_X86_64_MACOS;
#elif defined(__linux__) && (defined(__aarch64__) || defined(__arm64__))
    return TARGET_ARM64_LINUX;
#elif defined(_WIN32)
    return TARGET_X86_64_WINDOWS;
#else
    return TARGET_X86_64_LINUX;
#endif
}

bool parse_target_name(const char *str, TargetPlatform *out) {
    if (!str) return false;
    if (strcmp(str, "arm64-macos") == 0 || strcmp(str, "aarch64-macos") == 0 ||
        strcmp(str, "arm64-apple-darwin") == 0 || strcmp(str, "aarch64-apple-darwin") == 0) {
        *out = TARGET_ARM64_MACOS;
        return true;
    }
    if (strcmp(str, "arm64-linux") == 0 || strcmp(str, "aarch64-linux") == 0 ||
        strcmp(str, "aarch64-linux-gnu") == 0 || strcmp(str, "aarch64-unknown-linux-gnu") == 0) {
        *out = TARGET_ARM64_LINUX;
        return true;
    }
    if (strcmp(str, "x86_64-macos") == 0 || strcmp(str, "x86_64-apple-darwin") == 0 ||
        strcmp(str, "amd64-macos") == 0) {
        *out = TARGET_X86_64_MACOS;
        return true;
    }
    if (strcmp(str, "x86_64-linux") == 0 || strcmp(str, "x86_64-linux-gnu") == 0 ||
        strcmp(str, "x86_64-unknown-linux-gnu") == 0 || strcmp(str, "amd64-linux") == 0) {
        *out = TARGET_X86_64_LINUX;
        return true;
    }
    if (strcmp(str, "x86_64-windows") == 0 || strcmp(str, "x86_64-w64-mingw32") == 0 ||
        strcmp(str, "win64") == 0) {
        *out = TARGET_X86_64_WINDOWS;
        return true;
    }
    return false;
}

/* ============================================================================
 * Error Reporting & File Utilities
 * ============================================================================ */

void error_at(SourceLoc loc, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s:%d:%d: error: ", loc.filename ? loc.filename : "<input>", loc.line, loc.col);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

void fatal_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "xcc: error: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

char *read_file_text(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long sz = ftell(fp);
    if (sz < 0) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);
    char *buf = (char *)malloc((size_t)sz + 2);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[n] = '\n';
    buf[n + 1] = '\0';
    return buf;
}

/* ============================================================================
 * Built-in Primitive Types
 * ============================================================================ */

static Type k_ty_void   = { .kind = TY_VOID,  .size = 1, .align = 1, .is_unsigned = false, .is_complete = true };
static Type k_ty_bool   = { .kind = TY_BOOL,  .size = 1, .align = 1, .is_unsigned = true,  .is_complete = true };
static Type k_ty_char   = { .kind = TY_CHAR,  .size = 1, .align = 1, .is_unsigned = false, .is_complete = true };
static Type k_ty_uchar  = { .kind = TY_CHAR,  .size = 1, .align = 1, .is_unsigned = true,  .is_complete = true };
static Type k_ty_short  = { .kind = TY_SHORT, .size = 2, .align = 2, .is_unsigned = false, .is_complete = true };
static Type k_ty_ushort = { .kind = TY_SHORT, .size = 2, .align = 2, .is_unsigned = true,  .is_complete = true };
static Type k_ty_int    = { .kind = TY_INT,   .size = 4, .align = 4, .is_unsigned = false, .is_complete = true };
static Type k_ty_uint   = { .kind = TY_INT,   .size = 4, .align = 4, .is_unsigned = true,  .is_complete = true };
static Type k_ty_long   = { .kind = TY_LONG,  .size = 8, .align = 8, .is_unsigned = false, .is_complete = true };
static Type k_ty_ulong  = { .kind = TY_LONG,  .size = 8, .align = 8, .is_unsigned = true,  .is_complete = true };

Type *ty_void   = &k_ty_void;
Type *ty_bool   = &k_ty_bool;
Type *ty_char   = &k_ty_char;
Type *ty_uchar  = &k_ty_uchar;
Type *ty_short  = &k_ty_short;
Type *ty_ushort = &k_ty_ushort;
Type *ty_int    = &k_ty_int;
Type *ty_uint   = &k_ty_uint;
Type *ty_long   = &k_ty_long;
Type *ty_ulong  = &k_ty_ulong;

Type *new_type(TypeKind kind, int size, int align) {
    Type *ty = (Type *)calloc(1, sizeof(Type));
    ty->kind = kind;
    ty->size = size;
    ty->align = align;
    ty->is_complete = true;
    return ty;
}

Type *pointer_to(Type *base) {
    Type *ty = new_type(TY_PTR, 8, 8);
    ty->base = base;
    ty->is_unsigned = true;
    return ty;
}

Type *array_of(Type *base, int len) {
    Type *ty = new_type(TY_ARRAY, base->size * len, base->align);
    ty->base = base;
    ty->array_len = len;
    return ty;
}

Type *func_type(Type *ret_type) {
    Type *ty = new_type(TY_FUNC, 8, 8);
    ty->return_type = ret_type;
    return ty;
}

Type *copy_type(Type *ty) {
    Type *ret = (Type *)calloc(1, sizeof(Type));
    *ret = *ty;
    return ret;
}

bool is_integer_type(Type *ty) {
    if (!ty) return false;
    return ty->kind == TY_BOOL || ty->kind == TY_CHAR || ty->kind == TY_SHORT ||
           ty->kind == TY_INT || ty->kind == TY_LONG || ty->kind == TY_ENUM;
}

bool is_pointer_like(Type *ty) {
    if (!ty) return false;
    return ty->kind == TY_PTR || ty->kind == TY_ARRAY || ty->kind == TY_FUNC;
}

bool is_scalar_type(Type *ty) {
    return is_integer_type(ty) || is_pointer_like(ty);
}

Type *common_type(Type *t1, Type *t2) {
    if (!t1) return t2;
    if (!t2) return t1;
    if (t1->base) return pointer_to(t1->base);
    if (t2->base) return pointer_to(t2->base);
    int s1 = t1->size < 4 ? 4 : t1->size;
    int s2 = t2->size < 4 ? 4 : t2->size;
    if (s1 != s2) {
        return (s1 < s2) ? t2 : t1;
    }
    if (t1->is_unsigned || t2->is_unsigned) {
        return (s1 == 8) ? ty_ulong : ty_uint;
    }
    return (s1 == 8) ? ty_long : ty_int;
}

Member *find_struct_member(Type *struct_ty, const char *name) {
    if (!struct_ty) return NULL;
    for (Member *m = struct_ty->members; m; m = m->next) {
        if (m->name && strcmp(m->name, name) == 0) {
            return m;
        }
    }
    return NULL;
}

void format_type(Type *ty, char *buf, size_t buf_size) {
    if (!ty) {
        snprintf(buf, buf_size, "<null>");
        return;
    }
    switch (ty->kind) {
        case TY_VOID:  snprintf(buf, buf_size, "void"); break;
        case TY_BOOL:  snprintf(buf, buf_size, "bool"); break;
        case TY_CHAR:  snprintf(buf, buf_size, "%schar", ty->is_unsigned ? "unsigned " : ""); break;
        case TY_SHORT: snprintf(buf, buf_size, "%sshort", ty->is_unsigned ? "unsigned " : ""); break;
        case TY_INT:   snprintf(buf, buf_size, "%sint", ty->is_unsigned ? "unsigned " : ""); break;
        case TY_LONG:  snprintf(buf, buf_size, "%slong", ty->is_unsigned ? "unsigned " : ""); break;
        case TY_ENUM:  snprintf(buf, buf_size, "enum %s", ty->tag ? ty->tag : ""); break;
        case TY_PTR: {
            char inner[128];
            format_type(ty->base, inner, sizeof(inner));
            snprintf(buf, buf_size, "%s*", inner);
            break;
        }
        case TY_ARRAY: {
            char inner[128];
            format_type(ty->base, inner, sizeof(inner));
            snprintf(buf, buf_size, "%s[%d]", inner, ty->array_len);
            break;
        }
        case TY_STRUCT:
            snprintf(buf, buf_size, "struct %s", ty->tag ? ty->tag : "<anon>");
            break;
        case TY_UNION:
            snprintf(buf, buf_size, "union %s", ty->tag ? ty->tag : "<anon>");
            break;
        case TY_FUNC: {
            char inner[128];
            format_type(ty->return_type, inner, sizeof(inner));
            snprintf(buf, buf_size, "%s()", inner);
            break;
        }
    }
}
