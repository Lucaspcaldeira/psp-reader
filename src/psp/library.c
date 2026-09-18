#include <string.h>
#include <stdio.h>
#include <pspiofilemgr.h>
#include "library.h"

const char *library_fmt_name(LibFormat f)
{
    switch (f) {
    case LIB_FMT_PDF:  return "PDF";
    case LIB_FMT_TXT:  return "TXT";
    case LIB_FMT_EPUB: return "EPUB";
    default:          return "?";
    }
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int ends_with_ci(const char *name, const char *ext)
{
    size_t ln = strlen(name), le = strlen(ext);
    if (ln <= le)
        return 0;
    const char *s = name + (ln - le);
    for (size_t i = 0; i < le; ++i)
        if (lower(s[i]) != lower(ext[i]))
            return 0;
    return 1;
}

static LibFormat classify(const char *name)
{
    if (ends_with_ci(name, ".pdf"))
        return LIB_FMT_PDF;
    if (ends_with_ci(name, ".txt"))
        return LIB_FMT_TXT;
    if (ends_with_ci(name, ".epub"))
        return LIB_FMT_EPUB;
    return LIB_FMT_UNKNOWN;
}

/*
 * Comparacao para ordenar.
 *
 * Case-insensitive byte a byte. NAO e uma colacao correta para portugues -
 * "Ágata" vai depois de "Zebra", porque os bytes de UTF-8 de 'Á' sao 0xC3 0x81,
 * maiores que qualquer ASCII. Fazer melhor exigiria uma tabela de colacao, e o
 * ganho numa lista de dezenas de livros nao paga o custo em RAM. Registrado
 * para nao parecer descuido.
 */
static int name_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = lower(*a), cb = lower(*b);
        if (ca != cb)
            return (unsigned char)ca - (unsigned char)cb;
        a++; b++;
    }
    return (unsigned char)lower(*a) - (unsigned char)lower(*b);
}

void library_scan(Library *lib, const char *dir)
{
    memset(lib, 0, sizeof(*lib));

    SceUID d = sceIoDopen(dir);
    if (d < 0) {
        lib->err = d;
        return;
    }

    SceIoDirent e;
    /* memset obrigatorio a cada iteracao: sceIoDread nao limpa os campos que
     * nao usa, e lixo em d_stat.st_mode faz o teste de diretorio dar resultado
     * aleatorio - com o sintoma de arquivos aparecendo e desaparecendo da
     * lista entre varreduras. */
    memset(&e, 0, sizeof(e));

    while (sceIoDread(d, &e) > 0) {
        if (!FIO_S_ISDIR(e.d_stat.st_mode)) {
            LibFormat f = classify(e.d_name);
            if (f != LIB_FMT_UNKNOWN) {
                if (lib->count >= LIB_MAX) {
                    lib->truncated = 1;
                } else {
                    LibEntry *it = &lib->items[lib->count++];
                    /* Precisao explicita: d_name tem 256 bytes e o destino 128.
                     * O truncamento e intencional. */
                    snprintf(it->name, LIB_NAME_MAX, "%.*s",
                             LIB_NAME_MAX - 1, e.d_name);
                    it->size = (unsigned int)e.d_stat.st_size;
                    it->fmt  = f;
                }
            }
        }
        memset(&e, 0, sizeof(e));
    }
    sceIoDclose(d);

    /* Insertion sort: a lista tem dezenas de itens, e o codigo cabe em cinco
     * linhas sem precisar de qsort nem de comparador com ponteiro. */
    for (int i = 1; i < lib->count; ++i) {
        LibEntry tmp = lib->items[i];
        int j = i - 1;
        while (j >= 0 && name_cmp(lib->items[j].name, tmp.name) > 0) {
            lib->items[j + 1] = lib->items[j];
            j--;
        }
        lib->items[j + 1] = tmp;
    }
}
