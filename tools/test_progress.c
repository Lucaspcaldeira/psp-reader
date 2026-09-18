/*
 * Teste de host para src/read/progress.c. Roda via  .\test.ps1 progress
 *
 * O que se testa aqui e um ARQUIVO QUE SOBREVIVE: o progresso e escrito numa
 * sessao e lido em outra, semanas depois, possivelmente depois de o usuario ter
 * mexido nele com um editor de texto. Entao os casos que importam sao o
 * ida-e-volta exato, o nome de arquivo com caractere hostil, e o arquivo
 * corrompido - que nao pode derrubar o app na abertura.
 */
#include <stdio.h>
#include <string.h>

#include "progress.h"
#include "fs.h"

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, fmt, ...)                                                  \
    do {                                                                       \
        if (cond) { g_pass++; }                                                \
        else { g_fail++; printf("  FALHA %s:%d  " fmt "\n",                    \
                                __FILE__, __LINE__, ##__VA_ARGS__); }          \
    } while (0)

static const char *TMP = "/tmp/ereader_progress.txt";

static void test_ida_e_volta(void)
{
    printf("-- ida e volta --\n");
    remove(TMP);

    Progress p;
    progress_load(&p, TMP);
    CHECK(p.count == 0, "arquivo inexistente deveria dar lista vazia, deu %d",
          p.count);

    progress_set(&p, "livro.pdf", 12345, 42, 100, 3, 20);
    progress_set(&p, "outro.txt", 999, 7, 20, 0, 16);
    progress_set_theme(&p, 2);
    CHECK(progress_save(&p, TMP) == 0, "gravacao falhou");

    Progress q;
    progress_load(&q, TMP);
    CHECK(q.count == 2, "esperava 2 livros, leu %d", q.count);
    CHECK(q.theme == 2, "tema deveria ser 2, leu %d", q.theme);

    int unit = -1, total = -1, para = -1, px = -1;
    CHECK(progress_get(&q, "livro.pdf", 12345, &unit, &total, &para, &px) == 0,
          "nao achou livro.pdf");
    CHECK(unit == 42 && total == 100 && para == 3 && px == 20,
          "livro.pdf voltou unit=%d total=%d para=%d px=%d",
          unit, total, para, px);

    CHECK(progress_get(&q, "outro.txt", 999, &unit, &total, &para, &px) == 0,
          "nao achou outro.txt");
    CHECK(unit == 7 && total == 20 && para == 0 && px == 16,
          "outro.txt voltou unit=%d total=%d para=%d px=%d",
          unit, total, para, px);
}

/*
 * O tamanho faz parte da chave.
 *
 * Sem ele, trocar um livro por outro de mesmo nome - baixar outra edicao,
 * substituir um PDF por uma versao melhor - abriria o novo na pagina em que o
 * antigo parou, que e um lugar arbitrario. Melhor tratar como livro novo.
 */
static void test_chave_inclui_tamanho(void)
{
    printf("-- tamanho faz parte da chave --\n");
    remove(TMP);

    Progress p;
    progress_load(&p, TMP);
    progress_set(&p, "livro.pdf", 12345, 42, 100, 3, 20);
    progress_save(&p, TMP);

    Progress q;
    progress_load(&q, TMP);
    int unit = -1;
    CHECK(progress_get(&q, "livro.pdf", 12345, &unit, NULL, NULL, NULL) == 0,
          "mesmo tamanho deveria achar");
    CHECK(progress_get(&q, "livro.pdf", 999, &unit, NULL, NULL, NULL) != 0,
          "tamanho diferente NAO deveria achar");
}

/*
 * Nome de arquivo com TAB e com acento.
 *
 * TAB e o separador de campos, entao um nome que o contenha e exatamente o caso
 * que quebra um parser ingenuo. Aqui ele nao quebra porque o nome e o ULTIMO
 * campo: o resto da linha e o nome inteiro, sem corte.
 */
static void test_nome_hostil(void)
{
    printf("-- nome com TAB e com acento --\n");
    remove(TMP);

    const char *nome = "Comunica\xC3\xA7\xC3\xA3o\tN\xC3\xA3o-Violenta.pdf";

    Progress p;
    progress_load(&p, TMP);
    progress_set(&p, nome, 4242, 11, 30, 2, 18);
    progress_save(&p, TMP);

    Progress q;
    progress_load(&q, TMP);
    CHECK(q.count == 1, "esperava 1 livro, leu %d", q.count);
    int unit = -1, para = -1;
    CHECK(progress_get(&q, nome, 4242, &unit, NULL, &para, NULL) == 0,
          "nao achou o nome com TAB");
    CHECK(unit == 11 && para == 2, "voltou unit=%d para=%d", unit, para);
}

/*
 * Arquivo corrompido nao pode derrubar o app.
 *
 * Este arquivo vive no Memory Stick de um console que e desligado no botao. Ele
 * VAI aparecer truncado ou com lixo alguma hora, e a resposta certa e ignorar o
 * que nao se entende e abrir mesmo assim - perder o progresso e um aborrecimento,
 * nao abrir o leitor e um defeito.
 */
static void test_corrompido(void)
{
    printf("-- arquivo corrompido --\n");

    static const char *LIXO[] = {
        "",
        "lixo puro sem nenhuma estrutura",
        "#ereader1\nb\t\n",                          /* linha sem campos */
        "#ereader1\nb\t12\t",                        /* linha truncada */
        "#ereader1\ntheme\n",                        /* tema sem valor */
        "#ereader1\nb\tabc\tdef\tghi\tjkl\tnome.pdf\n", /* numeros invalidos */
        "#ereader1\nb\t1\t2\t3\t4",                  /* sem quebra final */
    };

    for (size_t i = 0; i < sizeof(LIXO) / sizeof(LIXO[0]); ++i) {
        fs_write_file(TMP, LIXO[i], (int)strlen(LIXO[i]));
        Progress p;
        progress_load(&p, TMP);
        /* O contrato e "nao quebra e nao inventa", nao "recupera". */
        CHECK(p.count >= 0 && p.count <= PROG_MAX_BOOKS,
              "caso %d: contagem absurda %d", (int)i, p.count);
    }

    /* A ultima linha, sem quebra final, e valida e deve ser lida - arquivo
     * cortado pelo desligamento perde a ultima linha, nao todas. */
    static const char *OK_LINE = "#ereader1\nb\t5\t6\t7\t8\t9\tfim.pdf";
    fs_write_file(TMP, OK_LINE, (int)strlen(OK_LINE));
    Progress p;
    progress_load(&p, TMP);
    int unit = -1;
    CHECK(progress_get(&p, "fim.pdf", 5, &unit, NULL, NULL, NULL) == 0,
          "linha sem quebra final deveria ser lida");
    CHECK(unit == 6, "unit deveria ser 6, veio %d", unit);
}

/* Lista cheia: entra o novo, sai o mais antigo, e nada estoura. */
static void test_lista_cheia(void)
{
    printf("-- lista cheia --\n");
    remove(TMP);

    Progress p;
    progress_load(&p, TMP);

    char nome[64];
    for (int i = 0; i < PROG_MAX_BOOKS + 20; ++i) {
        snprintf(nome, sizeof(nome), "livro%03d.pdf", i);
        progress_set(&p, nome, (unsigned int)(1000 + i), i, 50, 0, 18);
    }
    CHECK(p.count == PROG_MAX_BOOKS, "esperava %d entradas, tem %d",
          PROG_MAX_BOOKS, p.count);

    /* O ultimo inserido tem de estar la. */
    snprintf(nome, sizeof(nome), "livro%03d.pdf", PROG_MAX_BOOKS + 19);
    int unit = -1;
    CHECK(progress_get(&p, nome, (unsigned int)(1000 + PROG_MAX_BOOKS + 19),
                       &unit, NULL, NULL, NULL) == 0,
          "o livro mais recente deveria estar na lista");

    /* O primeiro de todos ja saiu. */
    CHECK(progress_get(&p, "livro000.pdf", 1000, &unit, NULL, NULL, NULL) != 0,
          "o livro mais antigo deveria ter saido");

    CHECK(progress_save(&p, TMP) == 0, "gravacao da lista cheia falhou");
    Progress q;
    progress_load(&q, TMP);
    CHECK(q.count == PROG_MAX_BOOKS, "releitura deu %d entradas", q.count);
}

/* Sem mudanca nao ha gravacao: o Memory Stick e lento e a escrita e cara. */
static void test_nao_grava_a_toa(void)
{
    printf("-- nao grava sem mudanca --\n");
    remove(TMP);

    Progress p;
    progress_load(&p, TMP);
    CHECK(progress_save(&p, TMP) == 0, "save limpo deveria dizer sucesso");

    char buf[16];
    CHECK(fs_read_file(TMP, buf, sizeof(buf)) == 0,
          "nao deveria ter criado arquivo nenhum");

    progress_set_theme(&p, 1);
    CHECK(p.dirty == 1, "trocar o tema deveria sujar");
    progress_set_theme(&p, 1);
    CHECK(progress_save(&p, TMP) == 0, "gravacao falhou");
    CHECK(p.dirty == 0, "depois de gravar nao deveria estar suja");
}

int main(void)
{
    test_ida_e_volta();
    test_chave_inclui_tamanho();
    test_nome_hostil();
    test_corrompido();
    test_lista_cheia();
    test_nao_grava_a_toa();

    remove(TMP);
    printf("\n%d ok, %d falhas\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
