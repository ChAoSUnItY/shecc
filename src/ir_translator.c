#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "defs.h"
#include "globals.c"
#include "lexer_qbesil.c"
#include "parser_qbesil.c"

var_t *init_var(var_t *var)
{
    var->consumed = -1;
    var->base = var;
    return var;
}

int write_literal_symbol(char *data)
{
    int start_len = elf_data->size;
    elf_write_str(elf_data, data);
    elf_write_byte(elf_data, 0);
    return start_len;
}

opcode_t qs_get_binary_op(qs_ir_op_t op)
{
    switch (op) {
    case QS_OP_ADD:
        return OP_add;
    case QS_OP_SUB:
        return OP_sub;
    case QS_OP_MUL:
        return OP_mul;
    case QS_OP_DIV:
        return OP_div;
    case QS_OP_REM:
        return OP_mod;
    case QS_OP_AND:
        return OP_bit_and;
    case QS_OP_OR:
        return OP_bit_or;
    case QS_OP_XOR:
        return OP_bit_xor;
    case QS_OP_SHR:
        return OP_rshift;
    case QS_OP_SHL:
        return OP_lshift;
    case QS_OP_CEQ:
        return OP_eq;
    case QS_OP_CNE:
        return OP_neq;
    case QS_OP_CLT:
        return OP_lt;
    case QS_OP_CLE:
        return OP_leq;
    case QS_OP_CGT:
        return OP_gt;
    case QS_OP_CGE:
        return OP_geq;
    default:
        fatal("Not an valid binary opcode");
        return 0;
    }
}

type_t *qs_convert_type(qs_ir_type_t type)
{
    switch (type) {
    case QS_TY_VOID:
        return TY_void;
    case QS_TY_BYTE:
        return TY_char;
    case QS_TY_WORD:
        return TY_int;
    case QS_TY_NULL:
        fatal("Not an valid type");
    }

    return NULL;
}

var_t *qs_gen_dest(qs_ir_val_t *val, basic_block_t *bb, block_t *blk)
{
    var_t *var;
    char *name = NULL;

    if (val->kind == QS_V_GLOBAL)
        name = trim_sigil(val->global->name);
    else if (val->kind == QS_V_TEMP)
        name = trim_sigil(val->temp->name);

    switch (val->kind) {
    case QS_V_CONST:
        fatal("Constamt cannot be used as destination");
        break;
    case QS_V_GLOBAL:
        var = find_global_var(name);
        if (!var)
            fatal("Unable to find global");
        return var;
    case QS_V_TEMP:
        var = find_local_var(name, blk);
        if (!var) {
            var = require_var(blk);
            strcpy(var->var_name, name);
        }
        return var;
    }

    return NULL;
}

var_t *qs_gen_value(qs_ir_val_t *val, basic_block_t *bb, block_t *blk)
{
    var_t *var;
    char *name = NULL;

    if (val->kind == QS_V_GLOBAL)
        name = trim_sigil(val->global->name);
    else if (val->kind == QS_V_TEMP)
        name = trim_sigil(val->temp->name);

    switch (val->kind) {
    case QS_V_CONST:
        var = require_var(blk);
        gen_name_to(var->var_name);
        var->init_val = val->ival;
        add_insn(blk, bb, OP_load_constant, var, NULL, NULL, 0, NULL);
        return var;
    case QS_V_GLOBAL: {
        var = find_global_var(name);
        if (var)
            return var;
        func_t *func = find_func(name);
        if (func) {
            var = require_var(blk);
            var->is_func = true;
            strcpy(var->var_name, name);
            return var;
        }
        fatal("Unable to find global (or function)");
        break;
    }
    case QS_V_TEMP:
        var = find_local_var(name, blk);
        if (!var) {
            var = require_var(blk);
            strcpy(var->var_name, name);
        }
        return var;
    }

    return NULL;
}

void qs_gen_inst(qs_ir_inst_t *inst, basic_block_t *bb, block_t *blk)
{
    qs_ir_val_t *rs1_val = inst->args, *rs2_val = NULL;
    opcode_t opcode;
    int sz;
    var_t *dest, *rs1, *rs2;

    if (inst->args)
        rs2_val = inst->args->next;

    switch (inst->op) {
    case QS_OP_ADD:
    case QS_OP_SUB:
    case QS_OP_MUL:
    case QS_OP_DIV:
    case QS_OP_REM:
    case QS_OP_AND:
    case QS_OP_OR:
    case QS_OP_XOR:
    case QS_OP_SHR:
    case QS_OP_SHL:
    case QS_OP_CEQ:
    case QS_OP_CNE:
    case QS_OP_CLT:
    case QS_OP_CLE:
    case QS_OP_CGT:
    case QS_OP_CGE:
        opcode = qs_get_binary_op(inst->op);
        dest = qs_gen_dest(inst->dest, bb, blk);
        rs1 = qs_gen_value(rs1_val, bb, blk);
        rs2 = qs_gen_value(rs2_val, bb, blk);

        add_insn(blk, bb, opcode, dest, rs1, rs2, 0, NULL);
        break;
    case QS_OP_NEG:
        dest = qs_gen_dest(inst->dest, bb, blk);
        rs1 = qs_gen_value(rs1_val, bb, blk);

        add_insn(blk, bb, OP_negate, dest, rs1, NULL, 0, NULL);
        break;
    case QS_OP_SAR:
        /* printf("[WARN]: SAR: Opcode not supported, fallback to SHR\n"); */
        dest = qs_gen_dest(inst->dest, bb, blk);
        rs1 = qs_gen_value(rs1_val, bb, blk);
        rs2 = qs_gen_value(rs2_val, bb, blk);

        add_insn(blk, bb, OP_rshift, dest, rs1, rs2, 0, NULL);
        break;
    case QS_OP_ADDR:
        dest = qs_gen_dest(inst->dest, bb, blk);
        rs1 = qs_gen_value(rs1_val, bb, blk);

        add_insn(blk, bb, OP_address_of, dest, rs1, NULL, 0, NULL);
        break;
    case QS_OP_LOADB:
    case QS_OP_LOADW:
        dest = qs_gen_dest(inst->dest, bb, blk);
        rs1 = qs_gen_value(rs1_val, bb, blk);
        sz = inst->op == QS_OP_LOADB ? 1 : 4;

        if (rs1->is_func) {
            var_t *temp_func_var = require_var(blk);
            gen_name_to(temp_func_var->var_name);
            add_insn(blk, bb, OP_address_of, temp_func_var, dest, NULL, 0, NULL);
            add_insn(blk, bb, OP_write, NULL, temp_func_var, rs1, sz, NULL);
            printf("TEMP: %s\n", temp_func_var->var_name);
        } else {
            add_insn(blk, bb, OP_read, dest, rs1, NULL, sz, NULL);
        }
        break;
    case QS_OP_STOREB:
    case QS_OP_STOREW:
        rs1 = qs_gen_value(rs1_val, bb, blk);
        rs2 = qs_gen_value(rs2_val, bb, blk);
        sz = inst->op == QS_OP_STOREB ? 1 : 4;

        add_insn(blk, bb, OP_write, NULL, rs1, rs2, sz, NULL);
        break;
    case QS_OP_ALLOC: {
        char *temp_name = trim_sigil(inst->dest->temp->name);

        if (find_local_var(temp_name, blk)) {
            printf("[INFO]: Shadowing temp variable \"%s\" in function \"%s\" \n", trim_sigil(inst->dest->temp->name), blk->func->return_def.var_name);
            fatal("ALLOC: Attempt to shadow temp variable via alloc");
        }

        if (inst->dest->kind != QS_V_TEMP) {
            fatal("ALLOC: Destination must be temp variable");
        }

        if (rs1_val->kind != QS_V_CONST) {
            fatal("ALLOC: Argument must be constant integer");
        }

        dest = require_var(blk);
        strcpy(dest->var_name, temp_name);

        if (rs1_val->ival == 1) {
            dest->type = TY_char;
        } else if (rs1_val->ival == 4) {
            dest->type = TY_int;
        } else {
            dest->type = TY_char;
            dest->array_size = rs1_val->ival;
        }

        add_insn(blk, bb, OP_allocat, dest, NULL, NULL, 0, NULL);
        add_symbol(bb, dest);
        break;
    }
    case QS_OP_COPY:
        dest = qs_gen_dest(inst->dest, bb, blk);
        rs1 = qs_gen_value(rs1_val, bb, blk);

        add_insn(blk, bb, OP_assign, dest, rs1, NULL, 0, NULL);
        break;
    case QS_OP_CALL: {
        int len = 0;
        var_t *args[MAX_PARAMS];
        qs_ir_val_t *arg = inst->args->next;
        char *name = rs1_val->kind == QS_V_GLOBAL ? trim_sigil(rs1_val->global->name) : trim_sigil(rs1_val->temp->name);
        bool is_fn_ptr = !find_func(name);

        for (int i = 0; arg; arg = arg->next) {
            args[i] = qs_gen_value(arg, bb, blk);
            i++;
            len++;
        }
        
        var_t *indirect_fn_ptr = is_fn_ptr ? qs_gen_value(rs1_val, bb, blk) : NULL;

        for (int i = 0; i < len; i++)
            add_insn(blk, bb, OP_push, NULL, args[i], NULL, len - i - 1, NULL);

        add_insn(blk, bb, is_fn_ptr ? OP_indirect : OP_call, NULL, indirect_fn_ptr, NULL, 0, is_fn_ptr ? NULL : name);

        if (inst->dest) {
            dest = qs_gen_dest(inst->dest, bb, blk);

            add_insn(blk, bb, OP_func_ret, dest, NULL, NULL, 0, NULL);
        }
        break;
    }
    case QS_OP_JMP:
        break;
    case QS_OP_JNZ:
        rs1 = qs_gen_value(rs1_val, bb, blk);

        add_insn(blk, bb, OP_branch, NULL, rs1, NULL, 0, NULL);
        break;
    case QS_OP_RET:
        if (!rs1_val) {
            add_insn(blk, bb, OP_return, NULL, NULL, NULL, 0, NULL);
            break;
        }

        rs1 = qs_gen_value(rs1_val, bb, blk);

        add_insn(blk, bb, OP_return, NULL, rs1, NULL, 0, NULL);
        break;
    default:
        fatal("Unknown opcode");
        break;
    }

    // HACK: Mark short circuited result as logical_ret
    if (!strncmp(bb->bb_label_name, "@L_and_shared", 13) ||
        !strncmp(bb->bb_label_name, "@L_or_shared", 12)) {
        dest->is_logical_ret = true;
    }
}

void qs_gen_block(qs_ir_block_t *ir_blk, block_t *blk)
{
    for (qs_ir_inst_t *in = ir_blk->ins; in; in = in->next) {
        qs_gen_inst(in, ir_blk->bb, blk);
    }
}

void qs_gen_func(qs_ir_func_t *ir_func, char *name)
{
    func_t *func = add_func(name, false);
    strcpy(func->return_def.var_name, name);
    func->return_def.type = qs_convert_type(ir_func->rty);
    init_var(&func->return_def);

    func->stack_size = 4;
    func->num_params = ir_func->nparams;
    for (int j = 0; j < func->num_params; j++) {
        qs_ir_temp_t *temp = &ir_func->temps[j];

        init_var(&func->param_defs[j]);
        strcpy(func->param_defs[j].var_name, trim_sigil(temp->name));
        func->param_defs[j].type = qs_convert_type(temp->type);
    }

    func->va_args = ir_func->variadic;

    qs_ir_block_t *blk = ir_func->blocks;

    if (blk)
        bb_connect(func->bbs, blk->bb, NEXT);

    while (blk) {
        qs_gen_block(blk, blk->bb->scope);
        blk = blk->next;
    }

    if (blk)
        bb_connect(blk->bb, func->exit, NEXT);
}

void qs_gen_data(qs_ir_data_t *ir_data, char *name)
{
    if (ir_data->ndataitem.len == 0) {
        printf("[WARNING]: Empty data item");
        return;
    }

    if (ir_data->ndataitem.len > 1) {
        fatal("DATA_ITEM: Unsupported multiple data item declaration");
        return;
    }

    qs_ir_dataitem_t *data_item = &ir_data->dataitems[0];

    var_t *global_var = require_var(GLOBAL_BLOCK);
    strcpy(global_var->var_name, name);
    global_var->is_global = true;
    add_insn(GLOBAL_BLOCK, GLOBAL_FUNC->bbs, OP_allocat, global_var, NULL, NULL,
             0, NULL);

    switch (data_item->kind) {
    case QS_DI_STR: {
        global_var->type = TY_char;
        global_var->is_ptr = 1;

        var_t *vd = require_typed_ptr_var(GLOBAL_BLOCK, TY_char, 1);
        gen_name_to(vd->var_name);
        vd->init_val = write_literal_symbol(data_item->str);

        add_insn(GLOBAL_BLOCK, GLOBAL_FUNC->bbs, OP_load_data_address, vd, NULL,
                 NULL, 0, NULL);
        add_insn(GLOBAL_BLOCK, GLOBAL_FUNC->bbs, OP_assign, global_var, vd,
                 NULL, 0, NULL);
        break;
    }
    case QS_DI_ZERO: {
        if (data_item->zbytes == 1) {
            global_var->type = TY_char;
        } else if (data_item->zbytes == 4) {
            global_var->type = TY_int;
        } else {
            global_var->type = TY_char;
            global_var->array_size = data_item->zbytes;
        }
        // var_t *vd = require_typed_var(GLOBAL_BLOCK, TY_int);
        // gen_name_to(vd->var_name);
        // vd->init_val = 0;

        // add_insn(GLOBAL_BLOCK, GLOBAL_FUNC->bbs, OP_load_constant, vd, NULL, NULL, 0, NULL);
        // add_insn(GLOBAL_BLOCK, GLOBAL_FUNC->bbs, OP_assign, global_var, vd, NULL, 0, NULL);
        break;
    }
    case QS_DI_CONST: {
        global_var->type = qs_convert_type(data_item->type);
        
        var_t *vd = require_typed_var(GLOBAL_BLOCK, TY_int);
        gen_name_to(vd->var_name);
        vd->init_val = data_item->ival;

        add_insn(GLOBAL_BLOCK, GLOBAL_FUNC->bbs, OP_load_constant, vd, NULL, NULL, 0, NULL);
        add_insn(GLOBAL_BLOCK, GLOBAL_FUNC->bbs, OP_assign, global_var, vd, NULL, 0, NULL);
        break;
    }
    default:
        fatal("DATA_ITEM: Unknown data item kind");
        break;
    }
}

void qs_gen_module(qs_ir_module_t *mod)
{
    char *name;
    func_t *func;

    /* set starting point of global stack manually */
    GLOBAL_FUNC = add_func("", true);
    GLOBAL_FUNC->stack_size = 4;
    GLOBAL_FUNC->bbs = arena_alloc(BB_ARENA, sizeof(basic_block_t));

    /* built-in types */
    TY_void = add_named_type("void");
    TY_void->base_type = TYPE_void;
    TY_void->size = 0;

    TY_char = add_named_type("char");
    TY_char->base_type = TYPE_char;
    TY_char->size = 1;

    TY_int = add_named_type("int");
    TY_int->base_type = TYPE_int;
    TY_int->size = 4;

    /* builtin type _Bool was introduced in C99 specification, it is more
     * well-known as macro type bool, which is defined in <std_bool.h> (in
     * shecc, it is defined in 'lib/c.c').
     */
    TY_bool = add_named_type("_Bool");
    TY_bool->base_type = TYPE_char;
    TY_bool->size = 1;

    /* Linux syscall */
    func = add_func("__syscall", true);
    func->return_def.type = TY_int;
    func->num_params = 0;
    func->va_args = 1;
    func->bbs = arena_alloc(BB_ARENA, sizeof(basic_block_t));

    GLOBAL_BLOCK = add_block(NULL, NULL, NULL);

    for (int i = 0; i < mod->nglobal.len; ++i) {
        name = trim_sigil(mod->globals[i].name);

        if (mod->globals[i].kind == QS_GLOBAL_DATA) {
            qs_gen_data(mod->globals[i].data, name);
        }
        if (mod->globals[i].kind == QS_GLOBAL_FUNC) {
            qs_gen_func(mod->globals[i].func, name);
        }
    }
}
