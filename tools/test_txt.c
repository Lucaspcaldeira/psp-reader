/*
 * Teste de host para src/read/txt.c. Roda via  .\test.ps1 txt
 *
 * Gera os arquivos que exercita, num diretorio temporario, em vez de depender de
 * um corpus: os casos que importam aqui sao de CODIFICACAO e de QUEBRA DE LINHA,
 * e escrever os bytes exatos e a unica forma de ter certeza de qual caso esta
 * sendo testado. Um .txt do mundo real nao diz qual e a sua codificacao - e
 * justamente por isso que o modulo existe.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "txt.h"
#include "doc.h"
#include "host_io.h"

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, fmt, ...)                                                  \
    do {                                                                       \
        if (cond) { g_pass++; }                                                \
        else { g_fail++; printf("  FALHA %s:%d  " fmt "\n",                    \
                                __FILE__, __LINE__, ##__VA_ARGS__); }          \
    } while (0)

static const char *TMP = "/tmp/ereader_txt_test.txt";

static void write_file(const void *data, size_t n)
{
    FILE *f = fopen(TMP, "wb");
    if (!f) { printf("  nao criou %s\n", TMP); exit(2); }
    fwrite(data, 1, n, f);
    fclose(f);
}

/* Abre o arquivo temporario e carrega uma unidade. Devolve 0 em sucesso. */
static int load(PdfArena *doc_a, PdfArena *pg_a, TxtDoc *d, int unit, Reflow *rf)
{
    PdfIo io;
    if (host_io_open(&io, TMP) != 0)
        return -1;
    if (txt_open(d, doc_a, &io) != 0)
        return -1;
    return txt_unit(d, pg_a, unit, rf);
}

static void expect_para(const Reflow *rf, int i, const char *want)
{
    if (i >= rf->nparas) {
        g_fail++;
        printf("  FALHA paragrafo %d nao existe (nparas=%d), esperado \"%s\"\n",
               i, rf->nparas, want);
        return;
    }
    const RfPara *p = &rf->paras[i];
    int wl = (int)strlen(want);
    if (p->len == wl && memcmp(rf->buf + p->off, want, (size_t)wl) == 0) {
        g_pass++;
        return;
    }
    g_fail++;
    printf("  FALHA paragrafo %d\n    obtido  : \"%.*s\"\n    esperado: \"%s\"\n",
           i, p->len, rf->buf + p->off, want);
}

/* ------------------------------------------------------------------------- */

/*
 * O feitio de todo livro em texto puro: quebra dura em ~70 colunas, paragrafo
 * separado por linha em branco.
 *
 * Sem remontar o paragrafo, essas linhas de 70 colunas apareceriam numa tela de
 * 480 px como uma longa e uma curta alternando - o mesmo defeito do PDF sem
 * reflow, e pela mesma razao: a largura para a qual o texto foi quebrado nao e a
 * largura em que ele vai ser lido.
 */
static void test_quebra_dura(void)
{
    printf("-- quebra dura em colunas fixas --\n");
    static const char SRC[] =
        "O primeiro paragrafo foi quebrado pelo autor em linhas de\n"
        "setenta colunas, que e como todo livro em texto puro vem.\n"
        "\n"
        "O segundo paragrafo vem depois de uma linha em branco, que\n"
        "e um sinal explicito e nao uma inferencia.\n";
    write_file(SRC, sizeof(SRC) - 1);

    PdfArena da, pa;
    pdf_arena_init(&da, 16384);
    pdf_arena_init(&pa, 16384);
    TxtDoc d;
    Reflow rf;

    CHECK(load(&da, &pa, &d, 0, &rf) == 0, "carga falhou");
    CHECK(rf.nparas == 2, "esperava 2 paragrafos, saiu %d", rf.nparas);
    expect_para(&rf, 0, "O primeiro paragrafo foi quebrado pelo autor em linhas "
                        "de setenta colunas, que e como todo livro em texto "
                        "puro vem.");
    expect_para(&rf, 1, "O segundo paragrafo vem depois de uma linha em branco, "
                        "que e um sinal explicito e nao uma inferencia.");
    txt_close(&d);
    pdf_arena_free(&pa);
    pdf_arena_free(&da);
}

/*
 * Latin-1 sem BOM: o defeito classico do leitor ingenuo.
 *
 * Assumir UTF-8 poria U+FFFD em todo acento, o que num livro em portugues e
 * varias vezes por linha. Os bytes aqui sao Latin-1 de verdade: E7 e c-cedilha,
 * E3 e a-til, E1 e a-agudo.
 */
static void test_latin1(void)
{
    printf("-- Latin-1 sem BOM --\n");
    static const unsigned char SRC[] = {
        0xE7, 0xE3, 'o', ' ', 'e', ' ', 'p', 0xE1, 'g', 'i', 'n', 'a', '\n'
    };
    write_file(SRC, sizeof(SRC));

    PdfArena da, pa;
    pdf_arena_init(&da, 16384);
    pdf_arena_init(&pa, 16384);
    TxtDoc d;
    Reflow rf;

    CHECK(load(&da, &pa, &d, 0, &rf) == 0, "carga falhou");
    CHECK(d.enc == TXT_ENC_LATIN1, "esperava Latin-1, detectou %s",
          txt_encoding_name(d.enc));
    /* "ção e página" em UTF-8. */
    expect_para(&rf, 0, "\xC3\xA7\xC3\xA3o e p\xC3\xA1gina");
    txt_close(&d);
    pdf_arena_free(&pa);
    pdf_arena_free(&da);
}

/* O mesmo texto em UTF-8 tem de ser detectado como UTF-8 e passar intacto. */
static void test_utf8(void)
{
    printf("-- UTF-8 sem BOM --\n");
    static const char SRC[] = "\xC3\xA7\xC3\xA3o e p\xC3\xA1gina\n";
    write_file(SRC, sizeof(SRC) - 1);

    PdfArena da, pa;
    pdf_arena_init(&da, 16384);
    pdf_arena_init(&pa, 16384);
    TxtDoc d;
    Reflow rf;

    CHECK(load(&da, &pa, &d, 0, &rf) == 0, "carga falhou");
    CHECK(d.enc == TXT_ENC_UTF8, "esperava UTF-8, detectou %s",
          txt_encoding_name(d.enc));
    expect_para(&rf, 0, "\xC3\xA7\xC3\xA3o e p\xC3\xA1gina");
    txt_close(&d);
    pdf_arena_free(&pa);
    pdf_arena_free(&da);
}

/* BOM UTF-8: encerra a adivinhacao e nao pode sobrar no texto. */
static void test_bom(void)
{
    printf("-- BOM UTF-8 --\n");
    static const char SRC[] = "\xEF\xBB\xBF" "Comeco do livro\n";
    write_file(SRC, sizeof(SRC) - 1);

    PdfArena da, pa;
    pdf_arena_init(&da, 16384);
    pdf_arena_init(&pa, 16384);
    TxtDoc d;
    Reflow rf;

    CHECK(load(&da, &pa, &d, 0, &rf) == 0, "carga falhou");
    CHECK(d.enc == TXT_ENC_UTF8, "esperava UTF-8");
    CHECK(d.data_off == 3, "o BOM deveria ter sido pulado, data_off=%lld",
          (long long)d.data_off);
    expect_para(&rf, 0, "Comeco do livro");
    txt_close(&d);
    pdf_arena_free(&pa);
    pdf_arena_free(&da);
}

/*
 * CRLF do Windows.
 *
 * Contar \r e \n como duas quebras transformaria todo arquivo do Windows numa
 * sequencia de paragrafos de uma linha - ou seja, exatamente o defeito que este
 * modulo existe para evitar, disparado so pela origem do arquivo.
 */
static void test_crlf(void)
{
    printf("-- CRLF do Windows --\n");
    static const char SRC[] =
        "Primeira linha do paragrafo,\r\n"
        "segunda linha do mesmo.\r\n"
        "\r\n"
        "Outro paragrafo.\r\n";
    write_file(SRC, sizeof(SRC) - 1);

    PdfArena da, pa;
    pdf_arena_init(&da, 16384);
    pdf_arena_init(&pa, 16384);
    TxtDoc d;
    Reflow rf;

    CHECK(load(&da, &pa, &d, 0, &rf) == 0, "carga falhou");
    CHECK(rf.nparas == 2, "esperava 2 paragrafos, saiu %d", rf.nparas);
    expect_para(&rf, 0, "Primeira linha do paragrafo, segunda linha do mesmo.");
    expect_para(&rf, 1, "Outro paragrafo.");
    txt_close(&d);
    pdf_arena_free(&pa);
    pdf_arena_free(&da);
}

/* Hifen de quebra tambem existe em texto puro. */
static void test_hifen(void)
{
    printf("-- hifen de quebra --\n");
    static const char SRC[] =
        "Uma palavra bem compri-\n"
        "da que o autor partiu, e Nao-\n"
        "Violenta que e composta.\n";
    write_file(SRC, sizeof(SRC) - 1);

    PdfArena da, pa;
    pdf_arena_init(&da, 16384);
    pdf_arena_init(&pa, 16384);
    TxtDoc d;
    Reflow rf;

    CHECK(load(&da, &pa, &d, 0, &rf) == 0, "carga falhou");
    expect_para(&rf, 0, "Uma palavra bem comprida que o autor partiu, e "
                        "Nao-Violenta que e composta.");
    CHECK(rf.hyphen_joins == 1, "esperava 1 hifen removido, houve %d",
          rf.hyphen_joins);
    txt_close(&d);
    pdf_arena_free(&pa);
    pdf_arena_free(&da);
}

/* Bloco recuado inteiro (citacao, verso) nao vira um paragrafo por linha. */
static void test_bloco_recuado(void)
{
    printf("-- bloco recuado nao vira um paragrafo por linha --\n");
    static const char SRC[] =
        "Texto normal antes da citacao, seguindo\n"
        "por duas linhas.\n"
        "\n"
        "    Este bloco esta todo recuado em quatro\n"
        "    espacos, como uma citacao longa costuma\n"
        "    aparecer em texto puro.\n";
    write_file(SRC, sizeof(SRC) - 1);

    PdfArena da, pa;
    pdf_arena_init(&da, 16384);
    pdf_arena_init(&pa, 16384);
    TxtDoc d;
    Reflow rf;

    CHECK(load(&da, &pa, &d, 0, &rf) == 0, "carga falhou");
    CHECK(rf.nparas == 2, "esperava 2 paragrafos, saiu %d", rf.nparas);
    expect_para(&rf, 1, "Este bloco esta todo recuado em quatro espacos, como "
                        "uma citacao longa costuma aparecer em texto puro.");
    txt_close(&d);
    pdf_arena_free(&pa);
    pdf_arena_free(&da);
}

/*
 * Arquivo grande: o indice de unidades tem de existir, cobrir tudo e nao
 * repetir texto. A unidade e a chave que o progresso de leitura guarda, entao
 * um indice instavel levaria o leitor de volta ao lugar errado.
 */
static void test_unidades(void)
{
    printf("-- indice de unidades --\n");
    char *big = (char *)malloc(200000);
    int n = 0;
    for (int p = 0; p < 400; ++p)
        n += sprintf(big + n,
                     "Paragrafo numero %d, com texto suficiente para ocupar "
                     "algum espaco e obrigar o indice a criar varias unidades "
                     "ao longo do arquivo inteiro.\n\n", p);
    write_file(big, (size_t)n);
    free(big);

    PdfArena da, pa;
    pdf_arena_init(&da, 65536);
    pdf_arena_init(&pa, 65536);

    PdfIo io;
    CHECK(host_io_open(&io, TMP) == 0, "nao abriu");
    TxtDoc d;
    CHECK(txt_open(&d, &da, &io) == 0, "txt_open falhou");
    CHECK(d.nunits > 5, "esperava varias unidades, saiu %d", d.nunits);
    CHECK(!d.truncated, "indice truncado num arquivo de %d bytes", n);

    /* As unidades sao crescentes e a primeira comeca no inicio. */
    CHECK(d.units[0] == d.data_off, "a unidade 0 nao comeca no inicio");
    int monotonic = 1;
    for (int i = 1; i < d.nunits; ++i)
        if (d.units[i] <= d.units[i - 1])
            monotonic = 0;
    CHECK(monotonic, "os offsets das unidades nao sao crescentes");

    /* Toda unidade carrega, e nenhuma sai vazia. */
    int empty = 0;
    for (int i = 0; i < d.nunits; ++i) {
        pdf_arena_reset(&pa);
        Reflow rf;
        if (txt_unit(&d, &pa, i, &rf) != 0 || rf.nparas == 0)
            empty++;
    }
    CHECK(empty == 0, "%d unidades de %d sairam vazias", empty, d.nunits);

    /* Estabilidade: a mesma unidade tem de dar o mesmo texto sempre. E o que o
     * progresso de leitura assume ao guardar um indice. */
    pdf_arena_reset(&pa);
    Reflow r1;
    txt_unit(&d, &pa, 3, &r1);
    int len1 = r1.buflen;
    char *copy = (char *)malloc((size_t)len1 + 1);
    memcpy(copy, r1.buf, (size_t)len1);

    pdf_arena_reset(&pa);
    Reflow r2;
    txt_unit(&d, &pa, 3, &r2);
    CHECK(r2.buflen == len1 && memcmp(r2.buf, copy, (size_t)len1) == 0,
          "a unidade 3 devolveu texto diferente na segunda leitura");
    free(copy);

    txt_close(&d);
    pdf_arena_free(&pa);
    pdf_arena_free(&da);
}

/* Arquivo vazio e arquivo de uma linha sem quebra final nao podem estourar. */
static void test_degenerado(void)
{
    printf("-- casos degenerados --\n");
    {
        /*
         * Arquivo de zero byte e recusado, e com mensagem propria.
         *
         * Nao ha o que exibir, entao "abrir e mostrar nada" seria pior: o
         * usuario ficaria olhando uma tela em branco sem saber se o leitor
         * quebrou ou se o arquivo esta vazio. Dizer qual dos dois e a diferenca
         * entre um erro e um mistério.
         */
        write_file("", 0);
        PdfArena da;
        pdf_arena_init(&da, 4096);
        Doc doc;
        PdfIo io;
        CHECK(host_io_open(&io, TMP) == 0, "nao abriu vazio");
        CHECK(doc_open(&doc, &da, &io, DOC_TXT) != 0,
              "arquivo vazio deveria ser recusado");
        CHECK(strstr(doc.err, "vazio") != NULL,
              "a mensagem deveria dizer que o arquivo esta vazio, disse \"%s\"",
              doc.err);
        doc_close(&doc);
        pdf_arena_free(&da);
    }
    {
        write_file("sem quebra no fim", 17);
        PdfArena da, pa;
        pdf_arena_init(&da, 4096);
        pdf_arena_init(&pa, 4096);
        TxtDoc d;
        Reflow rf;
        CHECK(load(&da, &pa, &d, 0, &rf) == 0, "carga falhou");
        expect_para(&rf, 0, "sem quebra no fim");
        txt_close(&d);
        pdf_arena_free(&pa);
        pdf_arena_free(&da);
    }
}

int main(void)
{
    test_quebra_dura();
    test_latin1();
    test_utf8();
    test_bom();
    test_crlf();
    test_hifen();
    test_bloco_recuado();
    test_unidades();
    test_degenerado();

    remove(TMP);
    printf("\n%d ok, %d falhas\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
