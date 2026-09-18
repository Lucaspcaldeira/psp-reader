#include <stdio.h>
#include <stdlib.h>
#include "host_io.h"

typedef struct {
    FILE *f;
    long long size;
} HostCtx;

static int h_read(void *ctx, long long off, void *dst, int len)
{
    HostCtx *h = (HostCtx *)ctx;
    if (fseek(h->f, (long)off, SEEK_SET) != 0)
        return -1;
    size_t got = fread(dst, 1, (size_t)len, h->f);
    return (int)got;
}

static long long h_size(void *ctx) { return ((HostCtx *)ctx)->size; }

static void h_close(void *ctx)
{
    HostCtx *h = (HostCtx *)ctx;
    if (h->f)
        fclose(h->f);
    free(h);
}

int host_io_open(PdfIo *io, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;

    HostCtx *h = (HostCtx *)calloc(1, sizeof(HostCtx));
    if (!h) {
        fclose(f);
        return -1;
    }
    h->f = f;
    fseek(f, 0, SEEK_END);
    h->size = ftell(f);
    fseek(f, 0, SEEK_SET);

    io->ctx   = h;
    io->read  = h_read;
    io->size  = h_size;
    io->close = h_close;
    return 0;
}
