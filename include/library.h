#ifndef EREADER_LIBRARY_H
#define EREADER_LIBRARY_H

/*
 * Biblioteca: listagem dos arquivos de ms0:/PSP/BOOKS/.
 *
 * E a tela inicial do app. Fica num modulo proprio, e nao no main.c, porque a
 * ordenacao e o filtro de extensao sao logica de verdade e vao crescer
 * (subpastas, ordenar por progresso de leitura, marcar livro comecado).
 */

#define LIB_MAX        128
#define LIB_NAME_MAX   128

typedef enum {
    LIB_FMT_UNKNOWN = 0,
    LIB_FMT_PDF,
    LIB_FMT_TXT,
    LIB_FMT_EPUB
} LibFormat;

typedef struct {
    char       name[LIB_NAME_MAX];   /* nome do arquivo, como veio do sceIo */
    unsigned int size;
    LibFormat  fmt;
} LibEntry;

typedef struct {
    LibEntry items[LIB_MAX];
    int count;
    int truncated;   /* 1 se havia mais de LIB_MAX arquivos */
    int err;         /* codigo de erro do sceIoDopen, 0 se ok */
} Library;

/* Varre `dir`, filtra por extensao conhecida e ordena por nome. */
void library_scan(Library *lib, const char *dir);

const char *library_fmt_name(LibFormat f);

#endif
