#include <string.h>
#include <math.h>

#include "layout.h"
#include "utf8.h"

void layout_defaults(LayoutOpts *o)
{
    if (!o)
        return;
    o->line_mul = 1.28f;
    o->para_gap = 0.35f;
    o->head_gap = 0.70f;
    o->indent   = 0.0f;
}

/*
 * Quebra DENTRO de uma palavra que nao cabe sozinha na largura util.
 *
 * Sem este caminho o laco de paginacao nao progride: a palavra nunca cabe, a
 * linha nunca fecha, e o leitor congela numa URL longa ou num nome quimico. O
 * corte anda ao menos um codepoint, sempre - e essa garantia, e nao a estetica,
 * que importa aqui.
 */
static int break_inside(const LayoutFont *f, const char *s, int n, float avail,
                        float *out_w)
{
    const char *p = s, *end = s + n;
    float w = 0.0f;
    int taken = 0;

    while (p < end) {
        const char *b = p;
        utf8_next(&p, end);
        int blen = (int)(p - b);
        float cw = f->measure(f->ud, b, blen);
        if (taken > 0 && w + cw > avail)
            break;
        w += cw;
        taken += blen;
    }
    if (taken <= 0)
        taken = 1;              /* nunca devolve zero: ver comentario acima */
    if (out_w)
        *out_w = w;
    return taken;
}

int layout_build(PdfArena *a, const Reflow *rf, const LayoutFont *f,
                 float text_w, float text_h, const LayoutOpts *o, Layout *out)
{
    LayoutOpts def;
    if (!o) {
        layout_defaults(&def);
        o = &def;
    }

    memset(out, 0, sizeof(*out));
    if (!rf || !f || !f->measure || text_w <= 1.0f || text_h <= 1.0f)
        return -1;
    if (rf->nparas <= 0)
        return 0;

    /*
     * Teto de linhas: no pior caso cada linha leva um codepoint (largura util
     * menor que um glifo), entao o limite honesto e proporcional aos bytes.
     * Dividir por 2 e a aposta de que a media real e muito maior, com
     * `truncated` cobrindo a exceccao em vez de estourar memoria.
     */
    int cap = rf->buflen / 2 + rf->nparas * 2 + 32;
    out->lines = (LayoutLine *)pdf_arena_alloc(a, sizeof(LayoutLine) * (size_t)cap);
    out->page_first = (int *)pdf_arena_alloc(a, sizeof(int) * (size_t)(cap + 2));
    if (!out->lines || !out->page_first)
        return -1;

    float step = f->line_height * o->line_mul;
    if (step <= 0.0f)
        step = 12.0f;
    float space_w = f->measure(f->ud, " ", 1);
    if (space_w <= 0.0f)
        space_w = f->measure(f->ud, "n", 1) * 0.4f;

    float y = 0.0f;                 /* topo da proxima linha, na pagina atual */
    out->page_first[0] = 0;
    out->npages = 1;

    for (int pi = 0; pi < rf->nparas; ++pi) {
        const RfPara *pa = &rf->paras[pi];
        if (pa->len <= 0)
            continue;

        unsigned base_flags = 0;
        if (pa->flags & RF_HEADING) base_flags |= LL_HEAD;
        if (pa->flags & RF_CENTER)  base_flags |= LL_CENTER;

        /* Vao antes do paragrafo. Nao se aplica no topo da pagina de tela: um
         * vao no topo e uma margem torta, nao uma separacao. */
        if (out->nlines > out->page_first[out->npages - 1]) {
            float gap = o->para_gap;
            if (base_flags & LL_HEAD)
                gap += o->head_gap;
            y += step * gap;
        }

        int pos = 0;
        int first_line = 1;
        int emitted = 0;

        while (pos < pa->len) {
            /*
             * Recuo de primeira linha. Nao vale para titulo nem para bloco
             * centralizado: nos dois casos o recuo briga com o alinhamento e o
             * resultado parece defeito.
             */
            float indent = 0.0f;
            if (first_line && o->indent > 0.0f &&
                !(base_flags & (LL_HEAD | LL_CENTER)) &&
                !(pi == 0 && (pa->flags & RF_CONT)))
                indent = o->indent;

            float avail = text_w - indent;
            if (avail < space_w * 2.0f)
                avail = space_w * 2.0f;

            /* --- guloso por PALAVRA ---------------------------------------
             * Medir palavra por palavra e somar, em vez de medir prefixos cada
             * vez maiores, deixa a quebra linear no tamanho do paragrafo. O
             * reflow ja normalizou o espaco em branco para um unico 0x20, entao
             * a fronteira de palavra e um teste de byte. */
            const char *base = rf->buf + pa->off;
            int cut = -1;
            float used = 0.0f;
            int k = pos;

            while (k < pa->len) {
                int ws = k;
                while (ws < pa->len && base[ws] == ' ')
                    ws++;
                if (ws >= pa->len)
                    break;
                int we = ws;
                while (we < pa->len && base[we] != ' ')
                    we++;

                float ww = f->measure(f->ud, base + ws, we - ws);
                float need = (cut < 0) ? ww : (space_w + ww);

                if (used + need > avail && cut >= 0)
                    break;
                if (used + need > avail && cut < 0) {
                    /* Primeira palavra da linha nao cabe: parte dentro dela. */
                    float bw = 0.0f;
                    int nb = break_inside(f, base + ws, we - ws, avail, &bw);
                    cut  = ws + nb;
                    used = bw;
                    break;
                }

                used += need;
                cut = we;
                k = we;
            }

            if (cut < 0 || cut <= pos) {
                /* So havia espaco daqui ate o fim. */
                break;
            }

            /* --- cabe nesta pagina de tela? -------------------------------- */
            int page_start = out->page_first[out->npages - 1];
            int on_page    = out->nlines - page_start;

            int overflow = (y + step > text_h + 0.5f);

            /*
             * Titulo nao fica sozinho no pe da pagina.
             *
             * Ler um titulo e ter que virar a tela para ver a primeira linha do
             * que ele intitula e um defeito de diagramacao classico. Empurrar
             * exige que a pagina ja tenha duas linhas - do contrario a pagina
             * nova comecaria vazia e o laco nao andaria.
             */
            if (!overflow && first_line && (base_flags & LL_HEAD) &&
                on_page >= 2 && y + step * 2.0f > text_h + 0.5f)
                overflow = 1;

            if (overflow && on_page > 0) {
                if (out->npages > cap) {
                    out->truncated = 1;
                    break;
                }
                out->page_first[out->npages++] = out->nlines;
                y = 0.0f;
            }

            if (out->nlines >= cap) {
                out->truncated = 1;
                break;
            }

            LayoutLine *ll = &out->lines[out->nlines++];
            ll->off   = pa->off + pos;
            ll->len   = cut - pos;
            ll->w     = used;
            ll->para  = pi;
            ll->top   = y;
            ll->flags = base_flags | (first_line ? LL_FIRST : 0u);

            if (base_flags & LL_CENTER) {
                ll->x = (text_w - used) * 0.5f;
                if (ll->x < 0.0f)
                    ll->x = 0.0f;
            } else {
                ll->x = indent;
            }

            y += step;
            emitted++;

            /* Consome o espaco da emenda: ele nao aparece no inicio da proxima
             * linha, senao toda linha comecaria deslocada meio glifo. */
            pos = cut;
            while (pos < pa->len && base[pos] == ' ')
                pos++;
            first_line = 0;
        }

        /* Marca a ultima linha do paragrafo - so se ESTE paragrafo emitiu
         * alguma, senao a marca cairia na ultima linha do anterior. */
        if (emitted > 0)
            out->lines[out->nlines - 1].flags |= LL_LAST;

        if (out->truncated)
            break;
    }

    /* Sentinela: page_first[npages] == nlines. Com ela, o numero de linhas de
     * qualquer pagina e uma subtracao, inclusive da ultima. */
    out->page_first[out->npages] = out->nlines;
    return 0;
}

int layout_page_of_line(const Layout *l, int line)
{
    if (!l || line < 0 || line >= l->nlines)
        return -1;
    for (int p = 0; p < l->npages; ++p)
        if (line < l->page_first[p + 1])
            return p;
    return l->npages - 1;
}
