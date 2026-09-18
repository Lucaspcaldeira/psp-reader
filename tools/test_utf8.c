/*
 * Teste de host para src/read/utf8.c. Roda via .\test.ps1 utf8
 *
 * Nao usa framework: um contador de falhas e uma macro. O valor esta em rodar
 * em segundos com ASan/UBSan ligados, nao em infraestrutura de teste.
 */
#include <stdio.h>
#include <string.h>
#include "utf8.h"

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, fmt, ...)                                                  \
    do {                                                                       \
        if (cond) { g_pass++; }                                                \
        else { g_fail++; printf("  FALHA %s:%d  " fmt "\n",                     \
                                __FILE__, __LINE__, ##__VA_ARGS__); }          \
    } while (0)

/* Decodifica um literal e confere codepoint + quantos bytes consumiu. */
static void expect_one(const char *label, const char *bytes, size_t n,
                       unsigned int want_cp, size_t want_adv)
{
    const char *p = bytes;
    unsigned int cp = utf8_next(&p, bytes + n);
    size_t adv = (size_t)(p - bytes);

    CHECK(cp == want_cp, "%s: codepoint U+%04X, esperado U+%04X", label, cp, want_cp);
    CHECK(adv == want_adv, "%s: avancou %zu bytes, esperado %zu", label, adv, want_adv);
}

int main(void)
{
    printf("-- ASCII --\n");
    expect_one("'A'", "A", 1, 0x41, 1);
    expect_one("NUL no meio", "\0x", 2, 0x00, 1);

    printf("-- 2 bytes: acentuacao portuguesa --\n");
    expect_one("a-agudo",   "\xC3\xA1", 2, 0x00E1, 2);  /* a */
    expect_one("c-cedilha", "\xC3\xA7", 2, 0x00E7, 2);  /* c */
    expect_one("o-tilde",   "\xC3\xB5", 2, 0x00F5, 2);  /* o */

    printf("-- 3 bytes: pontuacao tipografica (o que o Latin-1 perdia) --\n");
    expect_one("travessao longo",  "\xE2\x80\x94", 3, 0x2014, 3);
    expect_one("aspa dupla esq",   "\xE2\x80\x9C", 3, 0x201C, 3);
    expect_one("aspa dupla dir",   "\xE2\x80\x9D", 3, 0x201D, 3);
    expect_one("reticencias",      "\xE2\x80\xA6", 3, 0x2026, 3);

    printf("-- 4 bytes --\n");
    expect_one("U+1F4D6 livro", "\xF0\x9F\x93\x96", 4, 0x1F4D6, 4);

    printf("-- rejeicoes --\n");
    /* Overlong: C0 80 codifica U+0000 em 2 bytes. Vetor classico de bypass de
     * validacao; tem de virar U+FFFD, nao NUL. */
    expect_one("overlong C0 80", "\xC0\x80", 2, UTF8_REPLACEMENT, 2);
    /* E0 80 80 codifica U+0000 em 3 bytes. */
    expect_one("overlong E0 80 80", "\xE0\x80\x80", 3, UTF8_REPLACEMENT, 3);
    /* Surrogate U+D800, invalido em UTF-8. */
    expect_one("surrogate ED A0 80", "\xED\xA0\x80", 3, UTF8_REPLACEMENT, 3);
    /* Acima de U+10FFFF. */
    expect_one("fora do range F4 90 80 80", "\xF4\x90\x80\x80", 4, UTF8_REPLACEMENT, 4);
    /* Continuacao solta. */
    expect_one("continuacao solta 80", "\x80", 1, UTF8_REPLACEMENT, 1);
    /* 0xFF nao existe em UTF-8. */
    expect_one("byte FF", "\xFF", 1, UTF8_REPLACEMENT, 1);
    /* Truncado pelo fim do buffer: consome 1 so. */
    expect_one("truncado C3 no fim", "\xC3", 1, UTF8_REPLACEMENT, 1);
    /* Continuacao faltando: 'A' NAO pode ser engolido, e o proximo caractere. */
    expect_one("C3 seguido de 'A'", "\xC3\x41", 2, UTF8_REPLACEMENT, 1);

    printf("-- 'A' sobrevive a sequencia quebrada anterior --\n");
    {
        const char *s = "\xC3\x41";
        const char *p = s;
        const char *end = s + 2;
        unsigned int a = utf8_next(&p, end);
        unsigned int b = utf8_next(&p, end);
        CHECK(a == UTF8_REPLACEMENT, "primeiro deveria ser U+FFFD, veio U+%04X", a);
        CHECK(b == 0x41, "segundo deveria ser 'A', veio U+%04X", b);
        CHECK(p == end, "deveria ter consumido tudo");
    }

    printf("-- INVARIANTE: utf8_next sempre avanca --\n");
    {
        /* Todo byte isolado, de 0x00 a 0xFF, precisa avancar o ponteiro. Se um
         * unico nao avancar, o leitor trava num laco infinito no meio de um
         * livro - e arquivos reais tem bytes invalidos. */
        int stuck = 0;
        for (int i = 0; i < 256; ++i) {
            char buf[1] = { (char)i };
            const char *p = buf;
            utf8_next(&p, buf + 1);
            if (p == buf) { printf("  byte 0x%02X nao avancou\n", i); stuck++; }
        }
        CHECK(stuck == 0, "%d bytes nao avancaram", stuck);

        /* Idem para todo par de bytes com primeiro byte de sequencia longa. */
        stuck = 0;
        for (int a = 0xC0; a < 0x100; ++a) {
            for (int b = 0; b < 256; ++b) {
                char buf[2] = { (char)a, (char)b };
                const char *p = buf;
                utf8_next(&p, buf + 2);
                if (p == buf) stuck++;
            }
        }
        CHECK(stuck == 0, "%d pares nao avancaram", stuck);
    }

    printf("-- utf8_count --\n");
    {
        /* "Ola, mundo" com o-tilde e travessao: 8 codepoints em 11 bytes. */
        const char *s = "S\xC3\xA3o \xE2\x80\x94 fim";
        size_t bytes = strlen(s);
        CHECK(bytes == 12, "esperava 12 bytes, veio %zu", bytes);
        CHECK(utf8_count(s, s + bytes) == 9,
              "esperava 9 codepoints, veio %zu", utf8_count(s, s + bytes));
    }

    printf("-- round-trip encode/decode --\n");
    {
        unsigned int probes[] = {
            0x41, 0xE1, 0xE7, 0x2014, 0x201C, 0x2026, 0x20AC, 0xFFFD, 0x1F4D6, 0x10FFFF
        };
        for (size_t i = 0; i < sizeof(probes)/sizeof(probes[0]); ++i) {
            char buf[4];
            int n = utf8_encode(probes[i], buf);
            CHECK(n > 0, "encode de U+%04X falhou", probes[i]);
            if (n <= 0) continue;
            const char *p = buf;
            unsigned int back = utf8_next(&p, buf + n);
            CHECK(back == probes[i], "round-trip U+%04X voltou U+%04X", probes[i], back);
            CHECK(p == buf + n, "round-trip U+%04X consumiu %d de %d",
                  probes[i], (int)(p - buf), n);
        }
        /* Surrogate nao deve ser codificavel. */
        char buf[4];
        CHECK(utf8_encode(0xD800, buf) == 0, "surrogate nao deveria codificar");
        CHECK(utf8_encode(0x110000, buf) == 0, "acima do range nao deveria codificar");
    }

    printf("\n%d ok, %d falhas\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
