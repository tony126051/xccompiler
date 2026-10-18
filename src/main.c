#include "xcc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void print_usage(const char *prog) {
    printf("XCC - Cross-Platform C Compiler (written in C)\n\n");
    printf("Usage:\n");
    printf("  %s [options] <input.c> [-o <output>]\n\n", prog);
    printf("Options:\n");
    printf("  -o <file>             Write output to <file>\n");
    printf("  -target <target>      Specify target platform/architecture:\n");
    printf("                          arm64-macos    (arm64-apple-darwin)\n");
    printf("                          arm64-linux    (aarch64-unknown-linux-gnu)\n");
    printf("                          x86_64-macos   (x86_64-apple-darwin)\n");
    printf("                          x86_64-linux   (x86_64-unknown-linux-gnu)\n");
    printf("                          x86_64-windows (x86_64-w64-mingw32)\n");
    printf("  -E                    Run preprocessor only\n");
    printf("  -S                    Compile only; emit target assembly (.s)\n");
    printf("  -c                    Compile and assemble; emit object file (.o)\n");
    printf("  --emit-ast            Dump parsed Abstract Syntax Tree (AST)\n");
    printf("  --emit-ir             Dump platform-independent / Custom Hardened IR\n");
    printf("  --run                 Compile to native binary and execute immediately\n");
    printf("  --run-ir              Execute directly in built-in cross-platform IR VM\n");
    printf("  -I <dir>              Add directory to #include search path\n");
    printf("  -D <name>[=<val>]     Define preprocessor macro\n\n");
    printf("Anti-Reversing & Custom IR Protection Options:\n");
    printf("  --obf, -fobf          Enable Custom IR Obfuscation (CFF + BCF + MBA + String Enc)\n");
    printf("  --obf-cff             Enable Control-Flow Flattening & Branchless State Transitions\n");
    printf("  --obf-bcf             Enable Bogus Control Flow & Number-Theoretic Opaque Predicates\n");
    printf("  --obf-mba             Enable Mixed Boolean-Arithmetic (MBA) & Dynamic Constants\n");
    printf("  --obf-str             Enable Compile-Time String Encryption & Runtime Decryption\n");
    printf("  --obf-vm, -fvm        Enable Polymorphic Self-Checksumming Bytecode VM Virtualizer\n");
    printf("  --obf-all             Enable Dual-Track Protection (Full Custom IR Obfuscation + VM)\n");
    printf("  --obf-seed <uint64>   Set polymorphic encryption/shuffling seed\n");
    printf("  -h, --help            Display this help message\n");
}

static char *replace_ext(const char *path, const char *new_ext) {
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    const char *dot = strrchr(base, '.');
    size_t stem_len = dot ? (size_t)(dot - base) : strlen(base);
    size_t ext_len = strlen(new_ext);
    char *out = (char *)malloc(stem_len + ext_len + 1);
    memcpy(out, base, stem_len);
    memcpy(out + stem_len, new_ext, ext_len + 1);
    return out;
}

static void emit_target_asm(const IRProgram *ir, const TargetInfo *target, FILE *out) {
    if (target->is_arm64) {
        emit_arm64_asm(ir, target, out);
    } else {
        emit_x86_64_asm(ir, target, out);
    }
}

int main(int argc, char **argv) {
    const char *input_file = NULL;
    const char *output_file = NULL;
    TargetPlatform target_plat = get_host_target();
    bool flag_E = false;
    bool flag_S = false;
    bool flag_c = false;
    bool flag_emit_ast = false;
    bool flag_emit_ir = false;
    bool flag_run = false;
    bool flag_run_ir = false;

    ObfuscationConfig obf_cfg = {
        .enable_cff = false,
        .enable_bcf = false,
        .enable_mba = false,
        .enable_str_enc = false,
        .enable_vm_virt = false,
        .seed = 0xC0FFEE1337BEEF55ULL,
    };

    const char *inc_paths[64];
    int inc_count = 0;
    const char *defines[64];
    int def_count = 0;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(arg, "-E") == 0) { flag_E = true; continue; }
        if (strcmp(arg, "-S") == 0) { flag_S = true; continue; }
        if (strcmp(arg, "-c") == 0) { flag_c = true; continue; }
        if (strcmp(arg, "--emit-ast") == 0) { flag_emit_ast = true; continue; }
        if (strcmp(arg, "--emit-ir") == 0) { flag_emit_ir = true; continue; }
        if (strcmp(arg, "--run") == 0) { flag_run = true; continue; }
        if (strcmp(arg, "--run-ir") == 0) { flag_run_ir = true; continue; }
        if (strcmp(arg, "--obf") == 0 || strcmp(arg, "-fobf") == 0) {
            obf_cfg.enable_cff = true;
            obf_cfg.enable_bcf = true;
            obf_cfg.enable_mba = true;
            obf_cfg.enable_str_enc = true;
            continue;
        }
        if (strcmp(arg, "--obf-cff") == 0) { obf_cfg.enable_cff = true; continue; }
        if (strcmp(arg, "--obf-bcf") == 0) { obf_cfg.enable_bcf = true; continue; }
        if (strcmp(arg, "--obf-mba") == 0) { obf_cfg.enable_mba = true; continue; }
        if (strcmp(arg, "--obf-str") == 0) { obf_cfg.enable_str_enc = true; continue; }
        if (strcmp(arg, "--obf-vm") == 0 || strcmp(arg, "-fvm") == 0) {
            obf_cfg.enable_vm_virt = true;
            continue;
        }
        if (strcmp(arg, "--obf-all") == 0) {
            obf_cfg.enable_cff = true;
            obf_cfg.enable_bcf = true;
            obf_cfg.enable_mba = true;
            obf_cfg.enable_str_enc = true;
            obf_cfg.enable_vm_virt = true;
            continue;
        }
        if (strcmp(arg, "--obf-seed") == 0) {
            if (i + 1 >= argc) fatal_error("missing seed value after '--obf-seed'");
            obf_cfg.seed = strtoull(argv[++i], NULL, 0);
            continue;
        }
        if (strncmp(arg, "--obf-seed=", 11) == 0) {
            obf_cfg.seed = strtoull(arg + 11, NULL, 0);
            continue;
        }
        if (strcmp(arg, "-o") == 0) {
            if (i + 1 >= argc) fatal_error("missing filename after '-o'");
            output_file = argv[++i];
            continue;
        }
        if (strcmp(arg, "-target") == 0 || strcmp(arg, "--target") == 0) {
            if (i + 1 >= argc) fatal_error("missing target name after '%s'", arg);
            const char *tstr = argv[++i];
            if (!parse_target_name(tstr, &target_plat)) {
                fatal_error("unknown target '%s' (supported: arm64-macos, arm64-linux, x86_64-macos, x86_64-linux, x86_64-windows)", tstr);
            }
            continue;
        }
        if (strncmp(arg, "--target=", 9) == 0) {
            const char *tstr = arg + 9;
            if (!parse_target_name(tstr, &target_plat)) {
                fatal_error("unknown target '%s'", tstr);
            }
            continue;
        }
        if (strcmp(arg, "-I") == 0) {
            if (i + 1 >= argc) fatal_error("missing directory after '-I'");
            if (inc_count < 64) inc_paths[inc_count++] = argv[++i];
            continue;
        }
        if (strncmp(arg, "-I", 2) == 0) {
            if (inc_count < 64) inc_paths[inc_count++] = arg + 2;
            continue;
        }
        if (strcmp(arg, "-D") == 0) {
            if (i + 1 >= argc) fatal_error("missing macro definition after '-D'");
            if (def_count < 64) defines[def_count++] = argv[++i];
            continue;
        }
        if (strncmp(arg, "-D", 2) == 0) {
            if (def_count < 64) defines[def_count++] = arg + 2;
            continue;
        }
        if (arg[0] == '-') {
            fatal_error("unrecognized option '%s'", arg);
        }
        input_file = arg;
    }

    if (!input_file) {
        print_usage(argv[0]);
        return 1;
    }

    char *raw_source = read_file_text(input_file);
    if (!raw_source) {
        fatal_error("cannot open input file '%s'", input_file);
    }

    if ((obf_cfg.enable_cff || obf_cfg.enable_bcf || obf_cfg.enable_mba ||
         obf_cfg.enable_str_enc || obf_cfg.enable_vm_virt) && def_count < 64) {
        defines[def_count++] = "__XCC_OBFUSCATED__=1";
    }

    PreprocessorConfig pp_cfg = {
        .include_paths = inc_paths,
        .include_path_count = inc_count,
        .cmd_defines = defines,
        .cmd_define_count = def_count,
        .target = target_plat,
    };

    char *preprocessed = preprocess_source(input_file, raw_source, &pp_cfg);
    if (flag_E) {
        if (output_file) {
            FILE *fp = fopen(output_file, "w");
            if (!fp) fatal_error("cannot open output file '%s'", output_file);
            fputs(preprocessed, fp);
            fclose(fp);
        } else {
            fputs(preprocessed, stdout);
        }
        free(preprocessed);
        free(raw_source);
        return 0;
    }

    Token *tokens = tokenize(input_file, preprocessed);
    ProgramAST *ast = parse_program(tokens);

    if (flag_emit_ast) {
        dump_ast(ast, stdout);
        if (!flag_emit_ir && !flag_S && !flag_c && !flag_run && !flag_run_ir && !output_file) {
            return 0;
        }
    }

    IRProgram *ir = generate_ir(ast);
    obfuscate_ir(ir, &obf_cfg);

    if (flag_emit_ir) {
        if (output_file && !flag_S && !flag_c && !flag_run) {
            FILE *fp = fopen(output_file, "w");
            if (!fp) fatal_error("cannot open output file '%s'", output_file);
            dump_ir(ir, fp);
            fclose(fp);
            return 0;
        }
        dump_ir(ir, stdout);
        if (!flag_S && !flag_c && !flag_run && !flag_run_ir) {
            return 0;
        }
    }

    if (flag_run_ir) {
        return run_ir_vm(ir, 1, (char **)&input_file);
    }

    const TargetInfo *target = get_target_info(target_plat);

    /* -S: Emit assembly file */
    if (flag_S) {
        char *asm_path = output_file ? strdup(output_file) : replace_ext(input_file, ".s");
        FILE *fp = (strcmp(asm_path, "-") == 0) ? stdout : fopen(asm_path, "w");
        if (!fp) fatal_error("cannot open output file '%s'", asm_path);
        emit_target_asm(ir, target, fp);
        if (fp != stdout) fclose(fp);
        free(asm_path);
        return 0;
    }

    /* Emit assembly to temporary file for assembling/linking */
    char tmp_asm[256];
    snprintf(tmp_asm, sizeof(tmp_asm), "/tmp/xcc_%d.s", (int)getpid());
    FILE *asm_fp = fopen(tmp_asm, "w");
    if (!asm_fp) fatal_error("cannot create temporary assembly file '%s'", tmp_asm);
    emit_target_asm(ir, target, asm_fp);
    fclose(asm_fp);

    /* -c: Assemble into object file (.o) */
    if (flag_c) {
        char *obj_path = output_file ? strdup(output_file) : replace_ext(input_file, ".o");
        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "cc -target %s -x assembler -c \"%s\" -o \"%s\"",
                 target->triple, tmp_asm, obj_path);
        int rc = system(cmd);
        unlink(tmp_asm);
        free(obj_path);
        if (rc != 0) fatal_error("assembler command failed");
        return 0;
    }

    /* Full compilation & link into native executable */
    char tmp_bin[256];
    const char *bin_out = output_file ? output_file : "a.out";
    if (flag_run && !output_file) {
        snprintf(tmp_bin, sizeof(tmp_bin), "/tmp/xcc_bin_%d", (int)getpid());
        bin_out = tmp_bin;
    }

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "cc -target %s \"%s\" -o \"%s\"",
             target->triple, tmp_asm, bin_out);
    int rc = system(cmd);
    unlink(tmp_asm);
    if (rc != 0) {
        fatal_error("assembler/linker failed for target '%s' (tip: use -S to emit assembly or -c to emit object file)", target->name);
    }

    if (flag_run) {
        int run_rc = system(bin_out);
        if (!output_file) {
            unlink(bin_out);
        }
        if (WIFEXITED(run_rc)) {
            return WEXITSTATUS(run_rc);
        }
        return run_rc;
    }

    return 0;
}
