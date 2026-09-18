#ifndef EREADER_TXT_H
#define EREADER_TXT_H

#include "pdf_io.h"
#include "pdf_arena.h"
#include "reflow.h"

/*
 * Texto puro.
 *
 * Depois do PDF, este formato parece trivial - e quase e. As duas coisas que
 * ele exige de verdade sao as que um leitor ingenuo erra:
 *
 * 1. CODIFICACAO. Um .txt nao declara a sua. O arquivo pode ser UTF-8, pode ser
 *    Latin-1, pode ter BOM. Assumir UTF-8 e mostrar losangos em todo acento de
 *    um arquivo Latin-1 e o defeito classico, e num livro em portugues ele
 *    aparece em toda linha. Ver txt_detect_encoding.
 *
 * 2. QUEBRA DE LINHA DURA. Livro em texto puro - o feitio de todo Project
 *    Gutenberg - vem quebrado em ~70 colunas, com quebra de verdade no arquivo.
 *    Reexibir essas linhas numa tela de 480 px daria o mesmo defeito do PDF sem
 *    reflow: uma linha longa e uma curta, alternando. Entao aqui tambem se
 *    reconstroi o paragrafo, so que a partir de linhas em branco e recuo em vez
 *    de coordenadas - o que e MAIS confiavel que a heuristica do PDF, porque
 *    linha em branco e um sinal explicito e nao uma inferencia.
 *
 * Portavel: C99 puro sobre PdfIo. Testado no host.
 */

typedef enum {
    TXT_ENC_UTF8 = 0,
    TXT_ENC_LATIN1
} TxtEncoding;

/* Alvo de bytes por unidade de carga. Nao e um limite rigido: a unidade se
 * estende ate a proxima fronteira de paragrafo, para que o mesmo indice sempre
 * devolva o mesmo texto. */
#define TXT_UNIT_TARGET  3000
#define TXT_MAX_UNITS    8192

typedef struct {
    PdfStream   st;
    TxtEncoding enc;
    long long   data_off;    /* primeiro byte util (depois do BOM) */
    long long  *units;       /* offset de inicio de cada unidade */
    /*
     * clean[u] = a unidade u comeca numa fronteira de paragrafo de verdade.
     *
     * Vale 0 quando o corte teve de ser forcado no meio de um bloco maior que a
     * unidade. So nesse caso o paragrafo emenda com o da unidade vizinha, e e
     * essa distincao que impede o leitor de colar dois paragrafos que apenas
     * calharam de ficar em unidades adjacentes.
     */
    unsigned char *clean;
    int         nunits;
    int         truncated;   /* arquivo maior que TXT_MAX_UNITS unidades */
} TxtDoc;

/*
 * Abre e indexa. O indice e uma varredura unica do arquivo inteiro no momento
 * da abertura: e o preco de saber quantas unidades existem, que a barra de
 * progresso e o salto de posicao precisam saber. Um livro de 1 MB da ~350
 * unidades e o indice ocupa menos de 3 KB.
 *
 * `a` guarda o indice e precisa sobreviver ao TxtDoc.
 * Em sucesso assume a posse do io. Retorna 0 em sucesso.
 */
int  txt_open(TxtDoc *d, PdfArena *a, const PdfIo *io);
void txt_close(TxtDoc *d);

/*
 * Le uma unidade e monta os paragrafos. Tudo alocado em `a` (a arena de
 * pagina, reciclada a cada virada). Retorna 0 em sucesso.
 */
int  txt_unit(TxtDoc *d, PdfArena *a, int unit, Reflow *out);

const char *txt_encoding_name(TxtEncoding e);

#endif
