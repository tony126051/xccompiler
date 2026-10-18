#include "xcc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * SplitMix64 High-Entropy Polymorphic PRNG
 * ============================================================================ */

static uint64_t splitmix64_next(uint64_t *state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static uint64_t hash_str_seed(const char *s, uint64_t seed) {
    uint64_t h = seed ^ 0xCBF29CE484222325ULL;
    while (s && *s) {
        h ^= (uint64_t)(unsigned char)(*s++);
        h *= 0x100000001B3ULL;
    }
    return splitmix64_next(&h);
}

/* ============================================================================
 * IR Instruction Stream Builder Helpers
 * ============================================================================ */

typedef struct {
    IRInst *data;
    int count;
    int cap;
} InstVec;

static void vec_push(InstVec *v, IRInst in) {
    if (v->count + 1 > v->cap) {
        v->cap = v->cap ? v->cap * 2 : 64;
        v->data = (IRInst *)realloc(v->data, (size_t)v->cap * sizeof(IRInst));
    }
    v->data[v->count++] = in;
}

static IRInst make_inst(IROp op) {
    IRInst in;
    memset(&in, 0, sizeof(in));
    in.op = op;
    in.dst = -1;
    in.src1 = -1;
    in.src2 = -1;
    in.mem_size = 8;
    return in;
}

static int alloc_vreg(IRFunc *fn) {
    return fn->vreg_count++;
}

static int alloc_local_slot(IRFunc *fn, int size, int align, const char *debug_name) {
    if (fn->local_count + 1 > fn->local_cap) {
        fn->local_cap = fn->local_cap ? fn->local_cap * 2 : 16;
        fn->locals = (IRLocalSlot *)realloc(fn->locals, (size_t)fn->local_cap * sizeof(IRLocalSlot));
    }
    int id = fn->local_count++;
    memset(&fn->locals[id], 0, sizeof(IRLocalSlot));
    fn->locals[id].id = id;
    fn->locals[id].size = size > 0 ? size : 8;
    fn->locals[id].align = align > 0 ? align : 8;
    fn->locals[id].debug_name = debug_name;
    return id;
}

static int find_max_label(const IRProgram *ir) {
    int max_lbl = 100;
    for (const IRFunc *fn = ir->funcs; fn; fn = fn->next) {
        for (int i = 0; i < fn->inst_count; i++) {
            if (fn->insts[i].label > max_lbl) {
                max_lbl = fn->insts[i].label;
            }
        }
    }
    return max_lbl;
}

/* Construct an encrypted IR_OBF_IMM instruction that reconstructs `plain_val` at runtime */
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

static int vec_emit_obf_imm(InstVec *v, IRFunc *fn, int64_t val, uint64_t *rng, const char *comment) {
    int dst = alloc_vreg(fn);
    vec_push(v, make_obf_imm_inst(dst, val, rng, comment));
    return dst;
}

static int vec_emit_imm_auto(InstVec *v, IRFunc *fn, int64_t val, bool use_obf, uint64_t *rng, const char *comment) {
    if (use_obf) {
        return vec_emit_obf_imm(v, fn, val, rng, comment);
    }
    int dst = alloc_vreg(fn);
    IRInst in = make_inst(IR_IMM);
    in.dst = dst;
    in.imm = val;
    in.obf_comment = comment;
    vec_push(v, in);
    return dst;
}

static int vec_emit_binop(InstVec *v, IRFunc *fn, IROp op, int s1, int s2, uint64_t key) {
    int dst = alloc_vreg(fn);
    IRInst in = make_inst(op);
    in.dst = dst;
    in.src1 = s1;
    in.src2 = s2;
    in.obf_key = key;
    vec_push(v, in);
    return dst;
}

static void vec_emit_label(InstVec *v, int label_id, const char *comment) {
    IRInst in = make_inst(IR_LABEL);
    in.label = label_id;
    in.obf_comment = comment;
    vec_push(v, in);
}

static void vec_emit_jmp(InstVec *v, int label_id, const char *comment) {
    IRInst in = make_inst(IR_JMP);
    in.label = label_id;
    in.obf_comment = comment;
    vec_push(v, in);
}

static void vec_emit_jz(InstVec *v, int cond_vreg, int label_id, const char *comment) {
    IRInst in = make_inst(IR_JZ);
    in.src1 = cond_vreg;
    in.label = label_id;
    in.obf_comment = comment;
    vec_push(v, in);
}

static void vec_emit_jnz(InstVec *v, int cond_vreg, int label_id, const char *comment) {
    IRInst in = make_inst(IR_JNZ);
    in.src1 = cond_vreg;
    in.label = label_id;
    in.obf_comment = comment;
    vec_push(v, in);
}

static int vec_emit_load(InstVec *v, IRFunc *fn, int addr_vreg, int mem_size, bool is_unsigned) {
    int dst = alloc_vreg(fn);
    IRInst in = make_inst(IR_LOAD);
    in.dst = dst;
    in.src1 = addr_vreg;
    in.mem_size = mem_size;
    in.is_unsigned = is_unsigned;
    vec_push(v, in);
    return dst;
}

static void vec_emit_store(InstVec *v, int addr_vreg, int val_vreg, int mem_size) {
    IRInst in = make_inst(IR_STORE);
    in.src1 = addr_vreg;
    in.src2 = val_vreg;
    in.mem_size = mem_size;
    vec_push(v, in);
}

/* ============================================================================
 * Pass 1: Compile-Time String Literal Encryption & Runtime Decryption
 * ============================================================================ */

static const StringLiteral *find_string_by_id(const IRProgram *ir, int id) {
    for (const StringLiteral *s = ir->strings; s; s = s->next) {
        if (s->id == id) return s;
    }
    return NULL;
}

static uint8_t str_stream_key_byte(uint64_t enc_key, int index) {
    uint64_t mix = (enc_key >> ((index & 7) * 8)) + (uint64_t)index * 0x9E3779B9ULL;
    return (uint8_t)((mix ^ 0xA5ULL) & 0xFFULL);
}

static void emit_decrypt_string_to_buf(InstVec *v, IRFunc *fn, const StringLiteral *s,
                                       const char *buf_sym, bool use_mba, uint64_t *rng) {
    int v_src_base = alloc_vreg(fn);
    IRInst sa = make_inst(IR_STR_ADDR);
    sa.dst = v_src_base;
    sa.imm = s->id;
    vec_push(v, sa);

    int v_dst_base = alloc_vreg(fn);
    IRInst ga = make_inst(IR_GLOBAL_ADDR);
    ga.dst = v_dst_base;
    ga.sym_name = buf_sym;
    vec_push(v, ga);

    IROp add_op = use_mba ? IR_OBF_MBA_ADD : IR_ADD;
    IROp xor_op = use_mba ? IR_OBF_MBA_XOR : IR_BITXOR;

    for (int i = 0; i < s->len; i++) {
        int v_off = vec_emit_imm_auto(v, fn, i, use_mba, rng, NULL);
        int v_sp  = vec_emit_binop(v, fn, add_op, v_src_base, v_off, splitmix64_next(rng));
        int v_dp  = vec_emit_binop(v, fn, add_op, v_dst_base, v_off, splitmix64_next(rng));
        int v_enc_ch = vec_emit_load(v, fn, v_sp, 1, true);
        uint8_t kb = str_stream_key_byte(s->enc_key, i);
        int v_kb = vec_emit_imm_auto(v, fn, (int64_t)kb, use_mba, rng, NULL);
        int v_dec_ch = vec_emit_binop(v, fn, xor_op, v_enc_ch, v_kb, splitmix64_next(rng));
        vec_emit_store(v, v_dp, v_dec_ch, 1);
    }
    /* Null terminator */
    int v_end_off = vec_emit_imm_auto(v, fn, s->len, use_mba, rng, NULL);
    int v_end_dp  = vec_emit_binop(v, fn, add_op, v_dst_base, v_end_off, splitmix64_next(rng));
    int v_zero    = vec_emit_imm_auto(v, fn, 0, use_mba, rng, NULL);
    vec_emit_store(v, v_end_dp, v_zero, 1);
}

static void pass_encrypt_strings(IRProgram *ir, bool use_mba, uint64_t *rng) {
    if (!ir->strings) return;

    /* 1. Encrypt all string literals in-place and allocate .bss decryption buffers */
    for (StringLiteral *s = ir->strings; s; s = s->next) {
        if (s->is_encrypted) continue;
        s->is_encrypted = true;
        s->enc_key = splitmix64_next(rng) | 1ULL;

        for (int i = 0; i < s->len; i++) {
            uint8_t kb = str_stream_key_byte(s->enc_key, i);
            s->data[i] = (char)(((uint8_t)s->data[i]) ^ kb);
        }

        char buf_name[64];
        snprintf(buf_name, sizeof(buf_name), "__xcc_str_dec_%d", s->id);
        char *sym = strdup(buf_name);

        VarObj *g = (VarObj *)calloc(1, sizeof(VarObj));
        g->name = sym;
        int aligned_len = (s->len + 8) & ~7;
        g->type = array_of(ty_char, aligned_len);
        g->is_local = false;
        g->is_static = false;
        g->has_init = false;
        g->next = ir->globals;
        ir->globals = g;

        /* Update any global pointer initializers referencing .L.str.<id> */
        char orig_lbl[64];
        snprintf(orig_lbl, sizeof(orig_lbl), ".L.str.%d", s->id);
        for (VarObj *gv = ir->globals; gv; gv = gv->next) {
            for (GlobalInit *gi = gv->init_list; gi; gi = gi->next) {
                if (gi->label && strcmp(gi->label, orig_lbl) == 0) {
                    gi->label = strdup(sym);
                }
            }
        }
    }

    /* 2. Rewrite IR_STR_ADDR in all functions to decrypt on-demand into @__xcc_str_dec_<id> */
    for (IRFunc *fn = ir->funcs; fn; fn = fn->next) {
        InstVec vec = {0};

        /* In `main`, also pre-decrypt all strings at prologue so global string pointers are ready */
        if (strcmp(fn->name, "main") == 0) {
            for (const StringLiteral *s = ir->strings; s; s = s->next) {
                char buf_name[64];
                snprintf(buf_name, sizeof(buf_name), "__xcc_str_dec_%d", s->id);
                emit_decrypt_string_to_buf(&vec, fn, s, strdup(buf_name), use_mba, rng);
            }
        }

        for (int i = 0; i < fn->inst_count; i++) {
            IRInst in = fn->insts[i];
            if (in.op == IR_STR_ADDR) {
                const StringLiteral *s = find_string_by_id(ir, (int)in.imm);
                if (s && s->is_encrypted) {
                    char buf_name[64];
                    snprintf(buf_name, sizeof(buf_name), "__xcc_str_dec_%d", s->id);
                    const char *sym = strdup(buf_name);
                    emit_decrypt_string_to_buf(&vec, fn, s, sym, use_mba, rng);

                    IRInst ga = make_inst(IR_GLOBAL_ADDR);
                    ga.dst = in.dst;
                    ga.sym_name = sym;
                    vec_push(&vec, ga);
                    continue;
                }
            }
            vec_push(&vec, in);
        }

        free(fn->insts);
        fn->insts = vec.data;
        fn->inst_count = vec.count;
        fn->inst_cap = vec.cap;
    }
}

/* ============================================================================
 * Pass 2: Mixed Boolean-Arithmetic (MBA) & Dynamic Constant Encryption
 * ============================================================================ */

static void pass_mba_and_const_obf(IRFunc *fn, uint64_t *rng) {
    InstVec vec = {0};

    for (int i = 0; i < fn->inst_count; i++) {
        IRInst in = fn->insts[i];
        switch (in.op) {
            case IR_IMM: {
                char comment_buf[64];
                snprintf(comment_buf, sizeof(comment_buf), "const %lld", (long long)in.imm);
                vec_push(&vec, make_obf_imm_inst(in.dst, in.imm, rng, strdup(comment_buf)));
                break;
            }
            case IR_ADD:
                in.op = IR_OBF_MBA_ADD;
                in.obf_key = splitmix64_next(rng);
                vec_push(&vec, in);
                break;
            case IR_SUB:
                in.op = IR_OBF_MBA_SUB;
                in.obf_key = splitmix64_next(rng);
                vec_push(&vec, in);
                break;
            case IR_BITXOR:
                in.op = IR_OBF_MBA_XOR;
                in.obf_key = splitmix64_next(rng);
                vec_push(&vec, in);
                break;
            default:
                vec_push(&vec, in);
                break;
        }
    }

    free(fn->insts);
    fn->insts = vec.data;
    fn->inst_count = vec.count;
    fn->inst_cap = vec.cap;
    fn->is_obfuscated = true;
}

/* ============================================================================
 * Pass 3: Bogus Control Flow (BCF) + Number-Theoretic Opaque Predicates
 *         & Control-Flow Flattening (CFF) with Branchless State Transitions
 * ============================================================================ */

typedef struct {
    int start_idx;
    int end_idx;     /* Exclusive */
    int orig_label;  /* >0 if block began with IR_LABEL, else -1 */
    int exec_label;  /* Internal label for CFF dispatcher jump */
    int bogus_label; /* Label for Bogus Control Flow decoy block */
    uint32_t state_id;
} CFFBlock;

static int find_cff_block_by_label(const CFFBlock *blocks, int count, int orig_label) {
    for (int i = 0; i < count; i++) {
        if (blocks[i].orig_label == orig_label) {
            return i;
        }
    }
    return -1;
}

static void pass_bcf_only(IRFunc *fn, uint64_t *rng, int *max_label) {
    InstVec vec = {0};
    int decoy_slot = alloc_local_slot(fn, 8, 8, "__bcf_decoy");

    for (int i = 0; i < fn->inst_count; i++) {
        IRInst in = fn->insts[i];

        /* Do not inject between IR_ARG and IR_CALL */
        bool in_arg_seq = (in.op == IR_ARG || in.op == IR_CALL);
        if (!in_arg_seq && (i == 0 || in.op == IR_LABEL)) {
            vec_push(&vec, in);

            int skip_lbl = ++(*max_label);
            int bogus_lbl = ++(*max_label);

            int v_opq = alloc_vreg(fn);
            IRInst opq = make_inst(IR_OBF_OPAQUE_FALSE);
            opq.dst = v_opq;
            opq.src1 = -1;
            opq.obf_key = splitmix64_next(rng);
            vec_push(&vec, opq);

            vec_emit_jnz(&vec, v_opq, bogus_lbl, "opaque-false bogus branch");
            vec_emit_jmp(&vec, skip_lbl, "real path");

            /* Bogus decoy block */
            vec_emit_label(&vec, bogus_lbl, "bogus decoy block");
            int v_addr = alloc_vreg(fn);
            IRInst la = make_inst(IR_LOCAL_ADDR);
            la.dst = v_addr;
            la.imm = decoy_slot;
            vec_push(&vec, la);

            int v_d1 = vec_emit_obf_imm(&vec, fn, (int64_t)(splitmix64_next(rng) & 0xFFFF), rng, "decoy");
            int v_d2 = vec_emit_obf_imm(&vec, fn, (int64_t)(splitmix64_next(rng) & 0xFFFF), rng, "decoy");
            int v_dm = vec_emit_binop(&vec, fn, IR_OBF_MBA_XOR, v_d1, v_d2, splitmix64_next(rng));
            vec_emit_store(&vec, v_addr, v_dm, 8);

            vec_emit_label(&vec, skip_lbl, "resume real block");
            continue;
        }

        vec_push(&vec, in);
    }

    free(fn->insts);
    fn->insts = vec.data;
    fn->inst_count = vec.count;
    fn->inst_cap = vec.cap;
    fn->is_obfuscated = true;
}

static void pass_cff_and_bcf(IRFunc *fn, bool enable_bcf, bool enable_mba, uint64_t *rng, int *max_label) {
    if (fn->inst_count <= 1) return;

    /* 1. Identify basic block boundaries */
    int cap = 32;
    int bb_count = 0;
    CFFBlock *bbs = (CFFBlock *)calloc((size_t)cap, sizeof(CFFBlock));

    int cur_start = 0;
    for (int i = 0; i < fn->inst_count; i++) {
        IROp op = fn->insts[i].op;
        if (op == IR_LABEL && i > cur_start) {
            if (bb_count + 1 > cap) {
                cap *= 2;
                bbs = (CFFBlock *)realloc(bbs, (size_t)cap * sizeof(CFFBlock));
            }
            bbs[bb_count].start_idx = cur_start;
            bbs[bb_count].end_idx = i;
            bb_count++;
            cur_start = i;
        }
        if (op == IR_JMP || op == IR_JZ || op == IR_JNZ || op == IR_RET) {
            if (bb_count + 1 > cap) {
                cap *= 2;
                bbs = (CFFBlock *)realloc(bbs, (size_t)cap * sizeof(CFFBlock));
            }
            bbs[bb_count].start_idx = cur_start;
            bbs[bb_count].end_idx = i + 1;
            bb_count++;
            cur_start = i + 1;
        }
    }
    if (cur_start < fn->inst_count) {
        if (bb_count + 1 > cap) {
            cap *= 2;
            bbs = (CFFBlock *)realloc(bbs, (size_t)cap * sizeof(CFFBlock));
        }
        bbs[bb_count].start_idx = cur_start;
        bbs[bb_count].end_idx = fn->inst_count;
        bb_count++;
    }

    if (bb_count <= 1 && !enable_bcf) {
        free(bbs);
        return;
    }

    /* 2. Assign unique high-entropy 32-bit state IDs and execution labels to each block */
    for (int k = 0; k < bb_count; k++) {
        int s = bbs[k].start_idx;
        if (s < bbs[k].end_idx && fn->insts[s].op == IR_LABEL) {
            bbs[k].orig_label = fn->insts[s].label;
        } else {
            bbs[k].orig_label = -1;
        }
        bbs[k].exec_label = ++(*max_label);
        bbs[k].bogus_label = ++(*max_label);

        uint32_t st = 0;
        for (;;) {
            st = (uint32_t)((splitmix64_next(rng) & 0x7FFFFFFFULL) | 0x10000001ULL);
            bool dup = false;
            for (int j = 0; j < k; j++) {
                if (bbs[j].state_id == st) { dup = true; break; }
            }
            if (!dup) break;
        }
        bbs[k].state_id = st;
    }

    /* 3. Allocate stack state variable slot and decoy slot */
    int state_slot = alloc_local_slot(fn, 8, 8, "__cff_state");
    int decoy_slot = alloc_local_slot(fn, 8, 8, "__bcf_decoy");
    int dispatch_label = ++(*max_label);
    uint32_t dispatch_mask = (uint32_t)((splitmix64_next(rng) & 0x7FFFFFFFULL) | 0x05050505ULL);

    InstVec out = {0};

    /* 4. Emit Function Entry: Initialize encrypted state to bbs[0].state_id and jump to Dispatcher */
    int v_state_ptr = alloc_vreg(fn);
    IRInst la_state = make_inst(IR_LOCAL_ADDR);
    la_state.dst = v_state_ptr;
    la_state.imm = state_slot;
    vec_push(&out, la_state);

    char st_comment[64];
    snprintf(st_comment, sizeof(st_comment), "init CFF state=0x%08x (BB#0)", bbs[0].state_id);
    int v_init_st = vec_emit_obf_imm(&out, fn, (int64_t)bbs[0].state_id, rng, strdup(st_comment));
    vec_emit_store(&out, v_state_ptr, v_init_st, 8);
    vec_emit_jmp(&out, dispatch_label, "enter CFF master dispatcher");

    /* 5. Emit Encrypted Master State Dispatcher (.L_dispatch) */
    vec_emit_label(&out, dispatch_label, "CFF Master State-Machine Dispatcher");
    int v_cur_st = vec_emit_load(&out, fn, v_state_ptr, 8, false);
    int v_dmask  = vec_emit_obf_imm(&out, fn, (int64_t)dispatch_mask, rng, "CFF dispatch key mask");
    int v_masked = vec_emit_binop(&out, fn, IR_OBF_MBA_XOR, v_cur_st, v_dmask, splitmix64_next(rng));

    /* Permute dispatcher check order & block emission order with Fisher-Yates shuffle */
    int *perm_disp = (int *)calloc((size_t)bb_count, sizeof(int));
    int *perm_emit = (int *)calloc((size_t)bb_count, sizeof(int));
    for (int k = 0; k < bb_count; k++) {
        perm_disp[k] = k;
        perm_emit[k] = k;
    }
    for (int k = bb_count - 1; k > 0; k--) {
        int j1 = (int)(splitmix64_next(rng) % (uint64_t)(k + 1));
        int tmp1 = perm_disp[k]; perm_disp[k] = perm_disp[j1]; perm_disp[j1] = tmp1;

        int j2 = (int)(splitmix64_next(rng) % (uint64_t)(k + 1));
        int tmp2 = perm_emit[k]; perm_emit[k] = perm_emit[j2]; perm_emit[j2] = tmp2;
    }

    for (int m = 0; m < bb_count; m++) {
        int k = perm_disp[m];
        uint32_t expected_enc = bbs[k].state_id ^ dispatch_mask;
        char cbuf[64];
        snprintf(cbuf, sizeof(cbuf), "dispatch BB#%d (state^mask=0x%08x)", k, expected_enc);
        int v_exp = vec_emit_obf_imm(&out, fn, (int64_t)expected_enc, rng, strdup(cbuf));
        int v_eq  = vec_emit_binop(&out, fn, IR_EQ, v_masked, v_exp, 0);
        vec_emit_jnz(&out, v_eq, bbs[k].exec_label, "branch to flattened BB");
    }
    /* Unreachable default loop back to dispatcher */
    vec_emit_jmp(&out, dispatch_label, "dispatcher trap loop");

    /* 6. Emit Flattened Basic Blocks in Scrambled Physical Order + Bogus Decoy Blocks */
    for (int m = 0; m < bb_count; m++) {
        int k = perm_emit[m];
        CFFBlock *b = &bbs[k];

        char bb_hdr[64];
        snprintf(bb_hdr, sizeof(bb_hdr), "Flattened BB#%d (state=0x%08x)", k, b->state_id);
        vec_emit_label(&out, b->exec_label, strdup(bb_hdr));

        /* Optional Bogus Control Flow guard via Number-Theoretic Opaque Predicate */
        if (enable_bcf) {
            int v_opq = alloc_vreg(fn);
            IRInst opq = make_inst(IR_OBF_OPAQUE_FALSE);
            opq.dst = v_opq;
            opq.src1 = v_cur_st;
            opq.obf_key = splitmix64_next(rng);
            vec_push(&out, opq);
            vec_emit_jnz(&out, v_opq, b->bogus_label, "opaque-false -> bogus block");
        }

        int i_start = b->start_idx;
        if (i_start < b->end_idx && fn->insts[i_start].op == IR_LABEL) {
            i_start++; /* Strip original label; handled by CFF state */
        }

        int i_end = b->end_idx;
        IRInst term = make_inst(IR_NOP);
        bool has_term = false;
        if (i_end > i_start) {
            IROp last_op = fn->insts[i_end - 1].op;
            if (last_op == IR_JMP || last_op == IR_JZ || last_op == IR_JNZ || last_op == IR_RET) {
                term = fn->insts[i_end - 1];
                has_term = true;
                i_end--;
            }
        }

        /* Emit core instructions of basic block k */
        for (int idx = i_start; idx < i_end; idx++) {
            vec_push(&out, fn->insts[idx]);
        }

        /* Emit anti-decompiler junk entropy instruction at block tail */
        if (enable_mba) {
            IRInst junk = make_inst(IR_OBF_JUNK);
            junk.src1 = v_cur_st;
            junk.obf_key = splitmix64_next(rng);
            vec_push(&out, junk);
        }

        /* Emit State Transition at end of basic block k */
        if (has_term && term.op == IR_RET) {
            vec_push(&out, term);
        } else if (has_term && term.op == IR_JMP) {
            int target_bb = find_cff_block_by_label(bbs, bb_count, term.label);
            uint32_t next_st = (target_bb >= 0) ? bbs[target_bb].state_id : bbs[0].state_id;
            int v_nst = vec_emit_obf_imm(&out, fn, (int64_t)next_st, rng, "next state (unconditional)");
            vec_emit_store(&out, v_state_ptr, v_nst, 8);
            vec_emit_jmp(&out, dispatch_label, "return to CFF dispatcher");
        } else if (has_term && (term.op == IR_JZ || term.op == IR_JNZ)) {
            int taken_bb = find_cff_block_by_label(bbs, bb_count, term.label);
            int fall_bb  = (k + 1 < bb_count) ? (k + 1) : 0;
            uint32_t st_taken = (taken_bb >= 0) ? bbs[taken_bb].state_id : bbs[0].state_id;
            uint32_t st_fall  = bbs[fall_bb].state_id;

            /* Branchless Bitwise/MBA State Selection:
             * Eradicates conditional branches from the basic block! */
            int v_is_zero = vec_emit_binop(&out, fn, IR_LOGNOT, term.src1, -1, 0);
            int v_take_bit = v_is_zero;
            if (term.op == IR_JNZ) {
                v_take_bit = vec_emit_binop(&out, fn, IR_LOGNOT, v_is_zero, -1, 0);
            }
            /* v_mask = -v_take_bit (all 1s if taken, all 0s if fallthrough) */
            int v_mask  = vec_emit_binop(&out, fn, IR_NEG, v_take_bit, -1, 0);
            int v_nmask = vec_emit_binop(&out, fn, IR_BITNOT, v_mask, -1, 0);

            int v_st_t  = vec_emit_obf_imm(&out, fn, (int64_t)st_taken, rng, "branchless state (taken)");
            int v_st_f  = vec_emit_obf_imm(&out, fn, (int64_t)st_fall, rng, "branchless state (fallthrough)");

            int v_part1 = vec_emit_binop(&out, fn, IR_BITAND, v_st_t, v_mask, 0);
            int v_part2 = vec_emit_binop(&out, fn, IR_BITAND, v_st_f, v_nmask, 0);
            int v_next  = vec_emit_binop(&out, fn, IR_OBF_MBA_ADD, v_part1, v_part2, splitmix64_next(rng));

            vec_emit_store(&out, v_state_ptr, v_next, 8);
            vec_emit_jmp(&out, dispatch_label, "branchless transition to CFF dispatcher");
        } else {
            /* Fallthrough to block k + 1 */
            int fall_bb = (k + 1 < bb_count) ? (k + 1) : 0;
            uint32_t next_st = bbs[fall_bb].state_id;
            int v_nst = vec_emit_obf_imm(&out, fn, (int64_t)next_st, rng, "next state (fallthrough)");
            vec_emit_store(&out, v_state_ptr, v_nst, 8);
            vec_emit_jmp(&out, dispatch_label, "fallthrough to CFF dispatcher");
        }

        /* Emit interleaved Bogus Control Flow Decoy Block (.L_bogus_k) */
        if (enable_bcf) {
            vec_emit_label(&out, b->bogus_label, "Bogus Control Flow Ghost Block");
            int v_decoy_ptr = alloc_vreg(fn);
            IRInst la_dec = make_inst(IR_LOCAL_ADDR);
            la_dec.dst = v_decoy_ptr;
            la_dec.imm = decoy_slot;
            vec_push(&out, la_dec);

            uint32_t fake_target_st = bbs[(k + 1) % bb_count].state_id;
            int v_fk1 = vec_emit_obf_imm(&out, fn, (int64_t)fake_target_st, rng, "bogus state");
            int v_fk2 = vec_emit_binop(&out, fn, IR_OBF_MBA_XOR, v_cur_st, v_fk1, splitmix64_next(rng));
            vec_emit_store(&out, v_decoy_ptr, v_fk2, 8);
            vec_emit_store(&out, v_state_ptr, v_fk1, 8);
            vec_emit_jmp(&out, dispatch_label, "bogus jump to CFF dispatcher");
        }
    }

    free(perm_disp);
    free(perm_emit);
    free(bbs);

    free(fn->insts);
    fn->insts = out.data;
    fn->inst_count = out.count;
    fn->inst_cap = out.cap;
    fn->is_obfuscated = true;
}

/* ============================================================================
 * Pass 4: Polymorphic Self-Checksumming Bytecode VM Virtualizer (--obf-vm)
 * ============================================================================ */

typedef enum {
    VOP_NOP = 0,
    VOP_IMM,
    VOP_MOV,
    VOP_LOAD8_S,
    VOP_LOAD8_U,
    VOP_LOAD16_S,
    VOP_LOAD16_U,
    VOP_LOAD32_S,
    VOP_LOAD32_U,
    VOP_LOAD64,
    VOP_STORE8,
    VOP_STORE16,
    VOP_STORE32,
    VOP_STORE64,
    VOP_NEG,
    VOP_BITNOT,
    VOP_LOGNOT,
    VOP_CAST8_S,
    VOP_CAST8_U,
    VOP_CAST16_S,
    VOP_CAST16_U,
    VOP_CAST32_S,
    VOP_CAST32_U,
    VOP_ADD,
    VOP_SUB,
    VOP_MUL,
    VOP_DIV32_S,
    VOP_DIV32_U,
    VOP_DIV64_S,
    VOP_DIV64_U,
    VOP_MOD32_S,
    VOP_MOD32_U,
    VOP_MOD64_S,
    VOP_MOD64_U,
    VOP_AND,
    VOP_OR,
    VOP_XOR,
    VOP_SHL32,
    VOP_SHL64,
    VOP_SHR32_S,
    VOP_SHR32_U,
    VOP_SHR64_S,
    VOP_SHR64_U,
    VOP_EQ32,
    VOP_EQ64,
    VOP_NE32,
    VOP_NE64,
    VOP_LT32_S,
    VOP_LT32_U,
    VOP_LT64_S,
    VOP_LT64_U,
    VOP_LE32_S,
    VOP_LE32_U,
    VOP_LE64_S,
    VOP_LE64_U,
    VOP_GT32_S,
    VOP_GT32_U,
    VOP_GT64_S,
    VOP_GT64_U,
    VOP_GE32_S,
    VOP_GE32_U,
    VOP_GE64_S,
    VOP_GE64_U,
    VOP_JMP,
    VOP_JZ,
    VOP_JNZ,
    VOP_RET,
    VOP_BRIDGE,
    VOP_COUNT
} VMOpcode;

typedef struct {
    VMOpcode vop;
    uint16_t dst;
    uint16_t src1;
    uint16_t src2;
    int64_t imm;
} VMBCInst;

typedef struct {
    IRInst inst;
    int arg_vregs[64];
} VMBridgeSite;

static VMOpcode map_ir_to_vop(const IRInst *in) {
    bool m4 = (in->mem_size == 4);
    bool u  = in->is_unsigned;
    switch (in->op) {
        case IR_IMM:
        case IR_OBF_IMM: return VOP_IMM;
        case IR_MOV:     return VOP_MOV;
        case IR_LOAD:
            if (in->mem_size == 1) return u ? VOP_LOAD8_U : VOP_LOAD8_S;
            if (in->mem_size == 2) return u ? VOP_LOAD16_U : VOP_LOAD16_S;
            if (in->mem_size == 4) return u ? VOP_LOAD32_U : VOP_LOAD32_S;
            return VOP_LOAD64;
        case IR_STORE:
            if (in->mem_size == 1) return VOP_STORE8;
            if (in->mem_size == 2) return VOP_STORE16;
            if (in->mem_size == 4) return VOP_STORE32;
            return VOP_STORE64;
        case IR_NEG:     return VOP_NEG;
        case IR_BITNOT:  return VOP_BITNOT;
        case IR_LOGNOT:  return VOP_LOGNOT;
        case IR_CAST:
            if (in->mem_size == 1) return u ? VOP_CAST8_U : VOP_CAST8_S;
            if (in->mem_size == 2) return u ? VOP_CAST16_U : VOP_CAST16_S;
            if (in->mem_size == 4) return u ? VOP_CAST32_U : VOP_CAST32_S;
            return VOP_MOV;
        case IR_ADD:
        case IR_OBF_MBA_ADD: return VOP_ADD;
        case IR_SUB:
        case IR_OBF_MBA_SUB: return VOP_SUB;
        case IR_MUL:         return VOP_MUL;
        case IR_DIV:
            if (m4) return u ? VOP_DIV32_U : VOP_DIV32_S;
            return u ? VOP_DIV64_U : VOP_DIV64_S;
        case IR_MOD:
            if (m4) return u ? VOP_MOD32_U : VOP_MOD32_S;
            return u ? VOP_MOD64_U : VOP_MOD64_S;
        case IR_BITAND:      return VOP_AND;
        case IR_BITOR:       return VOP_OR;
        case IR_BITXOR:
        case IR_OBF_MBA_XOR: return VOP_XOR;
        case IR_SHL:         return m4 ? VOP_SHL32 : VOP_SHL64;
        case IR_SHR:
            if (m4) return u ? VOP_SHR32_U : VOP_SHR32_S;
            return u ? VOP_SHR64_U : VOP_SHR64_S;
        case IR_EQ:          return m4 ? VOP_EQ32 : VOP_EQ64;
        case IR_NE:          return m4 ? VOP_NE32 : VOP_NE64;
        case IR_LT:
            if (m4) return u ? VOP_LT32_U : VOP_LT32_S;
            return u ? VOP_LT64_U : VOP_LT64_S;
        case IR_LE:
            if (m4) return u ? VOP_LE32_U : VOP_LE32_S;
            return u ? VOP_LE64_U : VOP_LE64_S;
        case IR_GT:
            if (m4) return u ? VOP_GT32_U : VOP_GT32_S;
            return u ? VOP_GT64_U : VOP_GT64_S;
        case IR_GE:
            if (m4) return u ? VOP_GE32_U : VOP_GE32_S;
            return u ? VOP_GE64_U : VOP_GE64_S;
        case IR_JMP:         return VOP_JMP;
        case IR_JZ:          return VOP_JZ;
        case IR_JNZ:         return VOP_JNZ;
        case IR_RET:         return VOP_RET;
        default:             return VOP_BRIDGE;
    }
}

static void pass_virtualize_func(IRProgram *ir, IRFunc *fn, int func_idx, uint64_t *rng, int *max_label) {
    if (fn->inst_count <= 0) return;

    int orig_vreg_count = fn->vreg_count > 0 ? fn->vreg_count : 1;
    int vm_regs_slot = alloc_local_slot(fn, orig_vreg_count * 8, 8, "__pvm_vregs");

    /* 1. Generate per-function Polymorphic Opcode Permutation Table */
    uint16_t op_perm[VOP_COUNT];
    uint16_t salt = (uint16_t)((splitmix64_next(rng) & 0x7FF0ULL) | 0x0100ULL);
    for (int i = 0; i < VOP_COUNT; i++) {
        op_perm[i] = (uint16_t)(salt + i * 7 + (int)(splitmix64_next(rng) & 3ULL));
    }
    for (int i = VOP_COUNT - 1; i > 0; i--) {
        int j = (int)(splitmix64_next(rng) % (uint64_t)(i + 1));
        uint16_t t = op_perm[i]; op_perm[i] = op_perm[j]; op_perm[j] = t;
    }

    /* 2. Pass 1 over fn->insts: map IR_LABEL -> bytecode PC */
    int max_ir_lbl = *max_label + 16;
    int *lbl_to_bcpc = (int *)calloc((size_t)max_ir_lbl, sizeof(int));
    int bc_cap = fn->inst_count + 8;
    VMBCInst *bc = (VMBCInst *)calloc((size_t)bc_cap, sizeof(VMBCInst));
    int bc_count = 0;

    for (int i = 0; i < fn->inst_count; i++) {
        const IRInst *in = &fn->insts[i];
        if (in->op == IR_LABEL) {
            if (in->label >= 0 && in->label < max_ir_lbl) {
                lbl_to_bcpc[in->label] = bc_count;
            }
            continue;
        }
        if (in->op == IR_NOP || in->op == IR_OBF_JUNK || in->op == IR_ARG) {
            continue;
        }
        bc_count++;
    }

    /* 3. Pass 2 over fn->insts: emit VMBCInsts & collect native bridge sites */
    VMBridgeSite *bridges = (VMBridgeSite *)calloc((size_t)(fn->inst_count + 4), sizeof(VMBridgeSite));
    int bridge_count = 0;
    bool used_vops[VOP_COUNT];
    memset(used_vops, 0, sizeof(used_vops));

    int pending_args[64];
    memset(pending_args, 0, sizeof(pending_args));
    int pc = 0;

    for (int i = 0; i < fn->inst_count; i++) {
        const IRInst *in = &fn->insts[i];
        if (in->op == IR_LABEL || in->op == IR_NOP || in->op == IR_OBF_JUNK) {
            continue;
        }
        if (in->op == IR_ARG) {
            if (in->imm >= 0 && in->imm < 64) {
                pending_args[in->imm] = in->src1;
            }
            continue;
        }

        VMBCInst binst;
        memset(&binst, 0, sizeof(binst));
        binst.dst  = (uint16_t)(in->dst >= 0 ? in->dst : 0);
        binst.src1 = (uint16_t)(in->src1 >= 0 ? in->src1 : 0);
        binst.src2 = (uint16_t)(in->src2 >= 0 ? in->src2 : 0);

        if (in->op == IR_IMM) {
            binst.vop = VOP_IMM;
            binst.imm = in->imm;
        } else if (in->op == IR_OBF_IMM) {
            uint64_t enc = (uint64_t)in->imm;
            uint64_t k = in->obf_key;
            uint64_t r = in->obf_rot;
            binst.vop = VOP_IMM;
            binst.imm = (int64_t)(((enc ^ (k >> 17)) - k) ^ r);
        } else if (in->op == IR_OBF_OPAQUE_TRUE) {
            binst.vop = VOP_IMM;
            binst.imm = 1;
        } else if (in->op == IR_OBF_OPAQUE_FALSE) {
            binst.vop = VOP_IMM;
            binst.imm = 0;
        } else if (in->op == IR_JMP || in->op == IR_JZ || in->op == IR_JNZ) {
            binst.vop = map_ir_to_vop(in);
            binst.imm = (in->label >= 0 && in->label < max_ir_lbl) ? lbl_to_bcpc[in->label] : 0;
        } else if (in->op == IR_RET) {
            binst.vop = VOP_RET;
            binst.imm = (in->src1 >= 0) ? 1 : 0;
        } else if (in->op == IR_LOCAL_ADDR || in->op == IR_GLOBAL_ADDR ||
                   in->op == IR_STR_ADDR || in->op == IR_MEMCPY || in->op == IR_CALL) {
            int bid = bridge_count++;
            bridges[bid].inst = *in;
            if (in->op == IR_CALL) {
                for (int a = 0; a < in->call_arg_count && a < 64; a++) {
                    bridges[bid].arg_vregs[a] = pending_args[a];
                }
            }
            binst.vop = VOP_BRIDGE;
            binst.imm = bid;
        } else {
            binst.vop = map_ir_to_vop(in);
            binst.imm = in->imm;
        }

        used_vops[binst.vop] = true;
        bc[pc++] = binst;
    }

    /* 4. Encrypt Bytecode Image with SplitMix64 Rolling Key & Compute Anti-Tamper Tag */
    uint64_t key0_base = splitmix64_next(rng);
    uint64_t key1_base = splitmix64_next(rng);
    uint64_t step0     = splitmix64_next(rng) | 1ULL;
    uint64_t step1     = splitmix64_next(rng) | 1ULL;
    uint64_t integrity_tag = 0;

    GlobalInit head_gi = {0};
    GlobalInit *cur_gi = &head_gi;

    for (int idx = 0; idx < bc_count; idx++) {
        uint64_t raw0 = ((uint64_t)op_perm[bc[idx].vop] & 0xFFFFULL) |
                        (((uint64_t)bc[idx].dst & 0xFFFFULL) << 16) |
                        (((uint64_t)bc[idx].src1 & 0xFFFFULL) << 32) |
                        (((uint64_t)bc[idx].src2 & 0xFFFFULL) << 48);
        uint64_t raw1 = (uint64_t)bc[idx].imm;

        uint64_t k0 = key0_base ^ ((uint64_t)idx * step0);
        uint64_t k1 = key1_base ^ ((uint64_t)idx * step1);
        uint64_t enc0 = raw0 ^ k0;
        uint64_t enc1 = raw1 ^ k1;

        integrity_tag ^= enc0 ^ enc1;

        GlobalInit *gi0 = (GlobalInit *)calloc(1, sizeof(GlobalInit));
        gi0->offset = (idx * 2) * 8;
        gi0->size = 8;
        gi0->val = (int64_t)enc0;
        cur_gi = cur_gi->next = gi0;

        GlobalInit *gi1 = (GlobalInit *)calloc(1, sizeof(GlobalInit));
        gi1->offset = (idx * 2 + 1) * 8;
        gi1->size = 8;
        gi1->val = (int64_t)enc1;
        cur_gi = cur_gi->next = gi1;
    }

    char bc_sym_buf[128];
    snprintf(bc_sym_buf, sizeof(bc_sym_buf), "__xcc_vm_bc_%d_%s", func_idx, fn->name);
    char *bc_sym = strdup(bc_sym_buf);

    VarObj *bc_glob = (VarObj *)calloc(1, sizeof(VarObj));
    bc_glob->name = bc_sym;
    bc_glob->type = array_of(ty_ulong, bc_count * 2);
    bc_glob->is_local = false;
    bc_glob->is_static = false;
    bc_glob->has_init = true;
    bc_glob->init_list = head_gi.next;
    bc_glob->next = ir->globals;
    ir->globals = bc_glob;

    /* 5. Synthesize Self-Checksumming Polymorphic VM Interpreter in fn->insts */
    fn->vreg_count = 0; /* Reset native vregs for the VM interpreter itself */
    InstVec out = {0};

    int v_reg_base = alloc_vreg(fn);
    IRInst la_regs = make_inst(IR_LOCAL_ADDR);
    la_regs.dst = v_reg_base;
    la_regs.imm = vm_regs_slot;
    vec_push(&out, la_regs);

    int v_bc_base = alloc_vreg(fn);
    IRInst ga_bc = make_inst(IR_GLOBAL_ADDR);
    ga_bc.dst = v_bc_base;
    ga_bc.sym_name = bc_sym;
    vec_push(&out, ga_bc);

    /* Self-Checksumming Anti-Tamper Loop: derive key0_base from bytecode XOR-tag */
    int lbl_cs_loop = ++(*max_label);
    int lbl_cs_done = ++(*max_label);
    int v_tag = vec_emit_obf_imm(&out, fn, 0, rng, "init bytecode anti-tamper tag");
    int v_idx = vec_emit_obf_imm(&out, fn, 0, rng, "init word index");
    int v_tot = vec_emit_obf_imm(&out, fn, bc_count * 2, rng, "bytecode word count");
    int v_c3  = vec_emit_obf_imm(&out, fn, 3, rng, NULL);
    int v_c1  = vec_emit_obf_imm(&out, fn, 1, rng, NULL);

    vec_emit_label(&out, lbl_cs_loop, "VM Anti-Tamper Self-Checksumming Loop");
    int v_cs_cond = vec_emit_binop(&out, fn, IR_LT, v_idx, v_tot, 0);
    vec_emit_jz(&out, v_cs_cond, lbl_cs_done, "checksum complete");

    int v_cs_off = vec_emit_binop(&out, fn, IR_SHL, v_idx, v_c3, 0);
    int v_cs_ptr = vec_emit_binop(&out, fn, IR_OBF_MBA_ADD, v_bc_base, v_cs_off, splitmix64_next(rng));
    int v_cs_w   = vec_emit_load(&out, fn, v_cs_ptr, 8, true);
    int v_tag_n  = vec_emit_binop(&out, fn, IR_OBF_MBA_XOR, v_tag, v_cs_w, splitmix64_next(rng));
    IRInst mov_tag = make_inst(IR_MOV); mov_tag.dst = v_tag; mov_tag.src1 = v_tag_n; vec_push(&out, mov_tag);

    int v_idx_n  = vec_emit_binop(&out, fn, IR_OBF_MBA_ADD, v_idx, v_c1, splitmix64_next(rng));
    IRInst mov_idx = make_inst(IR_MOV); mov_idx.dst = v_idx; mov_idx.src1 = v_idx_n; vec_push(&out, mov_idx);
    vec_emit_jmp(&out, lbl_cs_loop, NULL);

    vec_emit_label(&out, lbl_cs_done, "Derive Rolling Decryption Key from Integrity Tag");
    uint64_t masked_key0_base = key0_base ^ integrity_tag;
    int v_mk0     = vec_emit_obf_imm(&out, fn, (int64_t)masked_key0_base, rng, "masked master key0");
    int v_k0_base = vec_emit_binop(&out, fn, IR_OBF_MBA_XOR, v_mk0, v_tag, splitmix64_next(rng));
    int v_k1_base = vec_emit_obf_imm(&out, fn, (int64_t)key1_base, rng, "master key1");
    int v_step0   = vec_emit_obf_imm(&out, fn, (int64_t)step0, rng, "rolling step0");
    int v_step1   = vec_emit_obf_imm(&out, fn, (int64_t)step1, rng, "rolling step1");
    int v_vpc     = vec_emit_obf_imm(&out, fn, 0, rng, "init Virtual PC (vpc = 0)");

    int v_c4    = vec_emit_obf_imm(&out, fn, 4, rng, NULL);
    int v_c8    = vec_emit_obf_imm(&out, fn, 8, rng, NULL);
    int v_c16   = vec_emit_obf_imm(&out, fn, 16, rng, NULL);
    int v_c32   = vec_emit_obf_imm(&out, fn, 32, rng, NULL);
    int v_c48   = vec_emit_obf_imm(&out, fn, 48, rng, NULL);
    int v_mffff = vec_emit_obf_imm(&out, fn, 0xFFFF, rng, NULL);

    /* Main VM Fetch-Decode-Execute Loop */
    int lbl_vm_loop = ++(*max_label);
    vec_emit_label(&out, lbl_vm_loop, "Polymorphic VM Fetch-Decode-Execute Loop");

    int v_boff   = vec_emit_binop(&out, fn, IR_SHL, v_vpc, v_c4, 0);
    int v_w0_ptr = vec_emit_binop(&out, fn, IR_OBF_MBA_ADD, v_bc_base, v_boff, splitmix64_next(rng));
    int v_enc0   = vec_emit_load(&out, fn, v_w0_ptr, 8, true);
    int v_w1_ptr = vec_emit_binop(&out, fn, IR_OBF_MBA_ADD, v_w0_ptr, v_c8, splitmix64_next(rng));
    int v_enc1   = vec_emit_load(&out, fn, v_w1_ptr, 8, true);

    /* Compute per-PC rolling keys k0 and k1 */
    int v_m0   = vec_emit_binop(&out, fn, IR_MUL, v_vpc, v_step0, 0);
    int v_rk0  = vec_emit_binop(&out, fn, IR_OBF_MBA_XOR, v_k0_base, v_m0, splitmix64_next(rng));
    int v_m1   = vec_emit_binop(&out, fn, IR_MUL, v_vpc, v_step1, 0);
    int v_rk1  = vec_emit_binop(&out, fn, IR_OBF_MBA_XOR, v_k1_base, v_m1, splitmix64_next(rng));

    int v_raw0 = vec_emit_binop(&out, fn, IR_OBF_MBA_XOR, v_enc0, v_rk0, splitmix64_next(rng));
    int v_imm  = vec_emit_binop(&out, fn, IR_OBF_MBA_XOR, v_enc1, v_rk1, splitmix64_next(rng));

    /* vpc++ */
    int v_vpc_next = vec_emit_binop(&out, fn, IR_OBF_MBA_ADD, v_vpc, v_c1, splitmix64_next(rng));
    IRInst mov_vpc = make_inst(IR_MOV); mov_vpc.dst = v_vpc; mov_vpc.src1 = v_vpc_next; vec_push(&out, mov_vpc);

    /* Unpack op, dst, src1, src2 */
    int v_op   = vec_emit_binop(&out, fn, IR_BITAND, v_raw0, v_mffff, 0);
    int v_sh16 = vec_emit_binop(&out, fn, IR_SHR, v_raw0, v_c16, 0);
    out.data[out.count - 1].is_unsigned = true;
    int v_dst  = vec_emit_binop(&out, fn, IR_BITAND, v_sh16, v_mffff, 0);

    int v_sh32 = vec_emit_binop(&out, fn, IR_SHR, v_raw0, v_c32, 0);
    out.data[out.count - 1].is_unsigned = true;
    int v_src1 = vec_emit_binop(&out, fn, IR_BITAND, v_sh32, v_mffff, 0);

    int v_sh48 = vec_emit_binop(&out, fn, IR_SHR, v_raw0, v_c48, 0);
    out.data[out.count - 1].is_unsigned = true;
    int v_src2 = vec_emit_binop(&out, fn, IR_BITAND, v_sh48, v_mffff, 0);

    /* Compute virtual register slot pointers and pre-load v_val1, v_val2 */
    int v_doff     = vec_emit_binop(&out, fn, IR_SHL, v_dst, v_c3, 0);
    int v_dst_ptr  = vec_emit_binop(&out, fn, IR_OBF_MBA_ADD, v_reg_base, v_doff, splitmix64_next(rng));
    int v_s1off    = vec_emit_binop(&out, fn, IR_SHL, v_src1, v_c3, 0);
    int v_src1_ptr = vec_emit_binop(&out, fn, IR_OBF_MBA_ADD, v_reg_base, v_s1off, splitmix64_next(rng));
    int v_s2off    = vec_emit_binop(&out, fn, IR_SHL, v_src2, v_c3, 0);
    int v_src2_ptr = vec_emit_binop(&out, fn, IR_OBF_MBA_ADD, v_reg_base, v_s2off, splitmix64_next(rng));

    int v_val1 = vec_emit_load(&out, fn, v_src1_ptr, 8, false);
    int v_val2 = vec_emit_load(&out, fn, v_src2_ptr, 8, false);

    /* Collect and shuffle active VM opcodes for dispatch */
    int active_vops[VOP_COUNT];
    int active_lbls[VOP_COUNT];
    int active_count = 0;
    for (int vop = 0; vop < VOP_COUNT; vop++) {
        if (used_vops[vop]) {
            active_vops[active_count] = vop;
            active_lbls[active_count] = ++(*max_label);
            active_count++;
        }
    }
    for (int i = active_count - 1; i > 0; i--) {
        int j = (int)(splitmix64_next(rng) % (uint64_t)(i + 1));
        int tv = active_vops[i]; active_vops[i] = active_vops[j]; active_vops[j] = tv;
        int tl = active_lbls[i]; active_lbls[i] = active_lbls[j]; active_lbls[j] = tl;
    }

    for (int idx = 0; idx < active_count; idx++) {
        int vop = active_vops[idx];
        char cbuf[64];
        snprintf(cbuf, sizeof(cbuf), "match custom VOP=0x%04x", op_perm[vop]);
        int v_code = vec_emit_obf_imm(&out, fn, (int64_t)op_perm[vop], rng, strdup(cbuf));
        int v_match = vec_emit_binop(&out, fn, IR_EQ, v_op, v_code, 0);
        vec_emit_jnz(&out, v_match, active_lbls[idx], NULL);
    }
    vec_emit_jmp(&out, lbl_vm_loop, NULL);

    /* Emit VM Opcode Handlers */
    for (int idx = 0; idx < active_count; idx++) {
        VMOpcode vop = (VMOpcode)active_vops[idx];
        vec_emit_label(&out, active_lbls[idx], "VM Opcode Handler");

        switch (vop) {
            case VOP_IMM:
                vec_emit_store(&out, v_dst_ptr, v_imm, 8);
                break;
            case VOP_MOV:
                vec_emit_store(&out, v_dst_ptr, v_val1, 8);
                break;
            case VOP_LOAD8_S:
            case VOP_LOAD8_U:
            case VOP_LOAD16_S:
            case VOP_LOAD16_U:
            case VOP_LOAD32_S:
            case VOP_LOAD32_U:
            case VOP_LOAD64: {
                int msz = 8;
                bool uns = false;
                if (vop == VOP_LOAD8_S)  { msz = 1; uns = false; }
                if (vop == VOP_LOAD8_U)  { msz = 1; uns = true; }
                if (vop == VOP_LOAD16_S) { msz = 2; uns = false; }
                if (vop == VOP_LOAD16_U) { msz = 2; uns = true; }
                if (vop == VOP_LOAD32_S) { msz = 4; uns = false; }
                if (vop == VOP_LOAD32_U) { msz = 4; uns = true; }
                int v_loaded = vec_emit_load(&out, fn, v_val1, msz, uns);
                vec_emit_store(&out, v_dst_ptr, v_loaded, 8);
                break;
            }
            case VOP_STORE8:
                vec_emit_store(&out, v_val1, v_val2, 1);
                break;
            case VOP_STORE16:
                vec_emit_store(&out, v_val1, v_val2, 2);
                break;
            case VOP_STORE32:
                vec_emit_store(&out, v_val1, v_val2, 4);
                break;
            case VOP_STORE64:
                vec_emit_store(&out, v_val1, v_val2, 8);
                break;
            case VOP_NEG: {
                int vr = vec_emit_binop(&out, fn, IR_NEG, v_val1, -1, 0);
                vec_emit_store(&out, v_dst_ptr, vr, 8);
                break;
            }
            case VOP_BITNOT: {
                int vr = vec_emit_binop(&out, fn, IR_BITNOT, v_val1, -1, 0);
                vec_emit_store(&out, v_dst_ptr, vr, 8);
                break;
            }
            case VOP_LOGNOT: {
                int vr = vec_emit_binop(&out, fn, IR_LOGNOT, v_val1, -1, 0);
                vec_emit_store(&out, v_dst_ptr, vr, 8);
                break;
            }
            case VOP_CAST8_S:
            case VOP_CAST8_U:
            case VOP_CAST16_S:
            case VOP_CAST16_U:
            case VOP_CAST32_S:
            case VOP_CAST32_U: {
                int vr = alloc_vreg(fn);
                IRInst ci = make_inst(IR_CAST);
                ci.dst = vr;
                ci.src1 = v_val1;
                if (vop == VOP_CAST8_S || vop == VOP_CAST8_U) ci.mem_size = 1;
                else if (vop == VOP_CAST16_S || vop == VOP_CAST16_U) ci.mem_size = 2;
                else ci.mem_size = 4;
                ci.is_unsigned = (vop == VOP_CAST8_U || vop == VOP_CAST16_U || vop == VOP_CAST32_U);
                vec_push(&out, ci);
                vec_emit_store(&out, v_dst_ptr, vr, 8);
                break;
            }
            case VOP_ADD: {
                int vr = vec_emit_binop(&out, fn, IR_OBF_MBA_ADD, v_val1, v_val2, splitmix64_next(rng));
                vec_emit_store(&out, v_dst_ptr, vr, 8);
                break;
            }
            case VOP_SUB: {
                int vr = vec_emit_binop(&out, fn, IR_OBF_MBA_SUB, v_val1, v_val2, splitmix64_next(rng));
                vec_emit_store(&out, v_dst_ptr, vr, 8);
                break;
            }
            case VOP_MUL: {
                int vr = vec_emit_binop(&out, fn, IR_MUL, v_val1, v_val2, 0);
                vec_emit_store(&out, v_dst_ptr, vr, 8);
                break;
            }
            case VOP_DIV32_S:
            case VOP_DIV32_U:
            case VOP_DIV64_S:
            case VOP_DIV64_U:
            case VOP_MOD32_S:
            case VOP_MOD32_U:
            case VOP_MOD64_S:
            case VOP_MOD64_U: {
                bool is_mod = (vop >= VOP_MOD32_S);
                int vr = alloc_vreg(fn);
                IRInst di = make_inst(is_mod ? IR_MOD : IR_DIV);
                di.dst = vr;
                di.src1 = v_val1;
                di.src2 = v_val2;
                di.mem_size = (vop == VOP_DIV32_S || vop == VOP_DIV32_U || vop == VOP_MOD32_S || vop == VOP_MOD32_U) ? 4 : 8;
                di.is_unsigned = (vop == VOP_DIV32_U || vop == VOP_DIV64_U || vop == VOP_MOD32_U || vop == VOP_MOD64_U);
                vec_push(&out, di);
                vec_emit_store(&out, v_dst_ptr, vr, 8);
                break;
            }
            case VOP_AND: {
                int vr = vec_emit_binop(&out, fn, IR_BITAND, v_val1, v_val2, 0);
                vec_emit_store(&out, v_dst_ptr, vr, 8);
                break;
            }
            case VOP_OR: {
                int vr = vec_emit_binop(&out, fn, IR_BITOR, v_val1, v_val2, 0);
                vec_emit_store(&out, v_dst_ptr, vr, 8);
                break;
            }
            case VOP_XOR: {
                int vr = vec_emit_binop(&out, fn, IR_OBF_MBA_XOR, v_val1, v_val2, splitmix64_next(rng));
                vec_emit_store(&out, v_dst_ptr, vr, 8);
                break;
            }
            case VOP_SHL32:
            case VOP_SHL64:
            case VOP_SHR32_S:
            case VOP_SHR32_U:
            case VOP_SHR64_S:
            case VOP_SHR64_U: {
                int vr = alloc_vreg(fn);
                IRInst si = make_inst((vop == VOP_SHL32 || vop == VOP_SHL64) ? IR_SHL : IR_SHR);
                si.dst = vr;
                si.src1 = v_val1;
                si.src2 = v_val2;
                si.mem_size = (vop == VOP_SHL32 || vop == VOP_SHR32_S || vop == VOP_SHR32_U) ? 4 : 8;
                si.is_unsigned = (vop == VOP_SHR32_U || vop == VOP_SHR64_U);
                vec_push(&out, si);
                vec_emit_store(&out, v_dst_ptr, vr, 8);
                break;
            }
            case VOP_EQ32: case VOP_EQ64:
            case VOP_NE32: case VOP_NE64:
            case VOP_LT32_S: case VOP_LT32_U: case VOP_LT64_S: case VOP_LT64_U:
            case VOP_LE32_S: case VOP_LE32_U: case VOP_LE64_S: case VOP_LE64_U:
            case VOP_GT32_S: case VOP_GT32_U: case VOP_GT64_S: case VOP_GT64_U:
            case VOP_GE32_S: case VOP_GE32_U: case VOP_GE64_S: case VOP_GE64_U: {
                IROp cop = IR_EQ;
                int msz = 8;
                bool uns = false;
                switch (vop) {
                    case VOP_EQ32:   cop = IR_EQ; msz = 4; break;
                    case VOP_EQ64:   cop = IR_EQ; msz = 8; break;
                    case VOP_NE32:   cop = IR_NE; msz = 4; break;
                    case VOP_NE64:   cop = IR_NE; msz = 8; break;
                    case VOP_LT32_S: cop = IR_LT; msz = 4; uns = false; break;
                    case VOP_LT32_U: cop = IR_LT; msz = 4; uns = true;  break;
                    case VOP_LT64_S: cop = IR_LT; msz = 8; uns = false; break;
                    case VOP_LT64_U: cop = IR_LT; msz = 8; uns = true;  break;
                    case VOP_LE32_S: cop = IR_LE; msz = 4; uns = false; break;
                    case VOP_LE32_U: cop = IR_LE; msz = 4; uns = true;  break;
                    case VOP_LE64_S: cop = IR_LE; msz = 8; uns = false; break;
                    case VOP_LE64_U: cop = IR_LE; msz = 8; uns = true;  break;
                    case VOP_GT32_S: cop = IR_GT; msz = 4; uns = false; break;
                    case VOP_GT32_U: cop = IR_GT; msz = 4; uns = true;  break;
                    case VOP_GT64_S: cop = IR_GT; msz = 8; uns = false; break;
                    case VOP_GT64_U: cop = IR_GT; msz = 8; uns = true;  break;
                    case VOP_GE32_S: cop = IR_GE; msz = 4; uns = false; break;
                    case VOP_GE32_U: cop = IR_GE; msz = 4; uns = true;  break;
                    case VOP_GE64_S: cop = IR_GE; msz = 8; uns = false; break;
                    case VOP_GE64_U: cop = IR_GE; msz = 8; uns = true;  break;
                    default: break;
                }
                int vr = alloc_vreg(fn);
                IRInst ci = make_inst(cop);
                ci.dst = vr;
                ci.src1 = v_val1;
                ci.src2 = v_val2;
                ci.mem_size = msz;
                ci.is_unsigned = uns;
                vec_push(&out, ci);
                vec_emit_store(&out, v_dst_ptr, vr, 8);
                break;
            }
            case VOP_JMP: {
                IRInst mv = make_inst(IR_MOV);
                mv.dst = v_vpc;
                mv.src1 = v_imm;
                vec_push(&out, mv);
                break;
            }
            case VOP_JZ:
            case VOP_JNZ: {
                /* Branchless Virtual PC selection inside VM handler */
                int v_iz = vec_emit_binop(&out, fn, IR_LOGNOT, v_val1, -1, 0);
                int v_tb = (vop == VOP_JZ) ? v_iz : vec_emit_binop(&out, fn, IR_LOGNOT, v_iz, -1, 0);
                int v_mk = vec_emit_binop(&out, fn, IR_NEG, v_tb, -1, 0);
                int v_nk = vec_emit_binop(&out, fn, IR_BITNOT, v_mk, -1, 0);
                int v_p1 = vec_emit_binop(&out, fn, IR_BITAND, v_imm, v_mk, 0);
                int v_p2 = vec_emit_binop(&out, fn, IR_BITAND, v_vpc, v_nk, 0);
                int v_nv = vec_emit_binop(&out, fn, IR_OBF_MBA_ADD, v_p1, v_p2, splitmix64_next(rng));
                IRInst mv = make_inst(IR_MOV);
                mv.dst = v_vpc;
                mv.src1 = v_nv;
                vec_push(&out, mv);
                break;
            }
            case VOP_RET: {
                IRInst rt = make_inst(IR_RET);
                rt.src1 = v_val1;
                vec_push(&out, rt);
                break;
            }
            case VOP_BRIDGE: {
                int lbl_bridge_done = ++(*max_label);
                int *site_lbls = (int *)calloc((size_t)(bridge_count + 1), sizeof(int));
                for (int b = 0; b < bridge_count; b++) {
                    site_lbls[b] = ++(*max_label);
                    int v_bid = vec_emit_obf_imm(&out, fn, b, rng, NULL);
                    int v_beq = vec_emit_binop(&out, fn, IR_EQ, v_imm, v_bid, 0);
                    vec_emit_jnz(&out, v_beq, site_lbls[b], NULL);
                }
                vec_emit_jmp(&out, lbl_bridge_done, NULL);

                for (int b = 0; b < bridge_count; b++) {
                    vec_emit_label(&out, site_lbls[b], "VM Native Bridge Site");
                    IRInst orig = bridges[b].inst;
                    if (orig.op == IR_LOCAL_ADDR || orig.op == IR_GLOBAL_ADDR || orig.op == IR_STR_ADDR) {
                        int vr = alloc_vreg(fn);
                        orig.dst = vr;
                        vec_push(&out, orig);
                        vec_emit_store(&out, v_dst_ptr, vr, 8);
                    } else if (orig.op == IR_MEMCPY) {
                        int v_off1 = vec_emit_obf_imm(&out, fn, orig.src1 * 8, rng, NULL);
                        int v_p1   = vec_emit_binop(&out, fn, IR_OBF_MBA_ADD, v_reg_base, v_off1, splitmix64_next(rng));
                        int v_a1   = vec_emit_load(&out, fn, v_p1, 8, false);

                        int v_off2 = vec_emit_obf_imm(&out, fn, orig.src2 * 8, rng, NULL);
                        int v_p2   = vec_emit_binop(&out, fn, IR_OBF_MBA_ADD, v_reg_base, v_off2, splitmix64_next(rng));
                        int v_a2   = vec_emit_load(&out, fn, v_p2, 8, false);

                        IRInst mc = orig;
                        mc.src1 = v_a1;
                        mc.src2 = v_a2;
                        vec_push(&out, mc);
                    } else if (orig.op == IR_CALL) {
                        int arg_tmps[64];
                        for (int a = 0; a < orig.call_arg_count && a < 64; a++) {
                            int v_ao = vec_emit_obf_imm(&out, fn, bridges[b].arg_vregs[a] * 8, rng, NULL);
                            int v_ap = vec_emit_binop(&out, fn, IR_OBF_MBA_ADD, v_reg_base, v_ao, splitmix64_next(rng));
                            arg_tmps[a] = vec_emit_load(&out, fn, v_ap, 8, false);
                        }
                        for (int a = 0; a < orig.call_arg_count && a < 64; a++) {
                            IRInst ai = make_inst(IR_ARG);
                            ai.imm = a;
                            ai.src1 = arg_tmps[a];
                            vec_push(&out, ai);
                        }
                        int v_ret = (orig.dst >= 0) ? alloc_vreg(fn) : -1;
                        IRInst ci = orig;
                        ci.dst = v_ret;
                        vec_push(&out, ci);
                        if (v_ret >= 0) {
                            vec_emit_store(&out, v_dst_ptr, v_ret, 8);
                        }
                    }
                    vec_emit_jmp(&out, lbl_bridge_done, NULL);
                }
                free(site_lbls);
                vec_emit_label(&out, lbl_bridge_done, NULL);
                break;
            }
            default:
                break;
        }

        if (vop != VOP_RET) {
            vec_emit_jmp(&out, lbl_vm_loop, "continue VM loop");
        }
    }

    /* Fallback return at function end */
    int v_zero_ret = vec_emit_obf_imm(&out, fn, 0, rng, NULL);
    IRInst ret_end = make_inst(IR_RET);
    ret_end.src1 = v_zero_ret;
    vec_push(&out, ret_end);

    free(lbl_to_bcpc);
    free(bc);
    free(bridges);

    free(fn->insts);
    fn->insts = out.data;
    fn->inst_count = out.count;
    fn->inst_cap = out.cap;
    fn->is_virtualized = true;
    fn->is_obfuscated = true;
}

/* ============================================================================
 * Top-Level Custom IR Obfuscation & Virtualization Driver
 * ============================================================================ */

void obfuscate_ir(IRProgram *ir, const ObfuscationConfig *cfg) {
    if (!ir || !cfg) return;
    if (!cfg->enable_cff && !cfg->enable_bcf && !cfg->enable_mba &&
        !cfg->enable_str_enc && !cfg->enable_vm_virt) {
        return;
    }

    uint64_t master_rng = cfg->seed ? cfg->seed : 0xC0FFEE1337BEEF55ULL;
    int max_label = find_max_label(ir);

    /* 1. Compile-time string encryption + runtime decryption */
    if (cfg->enable_str_enc) {
        pass_encrypt_strings(ir, cfg->enable_mba, &master_rng);
    }

    /* 2. Per-function Custom IR transformations */
    int func_idx = 0;
    for (IRFunc *fn = ir->funcs; fn; fn = fn->next, func_idx++) {
        uint64_t fn_rng = hash_str_seed(fn->name, master_rng ^ (uint64_t)func_idx);
        fn->obf_seed = fn_rng;

        if (cfg->enable_mba) {
            pass_mba_and_const_obf(fn, &fn_rng);
        }

        if (cfg->enable_cff) {
            pass_cff_and_bcf(fn, cfg->enable_bcf, cfg->enable_mba, &fn_rng, &max_label);
        } else if (cfg->enable_bcf) {
            pass_bcf_only(fn, &fn_rng, &max_label);
        }

        if (cfg->enable_vm_virt) {
            pass_virtualize_func(ir, fn, func_idx, &fn_rng, &max_label);
        }
    }
}
