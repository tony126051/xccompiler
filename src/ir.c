#include "xcc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct NamedLabel {
    char *name;
    int label_id;
    struct NamedLabel *next;
} NamedLabel;

typedef struct {
    IRFunc *fn;
    int next_label;
    int break_label;
    int continue_label;
    NamedLabel *named_labels;
} IRBuilder;

static int new_vreg(IRBuilder *b) {
    return b->fn->vreg_count++;
}

static int new_label(IRBuilder *b) {
    return ++b->next_label;
}

static int get_named_label(IRBuilder *b, const char *name) {
    for (NamedLabel *nl = b->named_labels; nl; nl = nl->next) {
        if (strcmp(nl->name, name) == 0) {
            return nl->label_id;
        }
    }
    NamedLabel *nl = (NamedLabel *)calloc(1, sizeof(NamedLabel));
    nl->name = strdup(name);
    nl->label_id = new_label(b);
    nl->next = b->named_labels;
    b->named_labels = nl;
    return nl->label_id;
}

static IRInst *emit_inst(IRBuilder *b, IROp op) {
    if (b->fn->inst_count + 1 > b->fn->inst_cap) {
        b->fn->inst_cap = b->fn->inst_cap ? b->fn->inst_cap * 2 : 64;
        b->fn->insts = (IRInst *)realloc(b->fn->insts, (size_t)b->fn->inst_cap * sizeof(IRInst));
    }
    IRInst *inst = &b->fn->insts[b->fn->inst_count++];
    memset(inst, 0, sizeof(IRInst));
    inst->op = op;
    inst->dst = -1;
    inst->src1 = -1;
    inst->src2 = -1;
    inst->mem_size = 8;
    return inst;
}

static void emit_label(IRBuilder *b, int label_id) {
    IRInst *inst = emit_inst(b, IR_LABEL);
    inst->label = label_id;
}

static void emit_jmp(IRBuilder *b, int label_id) {
    IRInst *inst = emit_inst(b, IR_JMP);
    inst->label = label_id;
}

static void emit_jz(IRBuilder *b, int cond_vreg, int label_id) {
    IRInst *inst = emit_inst(b, IR_JZ);
    inst->src1 = cond_vreg;
    inst->label = label_id;
}

static void emit_jnz(IRBuilder *b, int cond_vreg, int label_id) {
    IRInst *inst = emit_inst(b, IR_JNZ);
    inst->src1 = cond_vreg;
    inst->label = label_id;
}

static int emit_imm(IRBuilder *b, int64_t val) {
    int dst = new_vreg(b);
    IRInst *inst = emit_inst(b, IR_IMM);
    inst->dst = dst;
    inst->imm = val;
    return dst;
}

static int gen_expr(IRBuilder *b, const Node *n);
static int gen_addr(IRBuilder *b, const Node *n);

static int gen_addr(IRBuilder *b, const Node *n) {
    switch (n->kind) {
        case ND_VAR: {
            int dst = new_vreg(b);
            if (n->var->is_local) {
                IRInst *inst = emit_inst(b, IR_LOCAL_ADDR);
                inst->dst = dst;
                inst->imm = n->var->local_id;
            } else {
                IRInst *inst = emit_inst(b, IR_GLOBAL_ADDR);
                inst->dst = dst;
                inst->sym_name = n->var->name;
            }
            return dst;
        }
        case ND_STR_LIT: {
            int dst = new_vreg(b);
            IRInst *inst = emit_inst(b, IR_STR_ADDR);
            inst->dst = dst;
            inst->imm = n->str_id;
            return dst;
        }
        case ND_DEREF:
            return gen_expr(b, n->lhs);
        case ND_MEMBER: {
            int base_addr = gen_addr(b, n->lhs);
            if (n->member->offset == 0) {
                return base_addr;
            }
            int off_vreg = emit_imm(b, n->member->offset);
            int dst = new_vreg(b);
            IRInst *inst = emit_inst(b, IR_ADD);
            inst->dst = dst;
            inst->src1 = base_addr;
            inst->src2 = off_vreg;
            return dst;
        }
        case ND_COMMA:
            gen_expr(b, n->lhs);
            return gen_addr(b, n->rhs);
        default:
            error_at(n->loc, "expression is not an lvalue");
            return -1;
    }
}

static int emit_load_from(IRBuilder *b, int addr_vreg, Type *ty) {
    if (ty->kind == TY_ARRAY || ty->kind == TY_FUNC ||
        ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
        return addr_vreg;
    }
    int dst = new_vreg(b);
    IRInst *inst = emit_inst(b, IR_LOAD);
    inst->dst = dst;
    inst->src1 = addr_vreg;
    inst->mem_size = ty->size;
    inst->is_unsigned = ty->is_unsigned;
    return dst;
}

static void emit_store_to(IRBuilder *b, int addr_vreg, int val_vreg, Type *ty) {
    if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
        IRInst *inst = emit_inst(b, IR_MEMCPY);
        inst->src1 = addr_vreg;
        inst->src2 = val_vreg;
        inst->imm = ty->size;
        return;
    }
    if (ty->kind == TY_BOOL) {
        int zero = emit_imm(b, 0);
        int norm = new_vreg(b);
        IRInst *cmp = emit_inst(b, IR_NE);
        cmp->dst = norm;
        cmp->src1 = val_vreg;
        cmp->src2 = zero;
        val_vreg = norm;
    }
    IRInst *inst = emit_inst(b, IR_STORE);
    inst->src1 = addr_vreg;
    inst->src2 = val_vreg;
    inst->mem_size = ty->size;
}

static int gen_inc_dec(IRBuilder *b, const Node *n, bool is_inc, bool is_post) {
    int addr = gen_addr(b, n->lhs);
    int old_val = emit_load_from(b, addr, n->type);
    int step = 1;
    if (is_pointer_like(n->type) && n->type->base && n->type->base->size > 1) {
        step = n->type->base->size;
    }
    int step_vreg = emit_imm(b, step);
    int new_val = new_vreg(b);
    IRInst *op = emit_inst(b, is_inc ? IR_ADD : IR_SUB);
    op->dst = new_val;
    op->src1 = old_val;
    op->src2 = step_vreg;
    emit_store_to(b, addr, new_val, n->type);
    return is_post ? old_val : new_val;
}

static int gen_expr(IRBuilder *b, const Node *n) {
    if (!n) return -1;

    switch (n->kind) {
        case ND_INT_LIT:
            return emit_imm(b, n->int_val);

        case ND_STR_LIT: {
            int dst = new_vreg(b);
            IRInst *inst = emit_inst(b, IR_STR_ADDR);
            inst->dst = dst;
            inst->imm = n->str_id;
            return dst;
        }

        case ND_VAR:
        case ND_MEMBER: {
            int addr = gen_addr(b, n);
            return emit_load_from(b, addr, n->type);
        }

        case ND_DEREF: {
            int addr = gen_expr(b, n->lhs);
            return emit_load_from(b, addr, n->type);
        }

        case ND_ADDR:
            return gen_addr(b, n->lhs);

        case ND_ASSIGN: {
            int addr = gen_addr(b, n->lhs);
            int val = gen_expr(b, n->rhs);
            emit_store_to(b, addr, val, n->type);
            return val;
        }

        case ND_PRE_INC:  return gen_inc_dec(b, n, true, false);
        case ND_PRE_DEC:  return gen_inc_dec(b, n, false, false);
        case ND_POST_INC: return gen_inc_dec(b, n, true, true);
        case ND_POST_DEC: return gen_inc_dec(b, n, false, true);

        case ND_NEG: {
            int src = gen_expr(b, n->lhs);
            int dst = new_vreg(b);
            IRInst *inst = emit_inst(b, IR_NEG);
            inst->dst = dst;
            inst->src1 = src;
            return dst;
        }

        case ND_BITNOT: {
            int src = gen_expr(b, n->lhs);
            int dst = new_vreg(b);
            IRInst *inst = emit_inst(b, IR_BITNOT);
            inst->dst = dst;
            inst->src1 = src;
            return dst;
        }

        case ND_LOGNOT: {
            int src = gen_expr(b, n->lhs);
            int dst = new_vreg(b);
            IRInst *inst = emit_inst(b, IR_LOGNOT);
            inst->dst = dst;
            inst->src1 = src;
            return dst;
        }

        case ND_CAST: {
            int src = gen_expr(b, n->lhs);
            if (n->type->kind == TY_VOID) return src;
            if (n->type->kind == TY_BOOL) {
                int zero = emit_imm(b, 0);
                int dst = new_vreg(b);
                IRInst *inst = emit_inst(b, IR_NE);
                inst->dst = dst;
                inst->src1 = src;
                inst->src2 = zero;
                return dst;
            }
            int dst = new_vreg(b);
            IRInst *inst = emit_inst(b, IR_CAST);
            inst->dst = dst;
            inst->src1 = src;
            inst->mem_size = n->type->size;
            inst->is_unsigned = n->type->is_unsigned;
            return dst;
        }

        case ND_COMMA:
            gen_expr(b, n->lhs);
            return gen_expr(b, n->rhs);

        case ND_LOGAND: {
            int false_lbl = new_label(b);
            int end_lbl = new_label(b);
            int dst = new_vreg(b);

            int l = gen_expr(b, n->lhs);
            emit_jz(b, l, false_lbl);

            int r = gen_expr(b, n->rhs);
            emit_jz(b, r, false_lbl);

            IRInst *t_inst = emit_inst(b, IR_IMM);
            t_inst->dst = dst;
            t_inst->imm = 1;
            emit_jmp(b, end_lbl);

            emit_label(b, false_lbl);
            IRInst *f_inst = emit_inst(b, IR_IMM);
            f_inst->dst = dst;
            f_inst->imm = 0;

            emit_label(b, end_lbl);
            return dst;
        }

        case ND_LOGOR: {
            int true_lbl = new_label(b);
            int end_lbl = new_label(b);
            int dst = new_vreg(b);

            int l = gen_expr(b, n->lhs);
            emit_jnz(b, l, true_lbl);

            int r = gen_expr(b, n->rhs);
            emit_jnz(b, r, true_lbl);

            IRInst *f_inst = emit_inst(b, IR_IMM);
            f_inst->dst = dst;
            f_inst->imm = 0;
            emit_jmp(b, end_lbl);

            emit_label(b, true_lbl);
            IRInst *t_inst = emit_inst(b, IR_IMM);
            t_inst->dst = dst;
            t_inst->imm = 1;

            emit_label(b, end_lbl);
            return dst;
        }

        case ND_TERNARY: {
            int else_lbl = new_label(b);
            int end_lbl = new_label(b);
            int dst = new_vreg(b);

            int c = gen_expr(b, n->cond);
            emit_jz(b, c, else_lbl);

            int t = gen_expr(b, n->then_branch);
            IRInst *m1 = emit_inst(b, IR_MOV);
            m1->dst = dst;
            m1->src1 = t;
            emit_jmp(b, end_lbl);

            emit_label(b, else_lbl);
            int e = gen_expr(b, n->else_branch);
            IRInst *m2 = emit_inst(b, IR_MOV);
            m2->dst = dst;
            m2->src1 = e;

            emit_label(b, end_lbl);
            return dst;
        }

        case ND_FUNCALL: {
            int argc = n->arg_count;
            int *arg_vregs = NULL;
            if (argc > 0) {
                arg_vregs = (int *)malloc((size_t)argc * sizeof(int));
                int idx = 0;
                for (const Node *arg = n->args; arg; arg = arg->next) {
                    arg_vregs[idx++] = gen_expr(b, arg);
                }
                for (int i = 0; i < argc; i++) {
                    IRInst *ainst = emit_inst(b, IR_ARG);
                    ainst->src1 = arg_vregs[i];
                    ainst->imm = i;
                }
                free(arg_vregs);
            }
            int dst = new_vreg(b);
            IRInst *call = emit_inst(b, IR_CALL);
            call->dst = dst;
            call->sym_name = n->func_name;
            call->call_arg_count = argc;
            if (n->func_type && n->func_type->is_variadic) {
                call->call_is_variadic = true;
                call->call_fixed_count = n->func_type->param_count;
            } else {
                call->call_is_variadic = false;
                call->call_fixed_count = argc;
            }
            return dst;
        }

        case ND_ADD:
        case ND_SUB:
        case ND_MUL:
        case ND_DIV:
        case ND_MOD:
        case ND_BITAND:
        case ND_BITOR:
        case ND_BITXOR:
        case ND_SHL:
        case ND_SHR:
        case ND_EQ:
        case ND_NE:
        case ND_LT:
        case ND_LE:
        case ND_GT:
        case ND_GE: {
            int l = gen_expr(b, n->lhs);
            int r = gen_expr(b, n->rhs);
            int dst = new_vreg(b);
            IROp op = IR_NOP;
            switch (n->kind) {
                case ND_ADD:    op = IR_ADD; break;
                case ND_SUB:    op = IR_SUB; break;
                case ND_MUL:    op = IR_MUL; break;
                case ND_DIV:    op = IR_DIV; break;
                case ND_MOD:    op = IR_MOD; break;
                case ND_BITAND: op = IR_BITAND; break;
                case ND_BITOR:  op = IR_BITOR; break;
                case ND_BITXOR: op = IR_BITXOR; break;
                case ND_SHL:    op = IR_SHL; break;
                case ND_SHR:    op = IR_SHR; break;
                case ND_EQ:     op = IR_EQ; break;
                case ND_NE:     op = IR_NE; break;
                case ND_LT:     op = IR_LT; break;
                case ND_LE:     op = IR_LE; break;
                case ND_GT:     op = IR_GT; break;
                case ND_GE:     op = IR_GE; break;
                default: break;
            }
            IRInst *inst = emit_inst(b, op);
            inst->dst = dst;
            inst->src1 = l;
            inst->src2 = r;
            Type *ct = common_type(n->lhs->type, n->rhs->type);
            inst->mem_size = ct->size < 4 ? 4 : ct->size;
            inst->is_unsigned = ct->is_unsigned;
            return dst;
        }

        default:
            error_at(n->loc, "unsupported expression kind in IR generator (%d)", n->kind);
            return -1;
    }
}

static void gen_stmt(IRBuilder *b, const Node *n) {
    if (!n) return;

    switch (n->kind) {
        case ND_EMPTY:
            return;

        case ND_BLOCK:
            for (const Node *s = n->body; s; s = s->next) {
                gen_stmt(b, s);
            }
            return;

        case ND_EXPR_STMT:
            gen_expr(b, n->lhs);
            return;

        case ND_RETURN: {
            int val = -1;
            if (n->lhs) {
                val = gen_expr(b, n->lhs);
            }
            IRInst *inst = emit_inst(b, IR_RET);
            inst->src1 = val;
            return;
        }

        case ND_IF: {
            int c = gen_expr(b, n->cond);
            if (n->else_branch) {
                int else_lbl = new_label(b);
                int end_lbl = new_label(b);
                emit_jz(b, c, else_lbl);
                gen_stmt(b, n->then_branch);
                emit_jmp(b, end_lbl);
                emit_label(b, else_lbl);
                gen_stmt(b, n->else_branch);
                emit_label(b, end_lbl);
            } else {
                int end_lbl = new_label(b);
                emit_jz(b, c, end_lbl);
                gen_stmt(b, n->then_branch);
                emit_label(b, end_lbl);
            }
            return;
        }

        case ND_WHILE: {
            int begin_lbl = new_label(b);
            int end_lbl = new_label(b);
            int prev_brk = b->break_label;
            int prev_cnt = b->continue_label;
            b->break_label = end_lbl;
            b->continue_label = begin_lbl;

            emit_label(b, begin_lbl);
            int c = gen_expr(b, n->cond);
            emit_jz(b, c, end_lbl);
            gen_stmt(b, n->then_branch);
            emit_jmp(b, begin_lbl);
            emit_label(b, end_lbl);

            b->break_label = prev_brk;
            b->continue_label = prev_cnt;
            return;
        }

        case ND_DO_WHILE: {
            int begin_lbl = new_label(b);
            int cond_lbl = new_label(b);
            int end_lbl = new_label(b);
            int prev_brk = b->break_label;
            int prev_cnt = b->continue_label;
            b->break_label = end_lbl;
            b->continue_label = cond_lbl;

            emit_label(b, begin_lbl);
            gen_stmt(b, n->then_branch);
            emit_label(b, cond_lbl);
            int c = gen_expr(b, n->cond);
            emit_jnz(b, c, begin_lbl);
            emit_label(b, end_lbl);

            b->break_label = prev_brk;
            b->continue_label = prev_cnt;
            return;
        }

        case ND_FOR: {
            int begin_lbl = new_label(b);
            int step_lbl = new_label(b);
            int end_lbl = new_label(b);
            int prev_brk = b->break_label;
            int prev_cnt = b->continue_label;
            b->break_label = end_lbl;
            b->continue_label = step_lbl;

            if (n->init) gen_stmt(b, n->init);
            emit_label(b, begin_lbl);
            if (n->cond) {
                int c = gen_expr(b, n->cond);
                emit_jz(b, c, end_lbl);
            }
            gen_stmt(b, n->then_branch);
            emit_label(b, step_lbl);
            if (n->step) gen_expr(b, n->step);
            emit_jmp(b, begin_lbl);
            emit_label(b, end_lbl);

            b->break_label = prev_brk;
            b->continue_label = prev_cnt;
            return;
        }

        case ND_SWITCH: {
            int end_lbl = new_label(b);
            int prev_brk = b->break_label;
            b->break_label = end_lbl;

            int cond_vreg = gen_expr(b, n->cond);
            for (Node *cs = n->cases_next; cs; cs = cs->cases_next) {
                cs->case_label_id = new_label(b);
                int val_vreg = emit_imm(b, cs->int_val);
                int eq_vreg = new_vreg(b);
                IRInst *cmp = emit_inst(b, IR_EQ);
                cmp->dst = eq_vreg;
                cmp->src1 = cond_vreg;
                cmp->src2 = val_vreg;
                emit_jnz(b, eq_vreg, cs->case_label_id);
            }
            if (n->default_case) {
                n->default_case->case_label_id = new_label(b);
                emit_jmp(b, n->default_case->case_label_id);
            } else {
                emit_jmp(b, end_lbl);
            }

            gen_stmt(b, n->then_branch);
            emit_label(b, end_lbl);
            b->break_label = prev_brk;
            return;
        }

        case ND_CASE:
            emit_label(b, n->case_label_id);
            gen_stmt(b, n->lhs);
            return;

        case ND_BREAK:
            if (!b->break_label) error_at(n->loc, "'break' statement not in loop or switch");
            emit_jmp(b, b->break_label);
            return;

        case ND_CONTINUE:
            if (!b->continue_label) error_at(n->loc, "'continue' statement not in loop");
            emit_jmp(b, b->continue_label);
            return;

        case ND_GOTO:
            emit_jmp(b, get_named_label(b, n->label_name));
            return;

        case ND_LABEL:
            emit_label(b, get_named_label(b, n->label_name));
            gen_stmt(b, n->lhs);
            return;

        default:
            error_at(n->loc, "unsupported statement kind in IR generator (%d)", n->kind);
    }
}

IRProgram *generate_ir(const ProgramAST *ast) {
    IRProgram *prog = (IRProgram *)calloc(1, sizeof(IRProgram));
    prog->globals = ast->globals;
    prog->strings = ast->strings;

    IRFunc head = {0};
    IRFunc *cur = &head;
    int global_label_counter = 0;

    for (const FunctionDef *fn_ast = ast->functions; fn_ast; fn_ast = fn_ast->next) {
        IRFunc *fn = (IRFunc *)calloc(1, sizeof(IRFunc));
        fn->name = fn_ast->name;
        fn->is_static = fn_ast->is_static;

        /* Allocate local slots */
        fn->local_count = fn_ast->local_count;
        fn->local_cap = fn_ast->local_count > 0 ? fn_ast->local_count : 1;
        fn->locals = (IRLocalSlot *)calloc((size_t)fn->local_cap, sizeof(IRLocalSlot));

        for (VarObj *v = fn_ast->locals; v; v = v->next) {
            int id = v->local_id;
            fn->locals[id].id = id;
            fn->locals[id].size = v->type->size > 0 ? v->type->size : 8;
            fn->locals[id].align = v->type->align > 0 ? v->type->align : 8;
            fn->locals[id].debug_name = v->name;
        }

        /* Record parameter local IDs */
        int pcount = 0;
        for (VarObj *prm = fn_ast->params; prm; prm = prm->next) pcount++;
        fn->param_count = pcount;
        if (pcount > 0) {
            fn->param_local_ids = (int *)calloc((size_t)pcount, sizeof(int));
            fn->param_sizes = (int *)calloc((size_t)pcount, sizeof(int));
            int i = 0;
            for (VarObj *prm = fn_ast->params; prm; prm = prm->next) {
                fn->param_local_ids[i] = prm->local_id;
                fn->param_sizes[i] = prm->type->size;
                i++;
            }
        }

        IRBuilder b;
        memset(&b, 0, sizeof(b));
        b.fn = fn;
        b.next_label = global_label_counter;

        gen_stmt(&b, fn_ast->body);

        /* Ensure function ends with a return instruction */
        if (fn->inst_count == 0 || fn->insts[fn->inst_count - 1].op != IR_RET) {
            int zero = (strcmp(fn->name, "main") == 0) ? emit_imm(&b, 0) : -1;
            IRInst *ret = emit_inst(&b, IR_RET);
            ret->src1 = zero;
        }

        global_label_counter = b.next_label;
        cur = cur->next = fn;
    }

    prog->funcs = head.next;
    return prog;
}

/* ============================================================================
 * IR Pretty-Printer (--emit-ir)
 * ============================================================================ */

void dump_ir(const IRProgram *ir, FILE *out) {
    fprintf(out, "; === XCC Platform-Independent & Custom Hardened IR ===\n\n");

    for (const StringLiteral *s = ir->strings; s; s = s->next) {
        if (s->is_encrypted) {
            fprintf(out, "@.str.%d = obf_encrypted_const [%d x i8] key=0x%llx \"",
                    s->id, s->len + 1, (unsigned long long)s->enc_key);
        } else {
            fprintf(out, "@.str.%d = constant [%d x i8] \"", s->id, s->len + 1);
        }
        for (int i = 0; i < s->len; i++) {
            unsigned char c = (unsigned char)s->data[i];
            if (!s->is_encrypted) {
                if (c == '\n') fprintf(out, "\\n");
                else if (c == '\t') fprintf(out, "\\t");
                else if (c == '"') fprintf(out, "\\\"");
                else if (c == '\\') fprintf(out, "\\\\");
                else if (c >= 32 && c < 127) fprintf(out, "%c", c);
                else fprintf(out, "\\x%02x", c);
            } else {
                fprintf(out, "\\x%02x", c);
            }
        }
        fprintf(out, "\\00\"\n");
    }

    for (const VarObj *g = ir->globals; g; g = g->next) {
        if (g->type->kind == TY_FUNC) continue;
        fprintf(out, "@%s = %sglobal (%d bytes)\n",
                g->name, g->is_extern ? "extern " : "", g->type->size);
    }

    for (const IRFunc *fn = ir->funcs; fn; fn = fn->next) {
        fprintf(out, "\ndefine %s%s%s@%s(%d params, %d locals, %d vregs)",
                fn->is_static ? "internal " : "",
                fn->is_obfuscated ? "[custom-obf-ir] " : "",
                fn->is_virtualized ? "[polymorphic-vm] " : "",
                fn->name,
                fn->param_count, fn->local_count, fn->vreg_count);
        if (fn->obf_seed) {
            fprintf(out, " ; seed=0x%llx", (unsigned long long)fn->obf_seed);
        }
        fprintf(out, " {\n");

        for (int i = 0; i < fn->inst_count; i++) {
            const IRInst *in = &fn->insts[i];
            switch (in->op) {
                case IR_LABEL:
                    fprintf(out, ".L%d:", in->label);
                    if (in->obf_comment) fprintf(out, " ; %s", in->obf_comment);
                    fprintf(out, "\n");
                    break;
                case IR_IMM:
                    fprintf(out, "  %%v%d = imm %lld\n", in->dst, (long long)in->imm);
                    break;
                case IR_OBF_IMM:
                    fprintf(out, "  %%v%d = obf.imm enc=0x%llx, key=0x%llx, rot=0x%llx",
                            in->dst,
                            (unsigned long long)(uint64_t)in->imm,
                            (unsigned long long)in->obf_key,
                            (unsigned long long)in->obf_rot);
                    if (in->obf_comment) fprintf(out, " ; %s", in->obf_comment);
                    fprintf(out, "\n");
                    break;
                case IR_STR_ADDR:
                    fprintf(out, "  %%v%d = str_addr @.str.%lld\n", in->dst, (long long)in->imm);
                    break;
                case IR_LOCAL_ADDR:
                    fprintf(out, "  %%v%d = local_addr slot#%lld (%s)\n",
                            in->dst, (long long)in->imm,
                            fn->locals[in->imm].debug_name ? fn->locals[in->imm].debug_name : "tmp");
                    break;
                case IR_GLOBAL_ADDR:
                    fprintf(out, "  %%v%d = global_addr @%s\n", in->dst, in->sym_name);
                    break;
                case IR_MOV:
                    fprintf(out, "  %%v%d = mov %%v%d\n", in->dst, in->src1);
                    break;
                case IR_LOAD:
                    fprintf(out, "  %%v%d = load.%s%d [%%v%d]\n",
                            in->dst, in->is_unsigned ? "u" : "s", in->mem_size * 8, in->src1);
                    break;
                case IR_STORE:
                    fprintf(out, "  store.%d [%%v%d], %%v%d\n",
                            in->mem_size * 8, in->src1, in->src2);
                    break;
                case IR_MEMCPY:
                    fprintf(out, "  memcpy [%%v%d], [%%v%d], %lld\n",
                            in->src1, in->src2, (long long)in->imm);
                    break;
                case IR_NEG:
                    fprintf(out, "  %%v%d = neg %%v%d\n", in->dst, in->src1);
                    break;
                case IR_BITNOT:
                    fprintf(out, "  %%v%d = bitnot %%v%d\n", in->dst, in->src1);
                    break;
                case IR_LOGNOT:
                    fprintf(out, "  %%v%d = lognot %%v%d\n", in->dst, in->src1);
                    break;
                case IR_CAST:
                    fprintf(out, "  %%v%d = cast.%s%d %%v%d\n",
                            in->dst, in->is_unsigned ? "u" : "s", in->mem_size * 8, in->src1);
                    break;
                case IR_ADD:
                    fprintf(out, "  %%v%d = add %%v%d, %%v%d\n", in->dst, in->src1, in->src2);
                    break;
                case IR_SUB:
                    fprintf(out, "  %%v%d = sub %%v%d, %%v%d\n", in->dst, in->src1, in->src2);
                    break;
                case IR_MUL:
                    fprintf(out, "  %%v%d = mul %%v%d, %%v%d\n", in->dst, in->src1, in->src2);
                    break;
                case IR_DIV:
                    fprintf(out, "  %%v%d = %sdiv %%v%d, %%v%d\n",
                            in->dst, in->is_unsigned ? "u" : "s", in->src1, in->src2);
                    break;
                case IR_MOD:
                    fprintf(out, "  %%v%d = %smod %%v%d, %%v%d\n",
                            in->dst, in->is_unsigned ? "u" : "s", in->src1, in->src2);
                    break;
                case IR_BITAND:
                    fprintf(out, "  %%v%d = and %%v%d, %%v%d\n", in->dst, in->src1, in->src2);
                    break;
                case IR_BITOR:
                    fprintf(out, "  %%v%d = or %%v%d, %%v%d\n", in->dst, in->src1, in->src2);
                    break;
                case IR_BITXOR:
                    fprintf(out, "  %%v%d = xor %%v%d, %%v%d\n", in->dst, in->src1, in->src2);
                    break;
                case IR_OBF_MBA_ADD:
                    fprintf(out, "  %%v%d = obf.mba.add %%v%d, %%v%d, key=0x%llx ; [(x^y) + 2*(x&y)]\n",
                            in->dst, in->src1, in->src2, (unsigned long long)in->obf_key);
                    break;
                case IR_OBF_MBA_SUB:
                    fprintf(out, "  %%v%d = obf.mba.sub %%v%d, %%v%d, key=0x%llx ; [(x&~y) - (~x&y)]\n",
                            in->dst, in->src1, in->src2, (unsigned long long)in->obf_key);
                    break;
                case IR_OBF_MBA_XOR:
                    fprintf(out, "  %%v%d = obf.mba.xor %%v%d, %%v%d, key=0x%llx ; [((x|y)+k) - ((x&y)+k)]\n",
                            in->dst, in->src1, in->src2, (unsigned long long)in->obf_key);
                    break;
                case IR_OBF_OPAQUE_TRUE:
                    fprintf(out, "  %%v%d = obf.opaque.true %%v%d, key=0x%llx ; [((z*(z+1))&1)==0 -> 1]\n",
                            in->dst, in->src1, (unsigned long long)in->obf_key);
                    break;
                case IR_OBF_OPAQUE_FALSE:
                    fprintf(out, "  %%v%d = obf.opaque.false %%v%d, key=0x%llx ; [((z*(z+1))&1)!=0 -> 0]\n",
                            in->dst, in->src1, (unsigned long long)in->obf_key);
                    break;
                case IR_OBF_JUNK:
                    fprintf(out, "  obf.junk %%v%d, entropy=0x%llx\n",
                            in->src1, (unsigned long long)in->obf_key);
                    break;
                case IR_SHL:
                    fprintf(out, "  %%v%d = shl %%v%d, %%v%d\n", in->dst, in->src1, in->src2);
                    break;
                case IR_SHR:
                    fprintf(out, "  %%v%d = %sshr %%v%d, %%v%d\n",
                            in->dst, in->is_unsigned ? "l" : "a", in->src1, in->src2);
                    break;
                case IR_EQ:
                    fprintf(out, "  %%v%d = cmp.eq %%v%d, %%v%d\n", in->dst, in->src1, in->src2);
                    break;
                case IR_NE:
                    fprintf(out, "  %%v%d = cmp.ne %%v%d, %%v%d\n", in->dst, in->src1, in->src2);
                    break;
                case IR_LT:
                    fprintf(out, "  %%v%d = cmp.lt %%v%d, %%v%d\n", in->dst, in->src1, in->src2);
                    break;
                case IR_LE:
                    fprintf(out, "  %%v%d = cmp.le %%v%d, %%v%d\n", in->dst, in->src1, in->src2);
                    break;
                case IR_GT:
                    fprintf(out, "  %%v%d = cmp.gt %%v%d, %%v%d\n", in->dst, in->src1, in->src2);
                    break;
                case IR_GE:
                    fprintf(out, "  %%v%d = cmp.ge %%v%d, %%v%d\n", in->dst, in->src1, in->src2);
                    break;
                case IR_JMP:
                    fprintf(out, "  jmp .L%d", in->label);
                    if (in->obf_comment) fprintf(out, " ; %s", in->obf_comment);
                    fprintf(out, "\n");
                    break;
                case IR_JZ:
                    fprintf(out, "  jz %%v%d, .L%d", in->src1, in->label);
                    if (in->obf_comment) fprintf(out, " ; %s", in->obf_comment);
                    fprintf(out, "\n");
                    break;
                case IR_JNZ:
                    fprintf(out, "  jnz %%v%d, .L%d", in->src1, in->label);
                    if (in->obf_comment) fprintf(out, " ; %s", in->obf_comment);
                    fprintf(out, "\n");
                    break;
                case IR_ARG:
                    fprintf(out, "  arg #%lld = %%v%d\n", (long long)in->imm, in->src1);
                    break;
                case IR_CALL:
                    fprintf(out, "  %%v%d = call @%s(%d args)\n",
                            in->dst, in->sym_name, in->call_arg_count);
                    break;
                case IR_RET:
                    if (in->src1 >= 0) fprintf(out, "  ret %%v%d\n", in->src1);
                    else fprintf(out, "  ret void\n");
                    break;
                default:
                    break;
            }
        }
        fprintf(out, "}\n");
    }
}
