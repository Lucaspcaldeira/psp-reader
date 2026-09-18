#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "progress.h"
#include "fs.h"

/*
 * Formato: texto, uma linha por livro, campos separados por TAB.
 *
 *   #ereader1
 *   theme<TAB>1
 *   b<TAB>size<TAB>unit<TAB>total<TAB>para<TAB>px<TAB>nome do arquivo
 *
 * `total` e o numero de unidades do livro. Guardado junto porque a biblioteca
 * mostra o quanto ja foi lido de cada livro ANTES de abri-lo, e sem ele a conta
 * exigiria abrir os cinquenta arquivos da pasta a cada varredura.
 *
 * Texto e nao binario de proposito. O arquivo mora no Memory Stick do usuario,
 * que pode abri-lo no PC; um progresso perdido e um aborrecimento pequeno, e
 * poder consertar a mao com um editor de texto vale mais que os bytes
 * economizados. Tambem sobrevive a mudanca de alinhamento ou de endianness sem
 * versionamento de struct.
 *
 * O NOME VEM POR ULTIMO, e isso importa: nome de arquivo pode conter quase
 * qualquer coisa, inclusive TAB. Com ele no fim, o resto da linha e o nome
 * inteiro e nao ha ambiguidade nenhuma no corte dos campos.
 */

#define PROG_FILE_MAX  (PROG_MAX_BOOKS * (PROG_NAME_MAX + 48) + 64)

static const char *skip_field(const char *p, const char *end)
{
    while (p < end && *p != '\t' && *p != '\n')
        p++;
    return p;
}

static long parse_long(const char **p, const char *end)
{
    long v = 0;
    int neg = 0;
    if (*p < end && **p == '-') { neg = 1; (*p)++; }
    while (*p < end && **p >= '0' && **p <= '9') {
        v = v * 10 + (**p - '0');
        (*p)++;
    }
    *p = skip_field(*p, end);
    if (*p < end && **p == '\t')
        (*p)++;
    return neg ? -v : v;
}

static void parse(Progress *p, const char *s, int len)
{
    const char *end = s + len;
    const char *line = s;

    while (line < end) {
        const char *nl = line;
        while (nl < end && *nl != '\n')
            nl++;

        if (line < nl) {
            const char *q = line;
            if (*q == 'b' && q + 1 < nl && q[1] == '\t') {
                q += 2;
                unsigned int size = (unsigned int)parse_long(&q, nl);
                int unit  = (int)parse_long(&q, nl);
                int total = (int)parse_long(&q, nl);
                int para  = (int)parse_long(&q, nl);
                int px    = (int)parse_long(&q, nl);

                /* O que sobrou da linha e o nome, TABs inclusive. */
                int nlen = (int)(nl - q);
                if (nlen > 0 && nlen < PROG_NAME_MAX &&
                    p->count < PROG_MAX_BOOKS) {
                    ProgEntry *e = &p->entries[p->count++];
                    memcpy(e->name, q, (size_t)nlen);
                    e->name[nlen] = '\0';
                    e->size = size;
                    e->unit = unit < 0 ? 0 : unit;
                    e->total = total < 0 ? 0 : total;
                    e->para = para < 0 ? 0 : para;
                    e->px   = px;
                }
            } else if (nl - line > 6 && memcmp(line, "theme\t", 6) == 0) {
                const char *q = line + 6;
                p->theme = (int)parse_long(&q, nl);
            }
        }

        line = (nl < end) ? nl + 1 : end;
    }
}

void progress_load(Progress *p, const char *path)
{
    memset(p, 0, sizeof(*p));
    p->loaded = 1;

    char *buf = (char *)malloc(PROG_FILE_MAX);
    if (!buf)
        return;

    int n = fs_read_file(path, buf, PROG_FILE_MAX);
    if (n > 0)
        parse(p, buf, n);
    free(buf);
}

int progress_save(Progress *p, const char *path)
{
    if (!p->dirty)
        return 0;

    char *buf = (char *)malloc(PROG_FILE_MAX);
    if (!buf)
        return -1;

    int n = snprintf(buf, PROG_FILE_MAX, "#ereader1\ntheme\t%d\n", p->theme);

    for (int i = 0; i < p->count; ++i) {
        const ProgEntry *e = &p->entries[i];
        int room = PROG_FILE_MAX - n;
        if (room < PROG_NAME_MAX + 48)
            break;
        n += snprintf(buf + n, (size_t)room, "b\t%u\t%d\t%d\t%d\t%d\t%s\n",
                      e->size, e->unit, e->total, e->para, e->px, e->name);
    }

    int rc = fs_write_file(path, buf, n);
    free(buf);
    if (rc == 0)
        p->dirty = 0;
    return rc;
}

static int find(const Progress *p, const char *name, unsigned int size)
{
    for (int i = 0; i < p->count; ++i)
        if (p->entries[i].size == size &&
            strcmp(p->entries[i].name, name) == 0)
            return i;
    return -1;
}

int progress_get(const Progress *p, const char *name, unsigned int size,
                 int *unit, int *total, int *para, int *px)
{
    int i = find(p, name, size);
    if (i < 0)
        return -1;
    if (unit)  *unit  = p->entries[i].unit;
    if (total) *total = p->entries[i].total;
    if (para) *para = p->entries[i].para;
    if (px)   *px   = p->entries[i].px;
    return 0;
}

void progress_set(Progress *p, const char *name, unsigned int size,
                  int unit, int total, int para, int px)
{
    int i = find(p, name, size);

    if (i < 0) {
        if (p->count < PROG_MAX_BOOKS) {
            i = p->count++;
        } else {
            /*
             * Lista cheia: sai o do fim.
             *
             * A entrada mais nova sempre vai para a frente (ver o memmove
             * abaixo), entao o fim e o livro aberto ha mais tempo - o que menos
             * custa perder. E uma LRU sem campo de data, que no PSP e util: o
             * relogio do console frequentemente esta errado depois de a bateria
             * descarregar, e ordenar por data seria ordenar por ficcao.
             */
            i = PROG_MAX_BOOKS - 1;
        }
        snprintf(p->entries[i].name, PROG_NAME_MAX, "%s", name);
        p->entries[i].size = size;
    }

    ProgEntry e = p->entries[i];
    e.unit  = unit;
    e.total = total;
    e.para  = para;
    e.px    = px;

    /* Move para a frente, para que a lista fique em ordem de uso. */
    if (i > 0)
        memmove(&p->entries[1], &p->entries[0], sizeof(ProgEntry) * (size_t)i);
    p->entries[0] = e;

    p->dirty = 1;
}

void progress_set_theme(Progress *p, int theme)
{
    if (p->theme != theme) {
        p->theme = theme;
        p->dirty = 1;
    }
}
