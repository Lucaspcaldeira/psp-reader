#ifndef EREADER_PSP_IO_H
#define EREADER_PSP_IO_H

#include "pdf_io.h"

/*
 * Backend de PdfIo sobre sceIo. O equivalente de host (FILE*) vive em
 * tools/host_io.c, e e o par que permite a camada PDF inteira ser testada sem
 * o console.
 *
 * Em sucesso, o PdfIo assume a posse do descritor: pdf_stream_close() fecha.
 */
int psp_io_open(PdfIo *io, const char *path);

#endif
