#ifndef EREADER_DOC_H
#define EREADER_DOC_H

#include "pdf_doc.h"
#include "txt.h"
#include "epub.h"
#include "reflow.h"

/*
 * Documento, sem formato.
 *
 * Esta camada existe para que TUDO acima dela - paginacao, desenho, progresso de
 * leitura, controles - nao saiba de que formato o texto veio. Sem ela, cada
 * formato novo se espalharia pelo main.c em condicionais, e o terceiro formato
 * seria mais caro que o segundo.
 *
 * O contrato e estreito de proposito:
 *
 *   UNIDADE   a granularidade de CARGA. Pagina no PDF, bloco de ~3 KB no TXT,
 *             item da espinha no EPUB. Nao e o que o usuario vira - isso e a
 *             tela, que o layout produz. A unidade so precisa ser estavel: o
 *             mesmo indice devolve sempre o mesmo texto, porque o progresso de
 *             leitura guarda um indice.
 *
 *   SAIDA     Reflow, ou seja paragrafos logicos. Cada formato chega la pelo
 *             caminho que lhe cabe: o PDF por heuristica sobre coordenadas, o
 *             TXT por linha em branco e recuo. Nenhum dos dois vaza para cima.
 *
 * Portavel: C99 puro sobre PdfIo. Testado no host.
 */

typedef enum {
    DOC_NONE = 0,
    DOC_PDF,
    DOC_TXT,
    DOC_EPUB
} DocKind;

/* Numeros que so alguns formatos tem. Ficam aqui, e nao em Reflow, porque
 * Reflow e o resultado e estes sao o caminho - servem ao diagnostico. */
typedef struct {
    int runs;             /* PDF: runs de texto extraidos */
    int runs_vertical;    /* PDF: runs descartados por serem verticais */
    int codes_unmapped;   /* PDF: codigos sem mapeamento Unicode */
    int lines;            /* linhas de origem */
} DocStats;

typedef struct {
    DocKind kind;
    int     units;
    char    err[96];

    /* Metadados, vazios quando o formato nao os traz. */
    char    title[96];
    char    author[96];

    PdfDoc  pdf;
    TxtDoc  txt;
    EpubDoc epub;
} Doc;

const char *doc_kind_name(DocKind k);

/*
 * Abre. Em sucesso assume a posse do `io`; em falha o chamador continua dono e
 * `err` diz o que houve.
 *
 * `a` e a arena do DOCUMENTO (vive enquanto o livro estiver aberto), nao a de
 * pagina: o indice do TXT e a xref do PDF moram nela.
 */
int  doc_open(Doc *d, PdfArena *a, const PdfIo *io, DocKind kind);
void doc_close(Doc *d);

/*
 * Carrega uma unidade e devolve os paragrafos. Tudo alocado em `a`, que e a
 * arena de PAGINA e o chamador reseta a cada virada - e isso que mantem o uso
 * de memoria constante ao folhear.
 *
 * Retorna 0 em sucesso. `out->nparas == 0` com retorno 0 significa "unidade sem
 * texto aproveitavel" (capa, pagina de ilustracao), que nao e erro.
 */
int  doc_unit(Doc *d, PdfArena *a, int unit, Reflow *out, DocStats *st);

#endif
