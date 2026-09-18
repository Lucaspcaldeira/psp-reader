#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspdebug.h>
#include <string.h>
#include "gfx.h"

/* GfxVertex e GFX_VERTEX_FORMAT vivem em gfx.h: font.c monta o proprio batch
 * de sprites e precisa do mesmo formato. */

/* Display list. Alinhada em 16 bytes por exigencia da GE. */
static unsigned int __attribute__((aligned(16))) g_list[262144];

static unsigned int g_vram_top = 0;   /* alocador de VRAM: so avanca, nunca libera */
static void *g_fb0, *g_fb1, *g_zb;
static int   g_draw_on_fb0 = 1;
static int   g_filter_linear = 0;
static int   g_vsync = 1;

/*
 * Alocador de VRAM em bump pointer.
 * Os ponteiros de framebuffer no PSP sao OFFSETS relativos ao inicio da
 * eDRAM, nao enderecos. Por isso comecamos em 0 e so somamos.
 *
 * Orcamento (2 MB de VRAM):
 *   fb0  512*272*4 = 557 KB
 *   fb1  512*272*4 = 557 KB
 *   zb   512*272*2 = 278 KB
 *   -------------------------
 *   total          = 1.39 MB, sobrando ~650 KB
 */
static void *vram_alloc(unsigned int w, unsigned int h, unsigned int psm)
{
    unsigned int bpp;
    switch (psm) {
    case GU_PSM_T8:
        bpp = 1; break;
    case GU_PSM_5650:
    case GU_PSM_5551:
    case GU_PSM_4444:
    case GU_PSM_T16:
        bpp = 2; break;
    default:
        bpp = 4; break;
    }
    unsigned int size = w * h * bpp;
    void *result = (void *)g_vram_top;
    g_vram_top += size;
    return result;
}

void gfx_apply_sprite_state(void)
{
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);

    /* As duas linhas que importam: UV em pixels exige escala 1 e offset 0. */
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);

    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuColor(0xFFFFFFFF);
}

/*
 * Estado de render global, reaplicado A CADA FRAME de proposito: o estado do
 * GU persiste entre frames e um unico desenho que o suje contamina todos os
 * frames seguintes, com sintoma que nao aponta para a causa.
 */
static void apply_render_state(void)
{
    sceGuScissor(0, 0, GFX_SCR_W, GFX_SCR_H);
    sceGuEnable(GU_SCISSOR_TEST);

    /* 2D puro: sem teste de profundidade, a ordem de desenho decide. */
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_LIGHTING);

    sceGuShadeModel(GU_SMOOTH);
    gfx_apply_sprite_state();
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuAmbientColor(0xFFFFFFFF);
}

void gfx_init(void)
{
    g_fb0 = vram_alloc(GFX_BUF_W, GFX_SCR_H, GU_PSM_8888);
    g_fb1 = vram_alloc(GFX_BUF_W, GFX_SCR_H, GU_PSM_8888);
    g_zb  = vram_alloc(GFX_BUF_W, GFX_SCR_H, GU_PSM_4444);   /* 2 bytes/px */

    sceGuInit();
    sceGuStart(GU_DIRECT, g_list);

    sceGuDrawBuffer(GU_PSM_8888, g_fb0, GFX_BUF_W);
    sceGuDispBuffer(GFX_SCR_W, GFX_SCR_H, g_fb1, GFX_BUF_W);
    sceGuDepthBuffer(g_zb, GFX_BUF_W);

    /* O espaco de tela do GU e centrado em 2048,2048 (coordenadas fixas 12.4). */
    sceGuOffset(2048 - (GFX_SCR_W / 2), 2048 - (GFX_SCR_H / 2));
    sceGuViewport(2048, 2048, GFX_SCR_W, GFX_SCR_H);
    sceGuDepthRange(65535, 0);

    apply_render_state();

    sceGuFinish();
    sceGuSync(0, 0);

    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

void gfx_shutdown(void)
{
    sceGuTerm();
}

void gfx_set_filter_linear(int enable) { g_filter_linear = enable ? 1 : 0; }
void gfx_set_vsync(int enable)         { g_vsync = enable ? 1 : 0; }

void gfx_frame_begin(unsigned int clear_color)
{
    sceGuStart(GU_DIRECT, g_list);
    apply_render_state();
    sceGuClearColor(clear_color);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_FAST_CLEAR_BIT);
}

void gfx_frame_end(void)
{
    sceGuFinish();
    sceGuSync(0, 0);   /* bloqueia a CPU ate a GE terminar a lista */
}

void gfx_frame_present(void)
{
    if (g_vsync)
        sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
    g_draw_on_fb0 = !g_draw_on_fb0;
}

void gfx_dbg_begin(void)
{
    /* Aponta o debug screen pro buffer em que a GE acabou de desenhar,
     * senao o texto vai pro buffer exibido e pisca alternando frames. */
    void *target = g_draw_on_fb0 ? g_fb0 : g_fb1;
    pspDebugScreenSetOffset((int)target);
    pspDebugScreenSetXY(0, 0);
}

/*
 * Fatiar em 64 px nao e supersticao: e o que cabe no cache de textura da GE.
 * Um quad largo de uma vez derruba o hit rate; em fatias de 64 o hardware le
 * tudo quente. Some no emulador, aparece no console.
 */
#define SLICE 64.0f

static void draw_sliced(const Texture *t, float sx, float sy, float sw, float sh,
                        float x, float y, float w, float h, unsigned int color)
{
    gfx_apply_sprite_state();

    sceGuTexMode(GU_PSM_8888, 0, 0, t->swizzled);
    sceGuTexImage(0, t->pw, t->ph, t->pw, t->data);
    sceGuTexFilter(g_filter_linear ? GU_LINEAR : GU_NEAREST,
                   g_filter_linear ? GU_LINEAR : GU_NEAREST);

    float scale_x = w / sw;   /* pixels de tela por pixel de textura */

    for (float u = 0.0f; u < sw; u += SLICE) {
        float slice_w = (sw - u < SLICE) ? (sw - u) : SLICE;

        /* sceGuGetMemory aloca da propria display list: ideal pra vertices de
         * vida-de-um-frame, e ja vem alinhado. */
        GfxVertex *vtx = (GfxVertex *)sceGuGetMemory(2 * sizeof(GfxVertex));

        vtx[0].u = sx + u;
        vtx[0].v = sy;
        vtx[0].color = color;
        vtx[0].x = x + u * scale_x;
        vtx[0].y = y;
        vtx[0].z = 0.0f;

        vtx[1].u = sx + u + slice_w;
        vtx[1].v = sy + sh;
        vtx[1].color = color;
        vtx[1].x = x + (u + slice_w) * scale_x;
        vtx[1].y = y + h;
        vtx[1].z = 0.0f;

        /* GU_SPRITES: 2 vertices por retangulo, o hardware expande.
         * GU_TRANSFORM_2D: coordenadas ja em pixels de tela, sem matrizes. */
        sceGuDrawArray(GU_SPRITES, GFX_VERTEX_FORMAT, 2, 0, vtx);
    }
}

void gfx_draw_tex(const Texture *t, float x, float y, float w, float h,
                  unsigned int color)
{
    draw_sliced(t, 0.0f, 0.0f, (float)t->w, (float)t->h, x, y, w, h, color);
}

void gfx_draw_region(const Texture *t, float sx, float sy, float sw, float sh,
                     float x, float y, float w, float h, unsigned int color)
{
    draw_sliced(t, sx, sy, sw, sh, x, y, w, h, color);
}

void gfx_fill_rect(float x, float y, float w, float h, unsigned int color)
{
    gfx_apply_sprite_state();
    sceGuDisable(GU_TEXTURE_2D);

    GfxVertex *vtx = (GfxVertex *)sceGuGetMemory(2 * sizeof(GfxVertex));
    memset(vtx, 0, 2 * sizeof(GfxVertex));
    vtx[0].color = color;
    vtx[0].x = x;
    vtx[0].y = y;
    vtx[1].color = color;
    vtx[1].x = x + w;
    vtx[1].y = y + h;

    sceGuDrawArray(GU_SPRITES, GFX_VERTEX_FORMAT, 2, 0, vtx);

    sceGuEnable(GU_TEXTURE_2D);
}

unsigned int gfx_vram_used(void) { return g_vram_top; }
