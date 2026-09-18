#include <malloc.h>
#include <string.h>
#include "texture.h"

static unsigned int g_bytes = 0;

static int next_pow2(int v)
{
    int p = 8;                 /* o GU nao aceita textura menor que 8 */
    while (p < v && p < 512)
        p <<= 1;
    return p;
}

int tex_alloc(Texture *t, int w, int h)
{
    if (!t || w <= 0 || h <= 0 || w > 512 || h > 512)
        return -1;

    t->w  = w;
    t->h  = h;
    t->pw = next_pow2(w);
    t->ph = next_pow2(h);
    t->swizzled = 0;

    unsigned int bytes = (unsigned int)t->pw * (unsigned int)t->ph * 4u;

    /* memalign e nao malloc: o GU exige 16 bytes de alinhamento no ponteiro de
     * textura. Com malloc funciona por sorte na maioria dos casos e falha de
     * forma inexplicavel quando nao alinha. */
    t->data = (unsigned int *)memalign(16, bytes);
    if (!t->data) {
        t->w = t->h = t->pw = t->ph = 0;
        return -2;
    }

    memset(t->data, 0, bytes);
    g_bytes += bytes;
    return 0;
}

void tex_free(Texture *t)
{
    if (!t || !t->data)
        return;
    g_bytes -= (unsigned int)t->pw * (unsigned int)t->ph * 4u;
    free(t->data);
    t->data = NULL;
    t->w = t->h = t->pw = t->ph = 0;
}

unsigned int tex_bytes_used(void) { return g_bytes; }
