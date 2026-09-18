#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "reflow.h"
#include "utf8.h"

/* ------------------------------------------------------------------------- */
/* Utilidades                                                                 */

static int cmp_float(const void *pa, const void *pb)
{
    float a = *(const float *)pa, b = *(const float *)pb;
    return (a < b) ? -1 : (a > b) ? 1 : 0;
}

/*
 * Percentil, nao media.
 *
 * A media de X inicial de linha e envenenada por uma unica linha comecando fora
 * da margem - travessao de dialogo, aspa pendurada, nota de rodape recuada. O
 * percentil ignora a cauda por construcao, que e exatamente o comportamento
 * desejado num detector de margem.
 */
static float percentile(float *v, int n, float p)
{
    if (n <= 0)
        return 0.0f;
    qsort(v, (size_t)n, sizeof(float), cmp_float);
    int i = (int)(p * (float)(n - 1) + 0.5f);
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    return v[i];
}

/* --- classificacao de codepoint -------------------------------------------
 * Cobertura deliberada: ASCII, Latin-1 suplementar e Latin Extended-A. Isso
 * abrange portugues, espanhol, ingles, frances e alemao, que e o alcance
 * realista de uma biblioteca pessoal. Grego e cirilico cairiam na regra
 * conservadora (nao e letra), o que so desliga a remocao de hifen. */

static int cp_is_letter(unsigned int c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
        return 1;
    if (c >= 0xC0 && c <= 0xFF && c != 0xD7 && c != 0xF7)
        return 1;
    if (c >= 0x100 && c <= 0x17F)
        return 1;
    return 0;
}

static int cp_is_lower(unsigned int c)
{
    if (c >= 'a' && c <= 'z')
        return 1;
    if (c >= 0xDF && c <= 0xFF && c != 0xF7)
        return 1;
    return 0;
}

/* Hifen de QUEBRA, o unico que pode ser removido ao remontar a palavra.
 * Travessao (U+2013/2014) fica de fora de proposito: e pontuacao, nao quebra. */
static int cp_is_break_hyphen(unsigned int c)
{
    return c == 0x2D || c == 0x2010 || c == 0x00AD;
}

static int cp_is_terminal(unsigned int c)
{
    return c == '.' || c == '!' || c == '?' || c == ':' || c == ';' ||
           c == 0x2026 /* ... */ ||
           c == '"' || c == '\'' || c == ')' || c == ']' ||
           c == 0x201D || c == 0x2019 || c == 0xBB;
}

/* Ultimo e penultimo codepoint de uma fatia UTF-8. */
static void last_two_cp(const char *s, int n, unsigned int *last,
                        unsigned int *prev, int *last_bytes)
{
    unsigned int hist[2] = { 0, 0 };
    int blen[2] = { 0, 0 };
    const char *p = s, *end = s + n;
    while (p < end) {
        const char *b = p;
        unsigned int c = utf8_next(&p, end);
        hist[0] = hist[1];  blen[0] = blen[1];
        hist[1] = c;        blen[1] = (int)(p - b);
    }
    *last = hist[1];
    *prev = hist[0];
    *last_bytes = blen[1];
}

static unsigned int first_cp(const char *s, int n)
{
    const char *p = s, *end = s + n;
    if (p >= end)
        return 0;
    return utf8_next(&p, end);
}

/* Codepoints do primeiro "termo" da fatia, para estimar se ele teria caido na
 * linha anterior. Ver a regra de fim de paragrafo em text_ragged. */
static int first_word_cps(const char *s, int n)
{
    const char *p = s, *end = s + n;
    int cnt = 0;
    while (p < end) {
        unsigned int c = utf8_next(&p, end);
        if (c == ' ') {
            if (cnt > 0)
                break;
            continue;
        }
        cnt++;
        if (cnt > 40)
            break;
    }
    return cnt;
}

/*
 * Linha que e so um numero de pagina.
 *
 * Aceita algarismo romano porque a numeracao de prefacio de livro editorado e
 * romana - "xvii" no rodape de toda pagina de introducao. O limite de 10
 * codepoints e o que impede "MIX" ou "DIVIDI" de serem confundidos com romano.
 */
static int line_is_pagenum(const char *s, int n)
{
    const char *p = s, *end = s + n;
    int cps = 0, digits = 0, romans = 0;
    while (p < end) {
        unsigned int c = utf8_next(&p, end);
        cps++;
        if (cps > 10)
            return 0;
        if (c >= '0' && c <= '9') { digits++; continue; }
        if (c=='i'||c=='v'||c=='x'||c=='l'||c=='c'||c=='d'||c=='m'||
            c=='I'||c=='V'||c=='X'||c=='L'||c=='C'||c=='D'||c=='M') {
            romans++; continue;
        }
        if (c == ' ' || c == '.' || c == '-' || c == '|' || c == '[' ||
            c == ']' || c == 0x2013 || c == 0x2014)
            continue;
        return 0;
    }
    return cps > 0 && (digits + romans) > 0 && (digits == 0 || romans == 0);
}

/* ------------------------------------------------------------------------- */
/* Linha de trabalho                                                          */

/*
 * Copia mutavel da linha de origem.
 *
 * Existe porque a deteccao de coluna PARTE linhas: no espaco do PDF a linha da
 * coluna esquerda e a da direita estao na mesma altura, entao o agrupamento por
 * Y as juntou numa so. Desfazer isso cria linhas que nao existem em TextLines,
 * e nao ha por que sujar a saida do estagio anterior com elas.
 */
typedef struct {
    int   off, len;
    float y, x, x_end, size;
    int   col;
    int   drop;
} WLine;

static int cmp_wline(const void *pa, const void *pb)
{
    const WLine *a = (const WLine *)pa, *b = (const WLine *)pb;
    if (a->col != b->col)
        return a->col < b->col ? -1 : 1;
    /* Dentro da coluna: de cima para baixo, ou seja Y decrescente. */
    if (a->y > b->y) return -1;
    if (a->y < b->y) return  1;
    if (a->x < b->x) return -1;
    if (a->x > b->x) return  1;
    return 0;
}

/* ------------------------------------------------------------------------- */

int reflow_append(char *buf, int cap, int *len, int para_start,
                  const char *line, int nlen, int dehyphenate)
{
    int bl = *len;
    int ate_hyphen = 0;

    if (bl > para_start) {
        /*
         * Remonta a palavra partida no fim da linha anterior.
         *
         * O hifen so cai se o que vem depois comeca em MINUSCULA. "Nao-" +
         * "Violenta" e um composto de verdade e mantem o hifen; "gene-" +
         * "roso" e uma palavra que o diagramador partiu na largura do papel, e
         * essa largura nao existe mais depois do reflow.
         */
        unsigned int last, prev2;
        int lastb;
        last_two_cp(buf + para_start, bl - para_start, &last, &prev2, &lastb);

        if (cp_is_break_hyphen(last)) {
            if (dehyphenate && cp_is_letter(prev2) &&
                cp_is_lower(first_cp(line, nlen))) {
                bl -= lastb;              /* engole o hifen */
                ate_hyphen = 1;
            }
            /* Com ou sem hifen, cola sem espaco: a palavra e uma so. */
        } else if (bl < cap) {
            buf[bl++] = ' ';
        }
    }

    int take = nlen;
    if (bl + take > cap)
        take = cap - bl;
    if (take > 0) {
        memcpy(buf + bl, line, (size_t)take);
        bl += take;
    }

    *len = bl;
    return ate_hyphen;
}

void reflow_defaults(RfOpts *o)
{
    if (!o)
        return;
    o->drop_running   = 1;
    o->detect_columns = 1;
    o->dehyphenate    = 1;
}

/*
 * Coluna dupla: existe uma calha vertical?
 *
 * A pista e a maior lacuna interna que cada linha carrega de textlines. Numa
 * pagina de duas colunas quase toda linha visual tem uma lacuna larga, e todas
 * no MESMO X - e essa coincidencia, nao a largura da lacuna, que distingue uma
 * calha de um sumario com pontilhado ou de uma linha de tabela.
 *
 * Roda sobre as linhas de ORIGEM, porque sao elas que carregam gap_*, e antes de
 * qualquer particao. Devolve o X da calha, ou 0 se a pagina e de coluna unica.
 * `scratch` precisa caber tl->nlines floats e sai destruido.
 */
static float gutter_x(const TextLines *tl, float body_size, float *scratch)
{
    if (tl->nlines < 8)
        return 0.0f;

    float min_gap = body_size * 1.2f;
    if (min_gap < 6.0f)
        min_gap = 6.0f;

    int m = 0;
    for (int i = 0; i < tl->nlines; ++i) {
        const TextLine *ln = &tl->lines[i];
        if (ln->gap_off >= 0 && ln->gap_w >= min_gap)
            scratch[m++] = ln->gap_x + ln->gap_w * 0.5f;
    }

    /* Metade das linhas precisa ter a lacuna. Menos que isso e ornamento de
     * pagina, nao estrutura de coluna. */
    if (m < (tl->nlines + 1) / 2)
        return 0.0f;

    float med = percentile(scratch, m, 0.5f);

    /* Calha perto da borda nao e calha: e recuo de bloco ou marginalia. */
    if (med < tl->page_w * 0.28f || med > tl->page_w * 0.72f)
        return 0.0f;

    /* E precisam estar TODAS no mesmo X. `scratch` ja saiu ordenado do
     * percentil, entao a contagem da faixa e uma varredura. */
    float tol = tl->page_w * 0.05f;
    int agree = 0;
    for (int i = 0; i < m; ++i)
        if (fabsf(scratch[i] - med) <= tol)
            agree++;
    if (agree < (tl->nlines + 1) / 2)
        return 0.0f;

    return med;
}

int reflow_build(PdfArena *a, const TextLines *tl, const RfOpts *o, Reflow *out)
{
    RfOpts def;
    if (!o) {
        reflow_defaults(&def);
        o = &def;
    }

    memset(out, 0, sizeof(*out));
    out->columns = 1;
    if (!tl || tl->nlines <= 0)
        return 0;

    out->lines_in = tl->nlines;

    /* Uma linha pode virar duas (particao de coluna), dai o 2x. */
    int wcap = tl->nlines * 2 + 4;
    WLine *w = (WLine *)pdf_arena_alloc(a, sizeof(WLine) * (size_t)wcap);
    float *sc = (float *)pdf_arena_alloc(a, sizeof(float) * (size_t)wcap);
    out->paras = (RfPara *)pdf_arena_alloc(a, sizeof(RfPara) * RF_MAX_PARAS);

    /* +nlines: cada juncao de linha pode inserir um espaco. */
    int bcap = tl->buflen + tl->nlines + 16;
    out->buf = (char *)pdf_arena_alloc(a, (size_t)bcap);
    if (!w || !sc || !out->paras || !out->buf)
        return -1;

    /* --- corpo modal ------------------------------------------------------
     * Mediana e nao maximo: o maximo e o titulo, e usar o titulo como
     * referencia faria o texto inteiro parecer "corpo pequeno" e desligar a
     * deteccao de titulo. */
    for (int i = 0; i < tl->nlines; ++i)
        sc[i] = tl->lines[i].size > 0.0f ? tl->lines[i].size : 10.0f;
    out->body_size = percentile(sc, tl->nlines, 0.5f);
    if (out->body_size <= 0.0f)
        out->body_size = 10.0f;

    /* --- coluna ----------------------------------------------------------- */
    float split = 0.0f;
    if (o->detect_columns)
        split = gutter_x(tl, out->body_size, sc);

    int n = 0;
    for (int i = 0; i < tl->nlines && n < wcap; ++i) {
        const TextLine *ln = &tl->lines[i];

        int can_split = (split > 0.0f && ln->gap_off > ln->off &&
                         ln->gap_off < ln->off + ln->len - 1 &&
                         ln->gap_x <= split && ln->gap_x + ln->gap_w >= split);

        if (can_split && n + 1 < wcap) {
            /* Esquerda: [off, gap_off). Direita: (gap_off, fim]. O byte em
             * gap_off e o espaco que representava a calha, e vai embora. */
            WLine *L = &w[n++];
            L->off = ln->off;
            L->len = ln->gap_off - ln->off;
            L->y = ln->y;  L->x = ln->x;  L->x_end = ln->gap_x;
            L->size = ln->size;  L->col = 0;  L->drop = 0;

            WLine *R = &w[n++];
            R->off = ln->gap_off + 1;
            R->len = (ln->off + ln->len) - (ln->gap_off + 1);
            R->y = ln->y;  R->x = ln->gap_x + ln->gap_w;  R->x_end = ln->x_end;
            R->size = ln->size;  R->col = 1;  R->drop = 0;
            continue;
        }

        WLine *W = &w[n++];
        W->off = ln->off;  W->len = ln->len;
        W->y = ln->y;  W->x = ln->x;  W->x_end = ln->x_end;
        W->size = ln->size;  W->drop = 0;
        /*
         * Linha que atravessa a calha sem lacuna e um titulo de largura cheia.
         * Vai para a coluna 0 para nao aparecer depois do fim da esquerda -
         * imperfeito, mas ler o titulo antes da coluna que ele encima e a
         * ordem certa nas duas interpretacoes.
         */
        W->col = (split > 0.0f && W->x >= split) ? 1 : 0;
    }

    if (split > 0.0f) {
        out->columns = 1;
        for (int i = 0; i < n; ++i)
            if (w[i].col == 1) { out->columns = 2; break; }
    }

    qsort(w, (size_t)n, sizeof(WLine), cmp_wline);

    /* --- geometria do corpo -----------------------------------------------
     * Calculada sobre a pagina toda, e nao por coluna: com duas colunas o
     * numero de linhas por coluna ja e pequeno, e o percentil precisa de
     * amostra. As margens das duas colunas de um mesmo documento tem a mesma
     * largura util, que e o que a regra de linha curta consome. */
    for (int i = 0; i < n; ++i)
        sc[i] = w[i].x;
    out->body_x0 = percentile(sc, n, 0.25f);
    for (int i = 0; i < n; ++i)
        sc[i] = w[i].x_end;
    out->body_x1 = percentile(sc, n, 0.90f);
    if (out->body_x1 <= out->body_x0)
        out->body_x1 = out->body_x0 + tl->page_w * 0.5f;

    /* --- entrelinha modal -------------------------------------------------- */
    int nd = 0;
    for (int i = 1; i < n; ++i) {
        if (w[i].col != w[i - 1].col)
            continue;
        float d = w[i - 1].y - w[i].y;
        /* Fora dessa faixa nao e entrelinha: e salto de bloco ou dois pedacos
         * da mesma linha visual que a tolerancia de Y nao juntou. */
        if (d > out->body_size * 0.55f && d < out->body_size * 3.0f)
            sc[nd++] = d;
    }
    out->leading = nd > 0 ? percentile(sc, nd, 0.5f) : out->body_size * 1.2f;
    if (out->leading <= 0.0f)
        out->leading = out->body_size * 1.2f;

    /*
     * --- texto justificado ou nao?
     *
     * Isso decide qual regra de fim de paragrafo vale, e errar aqui e o pior
     * defeito possivel neste modulo. Em texto JUSTIFICADO toda linha interna
     * termina na margem, entao "terminou curta" significa fim de paragrafo. Em
     * texto ALINHADO A ESQUERDA toda linha termina curta, e a mesma regra
     * quebraria cada linha num paragrafo - devolvendo exatamente o defeito que
     * o reflow existe para corrigir.
     */
    float body_w = out->body_x1 - out->body_x0;
    int at_margin = 0, counted = 0;
    for (int i = 0; i < n; ++i) {
        if (w[i].size < out->body_size * 0.85f || w[i].size > out->body_size * 1.15f)
            continue;
        counted++;
        if (w[i].x_end >= out->body_x1 - out->body_size * 0.35f)
            at_margin++;
    }
    int justified = (counted >= 4 && at_margin * 100 >= counted * 55);

    /* --- cabecalho e rodape ------------------------------------------------
     * Detectado pelo ISOLAMENTO vertical, nao pela posicao na pagina: o que
     * distingue um cabecalho da primeira linha do texto e o vao maior que o
     * entrelinha entre os dois. A pagina inteira tem margem no topo; so o
     * cabecalho tem uma linha em branco embaixo de si.
     *
     * O corpo tem de ser MENOR OU IGUAL ao do texto, e essa condicao nao e
     * cosmetica: sem ela o titulo do capitulo e comido. Ele tambem esta isolado
     * no alto da pagina e tambem e curto - as duas outras condicoes nao o
     * distinguem de um titulo corrente. O que distingue e o tamanho: titulo
     * corrente e sempre igual ou menor que o texto, titulo de capitulo e maior.
     *
     * Com uma pagina por vez nao ha como confirmar que o texto se REPETE nas
     * outras (o que seria a prova). O criterio local resolve numero de pagina e
     * titulo corrente; o cruzamento entre paginas fica para o indice da Etapa 4.
     */
    if (o->drop_running && n >= 4) {
        for (int col = 0; col < RF_MAX_COLS; ++col) {
            int first = -1, last = -1;
            for (int i = 0; i < n; ++i) {
                if (w[i].col != col) continue;
                if (first < 0) first = i;
                last = i;
            }
            if (first < 0 || last - first < 3)
                continue;

            /* Ate duas linhas em cada ponta: cabecalho de duas linhas existe
             * (titulo do livro em cima, titulo do capitulo embaixo). */
            for (int k = 0; k < 2; ++k) {
                int i = first + k;
                if (i + 1 > last) break;
                float gap = w[i].y - w[i + 1].y;
                float wid = w[i].x_end - w[i].x;
                int isolated = gap > out->leading * 1.55f;
                int shortish = wid < body_w * 0.85f;
                /*
                 * Duas folgas de tamanho, e nao uma.
                 *
                 * A regra por isolamento e AMBIGUA - titulo de capitulo tambem
                 * esta isolado e tambem e curto - entao ela exige corpo menor ou
                 * igual ao do texto. A regra por numero de pagina nao e ambigua:
                 * uma linha que e so "8" ou "xvii" nao e conteudo em nenhuma
                 * leitura, e ali a folga pode ser larga. Sem essa diferenca, o
                 * numero de pagina de 12 pt num livro de corpo 11 escapa - foi o
                 * caso da pagina 8 de "A Ilha do Tesouro Recortado".
                 */
                int small    = w[i].size <= out->body_size * 1.05f;
                int numsize  = w[i].size <= out->body_size * 1.40f;
                int pagenum  = line_is_pagenum(tl->buf + w[i].off, w[i].len);
                /* Numero de pagina cai mesmo sem isolamento: um numero solto no
                 * topo nao e texto em nenhuma leitura. */
                if ((small && isolated && shortish) || (numsize && k == 0 && pagenum)) {
                    w[i].drop = 1;
                    out->lines_dropped++;
                } else {
                    break;
                }
            }
            for (int k = 0; k < 2; ++k) {
                int i = last - k;
                if (i - 1 < first) break;
                float gap = w[i - 1].y - w[i].y;
                float wid = w[i].x_end - w[i].x;
                int isolated = gap > out->leading * 1.55f;
                int shortish = wid < body_w * 0.85f;
                /*
                 * Duas folgas de tamanho, e nao uma.
                 *
                 * A regra por isolamento e AMBIGUA - titulo de capitulo tambem
                 * esta isolado e tambem e curto - entao ela exige corpo menor ou
                 * igual ao do texto. A regra por numero de pagina nao e ambigua:
                 * uma linha que e so "8" ou "xvii" nao e conteudo em nenhuma
                 * leitura, e ali a folga pode ser larga. Sem essa diferenca, o
                 * numero de pagina de 12 pt num livro de corpo 11 escapa - foi o
                 * caso da pagina 8 de "A Ilha do Tesouro Recortado".
                 */
                int small    = w[i].size <= out->body_size * 1.05f;
                int numsize  = w[i].size <= out->body_size * 1.40f;
                int pagenum  = line_is_pagenum(tl->buf + w[i].off, w[i].len);
                if ((small && isolated && shortish) || (numsize && k == 0 && pagenum)) {
                    w[i].drop = 1;
                    out->lines_dropped++;
                } else {
                    break;
                }
            }
        }
    }

    /* --- montagem dos paragrafos ------------------------------------------ */
    int bl = 0;
    RfPara *cur = NULL;
    int prev = -1;          /* indice da ultima linha aceita */
    float cur_right = 0.0f; /* maior x_end do paragrafo em construcao */

    for (int i = 0; i < n; ++i) {
        if (w[i].drop || w[i].len <= 0)
            continue;

        const char *ls = tl->buf + w[i].off;
        int         ll = w[i].len;

        int start_new = 1;
        if (cur && prev >= 0 && w[i].col == w[prev].col) {
            float gap = w[prev].y - w[i].y;
            float dx  = w[i].x - w[prev].x;
            float ind_min = out->body_size * 0.55f;
            if (ind_min < 4.0f)
                ind_min = 4.0f;

            unsigned int plast, pprev2;
            int pbytes;
            last_two_cp(out->buf + cur->off, bl - cur->off, &plast, &pprev2, &pbytes);
            int prev_hyphen = cp_is_break_hyphen(plast);

            start_new = 0;

            if (gap <= 0.0f)
                start_new = 1;                        /* subiu: fim do bloco */
            else if (gap > out->leading * 1.55f)
                start_new = 1;                        /* linha em branco */
            /*
             * Mudanca de MARGEM, medida contra a linha anterior e nao contra a
             * margem da pagina.
             *
             * A diferenca decide se uma epigrafe sobrevive. Um bloco recuado -
             * epigrafe, citacao longa, verso - tem margem propria, e comparar
             * cada linha dele com a margem da PAGINA acusa recuo em todas: o
             * bloco sai quebrado linha por linha, que e o defeito que o reflow
             * deveria estar corrigindo. Comparado com a linha anterior, o bloco
             * acusa recuo so na entrada, que e onde ele realmente comeca.
             */
            else if (dx > ind_min)
                start_new = 1;                        /* entrou recuando */
            /*
             * Saida de bloco (a linha volta para a esquerda) so vale se a
             * anterior NAO era a primeira do paragrafo. Sem essa condicao, o
             * padrao mais comum da tipografia de livro - primeira linha
             * recuada, resto na margem - seria lido como duas margens
             * diferentes, e todo paragrafo quebraria depois da primeira linha.
             */
            else if (dx < -ind_min && cur->nlines > 1)
                start_new = 1;
            else if (fabsf(w[i].size - w[prev].size) > out->body_size * 0.18f)
                start_new = 1;                        /* mudou de corpo */
            else if (!prev_hyphen) {
                /*
                 * Fim de paragrafo por linha curta.
                 *
                 * Em texto justificado basta o quanto faltou para a margem. Em
                 * texto alinhado a esquerda isso nao serve, e a pergunta passa a
                 * ser: a PRIMEIRA PALAVRA desta linha teria caido na linha
                 * anterior? Se teria, a quebra foi intencional. A largura da
                 * palavra e estimada em 0,5 do corpo por caractere, que e a
                 * media de uma fonte de texto - aproximacao suficiente para uma
                 * decisao binaria.
                 *
                 * A MARGEM DE REFERENCIA e a do bloco, nao a da pagina. Uma
                 * citacao recuada dos dois lados tem medida propria e fecha bem
                 * antes da margem da pagina; medida contra a pagina, toda linha
                 * dela parece curta e o bloco quebra linha por linha. A medida
                 * do bloco e a maior linha que ele ja mostrou - incluindo a
                 * linha candidata, porque no primeiro par ainda nao ha historia
                 * suficiente e ignora-la fecharia o bloco antes de comecar.
                 */
                float right = cur_right;
                if (w[i].x_end > right)
                    right = w[i].x_end;

                if (justified) {
                    if (w[prev].x_end < right - out->body_size * 2.2f)
                        start_new = 1;
                } else {
                    float wordw = (float)first_word_cps(ls, ll) * out->body_size * 0.5f;
                    if (w[prev].x_end + out->body_size * 0.5f + wordw <
                        right - out->body_size * 0.5f)
                        start_new = 1;
                }
            }
        }

        if (start_new) {
            if (out->nparas >= RF_MAX_PARAS) {
                out->truncated = 1;
                break;
            }
            cur = &out->paras[out->nparas++];
            memset(cur, 0, sizeof(*cur));
            cur->off  = bl;
            cur->len  = 0;
            cur->size  = w[i].size;
            cur->x     = w[i].x;
            cur->x_end = w[i].x_end;
            cur->y     = w[i].y;
            cur->col   = w[i].col;
            cur_right  = w[i].x_end;
        }

        /* Uma chamada so para os dois casos: com o paragrafo recem-aberto,
         * cur->off == bl e reflow_append copia sem separador nenhum. */
        out->hyphen_joins += reflow_append(out->buf, bcap, &bl, cur->off,
                                           ls, ll, o->dehyphenate);

        cur->len = bl - cur->off;
        cur->nlines++;
        if (w[i].size > cur->size)
            cur->size = w[i].size;
        if (w[i].x_end > cur_right)
            cur_right = w[i].x_end;
        prev = i;
    }

    out->buflen = bl;

    /* --- rotulos por paragrafo -------------------------------------------- */
    for (int p = 0; p < out->nparas; ++p) {
        RfPara *pa = &out->paras[p];

        /*
         * Titulo e corpo maior E BLOCO CURTO.
         *
         * A condicao de tamanho sozinha nao basta porque o corpo modal e a
         * mediana das linhas, e ela erra em pagina que e metade texto, metade
         * tabela: as linhas da tabela sao mais numerosas, a mediana desce para o
         * corpo delas, e os doze paragrafos de texto de verdade passam a ser
         * "maiores que o corpo". Titulo de doze linhas nao existe, e essa e a
         * condicao que sobrevive a um corpo modal errado.
         */
        if (pa->size > out->body_size * 1.12f && pa->nlines <= 3)
            pa->flags |= RF_HEADING;

        /*
         * Centralizado: sobra nas duas pontas, e sobras parecidas.
         *
         * Restrito a bloco curto de proposito. Um paragrafo de dez linhas cuja
         * primeira linha por acaso tem folga simetrica e texto normal, e
         * centralizar dez linhas por causa disso e um erro bem visivel.
         */
        if (pa->nlines <= 3) {
            float left  = pa->x - out->body_x0;
            float right = out->body_x1 - pa->x_end;
            if (left > body_w * 0.06f && right > body_w * 0.06f &&
                fabsf(left - right) < body_w * 0.12f)
                pa->flags |= RF_CENTER;
        }
    }

    /* Emenda entre paginas: quem consome (o indice da Etapa 4) usa isso para
     * juntar o paragrafo cortado na virada. Aqui so marcamos o que se ve. */
    if (out->nparas > 0) {
        RfPara *f = &out->paras[0];
        if (cp_is_lower(first_cp(out->buf + f->off, f->len)))
            f->flags |= RF_CONT;

        RfPara *l = &out->paras[out->nparas - 1];
        unsigned int last, prev2;
        int lb;
        last_two_cp(out->buf + l->off, l->len, &last, &prev2, &lb);
        if (!cp_is_terminal(last))
            l->flags |= RF_OPEN;
    }

    if (tl->truncated)
        out->truncated = 1;
    return 0;
}
