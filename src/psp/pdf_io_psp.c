#include <pspiofilemgr.h>
#include <stdlib.h>
#include "psp_io.h"

typedef struct {
    SceUID    fd;
    long long size;
} PspCtx;

static int p_read(void *ctx, long long off, void *dst, int len)
{
    PspCtx *c = (PspCtx *)ctx;
    if (c->fd < 0)
        return -1;

    /*
     * Um lseek por leitura, em vez de manter posicao.
     *
     * Parece desperdicio, mas o PdfStream ja agrupa em janelas de 4 KB, entao
     * chega aqui uma chamada por cluster do Memory Stick - nao uma por byte. E
     * a alternativa (confiar na posicao atual do descritor) quebraria no
     * primeiro salto do parser para tras, que acontece a todo lookahead de
     * "N G R".
     */
    if (sceIoLseek(c->fd, off, PSP_SEEK_SET) < 0)
        return -1;

    int got = sceIoRead(c->fd, dst, len);
    return got < 0 ? -1 : got;
}

static long long p_size(void *ctx)
{
    return ((PspCtx *)ctx)->size;
}

static void p_close(void *ctx)
{
    PspCtx *c = (PspCtx *)ctx;
    if (c->fd >= 0)
        sceIoClose(c->fd);
    free(c);
}

int psp_io_open(PdfIo *io, const char *path)
{
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
    if (fd < 0)
        return -1;

    PspCtx *c = (PspCtx *)calloc(1, sizeof(PspCtx));
    if (!c) {
        sceIoClose(fd);
        return -1;
    }
    c->fd = fd;

    /* Tamanho via lseek ao fim: sceIoGetstat existe, mas exige montar um
     * SceIoStat e trata caminho em vez de descritor - um seek e mais direto e
     * nao pode discordar do arquivo que ja esta aberto. */
    c->size = sceIoLseek(fd, 0, PSP_SEEK_END);
    sceIoLseek(fd, 0, PSP_SEEK_SET);

    if (c->size <= 0) {
        sceIoClose(fd);
        free(c);
        return -1;
    }

    io->ctx   = c;
    io->read  = p_read;
    io->size  = p_size;
    io->close = p_close;
    return 0;
}
