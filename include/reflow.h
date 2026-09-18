#ifndef EREADER_REFLOW_H
#define EREADER_REFLOW_H

#include "textlines.h"

/*
 * Linhas de origem -> paragrafos logicos.
 *
 * Este e o modulo que justifica o projeto. Um PDF nao tem paragrafos: tem
 * linhas com posicao, quebradas na largura do papel A4 por um diagramador que
 * nunca imaginou uma tela de 480x272. Reexibir essas linhas como estao - que e o
 * que a Etapa 3b fazia - produz o defeito visivel na primeira tela: cada linha
 * do livro vira uma longa mais uma curta, e toda palavra hifenizada no fim de
 * linha fica partida no meio.
 *
 * Reconstruir o paragrafo e o que permite repaginar. E, ao contrario de tudo na
 * camada de PDF abaixo daqui, NAO E ALGORITMO: nao existe no arquivo nenhuma
 * marca dizendo "aqui termina o paragrafo". So ha coordenadas, e a decisao sai
 * de heuristica sobre elas:
 *
 *   - lacuna vertical maior que o entrelinha    -> quebra
 *   - primeira linha recuada                    -> quebra
 *   - linha anterior terminando curta           -> quebra
 *   - mudanca de corpo                          -> quebra, e titulo
 *
 * Cada regra tem contra-exemplo, e e por isso que o modulo e portavel e
 * exercitado no host: mexer numa constante daqui e barato de julgar com
 * `.\test.ps1 reflow` e caro de julgar olhando a tela do console.
 *
 * Portavel: C99 puro, nenhuma dependencia de PSPSDK.
 */

/*
 * Teto de paragrafos por unidade de carga.
 *
 * 1024 e nao 512 por causa do EPUB: uma pagina de PDF nunca chega perto disso,
 * mas um capitulo de 48 KB de dialogo curto - peca de teatro, poesia, dicionario
 * - passa de 512 com facilidade, e estourar o teto significa PERDER texto. O
 * custo e a tabela de paragrafos: 1024 entradas sao ~32 KB de arena, contra os
 * ~800 KB que uma pagina densa ja usa.
 */
#define RF_MAX_PARAS  1024
#define RF_MAX_COLS   4

/* Flags de paragrafo. */
#define RF_HEADING  0x01u   /* corpo acima do corpo do texto: titulo */
#define RF_CENTER   0x02u   /* centralizado na coluna */
#define RF_CONT     0x04u   /* provavel continuacao da pagina anterior */
#define RF_OPEN     0x08u   /* provavelmente continua na pagina seguinte */

typedef struct {
    int      off, len;   /* fatia UTF-8 em Reflow.buf */
    float    size;       /* corpo dominante */
    float    x;          /* X da primeira linha de origem, em pontos */
    float    x_end;      /* X do fim da primeira linha - deteccao de centro */
    float    y;          /* Y da primeira linha de origem, em pontos */
    int      nlines;     /* linhas de origem que entraram */
    int      col;        /* coluna de origem, base 0 */
    unsigned flags;
} RfPara;

typedef struct {
    RfPara *paras;
    int     nparas;
    char   *buf;
    int     buflen;

    /* --- diagnostico -------------------------------------------------------
     * Existe porque toda decisao aqui e heuristica: sem contadores nao ha como
     * saber se a deteccao de rodape comeu um paragrafo de verdade. */
    int   lines_in;
    int   lines_dropped;   /* cabecalho, rodape, numero de pagina */
    int   hyphen_joins;    /* palavras remontadas */
    int   columns;         /* colunas detectadas (1 = pagina normal) */
    float body_size;       /* corpo modal do texto corrido */
    float leading;         /* entrelinha modal, em pontos */
    float body_x0;         /* margem esquerda do corpo */
    float body_x1;         /* margem direita do corpo */
    int   truncated;
} Reflow;

typedef struct {
    int drop_running;    /* descartar cabecalho e rodape        (padrao 1) */
    int detect_columns;  /* desfazer coluna dupla               (padrao 1) */
    int dehyphenate;     /* remontar palavra partida no fim     (padrao 1) */
} RfOpts;

void reflow_defaults(RfOpts *o);

/*
 * Emenda uma linha ao fim do paragrafo em construcao.
 *
 * Isolada do resto porque a regra de hifen nao tem nada a ver com PDF: ela vale
 * para qualquer formato cujo texto venha quebrado em linhas de largura fixa, e
 * isso inclui TXT de largura fixa (o feitio de todo livro do Project Gutenberg)
 * e XHTML de EPUB com quebra dura. Duplica-la em cada leitor de formato seria
 * garantir que as tres copias divergissem.
 *
 * `buf`/`len` sao o buffer e o comprimento atual; `para_start` e onde ESTE
 * paragrafo comeca, para que a emenda nunca coma o texto do anterior. Insere um
 * espaco quando emenda, exceto depois de hifen.
 *
 * Retorna 1 se removeu um hifen de quebra, 0 caso contrario.
 */
int reflow_append(char *buf, int cap, int *len, int para_start,
                  const char *line, int nlen, int dehyphenate);

/*
 * Monta os paragrafos. Tudo alocado em `a`; `tl` pode ser descartado depois.
 * Retorna 0 em sucesso (nparas pode ser 0: pagina sem texto aproveitavel).
 * `o` NULL usa os padroes.
 */
int reflow_build(PdfArena *a, const TextLines *tl, const RfOpts *o, Reflow *out);

#endif
