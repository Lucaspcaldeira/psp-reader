/*
 * Teste de integracao: abre um PDF de verdade e relata o que a camada
 * conseguiu entender.
 *
 *   .\test.ps1 pdf "F:\PSP\BOOKS\livro.pdf"
 *
 * Nao e um teste com resposta esperada fixa - e o instrumento que diz se um
 * arquivo real e legivel e por que nao, o que e exatamente o que falta saber
 * antes de escrever o extrator de texto da Etapa 3b.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pdf_doc.h"
#include "pdf_lex.h"
#include "host_io.h"

static const char *kindname(int k)
{
    switch (k) {
    case PDF_NULL:   return "null";
    case PDF_BOOL:   return "bool";
    case PDF_INT:    return "int";
    case PDF_REAL:   return "real";
    case PDF_STR:    return "string";
    case PDF_NAME:   return "name";
    case PDF_ARR:    return "array";
    case PDF_DICT:   return "dict";
    case PDF_STREAM: return "stream";
    case PDF_REF:    return "ref";
    }
    return "?";
}

/* Percorre a arvore de paginas e chama cb em cada folha. */
typedef void (*PageFn)(PdfDoc *, PdfArena *, PdfObj *page, int index, void *ud);

static void walk_pages(PdfDoc *doc, PdfArena *a, PdfObj *node,
                       int depth, int *idx, PageFn cb, void *ud)
{
    if (depth > 64)
        return;

    PdfObj *kids = pdf_doc_dget(doc, a, node, "Kids");
    if (!pdf_is(kids, PDF_ARR)) {
        cb(doc, a, node, (*idx)++, ud);
        return;
    }
    for (int i = 0; i < pdf_arr_len(kids); ++i) {
        PdfArena sub;
        pdf_arena_init(&sub, 8192);
        PdfObj *kid = pdf_doc_resolve(doc, &sub, pdf_arr_get(kids, i));
        walk_pages(doc, &sub, kid, depth + 1, idx, cb, ud);
        pdf_arena_free(&sub);
    }
}

typedef struct {
    int pages_seen;
    int pages_with_contents;
    int content_ok;
    int content_image;
    int content_err;
    long long content_bytes;
    int fonts_type1, fonts_truetype, fonts_type0, fonts_other;
    int with_tounicode;
    int max_report;

    /* O sinal que realmente importa: paginas que contem operador de exibicao de
     * texto, contra paginas que so desenham imagem. */
    int pages_with_text;
    int pages_image_only;
    long long text_ops;
} Stats;

/*
 * Conta operadores de exibicao de texto num content stream ja descomprimido.
 *
 * Feito com o LEXER, e nao com strstr, por duas razoes. A correta: "TJ" pode
 * aparecer dentro de uma string literal - "(TJ)Tj" tem uma ocorrencia real e
 * uma falsa, e strstr nao distingue. A util: e o primeiro exercicio do lexer
 * sobre content stream, que e exatamente a gramatica da Etapa 3b.
 */
static int count_text_ops(const unsigned char *data, int len)
{
    PdfMemCtx mc;
    PdfIo     io;
    PdfStream st;
    PdfArena  a;

    pdf_io_mem(&io, &mc, data, len);
    if (pdf_stream_init(&st, &io) != 0)
        return 0;
    pdf_arena_init(&a, 8192);

    int n = 0;
    for (;;) {
        PdfTok t;
        pdf_lex_next(&st, &a, &t);
        if (t.kind == PT_EOF)
            break;
        if (t.kind == PT_KEYWORD &&
            (pdf_tok_is(&t, "Tj") || pdf_tok_is(&t, "TJ") ||
             pdf_tok_is(&t, "'")  || pdf_tok_is(&t, "\"")))
            n++;

        /* A arena acumula um nome/palavra por token; reciclar de vez em quando
         * mantem o uso constante mesmo num stream de centenas de KB. */
        if (a.handed > 256u * 1024u)
            pdf_arena_reset(&a);
    }
    pdf_arena_free(&a);
    return n;
}

static void count_fonts(PdfDoc *doc, PdfArena *a, PdfObj *page, Stats *s)
{
    PdfObj *res = pdf_doc_dget(doc, a, page, "Resources");
    PdfObj *fonts = pdf_doc_dget(doc, a, res, "Font");
    if (!pdf_is(fonts, PDF_DICT))
        return;

    for (int i = 0; i < fonts->u.d.len; ++i) {
        PdfArena sub;
        pdf_arena_init(&sub, 4096);
        PdfObj *f = pdf_doc_resolve(doc, &sub, fonts->u.d.vals[i]);
        PdfObj *sub_t = pdf_doc_dget(doc, &sub, f, "Subtype");

        if (pdf_name_is(sub_t, "Type1") || pdf_name_is(sub_t, "MMType1"))
            s->fonts_type1++;
        else if (pdf_name_is(sub_t, "TrueType"))
            s->fonts_truetype++;
        else if (pdf_name_is(sub_t, "Type0"))
            s->fonts_type0++;
        else
            s->fonts_other++;

        if (pdf_dict_get(f, "ToUnicode"))
            s->with_tounicode++;

        pdf_arena_free(&sub);
    }
}

static void on_page(PdfDoc *doc, PdfArena *a, PdfObj *page, int index, void *ud)
{
    Stats *s = (Stats *)ud;
    s->pages_seen++;

    count_fonts(doc, a, page, s);

    PdfObj *contents = pdf_doc_dget(doc, a, page, "Contents");
    if (!contents || contents->kind == PDF_NULL)
        return;
    s->pages_with_contents++;

    /* /Contents pode ser um stream ou um array de streams que se concatenam. */
    int n = pdf_is(contents, PDF_ARR) ? pdf_arr_len(contents) : 1;
    int page_text_ops = 0;

    for (int i = 0; i < n; ++i) {
        PdfArena sub;
        pdf_arena_init(&sub, 16384);

        PdfObj *cs = pdf_is(contents, PDF_ARR)
                   ? pdf_doc_resolve(doc, &sub, pdf_arr_get(contents, i))
                   : contents;

        unsigned char *data = NULL;
        int len = 0;
        PdfFiltStatus st = pdf_doc_stream_data(doc, &sub, cs, &data, &len);

        if (st == PDF_FILT_OK) {
            s->content_ok++;
            s->content_bytes += len;
            page_text_ops += count_text_ops(data, len);

            /* As primeiras paginas com conteudo saem na tela: e a checagem
             * visual de que os operadores de texto realmente estao la, e nao
             * um stream vazio que "decodificou com sucesso". */
            if (s->max_report > 0) {
                s->max_report--;
                printf("\n  --- pagina %d, content stream de %d bytes ---\n",
                       index + 1, len);
                int show = len < 320 ? len : 320;
                fputs("  ", stdout);
                for (int k = 0; k < show; ++k) {
                    int c = data[k];
                    if (c == '\n')      fputs("\n  ", stdout);
                    else if (c >= 32 && c < 127) putchar(c);
                    else                putchar('.');
                }
                printf("\n  --- fim do recorte ---\n");
            }
        } else if (st == PDF_FILT_ERR_IMAGE) {
            s->content_image++;
        } else {
            s->content_err++;
            if (s->max_report > 0) {
                s->max_report--;
                printf("  pagina %d: content stream falhou: %s\n",
                       index + 1, pdf_filt_status_str(st));
            }
        }
        pdf_arena_free(&sub);
    }

    if (page_text_ops > 0) {
        s->pages_with_text++;
        s->text_ops += page_text_ops;
    } else {
        s->pages_image_only++;
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("uso: test_pdf <arquivo.pdf>\n");
        return 2;
    }

    PdfIo io;
    if (host_io_open(&io, argv[1]) != 0) {
        printf("nao abriu: %s\n", argv[1]);
        return 1;
    }

    PdfDoc doc;
    int rc = pdf_doc_open(&doc, &io);

    printf("arquivo : %s\n", argv[1]);
    printf("tamanho : %lld bytes\n", doc.st.size);
    printf("versao  : PDF-%d.%d\n", doc.ver_major, doc.ver_minor);
    if (doc.hdr_off != 0)
        printf("AVISO   : %%PDF- em offset %lld, nao 0\n", doc.hdr_off);
    printf("xref    : %d entradas%s\n", doc.xref_len,
           doc.reconstructed ? "  (RECONSTRUIDO por varredura)" : "");
    printf("cripto  : %s\n", doc.encrypted ? "SIM" : "nao");
    printf("paginas : %d\n", doc.pages);
    printf("abertura: %s%s%s\n", rc == 0 ? "ok" : "FALHOU",
           doc.err[0] ? " - " : "", doc.err);

    if (rc != 0) {
        pdf_doc_close(&doc);
        return 1;
    }

    /* Chaves do trailer: mostra se /Root e /Info foram localizados. */
    if (pdf_is(doc.trailer, PDF_DICT)) {
        printf("trailer :");
        for (int i = 0; i < doc.trailer->u.d.len; ++i)
            printf(" /%.*s(%s)",
                   doc.trailer->u.d.keys[i].len, doc.trailer->u.d.keys[i].p,
                   kindname(doc.trailer->u.d.vals[i]->kind));
        printf("\n");
    }

    /* Metadados de /Info, se houver. */
    {
        PdfArena a;
        pdf_arena_init(&a, 8192);
        PdfObj *info = pdf_doc_dget(&doc, &a, doc.trailer, "Info");
        const char *keys[] = { "Title", "Author", "Producer", "Creator", NULL };
        for (int i = 0; keys[i]; ++i) {
            PdfObj *v = pdf_doc_dget(&doc, &a, info, keys[i]);
            if (pdf_is(v, PDF_STR) && v->u.s.len > 0) {
                /* Strings de PDF podem estar em UTF-16BE com BOM FE FF. Aqui
                 * so mostramos os bytes imprimiveis - a decodificacao correta e
                 * problema da Etapa 3b. */
                printf("%-8s: ", keys[i]);
                for (int k = 0; k < v->u.s.len && k < 70; ++k) {
                    unsigned char c = (unsigned char)v->u.s.p[k];
                    putchar((c >= 32 && c < 127) ? c : '.');
                }
                printf("\n");
            }
        }
        pdf_arena_free(&a);
    }

    /* Varre a arvore de paginas de verdade, decodificando os content streams. */
    Stats s;
    memset(&s, 0, sizeof(s));
    s.max_report = 1;

    {
        PdfArena a;
        pdf_arena_init(&a, 16384);
        PdfObj *root  = pdf_doc_dget(&doc, &a, doc.trailer, "Root");
        PdfObj *pages = pdf_doc_dget(&doc, &a, root, "Pages");
        int idx = 0;
        if (pages)
            walk_pages(&doc, &a, pages, 0, &idx, on_page, &s);
        pdf_arena_free(&a);
    }

    printf("\n== varredura da arvore de paginas ==\n");
    printf("folhas visitadas   : %d\n", s.pages_seen);
    printf("com /Contents      : %d\n", s.pages_with_contents);
    printf("streams ok         : %d  (%lld bytes descomprimidos)\n",
           s.content_ok, s.content_bytes);
    printf("streams de imagem  : %d\n", s.content_image);
    printf("streams com erro   : %d\n", s.content_err);
    printf("fontes Type1/TT/T0 : %d / %d / %d  (outras: %d)\n",
           s.fonts_type1, s.fonts_truetype, s.fonts_type0, s.fonts_other);
    printf("fontes c/ToUnicode : %d\n", s.with_tounicode);
    printf("paginas com texto  : %d  (%lld operadores Tj/TJ)\n",
           s.pages_with_text, s.text_ops);
    printf("paginas so imagem  : %d\n", s.pages_image_only);
    printf("leituras fisicas   : %lld janelas de %d bytes\n",
           doc.st.reads, PDF_WINDOW);

    /*
     * O veredito.
     *
     * Contar filtro de imagem NAO responde "tem camada de texto": uma pagina
     * escaneada tem content stream Flate perfeitamente valido cujo unico
     * conteudo e "desenhe esta imagem". O sinal correto e a presenca de
     * operador de exibicao de texto, e por isso ele e medido com o lexer.
     */
    printf("\nveredito: ");
    if (s.content_ok == 0)
        printf("NENHUM content stream decodificou - arquivo ilegivel\n");
    else if (s.content_err > s.content_ok)
        printf("maioria dos streams falhou - arquivo provavelmente danificado\n");
    else if (s.pages_with_text == 0)
        printf("SEM CAMADA DE TEXTO - %d paginas, todas apenas imagem "
               "(escaneado)\n", s.pages_seen);
    else
        printf("%d de %d paginas tem texto extraivel\n",
               s.pages_with_text, s.pages_seen);

    int bad = (doc.pages <= 0) || (s.pages_seen != doc.pages);
    if (s.pages_seen != doc.pages)
        printf("\nATENCAO: /Count diz %d paginas, a varredura achou %d folhas\n",
               doc.pages, s.pages_seen);

    pdf_doc_close(&doc);
    return bad ? 1 : 0;
}
