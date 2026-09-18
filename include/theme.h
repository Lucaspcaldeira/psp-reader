#ifndef EREADER_THEME_H
#define EREADER_THEME_H

/*
 * Paleta de leitura.
 *
 * Nao e enfeite num leitor de livros. A tela do PSP e retroiluminada e nao
 * muda: o mesmo fundo claro que e confortavel de dia e ofuscante num quarto
 * escuro, que e onde metade da leitura acontece. Um leitor sem tema noturno
 * obriga a escolher entre ler e nao incomodar quem esta do lado.
 *
 * Tres, e nao uma escala continua: a escolha e entre situacoes de luz
 * distintas, nao um ajuste fino, e uma lista de tres cabe num botao.
 */

#define THEME_COUNT 3

typedef struct {
    const char  *name;
    unsigned int paper;    /* fundo da area de texto */
    unsigned int ink;      /* corpo do texto */
    unsigned int dim;      /* texto secundario */
    unsigned int bar;      /* fundo das barras de topo e rodape */
    unsigned int bar_txt;
    unsigned int err;
    unsigned int ok;
    unsigned int sel;      /* realce de selecao e fundo de painel */
} Theme;

/* Indice fora da faixa devolve o tema 0 - o arquivo de progresso vem do
 * Memory Stick e pode trazer qualquer numero. */
const Theme *theme_get(int i);

#endif
