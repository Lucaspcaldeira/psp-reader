/*
 * Extrai o texto de uma pagina e reconstroi as linhas, para julgar a olho se a
 * extracao esta correta.
 *
 *   .\test.ps1 pdftext "testdata\ilha.pdf" 3
 *
 * A reconstrucao de linhas aqui e um ENSAIO do reflow da Etapa 3c, nao a
 * implementacao final: agrupa por Y, ordena por X e insere espaco quando a
 * lacuna horizontal e grande. Serve para ver se os runs saem com posicao e
 * conteudo certos - com runs soltos na tela nao ha como saber.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "pdf_doc.h"
#include "pdf_text.h"
#include "host_io.h"

static int cmp_runs(const void *pa, const void *pb)
{
    const PdfTextRun *a = (const PdfTextRun *)pa;
    const PdfTextRun *b = (const PdfTextRun *)pb;

    /* Y decrescente: no PDF o Y cresce para CIMA, entao a primeira linha da
     * pagina tem o Y maior. */
    float dy = b->y - a->y;
    if (dy < -0.7f) return -1;
    if (dy >  0.7f) return  1;
    if (a->x < b->x) return -1;
    if (a->x > b->x) return  1;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("uso: test_pdftext <arquivo.pdf> [pagina] [max_linhas]\n");
        return 2;
    }
    int want_page = (argc >= 3) ? atoi(argv[2]) : 1;
    int max_lines = (argc >= 4) ? atoi(argv[3]) : 40;

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

    printf("arquivo: %s\n", argv[1]);
    printf("paginas: %d   pedida: %d\n", doc.pages, want_page);

    if (want_page < 1 || want_page > doc.pages) {
        printf("pagina fora da faixa\n");
        pdf_doc_close(&doc);
        return 1;
    }

    PdfArena a;
    pdf_arena_init(&a, 64 * 1024);

    PdfObj *page = pdf_doc_page(&doc, &a, want_page - 1);
    if (!page) {
        printf("pagina nao encontrada na arvore\n");
        pdf_arena_free(&a);
        pdf_doc_close(&doc);
        return 1;
    }

    PdfTextPage tp;
    int rc = pdf_text_extract(&doc, &a, page, &tp);

    printf("MediaBox: %.0f %.0f %.0f %.0f   /Rotate %d\n",
           tp.mb_x0, tp.mb_y0, tp.mb_x1, tp.mb_y1, tp.rotate);
    printf("extract : rc=%d  runs=%d  bytes=%d  fontes=%d  forms=%d  "
           "imgs_inline=%d%s\n",
           rc, tp.nruns, tp.textlen, tp.fonts_loaded, tp.forms_visited,
           tp.inline_images, tp.truncated ? "  (TRUNCADO)" : "");
    printf("codigos : %d total, %d sem mapeamento Unicode",
           tp.codes_total, tp.codes_unmapped);
    if (tp.codes_total > 0)
        printf("  (%.1f%%)",
               100.0 * (double)tp.codes_unmapped / (double)tp.codes_total);
    printf("\n");

    if (tp.nruns == 0) {
        printf("\nnenhum run: pagina sem camada de texto\n");
        pdf_arena_free(&a);
        pdf_doc_close(&doc);
        return 0;
    }

    /* Ordena em ordem de leitura. */
    PdfTextRun *sorted = (PdfTextRun *)malloc(sizeof(PdfTextRun) * (size_t)tp.nruns);
    memcpy(sorted, tp.runs, sizeof(PdfTextRun) * (size_t)tp.nruns);
    qsort(sorted, (size_t)tp.nruns, sizeof(PdfTextRun), cmp_runs);

    printf("\n===== texto reconstruido =====\n");

    int lines = 0;
    int i = 0;
    while (i < tp.nruns && lines < max_lines) {
        float line_y = sorted[i].y;
        float pen_x  = -1e9f;
        float size   = sorted[i].size;

        /* Buffer da linha para poder normalizar o espaco em branco antes de
         * imprimir - ver a regra abaixo. */
        char line[4096];
        int  ll = 0;

        while (i < tp.nruns && fabsf(sorted[i].y - line_y) <= 0.7f) {
            const PdfTextRun *r = &sorted[i];

            /*
             * Insercao de espaco por LACUNA, nao por caractere.
             *
             * Este e o detalhe central da extracao de texto de PDF: livro
             * editorado costuma posicionar palavra por palavra sem escrever
             * nenhum caractere de espaco. Sem esta regra, "Coleção Aventuras"
             * sai como "ColeçãoAventuras". O limiar de 0,25 do corpo aproxima a
             * largura de um espaco na maioria das fontes de texto.
             */
            if (pen_x > -1e8f && r->x - pen_x > size * 0.25f) {
                if (ll < (int)sizeof(line))
                    line[ll++] = ' ';
            }

            int take = r->len;
            if (ll + take > (int)sizeof(line))
                take = (int)sizeof(line) - ll;
            if (take > 0) {
                memcpy(line + ll, tp.text + r->off, (size_t)take);
                ll += take;
            }

            pen_x = r->x + r->width;
            if (r->size > size)
                size = r->size;
            i++;
        }

        /*
         * Colapsa espaco em branco repetido.
         *
         * Necessario porque as duas fontes de espaco se somam em texto
         * JUSTIFICADO: o PDF escreve o glifo de espaco E deixa uma lacuna larga
         * entre os runs, entao a regra de lacuna acima acrescenta um segundo
         * espaco. O resultado sem isto e "Ao  estudar  a  questao" com espaco
         * duplo em toda linha justificada - foi o que apareceu na primeira
         * extracao do "Comunicacao Nao-Violenta".
         *
         * A regra vai para o reflow da Etapa 3c, que precisa da mesma
         * normalizacao antes de medir larguras.
         */
        int out_n = 0;
        int prev_sp = 1;      /* 1 no inicio: come espaco a esquerda */
        for (int k = 0; k < ll; ++k) {
            char c = line[k];
            int sp = (c == ' ' || c == '\t' || c == '\r' || c == '\n');
            if (sp) {
                if (prev_sp)
                    continue;
                line[out_n++] = ' ';
                prev_sp = 1;
            } else {
                line[out_n++] = c;
                prev_sp = 0;
            }
        }
        while (out_n > 0 && line[out_n - 1] == ' ')
            out_n--;

        if (out_n > 0) {
            fwrite(line, 1, (size_t)out_n, stdout);
            putchar('\n');
            lines++;
        }
    }

    if (i < tp.nruns)
        printf("... (%d runs restantes)\n", tp.nruns - i);

    free(sorted);
    pdf_arena_free(&a);
    pdf_doc_close(&doc);
    return 0;
}
