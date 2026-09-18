/*
 * Backend de host para as duas funcoes de arquivo inteiro de fs.h.
 *
 * O par de src/psp/fs.c, e o que permite testar o progresso de leitura sem o
 * console. As funcoes de CAMINHO de fs.h nao existem aqui de proposito: elas
 * derivam de argv[0] do PSP e nao significam nada no host, entao os testes
 * passam caminhos absolutos direto.
 */
#include <stdio.h>
#include "fs.h"

int fs_read_file(const char *path, char *buf, int cap)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;                  /* nao existe: o mesmo contrato do PSP */
    size_t n = fread(buf, 1, (size_t)cap, f);
    fclose(f);
    return (int)n;
}

int fs_write_file(const char *path, const char *buf, int len)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    size_t n = fwrite(buf, 1, (size_t)len, f);
    fclose(f);
    return (n == (size_t)len) ? 0 : -1;
}
