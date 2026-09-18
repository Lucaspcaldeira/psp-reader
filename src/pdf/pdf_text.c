#include <string.h>
#include <math.h>
#include <stdio.h>
#include "pdf_text.h"
#include "pdf_lex.h"
#include "utf8.h"

/* ------------------------------------------------------------------------- */
/* Matriz                                                                     */

/*
 * Matriz do PDF: [a b c d e f] representa
 *
 *   | a  b  0 |
 *   | c  d  0 |
 *   | e  f  1 |
 *
 * O ponto e um vetor-LINHA multiplicado pela esquerda, entao a composicao
 * "primeiro m, depois n" e m x n - nao n x m. Inverter a ordem produz texto
 * espelhado ou fora da pagina, e o sintoma nao aponta para a causa.
 */
typedef struct { float a, b, c, d, e, f; } Mat;

static const Mat MAT_ID = { 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };

static Mat mat_mul(Mat m, Mat n)
{
    Mat r;
    r.a = m.a * n.a + m.b * n.c;
    r.b = m.a * n.b + m.b * n.d;
    r.c = m.c * n.a + m.d * n.c;
    r.d = m.c * n.b + m.d * n.d;
    r.e = m.e * n.a + m.f * n.c + n.e;
    r.f = m.e * n.b + m.f * n.d + n.f;
    return r;
}

static Mat mat_translate(float tx, float ty)
{
    Mat m = MAT_ID;
    m.e = tx;
    m.f = ty;
    return m;
}

/* ------------------------------------------------------------------------- */
/* Estado                                                                     */

typedef struct {
    char    name[40];
    PdfFont font;
} FontSlot;

typedef struct {
    Mat   ctm;
    int   font;      /* indice em cx->fonts, ou -1 */
    float fs;        /* Tf: corpo */
    float tc;        /* Tc: espacamento de caractere */
    float tw;        /* Tw: espacamento de palavra */
    float th;        /* Tz/100: escala horizontal */
    float tl;        /* TL: entrelinha */
    float ts;        /* Ts: elevacao */
    int   tr;        /* Tr: modo de renderizacao */
} GState;

#define GSTACK_MAX 32
#define OPS_MAX    32
#define ARR_MAX    512
#define FORM_DEPTH 8

typedef struct {
    int      is_str;
    PdfSlice s;
    double   num;
} ArrItem;

typedef struct {
    PdfDoc      *doc;
    PdfArena    *a;          /* arena de saida: runs e texto vivem aqui */
    PdfTextPage *out;

    FontSlot fonts[PDF_TEXT_MAX_FONTS];
    int      nfonts;

    GState gs;
    GState gstack[GSTACK_MAX];
    int    gsp;

    Mat tm, tlm;             /* matriz de texto e da linha */
    int in_text;

    /* Pilha de operandos. Content stream e pos-fixo: operandos primeiro. */
    struct {
        int      kind;       /* PT_INT, PT_REAL, PT_STR, PT_NAME, ou -1 = array */
        double   num;
        PdfSlice s;
    } ops[OPS_MAX];
    int nops;

    ArrItem arr[ARR_MAX];
    int     narr;
    int     in_arr;
} Ctx;

/* ------------------------------------------------------------------------- */
/* Saida                                                                      */

static void emit_run(Ctx *cx, float x, float y, float size,
                     float dx, float dy, const char *bytes, int len)
{
    PdfTextPage *o = cx->out;
    if (len <= 0)
        return;

    if (o->nruns >= PDF_TEXT_MAX_RUNS || o->textlen + len > PDF_TEXT_MAX_BYTES) {
        o->truncated = 1;
        return;
    }

    memcpy(o->text + o->textlen, bytes, (size_t)len);

    PdfTextRun *r = &o->runs[o->nruns++];
    r->x = x;
    r->y = y;
    r->size = size;
    r->width = sqrtf(dx * dx + dy * dy);
    r->dx = dx;
    r->dy = dy;
    r->off = o->textlen;
    r->len = len;

    o->textlen += len;
}

/* ------------------------------------------------------------------------- */
/* Recursos                                                                   */

static int font_lookup(Ctx *cx, PdfObj *resources, const PdfSlice *name)
{
    /* Cache por nome. Um Tf aparece centenas de vezes por pagina e carregar a
     * fonte envolve interpretar o CMap /ToUnicode - sem cache, a extracao de
     * uma pagina reparsearia o mesmo CMap dezenas de vezes. */
    for (int i = 0; i < cx->nfonts; ++i) {
        int nl = (int)strlen(cx->fonts[i].name);
        if (nl == name->len && memcmp(cx->fonts[i].name, name->p, (size_t)nl) == 0)
            return i;
    }
    if (cx->nfonts >= PDF_TEXT_MAX_FONTS)
        return -1;

    PdfObj *fdict = pdf_doc_dget(cx->doc, cx->a, resources, "Font");
    if (!pdf_is(fdict, PDF_DICT))
        return -1;

    /* Busca a chave comparando bytes: o nome vem do content stream como fatia,
     * nao como string terminada. */
    PdfObj *entry = NULL;
    for (int i = 0; i < fdict->u.d.len; ++i) {
        if (fdict->u.d.keys[i].len == name->len &&
            memcmp(fdict->u.d.keys[i].p, name->p, (size_t)name->len) == 0) {
            entry = fdict->u.d.vals[i];
            break;
        }
    }
    if (!entry)
        return -1;

    FontSlot *slot = &cx->fonts[cx->nfonts];
    int n = name->len < (int)sizeof(slot->name) - 1
          ? name->len : (int)sizeof(slot->name) - 1;
    memcpy(slot->name, name->p, (size_t)n);
    slot->name[n] = '\0';

    pdf_font_load(cx->doc, cx->a, entry, &slot->font);
    cx->out->fonts_loaded++;
    return cx->nfonts++;
}

/* ------------------------------------------------------------------------- */
/* Exibicao de texto                                                          */

static void show_bytes(Ctx *cx, const unsigned char *s, int len)
{
    if (len <= 0)
        return;

    PdfFont *f = (cx->gs.font >= 0) ? &cx->fonts[cx->gs.font].font : NULL;

    /* Sem fonte corrente nao ha como interpretar os codigos. Avancar a matriz
     * "no escuro" seria pior que nada: produziria posicoes erradas para o texto
     * seguinte. */
    if (!f)
        return;

    Mat m = mat_mul(cx->tm, cx->gs.ctm);

    /* Matriz de parametros de texto: [fs*th 0 0 fs 0 ts] */
    Mat param = MAT_ID;
    param.a = cx->gs.fs * cx->gs.th;
    param.d = cx->gs.fs;
    param.f = cx->gs.ts;

    Mat trm = mat_mul(param, m);

    float start_x = trm.e;
    float start_y = trm.f;

    /* Corpo efetivo: o fs escalado pela componente vertical de Tm x CTM. */
    float vscale = sqrtf(m.c * m.c + m.d * m.d);
    if (vscale <= 0.0f)
        vscale = 1.0f;
    float size = cx->gs.fs * vscale;

    char buf[1024];
    int  blen = 0;

    int i = 0;
    while (i < len) {
        unsigned int code = 0;
        int consumed = pdf_font_next_code(f, s + i, len - i, &code);
        i += consumed;

        cx->out->codes_total++;

        unsigned int cps[4];
        int ncp = pdf_font_to_unicode(f, code, cps, 4);
        if (ncp == 0) {
            cx->out->codes_unmapped++;
        } else {
            for (int k = 0; k < ncp; ++k) {
                char tmp[4];
                int tn = utf8_encode(cps[k], tmp);
                if (tn > 0 && blen + tn <= (int)sizeof(buf)) {
                    memcpy(buf + blen, tmp, (size_t)tn);
                    blen += tn;
                }
            }
        }

        /*
         * Avanco.  tx = ((w0 - Tj/1000) * Tfs + Tc + Tw) * Th
         *
         * Tw se aplica SOMENTE ao byte 32 em codificacao de um byte. Aplicar em
         * fonte de dois bytes e um erro comum: num Identity-H, o codigo 32 e um
         * glifo qualquer, e somar espacamento de palavra ali desalinha a linha
         * inteira.
         */
        float w0 = (float)pdf_font_width(f, code) / 1000.0f;
        float adv = w0 * cx->gs.fs + cx->gs.tc;
        if (consumed == 1 && code == 32)
            adv += cx->gs.tw;
        adv *= cx->gs.th;

        cx->tm = mat_mul(mat_translate(adv, 0.0f), cx->tm);
    }

    /* Avanco do run: deslocamento entre a origem inicial e a final, em pontos de
     * dispositivo. Medir assim em vez de somar avancos em espaco de texto
     * dispensa reaplicar a escala da CTM a mao, e ja sai como VETOR - que e o
     * que permite orientar pagina girada. */
    Mat m2 = mat_mul(cx->tm, cx->gs.ctm);
    Mat trm2 = mat_mul(param, m2);
    float dx = trm2.e - start_x;
    float dy = trm2.f - start_y;

    emit_run(cx, start_x, start_y, size, dx, dy, buf, blen);
}

/* Deslocamento entre elementos de um array TJ: em milesimos de unidade de
 * texto, e SUBTRAIDO (numero positivo aproxima o texto seguinte). */
static void tj_adjust(Ctx *cx, double amount)
{
    float tx = (float)(-amount / 1000.0) * cx->gs.fs * cx->gs.th;
    cx->tm = mat_mul(mat_translate(tx, 0.0f), cx->tm);
}

/* ------------------------------------------------------------------------- */
/* Interpretador                                                              */

static void run_stream(Ctx *cx, const unsigned char *data, int len,
                       PdfObj *resources, int depth);

/*
 * Pula um bloco de imagem embutida: BI <dict> ID <bytes crus> EI
 *
 * Obrigatorio, nao opcional. Os bytes entre ID e EI sao binarios arbitrarios e
 * o lexer os interpretaria como tokens - um byte '(' ali dentro abriria uma
 * string literal que engoliria o resto da pagina, e o texto depois da imagem
 * desapareceria sem erro nenhum.
 */
static void skip_inline_image(Ctx *cx, PdfStream *st)
{
    cx->out->inline_images++;

    /* Primeiro acha o ID (fim do dicionario da imagem). */
    PdfArena tmp;
    pdf_arena_init(&tmp, 1024);
    int found_id = 0;
    for (int guard = 0; guard < 4096; ++guard) {
        PdfTok t;
        pdf_lex_next(st, &tmp, &t);
        if (t.kind == PT_EOF)
            break;
        if (pdf_tok_is(&t, "ID")) {
            found_id = 1;
            break;
        }
    }
    pdf_arena_free(&tmp);
    if (!found_id)
        return;

    /* Depois do ID vem exatamente um byte de espaco, e depois os dados. */
    pdf_getc(st);

    /*
     * Procura "EI" delimitado por espaco em ambos os lados. A sequencia pode
     * ocorrer dentro dos dados binarios por coincidencia, e exigir os
     * delimitadores e o que reduz o falso positivo ao aceitavel - e o mesmo
     * critterio que os leitores de PDF usam.
     */
    int prev_ws = 1;
    for (;;) {
        int c = pdf_getc(st);
        if (c < 0)
            return;
        if (prev_ws && c == 'E') {
            int c2 = pdf_getc(st);
            if (c2 == 'I') {
                int c3 = pdf_peek(st);
                if (c3 < 0 || pdf_is_ws(c3) || pdf_is_delim(c3))
                    return;
            }
            if (c2 >= 0)
                pdf_ungetc(st);
        }
        prev_ws = pdf_is_ws(c);
    }
}

/* Executa um Form XObject no lugar. */
static void do_xobject(Ctx *cx, PdfObj *resources, const PdfSlice *name,
                       int depth)
{
    if (depth >= FORM_DEPTH)
        return;

    PdfObj *xdict = pdf_doc_dget(cx->doc, cx->a, resources, "XObject");
    if (!pdf_is(xdict, PDF_DICT))
        return;

    PdfObj *xo = NULL;
    for (int i = 0; i < xdict->u.d.len; ++i) {
        if (xdict->u.d.keys[i].len == name->len &&
            memcmp(xdict->u.d.keys[i].p, name->p, (size_t)name->len) == 0) {
            xo = pdf_doc_resolve(cx->doc, cx->a, xdict->u.d.vals[i]);
            break;
        }
    }
    if (!pdf_is(xo, PDF_STREAM))
        return;

    PdfObj *sub = pdf_doc_dget(cx->doc, cx->a, pdf_as_dict(xo), "Subtype");
    if (!pdf_name_is(sub, "Form"))
        return;      /* /Image: nada de texto aqui */

    /*
     * Entrar em Form XObject nao e refinamento: em muitos PDFs a pagina inteira
     * e um unico "Do", e todo o texto esta dentro do form. Ignorar isso daria
     * "pagina sem texto" num livro perfeitamente legivel.
     */
    PdfArena sub_arena;
    pdf_arena_init(&sub_arena, 32768);

    unsigned char *fdata = NULL;
    int flen = 0;
    if (pdf_doc_stream_data(cx->doc, &sub_arena, xo, &fdata, &flen)
            == PDF_FILT_OK && flen > 0) {

        GState saved = cx->gs;
        Mat saved_tm = cx->tm, saved_tlm = cx->tlm;
        int saved_intext = cx->in_text;

        /* /Matrix do form pre-multiplica a CTM. */
        PdfObj *mx = pdf_doc_dget(cx->doc, &sub_arena, pdf_as_dict(xo), "Matrix");
        if (pdf_arr_len(mx) == 6) {
            Mat fm;
            fm.a = (float)pdf_real(pdf_arr_get(mx, 0), 1.0);
            fm.b = (float)pdf_real(pdf_arr_get(mx, 1), 0.0);
            fm.c = (float)pdf_real(pdf_arr_get(mx, 2), 0.0);
            fm.d = (float)pdf_real(pdf_arr_get(mx, 3), 1.0);
            fm.e = (float)pdf_real(pdf_arr_get(mx, 4), 0.0);
            fm.f = (float)pdf_real(pdf_arr_get(mx, 5), 0.0);
            cx->gs.ctm = mat_mul(fm, cx->gs.ctm);
        }

        /* O form pode ter /Resources proprio; sem ele, herda o da pagina. */
        PdfObj *fres = pdf_doc_dget(cx->doc, &sub_arena, pdf_as_dict(xo),
                                    "Resources");
        cx->out->forms_visited++;

        /*
         * O cache de fontes e limpo ao entrar e ao sair: os nomes /F1, /F2 de um
         * form referem-se ao /Resources DELE. Reaproveitar o cache da pagina
         * faria o form desenhar com a fonte errada - e o efeito no texto
         * extraido seria acentuacao trocada, nao um erro visivel.
         */
        int saved_nfonts = cx->nfonts;
        cx->nfonts = 0;
        cx->gs.font = -1;

        run_stream(cx, fdata, flen, fres ? fres : resources, depth + 1);

        cx->nfonts  = saved_nfonts;
        cx->gs      = saved;
        cx->tm      = saved_tm;
        cx->tlm     = saved_tlm;
        cx->in_text = saved_intext;
    }
    pdf_arena_free(&sub_arena);
}

static double op_num(Ctx *cx, int from_top)
{
    int i = cx->nops - 1 - from_top;
    if (i < 0 || i >= OPS_MAX)
        return 0.0;
    return cx->ops[i].num;
}

static void run_stream(Ctx *cx, const unsigned char *data, int len,
                       PdfObj *resources, int depth)
{
    PdfMemCtx mc;
    PdfIo     io;
    PdfStream st;
    PdfArena  scratch;

    pdf_io_mem(&io, &mc, data, len);
    if (pdf_stream_init(&st, &io) != 0)
        return;
    pdf_arena_init(&scratch, 16384);

    cx->nops = 0;
    cx->narr = 0;
    cx->in_arr = 0;

    for (;;) {
        PdfTok t;
        pdf_lex_next(&st, &scratch, &t);
        if (t.kind == PT_EOF)
            break;

        /* --- coleta de operandos ----------------------------------------- */
        if (t.kind == PT_ARR_OPEN) {
            cx->in_arr = 1;
            cx->narr = 0;
            continue;
        }
        if (t.kind == PT_ARR_CLOSE) {
            cx->in_arr = 0;
            if (cx->nops < OPS_MAX) {
                cx->ops[cx->nops].kind = -1;       /* array */
                cx->nops++;
            }
            continue;
        }
        if (cx->in_arr) {
            if (cx->narr < ARR_MAX) {
                if (t.kind == PT_STR) {
                    cx->arr[cx->narr].is_str = 1;
                    cx->arr[cx->narr].s = t.s;
                    cx->narr++;
                } else if (t.kind == PT_INT || t.kind == PT_REAL) {
                    cx->arr[cx->narr].is_str = 0;
                    cx->arr[cx->narr].num =
                        (t.kind == PT_INT) ? (double)t.i : t.r;
                    cx->narr++;
                }
            }
            continue;
        }

        if (t.kind == PT_INT || t.kind == PT_REAL ||
            t.kind == PT_STR || t.kind == PT_NAME) {
            if (cx->nops < OPS_MAX) {
                cx->ops[cx->nops].kind = t.kind;
                cx->ops[cx->nops].num =
                    (t.kind == PT_INT) ? (double)t.i :
                    (t.kind == PT_REAL) ? t.r : 0.0;
                cx->ops[cx->nops].s = t.s;
                cx->nops++;
            } else {
                /* Pilha cheia: descarta o mais antigo em vez de ignorar o novo.
                 * Um operador com operandos demais e sinal de stream torto, e o
                 * operando mais recente e o mais provavel de ser o certo. */
                memmove(&cx->ops[0], &cx->ops[1],
                        sizeof(cx->ops[0]) * (OPS_MAX - 1));
                cx->ops[OPS_MAX - 1].kind = t.kind;
                cx->ops[OPS_MAX - 1].num =
                    (t.kind == PT_INT) ? (double)t.i :
                    (t.kind == PT_REAL) ? t.r : 0.0;
                cx->ops[OPS_MAX - 1].s = t.s;
            }
            continue;
        }

        if (t.kind != PT_KEYWORD) {
            cx->nops = 0;
            continue;
        }

        /* --- operadores --------------------------------------------------- */
        const char *k = t.s.p ? t.s.p : "";
        int kl = t.s.len;

        if (kl == 1 && k[0] == 'q') {
            if (cx->gsp < GSTACK_MAX)
                cx->gstack[cx->gsp++] = cx->gs;
        } else if (kl == 1 && k[0] == 'Q') {
            if (cx->gsp > 0)
                cx->gs = cx->gstack[--cx->gsp];
        } else if (kl == 2 && k[0] == 'c' && k[1] == 'm') {
            if (cx->nops >= 6) {
                Mat m;
                m.a = (float)op_num(cx, 5);
                m.b = (float)op_num(cx, 4);
                m.c = (float)op_num(cx, 3);
                m.d = (float)op_num(cx, 2);
                m.e = (float)op_num(cx, 1);
                m.f = (float)op_num(cx, 0);
                cx->gs.ctm = mat_mul(m, cx->gs.ctm);
            }
        } else if (kl == 2 && k[0] == 'B' && k[1] == 'T') {
            cx->tm = cx->tlm = MAT_ID;
            cx->in_text = 1;
        } else if (kl == 2 && k[0] == 'E' && k[1] == 'T') {
            cx->in_text = 0;
        } else if (kl == 2 && k[0] == 'T' && k[1] == 'f') {
            /* Tf tem dois operandos: /Nome corpo */
            if (cx->nops >= 2) {
                cx->gs.fs = (float)op_num(cx, 0);
                int ni = cx->nops - 2;
                if (ni >= 0 && cx->ops[ni].kind == PT_NAME)
                    cx->gs.font = font_lookup(cx, resources, &cx->ops[ni].s);
            }
        } else if (kl == 2 && k[0] == 'T' && k[1] == 'd') {
            if (cx->nops >= 2) {
                cx->tlm = mat_mul(mat_translate((float)op_num(cx, 1),
                                                (float)op_num(cx, 0)), cx->tlm);
                cx->tm = cx->tlm;
            }
        } else if (kl == 2 && k[0] == 'T' && k[1] == 'D') {
            if (cx->nops >= 2) {
                /* TD tambem define TL como -ty. */
                cx->gs.tl = -(float)op_num(cx, 0);
                cx->tlm = mat_mul(mat_translate((float)op_num(cx, 1),
                                                (float)op_num(cx, 0)), cx->tlm);
                cx->tm = cx->tlm;
            }
        } else if (kl == 2 && k[0] == 'T' && k[1] == 'm') {
            if (cx->nops >= 6) {
                Mat m;
                m.a = (float)op_num(cx, 5);
                m.b = (float)op_num(cx, 4);
                m.c = (float)op_num(cx, 3);
                m.d = (float)op_num(cx, 2);
                m.e = (float)op_num(cx, 1);
                m.f = (float)op_num(cx, 0);
                cx->tm = cx->tlm = m;
            }
        } else if (kl == 2 && k[0] == 'T' && k[1] == '*') {
            cx->tlm = mat_mul(mat_translate(0.0f, -cx->gs.tl), cx->tlm);
            cx->tm = cx->tlm;
        } else if (kl == 2 && k[0] == 'T' && k[1] == 'c') {
            if (cx->nops >= 1) cx->gs.tc = (float)op_num(cx, 0);
        } else if (kl == 2 && k[0] == 'T' && k[1] == 'w') {
            if (cx->nops >= 1) cx->gs.tw = (float)op_num(cx, 0);
        } else if (kl == 2 && k[0] == 'T' && k[1] == 'z') {
            if (cx->nops >= 1) {
                float z = (float)op_num(cx, 0);
                cx->gs.th = (z == 0.0f) ? 1.0f : z / 100.0f;
            }
        } else if (kl == 2 && k[0] == 'T' && k[1] == 'L') {
            if (cx->nops >= 1) cx->gs.tl = (float)op_num(cx, 0);
        } else if (kl == 2 && k[0] == 'T' && k[1] == 's') {
            if (cx->nops >= 1) cx->gs.ts = (float)op_num(cx, 0);
        } else if (kl == 2 && k[0] == 'T' && k[1] == 'r') {
            /*
             * Modo de renderizacao guardado mas NAO usado para filtrar.
             *
             * O modo 3 e invisivel, e a tentacao e pular. Mas e exatamente o
             * modo da camada de OCR sobreposta a uma pagina escaneada - pular
             * tornaria ilegivel justamente o caso em que o texto e o unico
             * conteudo aproveitavel.
             */
            if (cx->nops >= 1) cx->gs.tr = (int)op_num(cx, 0);
        } else if (kl == 2 && k[0] == 'T' && k[1] == 'j') {
            int ni = cx->nops - 1;
            if (ni >= 0 && cx->ops[ni].kind == PT_STR)
                show_bytes(cx, (const unsigned char *)cx->ops[ni].s.p,
                           cx->ops[ni].s.len);
        } else if (kl == 2 && k[0] == 'T' && k[1] == 'J') {
            for (int i = 0; i < cx->narr; ++i) {
                if (cx->arr[i].is_str)
                    show_bytes(cx, (const unsigned char *)cx->arr[i].s.p,
                               cx->arr[i].s.len);
                else
                    tj_adjust(cx, cx->arr[i].num);
            }
            cx->narr = 0;
        } else if (kl == 1 && k[0] == '\'') {
            /* ' = T* seguido de Tj */
            cx->tlm = mat_mul(mat_translate(0.0f, -cx->gs.tl), cx->tlm);
            cx->tm = cx->tlm;
            int ni = cx->nops - 1;
            if (ni >= 0 && cx->ops[ni].kind == PT_STR)
                show_bytes(cx, (const unsigned char *)cx->ops[ni].s.p,
                           cx->ops[ni].s.len);
        } else if (kl == 1 && k[0] == '"') {
            /* aw ac string " : define Tw e Tc, depois faz o mesmo que ' */
            if (cx->nops >= 3) {
                cx->gs.tw = (float)op_num(cx, 2);
                cx->gs.tc = (float)op_num(cx, 1);
            }
            cx->tlm = mat_mul(mat_translate(0.0f, -cx->gs.tl), cx->tlm);
            cx->tm = cx->tlm;
            int ni = cx->nops - 1;
            if (ni >= 0 && cx->ops[ni].kind == PT_STR)
                show_bytes(cx, (const unsigned char *)cx->ops[ni].s.p,
                           cx->ops[ni].s.len);
        } else if (kl == 2 && k[0] == 'D' && k[1] == 'o') {
            int ni = cx->nops - 1;
            if (ni >= 0 && cx->ops[ni].kind == PT_NAME)
                do_xobject(cx, resources, &cx->ops[ni].s, depth);
        } else if (kl == 2 && k[0] == 'B' && k[1] == 'I') {
            skip_inline_image(cx, &st);
        }
        /* Todo o resto - caminhos, cores, sombreamento - e irrelevante para
         * texto e cai fora sem tratamento. */

        cx->nops = 0;

        /* A arena de rascunho acumula um token por vez; reciclar entre
         * operadores mantem o uso constante numa pagina de centenas de KB.
         * Seguro aqui porque nenhum ponteiro para ela sobrevive ao operador. */
        if (scratch.handed > 192u * 1024u) {
            pdf_arena_reset(&scratch);
            cx->narr = 0;
        }
    }

    pdf_arena_free(&scratch);
}

/* ------------------------------------------------------------------------- */

int pdf_text_extract(PdfDoc *doc, PdfArena *a, PdfObj *page, PdfTextPage *out)
{
    memset(out, 0, sizeof(*out));

    out->runs = (PdfTextRun *)pdf_arena_alloc(
        a, sizeof(PdfTextRun) * PDF_TEXT_MAX_RUNS);
    out->text = (char *)pdf_arena_alloc(a, PDF_TEXT_MAX_BYTES);
    if (!out->runs || !out->text)
        return -1;

    /*
     * /MediaBox e herdado da arvore de paginas, entao pdf_doc_dget no no da
     * pagina pode nao encontrar. O padrao de 612x792 (Letter) e o que a
     * especificacao manda assumir.
     */
    out->mb_x0 = 0.0f;  out->mb_y0 = 0.0f;
    out->mb_x1 = 612.0f; out->mb_y1 = 792.0f;

    PdfObj *mb = pdf_doc_dget(doc, a, page, "MediaBox");
    if (pdf_arr_len(mb) == 4) {
        float v[4];
        for (int i = 0; i < 4; ++i)
            v[i] = (float)pdf_real(pdf_doc_resolve(doc, a, pdf_arr_get(mb, i)), 0.0);
        /* Normaliza: a especificacao permite os cantos em qualquer ordem. */
        out->mb_x0 = v[0] < v[2] ? v[0] : v[2];
        out->mb_x1 = v[0] < v[2] ? v[2] : v[0];
        out->mb_y0 = v[1] < v[3] ? v[1] : v[3];
        out->mb_y1 = v[1] < v[3] ? v[3] : v[1];
    }
    out->rotate = (int)pdf_int(pdf_doc_dget(doc, a, page, "Rotate"), 0);

    unsigned char *data = NULL;
    int len = 0;
    if (pdf_doc_page_content(doc, a, page, &data, &len) != PDF_FILT_OK ||
        len <= 0)
        return -1;

    PdfObj *res = pdf_doc_dget(doc, a, page, "Resources");

    Ctx cx;
    memset(&cx, 0, sizeof(cx));
    cx.doc = doc;
    cx.a   = a;
    cx.out = out;
    cx.gs.ctm  = MAT_ID;
    cx.gs.font = -1;
    cx.gs.th   = 1.0f;
    cx.gs.fs   = 0.0f;
    cx.tm = cx.tlm = MAT_ID;

    run_stream(&cx, data, len, res, 0);
    return 0;
}
