#ifndef EREADER_PDF_TEXT_H
#define EREADER_PDF_TEXT_H

#include "pdf_doc.h"
#include "pdf_font.h"

/*
 * Extracao de texto: content stream -> runs de texto posicionados.
 *
 * Um content stream nao tem paragrafos, linhas nem palavras. Tem comandos de
 * desenho: "coloque a matriz de texto aqui, escreva estes codigos com esta
 * fonte". A saida deste modulo e o nivel mais baixo que ainda carrega
 * significado - pedacos de texto com POSICAO - e e o que o reflow da Etapa 3c
 * transforma de volta em paragrafos.
 *
 * Um run e um trecho mostrado por UM operador de exibicao. Isso importa: PDFs
 * editorados frequentemente posicionam palavra por palavra sem escrever nenhum
 * caractere de espaco, e a unica pista de que ha um espaco entre duas palavras
 * e a lacuna horizontal entre dois runs. Concatenar tudo numa string perderia
 * essa informacao para sempre.
 */

#define PDF_TEXT_MAX_RUNS   6144
#define PDF_TEXT_MAX_BYTES  (48 * 1024)
#define PDF_TEXT_MAX_FONTS  48

typedef struct {
    float x, y;      /* origem no espaco do dispositivo, em pontos */
    float size;      /* corpo efetivo em pontos, ja com CTM aplicada */
    float width;     /* modulo do avanco total do run, em pontos */
    /*
     * Avanco como VETOR, nao so modulo.
     *
     * Em pagina normal dy e zero e dx == width, e a direcao nao interessa. Ela
     * passa a interessar em pagina com /Rotate: ali o content stream desenha o
     * texto girado, entao o run avanca ao longo de Y no espaco do usuario. Sem o
     * vetor nao ha como saber se "x + width" ou "y + width" e o fim do run, e a
     * pagina sai como uma coluna de silabas.
     */
    float dx, dy;
    int   off, len;  /* fatia UTF-8 dentro de PdfTextPage.text */
} PdfTextRun;

typedef struct {
    PdfTextRun *runs;
    int         nruns;

    char *text;      /* todos os runs concatenados, UTF-8, sem terminador */
    int   textlen;

    /* /MediaBox normalizado (x0 < x1, y0 < y1) e /Rotate. */
    float mb_x0, mb_y0, mb_x1, mb_y1;
    int   rotate;

    /* --- diagnostico ------------------------------------------------------ */
    int truncated;        /* estourou MAX_RUNS ou MAX_BYTES */
    int fonts_loaded;
    int codes_total;
    int codes_unmapped;   /* codigos sem mapeamento para Unicode */
    int forms_visited;    /* Form XObjects em que entramos */
    int inline_images;    /* blocos BI..EI pulados */
} PdfTextPage;

/*
 * Extrai o texto de uma pagina. Tudo e alocado em `a`.
 *
 * Retorna 0 se algo foi extraido, negativo se a pagina nao tem content stream
 * legivel. Uma pagina so de imagem devolve 0 com nruns == 0 - que e a
 * assinatura de "sem camada de texto", nao de erro.
 */
int pdf_text_extract(PdfDoc *doc, PdfArena *a, PdfObj *page, PdfTextPage *out);

#endif
