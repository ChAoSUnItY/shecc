/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the
 * file "LICENSE" for information on usage and redistribution of this file.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "../config"
#include "defs.h"
#include "globals.c"

/* C language syntactic analyzer */
int global_var_idx = 0;
int global_label_idx = 0;
char global_str_buf[MAX_VAR_LEN];

char *gen_name()
{
    sprintf(global_str_buf, ".t%d", global_var_idx++);
    return global_str_buf;
}

char *gen_label()
{
    sprintf(global_str_buf, ".label.%d", global_label_idx++);
    return global_str_buf;
}

var_t *require_var(block_t *blk)
{
    if (blk->next_local >= MAX_LOCALS)
        error("Too many locals");

    var_t *var = &blk->locals[blk->next_local++];
    var->consumed = -1;
    var->base = var;
    return var;
}

/* stack of the operands of 3AC */
var_t *operand_stack[MAX_OPERAND_STACK_SIZE];
int operand_stack_idx = 0;

void opstack_push(var_t *var)
{
    operand_stack[operand_stack_idx++] = var;
}

var_t *opstack_pop()
{
    return operand_stack[--operand_stack_idx];
}

void read_expr(block_t *parent, basic_block_t **bb);

int write_symbol(char *data, int len)
{
    int startLen = elf_data_idx;
    elf_write_data_str(data, len);
    return startLen;
}

int get_size(var_t *var, type_t *type)
{
    if (var->is_ptr || var->is_func)
        return PTR_SIZE;
    return type->size;
}

int get_operator_prio(opcode_t op)
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

int get_unary_operator_prio(opcode_t op)
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

opcode_t get_operator()
{
    opcode_t op = OP_generic;
    if (lex_accept(T_plus))
        op = OP_add;
    else if (lex_accept(T_minus))
        op = OP_sub;
    else if (lex_accept(T_asterisk))
        op = OP_mul;
    else if (lex_accept(T_divide))
        op = OP_div;
    else if (lex_accept(T_mod))
        op = OP_mod;
    else if (lex_accept(T_lshift))
        op = OP_lshift;
    else if (lex_accept(T_rshift))
        op = OP_rshift;
    else if (lex_accept(T_log_and))
        op = OP_log_and;
    else if (lex_accept(T_log_or))
        op = OP_log_or;
    else if (lex_accept(T_eq))
        op = OP_eq;
    else if (lex_accept(T_noteq))
        op = OP_neq;
    else if (lex_accept(T_lt))
        op = OP_lt;
    else if (lex_accept(T_le))
        op = OP_leq;
    else if (lex_accept(T_gt))
        op = OP_gt;
    else if (lex_accept(T_ge))
        op = OP_geq;
    else if (lex_accept(T_ampersand))
        op = OP_bit_and;
    else if (lex_accept(T_bit_or))
        op = OP_bit_or;
    else if (lex_accept(T_bit_xor))
        op = OP_bit_xor;
    else if (lex_peek(T_question, NULL))
        op = OP_ternary;
    return op;
}

int read_numeric_constant(char buffer[])
{
    int i = 0;
    int value = 0;
    while (buffer[i]) {
        if (i == 1 && (buffer[i] == 'x')) { /* hexadecimal */
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
        if (buffer[0] == '0') /* octal */
            value = value * 8 + buffer[i++] - '0';
        else
            value = value * 10 + buffer[i++] - '0';
    }
    return value;
}

int read_constant_expr_operand()
{
    char buffer[MAX_ID_LEN];
    int value;

    if (lex_peek(T_numeric, buffer)) {
        lex_expect(T_numeric);
        return read_numeric_constant(buffer);
    }

    if (lex_accept(T_open_bracket)) {
        value = read_constant_expr_operand();
        lex_expect(T_close_bracket);
        return value;
    }

    if (lex_peek(T_identifier, buffer) && !strcmp(buffer, "defined")) {
        char lookup_alias[MAX_TOKEN_LEN];

        lex_expect(T_identifier); /* defined */
        lex_expect_internal(T_open_bracket, 0);
        lex_ident(T_identifier, lookup_alias);
        lex_expect(T_close_bracket);

        return find_alias(lookup_alias) ? 1 : 0;
    }

    error("Unexpected token while evaluating constant");
    return -1;
}

int read_constant_infix_expr(int precedence)
{
    int lhs, rhs;

    /* Evaluate unary expression first */
    opcode_t op = get_operator();
    int current_precedence = get_unary_operator_prio(op);
    if (current_precedence != 0 && current_precedence >= precedence) {
        lhs = read_constant_infix_expr(current_precedence);

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
        lhs = read_constant_expr_operand();
    }

    while (true) {
        op = get_operator();
        current_precedence = get_operator_prio(op);

        if (current_precedence == 0 || current_precedence <= precedence) {
            break;
        }

        rhs = read_constant_infix_expr(current_precedence);

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
            error("Unexpected infix token while evaluating constant");
        }

        op = get_operator();
    }

    return lhs;
}

int read_constant_expr()
{
    return read_constant_infix_expr(0);
}

/* Skips lines where preprocessor match is false, this will stop once next
 * token is either 'T_cppd_elif', 'T_cppd_else' or 'cppd_endif'.
 */
void cppd_control_flow_skip_lines()
{
    while (!lex_peek(T_cppd_elif, NULL) && !lex_peek(T_cppd_else, NULL) &&
           !lex_peek(T_cppd_endif, NULL)) {
        next_token = lex_token();
    }
    skip_whitespace();
}

void check_def(char *alias, bool expected)
{
    if ((find_alias(alias) != NULL) == expected)
        preproc_match = true;
}

void read_defined_macro()
{
    char lookup_alias[MAX_TOKEN_LEN];

    lex_expect(T_identifier); /* defined */
    lex_expect_internal(T_open_bracket, 0);
    lex_ident(T_identifier, lookup_alias);
    lex_expect(T_close_bracket);

    check_def(lookup_alias, true);
}

/* read preprocessor directive at each potential positions: e.g.,
 * global statement / body statement
 */
bool read_preproc_directive()
{
    char token[MAX_ID_LEN];

    if (lex_peek(T_cppd_include, token)) {
        lex_expect(T_cppd_include);

        /* Basic #define syntax validation */
        if (lex_peek(T_string, NULL)) {
            /* #define "header.h" */
            lex_expect(T_string);
        } else {
            /* #define <stdlib.h> */
            lex_expect(T_lt);

            while (!lex_peek(T_gt, NULL)) {
                next_token = lex_token();
            }

            lex_expect(T_gt);
        }

        return true;
    }
    if (lex_accept(T_cppd_define)) {
        char alias[MAX_VAR_LEN];
        char value[MAX_VAR_LEN];

        lex_ident_internal(T_identifier, alias, false);

        if (lex_peek(T_numeric, value)) {
            lex_expect(T_numeric);
            add_alias(alias, value);
        } else if (lex_peek(T_string, value)) {
            lex_expect(T_string);
            add_alias(alias, value);
        } else if (lex_peek(T_identifier, value)) {
            lex_expect(T_identifier);
            add_alias(alias, value);
        } else if (lex_accept(T_open_bracket)) { /* function-like macro */
            macro_t *macro = add_macro(alias);

            skip_newline = false;
            while (lex_peek(T_identifier, alias)) {
                lex_expect(T_identifier);
                strcpy(macro->param_defs[macro->num_param_defs++].var_name,
                       alias);
                lex_accept(T_comma);
            }
            if (lex_accept(T_elipsis))
                macro->is_variadic = true;

            macro->start_source_idx = SOURCE->size;
            skip_macro_body();
        } else {
            /* Empty alias, may be dummy alias serves as include guard */
            value[0] = 0;
            add_alias(alias, value);
        }

        return true;
    }
    if (lex_peek(T_cppd_undef, token)) {
        char alias[MAX_VAR_LEN];

        lex_expect_internal(T_cppd_undef, false);
        lex_peek(T_identifier, alias);
        lex_expect(T_identifier);

        remove_alias(alias);
        remove_macro(alias);
        return true;
    }
    if (lex_peek(T_cppd_error, NULL)) {
        int i = 0;
        char error_diagnostic[MAX_LINE_LEN];

        do {
            error_diagnostic[i++] = next_char;
        } while (read_char(false) != '\n');
        error_diagnostic[i] = 0;

        error(error_diagnostic);
    }
    if (lex_accept(T_cppd_if)) {
        preproc_match = read_constant_expr() != 0;

        if (preproc_match) {
            skip_whitespace();
        } else {
            cppd_control_flow_skip_lines();
        }

        return true;
    }
    if (lex_accept(T_cppd_elif)) {
        if (preproc_match) {
            while (!lex_peek(T_cppd_endif, NULL)) {
                next_token = lex_token();
            }
            return true;
        }

        preproc_match = read_constant_expr() != 0;

        if (preproc_match) {
            skip_whitespace();
        } else {
            cppd_control_flow_skip_lines();
        }

        return true;
    }
    if (lex_accept(T_cppd_else)) {
        /* reach here has 2 possible cases:
         * 1. reach #ifdef preprocessor directive
         * 2. conditional expression in #elif is false
         */
        if (!preproc_match) {
            skip_whitespace();
            return true;
        }

        cppd_control_flow_skip_lines();
        return true;
    }
    if (lex_accept(T_cppd_endif)) {
        preproc_match = false;
        skip_whitespace();
        return true;
    }
    if (lex_accept_internal(T_cppd_ifdef, false)) {
        preproc_match = false;
        lex_ident(T_identifier, token);
        check_def(token, true);

        if (preproc_match) {
            skip_whitespace();
            return true;
        }

        cppd_control_flow_skip_lines();
        return true;
    }
    if (lex_accept_internal(T_cppd_ifndef, false)) {
        preproc_match = false;
        lex_ident(T_identifier, token);
        check_def(token, false);

        if (preproc_match) {
            skip_whitespace();
            return true;
        }

        cppd_control_flow_skip_lines();
        return true;
    }
    if (lex_accept_internal(T_cppd_pragma, false)) {
        lex_expect(T_identifier);
        return true;
    }

    return false;
}

void read_parameter_list_decl(func_t *func, int anon);

void read_inner_var_decl(var_t *vd, int anon, int is_param)
{
    vd->init_val = 0;
    vd->is_ptr = 0;

    while (lex_accept(T_asterisk))
        vd->is_ptr++;

    /* is it function pointer declaration? */
    if (lex_accept(T_open_bracket)) {
        func_t func;
        lex_expect(T_asterisk);
        lex_ident(T_identifier, vd->var_name);
        lex_expect(T_close_bracket);
        read_parameter_list_decl(&func, 1);
        vd->is_func = true;
    } else {
        if (anon == 0) {
            lex_ident(T_identifier, vd->var_name);
            if (!lex_peek(T_open_bracket, NULL) && !is_param) {
                if (vd->is_global) {
                    ph1_ir_t *ir = add_global_ir(OP_allocat);
                    ir->src0 = vd;
                    opstack_push(vd);
                } else {
                    ph1_ir_t *ph1_ir;
                    ph1_ir = add_ph1_ir(OP_allocat);
                    ph1_ir->src0 = vd;
                }
            }
        }
        if (lex_accept(T_open_square)) {
            char buffer[10];

            /* array with size */
            if (lex_peek(T_numeric, buffer)) {
                vd->array_size = read_numeric_constant(buffer);
                lex_expect(T_numeric);
            } else {
                /* array without size:
                 * regarded as a pointer although could be nested
                 */
                vd->is_ptr++;
            }
            lex_expect(T_close_square);
        } else {
            vd->array_size = 0;
        }
        vd->is_func = false;
    }
}

/* starting next_token, need to check the type */
void read_full_var_decl(var_t *vd, int anon, int is_param)
{
    lex_accept(T_struct); /* ignore struct definition */
    lex_ident(T_identifier, vd->type_name);
    read_inner_var_decl(vd, anon, is_param);
}

/* starting next_token, need to check the type */
void read_partial_var_decl(var_t *vd, var_t *template)
{
    strcpy(vd->type_name, template->type_name);
    read_inner_var_decl(vd, 0, 0);
}

void read_parameter_list_decl(func_t *func, int anon)
{
    int vn = 0;
    lex_expect(T_open_bracket);
    while (lex_peek(T_identifier, NULL) == 1) {
        read_full_var_decl(&func->param_defs[vn++], anon, 1);
        lex_accept(T_comma);
    }
    func->num_params = vn;

    /* Up to 'MAX_PARAMS' parameters are accepted for the variadic function. */
    if (lex_accept(T_elipsis))
        func->va_args = 1;

    lex_expect(T_close_bracket);
}

void read_literal_param(block_t *parent, basic_block_t *bb)
{
    char literal[MAX_TOKEN_LEN];

    lex_ident(T_string, literal);
    int index = write_symbol(literal, strlen(literal) + 1);

    ph1_ir_t *ph1_ir = add_ph1_ir(OP_load_data_address);
    var_t *vd = require_var(parent);
    strcpy(vd->var_name, gen_name());
    vd->init_val = index;
    ph1_ir->dest = vd;
    opstack_push(vd);
    add_insn(parent, bb, OP_load_data_address, ph1_ir->dest, NULL, NULL, 0,
             NULL);
}

void read_numeric_param(block_t *parent, basic_block_t *bb, int is_neg)
{
    char token[MAX_ID_LEN];
    int value = 0;
    int i = 0;
    char c;

    lex_ident(T_numeric, token);

    if (token[0] == '-') {
        is_neg = 1 - is_neg;
        i++;
    }
    if (token[0] == '0') {
        if (token[1] == 'x') { /* hexdecimal */
            i = 2;
            do {
                c = token[i++];
                if (is_digit(c))
                    c -= '0';
                else {
                    c |= 32; /* convert to lower case */
                    if (c >= 'a' && c <= 'f')
                        c = (c - 'a') + 10;
                    else
                        error("Invalid numeric constant");
                }

                value = (value * 16) + c;
            } while (is_hex(token[i]));
        } else { /* octal */
            do {
                c = token[i++];
                if (c > '7')
                    error("Invalid numeric constant");
                c -= '0';
                value = (value * 8) + c;
            } while (is_digit(token[i]));
        }
    } else {
        do {
            c = token[i++] - '0';
            value = (value * 10) + c;
        } while (is_digit(token[i]));
    }

    if (is_neg)
        value = -value;

    ph1_ir_t *ph1_ir = add_ph1_ir(OP_load_constant);
    var_t *vd = require_var(parent);
    vd->init_val = value;
    strcpy(vd->var_name, gen_name());
    ph1_ir->dest = vd;
    opstack_push(vd);
    add_insn(parent, bb, OP_load_constant, ph1_ir->dest, NULL, NULL, 0, NULL);
}

void read_char_param(block_t *parent, basic_block_t *bb)
{
    char token[5];

    lex_ident(T_char, token);

    ph1_ir_t *ph1_ir = add_ph1_ir(OP_load_constant);
    var_t *vd = require_var(parent);
    vd->init_val = token[0];
    strcpy(vd->var_name, gen_name());
    ph1_ir->dest = vd;
    opstack_push(vd);
    add_insn(parent, bb, OP_load_constant, ph1_ir->dest, NULL, NULL, 0, NULL);
}

void read_logical(opcode_t op,
                  block_t *parent,
                  basic_block_t **bb,
                  char *label_shared);
void read_ternary_operation(block_t *parent, basic_block_t **bb);
void read_func_parameters(block_t *parent, basic_block_t **bb)
{
    int param_num = 0;
    var_t *params[MAX_PARAMS];

    lex_expect(T_open_bracket);
    while (!lex_accept(T_close_bracket)) {
        read_expr(parent, bb);
        read_ternary_operation(parent, bb);

        params[param_num++] = opstack_pop();
        lex_accept(T_comma);
    }
    for (int i = 0; i < param_num; i++) {
        ph1_ir_t *ph1_ir = add_ph1_ir(OP_push);
        ph1_ir->src0 = params[i];
        /* The operand should keep alive before calling function. Pass the
         * number of remained parameters to allocator to extend their liveness.
         */
        add_insn(parent, *bb, OP_push, NULL, ph1_ir->src0, NULL, param_num - i,
                 NULL);
    }
}

void read_func_call(func_t *func, block_t *parent, basic_block_t **bb)
{
    /* direct function call */
    read_func_parameters(parent, bb);

    ph1_ir_t *ph1_ir = add_ph1_ir(OP_call);
    ph1_ir->param_num = func->num_params;
    strcpy(ph1_ir->func_name, func->return_def.var_name);
    add_insn(parent, *bb, OP_call, NULL, NULL, NULL, 0,
             func->return_def.var_name);
}

void read_indirect_call(block_t *parent, basic_block_t **bb)
{
    read_func_parameters(parent, bb);

    ph1_ir_t *ph1_ir = add_ph1_ir(OP_indirect);
    ph1_ir->src0 = opstack_pop();
    add_insn(parent, *bb, OP_indirect, NULL, ph1_ir->src0, NULL, 0, NULL);
}

ph1_ir_t side_effect[10];
int se_idx = 0;

void read_lvalue(lvalue_t *lvalue,
                 var_t *var,
                 block_t *parent,
                 basic_block_t **bb,
                 bool eval,
                 opcode_t op);

/* Maintain a stack of expression values and operators, depending on next
 * operators' priority. Either apply it or operator on stack first.
 */
void read_expr_operand(block_t *parent, basic_block_t **bb)
{
    ph1_ir_t *ph1_ir;
    var_t *vd;
    int is_neg = 0;

    if (lex_accept(T_minus)) {
        is_neg = 1;
        if (lex_peek(T_numeric, NULL) == 0 &&
            lex_peek(T_identifier, NULL) == 0 &&
            lex_peek(T_open_bracket, NULL) == 0) {
            error("Unexpected token after unary minus");
        }
    }

    if (lex_peek(T_string, NULL))
        read_literal_param(parent, *bb);
    else if (lex_peek(T_char, NULL))
        read_char_param(parent, *bb);
    else if (lex_peek(T_numeric, NULL))
        read_numeric_param(parent, *bb, is_neg);
    else if (lex_accept(T_log_not)) {
        read_expr_operand(parent, bb);

        ph1_ir = add_ph1_ir(OP_log_not);
        ph1_ir->src0 = opstack_pop();
        vd = require_var(parent);
        strcpy(vd->var_name, gen_name());
        ph1_ir->dest = vd;
        opstack_push(vd);
        add_insn(parent, *bb, OP_log_not, ph1_ir->dest, ph1_ir->src0, NULL, 0,
                 NULL);
    } else if (lex_accept(T_bit_not)) {
        read_expr_operand(parent, bb);

        ph1_ir = add_ph1_ir(OP_bit_not);
        ph1_ir->src0 = opstack_pop();
        vd = require_var(parent);
        strcpy(vd->var_name, gen_name());
        ph1_ir->dest = vd;
        opstack_push(vd);
        add_insn(parent, *bb, OP_bit_not, ph1_ir->dest, ph1_ir->src0, NULL, 0,
                 NULL);
    } else if (lex_accept(T_ampersand)) {
        char token[MAX_VAR_LEN];
        lvalue_t lvalue;

        lex_peek(T_identifier, token);
        var_t *var = find_var(token, parent);
        read_lvalue(&lvalue, var, parent, bb, false, OP_generic);

        if (!lvalue.is_reference) {
            ph1_ir = add_ph1_ir(OP_address_of);
            ph1_ir->src0 = opstack_pop();
            vd = require_var(parent);
            strcpy(vd->var_name, gen_name());
            ph1_ir->dest = vd;
            opstack_push(vd);
            add_insn(parent, *bb, OP_address_of, ph1_ir->dest, ph1_ir->src0,
                     NULL, 0, NULL);
        }
    } else if (lex_accept(T_asterisk)) {
        /* dereference */
        char token[MAX_VAR_LEN];
        lvalue_t lvalue;

        int open_bracket = lex_accept(T_open_bracket);
        lex_peek(T_identifier, token);
        var_t *var = find_var(token, parent);
        read_lvalue(&lvalue, var, parent, bb, true, OP_generic);
        if (open_bracket)
            lex_expect(T_close_bracket);

        ph1_ir = add_ph1_ir(OP_read);
        ph1_ir->src0 = opstack_pop();
        vd = require_var(parent);
        if (lvalue.is_ptr > 1)
            ph1_ir->size = PTR_SIZE;
        else
            ph1_ir->size = lvalue.type->size;
        strcpy(vd->var_name, gen_name());
        ph1_ir->dest = vd;
        opstack_push(vd);
        add_insn(parent, *bb, OP_read, ph1_ir->dest, ph1_ir->src0, ph1_ir->src1,
                 ph1_ir->size, NULL);
    } else if (lex_accept(T_open_bracket)) {
        read_expr(parent, bb);
        read_ternary_operation(parent, bb);
        lex_expect(T_close_bracket);
    } else if (lex_accept(T_sizeof)) {
        /* TODO: Use more generalized type grammar parsing function to handle
         * type reading
         */
        char token[MAX_TYPE_LEN];
        int ptr_cnt = 0;

        lex_expect(T_open_bracket);
        int find_type_flag = lex_accept(T_struct) ? 2 : 1;
        lex_ident(T_identifier, token);
        type_t *type = find_type(token, find_type_flag);
        if (!type)
            error("Unable to find type");

        while (lex_accept(T_asterisk))
            ptr_cnt++;

        ph1_ir = add_ph1_ir(OP_load_constant);
        vd = require_var(parent);
        vd->init_val = ptr_cnt ? PTR_SIZE : type->size;
        strcpy(vd->var_name, gen_name());
        ph1_ir->dest = vd;
        opstack_push(vd);
        lex_expect(T_close_bracket);
        add_insn(parent, *bb, OP_load_constant, ph1_ir->dest, NULL, NULL, 0,
                 NULL);
    } else {
        /* function call, constant or variable - read token and determine */
        opcode_t prefix_op = OP_generic;
        char token[MAX_ID_LEN];

        if (lex_accept(T_increment))
            prefix_op = OP_add;
        else if (lex_accept(T_decrement))
            prefix_op = OP_sub;

        lex_peek(T_identifier, token);

        /* is a constant or variable? */
        constant_t *con = find_constant(token);
        var_t *var = find_var(token, parent);
        func_t *func = find_func(token);
        int macro_param_idx = find_macro_param_src_idx(token, parent);
        macro_t *mac = find_macro(token);

        if (!strcmp(token, "__VA_ARGS__")) {
            /* 'size' has pointed at the character after __VA_ARGS__ */
            int remainder, t = SOURCE->size;
            macro_t *macro = parent->macro;

            if (!macro)
                error("The '__VA_ARGS__' identifier can only be used in macro");
            if (!macro->is_variadic)
                error("Unexpected identifier '__VA_ARGS__'");

            remainder = macro->num_params - macro->num_param_defs;
            for (int i = 0; i < remainder; i++) {
                SOURCE->size = macro->params[macro->num_params - remainder + i];
                next_char = SOURCE->elements[SOURCE->size];
                next_token = lex_token();
                read_expr(parent, bb);
            }
            SOURCE->size = t;
            next_char = SOURCE->elements[SOURCE->size];
            next_token = lex_token();
        } else if (mac) {
            if (parent->macro)
                error("Nested macro is not yet supported");

            parent->macro = mac;
            mac->num_params = 0;
            lex_expect(T_identifier);

            /* 'size' has pointed at the first parameter */
            while (!lex_peek(T_close_bracket, NULL)) {
                mac->params[mac->num_params++] = SOURCE->size;
                do {
                    next_token = lex_token();
                } while (next_token != T_comma &&
                         next_token != T_close_bracket);
            }
            /* move 'size' to the macro body */
            macro_return_idx = SOURCE->size;
            SOURCE->size = mac->start_source_idx;
            next_char = SOURCE->elements[SOURCE->size];
            lex_expect(T_close_bracket);

            skip_newline = 0;
            read_expr(parent, bb);

            /* cleanup */
            skip_newline = 1;
            parent->macro = NULL;
            macro_return_idx = 0;
        } else if (macro_param_idx) {
            /* "expand" the argument from where it comes from */
            int t = SOURCE->size;
            SOURCE->size = macro_param_idx;
            next_char = SOURCE->elements[SOURCE->size];
            next_token = lex_token();
            read_expr(parent, bb);
            SOURCE->size = t;
            next_char = SOURCE->elements[SOURCE->size];
            next_token = lex_token();
        } else if (con) {
            ph1_ir = add_ph1_ir(OP_load_constant);
            vd = require_var(parent);
            vd->init_val = con->value;
            strcpy(vd->var_name, gen_name());
            ph1_ir->dest = vd;
            opstack_push(vd);
            lex_expect(T_identifier);
            add_insn(parent, *bb, OP_load_constant, ph1_ir->dest, NULL, NULL, 0,
                     NULL);
        } else if (var) {
            /* evalue lvalue expression */
            lvalue_t lvalue;
            read_lvalue(&lvalue, var, parent, bb, true, prefix_op);

            /* is it an indirect call with function pointer? */
            if (lex_peek(T_open_bracket, NULL)) {
                read_indirect_call(parent, bb);

                ph1_ir = add_ph1_ir(OP_func_ret);
                vd = require_var(parent);
                strcpy(vd->var_name, gen_name());
                ph1_ir->dest = vd;
                opstack_push(vd);
                add_insn(parent, *bb, OP_func_ret, ph1_ir->dest, NULL, NULL, 0,
                         NULL);
            }
        } else if (func) {
            lex_expect(T_identifier);

            if (lex_peek(T_open_bracket, NULL)) {
                read_func_call(func, parent, bb);

                ph1_ir = add_ph1_ir(OP_func_ret);
                vd = require_var(parent);
                strcpy(vd->var_name, gen_name());
                ph1_ir->dest = vd;
                opstack_push(vd);
                add_insn(parent, *bb, OP_func_ret, ph1_ir->dest, NULL, NULL, 0,
                         NULL);
            } else {
                /* indirective function pointer assignment */
                vd = require_var(parent);
                vd->is_func = true;
                strcpy(vd->var_name, token);
                opstack_push(vd);
            }
        } else {
            printf("%s\n", token);
            /* unknown expression */
            error("Unrecognized expression token");
        }

        if (is_neg) {
            ph1_ir = add_ph1_ir(OP_negate);
            ph1_ir->src0 = opstack_pop();
            vd = require_var(parent);
            strcpy(vd->var_name, gen_name());
            ph1_ir->dest = vd;
            opstack_push(vd);
            add_insn(parent, *bb, OP_negate, ph1_ir->dest, ph1_ir->src0, NULL,
                     0, NULL);
        }
    }
}

void finalize_logical(opcode_t op,
                      block_t *parent,
                      basic_block_t **bb,
                      char *label_shared,
                      basic_block_t *shared_bb);

bool is_logical(opcode_t op)
{
    return op == OP_log_and || op == OP_log_or;
}

void read_expr(block_t *parent, basic_block_t **bb)
{
    ph1_ir_t *ph1_ir;
    var_t *vd;
    opcode_t oper_stack[10];
    int oper_stack_idx = 0;

    /* These variables used for parsing logical-and/or operation.
     *
     * For the logical-and operation, the false condition code path for
     * testing each operand uses the same code snippet (basic block).
     *
     * Likewise, when testing each operand for the logical-or operation,
     * all of them share a unified code path for the true condition.
     */
    bool has_prev_log_op = false;
    opcode_t prev_log_op = 0, pprev_log_op = 0;
    basic_block_t *log_and_shared_bb = bb_create(parent),
                  *log_or_shared_bb = bb_create(parent);
    char log_and_shared_label[MAX_VAR_LEN];
    char log_or_shared_label[MAX_VAR_LEN];
    strcpy(log_and_shared_label, gen_label());
    strcpy(log_or_shared_label, gen_label());

    read_expr_operand(parent, bb);

    opcode_t op = get_operator();
    if (op == OP_generic || op == OP_ternary)
        return;
    if (is_logical(op)) {
        bb_connect(*bb, op == OP_log_and ? log_and_shared_bb : log_or_shared_bb,
                   op == OP_log_and ? ELSE : THEN);
        read_logical(
            op, parent, bb,
            op == OP_log_and ? log_and_shared_label : log_or_shared_label);
        has_prev_log_op = true;
        prev_log_op = op;
    } else
        oper_stack[oper_stack_idx++] = op;
    read_expr_operand(parent, bb);
    op = get_operator();

    while (op != OP_generic && op != OP_ternary) {
        if (oper_stack_idx > 0) {
            int same = 0;
            do {
                opcode_t top_op = oper_stack[oper_stack_idx - 1];
                if (get_operator_prio(top_op) >= get_operator_prio(op)) {
                    ph1_ir = add_ph1_ir(top_op);
                    ph1_ir->src1 = opstack_pop();
                    ph1_ir->src0 = opstack_pop();
                    vd = require_var(parent);
                    strcpy(vd->var_name, gen_name());
                    ph1_ir->dest = vd;
                    opstack_push(vd);
                    add_insn(parent, *bb, ph1_ir->op, ph1_ir->dest,
                             ph1_ir->src0, ph1_ir->src1, 0, NULL);

                    oper_stack_idx--;
                } else
                    same = 1;
            } while (oper_stack_idx > 0 && same == 0);
        }
        if (is_logical(op)) {
            if (prev_log_op == 0 || prev_log_op == op) {
                bb_connect(
                    *bb,
                    op == OP_log_and ? log_and_shared_bb : log_or_shared_bb,
                    op == OP_log_and ? ELSE : THEN);
                read_logical(op, parent, bb,
                             op == OP_log_and ? log_and_shared_label
                                              : log_or_shared_label);
                prev_log_op = op;
                has_prev_log_op = true;
            } else if (prev_log_op == OP_log_and) {
                /* For example: a && b || c
                 * previous opcode: prev_log_op == OP_log_and
                 * current opcode:  op == OP_log_or
                 * current operand: b
                 *
                 * Finalize the logical-and operation and test the operand
                 * for the following logical-or operation.
                 * */
                finalize_logical(prev_log_op, parent, bb, log_and_shared_label,
                                 log_and_shared_bb);
                log_and_shared_bb = bb_create(parent);
                strcpy(log_and_shared_label, gen_label());

                bb_connect(*bb, log_or_shared_bb, THEN);
                read_logical(op, parent, bb, log_or_shared_label);

                /* Here are two cases to illustrate the following assignments
                 * after finalizing the logical-and operation and testing the
                 * operand for the following logical-or operation.
                 *
                 * 1. a && b || c
                 *    pprev opcode:    pprev_log_op == 0 (no opcode)
                 *    previous opcode: prev_log_op == OP_log_and
                 *    current opcode:  op == OP_log_or
                 *    current operand: b
                 *
                 *    The current opcode should become the previous opcode,
                 * and the pprev opcode remains 0.
                 *
                 * 2. a || b && c || d
                 *    pprev opcode:    pprev_log_op == OP_log_or
                 *    previous opcode: prev_log_op == OP_log_and
                 *    current opcode:  op == OP_log_or
                 *    current operand: b
                 *
                 *    The previous opcode should inherit the pprev opcode, which
                 * is equivalent to inheriting the current opcode because both
                 * of pprev opcode and current opcode are logical-or operator.
                 *
                 *    Thus, pprev opcode is considered used and is cleared to 0.
                 *
                 * Eventually, the current opcode becomes the previous opcode
                 * and pprev opcode is set to 0.
                 * */
                prev_log_op = op;
                pprev_log_op = 0;
            } else {
                /* For example: a || b && c
                 * previous opcode: prev_log_op == OP_log_or
                 * current opcode:  op == OP_log_and
                 * current operand: b
                 *
                 * Using the logical-and operation to test the current
                 * operand instead of using the logical-or operation.
                 *
                 * Then, the previous opcode becomes pprev opcode and
                 * the current opcode becomes the previous opcode.
                 * */
                bb_connect(*bb, log_and_shared_bb, ELSE);
                read_logical(op, parent, bb, log_and_shared_label);
                pprev_log_op = prev_log_op;
                prev_log_op = op;
            }
        } else {
            while (has_prev_log_op &&
                   (get_operator_prio(op) < get_operator_prio(prev_log_op))) {
                /* When encountering an operator with lower priority, conclude
                 * the current logical-and/or and create a new basic block for
                 * next logical-and/or operator.
                 */
                finalize_logical(prev_log_op, parent, bb,
                                 prev_log_op == OP_log_and
                                     ? log_and_shared_label
                                     : log_or_shared_label,
                                 prev_log_op == OP_log_and ? log_and_shared_bb
                                                           : log_or_shared_bb);
                if (prev_log_op == OP_log_and)
                    log_and_shared_bb = bb_create(parent);
                else
                    log_or_shared_bb = bb_create(parent);
                strcpy(prev_log_op == OP_log_and ? log_and_shared_label
                                                 : log_or_shared_label,
                       gen_label());

                /* After finalizing the previous logical-and/or operation,
                 * the prev_log_op should inherit pprev_log_op and continue
                 * to check whether to finalize a logical-and/or operation.
                 * */
                prev_log_op = pprev_log_op;
                has_prev_log_op = prev_log_op != 0;
                pprev_log_op = 0;
            }
        }
        read_expr_operand(parent, bb);
        if (!is_logical(op))
            oper_stack[oper_stack_idx++] = op;
        op = get_operator();
    }

    while (oper_stack_idx > 0) {
        ph1_ir = add_ph1_ir(oper_stack[--oper_stack_idx]);
        ph1_ir->src1 = opstack_pop();
        ph1_ir->src0 = opstack_pop();
        vd = require_var(parent);
        strcpy(vd->var_name, gen_name());
        ph1_ir->dest = vd;
        opstack_push(vd);
        add_insn(parent, *bb, ph1_ir->op, ph1_ir->dest, ph1_ir->src0,
                 ph1_ir->src1, 0, NULL);
    }
    while (has_prev_log_op) {
        finalize_logical(
            prev_log_op, parent, bb,
            prev_log_op == OP_log_and ? log_and_shared_label
                                      : log_or_shared_label,
            prev_log_op == OP_log_and ? log_and_shared_bb : log_or_shared_bb);

        prev_log_op = pprev_log_op;
        has_prev_log_op = prev_log_op != 0;
        pprev_log_op = 0;
    }
}

/* Return the address that an expression points to, or evaluate its value.
 *   x =;
 *   x[<expr>] =;
 *   x[expr].field =;
 *   x[expr]->field =;
 */
void read_lvalue(lvalue_t *lvalue,
                 var_t *var,
                 block_t *parent,
                 basic_block_t **bb,
                 bool eval,
                 opcode_t prefix_op)
{
    ph1_ir_t *ph1_ir;
    var_t *vd;
    bool is_address_got = false;
    bool is_member = false;

    /* already peeked and have the variable */
    lex_expect(T_identifier);

    lvalue->type = find_type(var->type_name, 0);
    lvalue->size = get_size(var, lvalue->type);
    lvalue->is_ptr = var->is_ptr;
    lvalue->is_func = var->is_func;
    lvalue->is_reference = false;

    opstack_push(var);

    if (lex_peek(T_open_square, NULL) || lex_peek(T_arrow, NULL) ||
        lex_peek(T_dot, NULL))
        lvalue->is_reference = true;

    while (lex_peek(T_open_square, NULL) || lex_peek(T_arrow, NULL) ||
           lex_peek(T_dot, NULL)) {
        if (lex_accept(T_open_square)) {
            /* if subscripted member's is not yet resolved, dereference to
             * resolve base address.
             * e.g., dereference of "->" in "data->raw[0]" would be performed
             * here.
             */
            if (lvalue->is_reference && lvalue->is_ptr && is_member) {
                ph1_ir = add_ph1_ir(OP_read);
                ph1_ir->src0 = opstack_pop();
                vd = require_var(parent);
                strcpy(vd->var_name, gen_name());
                ph1_ir->dest = vd;
                opstack_push(vd);
                ph1_ir->size = 4;
                add_insn(parent, *bb, OP_read, ph1_ir->dest, ph1_ir->src0, NULL,
                         ph1_ir->size, NULL);
            }

            /* var must be either a pointer or an array of some type */
            if (var->is_ptr == 0 && var->array_size == 0)
                error("Cannot apply square operator to non-pointer");

            /* if nested pointer, still pointer */
            if (var->is_ptr <= 1 && var->array_size == 0)
                lvalue->size = lvalue->type->size;

            read_expr(parent, bb);

            /* multiply by element size */
            if (lvalue->size != 1) {
                ph1_ir = add_ph1_ir(OP_load_constant);
                vd = require_var(parent);
                vd->init_val = lvalue->size;
                strcpy(vd->var_name, gen_name());
                ph1_ir->dest = vd;
                opstack_push(vd);
                add_insn(parent, *bb, OP_load_constant, ph1_ir->dest, NULL,
                         NULL, 0, NULL);

                ph1_ir = add_ph1_ir(OP_mul);
                ph1_ir->src1 = opstack_pop();
                ph1_ir->src0 = opstack_pop();
                vd = require_var(parent);
                strcpy(vd->var_name, gen_name());
                ph1_ir->dest = vd;
                opstack_push(vd);
                add_insn(parent, *bb, OP_mul, ph1_ir->dest, ph1_ir->src0,
                         ph1_ir->src1, 0, NULL);
            }

            ph1_ir = add_ph1_ir(OP_add);
            ph1_ir->src1 = opstack_pop();
            ph1_ir->src0 = opstack_pop();
            vd = require_var(parent);
            strcpy(vd->var_name, gen_name());
            ph1_ir->dest = vd;
            opstack_push(vd);
            add_insn(parent, *bb, OP_add, ph1_ir->dest, ph1_ir->src0,
                     ph1_ir->src1, 0, NULL);

            lex_expect(T_close_square);
            is_address_got = true;
            is_member = true;
            lvalue->is_reference = true;
        } else {
            char token[MAX_ID_LEN];

            if (lex_accept(T_arrow)) {
                /* resolve where the pointer points at from the calculated
                 * address in a structure.
                 */
                if (is_member) {
                    ph1_ir = add_ph1_ir(OP_read);
                    ph1_ir->src0 = opstack_pop();
                    vd = require_var(parent);
                    strcpy(vd->var_name, gen_name());
                    ph1_ir->dest = vd;
                    opstack_push(vd);
                    ph1_ir->size = 4;
                    add_insn(parent, *bb, OP_read, ph1_ir->dest, ph1_ir->src0,
                             NULL, ph1_ir->size, NULL);
                }
            } else {
                lex_expect(T_dot);

                if (!is_address_got) {
                    ph1_ir = add_ph1_ir(OP_address_of);
                    ph1_ir->src0 = opstack_pop();
                    vd = require_var(parent);
                    strcpy(vd->var_name, gen_name());
                    ph1_ir->dest = vd;
                    opstack_push(vd);
                    add_insn(parent, *bb, OP_address_of, ph1_ir->dest,
                             ph1_ir->src0, NULL, 0, NULL);

                    is_address_got = true;
                }
            }

            lex_ident(T_identifier, token);

            /* change type currently pointed to */
            var = find_member(token, lvalue->type);
            lvalue->type = find_type(var->type_name, 0);
            lvalue->is_ptr = var->is_ptr;
            lvalue->is_func = var->is_func;
            lvalue->size = get_size(var, lvalue->type);

            /* if it is an array, get the address of first element instead of
             * its value.
             */
            if (var->array_size > 0)
                lvalue->is_reference = false;

            /* move pointer to offset of structure */
            ph1_ir = add_ph1_ir(OP_load_constant);
            vd = require_var(parent);
            strcpy(vd->var_name, gen_name());
            vd->init_val = var->offset;
            ph1_ir->dest = vd;
            opstack_push(vd);
            add_insn(parent, *bb, OP_load_constant, ph1_ir->dest, NULL, NULL, 0,
                     NULL);

            ph1_ir = add_ph1_ir(OP_add);
            ph1_ir->src1 = opstack_pop();
            ph1_ir->src0 = opstack_pop();
            vd = require_var(parent);
            strcpy(vd->var_name, gen_name());
            ph1_ir->dest = vd;
            opstack_push(vd);
            add_insn(parent, *bb, OP_add, ph1_ir->dest, ph1_ir->src0,
                     ph1_ir->src1, 0, NULL);

            is_address_got = true;
            is_member = true;
        }
    }

    if (!eval)
        return;

    if (lex_peek(T_plus, NULL) && (var->is_ptr || var->array_size)) {
        while (lex_peek(T_plus, NULL) && (var->is_ptr || var->array_size)) {
            lex_expect(T_plus);
            if (lvalue->is_reference) {
                ph1_ir = add_ph1_ir(OP_read);
                ph1_ir->src0 = opstack_pop();
                vd = require_var(parent);
                ph1_ir->size = lvalue->size;
                strcpy(vd->var_name, gen_name());
                ph1_ir->dest = vd;
                opstack_push(vd);
                add_insn(parent, *bb, OP_read, ph1_ir->dest, ph1_ir->src0, NULL,
                         ph1_ir->size, NULL);
            }

            read_expr_operand(parent, bb);

            lvalue->size = lvalue->type->size;

            if (lvalue->size > 1) {
                ph1_ir = add_ph1_ir(OP_load_constant);
                vd = require_var(parent);
                strcpy(vd->var_name, gen_name());
                vd->init_val = lvalue->size;
                ph1_ir->dest = vd;
                opstack_push(vd);
                add_insn(parent, *bb, OP_load_constant, ph1_ir->dest, NULL,
                         NULL, 0, NULL);

                ph1_ir = add_ph1_ir(OP_mul);
                ph1_ir->src1 = opstack_pop();
                ph1_ir->src0 = opstack_pop();
                vd = require_var(parent);
                strcpy(vd->var_name, gen_name());
                ph1_ir->dest = vd;
                opstack_push(vd);
                add_insn(parent, *bb, OP_mul, ph1_ir->dest, ph1_ir->src0,
                         ph1_ir->src1, 0, NULL);
            }

            ph1_ir = add_ph1_ir(OP_add);
            ph1_ir->src1 = opstack_pop();
            ph1_ir->src0 = opstack_pop();
            vd = require_var(parent);
            strcpy(vd->var_name, gen_name());
            ph1_ir->dest = vd;
            opstack_push(vd);
            add_insn(parent, *bb, OP_add, ph1_ir->dest, ph1_ir->src0,
                     ph1_ir->src1, 0, NULL);
        }
    } else {
        var_t *t;

        /* If operand is a reference, read the value and push to stack
         * for the incoming addition/subtraction. Otherwise, use the
         * top element of stack as the one of operands and the destination.
         */
        if (lvalue->is_reference) {
            ph1_ir = add_ph1_ir(OP_read);
            ph1_ir->src0 = operand_stack[operand_stack_idx - 1];
            t = require_var(parent);
            ph1_ir->size = lvalue->size;
            strcpy(t->var_name, gen_name());
            ph1_ir->dest = t;
            opstack_push(t);
            add_insn(parent, *bb, OP_read, ph1_ir->dest, ph1_ir->src0, NULL,
                     ph1_ir->size, NULL);
        }
        if (prefix_op != OP_generic) {
            ph1_ir = add_ph1_ir(OP_load_constant);
            vd = require_var(parent);
            strcpy(vd->var_name, gen_name());
            vd->init_val = 1;
            ph1_ir->dest = vd;
            opstack_push(vd);
            add_insn(parent, *bb, OP_load_constant, ph1_ir->dest, NULL, NULL, 0,
                     NULL);

            ph1_ir = add_ph1_ir(prefix_op);
            ph1_ir->src1 = opstack_pop();
            if (lvalue->is_reference)
                ph1_ir->src0 = opstack_pop();
            else
                ph1_ir->src0 = operand_stack[operand_stack_idx - 1];
            vd = require_var(parent);
            strcpy(vd->var_name, gen_name());
            ph1_ir->dest = vd;
            add_insn(parent, *bb, ph1_ir->op, ph1_ir->dest, ph1_ir->src0,
                     ph1_ir->src1, 0, NULL);

            if (lvalue->is_reference) {
                ph1_ir = add_ph1_ir(OP_write);
                ph1_ir->src0 = vd;
                ph1_ir->dest = opstack_pop();
                ph1_ir->size = lvalue->size;
                /* The column of arguments of the new insn of 'OP_write' is
                 * different from 'ph1_ir'
                 */
                add_insn(parent, *bb, OP_write, NULL, ph1_ir->dest,
                         ph1_ir->src0, ph1_ir->size, NULL);
            } else {
                ph1_ir = add_ph1_ir(OP_assign);
                ph1_ir->src0 = vd;
                ph1_ir->dest = operand_stack[operand_stack_idx - 1];
                add_insn(parent, *bb, OP_assign, ph1_ir->dest, ph1_ir->src0,
                         NULL, 0, NULL);
            }
        } else if (lex_peek(T_increment, NULL) || lex_peek(T_decrement, NULL)) {
            side_effect[se_idx].op = OP_load_constant;
            vd = require_var(parent);
            strcpy(vd->var_name, gen_name());
            vd->init_val = 1;
            side_effect[se_idx].dest = vd;
            side_effect[se_idx].src0 = NULL;
            side_effect[se_idx].src1 = NULL;
            se_idx++;

            side_effect[se_idx].op = lex_accept(T_increment) ? OP_add : OP_sub;
            side_effect[se_idx].src1 = vd;
            if (lvalue->is_reference)
                side_effect[se_idx].src0 = opstack_pop();
            else
                side_effect[se_idx].src0 = operand_stack[operand_stack_idx - 1];
            vd = require_var(parent);
            strcpy(vd->var_name, gen_name());
            side_effect[se_idx].dest = vd;
            se_idx++;

            if (lvalue->is_reference) {
                side_effect[se_idx].op = OP_write;
                side_effect[se_idx].src1 = vd;
                side_effect[se_idx].src0 = opstack_pop();
                side_effect[se_idx].size = lvalue->size;
                side_effect[se_idx].dest = NULL;
                opstack_push(t);
                se_idx++;
            } else {
                side_effect[se_idx].op = OP_assign;
                side_effect[se_idx].src0 = vd;
                side_effect[se_idx].dest = operand_stack[operand_stack_idx - 1];
                side_effect[se_idx].src1 = NULL;
                se_idx++;
            }
        } else {
            if (lvalue->is_reference) {
                /* pop the address and keep the read value */
                t = opstack_pop();
                opstack_pop();
                opstack_push(t);
            }
        }
    }
}

void read_logical(opcode_t op,
                  block_t *parent,
                  basic_block_t **bb,
                  char *label_shared)
{
    char __label[MAX_VAR_LEN], *label_true, *label_else;
    var_t *vd;
    ph1_ir_t *ph1_ir;
    strcpy(__label, gen_label());
    if (op == OP_log_and) {
        label_true = __label;
        label_else = label_shared;
    } else if (op == OP_log_or) {
        label_true = label_shared;
        label_else = __label;
    } else
        error("encounter an invalid logical opcode in read_logical()");

    /* Test the operand before the logical-and/or operator */
    ph1_ir = add_ph1_ir(OP_branch);
    ph1_ir->dest = opstack_pop();
    vd = require_var(parent);
    strcpy(vd->var_name, label_true);
    ph1_ir->src0 = vd;
    vd = require_var(parent);
    strcpy(vd->var_name, label_else);
    ph1_ir->src1 = vd;
    add_insn(parent, *bb, OP_branch, NULL, ph1_ir->dest, NULL, 0, NULL);

    /* Create a proper branch label for the operand of the logical-and/or
     * operation.
     * */
    ph1_ir = add_ph1_ir(OP_label);
    vd = require_var(parent);
    strcpy(vd->var_name, op == OP_log_and ? label_true : label_else);
    ph1_ir->src0 = vd;

    basic_block_t *new_bb = bb_create(parent);
    bb_connect(*bb, new_bb, op == OP_log_and ? THEN : ELSE);

    bb[0] = new_bb;
}

void finalize_logical(opcode_t op,
                      block_t *parent,
                      basic_block_t **bb,
                      char *label_shared,
                      basic_block_t *shared_bb)
{
    char __label[MAX_VAR_LEN], label_end[MAX_VAR_LEN], *label_true, *label_else;
    strcpy(__label, gen_label());
    strcpy(label_end, gen_label());

    basic_block_t *then, *then_next, *else_if, *else_bb;
    basic_block_t *end = bb_create(parent);
    if (op == OP_log_and) {
        /* For example: a && b
         *
         * If handling the expression, the basic blocks will
         * connect to each other as the following illustration:
         *
         *  bb1                 bb2                bb3
         * +-----------+       +-----------+       +---------+
         * | teq a, #0 | True  | teq b, #0 | True  | ldr 1   |
         * | bne bb2   | ----> | bne bb3   | ----> | b   bb5 |
         * | b   bb4   |       | b   bb4   |       +---------+
         * +-----------+       +-----------+           |
         *      |                   |                  |
         *      | False             | False            |
         *      |                   |                  |
         *      |              +---------+         +--------+
         *      -------------> | ldr 0   | ------> |        |
         *                     | b   bb5 |         |        |
         *                     +---------+         +--------+
         *                      bb4                 bb5
         *
         * In this case, finalize_logical() should add some
         * instructions to bb2 ~ bb5 and properly connect them
         * to each other.
         *
         * Notice that
         * - bb1 has been handled by read_logical().
         * - bb2 is equivalent to '*bb'.
         * - bb3 needs to be created.
         * - bb4 is 'shared_bb'.
         * - bb5 needs to be created.
         *
         * Thus, here uses 'then', 'then_next', 'else_bb' and
         * 'end' to respectively point to bb2 ~ bb5. Subsequently,
         * perform the mentioned operations for finalizing.
         * */
        then = *bb;
        then_next = bb_create(parent);
        else_bb = shared_bb;
        bb_connect(then, then_next, THEN);
        bb_connect(then, else_bb, ELSE);
        bb_connect(then_next, end, NEXT);
        label_true = __label;
        label_else = label_shared;
    } else if (op == OP_log_or) {
        /* For example: a || b
         *
         * Similar to handling logical-and operations, it should
         * add some instructions to the basic blocks and connect
         * them to each other for logical-or operations as in
         * the figure:
         *
         *  bb1                 bb2                bb3
         * +-----------+       +-----------+       +---------+
         * | teq a, #0 | False | teq b, #0 | False | ldr 0   |
         * | bne bb4   | ----> | bne bb4   | ----> | b   bb5 |
         * | b   bb2   |       | b   bb3   |       +---------+
         * +-----------+       +-----------+           |
         *      |                   |                  |
         *      | True              | True             |
         *      |                   |                  |
         *      |              +---------+         +--------+
         *      -------------> | ldr 1   | ------> |        |
         *                     | b   bb5 |         |        |
         *                     +---------+         +--------+
         *                      bb4                 bb5
         *
         * Similarly, here uses 'else_if', 'else_bb', 'then' and
         * 'end' to respectively point to bb2 ~ bb5, and then
         * finishes the finalization.
         * */
        then = shared_bb;
        else_if = *bb;
        else_bb = bb_create(parent);
        bb_connect(else_if, then, THEN);
        bb_connect(else_if, else_bb, ELSE);
        bb_connect(then, end, NEXT);
        label_true = label_shared;
        label_else = __label;
    } else
        error("encounter an invalid logical opcode in finalize_logical()");
    bb_connect(else_bb, end, NEXT);
    var_t *log_op_res;

    /* Create the branch instruction for final logical-and/or operand */
    ph1_ir_t *ph1_ir = add_ph1_ir(OP_branch);
    var_t *vd = opstack_pop();
    ph1_ir->dest = vd;
    vd = require_var(parent);
    strcpy(vd->var_name, label_true);
    ph1_ir->src0 = vd;
    vd = require_var(parent);
    strcpy(vd->var_name, label_else);
    ph1_ir->src1 = vd;
    add_insn(parent, op == OP_log_and ? then : else_if, OP_branch, NULL,
             ph1_ir->dest, NULL, 0, NULL);

    /*
     * If handling logical-and operation, here creates a true branch for the
     * logical-and operation and assigns a true value.
     *
     * Otherwise, create a false branch and assign a false value for logical-or
     * operation.
     * */
    ph1_ir = add_ph1_ir(OP_label);
    vd = require_var(parent);
    strcpy(vd->var_name, op == OP_log_and ? label_true : label_else);
    ph1_ir->dest = vd;

    ph1_ir = add_ph1_ir(OP_load_constant);
    vd = require_var(parent);
    strcpy(vd->var_name, gen_name());
    vd->init_val = op == OP_log_and;
    ph1_ir->dest = vd;
    add_insn(parent, op == OP_log_and ? then_next : else_bb, OP_load_constant,
             ph1_ir->dest, NULL, NULL, 0, NULL);

    ph1_ir = add_ph1_ir(OP_assign);
    log_op_res = require_var(parent);
    strcpy(log_op_res->var_name, gen_name());
    ph1_ir->dest = log_op_res;
    ph1_ir->src0 = vd;
    add_insn(parent, op == OP_log_and ? then_next : else_bb, OP_assign,
             ph1_ir->dest, ph1_ir->src0, NULL, 0, NULL);

    /* After assigning a value, go to the final basic block. */
    ph1_ir = add_ph1_ir(OP_jump);
    vd = require_var(parent);
    strcpy(vd->var_name, label_end);
    ph1_ir->dest = vd;

    /* Create the shared branch and assign the other value
     * for the other condition of a logical-and/or operation.
     *
     * If handing a logical-and operation, assign a false value.
     * else, assign a true value for a logical-or operation.
     * */
    ph1_ir = add_ph1_ir(OP_label);
    vd = require_var(parent);
    strcpy(vd->var_name, op == OP_log_and ? label_else : label_true);
    ph1_ir->src0 = vd;

    ph1_ir = add_ph1_ir(OP_load_constant);
    vd = require_var(parent);
    strcpy(vd->var_name, gen_name());
    vd->init_val = op != OP_log_and;
    ph1_ir->dest = vd;
    add_insn(parent, op == OP_log_and ? else_bb : then, OP_load_constant,
             ph1_ir->dest, NULL, NULL, 0, NULL);

    ph1_ir = add_ph1_ir(OP_assign);
    ph1_ir->dest = log_op_res;
    ph1_ir->src0 = vd;
    add_insn(parent, op == OP_log_and ? else_bb : then, OP_assign, ph1_ir->dest,
             ph1_ir->src0, NULL, 0, NULL);

    ph1_ir = add_ph1_ir(OP_label);
    vd = require_var(parent);
    strcpy(vd->var_name, label_end);
    ph1_ir->src0 = vd;

    log_op_res->is_logical_ret = true;
    opstack_push(log_op_res);

    bb[0] = end;
}

void read_ternary_operation(block_t *parent, basic_block_t **bb)
{
    char true_label[MAX_VAR_LEN], false_label[MAX_VAR_LEN];
    char end_label[MAX_VAR_LEN];

    strcpy(true_label, gen_label());
    strcpy(false_label, gen_label());
    strcpy(end_label, gen_label());

    if (!lex_accept(T_question))
        return;

    /* ternary-operator */
    ph1_ir_t *ph1_ir = add_ph1_ir(OP_branch);
    ph1_ir->dest = opstack_pop();
    var_t *vd = require_var(parent);
    strcpy(vd->var_name, true_label);
    ph1_ir->src0 = vd;
    vd = require_var(parent);
    strcpy(vd->var_name, false_label);
    ph1_ir->src1 = vd;
    add_insn(parent, *bb, OP_branch, NULL, ph1_ir->dest, NULL, 0, NULL);

    basic_block_t *then_ = bb_create(parent);
    basic_block_t *else_ = bb_create(parent);
    basic_block_t *end_ternary = bb_create(parent);
    bb_connect(then_, end_ternary, NEXT);
    bb_connect(else_, end_ternary, NEXT);

    /* true branch */
    ph1_ir = add_ph1_ir(OP_label);
    vd = require_var(parent);
    strcpy(vd->var_name, true_label);
    ph1_ir->src0 = vd;

    read_expr(parent, &then_);
    bb_connect(*bb, then_, THEN);

    if (!lex_accept(T_colon)) {
        /* ternary operator in standard C needs three operands */
        /* TODO: Release dangling basic block */
        abort();
    }

    ph1_ir = add_ph1_ir(OP_assign);
    ph1_ir->src0 = opstack_pop();
    var_t *var = require_var(parent);
    strcpy(var->var_name, gen_name());
    ph1_ir->dest = var;
    add_insn(parent, then_, OP_assign, ph1_ir->dest, ph1_ir->src0, NULL, 0,
             NULL);

    /* jump true branch to end of expression */
    ph1_ir = add_ph1_ir(OP_jump);
    vd = require_var(parent);
    strcpy(vd->var_name, end_label);
    ph1_ir->dest = vd;

    /* false branch */
    ph1_ir = add_ph1_ir(OP_label);
    vd = require_var(parent);
    strcpy(vd->var_name, false_label);
    ph1_ir->src0 = vd;

    read_expr(parent, &else_);
    bb_connect(*bb, else_, ELSE);

    ph1_ir = add_ph1_ir(OP_assign);
    ph1_ir->src0 = opstack_pop();
    ph1_ir->dest = var;
    add_insn(parent, else_, OP_assign, ph1_ir->dest, ph1_ir->src0, NULL, 0,
             NULL);

    ph1_ir = add_ph1_ir(OP_label);
    vd = require_var(parent);
    strcpy(vd->var_name, end_label);
    ph1_ir->src0 = vd;

    var->is_ternary_ret = true;
    opstack_push(var);
    bb[0] = end_ternary;
}

bool read_body_assignment(char *token,
                          block_t *parent,
                          opcode_t prefix_op,
                          basic_block_t **bb)
{
    var_t *var = find_local_var(token, parent);
    if (!var)
        var = find_global_var(token);

    if (var) {
        ph1_ir_t *ph1_ir;
        int one = 0;
        opcode_t op = OP_generic;
        lvalue_t lvalue;
        int size = 0;

        /* has memory address that we want to set */
        read_lvalue(&lvalue, var, parent, bb, false, OP_generic);
        size = lvalue.size;

        if (lex_accept(T_increment)) {
            op = OP_add;
            one = 1;
        } else if (lex_accept(T_decrement)) {
            op = OP_sub;
            one = 1;
        } else if (lex_accept(T_pluseq)) {
            op = OP_add;
        } else if (lex_accept(T_minuseq)) {
            op = OP_sub;
        } else if (lex_accept(T_asteriskeq)) {
            op = OP_mul;
        } else if (lex_accept(T_divideeq)) {
            op = OP_div;
        } else if (lex_accept(T_modeq)) {
            op = OP_mod;
        } else if (lex_accept(T_lshifteq)) {
            op = OP_lshift;
        } else if (lex_accept(T_rshifteq)) {
            op = OP_rshift;
        } else if (lex_accept(T_xoreq)) {
            op = OP_bit_xor;
        } else if (lex_accept(T_oreq)) {
            op = OP_bit_or;
        } else if (lex_accept(T_andeq)) {
            op = OP_bit_and;
        } else if (lex_peek(T_open_bracket, NULL)) {
            var_t *vd;
            /* dereference lvalue into function address */
            ph1_ir = add_ph1_ir(OP_read);
            ph1_ir->src0 = opstack_pop();
            ph1_ir->size = PTR_SIZE;
            vd = require_var(parent);
            strcpy(vd->var_name, gen_name());
            ph1_ir->dest = vd;
            opstack_push(vd);
            add_insn(parent, *bb, OP_read, ph1_ir->dest, ph1_ir->src0, NULL,
                     PTR_SIZE, NULL);

            read_indirect_call(parent, bb);
            return true;
        } else if (prefix_op == OP_generic) {
            lex_expect(T_assign);
        } else {
            op = prefix_op;
            one = 1;
        }

        if (op != OP_generic) {
            var_t *vd, *t;
            int increment_size = 1;

            /* if we have a pointer, shift it by element size */
            if (lvalue.is_ptr)
                increment_size = lvalue.type->size;

            /* If operand is a reference, read the value and push to stack for
             * the incoming addition/subtraction. Otherwise, use the top element
             * of stack as the one of operands and the destination.
             */
            if (one == 1) {
                if (lvalue.is_reference) {
                    ph1_ir = add_ph1_ir(OP_read);
                    t = opstack_pop();
                    ph1_ir->src0 = t;
                    ph1_ir->size = lvalue.size;
                    vd = require_var(parent);
                    strcpy(vd->var_name, gen_name());
                    ph1_ir->dest = vd;
                    opstack_push(vd);
                    add_insn(parent, *bb, OP_read, ph1_ir->dest, ph1_ir->src0,
                             NULL, lvalue.size, NULL);
                } else
                    t = operand_stack[operand_stack_idx - 1];

                ph1_ir = add_ph1_ir(OP_load_constant);
                vd = require_var(parent);
                strcpy(vd->var_name, gen_name());
                vd->init_val = increment_size;
                ph1_ir->dest = vd;
                add_insn(parent, *bb, OP_load_constant, ph1_ir->dest, NULL,
                         NULL, 0, NULL);

                ph1_ir = add_ph1_ir(op);
                ph1_ir->src1 = vd;
                ph1_ir->src0 = opstack_pop();
                vd = require_var(parent);
                strcpy(vd->var_name, gen_name());
                ph1_ir->dest = vd;
                add_insn(parent, *bb, ph1_ir->op, ph1_ir->dest, ph1_ir->src0,
                         ph1_ir->src1, 0, NULL);

                if (lvalue.is_reference) {
                    ph1_ir = add_ph1_ir(OP_write);
                    ph1_ir->src0 = vd;
                    ph1_ir->dest = t;
                    ph1_ir->size = size;
                    add_insn(parent, *bb, OP_write, NULL, ph1_ir->dest,
                             ph1_ir->src0, size, NULL);
                } else {
                    ph1_ir = add_ph1_ir(OP_assign);
                    ph1_ir->src0 = vd;
                    ph1_ir->dest = t;
                    add_insn(parent, *bb, OP_assign, ph1_ir->dest, ph1_ir->src0,
                             NULL, 0, NULL);
                }
            } else {
                if (lvalue.is_reference) {
                    ph1_ir = add_ph1_ir(OP_read);
                    t = opstack_pop();
                    ph1_ir->src0 = t;
                    vd = require_var(parent);
                    ph1_ir->size = lvalue.size;
                    strcpy(vd->var_name, gen_name());
                    ph1_ir->dest = vd;
                    opstack_push(vd);
                    add_insn(parent, *bb, OP_read, ph1_ir->dest, ph1_ir->src0,
                             NULL, ph1_ir->size, NULL);
                } else
                    t = operand_stack[operand_stack_idx - 1];

                read_expr(parent, bb);

                ph1_ir = add_ph1_ir(OP_load_constant);
                vd = require_var(parent);
                vd->init_val = increment_size;
                strcpy(vd->var_name, gen_name());
                ph1_ir->dest = vd;
                opstack_push(vd);
                add_insn(parent, *bb, OP_load_constant, ph1_ir->dest, NULL,
                         NULL, 0, NULL);

                ph1_ir = add_ph1_ir(OP_mul);
                ph1_ir->src1 = opstack_pop();
                ph1_ir->src0 = opstack_pop();
                vd = require_var(parent);
                strcpy(vd->var_name, gen_name());
                ph1_ir->dest = vd;
                opstack_push(vd);
                add_insn(parent, *bb, OP_mul, ph1_ir->dest, ph1_ir->src0,
                         ph1_ir->src1, 0, NULL);

                ph1_ir = add_ph1_ir(op);
                ph1_ir->src1 = opstack_pop();
                ph1_ir->src0 = opstack_pop();
                vd = require_var(parent);
                strcpy(vd->var_name, gen_name());
                ph1_ir->dest = vd;
                add_insn(parent, *bb, op, ph1_ir->dest, ph1_ir->src0,
                         ph1_ir->src1, 0, NULL);

                if (lvalue.is_reference) {
                    ph1_ir = add_ph1_ir(OP_write);
                    ph1_ir->src0 = vd;
                    ph1_ir->dest = t;
                    ph1_ir->size = lvalue.size;
                    add_insn(parent, *bb, OP_write, NULL, ph1_ir->dest,
                             ph1_ir->src0, lvalue.size, NULL);
                } else {
                    ph1_ir = add_ph1_ir(OP_assign);
                    ph1_ir->src0 = vd;
                    ph1_ir->dest = t;
                    add_insn(parent, *bb, OP_assign, ph1_ir->dest, ph1_ir->src0,
                             NULL, 0, NULL);
                }
            }
        } else {
            read_expr(parent, bb);
            read_ternary_operation(parent, bb);

            if (lvalue.is_func) {
                ph1_ir = add_ph1_ir(OP_write);
                ph1_ir->src0 = opstack_pop();
                ph1_ir->dest = opstack_pop();
                add_insn(parent, *bb, OP_write, NULL, ph1_ir->dest,
                         ph1_ir->src0, PTR_SIZE, NULL);
            } else if (lvalue.is_reference) {
                ph1_ir = add_ph1_ir(OP_write);
                ph1_ir->src0 = opstack_pop();
                ph1_ir->dest = opstack_pop();
                ph1_ir->size = size;
                add_insn(parent, *bb, OP_write, NULL, ph1_ir->dest,
                         ph1_ir->src0, size, NULL);
            } else {
                ph1_ir = add_ph1_ir(OP_assign);
                ph1_ir->src0 = opstack_pop();
                ph1_ir->dest = opstack_pop();
                add_insn(parent, *bb, OP_assign, ph1_ir->dest, ph1_ir->src0,
                         NULL, 0, NULL);
            }
        }
        return true;
    }
    return false;
}

int read_primary_constant()
{
    /* return signed constant */
    int isneg = 0, res;
    char buffer[10];
    if (lex_accept(T_minus))
        isneg = 1;
    if (lex_accept(T_open_bracket)) {
        res = read_primary_constant();
        lex_expect(T_close_bracket);
    } else if (lex_peek(T_numeric, buffer)) {
        res = read_numeric_constant(buffer);
        lex_expect(T_numeric);
    } else if (lex_peek(T_char, buffer)) {
        res = buffer[0];
        lex_expect(T_char);
    } else
        error("Invalid value after assignment");
    if (isneg)
        return (-1) * res;
    return res;
}

int eval_expression_imm(opcode_t op, int op1, int op2)
{
    /* return immediate result */
    int tmp = op2;
    int res = 0;
    switch (op) {
    case OP_add:
        res = op1 + op2;
        break;
    case OP_sub:
        res = op1 - op2;
        break;
    case OP_mul:
        res = op1 * op2;
        break;
    case OP_div:
        res = op1 / op2;
        break;
    case OP_mod:
        /* TODO: provide arithmetic & operation instead of '&=' */
        /* TODO: do optimization for local expression */
        tmp &= (tmp - 1);
        if ((op2 != 0) && (tmp == 0)) {
            res = op1;
            res &= (op2 - 1);
        } else
            res = op1 % op2;
        break;
    case OP_lshift:
        res = op1 << op2;
        break;
    case OP_rshift:
        res = op1 >> op2;
        break;
    case OP_log_and:
        res = op1 && op2;
        break;
    case OP_log_or:
        res = op1 || op2;
        break;
    case OP_eq:
        res = op1 == op2;
        break;
    case OP_neq:
        res = op1 != op2;
        break;
    case OP_lt:
        res = op1 < op2 ? 1 : 0;
        break;
    case OP_gt:
        res = op1 > op2 ? 1 : 0;
        break;
    case OP_leq:
        res = op1 <= op2 ? 1 : 0;
        break;
    case OP_geq:
        res = op1 >= op2 ? 1 : 0;
        break;
    default:
        error("The requested operation is not supported.");
    }
    return res;
}

bool read_global_assignment(char *token);
void eval_ternary_imm(int cond, char *token)
{
    if (cond == 0) {
        while (next_token != T_colon) {
            next_token = lex_token();
        }
        lex_accept(T_colon);
        read_global_assignment(token);
    } else {
        read_global_assignment(token);
        lex_expect(T_colon);
        while (!lex_peek(T_semicolon, NULL)) {
            next_token = lex_token();
        }
    }
}

bool read_global_assignment(char *token)
{
    ph1_ir_t *ph1_ir;
    var_t *vd;
    block_t *parent = BLOCKS.head;

    /* global initialization must be constant */
    var_t *var = find_global_var(token);
    if (var) {
        opcode_t op_stack[10];
        opcode_t op, next_op;
        int val_stack[10];
        int op_stack_index = 0, val_stack_index = 0;
        int operand1, operand2;
        operand1 = read_primary_constant();
        op = get_operator();
        /* only one value after assignment */
        if (op == OP_generic) {
            ph1_ir = add_global_ir(OP_load_constant);
            vd = require_var(parent);
            strcpy(vd->var_name, gen_name());
            vd->init_val = operand1;
            ph1_ir->dest = vd;
            add_insn(parent, GLOBAL_FUNC->bbs, OP_load_constant, ph1_ir->dest,
                     NULL, NULL, 0, NULL);

            ph1_ir = add_global_ir(OP_assign);
            ph1_ir->src0 = vd;
            vd = require_var(parent);
            strcpy(vd->var_name, gen_name());
            ph1_ir->dest = opstack_pop();
            add_insn(parent, GLOBAL_FUNC->bbs, OP_assign, ph1_ir->dest,
                     ph1_ir->src0, NULL, 0, NULL);
            return true;
        }
        if (op == OP_ternary) {
            lex_expect(T_question);
            eval_ternary_imm(operand1, token);
            return true;
        }
        operand2 = read_primary_constant();
        next_op = get_operator();
        if (next_op == OP_generic) {
            /* only two operands, apply and return */
            ph1_ir = add_global_ir(OP_load_constant);
            vd = require_var(parent);
            strcpy(vd->var_name, gen_name());
            vd->init_val = eval_expression_imm(op, operand1, operand2);
            ph1_ir->dest = vd;
            add_insn(parent, GLOBAL_FUNC->bbs, OP_load_constant, ph1_ir->dest,
                     NULL, NULL, 0, NULL);

            ph1_ir = add_global_ir(OP_assign);
            ph1_ir->src0 = vd;
            vd = require_var(parent);
            strcpy(vd->var_name, gen_name());
            ph1_ir->dest = opstack_pop();
            add_insn(parent, GLOBAL_FUNC->bbs, OP_assign, ph1_ir->dest,
                     ph1_ir->src0, NULL, 0, NULL);
            return true;
        }
        if (op == OP_ternary) {
            lex_expect(T_question);
            int cond = eval_expression_imm(op, operand1, operand2);
            eval_ternary_imm(cond, token);
            return true;
        }

        /* using stack if operands more than two */
        op_stack[op_stack_index++] = op;
        op = next_op;
        val_stack[val_stack_index++] = operand1;
        val_stack[val_stack_index++] = operand2;

        while (op != OP_generic && op != OP_ternary) {
            if (op_stack_index > 0) {
                /* we have a continuation, use stack */
                int same_op = 0;
                do {
                    opcode_t stack_op = op_stack[op_stack_index - 1];
                    if (get_operator_prio(stack_op) >= get_operator_prio(op)) {
                        operand1 = val_stack[val_stack_index - 2];
                        operand2 = val_stack[val_stack_index - 1];
                        val_stack_index -= 2;

                        /* apply stack operator and push result back */
                        val_stack[val_stack_index++] =
                            eval_expression_imm(stack_op, operand1, operand2);

                        /* pop op stack */
                        op_stack_index--;
                    } else {
                        same_op = 1;
                    }
                    /* continue util next operation is higher prio */
                } while (op_stack_index > 0 && same_op == 0);
            }
            /* push next operand on stack */
            val_stack[val_stack_index++] = read_primary_constant();
            /* push operator on stack */
            op_stack[op_stack_index++] = op;
            op = get_operator();
        }
        /* unwind stack and apply operations */
        while (op_stack_index > 0) {
            opcode_t stack_op = op_stack[op_stack_index - 1];

            /* pop stack and apply operators */
            operand1 = val_stack[val_stack_index - 2];
            operand2 = val_stack[val_stack_index - 1];
            val_stack_index -= 2;

            /* apply stack operator and push value back on stack */
            val_stack[val_stack_index++] =
                eval_expression_imm(stack_op, operand1, operand2);

            if (op_stack_index == 1) {
                if (op == OP_ternary) {
                    lex_expect(T_question);
                    eval_ternary_imm(val_stack[0], token);
                } else {
                    ph1_ir = add_global_ir(OP_load_constant);
                    vd = require_var(parent);
                    strcpy(vd->var_name, gen_name());
                    vd->init_val = val_stack[0];
                    ph1_ir->dest = vd;
                    add_insn(parent, GLOBAL_FUNC->bbs, OP_load_constant,
                             ph1_ir->dest, NULL, NULL, 0, NULL);

                    ph1_ir = add_global_ir(OP_assign);
                    ph1_ir->src0 = vd;
                    vd = require_var(parent);
                    strcpy(vd->var_name, gen_name());
                    ph1_ir->dest = opstack_pop();
                    add_insn(parent, GLOBAL_FUNC->bbs, OP_assign, ph1_ir->dest,
                             ph1_ir->src0, NULL, 0, NULL);
                }
                return true;
            }

            /* pop op stack */
            op_stack_index--;
        }
        if (op == OP_ternary) {
            lex_expect(T_question);
            eval_ternary_imm(val_stack[0], token);
        } else {
            ph1_ir = add_global_ir(OP_load_constant);
            vd = require_var(parent);
            strcpy(vd->var_name, gen_name());
            vd->init_val = val_stack[0];
            ph1_ir->dest = vd;
            add_insn(parent, GLOBAL_FUNC->bbs, OP_load_constant, ph1_ir->dest,
                     NULL, NULL, 0, NULL);

            ph1_ir = add_global_ir(OP_assign);
            ph1_ir->src0 = vd;
            vd = require_var(parent);
            strcpy(vd->var_name, gen_name());
            ph1_ir->dest = opstack_pop();
            add_insn(parent, GLOBAL_FUNC->bbs, OP_assign, ph1_ir->dest,
                     ph1_ir->src0, NULL, 0, NULL);
        }
        return true;
    }
    return false;
}

var_t *break_exit[MAX_NESTING];
int break_exit_idx = 0;
var_t *continue_pos[MAX_NESTING];
int continue_pos_idx = 0;
basic_block_t *break_bb[MAX_NESTING];
basic_block_t *continue_bb[MAX_NESTING];

void perform_side_effect(block_t *parent, basic_block_t *bb)
{
    for (int i = 0; i < se_idx; i++) {
        ph1_ir_t *ph1_ir = add_ph1_ir(side_effect[i].op);
        memcpy(ph1_ir, &side_effect[i], sizeof(ph1_ir_t));
        add_insn(parent, bb, ph1_ir->op, ph1_ir->dest, ph1_ir->src0,
                 ph1_ir->src1, ph1_ir->size, ph1_ir->func_name);
    }
    se_idx = 0;
}

basic_block_t *read_code_block(func_t *func,
                               macro_t *macro,
                               block_t *parent,
                               basic_block_t *bb);

basic_block_t *read_body_statement(block_t *parent, basic_block_t *bb)
{
    char token[MAX_ID_LEN];
    ph1_ir_t *ph1_ir;
    macro_t *mac;
    func_t *func;
    type_t *type;
    var_t *vd, *var;
    opcode_t prefix_op = OP_generic;

    if (!bb)
        printf("Warning: unreachable code detected\n");

    /* statement can be:
     *   function call, variable declaration, assignment operation,
     *   keyword, block
     */

    if (lex_peek(T_open_curly, NULL))
        return read_code_block(parent->func, parent->macro, parent, bb);

    if (lex_accept(T_return)) {
        /* return void */
        if (lex_accept(T_semicolon)) {
            add_ph1_ir(OP_return);
            add_insn(parent, bb, OP_return, NULL, NULL, NULL, 0, NULL);
            bb_connect(bb, parent->func->exit, NEXT);
            return NULL;
        }

        /* get expression value into return value */
        read_expr(parent, &bb);
        read_ternary_operation(parent, &bb);

        /* apply side effect before function return */
        perform_side_effect(parent, bb);
        lex_expect(T_semicolon);

        ph1_ir = add_ph1_ir(OP_return);
        ph1_ir->src0 = opstack_pop();

        add_insn(parent, bb, OP_return, NULL, ph1_ir->src0, NULL, 0, NULL);
        bb_connect(bb, parent->func->exit, NEXT);
        return NULL;
    }

    if (lex_accept(T_if)) {
        char label_true[MAX_VAR_LEN], label_false[MAX_VAR_LEN];
        char label_endif[MAX_VAR_LEN];
        strcpy(label_true, gen_label());
        strcpy(label_false, gen_label());
        strcpy(label_endif, gen_label());

        basic_block_t *n = bb_create(parent);
        bb_connect(bb, n, NEXT);
        bb = n;

        lex_expect(T_open_bracket);
        read_expr(parent, &bb);
        lex_expect(T_close_bracket);

        ph1_ir = add_ph1_ir(OP_branch);
        ph1_ir->dest = opstack_pop();
        vd = require_var(parent);
        strcpy(vd->var_name, label_true);
        ph1_ir->src0 = vd;
        vd = require_var(parent);
        strcpy(vd->var_name, label_false);
        ph1_ir->src1 = vd;
        /* argument column is different with ph1_ir */
        add_insn(parent, bb, OP_branch, NULL, ph1_ir->dest, NULL, 0, NULL);

        ph1_ir = add_ph1_ir(OP_label);
        vd = require_var(parent);
        strcpy(vd->var_name, label_true);
        ph1_ir->src0 = vd;

        basic_block_t *then_ = bb_create(parent);
        basic_block_t *else_ = bb_create(parent);
        bb_connect(bb, then_, THEN);
        bb_connect(bb, else_, ELSE);

        basic_block_t *then_body = read_body_statement(parent, then_);
        basic_block_t *then_next_ = NULL;
        if (then_body) {
            then_next_ = bb_create(parent);
            bb_connect(then_body, then_next_, NEXT);
        }
        /* if we have an "else" block, jump to finish */
        if (lex_accept(T_else)) {
            /* jump true branch to finish */
            ph1_ir = add_ph1_ir(OP_jump);
            vd = require_var(parent);
            strcpy(vd->var_name, label_endif);
            ph1_ir->dest = vd;

            /* false branch */
            ph1_ir = add_ph1_ir(OP_label);
            vd = require_var(parent);
            strcpy(vd->var_name, label_false);
            ph1_ir->src0 = vd;

            basic_block_t *else_body = read_body_statement(parent, else_);
            basic_block_t *else_next_ = NULL;
            if (else_body) {
                else_next_ = bb_create(parent);
                bb_connect(else_body, else_next_, NEXT);
            }

            ph1_ir = add_ph1_ir(OP_label);
            vd = require_var(parent);
            strcpy(vd->var_name, label_endif);
            ph1_ir->src0 = vd;

            if (then_next_ && else_next_) {
                basic_block_t *next_ = bb_create(parent);
                bb_connect(then_next_, next_, NEXT);
                bb_connect(else_next_, next_, NEXT);
                return next_;
            }

            if (then_next_)
                return then_next_;
            if (else_next_)
                return else_next_;

            return NULL;
        } else {
            /* this is done, and link false jump */
            ph1_ir = add_ph1_ir(OP_label);
            vd = require_var(parent);
            strcpy(vd->var_name, label_false);
            ph1_ir->src0 = vd;

            if (then_next_) {
                bb_connect(else_, then_next_, NEXT);
                return then_next_;
            }
            return else_;
        }
    }

    if (lex_accept(T_while)) {
        var_t *var_continue, *var_break;
        char label_start[MAX_VAR_LEN], label_end[MAX_VAR_LEN];
        char label_body[MAX_VAR_LEN];
        strcpy(label_start, gen_label());
        strcpy(label_body, gen_label());
        strcpy(label_end, gen_label());

        basic_block_t *n = bb_create(parent);
        bb_connect(bb, n, NEXT);
        bb = n;

        continue_bb[continue_pos_idx] = bb;

        ph1_ir = add_ph1_ir(OP_label);
        var_continue = require_var(parent);
        strcpy(var_continue->var_name, label_start);
        ph1_ir->src0 = var_continue;

        continue_pos[continue_pos_idx++] = var_continue;
        var_break = require_var(parent);
        strcpy(var_break->var_name, label_end);
        break_exit[break_exit_idx++] = var_break;

        basic_block_t *cond = bb_create(parent);
        cond = bb;
        lex_expect(T_open_bracket);
        read_expr(parent, &bb);
        lex_expect(T_close_bracket);

        ph1_ir = add_ph1_ir(OP_branch);
        ph1_ir->dest = opstack_pop();
        vd = require_var(parent);
        strcpy(vd->var_name, label_body);
        ph1_ir->src0 = vd;
        vd = require_var(parent);
        strcpy(vd->var_name, label_end);
        ph1_ir->src1 = vd;
        add_insn(parent, bb, OP_branch, NULL, ph1_ir->dest, NULL, 0, NULL);

        ph1_ir = add_ph1_ir(OP_label);
        vd = require_var(parent);
        strcpy(vd->var_name, label_body);
        ph1_ir->src0 = vd;

        basic_block_t *then_ = bb_create(parent);
        basic_block_t *else_ = bb_create(parent);
        bb_connect(bb, then_, THEN);
        bb_connect(bb, else_, ELSE);
        break_bb[break_exit_idx - 1] = else_;

        basic_block_t *body_ = read_body_statement(parent, then_);

        continue_pos_idx--;
        break_exit_idx--;

        /* create exit jump for breaks */
        ph1_ir = add_ph1_ir(OP_jump);
        vd = require_var(parent);
        strcpy(vd->var_name, label_start);
        ph1_ir->dest = vd;

        ph1_ir = add_ph1_ir(OP_label);
        ph1_ir->src0 = var_break;

        /* workaround to keep variables alive */
        var_continue->init_val = ph1_ir_idx - 1;

        /* return, break, continue */
        if (body_)
            bb_connect(body_, cond, NEXT);

        return else_;
    }

    if (lex_accept(T_switch)) {
        int is_default = 0;
        char true_label[MAX_VAR_LEN], false_label[MAX_VAR_LEN];
        strcpy(true_label, gen_label());
        strcpy(false_label, gen_label());

        basic_block_t *n = bb_create(parent);
        bb_connect(bb, n, NEXT);
        bb = n;

        lex_expect(T_open_bracket);
        read_expr(parent, &bb);
        lex_expect(T_close_bracket);

        /* create exit jump for breaks */
        var_t *var_break = require_var(parent);
        break_exit[break_exit_idx++] = var_break;
        basic_block_t *switch_end = bb_create(parent);
        break_bb[break_exit_idx - 1] = switch_end;
        basic_block_t *true_body_ = bb_create(parent);

        lex_expect(T_open_curly);
        while (lex_peek(T_default, NULL) || lex_peek(T_case, NULL)) {
            if (lex_accept(T_default))
                is_default = 1;
            else {
                int case_val;

                lex_accept(T_case);
                if (lex_peek(T_numeric, NULL)) {
                    case_val = read_numeric_constant(token_str);
                    lex_expect(T_numeric); /* already read it */
                } else if (lex_peek(T_char, token)) {
                    case_val = token[0];
                    lex_expect(T_char);
                } else {
                    constant_t *cd = find_constant(token_str);
                    case_val = cd->value;
                    lex_expect(T_identifier); /* already read it */
                }

                ph1_ir = add_ph1_ir(OP_load_constant);
                vd = require_var(parent);
                strcpy(vd->var_name, gen_name());
                vd->init_val = case_val;
                ph1_ir->dest = vd;
                opstack_push(vd);
                add_insn(parent, bb, OP_load_constant, ph1_ir->dest, NULL, NULL,
                         0, NULL);

                ph1_ir = add_ph1_ir(OP_eq);
                vd = require_var(parent);
                strcpy(vd->var_name, gen_name());
                ph1_ir->src0 = opstack_pop();
                ph1_ir->src1 = operand_stack[operand_stack_idx - 1];
                vd = require_var(parent);
                strcpy(vd->var_name, gen_name());
                ph1_ir->dest = vd;
                add_insn(parent, bb, OP_eq, ph1_ir->dest, ph1_ir->src0,
                         ph1_ir->src1, 0, NULL);

                ph1_ir = add_ph1_ir(OP_branch);
                ph1_ir->dest = vd;
                vd = require_var(parent);
                strcpy(vd->var_name, true_label);
                ph1_ir->src0 = vd;
                vd = require_var(parent);
                strcpy(vd->var_name, false_label);
                ph1_ir->src1 = vd;
                add_insn(parent, bb, OP_branch, NULL, ph1_ir->dest, NULL, 0,
                         NULL);
            }
            lex_expect(T_colon);

            if (is_default)
                /* there's no condition if it is a default label */
                bb_connect(bb, true_body_, NEXT);
            else
                bb_connect(bb, true_body_, THEN);

            int control = 0;
            /* body is optional, can be another case */
            if (!is_default && !lex_peek(T_case, NULL) &&
                !lex_peek(T_close_curly, NULL) && !lex_peek(T_default, NULL)) {
                ph1_ir = add_ph1_ir(OP_label);
                vd = require_var(parent);
                strcpy(vd->var_name, true_label);
                ph1_ir->src0 = vd;

                /* only create new true label at the first line of case body */
                strcpy(true_label, gen_label());
            }

            while (!lex_peek(T_case, NULL) && !lex_peek(T_close_curly, NULL) &&
                   !lex_peek(T_default, NULL)) {
                true_body_ = read_body_statement(parent, true_body_);
                control = 1;
            }

            if (control && true_body_) {
                /* Create a new body block for next case, and connect the last
                 * body block which lacks 'break' to it to make that one ignore
                 * the upcoming cases.
                 */
                basic_block_t *n = bb_create(parent);
                bb_connect(true_body_, n, NEXT);
                true_body_ = n;
            }

            ph1_ir = add_ph1_ir(OP_label);
            vd = require_var(parent);
            strcpy(vd->var_name, false_label);
            ph1_ir->src0 = vd;

            if (!lex_peek(T_close_curly, NULL)) {
                if (is_default)
                    error("Label default should be the last one");

                /* create a new conditional block for next case */
                basic_block_t *n = bb_create(parent);
                bb_connect(bb, n, ELSE);
                bb = n;

                /* create a new body block for next case if the last body block
                 * exits 'switch'.
                 */
                if (!true_body_)
                    true_body_ = bb_create(parent);
            } else if (!is_default) {
                /* handle missing default label */
                bb_connect(bb, switch_end, ELSE);
            }

            /* only create new false label at the last line of case body */
            strcpy(false_label, gen_label());
        }

        /* remove the expression in switch() */
        opstack_pop();
        lex_expect(T_close_curly);

        if (true_body_)
            /* if the last label has no explicit break, connect it to the end */
            bb_connect(true_body_, switch_end, NEXT);

        strcpy(var_break->var_name, vd->var_name);
        break_exit_idx--;

        int dangling = 1;
        for (int i = 0; i < MAX_BB_PRED; i++)
            if (switch_end->prev[i].bb)
                dangling = 0;

        if (dangling)
            return NULL;

        return switch_end;
    }

    if (lex_accept(T_break)) {
        ph1_ir = add_ph1_ir(OP_jump);
        ph1_ir->dest = break_exit[break_exit_idx - 1];
        bb_connect(bb, break_bb[break_exit_idx - 1], NEXT);
        lex_expect(T_semicolon);
        return NULL;
    }

    if (lex_accept(T_continue)) {
        ph1_ir = add_ph1_ir(OP_jump);
        ph1_ir->dest = continue_pos[continue_pos_idx - 1];
        bb_connect(bb, continue_bb[continue_pos_idx - 1], NEXT);
        lex_expect(T_semicolon);
        return NULL;
    }

    if (lex_accept(T_for)) {
        var_t *var_condition, *var_break, *var_inc;
        char cond[MAX_VAR_LEN], body[MAX_VAR_LEN], inc[MAX_VAR_LEN],
            end[MAX_VAR_LEN];
        strcpy(cond, gen_label());
        strcpy(body, gen_label());
        strcpy(inc, gen_label());
        strcpy(end, gen_label());

        lex_expect(T_open_bracket);

        /* synthesize for loop block */
        block_t *blk = add_block(parent, parent->func, parent->macro);
        add_ph1_ir(OP_block_start);

        /* setup - execute once */
        basic_block_t *setup = bb_create(blk);
        bb_connect(bb, setup, NEXT);

        if (!lex_accept(T_semicolon)) {
            if (!lex_peek(T_identifier, token))
                error("Unexpected token");

            int find_type_flag = lex_accept(T_struct) ? 2 : 1;
            type = find_type(token, find_type_flag);
            if (type) {
                var = require_var(blk);
                read_full_var_decl(var, 0, 0);
                add_insn(blk, setup, OP_allocat, var, NULL, NULL, 0, NULL);
                add_symbol(setup, var);
                if (lex_accept(T_assign)) {
                    read_expr(blk, &setup);
                    read_ternary_operation(blk, &setup);

                    ph1_ir = add_ph1_ir(OP_assign);
                    ph1_ir->src0 = opstack_pop();
                    ph1_ir->dest = var;
                    add_insn(blk, setup, OP_assign, ph1_ir->dest, ph1_ir->src0,
                             NULL, 0, NULL);
                }
                while (lex_accept(T_comma)) {
                    var_t *nv;

                    /* add sequence point at T_comma */
                    perform_side_effect(blk, setup);

                    /* multiple (partial) declarations */
                    nv = require_var(blk);
                    read_partial_var_decl(nv, var); /* partial */
                    add_insn(blk, setup, OP_allocat, nv, NULL, NULL, 0, NULL);
                    add_symbol(setup, nv);
                    if (lex_accept(T_assign)) {
                        read_expr(blk, &setup);

                        ph1_ir = add_ph1_ir(OP_assign);
                        ph1_ir->src0 = opstack_pop();
                        ph1_ir->dest = nv;
                        add_insn(blk, setup, OP_assign, ph1_ir->dest,
                                 ph1_ir->src0, NULL, 0, NULL);
                    }
                }
            } else {
                read_body_assignment(token, blk, OP_generic, &setup);
            }

            lex_expect(T_semicolon);
        }

        basic_block_t *cond_ = bb_create(blk);
        basic_block_t *for_end = bb_create(parent);
        basic_block_t *cond_start = cond_;
        break_bb[break_exit_idx] = for_end;
        bb_connect(setup, cond_, NEXT);

        /* condition - check before the loop */
        ph1_ir = add_ph1_ir(OP_label);
        var_condition = require_var(blk);
        strcpy(var_condition->var_name, cond);
        ph1_ir->src0 = var_condition;
        if (!lex_accept(T_semicolon)) {
            read_expr(blk, &cond_);
            lex_expect(T_semicolon);
        } else {
            /* always true */
            ph1_ir = add_ph1_ir(OP_load_constant);
            vd = require_var(blk);
            vd->init_val = 1;
            strcpy(vd->var_name, gen_name());
            ph1_ir->dest = vd;
            opstack_push(vd);
            add_insn(blk, cond_, OP_load_constant, ph1_ir->dest, NULL, NULL, 0,
                     NULL);
        }
        bb_connect(cond_, for_end, ELSE);

        ph1_ir = add_ph1_ir(OP_branch);
        ph1_ir->dest = opstack_pop();
        vd = require_var(blk);
        strcpy(vd->var_name, body);
        ph1_ir->src0 = vd;
        vd = require_var(blk);
        strcpy(vd->var_name, end);
        ph1_ir->src1 = vd;
        add_insn(blk, cond_, OP_branch, NULL, ph1_ir->dest, NULL, 0, NULL);

        var_break = require_var(blk);
        strcpy(var_break->var_name, end);

        break_exit[break_exit_idx++] = var_break;

        basic_block_t *inc_ = bb_create(blk);
        continue_bb[continue_pos_idx] = inc_;

        /* increment after each loop */
        ph1_ir = add_ph1_ir(OP_label);
        var_inc = require_var(blk);
        strcpy(var_inc->var_name, inc);
        ph1_ir->src0 = var_inc;

        continue_pos[continue_pos_idx++] = var_inc;

        if (!lex_accept(T_close_bracket)) {
            if (lex_accept(T_increment))
                prefix_op = OP_add;
            else if (lex_accept(T_decrement))
                prefix_op = OP_sub;
            lex_peek(T_identifier, token);
            read_body_assignment(token, blk, prefix_op, &inc_);
            lex_expect(T_close_bracket);
        }

        /* jump back to condition */
        ph1_ir = add_ph1_ir(OP_jump);
        vd = require_var(blk);
        strcpy(vd->var_name, cond);
        ph1_ir->dest = vd;

        /* loop body */
        ph1_ir = add_ph1_ir(OP_label);
        vd = require_var(blk);
        strcpy(vd->var_name, body);
        ph1_ir->src0 = vd;

        basic_block_t *body_ = bb_create(blk);
        bb_connect(cond_, body_, THEN);
        body_ = read_body_statement(blk, body_);

        if (body_) {
            bb_connect(body_, inc_, NEXT);
            bb_connect(inc_, cond_start, NEXT);
        } else if (inc_->insn_list.head) {
            bb_connect(inc_, cond_start, NEXT);
        } else {
            /* TODO: Release dangling inc basic block */
        }

        /* jump to increment */
        ph1_ir = add_ph1_ir(OP_jump);
        vd = require_var(blk);
        strcpy(vd->var_name, inc);
        ph1_ir->dest = vd;

        ph1_ir = add_ph1_ir(OP_label);
        ph1_ir->src0 = var_break;

        var_condition->init_val = ph1_ir_idx - 1;

        continue_pos_idx--;
        break_exit_idx--;
        add_ph1_ir(OP_block_end);
        return for_end;
    }

    if (lex_accept(T_do)) {
        var_t *var_start, *var_condition, *var_break;

        basic_block_t *n = bb_create(parent);
        bb_connect(bb, n, NEXT);
        bb = n;

        basic_block_t *cond_ = bb_create(parent);
        basic_block_t *do_while_end = bb_create(parent);

        ph1_ir = add_ph1_ir(OP_label);
        var_start = require_var(parent);
        strcpy(var_start->var_name, gen_label());
        ph1_ir->src0 = var_start;

        var_condition = require_var(parent);
        strcpy(var_condition->var_name, gen_label());

        continue_bb[continue_pos_idx] = cond_;
        continue_pos[continue_pos_idx++] = var_condition;

        var_break = require_var(parent);
        strcpy(var_break->var_name, gen_label());

        break_bb[break_exit_idx] = do_while_end;
        break_exit[break_exit_idx++] = var_break;

        basic_block_t *do_body = read_body_statement(parent, bb);
        if (do_body)
            bb_connect(do_body, cond_, NEXT);

        lex_expect(T_while);
        lex_expect(T_open_bracket);

        ph1_ir = add_ph1_ir(OP_label);
        vd = require_var(parent);
        strcpy(vd->var_name, var_condition->var_name);
        ph1_ir->src0 = vd;

        read_expr(parent, &cond_);
        lex_expect(T_close_bracket);

        ph1_ir = add_ph1_ir(OP_branch);
        ph1_ir->dest = opstack_pop();
        vd = require_var(parent);
        strcpy(vd->var_name, var_start->var_name);
        ph1_ir->src0 = vd;
        vd = require_var(parent);
        strcpy(vd->var_name, var_break->var_name);
        ph1_ir->src1 = vd;
        add_insn(parent, cond_, OP_branch, NULL, ph1_ir->dest, NULL, 0, NULL);

        ph1_ir = add_ph1_ir(OP_label);
        ph1_ir->src0 = var_break;

        var_start->init_val = ph1_ir_idx - 1;
        lex_expect(T_semicolon);

        for (int i = 0; i < MAX_BB_PRED; i++) {
            if (cond_->prev[i].bb) {
                bb_connect(cond_, bb, THEN);
                bb_connect(cond_, do_while_end, ELSE);
                break;
            }
            /* if breaking out of loop, skip condition block */
        }

        continue_pos_idx--;
        break_exit_idx--;
        add_ph1_ir(OP_block_end);
        return do_while_end;
    }

    /* empty statement */
    if (lex_accept(T_semicolon))
        return bb;

    /* statement with prefix */
    if (lex_accept(T_increment))
        prefix_op = OP_add;
    else if (lex_accept(T_decrement))
        prefix_op = OP_sub;
    /* must be an identifier */
    if (!lex_peek(T_identifier, token))
        error("Unexpected token");

    /* is it a variable declaration? */
    int find_type_flag = lex_accept(T_struct) ? 2 : 1;
    type = find_type(token, find_type_flag);
    if (type) {
        var = require_var(parent);
        read_full_var_decl(var, 0, 0);
        add_insn(parent, bb, OP_allocat, var, NULL, NULL, 0, NULL);
        add_symbol(bb, var);
        if (lex_accept(T_assign)) {
            read_expr(parent, &bb);
            read_ternary_operation(parent, &bb);

            ph1_ir = add_ph1_ir(OP_assign);
            ph1_ir->src0 = opstack_pop();
            ph1_ir->dest = var;
            add_insn(parent, bb, OP_assign, ph1_ir->dest, ph1_ir->src0, NULL, 0,
                     NULL);
        }
        while (lex_accept(T_comma)) {
            var_t *nv;

            /* add sequence point at T_comma */
            perform_side_effect(parent, bb);

            /* multiple (partial) declarations */
            nv = require_var(parent);
            read_partial_var_decl(nv, var); /* partial */
            add_insn(parent, bb, OP_allocat, nv, NULL, NULL, 0, NULL);
            add_symbol(bb, nv);
            if (lex_accept(T_assign)) {
                read_expr(parent, &bb);

                ph1_ir = add_ph1_ir(OP_assign);
                ph1_ir->src0 = opstack_pop();
                ph1_ir->dest = nv;
                add_insn(parent, bb, OP_assign, ph1_ir->dest, ph1_ir->src0,
                         NULL, 0, NULL);
            }
        }
        lex_expect(T_semicolon);
        return bb;
    }

    mac = find_macro(token);
    if (mac) {
        if (parent->macro)
            error("Nested macro is not yet supported");

        parent->macro = mac;
        mac->num_params = 0;
        lex_expect(T_identifier);

        /* 'size' has pointed at the first parameter */
        while (!lex_peek(T_close_bracket, NULL)) {
            mac->params[mac->num_params++] = SOURCE->size;
            do {
                next_token = lex_token();
            } while (next_token != T_comma && next_token != T_close_bracket);
        }
        /* move 'size' to the macro body */
        macro_return_idx = SOURCE->size;
        SOURCE->size = mac->start_source_idx;
        next_char = SOURCE->elements[SOURCE->size];
        lex_expect(T_close_bracket);

        skip_newline = 0;
        bb = read_body_statement(parent, bb);

        /* cleanup */
        skip_newline = 1;
        parent->macro = NULL;
        macro_return_idx = 0;
        return bb;
    }

    /* is a function call? */
    func = find_func(token);
    if (func) {
        lex_expect(T_identifier);
        read_func_call(func, parent, &bb);
        perform_side_effect(parent, bb);
        lex_expect(T_semicolon);
        return bb;
    }

    /* is an assignment? */
    if (read_body_assignment(token, parent, prefix_op, &bb)) {
        perform_side_effect(parent, bb);
        lex_expect(T_semicolon);
        return bb;
    }

    error("Unrecognized statement token");
    return NULL;
}

basic_block_t *read_code_block(func_t *func,
                               macro_t *macro,
                               block_t *parent,
                               basic_block_t *bb)
{
    block_t *blk = add_block(parent, func, macro);
    bb->scope = blk;

    add_ph1_ir(OP_block_start);
    lex_expect(T_open_curly);

    while (!lex_accept(T_close_curly)) {
        if (read_preproc_directive())
            continue;
        bb = read_body_statement(blk, bb);
        perform_side_effect(blk, bb);
    }

    add_ph1_ir(OP_block_end);
    return bb;
}

void var_add_killed_bb(var_t *var, basic_block_t *bb);

void read_func_body(func_t *func)
{
    block_t *blk = add_block(NULL, func, NULL);
    func->bbs = bb_create(blk);
    func->exit = bb_create(blk);

    for (int i = 0; i < func->num_params; i++) {
        /* arguments */
        add_symbol(func->bbs, &func->param_defs[i]);
        func->param_defs[i].base = &func->param_defs[i];
        var_add_killed_bb(&func->param_defs[i], func->bbs);
    }
    basic_block_t *body = read_code_block(func, NULL, NULL, func->bbs);
    if (body)
        bb_connect(body, func->exit, NEXT);
}

/* if first token is type */
void read_global_decl(block_t *block)
{
    var_t *var = require_var(block);
    var->is_global = true;

    /* new function, or variables under parent */
    read_full_var_decl(var, 0, 0);

    if (lex_peek(T_open_bracket, NULL)) {
        /* function */
        func_t *func = add_func(var->var_name, false);
        memcpy(&func->return_def, var, sizeof(var_t));
        block->next_local--;

        read_parameter_list_decl(func, 0);

        if (lex_peek(T_open_curly, NULL)) {
            ph1_ir_t *ph1_ir = add_ph1_ir(OP_define);
            strcpy(ph1_ir->func_name, var->var_name);

            read_func_body(func);
            return;
        }
        if (lex_accept(T_semicolon)) /* forward definition */
            return;
        error("Syntax error in global declaration");
    } else
        add_insn(block, GLOBAL_FUNC->bbs, OP_allocat, var, NULL, NULL, 0, NULL);

    /* is a variable */
    if (lex_accept(T_assign)) {
        if (var->is_ptr == 0 && var->array_size == 0) {
            read_global_assignment(var->var_name);
            lex_expect(T_semicolon);
            return;
        }
        /* TODO: support global initialization for array and pointer */
        error("Global initialization for array and pointer not supported");
    } else if (lex_accept(T_comma))
        /* TODO: continuation */
        error("Global continuation not supported");
    else if (lex_accept(T_semicolon)) {
        opstack_pop();
        return;
    }
    error("Syntax error in global declaration");
}

void read_global_statement()
{
    char token[MAX_ID_LEN];
    block_t *block = BLOCKS.head; /* global block */

    if (lex_accept(T_struct)) {
        int i = 0, size = 0;

        lex_ident(T_identifier, token);

        /* has forward declaration? */
        type_t *type = find_type(token, 2);
        if (!type)
            type = add_type();

        strcpy(type->type_name, token);
        lex_expect(T_open_curly);
        do {
            var_t *v = &type->fields[i++];
            read_full_var_decl(v, 0, 1);
            v->offset = size;
            size += size_var(v);
            lex_expect(T_semicolon);
        } while (!lex_accept(T_close_curly));

        type->size = size;
        type->num_fields = i;
        type->base_type = TYPE_struct;
        lex_expect(T_semicolon);
    } else if (lex_accept(T_typedef)) {
        if (lex_accept(T_enum)) {
            int val = 0;
            type_t *type = add_type();

            type->base_type = TYPE_int;
            type->size = 4;
            lex_expect(T_open_curly);
            do {
                lex_ident(T_identifier, token);
                if (lex_accept(T_assign)) {
                    char value[MAX_ID_LEN];
                    lex_ident(T_numeric, value);
                    val = read_numeric_constant(value);
                }
                add_constant(token, val++);
            } while (lex_accept(T_comma));
            lex_expect(T_close_curly);
            lex_ident(T_identifier, token);
            strcpy(type->type_name, token);
            lex_expect(T_semicolon);
        } else if (lex_accept(T_struct)) {
            int i = 0, size = 0, has_struct_def = 0;
            type_t *tag = NULL, *type = add_type();

            /* is struct definition? */
            if (lex_peek(T_identifier, token)) {
                lex_expect(T_identifier);

                /* is existent? */
                tag = find_type(token, 2);
                if (!tag) {
                    tag = add_type();
                    tag->base_type = TYPE_struct;
                    strcpy(tag->type_name, token);
                }
            }

            /* typedef with struct definition */
            if (lex_accept(T_open_curly)) {
                has_struct_def = 1;
                do {
                    var_t *v = &type->fields[i++];
                    read_full_var_decl(v, 0, 1);
                    v->offset = size;
                    size += size_var(v);
                    lex_expect(T_semicolon);
                } while (!lex_accept(T_close_curly));
            }

            lex_ident(T_identifier, type->type_name);
            type->size = size;
            type->num_fields = i;
            type->base_type = TYPE_typedef;

            if (tag && has_struct_def == 1) {
                strcpy(token, tag->type_name);
                memcpy(tag, type, sizeof(type_t));
                tag->base_type = TYPE_struct;
                strcpy(tag->type_name, token);
            } else {
                /* If it is a forward declaration, build a connection between
                 * structure tag and alias. In 'find_type', it will retrieve
                 * infomation from base structure for alias.
                 */
                type->base_struct = tag;
            }

            lex_expect(T_semicolon);
        } else {
            char base_type[MAX_TYPE_LEN];
            type_t *base;
            type_t *type = add_type();
            lex_ident(T_identifier, base_type);
            base = find_type(base_type, 1);
            if (!base)
                error("Unable to find base type");
            type->base_type = base->base_type;
            type->size = base->size;
            type->num_fields = 0;
            lex_ident(T_identifier, type->type_name);
            lex_expect(T_semicolon);
        }
    } else if (lex_peek(T_identifier, NULL)) {
        read_global_decl(block);
    } else
        error("Syntax error in global statement");
}

void parse_internal()
{
    /* set starting point of global stack manually */
    GLOBAL_FUNC = add_func("", true);
    GLOBAL_FUNC->stack_size = 4;
    GLOBAL_FUNC->bbs = arena_alloc(BB_ARENA, sizeof(basic_block_t));

    /* built-in types */
    type_t *type = add_named_type("void");
    type->base_type = TYPE_void;
    type->size = 0;

    type = add_named_type("char");
    type->base_type = TYPE_char;
    type->size = 1;

    type = add_named_type("int");
    type->base_type = TYPE_int;
    type->size = 4;

    /* builtin type _Bool was introduced in C99 specification, it is more
     * well-known as macro type bool, which is defined in <std_bool.h> (in
     * shecc, it is defined in 'lib/c.c').
     */
    type = add_named_type("_Bool");
    type->base_type = TYPE_char;
    type->size = 1;

    add_block(NULL, NULL, NULL); /* global block */
    elf_add_symbol("", 0, 0);    /* undef symbol */

    /* architecture defines */
    add_alias(ARCH_PREDEFINED, "1");

    /* shecc run-time defines */
    add_alias("__SHECC__", "1");

    /* Linux syscall */
    func_t *func = add_func("__syscall", true);
    func->num_params = 0;
    func->va_args = 1;
    func->bbs = arena_alloc(BB_ARENA, sizeof(basic_block_t));

    /* lexer initialization */
    SOURCE->size = 0;
    next_char = SOURCE->elements[0];
    lex_expect(T_start);

    do {
        if (read_preproc_directive())
            continue;
        read_global_statement();
    } while (!lex_accept(T_eof));
}

/* Load specified source file and referred inclusion recursively */
void load_source_file(char *file)
{
    char buffer[MAX_LINE_LEN];

    FILE *f = fopen(file, "rb");
    if (!f)
        abort();

    for (;;) {
        if (!fgets(buffer, MAX_LINE_LEN, f)) {
            break;
        }
        if (!strncmp(buffer, "#pragma once", 12) &&
            hashmap_contains(INCLUSION_MAP, file)) {
            fclose(f);
            return;
        }
        if (!strncmp(buffer, "#include ", 9) && (buffer[9] == '"')) {
            char path[MAX_LINE_LEN];
            int c = strlen(file) - 1, inclusion_path_len = strlen(buffer) - 11;
            while (c > 0 && file[c] != '/')
                c--;
            if (c) {
                /* prepend directory name */
                snprintf(path, c + 2, "%s", file);
            }

            snprintf(path + c + 1, inclusion_path_len, "%s", buffer + 10);
            load_source_file(path);
        } else {
            source_push_str(SOURCE, buffer);
        }
    }

    hashmap_put(INCLUSION_MAP, file, NULL);
    fclose(f);
}

void parse(char *file)
{
    load_source_file(file);
    parse_internal();
}
