#ifndef EREADER_PROGRESS_H
#define EREADER_PROGRESS_H

/*
 * Onde o leitor parou, por livro, e as preferencias globais.
 *
 * Guardado num arquivo unico em ms0:/PSP/BOOKS/.ereader/progress.txt, e nao num
 * arquivo por livro: sao dezenas de livros, o arquivo inteiro tem alguns KB, e
 * uma leitura na abertura do app resolve tudo. Um arquivo por livro
 * multiplicaria os acessos ao Memory Stick, que e a operacao mais lenta do
 * console com folga.
 *
 * O QUE E GUARDADO, e por que estes campos:
 *
 *   unit   a unidade de carga (pagina do PDF, bloco do TXT). E o indice
 *          estavel: o mesmo numero devolve sempre o mesmo texto.
 *   para   o paragrafo do topo da tela DENTRO da unidade. Guardar o numero da
 *          tela nao serviria, porque a tela depende do corpo da fonte - com
 *          corpo maior ha mais telas, e a tela 3 de ontem nao e a de hoje.
 *   px     o corpo em que o livro estava sendo lido. Cada livro guarda o seu:
 *          um romance e um manual tecnico nao pedem o mesmo tamanho.
 *
 * A chave e nome + tamanho do arquivo. O tamanho e o que impede um livro
 * substituido por outro de mesmo nome de abrir no meio.
 */

#define PROG_MAX_BOOKS  128
#define PROG_NAME_MAX   128

typedef struct {
    char         name[PROG_NAME_MAX];
    unsigned int size;      /* bytes do arquivo, parte da chave */
    int          unit;
    int          total;   /* unidades do livro, para a barra de progresso */
    int          para;
    int          px;
} ProgEntry;

typedef struct {
    ProgEntry entries[PROG_MAX_BOOKS];
    int       count;

    /* Preferencias globais, no mesmo arquivo. */
    int       theme;

    int       dirty;
    int       loaded;
} Progress;

void progress_load(Progress *p, const char *path);

/* Grava se houver o que gravar. Retorna 0 em sucesso (ou se nada mudou). */
int  progress_save(Progress *p, const char *path);

/*
 * Posicao de um livro. Retorna 0 e preenche os campos se havia registro; -1 se
 * o livro e novo, e ai os campos ficam intocados.
 */
int  progress_get(const Progress *p, const char *name, unsigned int size,
                  int *unit, int *total, int *para, int *px);

/*
 * Registra a posicao. A entrada mais antiga sai quando a lista enche - o livro
 * que voce nao abre ha meses e o que menos custa perder.
 */
void progress_set(Progress *p, const char *name, unsigned int size,
                  int unit, int total, int para, int px);

void progress_set_theme(Progress *p, int theme);

#endif
