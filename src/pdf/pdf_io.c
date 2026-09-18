#include <string.h>
#include "pdf_io.h"

static int mem_read(void *ctx, long long off, void *dst, int len)
{
    PdfMemCtx *m = (PdfMemCtx *)ctx;
    if (off < 0 || off >= (long long)m->len)
        return 0;
    int avail = m->len - (int)off;
    if (len > avail)
        len = avail;
    memcpy(dst, m->p + off, (size_t)len);
    return len;
}

static long long mem_size(void *ctx)
{
    return (long long)((PdfMemCtx *)ctx)->len;
}

void pdf_io_mem(PdfIo *io, PdfMemCtx *ctx, const unsigned char *p, int len)
{
    ctx->p   = p;
    ctx->len = len < 0 ? 0 : len;
    io->ctx   = ctx;
    io->read  = mem_read;
    io->size  = mem_size;
    io->close = NULL;      /* memoria nao e nossa para liberar */
}

int pdf_stream_init(PdfStream *s, const PdfIo *io)
{
    memset(s, 0, sizeof(*s));
    s->io = *io;
    s->size = io->size ? io->size(io->ctx) : 0;
    if (s->size < 0)
        s->size = 0;
    s->win_off = -1;      /* nenhuma janela carregada */
    s->win_len = 0;
    return s->size > 0 ? 0 : -1;
}

void pdf_stream_close(PdfStream *s)
{
    if (s->io.close && s->io.ctx)
        s->io.close(s->io.ctx);
    s->io.ctx = NULL;
    s->win_off = -1;
    s->win_len = 0;
}

int pdf_stream_refill(PdfStream *s)
{
    if (s->pos < 0 || s->pos >= s->size)
        return 0;

    /*
     * Alinha a janela num multiplo de PDF_WINDOW em vez de comecar em s->pos.
     *
     * O parser costuma dar pequenos passos para tras (pdf_ungetc, e o rewind do
     * lookahead de "N G R"). Com janela comecando em pos, cada passo para tras
     * cruzando a fronteira forcaria uma releitura fisica; alinhada, o retorno
     * cai na mesma janela.
     */
    long long base = (s->pos / PDF_WINDOW) * PDF_WINDOW;

    int want = PDF_WINDOW;
    if (base + want > s->size)
        want = (int)(s->size - base);

    int got = s->io.read(s->io.ctx, base, s->win, want);
    if (got <= 0) {
        s->win_off = -1;
        s->win_len = 0;
        return 0;
    }

    s->win_off = base;
    s->win_len = got;
    s->reads++;
    return 1;
}

int pdf_stream_read(PdfStream *s, void *dst, int n)
{
    unsigned char *out = (unsigned char *)dst;
    int done = 0;

    while (done < n) {
        long long d = s->pos - s->win_off;
        if (d < 0 || d >= (long long)s->win_len) {
            if (!pdf_stream_refill(s))
                break;
            d = s->pos - s->win_off;
            if (d < 0 || d >= (long long)s->win_len)
                break;
        }

        int avail = s->win_len - (int)d;
        int take  = n - done;
        if (take > avail)
            take = avail;

        memcpy(out + done, s->win + d, (size_t)take);
        s->pos += take;
        done   += take;
    }
    return done;
}
