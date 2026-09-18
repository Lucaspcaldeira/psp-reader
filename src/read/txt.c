#include <string.h>

#include "txt.h"
#include "utf8.h"

const char *txt_encoding_name(TxtEncoding e)
{
    return (e == TXT_ENC_LATIN1) ? "Latin-1" : "UTF-8";
}

/* ------------------------------------------------------------------------- */
/* Codificacao                                                                */

/*
 * UTF-8 ou Latin-1?
 *
 * Um .txt nao declara a codificacao, entao ela e adivinhada - mas a adivinhacao
 * aqui e quase segura, e vale explicar por que: UTF-8 e um codigo com
 * REDUNDANCIA. Um byte >= 0x80 obriga a um numero exato de bytes de continuacao
 * na faixa 0x80..0xBF. Texto Latin-1 de verdade quase nunca satisfaz isso por
 * acaso: "ção" em Latin-1 e E7 E3 6F, e E7 exige duas continuacoes que nao
 * chegam.
 *
 * Entao a regra e: se ha byte alto e TODA sequencia alta e UTF-8 valida, e
 * UTF-8; qualquer violacao decide por Latin-1. Um arquivo so de ASCII cai em
 * UTF-8, que e o mesmo resultado para ele.
 *
 * A amostra e o comeco do arquivo. Livro que so tem acento no capitulo 12 seria
 * classificado errado - mas livro em portugues sem nenhum acento nos primeiros
 * 8 KB nao e um caso realista, e o custo de varrer 1 MB no Memory Stick para
 * cobri-lo nao se paga.
 */
#define ENC_SAMPLE 8192

static TxtEncoding detect_encoding(PdfStream *st, long long off)
{
    unsigned char buf[512];
    long long p = off;
    long long end = off + ENC_SAMPLE;
    if (end > st->size)
        end = st->size;

    int pending = 0;        /* bytes de continuacao ainda esperados */
    int saw_high = 0;

    while (p < end) {
        int want = (int)(end - p);
        if (want > (int)sizeof(buf))
            want = (int)sizeof(buf);
        pdf_seek(st, p);
        int got = pdf_stream_read(st, buf, want);
        if (got <= 0)
            break;

        for (int i = 0; i < got; ++i) {
            unsigned char c = buf[i];
            if (pending > 0) {
                if (c < 0x80 || c > 0xBF)
                    return TXT_ENC_LATIN1;      /* continuacao que nao veio */
                pending--;
                continue;
            }
            if (c < 0x80)
                continue;
            saw_high = 1;
            if (c >= 0xC2 && c <= 0xDF)      pending = 1;
            else if (c >= 0xE0 && c <= 0xEF) pending = 2;
            else if (c >= 0xF0 && c <= 0xF4) pending = 3;
            else return TXT_ENC_LATIN1;      /* 0x80..0xC1 e 0xF5..0xFF */
        }
        p += got;
    }

    /* Sequencia cortada pelo fim da AMOSTRA nao e erro: o resto do arquivo
     * continua. Cortada pelo fim do ARQUIVO seria, mas nao vale mudar de
     * codificacao por causa do ultimo caractere. */
    (void)saw_high;
    return TXT_ENC_UTF8;
}

/*
 * Um byte Latin-1 -> UTF-8.
 *
 * Latin-1 puro mapeia byte para codepoint identico, mas a faixa 0x80..0x9F e
 * de controles que nao aparecem em texto - ali o que existe no mundo real e
 * CP1252, com aspas curvas, travessao e reticencias. Traduzir essa faixa como
 * Latin-1 puro daria caracteres de controle invisiveis exatamente onde estava a
 * pontuacao tipografica, que e o que mais aparece num livro.
 */
static const unsigned short CP1252_HIGH[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
};

static unsigned int latin1_cp(unsigned char c)
{
    if (c >= 0x80 && c <= 0x9F)
        return CP1252_HIGH[c - 0x80];
    return c;
}

/* ------------------------------------------------------------------------- */
/* Indice de unidades                                                         */

/*
 * Fronteira de unidade.
 *
 * Uma unidade nunca pode comecar no meio de um paragrafo, senao o mesmo indice
 * devolveria textos diferentes conforme o caminho pelo qual se chegou nele - e
 * o progresso de leitura, que guarda um indice, apontaria para o lugar errado.
 * Entao o corte anda ate a proxima linha em branco depois do alvo; nao havendo
 * uma em tempo razoavel, cai na proxima quebra de linha.
 */
#define UNIT_SLACK 4000

static long long next_boundary(PdfStream *st, long long from, int *clean)
{
    *clean = 0;

    long long target = from + TXT_UNIT_TARGET;
    if (target >= st->size)
        return st->size;

    pdf_seek(st, target);
    long long limit = target + UNIT_SLACK;
    if (limit > st->size)
        limit = st->size;

    long long first_nl = -1;
    int nl_run = 0;

    while (pdf_tell(st) < limit) {
        int c = pdf_getc(st);
        if (c < 0)
            break;
        if (c == '\n') {
            if (first_nl < 0)
                first_nl = pdf_tell(st);
            nl_run++;
            if (nl_run >= 2) {
                *clean = 1;                   /* linha em branco: fronteira */
                return pdf_tell(st);
            }
        } else if (c != '\r' && c != ' ' && c != '\t') {
            nl_run = 0;
        }
    }

    if (first_nl > 0)
        return first_nl;
    return limit;
}

int txt_open(TxtDoc *d, PdfArena *a, const PdfIo *io)
{
    memset(d, 0, sizeof(*d));

    if (pdf_stream_init(&d->st, io) != 0)
        return -1;

    /* BOM UTF-8: quando existe, encerra a adivinhacao. */
    d->data_off = 0;
    unsigned char bom[3];
    pdf_seek(&d->st, 0);
    if (pdf_stream_read(&d->st, bom, 3) == 3 &&
        bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF) {
        d->data_off = 3;
        d->enc = TXT_ENC_UTF8;
    } else {
        d->enc = detect_encoding(&d->st, 0);
    }

    d->units = (long long *)pdf_arena_alloc(a, sizeof(long long) * TXT_MAX_UNITS);
    d->clean = (unsigned char *)pdf_arena_alloc(a, TXT_MAX_UNITS);
    if (!d->units || !d->clean) {
        pdf_stream_close(&d->st);
        return -1;
    }

    long long off = d->data_off;
    int clean = 1;                 /* o inicio do arquivo e fronteira limpa */
    while (off < d->st.size) {
        if (d->nunits >= TXT_MAX_UNITS) {
            d->truncated = 1;
            break;
        }
        d->clean[d->nunits] = (unsigned char)clean;
        d->units[d->nunits++] = off;
        long long nxt = next_boundary(&d->st, off, &clean);
        if (nxt <= off) {          /* nunca deixa o indice parar de crescer */
            nxt = off + TXT_UNIT_TARGET;
            clean = 0;
        }
        off = nxt;
    }
    if (d->nunits == 0) {
        d->clean[0] = 1;
        d->units[d->nunits++] = d->data_off;
    }

    return 0;
}

void txt_close(TxtDoc *d)
{
    pdf_stream_close(&d->st);
    memset(d, 0, sizeof(*d));
}

/* ------------------------------------------------------------------------- */
/* Leitura de uma unidade                                                     */

/* Uma linha logica do arquivo, ja em UTF-8. */
typedef struct {
    int off, len;    /* fatia no buffer transcrito */
    int indent;      /* espacos/tabs no inicio, ja removidos da fatia */
    int blank;
} TxtLine;

int txt_unit(TxtDoc *d, PdfArena *a, int unit, Reflow *out)
{
    memset(out, 0, sizeof(*out));
    out->columns = 1;

    if (unit < 0 || unit >= d->nunits)
        return -1;

    long long start = d->units[unit];
    long long end   = (unit + 1 < d->nunits) ? d->units[unit + 1] : d->st.size;
    int raw_len = (int)(end - start);
    if (raw_len <= 0)
        return 0;

    /* Latin-1 pode dobrar (ou triplicar, em CP1252) o tamanho ao virar UTF-8. */
    int cap = raw_len * 3 + 16;
    char *buf = (char *)pdf_arena_alloc(a, (size_t)cap);
    unsigned char *raw = (unsigned char *)pdf_arena_alloc(a, (size_t)raw_len);
    if (!buf || !raw)
        return -1;

    pdf_seek(&d->st, start);
    int got = pdf_stream_read(&d->st, raw, raw_len);
    if (got <= 0)
        return 0;

    /* --- transcricao para UTF-8 e corte em linhas ------------------------- */
    int max_lines = raw_len / 2 + 8;
    TxtLine *lines = (TxtLine *)pdf_arena_alloc(a, sizeof(TxtLine) * (size_t)max_lines);
    if (!lines)
        return -1;

    int nlines = 0;
    int bl = 0;
    int i = 0;

    while (i < got && nlines < max_lines) {
        TxtLine *ln = &lines[nlines];
        ln->off = bl;
        ln->indent = 0;

        /* Recuo: contado e descartado. Um tab conta por oito, que e a
         * convencao de terminal e o que o autor do arquivo tinha em mente. */
        while (i < got && (raw[i] == ' ' || raw[i] == '\t')) {
            ln->indent += (raw[i] == '\t') ? 8 : 1;
            i++;
        }

        while (i < got && raw[i] != '\n' && raw[i] != '\r') {
            unsigned char c = raw[i++];
            if (d->enc == TXT_ENC_LATIN1) {
                char tmp[4];
                int n = utf8_encode(latin1_cp(c), tmp);
                if (n > 0 && bl + n <= cap) {
                    memcpy(buf + bl, tmp, (size_t)n);
                    bl += n;
                }
            } else if (bl < cap) {
                buf[bl++] = (char)c;
            }
        }

        /* CRLF conta como uma quebra so; contar duas transformaria todo
         * arquivo do Windows numa sequencia de paragrafos de uma linha. */
        if (i < got && raw[i] == '\r')
            i++;
        if (i < got && raw[i] == '\n')
            i++;

        /* Espaco a direita fora: ele falsearia o teste de linha em branco. */
        while (bl > ln->off && (buf[bl - 1] == ' ' || buf[bl - 1] == '\t'))
            bl--;

        ln->len = bl - ln->off;
        ln->blank = (ln->len == 0);
        nlines++;
    }

    /* --- linhas -> paragrafos --------------------------------------------- */
    out->paras = (RfPara *)pdf_arena_alloc(a, sizeof(RfPara) * RF_MAX_PARAS);
    out->buf = (char *)pdf_arena_alloc(a, (size_t)(bl + nlines + 16));
    if (!out->paras || !out->buf)
        return -1;
    int ocap = bl + nlines + 16;

    out->lines_in  = nlines;
    out->body_size = 10.0f;      /* nao ha corpo no arquivo; valor nominal */
    out->leading   = 12.0f;

    /*
     * Recuo modal, para distinguir "primeira linha de paragrafo" de "bloco
     * inteiro recuado" (citacao, verso, codigo). Sem isso um bloco recuado
     * viraria um paragrafo por linha.
     */
    int base_indent = 0;
    {
        int hist[9];
        memset(hist, 0, sizeof(hist));
        for (int k = 0; k < nlines; ++k) {
            if (lines[k].blank)
                continue;
            int b = lines[k].indent;
            if (b > 8) b = 8;
            hist[b]++;
        }
        int best = 0;
        for (int b = 0; b <= 8; ++b)
            if (hist[b] > hist[best])
                best = b;
        base_indent = best;
    }

    int obl = 0;
    RfPara *cur = NULL;
    int prev_blank = 1;

    for (int k = 0; k < nlines; ++k) {
        const TxtLine *ln = &lines[k];
        if (ln->blank) {
            prev_blank = 1;
            continue;
        }

        /*
         * Nova unidade de texto quando: veio depois de linha em branco, ou
         * comeca mais recuada que o corpo. Os dois sinais sao EXPLICITOS no
         * arquivo - e a diferenca em relacao ao PDF, onde a mesma decisao sai
         * de coordenadas e de heuristica.
         */
        int start_new = (cur == NULL) || prev_blank ||
                        (ln->indent > base_indent + 1);

        if (start_new) {
            if (out->nparas >= RF_MAX_PARAS) {
                out->truncated = 1;
                break;
            }
            cur = &out->paras[out->nparas++];
            memset(cur, 0, sizeof(*cur));
            cur->off  = obl;
            cur->size = out->body_size;
            cur->x    = (float)ln->indent;
        }

        out->hyphen_joins += reflow_append(out->buf, ocap, &obl, cur->off,
                                           buf + ln->off, ln->len, 1);
        cur->len = obl - cur->off;
        cur->nlines++;
        prev_blank = 0;
    }

    out->buflen = obl;

    /*
     * Emenda com a unidade vizinha.
     *
     * O corte cai em linha em branco sempre que existe uma perto do alvo, e ai
     * nao ha emenda a fazer - o paragrafo terminou de verdade. So quando o corte
     * foi forcado no meio de um bloco e que as marcas valem, e por isso o indice
     * guarda se cada fronteira saiu limpa. Marcar sempre faria todo paragrafo
     * inicial de unidade parecer continuacao, e a Etapa de emenda juntaria
     * paragrafos que nao tem nada a ver um com o outro.
     */
    if (out->nparas > 0) {
        if (unit > 0 && !d->clean[unit])
            out->paras[0].flags |= RF_CONT;
        if (unit + 1 < d->nunits && !d->clean[unit + 1])
            out->paras[out->nparas - 1].flags |= RF_OPEN;
    }

    return 0;
}
