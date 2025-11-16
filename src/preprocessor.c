/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the
 * file "LICENSE" for information on usage and redistribution of this file.
 */

#include "defs.h"
#include "globals.c"

source_location_t synth_built_in_loc;
hashmap_t *PRAGMA_ONCE;
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

token_t *lex_ident_token(token_t *tk,
                         token_kind_t kind,
                         char *dest,
                         bool skip_space)
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
    token_t *param_names[MAX_PARAMS];
    token_t *replacement;
    bool is_variadic;
    token_t *variadic_tk;
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

hide_set_t *new_hide_set(char *name)
{
    hide_set_t *hs = arena_alloc(TOKEN_ARENA, sizeof(hide_set_t));
    hs->name = name;
    hs->next = NULL;
    return hs;
}

hide_set_t *hide_set_union(hide_set_t *hs1, hide_set_t *hs2)
{
    hide_set_t head;
    hide_set_t *cur = &head;

    for (; hs1; hs1 = hs1->next) {
        cur->next = new_hide_set(hs1->name);
        cur = cur->next;
    }
    for (; hs2; hs2 = hs2->next) {
        cur->next = new_hide_set(hs2->name);
        cur = cur->next;
    }

    return head.next;
}

bool hide_set_contains(hide_set_t *hs, char *name)
{
    for (; hs; hs = hs->next)
        if (!strcmp(hs->name, name))
            return true;
    return false;
}

void hide_set_free(hide_set_t *hs)
{
    for (hide_set_t *tmp; hs;) {
        tmp = hs;
        hs = hs->next;
        free(tmp);
    }
}

typedef enum { CK_if_then, CK_elif_then, CK_else_then } cond_kind_t;

typedef struct cond_incl {
    struct cond_incl *prev;
    cond_kind_t ctx;
    token_t *tk;
    bool included;
} cond_incl_t;

cond_incl_t *push_cond(cond_incl_t *ci, token_t *tk, bool included)
{
    cond_incl_t *cond = arena_alloc(TOKEN_ARENA, sizeof(cond_incl_t));
    cond->prev = ci;
    cond->ctx = CK_if_then;
    cond->tk = tk;
    cond->included = included;
    return cond;
}

typedef struct preprocess_ctx {
    hide_set_t *hide_set;
    hashmap_t *macro_args;
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
        switch (cur->next->kind) {
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
char *token_to_string(token_t *tk, char *dest);

int pp_get_operator_prio(opcode_t op)
{
    /* https://www.cs.uic.edu/~i109/Notes/COperatorPrecedenceTable.pdf */
    switch (op) {
    case OP_ternary:
        return 3;
    case OP_log_or:
        return 4;
    case OP_log_and:
        return 5;
    case OP_bit_or:
        return 6;
    case OP_bit_xor:
        return 7;
    case OP_bit_and:
        return 8;
    case OP_eq:
    case OP_neq:
        return 9;
    case OP_lt:
    case OP_leq:
    case OP_gt:
    case OP_geq:
        return 10;
    case OP_add:
    case OP_sub:
        return 12;
    case OP_mul:
    case OP_div:
    case OP_mod:
        return 13;
    default:
        return 0;
    }
}

int pp_get_unary_operator_prio(opcode_t op)
{
    switch (op) {
    case OP_add:
    case OP_sub:
    case OP_bit_not:
    case OP_log_not:
        return 14;
    default:
        return 0;
    }
}

token_t *pp_get_operator(token_t *tk, opcode_t *op)
{
    switch (tk->kind) {
    case T_plus:
        *op = OP_add;
        break;
    case T_minus:
        *op = OP_sub;
        break;
    case T_asterisk:
        *op = OP_mul;
        break;
    case T_divide:
        *op = OP_div;
        break;
    case T_mod:
        *op = OP_mod;
        break;
    case T_lshift:
        *op = OP_lshift;
        break;
    case T_rshift:
        *op = OP_rshift;
        break;
    case T_log_and:
        *op = OP_log_and;
        break;
    case T_log_or:
        *op = OP_log_or;
        break;
    case T_eq:
        *op = OP_eq;
        break;
    case T_noteq:
        *op = OP_neq;
        break;
    case T_lt:
        *op = OP_lt;
        break;
    case T_le:
        *op = OP_leq;
        break;
    case T_gt:
        *op = OP_gt;
        break;
    case T_ge:
        *op = OP_geq;
        break;
    case T_ampersand:
        *op = OP_bit_and;
        break;
    case T_bit_or:
        *op = OP_bit_or;
        break;
    case T_bit_xor:
        *op = OP_bit_xor;
        break;
    case T_question:
        *op = OP_ternary;
        break;
    default:
        /* Maybe it's an operand, we immediately return here. */
        *op = OP_generic;
        return tk;
    }
    tk = lex_next_token(tk, true);
    return tk;
}

int pp_read_numeric_constant(char buffer[])
{
    int i = 0;
    int value = 0;
    while (buffer[i]) {
        if (i == 1 && (buffer[i] | 32) == 'x') { /* hexadecimal */
            value = 0;
            i = 2;
            while (buffer[i]) {
                char c = buffer[i++];
                value <<= 4;
                if (is_digit(c))
                    value += c - '0';
                c |= 32; /* convert to lower case */
                if (c >= 'a' && c <= 'f')
                    value += (c - 'a') + 10;
            }
            return value;
        }
        if (i == 1 && (buffer[i] | 32) == 'b') { /* binary */
            value = 0;
            i = 2;
            while (buffer[i]) {
                char c = buffer[i++];
                value <<= 1;
                if (c == '1')
                    value += 1;
            }
            return value;
        }
        if (buffer[0] == '0') /* octal */
            value = value * 8 + buffer[i++] - '0';
        else
            value = value * 10 + buffer[i++] - '0';
    }
    return value;
}

token_t *pp_read_constant_expr_operand(token_t *tk, int *val)
{
    if (lex_peek_token(tk, T_numeric, true)) {
        tk = lex_next_token(tk, true);
        *val = pp_read_numeric_constant(tk->literal);
        return tk;
    }

    if (lex_peek_token(tk, T_open_bracket, true)) {
        tk = lex_next_token(tk, true);
        tk = pp_read_constant_expr_operand(tk, val);
        tk = lex_expect_token(tk, T_close_bracket, true);
        return tk;
    }

    if (lex_peek_token(tk, T_identifier, true)) {
        tk = lex_next_token(tk, true);


        if (!strcmp("defined", tk->literal)) {
            macro_nt *macro;
            tk = lex_expect_token(tk, T_open_bracket, true);
            tk = lex_expect_token(tk, T_identifier, true);
            macro = hashmap_get(MACROS, tk->literal);
            *val = macro && !macro->is_disabled;
            tk = lex_expect_token(tk, T_close_bracket, true);
        } else {
            /* Any identifier will fallback and evaluate as 0 */
            *val = 0;
        }

        return tk;
    }

    /* Unable to identify next token, so we advance to next non-whitespace token
     * and report its location with error message.
     */
    tk = lex_next_token(tk, true);
    error_at("Unexpected token while evaluating constant", &tk->location);
    return tk;
}

token_t *pp_read_constant_infix_expr(int precedence, token_t *tk, int *val)
{
    int lhs, rhs;

    /* Evaluate unary expression first */
    opcode_t op;
    tk = pp_get_operator(tk, &op);
    int current_precedence = pp_get_unary_operator_prio(op);
    if (current_precedence != 0 && current_precedence >= precedence) {
        tk = pp_read_constant_infix_expr(current_precedence, tk, &lhs);

        switch (op) {
        case OP_add:
            break;
        case OP_sub:
            lhs = -lhs;
            break;
        case OP_bit_not:
            lhs = ~lhs;
            break;
        case OP_log_not:
            lhs = !lhs;
            break;
        default:
            error("Unexpected unary token while evaluating constant");
        }
    } else {
        tk = pp_read_constant_expr_operand(tk, &lhs);
    }

    while (true) {
        tk = pp_get_operator(tk, &op);
        current_precedence = pp_get_operator_prio(op);

        if (current_precedence == 0 || current_precedence <= precedence) {
            break;
        }

        tk = pp_read_constant_infix_expr(current_precedence, tk, &rhs);

        switch (op) {
        case OP_add:
            lhs += rhs;
            break;
        case OP_sub:
            lhs -= rhs;
            break;
        case OP_mul:
            lhs *= rhs;
            break;
        case OP_div:
            lhs /= rhs;
            break;
        case OP_bit_and:
            lhs &= rhs;
            break;
        case OP_bit_or:
            lhs |= rhs;
            break;
        case OP_bit_xor:
            lhs ^= rhs;
            break;
        case OP_lshift:
            lhs <<= rhs;
            break;
        case OP_rshift:
            lhs >>= rhs;
            break;
        case OP_gt:
            lhs = lhs > rhs;
            break;
        case OP_geq:
            lhs = lhs >= rhs;
            break;
        case OP_lt:
            lhs = lhs < rhs;
            break;
        case OP_leq:
            lhs = lhs <= rhs;
            break;
        case OP_eq:
            lhs = lhs == rhs;
            break;
        case OP_neq:
            lhs = lhs != rhs;
            break;
        case OP_log_and:
            lhs = lhs && rhs;
            break;
        case OP_log_or:
            lhs = lhs || rhs;
            break;
        default:
            error_at("Unexpected infix token while evaluating constant",
                     &tk->location);
        }

        tk = pp_get_operator(tk, &op);
    }

    *val = lhs;
    return tk;
}

token_t *pp_read_constant_expr(token_t *tk, int *val)
{
    return pp_read_constant_infix_expr(0, tk, val);
}

token_t *skip_inner_cond_incl(token_t *tk)
{
    token_kind_t kind;

    while (tk->kind != T_eof) {
        kind = tk->kind;

        if (kind == T_cppd_if || kind == T_cppd_ifdef ||
            kind == T_cppd_ifndef) {
            tk = skip_inner_cond_incl(tk->next->next);
            continue;
        }

        if (kind == T_cppd_endif)
            return tk->next->next;

        tk = tk->next;
    }
    return tk;
}

token_t *skip_cond_incl(token_t *tk)
{
    token_kind_t kind;

    while (tk->kind != T_eof) {
        kind = tk->kind;

        if (kind == T_cppd_if || kind == T_cppd_ifdef ||
            kind == T_cppd_ifndef) {
            tk = skip_inner_cond_incl(tk);
            continue;
        }

        if (kind == T_cppd_elif || kind == T_cppd_else || kind == T_cppd_endif)
            break;

        tk = tk->next;
    }
    return tk;
}

token_t *preprocess_internal(token_t *tk, preprocess_ctx_t *ctx)
{
    token_t head;
    token_t *cur = &head;
    cond_incl_t *ci = NULL;

    while (tk) {
        macro_nt *macro = NULL;

        switch (tk->kind) {
        case T_identifier: {
            token_t *macro_tk = tk;
            preprocess_ctx_t expansion_ctx;
            expansion_ctx.expanded_from =
                ctx->expanded_from ? ctx->expanded_from : tk;
            expansion_ctx.macro_args = ctx->macro_args;
            expansion_ctx.trim_eof = true;

            token_t *macro_arg_replcaement = NULL;

            if (ctx->macro_args)
                macro_arg_replcaement =
                    hashmap_get(ctx->macro_args, tk->literal);

            if (macro_arg_replcaement) {
                /* TODO: We should consider ## here */
                expansion_ctx.hide_set = ctx->hide_set;
                macro_arg_replcaement =
                    preprocess_internal(macro_arg_replcaement, &expansion_ctx);
                cur->next = macro_arg_replcaement;
                cur = expansion_ctx.end_of_token;
                tk = lex_next_token(tk, false);
                continue;
            }

            if (hide_set_contains(ctx->hide_set, tk->literal))
                break;

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

            if (lex_peek_token(tk, T_open_bracket, true)) {
                token_t arg_head;
                token_t *arg_cur = &arg_head;
                int arg_idx = 0;
                int bracket_depth = 0;

                expansion_ctx.hide_set =
                    hide_set_union(ctx->hide_set, new_hide_set(tk->literal));
                expansion_ctx.macro_args = hashmap_create(8);

                tk = lex_next_token(tk, true);
                while (true) {
                    if (lex_peek_token(tk, T_open_bracket, false)) {
                        bracket_depth++;
                    } else if (lex_peek_token(tk, T_close_bracket, false)) {
                        bracket_depth--;
                    }

                    /* Expand arg if needed */
                    if (expansion_ctx.macro_args &&
                        lex_peek_token(tk, T_identifier, false)) {
                        token_t *arg_tk =
                            hashmap_get(ctx->macro_args, tk->next->literal);
                        preprocess_ctx_t arg_expansion_ctx;
                        arg_expansion_ctx.expanded_from = tk->next;
                        arg_expansion_ctx.hide_set = expansion_ctx.hide_set;
                        arg_expansion_ctx.macro_args = NULL;

                        if (arg_tk) {
                            arg_tk =
                                preprocess_internal(arg_tk, &arg_expansion_ctx);
                            tk = lex_next_token(tk, false);
                            arg_cur->next = arg_tk;
                            arg_cur = arg_expansion_ctx.end_of_token;
                            continue;
                        }
                    }

                    if (bracket_depth >= 0 &&
                        !lex_peek_token(tk, T_comma, false) &&
                        !lex_peek_token(tk, T_close_bracket, false)) {
                        tk = lex_next_token(tk, false);
                        arg_cur->next = copy_token(tk);
                        arg_cur = arg_cur->next;
                        continue;
                    }

                    token_t *param_tk;

                    if (arg_idx < macro->param_num) {
                        param_tk = macro->param_names[arg_idx++];
                        hashmap_put(expansion_ctx.macro_args, param_tk->literal,
                                    arg_head.next);
                    } else {
                        if (macro->is_variadic) {
                            param_tk = macro->variadic_tk;

                            if (hashmap_contains(expansion_ctx.macro_args,
                                                 param_tk->literal)) {
                                /* Joins previous token stream with comma
                                 * inserted between */
                                token_t *prev =
                                    hashmap_get(expansion_ctx.macro_args,
                                                param_tk->literal);

                                while (prev->next)
                                    prev = prev->next;

                                /* Borrows parameter's token location */
                                prev->next =
                                    new_token(T_comma, &param_tk->location, 1);
                                prev->next->next = arg_head.next;
                                prev = arg_cur;
                            } else {
                                hashmap_put(expansion_ctx.macro_args,
                                            param_tk->literal, arg_head.next);
                            }
                        } else {
                            error_at(
                                "Too many arguments supplied to macro "
                                "invocation",
                                &macro_tk->location);
                        }
                    }
                    arg_cur = &arg_head;

                    if (lex_peek_token(tk, T_comma, false)) {
                        tk = lex_next_token(tk, false);
                        continue;
                    }

                    if (lex_peek_token(tk, T_close_bracket, false)) {
                        tk = lex_next_token(tk, false);
                        break;
                    }
                }

                if (arg_idx < macro->param_num)
                    error_at("Too few arguments supplied to macro invocation",
                             &macro_tk->location);

                cur->next =
                    preprocess_internal(macro->replacement, &expansion_ctx);
                cur = expansion_ctx.end_of_token;

                hashmap_free(expansion_ctx.macro_args);
            } else {
                expansion_ctx.hide_set =
                    hide_set_union(ctx->hide_set, new_hide_set(tk->literal));
                cur->next =
                    preprocess_internal(macro->replacement, &expansion_ctx);
                cur = expansion_ctx.end_of_token;
            }

            tk = lex_next_token(tk, false);
            continue;
        }
        case T_cppd_include: {
            char inclusion_path[MAX_LINE_LEN];
            token_t *file_tk = NULL;
            preprocess_ctx_t inclusion_ctx;
            inclusion_ctx.hide_set = ctx->hide_set;
            inclusion_ctx.expanded_from = NULL;
            inclusion_ctx.macro_args = NULL;
            inclusion_ctx.trim_eof = true;

            if (lex_peek_token(tk, T_string, true)) {
                tk = lex_next_token(tk, true);
                strcpy(inclusion_path, tk->literal);
            } else {
                int sz = 0;
                char token_buffer[MAX_TOKEN_LEN], *literal;
                tk = lex_expect_token(tk, T_lt, true);

                while (!lex_peek_token(tk, T_gt, false)) {
                    tk = lex_next_token(tk, false);
                    literal = token_to_string(tk, token_buffer);

                    strcpy(inclusion_path + sz, literal);
                    sz += strlen(literal);
                }

                tk = lex_next_token(tk, false);
            }

            tk = lex_expect_token(tk, T_newline, true);
            tk = lex_next_token(tk, false);

            if (hashmap_contains(PRAGMA_ONCE, inclusion_path))
                continue;

            file_tk = lex_token_by_file(inclusion_path);
            cur->next = preprocess_internal(file_tk, &inclusion_ctx);

            cur = inclusion_ctx.end_of_token;
            continue;
        }
        case T_cppd_define: {
            token_t *r_head = NULL, *r_tail = NULL, *r_cur;

            macro = calloc(1, sizeof(macro_nt));
            tk = lex_expect_token(tk, T_identifier, true);
            macro = hashmap_get(MACROS, tk->literal);

            if (!macro) {
                macro = arena_calloc(TOKEN_ARENA, 1, sizeof(macro_nt));
                macro->name = tk->literal;
            } else {
                /* Ensures that #undef effect is overwritten */
                macro->is_disabled = false;
            }

            if (lex_peek_token(tk, T_open_bracket, false)) {
                /* function-like macro */
                tk = lex_next_token(tk, false);
                while (lex_peek_token(tk, T_identifier, true)) {
                    tk = lex_next_token(tk, true);
                    macro->param_names[macro->param_num++] = copy_token(tk);

                    if (lex_peek_token(tk, T_comma, true)) {
                        tk = lex_next_token(tk, true);
                    }
                }

                if (lex_peek_token(tk, T_elipsis, true)) {
                    tk = lex_next_token(tk, true);
                    macro->is_variadic = true;
                    macro->variadic_tk = copy_token(tk);
                    macro->variadic_tk->literal =
                        arena_strdup(TOKEN_ARENA, "__VA_ARGS__");
                }

                tk = lex_expect_token(tk, T_close_bracket, true);
            }

            tk = lex_skip_space(tk);
            while (!lex_peek_token(tk, T_newline, false)) {
                if (lex_peek_token(tk, T_backslash, false)) {
                    tk = lex_expect_token(tk, T_backslash, false);

                    if (!lex_peek_token(tk, T_newline, false))
                        error_at("Backslash and newline must not be separated",
                                 &tk->location);
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
        case T_cppd_undef: {
            tk = lex_expect_token(tk, T_identifier, true);
            macro = hashmap_get(MACROS, tk->literal);

            if (macro) {
                macro->is_disabled = true;
            }

            tk = lex_expect_token(tk, T_newline, true);
            continue;
        }
        case T_cppd_if: {
            token_t *cond_tk = tk;
            int defined;
            tk = pp_read_constant_expr(tk, &defined);
            ci = push_cond(ci, cond_tk, defined);

            if (!defined)
                tk = skip_cond_incl(tk);
            else
                tk = lex_expect_token(tk, T_newline, true);
            continue;
        }
        case T_cppd_ifdef: {
            token_t *cond_tk = tk;
            tk = lex_expect_token(tk, T_identifier, true);
            bool defined = hashmap_contains(MACROS, tk->literal);

            ci = push_cond(ci, cond_tk, defined);
            if (!defined)
                tk = skip_cond_incl(tk);
            else
                tk = lex_expect_token(tk, T_newline, true);
            continue;
        }
        case T_cppd_ifndef: {
            token_t *cond_tk = tk;
            tk = lex_expect_token(tk, T_identifier, true);
            bool defined = hashmap_contains(MACROS, tk->literal);

            ci = push_cond(ci, cond_tk, !defined);
            if (defined)
                tk = skip_cond_incl(tk);
            else
                tk = lex_expect_token(tk, T_newline, true);
            continue;
        }
        case T_cppd_elif: {
            if (!ci || ci->ctx == CK_else_then)
                error_at("Stray #elif", &ci->tk->location);
            int included;
            ci->ctx = CK_elif_then;
            tk = pp_read_constant_expr(tk, &included);

            if (!ci->included && included) {
                ci->included = true;
                tk = lex_expect_token(tk, T_newline, true);
            } else
                tk = skip_cond_incl(tk);
            continue;
        }
        case T_cppd_else: {
            if (!ci || ci->ctx == CK_else_then)
                error_at("Stray #else", &ci->tk->location);
            ci->ctx = CK_else_then;

            if (ci->included)
                tk = skip_cond_incl(tk);
            else
                tk = lex_expect_token(tk, T_newline, true);
            continue;
        }
        case T_cppd_endif: {
            if (!ci)
                error_at("Stray #endif", &tk->location);
            ci = ci->prev;
            tk = lex_expect_token(tk, T_newline, true);
            continue;
        }
        case T_cppd_pragma: {
            if (lex_peek_token(tk, T_identifier, true)) {
                tk = lex_next_token(tk, true);

                if (!strcmp("once", tk->literal))
                    hashmap_put(PRAGMA_ONCE, tk->location.filename, NULL);
            }

            while (!lex_peek_token(tk, T_newline, true))
                tk = lex_next_token(tk, true);

            tk = lex_expect_token(tk, T_newline, true);
            continue;
        }
        case T_cppd_error: {
            if (lex_peek_token(tk, T_string, true)) {
                tk = lex_next_token(tk, true);

                error_at(tk->literal, &tk->location);
            } else {
                error_at(
                    "Internal error, #error does not support non-string error "
                    "message",
                    &tk->location);
            }
            break;
        }
        case T_backslash: {
            /* This branch is designed to be failed since backslash should be
             * consumed by #define, and upon later expansion, it should not be
             * included previously while created by #define.
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

    if (ci)
        error_at("Unterminated conditional directive", &ci->tk->location);

    ctx->end_of_token = cur;
    return head.next;
}

token_t *preprocess(token_t *tk)
{
    preprocess_ctx_t ctx;
    ctx.hide_set = NULL;
    ctx.expanded_from = NULL;
    ctx.macro_args = NULL;
    ctx.trim_eof = false;

    /* Initialize built-in macros */
    PRAGMA_ONCE = hashmap_create(16);
    MACROS = hashmap_create(16);

    synth_built_in_loc.pos = 0;
    synth_built_in_loc.len = 1;
    synth_built_in_loc.column = 1;
    synth_built_in_loc.line = 1;
    synth_built_in_loc.filename = "<built-in>";

    macro_nt *macro = calloc(1, sizeof(macro_nt));
    macro->name = "__FILE__";
    macro->handler = file_macro_handler;
    hashmap_put(MACROS, "__FILE__", macro);

    macro = calloc(1, sizeof(macro_nt));
    macro->name = "__LINE__";
    macro->handler = line_macro_handler;
    hashmap_put(MACROS, "__LINE__", macro);

    macro = calloc(1, sizeof(macro_nt));
    macro->name = "__SHECC__";
    macro->replacement = new_token(T_numeric, &synth_built_in_loc, 1);
    macro->replacement->literal = "1";

    tk = preprocess_internal(tk, &ctx);

    hashmap_free(MACROS);
    hashmap_free(PRAGMA_ONCE);
    return tk;
}

char *token_to_string(token_t *tk, char *dest)
{
    switch (tk->kind) {
    case T_eof:
        return NULL;
    case T_numeric:
        return tk->literal;
    case T_identifier:
        return tk->literal;
    case T_string:
        snprintf(dest, MAX_TOKEN_LEN, "\"%s\"", tk->literal);
        return dest;
    case T_char:
        snprintf(dest, MAX_TOKEN_LEN, "'%s'", tk->literal);
        return dest;
    case T_comma:
        return ",";
    case T_open_bracket:
        return "(";
    case T_close_bracket:
        return ")";
    case T_open_curly:
        return "{";
    case T_close_curly:
        return "}";
    case T_open_square:
        return "[";
    case T_close_square:
        return "]";
    case T_asterisk:
        return "*";
    case T_divide:
        return "/";
    case T_mod:
        return "%";
    case T_bit_or:
        return "|";
    case T_bit_xor:
        return "^";
    case T_bit_not:
        return "~";
    case T_log_and:
        return "&&";
    case T_log_or:
        return "||";
    case T_log_not:
        return "!";
    case T_lt:
        return "<";
    case T_gt:
        return ">";
    case T_le:
        return "<=";
    case T_ge:
        return ">=";
    case T_lshift:
        return "<<";
    case T_rshift:
        return ">>";
    case T_dot:
        return ".";
    case T_arrow:
        return "->";
    case T_plus:
        return "+";
    case T_minus:
        return "-";
    case T_minuseq:
        return "-=";
    case T_pluseq:
        return "+=";
    case T_asteriskeq:
        return "*=";
    case T_divideeq:
        return "/=";
    case T_modeq:
        return "%=";
    case T_lshifteq:
        return "<<=";
    case T_rshifteq:
        return ">>=";
    case T_xoreq:
        return "^=";
    case T_oreq:
        return "|=";
    case T_andeq:
        return "&=";
    case T_eq:
        return "==";
    case T_noteq:
        return "!=";
    case T_assign:
        return "=";
    case T_increment:
        return "++";
    case T_decrement:
        return "--";
    case T_question:
        return "?";
    case T_colon:
        return ":";
    case T_semicolon:
        return ";";
    case T_ampersand:
        return "&";
    case T_return:
        return "return";
    case T_if:
        return "if";
    case T_else:
        return "else";
    case T_while:
        return "while";
    case T_for:
        return "for";
    case T_do:
        return "do";
    case T_typedef:
        return "typedef";
    case T_enum:
        return "enum";
    case T_struct:
        return "struct";
    case T_union:
        return "union";
    case T_sizeof:
        return "sizeof";
    case T_elipsis:
        return "...";
    case T_switch:
        return "switch";
    case T_case:
        return "case";
    case T_break:
        return "break";
    case T_default:
        return "default";
    case T_continue:
        return "continue";
    case T_goto:
        return "goto";
    case T_const:
        return "const";
    case T_newline:
        return "\n";
    case T_backslash:
        error_at(
            "Internal error, backslash should be ommited after "
            "preprocessing",
            &tk->location);
        break;
    case T_whitespace: {
        int i = 0;
        for (; i < tk->location.len; i++)
            dest[i] = ' ';
        dest[i] = '\0';
        return dest;
    }
    case T_tab:
        return "\t";
    case T_start:
        // FIXME: Unused token kind
        break;
    case T_cppd_include:
    case T_cppd_define:
    case T_cppd_undef:
    case T_cppd_error:
    case T_cppd_if:
    case T_cppd_elif:
    case T_cppd_else:
    case T_cppd_endif:
    case T_cppd_ifdef:
    case T_cppd_ifndef:
    case T_cppd_pragma:
        error_at(
            "Internal error, preprocessor directives should be ommited "
            "after preprocessing",
            &tk->location);
        break;
    default:
        error_at("Unknown token kind", &tk->location);
        printf("UNKNOWN_TOKEN");
        break;
    }

    return NULL;
}

void emit_preprocessed_token(token_t *tk)
{
    char token_buffer[MAX_TOKEN_LEN], *literal;

    while (tk) {
        literal = token_to_string(tk, token_buffer);

        if (literal)
            printf("%s", literal);

        tk = tk->next;
    }
}
