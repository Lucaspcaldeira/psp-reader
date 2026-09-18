#include <string.h>
#include "pdf_obj.h"

/* const de verdade: qualquer escrita aqui e bug em quem chama, e melhor que
 * estoure na hora do que corrompa o objeto "ausente" compartilhado. */
static const PdfObj g_null = { PDF_NULL, { 0 } };

const PdfObj *pdf_null(void) { return &g_null; }

PdfObj *pdf_new(PdfArena *a, PdfKind k)
{
    PdfObj *o = (PdfObj *)pdf_arena_alloc(a, sizeof(PdfObj));
    if (!o)
        return NULL;
    memset(o, 0, sizeof(*o));
    o->kind = (unsigned char)k;
    if (k == PDF_STREAM)
        o->u.stm.len = -1;
    return o;
}

/* Cresce dobrando. A arena nao tem realloc, entao "crescer" e alocar novo e
 * copiar; dobrar mantem isso amortizado em O(1) por insercao. O desperdicio de
 * ate 2x e aceitavel porque a arena inteira e reciclada por pagina. */
static void **grow(PdfArena *a, void **old, int old_len, int *cap, size_t esz)
{
    int ncap = (*cap < 4) ? 4 : (*cap * 2);
    void **nv = (void **)pdf_arena_alloc(a, esz * (size_t)ncap);
    if (!nv)
        return NULL;
    if (old && old_len > 0)
        memcpy(nv, old, esz * (size_t)old_len);
    *cap = ncap;
    return nv;
}

int pdf_arr_push(PdfArena *a, PdfObj *arr, PdfObj *v)
{
    if (!arr || arr->kind != PDF_ARR || !v)
        return -1;

    if (arr->u.a.len >= arr->u.a.cap) {
        PdfObj **nv = (PdfObj **)grow(a, (void **)arr->u.a.v, arr->u.a.len,
                                      &arr->u.a.cap, sizeof(PdfObj *));
        if (!nv)
            return -1;
        arr->u.a.v = nv;
    }
    arr->u.a.v[arr->u.a.len++] = v;
    return 0;
}

int pdf_dict_put(PdfArena *a, PdfObj *d, PdfSlice key, PdfObj *v)
{
    if (!d || d->kind != PDF_DICT || !v || !key.p)
        return -1;

    /* Chave repetida: sobrescreve. Dicionarios com chave duplicada existem em
     * PDF gerado por ferramenta ruim, e os leitores usam a ultima ocorrencia. */
    for (int i = 0; i < d->u.d.len; ++i) {
        if (d->u.d.keys[i].len == key.len &&
            memcmp(d->u.d.keys[i].p, key.p, (size_t)key.len) == 0) {
            d->u.d.vals[i] = v;
            return 0;
        }
    }

    if (d->u.d.len >= d->u.d.cap) {
        int cap_k = d->u.d.cap;
        int cap_v = d->u.d.cap;

        PdfSlice *nk = (PdfSlice *)grow(a, (void **)d->u.d.keys, d->u.d.len,
                                        &cap_k, sizeof(PdfSlice));
        if (!nk)
            return -1;
        PdfObj **nv = (PdfObj **)grow(a, (void **)d->u.d.vals, d->u.d.len,
                                      &cap_v, sizeof(PdfObj *));
        if (!nv)
            return -1;

        d->u.d.keys = nk;
        d->u.d.vals = nv;
        d->u.d.cap  = cap_k < cap_v ? cap_k : cap_v;
    }

    d->u.d.keys[d->u.d.len] = key;
    d->u.d.vals[d->u.d.len] = v;
    d->u.d.len++;
    return 0;
}

PdfObj *pdf_dict_get(const PdfObj *d, const char *name)
{
    const PdfObj *t = pdf_as_dict(d);
    if (!t || t->kind != PDF_DICT || !name)
        return NULL;

    int n = (int)strlen(name);
    for (int i = 0; i < t->u.d.len; ++i) {
        if (t->u.d.keys[i].len == n &&
            memcmp(t->u.d.keys[i].p, name, (size_t)n) == 0)
            return t->u.d.vals[i];
    }
    return NULL;
}

int pdf_is(const PdfObj *o, PdfKind k)
{
    return o && o->kind == (unsigned char)k;
}

long long pdf_int(const PdfObj *o, long long def)
{
    if (!o)
        return def;
    if (o->kind == PDF_INT)
        return o->u.i;
    /* Aceitar REAL onde se espera INT nao e leniencia gratuita: /Length e
     * contagens aparecem como "12.0" em PDF gerado por ferramentas que tratam
     * todo numero como float. */
    if (o->kind == PDF_REAL)
        return (long long)o->u.r;
    return def;
}

double pdf_real(const PdfObj *o, double def)
{
    if (!o)
        return def;
    if (o->kind == PDF_REAL)
        return o->u.r;
    if (o->kind == PDF_INT)
        return (double)o->u.i;
    return def;
}

int pdf_bool(const PdfObj *o, int def)
{
    if (!o || o->kind != PDF_BOOL)
        return def;
    return o->u.b;
}

int pdf_name_is(const PdfObj *o, const char *name)
{
    if (!o || o->kind != PDF_NAME || !name)
        return 0;
    int n = (int)strlen(name);
    return o->u.s.len == n && memcmp(o->u.s.p, name, (size_t)n) == 0;
}

int pdf_arr_len(const PdfObj *o)
{
    return (o && o->kind == PDF_ARR) ? o->u.a.len : 0;
}

PdfObj *pdf_arr_get(const PdfObj *o, int i)
{
    if (!o || o->kind != PDF_ARR || i < 0 || i >= o->u.a.len)
        return NULL;
    return o->u.a.v[i];
}

const PdfObj *pdf_as_dict(const PdfObj *o)
{
    if (!o)
        return NULL;
    if (o->kind == PDF_DICT)
        return o;
    if (o->kind == PDF_STREAM)
        return o->u.stm.dict;
    return NULL;
}
