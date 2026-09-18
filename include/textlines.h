#ifndef EREADER_TEXTLINES_H
#define EREADER_TEXTLINES_H

#include "pdf_text.h"

/*
 * Runs posicionados -> linhas de texto.
 *
 * Primeiro estagio do reflow, e o unico que nao depende de heuristica de
 * paragrafo: agrupar por Y, ordenar por X e decidir onde ha espaco e mecanico.
 * A juncao de linhas em paragrafos, a hifenizacao e a deteccao de colunas
 * acontecem em reflow.c, em cima daqui.
 *
 * Portavel: nenhuma dependencia de PSPSDK. Testado no host.
 */

#define TL_MAX_LINES 1024

typedef struct {
    int   off, len;   /* fatia UTF-8 em TextLines.buf */
    float y;          /* Y no espaco do PDF (cresce para CIMA) */
    float x;          /* X do inicio da linha - indentacao */
    float x_end;      /* X do fim - deteccao de fim de paragrafo */
    float size;       /* maior corpo da linha */

    /*
     * Maior lacuna horizontal INTERNA da linha, e onde ela cai no texto.
     *
     * Existe por um motivo so: deteccao de coluna dupla. O agrupamento por Y
     * junta a linha da coluna esquerda com a da direita, porque as duas estao
     * na mesma altura - depois disso a informacao de que havia um vale vertical
     * entre elas esta perdida para sempre. Guardar a lacuna aqui e o que
     * permite ao reflow desfazer a juncao sem ter que reprocessar os runs.
     *
     * gap_w <= 0 significa "linha sem lacuna digna de nota".
     */
    float gap_x;      /* X do inicio da lacuna */
    float gap_w;      /* largura da lacuna, em pontos */
    int   gap_off;    /* byte do espaco que a representa, em TextLines.buf */
} TextLine;

typedef struct {
    TextLine *lines;
    int       nlines;
    char     *buf;
    int       buflen;
    int       truncated;

    /*
     * Caixa da pagina COMO EXIBIDA, em pontos, origem no canto.
     *
     * Ja com /Rotate aplicado e ja transladada pela origem do /MediaBox, entao
     * quem consome as linhas nao precisa saber nada disso: as coordenadas aqui
     * sao sempre "X cresce para a direita, Y cresce para cima, canto em zero".
     * E o reflow depende de conhecer a caixa para decidir o que e margem,
     * cabecalho e rodape.
     */
    float page_w, page_h;
    int   rotate;      /* /Rotate normalizado (0, 90, 180, 270) */

    int   runs_vertical;   /* runs descartados por correrem na vertical */
} TextLines;

/*
 * Constroi as linhas a partir de uma pagina extraida. Tudo alocado em `a`.
 * Retorna 0 em sucesso (nlines pode ser 0: pagina sem texto).
 */
int textlines_build(PdfArena *a, const PdfTextPage *tp, TextLines *out);

#endif
