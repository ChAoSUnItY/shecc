#include <stdio.h>
#include <stdlib.h>

typedef struct qs_arena_chunk {
    struct qs_arena_chunk *next;
    int used;
    int cap;
    char *buf;
} qs_arena_chunk_t;

qs_arena_chunk_t *qs_arena_head;
int QS_ARENA_CHUNK_SIZE = 1 << 18; /* 256 KiB */

void qs_init_arena()
{
    qs_arena_head = NULL;
}

void *qs_arena_alloc(int n)
{
    if (n <= 0) {
        printf("Invalid arena allocation size: %d\n", n);
        exit(1);
    }
    /* 8‑bytes alignment */
    n = (n + 7) & ~7;
    if (!qs_arena_head || qs_arena_head->used + n > qs_arena_head->cap) {
        int cap = (n > QS_ARENA_CHUNK_SIZE ? n : QS_ARENA_CHUNK_SIZE);
        qs_arena_chunk_t *ch = malloc(sizeof(qs_arena_chunk_t));
        if (!ch) {
            printf("Failed to allocate memory for arena chunk\n");
            exit(1);
        }
        ch->buf = malloc(cap);
        if (!ch->buf) {
            printf("Failed to allocate memory for arena chunk buffer\n");
            free(ch);
            exit(1);
        }
        ch->next = qs_arena_head;
        ch->used = 0;
        ch->cap = cap;
        qs_arena_head = ch;
    }
    void *p = qs_arena_head->buf + qs_arena_head->used;
    qs_arena_head->used += n;
    return p;
}

char *qs_arena_strdup(char *s, int n)
{
    char *d = qs_arena_alloc(n + 1);
    for (int i = 0; i < n; i++)
        d[i] = s[i];
    d[n] = '\0';
    return d;
}

void qs_arena_free_all()
{
    while (qs_arena_head) {
        qs_arena_chunk_t *next = qs_arena_head->next;
        free(qs_arena_head->buf);
        free(qs_arena_head);
        qs_arena_head = next;
    }
    qs_arena_head = NULL;
}
