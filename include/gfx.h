#ifndef EREADER_GFX_H
#define EREADER_GFX_H

/* pspgu.h e necessario aqui, e nao so no .c: GFX_VERTEX_FORMAT expande para
 * constantes do GU e font.c inclui apenas este header. */
#include <pspgu.h>
#include "texture.h"

#define GFX_SCR_W    480
#define GFX_SCR_H    272
#define GFX_BUF_W    512   /* stride do framebuffer: 512, nao 480 */

#define GFX_RGBA(r,g,b,a) ((unsigned int)((a)<<24 | (b)<<16 | (g)<<8 | (r)))
#define GFX_WHITE  GFX_RGBA(255,255,255,255)
#define GFX_BLACK  GFX_RGBA(0,0,0,255)

/*
 * Vertice 2D. Fica no header porque font.c monta o proprio batch de sprites
 * (um draw call para uma linha inteira de texto, em vez de um por glifo) e
 * precisa do formato exato. Declarar duas vezes seria convidar divergencia
 * silenciosa entre os dois emissores.
 *
 * A ordem dos campos NAO e arbitraria: o GU espera texcoord, cor, posicao, na
 * ordem declarada no vertex format.
 *
 * Usamos as variantes de 32 bits (GU_TEXTURE_32BITF / GU_VERTEX_32BITF) e nao
 * as de 16 bits, que economizariam banda. Motivo: com u16/s16 a struct fica
 * com 14 bytes de campos e o compilador a alinha em 16 por causa do
 * `unsigned int color`, inserindo 2 bytes de padding no FIM. O GU le os
 * vertices empacotados, sem saber do padding, entao a partir do segundo
 * vertice tudo sai deslocado. Com floats os campos ja somam 24 bytes sem
 * padding nenhum.
 */
typedef struct {
    float u, v;
    unsigned int color;
    float x, y, z;
} GfxVertex;

#define GFX_VERTEX_FORMAT \
    (GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D)

void gfx_init(void);
void gfx_shutdown(void);

/*
 * Um frame tem tres passos separados de proposito, pra deixar visivel a ordem
 * real das operacoes no PSP:
 *
 *   gfx_frame_begin()    abre a display list e limpa o buffer de desenho
 *   ...desenhos...       enfileirados na display list, nada executou ainda
 *   gfx_frame_end()      envia a lista pra GE e ESPERA ela terminar
 *   ...overlay de debug...  CPU escrevendo direto no framebuffer, pos-GE
 *   gfx_frame_present()  espera o vblank e troca os buffers
 */
void gfx_frame_begin(unsigned int clear_color);
void gfx_frame_end(void);
void gfx_frame_present(void);

/* Filtro de amostragem. GU_LINEAR suaviza; para texto ja rasterizado com
 * antialiasing pelo FreeType, GU_NEAREST e o correto - o glifo e desenhado
 * 1:1 e filtrar de novo so borra. */
void gfx_set_filter_linear(int enable);

/* vsync ligado = espera o vblank antes de trocar buffers (trava em 60 fps). */
void gfx_set_vsync(int enable);

void gfx_draw_tex(const Texture *t, float x, float y, float w, float h,
                  unsigned int color);

void gfx_draw_region(const Texture *t, float sx, float sy, float sw, float sh,
                     float x, float y, float w, float h, unsigned int color);

/* Retangulo solido, sem textura. */
void gfx_fill_rect(float x, float y, float w, float h, unsigned int color);

/*
 * Estado minimo que todo desenho 2D assume. Publico porque font.c faz o
 * proprio batch de sprites (um unico draw call para uma linha inteira de
 * texto, em vez de passar glifo por glifo pelo gfx_draw_region) e precisa
 * garantir o mesmo estado antes de emitir.
 */
void gfx_apply_sprite_state(void);

/* Escreve texto de debug direto no framebuffer que acabou de ser desenhado.
 * So valido entre gfx_frame_end() e gfx_frame_present(). */
void gfx_dbg_begin(void);

unsigned int gfx_vram_used(void);

#endif
