#ifndef EREADER_PDF_FONT_H
#define EREADER_PDF_FONT_H

#include "pdf_doc.h"

/*
 * Fontes de PDF: do byte no content stream ao codepoint Unicode e a largura.
 *
 * Este e o modulo que decide se o texto extraido sai legivel ou sai como lixo.
 * Um content stream nao contem texto - contem CODIGOS, cujo significado
 * depende inteiramente do dicionario de fonte. O mesmo byte 0x41 pode ser 'A',
 * pode ser um glifo arbitrario de uma fonte com /Differences, ou pode ser meio
 * codigo de dois bytes numa fonte CID.
 *
 * Medido nos dois livros do corpus, sao exatamente dois caminhos que importam:
 *
 *   ilha.pdf   10 fontes Type1, 9 com /Encoding /WinAnsiEncoding
 *   cnv.pdf     3 fontes Type0 /Identity-H, CIDFontType2, todas com /ToUnicode
 *
 * Entao WinAnsi e ToUnicode sao os caminhos criticos; Standard e MacRoman
 * existem como reserva e ficam declarados como tal.
 */

typedef enum {
    PDF_ENC_NONE = 0,
    PDF_ENC_STANDARD,     /* padrao de fonte simples sem /Encoding */
    PDF_ENC_WINANSI,      /* CP1252 */
    PDF_ENC_MACROMAN,
    PDF_ENC_IDENTITY      /* Type0 Identity-H/V: o codigo E o CID, 2 bytes */
} PdfEncKind;

/*
 * Faixa de um CMap /ToUnicode.
 *
 * Guardado como faixas e nao como tabela de 65536 entradas: uma fonte
 * Identity-H mapeia codigos de 2 bytes, e uma tabela cheia custaria 128 KB por
 * fonte. Com tres fontes por pagina isso estouraria o orcamento de memoria
 * inteiro. As faixas de um livro real sao algumas dezenas.
 *
 * `dst` guarda a sequencia Unicode do PRIMEIRO codigo da faixa; para os
 * seguintes, o ultimo codepoint e incrementado - que e o que a especificacao
 * define para bfrange. Ate 4 codepoints cobre ligaduras (ffi) e o resto do que
 * aparece na pratica.
 */
typedef struct {
    unsigned int   lo, hi;
    unsigned short dst[4];
    unsigned char  dlen;
} PdfCMapRange;

typedef struct {
    unsigned int lo, hi;
    short w;              /* 1/1000 de unidade de espaco de texto */
} PdfWRange;

typedef struct {
    int        composite;    /* 1 = Type0 */
    int        code_bytes;   /* 1 em fonte simples, 2 em Identity-H */
    PdfEncKind base_enc;

    /*
     * Fonte simples: mapa direto de 256 codigos para codepoint.
     *
     * Ja com /Differences aplicado sobre a codificacao base. 0 significa sem
     * mapeamento. Meio kilobyte por fonte e barato e transforma a consulta num
     * indice de array, no caminho mais quente da extracao.
     */
    unsigned short simple[256];

    /* /ToUnicode. Tem PRIORIDADE sobre base_enc: quando o produtor do PDF
     * fornece um, ele e a fonte de verdade - o /Encoding pode ser um artefato
     * do subsetting. */
    PdfCMapRange *tou;
    int           tou_n;

    /* Larguras de fonte simples. */
    int    first_char;
    short *widths;
    int    nwidths;
    int    missing_width;

    /* Larguras de fonte CID. */
    int        default_width;   /* /DW, padrao 1000 */
    PdfWRange *wranges;
    int        nwranges;

    char basefont[64];
    char subtype[20];
} PdfFont;

/* Carrega a fonte a partir do dicionario. Toda alocacao vai para `a`.
 * Retorna 0 em sucesso; em falha, `out` fica utilizavel com padroes seguros
 * (WinAnsi, largura 500) - um texto com metricas aproximadas e melhor que
 * nenhum texto. */
int pdf_font_load(PdfDoc *doc, PdfArena *a, PdfObj *fontdict, PdfFont *out);

/* Le o proximo codigo de uma string de content stream.
 * Devolve quantos bytes consumiu (>= 1 sempre, para garantir progresso). */
int pdf_font_next_code(const PdfFont *f, const unsigned char *s, int len,
                       unsigned int *code);

/* Codepoints Unicode de um codigo. Devolve quantos escreveu; 0 se o codigo nao
 * tem mapeamento conhecido. */
int pdf_font_to_unicode(const PdfFont *f, unsigned int code,
                        unsigned int *out, int max);

/* Largura do codigo em 1/1000 de unidade de espaco de texto. */
int pdf_font_width(const PdfFont *f, unsigned int code);

/* --- exposto para teste ---------------------------------------------------- */

/* Codepoint de um codigo numa codificacao padrao, ou 0. */
unsigned int pdf_enc_lookup(PdfEncKind kind, int code);

/* Codepoint de um nome de glifo Adobe ("aacute", "uni00E1", "g42"), ou 0. */
unsigned int pdf_glyphname_to_unicode(const char *name, int len);

#endif
