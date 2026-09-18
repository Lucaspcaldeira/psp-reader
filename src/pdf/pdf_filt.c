#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "pdf_filt.h"

const char *pdf_filt_status_str(PdfFiltStatus s)
{
    switch (s) {
    case PDF_FILT_OK:          return "ok";
    case PDF_FILT_ERR_MEM:     return "sem memoria";
    case PDF_FILT_ERR_DATA:    return "dados corrompidos";
    case PDF_FILT_ERR_IMAGE:   return "filtro de imagem";
    case PDF_FILT_ERR_UNKNOWN: return "filtro desconhecido";
    }
    return "?";
}

int pdf_filt_is_image(const PdfObj *name)
{
    return pdf_name_is(name, "DCTDecode")  || pdf_name_is(name, "DCT") ||
           pdf_name_is(name, "JPXDecode")  ||
           pdf_name_is(name, "CCITTFaxDecode") || pdf_name_is(name, "CCF") ||
           pdf_name_is(name, "JBIG2Decode");
}

/* ------------------------------------------------------------------------- */
/* Buffer temporario crescente                                                */

/*
 * Os filtros escrevem num buffer de malloc que cresce, e so o resultado final
 * e copiado para a arena.
 *
 * Motivo: a arena nao tem realloc (e de proposito - realloc numa arena
 * invalidaria ponteiros ja entregues), e o tamanho descomprimido de um stream
 * Flate e desconhecido antes de descomprimir. Um /DecodeParms nao da o tamanho,
 * e nao existe campo no PDF que de.
 */
typedef struct {
    unsigned char *p;
    int len, cap;
    int oom;
} Buf;

static void buf_init(Buf *b) { memset(b, 0, sizeof(*b)); }
static void buf_free(Buf *b) { free(b->p); buf_init(b); }

static int buf_reserve(Buf *b, int extra)
{
    if (b->oom)
        return 0;
    if (b->len + extra <= b->cap)
        return 1;

    int ncap = b->cap ? b->cap : 8192;
    while (ncap < b->len + extra) {
        /* Teto de 24 MB: um /Length mentiroso ou um stream Flate hostil pode
         * pedir expansao ilimitada, e no PSP isso derruba o app em vez de
         * devolver erro. */
        if (ncap > (24 << 20)) {
            b->oom = 1;
            return 0;
        }
        ncap *= 2;
    }
    unsigned char *np = (unsigned char *)realloc(b->p, (size_t)ncap);
    if (!np) {
        b->oom = 1;
        return 0;
    }
    b->p = np;
    b->cap = ncap;
    return 1;
}

static int buf_put(Buf *b, int c)
{
    if (!buf_reserve(b, 1))
        return 0;
    b->p[b->len++] = (unsigned char)c;
    return 1;
}

/* ------------------------------------------------------------------------- */

static PdfFiltStatus do_flate(const unsigned char *src, int srclen, Buf *out)
{
    if (srclen <= 0)
        return PDF_FILT_OK;

    /*
     * Alguns geradores poem espaco em branco antes do cabecalho zlib. O
     * inflate rejeita isso como dado invalido, entao pulamos aqui.
     */
    int skip = 0;
    while (skip < srclen && (src[skip] == '\r' || src[skip] == '\n' ||
                             src[skip] == ' '  || src[skip] == '\t'))
        skip++;
    src += skip;
    srclen -= skip;
    if (srclen <= 0)
        return PDF_FILT_OK;

    /*
     * Duas tentativas: zlib com cabecalho e, se falhar de imediato, deflate
     * cru (windowBits negativo).
     *
     * Streams sem o cabecalho zlib de 2 bytes existem em PDF gerado por
     * bibliotecas que chamam deflate cru. Sem o segundo passo, esses streams
     * viram "dados corrompidos" e a pagina fica em branco.
     */
    for (int attempt = 0; attempt < 2; ++attempt) {
        z_stream z;
        memset(&z, 0, sizeof(z));
        if (inflateInit2(&z, attempt == 0 ? 15 : -15) != Z_OK)
            return PDF_FILT_ERR_MEM;

        z.next_in  = (Bytef *)src;
        z.avail_in = (uInt)srclen;

        int saved_len = out->len;
        unsigned char chunk[8192];
        int status = PDF_FILT_OK;
        int produced = 0;

        for (;;) {
            z.next_out  = chunk;
            z.avail_out = sizeof(chunk);

            int r = inflate(&z, Z_NO_FLUSH);
            int got = (int)(sizeof(chunk) - z.avail_out);

            if (got > 0) {
                if (!buf_reserve(out, got)) {
                    status = PDF_FILT_ERR_MEM;
                    break;
                }
                memcpy(out->p + out->len, chunk, (size_t)got);
                out->len += got;
                produced += got;
            }

            if (r == Z_STREAM_END)
                break;

            if (r != Z_OK) {
                /*
                 * Truncado ou corrompido. Se ja saiu alguma coisa, ficamos com
                 * o que saiu: pagina parcial e melhor que pagina vazia, e
                 * stream truncado no fim do arquivo e comum em PDF baixado
                 * incompleto.
                 */
                status = (produced > 0) ? PDF_FILT_OK : PDF_FILT_ERR_DATA;
                break;
            }

            if (z.avail_in == 0 && got == 0) {
                status = (produced > 0) ? PDF_FILT_OK : PDF_FILT_ERR_DATA;
                break;
            }
        }

        inflateEnd(&z);

        if (status == PDF_FILT_OK && produced > 0)
            return PDF_FILT_OK;
        if (status == PDF_FILT_ERR_MEM)
            return status;

        /* Desfaz o que a tentativa escreveu antes de tentar o modo cru. */
        out->len = saved_len;
    }
    return PDF_FILT_ERR_DATA;
}

static PdfFiltStatus do_ahx(const unsigned char *src, int srclen, Buf *out)
{
    int hi = -1;
    for (int i = 0; i < srclen; ++i) {
        int c = src[i];
        if (c == '>')
            break;
        int v;
        if      (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else continue;                       /* espaco e lixo sao ignorados */

        if (hi < 0) {
            hi = v;
        } else {
            if (!buf_put(out, (hi << 4) | v))
                return PDF_FILT_ERR_MEM;
            hi = -1;
        }
    }
    /* Nibble impar no fim completa com zero, conforme a especificacao. */
    if (hi >= 0 && !buf_put(out, hi << 4))
        return PDF_FILT_ERR_MEM;
    return PDF_FILT_OK;
}

static PdfFiltStatus do_a85(const unsigned char *src, int srclen, Buf *out)
{
    unsigned int tuple = 0;
    int count = 0;
    int i = 0;

    /* Prefixo <~ e opcional em PDF; presente em dados vindos de PostScript. */
    if (srclen >= 2 && src[0] == '<' && src[1] == '~')
        i = 2;

    for (; i < srclen; ++i) {
        int c = src[i];

        if (c == '~')                        /* ~> encerra */
            break;
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t' ||
            c == '\f' || c == 0)
            continue;

        if (c == 'z' && count == 0) {
            /* Atalho para quatro bytes zero. */
            for (int k = 0; k < 4; ++k)
                if (!buf_put(out, 0))
                    return PDF_FILT_ERR_MEM;
            continue;
        }

        if (c < '!' || c > 'u')
            return PDF_FILT_ERR_DATA;

        tuple = tuple * 85u + (unsigned int)(c - '!');
        if (++count == 5) {
            for (int k = 3; k >= 0; --k)
                if (!buf_put(out, (int)((tuple >> (k * 8)) & 0xFF)))
                    return PDF_FILT_ERR_MEM;
            tuple = 0;
            count = 0;
        }
    }

    if (count > 0) {
        /* Grupo final incompleto: completa com 'u' (84) e emite count-1 bytes. */
        for (int k = count; k < 5; ++k)
            tuple = tuple * 85u + 84u;
        for (int k = 0; k < count - 1; ++k)
            if (!buf_put(out, (int)((tuple >> ((3 - k) * 8)) & 0xFF)))
                return PDF_FILT_ERR_MEM;
    }
    return PDF_FILT_OK;
}

static PdfFiltStatus do_rl(const unsigned char *src, int srclen, Buf *out)
{
    int i = 0;
    while (i < srclen) {
        int n = src[i++];
        if (n == 128)
            break;                            /* EOD */
        if (n < 128) {
            /* n+1 bytes literais */
            int take = n + 1;
            if (i + take > srclen)
                take = srclen - i;
            if (take <= 0)
                break;
            if (!buf_reserve(out, take))
                return PDF_FILT_ERR_MEM;
            memcpy(out->p + out->len, src + i, (size_t)take);
            out->len += take;
            i += take;
        } else {
            /* o byte seguinte repetido 257-n vezes */
            if (i >= srclen)
                break;
            int c = src[i++];
            int rep = 257 - n;
            if (!buf_reserve(out, rep))
                return PDF_FILT_ERR_MEM;
            memset(out->p + out->len, c, (size_t)rep);
            out->len += rep;
        }
    }
    return PDF_FILT_OK;
}

/*
 * LZWDecode, a variante do PDF (e do TIFF).
 *
 * Difere do LZW do GIF em dois pontos: os codigos sao emitidos MSB primeiro, e
 * o parametro /EarlyChange (padrao 1) aumenta a largura do codigo um codigo
 * antes do que a aritmetica ingenua sugere. Ignorar o EarlyChange desalinha o
 * fluxo de bits no primeiro salto de largura, e a saida vira lixo a partir dali.
 */
static PdfFiltStatus do_lzw(const unsigned char *src, int srclen, Buf *out,
                            int early)
{
    enum { CLEAR = 256, EOD = 257, FIRST = 258, MAX_CODE = 4096 };

    /* Tabela como (prefixo, byte): reconstruir uma entrada e subir a cadeia de
     * prefixos, o que evita guardar strings de tamanho variavel. */
    static short  prefix[MAX_CODE];
    static unsigned char suffix[MAX_CODE];
    unsigned char stack[MAX_CODE];

    for (int i = 0; i < 256; ++i) {
        prefix[i] = -1;
        suffix[i] = (unsigned char)i;
    }

    int next     = FIRST;
    int codebits = 9;
    int prev     = -1;

    unsigned int acc = 0;
    int accbits = 0;
    int i = 0;

    for (;;) {
        while (accbits < codebits && i < srclen) {
            acc = (acc << 8) | src[i++];
            accbits += 8;
        }
        if (accbits < codebits)
            break;

        int code = (int)((acc >> (accbits - codebits)) & ((1u << codebits) - 1u));
        accbits -= codebits;

        if (code == EOD)
            break;

        if (code == CLEAR) {
            next     = FIRST;
            codebits = 9;
            prev     = -1;
            continue;
        }

        int emit = code;

        if (code >= next) {
            /* Codigo ainda nao definido: caso KwKwK, valido apenas quando e
             * exatamente o proximo a ser criado. */
            if (prev < 0 || code > next)
                return PDF_FILT_ERR_DATA;
            emit = prev;
        }

        /* Desempilha a cadeia de prefixos. */
        int sp = 0;
        int c  = emit;
        while (c >= 0 && sp < MAX_CODE) {
            stack[sp++] = suffix[c];
            c = prefix[c];
        }
        if (sp >= MAX_CODE)
            return PDF_FILT_ERR_DATA;

        unsigned char first_byte = stack[sp - 1];

        if (!buf_reserve(out, sp + 1))
            return PDF_FILT_ERR_MEM;
        while (sp > 0)
            out->p[out->len++] = stack[--sp];
        if (code >= next)
            out->p[out->len++] = first_byte;

        if (prev >= 0 && next < MAX_CODE) {
            prefix[next] = (short)prev;
            suffix[next] = first_byte;
            next++;
        }

        /* O -early e o ponto que quase todo mundo erra. */
        int limit = next + (early ? 1 : 0);
        if (limit >= 512 && codebits == 9)        codebits = 10;
        else if (limit >= 1024 && codebits == 10) codebits = 11;
        else if (limit >= 2048 && codebits == 11) codebits = 12;

        prev = code;
    }
    return PDF_FILT_OK;
}

/* ------------------------------------------------------------------------- */
/* Predictors                                                                 */

/*
 * Aplica o predictor inverso, em lugar, sobre b.
 *
 * Predictors existem porque comprimem melhor dados que variam pouco de linha
 * para linha. Sao obrigatorios para xref streams, que quase sempre usam
 * /Predictor 12 - sem isso, a tabela de referencia cruzada de qualquer PDF 1.5+
 * sai como lixo.
 */
static PdfFiltStatus unpredict(Buf *b, int pred, int colors, int bpc, int columns)
{
    if (pred < 2)
        return PDF_FILT_OK;

    if (colors <= 0) colors = 1;
    if (bpc <= 0) bpc = 8;
    if (columns <= 0) columns = 1;

    int bpp = (colors * bpc + 7) / 8;              /* bytes por pixel, min 1 */
    if (bpp < 1) bpp = 1;
    int rowlen = (columns * colors * bpc + 7) / 8;
    if (rowlen < 1)
        return PDF_FILT_ERR_DATA;

    if (pred == 2) {
        /* TIFF predictor 2: diferenca horizontal. Somente 8 bits por
         * componente e tratado; outras profundidades sao raras e ficariam
         * silenciosamente erradas, entao recusamos. */
        if (bpc != 8)
            return PDF_FILT_ERR_UNKNOWN;
        int rows = b->len / rowlen;
        for (int r = 0; r < rows; ++r) {
            unsigned char *row = b->p + (long)r * rowlen;
            for (int k = bpp; k < rowlen; ++k)
                row[k] = (unsigned char)(row[k] + row[k - bpp]);
        }
        return PDF_FILT_OK;
    }

    /* PNG predictors (10..15): cada linha vem precedida do byte de tipo de
     * filtro, entao a linha fisica tem rowlen+1 bytes. */
    int stride = rowlen + 1;
    int rows = b->len / stride;

    unsigned char *outp = (unsigned char *)malloc((size_t)rows * rowlen + 1);
    if (!outp)
        return PDF_FILT_ERR_MEM;
    memset(outp, 0, (size_t)rows * rowlen + 1);

    const unsigned char *prev = NULL;
    for (int r = 0; r < rows; ++r) {
        const unsigned char *in = b->p + (long)r * stride;
        int ft = in[0];
        in++;
        unsigned char *cur = outp + (long)r * rowlen;

        for (int k = 0; k < rowlen; ++k) {
            int a = (k >= bpp) ? cur[k - bpp] : 0;          /* esquerda */
            int up = prev ? prev[k] : 0;                     /* acima    */
            int c = (prev && k >= bpp) ? prev[k - bpp] : 0;  /* diagonal */
            int x = in[k];
            int v;

            switch (ft) {
            case 0: v = x;                    break;   /* None  */
            case 1: v = x + a;                break;   /* Sub   */
            case 2: v = x + up;               break;   /* Up    */
            case 3: v = x + ((a + up) >> 1);  break;   /* Average */
            case 4: {                                  /* Paeth */
                int p  = a + up - c;
                int pa = p > a  ? p - a  : a  - p;
                int pb = p > up ? p - up : up - p;
                int pc = p > c  ? p - c  : c  - p;
                int pr = (pa <= pb && pa <= pc) ? a : (pb <= pc ? up : c);
                v = x + pr;
                break;
            }
            default:
                /* Tipo de filtro invalido: trata como None em vez de abortar.
                 * Uma linha errada e melhor que perder a tabela xref inteira. */
                v = x;
                break;
            }
            cur[k] = (unsigned char)(v & 0xFF);
        }
        prev = cur;
    }

    free(b->p);
    b->p   = outp;
    b->len = rows * rowlen;
    b->cap = rows * rowlen + 1;
    return PDF_FILT_OK;
}

/* ------------------------------------------------------------------------- */

static PdfFiltStatus apply_one(const PdfObj *name, const PdfObj *parm,
                               const unsigned char *src, int srclen, Buf *out)
{
    if (pdf_filt_is_image(name))
        return PDF_FILT_ERR_IMAGE;

    PdfFiltStatus st;

    if (pdf_name_is(name, "FlateDecode") || pdf_name_is(name, "Fl"))
        st = do_flate(src, srclen, out);
    else if (pdf_name_is(name, "LZWDecode") || pdf_name_is(name, "LZW"))
        st = do_lzw(src, srclen, out, (int)pdf_int(pdf_dict_get(parm, "EarlyChange"), 1));
    else if (pdf_name_is(name, "ASCIIHexDecode") || pdf_name_is(name, "AHx"))
        st = do_ahx(src, srclen, out);
    else if (pdf_name_is(name, "ASCII85Decode") || pdf_name_is(name, "A85"))
        st = do_a85(src, srclen, out);
    else if (pdf_name_is(name, "RunLengthDecode") || pdf_name_is(name, "RL"))
        st = do_rl(src, srclen, out);
    else if (pdf_name_is(name, "Crypt")) {
        /* /Crypt com /Identity e no-op; e o unico caso que aparece em arquivo
         * nao criptografado. */
        if (!buf_reserve(out, srclen))
            return PDF_FILT_ERR_MEM;
        memcpy(out->p + out->len, src, (size_t)srclen);
        out->len += srclen;
        st = PDF_FILT_OK;
    } else {
        return PDF_FILT_ERR_UNKNOWN;
    }

    if (st != PDF_FILT_OK)
        return st;

    int pred = (int)pdf_int(pdf_dict_get(parm, "Predictor"), 1);
    if (pred > 1) {
        st = unpredict(out, pred,
                       (int)pdf_int(pdf_dict_get(parm, "Colors"), 1),
                       (int)pdf_int(pdf_dict_get(parm, "BitsPerComponent"), 8),
                       (int)pdf_int(pdf_dict_get(parm, "Columns"), 1));
    }
    return st;
}

PdfFiltStatus pdf_filt_apply(PdfArena *a,
                             const PdfObj *filter, const PdfObj *parms,
                             const unsigned char *src, int srclen,
                             unsigned char **out, int *outlen)
{
    *out = NULL;
    *outlen = 0;

    /* Sem filtro: os dados crus ja sao o resultado. */
    if (!filter || filter->kind == PDF_NULL) {
        unsigned char *p = (unsigned char *)pdf_arena_alloc(a, (size_t)srclen + 1);
        if (!p)
            return PDF_FILT_ERR_MEM;
        if (srclen > 0)
            memcpy(p, src, (size_t)srclen);
        p[srclen] = 0;
        *out = p;
        *outlen = srclen;
        return PDF_FILT_OK;
    }

    int n = (filter->kind == PDF_ARR) ? pdf_arr_len(filter) : 1;

    Buf cur, nxt;
    buf_init(&cur);
    buf_init(&nxt);

    /* Primeira etapa le de src; as seguintes leem da saida da anterior. */
    const unsigned char *in = src;
    int inlen = srclen;

    PdfFiltStatus st = PDF_FILT_OK;

    for (int i = 0; i < n; ++i) {
        const PdfObj *name = (filter->kind == PDF_ARR)
                           ? pdf_arr_get(filter, i) : filter;

        /* /DecodeParms acompanha o filtro: array paralelo quando ha cadeia,
         * dicionario unico quando ha um filtro so. */
        const PdfObj *parm = NULL;
        if (parms) {
            if (parms->kind == PDF_ARR)
                parm = pdf_arr_get(parms, i);
            else if (n == 1 || i == 0)
                parm = parms;
        }

        buf_init(&nxt);
        st = apply_one(name, parm, in, inlen, &nxt);
        if (st != PDF_FILT_OK) {
            buf_free(&nxt);
            buf_free(&cur);
            return st;
        }
        if (nxt.oom) {
            buf_free(&nxt);
            buf_free(&cur);
            return PDF_FILT_ERR_MEM;
        }

        buf_free(&cur);
        cur = nxt;
        in = cur.p;
        inlen = cur.len;
    }

    unsigned char *p = (unsigned char *)pdf_arena_alloc(a, (size_t)cur.len + 1);
    if (!p) {
        buf_free(&cur);
        return PDF_FILT_ERR_MEM;
    }
    if (cur.len > 0)
        memcpy(p, cur.p, (size_t)cur.len);
    p[cur.len] = 0;

    *out = p;
    *outlen = cur.len;
    buf_free(&cur);
    return PDF_FILT_OK;
}
