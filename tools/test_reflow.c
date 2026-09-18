/*
 * Teste de host para src/read/reflow.c e src/read/layout.c.
 * Roda via  .\test.ps1 reflow
 *
 * SINTETICO de proposito, sem PDF nenhum. O reflow decide por heuristica sobre
 * coordenadas, e a unica forma de julgar uma heuristica e alimentar o caso exato
 * que ela precisa acertar - e, mais importante, o caso vizinho que ela NAO deve
 * confundir com aquele. Um PDF real nao permite isso: nao da para pedir a ele
 * "agora a mesma pagina, mas alinhada a esquerda".
 *
 * A conferencia contra livro de verdade e o outro teste:
 *   .\test.ps1 pdfreflow "testdata\cnv.pdf" 20
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "reflow.h"
#include "layout.h"
#include "fakefont.h"

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, fmt, ...)                                                  \
    do {                                                                       \
        if (cond) { g_pass++; }                                                \
        else { g_fail++; printf("  FALHA %s:%d  " fmt "\n",                    \
                                __FILE__, __LINE__, ##__VA_ARGS__); }          \
    } while (0)

/* --- construcao de TextLines a mao ---------------------------------------- */

/*
 * Especificacao de uma linha de origem.
 *
 * x_end explicito, e nao medido: em texto justificado o diagramador ESTICA os
 * espacos para fechar a margem, entao o fim da linha nao e funcao do texto. E
 * justamente essa diferenca entre justificado e alinhado a esquerda que decide
 * qual regra de fim de paragrafo o reflow aplica, entao ela tem de ser dado de
 * entrada aqui.
 */
typedef struct {
    const char *t;
    float x, x_end, y, size;
    float gap_x, gap_w;   /* calha de coluna, 0 = sem */
    int   gap_at;         /* byte do espaco que a representa, -1 = sem */
} LSpec;

static void build_tl(TextLines *tl, const LSpec *ls, int n,
                     float page_w, float page_h, char *buf, TextLine *lines)
{
    memset(tl, 0, sizeof(*tl));
    tl->lines = lines;
    tl->buf = buf;
    tl->page_w = page_w;
    tl->page_h = page_h;

    int bl = 0;
    for (int i = 0; i < n; ++i) {
        int len = (int)strlen(ls[i].t);
        memcpy(buf + bl, ls[i].t, (size_t)len);
        TextLine *L = &lines[i];
        L->off = bl;
        L->len = len;
        L->x = ls[i].x;
        L->x_end = ls[i].x_end;
        L->y = ls[i].y;
        L->size = ls[i].size;
        L->gap_x = ls[i].gap_x;
        L->gap_w = ls[i].gap_w;
        L->gap_off = (ls[i].gap_at >= 0) ? bl + ls[i].gap_at : -1;
        bl += len;
    }
    tl->nlines = n;
    tl->buflen = bl;
}

/* Compara um paragrafo com o texto esperado, imprimindo os dois no erro - sem
 * isso um teste de reflow que falha nao diz nada de util. */
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

/* --- casos ---------------------------------------------------------------- */

#define SETUP(specs)                                                           \
    TextLines tl;                                                              \
    static char sbuf[8192];                                                    \
    static TextLine slines[256];                                               \
    build_tl(&tl, specs, (int)(sizeof(specs) / sizeof(specs[0])),              \
             595.0f, 842.0f, sbuf, slines);                                    \
    PdfArena ar;                                                               \
    pdf_arena_init(&ar, 64 * 1024);                                            \
    Reflow rf;                                                                 \
    RfOpts op;                                                                 \
    reflow_defaults(&op)

/*
 * Livro justificado com recuo de primeira linha: o caso comum em portugues.
 *
 * Margem util 72..523. As linhas internas fecham em 523 (justificado); as
 * ultimas de paragrafo terminam onde o texto acabou.
 */
static void test_justificado_com_recuo(void)
{
    printf("-- justificado com recuo de primeira linha --\n");
    static const LSpec ls[] = {
      { "O primeiro paragrafo comeca recuado e se-",  86, 523, 700, 10, 0,0,-1 },
      { "gue justificado ate o fim desta linha,",     72, 523, 688, 10, 0,0,-1 },
      { "fechando curto aqui.",                       72, 240, 676, 10, 0,0,-1 },
      { "O segundo paragrafo tambem e recuado e",     86, 523, 664, 10, 0,0,-1 },
      { "termina logo.",                              72, 180, 652, 10, 0,0,-1 },
    };
    SETUP(ls);

    CHECK(reflow_build(&ar, &tl, &op, &rf) == 0, "reflow_build falhou");
    CHECK(rf.nparas == 2, "esperava 2 paragrafos, saiu %d", rf.nparas);

    /* "se-" + "gue" remontado: a palavra estava partida pela largura do papel,
     * que nao existe mais depois do reflow. */
    expect_para(&rf, 0, "O primeiro paragrafo comeca recuado e segue "
                        "justificado ate o fim desta linha, fechando curto aqui.");
    expect_para(&rf, 1, "O segundo paragrafo tambem e recuado e termina logo.");
    CHECK(rf.hyphen_joins == 1, "esperava 1 juncao de hifen, houve %d",
          rf.hyphen_joins);
    pdf_arena_free(&ar);
}

/*
 * O caso vizinho e perigoso: texto ALINHADO A ESQUERDA.
 *
 * Aqui TODA linha termina curta. Uma regra de "terminou curto = fim de
 * paragrafo" calibrada no caso justificado quebraria cada linha num paragrafo,
 * devolvendo exatamente o defeito que o reflow existe para corrigir. A saida
 * correta e um paragrafo so.
 *
 * INVARIANTE do dado abaixo, e nao detalhe: numa pagina alinhada a esquerda a
 * linha quebrou porque a PROXIMA PALAVRA nao cabia. Entao cada x_end aqui esta
 * perto o bastante da margem para que a primeira palavra da linha seguinte
 * estourasse. Dado que viole isso nao e uma pagina alinhada a esquerda - e uma
 * pagina onde o diagramador quebrou linha no meio do nada, que nao existe, e
 * testar contra ela mede a regra errada.
 */
static void test_alinhado_esquerda(void)
{
    printf("-- alinhado a esquerda (nao justificado) --\n");
    /* Margem util 72..480. "irregular," ~50 pt, "pelo" ~20 pt, "termina"
     * ~35 pt: nenhuma delas cabe na sobra da linha anterior. */
    static const LSpec ls[] = {
      { "Nesta pagina o texto tem margem direita",  72, 470, 700, 10, 0,0,-1 },
      { "irregular, porque nao foi justificado",     72, 462, 688, 10, 0,0,-1 },
      { "pelo diagramador, e por isso cada linha",   72, 466, 676, 10, 0,0,-1 },
      { "termina num ponto diferente da anterior.",  72, 480, 664, 10, 0,0,-1 },
    };
    SETUP(ls);

    CHECK(reflow_build(&ar, &tl, &op, &rf) == 0, "reflow_build falhou");
    CHECK(rf.nparas == 1, "esperava 1 paragrafo, saiu %d", rf.nparas);
    expect_para(&rf, 0,
        "Nesta pagina o texto tem margem direita irregular, porque nao foi "
        "justificado pelo diagramador, e por isso cada linha termina num ponto "
        "diferente da anterior.");
    pdf_arena_free(&ar);
}

/* Paragrafo em bloco: sem recuo, separado por linha em branco. O unico sinal e
 * a lacuna vertical. */
static void test_bloco_com_linha_em_branco(void)
{
    printf("-- bloco separado por linha em branco --\n");
    static const LSpec ls[] = {
      { "Primeiro bloco, primeira linha dele,",  72, 523, 700, 10, 0,0,-1 },
      { "segunda linha do primeiro bloco.",      72, 400, 688, 10, 0,0,-1 },
      { "Segundo bloco, depois do vao,",         72, 523, 664, 10, 0,0,-1 },
      { "e a segunda linha dele.",               72, 300, 652, 10, 0,0,-1 },
    };
    SETUP(ls);

    CHECK(reflow_build(&ar, &tl, &op, &rf) == 0, "reflow_build falhou");
    CHECK(rf.nparas == 2, "esperava 2 paragrafos, saiu %d", rf.nparas);
    expect_para(&rf, 0, "Primeiro bloco, primeira linha dele, segunda linha do "
                        "primeiro bloco.");
    expect_para(&rf, 1, "Segundo bloco, depois do vao, e a segunda linha dele.");
    pdf_arena_free(&ar);
}

/* Hifen que NAO e de quebra: composto de verdade mantem o hifen. */
static void test_hifen_composto(void)
{
    printf("-- hifen de composto vs hifen de quebra --\n");
    static const LSpec ls[] = {
      { "Um exemplo de Comunicacao Nao-",       72, 523, 700, 10, 0,0,-1 },
      { "Violenta aplicada ao dia a dia, e de",  72, 523, 688, 10, 0,0,-1 },
      { "uma palavra qualquer bem compri-",      72, 523, 676, 10, 0,0,-1 },
      { "da que foi partida pelo diagramador.",  72, 470, 664, 10, 0,0,-1 },
    };
    SETUP(ls);

    CHECK(reflow_build(&ar, &tl, &op, &rf) == 0, "reflow_build falhou");
    expect_para(&rf, 0,
        "Um exemplo de Comunicacao Nao-Violenta aplicada ao dia a dia, e de "
        "uma palavra qualquer bem comprida que foi partida pelo diagramador.");
    CHECK(rf.hyphen_joins == 1, "esperava 1 juncao (so a minuscula), houve %d",
          rf.hyphen_joins);
    pdf_arena_free(&ar);
}

/* Cabecalho corrente e numero de pagina saem fora. O sinal e o isolamento
 * vertical, nao a posicao. */
static void test_cabecalho_e_rodape(void)
{
    printf("-- cabecalho corrente e numero de pagina --\n");
    static const LSpec ls[] = {
      { "COMUNICACAO NAO-VIOLENTA",              72, 260, 790, 9,  0,0,-1 },
      { "O texto do capitulo comeca aqui e vai", 72, 523, 700, 10, 0,0,-1 },
      { "seguindo normalmente por duas linhas.", 72, 460, 688, 10, 0,0,-1 },
      { "47",                                   290, 302,  56, 9,  0,0,-1 },
    };
    SETUP(ls);

    CHECK(reflow_build(&ar, &tl, &op, &rf) == 0, "reflow_build falhou");
    CHECK(rf.lines_dropped == 2, "esperava descartar 2 linhas, descartou %d",
          rf.lines_dropped);
    CHECK(rf.nparas == 1, "esperava 1 paragrafo, saiu %d", rf.nparas);
    expect_para(&rf, 0, "O texto do capitulo comeca aqui e vai seguindo "
                        "normalmente por duas linhas.");
    pdf_arena_free(&ar);
}

/* Com a deteccao desligada, as mesmas linhas voltam - confirma que o descarte
 * vem da regra e nao de um efeito colateral. */
static void test_cabecalho_desligado(void)
{
    printf("-- descarte de cabecalho desligavel --\n");
    static const LSpec ls[] = {
      { "COMUNICACAO NAO-VIOLENTA",              72, 260, 790, 9,  0,0,-1 },
      { "O texto do capitulo comeca aqui e vai", 72, 523, 700, 10, 0,0,-1 },
      { "seguindo normalmente por duas linhas.", 72, 460, 688, 10, 0,0,-1 },
      { "47",                                   290, 302,  56, 9,  0,0,-1 },
    };
    SETUP(ls);
    op.drop_running = 0;

    CHECK(reflow_build(&ar, &tl, &op, &rf) == 0, "reflow_build falhou");
    CHECK(rf.lines_dropped == 0, "com a regra desligada nao deveria descartar, "
          "descartou %d", rf.lines_dropped);
    CHECK(rf.nparas == 3, "esperava 3 paragrafos, saiu %d", rf.nparas);
    pdf_arena_free(&ar);
}

/*
 * O titulo do capitulo NAO e cabecalho corrente.
 *
 * Caso encontrado na pagina 30 do "Comunicacao Nao-Violenta": o titulo esta
 * isolado no alto da pagina e e curto, exatamente como um titulo corrente, e
 * foi engolido pela deteccao de rodape. Perder o titulo de um capitulo e pior
 * que manter um cabecalho: o cabecalho e ruido repetido, o titulo e conteudo
 * que nao aparece em nenhum outro lugar. O que separa os dois e o corpo -
 * cabecalho corrente e igual ou menor que o texto, titulo de capitulo e maior.
 */
static void test_titulo_nao_e_cabecalho(void)
{
    printf("-- titulo de capitulo nao e cabecalho --\n");
    static const LSpec ls[] = {
      { "2. A comunicacao que bloqueia a compaixao", 77, 450, 702, 19, 0,0,-1 },
      { "O texto do capitulo comeca bem abaixo do", 77, 535, 640, 15, 0,0,-1 },
      { "titulo, com o corpo do texto normal.",      77, 400, 622, 15, 0,0,-1 },
    };
    SETUP(ls);

    CHECK(reflow_build(&ar, &tl, &op, &rf) == 0, "reflow_build falhou");
    CHECK(rf.lines_dropped == 0, "o titulo do capitulo foi descartado como "
          "cabecalho (%d linhas fora)", rf.lines_dropped);
    expect_para(&rf, 0, "2. A comunicacao que bloqueia a compaixao");
    CHECK(rf.nparas > 0 && (rf.paras[0].flags & RF_HEADING),
          "o titulo deveria ter RF_HEADING");
    pdf_arena_free(&ar);
}

/*
 * Bloco com margem propria: epigrafe, citacao longa, verso.
 *
 * O outro defeito da pagina 30. Comparar o X de cada linha com a margem da
 * PAGINA acusa recuo em todas as linhas do bloco, e ele sai quebrado linha por
 * linha - o defeito que o reflow existe para corrigir, reaparecendo dentro do
 * reflow. A comparacao tem de ser com a linha ANTERIOR.
 *
 * O bloco aqui tem primeira linha recuada (152) sobre margem propria (137),
 * que e como a epigrafe do livro real esta diagramada, e fecha em 480 - sua
 * medida e mais estreita que a da pagina (535) nos DOIS lados, que e o outro
 * jeito de o bloco ser mal lido: contra a margem da pagina, toda linha dele
 * parece ter terminado curta, e a regra de fim de paragrafo dispara em todas.
 */
static void test_bloco_com_margem_propria(void)
{
    printf("-- bloco recuado com margem propria --\n");
    static const LSpec ls[] = {
      { "Uma linha de texto corrido antes da epigrafe,", 77, 535, 700, 15, 0,0,-1 },
      { "seguindo justificada pela pagina inteira,",     77, 535, 682, 15, 0,0,-1 },
      { "e mais uma para o corpo ser maioria,",          77, 535, 664, 15, 0,0,-1 },
      { "que termina aqui.",                             77, 200, 646, 15, 0,0,-1 },
      { "Nao julgueis, para que nao sejais julgados,",   152, 480, 610, 11, 0,0,-1 },
      { "pois com o critetio com que julgardes,",        137, 480, 596, 11, 0,0,-1 },
      { "sereis julgados.",                              137, 230, 582, 11, 0,0,-1 },
    };
    SETUP(ls);

    CHECK(reflow_build(&ar, &tl, &op, &rf) == 0, "reflow_build falhou");
    CHECK(rf.nparas == 2, "esperava 2 paragrafos (texto + epigrafe), saiu %d",
          rf.nparas);
    expect_para(&rf, 0, "Uma linha de texto corrido antes da epigrafe, seguindo "
                        "justificada pela pagina inteira, e mais uma para o "
                        "corpo ser maioria, que termina aqui.");
    expect_para(&rf, 1, "Nao julgueis, para que nao sejais julgados, pois com o "
                        "critetio com que julgardes, sereis julgados.");
    pdf_arena_free(&ar);
}

/* Titulo: corpo maior quebra o paragrafo e ganha a marca. */
static void test_titulo(void)
{
    printf("-- titulo por mudanca de corpo --\n");
    static const LSpec ls[] = {
      { "Capitulo 3",                            72, 200, 700, 18, 0,0,-1 },
      { "O texto do capitulo vem depois do",     86, 523, 660, 10, 0,0,-1 },
      { "titulo, com corpo menor.",              72, 300, 648, 10, 0,0,-1 },
    };
    SETUP(ls);

    CHECK(reflow_build(&ar, &tl, &op, &rf) == 0, "reflow_build falhou");
    CHECK(rf.nparas == 2, "esperava 2 paragrafos, saiu %d", rf.nparas);
    expect_para(&rf, 0, "Capitulo 3");
    CHECK(rf.nparas > 0 && (rf.paras[0].flags & RF_HEADING),
          "o titulo deveria ter RF_HEADING");
    CHECK(rf.nparas > 1 && !(rf.paras[1].flags & RF_HEADING),
          "o corpo nao deveria ter RF_HEADING");
    pdf_arena_free(&ar);
}

/*
 * Coluna dupla.
 *
 * O agrupamento por Y juntou a linha da esquerda com a da direita, porque estao
 * na mesma altura. A calha esta sempre no mesmo X, e e isso que o reflow usa
 * para desfazer a juncao. Sem isso o texto sai alternando entre as colunas,
 * frase por frase - ilegivel de um jeito que parece corrupcao de fonte.
 */
static void test_coluna_dupla(void)
{
    printf("-- coluna dupla --\n");
    /* Esquerda 72..280, calha 280..315, direita 315..523. O gap_at aponta o
     * espaco que representa a calha dentro do texto ja concatenado. */
    static const LSpec ls[] = {
      { "esquerda linha um dir linha um",   72, 523, 700, 10, 280, 35, 19 },
      { "esquerda linha dois dir linha dois", 72, 523, 688, 10, 280, 35, 20 },
      { "esquerda tres. dir tres.",         72, 523, 676, 10, 280, 35, 15 },
      { "esquerda quatro dir quatro",       72, 523, 664, 10, 280, 35, 16 },
      { "esquerda cinco dir cinco",         72, 523, 652, 10, 280, 35, 15 },
      { "esquerda seis dir seis",           72, 523, 640, 10, 280, 35, 14 },
      { "esquerda sete dir sete",           72, 523, 628, 10, 280, 35, 14 },
      { "esquerda oito dir oito",           72, 523, 616, 10, 280, 35, 14 },
    };
    SETUP(ls);

    CHECK(reflow_build(&ar, &tl, &op, &rf) == 0, "reflow_build falhou");
    CHECK(rf.columns == 2, "esperava detectar 2 colunas, detectou %d", rf.columns);

    /* A coluna esquerda tem de sair inteira ANTES da direita. */
    int ok_order = 0;
    if (rf.nparas >= 1) {
        const RfPara *p0 = &rf.paras[0];
        ok_order = (p0->len >= 8 &&
                    memcmp(rf.buf + p0->off, "esquerda", 8) == 0);
    }
    CHECK(ok_order, "o primeiro paragrafo deveria comecar pela coluna esquerda");

    int has_right = 0;
    for (int i = 0; i < rf.nparas; ++i)
        if (rf.paras[i].col == 1)
            has_right = 1;
    CHECK(has_right, "nenhum paragrafo saiu da coluna direita");

    /* Nenhum paragrafo deve MISTURAR as colunas: "esquerda ... dir ..." na
     * mesma frase e o defeito que este teste existe para pegar. */
    int mixed = 0;
    for (int i = 0; i < rf.nparas; ++i) {
        const RfPara *p = &rf.paras[i];
        int has_e = 0, has_d = 0;
        for (int k = 0; k + 8 <= p->len; ++k)
            if (memcmp(rf.buf + p->off + k, "esquerda", 8) == 0) has_e = 1;
        for (int k = 0; k + 4 <= p->len; ++k)
            if (memcmp(rf.buf + p->off + k, "dir ", 4) == 0) has_d = 1;
        if (has_e && has_d)
            mixed = 1;
    }
    CHECK(!mixed, "um paragrafo misturou as duas colunas");
    pdf_arena_free(&ar);
}

/* Pagina de coluna unica com um sumario pontilhado nao deve virar duas colunas:
 * ha lacuna larga, mas em X diferente em cada linha. */
static void test_nao_e_coluna(void)
{
    printf("-- lacuna larga que NAO e coluna --\n");
    static const LSpec ls[] = {
      { "Introducao 7",             72, 523, 700, 10, 150, 40,  10 },
      { "O primeiro capitulo 23",   72, 523, 688, 10, 260, 40,  19 },
      { "Um capitulo com nome longo 45", 72, 523, 676, 10, 330, 40, 26 },
      { "Notas 201",                72, 523, 664, 10, 120, 40,   5 },
      { "Indice 233",               72, 523, 652, 10, 130, 40,   6 },
      { "Agradecimentos 249",       72, 523, 640, 10, 230, 40,  14 },
      { "Sobre o autor 251",        72, 523, 628, 10, 210, 40,  13 },
      { "Colofao 255",              72, 523, 616, 10, 140, 40,   7 },
    };
    SETUP(ls);

    CHECK(reflow_build(&ar, &tl, &op, &rf) == 0, "reflow_build falhou");
    CHECK(rf.columns == 1, "lacunas em X diferente nao sao calha, mas detectou "
          "%d colunas", rf.columns);
    pdf_arena_free(&ar);
}

/* Pagina vazia e pagina de uma linha nao devem estourar nada. */
static void test_degenerado(void)
{
    printf("-- casos degenerados --\n");
    {
        TextLines tl;
        memset(&tl, 0, sizeof(tl));
        PdfArena ar;
        pdf_arena_init(&ar, 4096);
        Reflow rf;
        CHECK(reflow_build(&ar, &tl, NULL, &rf) == 0, "pagina vazia falhou");
        CHECK(rf.nparas == 0, "pagina vazia deu %d paragrafos", rf.nparas);
        pdf_arena_free(&ar);
    }
    {
        static const LSpec ls[] = {
          { "uma linha so", 72, 200, 700, 10, 0,0,-1 },
        };
        SETUP(ls);
        CHECK(reflow_build(&ar, &tl, &op, &rf) == 0, "uma linha falhou");
        CHECK(rf.nparas == 1, "uma linha deu %d paragrafos", rf.nparas);
        expect_para(&rf, 0, "uma linha so");
        pdf_arena_free(&ar);
    }
}

/* --- layout --------------------------------------------------------------- */

static void test_layout_quebra(void)
{
    printf("-- layout: quebra de linha e paginacao --\n");
    static const LSpec ls[] = {
      { "Um paragrafo longo o suficiente para",  86, 523, 700, 10, 0,0,-1 },
      { "ocupar varias linhas de tela quando",   72, 523, 688, 10, 0,0,-1 },
      { "repaginado para a largura de 452 pixels", 72, 523, 676, 10, 0,0,-1 },
      { "que sobra na tela do console.",         72, 300, 664, 10, 0,0,-1 },
    };
    SETUP(ls);
    CHECK(reflow_build(&ar, &tl, &op, &rf) == 0, "reflow_build falhou");
    CHECK(rf.nparas == 1, "esperava 1 paragrafo, saiu %d", rf.nparas);

    FakeFont ff;
    LayoutFont lf;
    ff_init(&ff, &lf, 18.0f);

    LayoutOpts lo;
    layout_defaults(&lo);
    lo.indent = 18.0f;

    Layout lay;
    /* 452 x 222: a area util real do leitor a 480x272 com barra e rodape. */
    CHECK(layout_build(&ar, &rf, &lf, 452.0f, 222.0f, &lo, &lay) == 0,
          "layout_build falhou");
    CHECK(lay.nlines > 1, "esperava mais de uma linha de tela, saiu %d",
          lay.nlines);
    CHECK(lay.npages >= 1, "esperava ao menos uma pagina");

    /* NENHUMA linha pode passar da largura util - e para isso que a medicao
     * existe. */
    for (int i = 0; i < lay.nlines; ++i)
        CHECK(lay.lines[i].w <= 452.0f + 0.5f,
              "linha %d mede %.1f, acima dos 452 uteis", i, lay.lines[i].w);

    /* O texto todo tem de sair, sem byte perdido nem repetido: cada linha
     * continua de onde a anterior parou, a menos dos espacos comidos. */
    int bytes = 0;
    for (int i = 0; i < lay.nlines; ++i)
        bytes += lay.lines[i].len;
    CHECK(bytes > rf.paras[0].len - lay.nlines - 1 && bytes <= rf.paras[0].len,
          "as linhas somam %d bytes, o paragrafo tem %d", bytes,
          rf.paras[0].len);

    CHECK(lay.nlines > 0 && (lay.lines[0].flags & LL_FIRST),
          "a primeira linha deveria ter LL_FIRST");
    CHECK(lay.nlines > 0 && lay.lines[0].x == 18.0f,
          "a primeira linha deveria estar recuada 18px, esta em %.1f",
          lay.nlines > 0 ? lay.lines[0].x : -1.0f);
    CHECK(lay.nlines > 1 && lay.lines[1].x == 0.0f,
          "a segunda linha nao deveria ter recuo");
    CHECK(lay.nlines > 0 && (lay.lines[lay.nlines-1].flags & LL_LAST),
          "a ultima linha deveria ter LL_LAST");

    /* A sentinela de page_first e o que dispensa caso especial na ultima
     * pagina. */
    CHECK(lay.page_first[lay.npages] == lay.nlines,
          "sentinela de page_first errada: %d, esperado %d",
          lay.page_first[lay.npages], lay.nlines);
    pdf_arena_free(&ar);
}

/*
 * Palavra maior que a linha.
 *
 * Este e o caso que TRAVA o leitor se estiver errado: se a quebra devolve zero
 * bytes, o laco de paginacao nao progride e o console congela no meio de um
 * livro. Aqui o teste tem de terminar - se ele nao terminar, a falha e o
 * timeout, e ja e o diagnostico.
 */
static void test_layout_palavra_gigante(void)
{
    printf("-- layout: palavra maior que a linha --\n");
    static const LSpec ls[] = {
      { "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        72, 523, 700, 10, 0,0,-1 },
    };
    SETUP(ls);
    CHECK(reflow_build(&ar, &tl, &op, &rf) == 0, "reflow_build falhou");

    FakeFont ff;
    LayoutFont lf;
    ff_init(&ff, &lf, 30.0f);            /* corpo grande: nada cabe */

    Layout lay;
    CHECK(layout_build(&ar, &rf, &lf, 60.0f, 222.0f, NULL, &lay) == 0,
          "layout_build falhou");
    CHECK(lay.nlines >= 2, "esperava varias linhas, saiu %d", lay.nlines);
    for (int i = 0; i < lay.nlines; ++i)
        CHECK(lay.lines[i].len > 0, "linha %d saiu vazia - o laco travaria", i);
    pdf_arena_free(&ar);
}

/* Largura absurda: uma linha mais estreita que um glifo nao pode gerar linha
 * vazia nem laco infinito. */
static void test_layout_largura_absurda(void)
{
    printf("-- layout: largura menor que um glifo --\n");
    static const LSpec ls[] = {
      { "abc def ghi", 72, 200, 700, 10, 0,0,-1 },
    };
    SETUP(ls);
    CHECK(reflow_build(&ar, &tl, &op, &rf) == 0, "reflow_build falhou");

    FakeFont ff;
    LayoutFont lf;
    ff_init(&ff, &lf, 24.0f);

    Layout lay;
    CHECK(layout_build(&ar, &rf, &lf, 3.0f, 100.0f, NULL, &lay) == 0,
          "layout_build falhou");
    for (int i = 0; i < lay.nlines; ++i)
        CHECK(lay.lines[i].len > 0, "linha %d vazia", i);
    pdf_arena_free(&ar);
}

/* Paginacao: com altura para 3 linhas, um texto de 10 linhas tem de dar 4
 * paginas de tela, e as linhas de cada pagina tem de caber nela. */
static void test_layout_paginacao(void)
{
    printf("-- layout: quantas telas --\n");
    static const LSpec ls[] = {
      { "Palavra uma duas tres quatro cinco seis sete oito nove dez onze doze "
        "treze quatorze quinze dezesseis dezessete dezoito dezenove vinte "
        "vinte e um vinte e dois vinte e tres vinte e quatro vinte e cinco",
        72, 523, 700, 10, 0,0,-1 },
    };
    SETUP(ls);
    CHECK(reflow_build(&ar, &tl, &op, &rf) == 0, "reflow_build falhou");

    FakeFont ff;
    LayoutFont lf;
    ff_init(&ff, &lf, 18.0f);           /* line_height 21,6; step 27,6 */

    LayoutOpts lo;
    layout_defaults(&lo);

    Layout lay;
    CHECK(layout_build(&ar, &rf, &lf, 200.0f, 90.0f, &lo, &lay) == 0,
          "layout_build falhou");
    CHECK(lay.npages >= 2, "esperava mais de uma tela, saiu %d", lay.npages);

    float step = lf.line_height * lo.line_mul;
    for (int p = 0; p < lay.npages; ++p) {
        int a0 = lay.page_first[p], a1 = lay.page_first[p + 1];
        CHECK(a1 > a0, "tela %d saiu vazia", p);
        for (int i = a0; i < a1; ++i)
            CHECK(lay.lines[i].top + step <= 90.0f + 0.5f,
                  "linha %d da tela %d passa do rodape (top=%.1f)",
                  i, p, lay.lines[i].top);
        /* Toda tela recomeca do topo: `top` e relativo a pagina, e nao ao
         * documento - e isso que deixa o desenho ser uma soma so. */
        CHECK(lay.lines[a0].top == 0.0f,
              "tela %d comeca em top=%.1f, deveria ser 0", p, lay.lines[a0].top);
    }

    /* layout_page_of_line tem de concordar com a tabela. */
    for (int p = 0; p < lay.npages; ++p) {
        int a0 = lay.page_first[p], a1 = lay.page_first[p + 1];
        for (int i = a0; i < a1; ++i)
            CHECK(layout_page_of_line(&lay, i) == p,
                  "linha %d: page_of_line deu %d, esperado %d", i,
                  layout_page_of_line(&lay, i), p);
    }
    pdf_arena_free(&ar);
}

int main(void)
{
    test_justificado_com_recuo();
    test_alinhado_esquerda();
    test_bloco_com_linha_em_branco();
    test_hifen_composto();
    test_cabecalho_e_rodape();
    test_cabecalho_desligado();
    test_titulo_nao_e_cabecalho();
    test_bloco_com_margem_propria();
    test_titulo();
    test_coluna_dupla();
    test_nao_e_coluna();
    test_degenerado();
    test_layout_quebra();
    test_layout_palavra_gigante();
    test_layout_largura_absurda();
    test_layout_paginacao();

    printf("\n%d ok, %d falhas\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
