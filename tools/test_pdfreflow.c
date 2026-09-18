/*
 * Reflow de um PDF de verdade, para julgar a olho.
 *
 *   .\test.ps1 pdfreflow "testdata\cnv.pdf" 20
 *   .\test.ps1 pdfreflow "testdata\cnv.pdf" 20 4     # quatro paginas seguidas
 *
 * O test_reflow sintetico prova que cada regra faz o que promete em isolamento.
 * Este responde a outra pergunta, que nenhum teste sintetico responde: as regras
 * acertam JUNTAS, num livro que ninguem diagramou pensando nelas. A saida e
 * feita para leitura humana - o paragrafo reconstruido e depois as telas
 * repaginadas - porque o julgamento aqui e "isso esta legivel?", e isso nao
 * cabe num assert.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pdf_doc.h"
#include "pdf_text.h"
#include "textlines.h"
#include "reflow.h"
#include "layout.h"
#include "host_io.h"
#include "fakefont.h"

/* Area util do leitor: 480x272 menos margens, barra de topo e rodape. */
#define SCR_TEXT_W 452.0f
#define SCR_TEXT_H 222.0f

static void print_flags(unsigned f)
{
    if (f & RF_HEADING) printf(" TITULO");
    if (f & RF_CENTER)  printf(" CENTRO");
    if (f & RF_CONT)    printf(" CONT");
    if (f & RF_OPEN)    printf(" ABERTO");
}

/*
 * Despejo das linhas de ORIGEM, com as coordenadas.
 *
 * E a ferramenta de calibragem do modulo: quando um paragrafo sai quebrado onde
 * nao devia, a pergunta e sempre "qual numero disparou a regra?", e ela nao tem
 * resposta sem ver o X, o Y e o corpo de cada linha como o reflow os viu.
 */
static void dump_lines(const TextLines *tl, const Reflow *rf)
{
    printf("\n----- linhas de origem -----\n");
    printf("  #     y      x   x_end  corpo   dy  recuo  sobra  texto\n");
    for (int i = 0; i < tl->nlines; ++i) {
        const TextLine *l = &tl->lines[i];
        float dy = (i > 0) ? tl->lines[i - 1].y - l->y : 0.0f;
        printf("%3d %6.1f %6.1f %6.1f %5.1f %5.1f %6.1f %6.1f  %.*s\n",
               i, l->y, l->x, l->x_end, l->size, dy,
               l->x - rf->body_x0, rf->body_x1 - l->x_end,
               l->len > 58 ? 58 : l->len, tl->buf + l->off);
    }
}

static void do_page(PdfDoc *doc, int page1, int font_px, int show_screens,
                    int want_lines)
{
    PdfArena a;
    pdf_arena_init(&a, 64 * 1024);

    printf("\n================ pagina %d do PDF ================\n", page1);

    PdfObj *page = pdf_doc_page(doc, &a, page1 - 1);
    if (!page) {
        printf("pagina nao encontrada na arvore\n");
        pdf_arena_free(&a);
        return;
    }

    PdfTextPage tp;
    if (pdf_text_extract(doc, &a, page, &tp) != 0) {
        printf("sem content stream legivel\n");
        pdf_arena_free(&a);
        return;
    }

    TextLines tl;
    if (textlines_build(&a, &tp, &tl) != 0) {
        printf("textlines_build falhou (memoria)\n");
        pdf_arena_free(&a);
        return;
    }

    Reflow rf;
    if (reflow_build(&a, &tl, NULL, &rf) != 0) {
        printf("reflow_build falhou (memoria)\n");
        pdf_arena_free(&a);
        return;
    }

    printf("caixa   : %.0f x %.0f pt  /Rotate %d\n",
           tl.page_w, tl.page_h, tl.rotate);
    printf("linhas  : %d de origem, %d descartadas (cabecalho/rodape)\n",
           rf.lines_in, rf.lines_dropped);
    printf("corpo   : %.1f pt   entrelinha %.1f pt   margens %.0f..%.0f\n",
           rf.body_size, rf.leading, rf.body_x0, rf.body_x1);
    printf("colunas : %d        hifens remontados: %d\n",
           rf.columns, rf.hyphen_joins);
    printf("saida   : %d paragrafos, %d bytes%s\n",
           rf.nparas, rf.buflen, rf.truncated ? "  (TRUNCADO)" : "");

    if (want_lines)
        dump_lines(&tl, &rf);

    if (rf.nparas == 0) {
        printf("\nnada aproveitavel: pagina sem camada de texto\n");
        pdf_arena_free(&a);
        return;
    }

    printf("\n----- paragrafos reconstruidos -----\n");
    for (int i = 0; i < rf.nparas; ++i) {
        const RfPara *p = &rf.paras[i];
        printf("[%02d col%d %2dl %.0fpt", i, p->col, p->nlines, p->size);
        print_flags(p->flags);
        printf("]\n%.*s\n\n", p->len, rf.buf + p->off);
    }

    if (!show_screens) {
        pdf_arena_free(&a);
        return;
    }

    FakeFont ff;
    LayoutFont lf;
    ff_init(&ff, &lf, (float)font_px);

    LayoutOpts lo;
    layout_defaults(&lo);
    lo.indent = (float)font_px;

    Layout lay;
    if (layout_build(&a, &rf, &lf, SCR_TEXT_W, SCR_TEXT_H, &lo, &lay) != 0) {
        printf("layout_build falhou\n");
        pdf_arena_free(&a);
        return;
    }

    printf("----- repaginado para %.0fx%.0f px a %d px de corpo -----\n",
           SCR_TEXT_W, SCR_TEXT_H, font_px);
    printf("%d linhas de tela em %d telas%s\n",
           lay.nlines, lay.npages, lay.truncated ? "  (TRUNCADO)" : "");
    /*
     * Custo de memoria de UMA pagina, do extrator ao layout.
     *
     * O numero importa porque no console a arena e reciclada por virada de
     * pagina, entao este e o pico, e ele tem de caber no orcamento de 16 MB de
     * heap do PSP-2000 com folga para o atlas de glifos e os framebuffers. Se um
     * dia crescer, e aqui que aparece.
     */
    printf("arena   : %d KB pedidos ao sistema, %d KB entregues\n",
           (int)(a.reserved / 1024), (int)(a.handed / 1024));

    for (int p = 0; p < lay.npages; ++p) {
        printf("\n    +----------------------------------------------"
               "-------+ tela %d/%d\n", p + 1, lay.npages);
        for (int i = lay.page_first[p]; i < lay.page_first[p + 1]; ++i) {
            const LayoutLine *l = &lay.lines[i];
            /* Reproduz o recuo em espacos so para a leitura ficar parecida com
             * o que vai para a tela. Um espaco por ~5 px. */
            int pad = (int)(l->x / 5.0f);
            printf("    | %*s%.*s\n", pad, "", l->len, rf.buf + l->off);
        }
        printf("    +---------------------------------------------------"
               "--+\n");
    }

    pdf_arena_free(&a);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("uso: test_pdfreflow <arquivo.pdf> [pagina] [n_paginas] "
               "[corpo_px] [linhas]\n");
        printf("  o ultimo argumento, se for \"linhas\", despeja as "
               "coordenadas de origem\n");
        return 2;
    }
    int page1   = (argc >= 3) ? atoi(argv[2]) : 1;
    int npages  = (argc >= 4) ? atoi(argv[3]) : 1;
    int font_px = (argc >= 5) ? atoi(argv[4]) : 18;
    int want_lines = 0;
    for (int i = 2; i < argc; ++i)
        if (strcmp(argv[i], "linhas") == 0)
            want_lines = 1;
    if (npages < 1) npages = 1;
    if (font_px < 8) font_px = 18;

    PdfIo io;
    if (host_io_open(&io, argv[1]) != 0) {
        printf("nao abriu: %s\n", argv[1]);
        return 1;
    }

    PdfDoc doc;
    if (pdf_doc_open(&doc, &io) != 0) {
        printf("pdf_doc_open falhou: %s\n", doc.err);
        pdf_doc_close(&doc);
        return 1;
    }

    printf("arquivo : %s\n", argv[1]);
    printf("paginas : %d\n", doc.pages);

    for (int k = 0; k < npages; ++k) {
        int p = page1 + k;
        if (p < 1 || p > doc.pages)
            break;
        do_page(&doc, p, font_px, 1, want_lines);
    }

    pdf_doc_close(&doc);
    return 0;
}
