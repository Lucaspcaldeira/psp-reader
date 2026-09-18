/*
 * Despeja um EPUB de verdade, para julgar a olho.
 *
 *   .\test.ps1 epubdump "livro.epub"          # so a estrutura
 *   .\test.ps1 epubdump "livro.epub" 0 3      # 3 unidades a partir da 0
 *   .\test.ps1 epubdump "livro.epub" 0 1 telas
 *
 * O test_epub sintetico prova que cada regra funciona em isolamento, com um
 * EPUB que este projeto mesmo construiu. Este responde a outra pergunta: o
 * arquivo que uma editora de verdade gerou, com o XHTML que o InDesign ou o
 * Calibre cospem, atravessa o mesmo caminho?
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "epub.h"
#include "layout.h"
#include "host_io.h"
#include "fakefont.h"

#define SCR_TEXT_W 452.0f
#define SCR_TEXT_H 222.0f

static void print_flags(unsigned f)
{
    if (f & RF_HEADING) printf(" TITULO");
    if (f & RF_CENTER)  printf(" CENTRO");
    if (f & RF_CONT)    printf(" CONT");
    if (f & RF_OPEN)    printf(" ABERTO");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("uso: test_epubdump <livro.epub> [unidade] [n] [telas]\n");
        return 2;
    }
    int first = (argc >= 3) ? atoi(argv[2]) : 0;
    int count = (argc >= 4) ? atoi(argv[3]) : 0;
    int want_screens = 0;
    for (int i = 2; i < argc; ++i)
        if (strcmp(argv[i], "telas") == 0)
            want_screens = 1;

    PdfIo io;
    if (host_io_open(&io, argv[1]) != 0) {
        printf("nao abriu: %s\n", argv[1]);
        return 1;
    }

    PdfArena da;
    pdf_arena_init(&da, 128 * 1024);

    EpubDoc d;
    if (epub_open(&d, &da, &io) != 0) {
        printf("epub_open falhou: %s\n", d.err);
        pdf_arena_free(&da);
        return 1;
    }

    printf("arquivo  : %s\n", argv[1]);
    printf("titulo   : %s\n", d.title[0] ? d.title : "(sem)");
    printf("autor    : %s\n", d.author[0] ? d.author : "(sem)");
    printf("zip      : %d membros%s\n", d.zip.nentries,
           d.zip.truncated ? "  (TRUNCADO)" : "");
    printf("espinha  : %d capitulos -> %d unidades%s\n",
           d.nspine, d.nunits, d.truncated ? "  (TRUNCADO)" : "");
    printf("indice   : %d KB de arena\n", (int)(da.reserved / 1024));

    /* Capitulos partidos em mais de uma unidade: o caso que a aritmetica de
     * fronteira errava, e o que vale conferir num livro real. */
    int split = 0;
    for (int i = 0; i < d.nunits; ++i)
        if (!d.units[i].first || !d.units[i].last)
            split++;
    printf("partidos : %d unidades vem de capitulo cortado\n", split);

    if (count <= 0) {
        epub_close(&d);
        pdf_arena_free(&da);
        return 0;
    }

    PdfArena pa;
    pdf_arena_init(&pa, 128 * 1024);

    for (int k = 0; k < count; ++k) {
        int u = first + k;
        if (u < 0 || u >= d.nunits)
            break;

        pdf_arena_reset(&pa);
        Reflow rf;
        printf("\n============ unidade %d de %d ", u, d.nunits);
        printf("(membro %s, bytes %lld..%lld) ============\n",
               d.zip.entries[d.units[u].zi].name,
               d.units[u].from, d.units[u].to);

        if (epub_unit(&d, &pa, u, &rf) != 0) {
            printf("epub_unit falhou\n");
            continue;
        }
        printf("%d paragrafos, %d bytes%s   arena %d KB\n",
               rf.nparas, rf.buflen, rf.truncated ? "  (TRUNCADO)" : "",
               (int)(pa.reserved / 1024));

        for (int i = 0; i < rf.nparas; ++i) {
            const RfPara *p = &rf.paras[i];
            printf("[%02d", i);
            print_flags(p->flags);
            printf("]\n%.*s\n\n", p->len, rf.buf + p->off);
        }

        if (!want_screens)
            continue;

        FakeFont ff;
        LayoutFont lf;
        ff_init(&ff, &lf, 18.0f);
        LayoutOpts lo;
        layout_defaults(&lo);
        lo.indent = 18.0f;

        Layout lay;
        if (layout_build(&pa, &rf, &lf, SCR_TEXT_W, SCR_TEXT_H, &lo, &lay) != 0)
            continue;

        printf("----- %d linhas em %d telas -----\n", lay.nlines, lay.npages);
        for (int p = 0; p < lay.npages; ++p) {
            printf("\n    +-------------------------------------------"
                   "----------+ tela %d/%d\n", p + 1, lay.npages);
            for (int i = lay.page_first[p]; i < lay.page_first[p + 1]; ++i) {
                const LayoutLine *l = &lay.lines[i];
                printf("    | %*s%.*s\n", (int)(l->x / 5.0f), "",
                       l->len, rf.buf + l->off);
            }
            printf("    +---------------------------------------------"
                   "--------+\n");
        }
    }

    pdf_arena_free(&pa);
    epub_close(&d);
    pdf_arena_free(&da);
    return 0;
}
