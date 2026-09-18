#ifndef EREADER_EPUB_H
#define EREADER_EPUB_H

#include "zip.h"
#include "reflow.h"

/*
 * EPUB.
 *
 * De longe o formato mais AGRADAVEL de suportar, e vale dizer por que: um EPUB
 * ja e texto marcado com estrutura. Onde o PDF obriga a adivinhar por
 * coordenadas o que e paragrafo, o EPUB traz um <p> dizendo. Todo o aparato
 * heuristico de reflow.c - lacuna vertical, margem de bloco, linha curta,
 * justificado ou nao - simplesmente nao e necessario aqui, porque a informacao
 * que ele tenta reconstruir nunca foi perdida.
 *
 * O caminho e:
 *
 *   ZIP -> META-INF/container.xml -> o OPF -> <spine> -> capitulos XHTML
 *
 * UNIDADE DE CARGA: um capitulo, ou um pedaco dele. Capitulo e a divisao
 * natural, mas existe EPUB de livro inteiro num arquivo so, e ali um capitulo
 * seriam megabytes. Entao capitulo grande e cortado em pedacos de tamanho fixo,
 * com o corte sempre logo depois de uma tag fechada - assim os pedacos se
 * encaixam exatamente e o mesmo indice devolve sempre o mesmo texto, que e o que
 * o progresso de leitura exige.
 */

#define EPUB_MAX_SPINE   1024
#define EPUB_MAX_UNITS   4096

/* Bytes de XHTML por unidade. Marcacao e verbosa: 48 KB de XHTML dao algo como
 * 20 KB de texto, ou seis a oito telas de leitura. */
#define EPUB_CHUNK       49152

/*
 * A unidade e uma FAIXA DE BYTES explicita, e nao um numero de pedaco.
 *
 * A primeira versao guardava (pedaco, total) e recalculava a faixa dos dois
 * lados por aritmetica, confiando que as duas contas dariam o mesmo. Davam - ate
 * o corte por numero de paragrafos entrar em cena: uma unidade densa batia no
 * teto de RF_MAX_PARAS e parava ANTES do fim da sua faixa, enquanto a unidade
 * seguinte comecava onde a aritmetica mandava. O paragrafo entre os dois pontos
 * sumia do livro, sem aviso nenhum.
 *
 * Guardar a faixa resolve por construcao: existe UM lugar que decide onde cada
 * unidade termina, e a proxima comeca exatamente onde a anterior parou.
 */
typedef struct {
    int       zi;         /* indice do membro no ZIP */
    long long from, to;   /* faixa no membro DESCOMPRIMIDO */
    int       first;      /* primeira faixa deste capitulo */
    int       last;       /* ultima faixa deste capitulo */
} EpubUnit;

typedef struct {
    Zip       zip;
    EpubUnit *units;
    int       nunits;
    int       nspine;
    int       truncated;
    char      title[96];
    char      author[96];
    char      err[64];
} EpubDoc;

/*
 * Abre, le o OPF e monta a lista de unidades. `a` e a arena do documento e
 * precisa sobreviver ao EpubDoc. Em sucesso assume a posse do io.
 */
int  epub_open(EpubDoc *d, PdfArena *a, const PdfIo *io);
void epub_close(EpubDoc *d);

/* Le uma unidade e monta os paragrafos. Tudo em `a`, a arena de pagina. */
int  epub_unit(EpubDoc *d, PdfArena *a, int unit, Reflow *out);

#endif
