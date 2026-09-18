#include <pspkernel.h>
#include <pspgu.h>
#include <string.h>
#include <math.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "font.h"
#include "gfx.h"
#include "texture.h"
#include "utf8.h"

/*
 * Atlas de 512x512 RGBA8888 = 1 MB na RAM principal.
 *
 * 512 e o maximo que o GU aceita numa dimensao de textura. A 18 px, uma celula
 * de glifo mede ~14x24, o que da ~36 colunas por ~21 linhas = ~750 glifos - com
 * folga confortavel para os ~200 codepoints distintos que um livro em portugues
 * usa de fato.
 *
 * Por que 8888 e nao T8+CLUT, que ocuparia 256 KB: o glifo e pura cobertura
 * (um canal), entao T8 seria 4x menor. Mas T8 exige carregar e manter uma CLUT
 * de rampa alfa e acerta-la junto de todo desenho de textura colorida do resto
 * da UI. 1 MB dentro de um orcamento de 16 MB nao justifica esse acoplamento.
 * Se o orcamento apertar, esta e a primeira otimizacao obvia.
 */
#define ATLAS_DIM   512
#define ATLAS_PAD   1        /* 1 px entre glifos: evita sangramento de vizinho */

/* Potencia de 2 e >= ao maximo de glifos que o atlas comporta, com folga.
 * O atlas enche muito antes desta tabela. */
#define GLYPH_SLOTS 1024

typedef struct {
    unsigned int cp;     /* 0 = slot vazio (codepoint 0 nunca e rasterizado) */
    short ax, ay;        /* canto superior esquerdo no atlas */
    short w, h;          /* dimensoes do bitmap */
    short bx, by;        /* bitmap_left / bitmap_top */
    float adv;           /* avanco horizontal em pixels */
} Glyph;

static FT_Library g_ft;
static FT_Face    g_face;
static int        g_ready = 0;
static int        g_px    = 18;

static Texture g_atlas;
static Glyph   g_slots[GLYPH_SLOTS];
static int     g_count = 0;

/* Empacotamento em prateleira: preenche uma faixa horizontal, desce. */
static int g_pen_x = ATLAS_PAD;
static int g_pen_y = ATLAS_PAD;
static int g_row_h = 0;
static int g_full  = 0;

static float g_line_h = 0.0f, g_asc = 0.0f, g_desc = 0.0f;

static char g_family[64] = "(nenhuma)";
static char g_style[64]  = "";

/* ------------------------------------------------------------------------- */

static unsigned int hash_cp(unsigned int cp)
{
    /* Knuth multiplicativo. Codepoints reais sao densos em faixas (ASCII,
     * Latin-1 Supplement, Punctuation), e mascarar o codepoint cru agruparia
     * tudo em poucos buckets. */
    return (cp * 2654435761u) >> 8;
}

static void atlas_reset(void)
{
    memset(g_slots, 0, sizeof(g_slots));
    g_count = 0;
    g_pen_x = ATLAS_PAD;
    g_pen_y = ATLAS_PAD;
    g_row_h = 0;
    g_full  = 0;
    if (g_atlas.data)
        memset(g_atlas.data, 0, (unsigned int)g_atlas.pw * g_atlas.ph * 4u);
}

static void read_metrics(void)
{
    /* As metricas do FreeType vem em 26.6 de ponto fixo: 64 unidades = 1 px. */
    g_line_h = (float)g_face->size->metrics.height    / 64.0f;
    g_asc    = (float)g_face->size->metrics.ascender  / 64.0f;
    g_desc   = (float)g_face->size->metrics.descender / 64.0f;
}

/*
 * Rasteriza um codepoint e o insere no atlas.
 * Devolve NULL se o glifo nao existe na fonte ou se o atlas encheu.
 */
static Glyph *glyph_insert(unsigned int cp, unsigned int slot)
{
    if (FT_Load_Char(g_face, cp, FT_LOAD_RENDER) != 0)
        return NULL;

    FT_GlyphSlot gs = g_face->glyph;
    FT_Bitmap   *bm = &gs->bitmap;

    int bw = (int)bm->width;
    int bh = (int)bm->rows;

    /* Espaco e outros glifos sem tinta: nao ocupam area no atlas, mas o avanco
     * conta. Registrar com w=h=0 evita rasterizar de novo a cada espaco. */
    if (bw <= 0 || bh <= 0) {
        Glyph *g = &g_slots[slot];
        g->cp = cp;
        g->ax = g->ay = g->w = g->h = 0;
        g->bx = (short)gs->bitmap_left;
        g->by = (short)gs->bitmap_top;
        g->adv = (float)gs->advance.x / 64.0f;
        g_count++;
        return g;
    }

    if (bw > ATLAS_DIM - 2 * ATLAS_PAD || bh > ATLAS_DIM - 2 * ATLAS_PAD)
        return NULL;   /* glifo maior que o atlas inteiro */

    /* Nova prateleira se nao cabe na atual. */
    if (g_pen_x + bw + ATLAS_PAD > ATLAS_DIM) {
        g_pen_x = ATLAS_PAD;
        g_pen_y += g_row_h + ATLAS_PAD;
        g_row_h = 0;
    }
    if (g_pen_y + bh + ATLAS_PAD > ATLAS_DIM) {
        /* Atlas cheio. NAO resetamos aqui: a display list deste frame pode ja
         * referenciar glifos nas posicoes antigas, e reescrever o atlas por
         * baixo dela trocaria letras na tela. O reset seguro so pode acontecer
         * entre frames - ver font_set_px(). Aqui apenas paramos de aceitar
         * glifos novos; o texto perde os caracteres ineditos mas nao corrompe
         * o que ja funciona. */
        g_full = 1;
        return NULL;
    }

    int ax = g_pen_x;
    int ay = g_pen_y;

    /*
     * Copia a cobertura para o canal alfa, com RGB fixo em branco.
     *
     * Com GU_TFX_MODULATE isso da exatamente a mistura correta: o RGB final vem
     * da cor do vertice e o alfa vem da cobertura do glifo vezes o alfa da cor.
     * Deixar RGB em branco e o que permite recolorir o texto sem tocar no atlas.
     */
    for (int row = 0; row < bh; ++row) {
        /* pitch e assinado: pode ser negativo se o bitmap for bottom-up. */
        const unsigned char *src = bm->buffer + (long)row * bm->pitch;
        unsigned int *dst = g_atlas.data + (long)(ay + row) * g_atlas.pw + ax;
        for (int col = 0; col < bw; ++col)
            dst[col] = ((unsigned int)src[col] << 24) | 0x00FFFFFFu;
    }

    /*
     * A GE le a textura da RAM principal sem passar pelo cache de dados da CPU.
     * Sem este writeback, os pixels do glifo ainda estao na cache e a GE le
     * lixo - o sintoma e glifo faltando ou embaralhado apenas na PRIMEIRA vez
     * que cada caractere aparece, o que aponta para todo lado menos para cache.
     */
    sceKernelDcacheWritebackRange(g_atlas.data,
                                  (unsigned int)g_atlas.pw * g_atlas.ph * 4u);

    g_pen_x += bw + ATLAS_PAD;
    if (bh > g_row_h)
        g_row_h = bh;

    Glyph *g = &g_slots[slot];
    g->cp  = cp;
    g->ax  = (short)ax;
    g->ay  = (short)ay;
    g->w   = (short)bw;
    g->h   = (short)bh;
    g->bx  = (short)gs->bitmap_left;
    g->by  = (short)gs->bitmap_top;
    g->adv = (float)gs->advance.x / 64.0f;
    g_count++;
    return g;
}

static Glyph *glyph_get(unsigned int cp)
{
    if (!g_ready || cp == 0)
        return NULL;

    unsigned int mask = GLYPH_SLOTS - 1;
    unsigned int i = hash_cp(cp) & mask;

    /* Sondagem linear. O limite de GLYPH_SLOTS iteracoes existe para o caso
     * degenerado de tabela cheia: sem ele, a busca por um codepoint ausente
     * numa tabela lotada nunca termina. */
    for (unsigned int probe = 0; probe < GLYPH_SLOTS; ++probe) {
        Glyph *g = &g_slots[i];
        if (g->cp == cp)
            return g;
        if (g->cp == 0)
            return glyph_insert(cp, i);
        i = (i + 1) & mask;
    }
    return NULL;
}

/* Glifo de fallback para codepoint ausente na fonte. U+FFFD se a fonte tiver;
 * senao '?'. Devolver NULL faria o caractere desaparecer sem deixar rastro. */
static Glyph *glyph_or_fallback(unsigned int cp)
{
    Glyph *g = glyph_get(cp);
    if (g)
        return g;
    g = glyph_get(0xFFFD);
    if (g)
        return g;
    return glyph_get('?');
}

/* ------------------------------------------------------------------------- */

int font_init(const char *ttf_path, int px)
{
    if (px < FONT_PX_MIN || px > FONT_PX_MAX)
        return -4;

    if (FT_Init_FreeType(&g_ft) != 0)
        return -1;

    if (FT_New_Face(g_ft, ttf_path, 0, &g_face) != 0) {
        FT_Done_FreeType(g_ft);
        return -2;
    }

    if (tex_alloc(&g_atlas, ATLAS_DIM, ATLAS_DIM) != 0) {
        FT_Done_Face(g_face);
        FT_Done_FreeType(g_ft);
        return -3;
    }

    /* O atlas e escrito em tempo de execucao, glifo por glifo, entao fica
     * LINEAR. Swizzlar exigiria recalcular o endereco de cada pixel a cada
     * insercao, e o ganho de amostragem em quads de ~14x24 nao paga isso. */
    g_atlas.swizzled = 0;

    if (g_face->family_name)
        snprintf(g_family, sizeof(g_family), "%s", g_face->family_name);
    if (g_face->style_name)
        snprintf(g_style, sizeof(g_style), "%s", g_face->style_name);

    g_px = px;
    FT_Set_Pixel_Sizes(g_face, 0, (FT_UInt)px);
    read_metrics();

    atlas_reset();
    g_ready = 1;
    return 0;
}

void font_shutdown(void)
{
    if (!g_ready)
        return;
    tex_free(&g_atlas);
    FT_Done_Face(g_face);
    FT_Done_FreeType(g_ft);
    g_ready = 0;
}

int font_ready(void) { return g_ready; }
int font_px(void)    { return g_px; }

int font_set_px(int px)
{
    if (!g_ready)
        return -1;
    if (px < FONT_PX_MIN || px > FONT_PX_MAX)
        return -4;
    if (px == g_px)
        return 0;

    g_px = px;
    if (FT_Set_Pixel_Sizes(g_face, 0, (FT_UInt)px) != 0)
        return -5;
    read_metrics();

    /* Todo glifo cacheado tem as metricas do corpo antigo: o atlas inteiro
     * precisa cair. Seguro aqui porque font_set_px() e chamada entre frames,
     * fora de qualquer display list em construcao. */
    atlas_reset();
    return 0;
}

float font_line_height(void) { return g_line_h; }
float font_ascender(void)    { return g_asc; }
float font_descender(void)   { return g_desc; }

int          font_glyphs_cached(void) { return g_count; }
int          font_atlas_full(void)    { return g_full; }
const char  *font_family(void)        { return g_family; }
const char  *font_style(void)         { return g_style; }
unsigned int font_atlas_bytes(void)
{
    return g_atlas.data ? (unsigned int)g_atlas.pw * g_atlas.ph * 4u : 0u;
}

float font_measure(const char *utf8, int nbytes)
{
    if (!g_ready || !utf8)
        return 0.0f;

    const char *p   = utf8;
    const char *end = utf8 + (nbytes < 0 ? (int)strlen(utf8) : nbytes);
    float w = 0.0f;

    while (p < end) {
        unsigned int cp = utf8_next(&p, end);
        Glyph *g = glyph_or_fallback(cp);
        if (g)
            w += g->adv;
    }
    return w;
}

int font_line_break(const char *utf8, int nbytes, float max_w, float *out_w)
{
    if (out_w)
        *out_w = 0.0f;
    if (!g_ready || !utf8)
        return 0;

    const char *start = utf8;
    const char *end   = utf8 + (nbytes < 0 ? (int)strlen(utf8) : nbytes);
    const char *p     = start;

    float w = 0.0f;
    int   last_break_bytes = 0;    /* bytes ate a ultima fronteira de palavra */
    float last_break_w     = 0.0f;

    while (p < end) {
        const char *before = p;
        unsigned int cp = utf8_next(&p, end);

        /* Quebra explicita encerra a linha aqui, consumindo o '\n'. */
        if (cp == '\n') {
            if (out_w)
                *out_w = w;
            return (int)(p - start);
        }

        Glyph *g = glyph_or_fallback(cp);
        float adv = g ? g->adv : 0.0f;

        if (w + adv > max_w) {
            if (last_break_bytes > 0) {
                if (out_w)
                    *out_w = last_break_w;
                return last_break_bytes;
            }
            /*
             * Nenhuma fronteira de palavra couber significa palavra unica mais
             * larga que a linha - URL, hash, palavra composta em alemao. Quebrar
             * dentro dela e obrigatorio: devolver 0 aqui faria o laco de
             * paginacao nunca avancar e o leitor congelaria nessa palavra.
             */
            if (before == start) {
                if (out_w)
                    *out_w = adv;
                return (int)(p - start);   /* ao menos um codepoint */
            }
            if (out_w)
                *out_w = w;
            return (int)(before - start);
        }

        w += adv;

        /* Fronteira de palavra: DEPOIS do espaco, para o espaco ficar no fim da
         * linha e nao no comeco da seguinte. */
        if (cp == ' ' || cp == '\t') {
            last_break_bytes = (int)(p - start);
            last_break_w     = w;
        }
    }

    if (out_w)
        *out_w = w;
    return (int)(end - start);
}

float font_draw(float x, float y, const char *utf8, int nbytes, unsigned int color)
{
    if (!g_ready || !utf8)
        return x;

    const char *end = utf8 + (nbytes < 0 ? (int)strlen(utf8) : nbytes);
    if (utf8 >= end)
        return x;

    /*
     * Passo 1: garantir que todo glifo esta no atlas, e contar quantos tem
     * tinta. Isso acontece ANTES de qualquer vertice ser emitido de proposito -
     * assim o numero de sprites e conhecido e a string inteira sai em UM draw
     * call, em vez de um por glifo.
     */
    int sprites = 0;
    {
        const char *p = utf8;
        while (p < end) {
            unsigned int cp = utf8_next(&p, end);
            Glyph *g = glyph_or_fallback(cp);
            if (g && g->w > 0 && g->h > 0)
                sprites++;
        }
    }

    float pen = x;
    if (sprites == 0) {
        /* Só espacos: nada a desenhar, mas o pen ainda avanca. */
        const char *p = utf8;
        while (p < end) {
            Glyph *g = glyph_or_fallback(utf8_next(&p, end));
            if (g)
                pen += g->adv;
        }
        return pen;
    }

    gfx_apply_sprite_state();
    /* Ultimo argumento 0: atlas linear, nao swizzled. */
    sceGuTexMode(GU_PSM_8888, 0, 0, g_atlas.swizzled);
    sceGuTexImage(0, g_atlas.pw, g_atlas.ph, g_atlas.pw, g_atlas.data);
    /* GU_NEAREST e o correto aqui: o FreeType ja entregou o glifo com
     * antialiasing, no tamanho exato em que sera desenhado. Filtrar de novo so
     * borra o que ja estava certo. */
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);

    GfxVertex *vtx = (GfxVertex *)sceGuGetMemory(2 * sprites * sizeof(GfxVertex));
    int nv = 0;

    const char *p = utf8;
    while (p < end) {
        unsigned int cp = utf8_next(&p, end);
        Glyph *g = glyph_or_fallback(cp);
        if (!g)
            continue;

        if (g->w > 0 && g->h > 0) {
            /*
             * Arredondar a posicao para pixel inteiro. Glifo em coordenada
             * fracionaria com GU_NEAREST cai entre texels e o texto sai
             * tremido de linha para linha - em leitura longa isso cansa a
             * vista muito antes de parecer "errado" numa captura de tela.
             */
            float gx = floorf(pen + 0.5f) + (float)g->bx;
            float gy = floorf(y   + 0.5f) - (float)g->by;

            vtx[nv].u = (float)g->ax;
            vtx[nv].v = (float)g->ay;
            vtx[nv].color = color;
            vtx[nv].x = gx;
            vtx[nv].y = gy;
            vtx[nv].z = 0.0f;
            nv++;

            vtx[nv].u = (float)(g->ax + g->w);
            vtx[nv].v = (float)(g->ay + g->h);
            vtx[nv].color = color;
            vtx[nv].x = gx + (float)g->w;
            vtx[nv].y = gy + (float)g->h;
            vtx[nv].z = 0.0f;
            nv++;
        }
        pen += g->adv;
    }

    if (nv > 0)
        sceGuDrawArray(GU_SPRITES, GFX_VERTEX_FORMAT, nv, 0, vtx);

    return pen;
}
