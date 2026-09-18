#ifndef EREADER_PDF_ARENA_H
#define EREADER_PDF_ARENA_H

#include <stddef.h>

/*
 * Alocador de arena para o grafo de objetos do PDF.
 *
 * Por que arena e nao malloc/free por no: um dicionario de PDF aninha
 * arbitrariamente, e um parser de descida recursiva com malloc por no precisa de
 * um free recursivo espelhado. Todo caminho de erro no meio da recursao passa a
 * ser um vazamento em potencial - e caminhos de erro sao a regra aqui, porque
 * PDF do mundo real e malformado com frequencia.
 *
 * Com arena, o descarte e uma chamada. O ciclo de vida vira explicito:
 *
 *   arena do documento   xref e trailer, vive enquanto o livro esta aberto
 *   arena de pagina      objetos de uma pagina, reciclada a cada virada
 *
 * A reciclagem por pagina e o que mantem o uso de memoria constante ao folhear
 * um livro de 500 paginas, em vez de crescer sem limite.
 */

typedef struct PdfArenaBlock PdfArenaBlock;

typedef struct {
    PdfArenaBlock *head;
    PdfArenaBlock *spare;    /* blocos reciclados por pdf_arena_reset() */
    size_t block_size;
    size_t reserved;         /* bytes pedidos ao sistema */
    size_t handed;           /* bytes entregues ao chamador */
    /*
     * Marca pegajosa de falha de alocacao.
     *
     * Um parser recursivo tem dezenas de pontos de alocacao. Checar cada um no
     * local transformaria o codigo em ruido de tratamento de erro, e um ponto
     * esquecido seria uma desreferencia de NULL. Em vez disso, quem constroi
     * objetos degrada para o singleton PDF_NULL e liga esta marca; o chamador
     * de nivel alto confere UMA vez, no fim.
     */
    int oom;
} PdfArena;

/* block_size 0 usa o padrao (16 KB). */
void pdf_arena_init(PdfArena *a, size_t block_size);

/*
 * Devolve memoria alinhada em 8 bytes, ou NULL (ligando `oom`).
 *
 * Alinhamento de 8 e nao 4 porque PdfObj guarda `double` e `long long`: no MIPS
 * do PSP um acesso desalinhado a 8 bytes nao e lento, e uma excecao de bus.
 *
 * Pedidos maiores que block_size ganham um bloco proprio, entao nao existe
 * tamanho maximo de alocacao.
 */
void *pdf_arena_alloc(PdfArena *a, size_t n);

/* Copia n bytes e acrescenta terminador. Util porque nomes e strings de PDF nao
 * sao terminados no arquivo. */
char *pdf_arena_dup(PdfArena *a, const void *src, size_t n);

/* Zera o uso mantendo os blocos para reaproveitamento: e isso que faz a virada
 * de pagina nao chamar o alocador do sistema. Tambem limpa `oom`. */
void pdf_arena_reset(PdfArena *a);

/* Devolve tudo ao sistema. */
void pdf_arena_free(PdfArena *a);

#endif
