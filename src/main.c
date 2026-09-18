/*
 * PSP Reader - leitor de livros digitais para PSP
 *
 * A interface. Tudo que decide COMO o texto e lido - parser, reflow, paginacao -
 * vive em src/pdf e src/read, e e portavel e testado no host. Aqui fica o que so
 * existe no console: quadros, botoes, e o estado de quem esta lendo o que.
 *
 *   BIBLIOTECA  ms0:/PSP/BOOKS/, com cursor e rolagem
 *      X  ->    DOCUMENTO: metadados, custo de abertura, onde a leitura parou
 *      X  ->    LEITURA: o texto reflowed, uma TELA por vez
 *   AMOSTRA     teste de tipografia (18 px foi aprovado no hardware)
 *   DIAGNOSTICO estado do FreeType, memoria e caminhos
 *
 * A unidade de leitura e a TELA, e nao a pagina do arquivo: uma pagina A4 rende
 * cinco ou seis telas de 480x272, e a virada atravessa a fronteira da pagina
 * sozinha. Ver PLAN.md secao 1 para por que o projeto e assim.
 */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspiofilemgr.h>
#include <psprtc.h>
#include <string.h>
#include <stdio.h>

#include "gfx.h"
#include "input.h"
#include "font.h"
#include "fs.h"
#include "utf8.h"
#include "library.h"
#include "psp_io.h"
#include "pdf_doc.h"
#include "pdf_lex.h"
#include "pdf_text.h"
#include "textlines.h"
#include "reflow.h"
#include "layout.h"
#include "doc.h"
#include "progress.h"
#include "theme.h"

/* Versao do modulo: 1.0, casando com o CHANGELOG.md. Os dois ultimos argumentos
 * sao maior e menor - o XMB nao os mostra, mas um dump do EBOOT mostra. */
PSP_MODULE_INFO("PSPREADER", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

/*
 * Heap explicito de 16 MB, alinhado ao orcamento do PLAN.md secao 7.
 *
 * Nao usamos PSP_HEAP_SIZE_KB(-256) ("tudo menos 256 KB") porque as threads do
 * kernel alocam FORA do nosso heap, e essa folga nao e nossa para gastar.
 *
 * 16 MB e o teto que ainda cabe num PSP-1000 (32 MB, ~24 MB de espaco de
 * usuario), mesmo o alvo sendo o 2000/3000. Consumo real: ~1 MB de atlas de
 * glifos, ~1 MB da face do FreeType, e agora as arenas do PDF mais o buffer
 * temporario de descompressao - o content stream de uma pagina descomprimido
 * chega a algumas centenas de KB.
 */
PSP_HEAP_SIZE_KB(16384);
PSP_MAIN_THREAD_STACK_SIZE_KB(256);

/*
 * --- cores -----------------------------------------------------------------
 *
 * Indireção por ponteiro de tema em vez de constantes: as cores agora mudam em
 * tempo de execucao. Os nomes COL_* continuam os mesmos de proposito - assim a
 * troca nao tocou nenhum dos cinquenta pontos de desenho espalhados pelo
 * arquivo, e um diff de tema nao se confunde com um diff de layout.
 */
static const Theme *g_th;      /* nunca NULL depois de main(); ver theme_get */

#define COL_PAPER   (g_th->paper)
#define COL_INK     (g_th->ink)
#define COL_DIM     (g_th->dim)
#define COL_BAR     (g_th->bar)
#define COL_BAR_TXT (g_th->bar_txt)
#define COL_ERR     (g_th->err)
#define COL_OK      (g_th->ok)
#define COL_SEL     (g_th->sel)

#define MARGIN_X   14.0f
#define BAR_H      22.0f
#define FOOTER_H   20.0f
#define BODY_TOP   (BAR_H + 6.0f)
#define BODY_BOT   ((float)GFX_SCR_H - FOOTER_H - 2.0f)

static int exit_callback(int arg1, int arg2, void *common)
{
    (void)arg1; (void)arg2; (void)common;
    sceKernelExitGame();
    return 0;
}

static int callback_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    int cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

static void setup_callbacks(void)
{
    int thid = sceKernelCreateThread("cb_thread", callback_thread,
                                     0x11, 0xFA0, THREAD_ATTR_USER, 0);
    if (thid >= 0)
        sceKernelStartThread(thid, 0, 0);
}

/* ------------------------------------------------------------------------- */
/* Desenho de texto                                                           */

/*
 * Desenha uma linha, cortando com reticencias se nao couber.
 *
 * Corta por LARGURA MEDIDA e em fronteira de codepoint, nao por contagem de
 * bytes: um corte no meio de uma sequencia UTF-8 produziria um glifo de
 * substituicao no fim de todo nome de arquivo acentuado.
 */
static void draw_clip(float x, float y, float max_w, const char *utf8,
                      unsigned int color)
{
    if (font_measure(utf8, -1) <= max_w) {
        font_draw(x, y, utf8, -1, color);
        return;
    }

    const char *ell = "\xE2\x80\xA6";              /* U+2026 reticencias */
    float ell_w = font_measure(ell, -1);
    float room = max_w - ell_w;
    if (room < 0.0f)
        room = 0.0f;

    int n = font_line_break(utf8, -1, room, NULL);

    /* font_line_break quebra em fronteira de palavra; aqui queremos encher a
     * linha, entao se ele parou muito antes usamos o corte por largura bruta. */
    float w = 0.0f;
    int cut = 0;
    const char *p = utf8;
    const char *end = utf8 + strlen(utf8);
    while (p < end) {
        const char *before = p;
        unsigned int cp = utf8_next(&p, end);
        (void)cp;
        char tmp[8];
        int blen = (int)(p - before);
        if (blen > 7)
            blen = 7;
        memcpy(tmp, before, (size_t)blen);
        tmp[blen] = 0;
        float aw = font_measure(tmp, blen);
        if (w + aw > room)
            break;
        w += aw;
        cut = (int)(p - utf8);
    }
    if (cut < n)
        cut = n;

    float endx = font_draw(x, y, utf8, cut, color);
    font_draw(endx, y, ell, -1, color);
}

static float draw_wrapped(float x, float y, float max_w, const char *utf8,
                          unsigned int color, float line_mul)
{
    int remaining = (int)strlen(utf8);
    const char *p = utf8;
    float step = font_line_height() * line_mul;

    while (remaining > 0 && y <= BODY_BOT) {
        int n = font_line_break(p, remaining, max_w, NULL);
        if (n <= 0)
            break;
        font_draw(x, y, p, n, color);
        p         += n;
        remaining -= n;
        y         += step;
    }
    return y;
}

static void draw_kv(float *y, const char *key, const char *val,
                    unsigned int color)
{
    float step = font_line_height() * 1.06f;
    float kw = font_measure(key, -1);
    font_draw(MARGIN_X, *y, key, -1, COL_DIM);
    draw_clip(MARGIN_X + kw + 6.0f, *y,
              (float)GFX_SCR_W - MARGIN_X * 2.0f - kw - 6.0f, val, color);
    *y += step;
}

/* ------------------------------------------------------------------------- */
/* Estado                                                                     */

enum { SCREEN_LIBRARY = 0, SCREEN_SAMPLE, SCREEN_DIAG, SCREEN_COUNT };

static const char *SCREEN_NAME[SCREEN_COUNT] = {
    "BIBLIOTECA", "AMOSTRA DE TEXTO", "DIAGNOSTICO"
};

static Library g_lib;
static int     g_cursor = 0;
static int     g_view   = 0;   /* primeiro item visivel */

/*
 * Documento aberto.
 *
 * PdfDoc contem um PdfStream com janela de 4 KB, entao mora em estatico e nao
 * na pilha - a pilha da thread principal tem 256 KB e o parser recursivo ja
 * usa uma parte dela.
 */
typedef struct {
    int    open;
    int    rc;
    char   name[LIB_NAME_MAX];
    unsigned int fsize;      /* parte da chave do progresso de leitura */
    float  open_ms;

    /* Amostragem de camada de texto: 4 paginas espalhadas pelo livro. So faz
     * sentido em PDF - e o unico formato onde "tem texto?" e uma pergunta. */
    int    sampled;
    int    sampled_with_text;
    long   sampled_ops;
    float  sample_ms;
    char   sample_err[48];
} DocView;

/*
 * O documento aberto, ja sem formato.
 *
 * Doc contem um PdfStream com janela de 4 KB, entao mora em estatico e nao na
 * pilha - a pilha da thread principal tem 256 KB e o parser recursivo ja usa
 * uma parte dela.
 *
 * A arena do DOCUMENTO e separada da de pagina porque os ciclos de vida sao
 * diferentes: o indice de unidades do TXT vive enquanto o livro estiver aberto,
 * e seria destruido pela reciclagem que acontece a cada virada de tela.
 */
static Doc      g_doc;
static PdfArena g_doc_arena;
static DocView  g_dv;
static Progress g_prog;

/*
 * Modo, e nao "mais uma tela ciclica".
 *
 * BROWSE, DOC e READ formam uma pilha de navegacao: O sempre volta um nivel.
 * Ciclar entre elas com L/R deixaria o usuario na tela de amostra de fonte com
 * um PDF aberto consumindo memoria e nada na interface dizendo isso.
 */
typedef enum { MODE_BROWSE = 0, MODE_DOC, MODE_READ } AppMode;
static AppMode g_mode = MODE_BROWSE;

/*
 * Leitura de uma pagina.
 *
 * A arena e reciclada a cada virada de pagina - e isso que mantem o uso de
 * memoria constante ao folhear um livro de 229 paginas, em vez de crescer sem
 * limite. Uma pagina densa custa cerca de 800 KB somando runs, texto, linhas,
 * paragrafos e layout, medido em `.\test.ps1 pdfreflow`.
 *
 * A "pagina" que o usuario vira agora e `screen`, e nao `page`: a pagina do PDF
 * foi diagramada para A4 e nao cabe em 480x272 de jeito nenhum, entao ela e
 * reflowed e repaginada aqui, e uma pagina de livro rende cinco ou seis telas.
 * A pagina do PDF sobrou como unidade de CARGA, nao de leitura.
 */
typedef struct {
    PdfArena  arena;
    Reflow    rf;
    Layout    lay;
    DocStats  stats;
    int       page;          /* unidade de carga do documento, base 0 */
    int       screen;        /* tela dentro desta unidade */
    int       ok;
    float     ms;            /* tempo de carga + reflow + layout */
} ReadView;

static ReadView g_rv;

/*
 * Painel de diagnostico do reflow, no triangulo.
 *
 * Nao e enfeite: todo julgamento do reflow e heuristico, e quando uma pagina sai
 * errada no console a pergunta e sempre "quantas linhas ele viu, quantas jogou
 * fora, que corpo achou que era o texto". Sem isso, o relato de bug possivel e
 * "a pagina 84 ficou estranha", que nao aponta para nenhuma regra.
 *
 * O triangulo ainda nao e o menu que o PLAN.md reserva para ele (ir para pagina,
 * marcadores, tema, Etapa 4); quando esse menu existir, o painel entra nele.
 */
static int g_read_diag = 0;

static u32 g_tick_res;

static float ms_since(u64 from)
{
    u64 now;
    sceRtcGetCurrentTick(&now);
    return (float)(now - from) * 1000.0f / (float)g_tick_res;
}

/* ------------------------------------------------------------------------- */
/* Abertura de documento                                                      */

/* Conta operadores de exibicao de texto num content stream descomprimido. */
static long count_text_ops(const unsigned char *data, int len)
{
    PdfMemCtx mc;
    PdfIo     io;
    PdfStream st;
    PdfArena  a;

    pdf_io_mem(&io, &mc, data, len);
    if (pdf_stream_init(&st, &io) != 0)
        return 0;
    pdf_arena_init(&a, 8192);

    long n = 0;
    for (;;) {
        PdfTok t;
        pdf_lex_next(&st, &a, &t);
        if (t.kind == PT_EOF)
            break;
        if (t.kind == PT_KEYWORD &&
            (pdf_tok_is(&t, "Tj") || pdf_tok_is(&t, "TJ") ||
             pdf_tok_is(&t, "'")  || pdf_tok_is(&t, "\"")))
            n++;
        /* A arena acumula um nome por token; reciclar mantem o uso constante
         * num stream de centenas de KB. */
        if (a.handed > 192u * 1024u)
            pdf_arena_reset(&a);
    }
    pdf_arena_free(&a);
    return n;
}

/*
 * Amostra a camada de texto em 4 paginas espalhadas.
 *
 * Nao todas: no "Comunicacao Nao-Violenta" sao 229 paginas e 3,1 MB
 * descomprimidos, o que no console levaria muitos segundos com a tela
 * congelada. E nao apenas a primeira: a pagina 1 daquele livro e a capa, so
 * imagem, e reportaria "sem texto" para um livro que tem 209 paginas de texto.
 */
static void sample_text_layer(void)
{
    u64 t0;
    sceRtcGetCurrentTick(&t0);

    g_dv.sampled = 0;
    g_dv.sampled_with_text = 0;
    g_dv.sampled_ops = 0;
    g_dv.sample_err[0] = '\0';

    int total = g_doc.pdf.pages;
    if (total <= 0)
        return;

    int probes[4];
    probes[0] = 0;
    probes[1] = total / 4;
    probes[2] = total / 2;
    probes[3] = (total * 3) / 4;

    for (int i = 0; i < 4; ++i) {
        int idx = probes[i];
        if (idx < 0 || idx >= total)
            continue;
        /* Nao amostra a mesma pagina duas vezes num livro curto. */
        int dup = 0;
        for (int k = 0; k < i; ++k)
            if (probes[k] == idx)
                dup = 1;
        if (dup)
            continue;

        PdfArena a;
        pdf_arena_init(&a, 32768);

        PdfObj *page = pdf_doc_page(&g_doc.pdf, &a, idx);
        if (page) {
            unsigned char *data = NULL;
            int len = 0;
            PdfFiltStatus st = pdf_doc_page_content(&g_doc.pdf, &a, page,
                                                    &data, &len);
            if (st == PDF_FILT_OK) {
                long ops = count_text_ops(data, len);
                g_dv.sampled++;
                if (ops > 0) {
                    g_dv.sampled_with_text++;
                    g_dv.sampled_ops += ops;
                }
            } else if (!g_dv.sample_err[0]) {
                snprintf(g_dv.sample_err, sizeof(g_dv.sample_err),
                         "pag %d: %s", idx + 1, pdf_filt_status_str(st));
            }
        }
        pdf_arena_free(&a);
    }

    g_dv.sample_ms = ms_since(t0);
}

/* ------------------------------------------------------------------------- */
/* Leitura                                                                    */

/*
 * Adaptador de medicao para o layout.
 *
 * O layout mede por callback para ser portavel e testavel no host, onde nao
 * existe FreeType. Do lado do console o callback e o font.c de verdade, com o
 * glifo rasterizado - a largura aqui e a largura que vai para a tela, nao uma
 * estimativa.
 */
static float measure_glyphs(void *ud, const char *utf8, int nbytes)
{
    (void)ud;
    return font_measure(utf8, nbytes);
}

static void read_layout_params(LayoutFont *lf, LayoutOpts *lo)
{
    lf->ud = NULL;
    lf->measure = measure_glyphs;
    lf->line_height = font_line_height();

    layout_defaults(lo);
    /* Recuo de um em: a convencao tipografica, e em 480 px de largura um recuo
     * maior come uma palavra inteira da primeira linha. */
    lo->indent = (float)font_px();
}

#define READ_TEXT_W ((float)GFX_SCR_W - 2.0f * MARGIN_X)
#define READ_TEXT_H (BODY_BOT - BODY_TOP)

/*
 * Carrega uma pagina do PDF e a repagina inteira.
 *
 * `last_screen` existe para o sentido INVERSO da leitura: ao voltar da primeira
 * tela de uma pagina, o usuario espera cair na ULTIMA tela da pagina anterior,
 * e nao na primeira. Sem isso, folhear para tras pula cinco telas de cada vez.
 */
static void read_load_page(int page, int last_screen)
{
    if (page < 0)
        page = 0;
    if (g_doc.units > 0 && page >= g_doc.units)
        page = g_doc.units - 1;

    /* Reciclagem, nao liberacao: os blocos ja pagos ao sistema sao
     * reaproveitados, entao virar pagina nao chama o alocador. */
    pdf_arena_reset(&g_rv.arena);
    memset(&g_rv.rf, 0, sizeof(g_rv.rf));
    memset(&g_rv.lay, 0, sizeof(g_rv.lay));
    g_rv.ok = 0;
    g_rv.screen = 0;
    g_rv.page = page;
    memset(&g_rv.stats, 0, sizeof(g_rv.stats));

    u64 t0;
    sceRtcGetCurrentTick(&t0);

    DocStats ds;
    if (doc_unit(&g_doc, &g_rv.arena, page, &g_rv.rf, &ds) == 0) {
        g_rv.stats = ds;
        LayoutFont lf;
        LayoutOpts lo;
        read_layout_params(&lf, &lo);
        if (layout_build(&g_rv.arena, &g_rv.rf, &lf,
                         READ_TEXT_W, READ_TEXT_H, &lo, &g_rv.lay) == 0)
            g_rv.ok = 1;
    }

    if (last_screen && g_rv.lay.npages > 0)
        g_rv.screen = g_rv.lay.npages - 1;

    g_rv.ms = ms_since(t0);
}

/* Vai para a tela que contem um paragrafo. Usado ao retomar a leitura e ao
 * trocar o corpo da fonte - os dois casos em que a tela muda de numero mas o
 * lugar do texto nao. */
static void read_seek_para(int para)
{
    if (para <= 0 || !g_rv.ok)
        return;
    for (int i = 0; i < g_rv.lay.nlines; ++i) {
        if (g_rv.lay.lines[i].para >= para) {
            int p = layout_page_of_line(&g_rv.lay, i);
            if (p >= 0)
                g_rv.screen = p;
            return;
        }
    }
}

/*
 * Comeca a ler.
 *
 * Retoma de onde parou, se houver registro. Caso contrario abre na primeira
 * unidade COM TEXTO: a pagina 1 de um PDF e quase sempre a capa, so imagem, e
 * abrir o livro nela daria a impressao de que nao ha texto nenhum no arquivo.
 */
static void read_start(void)
{
    int unit = 0, para = 0, px = 0;
    if (progress_get(&g_prog, g_dv.name, g_dv.fsize, &unit, NULL, &para, &px) == 0) {
        if (px >= FONT_PX_MIN && px <= FONT_PX_MAX && px != font_px())
            font_set_px(px);
        if (unit >= 0 && unit < g_doc.units) {
            read_load_page(unit, 0);
            read_seek_para(para);
            return;
        }
    }

    int limit = g_doc.units < 8 ? g_doc.units : 8;
    for (int i = 0; i < limit; ++i) {
        read_load_page(i, 0);
        if (g_rv.ok && g_rv.lay.npages > 0)
            return;
    }
    read_load_page(0, 0);
}

/* --- virada de tela -------------------------------------------------------
 * A tela e a unidade de leitura; a pagina do PDF e so a unidade de carga. Por
 * isso a virada atravessa a fronteira da pagina sozinha: o usuario nao deveria
 * precisar saber que existe uma. */

/*
 * Grava o progresso na troca de UNIDADE, nao na troca de tela.
 *
 * A cada tela seriam varias escritas por minuto no Memory Stick, que e a
 * operacao mais lenta do console e a que mais desgasta o cartao. A cada unidade
 * sao algumas por hora, e o que se perde num desligamento no botao - o jeito
 * como um PSP e desligado - e no maximo uma pagina de leitura. Gravar so no
 * fechamento seria mais barato ainda e perderia a sessao inteira, que e
 * exatamente o caso que o progresso existe para cobrir.
 */
static void book_remember(void);      /* definido junto do resto do livro */

static void read_flush_progress(void)
{
    book_remember();
    progress_save(&g_prog, fs_state_path("progress.txt"));
}

static void read_next_screen(void)
{
    if (g_rv.ok && g_rv.screen + 1 < g_rv.lay.npages) {
        g_rv.screen++;
        return;
    }
    if (g_rv.page + 1 < g_doc.units) {
        read_load_page(g_rv.page + 1, 0);
        read_flush_progress();
    }
}

static void read_prev_screen(void)
{
    if (g_rv.screen > 0) {
        g_rv.screen--;
        return;
    }
    if (g_rv.page > 0) {
        read_load_page(g_rv.page - 1, 1);
        read_flush_progress();
    }
}

/*
 * Troca do corpo da fonte.
 *
 * Mudar o corpo muda a largura de todo glifo, entao a paginacao inteira e outra
 * - nao ha como ajustar a existente. O caminho e repaginar, e para isso a pagina
 * e recarregada: e mais simples que manter duas arenas, e o custo (uma extracao)
 * e o mesmo de virar uma pagina, que o usuario ja aceita.
 *
 * O que NAO pode se perder e o lugar da leitura. Guardamos o paragrafo do topo
 * da tela e, depois de repaginar, voltamos para a tela onde ele foi cair.
 * Guardar o numero da tela nao serviria: com corpo maior ha mais telas, e a
 * tela 3 de antes nao e a tela 3 de agora.
 */
static void read_set_px(int px)
{
    int para = -1;
    if (g_rv.ok && g_rv.lay.npages > 0) {
        int l0 = g_rv.lay.page_first[g_rv.screen];
        if (l0 >= 0 && l0 < g_rv.lay.nlines)
            para = g_rv.lay.lines[l0].para;
    }

    if (font_set_px(px) != 0)
        return;

    read_load_page(g_rv.page, 0);
    read_seek_para(para);
}

/*
 * Anota onde a leitura esta, no registro em memoria.
 *
 * Guarda o PARAGRAFO do topo da tela, e nao o numero da tela: o numero da tela
 * so significa alguma coisa junto com o corpo da fonte em que ele foi contado, e
 * o usuario troca o corpo. O paragrafo e a mesma coisa em qualquer corpo.
 */
static void book_remember(void)
{
    if (!g_dv.open || g_dv.rc != 0 || !g_dv.name[0])
        return;

    int para = 0;
    if (g_rv.ok && g_rv.lay.npages > 0) {
        int l0 = g_rv.lay.page_first[g_rv.screen];
        if (l0 >= 0 && l0 < g_rv.lay.nlines)
            para = g_rv.lay.lines[l0].para;
    }
    progress_set(&g_prog, g_dv.name, g_dv.fsize, g_rv.page, g_doc.units, para, font_px());
}

static void book_close(void)
{
    if (g_dv.open) {
        book_remember();
        progress_save(&g_prog, fs_state_path("progress.txt"));
        doc_close(&g_doc);
        g_dv.open = 0;
    }
    memset(&g_dv, 0, sizeof(g_dv));
    pdf_arena_reset(&g_rv.arena);
    pdf_arena_reset(&g_doc_arena);
    memset(&g_rv.stats, 0, sizeof(g_rv.stats));
    memset(&g_rv.rf, 0, sizeof(g_rv.rf));
    memset(&g_rv.lay, 0, sizeof(g_rv.lay));
    g_rv.ok = 0;
    g_rv.screen = 0;
    g_mode = MODE_BROWSE;
}

static DocKind kind_of(LibFormat f)
{
    switch (f) {
    case LIB_FMT_PDF:  return DOC_PDF;
    case LIB_FMT_TXT:  return DOC_TXT;
    case LIB_FMT_EPUB: return DOC_EPUB;
    default:           return DOC_NONE;
    }
}

static void book_open(const LibEntry *it)
{
    book_close();
    snprintf(g_dv.name, sizeof(g_dv.name), "%s", it->name);
    g_dv.fsize = it->size;

    char path[320];
    snprintf(path, sizeof(path), "%s%s", fs_books_dir(), it->name);

    u64 t0;
    sceRtcGetCurrentTick(&t0);

    PdfIo io;
    if (psp_io_open(&io, path) != 0) {
        g_dv.rc = -100;
        g_dv.open_ms = ms_since(t0);
        g_mode = MODE_DOC;
        return;
    }

    g_dv.rc = doc_open(&g_doc, &g_doc_arena, &io, kind_of(it->fmt));
    g_dv.open_ms = ms_since(t0);
    g_dv.open = 1;            /* aberto o bastante para precisar de close */
    g_mode = MODE_DOC;

    /* A amostragem de camada de texto responde "este PDF tem texto ou e uma
     * pilha de imagens?". Em TXT a pergunta nao existe. */
    if (g_dv.rc == 0 && g_doc.kind == DOC_PDF)
        sample_text_layer();
}

/* ------------------------------------------------------------------------- */
/* Telas                                                                      */

static int lib_rows(void)
{
    float step = font_line_height() * 1.18f;
    if (step <= 0.0f)
        return 1;
    int n = (int)((BODY_BOT - BODY_TOP) / step);
    return n < 1 ? 1 : n;
}

static void lib_clamp(void)
{
    if (g_lib.count <= 0) {
        g_cursor = g_view = 0;
        return;
    }
    if (g_cursor < 0)
        g_cursor = 0;
    if (g_cursor >= g_lib.count)
        g_cursor = g_lib.count - 1;

    int rows = lib_rows();
    if (g_cursor < g_view)
        g_view = g_cursor;
    if (g_cursor >= g_view + rows)
        g_view = g_cursor - rows + 1;
    if (g_view > g_lib.count - rows)
        g_view = g_lib.count - rows;
    if (g_view < 0)
        g_view = 0;
}

static void screen_library(void)
{
    float step = font_line_height() * 1.18f;
    float y = BODY_TOP + font_ascender();

    if (g_lib.err < 0) {
        char buf[96];
        snprintf(buf, sizeof(buf), "sceIoDopen falhou: 0x%08X",
                 (unsigned int)g_lib.err);
        font_draw(MARGIN_X, y, buf, -1, COL_ERR);
        y += step;
        draw_wrapped(MARGIN_X, y, (float)GFX_SCR_W - 2 * MARGIN_X,
                     fs_books_dir(), COL_DIM, 1.1f);
        return;
    }

    if (g_lib.count == 0) {
        font_draw(MARGIN_X, y, "Nenhum EPUB, PDF ou TXT em:", -1, COL_INK);
        y += step;
        draw_wrapped(MARGIN_X, y, (float)GFX_SCR_W - 2 * MARGIN_X,
                     fs_books_dir(), COL_DIM, 1.1f);
        y += step * 0.4f;
        draw_wrapped(MARGIN_X, y, (float)GFX_SCR_W - 2 * MARGIN_X,
                     "Conecte o USB e copie seus arquivos para essa pasta.",
                     COL_DIM, 1.1f);
        return;
    }

    int rows = lib_rows();
    float row_h = step;

    for (int i = 0; i < rows; ++i) {
        int idx = g_view + i;
        if (idx >= g_lib.count)
            break;

        float top = BODY_TOP + (float)i * row_h;
        int sel = (idx == g_cursor);

        if (sel)
            gfx_fill_rect(MARGIN_X - 6.0f, top,
                          (float)GFX_SCR_W - 2.0f * (MARGIN_X - 6.0f), row_h,
                          COL_SEL);

        float base = top + font_ascender();

        /* Tamanho a direita, desenhado primeiro para saber quanta largura
         * sobra para o nome. */
        char sz[32];
        unsigned int kb = g_lib.items[idx].size / 1024u;
        if (kb >= 1024u)
            snprintf(sz, sizeof(sz), "%u,%u MB",
                     kb / 1024u, ((kb % 1024u) * 10u) / 1024u);
        else
            snprintf(sz, sizeof(sz), "%u KB", kb);
        float szw = font_measure(sz, -1);
        font_draw((float)GFX_SCR_W - MARGIN_X - szw, base, sz, -1, COL_DIM);

        float right = (float)GFX_SCR_W - MARGIN_X - szw - 10.0f;

        /*
         * Quanto ja foi lido, ANTES de abrir.
         *
         * E o que transforma a lista de arquivos numa estante: sem isso, saber
         * onde cada livro esta exige abrir um por um. O total de unidades vem
         * guardado junto com a posicao justamente para esta linha - calcula-lo
         * aqui obrigaria a abrir os cinquenta arquivos a cada varredura.
         */
        int pu = 0, ptot = 0;
        if (progress_get(&g_prog, g_lib.items[idx].name, g_lib.items[idx].size,
                         &pu, &ptot, NULL, NULL) == 0 && ptot > 0) {
            char pc[16];
            int pct = (pu * 100) / ptot;
            if (pct > 100) pct = 100;
            snprintf(pc, sizeof(pc), "%d%%", pct);
            float pw = font_measure(pc, -1);
            font_draw(right - pw, base, pc, -1, COL_OK);
            right -= pw + 10.0f;
        }

        draw_clip(MARGIN_X, base, right - MARGIN_X, g_lib.items[idx].name,
                  COL_INK);
    }
}

static void screen_doc(void)
{
    float y = BODY_TOP + font_ascender();
    char buf[160];

    draw_clip(MARGIN_X, y, (float)GFX_SCR_W - 2 * MARGIN_X, g_dv.name, COL_INK);
    y += font_line_height() * 1.3f;

    if (g_dv.rc == -100) {
        font_draw(MARGIN_X, y, "nao foi possivel abrir o arquivo", -1, COL_ERR);
        return;
    }

    if (g_dv.rc != 0) {
        font_draw(MARGIN_X, y, "este arquivo foi recusado", -1, COL_ERR);
        y += font_line_height() * 1.1f;
        draw_wrapped(MARGIN_X, y, (float)GFX_SCR_W - 2 * MARGIN_X,
                     g_doc.err[0] ? g_doc.err : "sem detalhe", COL_DIM, 1.1f);
        return;
    }

    if (g_doc.title[0])
        draw_kv(&y, "titulo", g_doc.title, COL_INK);
    if (g_doc.author[0])
        draw_kv(&y, "autor", g_doc.author, COL_INK);

    if (g_doc.kind == DOC_TXT) {
        snprintf(buf, sizeof(buf), "texto puro   %s   %d blocos%s",
                 txt_encoding_name(g_doc.txt.enc), g_doc.units,
                 g_doc.txt.truncated ? "  (TRUNCADO)" : "");
        draw_kv(&y, "formato", buf, COL_INK);
        snprintf(buf, sizeof(buf), "%.0f ms para indexar", g_dv.open_ms);
        draw_kv(&y, "abertura", buf, COL_INK);
    } else if (g_doc.kind == DOC_EPUB) {
        snprintf(buf, sizeof(buf), "EPUB   %d capitulos em %d unidades%s",
                 g_doc.epub.nspine, g_doc.units,
                 g_doc.epub.truncated ? "  (TRUNCADO)" : "");
        draw_kv(&y, "formato", buf, COL_INK);
        snprintf(buf, sizeof(buf), "%.0f ms   %d membros no ZIP",
                 g_dv.open_ms, g_doc.epub.zip.nentries);
        draw_kv(&y, "abertura", buf, COL_INK);
    } else {
        snprintf(buf, sizeof(buf), "PDF-%d.%d   %d paginas   %d objetos",
                 g_doc.pdf.ver_major, g_doc.pdf.ver_minor, g_doc.units,
                 g_doc.pdf.xref_len);
        draw_kv(&y, "formato", buf, COL_INK);

        snprintf(buf, sizeof(buf), "%.0f ms   %lld leituras de 4 KB",
                 g_dv.open_ms, g_doc.pdf.st.reads);
        draw_kv(&y, "abertura", buf, COL_INK);

        if (g_doc.pdf.reconstructed)
            draw_kv(&y, "aviso", "xref reconstruido por varredura", COL_ERR);
        if (g_doc.pdf.encrypted)
            draw_kv(&y, "aviso", "documento criptografado", COL_ERR);
    }

    /* Onde a leitura parou, se ja houve leitura. E a informacao que o usuario
     * procura nesta tela depois da primeira vez. */
    int pu = 0, pp = 0;
    if (progress_get(&g_prog, g_dv.name, g_dv.fsize, &pu, NULL, &pp, NULL) == 0) {
        snprintf(buf, sizeof(buf), "%s %d de %d",
                 g_doc.kind == DOC_PDF ? "pagina" : "trecho", pu + 1,
                 g_doc.units);
        draw_kv(&y, "retomar em", buf, COL_OK);
    }

    if (g_doc.kind != DOC_PDF)
        return;

    /* O veredito de camada de texto, que so o PDF precisa: e ele que decide se
     * este arquivo tem alguma coisa para ler ou e uma pilha de imagens. EPUB e
     * TXT nao tem essa duvida - se abriram, tem texto - e cair aqui faria os
     * dois exibirem "nenhuma pagina amostrada" em vermelho, um erro que nao
     * existe. */
    y += font_line_height() * 0.3f;
    if (g_dv.sampled == 0) {
        snprintf(buf, sizeof(buf), "nenhuma pagina amostrada  %s",
                 g_dv.sample_err);
        font_draw(MARGIN_X, y, buf, -1, COL_ERR);
    } else if (g_dv.sampled_with_text == 0) {
        snprintf(buf, sizeof(buf),
                 "SEM CAMADA DE TEXTO nas %d paginas amostradas", g_dv.sampled);
        draw_wrapped(MARGIN_X, y, (float)GFX_SCR_W - 2 * MARGIN_X,
                     buf, COL_ERR, 1.1f);
    } else {
        snprintf(buf, sizeof(buf),
                 "texto em %d de %d amostras, %ld operadores (%.0f ms)",
                 g_dv.sampled_with_text, g_dv.sampled, g_dv.sampled_ops,
                 g_dv.sample_ms);
        draw_wrapped(MARGIN_X, y, (float)GFX_SCR_W - 2 * MARGIN_X,
                     buf, COL_OK, 1.1f);
    }
}

/* --- tela de leitura ------------------------------------------------------ */

/*
 * Desenho de uma tela repaginada.
 *
 * Aqui nao ha mais decisao nenhuma: o layout ja resolveu quais bytes vao em qual
 * linha, com qual deslocamento e a que altura. O laco e uma soma. Foi por isso
 * que a quebra saiu daqui para um modulo portavel - a decisao e testavel no
 * host, o desenho nao precisa ser.
 */
static void screen_read(void)
{
    float y = BODY_TOP + font_ascender();
    char buf[160];

    if (!g_rv.ok) {
        snprintf(buf, sizeof(buf), "pagina %d: nao foi possivel extrair texto",
                 g_rv.page + 1);
        font_draw(MARGIN_X, y, buf, -1, COL_ERR);
        return;
    }
    if (g_rv.lay.npages == 0) {
        snprintf(buf, sizeof(buf), "pagina %d sem camada de texto",
                 g_rv.page + 1);
        font_draw(MARGIN_X, y, buf, -1, COL_ERR);
        y += font_line_height() * 1.3f;
        draw_wrapped(MARGIN_X, y, READ_TEXT_W,
                     "Provavelmente uma imagem: capa, ilustracao ou pagina "
                     "escaneada. Use L/R para ir a proxima.", COL_DIM, 1.1f);
        return;
    }

    if (g_rv.screen < 0)
        g_rv.screen = 0;
    if (g_rv.screen >= g_rv.lay.npages)
        g_rv.screen = g_rv.lay.npages - 1;

    int a0 = g_rv.lay.page_first[g_rv.screen];
    int a1 = g_rv.lay.page_first[g_rv.screen + 1];

    for (int i = a0; i < a1; ++i) {
        const LayoutLine *l = &g_rv.lay.lines[i];
        font_draw(MARGIN_X + l->x, y + l->top, g_rv.rf.buf + l->off, l->len,
                  COL_INK);
    }

    if (!g_read_diag)
        return;

    /* Painel por cima do texto, no pe: cobre o que estava ali, mas o objetivo e
     * justamente comparar o numero com a pagina que o produziu. */
    float step = font_line_height() * 1.05f;
    float ph   = step * 4.0f + 8.0f;
    float py   = BODY_BOT - ph;
    gfx_fill_rect(0.0f, py, (float)GFX_SCR_W, ph, COL_SEL);

    float dy = py + 4.0f + font_ascender();

    snprintf(buf, sizeof(buf), "origem %d linhas, %d fora   %d col   %d hifens",
             g_rv.rf.lines_in, g_rv.rf.lines_dropped, g_rv.rf.columns,
             g_rv.rf.hyphen_joins);
    font_draw(MARGIN_X, dy, buf, -1, COL_INK);
    dy += step;

    snprintf(buf, sizeof(buf), "corpo %.1fpt  entrelinha %.1fpt  margens %.0f-%.0f",
             g_rv.rf.body_size, g_rv.rf.leading, g_rv.rf.body_x0,
             g_rv.rf.body_x1);
    font_draw(MARGIN_X, dy, buf, -1, COL_INK);
    dy += step;

    snprintf(buf, sizeof(buf), "%d paragrafos -> %d linhas de tela%s%s",
             g_rv.rf.nparas, g_rv.lay.nlines,
             g_rv.rf.truncated ? "  RF-TRUNC" : "",
             g_rv.lay.truncated ? "  LAY-TRUNC" : "");
    font_draw(MARGIN_X, dy, buf, -1,
              (g_rv.rf.truncated || g_rv.lay.truncated) ? COL_ERR : COL_INK);
    dy += step;

    snprintf(buf, sizeof(buf), "%d runs, %d vert, %d sem unicode   %.0fms   %u KB",
             g_rv.stats.runs, g_rv.stats.runs_vertical, g_rv.stats.codes_unmapped,
             g_rv.ms,
             (unsigned int)(g_rv.arena.reserved / 1024u));
    font_draw(MARGIN_X, dy, buf, -1,
              g_rv.stats.codes_unmapped > 0 ? COL_ERR : COL_INK);
}

/* --- amostra de tipografia ------------------------------------------------ */

static const char *SAMPLE[] = {
    "A restrição de licença empurrou o projeto para a arquitetura melhor — "
    "e isso raramente acontece.",

    "“Não há camada de texto neste PDF”, dirá o leitor quando encontrar uma "
    "página escaneada… e essa é a única resposta honesta.",

    "Ação, coração, ambiguïdade, êxito, órgão, açúcar, você, três, sótão, "
    "público, tênue, ímã, cônjuge.",

    "Fora do Latin-1: → ← ≠ ≤ ∞ § ¶ † ‡ • ‰ ∑ √",
    NULL
};

#define SAMPLE_LINES_MAX 64
#define SAMPLE_LINE_MUL  1.35f
#define SAMPLE_PARA_GAP  0.45f

typedef struct {
    const char *p;
    int n;
    int para_end;
} SampleLine;

static SampleLine g_lines[SAMPLE_LINES_MAX];
static int g_line_count = 0;
static int g_line_px    = -1;
static int g_top_line   = 0;

static void build_sample_lines(void)
{
    g_line_count = 0;
    float max_w = (float)GFX_SCR_W - 2.0f * MARGIN_X;

    for (int i = 0; SAMPLE[i] && g_line_count < SAMPLE_LINES_MAX; ++i) {
        const char *p = SAMPLE[i];
        int rem = (int)strlen(p);
        while (rem > 0 && g_line_count < SAMPLE_LINES_MAX) {
            int n = font_line_break(p, rem, max_w, NULL);
            if (n <= 0)
                break;
            g_lines[g_line_count].p        = p;
            g_lines[g_line_count].n        = n;
            g_lines[g_line_count].para_end = 0;
            g_line_count++;
            p   += n;
            rem -= n;
        }
        if (g_line_count > 0)
            g_lines[g_line_count - 1].para_end = 1;
    }
    g_line_px = font_px();
}

/*
 * Quantas linhas caberiam comecando em `start`.
 *
 * Percorre exatamente o mesmo avanco do desenho, espacos de paragrafo
 * incluidos. Uma contagem que divide a altura util pelo passo de linha e ignora
 * esses espacos mente - e foi assim que a ultima linha do texto ficou
 * inalcancavel na primeira versao desta tela.
 */
static int sample_lines_from(int start)
{
    float step = font_line_height() * SAMPLE_LINE_MUL;
    float gap  = font_line_height() * SAMPLE_PARA_GAP;
    float y    = BODY_TOP + font_ascender();
    int   n    = 0;

    for (int i = start; i < g_line_count; ++i) {
        if (y > BODY_BOT)
            break;
        n++;
        y += step;
        if (g_lines[i].para_end)
            y += gap;
    }
    return n < 1 ? 1 : n;
}

static void sample_clamp(void)
{
    if (g_line_count <= 0) {
        g_top_line = 0;
        return;
    }
    if (g_top_line < 0)
        g_top_line = 0;
    if (g_top_line > g_line_count - 1)
        g_top_line = g_line_count - 1;
    while (g_top_line > 0 &&
           (g_top_line - 1) + sample_lines_from(g_top_line - 1) >= g_line_count)
        g_top_line--;
}

static void screen_sample(void)
{
    if (g_line_px != font_px()) {
        build_sample_lines();
        sample_clamp();
    }

    float step = font_line_height() * SAMPLE_LINE_MUL;
    float y = BODY_TOP + font_ascender();

    for (int i = g_top_line; i < g_line_count; ++i) {
        if (y > BODY_BOT)
            break;
        font_draw(MARGIN_X, y, g_lines[i].p, g_lines[i].n, COL_INK);
        y += step;
        if (g_lines[i].para_end)
            y += font_line_height() * SAMPLE_PARA_GAP;
    }
}

static void screen_diag(void)
{
    float y = BODY_TOP + font_ascender();
    char buf[192];

    snprintf(buf, sizeof(buf), "%s %s", font_family(), font_style());
    draw_kv(&y, "fonte", buf, COL_INK);

    snprintf(buf, sizeof(buf), "%d px   linha %.1f   asc %.1f   desc %.1f",
             font_px(), font_line_height(), font_ascender(), font_descender());
    draw_kv(&y, "corpo", buf, COL_DIM);

    snprintf(buf, sizeof(buf), "%d glifos%s   %u KB",
             font_glyphs_cached(), font_atlas_full() ? " (CHEIO)" : "",
             font_atlas_bytes() / 1024u);
    draw_kv(&y, "atlas", buf, font_atlas_full() ? COL_ERR : COL_DIM);

    snprintf(buf, sizeof(buf), "VRAM %u KB   heap 16 MB   sistema %u KB livre",
             gfx_vram_used() / 1024u,
             (unsigned int)(sceKernelTotalFreeMemSize() / 1024));
    draw_kv(&y, "memoria", buf, COL_DIM);

    y += font_line_height() * 0.4f;
    font_draw(MARGIN_X, y, "caminhos resolvidos de argv[0]:", -1, COL_INK);
    y += font_line_height() * 1.1f;
    y = draw_wrapped(MARGIN_X, y, (float)GFX_SCR_W - 2 * MARGIN_X,
                     fs_base(), COL_DIM, 1.05f);
    y = draw_wrapped(MARGIN_X, y, (float)GFX_SCR_W - 2 * MARGIN_X,
                     fs_books_dir(), COL_DIM, 1.05f);
}

/* --- barras --------------------------------------------------------------- */

static void draw_bar(int screen)
{
    gfx_fill_rect(0.0f, 0.0f, (float)GFX_SCR_W, BAR_H, COL_BAR);

    const char *title = (g_mode == MODE_DOC)  ? "DOCUMENTO"
                      : (g_mode == MODE_READ) ? "LEITURA"
                      : SCREEN_NAME[screen];
    font_draw(MARGIN_X, 16.0f, title, -1, COL_BAR_TXT);

    char buf[48];
    if (g_mode == MODE_READ) {
        /* Duas contagens porque sao duas coisas diferentes: a tela e onde o
         * leitor esta, a pagina do PDF e de onde o texto veio. Mostrar so a
         * segunda faria o contador parecer travado por cinco viradas. */
        /*
         * O "!" avisa que esta unidade perdeu texto por estourar um teto
         * interno. E raro, mas silenciar seria pior que tudo: o leitor
         * continuaria lendo sem saber que faltou um pedaco.
         */
        snprintf(buf, sizeof(buf), "%d/%d  %c%d/%d  %dpx%s",
                 g_rv.lay.npages > 0 ? g_rv.screen + 1 : 0, g_rv.lay.npages,
                 g_doc.kind == DOC_PDF ? 'p' : 'u',
                 g_rv.page + 1, g_doc.units, font_px(),
                 (g_rv.rf.truncated || g_rv.lay.truncated) ? " !" : "");
    } else if (g_mode == MODE_DOC) {
        snprintf(buf, sizeof(buf), "%d px", font_px());
    } else if (screen == SCREEN_LIBRARY && g_lib.count > 0) {
        snprintf(buf, sizeof(buf), "%d/%d", g_cursor + 1, g_lib.count);
    } else if (screen == SCREEN_SAMPLE && g_line_count > 0) {
        int last = g_top_line + sample_lines_from(g_top_line);
        if (last > g_line_count)
            last = g_line_count;
        snprintf(buf, sizeof(buf), "%d-%d/%d  %dpx",
                 g_top_line + 1, last, g_line_count, font_px());
    } else {
        snprintf(buf, sizeof(buf), "%d px", font_px());
    }
    float w = font_measure(buf, -1);
    font_draw((float)GFX_SCR_W - MARGIN_X - w, 16.0f, buf, -1, COL_BAR_TXT);
}

static void draw_footer(int screen)
{
    gfx_fill_rect(0.0f, (float)GFX_SCR_H - FOOTER_H, (float)GFX_SCR_W,
                  FOOTER_H, COL_BAR);

    const char *full, *shortv;
    if (g_mode == MODE_READ) {
        full   = "L/R virar   CIMA/BAIXO corpo   SELECT tema   ^ diag   O";
        shortv = "L/R virar  ^v corpo  O";
    } else if (g_mode == MODE_DOC) {
        full   = "X ler o texto   SELECT tema   O voltar para a biblioteca";
        shortv = "X ler   O voltar";
    } else if (screen == SCREEN_LIBRARY) {
        full   = "X abrir   CIMA/BAIXO escolher   L/R telas   SELECT tema";
        shortv = "X abrir  ^v  L/R";
    } else if (screen == SCREEN_SAMPLE) {
        full   = "ESQ/DIR rolar   CIMA/BAIXO corpo   L/R telas";
        shortv = "</> rolar  ^v corpo";
    } else {
        full   = "CIMA/BAIXO corpo   L/R telas   START sair";
        shortv = "^v corpo  L/R";
    }

    /* Escolhe pela largura medida: em 34 px a dica longa nao cabe em 480 px e
     * sairia cortada no meio de uma palavra. */
    float avail = (float)GFX_SCR_W - 2.0f * MARGIN_X;
    const char *hint = (font_measure(full, -1) <= avail) ? full : shortv;
    font_draw(MARGIN_X, (float)GFX_SCR_H - 6.0f, hint, -1, COL_BAR_TXT);
}

/* ------------------------------------------------------------------------- */

static void font_failure_loop(int err, const char *path)
{
    pspDebugScreenInit();
    pspDebugScreenSetXY(0, 2);
    pspDebugScreenPrintf("PSP Reader - FALHA AO CARREGAR A FONTE\n\n");
    pspDebugScreenPrintf("  font_init retornou %d\n", err);
    pspDebugScreenPrintf("  caminho: %s\n\n", path);
    switch (err) {
    case -1: pspDebugScreenPrintf("  FT_Init_FreeType falhou\n"); break;
    case -2: pspDebugScreenPrintf("  arquivo nao encontrado ou nao e um TTF\n"
                                  "  o data/ chegou junto do EBOOT?\n"); break;
    case -3: pspDebugScreenPrintf("  sem memoria para o atlas (1 MB)\n"); break;
    case -4: pspDebugScreenPrintf("  corpo de fonte fora da faixa\n"); break;
    default: pspDebugScreenPrintf("  erro desconhecido\n"); break;
    }
    pspDebugScreenPrintf("\n  START para sair\n");

    Input in;
    memset(&in, 0, sizeof(in));
    input_init();
    for (;;) {
        input_update(&in, 1.0f / 60.0f);
        if (in.pressed & PSP_CTRL_START)
            break;
        sceDisplayWaitVblankStart();
    }
    sceKernelExitGame();
}

int main(int argc, char *argv[])
{
    (void)argc;
    setup_callbacks();

    fs_init(argv ? argv[0] : NULL);
    fs_ensure_dirs();

    /*
     * O progresso e lido ANTES do video.
     *
     * Ele traz o tema, e o tema decide a cor do primeiro quadro. Carregar
     * depois faria o app abrir sempre em "Papel" e piscar para "Noite" no
     * primeiro frame - um flash branco na cara de quem escolheu tema escuro
     * justamente por estar no escuro.
     */
    progress_load(&g_prog, fs_state_path("progress.txt"));
    g_th = theme_get(g_prog.theme);

    gfx_init();
    input_init();

    const char *font_path = fs_path("data/Literata.ttf");
    int frc = font_init(font_path, 18);
    if (frc != 0) {
        gfx_shutdown();
        font_failure_loop(frc, font_path);
        return 0;
    }

    library_scan(&g_lib, fs_books_dir());
    lib_clamp();

    /* Blocos de 64 KB: uma pagina densa custa ~270 KB entre runs, texto e
     * linhas, entao blocos grandes reduzem o numero de idas ao alocador e a
     * reciclagem por virada de pagina reaproveita todos eles. */
    pdf_arena_init(&g_rv.arena, 64 * 1024);

    /* Arena do DOCUMENTO: vive enquanto o livro estiver aberto e nao e
     * reciclada por virada de tela. Blocos menores porque o que mora nela e um
     * indice, nao paginas. */
    pdf_arena_init(&g_doc_arena, 16 * 1024);

    Input in;
    memset(&in, 0, sizeof(in));

    g_tick_res = sceRtcGetTickResolution();
    u64 prev_tick;
    sceRtcGetCurrentTick(&prev_tick);

    int screen = SCREEN_LIBRARY;
    int running = 1;

    while (running) {
        u64 now;
        sceRtcGetCurrentTick(&now);
        float dt = (float)(now - prev_tick) / (float)g_tick_res;
        prev_tick = now;
        if (dt > 0.25f)
            dt = 0.25f;

        input_update(&in, dt);

        if (in.pressed & PSP_CTRL_START)
            running = 0;

        /* Tema em SELECT, valido em qualquer tela: a luz do ambiente muda
         * independentemente de onde o usuario esta no app. */
        if (in.pressed & PSP_CTRL_SELECT) {
            int t = (g_prog.theme + 1) % THEME_COUNT;
            progress_set_theme(&g_prog, t);
            g_th = theme_get(t);
        }

        if (g_mode == MODE_READ) {
            /*
             * Virar tela, e nao rolar.
             *
             * Depois do reflow o texto esta paginado de verdade, entao o gesto
             * e o de um livro: uma tela inteira por vez. Nas quatro teclas -
             * L/R e ESQ/DIR - porque nao existe motivo para o leitor descobrir
             * qual delas o autor preferiu.
             */
            if (in.repeated & (PSP_CTRL_RTRIGGER | PSP_CTRL_RIGHT))
                read_next_screen();
            if (in.repeated & (PSP_CTRL_LTRIGGER | PSP_CTRL_LEFT))
                read_prev_screen();

            /*
             * Corpo da fonte em cima/baixo, agora que ninguem mais rola.
             *
             * Em `pressed` e nao `repeated`, ao contrario de tudo o mais: cada
             * troca de corpo invalida o atlas de glifos inteiro E reextrai a
             * pagina para repaginar. No auto-repeat isso seria um punhado de
             * reextracoes por segundo, e o console pareceria travado enquanto o
             * usuario segura a tecla.
             */
            if (in.pressed & PSP_CTRL_UP)
                read_set_px(font_px() + 1);
            if (in.pressed & PSP_CTRL_DOWN)
                read_set_px(font_px() - 1);

            if (in.pressed & PSP_CTRL_TRIANGLE)
                g_read_diag = !g_read_diag;

            if (in.pressed & PSP_CTRL_CIRCLE)
                g_mode = MODE_DOC;

        } else if (g_mode == MODE_DOC) {
            if (in.pressed & PSP_CTRL_CIRCLE)
                book_close();
            if ((in.pressed & PSP_CTRL_CROSS) && g_dv.rc == 0) {
                read_start();
                g_mode = MODE_READ;
            }
        } else {
            if (in.repeated & PSP_CTRL_RTRIGGER)
                screen = (screen + 1) % SCREEN_COUNT;
            if (in.repeated & PSP_CTRL_LTRIGGER)
                screen = (screen + SCREEN_COUNT - 1) % SCREEN_COUNT;

            /* font_set_px limpa o atlas inteiro; chamada aqui, fora do frame. */
            if (in.repeated & PSP_CTRL_UP) {
                if (screen == SCREEN_LIBRARY) {
                    g_cursor--;
                    lib_clamp();
                } else {
                    font_set_px(font_px() + 1);
                }
            }
            if (in.repeated & PSP_CTRL_DOWN) {
                if (screen == SCREEN_LIBRARY) {
                    g_cursor++;
                    lib_clamp();
                } else {
                    font_set_px(font_px() - 1);
                }
            }

            if (screen == SCREEN_LIBRARY) {
                /* Salto de uma tela cheia com ESQ/DIR: numa lista de dezenas de
                 * livros, andar item por item e insuficiente. */
                int page = lib_rows();
                if (in.repeated & PSP_CTRL_RIGHT) { g_cursor += page; lib_clamp(); }
                if (in.repeated & PSP_CTRL_LEFT)  { g_cursor -= page; lib_clamp(); }

                if (in.pressed & PSP_CTRL_SQUARE) {
                    library_scan(&g_lib, fs_books_dir());
                    lib_clamp();
                }
                if ((in.pressed & PSP_CTRL_CROSS) && g_lib.count > 0)
                    book_open(&g_lib.items[g_cursor]);
            } else if (screen == SCREEN_SAMPLE) {
                int page = sample_lines_from(g_top_line) - 1;
                if (page < 1)
                    page = 1;
                if (in.repeated & PSP_CTRL_RIGHT) g_top_line += page;
                if (in.repeated & PSP_CTRL_LEFT)  g_top_line -= page;
                sample_clamp();
            }
        }

        gfx_frame_begin(COL_PAPER);

        if (g_mode == MODE_READ) {
            screen_read();
        } else if (g_mode == MODE_DOC) {
            screen_doc();
        } else {
            switch (screen) {
            case SCREEN_LIBRARY: screen_library(); break;
            case SCREEN_SAMPLE:  screen_sample();  break;
            case SCREEN_DIAG:    screen_diag();    break;
            default: break;
            }
        }

        draw_bar(screen);
        draw_footer(screen);

        gfx_frame_end();
        gfx_frame_present();
    }

    /* book_close ja grava o progresso do livro aberto; este save cobre o caso
     * de nao haver livro aberto e o tema ter mudado na biblioteca. */
    book_close();
    progress_save(&g_prog, fs_state_path("progress.txt"));

    pdf_arena_free(&g_rv.arena);
    pdf_arena_free(&g_doc_arena);
    font_shutdown();
    gfx_shutdown();
    sceKernelExitGame();
    return 0;
}
