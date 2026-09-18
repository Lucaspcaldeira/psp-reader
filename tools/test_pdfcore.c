/*
 * Testes de unidade da camada PDF, sem tocar em arquivo nenhum.
 * Roda via  .\test.ps1 pdfcore
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <zlib.h>

#include "pdf_arena.h"
#include "pdf_io.h"
#include "pdf_obj.h"
#include "pdf_lex.h"
#include "pdf_filt.h"
#include "pdf_doc.h"
#include "pdf_font.h"
#include "pdf_text.h"

static int g_fail = 0, g_pass = 0;

#define CHECK(cond, fmt, ...)                                                  \
    do {                                                                       \
        if (cond) { g_pass++; }                                                \
        else { g_fail++; printf("  FALHA %s:%d  " fmt "\n",                     \
                                __FILE__, __LINE__, ##__VA_ARGS__); }          \
    } while (0)

/* --- utilitarios ---------------------------------------------------------- */

/* Monta um PdfStream sobre um literal, para exercitar o lexer sem arquivo. */
typedef struct {
    PdfMemCtx mc;
    PdfIo     io;
    PdfStream st;
    PdfArena  a;
} Fix;

static void fix_open(Fix *f, const char *src, int len)
{
    if (len < 0)
        len = (int)strlen(src);
    pdf_arena_init(&f->a, 4096);
    pdf_io_mem(&f->io, &f->mc, (const unsigned char *)src, len);
    pdf_stream_init(&f->st, &f->io);
}

static void fix_close(Fix *f) { pdf_arena_free(&f->a); }

/* --- arena ---------------------------------------------------------------- */

static void test_arena(void)
{
    printf("-- arena --\n");
    PdfArena a;
    pdf_arena_init(&a, 128);

    /* Alinhamento de 8 e obrigatorio: no MIPS do PSP um acesso desalinhado a
     * long long e excecao de bus, nao apenas lentidao. */
    int misaligned = 0;
    for (int i = 1; i <= 64; ++i) {
        void *p = pdf_arena_alloc(&a, (size_t)i);
        if (!p || ((unsigned long)(size_t)p & 7u) != 0)
            misaligned++;
    }
    CHECK(misaligned == 0, "%d alocacoes desalinhadas em 8 bytes", misaligned);

    /* Pedido maior que o bloco ganha bloco proprio: nao existe teto. */
    void *big = pdf_arena_alloc(&a, 100000);
    CHECK(big != NULL, "alocacao de 100 KB com bloco de 128 falhou");
    CHECK(a.oom == 0, "oom marcado sem motivo");

    char *d = pdf_arena_dup(&a, "abc\0def", 7);
    CHECK(d && d[3] == '\0' && d[6] == 'f' && d[7] == '\0',
          "dup nao preservou nulo interno nem terminou");

    /* reset preserva capacidade: e o que faz a virada de pagina nao chamar o
     * alocador do sistema. */
    size_t reserved_before = a.reserved;
    pdf_arena_reset(&a);
    CHECK(a.handed == 0, "handed deveria zerar apos reset, veio %lu",
          (unsigned long)a.handed);
    void *after = pdf_arena_alloc(&a, 64);
    CHECK(after != NULL, "alocacao apos reset falhou");
    CHECK(a.reserved == reserved_before,
          "reset devolveu memoria ao sistema (%lu -> %lu); deveria reciclar",
          (unsigned long)reserved_before, (unsigned long)a.reserved);

    pdf_arena_free(&a);
}

/* --- lexer ---------------------------------------------------------------- */

static void expect_tok(const char *src, PdfTokKind kind, const char *sval)
{
    Fix f;
    fix_open(&f, src, -1);
    PdfTok t;
    pdf_lex_next(&f.st, &f.a, &t);

    CHECK(t.kind == kind, "%s: kind %d, esperado %d", src, t.kind, kind);
    if (sval && t.s.p) {
        int n = (int)strlen(sval);
        CHECK(t.s.len == n && memcmp(t.s.p, sval, (size_t)n) == 0,
              "%s: valor \"%.*s\" (%d bytes), esperado \"%s\"",
              src, t.s.len, t.s.p, t.s.len, sval);
    }
    fix_close(&f);
}

static void expect_str(const char *label, const char *src,
                       const char *want, int wantlen)
{
    Fix f;
    fix_open(&f, src, -1);
    PdfTok t;
    pdf_lex_next(&f.st, &f.a, &t);

    CHECK(t.kind == PT_STR, "%s: kind %d, esperado PT_STR", label, t.kind);
    CHECK(t.s.len == wantlen, "%s: %d bytes, esperado %d", label, t.s.len, wantlen);
    if (t.s.len == wantlen && t.s.p)
        CHECK(memcmp(t.s.p, want, (size_t)wantlen) == 0,
              "%s: bytes diferentes", label);
    fix_close(&f);
}

static void test_lexer(void)
{
    printf("-- lexer: tokens basicos --\n");
    expect_tok("/Type",      PT_NAME,    "Type");
    expect_tok("/A#20B",     PT_NAME,    "A B");        /* #20 = espaco */
    expect_tok("/",          PT_NAME,    "");           /* nome vazio e legal */
    expect_tok("<<",         PT_DICT_OPEN, NULL);
    expect_tok(">>",         PT_DICT_CLOSE, NULL);
    expect_tok("[",          PT_ARR_OPEN, NULL);
    expect_tok("obj",        PT_KEYWORD, "obj");
    expect_tok("  % comentario\n  /X", PT_NAME, "X");

    printf("-- lexer: numeros tolerantes --\n");
    {
        /* PDF real tem numeros malformados. Um lexer estrito rejeitaria o
         * arquivo inteiro por um numero decorativo que ninguem le. */
        struct { const char *src; PdfTokKind k; long long i; } cases[] = {
            { "0",     PT_INT,  0 },
            { "42",    PT_INT,  42 },
            { "-17",   PT_INT,  -17 },
            { "+8",    PT_INT,  8 },
            /* Sinal extra e consumido mas descartado. "--5" passado cru ao
             * strtoll daria 0 - plausivel e silenciosamente errado, o pior
             * resultado possivel num /MediaBox. */
            { "--5",   PT_INT,  -5 },
            { "34-",   PT_INT,  34 },
            { ".",     PT_INT,  0 },    /* so ponto: nao pode virar erro fatal */
            { "-",     PT_INT,  0 },
        };
        for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
            Fix f;
            fix_open(&f, cases[i].src, -1);
            PdfTok t;
            pdf_lex_next(&f.st, &f.a, &t);
            CHECK(t.kind == cases[i].k, "\"%s\": kind %d, esperado %d",
                  cases[i].src, t.kind, cases[i].k);
            if (t.kind == PT_INT)
                CHECK(t.i == cases[i].i, "\"%s\": %lld, esperado %lld",
                      cases[i].src, t.i, cases[i].i);
            fix_close(&f);
        }
    }
    {
        Fix f;
        fix_open(&f, "3.14", -1);
        PdfTok t;
        pdf_lex_next(&f.st, &f.a, &t);
        CHECK(t.kind == PT_REAL, "3.14 deveria ser PT_REAL");
        CHECK(t.r > 3.13 && t.r < 3.15, "3.14 veio %f", t.r);
        fix_close(&f);
    }

    printf("-- lexer: strings literais --\n");
    expect_str("simples",      "(abc)",            "abc", 3);
    expect_str("parens balanc","(a(b)c)",           "a(b)c", 5);
    expect_str("escape paren", "(a\\)b)",           "a)b", 3);
    expect_str("escape n",     "(a\\nb)",           "a\nb", 3);
    expect_str("octal",        "(\\101\\102)",      "AB", 2);
    expect_str("octal curto",  "(\\0\\1)",          "\0\1", 2);
    /* Continuacao de linha: a barra e a quebra desaparecem. */
    expect_str("continuacao",  "(ab\\\ncd)",        "abcd", 4);
    /* Barra antes de caractere sem significado: a barra some, o byte fica. */
    expect_str("escape inutil","(a\\qb)",           "aqb", 3);
    /* String nao fechada: para no fim, sem travar. */
    expect_str("nao fechada",  "(abc",              "abc", 3);
    /* Byte nulo dentro da string e legitimo (texto UTF-16BE). */
    expect_str("nulo interno", "(a\\000b)",         "a\0b", 3);

    printf("-- lexer: strings hexadecimais --\n");
    expect_str("hex par",    "<414243>",   "ABC", 3);
    /* Nibble impar: 41 42 4 -> 'A' 'B' 0x40, porque o ultimo completa com zero. */
    expect_str("hex impar",  "<41424>",    "AB\x40", 3);
    expect_str("hex c/ ws",  "<41 42\n43>", "ABC", 3);
    expect_str("hex vazio",  "<>",         "", 0);

    printf("-- lexer: INVARIANTE de progresso --\n");
    {
        /* Cada byte, isolado, precisa gerar um token e mover o ponteiro. Sem
         * isso, um byte inesperado num PDF malformado prende o parser num laco
         * infinito, e o sintoma no console e o console travado. */
        int stuck = 0;
        for (int i = 0; i < 256; ++i) {
            char buf[1] = { (char)i };
            Fix f;
            fix_open(&f, buf, 1);
            PdfTok t;
            long long before = pdf_tell(&f.st);
            pdf_lex_next(&f.st, &f.a, &t);
            if (pdf_tell(&f.st) == before && t.kind != PT_EOF)
                stuck++;
            fix_close(&f);
        }
        CHECK(stuck == 0, "%d bytes nao avancaram o lexer", stuck);
    }
}

/* --- parser --------------------------------------------------------------- */

static void test_parser(void)
{
    printf("-- parser: referencia indireta e rebobinagem --\n");
    {
        Fix f;
        fix_open(&f, "12 0 R", -1);
        PdfObj *o = pdf_parse(&f.st, &f.a);
        CHECK(pdf_is(o, PDF_REF), "\"12 0 R\" deveria ser PDF_REF, veio %d",
              o ? o->kind : -1);
        if (pdf_is(o, PDF_REF))
            CHECK(o->u.ref.num == 12 && o->u.ref.gen == 0,
                  "ref %d %d, esperado 12 0", o->u.ref.num, o->u.ref.gen);
        fix_close(&f);
    }
    {
        /* Dois inteiros SEM o R: o lookahead precisa rebobinar e devolver
         * apenas o primeiro inteiro, deixando o segundo para a proxima leitura.
         * Se rebobinar errado, todo array de numeros sai deslocado. */
        Fix f;
        fix_open(&f, "12 34", -1);
        PdfObj *a1 = pdf_parse(&f.st, &f.a);
        PdfObj *a2 = pdf_parse(&f.st, &f.a);
        CHECK(pdf_int(a1, -1) == 12, "primeiro veio %lld", pdf_int(a1, -1));
        CHECK(pdf_int(a2, -1) == 34, "segundo veio %lld", pdf_int(a2, -1));
        fix_close(&f);
    }
    {
        /* Tres inteiros: "1 2" nao e ref, mas "2 0 R" logo depois e. */
        Fix f;
        fix_open(&f, "[1 2 0 R 3]", -1);
        PdfObj *o = pdf_parse(&f.st, &f.a);
        CHECK(pdf_arr_len(o) == 3, "array deveria ter 3 itens, veio %d",
              pdf_arr_len(o));
        CHECK(pdf_int(pdf_arr_get(o, 0), -1) == 1, "item 0");
        CHECK(pdf_is(pdf_arr_get(o, 1), PDF_REF), "item 1 deveria ser ref");
        CHECK(pdf_int(pdf_arr_get(o, 2), -1) == 3, "item 2");
        fix_close(&f);
    }

    printf("-- parser: dicionarios --\n");
    {
        Fix f;
        fix_open(&f, "<< /Type /Page /MediaBox [0 0 612 792] /N 3 >>", -1);
        PdfObj *o = pdf_parse(&f.st, &f.a);
        CHECK(pdf_is(o, PDF_DICT), "deveria ser dict");
        CHECK(pdf_name_is(pdf_dict_get(o, "Type"), "Page"), "/Type errado");
        CHECK(pdf_int(pdf_dict_get(o, "N"), -1) == 3, "/N errado");
        PdfObj *mb = pdf_dict_get(o, "MediaBox");
        CHECK(pdf_arr_len(mb) == 4, "/MediaBox deveria ter 4 itens");
        CHECK(pdf_int(pdf_arr_get(mb, 2), 0) == 612, "/MediaBox[2]");
        fix_close(&f);
    }
    {
        /* Chave duplicada: a ultima vence, como fazem os leitores reais. */
        Fix f;
        fix_open(&f, "<< /A 1 /A 2 >>", -1);
        PdfObj *o = pdf_parse(&f.st, &f.a);
        CHECK(pdf_int(pdf_dict_get(o, "A"), -1) == 2,
              "chave duplicada: esperado 2 (a ultima), veio %lld",
              pdf_int(pdf_dict_get(o, "A"), -1));
        fix_close(&f);
    }
    {
        /* Array sem `]` fechando, seguido de endobj: nao pode engolir o resto
         * do documento. */
        Fix f;
        fix_open(&f, "[1 2 3 endobj", -1);
        PdfObj *o = pdf_parse(&f.st, &f.a);
        CHECK(pdf_arr_len(o) == 3, "array truncado deveria render 3, veio %d",
              pdf_arr_len(o));
        fix_close(&f);
    }
    {
        /* Aninhamento fundo nao pode estourar a pilha de 256 KB do PSP. */
        char deep[2048];
        int n = 0;
        for (int i = 0; i < 400; ++i) deep[n++] = '[';
        for (int i = 0; i < 400; ++i) deep[n++] = ']';
        deep[n] = 0;
        Fix f;
        fix_open(&f, deep, n);
        PdfObj *o = pdf_parse(&f.st, &f.a);
        CHECK(o != NULL, "aninhamento fundo devolveu NULL");
        fix_close(&f);
    }

    printf("-- parser: stream com /Length correto e mentiroso --\n");
    {
        const char *src =
            "<< /Length 5 >>\nstream\nHELLO\nendstream";
        Fix f;
        fix_open(&f, src, -1);
        PdfObj *o = pdf_parse(&f.st, &f.a);
        CHECK(pdf_is(o, PDF_STREAM), "deveria ser PDF_STREAM, veio %d",
              o ? o->kind : -1);
        if (pdf_is(o, PDF_STREAM))
            CHECK(o->u.stm.len == 5, "len %lld, esperado 5", o->u.stm.len);
        fix_close(&f);
    }
    {
        /*
         * /Length mentindo (diz 99, o real e 5). O parser confirma contra
         * `endstream` e corrige. Confiar no /Length aqui produziria stream
         * truncado ou invadindo o objeto seguinte - e /Length errado e um dos
         * defeitos mais comuns em PDF gerado por ferramenta ruim.
         */
        const char *src =
            "<< /Length 99 >>\nstream\nHELLO\nendstream";
        Fix f;
        fix_open(&f, src, -1);
        PdfObj *o = pdf_parse(&f.st, &f.a);
        CHECK(pdf_is(o, PDF_STREAM), "deveria ser PDF_STREAM");
        if (pdf_is(o, PDF_STREAM))
            CHECK(o->u.stm.len == 5,
                  "/Length mentiroso: len %lld, esperado 5 (via endstream)",
                  o->u.stm.len);
        fix_close(&f);
    }
    {
        /* /Length indireto: fica -1 aqui, porque resolver exige o xref. */
        const char *src = "<< /Length 7 0 R >>\nstream\nABC\nendstream";
        Fix f;
        fix_open(&f, src, -1);
        PdfObj *o = pdf_parse(&f.st, &f.a);
        CHECK(pdf_is(o, PDF_STREAM), "deveria ser PDF_STREAM");
        if (pdf_is(o, PDF_STREAM))
            CHECK(o->u.stm.len == 3,
                  "/Length indireto: esperado 3 via endstream, veio %lld",
                  o->u.stm.len);
        fix_close(&f);
    }
}

/* --- filtros -------------------------------------------------------------- */

/* Constroi um PdfObj PDF_NAME na arena, para passar como /Filter. */
static PdfObj *mkname(PdfArena *a, const char *s)
{
    PdfObj *o = pdf_new(a, PDF_NAME);
    o->u.s.p = pdf_arena_dup(a, s, strlen(s));
    o->u.s.len = (int)strlen(s);
    return o;
}

static void check_filter(const char *label, const char *fname,
                         const unsigned char *src, int srclen,
                         const unsigned char *want, int wantlen)
{
    PdfArena a;
    pdf_arena_init(&a, 8192);

    unsigned char *out = NULL;
    int outlen = 0;
    PdfFiltStatus st = pdf_filt_apply(&a, mkname(&a, fname), NULL,
                                      src, srclen, &out, &outlen);

    CHECK(st == PDF_FILT_OK, "%s: status %s", label, pdf_filt_status_str(st));
    if (st == PDF_FILT_OK) {
        CHECK(outlen == wantlen, "%s: %d bytes, esperado %d",
              label, outlen, wantlen);
        if (outlen == wantlen && out)
            CHECK(memcmp(out, want, (size_t)wantlen) == 0,
                  "%s: conteudo diferente", label);
    }
    pdf_arena_free(&a);
}

static void test_filters(void)
{
    printf("-- filtros: ASCIIHex --\n");
    check_filter("AHx", "ASCIIHexDecode",
                 (const unsigned char *)"48656C6C6F>", 11,
                 (const unsigned char *)"Hello", 5);
    check_filter("AHx com ws", "ASCIIHexDecode",
                 (const unsigned char *)"48 65\n6C 6C 6F >", 16,
                 (const unsigned char *)"Hello", 5);
    /* Nibble impar completa com zero. */
    check_filter("AHx impar", "ASCIIHexDecode",
                 (const unsigned char *)"4>", 2,
                 (const unsigned char *)"\x40", 1);

    printf("-- filtros: ASCII85 --\n");
    /* Conferido a mao, grupo de 4 bytes por grupo de 4 bytes:
     *   "Hell" -> 87cUR    "o wo" -> D]j7B    "rld" -> Ebo7 (n+1 digitos) */
    check_filter("A85", "ASCII85Decode",
                 (const unsigned char *)"87cURD]j7BEbo7~>", 16,
                 (const unsigned char *)"Hello world", 11);
    /* 'z' e o atalho para quatro bytes nulos. */
    check_filter("A85 z", "ASCII85Decode",
                 (const unsigned char *)"z~>", 3,
                 (const unsigned char *)"\0\0\0\0", 4);

    printf("-- filtros: RunLength --\n");
    {
        /* 2 -> 3 literais "ABC"; 254 -> proximo byte x3; 128 -> EOD */
        unsigned char src[] = { 2, 'A', 'B', 'C', 254, 'Z', 128 };
        check_filter("RL", "RunLengthDecode", src, sizeof(src),
                     (const unsigned char *)"ABCZZZ", 6);
    }

    printf("-- filtros: Flate (roundtrip com zlib) --\n");
    {
        /* Texto com repeticao, para o deflate ter o que comprimir. */
        char plain[4096];
        for (int i = 0; i < (int)sizeof(plain); ++i)
            plain[i] = (char)('A' + (i % 26));

        unsigned long clen = compressBound(sizeof(plain));
        unsigned char *comp = (unsigned char *)malloc(clen);
        int rc = compress(comp, &clen, (const Bytef *)plain, sizeof(plain));
        CHECK(rc == Z_OK, "compress falhou: %d", rc);

        check_filter("Flate", "FlateDecode", comp, (int)clen,
                     (const unsigned char *)plain, (int)sizeof(plain));
        free(comp);
    }
    {
        /* Espaco em branco antes do cabecalho zlib: alguns geradores fazem
         * isso, e o inflate rejeitaria sem o skip. */
        const char *plain = "abcabcabcabcabcabc";
        unsigned long clen = compressBound(18);
        unsigned char *comp = (unsigned char *)malloc(clen + 4);
        compress(comp + 2, &clen, (const Bytef *)plain, 18);
        comp[0] = '\r';
        comp[1] = '\n';
        check_filter("Flate c/ ws antes", "FlateDecode", comp, (int)clen + 2,
                     (const unsigned char *)plain, 18);
        free(comp);
    }
    {
        /* Lixo puro: precisa devolver erro, nao travar nem estourar buffer. */
        PdfArena a;
        pdf_arena_init(&a, 4096);
        unsigned char junk[64];
        for (int i = 0; i < 64; ++i)
            junk[i] = (unsigned char)(i * 7 + 3);
        unsigned char *out = NULL;
        int outlen = 0;
        PdfFiltStatus st = pdf_filt_apply(&a, mkname(&a, "FlateDecode"), NULL,
                                          junk, sizeof(junk), &out, &outlen);
        CHECK(st == PDF_FILT_ERR_DATA, "lixo deveria dar ERR_DATA, veio %s",
              pdf_filt_status_str(st));
        pdf_arena_free(&a);
    }

    printf("-- filtros: LZW (vetor da especificacao) --\n");
    {
        /*
         * Exemplo da especificacao do PDF: a cadeia "-----A---B" codifica para
         * estes 9 bytes. Os valores 45, 65 e 66 da especificacao sao DECIMAIS
         * ('-', 'A', 'B'), nao hexadecimais.
         *
         * Os codigos no fluxo sao 256(CLEAR) 45 258 258 65 259 66 257(EOD).
         *
         * Duas coisas sao exercitadas aqui: o empacotamento de codigos de 9
         * bits com MSB primeiro, e o caso KwKwK - os dois 258 seguidos, onde o
         * segundo referencia a entrada que o primeiro acabou de criar. Errar o
         * KwKwK nao produz lixo evidente: produz texto quase certo com letras
         * repetidas faltando, que e bem mais dificil de notar.
         *
         * NAO cobre /EarlyChange: aqui a tabela vai no maximo a 262, longe do
         * salto de largura em 511.
         */
        unsigned char enc[] = { 0x80, 0x0B, 0x60, 0x50, 0x22,
                                0x0C, 0x0C, 0x85, 0x01 };
        unsigned char want[] = { 45, 45, 45, 45, 45, 65, 45, 45, 45, 66 };
        check_filter("LZW", "LZWDecode", enc, sizeof(enc),
                     want, sizeof(want));
    }

    printf("-- filtros: imagem e desconhecido --\n");
    {
        PdfArena a;
        pdf_arena_init(&a, 2048);
        unsigned char dummy[4] = { 1, 2, 3, 4 };
        unsigned char *out = NULL;
        int outlen = 0;

        /* Filtro de imagem: reconhecer e o que permite dizer "este PDF nao tem
         * camada de texto" em vez de mostrar lixo na tela. */
        PdfFiltStatus st = pdf_filt_apply(&a, mkname(&a, "DCTDecode"), NULL,
                                          dummy, 4, &out, &outlen);
        CHECK(st == PDF_FILT_ERR_IMAGE, "DCTDecode deveria dar ERR_IMAGE");

        st = pdf_filt_apply(&a, mkname(&a, "NaoExisteDecode"), NULL,
                            dummy, 4, &out, &outlen);
        CHECK(st == PDF_FILT_ERR_UNKNOWN, "filtro inventado deveria dar UNKNOWN");

        /* Sem filtro: passa direto. */
        st = pdf_filt_apply(&a, NULL, NULL, dummy, 4, &out, &outlen);
        CHECK(st == PDF_FILT_OK && outlen == 4 && out && out[3] == 4,
              "sem filtro deveria copiar os dados crus");
        pdf_arena_free(&a);
    }

    printf("-- filtros: predictor PNG Up --\n");
    {
        /*
         * Predictors sao obrigatorios para xref stream: quase todo PDF 1.5+ usa
         * /Predictor 12. Sem isso, a tabela de referencia cruzada sai como lixo
         * e nenhum objeto e localizavel.
         *
         * Duas linhas de 3 colunas, filtro 2 (Up): a segunda linha e a
         * diferenca em relacao a primeira.
         */
        unsigned char raw[] = {
            2, 10, 20, 30,     /* filtro Up, mas primeira linha: acima = 0 */
            2,  1,  1,  1      /* +1 sobre a linha anterior */
        };
        unsigned long clen = compressBound(sizeof(raw));
        unsigned char *comp = (unsigned char *)malloc(clen);
        compress(comp, &clen, raw, sizeof(raw));

        PdfArena a;
        pdf_arena_init(&a, 4096);

        /* /DecodeParms << /Predictor 12 /Colors 1 /BitsPerComponent 8
         *                 /Columns 3 >> */
        PdfObj *parms = pdf_new(&a, PDF_DICT);
        struct { const char *k; long long v; } kv[] = {
            { "Predictor", 12 }, { "Colors", 1 },
            { "BitsPerComponent", 8 }, { "Columns", 3 }
        };
        for (size_t i = 0; i < sizeof(kv)/sizeof(kv[0]); ++i) {
            PdfObj *n = pdf_new(&a, PDF_INT);
            n->u.i = kv[i].v;
            PdfSlice key;
            key.p = pdf_arena_dup(&a, kv[i].k, strlen(kv[i].k));
            key.len = (int)strlen(kv[i].k);
            pdf_dict_put(&a, parms, key, n);
        }

        unsigned char *out = NULL;
        int outlen = 0;
        PdfFiltStatus st = pdf_filt_apply(&a, mkname(&a, "FlateDecode"), parms,
                                          comp, (int)clen, &out, &outlen);
        CHECK(st == PDF_FILT_OK, "predictor: status %s",
              pdf_filt_status_str(st));
        CHECK(outlen == 6, "predictor: %d bytes, esperado 6", outlen);
        if (outlen == 6 && out) {
            unsigned char want[6] = { 10, 20, 30, 11, 21, 31 };
            CHECK(memcmp(out, want, 6) == 0,
                  "predictor: [%d %d %d %d %d %d], esperado [10 20 30 11 21 31]",
                  out[0], out[1], out[2], out[3], out[4], out[5]);
        }
        free(comp);
        pdf_arena_free(&a);
    }
}

/* --- busca ---------------------------------------------------------------- */

static void test_find(void)
{
    printf("-- pdf_find / pdf_rfind com fronteira de bloco --\n");

    /*
     * A varredura le em blocos de 1 KB com sobreposicao. Sem a sobreposicao,
     * uma ocorrencia partida entre dois blocos passa batida - e `endstream`
     * partido na fronteira e justamente o caso que aparece em arquivo grande.
     * Este teste poe a agulha exatamente em cima de cada fronteira possivel.
     */
    const char *needle = "endstream";
    int nl = 9;
    int misses = 0;

    for (int pos = 1015; pos <= 1035; ++pos) {
        char *buf = (char *)malloc(4096);
        memset(buf, 'x', 4096);
        memcpy(buf + pos, needle, (size_t)nl);

        Fix f;
        fix_open(&f, buf, 4096);
        long long at = pdf_find(&f.st, 0, 0, needle);
        if (at != pos) {
            printf("  pdf_find na posicao %d devolveu %lld\n", pos, at);
            misses++;
        }
        long long rat = pdf_rfind(&f.st, 4096, 0, needle);
        if (rat != pos) {
            printf("  pdf_rfind na posicao %d devolveu %lld\n", pos, rat);
            misses++;
        }
        fix_close(&f);
        free(buf);
    }
    CHECK(misses == 0, "%d falhas de busca em fronteira de bloco", misses);

    /* Ausente devolve -1, nao um offset qualquer. */
    {
        Fix f;
        fix_open(&f, "nada aqui", -1);
        CHECK(pdf_find(&f.st, 0, 0, "endstream") == -1, "find deveria dar -1");
        CHECK(pdf_rfind(&f.st, 9, 0, "endstream") == -1, "rfind deveria dar -1");
        fix_close(&f);
    }
}

/* --- codificacoes e nomes de glifo --------------------------------------- */

static void test_encodings(void)
{
    printf("-- codificacoes --\n");

    /*
     * Estas tabelas foram digitadas a mao, centenas de codepoints. Um erro
     * nelas nao quebra nada visivelmente: produz UM caractere errado no meio do
     * texto de um livro, o que passa despercebido por muito tempo. Daqui saem
     * as amostras que garantem os pontos que importam.
     */
    struct { PdfEncKind e; int code; unsigned int want; const char *why; } t[] = {
        /* WinAnsi: o caminho critico (9 das 10 fontes do ilha.pdf) */
        { PDF_ENC_WINANSI, 0x41, 0x0041, "'A'" },
        { PDF_ENC_WINANSI, 0x20, 0x0020, "espaco" },
        { PDF_ENC_WINANSI, 0xE7, 0x00E7, "c cedilha (Latin-1 direto)" },
        { PDF_ENC_WINANSI, 0xE1, 0x00E1, "a agudo" },
        { PDF_ENC_WINANSI, 0xF5, 0x00F5, "o tilde" },
        { PDF_ENC_WINANSI, 0x80, 0x20AC, "euro (faixa CP1252)" },
        { PDF_ENC_WINANSI, 0x93, 0x201C, "aspa dupla esquerda" },
        { PDF_ENC_WINANSI, 0x94, 0x201D, "aspa dupla direita" },
        { PDF_ENC_WINANSI, 0x96, 0x2013, "meia-risca" },
        { PDF_ENC_WINANSI, 0x97, 0x2014, "travessao" },
        { PDF_ENC_WINANSI, 0x85, 0x2026, "reticencias" },
        { PDF_ENC_WINANSI, 0x81, 0x0000, "posicao vaga do CP1252" },

        /* Standard: as duas pegadinhas em ASCII */
        { PDF_ENC_STANDARD, 0x27, 0x2019, "apostrofo curvo, NAO reto" },
        { PDF_ENC_STANDARD, 0x60, 0x2018, "aspa simples esquerda" },
        { PDF_ENC_STANDARD, 0x41, 0x0041, "'A'" },
        { PDF_ENC_STANDARD, 0xD0, 0x2014, "travessao" },
        { PDF_ENC_STANDARD, 0xB7, 0x2022, "bolinha" },
        { PDF_ENC_STANDARD, 0xBC, 0x2026, "reticencias" },
        { PDF_ENC_STANDARD, 0xE9, 0x00D8, "O barrado" },

        /* MacRoman: reserva */
        { PDF_ENC_MACROMAN, 0x8E, 0x00E9, "e agudo" },
        { PDF_ENC_MACROMAN, 0x87, 0x00E1, "a agudo" },
        { PDF_ENC_MACROMAN, 0xD5, 0x2019, "apostrofo curvo" },
        { PDF_ENC_MACROMAN, 0xD1, 0x2014, "travessao" },
        { PDF_ENC_MACROMAN, 0x8D, 0x00E7, "c cedilha" },
    };

    for (size_t i = 0; i < sizeof(t)/sizeof(t[0]); ++i) {
        unsigned int got = pdf_enc_lookup(t[i].e, t[i].code);
        CHECK(got == t[i].want, "enc %d code 0x%02X (%s): U+%04X, esperado U+%04X",
              (int)t[i].e, t[i].code, t[i].why, got, t[i].want);
    }

    /* Latin-1 e identidade em WinAnsi de 0xA0 a 0xFF: verifica a faixa toda em
     * vez de confiar em amostras. */
    {
        int bad = 0;
        for (int c = 0xA0; c <= 0xFF; ++c)
            if (pdf_enc_lookup(PDF_ENC_WINANSI, c) != (unsigned int)c)
                bad++;
        CHECK(bad == 0, "%d posicoes de 0xA0..0xFF divergem do Latin-1", bad);
    }

    printf("-- nomes de glifo --\n");
    struct { const char *n; unsigned int want; } g[] = {
        { "space",       0x0020 },
        { "A",           0x0041 },   /* nome de um caractere */
        { "a",           0x0061 },
        { "zero",        0x0030 },
        { "aacute",      0x00E1 },
        { "ccedilla",    0x00E7 },
        { "atilde",      0x00E3 },
        { "ecircumflex", 0x00EA },
        { "emdash",      0x2014 },
        { "quoteright",  0x2019 },
        { "ellipsis",    0x2026 },
        { "germandbls",  0x00DF },
        { "uni00E9",     0x00E9 },   /* forma canonica da AGL */
        { "u00E9",       0x00E9 },
        { "a.sc",        0x0061 },   /* sufixo de variante e ignorado */
        { "one.oldstyle",0x0031 },
        /* Nomes de subset nao carregam informacao de caractere: devolver 0 e a
         * resposta honesta, porque inventar um codepoint aqui daria texto
         * plausivel e errado. */
        { "g42",         0 },
        { "cid1234",     0 },
        { "naoexiste",   0 },
    };
    for (size_t i = 0; i < sizeof(g)/sizeof(g[0]); ++i) {
        unsigned int got = pdf_glyphname_to_unicode(g[i].n, (int)strlen(g[i].n));
        CHECK(got == g[i].want, "glifo \"%s\": U+%04X, esperado U+%04X",
              g[i].n, got, g[i].want);
    }
}

/* --- documento sintetico completo ---------------------------------------- */

/*
 * PDF minimo montado a mao, exercitado de ponta a ponta.
 *
 * Testa numa tacada: reconstrucao do xref (o startxref aponta para 0 de
 * proposito), /Length ausente resolvido por endstream, fonte Type0 Identity-H,
 * CMap /ToUnicode com bfchar E bfrange, array /W nas duas formas, e a maquina
 * de estado do content stream.
 *
 * Sem arquivo externo: roda em qualquer maquina, sempre igual.
 */
static const char SYNTH_PDF[] =
    "%PDF-1.4\n"
    "1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n"
    "2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n"
    "3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100]\n"
    "   /Resources << /Font << /F1 4 0 R >> >> /Contents 6 0 R >> endobj\n"
    "4 0 obj << /Type /Font /Subtype /Type0 /BaseFont /Teste\n"
    "   /Encoding /Identity-H /DescendantFonts [5 0 R] /ToUnicode 7 0 R >> endobj\n"
    "5 0 obj << /Type /Font /Subtype /CIDFontType2 /BaseFont /Teste\n"
    "   /DW 1000 /W [1 [500 600] 3 5 700] >> endobj\n"
    "6 0 obj << >> stream\n"
    "BT /F1 12 Tf 10 50 Td <00010002000300040005> Tj ET\n"
    "endstream endobj\n"
    "7 0 obj << >> stream\n"
    "/CIDInit /ProcSet findresource begin\n"
    "12 dict begin\n"
    "begincmap\n"
    "1 begincodespacerange\n"
    "<0000> <FFFF>\n"
    "endcodespacerange\n"
    "2 beginbfchar\n"
    "<0001> <0041>\n"
    "<0002> <00E7>\n"
    "endbfchar\n"
    "1 beginbfrange\n"
    "<0003> <0005> <0061>\n"
    "endbfrange\n"
    "endcmap\n"
    "end end\n"
    "endstream endobj\n"
    "trailer << /Root 1 0 R /Size 8 >>\n"
    "startxref\n0\n%%EOF\n";

static void test_synth_document(void)
{
    printf("-- documento sintetico: xref reconstruido + Identity-H + ToUnicode --\n");

    PdfMemCtx mc;
    PdfIo io;
    pdf_io_mem(&io, &mc, (const unsigned char *)SYNTH_PDF,
               (int)sizeof(SYNTH_PDF) - 1);

    PdfDoc doc;
    int rc = pdf_doc_open(&doc, &io);
    CHECK(rc == 0, "pdf_doc_open falhou: %s", doc.err);
    if (rc != 0) { pdf_doc_close(&doc); return; }

    CHECK(doc.reconstructed == 1,
          "o xref deveria ter sido reconstruido (startxref aponta para 0)");
    CHECK(doc.pages == 1, "paginas: %d, esperado 1", doc.pages);
    CHECK(doc.ver_major == 1 && doc.ver_minor == 4,
          "versao PDF-%d.%d, esperado 1.4", doc.ver_major, doc.ver_minor);

    PdfArena a;
    pdf_arena_init(&a, 32768);

    PdfObj *page = pdf_doc_page(&doc, &a, 0);
    CHECK(page != NULL, "pagina 0 nao encontrada");

    if (page) {
        PdfTextPage tp;
        int erc = pdf_text_extract(&doc, &a, page, &tp);
        CHECK(erc == 0, "pdf_text_extract retornou %d", erc);

        CHECK(tp.mb_x1 == 200.0f && tp.mb_y1 == 100.0f,
              "MediaBox %.0fx%.0f, esperado 200x100", tp.mb_x1, tp.mb_y1);
        CHECK(tp.fonts_loaded == 1, "fontes carregadas: %d, esperado 1",
              tp.fonts_loaded);
        CHECK(tp.codes_total == 5, "codigos: %d, esperado 5", tp.codes_total);
        CHECK(tp.codes_unmapped == 0, "codigos sem mapeamento: %d, esperado 0",
              tp.codes_unmapped);
        CHECK(tp.nruns == 1, "runs: %d, esperado 1", tp.nruns);

        if (tp.nruns == 1) {
            const PdfTextRun *r = &tp.runs[0];

            /* Codigos 1..5 -> 'A' (bfchar), c-cedilha (bfchar), e a,b,c
             * (bfrange com incremento do ultimo codepoint). */
            const char *want = "A\xC3\xA7" "abc";
            int wl = (int)strlen(want);
            CHECK(r->len == wl, "bytes do run: %d, esperado %d", r->len, wl);
            if (r->len == wl)
                CHECK(memcmp(tp.text + r->off, want, (size_t)wl) == 0,
                      "texto extraido \"%.*s\", esperado \"Acabc\" com cedilha",
                      r->len, tp.text + r->off);

            CHECK(r->x > 9.9f && r->x < 10.1f, "x do run: %.2f, esperado 10", r->x);
            CHECK(r->y > 49.9f && r->y < 50.1f, "y do run: %.2f, esperado 50", r->y);
            CHECK(r->size > 11.9f && r->size < 12.1f,
                  "corpo: %.2f, esperado 12", r->size);

            /*
             * Largura esperada: as duas formas do /W tem de ser lidas
             * corretamente. [1 [500 600]] da 500 e 600 aos codigos 1 e 2;
             * [3 5 700] da 700 aos codigos 3, 4 e 5.
             *   (500 + 600 + 700*3) / 1000 * 12 = 38,4 pt
             * Se a leitura do /W se perder entre as duas formas, este numero
             * muda - e no leitor o efeito seria palavras empilhadas.
             */
            CHECK(r->width > 38.3f && r->width < 38.5f,
                  "largura do run: %.2f, esperado 38,40", r->width);
        }
    }

    pdf_arena_free(&a);
    pdf_doc_close(&doc);
}

int main(void)
{
    test_arena();
    test_lexer();
    test_parser();
    test_filters();
    test_find();
    test_encodings();
    test_synth_document();

    printf("\n%d ok, %d falhas\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
