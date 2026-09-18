#ifndef EREADER_ZIP_H
#define EREADER_ZIP_H

#include "pdf_io.h"
#include "pdf_arena.h"

/*
 * Leitor de ZIP, o suficiente para EPUB.
 *
 * Um EPUB e um ZIP: um container.xml apontando para um OPF, o OPF listando os
 * capitulos, e os capitulos em XHTML. Nada disso e legivel sem antes abrir o
 * ZIP, entao este modulo vem primeiro.
 *
 * O QUE ELE NAO FAZ, e por que:
 *
 *   - nao carrega o arquivo na memoria. Le o diretorio central por acesso
 *     direto no fim do arquivo, e cada membro sob demanda. Um EPUB pode ter
 *     centenas de MB de imagens que nunca vamos olhar.
 *   - nao descomprime membro inteiro. A leitura e em FLUXO, com janela fixa:
 *     e o que permite ler um capitulo de 2 MB num console com 16 MB de heap
 *     sem nunca ter 2 MB residentes. Ver zip_read_range.
 *   - so entende os dois metodos que aparecem em EPUB: 0 (armazenado, que o
 *     mimetype exige) e 8 (deflate). ZIP64, criptografia e os outros quinze
 *     metodos historicos sao recusados com clareza em vez de silenciosamente.
 *
 * Portavel: C99 + zlib sobre PdfIo. Testado no host.
 */

#define ZIP_MAX_ENTRIES  2048
#define ZIP_NAME_MAX     192

typedef struct {
    char      name[ZIP_NAME_MAX];
    long long lho;          /* offset do cabecalho local */
    long long csize;        /* tamanho comprimido */
    long long usize;        /* tamanho descomprimido */
    int       method;       /* 0 = armazenado, 8 = deflate */
} ZipEntry;

typedef struct {
    PdfStream st;
    ZipEntry *entries;
    int       nentries;
    int       truncated;    /* mais de ZIP_MAX_ENTRIES membros */
    char      err[64];
} Zip;

/*
 * Abre e le o diretorio central. `a` guarda a tabela de membros e precisa
 * sobreviver ao Zip. Em sucesso assume a posse do io. Retorna 0 em sucesso.
 */
int  zip_open(Zip *z, PdfArena *a, const PdfIo *io);
void zip_close(Zip *z);

/* Indice do membro pelo nome exato, ou -1. */
int  zip_find(const Zip *z, const char *name);

/*
 * Leitor SEQUENCIAL de um membro.
 *
 * Existe porque a alternativa - chamar zip_read_range em blocos sucessivos -
 * e quadratica: num deflate nao ha como chegar ao byte N sem descomprimir os N
 * anteriores, entao varrer 1 MB em blocos de 8 KB descomprimiria 64 MB. Com o
 * z_stream vivo entre as chamadas, a varredura custa uma passada.
 *
 * Cabe na pilha (uns 4 KB alem do estado do zlib).
 */
typedef struct {
    Zip      *z;
    int       idx;
    long long doff;      /* onde os dados comecam no arquivo */
    long long cpos;      /* bytes comprimidos ja lidos */
    long long upos;      /* bytes descomprimidos ja entregues */
    int       method;
    int       done;
    void     *zs;        /* z_stream*, opaco para nao vazar zlib.h no header */
    unsigned char zsmem[112];   /* o z_stream mora aqui: sem malloc */
    unsigned char in[4096];
} ZipReader;

int  zip_reader_open(Zip *z, int idx, ZipReader *r);
/* Devolve bytes entregues (0 no fim do membro), ou negativo em erro. */
int  zip_reader_read(ZipReader *r, void *dst, int cap);
void zip_reader_close(ZipReader *r);

/*
 * Le uma FAIXA do membro descomprimido: os bytes [from, from+cap).
 *
 * Implementado sobre o leitor sequencial, descartando o que vem antes de
 * `from`. Descomprimir e jogar fora e barato; guardar nao e - e o que permite
 * ler um capitulo de 2 MB num console de 16 MB sem nunca ter 2 MB residentes.
 *
 * Devolve quantos bytes escreveu em `dst`, ou negativo em erro.
 */
int  zip_read_range(Zip *z, int idx, long long from, void *dst, int cap);

/* Atalho: le o membro inteiro num buffer da arena. Recusa acima de `max`, que
 * e a defesa contra um membro que nao caberia na RAM do console. */
unsigned char *zip_read_all(Zip *z, PdfArena *a, int idx, int max, int *out_len);

#endif
