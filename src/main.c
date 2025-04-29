/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the
 * file "LICENSE" for information on usage and redistribution of this file.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Define target machine */
#include "../config"

/* The inclusion must follow the fixed order, otherwise it fails to build. */
#include "defs.h"

/* Initialize global objects */
#include "globals.c"

/* ELF manipulation */
#include "elf.c"

/* C language lexical analyzer */
#include "lexer.c"

/* C language syntactic analyzer */
#include "parser.c"

/* Custom IR generator */
#include "gen.c"

/* architecture-independent middle-end */
#include "ssa.c"

/* Register allocator */
#include "reg-alloc.c"

/* Peephole optimization */
#include "peephole.c"

/* Machine code generation. support ARMv7-A and RV32I */
#include "codegen.c"

/* inlined libc */
#include "../out/libc.inc"

void env_setup()
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
}

int main(int argc, char *argv[])
{
    bool libc = true;
    char *out = NULL, *in = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dump-ir"))
            dump_ir = true;
        else if (!strcmp(argv[i], "+m"))
            hard_mul_div = true;
        else if (!strcmp(argv[i], "--no-libc"))
            libc = false;
        else if (!strcmp(argv[i], "-o")) {
            if (i < argc + 1) {
                out = argv[i + 1];
                i++;
            } else
                /* unsupported options */
                abort();
        } else if (!strcmp(argv[i], "--custom"))
            custom_gen = true;
        else
            in = argv[i];
    }

    if (!in && !custom_gen) {
        printf("Missing source file!\n");
        printf(
            "Usage: shecc [-o output] [+m] [--dump-ir] [--no-libc] "
            "<input.c>\n");
        return -1;
    }

    /* initialize global objects */
    global_init();

    /* include libc */
    if (libc)
        libc_generate();

    env_setup();

    /* load and parse source code into IR */
    if (in || libc)
        parse(in);

    if (custom_gen)
        gen();

    /* dump first phase IR */
    if (dump_ir)
        dump_ph1_ir();

    ssa_build(dump_ir);

    /* SSA-based optimization */
    optimize();

    /* SSA-based liveness analyses */
    liveness_analysis();

    /* allocate register from IR */
    reg_alloc();

    peephole();

    /* flatten CFG to linear instruction */
    cfg_flatten();

    /* dump second phase IR */
    if (dump_ir)
        dump_ph2_ir();

    /* generate code from IR */
    code_generate();

    /* output code in ELF */
    elf_generate(out);

    /* release allocated objects */
    global_release();

    exit(0);
}
