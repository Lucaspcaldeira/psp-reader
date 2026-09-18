#include "theme.h"
#include "gfx.h"

/*
 * As tres paletas.
 *
 * PAPEL nao e branco: 247,242,230 e um creme quente, porque branco puro numa
 * tela retroiluminada cansa em leitura longa - e a mesma razao pela qual papel
 * de livro nao e branco de escritorio. A tinta tambem nao e preta pelo mesmo
 * motivo.
 *
 * NOITE inverte, mas nao para preto e branco puros: fundo levemente azulado
 * escuro e tinta levemente quente. Branco puro sobre preto puro produz halo
 * ao redor das letras na LCD do PSP, que e o defeito que faz tema escuro
 * parecer borrado.
 *
 * CLARO e o unico com contraste maximo, e existe para uma situacao especifica:
 * sol. Ali o conforto nao importa, so enxergar.
 */
static const Theme THEMES[THEME_COUNT] = {
    {
        "Papel",
        GFX_RGBA(247, 242, 230, 255),   /* paper   */
        GFX_RGBA( 32,  30,  28, 255),   /* ink     */
        GFX_RGBA(120, 114, 104, 255),   /* dim     */
        GFX_RGBA( 32,  30,  28, 255),   /* bar     */
        GFX_RGBA(247, 242, 230, 255),   /* bar_txt */
        GFX_RGBA(170,  40,  40, 255),   /* err     */
        GFX_RGBA( 40, 110,  60, 255),   /* ok      */
        GFX_RGBA(216, 206, 184, 255)    /* sel     */
    },
    {
        "Noite",
        GFX_RGBA( 18,  18,  22, 255),
        GFX_RGBA(206, 200, 188, 255),
        GFX_RGBA(122, 118, 110, 255),
        GFX_RGBA(  8,   8,  10, 255),
        GFX_RGBA(176, 170, 158, 255),
        GFX_RGBA(214,  96,  88, 255),
        GFX_RGBA(110, 178, 120, 255),
        GFX_RGBA( 44,  44,  52, 255)
    },
    {
        "Claro",
        GFX_RGBA(255, 255, 255, 255),
        GFX_RGBA(  0,   0,   0, 255),
        GFX_RGBA(105, 105, 105, 255),
        GFX_RGBA(  0,   0,   0, 255),
        GFX_RGBA(255, 255, 255, 255),
        GFX_RGBA(190,   0,   0, 255),
        GFX_RGBA(  0, 120,   0, 255),
        GFX_RGBA(210, 210, 210, 255)
    }
};

const Theme *theme_get(int i)
{
    if (i < 0 || i >= THEME_COUNT)
        i = 0;
    return &THEMES[i];
}
