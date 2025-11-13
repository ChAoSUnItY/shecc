/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the
 * file "LICENSE" for information on usage and redistribution of this file.
 */

#include "defs.h"
#include "globals.c"

token_t *head_token;
token_t *prev_token;
token_t *cur_token;

hashmap_t *MACROS;

void lex_next_token()
{
    prev_token = cur_token;
    cur_token = cur_token->next;
}

bool lex_peek_token(token_kind_t kind)
{
    return cur_token->next && cur_token->next->kind == kind;
}

void lex_expect_token(token_kind_t kind)
{
    if (cur_token->next) {
        if (cur_token->next->kind == kind) {
            lex_next_token();
            return;
        }

        error_at("Unexpected token kind", &cur_token->next->location);
    }

    error_at("Expect token after this token", &cur_token->location);
}

bool lex_accept_token(token_kind_t kind)
{
    if (cur_token->next && cur_token->next->kind == kind) {
        lex_next_token();
        return true;
    }

    return false;
}

void lex_ident_token(token_kind_t kind, char *dest)
{
    lex_expect_token(kind);
    strcpy(dest, cur_token->literal);
}

typedef struct macro_n {
    char name[MAX_TOKEN_LEN];
    int param_num;
    bool is_variadic;
    token_t *param_names[MAX_PARAMS];
    token_t *replacement;
    bool is_disabled;
} macro_nt;

void expand_obj_macro(macro_nt *macro)
{
    token_t *start_token = prev_token, *next_token = cur_token->next,
            *cur_macro_token = macro->replacement;

    cur_token = prev_token;

    while (cur_macro_token) {
        cur_token->next = arena_alloc(TOKEN_ARENA, sizeof(token_t));
        lex_next_token();
        memcpy(cur_token, cur_macro_token, sizeof(token_t));

        cur_macro_token = cur_macro_token->next;
    }

    cur_token->next = next_token;
    prev_token = start_token;
    cur_token = next_token;
}

void preprocess_internal()
{
    macro_nt *macro;

    while (cur_token->kind != T_eof) {
        switch (cur_token->kind) {
        case T_cppd_define: {
            token_t *prev_define_token = prev_token,
                    *replcaement_head = NULL, *replcaement_tail = NULL, *replacement_cur;

            macro = calloc(1, sizeof(macro_nt));
            lex_ident_token(T_identifier, macro->name);
            
            while (!lex_peek_token(T_newline)) {
                if (lex_accept_token(T_backslash))
                    continue;
                
                replacement_cur = arena_calloc(TOKEN_ARENA, 1, sizeof(token_t));
                lex_next_token();
                memcpy(replacement_cur, cur_token, sizeof(token_t));

                if (!replcaement_head) {
                    replcaement_head = replacement_cur;
                    replcaement_tail = replcaement_head;
                } else {
                    replcaement_tail->next = replacement_cur;
                    replcaement_tail = replacement_cur;
                }

                replacement_cur->next = NULL;
            }

            lex_expect_token(T_newline);
            macro->replacement = replcaement_head;
            hashmap_put(MACROS, macro->name, macro);

            if (prev_define_token)
                prev_define_token->next = cur_token;
            else
                head_token = cur_token;
            break;
        }
        case T_identifier: {
            macro = hashmap_get(MACROS, cur_token->literal);

            if (!macro)
                break;

            if (macro->is_disabled)
                break;

            if (cur_token->next && cur_token->next->kind == T_open_bracket) {
                /* Check if it's an function-like macro invocation */
            } else {
                expand_obj_macro(macro);
                continue;
            }
            break;
        }
        default:
            break;
        }

        lex_next_token();
    }
}

token_t *preprocess(token_t *token)
{
    MACROS = hashmap_create(8);

    head_token = token;
    cur_token = token;
    preprocess_internal();

    return head_token;
}
