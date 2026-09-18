#include <string.h>
#include <stdio.h>

#include "doc.h"
#include "pdf_text.h"
#include "textlines.h"
#include "utf8.h"

const char *doc_kind_name(DocKind k)
{
    switch (k) {
    case DOC_PDF:  return "PDF";
    case DOC_TXT:  return "TXT";
    case DOC_EPUB: return "EPUB";
    default:      return "?";
    }
}

/* ------------------------------------------------------------------------- */
/* Metadados                                                                  */

/*
 * String de PDF -> UTF-8.
 *
 * Duas codificacoes possiveis e nenhuma declarada por campo: com BOM FE FF e
 * UTF-16BE, sem BOM e PDFDocEncoding, que para o alcance de um titulo de livro
 * coincide com Latin-1. Tratar tudo como bytes crus poria o titulo com meio
 * caractere a cada acento.
 */
static void pdf_str_to_utf8(const PdfObj *s, char *out, int out_size)
{
    out[0] = '\0';
    if (!pdf_is(s, PDF_STR) || s->u.s.len <= 0)
        return;

    const unsigned char *p = (const unsigned char *)s->u.s.p;
    int len = s->u.s.len;
    int o = 0;

    if (len >= 2 && p[0] == 0xFE && p[1] == 0xFF) {
        /* UTF-16BE. Surrogates sao ignorados: nao aparecem em titulo de livro,
         * e emitir metade de um par produziria bytes invalidos. */
        for (int i = 2; i + 1 < len && o < out_size - 5; i += 2) {
            unsigned int cp = ((unsigned int)p[i] << 8) | p[i + 1];
            if (cp >= 0xD800 && cp <= 0xDFFF)
                continue;
            char tmp[4];
            int n = utf8_encode(cp, tmp);
            for (int k = 0; k < n && o < out_size - 1; ++k)
                out[o++] = tmp[k];
        }
    } else {
        for (int i = 0; i < len && o < out_size - 3; ++i) {
            char tmp[4];
            int n = utf8_encode(p[i], tmp);   /* Latin-1 -> UTF-8 */
            for (int k = 0; k < n && o < out_size - 1; ++k)
                out[o++] = tmp[k];
        }
    }
    out[o] = '\0';
}

/* ------------------------------------------------------------------------- */

int doc_open(Doc *d, PdfArena *a, const PdfIo *io, DocKind kind)
{
    memset(d, 0, sizeof(*d));
    d->kind = kind;

    if (kind == DOC_PDF) {
        int rc = pdf_doc_open(&d->pdf, io);
        if (rc != 0) {
            /*
             * `kind` continua DOC_PDF de proposito, mesmo tendo falhado.
             *
             * pdf_doc_open assume a posse do descritor logo na primeira linha,
             * antes de qualquer coisa poder dar errado, e exige pdf_doc_close()
             * tambem no caminho de erro. Zerar `kind` aqui faria doc_close()
             * nao ter o que fechar, e cada livro que falhasse ao abrir vazaria
             * um descritor - ate o console ficar sem nenhum.
             */
            snprintf(d->err, sizeof(d->err), "%s", d->pdf.err);
            d->units = 0;
            return rc;
        }
        d->units = d->pdf.pages;

        PdfArena tmp;
        pdf_arena_init(&tmp, 16384);
        PdfObj *info = pdf_doc_dget(&d->pdf, &tmp, d->pdf.trailer, "Info");
        pdf_str_to_utf8(pdf_doc_dget(&d->pdf, &tmp, info, "Title"),
                        d->title, sizeof(d->title));
        pdf_str_to_utf8(pdf_doc_dget(&d->pdf, &tmp, info, "Author"),
                        d->author, sizeof(d->author));
        pdf_arena_free(&tmp);
        return 0;
    }

    if (kind == DOC_TXT) {
        /* Arquivo vazio e o unico modo de falha realista aqui, e vale dize-lo:
         * "nao foi possivel indexar" mandaria o usuario procurar um defeito no
         * leitor quando o defeito esta no arquivo. */
        long long sz = io->size ? io->size(io->ctx) : 0;
        if (sz <= 0) {
            snprintf(d->err, sizeof(d->err), "arquivo vazio");
            if (io->close)
                io->close(io->ctx);
            d->kind = DOC_NONE;
            return -1;
        }
        if (txt_open(&d->txt, a, io) != 0) {
            snprintf(d->err, sizeof(d->err), "nao foi possivel indexar o texto");
            d->units = 0;
            return -1;
        }
        d->units = d->txt.nunits;
        return 0;
    }

    if (kind == DOC_EPUB) {
        if (epub_open(&d->epub, a, io) != 0) {
            snprintf(d->err, sizeof(d->err), "%s", d->epub.err);
            /* Como no PDF: zip_open assume a posse do descritor antes de
             * poder falhar, entao doc_close continua sendo obrigatorio. */
            d->units = 0;
            return -1;
        }
        d->units = d->epub.nunits;
        snprintf(d->title, sizeof(d->title), "%s", d->epub.title);
        snprintf(d->author, sizeof(d->author), "%s", d->epub.author);
        return 0;
    }

    snprintf(d->err, sizeof(d->err), "formato nao suportado");
    d->kind = DOC_NONE;
    return -1;
}

void doc_close(Doc *d)
{
    switch (d->kind) {
    case DOC_PDF:  pdf_doc_close(&d->pdf);   break;
    case DOC_TXT:  txt_close(&d->txt);       break;
    case DOC_EPUB: epub_close(&d->epub);     break;
    default: break;
    }
    memset(d, 0, sizeof(*d));
}

int doc_unit(Doc *d, PdfArena *a, int unit, Reflow *out, DocStats *st)
{
    memset(out, 0, sizeof(*out));
    out->columns = 1;
    if (st)
        memset(st, 0, sizeof(*st));

    if (unit < 0 || unit >= d->units)
        return -1;

    if (d->kind == DOC_TXT) {
        int rc = txt_unit(&d->txt, a, unit, out);
        if (st)
            st->lines = out->lines_in;
        return rc;
    }

    if (d->kind == DOC_EPUB) {
        int rc = epub_unit(&d->epub, a, unit, out);
        if (st)
            st->lines = out->lines_in;
        return rc;
    }

    if (d->kind != DOC_PDF)
        return -1;

    /*
     * O caminho do PDF, em tres estagios que so aqui aparecem juntos:
     * content stream -> runs posicionados -> linhas -> paragrafos. Cada um vive
     * num modulo proprio porque cada um falha de um jeito diferente, e o
     * diagnostico precisa saber em qual deles a pagina se perdeu.
     */
    PdfObj *page = pdf_doc_page(&d->pdf, a, unit);
    if (!page)
        return -1;

    PdfTextPage tp;
    if (pdf_text_extract(&d->pdf, a, page, &tp) != 0)
        return -1;

    TextLines tl;
    if (textlines_build(a, &tp, &tl) != 0)
        return -1;

    if (st) {
        st->runs           = tp.nruns;
        st->runs_vertical  = tl.runs_vertical;
        st->codes_unmapped = tp.codes_unmapped;
        st->lines          = tl.nlines;
    }

    return reflow_build(a, &tl, NULL, out);
}
