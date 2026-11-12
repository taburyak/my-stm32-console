#include "script_engine.h"
#include "console_sdk.h"

/* ==========================================================================
 * Embedded C-Script Interpreter (PicoC-style subset)
 *
 * Runs entirely in unprivileged Thread Mode inside SandboxTask:
 *   - Interpreter code resides in MCU Flash (0 WS, read-only for all).
 *   - Source code, token stream, symbol tables, call stack, and script arrays
 *     reside in the 512 KB External SRAM App Sandbox (0x68080000..0x680FFFFF).
 *   - All hardware interaction goes through standard SDK SVC system calls.
 * ========================================================================== */

#define MAX_TOKENS        2048U
#define MAX_IDENTS        224U
#define MAX_IDENT_LEN     20U
#define MAX_GLOBALS       128U
#define MAX_LOCALS        64U
#define MAX_FUNCS         32U
#define MAX_PARAMS        8U
#define MAX_CALL_DEPTH    24U
#define MAX_LOOP_DEPTH    16U
#define MAX_STR_POOL      2048U

typedef enum {
    TOK_EOF = 0,
    TOK_NUM,
    TOK_STR,
    TOK_IDENT,
    /* Keywords */
    TOK_KW_INT,
    TOK_KW_UINT,
    TOK_KW_CHAR,
    TOK_KW_VOID,
    TOK_KW_BOOL,
    TOK_KW_CONST,
    TOK_KW_STATIC,
    TOK_KW_IF,
    TOK_KW_ELSE,
    TOK_KW_WHILE,
    TOK_KW_FOR,
    TOK_KW_DO,
    TOK_KW_RETURN,
    TOK_KW_BREAK,
    TOK_KW_CONTINUE,
    /* Punctuation & Operators */
    TOK_LPAREN,    /* ( */
    TOK_RPAREN,    /* ) */
    TOK_LBRACE,    /* { */
    TOK_RBRACE,    /* } */
    TOK_LBRACK,    /* [ */
    TOK_RBRACK,    /* ] */
    TOK_SEMI,      /* ; */
    TOK_COMMA,     /* , */
    TOK_QUESTION,  /* ? */
    TOK_COLON,     /* : */
    TOK_TILDE,     /* ~ */
    TOK_BANG,      /* ! */
    TOK_PLUS,      /* + */
    TOK_MINUS,     /* - */
    TOK_STAR,      /* * */
    TOK_SLASH,     /* / */
    TOK_PERCENT,   /* % */
    TOK_AMP,       /* & */
    TOK_PIPE,      /* | */
    TOK_CARET,     /* ^ */
    TOK_LSHIFT,    /* << */
    TOK_RSHIFT,    /* >> */
    TOK_EQ,        /* == */
    TOK_NE,        /* != */
    TOK_LT,        /* < */
    TOK_LE,        /* <= */
    TOK_GT,        /* > */
    TOK_GE,        /* >= */
    TOK_LAND,      /* && */
    TOK_LOR,       /* || */
    TOK_INC,       /* ++ */
    TOK_DEC,       /* -- */
    TOK_ASSIGN,    /* = */
    TOK_ADD_ASSIGN,/* += */
    TOK_SUB_ASSIGN,/* -= */
    TOK_MUL_ASSIGN,/* *= */
    TOK_DIV_ASSIGN,/* /= */
    TOK_MOD_ASSIGN,/* %= */
    TOK_AND_ASSIGN,/* &= */
    TOK_OR_ASSIGN, /* |= */
    TOK_XOR_ASSIGN,/* ^= */
    TOK_SHL_ASSIGN,/* <<= */
    TOK_SHR_ASSIGN /* >>= */
} TokenType_t;

typedef struct {
    uint8_t  type;
    uint8_t  flags;
    uint16_t line;
    int32_t  val;   /* Literal value, string offset, or identifier index */
} Token_t;

typedef struct {
    uint16_t name_id;
    uint8_t  is_array;
    uint8_t  elem_bytes; /* 1, 2, or 4 */
    int32_t  val;        /* Scalar value or pointer into array arena */
    uint32_t array_len;
} VarSlot_t;

typedef struct {
    uint16_t name_id;
    uint8_t  param_count;
    uint16_t param_ids[MAX_PARAMS];
    uint16_t body_tok;   /* Index of '{' token */
    uint16_t end_tok;    /* Index after matching '}' token */
} FuncDef_t;

typedef enum {
    FLOW_NORMAL = 0,
    FLOW_RETURN,
    FLOW_BREAK,
    FLOW_CONTINUE,
    FLOW_EXIT,
    FLOW_ERROR
} FlowState_t;

typedef struct {
    /* Tokenizer & symbol tables (ALL in 0-WS Internal SRAM sandbox 0x20018000!) */
    Token_t   tokens[MAX_TOKENS];
    uint32_t  num_tokens;
    char      idents[MAX_IDENTS][MAX_IDENT_LEN];
    uint16_t  num_idents;
    char      str_pool[MAX_STR_POOL];
    uint32_t  str_pool_used;

    /* Variables & functions (in 0-WS Internal SRAM sandbox 0x20018000!) */
    VarSlot_t globals[MAX_GLOBALS];
    uint16_t  num_globals;
    int16_t   global_ById[MAX_IDENTS]; /* O(1) lookup: ident_id -> globals[] index, or -1 */
    VarSlot_t locals[MAX_LOCALS];
    uint16_t  num_locals;
    uint16_t  frame_base[MAX_CALL_DEPTH];
    uint8_t   call_depth;
    FuncDef_t funcs[MAX_FUNCS];
    uint16_t  num_funcs;
    int16_t   func_ById[MAX_IDENTS];   /* O(1) lookup: ident_id -> funcs[] index, or -1 */

    /* Array bump arena inside 0x680D0000..0x680FFFFF */
    uint32_t  array_used;

    /* Execution cursor & control flow */
    uint32_t    pc;
    FlowState_t flow;
    int32_t     ret_val;
    uint16_t    err_line;
    char        err_msg[64];

    /* Persistent SDK_Input_t helper for scripts */
    SDK_Input_t sdk_in;
    bool        sdk_in_ready;
    uint32_t    frame_next_tick;
    char        fmt_buf[96];
} ScriptVM_t;

_Static_assert(sizeof(ScriptVM_t) <= SCRIPT_VM_INTERNAL_MAX, "ScriptVM_t exceeds 28 KB internal sandbox");

#define VM     ((ScriptVM_t *)SCRIPT_VM_INTERNAL_ADDR)
#define TOKENS (VM->tokens)

/* ==========================================================================
 * String & Character Utilities (freestanding, no libc dependencies)
 * ========================================================================== */

static bool sc_is_space(char c)
{
    return (c == ' ' || c == '\t' || c == '\r' || c == '\n');
}

static bool sc_is_alpha(char c)
{
    return ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_');
}

static bool sc_is_digit(char c)
{
    return (c >= '0' && c <= '9');
}

static bool sc_streq(const char *a, const char *b)
{
    while (*a && *b)
    {
        if (*a != *b) return false;
        a++;
        b++;
    }
    return (*a == *b);
}

static void sc_strcpy_max(char *dst, const char *src, uint32_t max_len)
{
    if (max_len == 0U) return;
    uint32_t i = 0;
    while (i + 1U < max_len && src[i] != '\0')
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void sc_error(uint16_t line, const char *msg)
{
    if (VM->flow == FLOW_ERROR) return;
    VM->flow     = FLOW_ERROR;
    VM->err_line = line;
    sc_strcpy_max(VM->err_msg, msg, sizeof(VM->err_msg));
}

/* ==========================================================================
 * Built-in Constants Table (Colors, Keys, Screen dimensions, Audio flags)
 * ========================================================================== */

typedef struct {
    const char *name;
    int32_t     value;
} BuiltinConst_t;

static const BuiltinConst_t s_builtin_consts[] = {
    { "TRUE",              1 },
    { "true",              1 },
    { "FALSE",             0 },
    { "false",             0 },
    { "NULL",              0 },
    { "SCREEN_W",          (int32_t)SDK_SCREEN_W },
    { "SCREEN_H",          (int32_t)SDK_SCREEN_H },
    { "SDK_SCREEN_W",      (int32_t)SDK_SCREEN_W },
    { "SDK_SCREEN_H",      (int32_t)SDK_SCREEN_H },
    /* Keys */
    { "KEY_LEFT",          (int32_t)KEY_LEFT },
    { "KEY_RIGHT",         (int32_t)KEY_RIGHT },
    { "KEY_UP",            (int32_t)KEY_UP },
    { "KEY_DOWN",          (int32_t)KEY_DOWN },
    { "KEY_A",             (int32_t)KEY_A },
    { "KEY_B",             (int32_t)KEY_B },
    { "KEY_X",             (int32_t)KEY_X },
    { "KEY_EXIT",          (int32_t)KEY_EXIT },
    { "KEY_START",         (int32_t)KEY_START },
    /* Gamepad buttons */
    { "SDK_PAD_UP",        (int32_t)SDK_PAD_UP },
    { "SDK_PAD_DOWN",      (int32_t)SDK_PAD_DOWN },
    { "SDK_PAD_LEFT",      (int32_t)SDK_PAD_LEFT },
    { "SDK_PAD_RIGHT",     (int32_t)SDK_PAD_RIGHT },
    { "SDK_PAD_A",         (int32_t)SDK_PAD_A },
    { "SDK_PAD_B",         (int32_t)SDK_PAD_B },
    { "SDK_PAD_X",         (int32_t)SDK_PAD_X },
    { "SDK_PAD_Y",         (int32_t)SDK_PAD_Y },
    { "SDK_PAD_L1",        (int32_t)SDK_PAD_L1 },
    { "SDK_PAD_R1",        (int32_t)SDK_PAD_R1 },
    { "SDK_PAD_L2",        (int32_t)SDK_PAD_L2 },
    { "SDK_PAD_R2",        (int32_t)SDK_PAD_R2 },
    { "SDK_PAD_START",     (int32_t)SDK_PAD_START },
    { "SDK_PAD_SELECT",    (int32_t)SDK_PAD_SELECT },
    { "SDK_PAD_L3",        (int32_t)SDK_PAD_L3 },
    { "SDK_PAD_R3",        (int32_t)SDK_PAD_R3 },
    /* Palette */
    { "COLOR_BLACK",       (int32_t)COLOR_BLACK },
    { "COLOR_WHITE",       (int32_t)COLOR_WHITE },
    { "COLOR_CYAN",        (int32_t)COLOR_CYAN },
    { "COLOR_NEON_GREEN",  (int32_t)COLOR_NEON_GREEN },
    { "COLOR_GREEN",       (int32_t)COLOR_NEON_GREEN },
    { "COLOR_AMBER",       (int32_t)COLOR_AMBER },
    { "COLOR_ORANGE",      (int32_t)COLOR_AMBER },
    { "COLOR_RED",         (int32_t)COLOR_RED },
    { "COLOR_BLUE",        (int32_t)COLOR_BLUE },
    { "COLOR_MAGENTA",     (int32_t)COLOR_MAGENTA },
    { "COLOR_YELLOW",      (int32_t)COLOR_YELLOW },
    { "COLOR_DARK_BG",     (int32_t)COLOR_DARK_BG },
    { "COLOR_PANEL_BG",    (int32_t)COLOR_PANEL_BG },
    { "COLOR_GRID_LINE",   (int32_t)COLOR_GRID_LINE },
    /* File flags */
    { "SDK_FILE_READ",     (int32_t)SDK_FILE_READ },
    { "SDK_FILE_WRITE",    (int32_t)SDK_FILE_WRITE },
    { "SDK_FILE_CREATE",   (int32_t)SDK_FILE_CREATE },
    { "SDK_FILE_TRUNC",    (int32_t)SDK_FILE_TRUNC },
    { "SDK_FILE_APPEND",   (int32_t)SDK_FILE_APPEND },
    { "SDK_TEMP_INVALID",  (int32_t)SDK_TEMP_INVALID }
};

static bool sc_lookup_const(const char *name, int32_t *out_val)
{
    for (uint32_t i = 0; i < sizeof(s_builtin_consts) / sizeof(s_builtin_consts[0]); i++)
    {
        if (sc_streq(name, s_builtin_consts[i].name))
        {
            *out_val = s_builtin_consts[i].value;
            return true;
        }
    }
    return false;
}

static int32_t sc_intern_ident(const char *name, uint16_t line)
{
    for (uint16_t i = 0; i < VM->num_idents; i++)
    {
        if (sc_streq(VM->idents[i], name))
        {
            return (int32_t)i;
        }
    }
    if (VM->num_idents >= MAX_IDENTS)
    {
        sc_error(line, "Too many identifiers");
        return 0;
    }
    uint16_t id = VM->num_idents++;
    sc_strcpy_max(VM->idents[id], name, MAX_IDENT_LEN);
    return (int32_t)id;
}

static bool sc_push_token(TokenType_t type, int32_t val, uint16_t line)
{
    if (VM->num_tokens >= MAX_TOKENS)
    {
        sc_error(line, "Script too large (max tokens)");
        return false;
    }
    Token_t *t = &TOKENS[VM->num_tokens++];
    t->type  = (uint8_t)type;
    t->flags = 0U;
    t->line  = line;
    t->val   = val;
    return true;
}

/* ==========================================================================
 * Lexer (Tokenizer + #define constant folding)
 * ========================================================================== */

static bool sc_tokenize(const char *src)
{
    uint32_t i = 0;
    uint16_t line = 1;

    while (src[i] != '\0' && VM->flow == FLOW_NORMAL)
    {
        char c = src[i];

        if (c == '\n')
        {
            line++;
            i++;
            continue;
        }
        if (sc_is_space(c))
        {
            i++;
            continue;
        }

        /* Single-line comment // */
        if (c == '/' && src[i + 1U] == '/')
        {
            i += 2U;
            while (src[i] != '\0' && src[i] != '\n') i++;
            continue;
        }

        /* Block comment */
        if (c == '/' && src[i + 1U] == '*')
        {
            i += 2U;
            while (src[i] != '\0' && !(src[i] == '*' && src[i + 1U] == '/'))
            {
                if (src[i] == '\n') line++;
                i++;
            }
            if (src[i] != '\0') i += 2U;
            continue;
        }

        /* Preprocessor directive (#include ignored, #define NAME VALUE stored as global const) */
        if (c == '#')
        {
            i++;
            while (src[i] == ' ' || src[i] == '\t') i++;
            char dir[16];
            uint32_t d = 0;
            while (sc_is_alpha(src[i]) && d + 1U < sizeof(dir))
            {
                dir[d++] = src[i++];
            }
            dir[d] = '\0';

            if (sc_streq(dir, "define"))
            {
                while (src[i] == ' ' || src[i] == '\t') i++;
                char mname[MAX_IDENT_LEN];
                uint32_t m = 0;
                while ((sc_is_alpha(src[i]) || sc_is_digit(src[i])) && m + 1U < sizeof(mname))
                {
                    mname[m++] = src[i++];
                }
                mname[m] = '\0';

                while (src[i] == ' ' || src[i] == '\t' || src[i] == '(') i++;
                bool neg = false;
                if (src[i] == '-') { neg = true; i++; }
                else if (src[i] == '+') { i++; }

                int32_t mval = 0;
                if (src[i] == '0' && (src[i + 1U] == 'x' || src[i + 1U] == 'X'))
                {
                    i += 2U;
                    while (1)
                    {
                        char hc = src[i];
                        if (hc >= '0' && hc <= '9')      mval = (mval << 4) | (hc - '0');
                        else if (hc >= 'a' && hc <= 'f') mval = (mval << 4) | (hc - 'a' + 10);
                        else if (hc >= 'A' && hc <= 'F') mval = (mval << 4) | (hc - 'A' + 10);
                        else break;
                        i++;
                    }
                }
                else
                {
                    while (sc_is_digit(src[i]))
                    {
                        mval = mval * 10 + (src[i] - '0');
                        i++;
                    }
                }
                if (neg) mval = -mval;

                if ( m > 0U && VM->num_globals < MAX_GLOBALS)
                {
                    int32_t id = sc_intern_ident(mname, line);
                    uint16_t gidx = VM->num_globals++;
                    if ((uint16_t)id < MAX_IDENTS)
                    {
                        VM->global_ById[(uint16_t)id] = (int16_t)gidx;
                    }
                    VarSlot_t *g = &VM->globals[gidx];
                    g->name_id    = (uint16_t)id;
                    g->is_array   = 0U;
                    g->elem_bytes = 4U;
                    g->val        = mval;
                    g->array_len  = 0U;
                }
            }

            while (src[i] != '\0' && src[i] != '\n') i++;
            continue;
        }

        /* Numeric literal (decimal, hex 0x..., binary 0b...) */
        if (sc_is_digit(c))
        {
            uint32_t uval = 0;
            if (c == '0' && (src[i + 1U] == 'x' || src[i + 1U] == 'X'))
            {
                i += 2U;
                while (1)
                {
                    char hc = src[i];
                    if (hc >= '0' && hc <= '9')      uval = (uval << 4) | (uint32_t)(hc - '0');
                    else if (hc >= 'a' && hc <= 'f') uval = (uval << 4) | (uint32_t)(hc - 'a' + 10);
                    else if (hc >= 'A' && hc <= 'F') uval = (uval << 4) | (uint32_t)(hc - 'A' + 10);
                    else break;
                    i++;
                }
            }
            else if (c == '0' && (src[i + 1U] == 'b' || src[i + 1U] == 'B'))
            {
                i += 2U;
                while (src[i] == '0' || src[i] == '1')
                {
                    uval = (uval << 1) | (uint32_t)(src[i] - '0');
                    i++;
                }
            }
            else
            {
                while (sc_is_digit(src[i]))
                {
                    uval = uval * 10U + (uint32_t)(src[i] - '0');
                    i++;
                }
            }
            /* Skip integer suffixes U, L, UL */
            while (src[i] == 'u' || src[i] == 'U' || src[i] == 'l' || src[i] == 'L') i++;
            sc_push_token(TOK_NUM, (int32_t)uval, line);
            continue;
        }

        /* Character literal 'a' */
        if (c == '\'')
        {
            i++;
            char ch = src[i++];
            if (ch == '\\')
            {
                char esc = src[i++];
                if (esc == 'n') ch = '\n';
                else if (esc == 'r') ch = '\r';
                else if (esc == 't') ch = '\t';
                else if (esc == '0') ch = '\0';
                else ch = esc;
            }
            if (src[i] == '\'') i++;
            sc_push_token(TOK_NUM, (int32_t)(uint8_t)ch, line);
            continue;
        }

        /* String literal "..." */
        if (c == '"')
        {
            i++;
            uint32_t start_off = VM->str_pool_used;
            while (src[i] != '\0' && src[i] != '"')
            {
                char ch = src[i++];
                if (ch == '\\' && src[i] != '\0')
                {
                    char esc = src[i++];
                    if (esc == 'n') ch = '\n';
                    else if (esc == 'r') ch = '\r';
                    else if (esc == 't') ch = '\t';
                    else ch = esc;
                }
                if (VM->str_pool_used + 2U >= MAX_STR_POOL)
                {
                    sc_error(line, "String pool overflow");
                    return false;
                }
                VM->str_pool[VM->str_pool_used++] = ch;
            }
            if (src[i] == '"') i++;
            VM->str_pool[VM->str_pool_used++] = '\0';
            sc_push_token(TOK_STR, (int32_t)&VM->str_pool[start_off], line);
            continue;
        }

        /* Identifier or keyword */
        if (sc_is_alpha(c))
        {
            char ident[MAX_IDENT_LEN];
            uint32_t k = 0;
            while (sc_is_alpha(src[i]) || sc_is_digit(src[i]))
            {
                if (k + 1U < sizeof(ident))
                {
                    ident[k++] = src[i];
                }
                i++;
            }
            ident[k] = '\0';

            int32_t cval = 0;
            if (sc_lookup_const(ident, &cval))
            {
                sc_push_token(TOK_NUM, cval, line);
            }
            else if (sc_streq(ident, "int") || sc_streq(ident, "int32_t") || sc_streq(ident, "int16_t") || sc_streq(ident, "int8_t"))
                sc_push_token(TOK_KW_INT, 0, line);
            else if (sc_streq(ident, "uint32_t") || sc_streq(ident, "uint16_t") || sc_streq(ident, "uint8_t") || sc_streq(ident, "unsigned"))
                sc_push_token(TOK_KW_UINT, 0, line);
            else if (sc_streq(ident, "char"))
                sc_push_token(TOK_KW_CHAR, 0, line);
            else if (sc_streq(ident, "void"))
                sc_push_token(TOK_KW_VOID, 0, line);
            else if (sc_streq(ident, "bool"))
                sc_push_token(TOK_KW_BOOL, 0, line);
            else if (sc_streq(ident, "const"))
                sc_push_token(TOK_KW_CONST, 0, line);
            else if (sc_streq(ident, "static"))
                sc_push_token(TOK_KW_STATIC, 0, line);
            else if (sc_streq(ident, "if"))
                sc_push_token(TOK_KW_IF, 0, line);
            else if (sc_streq(ident, "else"))
                sc_push_token(TOK_KW_ELSE, 0, line);
            else if (sc_streq(ident, "while"))
                sc_push_token(TOK_KW_WHILE, 0, line);
            else if (sc_streq(ident, "for"))
                sc_push_token(TOK_KW_FOR, 0, line);
            else if (sc_streq(ident, "do"))
                sc_push_token(TOK_KW_DO, 0, line);
            else if (sc_streq(ident, "return"))
                sc_push_token(TOK_KW_RETURN, 0, line);
            else if (sc_streq(ident, "break"))
                sc_push_token(TOK_KW_BREAK, 0, line);
            else if (sc_streq(ident, "continue"))
                sc_push_token(TOK_KW_CONTINUE, 0, line);
            else
            {
                int32_t id = sc_intern_ident(ident, line);
                sc_push_token(TOK_IDENT, id, line);
            }
            continue;
        }

        /* Multi-character operators */
        char n1 = src[i + 1U];
        char n2 = (n1 != '\0') ? src[i + 2U] : '\0';

        if (c == '<' && n1 == '<' && n2 == '=') { sc_push_token(TOK_SHL_ASSIGN, 0, line); i += 3U; continue; }
        if (c == '>' && n1 == '>' && n2 == '=') { sc_push_token(TOK_SHR_ASSIGN, 0, line); i += 3U; continue; }
        if (c == '<' && n1 == '<') { sc_push_token(TOK_LSHIFT, 0, line); i += 2U; continue; }
        if (c == '>' && n1 == '>') { sc_push_token(TOK_RSHIFT, 0, line); i += 2U; continue; }
        if (c == '=' && n1 == '=') { sc_push_token(TOK_EQ, 0, line);     i += 2U; continue; }
        if (c == '!' && n1 == '=') { sc_push_token(TOK_NE, 0, line);     i += 2U; continue; }
        if (c == '<' && n1 == '=') { sc_push_token(TOK_LE, 0, line);     i += 2U; continue; }
        if (c == '>' && n1 == '=') { sc_push_token(TOK_GE, 0, line);     i += 2U; continue; }
        if (c == '&' && n1 == '&') { sc_push_token(TOK_LAND, 0, line);   i += 2U; continue; }
        if (c == '|' && n1 == '|') { sc_push_token(TOK_LOR, 0, line);    i += 2U; continue; }
        if (c == '+' && n1 == '+') { sc_push_token(TOK_INC, 0, line);    i += 2U; continue; }
        if (c == '-' && n1 == '-') { sc_push_token(TOK_DEC, 0, line);    i += 2U; continue; }
        if (c == '+' && n1 == '=') { sc_push_token(TOK_ADD_ASSIGN, 0, line); i += 2U; continue; }
        if (c == '-' && n1 == '=') { sc_push_token(TOK_SUB_ASSIGN, 0, line); i += 2U; continue; }
        if (c == '*' && n1 == '=') { sc_push_token(TOK_MUL_ASSIGN, 0, line); i += 2U; continue; }
        if (c == '/' && n1 == '=') { sc_push_token(TOK_DIV_ASSIGN, 0, line); i += 2U; continue; }
        if (c == '%' && n1 == '=') { sc_push_token(TOK_MOD_ASSIGN, 0, line); i += 2U; continue; }
        if (c == '&' && n1 == '=') { sc_push_token(TOK_AND_ASSIGN, 0, line); i += 2U; continue; }
        if (c == '|' && n1 == '=') { sc_push_token(TOK_OR_ASSIGN, 0, line);  i += 2U; continue; }
        if (c == '^' && n1 == '=') { sc_push_token(TOK_XOR_ASSIGN, 0, line); i += 2U; continue; }

        /* Single-character tokens */
        TokenType_t single = TOK_EOF;
        switch (c)
        {
            case '(': single = TOK_LPAREN;   break;
            case ')': single = TOK_RPAREN;   break;
            case '{': single = TOK_LBRACE;   break;
            case '}': single = TOK_RBRACE;   break;
            case '[': single = TOK_LBRACK;   break;
            case ']': single = TOK_RBRACK;   break;
            case ';': single = TOK_SEMI;     break;
            case ',': single = TOK_COMMA;    break;
            case '?': single = TOK_QUESTION; break;
            case ':': single = TOK_COLON;    break;
            case '~': single = TOK_TILDE;    break;
            case '!': single = TOK_BANG;     break;
            case '+': single = TOK_PLUS;     break;
            case '-': single = TOK_MINUS;    break;
            case '*': single = TOK_STAR;     break;
            case '/': single = TOK_SLASH;    break;
            case '%': single = TOK_PERCENT;  break;
            case '&': single = TOK_AMP;      break;
            case '|': single = TOK_PIPE;     break;
            case '^': single = TOK_CARET;    break;
            case '<': single = TOK_LT;       break;
            case '>': single = TOK_GT;       break;
            case '=': single = TOK_ASSIGN;   break;
            default:
                sc_error(line, "Unexpected character");
                return false;
        }
        sc_push_token(single, 0, line);
        i++;
    }

    sc_push_token(TOK_EOF, 0, line);
    return (VM->flow == FLOW_NORMAL);
}

/* ==========================================================================
 * Parser Helpers & Variable Lookup
 * ========================================================================== */

static Token_t *sc_cur(void)
{
    uint32_t idx = (VM->pc < MAX_TOKENS) ? VM->pc : (MAX_TOKENS - 1U);
    return &TOKENS[idx];
}

static TokenType_t sc_peek(void)
{
    uint32_t idx = (VM->pc < MAX_TOKENS) ? VM->pc : (MAX_TOKENS - 1U);
    return (TokenType_t)TOKENS[idx].type;
}

static bool sc_match(TokenType_t t)
{
    if (sc_peek() == t)
    {
        VM->pc++;
        return true;
    }
    return false;
}

static bool sc_expect(TokenType_t t, const char *err)
{
    if (sc_peek() == t)
    {
        VM->pc++;
        return true;
    }
    sc_error(sc_cur()->line, err);
    return false;
}

static bool sc_is_type_token(TokenType_t t)
{
    return (t == TOK_KW_INT || t == TOK_KW_UINT || t == TOK_KW_CHAR ||
            t == TOK_KW_VOID || t == TOK_KW_BOOL || t == TOK_KW_CONST || t == TOK_KW_STATIC);
}

static void sc_consume_type_specifiers(uint8_t *out_elem_bytes)
{
    uint8_t bytes = 4U;
    while (sc_is_type_token(sc_peek()))
    {
        if (sc_peek() == TOK_KW_CHAR || sc_peek() == TOK_KW_BOOL)
        {
            bytes = 1U;
        }
        VM->pc++;
    }
    /* Optional pointer '*' */
    while (sc_match(TOK_STAR))
    {
        bytes = 4U;
    }
    if (out_elem_bytes != NULL)
    {
        *out_elem_bytes = bytes;
    }
}

static VarSlot_t *sc_find_var(uint16_t name_id)
{
    uint16_t base = (VM->call_depth > 0U && VM->call_depth <= MAX_CALL_DEPTH)
                        ? VM->frame_base[VM->call_depth - 1U]
                        : 0U;
    uint16_t nloc = (VM->num_locals <= MAX_LOCALS) ? VM->num_locals : MAX_LOCALS;
    if (base > nloc) base = nloc;

    for (int32_t i = (int32_t)nloc - 1; i >= (int32_t)base; i--)
    {
        if (VM->locals[i].name_id == name_id)
        {
            return &VM->locals[i];
        }
    }

    if (name_id < MAX_IDENTS)
    {
        int16_t gidx = VM->global_ById[name_id];
        if (gidx >= 0 && (uint16_t)gidx < VM->num_globals)
        {
            return &VM->globals[gidx];
        }
    }
    return NULL;
}

static VarSlot_t *sc_add_var(uint16_t name_id, bool is_global, uint8_t elem_bytes, uint16_t line)
{
    if (is_global)
    {
        if (name_id < MAX_IDENTS && VM->global_ById[name_id] >= 0)
        {
            return &VM->globals[VM->global_ById[name_id]];
        }
        if (VM->num_globals >= MAX_GLOBALS)
        {
            sc_error(line, "Too many global variables");
            return NULL;
        }
        uint16_t gidx = VM->num_globals++;
        if (name_id < MAX_IDENTS)
        {
            VM->global_ById[name_id] = (int16_t)gidx;
        }
        VarSlot_t *v = &VM->globals[gidx];
        v->name_id    = name_id;
        v->is_array   = 0U;
        v->elem_bytes = elem_bytes;
        v->val        = 0;
        v->array_len  = 0U;
        return v;
    }
    else
    {
        /* Reuse existing local variable slot within the current function frame */
        uint16_t base = (VM->call_depth > 0U && VM->call_depth <= MAX_CALL_DEPTH)
                            ? VM->frame_base[VM->call_depth - 1U]
                            : 0U;
        for (uint16_t i = base; i < VM->num_locals; i++)
        {
            if (VM->locals[i].name_id == name_id)
            {
                return &VM->locals[i];
            }
        }
        if (VM->num_locals >= MAX_LOCALS)
        {
            sc_error(line, "Local variable stack overflow");
            return NULL;
        }
        VarSlot_t *v = &VM->locals[VM->num_locals++];
        v->name_id    = name_id;
        v->is_array   = 0U;
        v->elem_bytes = elem_bytes;
        v->val        = 0;
        v->array_len  = 0U;
        return v;
    }
}

static void *sc_alloc_array(uint32_t count, uint8_t elem_bytes, uint16_t line)
{
    uint32_t bytes = (count * (uint32_t)elem_bytes + 3U) & ~3U;
    if (bytes == 0U || (VM->array_used + bytes) > SCRIPT_ARRAY_MAX_SIZE)
    {
        sc_error(line, "Out of array memory (192 KB max)");
        return NULL;
    }
    uint32_t *ptr = (uint32_t *)(SCRIPT_ARRAY_BASE_ADDR + VM->array_used);
    VM->array_used += bytes;
    for (uint32_t w = 0U; w < (bytes / 4U); w++)
    {
        ptr[w] = 0U;
    }
    return ptr;
}

static int32_t sc_array_get(const VarSlot_t *v, int32_t idx, uint16_t line)
{
    if (idx < 0 || (uint32_t)idx >= v->array_len)
    {
        sc_error(line, "Array index out of bounds");
        return 0;
    }
    uintptr_t base = (uintptr_t)(uint32_t)v->val;
    if (v->elem_bytes == 1U) return (int32_t)((const uint8_t *)base)[idx];
    if (v->elem_bytes == 2U) return (int32_t)((const int16_t *)base)[idx];
    return ((const int32_t *)base)[idx];
}

static void sc_array_set(VarSlot_t *v, int32_t idx, int32_t val, uint16_t line)
{
    if (idx < 0 || (uint32_t)idx >= v->array_len)
    {
        sc_error(line, "Array index out of bounds");
        return;
    }
    uintptr_t base = (uintptr_t)(uint32_t)v->val;
    if (v->elem_bytes == 1U)      ((uint8_t *)base)[idx] = (uint8_t)val;
    else if (v->elem_bytes == 2U) ((int16_t *)base)[idx] = (int16_t)val;
    else                          ((int32_t *)base)[idx] = val;
}

/* Forward declarations */
static int32_t sc_eval_expr(void);
static void    sc_exec_stmt(void);
static void    sc_exec_block(void);

/* Skip a balanced '{ ... }' block without executing */
static void sc_skip_brace_block(void)
{
    if (!sc_expect(TOK_LBRACE, "Expected '{'")) return;
    uint32_t depth = 1U;
    while (depth > 0U && sc_peek() != TOK_EOF)
    {
        TokenType_t t = sc_peek();
        if (t == TOK_LBRACE) depth++;
        else if (t == TOK_RBRACE) depth--;
        VM->pc++;
    }
}

/* Skip a statement (or '{ ... }' block) without executing */
static void sc_skip_stmt(void)
{
    if (sc_peek() == TOK_LBRACE)
    {
        sc_skip_brace_block();
        return;
    }
    if (sc_peek() == TOK_KW_IF)
    {
        VM->pc++;
        if (sc_match(TOK_LPAREN))
        {
            uint32_t d = 1U;
            while (d > 0U && sc_peek() != TOK_EOF)
            {
                if (sc_peek() == TOK_LPAREN) d++;
                else if (sc_peek() == TOK_RPAREN) d--;
                VM->pc++;
            }
        }
        sc_skip_stmt();
        if (sc_match(TOK_KW_ELSE))
        {
            sc_skip_stmt();
        }
        return;
    }
    if (sc_peek() == TOK_KW_WHILE || sc_peek() == TOK_KW_FOR)
    {
        VM->pc++;
        if (sc_match(TOK_LPAREN))
        {
            uint32_t d = 1U;
            while (d > 0U && sc_peek() != TOK_EOF)
            {
                if (sc_peek() == TOK_LPAREN) d++;
                else if (sc_peek() == TOK_RPAREN) d--;
                VM->pc++;
            }
        }
        sc_skip_stmt();
        return;
    }
    while (sc_peek() != TOK_SEMI && sc_peek() != TOK_RBRACE && sc_peek() != TOK_EOF)
    {
        VM->pc++;
    }
    sc_match(TOK_SEMI);
}

/* ==========================================================================
 * Built-in SDK API Functions Callable from C-Scripts
 * ========================================================================== */

static bool sc_call_builtin(const char *name, int32_t *args, uint8_t argc, uint16_t line, int32_t *out)
{
    *out = 0;

    if (sc_streq(name, "RGB565") || sc_streq(name, "rgb565"))
    {
        if (argc >= 3U) *out = (int32_t)RGB565(args[0], args[1], args[2]);
        return true;
    }
    if (sc_streq(name, "abs"))
    {
        if (argc >= 1U) *out = (args[0] < 0) ? -args[0] : args[0];
        return true;
    }
    if (sc_streq(name, "min"))
    {
        if (argc >= 2U) *out = (args[0] < args[1]) ? args[0] : args[1];
        return true;
    }
    if (sc_streq(name, "max"))
    {
        if (argc >= 2U) *out = (args[0] > args[1]) ? args[0] : args[1];
        return true;
    }
    if (sc_streq(name, "clamp"))
    {
        if (argc >= 3U)
        {
            int32_t v = args[0];
            if (v < args[1]) v = args[1];
            if (v > args[2]) v = args[2];
            *out = v;
        }
        return true;
    }
    if (sc_streq(name, "clear_screen") || sc_streq(name, "sys_clear_screen"))
    {
        uint16_t col = (argc >= 1U) ? (uint16_t)args[0] : COLOR_BLACK;
        sys_clear_screen(col);
        return true;
    }
    if (sc_streq(name, "fill_rect") || sc_streq(name, "sys_lcd_fill_rect"))
    {
        if (argc >= 5U)
        {
            sys_lcd_fill_rect((uint16_t)args[0], (uint16_t)args[1],
                              (uint16_t)args[2], (uint16_t)args[3], (uint16_t)args[4]);
        }
        return true;
    }
    if (sc_streq(name, "draw_pixel") || sc_streq(name, "sys_draw_pixel"))
    {
        if (argc >= 3U)
        {
            sys_draw_pixel((uint16_t)args[0], (uint16_t)args[1], (uint16_t)args[2]);
        }
        return true;
    }
    if (sc_streq(name, "draw_text") || sc_streq(name, "sys_draw_text"))
    {
        if (argc >= 4U)
        {
            const char *s = (const char *)(uintptr_t)(uint32_t)args[2];
            uint16_t fg   = (uint16_t)args[3];
            uint16_t bg   = (argc >= 5U) ? (uint16_t)args[4] : COLOR_BLACK;
            uint8_t scale = (argc >= 6U) ? (uint8_t)args[5]  : 1U;
            if (s != NULL)
            {
                sys_draw_text((uint16_t)args[0], (uint16_t)args[1], s, fg, bg, scale);
            }
        }
        return true;
    }
    if (sc_streq(name, "draw_num") || sc_streq(name, "sys_draw_num"))
    {
        if (argc >= 4U)
        {
            int32_t val   = args[2];
            uint16_t fg   = (uint16_t)args[3];
            uint16_t bg   = (argc >= 5U) ? (uint16_t)args[4] : COLOR_BLACK;
            uint8_t scale = (argc >= 6U) ? (uint8_t)args[5]  : 1U;
            uint8_t dig   = (argc >= 7U) ? (uint8_t)args[6]  : 1U;

            char *p = VM->fmt_buf;
            uint32_t mag = (val < 0) ? (uint32_t)(-val) : (uint32_t)val;
            if (val < 0) *p++ = '-';
            sdk_uint_to_str(mag, p, dig);
            sys_draw_text((uint16_t)args[0], (uint16_t)args[1], VM->fmt_buf, fg, bg, scale);
        }
        return true;
    }
    if (sc_streq(name, "get_keys") || sc_streq(name, "sys_get_keys"))
    {
        *out = (int32_t)sys_get_keys();
        return true;
    }
    if (sc_streq(name, "input_poll") || sc_streq(name, "sdk_input_poll"))
    {
        if (!VM->sdk_in_ready)
        {
            sdk_input_init(&VM->sdk_in);
            VM->sdk_in_ready = true;
        }
        uint32_t keys = sys_get_keys();
        sdk_input_update(&VM->sdk_in, keys, sys_get_tick());
        *out = (int32_t)keys;
        return true;
    }
    if (sc_streq(name, "input_pressed"))
    {
        *out = (int32_t)(VM->sdk_in.pressed & ((argc >= 1U) ? (uint32_t)args[0] : 0xFFFFFFFFUL));
        return true;
    }
    if (sc_streq(name, "input_held"))
    {
        *out = (int32_t)(VM->sdk_in.held & ((argc >= 1U) ? (uint32_t)args[0] : 0xFFFFFFFFUL));
        return true;
    }
    if (sc_streq(name, "input_repeat"))
    {
        *out = (int32_t)(VM->sdk_in.repeat & ((argc >= 1U) ? (uint32_t)args[0] : 0xFFFFFFFFUL));
        return true;
    }
    if (sc_streq(name, "pad_lx"))
    {
        SDK_GamepadState_t pad;
        *out = sys_get_gamepad(&pad) ? (int32_t)pad.left_x : 0;
        return true;
    }
    if (sc_streq(name, "pad_ly"))
    {
        SDK_GamepadState_t pad;
        *out = sys_get_gamepad(&pad) ? (int32_t)pad.left_y : 0;
        return true;
    }
    if (sc_streq(name, "pad_rx"))
    {
        SDK_GamepadState_t pad;
        *out = sys_get_gamepad(&pad) ? (int32_t)pad.right_x : 0;
        return true;
    }
    if (sc_streq(name, "pad_ry"))
    {
        SDK_GamepadState_t pad;
        *out = sys_get_gamepad(&pad) ? (int32_t)pad.right_y : 0;
        return true;
    }
    if (sc_streq(name, "pad_buttons"))
    {
        SDK_GamepadState_t pad;
        *out = sys_get_gamepad(&pad) ? (int32_t)pad.buttons : 0;
        return true;
    }
    if (sc_streq(name, "mouse_x"))
    {
        SDK_MouseState_t m;
        *out = sys_get_mouse(&m) ? (int32_t)m.x : -1;
        return true;
    }
    if (sc_streq(name, "mouse_y"))
    {
        SDK_MouseState_t m;
        *out = sys_get_mouse(&m) ? (int32_t)m.y : -1;
        return true;
    }
    if (sc_streq(name, "mouse_buttons"))
    {
        SDK_MouseState_t m;
        *out = sys_get_mouse(&m) ? (int32_t)m.buttons : 0;
        return true;
    }
    if (sc_streq(name, "get_tick") || sc_streq(name, "sys_get_tick"))
    {
        *out = (int32_t)sys_get_tick();
        return true;
    }
    if (sc_streq(name, "sleep_ms") || sc_streq(name, "delay_ms") ||
        sc_streq(name, "sys_sleep_ms") || sc_streq(name, "sys_delay_ms"))
    {
        if (argc >= 1U && args[0] > 0)
        {
            sys_delay_ms((uint32_t)args[0]);
        }
        return true;
    }
    if (sc_streq(name, "frame_sync") || sc_streq(name, "sdk_frame_sync"))
    {
        uint32_t period = (argc >= 1U && args[0] > 0) ? (uint32_t)args[0] : 16U;
        sdk_frame_sync(&VM->frame_next_tick, period);
        return true;
    }
    if (sc_streq(name, "beep") || sc_streq(name, "sys_beep"))
    {
        uint16_t ms = (argc >= 1U) ? (uint16_t)args[0] : 40U;
        sys_beep(ms);
        return true;
    }
    if (sc_streq(name, "led_toggle") || sc_streq(name, "sys_led_toggle"))
    {
        uint32_t id = (argc >= 1U) ? (uint32_t)args[0] : 1U;
        sys_led_toggle(id);
        return true;
    }
    if (sc_streq(name, "rand") || sc_streq(name, "random") || sc_streq(name, "sys_random"))
    {
        if (argc >= 1U && args[0] > 0)
        {
            *out = (int32_t)sys_random_range((uint32_t)args[0]);
        }
        else
        {
            *out = (int32_t)(sys_random() & 0x7FFFFFFFUL);
        }
        return true;
    }
    if (sc_streq(name, "rand_range") || sc_streq(name, "sys_random_range"))
    {
        if (argc == 1U && args[0] > 0)
        {
            *out = (int32_t)sys_random_range((uint32_t)args[0]);
        }
        else if (argc >= 2U && args[1] > args[0])
        {
            *out = args[0] + (int32_t)sys_random_range((uint32_t)(args[1] - args[0]));
        }
        return true;
    }
    if (sc_streq(name, "eeprom_read_u8"))
    {
        uint8_t b = 0U;
        if (argc >= 1U && sys_eeprom_read((uint32_t)args[0], &b, 1U) == 1)
        {
            *out = (int32_t)b;
        }
        else
        {
            *out = -1;
        }
        return true;
    }
    if (sc_streq(name, "eeprom_write_u8"))
    {
        if (argc >= 2U)
        {
            uint8_t b = (uint8_t)args[1];
            *out = sys_eeprom_write((uint32_t)args[0], &b, 1U);
        }
        return true;
    }
    if (sc_streq(name, "save_int"))
    {
        if (argc >= 2U)
        {
            const char *path = (const char *)(uintptr_t)(uint32_t)args[0];
            int32_t val = args[1];
            if (path != NULL)
            {
                int32_t fd = sys_file_open(path, SDK_FILE_WRITE | SDK_FILE_CREATE | SDK_FILE_TRUNC);
                if (fd >= 0)
                {
                    int32_t w = sys_file_write(fd, &val, sizeof(val));
                    (void)sys_file_close(fd);
                    *out = (w == (int32_t)sizeof(val)) ? 1 : 0;
                }
            }
        }
        return true;
    }
    if (sc_streq(name, "load_int"))
    {
        int32_t def_val = (argc >= 2U) ? args[1] : 0;
        *out = def_val;
        if (argc >= 1U)
        {
            const char *path = (const char *)(uintptr_t)(uint32_t)args[0];
            if (path != NULL)
            {
                int32_t fd = sys_file_open(path, SDK_FILE_READ);
                if (fd >= 0)
                {
                    int32_t val = 0;
                    int32_t r = sys_file_read(fd, &val, sizeof(val));
                    (void)sys_file_close(fd);
                    if (r == (int32_t)sizeof(val))
                    {
                        *out = val;
                    }
                }
            }
        }
        return true;
    }
    if (sc_streq(name, "file_open") || sc_streq(name, "sys_file_open"))
    {
        if (argc >= 2U)
        {
            const char *path = (const char *)(uintptr_t)(uint32_t)args[0];
            *out = (path != NULL) ? sys_file_open(path, (uint32_t)args[1]) : SDK_ERR_INVALID;
        }
        return true;
    }
    if (sc_streq(name, "file_read") || sc_streq(name, "sys_file_read"))
    {
        if (argc >= 3U)
        {
            void *buf = (void *)(uintptr_t)(uint32_t)args[1];
            *out = (buf != NULL) ? sys_file_read(args[0], buf, (uint32_t)args[2]) : SDK_ERR_INVALID;
        }
        return true;
    }
    if (sc_streq(name, "file_write") || sc_streq(name, "sys_file_write"))
    {
        if (argc >= 3U)
        {
            const void *buf = (const void *)(uintptr_t)(uint32_t)args[1];
            *out = (buf != NULL) ? sys_file_write(args[0], buf, (uint32_t)args[2]) : SDK_ERR_INVALID;
        }
        return true;
    }
    if (sc_streq(name, "file_seek") || sc_streq(name, "sys_file_seek"))
    {
        if (argc >= 2U)
        {
            *out = sys_file_seek(args[0], (uint32_t)args[1]);
        }
        return true;
    }
    if (sc_streq(name, "file_close") || sc_streq(name, "sys_file_close"))
    {
        if (argc >= 1U)
        {
            *out = sys_file_close(args[0]);
        }
        return true;
    }
    if (sc_streq(name, "file_size") || sc_streq(name, "sys_file_size"))
    {
        if (argc >= 1U)
        {
            *out = sys_file_size(args[0]);
        }
        return true;
    }
    if (sc_streq(name, "file_remove") || sc_streq(name, "sys_file_remove"))
    {
        if (argc >= 1U)
        {
            const char *path = (const char *)(uintptr_t)(uint32_t)args[0];
            *out = (path != NULL) ? sys_file_remove(path) : SDK_ERR_INVALID;
        }
        return true;
    }
    if (sc_streq(name, "play_tone") || sc_streq(name, "sys_play_tone"))
    {
        if (argc >= 2U)
        {
            sys_play_tone((uint16_t)args[0], (uint16_t)args[1]);
        }
        return true;
    }
    if (sc_streq(name, "temp_mcu") || sc_streq(name, "sys_get_mcu_temp_deci"))
    {
        *out = sys_get_mcu_temp_deci();
        return true;
    }
    if (sc_streq(name, "temp_ext") || sc_streq(name, "sys_get_ext_temp_deci"))
    {
        *out = sys_get_ext_temp_deci();
        return true;
    }
    if (sc_streq(name, "weather_temp") || sc_streq(name, "sys_get_weather_temp_deci"))
    {
        *out = sys_get_weather_temp_deci();
        return true;
    }
    if (sc_streq(name, "esp_online") || sc_streq(name, "sys_esp_is_online"))
    {
        *out = sys_esp_is_online() ? 1 : 0;
        return true;
    }
    if (sc_streq(name, "esp_hud_clear") || sc_streq(name, "sys_esp_hud_clear"))
    {
        *out = sys_esp_hud_clear();
        return true;
    }
    if (sc_streq(name, "esp_hud_text") || sc_streq(name, "sys_esp_hud_text"))
    {
        if (argc >= 2U)
        {
            const char *s = (const char *)(uintptr_t)(uint32_t)args[1];
            *out = (s != NULL) ? sys_esp_hud_text((uint8_t)args[0], s) : SDK_ERR_INVALID;
        }
        return true;
    }
    if (sc_streq(name, "esp_hud_num"))
    {
        if (argc >= 3U)
        {
            const char *label = (const char *)(uintptr_t)(uint32_t)args[1];
            int32_t val = args[2];
            char buf[52];
            uint32_t idx = 0U;
            if (label != NULL)
            {
                while (label[idx] != '\0' && idx < 36U)
                {
                    buf[idx] = label[idx];
                    idx++;
                }
            }
            if (val < 0)
            {
                buf[idx++] = '-';
            }
            uint32_t mag = (val < 0) ? (uint32_t)(-val) : (uint32_t)val;
            sdk_uint_to_str(mag, &buf[idx], 1U);
            *out = sys_esp_hud_text((uint8_t)args[0], buf);
        }
        return true;
    }
    if (sc_streq(name, "esp_led") || sc_streq(name, "sys_esp_set_led"))
    {
        if (argc >= 3U)
        {
            *out = sys_esp_set_led((uint8_t)args[0], (uint8_t)args[1], (uint8_t)args[2]);
        }
        return true;
    }
    if (sc_streq(name, "esp_hud_restore") || sc_streq(name, "sys_esp_hud_restore"))
    {
        *out = sys_esp_hud_restore();
        return true;
    }
    if (sc_streq(name, "exit") || sc_streq(name, "sys_exit") || sc_streq(name, "exit_app"))
    {
        VM->flow = FLOW_EXIT;
        sys_exit();
        return true;
    }

    (void)line;
    return false;
}

static int32_t sc_call_user_func(const FuncDef_t *fn, int32_t *args, uint8_t argc, uint16_t line)
{
    if (VM->call_depth >= MAX_CALL_DEPTH)
    {
        sc_error(line, "Call stack overflow");
        return 0;
    }

    uint16_t saved_locals = VM->num_locals;
    uint32_t saved_pc     = VM->pc;

    VM->frame_base[VM->call_depth++] = saved_locals;

    for (uint8_t i = 0; i < fn->param_count; i++)
    {
        VarSlot_t *p = sc_add_var(fn->param_ids[i], false, 4U, line);
        if (p == NULL) return 0;
        p->val = (i < argc) ? args[i] : 0;
    }

    VM->pc = fn->body_tok;
    sc_exec_block();

    int32_t result = 0;
    if (VM->flow == FLOW_RETURN)
    {
        result   = VM->ret_val;
        VM->flow = FLOW_NORMAL;
    }

    VM->call_depth--;
    VM->num_locals = saved_locals;
    VM->pc         = saved_pc;
    return result;
}

/* ==========================================================================
 * Recursive-Descent Expression Evaluator
 * ========================================================================== */

static int32_t sc_eval_assign(void);

static int32_t sc_eval_primary(void)
{
    Token_t *t = sc_cur();

    if (sc_match(TOK_NUM) || sc_match(TOK_STR))
    {
        return t->val;
    }

    if (sc_match(TOK_LPAREN))
    {
        /* Handle C type cast: (int)x, (uint16_t)y */
        if (sc_is_type_token(sc_peek()))
        {
            sc_consume_type_specifiers(NULL);
            sc_expect(TOK_RPAREN, "Expected ')' after cast");
            return sc_eval_primary();
        }
        int32_t v = sc_eval_expr();
        sc_expect(TOK_RPAREN, "Expected ')'");
        return v;
    }

    if (sc_match(TOK_IDENT))
    {
        uint16_t id   = (uint16_t)t->val;
        uint16_t line = t->line;

        /* Function call: ident(...) */
        if (sc_match(TOK_LPAREN))
        {
            int32_t args[MAX_PARAMS];
            uint8_t argc = 0U;
            if (sc_peek() != TOK_RPAREN)
            {
                do
                {
                    int32_t v = sc_eval_assign();
                    if (argc < MAX_PARAMS) args[argc++] = v;
                } while (sc_match(TOK_COMMA));
            }
            sc_expect(TOK_RPAREN, "Expected ')' after arguments");

            const char *fname = VM->idents[id];
            int32_t res = 0;
            if (sc_call_builtin(fname, args, argc, line, &res))
            {
                return res;
            }
            for (uint16_t f = 0; f < VM->num_funcs; f++)
            {
                if (VM->funcs[f].name_id == id)
                {
                    return sc_call_user_func(&VM->funcs[f], args, argc, line);
                }
            }
            sc_error(line, "Unknown function");
            return 0;
        }

        VarSlot_t *var = sc_find_var(id);
        if (var == NULL)
        {
            sc_error(line, "Undefined variable");
            return 0;
        }

        /* Array element: ident[index] (+ optional assignment / ++ / --) */
        if (sc_match(TOK_LBRACK))
        {
            int32_t idx = sc_eval_expr();
            sc_expect(TOK_RBRACK, "Expected ']'");
            if (!var->is_array)
            {
                sc_error(line, "Variable is not an array");
                return 0;
            }

            TokenType_t op = sc_peek();
            if (op >= TOK_ASSIGN && op <= TOK_SHR_ASSIGN)
            {
                VM->pc++;
                int32_t rhs = sc_eval_assign();
                int32_t cur = (op == TOK_ASSIGN) ? 0 : sc_array_get(var, idx, line);
                int32_t nv  = rhs;
                switch (op)
                {
                    case TOK_ADD_ASSIGN: nv = cur + rhs; break;
                    case TOK_SUB_ASSIGN: nv = cur - rhs; break;
                    case TOK_MUL_ASSIGN: nv = cur * rhs; break;
                    case TOK_DIV_ASSIGN: nv = (rhs != 0) ? (cur / rhs) : 0; break;
                    case TOK_MOD_ASSIGN: nv = (rhs != 0) ? (cur % rhs) : 0; break;
                    case TOK_AND_ASSIGN: nv = cur & rhs; break;
                    case TOK_OR_ASSIGN:  nv = cur | rhs; break;
                    case TOK_XOR_ASSIGN: nv = cur ^ rhs; break;
                    case TOK_SHL_ASSIGN: nv = cur << rhs; break;
                    case TOK_SHR_ASSIGN: nv = cur >> rhs; break;
                    default: break;
                }
                sc_array_set(var, idx, nv, line);
                return nv;
            }
            if (sc_match(TOK_INC))
            {
                int32_t cur = sc_array_get(var, idx, line);
                sc_array_set(var, idx, cur + 1, line);
                return cur;
            }
            if (sc_match(TOK_DEC))
            {
                int32_t cur = sc_array_get(var, idx, line);
                sc_array_set(var, idx, cur - 1, line);
                return cur;
            }
            return sc_array_get(var, idx, line);
        }

        /* Scalar variable (+ optional assignment / ++ / --) */
        TokenType_t op = sc_peek();
        if (op >= TOK_ASSIGN && op <= TOK_SHR_ASSIGN)
        {
            VM->pc++;
            int32_t rhs = sc_eval_assign();
            int32_t cur = var->val;
            switch (op)
            {
                case TOK_ASSIGN:     var->val = rhs; break;
                case TOK_ADD_ASSIGN: var->val = cur + rhs; break;
                case TOK_SUB_ASSIGN: var->val = cur - rhs; break;
                case TOK_MUL_ASSIGN: var->val = cur * rhs; break;
                case TOK_DIV_ASSIGN: var->val = (rhs != 0) ? (cur / rhs) : 0; break;
                case TOK_MOD_ASSIGN: var->val = (rhs != 0) ? (cur % rhs) : 0; break;
                case TOK_AND_ASSIGN: var->val = cur & rhs; break;
                case TOK_OR_ASSIGN:  var->val = cur | rhs; break;
                case TOK_XOR_ASSIGN: var->val = cur ^ rhs; break;
                case TOK_SHL_ASSIGN: var->val = cur << rhs; break;
                case TOK_SHR_ASSIGN: var->val = cur >> rhs; break;
                default: break;
            }
            return var->val;
        }
        if (sc_match(TOK_INC))
        {
            int32_t old = var->val;
            var->val = old + 1;
            return old;
        }
        if (sc_match(TOK_DEC))
        {
            int32_t old = var->val;
            var->val = old - 1;
            return old;
        }
        return var->val;
    }

    sc_error(t->line, "Syntax error in expression");
    return 0;
}

static int32_t sc_eval_unary(void)
{
    if (sc_match(TOK_PLUS))  return sc_eval_unary();
    if (sc_match(TOK_MINUS)) return -sc_eval_unary();
    if (sc_match(TOK_BANG))  return !sc_eval_unary();
    if (sc_match(TOK_TILDE)) return ~sc_eval_unary();
    if (sc_match(TOK_INC))
    {
        if (sc_peek() == TOK_IDENT)
        {
            VarSlot_t *v = sc_find_var((uint16_t)sc_cur()->val);
            VM->pc++;
            if (v != NULL) return ++(v->val);
        }
        return 0;
    }
    if (sc_match(TOK_DEC))
    {
        if (sc_peek() == TOK_IDENT)
        {
            VarSlot_t *v = sc_find_var((uint16_t)sc_cur()->val);
            VM->pc++;
            if (v != NULL) return --(v->val);
        }
        return 0;
    }
    return sc_eval_primary();
}

static int32_t sc_eval_mul(void)
{
    int32_t v = sc_eval_unary();
    for (;;)
    {
        if (sc_match(TOK_STAR))
        {
            v *= sc_eval_unary();
        }
        else if (sc_match(TOK_SLASH))
        {
            int32_t r = sc_eval_unary();
            v = (r != 0) ? (v / r) : 0;
        }
        else if (sc_match(TOK_PERCENT))
        {
            int32_t r = sc_eval_unary();
            v = (r != 0) ? (v % r) : 0;
        }
        else break;
    }
    return v;
}

static int32_t sc_eval_add(void)
{
    int32_t v = sc_eval_mul();
    for (;;)
    {
        if (sc_match(TOK_PLUS))       v += sc_eval_mul();
        else if (sc_match(TOK_MINUS)) v -= sc_eval_mul();
        else break;
    }
    return v;
}

static int32_t sc_eval_shift(void)
{
    int32_t v = sc_eval_add();
    for (;;)
    {
        if (sc_match(TOK_LSHIFT))      v <<= sc_eval_add();
        else if (sc_match(TOK_RSHIFT)) v >>= sc_eval_add();
        else break;
    }
    return v;
}

static int32_t sc_eval_rel(void)
{
    int32_t v = sc_eval_shift();
    for (;;)
    {
        if (sc_match(TOK_LT))      v = (v <  sc_eval_shift()) ? 1 : 0;
        else if (sc_match(TOK_LE)) v = (v <= sc_eval_shift()) ? 1 : 0;
        else if (sc_match(TOK_GT)) v = (v >  sc_eval_shift()) ? 1 : 0;
        else if (sc_match(TOK_GE)) v = (v >= sc_eval_shift()) ? 1 : 0;
        else break;
    }
    return v;
}

static int32_t sc_eval_eq(void)
{
    int32_t v = sc_eval_rel();
    for (;;)
    {
        if (sc_match(TOK_EQ))      v = (v == sc_eval_rel()) ? 1 : 0;
        else if (sc_match(TOK_NE)) v = (v != sc_eval_rel()) ? 1 : 0;
        else break;
    }
    return v;
}

static int32_t sc_eval_bitand(void)
{
    int32_t v = sc_eval_eq();
    while (sc_match(TOK_AMP)) v &= sc_eval_eq();
    return v;
}

static int32_t sc_eval_bitxor(void)
{
    int32_t v = sc_eval_bitand();
    while (sc_match(TOK_CARET)) v ^= sc_eval_bitand();
    return v;
}

static int32_t sc_eval_bitor(void)
{
    int32_t v = sc_eval_bitxor();
    while (sc_match(TOK_PIPE)) v |= sc_eval_bitxor();
    return v;
}

static int32_t sc_eval_land(void)
{
    int32_t v = sc_eval_bitor();
    while (sc_match(TOK_LAND))
    {
        int32_t r = sc_eval_bitor();
        v = (v != 0 && r != 0) ? 1 : 0;
    }
    return v;
}

static int32_t sc_eval_lor(void)
{
    int32_t v = sc_eval_land();
    while (sc_match(TOK_LOR))
    {
        int32_t r = sc_eval_land();
        v = (v != 0 || r != 0) ? 1 : 0;
    }
    return v;
}

static int32_t sc_eval_ternary(void)
{
    int32_t cond = sc_eval_lor();
    if (sc_match(TOK_QUESTION))
    {
        int32_t tval = sc_eval_expr();
        sc_expect(TOK_COLON, "Expected ':' in ternary");
        int32_t fval = sc_eval_ternary();
        return (cond != 0) ? tval : fval;
    }
    return cond;
}

static int32_t sc_eval_assign(void)
{
    return sc_eval_ternary();
}

static int32_t sc_eval_expr(void)
{
    int32_t v = sc_eval_assign();
    while (sc_match(TOK_COMMA))
    {
        v = sc_eval_assign();
    }
    return v;
}

/* ==========================================================================
 * Statement & Declaration Executor
 * ========================================================================== */

static void sc_parse_var_decl(bool is_global)
{
    uint8_t elem_bytes = 4U;
    sc_consume_type_specifiers(&elem_bytes);

    do
    {
        while (sc_match(TOK_STAR)) elem_bytes = 4U;

        Token_t *id_tok = sc_cur();
        if (!sc_expect(TOK_IDENT, "Expected variable name")) return;

        VarSlot_t *v = sc_add_var((uint16_t)id_tok->val, is_global, elem_bytes, id_tok->line);
        if (v == NULL) return;

        if (sc_match(TOK_LBRACK))
        {
            int32_t len = 0;
            if (sc_peek() != TOK_RBRACK)
            {
                len = sc_eval_expr();
            }
            sc_expect(TOK_RBRACK, "Expected ']'");
            if (len <= 0) len = 1;

            void *arr = sc_alloc_array((uint32_t)len, elem_bytes, id_tok->line);
            if (arr == NULL) return;
            v->is_array  = 1U;
            v->array_len = (uint32_t)len;
            v->val       = (int32_t)(uintptr_t)arr;

            if (sc_match(TOK_ASSIGN))
            {
                if (sc_match(TOK_LBRACE))
                {
                    int32_t idx = 0;
                    if (sc_peek() != TOK_RBRACE)
                    {
                        do
                        {
                            int32_t item = sc_eval_assign();
                            if (idx < len) sc_array_set(v, idx, item, id_tok->line);
                            idx++;
                        } while (sc_match(TOK_COMMA) && sc_peek() != TOK_RBRACE);
                    }
                    sc_expect(TOK_RBRACE, "Expected '}'");
                }
                else if (sc_peek() == TOK_STR)
                {
                    const char *s = (const char *)(uintptr_t)(uint32_t)sc_cur()->val;
                    VM->pc++;
                    for (int32_t k = 0; k < len && s[k] != '\0'; k++)
                    {
                        sc_array_set(v, k, (int32_t)(uint8_t)s[k], id_tok->line);
                    }
                }
            }
        }
        else if (sc_match(TOK_ASSIGN))
        {
            v->val = sc_eval_assign();
        }
    } while (sc_match(TOK_COMMA));
}

static void sc_exec_block(void)
{
    if (!sc_expect(TOK_LBRACE, "Expected '{'")) return;

    while (sc_peek() != TOK_RBRACE && sc_peek() != TOK_EOF && VM->flow == FLOW_NORMAL)
    {
        sc_exec_stmt();
    }

    if (VM->flow == FLOW_NORMAL)
    {
        sc_expect(TOK_RBRACE, "Expected '}'");
    }
}

static void sc_exec_stmt(void)
{
    if (VM->flow != FLOW_NORMAL) return;

    TokenType_t t = sc_peek();

    if (t == TOK_SEMI)
    {
        VM->pc++;
        return;
    }

    if (t == TOK_LBRACE)
    {
        sc_exec_block();
        return;
    }

    if (sc_is_type_token(t))
    {
        sc_parse_var_decl(false);
        sc_expect(TOK_SEMI, "Expected ';' after declaration");
        return;
    }

    if (sc_match(TOK_KW_IF))
    {
        sc_expect(TOK_LPAREN, "Expected '(' after 'if'");
        int32_t cond = sc_eval_expr();
        sc_expect(TOK_RPAREN, "Expected ')' after condition");

        if (cond != 0)
        {
            sc_exec_stmt();
            if (sc_match(TOK_KW_ELSE))
            {
                sc_skip_stmt();
            }
        }
        else
        {
            sc_skip_stmt();
            if (sc_match(TOK_KW_ELSE))
            {
                sc_exec_stmt();
            }
        }
        return;
    }

    if (sc_match(TOK_KW_WHILE))
    {
        sc_expect(TOK_LPAREN, "Expected '(' after 'while'");
        uint32_t cond_pc = VM->pc;

        for (;;)
        {
            VM->pc = cond_pc;
            int32_t cond = sc_eval_expr();
            sc_expect(TOK_RPAREN, "Expected ')'");
            if (cond == 0 || VM->flow != FLOW_NORMAL)
            {
                sc_skip_stmt();
                break;
            }
            uint32_t body_pc = VM->pc;
            sc_exec_stmt();

            if (VM->flow == FLOW_BREAK)
            {
                VM->flow = FLOW_NORMAL;
                VM->pc   = body_pc;
                sc_skip_stmt();
                break;
            }
            if (VM->flow == FLOW_CONTINUE)
            {
                VM->flow = FLOW_NORMAL;
            }
            if (VM->flow != FLOW_NORMAL)
            {
                break;
            }
        }
        return;
    }

    if (sc_match(TOK_KW_DO))
    {
        uint32_t body_pc = VM->pc;
        for (;;)
        {
            VM->pc = body_pc;
            sc_exec_stmt();

            if (VM->flow == FLOW_BREAK)
            {
                VM->flow = FLOW_NORMAL;
                VM->pc   = body_pc;
                sc_skip_stmt();
                if (sc_match(TOK_KW_WHILE))
                {
                    if (sc_match(TOK_LPAREN))
                    {
                        uint32_t d = 1U;
                        while (d > 0U && sc_peek() != TOK_EOF)
                        {
                            if (sc_peek() == TOK_LPAREN) d++;
                            else if (sc_peek() == TOK_RPAREN) d--;
                            VM->pc++;
                        }
                    }
                    sc_match(TOK_SEMI);
                }
                break;
            }
            if (VM->flow == FLOW_CONTINUE)
            {
                VM->flow = FLOW_NORMAL;
                VM->pc   = body_pc;
                sc_skip_stmt();
            }
            if (VM->flow != FLOW_NORMAL)
            {
                break;
            }

            if (!sc_expect(TOK_KW_WHILE, "Expected 'while' after 'do'")) break;
            sc_expect(TOK_LPAREN, "Expected '('");
            int32_t cond = sc_eval_expr();
            sc_expect(TOK_RPAREN, "Expected ')'");
            sc_expect(TOK_SEMI, "Expected ';'");
            if (cond == 0 || VM->flow != FLOW_NORMAL)
            {
                break;
            }
        }
        return;
    }

    if (sc_match(TOK_KW_FOR))
    {
        sc_expect(TOK_LPAREN, "Expected '(' after 'for'");

        /* Init clause */
        if (sc_peek() != TOK_SEMI)
        {
            if (sc_is_type_token(sc_peek())) sc_parse_var_decl(false);
            else                             (void)sc_eval_expr();
        }
        sc_expect(TOK_SEMI, "Expected ';' in 'for'");

        uint32_t cond_pc = VM->pc;
        while (sc_peek() != TOK_SEMI && sc_peek() != TOK_EOF) VM->pc++;
        sc_expect(TOK_SEMI, "Expected second ';' in 'for'");

        uint32_t step_pc = VM->pc;
        uint32_t d = 1U;
        while (d > 0U && sc_peek() != TOK_EOF)
        {
            if (sc_peek() == TOK_LPAREN) d++;
            else if (sc_peek() == TOK_RPAREN) d--;
            if (d > 0U) VM->pc++;
        }
        sc_expect(TOK_RPAREN, "Expected ')' in 'for'");
        uint32_t body_pc = VM->pc;

        for (;;)
        {
            VM->pc = cond_pc;
            int32_t cond = (sc_peek() == TOK_SEMI) ? 1 : sc_eval_expr();
            if (cond == 0 || VM->flow != FLOW_NORMAL)
            {
                VM->pc = body_pc;
                sc_skip_stmt();
                break;
            }

            VM->pc = body_pc;
            sc_exec_stmt();

            if (VM->flow == FLOW_BREAK)
            {
                VM->flow = FLOW_NORMAL;
                VM->pc   = body_pc;
                sc_skip_stmt();
                break;
            }
            if (VM->flow == FLOW_CONTINUE)
            {
                VM->flow = FLOW_NORMAL;
            }
            if (VM->flow != FLOW_NORMAL)
            {
                break;
            }

            VM->pc = step_pc;
            if (sc_peek() != TOK_RPAREN)
            {
                (void)sc_eval_expr();
            }
        }
        return;
    }

    if (sc_match(TOK_KW_RETURN))
    {
        int32_t v = 0;
        if (sc_peek() != TOK_SEMI)
        {
            v = sc_eval_expr();
        }
        sc_expect(TOK_SEMI, "Expected ';' after return");
        VM->ret_val = v;
        VM->flow    = FLOW_RETURN;
        return;
    }

    if (sc_match(TOK_KW_BREAK))
    {
        sc_expect(TOK_SEMI, "Expected ';' after break");
        VM->flow = FLOW_BREAK;
        return;
    }

    if (sc_match(TOK_KW_CONTINUE))
    {
        sc_expect(TOK_SEMI, "Expected ';' after continue");
        VM->flow = FLOW_CONTINUE;
        return;
    }

    (void)sc_eval_expr();
    sc_expect(TOK_SEMI, "Expected ';'");
}

/* ==========================================================================
 * Top-Level Program Scanner (Globals & Function Definitions)
 * ========================================================================== */

static bool sc_parse_program(void)
{
    VM->pc = 0U;
    while (sc_peek() != TOK_EOF && VM->flow == FLOW_NORMAL)
    {
        if (sc_is_type_token(sc_peek()))
        {
            uint32_t save_pc = VM->pc;
            uint8_t elem_bytes = 4U;
            sc_consume_type_specifiers(&elem_bytes);

            if (sc_peek() == TOK_IDENT && TOKENS[VM->pc + 1U].type == TOK_LPAREN)
            {
                /* Function definition */
                Token_t *fn_tok = sc_cur();
                VM->pc += 2U; /* skip ident and '(' */

                if (VM->num_funcs >= MAX_FUNCS)
                {
                    sc_error(fn_tok->line, "Too many functions");
                    return false;
                }
                FuncDef_t *fn = &VM->funcs[VM->num_funcs++];
                fn->name_id     = (uint16_t)fn_tok->val;
                fn->param_count = 0U;

                if (sc_peek() != TOK_RPAREN)
                {
                    if (sc_peek() == TOK_KW_VOID && TOKENS[VM->pc + 1U].type == TOK_RPAREN)
                    {
                        VM->pc++;
                    }
                    else
                    {
                        do
                        {
                            sc_consume_type_specifiers(NULL);
                            Token_t *pt = sc_cur();
                            if (!sc_expect(TOK_IDENT, "Expected parameter name")) return false;
                            if (sc_match(TOK_LBRACK))
                            {
                                sc_expect(TOK_RBRACK, "Expected ']'");
                            }
                            if (fn->param_count < MAX_PARAMS)
                            {
                                fn->param_ids[fn->param_count++] = (uint16_t)pt->val;
                            }
                        } while (sc_match(TOK_COMMA));
                    }
                }
                if (!sc_expect(TOK_RPAREN, "Expected ')' after parameters")) return false;

                /* Forward prototype ';' vs function body '{ ... }' */
                if (sc_match(TOK_SEMI))
                {
                    VM->num_funcs--;
                    continue;
                }

                fn->body_tok = (uint16_t)VM->pc;
                sc_skip_brace_block();
                fn->end_tok = (uint16_t)VM->pc;
            }
            else
            {
                /* Global variable declaration */
                VM->pc = save_pc;
                sc_parse_var_decl(true);
                sc_expect(TOK_SEMI, "Expected ';' after global declaration");
            }
        }
        else
        {
            sc_error(sc_cur()->line, "Expected top-level declaration");
            return false;
        }
    }
    return (VM->flow == FLOW_NORMAL);
}

static void sc_show_error_screen(void)
{
    sys_clear_screen(COLOR_DARK_BG);
    sys_draw_text(12, 16, "C-SCRIPT ERROR", COLOR_RED, COLOR_DARK_BG, 2);

    sys_draw_text(12, 56, "LINE:", COLOR_AMBER, COLOR_DARK_BG, 2);
    sdk_uint_to_str(VM->err_line, VM->fmt_buf, 1U);
    sys_draw_text(80, 56, VM->fmt_buf, COLOR_WHITE, COLOR_DARK_BG, 2);

    sys_draw_text(12, 96, VM->err_msg, COLOR_CYAN, COLOR_DARK_BG, 1);
    sys_draw_text(12, 200, "PRESS ESC / B / A TO EXIT", COLOR_NEON_GREEN, COLOR_DARK_BG, 1);
    sys_beep(120);

    while (sys_get_keys() != 0U)
    {
        sys_sleep_ms(20);
    }
    for (;;)
    {
        if (sys_get_keys() & (KEY_EXIT | KEY_A | KEY_B))
        {
            break;
        }
        sys_sleep_ms(20);
    }
}

static void sc_zero_vm(void)
{
    /* ScriptVM_t lives in 0-WS 32-bit internal SRAM sandbox (0x20018000) */
    uint32_t *p = (uint32_t *)SCRIPT_VM_INTERNAL_ADDR;
    uint32_t words = (uint32_t)(sizeof(ScriptVM_t) / 4U);
    while (words--)
    {
        *p++ = 0U;
    }
    for (uint16_t i = 0; i < MAX_IDENTS; i++)
    {
        VM->global_ById[i] = -1;
        VM->func_ById[i]   = -1;
    }
}

void ScriptEngine_RunSandbox(void)
{
    const char *src = (const char *)SCRIPT_SOURCE_BASE_ADDR;
    sc_zero_vm();

    if (sc_tokenize(src) && sc_parse_program())
    {
        const FuncDef_t *entry = NULL;
        for (uint16_t i = 0; i < VM->num_funcs; i++)
        {
            const char *name = VM->idents[VM->funcs[i].name_id];
            if (sc_streq(name, "main") || sc_streq(name, "app_main"))
            {
                entry = &VM->funcs[i];
                break;
            }
        }

        if (entry == NULL)
        {
            sc_error(1U, "Missing main() or app_main()");
        }
        else
        {
            (void)sc_call_user_func(entry, NULL, 0U, 1U);
        }
    }

    if (VM->flow == FLOW_ERROR)
    {
        sc_show_error_screen();
    }

    sys_exit();
}
