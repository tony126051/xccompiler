#include "xcc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Symbol & Scope Tables
 * ============================================================================ */

typedef struct VarScope VarScope;
struct VarScope {
    char *name;
    VarObj *var;
    Type *type_def;
    Type *enum_ty;
    int64_t enum_val;
    VarScope *next;
};

typedef struct TagScope TagScope;
struct TagScope {
    char *name;
    Type *type;
    TagScope *next;
};

typedef struct Scope Scope;
struct Scope {
    VarScope *vars;
    TagScope *tags;
    Scope *parent;
};

typedef struct {
    Token *tok;
    Scope *scope;
    VarObj *locals;
    int local_count;
    VarObj *globals;
    FunctionDef *funcs;
    StringLiteral *strings;
    int str_count;
    Node *current_switch;
    Type *current_func_ret;
} Parser;

static void enter_scope(Parser *p) {
    Scope *sc = (Scope *)calloc(1, sizeof(Scope));
    sc->parent = p->scope;
    p->scope = sc;
}

static void leave_scope(Parser *p) {
    if (p->scope) {
        p->scope = p->scope->parent;
    }
}

static VarScope *push_var_scope(Parser *p, const char *name) {
    VarScope *vs = (VarScope *)calloc(1, sizeof(VarScope));
    vs->name = strdup(name);
    vs->next = p->scope->vars;
    p->scope->vars = vs;
    return vs;
}

static VarScope *find_var(Parser *p, const char *name) {
    for (Scope *sc = p->scope; sc; sc = sc->parent) {
        for (VarScope *vs = sc->vars; vs; vs = vs->next) {
            if (strcmp(vs->name, name) == 0) {
                return vs;
            }
        }
    }
    return NULL;
}

static Type *find_typedef(Parser *p, const char *name) {
    VarScope *vs = find_var(p, name);
    if (vs && vs->type_def) return vs->type_def;
    return NULL;
}

static Type *find_tag(Parser *p, const char *name) {
    for (Scope *sc = p->scope; sc; sc = sc->parent) {
        for (TagScope *ts = sc->tags; ts; ts = ts->next) {
            if (strcmp(ts->name, name) == 0) {
                return ts->type;
            }
        }
    }
    return NULL;
}

static Type *find_tag_in_current_scope(Parser *p, const char *name) {
    for (TagScope *ts = p->scope->tags; ts; ts = ts->next) {
        if (strcmp(ts->name, name) == 0) {
            return ts->type;
        }
    }
    return NULL;
}

static void push_tag_scope(Parser *p, const char *name, Type *ty) {
    TagScope *ts = (TagScope *)calloc(1, sizeof(TagScope));
    ts->name = strdup(name);
    ts->type = ty;
    ts->next = p->scope->tags;
    p->scope->tags = ts;
}

/* ============================================================================
 * Token Navigation Helpers
 * ============================================================================ */

static bool at_eof(Parser *p) {
    return p->tok->kind == TOK_EOF;
}

static bool peek_is(Parser *p, TokenKind kind) {
    return p->tok->kind == kind;
}

static bool consume(Parser *p, TokenKind kind) {
    if (p->tok->kind == kind) {
        p->tok = p->tok->next;
        return true;
    }
    return false;
}

static Token *expect(Parser *p, TokenKind kind) {
    if (p->tok->kind != kind) {
        error_at(p->tok->loc, "expected '%s', but got '%.*s'",
                 token_kind_name(kind), p->tok->len, p->tok->lexeme);
    }
    Token *t = p->tok;
    p->tok = p->tok->next;
    return t;
}

static char *expect_ident(Parser *p) {
    if (p->tok->kind != TOK_IDENT) {
        error_at(p->tok->loc, "expected identifier, but got '%.*s'", p->tok->len, p->tok->lexeme);
    }
    char *name = strdup(p->tok->str_val);
    p->tok = p->tok->next;
    return name;
}

static bool is_typename(Parser *p, Token *tok) {
    switch (tok->kind) {
        case TOK_VOID:
        case TOK_CHAR:
        case TOK_SHORT:
        case TOK_INT:
        case TOK_LONG:
        case TOK_SIGNED:
        case TOK_UNSIGNED:
        case TOK_BOOL:
        case TOK_STRUCT:
        case TOK_UNION:
        case TOK_ENUM:
        case TOK_TYPEDEF:
        case TOK_STATIC:
        case TOK_EXTERN:
        case TOK_CONST:
            return true;
        case TOK_IDENT:
            return find_typedef(p, tok->str_val) != NULL;
        default:
            return false;
    }
}

/* ============================================================================
 * AST Node Constructors & Type Inference
 * ============================================================================ */

static Node *new_node(NodeKind kind, SourceLoc loc) {
    Node *n = (Node *)calloc(1, sizeof(Node));
    n->kind = kind;
    n->loc = loc;
    return n;
}

static Node *new_int_node(int64_t val, SourceLoc loc) {
    Node *n = new_node(ND_INT_LIT, loc);
    n->int_val = val;
    n->type = (val > 2147483647LL || val < -2147483648LL) ? ty_long : ty_int;
    return n;
}

static Node *new_unary(NodeKind kind, Node *lhs, Type *ty, SourceLoc loc) {
    Node *n = new_node(kind, loc);
    n->lhs = lhs;
    n->type = ty;
    return n;
}

static Node *new_binary(NodeKind kind, Node *lhs, Node *rhs, Type *ty, SourceLoc loc) {
    Node *n = new_node(kind, loc);
    n->lhs = lhs;
    n->rhs = rhs;
    n->type = ty;
    return n;
}

static VarObj *new_local_var(Parser *p, const char *name, Type *ty) {
    VarObj *v = (VarObj *)calloc(1, sizeof(VarObj));
    v->name = strdup(name);
    v->type = ty;
    v->is_local = true;
    v->local_id = p->local_count++;
    v->next = p->locals;
    p->locals = v;
    if (name[0] != '\0') {
        push_var_scope(p, name)->var = v;
    }
    return v;
}

static VarObj *new_global_var(Parser *p, const char *name, Type *ty, bool is_static, bool is_extern) {
    /* Check if already declared in globals */
    for (VarObj *g = p->globals; g; g = g->next) {
        if (strcmp(g->name, name) == 0) {
            if (!is_extern && g->is_extern) {
                g->is_extern = false;
            }
            g->type = ty;
            return g;
        }
    }
    VarObj *v = (VarObj *)calloc(1, sizeof(VarObj));
    v->name = strdup(name);
    v->type = ty;
    v->is_local = false;
    v->is_static = is_static;
    v->is_extern = is_extern;
    v->next = p->globals;
    p->globals = v;
    push_var_scope(p, name)->var = v;
    return v;
}

static int register_string_literal(Parser *p, const char *data, int len) {
    StringLiteral *s = (StringLiteral *)calloc(1, sizeof(StringLiteral));
    s->id = p->str_count++;
    s->data = (char *)malloc((size_t)len + 1);
    memcpy(s->data, data, (size_t)len);
    s->data[len] = '\0';
    s->len = len;
    s->next = p->strings;
    p->strings = s;
    return s->id;
}

static int align_to(int n, int align) {
    if (align <= 1) return n;
    return (n + align - 1) & ~(align - 1);
}

/* Forward declarations for recursive-descent parser */
static Type *parse_declspec(Parser *p, bool *is_typedef, bool *is_static, bool *is_extern);
static Type *parse_declarator(Parser *p, Type *base, char **out_name);
static Type *parse_abstract_declarator(Parser *p, Type *base);
static Type *parse_type_name(Parser *p);
static Node *parse_stmt(Parser *p);
static Node *parse_compound_stmt(Parser *p);
static Node *parse_expr(Parser *p);
static Node *parse_assign(Parser *p);
static Node *parse_conditional(Parser *p);
static Node *parse_unary(Parser *p);
static int64_t eval_const_expr(Parser *p);

/* ============================================================================
 * Pointer Arithmetic & Binary Operator Builders
 * ============================================================================ */

static int pointee_stride(Type *ty) {
    if (!ty || !ty->base) return 1;
    if (ty->base->kind == TY_VOID || ty->base->size <= 0) return 1;
    return ty->base->size;
}

static Node *build_add(Node *lhs, Node *rhs, SourceLoc loc) {
    /* numeric + numeric */
    if (is_integer_type(lhs->type) && is_integer_type(rhs->type)) {
        return new_binary(ND_ADD, lhs, rhs, common_type(lhs->type, rhs->type), loc);
    }
    /* ptr + ptr is invalid */
    if (is_pointer_like(lhs->type) && is_pointer_like(rhs->type)) {
        error_at(loc, "invalid operands to binary '+' (pointer + pointer)");
    }
    /* Canonicalize int + ptr -> ptr + int */
    if (!is_pointer_like(lhs->type) && is_pointer_like(rhs->type)) {
        Node *tmp = lhs;
        lhs = rhs;
        rhs = tmp;
    }
    int stride = pointee_stride(lhs->type);
    if (stride > 1) {
        rhs = new_binary(ND_MUL, rhs, new_int_node(stride, loc), ty_long, loc);
    }
    Type *res_ty = (lhs->type->kind == TY_ARRAY) ? pointer_to(lhs->type->base) : lhs->type;
    return new_binary(ND_ADD, lhs, rhs, res_ty, loc);
}

static Node *build_sub(Node *lhs, Node *rhs, SourceLoc loc) {
    /* numeric - numeric */
    if (is_integer_type(lhs->type) && is_integer_type(rhs->type)) {
        return new_binary(ND_SUB, lhs, rhs, common_type(lhs->type, rhs->type), loc);
    }
    /* ptr - int */
    if (is_pointer_like(lhs->type) && is_integer_type(rhs->type)) {
        int stride = pointee_stride(lhs->type);
        if (stride > 1) {
            rhs = new_binary(ND_MUL, rhs, new_int_node(stride, loc), ty_long, loc);
        }
        Type *res_ty = (lhs->type->kind == TY_ARRAY) ? pointer_to(lhs->type->base) : lhs->type;
        return new_binary(ND_SUB, lhs, rhs, res_ty, loc);
    }
    /* ptr - ptr */
    if (is_pointer_like(lhs->type) && is_pointer_like(rhs->type)) {
        int stride = pointee_stride(lhs->type);
        Node *diff = new_binary(ND_SUB, lhs, rhs, ty_long, loc);
        if (stride > 1) {
            return new_binary(ND_DIV, diff, new_int_node(stride, loc), ty_long, loc);
        }
        return diff;
    }
    error_at(loc, "invalid operands to binary '-'");
    return NULL;
}

/* ============================================================================
 * Constant Expression Evaluation
 * ============================================================================ */

static int64_t eval_node_const(Node *n, char **label_out) {
    if (!n) return 0;
    switch (n->kind) {
        case ND_INT_LIT:
            return n->int_val;
        case ND_NEG:
            return -eval_node_const(n->lhs, NULL);
        case ND_BITNOT:
            return ~eval_node_const(n->lhs, NULL);
        case ND_LOGNOT:
            return !eval_node_const(n->lhs, NULL);
        case ND_CAST: {
            int64_t v = eval_node_const(n->lhs, label_out);
            if (n->type->kind == TY_BOOL) return v != 0;
            if (n->type->size == 1) return n->type->is_unsigned ? (int64_t)(uint8_t)v : (int64_t)(int8_t)v;
            if (n->type->size == 2) return n->type->is_unsigned ? (int64_t)(uint16_t)v : (int64_t)(int16_t)v;
            if (n->type->size == 4) return n->type->is_unsigned ? (int64_t)(uint32_t)v : (int64_t)(int32_t)v;
            return v;
        }
        case ND_ADD:
            return eval_node_const(n->lhs, label_out) + eval_node_const(n->rhs, label_out);
        case ND_SUB:
            return eval_node_const(n->lhs, label_out) - eval_node_const(n->rhs, NULL);
        case ND_MUL:
            return eval_node_const(n->lhs, NULL) * eval_node_const(n->rhs, NULL);
        case ND_DIV: {
            int64_t r = eval_node_const(n->rhs, NULL);
            if (r == 0) error_at(n->loc, "division by zero in constant expression");
            return eval_node_const(n->lhs, NULL) / r;
        }
        case ND_MOD: {
            int64_t r = eval_node_const(n->rhs, NULL);
            if (r == 0) error_at(n->loc, "modulo by zero in constant expression");
            return eval_node_const(n->lhs, NULL) % r;
        }
        case ND_BITAND:
            return eval_node_const(n->lhs, NULL) & eval_node_const(n->rhs, NULL);
        case ND_BITOR:
            return eval_node_const(n->lhs, NULL) | eval_node_const(n->rhs, NULL);
        case ND_BITXOR:
            return eval_node_const(n->lhs, NULL) ^ eval_node_const(n->rhs, NULL);
        case ND_SHL:
            return eval_node_const(n->lhs, NULL) << eval_node_const(n->rhs, NULL);
        case ND_SHR:
            return eval_node_const(n->lhs, NULL) >> eval_node_const(n->rhs, NULL);
        case ND_EQ:
            return eval_node_const(n->lhs, NULL) == eval_node_const(n->rhs, NULL);
        case ND_NE:
            return eval_node_const(n->lhs, NULL) != eval_node_const(n->rhs, NULL);
        case ND_LT:
            return eval_node_const(n->lhs, NULL) < eval_node_const(n->rhs, NULL);
        case ND_LE:
            return eval_node_const(n->lhs, NULL) <= eval_node_const(n->rhs, NULL);
        case ND_GT:
            return eval_node_const(n->lhs, NULL) > eval_node_const(n->rhs, NULL);
        case ND_GE:
            return eval_node_const(n->lhs, NULL) >= eval_node_const(n->rhs, NULL);
        case ND_LOGAND:
            return eval_node_const(n->lhs, NULL) && eval_node_const(n->rhs, NULL);
        case ND_LOGOR:
            return eval_node_const(n->lhs, NULL) || eval_node_const(n->rhs, NULL);
        case ND_TERNARY:
            return eval_node_const(n->cond, NULL)
                       ? eval_node_const(n->then_branch, label_out)
                       : eval_node_const(n->else_branch, label_out);
        case ND_ADDR:
            if (label_out && n->lhs && n->lhs->kind == ND_VAR && !n->lhs->var->is_local) {
                *label_out = strdup(n->lhs->var->name);
                return 0;
            }
            break;
        case ND_VAR:
            if (label_out && !n->var->is_local &&
                (n->type->kind == TY_ARRAY || n->type->kind == TY_FUNC)) {
                *label_out = strdup(n->var->name);
                return 0;
            }
            break;
        case ND_STR_LIT:
            if (label_out) {
                char buf[64];
                snprintf(buf, sizeof(buf), ".L.str.%d", n->str_id);
                *label_out = strdup(buf);
                return 0;
            }
            break;
        default:
            break;
    }
    error_at(n->loc, "expression is not a compile-time constant");
    return 0;
}

static int64_t eval_const_expr(Parser *p) {
    Node *n = parse_conditional(p);
    return eval_node_const(n, NULL);
}

/* ============================================================================
 * Struct, Union, and Enum Specifiers
 * ============================================================================ */

static Type *parse_struct_or_union(Parser *p, TypeKind kind) {
    char *tag = NULL;
    if (peek_is(p, TOK_IDENT)) {
        tag = expect_ident(p);
    }

    if (tag && !peek_is(p, TOK_LBRACE)) {
        Type *existing = find_tag(p, tag);
        if (existing) {
            free(tag);
            return existing;
        }
        Type *ty = new_type(kind, 0, 1);
        ty->tag = tag;
        ty->is_complete = false;
        push_tag_scope(p, tag, ty);
        return ty;
    }

    expect(p, TOK_LBRACE);

    Type *ty = NULL;
    if (tag) {
        ty = find_tag_in_current_scope(p, tag);
        if (ty && ty->kind == kind && !ty->is_complete) {
            /* Completing a forward-declared struct/union */
        } else {
            ty = new_type(kind, 0, 1);
            ty->tag = strdup(tag);
            push_tag_scope(p, tag, ty);
        }
        free(tag);
    } else {
        ty = new_type(kind, 0, 1);
    }

    Member head = {0};
    Member *cur = &head;
    int offset = 0;
    int max_align = 1;
    int max_size = 0;

    while (!consume(p, TOK_RBRACE)) {
        Type *basety = parse_declspec(p, NULL, NULL, NULL);
        bool first = true;
        while (!consume(p, TOK_SEMI)) {
            if (!first) expect(p, TOK_COMMA);
            first = false;
            char *mem_name = NULL;
            Type *mem_ty = parse_declarator(p, basety, &mem_name);

            Member *m = (Member *)calloc(1, sizeof(Member));
            m->name = mem_name;
            m->type = mem_ty;
            if (mem_ty->align > max_align) max_align = mem_ty->align;

            if (kind == TY_STRUCT) {
                offset = align_to(offset, mem_ty->align);
                m->offset = offset;
                offset += mem_ty->size;
            } else {
                m->offset = 0;
                if (mem_ty->size > max_size) max_size = mem_ty->size;
            }
            cur = cur->next = m;
        }
    }

    ty->members = head.next;
    ty->align = max_align;
    ty->size = (kind == TY_STRUCT) ? align_to(offset, max_align) : align_to(max_size, max_align);
    ty->is_complete = true;
    return ty;
}

static Type *parse_enum(Parser *p) {
    char *tag = NULL;
    if (peek_is(p, TOK_IDENT)) {
        tag = expect_ident(p);
    }

    if (tag && !peek_is(p, TOK_LBRACE)) {
        Type *existing = find_tag(p, tag);
        if (!existing) {
            error_at(p->tok->loc, "unknown enum tag '%s'", tag);
        }
        free(tag);
        return existing;
    }

    expect(p, TOK_LBRACE);
    Type *ty = new_type(TY_ENUM, 4, 4);
    if (tag) {
        ty->tag = tag;
        push_tag_scope(p, tag, ty);
    }

    int64_t val = 0;
    while (!consume(p, TOK_RBRACE)) {
        char *name = expect_ident(p);
        if (consume(p, TOK_ASSIGN)) {
            val = eval_const_expr(p);
        }
        VarScope *vs = push_var_scope(p, name);
        vs->enum_ty = ty;
        vs->enum_val = val++;
        free(name);
        if (!consume(p, TOK_COMMA)) {
            expect(p, TOK_RBRACE);
            break;
        }
    }
    return ty;
}

/* ============================================================================
 * Declaration Specifiers & Declarators
 * ============================================================================ */

static Type *parse_declspec(Parser *p, bool *is_typedef, bool *is_static, bool *is_extern) {
    if (is_typedef) *is_typedef = false;
    if (is_static) *is_static = false;
    if (is_extern) *is_extern = false;

    int void_cnt = 0;
    int bool_cnt = 0;
    int char_cnt = 0;
    int short_cnt = 0;
    int int_cnt = 0;
    int long_cnt = 0;
    int signed_cnt = 0;
    int unsigned_cnt = 0;
    Type *user_type = NULL;

    while (is_typename(p, p->tok)) {
        Token *t = p->tok;
        if (t->kind == TOK_TYPEDEF) {
            if (is_typedef) *is_typedef = true;
            p->tok = p->tok->next;
            continue;
        }
        if (t->kind == TOK_STATIC) {
            if (is_static) *is_static = true;
            p->tok = p->tok->next;
            continue;
        }
        if (t->kind == TOK_EXTERN) {
            if (is_extern) *is_extern = true;
            p->tok = p->tok->next;
            continue;
        }
        if (t->kind == TOK_CONST) {
            p->tok = p->tok->next;
            continue;
        }

        /* User-defined type (typedef, struct, union, enum) */
        if (t->kind == TOK_IDENT) {
            if (user_type || void_cnt || bool_cnt || char_cnt || short_cnt || int_cnt || long_cnt) {
                break;
            }
            user_type = find_typedef(p, t->str_val);
            p->tok = p->tok->next;
            continue;
        }
        if (t->kind == TOK_STRUCT) {
            p->tok = p->tok->next;
            user_type = parse_struct_or_union(p, TY_STRUCT);
            continue;
        }
        if (t->kind == TOK_UNION) {
            p->tok = p->tok->next;
            user_type = parse_struct_or_union(p, TY_UNION);
            continue;
        }
        if (t->kind == TOK_ENUM) {
            p->tok = p->tok->next;
            user_type = parse_enum(p);
            continue;
        }

        switch (t->kind) {
            case TOK_VOID:     void_cnt++; break;
            case TOK_BOOL:     bool_cnt++; break;
            case TOK_CHAR:     char_cnt++; break;
            case TOK_SHORT:    short_cnt++; break;
            case TOK_INT:      int_cnt++; break;
            case TOK_LONG:     long_cnt++; break;
            case TOK_SIGNED:   signed_cnt++; break;
            case TOK_UNSIGNED: unsigned_cnt++; break;
            default: break;
        }
        p->tok = p->tok->next;
    }

    if (user_type) return user_type;
    if (void_cnt) return ty_void;
    if (bool_cnt) return ty_bool;
    if (char_cnt) return unsigned_cnt ? ty_uchar : ty_char;
    if (short_cnt) return unsigned_cnt ? ty_ushort : ty_short;
    if (long_cnt) return unsigned_cnt ? ty_ulong : ty_long;
    if (unsigned_cnt) return ty_uint;
    (void)signed_cnt;
    return ty_int;
}

static Type *parse_type_suffix(Parser *p, Type *base) {
    /* Function parameter list */
    if (consume(p, TOK_LPAREN)) {
        Type *fty = func_type(base);
        Param head = {0};
        Param *cur = &head;
        int count = 0;

        if (peek_is(p, TOK_VOID) && p->tok->next->kind == TOK_RPAREN) {
            p->tok = p->tok->next->next;
            fty->params = NULL;
            fty->param_count = 0;
            return fty;
        }

        while (!consume(p, TOK_RPAREN)) {
            if (count > 0) expect(p, TOK_COMMA);
            if (consume(p, TOK_ELLIPSIS)) {
                fty->is_variadic = true;
                expect(p, TOK_RPAREN);
                break;
            }
            Type *pty = parse_declspec(p, NULL, NULL, NULL);
            char *pname = NULL;
            pty = parse_declarator(p, pty, &pname);
            if (pty->kind == TY_ARRAY) {
                /* Array parameter decays to pointer */
                pty = pointer_to(pty->base);
            }
            Param *param = (Param *)calloc(1, sizeof(Param));
            param->name = pname;
            param->type = pty;
            cur = cur->next = param;
            count++;
        }
        fty->params = head.next;
        fty->param_count = count;
        return fty;
    }

    /* Array dimensions */
    if (consume(p, TOK_LBRACKET)) {
        int len = -1;
        if (!peek_is(p, TOK_RBRACKET)) {
            len = (int)eval_const_expr(p);
        }
        expect(p, TOK_RBRACKET);
        base = parse_type_suffix(p, base);
        return array_of(base, len < 0 ? 0 : len);
    }

    return base;
}

static Type *parse_declarator(Parser *p, Type *base, char **out_name) {
    while (consume(p, TOK_STAR)) {
        base = pointer_to(base);
        while (consume(p, TOK_CONST)) {}
    }

    /* Handle parenthesized declarator like int (*fp)(int) or int (*arr)[4] */
    if (peek_is(p, TOK_LPAREN) && !is_typename(p, p->tok->next) && p->tok->next->kind != TOK_RPAREN) {
        Token *start = p->tok;
        p->tok = p->tok->next; /* skip '(' */
        Type placeholder = {0};
        parse_declarator(p, &placeholder, out_name);
        expect(p, TOK_RPAREN);
        Type *real_base = parse_type_suffix(p, base);
        Token *after_suffix = p->tok;
        p->tok = start->next;
        Type *res = parse_declarator(p, real_base, out_name);
        p->tok = after_suffix;
        return res;
    }

    if (out_name && peek_is(p, TOK_IDENT)) {
        *out_name = expect_ident(p);
    } else if (out_name) {
        *out_name = NULL;
    }

    return parse_type_suffix(p, base);
}

static Type *parse_abstract_declarator(Parser *p, Type *base) {
    while (consume(p, TOK_STAR)) {
        base = pointer_to(base);
        while (consume(p, TOK_CONST)) {}
    }
    if (peek_is(p, TOK_LPAREN) && !is_typename(p, p->tok->next) && p->tok->next->kind != TOK_RPAREN) {
        Token *start = p->tok;
        p->tok = p->tok->next;
        Type placeholder = {0};
        parse_abstract_declarator(p, &placeholder);
        expect(p, TOK_RPAREN);
        Type *real_base = parse_type_suffix(p, base);
        Token *after_suffix = p->tok;
        p->tok = start->next;
        Type *res = parse_abstract_declarator(p, real_base);
        p->tok = after_suffix;
        return res;
    }
    return parse_type_suffix(p, base);
}

static Type *parse_type_name(Parser *p) {
    Type *base = parse_declspec(p, NULL, NULL, NULL);
    return parse_abstract_declarator(p, base);
}

/* ============================================================================
 * Expression Parsing
 * ============================================================================ */

static Node *parse_primary(Parser *p) {
    SourceLoc loc = p->tok->loc;

    if (consume(p, TOK_LPAREN)) {
        Node *n = parse_expr(p);
        expect(p, TOK_RPAREN);
        return n;
    }

    if (peek_is(p, TOK_SIZEOF)) {
        p->tok = p->tok->next;
        if (peek_is(p, TOK_LPAREN) && is_typename(p, p->tok->next)) {
            p->tok = p->tok->next;
            Type *ty = parse_type_name(p);
            expect(p, TOK_RPAREN);
            return new_int_node(ty->size, loc);
        }
        Node *n = parse_unary(p);
        return new_int_node(n->type->size, loc);
    }

    if (peek_is(p, TOK_INT_LIT) || peek_is(p, TOK_CHAR_LIT)) {
        int64_t val = p->tok->int_val;
        p->tok = p->tok->next;
        return new_int_node(val, loc);
    }

    if (peek_is(p, TOK_STR_LIT)) {
        Token *t = p->tok;
        p->tok = p->tok->next;
        int id = register_string_literal(p, t->str_val, t->str_len);
        Node *n = new_node(ND_STR_LIT, loc);
        n->str_id = id;
        n->str_data = t->str_val;
        n->str_len = t->str_len;
        n->type = array_of(ty_char, t->str_len + 1);
        return n;
    }

    if (peek_is(p, TOK_IDENT)) {
        char *name = expect_ident(p);

        /* Function call */
        if (consume(p, TOK_LPAREN)) {
            Node *n = new_node(ND_FUNCALL, loc);
            n->func_name = name;

            VarScope *vs = find_var(p, name);
            Type *fty = NULL;
            if (vs && vs->var && vs->var->type->kind == TY_FUNC) {
                fty = vs->var->type;
            } else if (vs && vs->var && vs->var->type->kind == TY_PTR &&
                       vs->var->type->base && vs->var->type->base->kind == TY_FUNC) {
                fty = vs->var->type->base;
            }
            n->func_type = fty;
            n->type = fty ? fty->return_type : ty_int;

            Node head = {0};
            Node *cur = &head;
            int argc = 0;
            while (!consume(p, TOK_RPAREN)) {
                if (argc > 0) expect(p, TOK_COMMA);
                Node *arg = parse_assign(p);
                if (arg->type->kind == TY_ARRAY) {
                    arg = new_unary(ND_ADDR, arg, pointer_to(arg->type->base), arg->loc);
                }
                cur = cur->next = arg;
                argc++;
            }
            n->args = head.next;
            n->arg_count = argc;
            return n;
        }

        /* Variable or enum constant */
        VarScope *vs = find_var(p, name);
        if (!vs) {
            error_at(loc, "undeclared identifier '%s'", name);
        }
        free(name);
        if (vs->enum_ty) {
            return new_int_node(vs->enum_val, loc);
        }
        if (!vs->var) {
            error_at(loc, "identifier is a typedef, not a variable");
        }
        Node *n = new_node(ND_VAR, loc);
        n->var = vs->var;
        n->type = vs->var->type;
        return n;
    }

    error_at(loc, "expected expression, but got '%.*s'", p->tok->len, p->tok->lexeme);
    return NULL;
}

static Node *struct_member_access(Node *lhs, const char *mem_name, SourceLoc loc) {
    if (lhs->type->kind != TY_STRUCT && lhs->type->kind != TY_UNION) {
        error_at(loc, "member reference base type is not a struct or union");
    }
    Member *mem = find_struct_member(lhs->type, mem_name);
    if (!mem) {
        error_at(loc, "no member named '%s' in struct/union", mem_name);
    }
    Node *n = new_node(ND_MEMBER, loc);
    n->lhs = lhs;
    n->member = mem;
    n->type = mem->type;
    return n;
}

static Node *parse_postfix(Parser *p) {
    Node *n = parse_primary(p);

    for (;;) {
        SourceLoc loc = p->tok->loc;

        /* Array subscript: a[i] -> *(a + i) */
        if (consume(p, TOK_LBRACKET)) {
            Node *idx = parse_expr(p);
            expect(p, TOK_RBRACKET);
            Node *add = build_add(n, idx, loc);
            n = new_unary(ND_DEREF, add, add->type->base, loc);
            continue;
        }

        /* Direct member access: x.m */
        if (consume(p, TOK_DOT)) {
            char *mem_name = expect_ident(p);
            n = struct_member_access(n, mem_name, loc);
            free(mem_name);
            continue;
        }

        /* Pointer member access: x->m -> (*x).m */
        if (consume(p, TOK_ARROW)) {
            char *mem_name = expect_ident(p);
            if (!is_pointer_like(n->type)) {
                error_at(loc, "member reference type is not a pointer");
            }
            Node *deref = new_unary(ND_DEREF, n, n->type->base, loc);
            n = struct_member_access(deref, mem_name, loc);
            free(mem_name);
            continue;
        }

        /* Postfix ++ */
        if (consume(p, TOK_INC)) {
            n = new_unary(ND_POST_INC, n, n->type, loc);
            continue;
        }

        /* Postfix -- */
        if (consume(p, TOK_DEC)) {
            n = new_unary(ND_POST_DEC, n, n->type, loc);
            continue;
        }

        break;
    }
    return n;
}

static Node *parse_unary(Parser *p) {
    SourceLoc loc = p->tok->loc;

    /* Cast expression: (type-name) unary */
    if (peek_is(p, TOK_LPAREN) && is_typename(p, p->tok->next)) {
        p->tok = p->tok->next;
        Type *ty = parse_type_name(p);
        expect(p, TOK_RPAREN);
        Node *operand = parse_unary(p);
        return new_unary(ND_CAST, operand, ty, loc);
    }

    if (consume(p, TOK_PLUS)) {
        return parse_unary(p);
    }
    if (consume(p, TOK_MINUS)) {
        Node *operand = parse_unary(p);
        return new_unary(ND_NEG, operand, operand->type->size < 4 ? ty_int : operand->type, loc);
    }
    if (consume(p, TOK_TILDE)) {
        Node *operand = parse_unary(p);
        return new_unary(ND_BITNOT, operand, operand->type->size < 4 ? ty_int : operand->type, loc);
    }
    if (consume(p, TOK_BANG)) {
        Node *operand = parse_unary(p);
        return new_unary(ND_LOGNOT, operand, ty_int, loc);
    }
    if (consume(p, TOK_AMP)) {
        Node *operand = parse_unary(p);
        if (operand->type->kind == TY_ARRAY) {
            return new_unary(ND_ADDR, operand, pointer_to(operand->type->base), loc);
        }
        return new_unary(ND_ADDR, operand, pointer_to(operand->type), loc);
    }
    if (consume(p, TOK_STAR)) {
        Node *operand = parse_unary(p);
        if (!operand->type->base) {
            error_at(loc, "indirection requires pointer operand");
        }
        Type *base = (operand->type->base->kind == TY_VOID) ? ty_char : operand->type->base;
        return new_unary(ND_DEREF, operand, base, loc);
    }
    if (consume(p, TOK_INC)) {
        Node *operand = parse_unary(p);
        return new_unary(ND_PRE_INC, operand, operand->type, loc);
    }
    if (consume(p, TOK_DEC)) {
        Node *operand = parse_unary(p);
        return new_unary(ND_PRE_DEC, operand, operand->type, loc);
    }

    return parse_postfix(p);
}

static Node *parse_mul(Parser *p) {
    Node *n = parse_unary(p);
    for (;;) {
        SourceLoc loc = p->tok->loc;
        if (consume(p, TOK_STAR)) {
            Node *r = parse_unary(p);
            n = new_binary(ND_MUL, n, r, common_type(n->type, r->type), loc);
        } else if (consume(p, TOK_SLASH)) {
            Node *r = parse_unary(p);
            n = new_binary(ND_DIV, n, r, common_type(n->type, r->type), loc);
        } else if (consume(p, TOK_PERCENT)) {
            Node *r = parse_unary(p);
            n = new_binary(ND_MOD, n, r, common_type(n->type, r->type), loc);
        } else {
            return n;
        }
    }
}

static Node *parse_add(Parser *p) {
    Node *n = parse_mul(p);
    for (;;) {
        SourceLoc loc = p->tok->loc;
        if (consume(p, TOK_PLUS)) {
            Node *r = parse_mul(p);
            n = build_add(n, r, loc);
        } else if (consume(p, TOK_MINUS)) {
            Node *r = parse_mul(p);
            n = build_sub(n, r, loc);
        } else {
            return n;
        }
    }
}

static Node *parse_shift(Parser *p) {
    Node *n = parse_add(p);
    for (;;) {
        SourceLoc loc = p->tok->loc;
        if (consume(p, TOK_LSHIFT)) {
            Node *r = parse_add(p);
            n = new_binary(ND_SHL, n, r, n->type->size < 4 ? ty_int : n->type, loc);
        } else if (consume(p, TOK_RSHIFT)) {
            Node *r = parse_add(p);
            n = new_binary(ND_SHR, n, r, n->type->size < 4 ? ty_int : n->type, loc);
        } else {
            return n;
        }
    }
}

static Node *parse_relational(Parser *p) {
    Node *n = parse_shift(p);
    for (;;) {
        SourceLoc loc = p->tok->loc;
        if (consume(p, TOK_LT)) {
            n = new_binary(ND_LT, n, parse_shift(p), ty_int, loc);
        } else if (consume(p, TOK_LE)) {
            n = new_binary(ND_LE, n, parse_shift(p), ty_int, loc);
        } else if (consume(p, TOK_GT)) {
            n = new_binary(ND_GT, n, parse_shift(p), ty_int, loc);
        } else if (consume(p, TOK_GE)) {
            n = new_binary(ND_GE, n, parse_shift(p), ty_int, loc);
        } else {
            return n;
        }
    }
}

static Node *parse_equality(Parser *p) {
    Node *n = parse_relational(p);
    for (;;) {
        SourceLoc loc = p->tok->loc;
        if (consume(p, TOK_EQ)) {
            n = new_binary(ND_EQ, n, parse_relational(p), ty_int, loc);
        } else if (consume(p, TOK_NE)) {
            n = new_binary(ND_NE, n, parse_relational(p), ty_int, loc);
        } else {
            return n;
        }
    }
}

static Node *parse_bitand(Parser *p) {
    Node *n = parse_equality(p);
    while (consume(p, TOK_AMP)) {
        SourceLoc loc = p->tok->loc;
        Node *r = parse_equality(p);
        n = new_binary(ND_BITAND, n, r, common_type(n->type, r->type), loc);
    }
    return n;
}

static Node *parse_bitxor(Parser *p) {
    Node *n = parse_bitand(p);
    while (consume(p, TOK_CARET)) {
        SourceLoc loc = p->tok->loc;
        Node *r = parse_bitand(p);
        n = new_binary(ND_BITXOR, n, r, common_type(n->type, r->type), loc);
    }
    return n;
}

static Node *parse_bitor(Parser *p) {
    Node *n = parse_bitxor(p);
    while (consume(p, TOK_PIPE)) {
        SourceLoc loc = p->tok->loc;
        Node *r = parse_bitxor(p);
        n = new_binary(ND_BITOR, n, r, common_type(n->type, r->type), loc);
    }
    return n;
}

static Node *parse_logand(Parser *p) {
    Node *n = parse_bitor(p);
    while (consume(p, TOK_LOGAND)) {
        SourceLoc loc = p->tok->loc;
        Node *r = parse_bitor(p);
        n = new_binary(ND_LOGAND, n, r, ty_int, loc);
    }
    return n;
}

static Node *parse_logor(Parser *p) {
    Node *n = parse_logand(p);
    while (consume(p, TOK_LOGOR)) {
        SourceLoc loc = p->tok->loc;
        Node *r = parse_logand(p);
        n = new_binary(ND_LOGOR, n, r, ty_int, loc);
    }
    return n;
}

static Node *parse_conditional(Parser *p) {
    Node *cond = parse_logor(p);
    if (!consume(p, TOK_QUESTION)) {
        return cond;
    }
    SourceLoc loc = p->tok->loc;
    Node *then_b = parse_expr(p);
    expect(p, TOK_COLON);
    Node *else_b = parse_conditional(p);
    Node *n = new_node(ND_TERNARY, loc);
    n->cond = cond;
    n->then_branch = then_b;
    n->else_branch = else_b;
    n->type = common_type(then_b->type, else_b->type);
    return n;
}

/* Convert compound assignment `A op= B` into `tmp = &A, *tmp = *tmp op B` so A is evaluated once */
static Node *build_compound_assign(Parser *p, NodeKind op, Node *lhs, Node *rhs, SourceLoc loc) {
    VarObj *tmp_ptr = new_local_var(p, "", pointer_to(lhs->type));
    Node *var_node1 = new_node(ND_VAR, loc);
    var_node1->var = tmp_ptr;
    var_node1->type = tmp_ptr->type;

    Node *var_node2 = new_node(ND_VAR, loc);
    var_node2->var = tmp_ptr;
    var_node2->type = tmp_ptr->type;

    Node *var_node3 = new_node(ND_VAR, loc);
    var_node3->var = tmp_ptr;
    var_node3->type = tmp_ptr->type;

    Node *e1 = new_binary(ND_ASSIGN, var_node1, new_unary(ND_ADDR, lhs, tmp_ptr->type, loc), tmp_ptr->type, loc);
    Node *deref_lhs = new_unary(ND_DEREF, var_node2, lhs->type, loc);
    Node *deref_read = new_unary(ND_DEREF, var_node3, lhs->type, loc);

    Node *val = NULL;
    if (op == ND_ADD) {
        val = build_add(deref_read, rhs, loc);
    } else if (op == ND_SUB) {
        val = build_sub(deref_read, rhs, loc);
    } else {
        val = new_binary(op, deref_read, rhs, common_type(lhs->type, rhs->type), loc);
    }
    Node *e2 = new_binary(ND_ASSIGN, deref_lhs, val, lhs->type, loc);
    return new_binary(ND_COMMA, e1, e2, lhs->type, loc);
}

static Node *parse_assign(Parser *p) {
    Node *n = parse_conditional(p);
    SourceLoc loc = p->tok->loc;

    if (consume(p, TOK_ASSIGN)) {
        return new_binary(ND_ASSIGN, n, parse_assign(p), n->type, loc);
    }
    if (consume(p, TOK_ADD_ASSIGN)) {
        return build_compound_assign(p, ND_ADD, n, parse_assign(p), loc);
    }
    if (consume(p, TOK_SUB_ASSIGN)) {
        return build_compound_assign(p, ND_SUB, n, parse_assign(p), loc);
    }
    if (consume(p, TOK_MUL_ASSIGN)) {
        return build_compound_assign(p, ND_MUL, n, parse_assign(p), loc);
    }
    if (consume(p, TOK_DIV_ASSIGN)) {
        return build_compound_assign(p, ND_DIV, n, parse_assign(p), loc);
    }
    if (consume(p, TOK_MOD_ASSIGN)) {
        return build_compound_assign(p, ND_MOD, n, parse_assign(p), loc);
    }
    if (consume(p, TOK_AND_ASSIGN)) {
        return build_compound_assign(p, ND_BITAND, n, parse_assign(p), loc);
    }
    if (consume(p, TOK_OR_ASSIGN)) {
        return build_compound_assign(p, ND_BITOR, n, parse_assign(p), loc);
    }
    if (consume(p, TOK_XOR_ASSIGN)) {
        return build_compound_assign(p, ND_BITXOR, n, parse_assign(p), loc);
    }
    if (consume(p, TOK_SHL_ASSIGN)) {
        return build_compound_assign(p, ND_SHL, n, parse_assign(p), loc);
    }
    if (consume(p, TOK_SHR_ASSIGN)) {
        return build_compound_assign(p, ND_SHR, n, parse_assign(p), loc);
    }
    return n;
}

static Node *parse_expr(Parser *p) {
    Node *n = parse_assign(p);
    while (consume(p, TOK_COMMA)) {
        SourceLoc loc = p->tok->loc;
        Node *r = parse_assign(p);
        n = new_binary(ND_COMMA, n, r, r->type, loc);
    }
    return n;
}

/* ============================================================================
 * Local Variable Declarations & Initializers
 * ============================================================================ */

static Node *make_var_ref(VarObj *v, SourceLoc loc) {
    Node *n = new_node(ND_VAR, loc);
    n->var = v;
    n->type = v->type;
    return n;
}

static void parse_local_initializer(Parser *p, Node *lhs_expr, Type *ty, Node ***cur_stmt_tail, SourceLoc loc) {
    /* char arr[] = "string" */
    if (ty->kind == TY_ARRAY && ty->base->kind == TY_CHAR && peek_is(p, TOK_STR_LIT)) {
        Token *t = p->tok;
        p->tok = p->tok->next;
        int slen = t->str_len + 1;
        if (ty->array_len == 0) {
            ty->array_len = slen;
            ty->size = slen;
        }
        int limit = ty->array_len;
        for (int i = 0; i < limit; i++) {
            int ch = (i < t->str_len) ? (unsigned char)t->str_val[i] : 0;
            Node *addr = build_add(lhs_expr, new_int_node(i, loc), loc);
            Node *deref = new_unary(ND_DEREF, addr, ty_char, loc);
            Node *asg = new_binary(ND_ASSIGN, deref, new_int_node(ch, loc), ty_char, loc);
            Node *st = new_unary(ND_EXPR_STMT, asg, ty_void, loc);
            **cur_stmt_tail = st;
            *cur_stmt_tail = &st->next;
        }
        return;
    }

    /* Array brace initializer { e0, e1, ... } */
    if (ty->kind == TY_ARRAY && consume(p, TOK_LBRACE)) {
        int i = 0;
        while (!consume(p, TOK_RBRACE)) {
            if (i > 0) expect(p, TOK_COMMA);
            if (consume(p, TOK_RBRACE)) break;
            Node *addr = build_add(lhs_expr, new_int_node(i, loc), loc);
            Node *elem = new_unary(ND_DEREF, addr, ty->base, loc);
            parse_local_initializer(p, elem, ty->base, cur_stmt_tail, loc);
            i++;
        }
        if (ty->array_len == 0) {
            ty->array_len = i;
            ty->size = i * ty->base->size;
        }
        /* Zero-fill remaining elements */
        for (; i < ty->array_len; i++) {
            Node *addr = build_add(lhs_expr, new_int_node(i, loc), loc);
            Node *elem = new_unary(ND_DEREF, addr, ty->base, loc);
            if (is_scalar_type(ty->base)) {
                Node *asg = new_binary(ND_ASSIGN, elem, new_int_node(0, loc), ty->base, loc);
                Node *st = new_unary(ND_EXPR_STMT, asg, ty_void, loc);
                **cur_stmt_tail = st;
                *cur_stmt_tail = &st->next;
            }
        }
        return;
    }

    /* Struct brace initializer { m0, m1, ... } */
    if (ty->kind == TY_STRUCT && consume(p, TOK_LBRACE)) {
        Member *mem = ty->members;
        int i = 0;
        while (!consume(p, TOK_RBRACE)) {
            if (i > 0) expect(p, TOK_COMMA);
            if (consume(p, TOK_RBRACE)) break;
            /* Designated initializer `.field = val` */
            if (consume(p, TOK_DOT)) {
                char *fname = expect_ident(p);
                expect(p, TOK_ASSIGN);
                Member *target_m = find_struct_member(ty, fname);
                if (!target_m) error_at(loc, "no member named '%s'", fname);
                free(fname);
                Node *mnode = new_node(ND_MEMBER, loc);
                mnode->lhs = lhs_expr;
                mnode->member = target_m;
                mnode->type = target_m->type;
                parse_local_initializer(p, mnode, target_m->type, cur_stmt_tail, loc);
                mem = target_m->next;
            } else if (mem) {
                Node *mnode = new_node(ND_MEMBER, loc);
                mnode->lhs = lhs_expr;
                mnode->member = mem;
                mnode->type = mem->type;
                parse_local_initializer(p, mnode, mem->type, cur_stmt_tail, loc);
                mem = mem->next;
            } else {
                parse_assign(p);
            }
            i++;
        }
        /* Zero-fill remaining scalar fields if partially initialized */
        for (; mem; mem = mem->next) {
            if (is_scalar_type(mem->type)) {
                Node *mnode = new_node(ND_MEMBER, loc);
                mnode->lhs = lhs_expr;
                mnode->member = mem;
                mnode->type = mem->type;
                Node *asg = new_binary(ND_ASSIGN, mnode, new_int_node(0, loc), mem->type, loc);
                Node *st = new_unary(ND_EXPR_STMT, asg, ty_void, loc);
                **cur_stmt_tail = st;
                *cur_stmt_tail = &st->next;
            }
        }
        return;
    }

    /* Scalar or struct assignment */
    bool braced_scalar = consume(p, TOK_LBRACE);
    Node *rhs = parse_assign(p);
    if (braced_scalar) {
        consume(p, TOK_COMMA);
        expect(p, TOK_RBRACE);
    }
    Node *asg = new_binary(ND_ASSIGN, lhs_expr, rhs, ty, loc);
    Node *st = new_unary(ND_EXPR_STMT, asg, ty_void, loc);
    **cur_stmt_tail = st;
    *cur_stmt_tail = &st->next;
}

static Node *parse_declaration_stmt(Parser *p) {
    SourceLoc loc = p->tok->loc;
    bool is_typedef = false;
    bool is_static = false;
    bool is_extern = false;
    Type *basety = parse_declspec(p, &is_typedef, &is_static, &is_extern);

    Node head = {0};
    Node **cur_tail = &head.next;

    int count = 0;
    while (!consume(p, TOK_SEMI)) {
        if (count++ > 0) expect(p, TOK_COMMA);
        char *name = NULL;
        Type *ty = parse_declarator(p, basety, &name);
        if (!name) {
            error_at(loc, "declaration missing variable name");
        }
        if (is_typedef) {
            push_var_scope(p, name)->type_def = ty;
            free(name);
            continue;
        }
        if (ty->kind == TY_VOID) {
            error_at(loc, "variable '%s' declared void", name);
        }
        VarObj *v = new_local_var(p, name, ty);
        free(name);

        if (consume(p, TOK_ASSIGN)) {
            Node *var_ref = make_var_ref(v, loc);
            parse_local_initializer(p, var_ref, v->type, &cur_tail, loc);
        }
    }

    Node *blk = new_node(ND_BLOCK, loc);
    blk->body = head.next;
    return blk;
}

/* ============================================================================
 * Statement Parsing
 * ============================================================================ */

static Node *parse_stmt(Parser *p) {
    SourceLoc loc = p->tok->loc;

    if (consume(p, TOK_SEMI)) {
        return new_node(ND_EMPTY, loc);
    }

    if (peek_is(p, TOK_LBRACE)) {
        return parse_compound_stmt(p);
    }

    if (consume(p, TOK_IF)) {
        expect(p, TOK_LPAREN);
        Node *cond = parse_expr(p);
        expect(p, TOK_RPAREN);
        Node *then_b = parse_stmt(p);
        Node *else_b = NULL;
        if (consume(p, TOK_ELSE)) {
            else_b = parse_stmt(p);
        }
        Node *n = new_node(ND_IF, loc);
        n->cond = cond;
        n->then_branch = then_b;
        n->else_branch = else_b;
        return n;
    }

    if (consume(p, TOK_WHILE)) {
        expect(p, TOK_LPAREN);
        Node *cond = parse_expr(p);
        expect(p, TOK_RPAREN);
        Node *body = parse_stmt(p);
        Node *n = new_node(ND_WHILE, loc);
        n->cond = cond;
        n->then_branch = body;
        return n;
    }

    if (consume(p, TOK_DO)) {
        Node *body = parse_stmt(p);
        expect(p, TOK_WHILE);
        expect(p, TOK_LPAREN);
        Node *cond = parse_expr(p);
        expect(p, TOK_RPAREN);
        expect(p, TOK_SEMI);
        Node *n = new_node(ND_DO_WHILE, loc);
        n->then_branch = body;
        n->cond = cond;
        return n;
    }

    if (consume(p, TOK_FOR)) {
        expect(p, TOK_LPAREN);
        enter_scope(p);
        Node *init = NULL;
        if (!consume(p, TOK_SEMI)) {
            if (is_typename(p, p->tok)) {
                init = parse_declaration_stmt(p);
            } else {
                init = new_unary(ND_EXPR_STMT, parse_expr(p), ty_void, p->tok->loc);
                expect(p, TOK_SEMI);
            }
        }
        Node *cond = NULL;
        if (!consume(p, TOK_SEMI)) {
            cond = parse_expr(p);
            expect(p, TOK_SEMI);
        }
        Node *step = NULL;
        if (!consume(p, TOK_RPAREN)) {
            step = parse_expr(p);
            expect(p, TOK_RPAREN);
        }
        Node *body = parse_stmt(p);
        leave_scope(p);

        Node *n = new_node(ND_FOR, loc);
        n->init = init;
        n->cond = cond;
        n->step = step;
        n->then_branch = body;
        return n;
    }

    if (consume(p, TOK_SWITCH)) {
        expect(p, TOK_LPAREN);
        Node *cond = parse_expr(p);
        expect(p, TOK_RPAREN);
        Node *n = new_node(ND_SWITCH, loc);
        n->cond = cond;
        Node *prev_sw = p->current_switch;
        p->current_switch = n;
        n->then_branch = parse_stmt(p);
        p->current_switch = prev_sw;
        return n;
    }

    if (consume(p, TOK_CASE)) {
        if (!p->current_switch) {
            error_at(loc, "'case' statement not within a switch");
        }
        int64_t val = eval_const_expr(p);
        expect(p, TOK_COLON);
        Node *n = new_node(ND_CASE, loc);
        n->int_val = val;
        n->lhs = parse_stmt(p);
        n->cases_next = p->current_switch->cases_next;
        p->current_switch->cases_next = n;
        return n;
    }

    if (consume(p, TOK_DEFAULT)) {
        if (!p->current_switch) {
            error_at(loc, "'default' statement not within a switch");
        }
        expect(p, TOK_COLON);
        Node *n = new_node(ND_CASE, loc);
        n->lhs = parse_stmt(p);
        p->current_switch->default_case = n;
        return n;
    }

    if (consume(p, TOK_BREAK)) {
        expect(p, TOK_SEMI);
        return new_node(ND_BREAK, loc);
    }

    if (consume(p, TOK_CONTINUE)) {
        expect(p, TOK_SEMI);
        return new_node(ND_CONTINUE, loc);
    }

    if (consume(p, TOK_GOTO)) {
        char *lbl = expect_ident(p);
        expect(p, TOK_SEMI);
        Node *n = new_node(ND_GOTO, loc);
        n->label_name = lbl;
        return n;
    }

    if (consume(p, TOK_RETURN)) {
        Node *n = new_node(ND_RETURN, loc);
        if (!consume(p, TOK_SEMI)) {
            n->lhs = parse_expr(p);
            expect(p, TOK_SEMI);
        }
        return n;
    }

    /* Named label: ident ':' stmt */
    if (peek_is(p, TOK_IDENT) && p->tok->next->kind == TOK_COLON) {
        char *lbl = expect_ident(p);
        expect(p, TOK_COLON);
        Node *n = new_node(ND_LABEL, loc);
        n->label_name = lbl;
        n->lhs = parse_stmt(p);
        return n;
    }

    /* Local declaration */
    if (is_typename(p, p->tok)) {
        return parse_declaration_stmt(p);
    }

    /* Expression statement */
    Node *expr = parse_expr(p);
    expect(p, TOK_SEMI);
    return new_unary(ND_EXPR_STMT, expr, ty_void, loc);
}

static Node *parse_compound_stmt(Parser *p) {
    SourceLoc loc = p->tok->loc;
    expect(p, TOK_LBRACE);
    enter_scope(p);

    Node head = {0};
    Node *cur = &head;
    while (!consume(p, TOK_RBRACE)) {
        if (at_eof(p)) {
            error_at(p->tok->loc, "unexpected EOF inside compound block");
        }
        cur = cur->next = parse_stmt(p);
    }

    leave_scope(p);
    Node *blk = new_node(ND_BLOCK, loc);
    blk->body = head.next;
    return blk;
}

/* ============================================================================
 * Top-Level Declarations & Global Initializers
 * ============================================================================ */

static void append_global_init(GlobalInit ***tail, int offset, int size, int64_t val, char *label, int64_t addend) {
    GlobalInit *gi = (GlobalInit *)calloc(1, sizeof(GlobalInit));
    gi->offset = offset;
    gi->size = size;
    gi->val = val;
    gi->label = label;
    gi->addend = addend;
    **tail = gi;
    *tail = &gi->next;
}

static void parse_global_initializer(Parser *p, Type *ty, int base_offset, GlobalInit ***tail) {
    if (ty->kind == TY_ARRAY && ty->base->kind == TY_CHAR && peek_is(p, TOK_STR_LIT)) {
        Token *t = p->tok;
        p->tok = p->tok->next;
        int slen = t->str_len + 1;
        if (ty->array_len == 0) {
            ty->array_len = slen;
            ty->size = slen;
        }
        for (int i = 0; i < ty->array_len; i++) {
            int ch = (i < t->str_len) ? (unsigned char)t->str_val[i] : 0;
            append_global_init(tail, base_offset + i, 1, ch, NULL, 0);
        }
        return;
    }

    if (ty->kind == TY_ARRAY && consume(p, TOK_LBRACE)) {
        int i = 0;
        while (!consume(p, TOK_RBRACE)) {
            if (i > 0) expect(p, TOK_COMMA);
            if (consume(p, TOK_RBRACE)) break;
            parse_global_initializer(p, ty->base, base_offset + i * ty->base->size, tail);
            i++;
        }
        if (ty->array_len == 0) {
            ty->array_len = i;
            ty->size = i * ty->base->size;
        }
        return;
    }

    if (ty->kind == TY_STRUCT && consume(p, TOK_LBRACE)) {
        Member *mem = ty->members;
        int i = 0;
        while (!consume(p, TOK_RBRACE)) {
            if (i > 0) expect(p, TOK_COMMA);
            if (consume(p, TOK_RBRACE)) break;
            if (mem) {
                parse_global_initializer(p, mem->type, base_offset + mem->offset, tail);
                mem = mem->next;
            } else {
                parse_assign(p);
            }
            i++;
        }
        return;
    }

    Node *expr = parse_assign(p);
    char *label = NULL;
    int64_t val = eval_node_const(expr, &label);
    append_global_init(tail, base_offset, ty->size, label ? 0 : val, label, label ? val : 0);
}

ProgramAST *parse_program(Token *tokens) {
    Parser p;
    memset(&p, 0, sizeof(p));
    p.tok = tokens;
    enter_scope(&p);

    FunctionDef func_head = {0};
    FunctionDef *func_cur = &func_head;

    while (!at_eof(&p)) {
        bool is_typedef = false;
        bool is_static = false;
        bool is_extern = false;
        Type *basety = parse_declspec(&p, &is_typedef, &is_static, &is_extern);

        /* Standalone struct/union/enum declaration like `struct Foo { ... };` */
        if (consume(&p, TOK_SEMI)) {
            continue;
        }

        int count = 0;
        while (!at_eof(&p)) {
            if (count++ > 0) {
                expect(&p, TOK_COMMA);
            }
            char *name = NULL;
            Type *ty = parse_declarator(&p, basety, &name);
            if (!name) {
                error_at(p.tok->loc, "expected identifier in top-level declaration");
            }

            if (is_typedef) {
                push_var_scope(&p, name)->type_def = ty;
                free(name);
                if (consume(&p, TOK_SEMI)) break;
                continue;
            }

            /* Function declaration or definition */
            if (ty->kind == TY_FUNC) {
                new_global_var(&p, name, ty, is_static, true);

                if (consume(&p, TOK_SEMI)) {
                    free(name);
                    break;
                }

                /* Function definition with body `{ ... }` */
                FunctionDef *fn = (FunctionDef *)calloc(1, sizeof(FunctionDef));
                fn->name = name;
                fn->type = ty;
                fn->is_static = is_static;

                p.locals = NULL;
                p.local_count = 0;
                p.current_func_ret = ty->return_type;
                enter_scope(&p);

                /* Create local variables for function parameters (in order) */
                VarObj param_head = {0};
                VarObj *param_cur = &param_head;
                for (Param *prm = ty->params; prm; prm = prm->next) {
                    const char *pname = prm->name ? prm->name : "";
                    VarObj *v = new_local_var(&p, pname, prm->type);
                    v->is_param = true;

                    VarObj *p_copy = (VarObj *)calloc(1, sizeof(VarObj));
                    *p_copy = *v;
                    p_copy->next = NULL;
                    param_cur = param_cur->next = p_copy;
                }
                fn->params = param_head.next;

                fn->body = parse_compound_stmt(&p);
                fn->locals = p.locals;
                fn->local_count = p.local_count;
                leave_scope(&p);

                func_cur = func_cur->next = fn;
                break;
            }

            /* Global variable */
            VarObj *gvar = new_global_var(&p, name, ty, is_static, is_extern);
            free(name);

            if (consume(&p, TOK_ASSIGN)) {
                gvar->has_init = true;
                gvar->is_extern = false;
                GlobalInit *head_init = NULL;
                GlobalInit **tail_init = &head_init;
                parse_global_initializer(&p, gvar->type, 0, &tail_init);
                gvar->init_list = head_init;
            }

            if (consume(&p, TOK_SEMI)) {
                break;
            }
        }
    }

    ProgramAST *prog = (ProgramAST *)calloc(1, sizeof(ProgramAST));
    prog->functions = func_head.next;
    prog->globals = p.globals;
    prog->strings = p.strings;
    return prog;
}

/* ============================================================================
 * AST Pretty-Printer (--emit-ast)
 * ============================================================================ */

static void dump_indent(FILE *out, int indent) {
    for (int i = 0; i < indent; i++) fprintf(out, "  ");
}

static void dump_node(const Node *n, FILE *out, int indent) {
    if (!n) return;
    dump_indent(out, indent);
    char ty_buf[64];
    format_type(n->type, ty_buf, sizeof(ty_buf));

    switch (n->kind) {
        case ND_BLOCK:
            fprintf(out, "Block {\n");
            for (const Node *s = n->body; s; s = s->next) dump_node(s, out, indent + 1);
            dump_indent(out, indent);
            fprintf(out, "}\n");
            break;
        case ND_IF:
            fprintf(out, "If\n");
            dump_node(n->cond, out, indent + 1);
            dump_node(n->then_branch, out, indent + 1);
            if (n->else_branch) dump_node(n->else_branch, out, indent + 1);
            break;
        case ND_WHILE:
            fprintf(out, "While\n");
            dump_node(n->cond, out, indent + 1);
            dump_node(n->then_branch, out, indent + 1);
            break;
        case ND_DO_WHILE:
            fprintf(out, "DoWhile\n");
            dump_node(n->then_branch, out, indent + 1);
            dump_node(n->cond, out, indent + 1);
            break;
        case ND_FOR:
            fprintf(out, "For\n");
            if (n->init) dump_node(n->init, out, indent + 1);
            if (n->cond) dump_node(n->cond, out, indent + 1);
            if (n->step) dump_node(n->step, out, indent + 1);
            dump_node(n->then_branch, out, indent + 1);
            break;
        case ND_RETURN:
            fprintf(out, "Return\n");
            if (n->lhs) dump_node(n->lhs, out, indent + 1);
            break;
        case ND_EXPR_STMT:
            fprintf(out, "ExprStmt\n");
            dump_node(n->lhs, out, indent + 1);
            break;
        case ND_INT_LIT:
            fprintf(out, "IntLit(%lld : %s)\n", (long long)n->int_val, ty_buf);
            break;
        case ND_STR_LIT:
            fprintf(out, "StrLit(#%d \"%s\" : %s)\n", n->str_id, n->str_data, ty_buf);
            break;
        case ND_VAR:
            fprintf(out, "Var(%s : %s)\n", n->var->name, ty_buf);
            break;
        case ND_MEMBER:
            fprintf(out, "Member(.%s : %s)\n", n->member->name, ty_buf);
            dump_node(n->lhs, out, indent + 1);
            break;
        case ND_FUNCALL:
            fprintf(out, "Call(%s : %s)\n", n->func_name, ty_buf);
            for (const Node *a = n->args; a; a = a->next) dump_node(a, out, indent + 1);
            break;
        case ND_ASSIGN:
            fprintf(out, "Assign(: %s)\n", ty_buf);
            dump_node(n->lhs, out, indent + 1);
            dump_node(n->rhs, out, indent + 1);
            break;
        default:
            fprintf(out, "Node(kind=%d : %s)\n", n->kind, ty_buf);
            if (n->lhs) dump_node(n->lhs, out, indent + 1);
            if (n->rhs) dump_node(n->rhs, out, indent + 1);
            break;
    }
}

void dump_ast(const ProgramAST *prog, FILE *out) {
    fprintf(out, "=== Program AST ===\n");
    for (const FunctionDef *fn = prog->functions; fn; fn = fn->next) {
        char ty_buf[64];
        format_type(fn->type->return_type, ty_buf, sizeof(ty_buf));
        fprintf(out, "\nFunction %s -> %s (%d locals):\n", fn->name, ty_buf, fn->local_count);
        dump_node(fn->body, out, 1);
    }
}
