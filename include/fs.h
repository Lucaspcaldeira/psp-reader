#ifndef EREADER_FS_H
#define EREADER_FS_H

/*
 * Resolucao de caminho.
 *
 * Por que isso existe: quando o homebrew e lancado pelo XMB, o diretorio de
 * trabalho NAO e garantido. Um "data/font.ttf" relativo funciona no PPSSPP e
 * falha no hardware real. Entao derivamos a pasta base de argv[0], que o
 * kernel entrega como "ms0:/PSP/GAME/EREADER/EBOOT.PBP".
 */
void        fs_init(const char *argv0);

/* Pasta do app, com barra final. Ex: "ms0:/PSP/GAME/EREADER/" */
const char *fs_base(void);

/* Devolve caminho absoluto para um asset do app. Buffer estatico rotativo:
 * valido para uso imediato (ex: passar direto pra fopen), nao guarde. */
const char *fs_path(const char *relative);

/*
 * Pasta dos livros do usuario: "ms0:/PSP/BOOKS/".
 *
 * Fica FORA da pasta do app de proposito, para sobreviver a uma reinstalacao
 * do homebrew - apagar ms0:/PSP/GAME/EREADER/ nao pode levar a biblioteca do
 * usuario embora.
 *
 * O prefixo do dispositivo e derivado de argv[0] em vez de fixado em "ms0:",
 * porque no PSP Go a memoria interna e "ef0:" - com "ms0:" fixo o app nao
 * acharia livro nenhum num Go sem Memory Stick.
 */
const char *fs_books_dir(void);

/* Caminho dentro de ms0:/PSP/BOOKS/.ereader/ , onde ficam indices de paginacao
 * (.pri) e progresso de leitura. Buffer estatico rotativo. */
const char *fs_state_path(const char *relative);

/* Cria a pasta de livros e a de estado se nao existirem. Retorna 0 em sucesso. */
int fs_ensure_dirs(void);

/*
 * Leitura e escrita de arquivo INTEIRO e pequeno.
 *
 * Existe para o progresso de leitura, que e alguns KB. Deliberadamente NAO e a
 * interface de leitura de livro - essa e PdfIo, com janela e acesso por offset,
 * porque livro nao cabe na RAM (o maior do corpus tem 72 MB). Sao duas
 * necessidades diferentes e misturar as duas produziria uma interface que serve
 * mal as duas.
 *
 * fs_read_file devolve os bytes lidos (0 se o arquivo nao existe, que para o
 * progresso e o caso normal da primeira execucao) ou negativo em erro.
 * fs_write_file devolve 0 em sucesso.
 *
 * O backend de PSP e sceIo; o de host, para os testes, vive em tools/host_fs.c.
 */
int fs_read_file(const char *path, char *buf, int cap);
int fs_write_file(const char *path, const char *buf, int len);

#endif
