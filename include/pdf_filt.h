#ifndef EREADER_PDF_FILT_H
#define EREADER_PDF_FILT_H

#include "pdf_obj.h"

/*
 * Filtros de stream do PDF.
 *
 * Praticamente todo stream de PDF e comprimido: nos dois livros do corpus de
 * teste ha 46 e 365 ocorrencias de FlateDecode. Sem esta camada, nao ha uma
 * unica pagina de texto legivel.
 */

typedef enum {
    PDF_FILT_OK = 0,
    PDF_FILT_ERR_MEM,        /* sem memoria */
    PDF_FILT_ERR_DATA,       /* dados corrompidos */
    PDF_FILT_ERR_IMAGE,      /* filtro de imagem (DCT/JPX/CCITT): nao e texto */
    PDF_FILT_ERR_UNKNOWN     /* filtro nao reconhecido */
} PdfFiltStatus;

/*
 * Aplica a cadeia de filtros de `filter` sobre src.
 *
 * `filter` pode ser PDF_NAME (um filtro) ou PDF_ARR de nomes (cadeia aplicada
 * em ordem). `parms` acompanha: dicionario unico, array paralelo, ou null.
 * Ambos precisam vir JA resolvidos - referencias indiretas sao problema de
 * quem chama, porque resolver exige o documento.
 *
 * Em sucesso, *out aponta para memoria na arena e *outlen tem o tamanho.
 *
 * Filtros de imagem devolvem PDF_FILT_ERR_IMAGE em vez de tentar decodificar:
 * um PDF escaneado e feito de streams DCTDecode, e reconhecer isso e o que
 * permite dizer "este PDF nao tem camada de texto" em vez de mostrar lixo.
 */
PdfFiltStatus pdf_filt_apply(PdfArena *a,
                             const PdfObj *filter, const PdfObj *parms,
                             const unsigned char *src, int srclen,
                             unsigned char **out, int *outlen);

/* Nome legivel do status, para mensagem de erro na tela. */
const char *pdf_filt_status_str(PdfFiltStatus s);

/* 1 se o nome for um filtro de imagem. Usado para detectar pagina escaneada
 * sem tentar decodificar. */
int pdf_filt_is_image(const PdfObj *name);

#endif
