#ifndef EREADER_HOST_IO_H
#define EREADER_HOST_IO_H

#include "pdf_io.h"

/* Backend de PdfIo sobre FILE*, para os testes de host. O equivalente de PSP
 * (sceIo) vive em src/psp/pdf_io_psp.c. */
int  host_io_open(PdfIo *io, const char *path);

#endif
