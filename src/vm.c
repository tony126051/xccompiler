#include "xcc.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name;
    uint8_t *addr;
    int size;
} VMGlobal;

typedef struct {
    const IRProgram *ir;
    VMGlobal *globals;
    int global_count;
    uint8_t **str_addrs;
    int max_str_id;
} VMState;

static const IRFunc *find_ir_func(const IRProgram *ir, const char *name) {
    for (const IRFunc *fn = ir->funcs; fn; fn = fn->next) {
        if (strcmp(fn->name, name) == 0) return fn;
    }
    return NULL;
}

static uint8_t *find_vm_global(VMState *vm, const char *name) {
    for (int i = 0; i < vm->global_count; i++) {
        if (strcmp(vm->globals[i].name, name) == 0) {
            return vm->globals[i].addr;
        }
    }
    return NULL;
}

/* Dispatch common C standard library functions inside the IR VM */
static int64_t call_host_libc(const char *name, int argc, const int64_t *args) {
    if (strcmp(name, "putchar") == 0) {
        return putchar(argc > 0 ? (int)args[0] : 0);
    }
    if (strcmp(name, "puts") == 0) {
        return puts(argc > 0 ? (const char *)(uintptr_t)args[0] : "");
    }
    if (strcmp(name, "printf") == 0) {
        const char *fmt = argc > 0 ? (const char *)(uintptr_t)args[0] : "";
        int64_t a1 = argc > 1 ? args[1] : 0;
        int64_t a2 = argc > 2 ? args[2] : 0;
        int64_t a3 = argc > 3 ? args[3] : 0;
        int64_t a4 = argc > 4 ? args[4] : 0;
        int64_t a5 = argc > 5 ? args[5] : 0;
        int64_t a6 = argc > 6 ? args[6] : 0;
        return printf(fmt, a1, a2, a3, a4, a5, a6);
    }
    if (strcmp(name, "snprintf") == 0) {
        char *buf = (char *)(uintptr_t)args[0];
        size_t sz = (size_t)args[1];
        const char *fmt = (const char *)(uintptr_t)args[2];
        int64_t a1 = argc > 3 ? args[3] : 0;
        int64_t a2 = argc > 4 ? args[4] : 0;
        int64_t a3 = argc > 5 ? args[5] : 0;
        int64_t a4 = argc > 6 ? args[6] : 0;
        return snprintf(buf, sz, fmt, a1, a2, a3, a4);
    }
    if (strcmp(name, "malloc") == 0) {
        return (int64_t)(uintptr_t)malloc((size_t)args[0]);
    }
    if (strcmp(name, "calloc") == 0) {
        return (int64_t)(uintptr_t)calloc((size_t)args[0], (size_t)args[1]);
    }
    if (strcmp(name, "realloc") == 0) {
        return (int64_t)(uintptr_t)realloc((void *)(uintptr_t)args[0], (size_t)args[1]);
    }
    if (strcmp(name, "free") == 0) {
        free((void *)(uintptr_t)args[0]);
        return 0;
    }
    if (strcmp(name, "strlen") == 0) {
        return (int64_t)strlen((const char *)(uintptr_t)args[0]);
    }
    if (strcmp(name, "strcmp") == 0) {
        return strcmp((const char *)(uintptr_t)args[0], (const char *)(uintptr_t)args[1]);
    }
    if (strcmp(name, "strncmp") == 0) {
        return strncmp((const char *)(uintptr_t)args[0], (const char *)(uintptr_t)args[1], (size_t)args[2]);
    }
    if (strcmp(name, "strcpy") == 0) {
        return (int64_t)(uintptr_t)strcpy((char *)(uintptr_t)args[0], (const char *)(uintptr_t)args[1]);
    }
    if (strcmp(name, "memcpy") == 0) {
        return (int64_t)(uintptr_t)memcpy((void *)(uintptr_t)args[0], (const void *)(uintptr_t)args[1], (size_t)args[2]);
    }
    if (strcmp(name, "memset") == 0) {
        return (int64_t)(uintptr_t)memset((void *)(uintptr_t)args[0], (int)args[1], (size_t)args[2]);
    }
    if (strcmp(name, "memcmp") == 0) {
        return memcmp((const void *)(uintptr_t)args[0], (const void *)(uintptr_t)args[1], (size_t)args[2]);
    }
    if (strcmp(name, "abs") == 0) {
        return abs((int)args[0]);
    }
    if (strcmp(name, "atoi") == 0) {
        return atoi((const char *)(uintptr_t)args[0]);
    }
    if (strcmp(name, "exit") == 0) {
        exit((int)args[0]);
    }
    fatal_error("IR VM: unimplemented external function '%s'", name);
    return 0;
}

static int64_t exec_func(VMState *vm, const IRFunc *fn, int argc, const int64_t *args) {
    /* Allocate frame memory for all local slots */
    int total_local_bytes = 0;
    int *slot_offsets = (int *)calloc((size_t)(fn->local_count > 0 ? fn->local_count : 1), sizeof(int));
    for (int i = 0; i < fn->local_count; i++) {
        int sz = fn->locals[i].size > 0 ? fn->locals[i].size : 8;
        total_local_bytes = (total_local_bytes + 7) & ~7;
        slot_offsets[i] = total_local_bytes;
        total_local_bytes += sz;
    }
    uint8_t *frame_mem = (uint8_t *)calloc(1, (size_t)(total_local_bytes + 16));
    int64_t *vregs = (int64_t *)calloc((size_t)(fn->vreg_count > 0 ? fn->vreg_count : 1), sizeof(int64_t));

    /* Copy parameters into local slots */
    for (int i = 0; i < fn->param_count && i < argc; i++) {
        int slot_id = fn->param_local_ids[i];
        int psize = fn->param_sizes[i];
        uint8_t *dst = frame_mem + slot_offsets[slot_id];
        memcpy(dst, &args[i], (size_t)psize);
    }

    int64_t pending_args[64];
    int pc = 0;
    int64_t ret_val = 0;

    while (pc < fn->inst_count) {
        const IRInst *in = &fn->insts[pc++];
        switch (in->op) {
            case IR_NOP:
            case IR_LABEL:
                break;

            case IR_IMM:
                vregs[in->dst] = in->imm;
                break;

            case IR_OBF_IMM: {
                uint64_t enc = (uint64_t)in->imm;
                uint64_t key = in->obf_key;
                uint64_t rot = in->obf_rot;
                uint64_t t = ((enc ^ (key >> 17)) - key) ^ rot;
                vregs[in->dst] = (int64_t)t;
                break;
            }

            case IR_STR_ADDR:
                vregs[in->dst] = (int64_t)(uintptr_t)vm->str_addrs[in->imm];
                break;

            case IR_LOCAL_ADDR:
                vregs[in->dst] = (int64_t)(uintptr_t)(frame_mem + slot_offsets[in->imm]);
                break;

            case IR_GLOBAL_ADDR:
                vregs[in->dst] = (int64_t)(uintptr_t)find_vm_global(vm, in->sym_name);
                break;

            case IR_MOV:
                vregs[in->dst] = vregs[in->src1];
                break;

            case IR_LOAD: {
                const void *ptr = (const void *)(uintptr_t)vregs[in->src1];
                if (in->mem_size == 1) {
                    vregs[in->dst] = in->is_unsigned ? (int64_t)*(const uint8_t *)ptr : (int64_t)*(const int8_t *)ptr;
                } else if (in->mem_size == 2) {
                    vregs[in->dst] = in->is_unsigned ? (int64_t)*(const uint16_t *)ptr : (int64_t)*(const int16_t *)ptr;
                } else if (in->mem_size == 4) {
                    vregs[in->dst] = in->is_unsigned ? (int64_t)*(const uint32_t *)ptr : (int64_t)*(const int32_t *)ptr;
                } else {
                    vregs[in->dst] = *(const int64_t *)ptr;
                }
                break;
            }

            case IR_STORE: {
                void *ptr = (void *)(uintptr_t)vregs[in->src1];
                int64_t v = vregs[in->src2];
                if (in->mem_size == 1) *(uint8_t *)ptr = (uint8_t)v;
                else if (in->mem_size == 2) *(uint16_t *)ptr = (uint16_t)v;
                else if (in->mem_size == 4) *(uint32_t *)ptr = (uint32_t)v;
                else *(int64_t *)ptr = v;
                break;
            }

            case IR_MEMCPY:
                memcpy((void *)(uintptr_t)vregs[in->src1],
                       (const void *)(uintptr_t)vregs[in->src2],
                       (size_t)in->imm);
                break;

            case IR_NEG:    vregs[in->dst] = -vregs[in->src1]; break;
            case IR_BITNOT: vregs[in->dst] = ~vregs[in->src1]; break;
            case IR_LOGNOT: vregs[in->dst] = !vregs[in->src1]; break;

            case IR_CAST: {
                int64_t v = vregs[in->src1];
                if (in->mem_size == 1) vregs[in->dst] = in->is_unsigned ? (int64_t)(uint8_t)v : (int64_t)(int8_t)v;
                else if (in->mem_size == 2) vregs[in->dst] = in->is_unsigned ? (int64_t)(uint16_t)v : (int64_t)(int16_t)v;
                else if (in->mem_size == 4) vregs[in->dst] = in->is_unsigned ? (int64_t)(uint32_t)v : (int64_t)(int32_t)v;
                else vregs[in->dst] = v;
                break;
            }

            case IR_ADD:    vregs[in->dst] = vregs[in->src1] + vregs[in->src2]; break;
            case IR_SUB:    vregs[in->dst] = vregs[in->src1] - vregs[in->src2]; break;
            case IR_MUL:    vregs[in->dst] = vregs[in->src1] * vregs[in->src2]; break;
            case IR_DIV:
                if (in->is_unsigned) vregs[in->dst] = (int64_t)((uint64_t)vregs[in->src1] / (uint64_t)vregs[in->src2]);
                else vregs[in->dst] = vregs[in->src1] / vregs[in->src2];
                break;
            case IR_MOD:
                if (in->is_unsigned) vregs[in->dst] = (int64_t)((uint64_t)vregs[in->src1] % (uint64_t)vregs[in->src2]);
                else vregs[in->dst] = vregs[in->src1] % vregs[in->src2];
                break;
            case IR_BITAND: vregs[in->dst] = vregs[in->src1] & vregs[in->src2]; break;
            case IR_BITOR:  vregs[in->dst] = vregs[in->src1] | vregs[in->src2]; break;
            case IR_BITXOR: vregs[in->dst] = vregs[in->src1] ^ vregs[in->src2]; break;
            case IR_OBF_MBA_ADD: {
                uint64_t x = (uint64_t)vregs[in->src1];
                uint64_t y = (uint64_t)vregs[in->src2];
                uint64_t k = in->obf_key;
                uint64_t u = (x ^ y) ^ k;
                uint64_t v = (x & y) << 1;
                vregs[in->dst] = (int64_t)((u ^ k) + v);
                break;
            }
            case IR_OBF_MBA_SUB: {
                uint64_t x = (uint64_t)vregs[in->src1];
                uint64_t y = (uint64_t)vregs[in->src2];
                uint64_t k = in->obf_key;
                uint64_t t1 = ((x & ~y) ^ k) ^ k;
                uint64_t t2 = (~x & y);
                vregs[in->dst] = (int64_t)(t1 - t2);
                break;
            }
            case IR_OBF_MBA_XOR: {
                uint64_t x = (uint64_t)vregs[in->src1];
                uint64_t y = (uint64_t)vregs[in->src2];
                uint64_t k = in->obf_key;
                vregs[in->dst] = (int64_t)(((x | y) + k) - ((x & y) + k));
                break;
            }
            case IR_OBF_OPAQUE_TRUE: {
                uint64_t x = (in->src1 >= 0) ? (uint64_t)vregs[in->src1] : (uint64_t)(uintptr_t)frame_mem;
                uint64_t z = x ^ in->obf_key;
                vregs[in->dst] = (((z * (z + 1ULL)) & 1ULL) == 0ULL) ? 1 : 0;
                break;
            }
            case IR_OBF_OPAQUE_FALSE: {
                uint64_t x = (in->src1 >= 0) ? (uint64_t)vregs[in->src1] : (uint64_t)(uintptr_t)frame_mem;
                uint64_t z = x ^ in->obf_key;
                vregs[in->dst] = (((z * (z + 1ULL)) & 1ULL) != 0ULL) ? 1 : 0;
                break;
            }
            case IR_OBF_JUNK:
                break;
            case IR_SHL:    vregs[in->dst] = vregs[in->src1] << vregs[in->src2]; break;
            case IR_SHR:
                if (in->is_unsigned) vregs[in->dst] = (int64_t)((uint64_t)vregs[in->src1] >> vregs[in->src2]);
                else vregs[in->dst] = vregs[in->src1] >> vregs[in->src2];
                break;

            case IR_EQ: vregs[in->dst] = (vregs[in->src1] == vregs[in->src2]); break;
            case IR_NE: vregs[in->dst] = (vregs[in->src1] != vregs[in->src2]); break;
            case IR_LT:
                vregs[in->dst] = in->is_unsigned
                                     ? ((uint64_t)vregs[in->src1] < (uint64_t)vregs[in->src2])
                                     : (vregs[in->src1] < vregs[in->src2]);
                break;
            case IR_LE:
                vregs[in->dst] = in->is_unsigned
                                     ? ((uint64_t)vregs[in->src1] <= (uint64_t)vregs[in->src2])
                                     : (vregs[in->src1] <= vregs[in->src2]);
                break;
            case IR_GT:
                vregs[in->dst] = in->is_unsigned
                                     ? ((uint64_t)vregs[in->src1] > (uint64_t)vregs[in->src2])
                                     : (vregs[in->src1] > vregs[in->src2]);
                break;
            case IR_GE:
                vregs[in->dst] = in->is_unsigned
                                     ? ((uint64_t)vregs[in->src1] >= (uint64_t)vregs[in->src2])
                                     : (vregs[in->src1] >= vregs[in->src2]);
                break;

            case IR_JMP:
            case IR_JZ:
            case IR_JNZ: {
                bool take = (in->op == IR_JMP) ||
                            (in->op == IR_JZ && vregs[in->src1] == 0) ||
                            (in->op == IR_JNZ && vregs[in->src1] != 0);
                if (take) {
                    for (int j = 0; j < fn->inst_count; j++) {
                        if (fn->insts[j].op == IR_LABEL && fn->insts[j].label == in->label) {
                            pc = j;
                            break;
                        }
                    }
                }
                break;
            }

            case IR_ARG:
                if (in->imm >= 0 && in->imm < 64) {
                    pending_args[in->imm] = vregs[in->src1];
                }
                break;

            case IR_CALL: {
                int64_t call_args[64];
                for (int a = 0; a < in->call_arg_count && a < 64; a++) {
                    call_args[a] = pending_args[a];
                }
                const IRFunc *callee = find_ir_func(vm->ir, in->sym_name);
                int64_t res = 0;
                if (callee) {
                    res = exec_func(vm, callee, in->call_arg_count, call_args);
                } else {
                    res = call_host_libc(in->sym_name, in->call_arg_count, call_args);
                }
                if (in->dst >= 0) {
                    vregs[in->dst] = res;
                }
                break;
            }

            case IR_RET:
                ret_val = (in->src1 >= 0) ? vregs[in->src1] : 0;
                pc = fn->inst_count;
                break;
        }
    }

    free(slot_offsets);
    free(frame_mem);
    free(vregs);
    return ret_val;
}

int run_ir_vm(const IRProgram *ir, int argc, char **argv) {
    VMState vm;
    memset(&vm, 0, sizeof(vm));
    vm.ir = ir;

    /* Set up string literal table */
    int max_sid = 0;
    for (const StringLiteral *s = ir->strings; s; s = s->next) {
        if (s->id + 1 > max_sid) max_sid = s->id + 1;
    }
    vm.max_str_id = max_sid;
    vm.str_addrs = (uint8_t **)calloc((size_t)(max_sid > 0 ? max_sid : 1), sizeof(uint8_t *));
    for (const StringLiteral *s = ir->strings; s; s = s->next) {
        vm.str_addrs[s->id] = (uint8_t *)s->data;
    }

    /* Set up global variables */
    int gcount = 0;
    for (const VarObj *g = ir->globals; g; g = g->next) {
        if (g->type->kind != TY_FUNC) gcount++;
    }
    vm.global_count = gcount;
    vm.globals = (VMGlobal *)calloc((size_t)(gcount > 0 ? gcount : 1), sizeof(VMGlobal));
    int idx = 0;
    for (const VarObj *g = ir->globals; g; g = g->next) {
        if (g->type->kind == TY_FUNC) continue;
        int sz = g->type->size > 0 ? g->type->size : 8;
        vm.globals[idx].name = g->name;
        vm.globals[idx].size = sz;
        vm.globals[idx].addr = (uint8_t *)calloc(1, (size_t)sz);
        idx++;
    }

    /* Initialize globals */
    idx = 0;
    for (const VarObj *g = ir->globals; g; g = g->next) {
        if (g->type->kind == TY_FUNC) continue;
        uint8_t *base = vm.globals[idx++].addr;
        for (const GlobalInit *gi = g->init_list; gi; gi = gi->next) {
            uint8_t *dst = base + gi->offset;
            if (gi->label) {
                uintptr_t ptr_val = 0;
                if (strncmp(gi->label, ".L.str.", 7) == 0) {
                    int sid = atoi(gi->label + 7);
                    ptr_val = (uintptr_t)vm.str_addrs[sid];
                } else {
                    ptr_val = (uintptr_t)find_vm_global(&vm, gi->label);
                }
                ptr_val += (uintptr_t)gi->addend;
                memcpy(dst, &ptr_val, sizeof(uintptr_t));
            } else {
                memcpy(dst, &gi->val, (size_t)gi->size);
            }
        }
    }

    const IRFunc *main_fn = find_ir_func(ir, "main");
    if (!main_fn) {
        fatal_error("IR VM: function 'main' not found");
    }

    int64_t main_args[2] = { (int64_t)argc, (int64_t)(uintptr_t)argv };
    int64_t exit_code = exec_func(&vm, main_fn, 2, main_args);

    for (int i = 0; i < vm.global_count; i++) {
        free(vm.globals[i].addr);
    }
    free(vm.globals);
    free(vm.str_addrs);
    return (int)exit_code;
}
