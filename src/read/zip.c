#include <string.h>
#include <stdio.h>
#include <zlib.h>

#include "zip.h"

/* ------------------------------------------------------------------------- */
/* Leitura de inteiros pequenos                                               */

static unsigned int rd16(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

static unsigned int rd32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static void set_err(Zip *z, const char *m)
{
    snprintf(z->err, sizeof(z->err), "%s", m);
}

/* ------------------------------------------------------------------------- */
/* Diretorio central                                                          */

/*
 * O EOCD (End Of Central Directory) fica NO FIM do arquivo, e o fim e uma
 * posicao variavel porque o registro termina com um comentario de ate 64 KB.
 * Entao a busca e para tras a partir do fim, e nao para frente a partir do
 * inicio.
 *
 * Ler o ZIP de tras para frente e o que dispensa varrer o arquivo inteiro: num
 * EPUB de 80 MB, achar um capitulo de 20 KB custa duas leituras.
 */
#define EOCD_SIG  0x06054B50u
#define CEN_SIG   0x02014B50u
#define LOC_SIG   0x04034B50u

static long long find_eocd(PdfStream *st)
{
    /* 22 bytes do registro + ate 65535 de comentario. */
    long long span = 22 + 65535;
    if (span > st->size)
        span = st->size;
    long long start = st->size - span;

    unsigned char buf[4096];
    long long pos = st->size;

    while (pos > start) {
        long long chunk = 4096;
        if (chunk > pos - start)
            chunk = pos - start;
        /* Sobreposicao de 3 bytes: a assinatura pode estar partida entre dois
         * blocos, e sem isso ela seria perdida silenciosamente. */
        long long at = pos - chunk;
        pdf_seek(st, at);
        int got = pdf_stream_read(st, buf, (int)chunk);
        if (got <= 0)
            break;

        for (int i = got - 4; i >= 0; --i)
            if (rd32(buf + i) == EOCD_SIG)
                return at + i;

        /*
         * A parada e "cheguei ao inicio da faixa", e nao "a proxima posicao
         * seria menor": no ultimo bloco a sobreposicao de 3 bytes e maior que o
         * proprio bloco, e `pos = at + 3` devolveria pos ao valor anterior -
         * um laco que nunca termina, com o sintoma de o app congelar ao abrir
         * um EPUB pequeno.
         */
        if (at <= start)
            break;
        pos = at + 3;
    }
    return -1;
}

int zip_open(Zip *z, PdfArena *a, const PdfIo *io)
{
    memset(z, 0, sizeof(*z));

    if (pdf_stream_init(&z->st, io) != 0) {
        set_err(z, "arquivo vazio ou ilegivel");
        return -1;
    }

    long long eocd = find_eocd(&z->st);
    if (eocd < 0) {
        set_err(z, "nao parece um ZIP (sem diretorio central)");
        return -1;
    }

    unsigned char hdr[22];
    pdf_seek(&z->st, eocd);
    if (pdf_stream_read(&z->st, hdr, 22) != 22) {
        set_err(z, "diretorio central truncado");
        return -1;
    }

    unsigned int total = rd16(hdr + 10);
    unsigned int cdsz  = rd32(hdr + 12);
    unsigned int cdoff = rd32(hdr + 16);

    if (total == 0xFFFFu || cdoff == 0xFFFFFFFFu) {
        set_err(z, "ZIP64 nao suportado");
        return -1;
    }
    if ((long long)cdoff + (long long)cdsz > z->st.size) {
        set_err(z, "diretorio central aponta para fora do arquivo");
        return -1;
    }

    z->entries = (ZipEntry *)pdf_arena_alloc(a, sizeof(ZipEntry) * ZIP_MAX_ENTRIES);
    if (!z->entries) {
        set_err(z, "sem memoria para o indice");
        return -1;
    }

    long long p = cdoff;
    long long cend = (long long)cdoff + cdsz;

    for (unsigned int i = 0; i < total && p < cend; ++i) {
        unsigned char e[46];
        pdf_seek(&z->st, p);
        if (pdf_stream_read(&z->st, e, 46) != 46)
            break;
        if (rd32(e) != CEN_SIG)
            break;

        unsigned int nlen = rd16(e + 28);
        unsigned int elen = rd16(e + 30);
        unsigned int clen = rd16(e + 32);

        if (z->nentries >= ZIP_MAX_ENTRIES) {
            z->truncated = 1;
            break;
        }

        ZipEntry *en = &z->entries[z->nentries];
        memset(en, 0, sizeof(*en));
        en->method = (int)rd16(e + 10);
        en->csize  = rd32(e + 20);
        en->usize  = rd32(e + 24);
        en->lho    = rd32(e + 42);

        unsigned int take = nlen;
        if (take > ZIP_NAME_MAX - 1)
            take = ZIP_NAME_MAX - 1;
        if (pdf_stream_read(&z->st, en->name, (int)take) != (int)take)
            break;
        en->name[take] = '\0';

        /* Nome maior que o buffer vira entrada invalida em vez de entrada com
         * nome truncado: um nome truncado casaria por engano com outro membro. */
        if (nlen < ZIP_NAME_MAX)
            z->nentries++;

        p += 46 + nlen + elen + clen;
    }

    if (z->nentries == 0) {
        set_err(z, "ZIP sem membros legiveis");
        return -1;
    }
    return 0;
}

void zip_close(Zip *z)
{
    pdf_stream_close(&z->st);
    memset(z, 0, sizeof(*z));
}

int zip_find(const Zip *z, const char *name)
{
    for (int i = 0; i < z->nentries; ++i)
        if (strcmp(z->entries[i].name, name) == 0)
            return i;
    return -1;
}

/* ------------------------------------------------------------------------- */
/* Leitura de membro                                                          */

/*
 * Onde os dados de um membro comecam.
 *
 * O offset do diretorio central aponta para o cabecalho LOCAL, cujos campos de
 * nome e extra tem tamanho proprio - e que frequentemente DIFEREM dos do
 * diretorio central. Usar os tamanhos do diretorio central aqui e um erro
 * classico: funciona na maioria dos arquivos e produz lixo em alguns.
 */
static long long data_offset(Zip *z, const ZipEntry *en)
{
    unsigned char lh[30];
    pdf_seek(&z->st, en->lho);
    if (pdf_stream_read(&z->st, lh, 30) != 30)
        return -1;
    if (rd32(lh) != LOC_SIG)
        return -1;
    return en->lho + 30 + rd16(lh + 26) + rd16(lh + 28);
}

#define ZBUF 4096

int zip_reader_open(Zip *z, int idx, ZipReader *r)
{
    if (idx < 0 || idx >= z->nentries)
        return -1;

    memset(r, 0, sizeof(*r));
    r->z = z;
    r->idx = idx;
    r->method = z->entries[idx].method;

    r->doff = data_offset(z, &z->entries[idx]);
    if (r->doff < 0) {
        set_err(z, "cabecalho local invalido");
        return -1;
    }

    if (r->method == 0)
        return 0;
    if (r->method != 8) {
        set_err(z, "metodo de compressao nao suportado");
        return -1;
    }

    /*
     * O z_stream mora dentro do ZipReader, e nao num malloc.
     *
     * Um leitor por vez, num console sem pressao de memoria mas com um heap
     * pequeno e fragmentavel: alocar e liberar um z_stream a cada virada de
     * tela seria a fonte de fragmentacao mais boba possivel.
     */
    if (sizeof(z_stream) > sizeof(r->zsmem)) {
        set_err(z, "z_stream maior que o esperado");
        return -1;
    }
    z_stream *zs = (z_stream *)(void *)r->zsmem;
    memset(zs, 0, sizeof(*zs));

    /*
     * Deflate CRU (windowBits negativo): dentro de um ZIP o fluxo nao tem o
     * cabecalho de dois bytes do zlib. Passar 15 aqui faz o inflate recusar o
     * primeiro byte como dado invalido, e o sintoma e "todo EPUB esta
     * corrompido".
     */
    if (inflateInit2(zs, -15) != Z_OK) {
        set_err(z, "inflateInit falhou");
        return -1;
    }
    r->zs = zs;
    return 0;
}

void zip_reader_close(ZipReader *r)
{
    if (r->zs) {
        inflateEnd((z_stream *)r->zs);
        r->zs = NULL;
    }
}

int zip_reader_read(ZipReader *r, void *dst, int cap)
{
    if (cap <= 0 || r->done)
        return 0;

    ZipEntry *en = &r->z->entries[r->idx];

    if (r->method == 0) {
        long long left = en->usize - r->upos;
        if (left <= 0) {
            r->done = 1;
            return 0;
        }
        int want = (left < cap) ? (int)left : cap;
        pdf_seek(&r->z->st, r->doff + r->upos);
        int got = pdf_stream_read(&r->z->st, dst, want);
        if (got > 0)
            r->upos += got;
        return got;
    }

    z_stream *zs = (z_stream *)r->zs;
    if (!zs)
        return -1;

    int written = 0;

    while (written < cap) {
        if (zs->avail_in == 0) {
            long long left = en->csize - r->cpos;
            if (left <= 0)
                break;
            int want = (left < ZBUF) ? (int)left : ZBUF;
            pdf_seek(&r->z->st, r->doff + r->cpos);
            int got = pdf_stream_read(&r->z->st, r->in, want);
            if (got <= 0)
                break;
            r->cpos += got;
            zs->next_in = r->in;
            zs->avail_in = (unsigned int)got;
        }

        zs->next_out = (unsigned char *)dst + written;
        zs->avail_out = (unsigned int)(cap - written);

        unsigned int before = zs->avail_out;
        int rc = inflate(zs, Z_NO_FLUSH);
        unsigned int produced = before - zs->avail_out;
        written += (int)produced;

        if (rc == Z_STREAM_END) {
            r->done = 1;
            break;
        }
        if (rc != Z_OK)
            return written > 0 ? written : -1;
        if (produced == 0 && zs->avail_in == 0 && r->cpos >= en->csize) {
            r->done = 1;
            break;
        }
    }

    r->upos += written;
    return written;
}

int zip_read_range(Zip *z, int idx, long long from, void *dst, int cap)
{
    if (cap <= 0)
        return -1;

    ZipReader r;
    if (zip_reader_open(z, idx, &r) != 0)
        return -1;

    /* Membro armazenado pula direto; comprimido paga o preco de descomprimir e
     * descartar, que e tempo e nao memoria. */
    if (r.method == 0) {
        r.upos = from;
    } else {
        unsigned char skip[ZBUF];
        while (r.upos < from && !r.done) {
            long long need = from - r.upos;
            int want = (need < ZBUF) ? (int)need : ZBUF;
            int got = zip_reader_read(&r, skip, want);
            if (got <= 0)
                break;
        }
    }

    int total = 0;
    while (total < cap) {
        int got = zip_reader_read(&r, (unsigned char *)dst + total, cap - total);
        if (got <= 0)
            break;
        total += got;
    }

    zip_reader_close(&r);
    return total;
}

unsigned char *zip_read_all(Zip *z, PdfArena *a, int idx, int max, int *out_len)
{
    if (out_len)
        *out_len = 0;
    if (idx < 0 || idx >= z->nentries)
        return NULL;

    long long usz = z->entries[idx].usize;
    if (usz <= 0 || usz > max) {
        set_err(z, "membro grande demais para carregar inteiro");
        return NULL;
    }

    unsigned char *buf = (unsigned char *)pdf_arena_alloc(a, (size_t)usz + 1);
    if (!buf)
        return NULL;

    int n = zip_read_range(z, idx, 0, buf, (int)usz);
    if (n < 0)
        return NULL;
    buf[n] = '\0';
    if (out_len)
        *out_len = n;
    return buf;
}
