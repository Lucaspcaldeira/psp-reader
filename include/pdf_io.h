#ifndef EREADER_PDF_IO_H
#define EREADER_PDF_IO_H

/*
 * A unica coisa que a camada de PDF sabe sobre armazenamento.
 *
 * Toda a camada de PDF e de reflow e C99 puro sobre esta interface de tres
 * funcoes. E isso que permite compilar o parser no host e depurar contra PDFs
 * reais em ciclos de segundos, em vez de "compila, copia pro Memory Stick,
 * desconecta USB, abre no XMB, olha a tela".
 *
 * Backends:
 *   host  tools/pdf_io_stdio.c   (FILE*)
 *   PSP   src/psp/pdf_io_psp.c   (sceIo)
 */

typedef struct {
    void *ctx;

    /* Le len bytes a partir de off. Devolve bytes lidos (pode ser menor que len
     * no fim do arquivo), ou negativo em erro de I/O. */
    int (*read)(void *ctx, long long off, void *dst, int len);

    long long (*size)(void *ctx);
    void (*close)(void *ctx);
} PdfIo;

/*
 * Janela de leitura.
 *
 * 4 KB e o tamanho de cluster tipico do Memory Stick: uma janela menor
 * multiplicaria as leituras fisicas, e o acesso ao Memory Stick e a operacao
 * mais lenta do console com folga. O parser le byte a byte, entao sem esta
 * janela cada caractere de um content stream custaria uma ida ao cartao.
 */
#define PDF_WINDOW 4096

typedef struct {
    PdfIo     io;
    long long size;
    long long pos;        /* posicao logica de leitura */
    long long win_off;    /* offset do primeiro byte da janela */
    int       win_len;
    long long reads;      /* leituras fisicas, para diagnostico */
    unsigned char win[PDF_WINDOW];
} PdfStream;

int  pdf_stream_init(PdfStream *s, const PdfIo *io);
void pdf_stream_close(PdfStream *s);

/*
 * Backend sobre um buffer em memoria.
 *
 * Existe para os object streams (PDF 1.5+): um ObjStm e um stream comprimido
 * que, descomprimido, contem varios objetos na mesma sintaxe do arquivo. Com
 * isto, o mesmo lexer e o mesmo parser leem de dentro do buffer, sem uma
 * segunda implementacao das regras de espacamento e de token do PDF.
 *
 * `ctx` precisa continuar valido enquanto o PdfStream for usado - o PdfIo
 * guarda o ponteiro, nao uma copia.
 */
typedef struct {
    const unsigned char *p;
    int len;
} PdfMemCtx;

void pdf_io_mem(PdfIo *io, PdfMemCtx *ctx, const unsigned char *p, int len);

/* Recarrega a janela em torno de s->pos. Devolve 1 se ha ao menos 1 byte
 * disponivel. Publica porque pdf_getc() e inline. */
int  pdf_stream_refill(PdfStream *s);

/* Le n bytes em dst, atravessando janelas. Devolve bytes lidos. */
int  pdf_stream_read(PdfStream *s, void *dst, int n);

static inline void pdf_seek(PdfStream *s, long long off) { s->pos = off; }
static inline long long pdf_tell(const PdfStream *s)     { return s->pos; }

/* Proximo byte, ou -1 no fim do arquivo. */
static inline int pdf_getc(PdfStream *s)
{
    long long d = s->pos - s->win_off;
    if (d < 0 || d >= (long long)s->win_len) {
        if (!pdf_stream_refill(s))
            return -1;
        d = s->pos - s->win_off;
        if (d < 0 || d >= (long long)s->win_len)
            return -1;
    }
    s->pos++;
    return s->win[d];
}

/* Olha o proximo byte sem consumir. */
static inline int pdf_peek(PdfStream *s)
{
    int c = pdf_getc(s);
    if (c >= 0)
        s->pos--;
    return c;
}

static inline void pdf_ungetc(PdfStream *s)
{
    if (s->pos > 0)
        s->pos--;
}

#endif
