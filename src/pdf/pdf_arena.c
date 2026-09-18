#include <stdlib.h>
#include <string.h>
#include "pdf_arena.h"

#define DEFAULT_BLOCK 16384u
#define ALIGN         8u

struct PdfArenaBlock {
    PdfArenaBlock *next;
    size_t cap;
    size_t used;
    unsigned char *data;
};

void pdf_arena_init(PdfArena *a, size_t block_size)
{
    memset(a, 0, sizeof(*a));
    a->block_size = block_size ? block_size : DEFAULT_BLOCK;
}

static PdfArenaBlock *block_new(size_t cap)
{
    /* Cabecalho e dados numa alocacao so: menos chamadas ao alocador do
     * sistema e menos fragmentacao no heap de 16 MB do PSP. */
    PdfArenaBlock *b = (PdfArenaBlock *)malloc(sizeof(PdfArenaBlock) + cap);
    if (!b)
        return NULL;
    b->next = NULL;
    b->cap  = cap;
    b->used = 0;
    b->data = (unsigned char *)(b + 1);
    return b;
}

void *pdf_arena_alloc(PdfArena *a, size_t n)
{
    if (n == 0)
        n = 1;

    size_t need = (n + (ALIGN - 1)) & ~(size_t)(ALIGN - 1);

    /* Overflow no arredondamento significa n absurdo (vindo de um /Length
     * mentiroso, por exemplo). Recusar e o comportamento correto. */
    if (need < n) {
        a->oom = 1;
        return NULL;
    }

    if (a->head && a->head->used + need <= a->head->cap) {
        void *p = a->head->data + a->head->used;
        a->head->used += need;
        a->handed += need;
        return p;
    }

    /*
     * Reaproveita um bloco reciclado por reset(), mas so se ele couber o pedido.
     * Blocos sobressalentes menores continuam na lista para pedidos menores em
     * vez de serem descartados.
     */
    PdfArenaBlock **pp = &a->spare;
    while (*pp) {
        if ((*pp)->cap >= need) {
            PdfArenaBlock *b = *pp;
            *pp = b->next;
            b->used = need;
            b->next = a->head;
            a->head = b;
            a->handed += need;
            return b->data;
        }
        pp = &(*pp)->next;
    }

    size_t cap = a->block_size;
    if (cap < need)
        cap = need;      /* pedido grande ganha bloco proprio */

    PdfArenaBlock *b = block_new(cap);
    if (!b) {
        a->oom = 1;
        return NULL;
    }
    b->used = need;
    b->next = a->head;
    a->head = b;
    a->reserved += sizeof(PdfArenaBlock) + cap;
    a->handed   += need;
    return b->data;
}

char *pdf_arena_dup(PdfArena *a, const void *src, size_t n)
{
    char *p = (char *)pdf_arena_alloc(a, n + 1);
    if (!p)
        return NULL;
    if (n)
        memcpy(p, src, n);
    p[n] = '\0';
    return p;
}

void pdf_arena_reset(PdfArena *a)
{
    /* Move os blocos em uso para a lista de sobressalentes, preservando a
     * capacidade ja paga ao sistema. */
    PdfArenaBlock *b = a->head;
    while (b) {
        PdfArenaBlock *next = b->next;
        b->used = 0;
        b->next = a->spare;
        a->spare = b;
        b = next;
    }
    a->head   = NULL;
    a->handed = 0;
    a->oom    = 0;
}

static void chain_free(PdfArenaBlock *b)
{
    while (b) {
        PdfArenaBlock *next = b->next;
        free(b);
        b = next;
    }
}

void pdf_arena_free(PdfArena *a)
{
    chain_free(a->head);
    chain_free(a->spare);
    a->head = a->spare = NULL;
    a->reserved = a->handed = 0;
    a->oom = 0;
}
