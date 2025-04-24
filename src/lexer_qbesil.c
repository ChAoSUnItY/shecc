#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    QS_TOK_EOF,
    /* punctuation */
    QS_TOK_LPAREN,
    QS_TOK_RPAREN,
    QS_TOK_LBRACE,
    QS_TOK_RBRACE,
    QS_TOK_COMMA,
    QS_TOK_EQ,
    QS_TOK_PLUS,
    /* literals / idents */
    QS_TOK_INT,
    QS_TOK_STRING,
    QS_TOK_IDENT, /* bare identifier */
    QS_TOK_GLOBAL,
    QS_TOK_TEMP,
    QS_TOK_LABEL,
    /* keywords */
    QS_TOK_KW_DATA,
    QS_TOK_KW_FUNCTION,
    QS_TOK_KW_JMP,
    QS_TOK_KW_JNZ,
    QS_TOK_KW_RET,
    QS_TOK_KW_HLT,
    QS_TOK_KW_CALL,
    QS_TOK_KW_PHI,
    QS_TOK_KW_BYTE,
    QS_TOK_KW_WORD,
    QS_TOK_KW_VOID,
    QS_TOK_ELLIPSIS
} qs_token_kind_t;

typedef struct {
    qs_token_kind_t k;
    char *text; /* points into arena (or static) */
    int line;
    int col;
    int ival; /* for TOK_INT */
    int len;  /* length of text (no NUL) */
} qs_token_t;

char *qs_src; /* current pointer */
int qs_cur_line;
int qs_cur_col;
qs_token_t qs_tok;

/* support 1 hard-coded argument */
void qs_error_at(int line, int col, char *fmt, int arg1)
{
    printf("Error %d:%d: ", line, col);
    printf(fmt, arg1);
    printf("\n");
    qs_arena_free_all();
    exit(1);
}

bool qs_is_alpha(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

bool qs_is_digit(char c)
{
    return (c >= '0' && c <= '9');
}

bool qs_isalnum(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z');
}

bool qs_is_ident(char c)
{
    return qs_isalnum(c) || c == '_';
}

int qs_stoi(char *s)
{
    int val = 0;
    int sign = 1;
    if (*s == '-') {
        sign = -1;
        ++s;
    }
    while (*s) {
        val = val * 10 + (*s - '0');
        ++s;
    }
    return sign * val;
}

qs_token_kind_t qs_kw_lookup(char *txt, int n)
{
    switch (n) {
    case 1:
        if (txt[0] == 'b')
            return QS_TOK_KW_BYTE;
        if (txt[0] == 'w')
            return QS_TOK_KW_WORD;
        if (txt[0] == 'v')
            return QS_TOK_KW_VOID;
        break;
    case 3:
        // "jmp", "jnz", "ret", "hlt", "phi"
        if (txt[0] == 'j' && txt[1] == 'm' && txt[2] == 'p')
            return QS_TOK_KW_JMP;
        if (txt[0] == 'j' && txt[1] == 'n' && txt[2] == 'z')
            return QS_TOK_KW_JNZ;
        if (txt[0] == 'r' && txt[1] == 'e' && txt[2] == 't')
            return QS_TOK_KW_RET;
        if (txt[0] == 'h' && txt[1] == 'l' && txt[2] == 't')
            return QS_TOK_KW_HLT;
        if (txt[0] == 'p' && txt[1] == 'h' && txt[2] == 'i')
            return QS_TOK_KW_PHI;
        break;
    case 4:
        // "data", "call"
        if (txt[0] == 'd' && txt[1] == 'a' && txt[2] == 't' && txt[3] == 'a')
            return QS_TOK_KW_DATA;
        if (txt[0] == 'c' && txt[1] == 'a' && txt[2] == 'l' && txt[3] == 'l')
            return QS_TOK_KW_CALL;
        break;
    case 8:
        // "function"
        if (txt[0] == 'f' && txt[1] == 'u' && txt[2] == 'n' && txt[3] == 'c' &&
            txt[4] == 't' && txt[5] == 'i' && txt[6] == 'o' && txt[7] == 'n')
            return QS_TOK_KW_FUNCTION;
        break;
    }
    return QS_TOK_IDENT;
}

void qs_next_tok()
{
    /* skip whitespace / comments */
    while (true) {
        char c = *qs_src;
        if (c == ' ' || c == '\t') {
            ++qs_src;
            ++qs_cur_col;
            continue;
        }
        if (c == '\n') {
            ++qs_src;
            ++qs_cur_line;
            qs_cur_col = 1;
            continue;
        }
        if (c == '#') {
            while (*qs_src && *qs_src != '\n')
                ++qs_src;
            continue;
        }
        break;
    }

    qs_tok.line = qs_cur_line;
    qs_tok.col = qs_cur_col;
    qs_tok.text = NULL;
    qs_tok.len = 0;
    qs_tok.ival = 0;

    char c = *qs_src;
    if (!c) {
        qs_tok.k = QS_TOK_EOF;
        return;
    }

    /* punctuation */
    switch (c) {
    case 40:  // '('
        ++qs_src;
        ++qs_cur_col;
        qs_tok.k = QS_TOK_LPAREN;
        return;
    case 41:  // ')'
        ++qs_src;
        ++qs_cur_col;
        qs_tok.k = QS_TOK_RPAREN;
        return;
    case 123:  // '{'
        ++qs_src;
        ++qs_cur_col;
        qs_tok.k = QS_TOK_LBRACE;
        return;
    case 125:  // '}'
        ++qs_src;
        ++qs_cur_col;
        qs_tok.k = QS_TOK_RBRACE;
        return;
    case 44:  // ','
        ++qs_src;
        ++qs_cur_col;
        qs_tok.k = QS_TOK_COMMA;
        return;
    case 61:  // '='
        ++qs_src;
        ++qs_cur_col;
        qs_tok.k = QS_TOK_EQ;
        return;
    case 43:  // '+'
        ++qs_src;
        ++qs_cur_col;
        qs_tok.k = QS_TOK_PLUS;
        return;
    case 46:  // '.'
        if (qs_src[1] == '.' && qs_src[2] == '.') {
            qs_src += 3;
            qs_cur_col += 3;
            qs_tok.k = QS_TOK_ELLIPSIS;
            qs_tok.text = "...";
            qs_tok.len = 3;
            return;
        }
        qs_error_at(qs_tok.line, qs_tok.col, "unexpected '.'", 0);
    }

    /* sigil identifiers */
    if (c == '$' || c == '%' || c == '@') {
        char sig = c;
        char *start = qs_src;
        ++qs_src;
        ++qs_cur_col;
        while (qs_is_ident(*qs_src)) {
            ++qs_src;
            ++qs_cur_col;
        }
        int n = qs_src - start;

        char *name = qs_arena_strdup(start, n);
        qs_tok.text = name;
        qs_tok.len = n;
        if (sig == '$')
            qs_tok.k = QS_TOK_GLOBAL;
        else if (sig == '@')
            qs_tok.k = QS_TOK_LABEL;
        else
            qs_tok.k = QS_TOK_TEMP;
        return;
    }

    /* number */
    if (c == '-' || qs_is_digit(c)) {
        char *start = qs_src;
        if (c == '-') {
            ++qs_src;
            ++qs_cur_col;
            if (!qs_is_digit(*qs_src))
                qs_error_at(qs_tok.line, qs_tok.col, "expected digit after '-'",
                            0);
        }
        while (qs_is_digit(*qs_src)) {
            ++qs_src;
            ++qs_cur_col;
        }

        int n = qs_src - start;
        if (n >= 32)
            qs_error_at(qs_tok.line, qs_tok.col, "integer too long", 0);

        char buf[32];
        for (int i = 0; i < n; ++i)
            buf[i] = start[i];
        buf[n] = '\0';

        qs_tok.ival = qs_stoi(buf);
        qs_tok.k = QS_TOK_INT;
        return;
    }

    /* string */
    /* support basic escape character */
    if (c == '"') {
        ++qs_src;
        ++qs_cur_col;

        char *p = qs_src;
        int len = 0;
        while (*p && *p != '"') {
            // skip \"
            if (*p == '\\') {
                ++p;
                if (!*p)
                    break;
            }
            ++len;
            ++p;
        }
        if (*p != '"') {
            qs_error_at(qs_tok.line, qs_tok.col, "unterminated string", 0);
        }

        char *dest = qs_arena_alloc(len + 1);

        int i = 0;
        while (*qs_src && *qs_src != '"') {
            char ch = *qs_src++;
            ++qs_cur_col;

            if (ch == '\\' && *qs_src) {
                char esc = *qs_src++;
                ++qs_cur_col;
                switch (esc) {
                case 110:  // 'n'
                    ch = '\n';
                    break;
                case 116:  // 't'
                    ch = '\t';
                    break;
                case 114:  // 'r'
                    ch = '\r';
                    break;
                case 48:  // '0'
                    ch = '\0';
                    break;
                case 34:  // '"'
                    ch = '"';
                    break;
                case 92:  // '\\'
                    ch = '\\';
                    break;
                default:
                    ch = esc;
                    break;
                }
            }
            dest[i++] = ch;
        }

        ++qs_src;
        ++qs_cur_col;

        dest[i] = '\0';
        qs_tok.k = QS_TOK_STRING;
        qs_tok.text = dest;
        qs_tok.len = len;
        return;
    }

    /* bare identifier / keyword */
    if (qs_is_alpha(c) || c == '_') {
        char *start = qs_src;
        while (qs_is_ident(*qs_src)) {
            ++qs_src;
            ++qs_cur_col;
        }
        int n = qs_src - start;

        char *name = qs_arena_strdup(start, n);
        qs_tok.k = qs_kw_lookup(name, n);
        qs_tok.text = name;
        qs_tok.len = n;
        return;
    }

    qs_error_at(qs_tok.line, qs_tok.col, "unrecognised char '%c'", c);
}

bool qs_peek(qs_token_kind_t k)
{
    return qs_tok.k == k;
}

bool qs_accept(qs_token_kind_t k)
{
    if (qs_tok.k == k) {
        qs_next_tok();
        return true;
    }
    return false;
}

void qs_expect(qs_token_kind_t k)
{
    if (qs_tok.k != k)
        qs_error_at(qs_tok.line, qs_tok.col, "expected token %d", k);
    qs_next_tok();
}

void qs_load_source_file(char *file)
{
    char buffer[MAX_LINE_LEN];

    FILE *f = fopen(file, "rb");
    if (!f)
        abort();

    for (;;) {
        if (!fgets(buffer, MAX_LINE_LEN, f)) {
            fclose(f);
            return;
        }
        strcpy(SOURCE->elements + SOURCE->size, buffer);
        SOURCE->size += strlen(buffer);
    }
    fclose(f);
}

void qs_init_lexer(char *file)
{
    qs_load_source_file(file);
    char *input = SOURCE->elements;
    qs_src = input;
    qs_cur_line = 1;
    qs_cur_col = 1;
}
