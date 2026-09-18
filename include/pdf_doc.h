#ifndef EREADER_PDF_DOC_H
#define EREADER_PDF_DOC_H

#include "pdf_io.h"
#include "pdf_obj.h"
#include "pdf_filt.h"

/*
 * Documento PDF: tabela de referencia cruzada, trailer e resolucao de objetos.
 *
 * Esta camada e onde vive a tolerancia a arquivo malformado, que o PLAN.md
 * seccao 9 aponta como o maior risco do projeto. Tres defesas concretas:
 *
 *   - /Length de stream tratado como dica, confirmada contra `endstream`
 *   - cadeia /Prev com limite de profundidade e deteccao de ciclo
 *   - reconstrucao completa do xref por varredura de "N G obj" quando a tabela
 *     esta corrompida ou os offsets nao batem
 *
 * A ultima e a que salva PDF real: offset de xref errado por poucos bytes e
 * defeito comum de ferramenta que edita o arquivo sem reescrever a tabela.
 */

typedef struct {
    unsigned char type;   /* 0 = livre, 1 = offset no arquivo, 2 = em ObjStm */
    long long     off;    /* tipo 1: offset. tipo 2: numero do ObjStm */
    int           idx;    /* tipo 1: geracao. tipo 2: indice dentro do ObjStm */
} PdfXrefEnt;

typedef struct {
    PdfStream   st;

    /* Arena de vida do documento: xref e trailer ficam aqui e sobrevivem a
     * qualquer reciclagem de arena de pagina. */
    PdfArena    arena;

    PdfXrefEnt *xref;
    int         xref_len;

    PdfObj     *trailer;

    int ver_major, ver_minor;

    /*
     * Deslocamento do cabecalho.
     *
     * `%PDF-` nao precisa estar no byte 0 - a especificacao permite lixo antes,
     * e arquivos servidos por HTTP as vezes trazem cabecalho colado. Quando
     * isso acontece, TODOS os offsets do xref sao relativos ao `%PDF-`, nao ao
     * inicio do arquivo. Sem esse ajuste, cada objeto e lido no lugar errado.
     */
    long long   hdr_off;

    int encrypted;        /* /Encrypt presente no trailer */
    int pages;            /* numero de paginas */
    int reconstructed;    /* 1 se o xref veio de varredura, nao da tabela */

    /* Cache de um object stream decodificado. Um slot basta: os objetos de uma
     * pagina costumam vir todos do mesmo ObjStm, entao a taxa de acerto e alta
     * e o custo e um buffer. */
    int            objstm_num;
    unsigned char *objstm_data;
    int            objstm_len;

    char err[80];
} PdfDoc;

/*
 * Abre o documento: le cabecalho, xref e trailer, e conta as paginas.
 * Assume a posse de `io` (pdf_doc_close fecha).
 *
 * Retorna 0 em sucesso. Em erro, doc->err descreve o problema.
 */
int  pdf_doc_open(PdfDoc *doc, const PdfIo *io);
void pdf_doc_close(PdfDoc *doc);

/*
 * Objeto por numero, parseado na arena `a`.
 *
 * `a` normalmente e uma arena de pagina, reciclada a cada virada. Nao ha cache
 * de objeto ainda: cada chamada reparseia. Isso e deliberado nesta etapa -
 * medir antes de cachear.
 */
PdfObj *pdf_doc_get(PdfDoc *doc, PdfArena *a, int num);

/* Se `o` for PDF_REF, resolve (recursivamente, com limite); senao devolve `o`. */
PdfObj *pdf_doc_resolve(PdfDoc *doc, PdfArena *a, PdfObj *o);

/* Atalho: valor de uma chave, ja resolvido. */
PdfObj *pdf_doc_dget(PdfDoc *doc, PdfArena *a, const PdfObj *dict, const char *key);

/*
 * Bytes de um stream, com os filtros aplicados.
 *
 * Resolve /Length indireto - que e justamente o caso que nao pode ser tratado
 * durante a leitura do xref, porque resolver exige o xref.
 */
PdfFiltStatus pdf_doc_stream_data(PdfDoc *doc, PdfArena *a, PdfObj *stream,
                                  unsigned char **out, int *outlen);

/* 1 se o stream usa filtro de imagem (pagina escaneada). */
int pdf_doc_stream_is_image(PdfDoc *doc, PdfArena *a, PdfObj *stream);

/*
 * Objeto da pagina de indice `index` (base 0), ou NULL se fora da faixa.
 *
 * Usa /Count dos nos intermediarios para pular subarvores inteiras, o que
 * torna o acesso proporcional a profundidade da arvore e nao ao numero de
 * paginas. Num livro de 500 paginas a diferenca entre isso e uma varredura
 * completa e a diferenca entre virar pagina na hora e esperar.
 *
 * Quando /Count esta ausente ou errado, cai para varredura em profundidade.
 */
PdfObj *pdf_doc_page(PdfDoc *doc, PdfArena *a, int index);

/*
 * Concatena os content streams de uma pagina, descomprimidos.
 *
 * /Contents pode ser um stream unico ou um array de streams que, pela
 * especificacao, formam UM fluxo continuo - um operador pode comecar num
 * pedaco e terminar no seguinte. Decodificar separadamente e concatenar depois
 * e a unica leitura correta.
 */
PdfFiltStatus pdf_doc_page_content(PdfDoc *doc, PdfArena *a, PdfObj *page,
                                   unsigned char **out, int *outlen);

#endif
