#ifndef EREADER_FONT_H
#define EREADER_FONT_H

/*
 * Texto via FreeType, com atlas de glifos em textura.
 *
 * Substitui o intraFont do projeto midnight por dois motivos:
 *
 *  1) LICENCA. intraFont e CC BY-SA 3.0 (share-alike viral); este projeto e
 *     MIT. FreeType e FTL, permissiva. Ver PLAN.md secao 2.
 *
 *  2) UNICODE. O intraFont nao tem modo UTF-8 - sua flag STRING_ASCII e
 *     Latin-1, 1 byte por glifo, o que obriga conversao com perda. Num leitor
 *     de livros isso destroi exatamente a pontuacao que aparece em toda pagina
 *     de texto editorado: travessao, aspas curvas, reticencias. Aqui o texto e
 *     UTF-8 puro do PDF ate o pixel.
 *
 * Bonus: corpo de fonte de verdade. Mudar o tamanho do texto e uma funcao
 * obrigatoria de um leitor, e com .pgf o escalonamento e de bitmap.
 */

#define FONT_PX_MIN 12
#define FONT_PX_MAX 34

/* Carrega o TTF e prepara o atlas no corpo `px`. Retorna 0 em sucesso.
 *   -1 falha ao inicializar o FreeType
 *   -2 arquivo nao encontrado ou formato desconhecido
 *   -3 falha ao alocar o atlas
 *   -4 corpo de fonte fora de FONT_PX_MIN..FONT_PX_MAX
 */
int  font_init(const char *ttf_path, int px);
void font_shutdown(void);
int  font_ready(void);

/* Troca o corpo da fonte. Invalida o atlas inteiro (as metricas de todo glifo
 * mudam), entao a primeira pagina depois disso rasteriza tudo de novo.
 * Retorna 0 em sucesso. */
int  font_set_px(int px);
int  font_px(void);

/* Metricas do corpo atual, em pixels. line_height ja e o avanco de linha
 * recomendado pela fonte; o leitor aplica seu proprio multiplicador em cima. */
float font_line_height(void);
float font_ascender(void);    /* positivo, do baseline para cima */
float font_descender(void);   /* negativo, do baseline para baixo */

/* nbytes < 0 significa "ate o terminador". */
float font_measure(const char *utf8, int nbytes);

/*
 * Quebra de linha por largura real de glifo, nao por contagem de caracteres.
 * Devolve quantos BYTES entram nesta linha, quebrando na ultima fronteira de
 * palavra que couber. Em *out_w (se nao-NULL) vai a largura consumida.
 *
 * Se uma unica palavra nao couber em max_w, quebra dentro dela - do contrario o
 * laco de paginacao nao progride e o leitor congela numa URL longa.
 */
int  font_line_break(const char *utf8, int nbytes, float max_w, float *out_w);

/*
 * Desenha. `y` e a BASELINE, nao o topo: use font_ascender() para converter de
 * uma coordenada de topo. Retorna o x final (pen depois do ultimo glifo).
 *
 * Emite um unico draw call para toda a string.
 */
float font_draw(float x, float y, const char *utf8, int nbytes, unsigned int color);

/* --- diagnostico, para o overlay de debug --- */
int          font_glyphs_cached(void);
int          font_atlas_full(void);
const char  *font_family(void);
const char  *font_style(void);
unsigned int font_atlas_bytes(void);

#endif
