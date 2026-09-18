#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "pdf_doc.h"
#include "pdf_lex.h"

/* Limites de sanidade. Todos existem contra arquivo corrompido ou hostil, nao
 * contra arquivo grande legitimo. */
#define MAX_XREF_DEPTH   32       /* elos da cadeia /Prev */
#define MAX_OBJNUM       8388608  /* 8M objetos: acima disso e /Size absurdo */
#define MAX_RESOLVE      32       /* ref -> ref -> ref ... */
#define MAX_PAGETREE     65536    /* nos visitados ao contar paginas */

static void set_err(PdfDoc *doc, const char *msg)
{
    if (!doc->err[0])
        snprintf(doc->err, sizeof(doc->err), "%s", msg);
}

/* ------------------------------------------------------------------------- */
/* Cabecalho                                                                  */

static void read_header(PdfDoc *doc)
{
    doc->ver_major = 0;
    doc->ver_minor = 0;
    doc->hdr_off   = 0;

    /* A especificacao manda procurar %PDF- nos primeiros 1024 bytes. */
    long long at = pdf_find(&doc->st, 0, 1024, "%PDF-");
    if (at < 0)
        return;

    doc->hdr_off = at;

    pdf_seek(&doc->st, at + 5);
    int maj = pdf_getc(&doc->st);
    int dot = pdf_getc(&doc->st);
    int min = pdf_getc(&doc->st);

    if (maj >= '0' && maj <= '9' && dot == '.' && min >= '0' && min <= '9') {
        doc->ver_major = maj - '0';
        doc->ver_minor = min - '0';
    }
}

/* ------------------------------------------------------------------------- */
/* Tabela de xref                                                             */

static int xref_ensure(PdfDoc *doc, int size)
{
    if (size <= doc->xref_len)
        return 0;
    if (size <= 0 || size > MAX_OBJNUM) {
        set_err(doc, "/Size absurdo no trailer");
        return -1;
    }

    PdfXrefEnt *n = (PdfXrefEnt *)calloc((size_t)size, sizeof(PdfXrefEnt));
    if (!n) {
        set_err(doc, "sem memoria para o xref");
        return -1;
    }
    if (doc->xref && doc->xref_len > 0)
        memcpy(n, doc->xref, (size_t)doc->xref_len * sizeof(PdfXrefEnt));
    free(doc->xref);
    doc->xref = n;
    doc->xref_len = size;
    return 0;
}

/*
 * Registra uma entrada apenas se o slot ainda estiver vazio.
 *
 * A cadeia e percorrida do xref MAIS NOVO para o mais antigo (startxref e
 * depois /Prev), entao "quem chega primeiro vence" e exatamente a semantica de
 * atualizacao incremental do PDF: a versao mais recente de um objeto sobrepoe
 * as anteriores. O arquivo "A Ilha do Tesouro" do corpus tem /Prev, ou seja,
 * essa regra decide qual versao de cada objeto e lida.
 */
static void xref_set(PdfDoc *doc, int num, unsigned char type,
                     long long off, int idx)
{
    if (num < 0 || num >= doc->xref_len)
        return;
    if (doc->xref[num].type != 0)
        return;
    doc->xref[num].type = type;
    doc->xref[num].off  = off;
    doc->xref[num].idx  = idx;
}

static void merge_trailer(PdfDoc *doc, PdfObj *t)
{
    if (!pdf_is(t, PDF_DICT))
        return;
    if (!doc->trailer) {
        doc->trailer = t;
        return;
    }
    /* Trailers mais antigos preenchem apenas as chaves que faltam. */
    for (int i = 0; i < t->u.d.len; ++i) {
        PdfSlice k = t->u.d.keys[i];
        char name[64];
        int n = k.len < (int)sizeof(name) - 1 ? k.len : (int)sizeof(name) - 1;
        memcpy(name, k.p, (size_t)n);
        name[n] = '\0';
        if (!pdf_dict_get(doc->trailer, name))
            pdf_dict_put(&doc->arena, doc->trailer, k, t->u.d.vals[i]);
    }
}

static int load_xref_at(PdfDoc *doc, long long off, int depth,
                        long long *seen, int *seen_n);

/* Tabela classica: "xref" seguido de subsecoes e de "trailer". */
static int load_xref_classic(PdfDoc *doc, int depth,
                             long long *seen, int *seen_n)
{
    PdfStream *st = &doc->st;
    PdfArena  *a  = &doc->arena;

    for (;;) {
        long long save = pdf_tell(st);
        PdfTok t;
        pdf_lex_next(st, a, &t);

        if (pdf_tok_is(&t, "trailer")) {
            PdfObj *tr = pdf_parse(st, a);
            merge_trailer(doc, tr);

            /* /Size define o tamanho da tabela. Vem no trailer, ou seja, depois
             * das entradas - por isso xref_ensure e chamado tambem durante a
             * leitura das subsecoes. */
            long long size = pdf_int(pdf_dict_get(tr, "Size"), 0);
            if (size > 0)
                xref_ensure(doc, (int)size);

            /*
             * /XRefStm: arquivo de referencia hibrida. Um PDF 1.5 que quer ser
             * lido por leitores antigos traz a tabela classica E um xref stream
             * com os objetos que so existem em ObjStm. Ignorar o /XRefStm faz
             * esses objetos simplesmente desaparecerem.
             */
            PdfObj *xs = pdf_dict_get(tr, "XRefStm");
            if (pdf_is(xs, PDF_INT))
                load_xref_at(doc, pdf_int(xs, 0) + doc->hdr_off, depth + 1,
                             seen, seen_n);

            PdfObj *prev = pdf_dict_get(tr, "Prev");
            if (pdf_is(prev, PDF_INT))
                load_xref_at(doc, pdf_int(prev, 0) + doc->hdr_off, depth + 1,
                             seen, seen_n);
            return 0;
        }

        if (t.kind != PT_INT) {
            /* Nem subsecao nem trailer: fim (ou tabela truncada). */
            pdf_seek(st, save);
            return 0;
        }

        int start = (int)t.i;

        PdfTok t2;
        pdf_lex_next(st, a, &t2);
        if (t2.kind != PT_INT)
            return 0;
        int count = (int)t2.i;

        if (count < 0 || start < 0 || start > MAX_OBJNUM)
            return 0;
        if (start + count > 0 && xref_ensure(doc, start + count) != 0)
            return -1;

        /*
         * Entradas lidas pelo lexer, e nao por offset fixo de 20 bytes.
         *
         * A especificacao fixa 20 bytes por entrada, mas geradores reais
         * escrevem 19 (sem o espaco final) ou usam CR em vez de CRLF. Ler via
         * tokens - INT INT KEYWORD - absorve todas essas variacoes; aritmetica
         * de offset fixo desalinha a tabela inteira na primeira entrada torta.
         */
        for (int i = 0; i < count; ++i) {
            PdfTok o, g, k;
            pdf_lex_next(st, a, &o);
            if (o.kind != PT_INT)
                return 0;
            pdf_lex_next(st, a, &g);
            if (g.kind != PT_INT)
                return 0;
            pdf_lex_next(st, a, &k);

            if (pdf_tok_is(&k, "n"))
                xref_set(doc, start + i, 1, o.i + doc->hdr_off, (int)g.i);
            else if (pdf_tok_is(&k, "f"))
                xref_set(doc, start + i, 0, 0, 0);
            else
                return 0;   /* nem n nem f: tabela corrompida daqui pra frente */
        }
    }
}

/* xref stream (PDF 1.5+): a tabela vive num stream binario. */
static int load_xref_stream(PdfDoc *doc, int depth,
                            long long *seen, int *seen_n)
{
    PdfStream *st = &doc->st;
    PdfArena  *a  = &doc->arena;

    /* Cabecalho do objeto indireto: N G obj */
    PdfTok t1, t2, t3;
    pdf_lex_next(st, a, &t1);
    pdf_lex_next(st, a, &t2);
    pdf_lex_next(st, a, &t3);
    if (t1.kind != PT_INT || t2.kind != PT_INT || !pdf_tok_is(&t3, "obj"))
        return -1;

    PdfObj *s = pdf_parse(st, a);
    if (!pdf_is(s, PDF_STREAM))
        return -1;

    const PdfObj *d = pdf_as_dict(s);

    long long size = pdf_int(pdf_dict_get(d, "Size"), 0);
    if (size > 0 && xref_ensure(doc, (int)size) != 0)
        return -1;

    PdfObj *w = pdf_dict_get(d, "W");
    if (!pdf_is(w, PDF_ARR) || pdf_arr_len(w) < 3)
        return -1;

    int wid[8];
    int nw = pdf_arr_len(w);
    if (nw > 8)
        nw = 8;
    int rowlen = 0;
    for (int i = 0; i < nw; ++i) {
        wid[i] = (int)pdf_int(pdf_arr_get(w, i), 0);
        if (wid[i] < 0 || wid[i] > 8)
            return -1;
        rowlen += wid[i];
    }
    if (rowlen <= 0)
        return -1;

    unsigned char *data = NULL;
    int datalen = 0;
    if (pdf_doc_stream_data(doc, a, s, &data, &datalen) != PDF_FILT_OK)
        return -1;

    /* /Index e uma lista de pares [primeiro quantidade ...]; ausente significa
     * [0 Size]. */
    PdfObj *index = pdf_dict_get(d, "Index");
    int pairs = pdf_is(index, PDF_ARR) ? pdf_arr_len(index) / 2 : 1;

    int pos = 0;
    for (int p = 0; p < pairs; ++p) {
        int first, cnt;
        if (pdf_is(index, PDF_ARR)) {
            first = (int)pdf_int(pdf_arr_get(index, p * 2), 0);
            cnt   = (int)pdf_int(pdf_arr_get(index, p * 2 + 1), 0);
        } else {
            first = 0;
            cnt   = (int)size;
        }
        if (first < 0 || cnt < 0)
            break;
        if (first + cnt > 0 && xref_ensure(doc, first + cnt) != 0)
            break;

        for (int i = 0; i < cnt; ++i) {
            if (pos + rowlen > datalen)
                break;

            long long f[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
            for (int k = 0; k < nw; ++k) {
                long long v = 0;
                for (int b = 0; b < wid[k]; ++b)
                    v = (v << 8) | data[pos++];
                f[k] = v;
            }

            /* Largura 0 no campo de tipo significa "todos sao tipo 1". */
            long long type = (wid[0] == 0) ? 1 : f[0];

            if (type == 1)
                xref_set(doc, first + i, 1, f[1] + doc->hdr_off, (int)f[2]);
            else if (type == 2)
                xref_set(doc, first + i, 2, f[1], (int)f[2]);
            else
                xref_set(doc, first + i, 0, 0, 0);
        }
    }

    merge_trailer(doc, (PdfObj *)pdf_as_dict(s));

    PdfObj *prev = pdf_dict_get(d, "Prev");
    if (pdf_is(prev, PDF_INT))
        load_xref_at(doc, pdf_int(prev, 0) + doc->hdr_off, depth + 1,
                     seen, seen_n);
    return 0;
}

static int load_xref_at(PdfDoc *doc, long long off, int depth,
                        long long *seen, int *seen_n)
{
    if (depth > MAX_XREF_DEPTH)
        return -1;
    if (off < 0 || off >= doc->st.size)
        return -1;

    /* Deteccao de ciclo. Uma cadeia /Prev que aponta para tras em circulo
     * existe em arquivo corrompido, e sem essa checagem o app trava lendo a
     * mesma tabela para sempre. */
    for (int i = 0; i < *seen_n; ++i)
        if (seen[i] == off)
            return 0;
    if (*seen_n < MAX_XREF_DEPTH)
        seen[(*seen_n)++] = off;

    pdf_seek(&doc->st, off);
    pdf_skip_ws(&doc->st);

    long long save = pdf_tell(&doc->st);
    PdfTok t;
    pdf_lex_next(&doc->st, &doc->arena, &t);

    if (pdf_tok_is(&t, "xref"))
        return load_xref_classic(doc, depth, seen, seen_n);

    pdf_seek(&doc->st, save);
    return load_xref_stream(doc, depth, seen, seen_n);
}

/* ------------------------------------------------------------------------- */
/* Reconstrucao por varredura                                                 */

/*
 * Ultimo recurso: varre o arquivo inteiro procurando "N G obj".
 *
 * E o que salva arquivo com xref quebrado, e xref quebrado nao e caso
 * excepcional - e o defeito mais comum em PDF que passou por alguma ferramenta
 * que editou bytes sem reescrever a tabela.
 *
 * Uma ocorrencia posterior sobrepoe a anterior de proposito: numa atualizacao
 * incremental, a versao valida de um objeto e a que aparece por ultimo.
 */
static int reconstruct_xref(PdfDoc *doc)
{
    PdfStream *st = &doc->st;
    PdfArena  *a  = &doc->arena;

    if (xref_ensure(doc, 4096) != 0)
        return -1;

    long long at = 0;
    int found = 0;

    for (;;) {
        long long hit = pdf_find(st, at, 0, "obj");
        if (hit < 0)
            break;
        at = hit + 3;

        /* "obj" precisa ser palavra inteira: "endobj" tambem contem "obj". */
        if (hit > 0) {
            pdf_seek(st, hit - 1);
            int before = pdf_getc(st);
            if (!pdf_is_ws(before) && !pdf_is_delim(before))
                continue;
        }
        pdf_seek(st, hit + 3);
        int after = pdf_getc(st);
        if (after >= 0 && !pdf_is_ws(after) && !pdf_is_delim(after))
            continue;

        /* Recua para achar o inicio de "N G". 32 bytes cobrem numeros longos
         * com espacamento folgado. */
        long long back = hit - 32;
        if (back < 0)
            back = 0;

        /* Varre para frente a partir de `back` procurando o par de inteiros que
         * termina exatamente em `hit`. */
        long long best = -1;
        int best_num = -1;

        long long probe = back;
        while (probe < hit) {
            pdf_seek(st, probe);

            /*
             * O inicio do TOKEN, nao o inicio da sondagem.
             *
             * pdf_lex_next pula espacos e COMENTARIOS antes do token, entao a
             * posicao de onde comecamos a sondar pode estar muito antes do
             * numero do objeto. Gravar `probe` como offset registrava o objeto 1
             * no offset 0 - em cima do "%PDF-1.4" - e pdf_doc_get, que
             * (corretamente) recusa offset 0, devolvia nulo. O /Root virava
             * nulo e o documento inteiro parecia nao ter arvore de paginas,
             * sem nenhuma pista apontando para a reconstrucao.
             */
            pdf_skip_ws(st);
            long long tok_start = pdf_tell(st);
            if (tok_start >= hit)
                break;

            PdfTok n1, n2;
            pdf_lex_next(st, a, &n1);
            if (n1.kind == PT_INT) {
                long long after_n1 = pdf_tell(st);
                pdf_lex_next(st, a, &n2);
                if (n2.kind == PT_INT) {
                    pdf_skip_ws(st);
                    if (pdf_tell(st) == hit) {
                        best = tok_start;
                        best_num = (int)n1.i;
                        break;
                    }
                }
                probe = after_n1;
                continue;
            }
            probe = tok_start + 1;
        }

        if (best < 0 || best_num < 0 || best_num > MAX_OBJNUM)
            continue;

        if (best_num >= doc->xref_len) {
            int want = best_num + 1;
            if (want < doc->xref_len * 2)
                want = doc->xref_len * 2;
            if (xref_ensure(doc, want) != 0)
                break;
        }

        /* Sobrescreve incondicionalmente: o ultimo vence. */
        doc->xref[best_num].type = 1;
        doc->xref[best_num].off  = best;
        doc->xref[best_num].idx  = 0;
        found++;
    }

    if (!found)
        return -1;

    doc->reconstructed = 1;

    /* Trailer: procura o ultimo do arquivo. */
    long long tr = pdf_rfind(st, doc->st.size, 0, "trailer");
    while (tr >= 0) {
        pdf_seek(st, tr + 7);
        PdfObj *t = pdf_parse(st, a);
        if (pdf_is(t, PDF_DICT) && pdf_dict_get(t, "Root")) {
            merge_trailer(doc, t);
            break;
        }
        tr = pdf_rfind(st, tr, 0, "trailer");
    }

    /*
     * Sem trailer utilizavel, procuramos o proprio /Catalog entre os objetos.
     * Sem /Root nao ha arvore de paginas, e o documento seria inutil apesar de
     * todos os objetos estarem localizados.
     */
    if (!doc->trailer || !pdf_dict_get(doc->trailer, "Root")) {
        for (int num = 1; num < doc->xref_len; ++num) {
            if (doc->xref[num].type != 1)
                continue;
            PdfArena tmp;
            pdf_arena_init(&tmp, 4096);
            PdfObj *o = pdf_doc_get(doc, &tmp, num);
            int is_cat = pdf_name_is(pdf_dict_get(o, "Type"), "Catalog");
            pdf_arena_free(&tmp);

            if (is_cat) {
                if (!doc->trailer)
                    doc->trailer = pdf_new(a, PDF_DICT);
                PdfObj *ref = pdf_new(a, PDF_REF);
                if (doc->trailer && ref) {
                    ref->u.ref.num = num;
                    ref->u.ref.gen = 0;
                    PdfSlice k;
                    k.p = pdf_arena_dup(a, "Root", 4);
                    k.len = 4;
                    if (k.p)
                        pdf_dict_put(a, doc->trailer, k, ref);
                }
                break;
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Leitura de objetos                                                         */

static PdfObj *get_from_objstm(PdfDoc *doc, PdfArena *a, int num,
                               int stm_num, int idx);

PdfObj *pdf_doc_get(PdfDoc *doc, PdfArena *a, int num)
{
    if (!doc->xref || num < 0 || num >= doc->xref_len)
        return (PdfObj *)pdf_null();

    PdfXrefEnt *e = &doc->xref[num];

    if (e->type == 2)
        return get_from_objstm(doc, a, num, (int)e->off, e->idx);

    if (e->type != 1 || e->off <= 0 || e->off >= doc->st.size)
        return (PdfObj *)pdf_null();

    pdf_seek(&doc->st, e->off);

    PdfTok t1, t2, t3;
    pdf_lex_next(&doc->st, a, &t1);
    pdf_lex_next(&doc->st, a, &t2);
    pdf_lex_next(&doc->st, a, &t3);

    if (t1.kind != PT_INT || !pdf_tok_is(&t3, "obj"))
        return (PdfObj *)pdf_null();

    /*
     * Confere que o numero encontrado e o pedido.
     *
     * Se nao for, o offset do xref esta errado - e ler o objeto de la produz
     * silenciosamente o conteudo ERRADO, que e muito pior que nao ler nada:
     * o documento parece abrir e o texto sai trocado.
     */
    if ((int)t1.i != num)
        return (PdfObj *)pdf_null();

    (void)t2;
    return pdf_parse(&doc->st, a);
}

PdfObj *pdf_doc_resolve(PdfDoc *doc, PdfArena *a, PdfObj *o)
{
    int guard = 0;
    while (pdf_is(o, PDF_REF) && guard++ < MAX_RESOLVE)
        o = pdf_doc_get(doc, a, o->u.ref.num);
    return o ? o : (PdfObj *)pdf_null();
}

PdfObj *pdf_doc_dget(PdfDoc *doc, PdfArena *a, const PdfObj *dict, const char *key)
{
    PdfObj *v = pdf_dict_get(dict, key);
    if (!v)
        return NULL;
    return pdf_doc_resolve(doc, a, v);
}

/* ------------------------------------------------------------------------- */
/* Streams                                                                    */

PdfFiltStatus pdf_doc_stream_data(PdfDoc *doc, PdfArena *a, PdfObj *stream,
                                  unsigned char **out, int *outlen)
{
    *out = NULL;
    *outlen = 0;

    if (!pdf_is(stream, PDF_STREAM))
        return PDF_FILT_ERR_DATA;

    const PdfObj *d = pdf_as_dict(stream);
    long long off = stream->u.stm.off;
    long long len = stream->u.stm.len;

    /*
     * /Length indireto: o caso que nao pode ser resolvido no parser, porque
     * resolver exige o xref, e o xref e lido com o parser. Aqui o xref ja
     * existe, entao da.
     */
    if (len < 0) {
        PdfObj *lo = pdf_dict_get(d, "Length");
        if (pdf_is(lo, PDF_REF)) {
            /* Arena propria: nao poluir a do chamador com o objeto do /Length,
             * que e um inteiro solto sem valor depois disso. */
            PdfArena tmp;
            pdf_arena_init(&tmp, 512);
            PdfObj *rl = pdf_doc_resolve(doc, &tmp, lo);
            long long v = pdf_int(rl, -1);
            pdf_arena_free(&tmp);
            if (v >= 0 && off + v <= doc->st.size)
                len = v;
        }
    }

    if (len < 0) {
        /* Ainda desconhecido: `endstream` decide. */
        long long e = pdf_find(&doc->st, off, 0, "endstream");
        len = (e >= 0) ? (e - off) : (doc->st.size - off);
        if (len < 0)
            len = 0;
    }

    if (off < 0 || off > doc->st.size)
        return PDF_FILT_ERR_DATA;
    if (off + len > doc->st.size)
        len = doc->st.size - off;
    if (len < 0)
        len = 0;

    unsigned char *raw = (unsigned char *)malloc((size_t)len + 1);
    if (!raw)
        return PDF_FILT_ERR_MEM;

    pdf_seek(&doc->st, off);
    int got = pdf_stream_read(&doc->st, raw, (int)len);
    raw[got > 0 ? got : 0] = 0;

    /* /Filter e /DecodeParms podem ser referencias indiretas. */
    PdfObj *filter = pdf_dict_get(d, "Filter");
    PdfObj *parms  = pdf_dict_get(d, "DecodeParms");
    if (!parms)
        parms = pdf_dict_get(d, "DP");

    PdfArena tmp;
    pdf_arena_init(&tmp, 2048);
    if (pdf_is(filter, PDF_REF))
        filter = pdf_doc_resolve(doc, &tmp, filter);
    if (pdf_is(parms, PDF_REF))
        parms = pdf_doc_resolve(doc, &tmp, parms);

    PdfFiltStatus st = pdf_filt_apply(a, filter, parms, raw, got, out, outlen);

    pdf_arena_free(&tmp);
    free(raw);
    return st;
}

int pdf_doc_stream_is_image(PdfDoc *doc, PdfArena *a, PdfObj *stream)
{
    if (!pdf_is(stream, PDF_STREAM))
        return 0;
    PdfObj *filter = pdf_doc_dget(doc, a, pdf_as_dict(stream), "Filter");
    if (!filter)
        return 0;
    if (filter->kind == PDF_ARR) {
        for (int i = 0; i < pdf_arr_len(filter); ++i)
            if (pdf_filt_is_image(pdf_arr_get(filter, i)))
                return 1;
        return 0;
    }
    return pdf_filt_is_image(filter);
}

/* ------------------------------------------------------------------------- */
/* Object streams (tipo 2)                                                    */

static int load_objstm(PdfDoc *doc, int stm_num)
{
    if (doc->objstm_num == stm_num && doc->objstm_data)
        return 0;

    PdfArena tmp;
    pdf_arena_init(&tmp, 8192);

    PdfObj *s = pdf_doc_get(doc, &tmp, stm_num);
    if (!pdf_is(s, PDF_STREAM)) {
        pdf_arena_free(&tmp);
        return -1;
    }

    unsigned char *data = NULL;
    int len = 0;
    if (pdf_doc_stream_data(doc, &tmp, s, &data, &len) != PDF_FILT_OK) {
        pdf_arena_free(&tmp);
        return -1;
    }

    /* Copia para fora da arena temporaria: o cache tem de sobreviver a ela. */
    unsigned char *keep = (unsigned char *)malloc((size_t)len + 1);
    if (!keep) {
        pdf_arena_free(&tmp);
        return -1;
    }
    memcpy(keep, data, (size_t)len);
    keep[len] = 0;

    free(doc->objstm_data);
    doc->objstm_data = keep;
    doc->objstm_len  = len;
    doc->objstm_num  = stm_num;

    pdf_arena_free(&tmp);
    return 0;
}

/*
 * Um ObjStm guarda vários objetos concatenados, precedidos por uma tabela de
 * pares (numero, offset) de /N entradas. /First e onde os dados comecam.
 */
static PdfObj *get_from_objstm(PdfDoc *doc, PdfArena *a, int num,
                               int stm_num, int idx)
{
    if (load_objstm(doc, stm_num) != 0)
        return (PdfObj *)pdf_null();

    PdfArena tmp;
    pdf_arena_init(&tmp, 4096);
    PdfObj *s = pdf_doc_get(doc, &tmp, stm_num);
    int n     = (int)pdf_int(pdf_doc_dget(doc, &tmp, pdf_as_dict(s), "N"), 0);
    int first = (int)pdf_int(pdf_doc_dget(doc, &tmp, pdf_as_dict(s), "First"), 0);
    pdf_arena_free(&tmp);

    if (n <= 0 || first < 0 || first > doc->objstm_len)
        return (PdfObj *)pdf_null();

    /*
     * Le a tabela de pares com um PdfStream sobre memoria.
     *
     * Reaproveitar o lexer em vez de escrever um parser de inteiros a parte
     * evita duplicar as regras de espacamento do PDF - e a tabela de um ObjStm
     * segue as mesmas regras do resto do arquivo.
     */
    PdfMemCtx  mc;
    PdfIo      io;
    PdfStream  ms;

    pdf_io_mem(&io, &mc, doc->objstm_data, doc->objstm_len);
    if (pdf_stream_init(&ms, &io) != 0)
        return (PdfObj *)pdf_null();

    int want_off = -1;

    {
        PdfArena t2;
        pdf_arena_init(&t2, 2048);

        for (int i = 0; i < n; ++i) {
            PdfTok a1, a2;
            pdf_lex_next(&ms, &t2, &a1);
            pdf_lex_next(&ms, &t2, &a2);
            if (a1.kind != PT_INT || a2.kind != PT_INT)
                break;
            /* Casa pelo NUMERO do objeto, nao apenas pelo indice: o /Index do
             * xref stream pode estar errado, e conferir o numero e o que
             * garante que o objeto devolvido e o pedido - em vez de devolver
             * silenciosamente o objeto vizinho. */
            if ((int)a1.i == num) {
                want_off = first + (int)a2.i;
                break;
            }
            if (i == idx && want_off < 0)
                want_off = first + (int)a2.i;   /* plano B pelo indice */
        }
        pdf_arena_free(&t2);
    }

    if (want_off < 0 || want_off >= doc->objstm_len)
        return (PdfObj *)pdf_null();

    /* Mesmo stream, so reposicionado: a tabela e os dados moram no mesmo
     * buffer. */
    pdf_seek(&ms, want_off);
    return pdf_parse(&ms, a);
}

/* ------------------------------------------------------------------------- */
/* Arvore de paginas                                                          */

static int count_pages_walk(PdfDoc *doc, PdfArena *a, PdfObj *node,
                            int depth, int *visited)
{
    if (depth > 64 || ++(*visited) > MAX_PAGETREE)
        return 0;

    PdfObj *type = pdf_doc_dget(doc, a, node, "Type");
    if (pdf_name_is(type, "Page"))
        return 1;

    PdfObj *kids = pdf_doc_dget(doc, a, node, "Kids");
    if (!pdf_is(kids, PDF_ARR)) {
        /* Nem /Page nem /Pages com /Kids. Alguns arquivos omitem /Type nas
         * folhas; se tem /Contents, e pagina. */
        if (pdf_dict_get(node, "Contents"))
            return 1;
        return 0;
    }

    int total = 0;
    for (int i = 0; i < pdf_arr_len(kids); ++i) {
        PdfArena sub;
        pdf_arena_init(&sub, 4096);
        PdfObj *kid = pdf_doc_resolve(doc, &sub, pdf_arr_get(kids, i));
        total += count_pages_walk(doc, &sub, kid, depth + 1, visited);
        pdf_arena_free(&sub);
    }
    return total;
}

static void compute_pages(PdfDoc *doc)
{
    doc->pages = 0;

    PdfArena a;
    pdf_arena_init(&a, 8192);

    PdfObj *root = pdf_doc_dget(doc, &a, doc->trailer, "Root");
    PdfObj *pages = pdf_doc_dget(doc, &a, root, "Pages");

    /*
     * /Count primeiro, porque e O(1) e correto na esmagadora maioria dos
     * arquivos. A varredura da arvore e o plano B: /Count errado ou ausente
     * existe, e nesse caso contar as folhas e a unica resposta confiavel.
     */
    long long c = pdf_int(pdf_doc_dget(doc, &a, pages, "Count"), 0);
    if (c > 0 && c < MAX_PAGETREE) {
        doc->pages = (int)c;
    } else if (pages) {
        int visited = 0;
        doc->pages = count_pages_walk(doc, &a, pages, 0, &visited);
    }

    if (doc->pages == 0)
        set_err(doc, "arvore de paginas nao encontrada");

    pdf_arena_free(&a);
}

/* ------------------------------------------------------------------------- */
/* Acesso a pagina por indice                                                 */

static PdfObj *page_at(PdfDoc *doc, PdfArena *a, PdfObj *node,
                       int want, int *seen, int depth)
{
    if (depth > 64)
        return NULL;

    PdfObj *kids = pdf_doc_dget(doc, a, node, "Kids");
    if (!pdf_is(kids, PDF_ARR)) {
        /* Folha. */
        if (*seen == want)
            return node;
        (*seen)++;
        return NULL;
    }

    for (int i = 0; i < pdf_arr_len(kids); ++i) {
        PdfObj *kid = pdf_doc_resolve(doc, a, pdf_arr_get(kids, i));
        if (!kid)
            continue;

        /*
         * Salto por /Count: se a pagina procurada nao esta nesta subarvore,
         * pula sem descer. E o que faz o acesso ser proporcional a profundidade
         * em vez de ao numero de paginas.
         *
         * So confiamos no /Count quando ele e positivo E o no tem /Kids - um
         * /Count mentiroso faria pular paginas que existem, e o sintoma seria
         * "o livro tem 500 paginas mas a 300 mostra a 280".
         */
        PdfObj *ktype  = pdf_doc_dget(doc, a, kid, "Type");
        PdfObj *kkids  = pdf_dict_get(kid, "Kids");
        if (kkids && !pdf_name_is(ktype, "Page")) {
            long long cnt = pdf_int(pdf_doc_dget(doc, a, kid, "Count"), -1);
            if (cnt > 0 && *seen + cnt <= want) {
                *seen += (int)cnt;
                continue;
            }
        }

        PdfObj *found = page_at(doc, a, kid, want, seen, depth + 1);
        if (found)
            return found;
    }
    return NULL;
}

PdfObj *pdf_doc_page(PdfDoc *doc, PdfArena *a, int index)
{
    if (index < 0)
        return NULL;

    PdfObj *root  = pdf_doc_dget(doc, a, doc->trailer, "Root");
    PdfObj *pages = pdf_doc_dget(doc, a, root, "Pages");
    if (!pages)
        return NULL;

    int seen = 0;
    return page_at(doc, a, pages, index, &seen, 0);
}

PdfFiltStatus pdf_doc_page_content(PdfDoc *doc, PdfArena *a, PdfObj *page,
                                   unsigned char **out, int *outlen)
{
    *out = NULL;
    *outlen = 0;

    PdfObj *contents = pdf_doc_dget(doc, a, page, "Contents");
    if (!contents || contents->kind == PDF_NULL)
        return PDF_FILT_ERR_DATA;

    if (!pdf_is(contents, PDF_ARR))
        return pdf_doc_stream_data(doc, a, contents, out, outlen);

    int n = pdf_arr_len(contents);

    /* Duas passadas: soma os tamanhos, depois concatena. Evita um buffer
     * crescente e deixa o resultado numa unica alocacao de arena. */
    unsigned char **parts = (unsigned char **)pdf_arena_alloc(a,
                                sizeof(unsigned char *) * (size_t)(n > 0 ? n : 1));
    int *lens = (int *)pdf_arena_alloc(a, sizeof(int) * (size_t)(n > 0 ? n : 1));
    if (!parts || !lens)
        return PDF_FILT_ERR_MEM;

    int total = 0;
    for (int i = 0; i < n; ++i) {
        parts[i] = NULL;
        lens[i]  = 0;
        PdfObj *cs = pdf_doc_resolve(doc, a, pdf_arr_get(contents, i));
        if (pdf_doc_stream_data(doc, a, cs, &parts[i], &lens[i]) != PDF_FILT_OK) {
            parts[i] = NULL;
            lens[i]  = 0;
            continue;   /* um pedaco ilegivel nao invalida os outros */
        }
        total += lens[i] + 1;   /* +1 para o separador */
    }

    if (total <= 0)
        return PDF_FILT_ERR_DATA;

    unsigned char *buf = (unsigned char *)pdf_arena_alloc(a, (size_t)total + 1);
    if (!buf)
        return PDF_FILT_ERR_MEM;

    int at = 0;
    for (int i = 0; i < n; ++i) {
        if (!parts[i] || lens[i] <= 0)
            continue;
        memcpy(buf + at, parts[i], (size_t)lens[i]);
        at += lens[i];
        /*
         * Separador de espaco entre pedacos.
         *
         * A especificacao diz que os pedacos formam um fluxo continuo, mas
         * colar sem separador funde o ultimo token de um com o primeiro do
         * seguinte: "...BT" + "/F1..." viraria "BT/F1", e um "Q" final colado
         * num "q" inicial viraria a palavra "Qq", que nao e operador nenhum.
         */
        buf[at++] = '\n';
    }
    buf[at] = 0;

    *out = buf;
    *outlen = at;
    return PDF_FILT_OK;
}

/* ------------------------------------------------------------------------- */

int pdf_doc_open(PdfDoc *doc, const PdfIo *io)
{
    memset(doc, 0, sizeof(*doc));
    doc->objstm_num = -1;
    pdf_arena_init(&doc->arena, 32768);

    if (pdf_stream_init(&doc->st, io) != 0) {
        set_err(doc, "arquivo vazio ou ilegivel");
        return -1;
    }

    read_header(doc);
    if (doc->ver_major == 0) {
        /* Sem %PDF- nos primeiros 1024 bytes. Ainda tentamos: alguns arquivos
         * tem cabecalho corrompido mas corpo intacto. */
        set_err(doc, "cabecalho %PDF- nao encontrado");
    }

    /* startxref fica no fim. 2048 bytes cobrem com folga o rodape legitimo. */
    long long sx = pdf_rfind(&doc->st, doc->st.size, 2048, "startxref");
    int ok = -1;

    if (sx >= 0) {
        pdf_seek(&doc->st, sx + 9);
        PdfTok t;
        pdf_lex_next(&doc->st, &doc->arena, &t);
        if (t.kind == PT_INT) {
            long long seen[MAX_XREF_DEPTH];
            int seen_n = 0;
            ok = load_xref_at(doc, t.i + doc->hdr_off, 0, seen, &seen_n);
        }
    }

    /* Sem xref utilizavel, ou xref sem /Root: reconstroi. */
    if (ok != 0 || !doc->trailer || !pdf_dict_get(doc->trailer, "Root")) {
        doc->err[0] = '\0';        /* a reconstrucao pode resolver */
        if (reconstruct_xref(doc) != 0) {
            set_err(doc, "xref ilegivel e reconstrucao falhou");
            return -1;
        }
    }

    doc->encrypted = pdf_dict_get(doc->trailer, "Encrypt") ? 1 : 0;

    compute_pages(doc);

    /*
     * Documento criptografado nao e erro de abertura: o cabecalho, a contagem de
     * paginas e os metadados costumam ser legiveis, e informar "criptografado"
     * na tela e mais util que "falhou".
     */
    if (doc->pages == 0 && !doc->encrypted) {
        set_err(doc, "nenhuma pagina encontrada");
        return -1;
    }
    return 0;
}

void pdf_doc_close(PdfDoc *doc)
{
    pdf_stream_close(&doc->st);
    pdf_arena_free(&doc->arena);
    free(doc->xref);
    free(doc->objstm_data);
    doc->xref = NULL;
    doc->objstm_data = NULL;
    doc->xref_len = 0;
    doc->trailer = NULL;
}
