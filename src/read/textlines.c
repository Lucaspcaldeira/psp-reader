#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "textlines.h"

static int cmp_runs(const void *pa, const void *pb)
{
    const PdfTextRun *a = (const PdfTextRun *)pa;
    const PdfTextRun *b = (const PdfTextRun *)pb;

    /*
     * Y decrescente primeiro: no espaco do PDF o Y cresce para CIMA, entao a
     * primeira linha da pagina e a de maior Y. Inverter isso devolve o livro de
     * baixo para cima, uma linha por vez - o tipo de erro que parece defeito de
     * fonte.
     *
     * A tolerancia de 0,7 pt agrupa runs da mesma linha visual que diferem por
     * arredondamento de matriz. O agrupamento fino, proporcional ao corpo, e
     * feito na varredura adiante.
     */
    if (b->y - a->y < -0.7f) return -1;
    if (b->y - a->y >  0.7f) return  1;
    if (a->x < b->x) return -1;
    if (a->x > b->x) return  1;
    return 0;
}

/*
 * Runs para o espaco EXIBIDO: translada pela origem do /MediaBox e desfaz o
 * /Rotate.
 *
 * Sem isto, uma pagina com /Rotate 90 sai como uma coluna de silabas: o texto
 * ali e desenhado girado no espaco do usuario, entao o agrupamento por Y ve cada
 * pedaco numa "linha" diferente. O avanco tambem gira, e por isso o run carrega
 * o vetor (dx, dy) e nao so o modulo.
 *
 * Convencao de saida, valida para qualquer /Rotate: X cresce para a direita, Y
 * cresce para cima, canto inferior esquerdo em (0, 0).
 */
static void to_display_space(PdfTextRun *runs, int n, const PdfTextPage *tp,
                             int rot, float pw, float ph)
{
    (void)pw; (void)ph;
    float w = tp->mb_x1 - tp->mb_x0;
    float h = tp->mb_y1 - tp->mb_y0;

    for (int i = 0; i < n; ++i) {
        PdfTextRun *r = &runs[i];
        float x = r->x - tp->mb_x0;
        float y = r->y - tp->mb_y0;
        float dx = r->dx, dy = r->dy;

        switch (rot) {
        case 90:                                  /* conteudo gira 90 no sentido do relogio */
            r->x = y;          r->y = w - x;
            r->dx = dy;        r->dy = -dx;
            break;
        case 180:
            r->x = w - x;      r->y = h - y;
            r->dx = -dx;       r->dy = -dy;
            break;
        case 270:
            r->x = h - y;      r->y = x;
            r->dx = -dy;       r->dy = dx;
            break;
        default:
            r->x = x;          r->y = y;
            break;
        }
    }
}

/*
 * Avanco horizontal utilizavel de um run.
 *
 * Depois da rotacao, texto de leitura tem |dx| ~ width. Um run em que dx e
 * pequeno e texto genuinamente VERTICAL. Ali o modulo e a unica coisa
 * aproveitavel: nao mede o fim do run na horizontal, mas mantem a caneta
 * andando, e um laco de caneta parada gruda palavras que nao se tocam.
 */
static float run_adv(const PdfTextRun *r)
{
    float adx = fabsf(r->dx);
    return (adx >= r->width * 0.5f) ? adx : r->width;
}

/*
 * Texto VERTICAL nao entra na leitura.
 *
 * Depois de desfeito o /Rotate, um run que avanca mais na vertical que na
 * horizontal nao e texto corrido desta pagina: e marginalia girada na lombada,
 * rotulo de eixo de grafico, credito na borda. Ele nao pertence ao fluxo em
 * nenhuma ordem de leitura, e deixa-lo passar e pior que perde-lo - o
 * agrupamento por Y fatia a marginalia em pedacos e enfia um em cada linha do
 * corpo, e o resultado sao palavras coladas no meio da frase, como
 * "o homem que estavaGrandiosas diante de mim". Isso apareceu na pagina 8 de
 * "A Ilha do Tesouro Recortado", onde "Colecao Aventuras Grandiosas" corre pela
 * borda da pagina.
 */
static int run_is_vertical(const PdfTextRun *r)
{
    return fabsf(r->dy) > fabsf(r->dx) * 1.5f && r->width > 0.0f;
}

int textlines_build(PdfArena *a, const PdfTextPage *tp, TextLines *out)
{
    memset(out, 0, sizeof(*out));

    if (!tp)
        return 0;

    int rot = tp->rotate % 360;
    if (rot < 0)
        rot += 360;
    rot = (rot / 90) * 90;      /* /Rotate fora de multiplo de 90 e invalido */

    float pw = tp->mb_x1 - tp->mb_x0;
    float ph = tp->mb_y1 - tp->mb_y0;
    if (rot == 90 || rot == 270) {
        float t = pw; pw = ph; ph = t;
    }
    out->page_w = pw;
    out->page_h = ph;
    out->rotate = rot;

    if (tp->nruns <= 0) {
        out->truncated = tp->truncated;
        return 0;
    }

    out->lines = (TextLine *)pdf_arena_alloc(a, sizeof(TextLine) * TL_MAX_LINES);

    /* +nruns porque cada fronteira de run pode render um espaco inserido. */
    int cap = tp->textlen + tp->nruns + 16;
    out->buf = (char *)pdf_arena_alloc(a, (size_t)cap);
    if (!out->lines || !out->buf)
        return -1;

    /* Copia para ordenar sem mexer na saida do extrator, que o chamador pode
     * querer usar de outra forma. */
    PdfTextRun *sorted = (PdfTextRun *)pdf_arena_alloc(
        a, sizeof(PdfTextRun) * (size_t)tp->nruns);
    if (!sorted)
        return -1;
    memcpy(sorted, tp->runs, sizeof(PdfTextRun) * (size_t)tp->nruns);
    to_display_space(sorted, tp->nruns, tp, rot, pw, ph);

    /* Peneira os verticais ANTES de ordenar: depois eles ja estao intercalados
     * com o corpo e nao ha como distinguir sem repetir a conta. */
    int nruns = 0;
    for (int k = 0; k < tp->nruns; ++k) {
        if (run_is_vertical(&sorted[k])) {
            out->runs_vertical++;
            continue;
        }
        sorted[nruns++] = sorted[k];
    }
    if (nruns <= 0)
        return 0;

    qsort(sorted, (size_t)nruns, sizeof(PdfTextRun), cmp_runs);

    int bl = 0;
    int i = 0;

    while (i < nruns) {
        if (out->nlines >= TL_MAX_LINES) {
            out->truncated = 1;
            break;
        }

        float line_y = sorted[i].y;
        float size   = sorted[i].size;
        if (size <= 0.0f)
            size = 10.0f;

        /* Tolerancia proporcional ao corpo: 0,3 do corpo mantem subscritos e
         * mudancas de fonte na mesma linha, sem juntar duas linhas de texto. */
        float tol = size * 0.3f;
        if (tol < 0.7f)
            tol = 0.7f;

        int line_start = bl;
        float pen_x = -1e9f;
        float x0 = sorted[i].x;
        float x1 = sorted[i].x;

        /* Maior lacuna interna: ver comentario em TextLine. gap_pre e o indice
         * PRE-normalizacao do espaco inserido; a normalizacao adiante o traduz,
         * porque ela reescreve o buffer no lugar e desloca tudo. */
        float gap_x = 0.0f, gap_w = 0.0f;
        int   gap_pre = -1;

        while (i < nruns && fabsf(sorted[i].y - line_y) <= tol) {
            const PdfTextRun *r = &sorted[i];

            /*
             * Espaco por LACUNA.
             *
             * Livro editorado posiciona palavra por palavra e frequentemente nao
             * escreve caractere de espaco nenhum: a unica pista e a distancia
             * entre o fim de um run e o inicio do proximo. Sem esta regra,
             * "Colecao Aventuras" sai "ColecaoAventuras".
             */
            if (pen_x > -1e8f && r->x - pen_x > size * 0.25f) {
                if (r->x - pen_x > gap_w) {
                    gap_w   = r->x - pen_x;
                    gap_x   = pen_x;
                    gap_pre = bl;
                }
                if (bl < cap)
                    out->buf[bl++] = ' ';
            }

            int take = r->len;
            if (bl + take > cap)
                take = cap - bl;
            if (take > 0) {
                memcpy(out->buf + bl, tp->text + r->off, (size_t)take);
                bl += take;
            }

            float adv = run_adv(r);
            pen_x = r->x + adv;
            if (pen_x > x1)
                x1 = pen_x;
            if (r->size > size)
                size = r->size;
            i++;
        }

        /*
         * Normaliza o espaco em branco da linha.
         *
         * Em texto JUSTIFICADO as duas fontes de espaco se somam: o PDF escreve
         * o glifo de espaco E deixa lacuna larga entre os runs, entao a regra
         * acima acrescenta um segundo. O resultado sem isto e espaco duplo em
         * toda linha justificada.
         */
        int w = line_start;
        int prev_sp = 1;      /* 1 no inicio: descarta espaco a esquerda */
        int gap_off = -1;
        for (int k = line_start; k < bl; ++k) {
            char c = out->buf[k];
            int sp = (c == ' ' || c == '\t' || c == '\r' || c == '\n');
            if (k == gap_pre) {
                /* O espaco da lacuna pode ser colapsado contra um espaco que o
                 * PDF ja tinha escrito; nesse caso quem sobrevive e o anterior,
                 * em w-1. */
                gap_off = (sp && prev_sp) ? w - 1 : w;
            }
            if (sp) {
                if (prev_sp)
                    continue;
                out->buf[w++] = ' ';
                prev_sp = 1;
            } else {
                out->buf[w++] = c;
                prev_sp = 0;
            }
        }
        while (w > line_start && out->buf[w - 1] == ' ')
            w--;
        bl = w;

        /* Lacuna que caiu fora depois do corte de espaco a direita deixa de
         * existir: nao ha texto depois dela para separar. */
        if (gap_off < line_start || gap_off >= bl) {
            gap_off = -1;
            gap_w   = 0.0f;
        }

        if (bl > line_start) {
            TextLine *ln = &out->lines[out->nlines++];
            ln->off   = line_start;
            ln->len   = bl - line_start;
            ln->y     = line_y;
            ln->x     = x0;
            ln->x_end = x1;
            ln->size  = size;
            ln->gap_x = gap_x;
            ln->gap_w = gap_w;
            ln->gap_off = gap_off;
        } else {
            /* Linha que sobrou vazia depois da normalizacao (so espacos). Nao
             * entra: uma linha em branco no meio do texto viraria um paragrafo
             * fantasma no reflow. */
            bl = line_start;
        }
    }

    out->buflen = bl;
    if (tp->truncated)
        out->truncated = 1;
    return 0;
}
