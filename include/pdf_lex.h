#ifndef EREADER_PDF_LEX_H
#define EREADER_PDF_LEX_H

#include "pdf_io.h"
#include "pdf_obj.h"

/*
 * Tokenizer e parser de objetos do PDF.
 *
 * Existe uma camada de token separada, em vez de descida recursiva direta sobre
 * bytes, porque content stream (Etapa 3b) e a MESMA sintaxe com uma diferenca:
 * operandos vem antes do operador, em notacao pos-fixa. Com tokens, o
 * interpretador de content stream reaproveita este lexer inteiro e so muda a
 * gramatica de cima.
 */

typedef enum {
    PT_EOF = 0,
    PT_INT,
    PT_REAL,
    PT_STR,         /* (literal) ou <hex>, ja decodificada */
    PT_NAME,        /* /Nome, sem a barra, com #xx resolvido */
    PT_KEYWORD,     /* obj, endobj, stream, R, true, false, null, Tj, ... */
    PT_ARR_OPEN,    /* [ */
    PT_ARR_CLOSE,   /* ] */
    PT_DICT_OPEN,   /* << */
    PT_DICT_CLOSE,  /* >> */
    PT_BRACE_OPEN,  /* { - so aparece em funcoes PostScript */
    PT_BRACE_CLOSE, /* } */
    PT_JUNK         /* byte que nao inicia token valido; consumido, ignoravel */
} PdfTokKind;

typedef struct {
    PdfTokKind kind;
    long long  i;      /* PT_INT */
    double     r;      /* PT_REAL */
    PdfSlice   s;      /* PT_STR, PT_NAME, PT_KEYWORD */
} PdfTok;

/* Pula espacos e comentarios. */
void pdf_skip_ws(PdfStream *st);

/*
 * Proximo token. Sempre progride: um byte que nao inicia token valido e
 * consumido e devolvido como PT_JUNK.
 *
 * Sem essa garantia, um byte inesperado no meio de um PDF malformado prende o
 * parser num laco infinito - e o sintoma no console e o console travado, sem
 * nenhuma pista.
 */
int pdf_lex_next(PdfStream *st, PdfArena *a, PdfTok *t);

/*
 * Proximo objeto completo.
 *
 * Reconhece "N G R" com lookahead de dois tokens, rebobinando quando o padrao
 * nao fecha. Um dicionario seguido da palavra `stream` vira PDF_STREAM com o
 * offset dos dados; o comprimento fica -1 quando /Length e referencia
 * indireta, porque resolver exige o xref - que por sua vez e lido com este
 * mesmo parser. Quem precisa dos bytes chama pdf_doc_stream_data().
 */
PdfObj *pdf_parse(PdfStream *st, PdfArena *a);

/* Como pdf_parse, mas o primeiro token ja foi lido. */
PdfObj *pdf_parse_from(PdfStream *st, PdfArena *a, const PdfTok *first);

/* Compara um PT_KEYWORD com um literal. */
int pdf_tok_is(const PdfTok *t, const char *kw);

/*
 * Procura a proxima ocorrencia de `needle` a partir de `from`, ate no maximo
 * `limit` bytes. Devolve o offset, ou -1.
 *
 * Usado para achar `endstream` quando /Length mente (acontece), e `startxref`
 * no fim do arquivo.
 */
long long pdf_find(PdfStream *st, long long from, long long limit,
                   const char *needle);

/* Como pdf_find, mas varrendo para tras a partir de `from`. */
long long pdf_rfind(PdfStream *st, long long from, long long limit,
                    const char *needle);

/* Classificacao de bytes do PDF, exposta porque o interpretador de content
 * stream tambem precisa. */
int pdf_is_ws(int c);
int pdf_is_delim(int c);

#endif
