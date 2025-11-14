/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the
 * file "LICENSE" for information on usage and redistribution of this file.
 */

#include "defs.h"
#include "globals.c"

hashmap_t *MACROS;

token_t *lex_skip_space(token_t *tk)
{
    while (tk->next->kind == T_whitespace || tk->next->kind == T_tab)
        tk = tk->next;
    return tk;
}

token_t *lex_next_token(token_t *tk, bool skip_space)
{
    if (skip_space)
        tk = lex_skip_space(tk);
    return tk->next;
}

bool lex_peek_token(token_t *tk, token_kind_t kind, bool skip_space)
{
    if (skip_space)
        tk = lex_skip_space(tk);
    return tk->next && tk->next->kind == kind;
}

token_t *lex_expect_token(token_t *tk, token_kind_t kind, bool skip_space)
{
    if (skip_space)
        tk = lex_skip_space(tk);
    if (tk->next) {
        if (tk->next->kind == kind) {
            return lex_next_token(tk, false);
        }

        error_at("Unexpected token kind", &tk->next->location);
    }

    error_at("Expect token after this token", &tk->location);
    return tk;
}

token_t *lex_ident_token(token_t *tk, token_kind_t kind, char *dest, bool skip_space)
{
    tk = lex_expect_token(tk, kind, skip_space);
    strcpy(dest, tk->literal);
    return tk;
}

/* Copies and isolate the given copied token */
token_t *copy_token(token_t *tk)
{
    token_t *new_tk = arena_calloc(TOKEN_ARENA, 1, sizeof(token_t));
    memcpy(new_tk, tk, sizeof(token_t));
    new_tk->next = NULL;
    return new_tk;
}

typedef struct macro_n {
    char *name;
    int param_num;
    bool is_variadic;
    token_t *param_names[MAX_PARAMS];
    token_t *replacement;
    bool is_disabled;
    /* build-in function-like macro handler */
    token_t *(*handler)(token_t *);
} macro_nt;

token_t *file_macro_handler(token_t *tk)
{
    token_t *new_tk = copy_token(tk);
    new_tk->kind = T_string;
    new_tk->literal = tk->location.filename;
    memcpy(&new_tk->location, &tk->location, sizeof(source_location_t));
    return new_tk;
}

token_t *line_macro_handler(token_t *tk)
{
    char line[MAX_TOKEN_LEN];
    snprintf(line, MAX_TOKEN_LEN, "%d", tk->location.line);
    
    token_t *new_tk = copy_token(tk);
    new_tk->kind = T_numeric;
    new_tk->literal = arena_strdup(TOKEN_ARENA, line);
    memcpy(&new_tk->location, &tk->location, sizeof(source_location_t));
    return new_tk;
}

typedef struct hide_set {
    char *name;
    struct hide_set *next;
} hide_set_t;

typedef struct preprocess_ctx {
    hide_set_t *hide_set;
    token_t *expanded_from;
    token_t *end_of_token; /* end of token stream of current context */
    bool trim_eof;
} preprocess_ctx_t;

/* Removes unnecessary tokens from token stream, e.g. whitespace */
token_t *trim_token(token_t *tk)
{
    token_t head;
    token_t *cur = &head;
    head.next = tk;

    while (cur->next) {
        switch (cur->next->kind)
        {
            case T_newline:
            case T_backslash:
            case T_whitespace:
            case T_tab:
                cur->next = cur->next->next;
                break;
            default:
                cur = cur->next;
                break;
        }
    }

    return head.next;
}

token_t *preprocess_internal(token_t *tk, preprocess_ctx_t *ctx);

token_t *expand_obj_macro(macro_nt *macro, preprocess_ctx_t *ctx)
{
    token_t *r_head = NULL, *r_tail = NULL, *cur;
    token_t *cur_macro_token = macro->replacement;

    while (cur_macro_token) {
        cur = copy_token(cur_macro_token);

        if (!r_head) {
            r_head = cur;
            r_tail = r_head;
        } else {
            r_tail->next = cur;
            r_tail = cur;
        }

        cur_macro_token = cur_macro_token->next;
    }

    r_head = preprocess_internal(r_head, ctx);
    cur = ctx->end_of_token;

    return r_head;
}

token_t *preprocess_internal(token_t *tk, preprocess_ctx_t *ctx)
{
    token_t head;
    token_t *cur = &head;
    macro_nt *macro;

    while (tk) {
        switch (tk->kind) {
        case T_cppd_include: {
            char *inclusion_path;
            token_t *file_tk = NULL;
            preprocess_ctx_t inclusion_ctx;
            inclusion_ctx.hide_set = ctx->hide_set;
            inclusion_ctx.expanded_from = NULL;
            inclusion_ctx.trim_eof = true;
            
            tk = lex_expect_token(tk, T_inclusion_path, true);
            inclusion_path = tk->literal;
            tk = lex_expect_token(tk, T_newline, true);
            tk = lex_next_token(tk, false);
            
            file_tk = lex_token_by_file(inclusion_path);
            cur->next = preprocess_internal(file_tk, &inclusion_ctx);

            cur = inclusion_ctx.end_of_token;
            continue;
        }
        case T_cppd_define: {
            token_t *r_head = NULL, *r_tail = NULL, *r_cur;

            macro = calloc(1, sizeof(macro_nt));
            tk = lex_expect_token(tk, T_identifier, true);
            macro->name = tk->literal;

            tk = lex_skip_space(tk);
            while (!lex_peek_token(tk, T_newline, false)) {
                if (lex_peek_token(tk, T_backslash, false)) {
                    tk = lex_expect_token(tk, T_backslash, false);

                    if (!lex_peek_token(tk, T_newline, false))
                        error_at("Backslash and newline must not be separated" , &tk->location);
                    else
                        tk = lex_expect_token(tk, T_newline, false);

                    tk = lex_next_token(tk, false);
                    continue;
                }
                
                tk = lex_next_token(tk, false);
                r_cur = copy_token(tk);
                r_cur->next = NULL;

                if (!r_head) {
                    r_head = r_cur;
                    r_tail = r_head;
                } else {
                    r_tail->next = r_cur;
                    r_tail = r_cur;
                }
            }

            tk = lex_expect_token(tk, T_newline, false);
            tk = lex_next_token(tk, false);
            macro->replacement = r_head;
            hashmap_put(MACROS, macro->name, macro);
            continue;
        }
        case T_identifier: {
            preprocess_ctx_t expansion_ctx;
            expansion_ctx.hide_set = ctx->hide_set;
            expansion_ctx.expanded_from = ctx->expanded_from ? ctx->expanded_from : tk;
            expansion_ctx.trim_eof = true;

            macro = hashmap_get(MACROS, tk->literal);

            if (!macro)
                break;

            if (macro->is_disabled)
                break;

            if (macro->handler) {
                cur->next = macro->handler(expansion_ctx.expanded_from);
                cur = cur->next;
                tk = lex_next_token(tk, false);
                continue;
            }

            if (tk->next && tk->next->kind == T_open_bracket) {
                /* Check if it's an function-like macro invocation */
            } else {
                cur->next = expand_obj_macro(macro, &expansion_ctx);
                cur = expansion_ctx.end_of_token;
            }

            tk = lex_next_token(tk, false);
            continue;
        }
        case T_backslash: {
            /* This branch is designed to be failed since backslash should be consumed by #define, and
             * upon later expansion, it should not be included previously while created by #define.
             */
            error_at("Backslash is not allowed here", &cur->location);
            break;
        }
        case T_eof: {
            if (ctx->trim_eof) {
                tk = lex_next_token(tk, false);
                continue;
            }
            break;
        }
        default:
            break;
        }

        cur->next = copy_token(tk);
        cur = cur->next;
        tk = lex_next_token(tk, false);
    }

    ctx->end_of_token = cur;
    return head.next;
}

token_t *preprocess(token_t *tk)
{
    preprocess_ctx_t ctx;
    ctx.hide_set = NULL;
    ctx.expanded_from = NULL;
    ctx.trim_eof = false;

    /* Initialize built-in macros */
    MACROS = hashmap_create(16);

    macro_nt *macro = calloc(1, sizeof(macro_nt));
    macro->name = "__FILE__";
    macro->handler = file_macro_handler;
    hashmap_put(MACROS, "__FILE__", macro);
    
    macro = calloc(1, sizeof(macro_nt));
    macro->name = "__LINE__";
    macro->handler = line_macro_handler;
    hashmap_put(MACROS, "__LINE__", macro);

    tk = preprocess_internal(tk, &ctx);
    return tk;
}
