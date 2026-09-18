#include <string.h>
#include <stdio.h>
#include <pspiofilemgr.h>
#include "fs.h"

static char g_base[192]  = "./";
static char g_books[192] = "ms0:/PSP/BOOKS/";
static char g_state[224] = "ms0:/PSP/BOOKS/.ereader/";

#define SLOTS 4
static char g_buf[SLOTS][320];
static int  g_slot = 0;

static const char *rotate(void)
{
    char *out = g_buf[g_slot];
    g_slot = (g_slot + 1) % SLOTS;
    return out;
}

void fs_init(const char *argv0)
{
    if (!argv0 || !*argv0)
        return;

    /* Corta no ultimo separador, mantendo a barra final.
     * O kernel do PSP sempre usa '/', mesmo em ms0: - nao ha caso de barra
     * invertida pra tratar aqui. */
    const char *last = strrchr(argv0, '/');
    if (!last)
        return;

    size_t len = (size_t)(last - argv0) + 1;
    if (len >= sizeof(g_base))
        len = sizeof(g_base) - 1;
    memcpy(g_base, argv0, len);
    g_base[len] = '\0';

    /*
     * Deriva o dispositivo do proprio argv[0] em vez de assumir "ms0:".
     * No PSP Go a memoria interna e "ef0:", e um Go sem Memory Stick nao tem
     * ms0: nenhum - com o prefixo fixo, a biblioteca apareceria sempre vazia
     * e o sintoma seria "nao encontra meus livros", sem pista da causa.
     */
    const char *colon = strchr(g_base, ':');
    if (colon) {
        size_t dev = (size_t)(colon - g_base) + 1;   /* inclui o ':' */
        if (dev < sizeof(g_books) - 32) {
            memcpy(g_books, g_base, dev);
            g_books[dev] = '\0';
            strcat(g_books, "/PSP/BOOKS/");

            snprintf(g_state, sizeof(g_state), "%s.ereader/", g_books);
        }
    }
}

const char *fs_base(void)       { return g_base; }
const char *fs_books_dir(void)  { return g_books; }

const char *fs_path(const char *relative)
{
    char *out = (char *)rotate();
    snprintf(out, sizeof(g_buf[0]), "%s%s", g_base, relative);
    return out;
}

const char *fs_state_path(const char *relative)
{
    char *out = (char *)rotate();
    snprintf(out, sizeof(g_buf[0]), "%s%s", g_state, relative);
    return out;
}

int fs_ensure_dirs(void)
{
    /* 0777 e o que o VFAT do Memory Stick aceita; ele nao tem permissoes reais.
     * Erro de "ja existe" nao e falha - so devolvemos erro se depois disso o
     * diretorio ainda nao abrir. */
    sceIoMkdir(g_books, 0777);
    sceIoMkdir(g_state, 0777);

    SceUID d = sceIoDopen(g_books);
    if (d < 0)
        return -1;
    sceIoDclose(d);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Arquivos pequenos e inteiros (progresso de leitura)                        */

int fs_read_file(const char *path, char *buf, int cap)
{
    SceUID f = sceIoOpen(path, PSP_O_RDONLY, 0777);
    if (f < 0)
        return 0;              /* nao existe: primeira execucao, nao e erro */

    int total = 0;
    while (total < cap) {
        int n = sceIoRead(f, buf + total, cap - total);
        if (n <= 0)
            break;
        total += n;
    }
    sceIoClose(f);
    return total;
}

int fs_write_file(const char *path, const char *buf, int len)
{
    /*
     * Grava em arquivo temporario e renomeia por cima.
     *
     * O PSP e desligado no botao, com frequencia no meio de uma gravacao. Se a
     * escrita fosse direta no arquivo final, um desligamento na hora errada
     * deixaria o progresso de TODOS os livros truncado - perder a posicao de um
     * livro por nao ter gravado ainda e aceitavel, perder a dos outros
     * cinquenta porque o arquivo ficou pela metade nao e.
     */
    char tmp[320];
    snprintf(tmp, sizeof(tmp), "%s.new", path);

    SceUID f = sceIoOpen(tmp, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (f < 0)
        return -1;

    int total = 0;
    while (total < len) {
        int n = sceIoWrite(f, buf + total, len - total);
        if (n <= 0)
            break;
        total += n;
    }
    sceIoClose(f);

    if (total != len) {
        sceIoRemove(tmp);
        return -1;
    }

    /* sceIoRename nao sobrescreve: o destino tem de sair da frente antes. */
    sceIoRemove(path);
    if (sceIoRename(tmp, path) < 0) {
        sceIoRemove(tmp);
        return -1;
    }
    return 0;
}
