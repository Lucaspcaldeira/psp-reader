#ifndef EREADER_LAYOUT_H
#define EREADER_LAYOUT_H

#include "reflow.h"

/*
 * Paragrafos -> paginas de TELA.
 *
 * O outro lado do reflow. O reflow desfez a paginacao do papel; aqui o texto e
 * repaginado para 480x272 com quebra de linha por largura real de glifo e o
 * corpo de fonte que o leitor escolheu. A "pagina" que o usuario vira com L/R
 * passa a ser esta, e nao mais a pagina do PDF.
 *
 * MEDICAO POR CALLBACK, e nao chamando font.c: este modulo e portavel e roda no
 * host, onde nao existe FreeType nem atlas de textura. Quebrar linha e uma
 * decisao sobre numeros, e a origem dos numeros nao deveria amarrar a decisao ao
 * console. O custo e uma chamada indireta por palavra - irrelevante diante da
 * rasterizacao.
 *
 * Contar caractere em vez de medir glifo seria mais simples e esta errado: em
 * fonte proporcional "WWWWW" e quase o triplo de "iiiii". A conta por caractere
 * quebra ora antes da margem, deixando a coluna esburacada, ora depois, cortando
 * palavra fora da tela.
 */

typedef struct {
    void  *ud;
    /* Largura de uma fatia UTF-8 em pixels. nbytes < 0 = ate o terminador. */
    float (*measure)(void *ud, const char *utf8, int nbytes);
    float  line_height;   /* avanco de linha da fonte, px */
} LayoutFont;

typedef struct {
    float line_mul;   /* multiplicador do entrelinha (1.0 = o da fonte) */
    float para_gap;   /* vao entre paragrafos, em avancos de linha */
    float head_gap;   /* vao extra antes de titulo, em avancos de linha */
    float indent;     /* recuo da primeira linha do paragrafo, px (0 desliga) */
} LayoutOpts;

/* Flags de linha de tela. */
#define LL_FIRST   0x01u   /* primeira linha do paragrafo */
#define LL_LAST    0x02u   /* ultima linha do paragrafo */
#define LL_HEAD    0x04u   /* paragrafo e titulo */
#define LL_CENTER  0x08u   /* desenhar centralizada */

typedef struct {
    int      off, len;   /* fatia UTF-8 em Reflow.buf */
    float    x;          /* deslocamento horizontal em px (recuo ou centro) */
    float    top;        /* topo da linha em px, RELATIVO a pagina de tela */
    float    w;          /* largura medida, px */
    int      para;
    unsigned flags;
} LayoutLine;

typedef struct {
    LayoutLine *lines;
    int         nlines;

    /* page_first[p] = indice da primeira linha da tela p. Tem npages+1
     * entradas: a ultima e nlines, o que dispensa caso especial na contagem de
     * linhas da ultima pagina. */
    int *page_first;
    int  npages;

    int truncated;
} Layout;

void layout_defaults(LayoutOpts *o);

/*
 * Repagina. Tudo alocado em `a`; as fatias apontam para `rf->buf`, que precisa
 * sobreviver ao Layout. `o` NULL usa os padroes.
 *
 * text_w e text_h sao a area util de texto em pixels, ja descontadas as margens
 * e as barras. Retorna 0 em sucesso.
 */
int layout_build(PdfArena *a, const Reflow *rf, const LayoutFont *f,
                 float text_w, float text_h, const LayoutOpts *o, Layout *out);

/* Em que pagina de tela cai uma linha. -1 se fora da faixa. */
int layout_page_of_line(const Layout *l, int line);

#endif
