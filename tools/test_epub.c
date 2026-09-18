/*
 * Teste de host para src/read/zip.c e src/read/epub.c.
 * Roda via  .\test.ps1 epub
 *
 * CONSTROI o EPUB que testa, com deflate de verdade, em vez de depender de um
 * arquivo de corpus. Dois motivos:
 *
 *   1. EPUB nao e redistribuivel, entao um corpus versionado nao existe e o
 *      teste nao rodaria em outra maquina.
 *   2. Os casos que interessam sao estruturais - OPF em subpasta, href com
 *      "../", capitulo grande cortado em pedacos, membro armazenado vs
 *      comprimido - e produzir cada um sob encomenda e a unica forma de saber
 *      que foi ELE que passou.
 *
 * Para conferir contra um EPUB real:  .\test.ps1 epubdump "livro.epub" 0 3
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "epub.h"
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

static const char *TMP = "/tmp/ereader_test.epub";

/* ------------------------------------------------------------------------- */
/* Escritor de ZIP minimo                                                     */

typedef struct {
    char           name[128];
    unsigned char *cdata;
    int            csize, usize;
    int            method;
    unsigned int   crc;
    long           lho;
} Member;

static Member g_mem[16];
static int    g_nmem;

static void put16(FILE *f, unsigned int v)
{
    fputc((int)(v & 0xFF), f);
    fputc((int)((v >> 8) & 0xFF), f);
}

static void put32(FILE *f, unsigned int v)
{
    put16(f, v & 0xFFFF);
    put16(f, (v >> 16) & 0xFFFF);
}

/* `store` forca metodo 0, que e o que a especificacao de EPUB exige do
 * mimetype - e um caminho de codigo diferente no leitor. */
static void add_member(const char *name, const char *data, int store)
{
    Member *m = &g_mem[g_nmem++];
    snprintf(m->name, sizeof(m->name), "%s", name);
    m->usize = (int)strlen(data);
    m->crc = (unsigned int)crc32(0, (const Bytef *)data, (uInt)m->usize);

    if (store) {
        m->method = 0;
        m->csize = m->usize;
        m->cdata = (unsigned char *)malloc((size_t)m->usize + 1);
        memcpy(m->cdata, data, (size_t)m->usize);
        return;
    }

    m->method = 8;
    uLongf bound = compressBound((uLong)m->usize) + 64;
    unsigned char *tmp = (unsigned char *)malloc(bound);

    /* Deflate CRU (windowBits -15): dentro de um ZIP nao ha cabecalho zlib. */
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8,
                 Z_DEFAULT_STRATEGY);
    zs.next_in = (Bytef *)(void *)data;
    zs.avail_in = (uInt)m->usize;
    zs.next_out = tmp;
    zs.avail_out = (uInt)bound;
    deflate(&zs, Z_FINISH);
    m->csize = (int)(bound - zs.avail_out);
    deflateEnd(&zs);

    m->cdata = tmp;
}

static void write_zip(void)
{
    FILE *f = fopen(TMP, "wb");
    if (!f) { printf("  nao criou %s\n", TMP); exit(2); }

    for (int i = 0; i < g_nmem; ++i) {
        Member *m = &g_mem[i];
        m->lho = ftell(f);
        put32(f, 0x04034B50u);
        put16(f, 20); put16(f, 0);
        put16(f, (unsigned int)m->method);
        put16(f, 0); put16(f, 0);
        put32(f, m->crc);
        put32(f, (unsigned int)m->csize);
        put32(f, (unsigned int)m->usize);
        put16(f, (unsigned int)strlen(m->name));
        put16(f, 0);
        fwrite(m->name, 1, strlen(m->name), f);
        fwrite(m->cdata, 1, (size_t)m->csize, f);
    }

    long cdoff = ftell(f);
    for (int i = 0; i < g_nmem; ++i) {
        Member *m = &g_mem[i];
        put32(f, 0x02014B50u);
        put16(f, 20); put16(f, 20); put16(f, 0);
        put16(f, (unsigned int)m->method);
        put16(f, 0); put16(f, 0);
        put32(f, m->crc);
        put32(f, (unsigned int)m->csize);
        put32(f, (unsigned int)m->usize);
        put16(f, (unsigned int)strlen(m->name));
        put16(f, 0); put16(f, 0); put16(f, 0); put16(f, 0);
        put32(f, 0);
        put32(f, (unsigned int)m->lho);
        fwrite(m->name, 1, strlen(m->name), f);
    }
    long cdend = ftell(f);

    put32(f, 0x06054B50u);
    put16(f, 0); put16(f, 0);
    put16(f, (unsigned int)g_nmem);
    put16(f, (unsigned int)g_nmem);
    put32(f, (unsigned int)(cdend - cdoff));
    put32(f, (unsigned int)cdoff);
    put16(f, 0);
    fclose(f);
}

static void reset_members(void)
{
    for (int i = 0; i < g_nmem; ++i)
        free(g_mem[i].cdata);
    g_nmem = 0;
}

/* ------------------------------------------------------------------------- */

static const char *CONTAINER =
    "<?xml version=\"1.0\"?>\n"
    "<container version=\"1.0\" "
    "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
    "  <rootfiles>\n"
    "    <rootfile full-path=\"OEBPS/content.opf\" "
    "media-type=\"application/oebps-package+xml\"/>\n"
    "  </rootfiles>\n"
    "</container>\n";

/*
 * OPF com dois detalhes que quebram leitor ingenuo: o href do capitulo 2 sobe
 * um nivel com "../", e a ordem da espinha NAO e a ordem do manifesto - que e
 * exatamente o motivo de a espinha existir.
 */
static const char *OPF =
    "<?xml version=\"1.0\"?>\n"
    "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\">\n"
    "  <metadata>\n"
    "    <dc:title>O Livro de Teste</dc:title>\n"
    "    <dc:creator>Autora Fulana</dc:creator>\n"
    "  </metadata>\n"
    "  <manifest>\n"
    "    <item id=\"c2\" href=\"../OEBPS/Text/cap2.xhtml\" "
    "media-type=\"application/xhtml+xml\"/>\n"
    "    <item id=\"c1\" href=\"Text/cap1.xhtml\" "
    "media-type=\"application/xhtml+xml\"/>\n"
    "    <item id=\"css\" href=\"Styles/x.css\" media-type=\"text/css\"/>\n"
    "  </manifest>\n"
    "  <spine>\n"
    "    <itemref idref=\"c1\"/>\n"
    "    <itemref idref=\"c2\"/>\n"
    "  </spine>\n"
    "</package>\n";

static const char *CAP1 =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<html xmlns=\"http://www.w3.org/1999/xhtml\">\n"
    "<head>\n"
    "  <title>Capitulo 1</title>\n"
    "  <style type=\"text/css\">p { margin: 0 } .x { color: red }</style>\n"
    "  <script>var x = 1; if (x < 2) { alert('nao sou texto'); }</script>\n"
    "</head>\n"
    "<body>\n"
    "  <h1>O Primeiro Capitulo</h1>\n"
    "  <p>Este paragrafo tem <em>enfase</em> e um <a href=\"x\">link</a>\n"
    "     dentro dele, e quebra de linha no meio do arquivo.</p>\n"
    "  <p>Segundo paragrafo, com entidades: &amp; &lt; &gt; &mdash; &#233; "
    "&#x2014; &ccedil;.</p>\n"
    "  <p>Terceiro&nbsp;paragrafo com espaco inquebravel.</p>\n"
    "</body>\n"
    "</html>\n";

static const char *CAP2 =
    "<html><body>\n"
    "<p>O capitulo dois existe para provar que a ordem da espinha vale.</p>\n"
    "</body></html>\n";

static void build_basic_epub(void)
{
    reset_members();
    add_member("mimetype", "application/epub+zip", 1);   /* armazenado */
    add_member("META-INF/container.xml", CONTAINER, 0);
    add_member("OEBPS/content.opf", OPF, 0);
    add_member("OEBPS/Text/cap1.xhtml", CAP1, 0);
    add_member("OEBPS/Text/cap2.xhtml", CAP2, 0);
    add_member("OEBPS/Styles/x.css", "p { margin: 0 }", 0);
    write_zip();
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

static void test_estrutura(void)
{
    printf("-- container, OPF, espinha --\n");
    build_basic_epub();

    PdfArena da;
    pdf_arena_init(&da, 65536);
    PdfIo io;
    CHECK(host_io_open(&io, TMP) == 0, "nao abriu");

    EpubDoc d;
    CHECK(epub_open(&d, &da, &io) == 0, "epub_open falhou: %s", d.err);
    CHECK(d.nspine == 2, "esperava 2 capitulos, achou %d", d.nspine);
    CHECK(strcmp(d.title, "O Livro de Teste") == 0,
          "titulo saiu \"%s\"", d.title);
    CHECK(strcmp(d.author, "Autora Fulana") == 0, "autor saiu \"%s\"", d.author);

    /* O mimetype e armazenado, nao comprimido: o leitor tem de aceitar os dois
     * metodos, e este e o unico membro que exercita o metodo 0. */
    int mi = zip_find(&d.zip, "mimetype");
    CHECK(mi >= 0, "mimetype nao encontrado");
    CHECK(mi >= 0 && d.zip.entries[mi].method == 0,
          "mimetype deveria estar armazenado");

    epub_close(&d);
    pdf_arena_free(&da);
}

static void test_texto(void)
{
    printf("-- XHTML -> paragrafos --\n");
    build_basic_epub();

    PdfArena da, pa;
    pdf_arena_init(&da, 65536);
    pdf_arena_init(&pa, 65536);
    PdfIo io;
    host_io_open(&io, TMP);

    EpubDoc d;
    CHECK(epub_open(&d, &da, &io) == 0, "epub_open falhou: %s", d.err);

    Reflow rf;
    CHECK(epub_unit(&d, &pa, 0, &rf) == 0, "epub_unit falhou");
    CHECK(rf.nparas == 4, "esperava 4 paragrafos, saiu %d", rf.nparas);

    /* O titulo vira paragrafo com a marca, e nao texto solto. */
    expect_para(&rf, 0, "O Primeiro Capitulo");
    CHECK(rf.nparas > 0 && (rf.paras[0].flags & RF_HEADING),
          "o <h1> deveria virar RF_HEADING");

    /* Tag inline some sem separar palavra; quebra de linha do arquivo vira um
     * espaco so. */
    expect_para(&rf, 1, "Este paragrafo tem enfase e um link dentro dele, e "
                        "quebra de linha no meio do arquivo.");

    /* Entidades nomeadas, decimais e hexadecimais. */
    expect_para(&rf, 2, "Segundo paragrafo, com entidades: & < > \xE2\x80\x94 "
                        "\xC3\xA9 \xE2\x80\x94 \xC3\xA7.");

    /* &nbsp; vira espaco comum: mantido como U+00A0 a quebra de linha nao o
     * reconheceria e a palavra seguinte sairia colada. */
    expect_para(&rf, 3, "Terceiro paragrafo com espaco inquebravel.");

    /* <style> e <script> nao sao texto de leitura. */
    int leaked = 0;
    for (int k = 0; k + 6 <= rf.buflen; ++k) {
        if (memcmp(rf.buf + k, "margin", 6) == 0) leaked = 1;
        if (memcmp(rf.buf + k, "alert(", 6) == 0) leaked = 1;
    }
    CHECK(!leaked, "conteudo de <style> ou <script> vazou para o texto");

    epub_close(&d);
    pdf_arena_free(&pa);
    pdf_arena_free(&da);
}

/* A ordem de leitura e a da ESPINHA, nao a do manifesto nem a do ZIP. */
static void test_ordem_da_espinha(void)
{
    printf("-- a ordem e a da espinha --\n");
    build_basic_epub();

    PdfArena da, pa;
    pdf_arena_init(&da, 65536);
    pdf_arena_init(&pa, 65536);
    PdfIo io;
    host_io_open(&io, TMP);

    EpubDoc d;
    epub_open(&d, &da, &io);

    Reflow rf;
    epub_unit(&d, &pa, d.nunits - 1, &rf);
    expect_para(&rf, 0, "O capitulo dois existe para provar que a ordem da "
                        "espinha vale.");

    epub_close(&d);
    pdf_arena_free(&pa);
    pdf_arena_free(&da);
}

/*
 * Capitulo grande cortado em pedacos.
 *
 * Este e o caso que faz um EPUB de livro-inteiro-num-arquivo caber num console
 * de 16 MB. O que se exige aqui e mais forte que "funciona": os pedacos tem de
 * se ENCAIXAR, sem repetir nem perder paragrafo, porque o progresso de leitura
 * guarda um indice de unidade e um indice instavel leva de volta ao lugar
 * errado.
 */
static void test_capitulo_grande(void)
{
    printf("-- capitulo grande em varios pedacos --\n");

    int nparas = 900;
    size_t cap = (size_t)nparas * 140 + 256;
    char *big = (char *)malloc(cap);
    int n = sprintf(big, "<html><body>\n");
    for (int i = 0; i < nparas; ++i)
        n += sprintf(big + n,
                     "<p>Paragrafo %d com texto suficiente para que o capitulo "
                     "passe de varios pedacos de corte.</p>\n", i);
    n += sprintf(big + n, "</body></html>\n");

    reset_members();
    add_member("mimetype", "application/epub+zip", 1);
    add_member("META-INF/container.xml", CONTAINER, 0);
    add_member("OEBPS/content.opf",
               "<package><manifest>"
               "<item id=\"c1\" href=\"Text/cap1.xhtml\"/>"
               "</manifest><spine><itemref idref=\"c1\"/></spine></package>", 0);
    add_member("OEBPS/Text/cap1.xhtml", big, 0);
    write_zip();
    free(big);

    PdfArena da, pa;
    pdf_arena_init(&da, 65536);
    pdf_arena_init(&pa, 256 * 1024);
    PdfIo io;
    host_io_open(&io, TMP);

    EpubDoc d;
    CHECK(epub_open(&d, &da, &io) == 0, "epub_open falhou: %s", d.err);
    CHECK(d.nspine == 1, "esperava 1 capitulo, achou %d", d.nspine);
    CHECK(d.nunits > 1, "capitulo grande deveria virar varias unidades, virou %d",
          d.nunits);

    /*
     * Percorre todas as unidades e confere que os paragrafos saem na ordem
     * 0,1,2,... sem buraco e sem repeticao. E o teste do encaixe.
     */
    int expected = 0;
    int gaps = 0, dups = 0, total = 0;

    for (int u = 0; u < d.nunits; ++u) {
        pdf_arena_reset(&pa);
        Reflow rf;
        if (epub_unit(&d, &pa, u, &rf) != 0) {
            g_fail++;
            printf("  FALHA unidade %d nao carregou\n", u);
            break;
        }
        for (int i = 0; i < rf.nparas; ++i) {
            int got = -1;
            /* "Paragrafo N com..." - le o N de volta. */
            if (rf.paras[i].len > 11 &&
                memcmp(rf.buf + rf.paras[i].off, "Paragrafo ", 10) == 0)
                got = atoi(rf.buf + rf.paras[i].off + 10);
            if (got < 0)
                continue;
            total++;
            if (got < expected) dups++;
            else if (got > expected) gaps++;
            expected = got + 1;
        }
    }

    CHECK(gaps == 0, "%d paragrafos foram PERDIDOS entre pedacos", gaps);
    CHECK(dups == 0, "%d paragrafos foram REPETIDOS entre pedacos", dups);
    CHECK(total == nparas, "esperava %d paragrafos no total, contei %d",
          nparas, total);

    epub_close(&d);
    pdf_arena_free(&pa);
    pdf_arena_free(&da);
}

/*
 * OPF com prefixo de namespace em toda tag.
 *
 * EPUB 2 gerado por ferramenta escreve <opf:manifest>, <opf:item>,
 * <opf:itemref>. Comparar o nome cru faria a espinha nao ser encontrada e o
 * livro abriria VAZIO - o pior tipo de defeito, porque nao ha nada na tela
 * apontando para um prefixo de tres letras como causa.
 */
static void test_namespace(void)
{
    printf("-- OPF com prefixo de namespace --\n");

    reset_members();
    add_member("mimetype", "application/epub+zip", 1);
    add_member("META-INF/container.xml", CONTAINER, 0);
    add_member("OEBPS/content.opf",
               "<?xml version=\"1.0\"?>\n"
               "<opf:package xmlns:opf=\"http://www.idpf.org/2007/opf\">\n"
               " <opf:metadata>\n"
               "  <dc:title>Livro Com Prefixo</dc:title>\n"
               "  <dc:creator>Autor Prefixado</dc:creator>\n"
               " </opf:metadata>\n"
               " <opf:manifest>\n"
               "  <opf:item id=\"c1\" href=\"Text/cap1.xhtml\"/>\n"
               " </opf:manifest>\n"
               " <opf:spine>\n"
               "  <opf:itemref idref=\"c1\"/>\n"
               " </opf:spine>\n"
               "</opf:package>\n", 0);
    add_member("OEBPS/Text/cap1.xhtml",
               "<html:html><html:body>\n"
               "<html:p>Tag de paragrafo tambem com prefixo.</html:p>\n"
               "</html:body></html:html>\n", 0);
    write_zip();

    PdfArena da, pa;
    pdf_arena_init(&da, 65536);
    pdf_arena_init(&pa, 65536);
    PdfIo io;
    host_io_open(&io, TMP);

    EpubDoc d;
    CHECK(epub_open(&d, &da, &io) == 0, "epub_open falhou: %s", d.err);
    CHECK(d.nspine == 1, "esperava 1 capitulo, achou %d", d.nspine);
    CHECK(strcmp(d.title, "Livro Com Prefixo") == 0,
          "titulo saiu \"%s\"", d.title);
    CHECK(strcmp(d.author, "Autor Prefixado") == 0,
          "autor saiu \"%s\"", d.author);

    Reflow rf;
    CHECK(epub_unit(&d, &pa, 0, &rf) == 0, "epub_unit falhou");
    expect_para(&rf, 0, "Tag de paragrafo tambem com prefixo.");

    epub_close(&d);
    pdf_arena_free(&pa);
    pdf_arena_free(&da);
}

/* Arquivo que nao e ZIP, e ZIP que nao e EPUB: recusa com mensagem, sem
 * quebrar. */
static void test_recusas(void)
{
    printf("-- arquivos invalidos --\n");
    {
        FILE *f = fopen(TMP, "wb");
        fputs("isto nao e um zip, nem de longe", f);
        fclose(f);

        PdfArena da;
        pdf_arena_init(&da, 8192);
        PdfIo io;
        host_io_open(&io, TMP);
        Doc doc;
        CHECK(doc_open(&doc, &da, &io, DOC_EPUB) != 0,
              "arquivo que nao e ZIP deveria ser recusado");
        CHECK(doc.err[0] != '\0', "a recusa deveria trazer mensagem");
        doc_close(&doc);
        pdf_arena_free(&da);
    }
    {
        /* ZIP valido, mas sem container.xml: e um ZIP, nao um EPUB. */
        reset_members();
        add_member("leiame.txt", "so um zip comum", 0);
        write_zip();

        PdfArena da;
        pdf_arena_init(&da, 8192);
        PdfIo io;
        host_io_open(&io, TMP);
        Doc doc;
        CHECK(doc_open(&doc, &da, &io, DOC_EPUB) != 0,
              "ZIP sem container.xml deveria ser recusado");
        CHECK(strstr(doc.err, "EPUB") != NULL,
              "a mensagem deveria explicar que nao e um EPUB, disse \"%s\"",
              doc.err);
        doc_close(&doc);
        pdf_arena_free(&da);
    }
}

int main(void)
{
    test_estrutura();
    test_texto();
    test_ordem_da_espinha();
    test_capitulo_grande();
    test_namespace();
    test_recusas();

    reset_members();
    remove(TMP);
    printf("\n%d ok, %d falhas\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
