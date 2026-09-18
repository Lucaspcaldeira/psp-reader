#include <string.h>
#include <stdlib.h>
#include "pdf_lex.h"

/* Espaco em branco do PDF, incluindo NUL - que a especificacao trata como
 * espaco e aparece de fato como enchimento em alguns geradores. */
int pdf_is_ws(int c)
{
    return c == 0x00 || c == 0x09 || c == 0x0A ||
           c == 0x0C || c == 0x0D || c == 0x20;
}

int pdf_is_delim(int c)
{
    return c == '(' || c == ')' || c == '<' || c == '>' ||
           c == '[' || c == ']' || c == '{' || c == '}' ||
           c == '/' || c == '%';
}

static int is_regular(int c)
{
    return c >= 0 && !pdf_is_ws(c) && !pdf_is_delim(c);
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void pdf_skip_ws(PdfStream *st)
{
    for (;;) {
        int c = pdf_getc(st);
        if (c < 0)
            return;
        if (pdf_is_ws(c))
            continue;
        if (c == '%') {
            /* Comentario ate o fim da linha. */
            for (;;) {
                int d = pdf_getc(st);
                if (d < 0 || d == '\n' || d == '\r')
                    break;
            }
            continue;
        }
        pdf_ungetc(st);
        return;
    }
}

/* ------------------------------------------------------------------------- */
/* Strings literais: (...)                                                    */

/*
 * Varre uma string literal a partir da posicao logo depois do '(' e escreve os
 * bytes DECODIFICADOS em out (se out != NULL e couber em cap).
 *
 * Devolve o numero de bytes decodificados, independente de cap. Chamando duas
 * vezes - primeiro para medir, depois para escrever - evita qualquer buffer
 * crescente, com uma unica implementacao das regras de escape.
 */
static int scan_lit_string(PdfStream *st, unsigned char *out, int cap)
{
    int depth = 1;
    int n = 0;

    for (;;) {
        int c = pdf_getc(st);
        if (c < 0)
            break;                      /* string nao fechada: para no EOF */

        if (c == '\\') {
            int e = pdf_getc(st);
            if (e < 0)
                break;
            switch (e) {
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case '(': c = '(';  break;
            case ')': c = ')';  break;
            case '\\': c = '\\'; break;
            case '\r':
                /* Continuacao de linha: a barra invertida e a quebra
                 * desaparecem. \r\n conta como uma quebra so. */
                if (pdf_peek(st) == '\n')
                    pdf_getc(st);
                continue;
            case '\n':
                continue;
            default:
                if (e >= '0' && e <= '7') {
                    /* Escape octal de 1 a 3 digitos. */
                    int v = e - '0';
                    for (int k = 0; k < 2; ++k) {
                        int d = pdf_peek(st);
                        if (d < '0' || d > '7')
                            break;
                        pdf_getc(st);
                        v = v * 8 + (d - '0');
                    }
                    c = v & 0xFF;
                } else {
                    /* Barra invertida antes de caractere sem significado: a
                     * especificacao manda ignorar a barra e manter o byte. */
                    c = e;
                }
                break;
            }
        } else if (c == '(') {
            /* Parenteses balanceados nao precisam de escape dentro da string. */
            depth++;
        } else if (c == ')') {
            depth--;
            if (depth == 0)
                break;
        }

        if (out && n < cap)
            out[n] = (unsigned char)c;
        n++;
    }
    return n;
}

/* Hex strings: <...>  Digitos nao-hex sao ignorados; contagem impar completa
 * com 0 no ultimo nibble, conforme a especificacao. */
static int scan_hex_string(PdfStream *st, unsigned char *out, int cap)
{
    int n = 0;
    int hi = -1;

    for (;;) {
        int c = pdf_getc(st);
        if (c < 0 || c == '>')
            break;
        int v = hexval(c);
        if (v < 0)
            continue;
        if (hi < 0) {
            hi = v;
        } else {
            if (out && n < cap)
                out[n] = (unsigned char)((hi << 4) | v);
            n++;
            hi = -1;
        }
    }
    if (hi >= 0) {
        if (out && n < cap)
            out[n] = (unsigned char)(hi << 4);
        n++;
    }
    return n;
}

/* ------------------------------------------------------------------------- */

/* A especificacao limita nomes a 127 bytes. Um limite generoso aqui evita
 * alocacao dinamica no caminho mais quente do lexer. */
#define NAME_MAX 256

static int lex_name(PdfStream *st, PdfArena *a, PdfTok *t)
{
    unsigned char buf[NAME_MAX];
    int n = 0;

    for (;;) {
        int c = pdf_peek(st);
        if (!is_regular(c))
            break;
        pdf_getc(st);

        if (c == '#') {
            /* #xx: escape hexadecimal. So consome os digitos se AMBOS forem
             * hex validos - senao o '#' e literal, que e o que aparece em
             * nomes malformados. */
            int h1 = pdf_peek(st);
            int v1 = hexval(h1);
            if (v1 >= 0) {
                pdf_getc(st);
                int h2 = pdf_peek(st);
                int v2 = hexval(h2);
                if (v2 >= 0) {
                    pdf_getc(st);
                    c = (v1 << 4) | v2;
                } else {
                    /* Um digito hex so: devolve e trata '#' como literal. */
                    pdf_ungetc(st);
                }
            }
        }

        if (n < NAME_MAX - 1)
            buf[n++] = (unsigned char)c;
    }

    t->kind  = PT_NAME;
    t->s.p   = pdf_arena_dup(a, buf, (size_t)n);
    t->s.len = t->s.p ? n : 0;
    return 0;
}

static int lex_keyword(PdfStream *st, PdfArena *a, PdfTok *t)
{
    unsigned char buf[NAME_MAX];
    int n = 0;

    for (;;) {
        int c = pdf_peek(st);
        if (!is_regular(c))
            break;
        pdf_getc(st);
        if (n < NAME_MAX - 1)
            buf[n++] = (unsigned char)c;
    }

    t->kind  = PT_KEYWORD;
    t->s.p   = pdf_arena_dup(a, buf, (size_t)n);
    t->s.len = t->s.p ? n : 0;
    return 0;
}

/*
 * Numeros.
 *
 * Deliberadamente permissivo. PDFs reais tem "--5", "6.-2", "34.5-" e ".".
 * Um lexer estrito rejeitaria o arquivo inteiro por causa de um numero
 * decorativo numa entrada de /MediaBox que ninguem le. A regra aqui e: consuma
 * todos os bytes que parecem numero, interprete o que der, e nunca falhe.
 */
static int lex_number(PdfStream *st, PdfTok *t)
{
    char buf[64];
    int n = 0;
    int seen_dot = 0;
    int seen_digit = 0;

    for (;;) {
        int c = pdf_peek(st);
        if (c < 0)
            break;

        if (c >= '0' && c <= '9') {
            seen_digit = 1;
        } else if (c == '.') {
            seen_dot = 1;
        } else if (c == '+' || c == '-') {
            /*
             * Sinal fora da primeira posicao e CONSUMIDO mas nao copiado.
             *
             * "--5" e "34.5-" aparecem em PDF real. Passar a string crua para
             * strtoll faria "--5" virar 0 - um valor plausivel e silenciosamente
             * errado, que e o pior resultado possivel num /MediaBox ou num
             * deslocamento de texto. Normalizando, "--5" da -5 e "34.5-" da
             * 34.5, que e o que os leitores de PDF fazem.
             */
            pdf_getc(st);
            if (n == 0 && c == '-' && n < (int)sizeof(buf) - 1)
                buf[n++] = '-';
            continue;
        } else {
            break;
        }

        pdf_getc(st);
        if (n < (int)sizeof(buf) - 1)
            buf[n++] = (char)c;
    }
    buf[n] = '\0';

    if (!seen_digit) {
        /* So sinais e/ou pontos: trata como zero inteiro em vez de erro. */
        t->kind = PT_INT;
        t->i = 0;
        return 0;
    }

    if (seen_dot) {
        t->kind = PT_REAL;
        t->r = strtod(buf, NULL);
    } else {
        t->kind = PT_INT;
        t->i = strtoll(buf, NULL, 10);
    }
    return 0;
}

int pdf_lex_next(PdfStream *st, PdfArena *a, PdfTok *t)
{
    memset(t, 0, sizeof(*t));
    pdf_skip_ws(st);

    int c = pdf_getc(st);
    if (c < 0) {
        t->kind = PT_EOF;
        return 0;
    }

    switch (c) {
    case '[': t->kind = PT_ARR_OPEN;    return 0;
    case ']': t->kind = PT_ARR_CLOSE;   return 0;
    case '{': t->kind = PT_BRACE_OPEN;  return 0;
    case '}': t->kind = PT_BRACE_CLOSE; return 0;

    case '/':
        return lex_name(st, a, t);

    case '(': {
        long long start = pdf_tell(st);
        int len = scan_lit_string(st, NULL, 0);       /* passo 1: medir */
        long long after = pdf_tell(st);

        unsigned char *buf = (unsigned char *)pdf_arena_alloc(a, (size_t)len + 1);
        if (buf) {
            pdf_seek(st, start);
            scan_lit_string(st, buf, len);            /* passo 2: decodificar */
            buf[len] = 0;
            pdf_seek(st, after);
            t->s.p = (char *)buf;
            t->s.len = len;
        } else {
            t->s.p = NULL;
            t->s.len = 0;
        }
        t->kind = PT_STR;
        return 0;
    }

    case '<': {
        if (pdf_peek(st) == '<') {
            pdf_getc(st);
            t->kind = PT_DICT_OPEN;
            return 0;
        }
        long long start = pdf_tell(st);
        int len = scan_hex_string(st, NULL, 0);
        long long after = pdf_tell(st);

        unsigned char *buf = (unsigned char *)pdf_arena_alloc(a, (size_t)len + 1);
        if (buf) {
            pdf_seek(st, start);
            scan_hex_string(st, buf, len);
            buf[len] = 0;
            pdf_seek(st, after);
            t->s.p = (char *)buf;
            t->s.len = len;
        } else {
            t->s.p = NULL;
            t->s.len = 0;
        }
        t->kind = PT_STR;
        return 0;
    }

    case '>':
        if (pdf_peek(st) == '>') {
            pdf_getc(st);
            t->kind = PT_DICT_CLOSE;
            return 0;
        }
        /* '>' solto: lixo. Ja consumido, entao progredimos. */
        t->kind = PT_JUNK;
        return 0;

    case ')':
        /* ')' sem '(' correspondente. */
        t->kind = PT_JUNK;
        return 0;

    default:
        break;
    }

    if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.') {
        pdf_ungetc(st);
        return lex_number(st, t);
    }

    if (is_regular(c)) {
        pdf_ungetc(st);
        return lex_keyword(st, a, t);
    }

    t->kind = PT_JUNK;
    return 0;
}

int pdf_tok_is(const PdfTok *t, const char *kw)
{
    if (!t || t->kind != PT_KEYWORD || !t->s.p || !kw)
        return 0;
    int n = (int)strlen(kw);
    return t->s.len == n && memcmp(t->s.p, kw, (size_t)n) == 0;
}

/* ------------------------------------------------------------------------- */
/* Busca de subcadeia no arquivo                                              */

/*
 * Janela de varredura com sobreposicao.
 *
 * A sobreposicao de (tamanho da agulha - 1) bytes entre blocos e o detalhe que
 * importa: sem ela, uma ocorrencia que cai exatamente na fronteira de dois
 * blocos passa batida. `endstream` partido entre dois blocos e justamente o
 * caso que aparece em arquivo grande.
 */
#define SCAN_CHUNK 1024

long long pdf_find(PdfStream *st, long long from, long long limit,
                   const char *needle)
{
    int nl = (int)strlen(needle);
    if (nl <= 0 || nl > SCAN_CHUNK)
        return -1;

    long long end = (limit > 0 && from + limit < st->size) ? from + limit : st->size;
    unsigned char buf[SCAN_CHUNK];
    long long at = from;

    while (at < end) {
        int want = SCAN_CHUNK;
        if (at + want > end)
            want = (int)(end - at);
        if (want < nl)
            break;

        pdf_seek(st, at);
        int got = pdf_stream_read(st, buf, want);
        if (got < nl)
            break;

        for (int i = 0; i + nl <= got; ++i) {
            if (buf[i] == (unsigned char)needle[0] &&
                memcmp(buf + i, needle, (size_t)nl) == 0)
                return at + i;
        }
        at += got - (nl - 1);
    }
    return -1;
}

long long pdf_rfind(PdfStream *st, long long from, long long limit,
                    const char *needle)
{
    int nl = (int)strlen(needle);
    if (nl <= 0 || nl > SCAN_CHUNK)
        return -1;

    long long low = (limit > 0 && from - limit > 0) ? from - limit : 0;
    unsigned char buf[SCAN_CHUNK];
    long long hi = from;

    while (hi > low) {
        long long lo = hi - SCAN_CHUNK;
        if (lo < low)
            lo = low;
        int want = (int)(hi - lo);
        if (want < nl)
            break;

        pdf_seek(st, lo);
        int got = pdf_stream_read(st, buf, want);
        if (got < nl)
            break;

        for (int i = got - nl; i >= 0; --i) {
            if (buf[i] == (unsigned char)needle[0] &&
                memcmp(buf + i, needle, (size_t)nl) == 0)
                return lo + i;
        }
        hi = lo + (nl - 1);
    }
    return -1;
}

/* ------------------------------------------------------------------------- */
/* Parser de objetos                                                          */

/* Aninhamento maximo. PDF legitimo raramente passa de 10; o limite existe para
 * um arquivo hostil ou corrompido nao estourar a pilha de 256 KB do PSP com
 * arrays aninhados. */
#define MAX_DEPTH 48

static PdfObj *parse_at(PdfStream *st, PdfArena *a, const PdfTok *first, int depth);

/* Devolve o singleton null como PdfObj* mutavel.
 *
 * O cast descarta const de proposito e e seguro por convencao: nada no parser
 * escreve num objeto depois de construi-lo, e a alternativa - propagar NULL -
 * exigiria checagem em cada um dos pontos de construcao. */
static PdfObj *null_obj(void)
{
    return (PdfObj *)pdf_null();
}

static PdfObj *parse_dict_or_stream(PdfStream *st, PdfArena *a, int depth)
{
    PdfObj *d = pdf_new(a, PDF_DICT);
    if (!d)
        return null_obj();

    for (;;) {
        PdfTok t;
        pdf_lex_next(st, a, &t);

        if (t.kind == PT_DICT_CLOSE || t.kind == PT_EOF)
            break;

        if (t.kind != PT_NAME) {
            /*
             * Chave que nao e nome. Em vez de abortar o dicionario, descarta o
             * token e segue: dicionarios com um token perdido no meio existem,
             * e as outras chaves continuam uteis.
             */
            if (t.kind == PT_JUNK)
                continue;
            /* Consome um objeto para nao reprocessar o mesmo token. */
            parse_at(st, a, &t, depth + 1);
            continue;
        }

        PdfSlice key = t.s;
        PdfObj *val = parse_at(st, a, NULL, depth + 1);
        if (key.p)
            pdf_dict_put(a, d, key, val ? val : null_obj());
    }

    /* Um dicionario seguido de `stream` e o cabecalho de um stream. */
    long long save = pdf_tell(st);
    PdfTok t;
    pdf_lex_next(st, a, &t);

    if (!pdf_tok_is(&t, "stream")) {
        pdf_seek(st, save);
        return d;
    }

    /*
     * Depois de `stream` vem CRLF ou LF - nunca CR sozinho, pela especificacao.
     * Consumir exatamente isso importa: um byte a mais ou a menos desloca todo
     * o conteudo, e num stream Flate o sintoma e "dados corrompidos" sem pista
     * da origem.
     */
    int c = pdf_getc(st);
    if (c == '\r') {
        if (pdf_peek(st) == '\n')
            pdf_getc(st);
    } else if (c != '\n' && c >= 0) {
        /* Gerador fora da especificacao: os dados comecam aqui mesmo. */
        pdf_ungetc(st);
    }

    PdfObj *s = pdf_new(a, PDF_STREAM);
    if (!s)
        return d;

    s->u.stm.dict = d;
    s->u.stm.off  = pdf_tell(st);
    s->u.stm.len  = -1;

    PdfObj *len = pdf_dict_get(d, "Length");
    if (pdf_is(len, PDF_INT) || pdf_is(len, PDF_REAL)) {
        long long n = pdf_int(len, -1);
        /*
         * /Length e tratado como DICA, nao como verdade.
         *
         * Confirmamos procurando `endstream` onde ele deveria estar. Se nao
         * estiver, varremos - porque /Length errado e um dos defeitos mais
         * comuns em PDF gerado por ferramenta ruim, e confiar nele produz
         * stream truncado ou invadindo o objeto seguinte.
         */
        if (n >= 0 && s->u.stm.off + n <= st->size) {
            long long after = s->u.stm.off + n;
            long long found = pdf_find(st, after, 32, "endstream");
            if (found >= 0) {
                s->u.stm.len = n;
                pdf_seek(st, found + 9);
                return s;
            }
        }
    }

    /* Sem /Length utilizavel: a posicao de `endstream` define o comprimento. */
    long long e = pdf_find(st, s->u.stm.off, 0, "endstream");
    if (e >= 0) {
        long long n = e - s->u.stm.off;
        /* `endstream` e precedido por uma quebra de linha que NAO faz parte dos
         * dados. Descontar evita um byte de lixo no fim de cada stream. */
        if (n > 0) {
            pdf_seek(st, e - 1);
            int prev = pdf_getc(st);
            if (prev == '\n') {
                n--;
                if (n > 0) {
                    pdf_seek(st, e - 2);
                    if (pdf_getc(st) == '\r')
                        n--;
                }
            } else if (prev == '\r') {
                n--;
            }
        }
        s->u.stm.len = n < 0 ? 0 : n;
        pdf_seek(st, e + 9);
    } else {
        /* Stream sem `endstream`: assume ate o fim do arquivo. */
        s->u.stm.len = st->size - s->u.stm.off;
        pdf_seek(st, st->size);
    }
    return s;
}

static PdfObj *parse_array(PdfStream *st, PdfArena *a, int depth)
{
    PdfObj *arr = pdf_new(a, PDF_ARR);
    if (!arr)
        return null_obj();

    for (;;) {
        PdfTok t;
        long long save = pdf_tell(st);
        pdf_lex_next(st, a, &t);

        if (t.kind == PT_ARR_CLOSE || t.kind == PT_EOF)
            break;

        /*
         * `]` faltando. Um dicionario ou `endobj` aparecendo dentro de um array
         * significa que o array nunca fechou. Devolver o token e encerrar o
         * array salva o resto do objeto; continuar engoliria o documento todo
         * dentro deste array.
         */
        if (t.kind == PT_DICT_CLOSE || pdf_tok_is(&t, "endobj") ||
            pdf_tok_is(&t, "stream")) {
            pdf_seek(st, save);
            break;
        }

        if (t.kind == PT_JUNK)
            continue;

        PdfObj *v = parse_at(st, a, &t, depth + 1);
        pdf_arr_push(a, arr, v ? v : null_obj());
    }
    return arr;
}

static PdfObj *parse_at(PdfStream *st, PdfArena *a, const PdfTok *first, int depth)
{
    if (depth > MAX_DEPTH)
        return null_obj();

    PdfTok t;
    if (first) {
        t = *first;
    } else {
        pdf_lex_next(st, a, &t);
    }

    switch (t.kind) {
    case PT_EOF:
    case PT_JUNK:
        return null_obj();

    case PT_INT: {
        /*
         * Lookahead de dois tokens para "N G R".
         *
         * Rebobinar e barato porque a janela de leitura e alinhada: o retorno
         * quase sempre cai na mesma janela ja carregada.
         */
        long long save = pdf_tell(st);
        PdfTok t2, t3;
        pdf_lex_next(st, a, &t2);
        if (t2.kind == PT_INT) {
            pdf_lex_next(st, a, &t3);
            if (pdf_tok_is(&t3, "R")) {
                PdfObj *o = pdf_new(a, PDF_REF);
                if (!o)
                    return null_obj();
                o->u.ref.num = (int)t.i;
                o->u.ref.gen = (int)t2.i;
                return o;
            }
        }
        pdf_seek(st, save);

        PdfObj *o = pdf_new(a, PDF_INT);
        if (!o)
            return null_obj();
        o->u.i = t.i;
        return o;
    }

    case PT_REAL: {
        PdfObj *o = pdf_new(a, PDF_REAL);
        if (!o)
            return null_obj();
        o->u.r = t.r;
        return o;
    }

    case PT_STR: {
        PdfObj *o = pdf_new(a, PDF_STR);
        if (!o)
            return null_obj();
        o->u.s = t.s;
        return o;
    }

    case PT_NAME: {
        PdfObj *o = pdf_new(a, PDF_NAME);
        if (!o)
            return null_obj();
        o->u.s = t.s;
        return o;
    }

    case PT_ARR_OPEN:
        return parse_array(st, a, depth);

    case PT_DICT_OPEN:
        return parse_dict_or_stream(st, a, depth);

    case PT_KEYWORD:
        if (pdf_tok_is(&t, "true") || pdf_tok_is(&t, "false")) {
            PdfObj *o = pdf_new(a, PDF_BOOL);
            if (!o)
                return null_obj();
            o->u.b = pdf_tok_is(&t, "true");
            return o;
        }
        /* null, e qualquer palavra que nao seja objeto (endobj, R solto...) */
        return null_obj();

    case PT_ARR_CLOSE:
    case PT_DICT_CLOSE:
    case PT_BRACE_OPEN:
    case PT_BRACE_CLOSE:
    default:
        return null_obj();
    }
}

PdfObj *pdf_parse(PdfStream *st, PdfArena *a)
{
    return parse_at(st, a, NULL, 0);
}

PdfObj *pdf_parse_from(PdfStream *st, PdfArena *a, const PdfTok *first)
{
    return parse_at(st, a, first, 0);
}
