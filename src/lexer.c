/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the
 * file "LICENSE" for information on usage and redistribution of this file.
 */
#include <stdbool.h>

#include "defs.h"
#include "globals.c"

/* Hash table constants */
#define NUM_DIRECTIVES 11
#define NUM_KEYWORDS 18

/* Token mapping structure for elegant initialization */
typedef struct {
    char *name;
    token_kind_t token;
} token_mapping_t;

/* Preprocessor directive hash table using existing shecc hashmap */
hashmap_t *DIRECTIVE_MAP = NULL;
/* C keywords hash table */
hashmap_t *KEYWORD_MAP = NULL;
/* Token arrays for cleanup */
token_kind_t *directive_tokens_storage = NULL;
token_kind_t *keyword_tokens_storage = NULL;

typedef struct token_stream {
    token_t *head;
    token_t *tail;
} token_stream_t;

hashmap_t *TOKEN_CACHE = NULL;
strbuf_t *LIBC_SRC;

void lex_init_directives()
{
    if (DIRECTIVE_MAP)
        return;

    DIRECTIVE_MAP = hashmap_create(16); /* Small capacity for directives */

    /* Initialization using struct compound literals for elegance */
    directive_tokens_storage =
        arena_alloc(GENERAL_ARENA, NUM_DIRECTIVES * sizeof(token_kind_t));

    /* Use array compound literal for directive mappings */
    token_mapping_t directives[] = {
        {"#define", T_cppd_define},   {"#elif", T_cppd_elif},
        {"#else", T_cppd_else},       {"#endif", T_cppd_endif},
        {"#error", T_cppd_error},     {"#if", T_cppd_if},
        {"#ifdef", T_cppd_ifdef},     {"#ifndef", T_cppd_ifndef},
        {"#include", T_cppd_include}, {"#pragma", T_cppd_pragma},
        {"#undef", T_cppd_undef},
    };

    /* hashmap insertion */
    for (int i = 0; i < NUM_DIRECTIVES; i++) {
        directive_tokens_storage[i] = directives[i].token;
        hashmap_put(DIRECTIVE_MAP, directives[i].name,
                    &directive_tokens_storage[i]);
    }
}

void lex_init_keywords()
{
    if (KEYWORD_MAP)
        return;

    KEYWORD_MAP = hashmap_create(32); /* Capacity for keywords */

    /* Initialization using struct compound literals for elegance */
    keyword_tokens_storage =
        arena_alloc(GENERAL_ARENA, NUM_KEYWORDS * sizeof(token_kind_t));

    /* Use array compound literal for keyword mappings */
    token_mapping_t keywords[] = {
        {"if", T_if},
        {"while", T_while},
        {"for", T_for},
        {"do", T_do},
        {"else", T_else},
        {"return", T_return},
        {"typedef", T_typedef},
        {"enum", T_enum},
        {"struct", T_struct},
        {"sizeof", T_sizeof},
        {"switch", T_switch},
        {"case", T_case},
        {"break", T_break},
        {"default", T_default},
        {"continue", T_continue},
        {"goto", T_goto},
        {"union", T_union},
        {"const", T_const},
    };

    /* hashmap insertion */
    for (int i = 0; i < NUM_KEYWORDS; i++) {
        keyword_tokens_storage[i] = keywords[i].token;
        hashmap_put(KEYWORD_MAP, keywords[i].name, &keyword_tokens_storage[i]);
    }
}

/* Hash table lookup for preprocessor directives */
token_kind_t lookup_directive(char *token)
{
    if (!DIRECTIVE_MAP)
        lex_init_directives();

    token_kind_t *result = hashmap_get(DIRECTIVE_MAP, token);
    if (result)
        return *result;

    return T_identifier;
}

/* Hash table lookup for C keywords */
token_kind_t lookup_keyword(char *token)
{
    if (!KEYWORD_MAP)
        lex_init_keywords();

    token_kind_t *result = hashmap_get(KEYWORD_MAP, token);
    if (result)
        return *result;

    return T_identifier;
}


/* Cleanup function for lexer hashmaps */
void lexer_cleanup()
{
    if (DIRECTIVE_MAP) {
        hashmap_free(DIRECTIVE_MAP);
        DIRECTIVE_MAP = NULL;
    }

    if (KEYWORD_MAP) {
        hashmap_free(KEYWORD_MAP);
        KEYWORD_MAP = NULL;
    }

    /* Token storage arrays are allocated from GENERAL_ARENA and will be
     * automatically freed when the arena is freed in global_release().
     * No need to explicitly free them here.
     */
    directive_tokens_storage = NULL;
    keyword_tokens_storage = NULL;
}

bool is_whitespace(char c)
{
    return c == ' ' || c == '\t';
}

char peek_char(int offset);


bool is_newline(char c)
{
    return c == '\r' || c == '\n';
}

/* is it alphabet, number or '_'? */
bool is_alnum(char c)
{
    return ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || (c == '_'));
}

bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

bool is_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

int hex_digit_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool is_numeric(char buffer[])
{
    bool hex = false;
    int size = strlen(buffer);

    if (size > 2 && buffer[0] == '0' && (buffer[1] | 32) == 'x')
        hex = true;

    for (int i = hex ? 2 : 0; i < size; i++) {
        if (hex && !is_hex(buffer[i]))
            return false;
        if (!hex && !is_digit(buffer[i]))
            return false;
    }
    return true;
}

/* NEW */
char peek(strbuf_t *buf, int offset)
{
    return buf->elements[buf->size + offset];
}

char read_offset(strbuf_t *buf, int offset)
{
    buf->size += offset;
    return buf->elements[buf->size];
}

char read(strbuf_t *buf)
{
    buf->size++;
    return buf->elements[buf->size];
}

strbuf_t *read_file(char *filename)
{
    char buffer[MAX_LINE_LEN];
    FILE *f = fopen(filename, "rb");
    strbuf_t *src;

    if (!f) {
        printf("filename: %s\n", filename);
        fatal("source file cannot be found.");
    }

    fseek(f, 0, SEEK_END);
    int len = ftell(f);
    src = strbuf_create(len + 1);
    fseek(f, 0, SEEK_SET);

    while (fgets(buffer, MAX_LINE_LEN, f))
        strbuf_puts(src, buffer);

    fclose(f);
    src->elements[len] = '\0';
    return src;
}

strbuf_t *cache_or_read_file(char *filename)
{
    strbuf_t *buf;

    if (!hashmap_contains(SRC_FILE_MAP, filename)) {
        buf = read_file(filename);
        hashmap_put(SRC_FILE_MAP, filename, buf);
    } else {
        buf = hashmap_get(SRC_FILE_MAP, filename);
    }

    return buf;
}

token_t *new_token(token_kind_t kind, source_location_t *loc, int len)
{
    token_t *token = arena_calloc(TOKEN_ARENA, 1, sizeof(token_t));
    token->kind = kind;
    memcpy(&token->location, loc, sizeof(source_location_t));
    token->location.len = len;
    return token;
}

token_t *lex_token_nt(strbuf_t *buf, source_location_t *loc, token_t *prev)
{
    token_t *token;
    char token_buffer[MAX_TOKEN_LEN], ch = peek(buf, 0);

    loc->pos = buf->size;

    if (ch == '#') {
        if (loc->column != 1)
            error_at("Directive must be on the start of line", loc);

        int sz = 0;

        do {
            if (sz >= MAX_TOKEN_LEN - 1) {
                loc->len = sz;
                error_at("Token too long", loc);
            }
            token_buffer[sz++] = ch;
            ch = read(buf);
        } while (is_alnum(ch));
        token_buffer[sz] = '\0';

        token_kind_t directive_kind = lookup_directive(token_buffer);
        if (directive_kind == T_identifier) {
            loc->len = sz;
            error_at("Unsupported directive", loc);
        }

        token = new_token(directive_kind, loc, sz);
        loc->column += sz;
        return token;
    }

    if (ch == '\\') {
        read(buf);
        token = new_token(T_backslash, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '\n') {
        read(buf);
        token = new_token(T_newline, loc, 1);
        loc->line++;
        loc->column = 1;
        return token;
    }

    if (ch == '/') {
        ch = read(buf);

        if (ch == '*') {
            /* C-style comment */
            int pos = buf->size;
            do {
                /* advance one char */
                pos++;
                loc->column++;
                ch = buf->elements[pos];
                if (ch == '*') {
                    /* look ahead */
                    pos++;
                    loc->column++;
                    ch = buf->elements[pos];
                    if (ch == '/') {
                        /* consume closing '/', then commit and skip trailing
                         * whitespaces
                         */
                        pos++;
                        loc->column += 2;
                        buf->size = pos;
                        return lex_token_nt(buf, loc, prev);
                    }
                }

                if (ch == '\n') {
                    loc->line++;
                    loc->column = 1;
                }
            } while (ch);

            error_at("Unenclosed C-style comment", loc);
            return NULL;
        }

        if (ch == '/') {
            /* C++-style comment */
            int pos = buf->size;
            do {
                pos++;
                ch = buf->elements[pos];
            } while (ch && !is_newline(ch));
            loc->column += pos - buf->size + 1;
            buf->size = pos;
            return lex_token_nt(buf, loc, prev);
        }

        if (ch == '=') {
            ch = read(buf);
            token = new_token(T_divideeq, loc, 2);
            loc->column += 2;
            return token;
        }

        token = new_token(T_divide, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == ' ') {
        /* Compacts sequence of whitespace together */
        int sz = 1;

        while (read(buf) == ' ')
            sz++;

        token = new_token(T_whitespace, loc, sz);
        loc->column += sz;
        return token;
    }

    if (ch == '\t') {
        read(buf);
        token = new_token(T_tab, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '\0') {
        read(buf);
        token = new_token(T_eof, loc, 1);
        loc->column++;
        return token;
    }

    if (is_digit(ch)) {
        int sz = 0;
        token_buffer[sz++] = ch;
        ch = read(buf);

        if (token_buffer[0] == '0' && ((ch | 32) == 'x')) {
            /* Hexadecimal: starts with 0x or 0X */
            if (sz >= MAX_TOKEN_LEN - 1) {
                loc->len = sz;
                error_at("Token too long", loc);
            }
            token_buffer[sz++] = ch;

            ch = read(buf);
            if (!is_hex(ch)) {
                loc->len = 3;
                error_at("Invalid hex literal: expected hex digit after 0x",
                         loc);
            }

            do {
                if (sz >= MAX_TOKEN_LEN - 1) {
                    loc->len = sz;
                    error_at("Token too long", loc);
                }
                token_buffer[sz++] = ch;
                ch = read(buf);
            } while (is_hex(ch));

        } else if (token_buffer[0] == '0' && ((ch | 32) == 'b')) {
            /* Binary literal: 0b or 0B */
            if (sz >= MAX_TOKEN_LEN - 1) {
                loc->len = sz;
                error_at("Token too long", loc);
            }
            token_buffer[sz++] = ch;

            ch = read(buf);
            if (ch != '0' && ch != '1') {
                loc->len = 3;
                error_at("Binary literal expects 0 or 1 after 0b", loc);
            }

            do {
                if (sz >= MAX_TOKEN_LEN - 1) {
                    loc->len = sz;
                    error_at("Token too long", loc);
                }
                token_buffer[sz++] = ch;
                ch = read(buf);
            } while (ch == '0' || ch == '1');

        } else if (token_buffer[0] == '0') {
            /* Octal: starts with 0 but not followed by 'x' or 'b' */
            while (is_digit(ch)) {
                if (ch >= '8') {
                    loc->pos += sz;
                    loc->column += sz;
                    error_at("Invalid octal digit, must be in range 0-7", loc);
                }
                if (sz >= MAX_TOKEN_LEN - 1) {
                    loc->len = sz;
                    error_at("Token too long", loc);
                }
                token_buffer[sz++] = ch;
                ch = read(buf);
            }

        } else {
            /* Decimal */
            while (is_digit(ch)) {
                if (sz >= MAX_TOKEN_LEN - 1) {
                    loc->len = sz;
                    error_at("Token too long", loc);
                }
                token_buffer[sz++] = ch;
                ch = read(buf);
            }
        }

        token_buffer[sz] = '\0';
        token = new_token(T_numeric, loc, sz);
        token->literal = arena_strdup(TOKEN_ARENA, token_buffer);
        loc->column += sz;
        return token;
    }

    if (ch == '(') {
        ch = read(buf);
        token = new_token(T_open_bracket, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == ')') {
        ch = read(buf);
        token = new_token(T_close_bracket, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '{') {
        ch = read(buf);
        token = new_token(T_open_curly, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '}') {
        ch = read(buf);
        token = new_token(T_close_curly, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '[') {
        ch = read(buf);
        token = new_token(T_open_square, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == ']') {
        ch = read(buf);
        token = new_token(T_close_square, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == ',') {
        ch = read(buf);
        token = new_token(T_comma, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '^') {
        ch = read(buf);

        if (ch == '=') {
            ch = read(buf);
            token = new_token(T_xoreq, loc, 2);
            loc->column += 2;
            return token;
        }

        token = new_token(T_bit_xor, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '~') {
        ch = read(buf);
        token = new_token(T_bit_not, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '"') {
        int start_pos = buf->size;
        int sz = 0;
        bool special = false;

        ch = read(buf);
        while (ch != '"' || special) {
            if ((sz > 0) && (token_buffer[sz - 1] == '\\')) {
                if (ch == 'n')
                    token_buffer[sz - 1] = '\n';
                else if (ch == '"')
                    token_buffer[sz - 1] = '"';
                else if (ch == 'r')
                    token_buffer[sz - 1] = '\r';
                else if (ch == '\'')
                    token_buffer[sz - 1] = '\'';
                else if (ch == 't')
                    token_buffer[sz - 1] = '\t';
                else if (ch == '\\')
                    token_buffer[sz - 1] = '\\';
                else if (ch == '0')
                    token_buffer[sz - 1] = '\0';
                else if (ch == 'a')
                    token_buffer[sz - 1] = '\a';
                else if (ch == 'b')
                    token_buffer[sz - 1] = '\b';
                else if (ch == 'v')
                    token_buffer[sz - 1] = '\v';
                else if (ch == 'f')
                    token_buffer[sz - 1] = '\f';
                else if (ch == 'e') /* GNU extension: ESC character */
                    token_buffer[sz - 1] = 27;
                else if (ch == '?')
                    token_buffer[sz - 1] = '?';
                else if (ch == 'x') {
                    /* Hexadecimal escape sequence \xHH */
                    ch = read(buf);
                    if (!is_hex(ch)) {
                        loc->pos += sz;
                        loc->len = 3;
                        error_at("Invalid hex escape sequence", loc);
                    }
                    int value = 0;
                    int count = 0;
                    while (is_hex(ch) && count < 2) {
                        value = (value << 4) + hex_digit_value(ch);
                        ch = read(buf);
                        count++;
                    }
                    token_buffer[sz - 1] = value;
                    /* Back up one character as we read one too many */
                    buf->size--;
                    ch = buf->elements[buf->size];
                } else if (ch >= '0' && ch <= '7') {
                    /* Octal escape sequence \nnn */
                    int value = ch - '0';
                    ch = read(buf);
                    if (ch >= '0' && ch <= '7') {
                        value = (value << 3) + (ch - '0');
                        ch = read(buf);
                        if (ch >= '0' && ch <= '7') {
                            value = (value << 3) + (ch - '0');
                        } else {
                            /* Back up one character */
                            buf->size--;
                            ch = buf->elements[buf->size];
                        }
                    } else {
                        /* Back up one character */
                        buf->size--;
                        ch = buf->elements[buf->size];
                    }
                    token_buffer[sz - 1] = value;
                } else {
                    /* Handle unknown escapes gracefully */
                    token_buffer[sz - 1] = ch;
                }
            } else {
                if (sz >= MAX_TOKEN_LEN - 1) {
                    loc->len = sz + 1;
                    error_at("String literal too long", loc);
                }
                token_buffer[sz++] = ch;
            }

            if (ch == '\\')
                special = true;
            else
                special = false;

            ch = read(buf);
        }
        token_buffer[sz] = '\0';

        read(buf);
        token = new_token(T_string, loc, sz + 2);
        token->literal = arena_strdup(TOKEN_ARENA, token_buffer);
        loc->column += buf->size - start_pos;
        return token;
    }

    if (ch == '\'') {
        int start_pos = buf->size;

        ch = read(buf);
        if (ch == '\\') {
            ch = read(buf);
            if (ch == 'n')
                token_buffer[0] = '\n';
            else if (ch == 'r')
                token_buffer[0] = '\r';
            else if (ch == '\'')
                token_buffer[0] = '\'';
            else if (ch == '"')
                token_buffer[0] = '"';
            else if (ch == 't')
                token_buffer[0] = '\t';
            else if (ch == '\\')
                token_buffer[0] = '\\';
            else if (ch == '0')
                token_buffer[0] = '\0';
            else if (ch == 'a')
                token_buffer[0] = '\a';
            else if (ch == 'b')
                token_buffer[0] = '\b';
            else if (ch == 'v')
                token_buffer[0] = '\v';
            else if (ch == 'f')
                token_buffer[0] = '\f';
            else if (ch == 'e') /* GNU extension: ESC character */
                token_buffer[0] = 27;
            else if (ch == '?')
                token_buffer[0] = '?';
            else if (ch == 'x') {
                /* Hexadecimal escape sequence \xHH */
                ch = read(buf);
                if (!is_hex(ch)) {
                    loc->pos++;
                    loc->len = 3;
                    error_at("Invalid hex escape sequence", loc);
                }
                int value = 0;
                int count = 0;
                while (is_hex(ch) && count < 2) {
                    value = (value << 4) + hex_digit_value(ch);
                    ch = read(buf);
                    count++;
                }
                token_buffer[0] = value;
                /* Back up one character as we read one too many */
                buf->size--;
                ch = buf->elements[buf->size];
            } else if (ch >= '0' && ch <= '7') {
                /* Octal escape sequence \nnn */
                int value = ch - '0';
                ch = read(buf);
                if (ch >= '0' && ch <= '7') {
                    value = (value << 3) + (ch - '0');
                    ch = read(buf);
                    if (ch >= '0' && ch <= '7') {
                        value = (value << 3) + (ch - '0');
                    } else {
                        /* Back up one character */
                        buf->size--;
                        ch = buf->elements[buf->size];
                    }
                } else {
                    /* Back up one character */
                    buf->size--;
                    ch = buf->elements[buf->size];
                }
                token_buffer[0] = value;
            } else {
                /* Handle unknown escapes gracefully */
                token_buffer[0] = ch;
            }
        } else {
            token_buffer[0] = ch;
        }
        token_buffer[1] = '\0';

        ch = read(buf);
        if (ch != '\'') {
            loc->len = 2;
            error_at("Unenclosed character literal", loc);
        }

        read(buf);
        token = new_token(T_char, loc, 3);
        token->literal = arena_strdup(TOKEN_ARENA, token_buffer);
        loc->column += buf->size - start_pos;
        return token;
    }

    if (ch == '*') {
        ch = read(buf);

        if (ch == '=') {
            read(buf);
            token = new_token(T_asteriskeq, loc, 2);
            loc->column += 2;
            return token;
        }

        token = new_token(T_asterisk, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '&') {
        ch = read(buf);

        if (ch == '&') {
            read(buf);
            token = new_token(T_log_and, loc, 2);
            loc->column += 2;
            return token;
        }

        if (ch == '=') {
            read(buf);
            token = new_token(T_andeq, loc, 2);
            loc->column += 2;
            return token;
        }

        token = new_token(T_ampersand, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '|') {
        ch = read(buf);

        if (ch == '|') {
            read(buf);
            token = new_token(T_log_or, loc, 2);
            loc->column += 2;
            return token;
        }

        if (ch == '=') {
            read(buf);
            token = new_token(T_oreq, loc, 2);
            loc->column += 2;
            return token;
        }

        token = new_token(T_bit_or, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '<') {
        ch = read(buf);

        if (ch == '=') {
            read(buf);
            token = new_token(T_le, loc, 2);
            loc->column += 2;
            return token;
        }

        if (ch == '<') {
            ch = read(buf);

            if (ch == '=') {
                read(buf);
                token = new_token(T_lshifteq, loc, 3);
                loc->column += 3;
                return token;
            }

            token = new_token(T_lshift, loc, 2);
            loc->column += 2;
            return token;
        }

        token = new_token(T_lt, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '%') {
        ch = read(buf);

        if (ch == '=') {
            read(buf);
            token = new_token(T_modeq, loc, 2);
            loc->column += 2;
            return token;
        }

        token = new_token(T_mod, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '>') {
        ch = read(buf);

        if (ch == '=') {
            read(buf);
            token = new_token(T_ge, loc, 2);
            loc->column += 2;
            return token;
        }

        if (ch == '>') {
            ch = read(buf);

            if (ch == '=') {
                read(buf);
                token = new_token(T_rshifteq, loc, 3);
                loc->column += 3;
                return token;
            }

            token = new_token(T_rshift, loc, 2);
            loc->column += 2;
            return token;
        }

        token = new_token(T_gt, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '!') {
        ch = read(buf);

        if (ch == '=') {
            read(buf);
            token = new_token(T_noteq, loc, 2);
            loc->column += 2;
            return token;
        }

        token = new_token(T_log_not, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '.') {
        ch = read(buf);

        if (ch == '.' && peek(buf, 1) == '.') {
            buf->size += 2;
            token = new_token(T_elipsis, loc, 3);
            loc->column += 3;
            return token;
        }

        token = new_token(T_dot, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '-') {
        ch = read(buf);

        if (ch == '>') {
            read(buf);
            token = new_token(T_arrow, loc, 2);
            loc->column += 2;
            return token;
        }

        if (ch == '-') {
            read(buf);
            token = new_token(T_decrement, loc, 2);
            loc->column += 2;
            return token;
        }

        if (ch == '=') {
            read(buf);
            token = new_token(T_minuseq, loc, 2);
            loc->column += 2;
            return token;
        }

        token = new_token(T_minus, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '+') {
        ch = read(buf);

        if (ch == '+') {
            read(buf);
            token = new_token(T_increment, loc, 2);
            loc->column += 2;
            return token;
        }

        if (ch == '=') {
            read(buf);
            token = new_token(T_pluseq, loc, 2);
            loc->column += 2;
            return token;
        }

        token = new_token(T_plus, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == ';') {
        read(buf);
        token = new_token(T_semicolon, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '?') {
        read(buf);
        token = new_token(T_question, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == ':') {
        read(buf);
        token = new_token(T_colon, loc, 1);
        loc->column++;
        return token;
    }

    if (ch == '=') {
        ch = read(buf);

        if (ch == '=') {
            read(buf);
            token = new_token(T_eq, loc, 2);
            loc->column += 2;
            return token;
        }

        token = new_token(T_assign, loc, 1);
        loc->column++;
        return token;
    }

    if (is_alnum(ch)) {
        int sz = 0;
        do {
            if (sz >= MAX_TOKEN_LEN - 1) {
                loc->len = sz;
                error_at("Token too long", loc);
            }
            token_buffer[sz++] = ch;
            ch = read(buf);
        } while (is_alnum(ch));
        token_buffer[sz] = 0;

        /* Fast path for common keywords - avoid hashmap lookup */
        token_kind_t kind = T_identifier;

        /* Check most common keywords inline based on token length and first
         * character.
         */
        switch (sz) {
        case 2: /* 2-letter keywords: if, do */
            if (token_buffer[0] == 'i' && token_buffer[1] == 'f')
                kind = T_if;
            else if (token_buffer[0] == 'd' && token_buffer[1] == 'o')
                kind = T_do;
            break;

        case 3: /* 3-letter keywords: for */
            if (token_buffer[0] == 'f' && token_buffer[1] == 'o' &&
                token_buffer[2] == 'r')
                kind = T_for;
            break;

        case 4: /* 4-letter keywords: else, enum, case */
            if (token_buffer[0] == 'e') {
                if (!memcmp(token_buffer, "else", 4))
                    kind = T_else;
                else if (!memcmp(token_buffer, "enum", 4))
                    kind = T_enum;
            } else if (!memcmp(token_buffer, "case", 4))
                kind = T_case;
            else if (!memcmp(token_buffer, "goto", 4))
                kind = T_goto;
            break;

        case 5: /* 5-letter keywords: while, break, union, const */
            if (token_buffer[0] == 'w' && !memcmp(token_buffer, "while", 5))
                kind = T_while;
            else if (token_buffer[0] == 'b' &&
                     !memcmp(token_buffer, "break", 5))
                kind = T_break;
            else if (token_buffer[0] == 'u' &&
                     !memcmp(token_buffer, "union", 5))
                kind = T_union;
            else if (token_buffer[0] == 'c' &&
                     !memcmp(token_buffer, "const", 5))
                kind = T_const;
            break;

        case 6: /* 6-letter keywords: return, struct, switch, sizeof */
            if (token_buffer[0] == 'r' && !memcmp(token_buffer, "return", 6))
                kind = T_return;
            else if (token_buffer[0] == 's') {
                if (!memcmp(token_buffer, "struct", 6))
                    kind = T_struct;
                else if (!memcmp(token_buffer, "switch", 6))
                    kind = T_switch;
                else if (!memcmp(token_buffer, "sizeof", 6))
                    kind = T_sizeof;
            }
            break;

        case 7: /* 7-letter keywords: typedef, default */
            if (!memcmp(token_buffer, "typedef", 7))
                kind = T_typedef;
            else if (!memcmp(token_buffer, "default", 7))
                kind = T_default;
            break;

        case 8: /* 8-letter keywords: continue */
            if (!memcmp(token_buffer, "continue", 8))
                kind = T_continue;
            break;

        default:
            /* Keywords longer than 8 chars or identifiers - use hashmap */
            break;
        }

        /* Fall back to hashmap for uncommon keywords */
        if (kind == T_identifier)
            kind = lookup_keyword(token_buffer);

        token = new_token(kind, loc, sz);
        token->literal = arena_strdup(TOKEN_ARENA, token_buffer);
        loc->column += sz;
        return token;
    }

    error_at("Unexpected token", loc);
    return NULL;
}

token_stream_t *lex_token_by_file(char *filename)
{
    /* FIXME: We should normalize filename first to make cache works as expected
     */

    token_t *head = NULL, *tail = NULL, *cur = NULL, *prev = NULL;
    token_stream_t *tks;
    /* initialie source location with the following configuration:
     * pos is at 0,
     * len is 1 for reporting convenience,
     * and the column and line number are set to 1.
     */
    source_location_t loc = {0, 1, 1, 1, filename};
    strbuf_t *buf;

    if (!TOKEN_CACHE)
        TOKEN_CACHE = hashmap_create(8);

    tks = hashmap_get(TOKEN_CACHE, filename);

    /* Already cached, just return the computed token stream */
    if (tks)
        return tks;

    buf = cache_or_read_file(filename);

    /* Borrows strbuf_t#size to use as source index */
    buf->size = 0;

    while (buf->size < buf->capacity) {
        cur = lex_token_nt(buf, &loc, prev);

        if (cur->kind != T_whitespace && cur->kind != T_tab &&
            cur->kind != T_eof)
            prev = cur;

        /* Append token to token stream */
        if (!head) {
            /* Token stream unintialized  */
            head = cur;
            tail = head;
        }

        tail->next = cur;
        tail = cur;

        if (cur->kind == T_eof)
            break;
    }

    if (!head) {
        head = arena_calloc(TOKEN_ARENA, 1, sizeof(token_t));
        head->kind = T_eof;
        memcpy(&head->location, &loc, sizeof(source_location_t));
        tail = head;
    }

    if (tail->kind != T_eof)
        error_at("Internal error, expected eof at the end of file",
                 &tail->location);

    tks = malloc(sizeof(token_stream_t));
    tks->head = head;
    tks->tail = tail;
    hashmap_put(TOKEN_CACHE, filename, tks);
    return tks;
}

token_stream_t *include_libc()
{
    token_t *head = NULL, *tail = NULL, *cur = NULL, *prev = NULL;
    token_stream_t *tks;
    char *filename = "lib/c.c";
    strbuf_t *buf = LIBC_SRC;
    source_location_t loc = {0, 1, 1, 1, filename};

    tks = hashmap_get(TOKEN_CACHE, filename);

    if (tks)
        return tks;

    if (!TOKEN_CACHE)
        TOKEN_CACHE = hashmap_create(8);

    if (!hashmap_contains(SRC_FILE_MAP, filename))
        hashmap_put(SRC_FILE_MAP, filename, LIBC_SRC);

    /* Borrows strbuf_t#size to use as source index */
    buf->size = 0;

    while (buf->size < buf->capacity) {
        cur = lex_token_nt(buf, &loc, prev);

        if (cur->kind != T_whitespace && cur->kind != T_tab &&
            cur->kind != T_eof)
            prev = cur;

        /* Append token to token stream */
        if (!head) {
            /* Token stream unintialized  */
            head = cur;
            tail = head;
        }

        tail->next = cur;
        tail = cur;

        if (cur->kind == T_eof)
            break;
    }

    if (!head)
        fatal("Unable to include libc");

    if (tail->kind != T_eof)
        error_at("Internal error, expected eof at the end of file",
                 &tail->location);

    /* Discard eof so later input file's token stream can join at correct
     * position */
    prev->next = NULL;
    tail = prev;

    tks = malloc(sizeof(token_stream_t));
    tks->head = head;
    tks->tail = tail;
    hashmap_put(TOKEN_CACHE, filename, tks);
    return tks;
}

void skip_unused_token(void)
{
    while (cur_token && cur_token->next) {
        if (cur_token->next->kind == T_whitespace ||
            cur_token->next->kind == T_newline ||
            cur_token->next->kind == T_tab)
            cur_token = cur_token->next;
        else
            break;
    }
}

/* Lex next token with aliasing enabled */
token_kind_t lex_token(void)
{
    skip_unused_token();
    /* if reached eof, we always return eof token to avoid any advancement */
    if (cur_token->kind == T_eof)
        return T_eof;

    cur_token = cur_token->next;
    return cur_token->kind;
}

/* Accepts next token if token types are matched. To disable aliasing on next
 * token, use 'lex_accept_internal'.
 */
bool lex_accept(token_kind_t kind)
{
    skip_unused_token();
    if (cur_token->next && cur_token->next->kind == kind) {
        lex_token();
        return true;
    }
    return false;
}

/* Peeks next token and copy token's literal to value if token types are
 * matched.
 */
bool lex_peek(token_kind_t kind, char *value)
{
    skip_unused_token();
    if (cur_token->next && cur_token->next->kind == kind) {
        if (!value)
            return true;
        strcpy(value, cur_token->next->literal);
        return true;
    }
    return false;
}

/* Strictly match next token with given token type and copy token's literal to
 * value. To disable aliasing on next token, use 'lex_ident_internal'.
 */
void lex_ident(token_kind_t token, char *value)
{
    skip_unused_token();
    if (cur_token->next && cur_token->next->kind == token) {
        lex_token();
        if (value)
            strcpy(value, cur_token->literal);
        return;
    }
    token_t *tk = cur_token->next ? cur_token->next : cur_token;
    error_at("Unexpected token", &tk->location);
}

/* Strictly match next token with given token type. To disable aliasing on next
 * token, use 'lex_expect_internal'.
 */
void lex_expect(token_kind_t token)
{
    skip_unused_token();
    if (cur_token->next && cur_token->next->kind == token) {
        lex_token();
        return;
    }
    token_t *tk = cur_token->next ? cur_token->next : cur_token;
    error_at("Unexpected token", &tk->location);
}
