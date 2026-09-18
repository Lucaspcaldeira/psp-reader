#ifndef EREADER_TEXTURE_H
#define EREADER_TEXTURE_H

/*
 * Textura RGBA8888 na RAM principal.
 *
 * Duas restricoes do GU moldam essa struct:
 *  1) as dimensoes enviadas ao hardware precisam ser potencia de 2 (<= 512),
 *     entao guardamos pw/ph (padded) separado de w/h (imagem real). As UVs
 *     usam w/h; o buffer usa pw/ph.
 *  2) o buffer precisa de alinhamento de 16 bytes (pedimos 16).
 *
 * O campo `swizzled` existe porque este projeto tem os dois casos, ao
 * contrario do midnight (onde tudo era swizzled e o flag era hardcoded no
 * sceGuTexMode):
 *
 *   - assets estaticos (UI, icones): swizzled, mais rapidos de amostrar
 *   - atlas de glifos: LINEAR, porque escrevemos glifos nele em tempo de
 *     execucao. Swizzlar exigiria recalcular o endereco de cada pixel a cada
 *     glifo novo, e o ganho de amostragem em quads de ~14x18 px nao paga isso.
 */
typedef struct {
    unsigned int *data;
    int w, h;        /* dimensoes reais da imagem */
    int pw, ph;      /* dimensoes potencia de 2   */
    int swizzled;    /* 1 = layout swizzled do GU */
} Texture;

/* Aloca uma textura vazia (zerada), linear. pw/ph sao arredondados para a
 * proxima potencia de 2. Retorna 0 em sucesso, negativo em erro. */
int  tex_alloc(Texture *t, int w, int h);

void tex_free(Texture *t);

/* Total de bytes que as texturas alocadas ocupam na RAM principal. */
unsigned int tex_bytes_used(void);

#endif
