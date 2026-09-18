#include "utf8.h"

/* Menor codepoint que cada comprimento de sequencia pode representar
 * legitimamente. E o teste de overlong: se o valor decodificado ficar abaixo
 * do minimo do seu comprimento, a sequencia e uma codificacao inflada do mesmo
 * caractere e precisa ser rejeitada. */
static const unsigned int MIN_FOR_LEN[5] = { 0, 0x0, 0x80, 0x800, 0x10000 };

unsigned int utf8_next(const char **p, const char *end)
{
    const unsigned char *s = (const unsigned char *)*p;
    if ((const char *)s >= end)
        return 0;

    unsigned char c = s[0];

    /* ASCII: o caso de longe mais comum em texto de livro ocidental. */
    if (c < 0x80) {
        *p = (const char *)(s + 1);
        return c;
    }

    int len;
    unsigned int cp;

    if ((c & 0xE0) == 0xC0)      { len = 2; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
    else {
        /* 10xxxxxx (continuacao solta) ou 0xFE/0xFF, que nao existem em UTF-8.
         * Avanca 1 para garantir progresso. */
        *p = (const char *)(s + 1);
        return UTF8_REPLACEMENT;
    }

    /* Truncado pelo fim do buffer. Consome so 1 byte: o resto pode ser o inicio
     * de uma sequencia valida se o chamador reabastecer o buffer. */
    if ((const char *)(s + len) > end) {
        *p = (const char *)(s + 1);
        return UTF8_REPLACEMENT;
    }

    for (int i = 1; i < len; ++i) {
        if ((s[i] & 0xC0) != 0x80) {
            /* Continuacao faltando. Avanca somente ate onde a sequencia era
             * valida, para nao engolir o byte que quebrou o padrao - ele pode
             * ser o inicio legitimo do proximo caractere. */
            *p = (const char *)(s + i);
            return UTF8_REPLACEMENT;
        }
        cp = (cp << 6) | (unsigned int)(s[i] & 0x3F);
    }

    *p = (const char *)(s + len);

    if (cp < MIN_FOR_LEN[len])            return UTF8_REPLACEMENT;  /* overlong */
    if (cp >= 0xD800u && cp <= 0xDFFFu)   return UTF8_REPLACEMENT;  /* surrogate */
    if (cp > 0x10FFFFu)                   return UTF8_REPLACEMENT;

    return cp;
}

int utf8_encode(unsigned int cp, char out[4])
{
    if (cp >= 0xD800u && cp <= 0xDFFFu) return 0;
    if (cp > 0x10FFFFu)                 return 0;

    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

size_t utf8_count(const char *s, const char *end)
{
    size_t n = 0;
    while (s < end) {
        utf8_next(&s, end);
        n++;
    }
    return n;
}
