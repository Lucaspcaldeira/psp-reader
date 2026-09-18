#include <string.h>
#include <stdio.h>
#include "pdf_font.h"
#include "pdf_lex.h"

/* ========================================================================= */
/* Codificacoes padrao                                                        */
/* ========================================================================= */

/*
 * WinAnsiEncoding e CP1252: ASCII em 0x20..0x7E, Latin-1 em 0xA0..0xFF, e uma
 * faixa propria de 32 posicoes em 0x80..0x9F.
 *
 * Expresso como funcao em vez de tabela de 256 porque as duas faixas grandes
 * sao identidade - so os 32 do meio precisam de dados. Menos bytes de rodata e
 * impossivel errar por desalinhamento de tabela.
 */
static const unsigned short WIN_HI[32] = {
    0x20AC, 0x0000, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x0000, 0x017D, 0x0000,
    0x0000, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x0000, 0x017E, 0x0178
};

/*
 * StandardEncoding, faixa 0xA1..0xFF.
 *
 * Reserva: usada quando uma fonte simples nao declara /Encoding. Note as duas
 * pegadinhas em ASCII, tratadas em pdf_enc_lookup: 0x27 e quoteright (U+2019)
 * e 0x60 e quoteleft (U+2018), nao os ASCII retos. Um texto extraido com
 * StandardEncoding tratada como ASCII sai com apostrofos errados em toda
 * contracao.
 */
static const unsigned short STD_HI[95] = {
    /* A1 */ 0x00A1, 0x00A2, 0x00A3, 0x2044, 0x00A5, 0x0192, 0x00A7,
    /* A8 */ 0x00A4, 0x0027, 0x201C, 0x00AB, 0x2039, 0x203A, 0xFB01, 0xFB02,
    /* B0 */ 0x0000, 0x2013, 0x2020, 0x2021, 0x00B7, 0x0000, 0x00B6, 0x2022,
    /* B8 */ 0x201A, 0x201E, 0x201D, 0x00BB, 0x2026, 0x2030, 0x0000, 0x00BF,
    /* C0 */ 0x0000, 0x0060, 0x00B4, 0x02C6, 0x02DC, 0x00AF, 0x02D8, 0x02D9,
    /* C8 */ 0x00A8, 0x0000, 0x02DA, 0x00B8, 0x0000, 0x02DD, 0x02DB, 0x02C7,
    /* D0 */ 0x2014, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    /* D8 */ 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    /* E0 */ 0x0000, 0x00C6, 0x0000, 0x00AA, 0x0000, 0x0000, 0x0000, 0x0000,
    /* E8 */ 0x0141, 0x00D8, 0x0152, 0x00BA, 0x0000, 0x0000, 0x0000, 0x0000,
    /* F0 */ 0x0000, 0x00E6, 0x0000, 0x0000, 0x0000, 0x0131, 0x0000, 0x0000,
    /* F8 */ 0x0142, 0x00F8, 0x0153, 0x00DF, 0x0000, 0x0000, 0x0000
};

/*
 * MacRomanEncoding, faixa 0x80..0xFF.
 *
 * Reserva distante: nenhum dos dois livros do corpus usa. Mantida por
 * completude, com uma ressalva honesta - a posicao 0xDB e "currency" na
 * MacRomanEncoding do PDF, e nao o Euro do Mac OS moderno. Fontes que dependem
 * do resto exotico dessa faixa nao foram exercitadas.
 */
static const unsigned short MAC_HI[128] = {
    /* 80 */ 0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1,
    /* 88 */ 0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
    /* 90 */ 0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3,
    /* 98 */ 0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
    /* A0 */ 0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF,
    /* A8 */ 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
    /* B0 */ 0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211,
    /* B8 */ 0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
    /* C0 */ 0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB,
    /* C8 */ 0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
    /* D0 */ 0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA,
    /* D8 */ 0x00FF, 0x0178, 0x2044, 0x00A4, 0x2039, 0x203A, 0xFB01, 0xFB02,
    /* E0 */ 0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1,
    /* E8 */ 0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
    /* F0 */ 0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC,
    /* F8 */ 0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7
};

unsigned int pdf_enc_lookup(PdfEncKind kind, int code)
{
    if (code < 0 || code > 255)
        return 0;

    switch (kind) {
    case PDF_ENC_WINANSI:
        if (code >= 0x20 && code <= 0x7E) return (unsigned int)code;
        if (code >= 0x80 && code <= 0x9F) return WIN_HI[code - 0x80];
        if (code >= 0xA0)                 return (unsigned int)code;  /* Latin-1 */
        return 0;

    case PDF_ENC_STANDARD:
        /* As duas excecoes tipograficas da StandardEncoding em ASCII. */
        if (code == 0x27) return 0x2019;
        if (code == 0x60) return 0x2018;
        if (code >= 0x20 && code <= 0x7E) return (unsigned int)code;
        if (code >= 0xA1)                 return STD_HI[code - 0xA1];
        return 0;

    case PDF_ENC_MACROMAN:
        if (code >= 0x20 && code <= 0x7E) return (unsigned int)code;
        if (code >= 0x80)                 return MAC_HI[code - 0x80];
        return 0;

    default:
        return 0;
    }
}

/* ========================================================================= */
/* Nomes de glifo (subconjunto da Adobe Glyph List)                           */
/* ========================================================================= */

/*
 * Subconjunto da AGL, nao a lista inteira.
 *
 * A AGL completa tem cerca de 4000 entradas. Aqui estao os nomes que cobrem
 * Latin-1, Latin Extended-A util e a pontuacao tipografica - o repertorio de
 * texto ocidental. Nomes fora disso caem nas regras programaticas de
 * pdf_glyphname_to_unicode (uniXXXX, uXXXX, nome de um caractere so).
 *
 * Busca linear: e consultada uma vez por entrada de /Differences no carregamento
 * da fonte, nunca no laco de extracao.
 */
typedef struct { const char *n; unsigned short u; } GlyphName;

static const GlyphName AGL[] = {
    /* ASCII com nome proprio */
    {"space",0x0020},{"exclam",0x0021},{"quotedbl",0x0022},{"numbersign",0x0023},
    {"dollar",0x0024},{"percent",0x0025},{"ampersand",0x0026},{"quotesingle",0x0027},
    {"parenleft",0x0028},{"parenright",0x0029},{"asterisk",0x002A},{"plus",0x002B},
    {"comma",0x002C},{"hyphen",0x002D},{"period",0x002E},{"slash",0x002F},
    {"zero",0x0030},{"one",0x0031},{"two",0x0032},{"three",0x0033},{"four",0x0034},
    {"five",0x0035},{"six",0x0036},{"seven",0x0037},{"eight",0x0038},{"nine",0x0039},
    {"colon",0x003A},{"semicolon",0x003B},{"less",0x003C},{"equal",0x003D},
    {"greater",0x003E},{"question",0x003F},{"at",0x0040},
    {"bracketleft",0x005B},{"backslash",0x005C},{"bracketright",0x005D},
    {"asciicircum",0x005E},{"underscore",0x005F},{"grave",0x0060},
    {"braceleft",0x007B},{"bar",0x007C},{"braceright",0x007D},{"asciitilde",0x007E},

    /* Latin-1 */
    {"exclamdown",0x00A1},{"cent",0x00A2},{"sterling",0x00A3},{"currency",0x00A4},
    {"yen",0x00A5},{"brokenbar",0x00A6},{"section",0x00A7},{"dieresis",0x00A8},
    {"copyright",0x00A9},{"ordfeminine",0x00AA},{"guillemotleft",0x00AB},
    {"logicalnot",0x00AC},{"registered",0x00AE},{"macron",0x00AF},
    {"degree",0x00B0},{"plusminus",0x00B1},{"twosuperior",0x00B2},
    {"threesuperior",0x00B3},{"acute",0x00B4},{"mu",0x00B5},{"paragraph",0x00B6},
    {"periodcentered",0x00B7},{"cedilla",0x00B8},{"onesuperior",0x00B9},
    {"ordmasculine",0x00BA},{"guillemotright",0x00BB},{"onequarter",0x00BC},
    {"onehalf",0x00BD},{"threequarters",0x00BE},{"questiondown",0x00BF},
    {"Agrave",0x00C0},{"Aacute",0x00C1},{"Acircumflex",0x00C2},{"Atilde",0x00C3},
    {"Adieresis",0x00C4},{"Aring",0x00C5},{"AE",0x00C6},{"Ccedilla",0x00C7},
    {"Egrave",0x00C8},{"Eacute",0x00C9},{"Ecircumflex",0x00CA},{"Edieresis",0x00CB},
    {"Igrave",0x00CC},{"Iacute",0x00CD},{"Icircumflex",0x00CE},{"Idieresis",0x00CF},
    {"Eth",0x00D0},{"Ntilde",0x00D1},{"Ograve",0x00D2},{"Oacute",0x00D3},
    {"Ocircumflex",0x00D4},{"Otilde",0x00D5},{"Odieresis",0x00D6},{"multiply",0x00D7},
    {"Oslash",0x00D8},{"Ugrave",0x00D9},{"Uacute",0x00DA},{"Ucircumflex",0x00DB},
    {"Udieresis",0x00DC},{"Yacute",0x00DD},{"Thorn",0x00DE},{"germandbls",0x00DF},
    {"agrave",0x00E0},{"aacute",0x00E1},{"acircumflex",0x00E2},{"atilde",0x00E3},
    {"adieresis",0x00E4},{"aring",0x00E5},{"ae",0x00E6},{"ccedilla",0x00E7},
    {"egrave",0x00E8},{"eacute",0x00E9},{"ecircumflex",0x00EA},{"edieresis",0x00EB},
    {"igrave",0x00EC},{"iacute",0x00ED},{"icircumflex",0x00EE},{"idieresis",0x00EF},
    {"eth",0x00F0},{"ntilde",0x00F1},{"ograve",0x00F2},{"oacute",0x00F3},
    {"ocircumflex",0x00F4},{"otilde",0x00F5},{"odieresis",0x00F6},{"divide",0x00F7},
    {"oslash",0x00F8},{"ugrave",0x00F9},{"uacute",0x00FA},{"ucircumflex",0x00FB},
    {"udieresis",0x00FC},{"yacute",0x00FD},{"thorn",0x00FE},{"ydieresis",0x00FF},

    /* Latin Extended-A e modificadores usados em texto ocidental */
    {"dotlessi",0x0131},{"Lslash",0x0141},{"lslash",0x0142},
    {"OE",0x0152},{"oe",0x0153},{"Scaron",0x0160},{"scaron",0x0161},
    {"Ydieresis",0x0178},{"Zcaron",0x017D},{"zcaron",0x017E},{"florin",0x0192},
    {"circumflex",0x02C6},{"caron",0x02C7},{"breve",0x02D8},{"dotaccent",0x02D9},
    {"ring",0x02DA},{"ogonek",0x02DB},{"tilde",0x02DC},{"hungarumlaut",0x02DD},

    /* Pontuacao tipografica: a que mais aparece em livro editorado */
    {"endash",0x2013},{"emdash",0x2014},{"quoteleft",0x2018},{"quoteright",0x2019},
    {"quotesinglbase",0x201A},{"quotedblleft",0x201C},{"quotedblright",0x201D},
    {"quotedblbase",0x201E},{"dagger",0x2020},{"daggerdbl",0x2021},
    {"bullet",0x2022},{"ellipsis",0x2026},{"perthousand",0x2030},
    {"guilsinglleft",0x2039},{"guilsinglright",0x203A},{"fraction",0x2044},
    {"Euro",0x20AC},{"trademark",0x2122},{"minus",0x2212},{"lozenge",0x25CA},
    {"fi",0xFB01},{"fl",0xFB02},

    {NULL,0}
};

static int hexdig(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

unsigned int pdf_glyphname_to_unicode(const char *name, int len)
{
    if (!name || len <= 0)
        return 0;

    /* uniXXXX: forma canonica da AGL para codepoint arbitrario. Sequencias de
     * varios uniXXXX colados existem; usamos o primeiro. */
    if (len >= 7 && memcmp(name, "uni", 3) == 0) {
        unsigned int v = 0;
        int ok = 1;
        for (int i = 3; i < 7; ++i) {
            int d = hexdig(name[i]);
            if (d < 0) { ok = 0; break; }
            v = (v << 4) | (unsigned int)d;
        }
        if (ok)
            return v;
    }

    /* uXXXX ate uXXXXXXXX */
    if (len >= 5 && name[0] == 'u') {
        unsigned int v = 0;
        int i = 1;
        for (; i < len && i <= 8; ++i) {
            int d = hexdig(name[i]);
            if (d < 0)
                break;
            v = (v << 4) | (unsigned int)d;
        }
        if (i >= 5 && i == len)
            return v;
    }

    /* Sufixo de variante: "a.sc", "one.oldstyle". O nome base e o que importa. */
    int base = len;
    for (int i = 0; i < len; ++i) {
        if (name[i] == '.') {
            base = i;
            break;
        }
    }
    if (base <= 0)
        return 0;

    /*
     * Nome base de um caractere so: e o proprio caractere.
     *
     * Testado contra `base` e nao contra `len`, para que "a.sc" tambem caia
     * aqui - a AGL nao lista as letras isoladas na tabela, entao sem este
     * caminho "a.sc" nao resolveria para nada.
     */
    if (base == 1) {
        unsigned char c = (unsigned char)name[0];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9'))
            return c;
    }

    for (int i = 0; AGL[i].n; ++i) {
        int nl = (int)strlen(AGL[i].n);
        if (nl == base && memcmp(AGL[i].n, name, (size_t)base) == 0)
            return AGL[i].u;
    }

    /*
     * gNN, cidNN, indexNN: nomes de glifo de subset que NAO carregam informacao
     * de caractere. Devolver 0 e o comportamento correto - inventar um
     * codepoint aqui produziria texto plausivel e errado, que e pior que uma
     * lacuna visivel.
     */
    return 0;
}

/* ========================================================================= */
/* CMap /ToUnicode                                                            */
/* ========================================================================= */

/* Converte os bytes de uma string hexadecimal de CMap num inteiro. */
static unsigned int bytes_to_code(const unsigned char *p, int len)
{
    unsigned int v = 0;
    if (len > 4)
        len = 4;
    for (int i = 0; i < len; ++i)
        v = (v << 8) | p[i];
    return v;
}

/* Interpreta uma string de destino do bfchar/bfrange como sequencia UTF-16BE. */
static int dst_from_str(const unsigned char *p, int len, unsigned short *out,
                        int max)
{
    int n = 0;
    for (int i = 0; i + 1 < len && n < max; i += 2) {
        unsigned short u = (unsigned short)((p[i] << 8) | p[i + 1]);
        /*
         * Surrogates: um par UTF-16 vira um codepoint acima de U+FFFF, que nao
         * cabe num unsigned short. Fora do repertorio de um livro de texto, e
         * emitir metade do par produziria bytes UTF-8 invalidos - entao o par
         * inteiro e descartado.
         */
        if (u >= 0xD800 && u <= 0xDBFF) {
            i += 2;             /* pula o low surrogate tambem */
            continue;
        }
        if (u >= 0xDC00 && u <= 0xDFFF)
            continue;
        out[n++] = u;
    }
    return n;
}

#define TOU_MAX 4096

/*
 * Interpreta um stream /ToUnicode.
 *
 * Um CMap e PostScript, mas a parte que interessa e regular: blocos
 * begincodespacerange / beginbfchar / beginbfrange. Reaproveitamos o lexer do
 * PDF, que trata as mesmas regras de token - escrever um parser de PostScript
 * completo aqui seria trabalho sem retorno.
 */
static void parse_tounicode(PdfArena *a, const unsigned char *data, int len,
                            PdfFont *f)
{
    PdfMemCtx mc;
    PdfIo     io;
    PdfStream st;
    PdfArena  tmp;

    pdf_io_mem(&io, &mc, data, len);
    if (pdf_stream_init(&st, &io) != 0)
        return;
    pdf_arena_init(&tmp, 8192);

    PdfCMapRange *ranges = (PdfCMapRange *)pdf_arena_alloc(
        a, sizeof(PdfCMapRange) * TOU_MAX);
    if (!ranges) {
        pdf_arena_free(&tmp);
        return;
    }
    int n = 0;

    /* Pilha curta dos ultimos tokens: os blocos de CMap usam notacao pos-fixa
     * ("2 beginbfchar"), entao o operando vem antes do operador. */
    PdfTok prev[3];
    memset(prev, 0, sizeof(prev));

    int code_bytes_seen = 0;

    for (;;) {
        PdfTok t;
        pdf_lex_next(&st, &tmp, &t);
        if (t.kind == PT_EOF)
            break;

        if (t.kind == PT_KEYWORD && pdf_tok_is(&t, "begincodespacerange")) {
            /* Determina o tamanho do codigo pelo comprimento das strings da
             * faixa. Uma fonte Identity-H declara <0000> <FFFF>: 2 bytes. */
            for (;;) {
                PdfTok lo, hi;
                pdf_lex_next(&st, &tmp, &lo);
                if (lo.kind != PT_STR)
                    break;
                pdf_lex_next(&st, &tmp, &hi);
                if (hi.kind != PT_STR)
                    break;
                if (lo.s.len > code_bytes_seen)
                    code_bytes_seen = lo.s.len;
            }
        } else if (t.kind == PT_KEYWORD && pdf_tok_is(&t, "beginbfchar")) {
            for (;;) {
                PdfTok src, dst;
                pdf_lex_next(&st, &tmp, &src);
                if (src.kind != PT_STR)
                    break;                    /* endbfchar, ou lixo */
                pdf_lex_next(&st, &tmp, &dst);
                if (n >= TOU_MAX)
                    break;

                unsigned int code = bytes_to_code(
                    (const unsigned char *)src.s.p, src.s.len);
                if (src.s.len > code_bytes_seen)
                    code_bytes_seen = src.s.len;

                PdfCMapRange *r = &ranges[n];
                r->lo = r->hi = code;
                r->dlen = 0;

                if (dst.kind == PT_STR) {
                    r->dlen = (unsigned char)dst_from_str(
                        (const unsigned char *)dst.s.p, dst.s.len, r->dst, 4);
                } else if (dst.kind == PT_NAME) {
                    unsigned int u = pdf_glyphname_to_unicode(dst.s.p, dst.s.len);
                    if (u) { r->dst[0] = (unsigned short)u; r->dlen = 1; }
                }
                if (r->dlen > 0)
                    n++;
            }
        } else if (t.kind == PT_KEYWORD && pdf_tok_is(&t, "beginbfrange")) {
            for (;;) {
                PdfTok lo, hi, dst;
                pdf_lex_next(&st, &tmp, &lo);
                if (lo.kind != PT_STR)
                    break;
                pdf_lex_next(&st, &tmp, &hi);
                if (hi.kind != PT_STR)
                    break;
                pdf_lex_next(&st, &tmp, &dst);

                unsigned int c_lo = bytes_to_code(
                    (const unsigned char *)lo.s.p, lo.s.len);
                unsigned int c_hi = bytes_to_code(
                    (const unsigned char *)hi.s.p, hi.s.len);
                if (lo.s.len > code_bytes_seen)
                    code_bytes_seen = lo.s.len;
                if (c_hi < c_lo)
                    c_hi = c_lo;

                if (dst.kind == PT_ARR_OPEN) {
                    /*
                     * Forma de array: um destino por codigo da faixa. Expandida
                     * em entradas de um codigo cada, porque os destinos nao sao
                     * consecutivos - e o incremento do ultimo codepoint, que a
                     * outra forma usa, nao se aplica.
                     */
                    unsigned int c = c_lo;
                    for (;;) {
                        PdfTok item;
                        pdf_lex_next(&st, &tmp, &item);
                        if (item.kind != PT_STR)
                            break;            /* ']' ou lixo */
                        if (n >= TOU_MAX || c > c_hi)
                            continue;
                        PdfCMapRange *r = &ranges[n];
                        r->lo = r->hi = c;
                        r->dlen = (unsigned char)dst_from_str(
                            (const unsigned char *)item.s.p, item.s.len,
                            r->dst, 4);
                        if (r->dlen > 0)
                            n++;
                        c++;
                    }
                } else if (dst.kind == PT_STR) {
                    if (n >= TOU_MAX)
                        break;
                    PdfCMapRange *r = &ranges[n];
                    r->lo = c_lo;
                    r->hi = c_hi;
                    r->dlen = (unsigned char)dst_from_str(
                        (const unsigned char *)dst.s.p, dst.s.len, r->dst, 4);
                    if (r->dlen > 0)
                        n++;
                } else {
                    break;
                }
            }
        }

        prev[2] = prev[1];
        prev[1] = prev[0];
        prev[0] = t;

        /* A arena temporaria acumula um nome por token; reciclar mantem o uso
         * constante num CMap de centenas de KB. */
        if (tmp.handed > 128u * 1024u)
            pdf_arena_reset(&tmp);
    }

    f->tou   = ranges;
    f->tou_n = n;
    if (code_bytes_seen >= 2 && !f->composite) {
        /* Um CMap com codigos de 2 bytes numa fonte que parecia simples: o
         * codespacerange e mais confiavel que o /Subtype nesse caso. */
        f->code_bytes = 2;
    }

    pdf_arena_free(&tmp);
}

/* ========================================================================= */
/* Carregamento                                                               */
/* ========================================================================= */

static void slice_to_cstr(const PdfObj *o, char *dst, int cap)
{
    dst[0] = '\0';
    if (!o || (o->kind != PDF_NAME && o->kind != PDF_STR))
        return;
    int n = o->u.s.len < cap - 1 ? o->u.s.len : cap - 1;
    memcpy(dst, o->u.s.p, (size_t)n);
    dst[n] = '\0';
}

static PdfEncKind enc_from_name(const PdfObj *o)
{
    if (pdf_name_is(o, "WinAnsiEncoding"))   return PDF_ENC_WINANSI;
    if (pdf_name_is(o, "MacRomanEncoding"))  return PDF_ENC_MACROMAN;
    if (pdf_name_is(o, "StandardEncoding"))  return PDF_ENC_STANDARD;
    if (pdf_name_is(o, "MacExpertEncoding")) return PDF_ENC_STANDARD;  /* aprox. */
    return PDF_ENC_NONE;
}

static void load_simple_widths(PdfDoc *doc, PdfArena *a, PdfObj *fd,
                               PdfFont *f)
{
    f->first_char = (int)pdf_int(pdf_doc_dget(doc, a, fd, "FirstChar"), 0);

    PdfObj *w = pdf_doc_dget(doc, a, fd, "Widths");
    if (pdf_is(w, PDF_ARR) && pdf_arr_len(w) > 0) {
        int n = pdf_arr_len(w);
        f->widths = (short *)pdf_arena_alloc(a, sizeof(short) * (size_t)n);
        if (f->widths) {
            for (int i = 0; i < n; ++i) {
                PdfObj *e = pdf_doc_resolve(doc, a, pdf_arr_get(w, i));
                long long v = (long long)pdf_real(e, 0.0);
                if (v < -32768) v = -32768;
                if (v > 32767)  v = 32767;
                f->widths[i] = (short)v;
            }
            f->nwidths = n;
        }
    }

    PdfObj *desc = pdf_doc_dget(doc, a, fd, "FontDescriptor");
    f->missing_width = (int)pdf_int(pdf_doc_dget(doc, a, desc, "MissingWidth"), 0);
}

/*
 * Larguras de fonte CID, do array /W.
 *
 * Duas formas se alternam no mesmo array:
 *   c [w1 w2 ... wn]   larguras individuais a partir do CID c
 *   c1 c2 w            largura w para todos os CIDs de c1 a c2
 *
 * Ler as duas exige olhar o tipo do token seguinte, nao a posicao - e onde uma
 * implementacao ingenua se perde e desloca todas as larguras a partir do
 * primeiro array.
 */
static void load_cid_widths(PdfDoc *doc, PdfArena *a, PdfObj *cidfont,
                            PdfFont *f)
{
    f->default_width = (int)pdf_int(pdf_doc_dget(doc, a, cidfont, "DW"), 1000);

    PdfObj *w = pdf_doc_dget(doc, a, cidfont, "W");
    if (!pdf_is(w, PDF_ARR))
        return;

    int len = pdf_arr_len(w);

    /* Conta o pior caso para alocar uma vez. */
    int cap = 0;
    for (int i = 0; i < len; ++i) {
        PdfObj *e = pdf_doc_resolve(doc, a, pdf_arr_get(w, i));
        if (pdf_is(e, PDF_ARR))
            cap += pdf_arr_len(e);
        else
            cap += 1;
    }
    if (cap <= 0)
        return;

    f->wranges = (PdfWRange *)pdf_arena_alloc(a, sizeof(PdfWRange) * (size_t)cap);
    if (!f->wranges)
        return;

    int n = 0;
    int i = 0;
    while (i < len && n < cap) {
        PdfObj *first = pdf_doc_resolve(doc, a, pdf_arr_get(w, i));
        if (!first || (first->kind != PDF_INT && first->kind != PDF_REAL))
            break;
        unsigned int c1 = (unsigned int)pdf_int(first, 0);
        i++;
        if (i >= len)
            break;

        PdfObj *second = pdf_doc_resolve(doc, a, pdf_arr_get(w, i));

        if (pdf_is(second, PDF_ARR)) {
            int m = pdf_arr_len(second);
            for (int k = 0; k < m && n < cap; ++k) {
                PdfObj *wv = pdf_doc_resolve(doc, a, pdf_arr_get(second, k));
                f->wranges[n].lo = c1 + (unsigned int)k;
                f->wranges[n].hi = c1 + (unsigned int)k;
                f->wranges[n].w  = (short)pdf_real(wv, (double)f->default_width);
                n++;
            }
            i++;
        } else {
            /* c1 c2 w */
            unsigned int c2 = (unsigned int)pdf_int(second, 0);
            i++;
            if (i >= len)
                break;
            PdfObj *wv = pdf_doc_resolve(doc, a, pdf_arr_get(w, i));
            i++;
            if (c2 < c1)
                c2 = c1;
            f->wranges[n].lo = c1;
            f->wranges[n].hi = c2;
            f->wranges[n].w  = (short)pdf_real(wv, (double)f->default_width);
            n++;
        }
    }
    f->nwranges = n;
}

int pdf_font_load(PdfDoc *doc, PdfArena *a, PdfObj *fontdict, PdfFont *out)
{
    memset(out, 0, sizeof(*out));
    out->code_bytes    = 1;
    out->default_width = 1000;
    out->missing_width = 0;

    PdfObj *fd = pdf_doc_resolve(doc, a, fontdict);
    if (!pdf_is(fd, PDF_DICT)) {
        /* Sem dicionario nao ha nada a fazer, mas devolvemos padroes seguros:
         * texto com metricas aproximadas e melhor que pagina vazia. */
        out->base_enc = PDF_ENC_WINANSI;
        for (int c = 0; c < 256; ++c)
            out->simple[c] = (unsigned short)pdf_enc_lookup(PDF_ENC_WINANSI, c);
        return -1;
    }

    slice_to_cstr(pdf_doc_dget(doc, a, fd, "Subtype"),  out->subtype,  sizeof(out->subtype));
    slice_to_cstr(pdf_doc_dget(doc, a, fd, "BaseFont"), out->basefont, sizeof(out->basefont));

    PdfObj *enc = pdf_doc_dget(doc, a, fd, "Encoding");

    if (strcmp(out->subtype, "Type0") == 0) {
        out->composite  = 1;
        out->code_bytes = 2;
        out->base_enc   = PDF_ENC_IDENTITY;

        /*
         * Somente Identity-H/V e tratado como identidade. Outros CMaps
         * predefinidos (UniJIS, GBK...) mapeiam codigo para CID de forma nao
         * trivial; sem o CMap, o /ToUnicode e a unica fonte de verdade, e por
         * isso ele tem prioridade adiante.
         */
        PdfObj *df = pdf_doc_dget(doc, a, fd, "DescendantFonts");
        PdfObj *cid = pdf_doc_resolve(doc, a, pdf_arr_get(df, 0));
        if (pdf_is(cid, PDF_DICT))
            load_cid_widths(doc, a, cid, out);
        else
            out->default_width = 1000;
    } else {
        /*
         * Fonte simples. A codificacao base vem de /Encoding, que pode ser um
         * nome ou um dicionario com /BaseEncoding e /Differences.
         *
         * Sem /Encoding, a especificacao manda usar a codificacao embutida na
         * fonte - que esta dentro do programa Type1/CFF e nao e legivel aqui.
         * StandardEncoding e a reserva; ela erra os acentos de uma fonte com
         * codificacao customizada, mas acerta ASCII, que e a maior parte.
         */
        PdfEncKind base = PDF_ENC_NONE;
        PdfObj *diffs = NULL;

        if (pdf_is(enc, PDF_NAME)) {
            base = enc_from_name(enc);
        } else if (pdf_is(enc, PDF_DICT)) {
            base = enc_from_name(pdf_doc_dget(doc, a, enc, "BaseEncoding"));
            diffs = pdf_doc_dget(doc, a, enc, "Differences");
        }
        if (base == PDF_ENC_NONE)
            base = PDF_ENC_STANDARD;

        out->base_enc = base;
        for (int c = 0; c < 256; ++c)
            out->simple[c] = (unsigned short)pdf_enc_lookup(base, c);

        /*
         * /Differences: [ codigo /nome /nome ... codigo /nome ... ]
         * Cada inteiro reposiciona o cursor; cada nome ocupa a posicao atual e
         * avanca. Tratar o array como pares fixos e um erro classico que
         * desloca tudo depois do primeiro salto de codigo.
         */
        if (pdf_is(diffs, PDF_ARR)) {
            int cur = 0;
            for (int i = 0; i < pdf_arr_len(diffs); ++i) {
                PdfObj *e = pdf_doc_resolve(doc, a, pdf_arr_get(diffs, i));
                if (!e)
                    continue;
                if (e->kind == PDF_INT || e->kind == PDF_REAL) {
                    cur = (int)pdf_int(e, 0);
                } else if (e->kind == PDF_NAME) {
                    if (cur >= 0 && cur < 256) {
                        unsigned int u = pdf_glyphname_to_unicode(e->u.s.p,
                                                                 e->u.s.len);
                        /* Nome desconhecido (gNN de subset) NAO apaga o que a
                         * codificacao base ja dava: manter o mapeamento base e
                         * melhor que criar uma lacuna. */
                        if (u)
                            out->simple[cur] = (unsigned short)u;
                    }
                    cur++;
                }
            }
        }

        load_simple_widths(doc, a, fd, out);
    }

    /* /ToUnicode por ultimo: tem prioridade sobre tudo. */
    PdfObj *tou = pdf_doc_dget(doc, a, fd, "ToUnicode");
    if (pdf_is(tou, PDF_STREAM)) {
        unsigned char *data = NULL;
        int len = 0;
        if (pdf_doc_stream_data(doc, a, tou, &data, &len) == PDF_FILT_OK &&
            len > 0)
            parse_tounicode(a, data, len, out);
    }

    return 0;
}

/* ========================================================================= */
/* Consulta                                                                   */
/* ========================================================================= */

int pdf_font_next_code(const PdfFont *f, const unsigned char *s, int len,
                       unsigned int *code)
{
    if (len <= 0) {
        *code = 0;
        return 1;      /* nunca devolve 0: o chamador precisa progredir */
    }
    if (f->code_bytes >= 2 && len >= 2) {
        *code = ((unsigned int)s[0] << 8) | s[1];
        return 2;
    }
    if (f->code_bytes >= 2) {
        /* Byte impar no fim de uma string de codigos de 2 bytes: PDF
         * malformado. Consome o que sobrou em vez de girar em falso. */
        *code = s[0];
        return 1;
    }
    *code = s[0];
    return 1;
}

int pdf_font_to_unicode(const PdfFont *f, unsigned int code,
                        unsigned int *out, int max)
{
    if (max <= 0)
        return 0;

    /* /ToUnicode primeiro. */
    for (int i = 0; i < f->tou_n; ++i) {
        const PdfCMapRange *r = &f->tou[i];
        if (code < r->lo || code > r->hi)
            continue;

        int n = r->dlen < max ? r->dlen : max;
        for (int k = 0; k < n; ++k)
            out[k] = r->dst[k];

        /* Numa faixa, o ULTIMO codepoint recebe o deslocamento. */
        if (n > 0 && code > r->lo)
            out[n - 1] += (code - r->lo);
        return n;
    }

    if (!f->composite && code < 256 && f->simple[code]) {
        out[0] = f->simple[code];
        return 1;
    }

    /*
     * Sem mapeamento.
     *
     * Numa fonte CID com Identity-H e sem /ToUnicode, o codigo e um indice de
     * glifo dentro do arquivo de fonte - nao tem relacao nenhuma com Unicode.
     * Devolver 0 e a resposta honesta; adivinhar produziria texto plausivel e
     * errado, que e o pior resultado possivel num leitor de livros.
     */
    return 0;
}

int pdf_font_width(const PdfFont *f, unsigned int code)
{
    if (f->composite) {
        for (int i = 0; i < f->nwranges; ++i)
            if (code >= f->wranges[i].lo && code <= f->wranges[i].hi)
                return f->wranges[i].w;
        return f->default_width;
    }

    int idx = (int)code - f->first_char;
    if (f->widths && idx >= 0 && idx < f->nwidths) {
        short w = f->widths[idx];
        /* Largura 0 no array e legitima (glifo sem avanco), mas em fonte de
         * subsetting mal gerada ela aparece onde deveria haver largura real.
         * Cair no MissingWidth nesse caso evita todas as palavras empilhadas
         * na mesma coluna. */
        if (w != 0)
            return w;
    }
    if (f->missing_width > 0)
        return f->missing_width;

    /*
     * Ultima reserva: 500/1000, meia largura de em.
     *
     * Nao e a largura certa de nada, mas mantem o texto avancando de forma
     * plausivel. Zero faria todos os glifos se sobreporem no mesmo ponto, e o
     * reflow leria a linha inteira como uma palavra so.
     */
    return 500;
}
