#ifndef XCC_H
#define XCC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ============================================================================
 * Target Platforms & Architectures
 * ============================================================================ */

typedef enum {
    TARGET_ARM64_MACOS,
    TARGET_ARM64_LINUX,
    TARGET_X86_64_MACOS,
    TARGET_X86_64_LINUX,
    TARGET_X86_64_WINDOWS,
} TargetPlatform;

typedef struct {
    TargetPlatform platform;
    const char *name;
    const char *triple;
    bool is_macos;
    bool is_linux;
    bool is_windows;
    bool is_arm64;
    bool is_x86_64;
    const char *symbol_prefix; /* "_" on macOS, "" on Linux/Windows */
} TargetInfo;

const TargetInfo *get_target_info(TargetPlatform platform);
TargetPlatform get_host_target(void);
bool parse_target_name(const char *str, TargetPlatform *out);

/* ============================================================================
 * Source Location & Diagnostics
 * ============================================================================ */

typedef struct {
    const char *filename;
    int line;
    int col;
} SourceLoc;

void error_at(SourceLoc loc, const char *fmt, ...);
void fatal_error(const char *fmt, ...);

/* ============================================================================
 * Preprocessor
 * ============================================================================ */

typedef struct {
    const char **include_paths;
    int include_path_count;
    const char **cmd_defines;
    int cmd_define_count;
    TargetPlatform target;
} PreprocessorConfig;

char *preprocess_source(const char *filename, const char *source, const PreprocessorConfig *cfg);
char *read_file_text(const char *path);

/* ============================================================================
 * Lexer & Tokens
 * ============================================================================ */

typedef enum {
    TOK_EOF = 0,
    TOK_IDENT,
    TOK_INT_LIT,
    TOK_CHAR_LIT,
    TOK_STR_LIT,

    /* Keywords */
    TOK_VOID,
    TOK_CHAR,
    TOK_SHORT,
    TOK_INT,
    TOK_LONG,
    TOK_SIGNED,
    TOK_UNSIGNED,
    TOK_BOOL,
    TOK_STRUCT,
    TOK_UNION,
    TOK_ENUM,
    TOK_TYPEDEF,
    TOK_STATIC,
    TOK_EXTERN,
    TOK_CONST,
    TOK_SIZEOF,
    TOK_IF,
    TOK_ELSE,
    TOK_WHILE,
    TOK_DO,
    TOK_FOR,
    TOK_SWITCH,
    TOK_CASE,
    TOK_DEFAULT,
    TOK_BREAK,
    TOK_CONTINUE,
    TOK_RETURN,
    TOK_GOTO,

    /* Punctuation & Operators */
    TOK_LPAREN,      /* ( */
    TOK_RPAREN,      /* ) */
    TOK_LBRACE,      /* { */
    TOK_RBRACE,      /* } */
    TOK_LBRACKET,    /* [ */
    TOK_RBRACKET,    /* ] */
    TOK_SEMI,        /* ; */
    TOK_COMMA,       /* , */
    TOK_DOT,         /* . */
    TOK_ARROW,       /* -> */
    TOK_ELLIPSIS,    /* ... */
    TOK_QUESTION,    /* ? */
    TOK_COLON,       /* : */

    TOK_PLUS,        /* + */
    TOK_MINUS,       /* - */
    TOK_STAR,        /* * */
    TOK_SLASH,       /* / */
    TOK_PERCENT,     /* % */
    TOK_AMP,         /* & */
    TOK_PIPE,        /* | */
    TOK_CARET,       /* ^ */
    TOK_TILDE,       /* ~ */
    TOK_BANG,        /* ! */
    TOK_LSHIFT,      /* << */
    TOK_RSHIFT,      /* >> */

    TOK_INC,         /* ++ */
    TOK_DEC,         /* -- */

    TOK_EQ,          /* == */
    TOK_NE,          /* != */
    TOK_LT,          /* < */
    TOK_LE,          /* <= */
    TOK_GT,          /* > */
    TOK_GE,          /* >= */
    TOK_LOGAND,      /* && */
    TOK_LOGOR,       /* || */

    TOK_ASSIGN,      /* = */
    TOK_ADD_ASSIGN,  /* += */
    TOK_SUB_ASSIGN,  /* -= */
    TOK_MUL_ASSIGN,  /* *= */
    TOK_DIV_ASSIGN,  /* /= */
    TOK_MOD_ASSIGN,  /* %= */
    TOK_AND_ASSIGN,  /* &= */
    TOK_OR_ASSIGN,   /* |= */
    TOK_XOR_ASSIGN,  /* ^= */
    TOK_SHL_ASSIGN,  /* <<= */
    TOK_SHR_ASSIGN,  /* >>= */
} TokenKind;

typedef struct Token Token;
struct Token {
    TokenKind kind;
    SourceLoc loc;
    const char *lexeme;
    int len;
    int64_t int_val;
    char *str_val;      /* For identifiers and decoded string literals */
    int str_len;        /* Byte length for string literals (excluding null terminator) */
    Token *next;
};

Token *tokenize(const char *filename, const char *source);
const char *token_kind_name(TokenKind kind);

/* ============================================================================
 * Type System
 * ============================================================================ */

typedef enum {
    TY_VOID,
    TY_BOOL,
    TY_CHAR,
    TY_SHORT,
    TY_INT,
    TY_LONG,
    TY_ENUM,
    TY_PTR,
    TY_ARRAY,
    TY_STRUCT,
    TY_UNION,
    TY_FUNC,
} TypeKind;

typedef struct Type Type;
typedef struct Member Member;
typedef struct Param Param;

struct Member {
    char *name;
    Type *type;
    int offset;
    Member *next;
};

struct Param {
    char *name;
    Type *type;
    Param *next;
};

struct Type {
    TypeKind kind;
    int size;
    int align;
    bool is_unsigned;

    /* Pointer or Array base type */
    Type *base;
    int array_len;

    /* Struct / Union */
    char *tag;
    Member *members;
    bool is_complete;

    /* Function type */
    Type *return_type;
    Param *params;
    int param_count;
    bool is_variadic;
};

extern Type *ty_void;
extern Type *ty_bool;
extern Type *ty_char;
extern Type *ty_uchar;
extern Type *ty_short;
extern Type *ty_ushort;
extern Type *ty_int;
extern Type *ty_uint;
extern Type *ty_long;
extern Type *ty_ulong;

Type *new_type(TypeKind kind, int size, int align);
Type *pointer_to(Type *base);
Type *array_of(Type *base, int len);
Type *func_type(Type *ret_type);
Type *copy_type(Type *ty);
bool is_integer_type(Type *ty);
bool is_pointer_like(Type *ty);
bool is_scalar_type(Type *ty);
Type *common_type(Type *t1, Type *t2);
Member *find_struct_member(Type *struct_ty, const char *name);
void format_type(Type *ty, char *buf, size_t buf_size);

/* ============================================================================
 * Abstract Syntax Tree (AST)
 * ============================================================================ */

typedef enum {
    /* Statements */
    ND_BLOCK,
    ND_IF,
    ND_WHILE,
    ND_DO_WHILE,
    ND_FOR,
    ND_SWITCH,
    ND_CASE,
    ND_BREAK,
    ND_CONTINUE,
    ND_GOTO,
    ND_LABEL,
    ND_RETURN,
    ND_EXPR_STMT,
    ND_EMPTY,

    /* Expressions */
    ND_INT_LIT,
    ND_STR_LIT,
    ND_VAR,
    ND_ADDR,         /* &x */
    ND_DEREF,        /* *x */
    ND_MEMBER,       /* x.m */
    ND_CAST,         /* (type)x */
    ND_NEG,          /* -x */
    ND_BITNOT,       /* ~x */
    ND_LOGNOT,       /* !x */
    ND_PRE_INC,      /* ++x */
    ND_PRE_DEC,      /* --x */
    ND_POST_INC,     /* x++ */
    ND_POST_DEC,     /* x-- */

    /* Binary Operators */
    ND_ADD,
    ND_SUB,
    ND_MUL,
    ND_DIV,
    ND_MOD,
    ND_BITAND,
    ND_BITOR,
    ND_BITXOR,
    ND_SHL,
    ND_SHR,
    ND_EQ,
    ND_NE,
    ND_LT,
    ND_LE,
    ND_GT,
    ND_GE,
    ND_LOGAND,
    ND_LOGOR,
    ND_ASSIGN,
    ND_COMMA,
    ND_TERNARY,      /* cond ? then : els */
    ND_FUNCALL,
} NodeKind;

typedef struct VarObj VarObj;
typedef struct Node Node;
typedef struct GlobalInit GlobalInit;

struct GlobalInit {
    int offset;
    int size;
    int64_t val;
    char *label;     /* Optional symbol/string label reference */
    int64_t addend;
    GlobalInit *next;
};

struct VarObj {
    char *name;
    Type *type;
    bool is_local;
    bool is_static;
    bool is_extern;
    bool is_param;

    /* Local variable offset or IR slot ID */
    int local_id;
    int stack_offset;

    /* Global initializer data */
    bool has_init;
    GlobalInit *init_list;

    VarObj *next;
};

struct Node {
    NodeKind kind;
    SourceLoc loc;
    Type *type;
    Node *next; /* Next statement in block or next argument in function call */

    /* Integer literal */
    int64_t int_val;

    /* String literal */
    int str_id;
    char *str_data;
    int str_len;

    /* Variable access */
    VarObj *var;

    /* Struct member access */
    Member *member;

    /* Unary / Binary / Ternary children */
    Node *lhs;
    Node *rhs;
    Node *cond;
    Node *then_branch;
    Node *else_branch;
    Node *init;
    Node *step;

    /* Block statements */
    Node *body;

    /* Switch / Case / Goto / Label */
    char *label_name;
    int case_label_id;
    Node *cases_next;
    Node *default_case;

    /* Function call */
    char *func_name;
    Type *func_type;
    Node *args;
    int arg_count;
};

typedef struct FunctionDef FunctionDef;
struct FunctionDef {
    char *name;
    Type *type;
    bool is_static;
    bool is_external_decl;
    VarObj *params;
    VarObj *locals;
    int local_count;
    Node *body;
    FunctionDef *next;
};

typedef struct StringLiteral StringLiteral;
struct StringLiteral {
    int id;
    char *data;
    int len;
    bool is_encrypted;
    uint64_t enc_key;
    StringLiteral *next;
};

typedef struct ProgramAST {
    FunctionDef *functions;
    VarObj *globals;
    StringLiteral *strings;
} ProgramAST;

ProgramAST *parse_program(Token *tokens);
void dump_ast(const ProgramAST *prog, FILE *out);

/* ============================================================================
 * Platform-Independent Intermediate Representation (3-Address Code IR)
 * ============================================================================ */

typedef enum {
    IR_NOP = 0,
    IR_IMM,          /* dst = imm */
    IR_STR_ADDR,     /* dst = address of string literal str_id */
    IR_LOCAL_ADDR,   /* dst = address of local slot (imm = local_id) */
    IR_GLOBAL_ADDR,  /* dst = address of global symbol (name) */
    IR_MOV,          /* dst = src1 */

    IR_LOAD,         /* dst = *[src1] (size = mem_size, is_unsigned) */
    IR_STORE,        /* *[src1] = src2 (size = mem_size) */
    IR_MEMCPY,       /* copy imm bytes from src2 to src1 */

    IR_NEG,          /* dst = -src1 */
    IR_BITNOT,       /* dst = ~src1 */
    IR_LOGNOT,       /* dst = !src1 */
    IR_CAST,         /* dst = (size/sign) src1 */

    IR_ADD,          /* dst = src1 + src2 */
    IR_SUB,          /* dst = src1 - src2 */
    IR_MUL,          /* dst = src1 * src2 */
    IR_DIV,          /* dst = src1 / src2 (signed/unsigned) */
    IR_MOD,          /* dst = src1 % src2 (signed/unsigned) */
    IR_BITAND,       /* dst = src1 & src2 */
    IR_BITOR,        /* dst = src1 | src2 */
    IR_BITXOR,       /* dst = src1 ^ src2 */
    IR_SHL,          /* dst = src1 << src2 */
    IR_SHR,          /* dst = src1 >> src2 (arithmetic or logical) */

    IR_EQ,           /* dst = (src1 == src2) */
    IR_NE,           /* dst = (src1 != src2) */
    IR_LT,           /* dst = (src1 < src2) */
    IR_LE,           /* dst = (src1 <= src2) */
    IR_GT,           /* dst = (src1 > src2) */
    IR_GE,           /* dst = (src1 >= src2) */

    IR_LABEL,        /* .L<label>: */
    IR_JMP,          /* goto .L<label> */
    IR_JZ,           /* if (src1 == 0) goto .L<label> */
    IR_JNZ,          /* if (src1 != 0) goto .L<label> */

    IR_ARG,          /* Prepare argument #imm (src1) for upcoming call */
    IR_CALL,         /* dst = call name(arg_count, fixed_params) */
    IR_RET,          /* return src1 (or void if src1 == -1) */

    /* Custom Anti-Reversing / Hardened Obfuscated IR Instructions */
    IR_OBF_IMM,          /* dst = dynamic_reconstruct(imm=enc, obf_key, obf_rot) */
    IR_OBF_MBA_ADD,      /* dst = ((src1 ^ src2) + ((src1 & src2) << 1)) keyed with obf_key */
    IR_OBF_MBA_SUB,      /* dst = ((src1 & ~src2) - (~src1 & src2)) keyed with obf_key */
    IR_OBF_MBA_XOR,      /* dst = ((src1 | src2) + obf_key) - ((src1 & src2) + obf_key) */
    IR_OBF_OPAQUE_TRUE,  /* dst = ((((src1 ^ obf_key) * ((src1 ^ obf_key) + 1)) & 1) == 0) -> 1 */
    IR_OBF_OPAQUE_FALSE, /* dst = ((((src1 ^ obf_key) * ((src1 ^ obf_key) + 1)) & 1) != 0) -> 0 */
    IR_OBF_JUNK,         /* Anti-decompiler register/stack entropy perturbation */
} IROp;

typedef struct {
    IROp op;
    int dst;         /* Virtual register index (or -1) */
    int src1;        /* Virtual register index (or -1) */
    int src2;        /* Virtual register index (or -1) */
    int64_t imm;     /* Immediate value, local_id, str_id, or arg index */
    int label;       /* Target or defined label ID */
    int mem_size;    /* 1, 2, 4, or 8 bytes for LOAD/STORE/CAST/DIV/MOD/CMP */
    bool is_unsigned;/* True if unsigned operation */
    const char *sym_name; /* Symbol name for GLOBAL_ADDR or CALL */
    int call_arg_count;   /* Total args in IR_CALL */
    int call_fixed_count; /* Number of non-variadic params (for Apple ARM64 ABI) */
    bool call_is_variadic;

    /* Custom Anti-Reversing IR metadata */
    uint64_t obf_key;        /* Polymorphic key for MBA / Opaque / Constant encryption */
    uint64_t obf_rot;        /* Secondary polymorphic mask for IR_OBF_IMM */
    const char *obf_comment; /* Diagnostic comment for --emit-ir */
} IRInst;

typedef struct {
    int id;
    int size;
    int align;
    int stack_offset; /* Assigned by backend */
    const char *debug_name;
} IRLocalSlot;

typedef struct IRFunc IRFunc;
struct IRFunc {
    const char *name;
    bool is_static;
    int param_count;
    int *param_local_ids; /* Local slot IDs for parameters */
    int *param_sizes;

    IRLocalSlot *locals;
    int local_count;
    int local_cap;

    IRInst *insts;
    int inst_count;
    int inst_cap;

    int vreg_count;

    bool is_obfuscated;
    bool is_virtualized;
    uint64_t obf_seed;
    IRFunc *next;
};

typedef struct {
    IRFunc *funcs;
    VarObj *globals;
    StringLiteral *strings;
} IRProgram;

typedef struct {
    bool enable_cff;     /* Control-Flow Flattening + Branchless State Selection */
    bool enable_bcf;     /* Bogus Control Flow + Number-Theoretic Opaque Predicates */
    bool enable_mba;     /* Mixed Boolean-Arithmetic + Dynamic Constant Encryption */
    bool enable_str_enc; /* Compile-time String Encryption + Runtime Decryption */
    bool enable_vm_virt; /* Polymorphic Self-Checksumming Bytecode VM Virtualization */
    uint64_t seed;       /* Polymorphic PRNG seed */
} ObfuscationConfig;

IRProgram *generate_ir(const ProgramAST *ast);
void obfuscate_ir(IRProgram *ir, const ObfuscationConfig *cfg);
void dump_ir(const IRProgram *ir, FILE *out);
int run_ir_vm(const IRProgram *ir, int argc, char **argv);

/* ============================================================================
 * Native Code Generators
 * ============================================================================ */

void emit_arm64_asm(const IRProgram *ir, const TargetInfo *target, FILE *out);
void emit_x86_64_asm(const IRProgram *ir, const TargetInfo *target, FILE *out);

#endif /* XCC_H */
