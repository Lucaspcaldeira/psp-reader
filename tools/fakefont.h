#ifndef EREADER_FAKEFONT_H
#define EREADER_FAKEFONT_H

#include <stddef.h>
#include "layout.h"
#include "utf8.h"

/*
 * Metrica de fonte sintetica para os testes de host.
 *
 * O layout mede por callback exatamente para poder ser testado sem FreeType.
 * Esta e a implementacao do outro lado: uma tabela grosseira de largura por
 * classe de caractere, calibrada por olho contra uma serifada de texto.
 *
 * NAO precisa ser exata, e nao deve fingir que e. O que se testa aqui e a
 * DECISAO de quebra - se a palavra que nao cabe desce, se a que cabe fica, se o
 * laco progride numa palavra maior que a linha. Para isso, "proporcional e
 * plausivel" e suficiente; a largura de verdade so existe no console, com o
 * glifo rasterizado.
 */

static float ff_cp_em(unsigned int c)
{
    switch (c) {
    case ' ':  return 0.26f;
    case 'i': case 'l': case 'j': case 'I': case '.': case ',':
    case ';': case ':': case '!': case '|': case '\'': case '`':
        return 0.30f;
    case '(': case ')': case '[': case ']': case '/': case '-':
    case 'f': case 't': case 'r':
        return 0.38f;
    case 'm': case 'w': return 0.80f;
    case 'M': case 'W': return 0.88f;
    default: break;
    }
    if (c == 0x2014) return 1.00f;              /* travessao */
    if (c == 0x2013) return 0.50f;              /* meia risca */
    if (c == 0x2026) return 0.90f;              /* reticencias */
    if (c >= 'A' && c <= 'Z') return 0.66f;
    if (c >= '0' && c <= '9') return 0.50f;
    if (c >= 0xC0 && c <= 0xDE) return 0.66f;   /* maiuscula acentuada */
    return 0.50f;
}

typedef struct { float px; } FakeFont;

static float ff_measure(void *ud, const char *s, int n)
{
    const FakeFont *ff = (const FakeFont *)ud;
    if (!s)
        return 0.0f;
    if (n < 0) {
        size_t k = 0;
        while (s[k]) k++;
        n = (int)k;
    }
    const char *p = s, *end = s + n;
    float em = 0.0f;
    while (p < end)
        em += ff_cp_em(utf8_next(&p, end));
    return em * ff->px;
}

static void ff_init(FakeFont *ff, LayoutFont *lf, float px)
{
    ff->px = px;
    lf->ud = ff;
    lf->measure = ff_measure;
    /* Entrelinha tipica de uma serifada: ~1,2 do corpo. */
    lf->line_height = px * 1.2f;
}

#endif
