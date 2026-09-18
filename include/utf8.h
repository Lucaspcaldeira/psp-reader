#ifndef EREADER_UTF8_H
#define EREADER_UTF8_H

#include <stddef.h>

/*
 * Decodificacao UTF-8.
 *
 * Por que isso e um modulo proprio e nao um detalhe escondido em font.c: todo
 * o pipeline deste projeto e UTF-8 de ponta a ponta - texto extraido do PDF,
 * reflow, medicao de largura, rasterizacao. Nao existe conversao para Latin-1
 * em nenhum ponto.
 *
 * Essa e a diferenca em relacao ao projeto midnight, que usa intraFont. A flag
 * INTRAFONT_STRING_ASCII do intraFont e na verdade Latin-1, 1 byte por glifo,
 * o que obriga uma conversao COM PERDA: travessao vira '-', aspas curvas viram
 * '"', reticencias viram tres pontos. Num jogo de dialogos curtos passa. Num
 * leitor de livros, essa e exatamente a pontuacao que aparece em toda pagina de
 * texto editorado. FreeType rasteriza qualquer codepoint, entao aqui nao ha
 * motivo para perder nada.
 */

/* U+FFFD REPLACEMENT CHARACTER. Devolvido para qualquer sequencia invalida. */
#define UTF8_REPLACEMENT 0xFFFDu

/*
 * Decodifica um codepoint a partir de *p e avanca *p para o proximo.
 *
 * INVARIANTE CRITICA: *p sempre avanca ao menos 1 byte quando *p < end. Um
 * decodificador que nao avanca em byte invalido trava o leitor num laco
 * infinito no meio de um livro - e livros de verdade tem bytes invalidos, seja
 * por PDF malformado, seja por arquivo truncado.
 *
 * Rejeita, devolvendo UTF8_REPLACEMENT:
 *   - continuacao solta (10xxxxxx no inicio)
 *   - sequencia truncada pelo fim do buffer
 *   - byte de continuacao faltando no meio
 *   - codificacao longa demais (overlong), ex. C0 80 para U+0000
 *   - surrogates U+D800..U+DFFF, que nao sao codepoints validos em UTF-8
 *   - acima de U+10FFFF
 */
unsigned int utf8_next(const char **p, const char *end);

/* Codifica cp em out (1 a 4 bytes). Retorna quantos bytes escreveu, ou 0 se
 * cp nao for um codepoint valido. Nao escreve terminador. */
int utf8_encode(unsigned int cp, char out[4]);

/* Quantos codepoints existem em [s, end). Sequencias invalidas contam 1. */
size_t utf8_count(const char *s, const char *end);

#endif
