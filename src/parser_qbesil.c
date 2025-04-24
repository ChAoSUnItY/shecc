#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * generic dynamic array implementation supporting multiple data types with
 * arena-based memory management.
 */

typedef struct {
    int len;
    int cap;
    int elem_size;
} qs_dynarr_sz_t;

void *qs_dynarr_reserve(char *data, qs_dynarr_sz_t *sz, int new_cap)
{
    if (new_cap <= sz->cap)
        return data;
    char *new_data = qs_arena_alloc(new_cap * sz->elem_size);
    int real_size = sz->len * sz->elem_size;
    if (data)
        for (int i = 0; i < real_size; ++i)
            new_data[i] = data[i];
    sz->cap = new_cap;
    return new_data;
}

void *qs_dynarr_init(qs_dynarr_sz_t *sz, int init_len, int elem_size)
{
    sz->len = 0;
    sz->cap = 0;
    sz->elem_size = elem_size;
    if (init_len > 0)
        return qs_dynarr_reserve(NULL, sz, init_len);
    return NULL;
}

void *qs_dynarr_push(char *data, qs_dynarr_sz_t *sz, char *elem)
{
    if (sz->len == sz->cap) {
        int new_cap = sz->cap ? sz->cap * 2 : 4;
        data = qs_dynarr_reserve(data, sz, new_cap);
    }
    int real_size = sz->len * sz->elem_size;
    for (int i = 0; i < sz->elem_size; ++i)
        data[real_size + i] = elem[i];
    sz->len++;
    return data;
}

void *qs_dynarr_get(char *data, qs_dynarr_sz_t *sz, int index)
{
    if (index < 0 || index >= sz->len)
        return NULL;
    return data + index * sz->elem_size;
}

/*
 * QBE SIL IR structure
 */

typedef enum {
    QS_OP_ADD,
    QS_OP_SUB,
    QS_OP_MUL,
    QS_OP_DIV,
    QS_OP_REM,
    QS_OP_NEG,
    QS_OP_AND,
    QS_OP_OR,
    QS_OP_XOR,
    QS_OP_SAR,
    QS_OP_SHR,
    QS_OP_SHL,
    QS_OP_LOADB,
    QS_OP_LOADW,
    QS_OP_STOREB,
    QS_OP_STOREW,
    QS_OP_BLITS,
    QS_OP_ALLOC,
    QS_OP_CEQ,
    QS_OP_CNE,
    QS_OP_CLT,
    QS_OP_CLE,
    QS_OP_CGT,
    QS_OP_CGE,
    QS_OP_EXTSB,
    QS_OP_COPY,
    QS_OP_CALL,
    QS_OP_PHI,
    QS_OP_JMP,
    QS_OP_JNZ,
    QS_OP_RET,
    QS_OP_HLT
} qs_ir_op_t;

typedef enum { QS_TY_VOID, QS_TY_BYTE, QS_TY_WORD, QS_TY_NULL } qs_ir_type_t;

typedef struct qs_ir_val qs_ir_val_t;
typedef struct qs_ir_inst qs_ir_inst_t;
typedef struct qs_ir_temp qs_ir_temp_t;
typedef struct qs_ir_block qs_ir_block_t;
typedef struct qs_ir_func qs_ir_func_t;
typedef struct qs_ir_dataitem qs_ir_dataitem_t;
typedef struct qs_ir_data qs_ir_data_t;
typedef struct qs_ir_module qs_ir_module_t;
typedef struct qs_ir_global qs_ir_global_t;

typedef enum { QS_V_TEMP, QS_V_CONST, QS_V_GLOBAL } qs_ir_val_kind_t;

struct qs_ir_val {
    qs_ir_val_kind_t kind;
    qs_ir_type_t type;

    qs_ir_temp_t *temp;
    int ival;
    qs_ir_global_t *global;
};

struct qs_ir_inst {
    qs_ir_op_t op;

    qs_ir_val_t *dest;

    qs_ir_val_t *args;
    qs_dynarr_sz_t narg;

    qs_ir_block_t **blocks;
    qs_dynarr_sz_t nblock;
};

struct qs_ir_temp {
    char *name;
    qs_ir_type_t type;
    int isparam;
};

struct qs_ir_block {
    char *name;

    qs_ir_inst_t *ins;
    qs_dynarr_sz_t nin;

    qs_ir_block_t **preds;
    qs_dynarr_sz_t npred;

    qs_ir_block_t **succs;
    qs_dynarr_sz_t nsucc;

    bool resolved;  // support forward reference
};

struct qs_ir_func {
    qs_ir_type_t rty;

    qs_ir_temp_t *temps;
    qs_dynarr_sz_t ntemp;

    int nparams;
    bool variadic;

    qs_ir_block_t *blocks;
    qs_dynarr_sz_t nblock;
};

typedef enum {
    QS_DI_SYM,
    QS_DI_STR,
    QS_DI_CONST,
    QS_DI_ZERO
} qs_ir_dataitem_kind_t;

struct qs_ir_dataitem {
    qs_ir_dataitem_kind_t kind;
    qs_ir_type_t type;
    int size;

    qs_ir_val_t *sym;
    int offset;

    char *str;

    int ival;

    int zbytes;
};

struct qs_ir_data {
    int size;
    qs_ir_dataitem_t *dataitems;
    qs_dynarr_sz_t ndataitem;
};

struct qs_ir_module {
    qs_ir_func_t *funcs;
    qs_dynarr_sz_t nfunc;

    qs_ir_data_t *datas;
    qs_dynarr_sz_t ndata;

    qs_ir_global_t *globals;
    qs_dynarr_sz_t nglobal;
};

typedef enum {
    QS_GLOBAL_UNDEF,
    QS_GLOBAL_DATA,
    QS_GLOBAL_FUNC
} qs_ir_global_kind_t;

struct qs_ir_global {
    qs_ir_global_kind_t kind;
    char *name;

    qs_ir_func_t *func;
    qs_ir_data_t *data;
};

/*
 *  IR builder functions
 */

qs_ir_module_t *qs_new_module();

qs_ir_global_t *qs_new_global_sym(qs_ir_module_t *m, char *name);

qs_ir_func_t *qs_new_func(qs_ir_module_t *m,
                          char *name,
                          qs_ir_type_t rty,
                          qs_ir_global_t *g);

qs_ir_block_t *qs_new_block(qs_ir_func_t *f, char *name);

void qs_block_add_succ(qs_ir_block_t *blk, qs_ir_block_t *succ);
void qs_block_add_pred(qs_ir_block_t *blk, qs_ir_block_t *pred);

qs_ir_temp_t *qs_new_temp(qs_ir_func_t *f,
                          char *name,
                          qs_ir_type_t ty,
                          bool isparam);

qs_ir_data_t *qs_new_data(qs_ir_module_t *m, char *name, qs_ir_global_t *g);

qs_ir_inst_t *qs_new_inst(qs_ir_block_t *blk, qs_ir_op_t op);

void qs_inst_add_arg(qs_ir_inst_t *inst, qs_ir_val_t *val);
void qs_inst_add_block(qs_ir_inst_t *inst, qs_ir_block_t *blk);

qs_ir_val_t *qs_new_val_temp(qs_ir_type_t ty, qs_ir_temp_t *temp);
qs_ir_val_t *qs_new_val_const(qs_ir_type_t ty, int ival);
qs_ir_val_t *qs_new_val_global(qs_ir_type_t ty, qs_ir_global_t *global);

void qs_data_add_sym(qs_ir_data_t *d,
                     qs_ir_type_t ty,
                     qs_ir_val_t *sym,
                     int offset);
void qs_data_add_str(qs_ir_data_t *d, qs_ir_type_t ty, char *str);
void qs_data_add_const(qs_ir_data_t *d, qs_ir_type_t ty, int ival);
void qs_data_add_zero(qs_ir_data_t *d, int zbytes);

qs_ir_global_t *qs_find_global_sym(qs_ir_module_t *m, char *name);
qs_ir_block_t *qs_find_block(qs_ir_func_t *f, char *name);
qs_ir_temp_t *qs_find_temp(qs_ir_func_t *f, char *name);

qs_ir_block_t *qs_block_find_succ(qs_ir_block_t *blk, char *name);
qs_ir_block_t *qs_block_find_pred(qs_ir_block_t *blk, char *name);

qs_ir_module_t *qs_new_module()
{
    qs_ir_module_t *m = qs_arena_alloc(sizeof(qs_ir_module_t));
    m->funcs = qs_dynarr_init(&m->nfunc, 0, sizeof(qs_ir_func_t));
    m->datas = qs_dynarr_init(&m->ndata, 0, sizeof(qs_ir_data_t));
    m->globals = qs_dynarr_init(&m->nglobal, 0, sizeof(qs_ir_global_t));
    return m;
}

qs_ir_global_t *qs_new_global_sym(qs_ir_module_t *m, char *name)
{
    qs_ir_global_t g;
    g.kind = QS_GLOBAL_UNDEF;
    g.name = qs_arena_strdup(name, strlen(name));
    g.func = NULL;
    g.data = NULL;

    // avoid incompatible pointer conversion warning
    void *data = m->globals;
    void *elem_ptr = &g;
    m->globals = qs_dynarr_push(data, &m->nglobal, elem_ptr);
    data = m->globals;
    return qs_dynarr_get(data, &m->nglobal, m->nglobal.len - 1);
}

qs_ir_func_t *qs_new_func(qs_ir_module_t *m,
                          char *name,
                          qs_ir_type_t rty,
                          qs_ir_global_t *g)
{
    qs_ir_func_t f;
    f.rty = rty;
    f.temps = qs_dynarr_init(&f.ntemp, 0, sizeof(qs_ir_temp_t));
    f.nparams = 0;
    f.variadic = false;
    f.blocks = qs_dynarr_init(&f.nblock, 0, sizeof(qs_ir_block_t));

    // avoid incompatible pointer conversion warning
    void *data = m->funcs;
    void *elem_ptr = &f;
    m->funcs = qs_dynarr_push(data, &m->nfunc, elem_ptr);
    data = m->funcs;
    qs_ir_func_t *fp = qs_dynarr_get(data, &m->nfunc, m->nfunc.len - 1);

    if (!g)
        g = qs_new_global_sym(m, name);
    g->func = fp;
    g->kind = QS_GLOBAL_FUNC;

    return fp;
}

qs_ir_block_t *qs_new_block(qs_ir_func_t *f, char *name)
{
    qs_ir_block_t blk;
    blk.name = qs_arena_strdup(name, strlen(name));
    blk.ins = qs_dynarr_init(&blk.nin, 0, sizeof(qs_ir_inst_t));
    blk.preds = qs_dynarr_init(&blk.npred, 0, sizeof(qs_ir_block_t *));
    blk.succs = qs_dynarr_init(&blk.nsucc, 0, sizeof(qs_ir_block_t *));
    blk.resolved = false;

    // avoid incompatible pointer conversion warning
    void *data = f->blocks;
    void *elem_ptr = &blk;
    f->blocks = qs_dynarr_push(data, &f->nblock, elem_ptr);
    data = f->blocks;
    return qs_dynarr_get(data, &f->nblock, f->nblock.len - 1);
}

void qs_block_add_succ(qs_ir_block_t *blk, qs_ir_block_t *succ)
{
    // avoid incompatible pointer conversion warning
    void *data = blk->succs;
    void *elem_ptr = &succ;
    blk->succs = qs_dynarr_push(data, &blk->nsucc, elem_ptr);
}

void qs_block_add_pred(qs_ir_block_t *blk, qs_ir_block_t *pred)
{
    // avoid incompatible pointer conversion warning
    void *data = blk->preds;
    void *elem_ptr = &pred;
    blk->preds = qs_dynarr_push(data, &blk->npred, elem_ptr);
}

qs_ir_temp_t *qs_new_temp(qs_ir_func_t *f,
                          char *name,
                          qs_ir_type_t ty,
                          bool isparam)
{
    qs_ir_temp_t temp;
    temp.name = qs_arena_strdup(name, strlen(name));
    temp.type = ty;
    temp.isparam = isparam;

    // avoid incompatible pointer conversion warning
    void *data = f->temps;
    void *elem_ptr = &temp;
    f->temps = qs_dynarr_push(data, &f->ntemp, elem_ptr);
    data = f->temps;
    return qs_dynarr_get(data, &f->ntemp, f->ntemp.len - 1);
}

qs_ir_data_t *qs_new_data(qs_ir_module_t *m, char *name, qs_ir_global_t *g)
{
    qs_ir_data_t d;
    d.size = 0;
    d.dataitems = qs_dynarr_init(&d.ndataitem, 0, sizeof(qs_ir_dataitem_t));

    // avoid incompatible pointer conversion warning
    void *data = m->datas;
    void *elem_ptr = &d;
    m->datas = qs_dynarr_push(data, &m->ndata, elem_ptr);
    data = m->datas;
    qs_ir_data_t *dp = qs_dynarr_get(data, &m->ndata, m->ndata.len - 1);

    if (!g)
        g = qs_new_global_sym(m, name);
    g->data = dp;
    g->kind = QS_GLOBAL_DATA;

    return dp;
}

qs_ir_inst_t *qs_new_inst(qs_ir_block_t *blk, qs_ir_op_t op)
{
    qs_ir_inst_t inst;
    inst.op = op;
    inst.dest = NULL;
    inst.args = qs_dynarr_init(&inst.narg, 0, sizeof(qs_ir_val_t));
    inst.blocks = qs_dynarr_init(&inst.nblock, 0, sizeof(qs_ir_block_t *));

    // avoid incompatible pointer conversion warning
    void *data = blk->ins;
    void *elem_ptr = &inst;
    blk->ins = qs_dynarr_push(data, &blk->nin, elem_ptr);
    data = blk->ins;
    return qs_dynarr_get(data, &blk->nin, blk->nin.len - 1);
}

void qs_inst_add_arg(qs_ir_inst_t *inst, qs_ir_val_t *val)
{
    // avoid incompatible pointer conversion warning
    void *data = inst->args;
    void *elem_ptr = val;
    inst->args = qs_dynarr_push(data, &inst->narg, elem_ptr);
}

void qs_inst_add_block(qs_ir_inst_t *inst, qs_ir_block_t *blk)
{
    // avoid incompatible pointer conversion warning
    void *data = inst->blocks;
    void *elem_ptr = &blk;
    inst->blocks = qs_dynarr_push(data, &inst->nblock, elem_ptr);
}

qs_ir_val_t *qs_new_val_temp(qs_ir_type_t ty, qs_ir_temp_t *temp)
{
    qs_ir_val_t *val = qs_arena_alloc(sizeof(qs_ir_val_t));
    val->kind = QS_V_TEMP;
    val->type = ty;
    val->temp = temp;
    return val;
}

qs_ir_val_t *qs_new_val_const(qs_ir_type_t ty, int ival)
{
    qs_ir_val_t *val = qs_arena_alloc(sizeof(qs_ir_val_t));
    val->kind = QS_V_CONST;
    val->type = ty;
    val->ival = ival;
    return val;
}

qs_ir_val_t *qs_new_val_global(qs_ir_type_t ty, qs_ir_global_t *global)
{
    qs_ir_val_t *val = qs_arena_alloc(sizeof(qs_ir_val_t));
    val->kind = QS_V_GLOBAL;
    val->type = ty;
    val->global = global;
    return val;
}

void qs_data_add_sym(qs_ir_data_t *d,
                     qs_ir_type_t ty,
                     qs_ir_val_t *sym,
                     int offset)
{
    if (sym->kind != QS_V_GLOBAL) {
        printf(
            "DATAITEM only accepts a global symbol as a pointer to an "
            "object\n");
        exit(1);
    }

    qs_ir_dataitem_t item;
    item.kind = QS_DI_SYM;
    // Assuming pointer to global symbol are word-sized
    // But user still can decide type
    item.type = ty;
    // and assuming word is 4 bytes
    item.size = 4;
    item.sym = sym;

    item.offset = offset;

    // avoid incompatible pointer conversion warning
    void *data = d->dataitems;
    void *elem_ptr = &item;
    d->dataitems = qs_dynarr_push(data, &d->ndataitem, elem_ptr);
}

void qs_data_add_str(qs_ir_data_t *d, qs_ir_type_t ty, char *str)
{
    int length = strlen(str);

    qs_ir_dataitem_t item;
    item.kind = QS_DI_STR;
    item.type = ty;
    item.size = length + 1;  // include null terminator
    item.str = qs_arena_strdup(str, length);

    // avoid incompatible pointer conversion warning
    void *data = d->dataitems;
    void *elem_ptr = &item;
    d->dataitems = qs_dynarr_push(data, &d->ndataitem, elem_ptr);
}

void qs_data_add_const(qs_ir_data_t *d, qs_ir_type_t ty, int ival)
{
    qs_ir_dataitem_t item;
    item.kind = QS_DI_CONST;
    item.type = ty;
    item.size = (ty == QS_TY_BYTE) ? 1 : 4;  // Assuming word is 4 bytes
    item.ival = ival;

    // avoid incompatible pointer conversion warning
    void *data = d->dataitems;
    void *elem_ptr = &item;
    d->dataitems = qs_dynarr_push(data, &d->ndataitem, elem_ptr);
}

void qs_data_add_zero(qs_ir_data_t *d, int zbytes)
{
    qs_ir_dataitem_t item;
    item.kind = QS_DI_ZERO;
    item.type = QS_TY_BYTE;
    item.size = zbytes;
    item.zbytes = zbytes;

    // avoid incompatible pointer conversion warning
    void *data = d->dataitems;
    void *elem_ptr = &item;
    d->dataitems = qs_dynarr_push(data, &d->ndataitem, elem_ptr);
}

qs_ir_global_t *qs_find_global_sym(qs_ir_module_t *m, char *name)
{
    for (int i = 0; i < m->nglobal.len; ++i) {
        if (strcmp(m->globals[i].name, name) == 0)
            return &m->globals[i];
    }
    return NULL;
}

qs_ir_block_t *qs_find_block(qs_ir_func_t *f, char *name)
{
    for (int i = 0; i < f->nblock.len; ++i) {
        if (strcmp(f->blocks[i].name, name) == 0)
            return &f->blocks[i];
    }
    return NULL;
}

qs_ir_temp_t *qs_find_temp(qs_ir_func_t *f, char *name)
{
    for (int i = 0; i < f->ntemp.len; ++i) {
        if (strcmp(f->temps[i].name, name) == 0)
            return &f->temps[i];
    }
    return NULL;
}

qs_ir_block_t *qs_block_find_succ(qs_ir_block_t *blk, char *name)
{
    for (int i = 0; i < blk->nsucc.len; ++i) {
        if (strcmp(blk->succs[i]->name, name) == 0)
            return blk->succs[i];
    }
    return NULL;
}

qs_ir_block_t *qs_block_find_pred(qs_ir_block_t *blk, char *name)
{
    for (int i = 0; i < blk->npred.len; ++i) {
        if (strcmp(blk->preds[i]->name, name) == 0)
            return blk->preds[i];
    }
    return NULL;
}

/*
 * parsing functions
 */

qs_ir_type_t qs_parse_type()
{
    if (qs_accept(QS_TOK_KW_BYTE))
        return QS_TY_BYTE;
    if (qs_accept(QS_TOK_KW_WORD))
        return QS_TY_WORD;
    return QS_TY_NULL;
}

qs_ir_type_t qs_parse_ret_type()
{
    if (qs_accept(QS_TOK_KW_VOID))
        return QS_TY_VOID;
    return qs_parse_type();
}

qs_ir_val_t *qs_parse_value(qs_ir_module_t *mod,
                            qs_ir_func_t *func,
                            qs_ir_type_t expect_type)
{
    if (qs_peek(QS_TOK_INT)) {
        qs_ir_val_t *v = qs_new_val_const(expect_type, qs_tok.ival);
        qs_next_tok();
        return v;
    }
    if (qs_peek(QS_TOK_GLOBAL)) {
        qs_ir_global_t *g = qs_find_global_sym(mod, qs_tok.text);
        if (!g)
            g = qs_new_global_sym(mod, qs_tok.text);
        qs_ir_val_t *v = qs_new_val_global(expect_type, g);
        qs_next_tok();
        return v;
    }
    if (qs_peek(QS_TOK_TEMP)) {
        qs_ir_temp_t *t = qs_find_temp(func, qs_tok.text);
        if (!t)
            qs_error_at(qs_tok.line, qs_tok.col, "undefined temp", 0);
        qs_ir_val_t *v = qs_new_val_temp(expect_type, t);
        qs_next_tok();
        return v;
    }
    return NULL;
}

bool qs_has_terminator(qs_ir_block_t *blk)
{
    switch (blk->ins[blk->nin.len - 1].op) {
    case QS_OP_JMP:
    case QS_OP_JNZ:
    case QS_OP_RET:
    case QS_OP_HLT:
        return true;
    default:
        return false;
    }
}

qs_ir_op_t op_from_ident(char *s)
{
#define MAP(a, b)          \
    if (strcmp(s, a) == 0) \
    return b
    MAP("add", QS_OP_ADD);
    MAP("sub", QS_OP_SUB);
    MAP("mul", QS_OP_MUL);
    MAP("div", QS_OP_DIV);
    MAP("rem", QS_OP_REM);
    MAP("neg", QS_OP_NEG);
    MAP("and", QS_OP_AND);
    MAP("or", QS_OP_OR);
    MAP("xor", QS_OP_XOR);
    MAP("sar", QS_OP_SAR);
    MAP("shr", QS_OP_SHR);
    MAP("shl", QS_OP_SHL);
    MAP("loadb", QS_OP_LOADB);
    MAP("loadw", QS_OP_LOADW);
    MAP("storeb", QS_OP_STOREB);
    MAP("storew", QS_OP_STOREW);
    MAP("blits", QS_OP_BLITS);
    MAP("alloc", QS_OP_ALLOC);
    MAP("ceq", QS_OP_CEQ);
    MAP("cne", QS_OP_CNE);
    MAP("clt", QS_OP_CLT);
    MAP("cle", QS_OP_CLE);
    MAP("cgt", QS_OP_CGT);
    MAP("cge", QS_OP_CGE);
    MAP("extsb", QS_OP_EXTSB);
    MAP("copy", QS_OP_COPY);
#undef MAP
    return -1;
}

void qs_parse_block(qs_ir_module_t *mod, qs_ir_func_t *func, qs_ir_block_t *blk)
{
    bool has_dest = false;
    char *dest_name;
    qs_ir_type_t type;
    /* phi instruction */
    while (true) {
        if (!qs_peek(QS_TOK_TEMP))
            break;
        dest_name = qs_tok.text;
        qs_next_tok();
        qs_expect(QS_TOK_EQ);
        type = qs_parse_type();
        if (type == QS_TY_NULL)
            qs_error_at(qs_tok.line, qs_tok.col, "expect type w|b", 0);
        has_dest = true;
        if (!qs_accept(QS_TOK_KW_PHI))
            break;
        has_dest = false;

        qs_ir_inst_t *phi = qs_new_inst(blk, QS_OP_PHI);

        qs_ir_temp_t *dest = qs_find_temp(func, dest_name);
        if (!dest)
            dest = qs_new_temp(func, dest_name, type, false);
        qs_ir_val_t *dest_val = qs_new_val_temp(type, dest);
        phi->dest = dest_val;

        do {
            char *label_name = qs_tok.text;
            qs_expect(QS_TOK_LABEL);
            qs_ir_block_t *target = qs_block_find_pred(blk, label_name);
            if (!target)
                target = qs_new_block(func, label_name);

            // assuming equal to dest type
            qs_ir_val_t *val = qs_parse_value(mod, func, type);

            // I can't put if here!!
            // if (!val)
            //     qs_error_at(qs_tok.line, qs_tok.col, "expected value", 0);

            qs_inst_add_block(phi, target);
            qs_inst_add_arg(phi, val);
        } while (qs_accept(QS_TOK_COMMA));
    }

    /* Instruction */
    while (true) {
        /* optional dest and type */
        if (!has_dest && qs_peek(QS_TOK_TEMP)) {
            dest_name = qs_tok.text;
            qs_next_tok();
            qs_expect(QS_TOK_EQ);
            type = qs_parse_type();
            has_dest = true;
        }
        if (!has_dest) {
            dest_name = NULL;
            type = QS_TY_NULL;
        }
        /* call */
        if (qs_accept(QS_TOK_KW_CALL)) {
            qs_ir_inst_t *call = qs_new_inst(blk, QS_OP_CALL);
            // assuming func pointer word-sized
            qs_ir_val_t *val = qs_parse_value(mod, func, QS_TY_WORD);
            if (val->kind != QS_V_GLOBAL)
                qs_error_at(qs_tok.line, qs_tok.col, "expected global symbol",
                            0);

            if (has_dest) {
                qs_ir_temp_t *dest = qs_find_temp(func, dest_name);
                if (!dest)
                    dest = qs_new_temp(func, dest_name, type, false);
                qs_ir_val_t *dest_val = qs_new_val_temp(type, dest);
                call->dest = dest_val;
            }

            qs_inst_add_arg(call, val);

            qs_expect(QS_TOK_LPAREN);
            while (!qs_peek(QS_TOK_RPAREN)) {
                qs_ir_type_t arg_type = qs_parse_type();
                if (arg_type == QS_TY_NULL)
                    qs_error_at(qs_tok.line, qs_tok.col, "expect type w|b", 0);
                qs_ir_val_t *arg = qs_parse_value(mod, func, arg_type);
                if (!arg)
                    qs_error_at(qs_tok.line, qs_tok.col, "expect value", 0);
                qs_inst_add_arg(call, arg);
                qs_accept(QS_TOK_COMMA);
            }
            qs_expect(QS_TOK_RPAREN);
        } else {
            /* General Ops */
            if (qs_tok.k != QS_TOK_IDENT)
                break;
            qs_ir_op_t op = op_from_ident(qs_tok.text);
            if (op < 0)
                qs_error_at(qs_tok.line, qs_tok.col, "unkown op", 0);
            qs_ir_inst_t *inst = qs_new_inst(blk, op);
            if (has_dest) {
                qs_ir_temp_t *dest = qs_find_temp(func, dest_name);
                if (!dest)
                    dest = qs_new_temp(func, dest_name, type, false);
                qs_ir_val_t *dest_val = qs_new_val_temp(type, dest);
                inst->dest = dest_val;
            }
            qs_next_tok();

            do {
                // todo: set different types for different instructions
                qs_ir_val_t *arg = qs_parse_value(mod, func, QS_TY_WORD);
                if (!arg)
                    qs_error_at(qs_tok.line, qs_tok.col, "expected value", 0);
                qs_inst_add_arg(inst, arg);
            } while (qs_accept(QS_TOK_COMMA));
        }
        has_dest = false;
    }

    /* jump */
    if (qs_accept(QS_TOK_KW_JMP)) {
        char *label_name = qs_tok.text;
        qs_expect(QS_TOK_LABEL);
        qs_ir_block_t *target = qs_find_block(func, label_name);
        if (!target)
            target = qs_new_block(func, label_name);
        qs_block_add_succ(blk, target);
        qs_block_add_pred(target, blk);
        qs_ir_inst_t *jmp = qs_new_inst(blk, QS_OP_JMP);
        qs_inst_add_block(jmp, target);
        return;
    }
    if (qs_accept(QS_TOK_KW_JNZ)) {
        qs_ir_val_t *cond = qs_parse_value(mod, func, QS_TY_WORD);
        qs_expect(QS_TOK_COMMA);
        char *label1_name = qs_tok.text;
        qs_expect(QS_TOK_LABEL);
        qs_ir_block_t *target1 = qs_find_block(func, label1_name);
        if (!target1)
            target1 = qs_new_block(func, label1_name);
        qs_expect(QS_TOK_COMMA);
        char *label2_name = qs_tok.text;
        qs_expect(QS_TOK_LABEL);
        qs_ir_block_t *target2 = qs_find_block(func, label2_name);
        if (!target2)
            target2 = qs_new_block(func, label2_name);
        qs_ir_inst_t *jnz = qs_new_inst(blk, QS_OP_JNZ);
        qs_block_add_succ(blk, target1);
        qs_block_add_succ(blk, target2);
        qs_block_add_pred(target1, blk);
        qs_block_add_pred(target2, blk);
        qs_inst_add_arg(jnz, cond);
        qs_inst_add_block(jnz, target1);
        qs_inst_add_block(jnz, target2);
        return;
    }
    if (qs_accept(QS_TOK_KW_RET)) {
        qs_ir_val_t *val = qs_parse_value(mod, func, QS_TY_WORD);
        qs_ir_inst_t *ret = qs_new_inst(blk, QS_OP_RET);
        qs_inst_add_arg(ret, val);
        return;
    }
    if (qs_accept(QS_TOK_KW_HLT)) {
        qs_new_inst(blk, QS_OP_HLT);
        return;
    }
}

void qs_parse_function(qs_ir_module_t *mod)
{
    qs_ir_type_t ret_type = qs_parse_ret_type();
    if (ret_type == QS_TY_NULL) {
        // by https://c9x.me/compile/doc/il.html#Functions
        // "If the return type is missing, the function must not return any
        // value."
        ret_type = QS_TY_VOID;
    }
    char *func_name = qs_tok.text;
    qs_expect(QS_TOK_GLOBAL);
    qs_ir_global_t *g = qs_find_global_sym(mod, func_name);
    if (!g)
        g = qs_new_global_sym(mod, func_name);
    else if (g->kind != QS_GLOBAL_UNDEF)
        qs_error_at(qs_tok.line, qs_tok.col, "function redefined", 0);
    qs_ir_func_t *func = qs_new_func(mod, func_name, ret_type, g);

    qs_expect(QS_TOK_LPAREN);
    int param_cnt = 0;
    while (!qs_peek(QS_TOK_RPAREN)) {
        if (qs_accept(QS_TOK_ELLIPSIS)) {
            func->variadic = true;
            break;
        }
        ++param_cnt;
        qs_ir_type_t type = qs_parse_type();
        if (type == QS_TY_NULL) {
            qs_error_at(qs_tok.line, qs_tok.col, "expect type w|b", 0);
        }
        char *temp_name = qs_tok.text;
        qs_expect(QS_TOK_TEMP);

        qs_new_temp(func, temp_name, type, true);

        qs_accept(QS_TOK_COMMA);
    }
    func->nparams = param_cnt;
    qs_expect(QS_TOK_RPAREN);

    qs_expect(QS_TOK_LBRACE);
    char *blk_name = qs_tok.text;
    qs_ir_block_t *prev_blk = NULL;
    if (!qs_accept(QS_TOK_LABEL))
        blk_name = "_entry";
    do {
        qs_ir_block_t *cur_blk = qs_find_block(func, blk_name);
        if (!cur_blk)
            cur_blk = qs_new_block(func, blk_name);
        else if (cur_blk->resolved)
            qs_error_at(qs_tok.line, qs_tok.col, "block redefined", 0);
        cur_blk->resolved = true;

        if (prev_blk && !qs_has_terminator(prev_blk)) {
            // dynamic push may change memory address of block
            prev_blk = qs_find_block(func, prev_blk->name);
            qs_block_add_succ(prev_blk, cur_blk);
            qs_block_add_pred(cur_blk, prev_blk);
            qs_ir_inst_t *jmp = qs_new_inst(prev_blk, QS_OP_JMP);
            qs_inst_add_block(jmp, cur_blk);
        }

        qs_parse_block(mod, func, cur_blk);

        blk_name = qs_tok.text;
        prev_blk = cur_blk;
    } while (qs_accept(QS_TOK_LABEL));
    qs_expect(QS_TOK_RBRACE);
}

void qs_parse_dataitem(qs_ir_module_t *mod,
                       qs_ir_data_t *data,
                       qs_ir_type_t expect_type)
{
    do {
        if (qs_peek(QS_TOK_GLOBAL)) {
            char *global_name = qs_tok.text;
            int offset = 0;
            qs_next_tok();
            if (qs_accept(QS_TOK_PLUS)) {
                offset = qs_tok.ival;
                qs_expect(QS_TOK_INT);
            }
            qs_ir_global_t *global = qs_find_global_sym(mod, global_name);
            if (!global)
                global = qs_new_global_sym(mod, global_name);
            qs_ir_val_t *val = qs_new_val_global(expect_type, global);
            qs_data_add_sym(data, expect_type, val, offset);
        } else if (qs_peek(QS_TOK_STRING)) {
            qs_data_add_str(data, expect_type, qs_tok.text);
            qs_next_tok();
        } else if (qs_peek(QS_TOK_INT)) {
            qs_data_add_const(data, expect_type, qs_tok.ival);
            qs_next_tok();
        } else {
            qs_error_at(qs_tok.line, qs_tok.col,
                        "expected global symbol | string | number", 0);
        }
    } while (qs_peek(QS_TOK_GLOBAL) || qs_peek(QS_TOK_STRING) ||
             qs_peek(QS_TOK_INT));
}

void qs_parse_data(qs_ir_module_t *mod)
{
    char *data_name = qs_tok.text;
    qs_expect(QS_TOK_GLOBAL);

    qs_ir_global_t *g = qs_find_global_sym(mod, data_name);
    if (!g)
        g = qs_new_global_sym(mod, data_name);
    else if (g->kind != QS_GLOBAL_UNDEF)
        qs_error_at(qs_tok.line, qs_tok.col, "data redefined", 0);
    qs_ir_data_t *data = qs_new_data(mod, data_name, g);

    qs_expect(QS_TOK_EQ);
    qs_expect(QS_TOK_LBRACE);

    while (!qs_peek(QS_TOK_RBRACE)) {
        qs_ir_type_t type = qs_parse_type();

        if (type != QS_TY_NULL) {
            qs_parse_dataitem(mod, data, type);
        } else {
            if (!qs_peek(QS_TOK_IDENT) || qs_tok.len != 1 ||
                qs_tok.text[0] != 'z')
                qs_error_at(qs_tok.line, qs_tok.col, "expected b|w|z", 0);
            qs_next_tok();
            int ival = qs_tok.ival;
            qs_expect(QS_TOK_INT);
            qs_data_add_zero(data, ival);
        }
        qs_accept(QS_TOK_COMMA);
    }

    qs_expect(QS_TOK_RBRACE);
}

qs_ir_module_t *qs_parse_module()
{
    qs_ir_module_t *mod = qs_new_module();
    qs_next_tok();
    while (qs_tok.k != QS_TOK_EOF) {
        if (qs_accept(QS_TOK_KW_DATA)) {
            qs_parse_data(mod);
        } else if (qs_accept(QS_TOK_KW_FUNCTION))
            qs_parse_function(mod);
        else
            qs_error_at(qs_tok.line, qs_tok.col,
                        "expected top-level definition", 0);
    }
    return mod;
}

/*
 * print IR
 */

void qs_print_type(qs_ir_type_t ty)
{
    if (ty == QS_TY_VOID) {
        printf("VOID");
        return;
    }
    if (ty == QS_TY_BYTE) {
        printf("BYTE");
        return;
    }
    if (ty == QS_TY_WORD) {
        printf("WORD");
        return;
    }
}

void qs_print_value(qs_ir_val_t *val)
{
    qs_print_type(val->type);
    switch (val->kind) {
    case QS_V_CONST:
        printf(" %d", val->ival);
        break;
    case QS_V_GLOBAL:
        printf(" %s", val->global->name);
        break;
    case QS_V_TEMP:
        printf(" %s", val->temp->name);
        break;
    default:
        printf("Unexpected value kind\n");
        exit(1);
    }
}

void qs_print_inst(qs_ir_inst_t *in)
{
    printf("    ");
    switch (in->op) {
    case QS_OP_ADD:
        qs_print_value(in->dest);
        printf(" = ADD ");
        qs_print_value(&in->args[0]);
        printf(" ");
        qs_print_value(&in->args[1]);
        printf("\n");
        break;
    case QS_OP_SUB:
        qs_print_value(in->dest);
        printf(" = SUB ");
        qs_print_value(&in->args[0]);
        printf(" ");
        qs_print_value(&in->args[1]);
        printf("\n");
        break;
    case QS_OP_MUL:
        qs_print_value(in->dest);
        printf(" = MUL ");
        qs_print_value(&in->args[0]);
        printf(" ");
        qs_print_value(&in->args[1]);
        printf("\n");
        break;
    case QS_OP_DIV:
        qs_print_value(in->dest);
        printf(" = DIV ");
        qs_print_value(&in->args[0]);
        printf(" ");
        qs_print_value(&in->args[1]);
        printf("\n");
        break;
    case QS_OP_REM:
        qs_print_value(in->dest);
        printf(" = REM ");
        qs_print_value(&in->args[0]);
        printf(" ");
        qs_print_value(&in->args[1]);
        printf("\n");
        break;
    case QS_OP_NEG:
        qs_print_value(in->dest);
        printf(" = NEG ");
        qs_print_value(&in->args[0]);
        printf("\n");
        break;
    case QS_OP_AND:
        qs_print_value(in->dest);
        printf(" = AND ");
        qs_print_value(&in->args[0]);
        printf(" ");
        qs_print_value(&in->args[1]);
        printf("\n");
        break;
    case QS_OP_OR:
        qs_print_value(in->dest);
        printf(" = OR ");
        qs_print_value(&in->args[0]);
        printf(" ");
        qs_print_value(&in->args[1]);
        printf("\n");
        break;
    case QS_OP_XOR:
        qs_print_value(in->dest);
        printf(" = XOR ");
        qs_print_value(&in->args[0]);
        printf(" ");
        qs_print_value(&in->args[1]);
        printf("\n");
        break;
    case QS_OP_SAR:
        qs_print_value(in->dest);
        printf(" = SAR ");
        qs_print_value(&in->args[0]);
        printf(" ");
        qs_print_value(&in->args[1]);
        printf("\n");
        break;
    case QS_OP_SHR:
        qs_print_value(in->dest);
        printf(" = SHR ");
        qs_print_value(&in->args[0]);
        printf(" ");
        qs_print_value(&in->args[1]);
        printf("\n");
        break;
    case QS_OP_SHL:
        qs_print_value(in->dest);
        printf(" = SHL ");
        qs_print_value(&in->args[0]);
        printf(" ");
        qs_print_value(&in->args[1]);
        printf("\n");
        break;
    case QS_OP_LOADB:
        qs_print_value(in->dest);
        printf(" = LOADB ");
        qs_print_value(&in->args[0]);
        printf("\n");
        break;
    case QS_OP_LOADW:
        qs_print_value(in->dest);
        printf(" = LOADW ");
        qs_print_value(&in->args[0]);
        printf("\n");
        break;
    case QS_OP_STOREB:
        printf("STOREB ");
        qs_print_value(&in->args[0]);
        printf(" ");
        qs_print_value(&in->args[1]);
        printf("\n");
        break;
    case QS_OP_STOREW:
        printf("STOREW ");
        qs_print_value(&in->args[0]);
        printf(" ");
        qs_print_value(&in->args[1]);
        printf("\n");
        break;
    case QS_OP_BLITS:
        printf("BLITS ");
        qs_print_value(&in->args[0]);
        printf(" ");
        qs_print_value(&in->args[1]);
        printf(" ");
        qs_print_value(&in->args[2]);
        printf("\n");
        break;
    case QS_OP_ALLOC:
        qs_print_value(in->dest);
        printf("ALLOC ");
        qs_print_value(&in->args[0]);
        printf("\n");
        break;
    case QS_OP_CEQ:
        qs_print_value(in->dest);
        printf(" = CEQ ");
        qs_print_value(&in->args[0]);
        printf(" ");
        qs_print_value(&in->args[1]);
        printf("\n");
        break;
    case QS_OP_CNE:
        qs_print_value(in->dest);
        printf(" = CNE ");
        qs_print_value(&in->args[0]);
        printf(" ");
        qs_print_value(&in->args[1]);
        printf("\n");
        break;
    case QS_OP_CLT:
        qs_print_value(in->dest);
        printf(" = CLT ");
        qs_print_value(&in->args[0]);
        printf(" ");
        qs_print_value(&in->args[1]);
        printf("\n");
        break;
    case QS_OP_CLE:
        qs_print_value(in->dest);
        printf(" = CLE ");
        qs_print_value(&in->args[0]);
        printf(" ");
        qs_print_value(&in->args[1]);
        printf("\n");
        break;
    case QS_OP_CGT:
        qs_print_value(in->dest);
        printf(" = CGT ");
        qs_print_value(&in->args[0]);
        printf(" ");
        qs_print_value(&in->args[1]);
        printf("\n");
        break;
    case QS_OP_CGE:
        qs_print_value(in->dest);
        printf(" = CGE ");
        qs_print_value(&in->args[0]);
        printf(" ");
        qs_print_value(&in->args[1]);
        printf("\n");
        break;
    case QS_OP_EXTSB:
        qs_print_value(in->dest);
        printf(" = EXTSB ");
        qs_print_value(&in->args[0]);
        printf("\n");
        break;
    case QS_OP_COPY:
        qs_print_value(in->dest);
        printf(" = COPY ");
        qs_print_value(&in->args[0]);
        printf("\n");
        break;
    case QS_OP_CALL:
        if (in->dest) {
            qs_print_value(in->dest);
            printf(" = ");
        }
        printf("CALL ");
        qs_print_value(&in->args[0]);
        printf("(");
        for (int i = 1; i < in->narg.len; ++i) {
            if (i > 1)
                printf(", ");
            qs_print_value(&in->args[i]);
        }
        printf(")\n");
        break;
    case QS_OP_PHI:
        qs_print_value(in->dest);
        printf(" = PHI ");
        for (int i = 0; i < in->narg.len; ++i) {
            if (i)
                printf(", ");
            printf("%s ", in->blocks[i]->name);
            qs_print_value(&in->args[i]);
        }
        printf("\n");
        break;
    case QS_OP_JMP:
        printf("JMP %s\n", in->blocks[0]->name);
        break;
    case QS_OP_JNZ:
        printf("JNZ ");
        qs_print_value(&in->args[0]);
        printf(", %s, %s\n", in->blocks[0]->name, in->blocks[1]->name);
        break;
    case QS_OP_RET:
        printf("RET ");
        qs_print_value(&in->args[0]);
        printf("\n");
        break;
    case QS_OP_HLT:
        printf("HLT\n");
        break;
    default:
        printf("Unexpected instruction\n");
        exit(1);
    }
}

void qs_print_block(qs_ir_block_t *blk)
{
    for (int i = 0; i < blk->nin.len; ++i) {
        qs_print_inst(&blk->ins[i]);
    }
}

void qs_print_func(qs_ir_func_t *func)
{
    printf("(");
    for (int i = 0; i < func->nparams; ++i) {
        if (i)
            printf(", ");
        qs_print_type(func->temps[i].type);
        printf(" %s", func->temps[i].name);
    }
    if (func->variadic)
        printf(", ...");
    printf(") {\n");
    for (int i = 0; i < func->nblock.len; ++i) {
        printf("BLOCK %s:\n", func->blocks[i].name, func->blocks[i].nin.len);
        qs_print_block(&func->blocks[i]);
    }
    printf("}\n");
}

void qs_print_data(qs_ir_data_t *data)
{
    printf("{\n");
    for (int i = 0; i < data->ndataitem.len; ++i) {
        printf("    ");
        if (data->dataitems[i].kind == QS_DI_SYM) {
            qs_print_type(data->dataitems[i].type);
            printf(" %s + %d\n", data->dataitems[i].sym->global->name,
                   data->dataitems[i].offset);
        }
        if (data->dataitems[i].kind == QS_DI_STR) {
            qs_print_type(data->dataitems[i].type);
            printf(" \"%s\"\n", data->dataitems[i].str);
        }
        if (data->dataitems[i].kind == QS_DI_CONST) {
            qs_print_type(data->dataitems[i].type);
            printf(" %d\n", data->dataitems[i].ival);
        }
        if (data->dataitems[i].kind == QS_DI_ZERO) {
            printf("ZERO %d bytes\n", data->dataitems[i].zbytes);
        }
    }
    printf("}\n");
}

void qs_print_module(qs_ir_module_t *mod)
{
    for (int i = 0; i < mod->nglobal.len; ++i) {
        if (mod->globals[i].kind == QS_GLOBAL_DATA) {
            printf("data %s", mod->globals[i].name);
            qs_print_data(mod->globals[i].data);
        }
        if (mod->globals[i].kind == QS_GLOBAL_FUNC) {
            printf("function ");
            qs_print_type(mod->globals[i].func->rty);
            printf(" %s", mod->globals[i].name);
            qs_print_func(mod->globals[i].func);
        }
    }
}

void qs_parse(char *in)
{
    qs_init_arena();
    qs_init_lexer(in);
    qs_ir_module_t *mod = qs_parse_module();
    qs_print_module(mod);
}
