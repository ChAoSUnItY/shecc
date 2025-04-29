#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "../config"
#include "defs.h"
#include "globals.c"

var_t *alloc_var(block_t *blk)
{
    if (blk->next_local >= MAX_LOCALS)
        error("Too many locals");

    var_t *var = &blk->locals[blk->next_local++];
    var->consumed = -1;
    var->base = var;
    return var;
}

basic_block_t *gen_function_body(func_t *func,
                                 block_t *parent,
                                 basic_block_t *bb)
{
    block_t *blk = add_block(parent, func, NULL);
    bb->scope = blk;

    /* Returns 0 */
    var_t *vd = alloc_var(blk);
    strcpy(vd->var_name, "tmp");
    vd->init_val = 0;
    add_insn(parent, bb, OP_load_constant, vd, NULL, NULL, 0, NULL);

    add_insn(parent, bb, OP_return, NULL, vd, NULL, 0, NULL);
    bb_connect(bb, parent->func->exit, NEXT);

    return NULL;
}

void gen()
{
    /* Synthesize a main function that returns 0 */
    block_t *global_block = BLOCKS.head;
    var_t *main_def = alloc_var(global_block);
    strcpy(main_def->var_name, "main");
    func_t *func = add_func("main", false);
    memcpy(&func->return_def, main_def, sizeof(var_t));
    global_block->next_local--;

    /* main function's body */
    /* Don't inherit global block as function's block's parent */
    block_t *main_block = add_block(NULL, func, NULL);
    func->bbs = bb_create(main_block);
    func->exit = bb_create(main_block);

    basic_block_t *bb = gen_function_body(func, main_block, func->bbs);

    if (bb)
        bb_connect(bb, func->exit, THEN);
}
