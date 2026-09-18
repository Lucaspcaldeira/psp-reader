#ifndef EREADER_PDF_OBJ_H
#define EREADER_PDF_OBJ_H

#include "pdf_arena.h"

/*
 * Modelo de objeto do PDF.
 *
 * Um arquivo PDF e, em essencia, um grafo de oito tipos de objeto ligados por
 * referencias indiretas ("12 0 R"). Tudo mais - paginas, fontes, o texto - e
 * convencao construida sobre isso.
 */

typedef enum {
    PDF_NULL = 0,
    PDF_BOOL,
    PDF_INT,
    PDF_REAL,
    PDF_STR,      /* string literal (...) ou hexadecimal <...>, ja decodificada */
    PDF_NAME,     /* /Name, sem a barra, com #xx ja resolvido */
    PDF_ARR,
    PDF_DICT,
    PDF_STREAM,   /* dicionario + dados crus no arquivo */
    PDF_REF       /* referencia indireta, ainda nao resolvida */
} PdfKind;

typedef struct PdfObj PdfObj;

/*
 * Fatia de bytes. Strings de PDF NAO sao terminadas em zero no arquivo e podem
 * conter bytes nulos legitimamente (texto UTF-16BE, por exemplo), entao o
 * comprimento e obrigatorio. `p` tem terminador acrescentado pela arena, o que
 * torna seguro passar para strcmp quando se sabe que nao ha nulos internos.
 */
typedef struct {
    char *p;
    int   len;
} PdfSlice;

typedef struct {
    PdfObj **v;
    int len, cap;
} PdfArr;

/*
 * Dicionario como vetores paralelos com busca linear.
 *
 * Sem tabela hash de proposito: dicionarios de PDF tem tipicamente 3 a 10
 * entradas, e uma varredura linear sobre memoria contigua ganha de hash nesse
 * tamanho - alem de nao gastar bytes com buckets. Um /Resources gordo com 40
 * entradas ainda e uma varredura trivial.
 */
typedef struct {
    PdfSlice *keys;
    PdfObj  **vals;
    int len, cap;
} PdfDict;

struct PdfObj {
    unsigned char kind;
    union {
        int       b;
        long long i;
        double    r;
        PdfSlice  s;
        PdfArr    a;
        PdfDict   d;
        struct {
            PdfObj   *dict;
            long long off;   /* offset do primeiro byte de dados no arquivo */
            long long len;   /* /Length resolvido, ou -1 se ainda desconhecido */
        } stm;
        struct { int num, gen; } ref;
    } u;
};

/*
 * Singleton imutavel para "ausente" e para degradacao em falta de memoria.
 *
 * Devolver isto em vez de NULL e o que permite ao parser recursivo nao checar
 * alocacao em todo ponto: o grafo continua percorrivel, so fica incompleto, e a
 * marca `oom` da arena registra o que aconteceu.
 */
const PdfObj *pdf_null(void);

PdfObj *pdf_new(PdfArena *a, PdfKind k);

int pdf_arr_push(PdfArena *a, PdfObj *arr, PdfObj *v);

/* Chaves repetidas: a ULTIMA vence, que e o que os leitores de PDF fazem na
 * pratica com dicionarios malformados. */
int pdf_dict_put(PdfArena *a, PdfObj *d, PdfSlice key, PdfObj *v);

/* Devolve NULL se ausente (e nao o singleton null: quem chama precisa
 * distinguir "chave ausente" de "chave presente com valor null"). */
PdfObj *pdf_dict_get(const PdfObj *d, const char *name);

/* --- acessores tolerantes -------------------------------------------------
 * Todos aceitam NULL e tipo errado, devolvendo o padrao. PDF do mundo real tem
 * tipos trocados com frequencia; um acessor que aborta transforma cada
 * inconsistencia num travamento. */

int       pdf_is(const PdfObj *o, PdfKind k);
long long pdf_int(const PdfObj *o, long long def);
double    pdf_real(const PdfObj *o, double def);
int       pdf_bool(const PdfObj *o, int def);

/* Compara um PDF_NAME com um literal C. */
int pdf_name_is(const PdfObj *o, const char *name);

/* Numero de itens de um array; 0 se nao for array. */
int     pdf_arr_len(const PdfObj *o);
PdfObj *pdf_arr_get(const PdfObj *o, int i);

/* O dicionario de um PDF_STREAM, ou o proprio objeto se for PDF_DICT.
 * Existe porque quase todo uso de /Type, /Length etc. quer os dois casos. */
const PdfObj *pdf_as_dict(const PdfObj *o);

#endif
