#include <string.h>
#include <stdio.h>

#include "epub.h"
#include "utf8.h"

/* ------------------------------------------------------------------------- */
/* Varredura de XML                                                           */

/*
 * NAO e um parser de XML, e nao deveria ser.
 *
 * O que precisamos do container.xml e do OPF sao tres coisas: o caminho do OPF,
 * a tabela id->href do manifesto, e a ordem dos idrefs na espinha. Isso e busca
 * de atributo em tag conhecida. Um parser de XML de verdade - com namespaces,
 * entidades declaradas, DTD - seria mais codigo que todo o resto deste modulo,
 * para responder as mesmas tres perguntas.
 *
 * O risco assumido e claro: um OPF que use CDATA ou comentario contendo algo
 * parecido com uma tag pode confundir a varredura. Nunca vi um, e a consequencia
 * seria um capitulo fora de ordem, nao um travamento.
 */

static int ci_eq(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb)
            return 0;
    }
    return 1;
}

/* Fim do nome da tag que comeca em `n` (primeiro caractere depois do '<' e da
 * barra de fechamento, se houver). */
static const char *name_end(const char *n, const char *end)
{
    while (n < end && *n != ' ' && *n != '\t' && *n != '\n' && *n != '\r' &&
           *n != '>' && *n != '/')
        n++;
    return n;
}

/*
 * Descarta o prefixo de namespace de um nome de tag.
 *
 * EPUB 2 gerado por ferramenta escreve com frequencia <opf:item>, <opf:spine>,
 * <html:p>. Comparar o nome cru faria a espinha nao ser encontrada nesses
 * arquivos, e o livro abriria vazio - sem nenhuma pista de que a causa foi um
 * prefixo de tres letras. O nome que importa e sempre o que vem depois do ':'.
 */
static const char *strip_ns(const char *n, const char *ne, int *len)
{
    for (const char *c = n; c < ne; ++c) {
        if (*c == ':') {
            *len = (int)(ne - (c + 1));
            return c + 1;
        }
    }
    *len = (int)(ne - n);
    return n;
}

/* Proxima tag `<name` a partir de `p`, ignorando prefixo de namespace. Devolve
 * o inicio do '<', ou NULL. */
static const char *find_tag(const char *p, const char *end, const char *name)
{
    int want = (int)strlen(name);
    while (p < end) {
        const char *lt = (const char *)memchr(p, '<', (size_t)(end - p));
        if (!lt)
            return NULL;

        const char *n = lt + 1;
        if (n < end && *n == '/')
            n++;
        const char *ne = name_end(n, end);

        int nl;
        const char *nm = strip_ns(n, ne, &nl);
        if (nl == want && ci_eq(nm, name, nl) && lt[1] != '/')
            return lt;

        p = lt + 1;
    }
    return NULL;
}

/* Fim da tag que comeca em `lt` (a posicao logo depois do '>'). */
static const char *tag_end(const char *lt, const char *end)
{
    const char *gt = (const char *)memchr(lt, '>', (size_t)(end - lt));
    return gt ? gt + 1 : end;
}

/*
 * Valor de um atributo dentro de UMA tag. Copia para `out` desescapando as
 * cinco entidades que a especificacao de XML garante - href com &amp; existe.
 */
static int attr(const char *lt, const char *end, const char *name,
                char *out, int cap)
{
    out[0] = '\0';
    const char *gt = (const char *)memchr(lt, '>', (size_t)(end - lt));
    if (!gt)
        gt = end;

    int n = (int)strlen(name);
    const char *p = lt + 1;

    while (p + n + 1 < gt) {
        /* O atributo tem de comecar depois de um espaco, senao "href" casaria
         * dentro de "xlink:href" - ou pior, dentro de um valor. */
        int boundary = (p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n' ||
                        p[-1] == '\r');
        if (boundary && ci_eq(p, name, n)) {
            const char *q = p + n;
            while (q < gt && (*q == ' ' || *q == '\t'))
                q++;
            if (q < gt && *q == '=') {
                q++;
                while (q < gt && (*q == ' ' || *q == '\t'))
                    q++;
                char quote = (q < gt && (*q == '"' || *q == '\'')) ? *q : 0;
                if (quote)
                    q++;
                int o = 0;
                while (q < gt && o < cap - 1) {
                    if (quote && *q == quote)
                        break;
                    if (!quote && (*q == ' ' || *q == '>'))
                        break;
                    if (*q == '&') {
                        if (gt - q >= 5 && memcmp(q, "&amp;", 5) == 0) {
                            out[o++] = '&'; q += 5; continue;
                        }
                        if (gt - q >= 4 && memcmp(q, "&lt;", 4) == 0) {
                            out[o++] = '<'; q += 4; continue;
                        }
                        if (gt - q >= 4 && memcmp(q, "&gt;", 4) == 0) {
                            out[o++] = '>'; q += 4; continue;
                        }
                        if (gt - q >= 6 && memcmp(q, "&quot;", 6) == 0) {
                            out[o++] = '"'; q += 6; continue;
                        }
                        if (gt - q >= 6 && memcmp(q, "&apos;", 6) == 0) {
                            out[o++] = '\''; q += 6; continue;
                        }
                    }
                    out[o++] = *q++;
                }
                out[o] = '\0';
                return o;
            }
        }
        p++;
    }
    return 0;
}

/* Texto entre <name ...> e </name>, para titulo e autor do OPF. */
static void tag_text(const char *p, const char *end, const char *name,
                     char *out, int cap)
{
    out[0] = '\0';
    const char *lt = find_tag(p, end, name);
    if (!lt)
        return;
    const char *s = tag_end(lt, end);
    const char *e = s;
    while (e < end && *e != '<')
        e++;
    int n = (int)(e - s);
    if (n >= cap)
        n = cap - 1;
    /* Espaco em volta some: OPF gerado por ferramenta costuma indentar o
     * conteudo da tag numa linha propria. */
    while (n > 0 && (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t')) {
        s++; n--;
    }
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\n' || s[n-1] == '\r' ||
                     s[n-1] == '\t'))
        n--;
    memcpy(out, s, (size_t)n);
    out[n] = '\0';
}

/* ------------------------------------------------------------------------- */
/* Caminhos dentro do ZIP                                                     */

/*
 * Resolve um href relativo contra o diretorio do OPF, normalizando ".." e ".".
 *
 * Necessario porque os hrefs do manifesto sao relativos ao OPF, e o OPF quase
 * nunca esta na raiz - o arranjo tipico e OEBPS/content.opf com hrefs como
 * "Text/cap01.xhtml" ou "../Images/capa.jpg". Sem normalizar, o nome procurado
 * no ZIP nao casaria com nenhum membro e o livro apareceria vazio.
 */
static void resolve(const char *base, const char *href, char *out, int cap)
{
    char tmp[ZIP_NAME_MAX * 2];
    int n = 0;

    if (href[0] != '/') {
        const char *slash = strrchr(base, '/');
        if (slash) {
            n = (int)(slash - base) + 1;
            if (n > (int)sizeof(tmp) - 1)
                n = (int)sizeof(tmp) - 1;
            memcpy(tmp, base, (size_t)n);
        }
    }
    snprintf(tmp + n, sizeof(tmp) - (size_t)n, "%s", href);

    /* Normaliza componente a componente, empilhando os segmentos validos. */
    const char *seg[64];
    int seglen[64];
    int ns = 0;
    const char *p = tmp;

    while (*p) {
        const char *s = p;
        while (*p && *p != '/')
            p++;
        int l = (int)(p - s);
        if (l == 1 && s[0] == '.') {
            /* nada */
        } else if (l == 2 && s[0] == '.' && s[1] == '.') {
            if (ns > 0)
                ns--;
        } else if (l > 0 && ns < 64) {
            seg[ns] = s;
            seglen[ns] = l;
            ns++;
        }
        if (*p == '/')
            p++;
    }

    int o = 0;
    for (int i = 0; i < ns; ++i) {
        if (i > 0 && o < cap - 1)
            out[o++] = '/';
        for (int k = 0; k < seglen[i] && o < cap - 1; ++k)
            out[o++] = seg[i][k];
    }
    out[o] = '\0';
}

/* ------------------------------------------------------------------------- */
/* Abertura                                                                   */

#define OPF_MAX  (512 * 1024)

static void set_err(EpubDoc *d, const char *m)
{
    snprintf(d->err, sizeof(d->err), "%s", m);
}

/* Declaradas adiante: a varredura de planejamento precisa da mesma nocao de
 * "tag que quebra bloco" que o conversor de XHTML usa, senao a contagem de
 * paragrafos do planejamento nao seria a mesma da leitura. */
static int is_block(const char *name, int n);

/*
 * Planeja as unidades de um capitulo.
 *
 * Capitulo pequeno - o caso comum, e o unico em EPUB bem formado - vira uma
 * unidade so, sem descomprimir nada: o tamanho ja veio do diretorio central.
 *
 * Capitulo grande e VARRIDO uma vez, e os cortes saem dessa varredura. Dois
 * limites, e os dois importam:
 *
 *   bytes      para a unidade caber na arena de pagina;
 *   paragrafos para a unidade caber em RF_MAX_PARAS.
 *
 * Cortar so por bytes foi o defeito da primeira versao: uma unidade de texto
 * denso estourava o teto de paragrafos e era truncada em silencio, e o
 * paragrafo do corte desaparecia do livro. Quem decide onde cortar tem de ser
 * quem conhece OS DOIS limites, e tem de ser um so.
 *
 * O corte cai sempre logo depois de um '>', para nao partir uma tag ao meio.
 */
static int plan_chapter(EpubDoc *d, int zi)
{
    long long usz = d->zip.entries[zi].usize;

    if (d->nunits >= EPUB_MAX_UNITS) {
        d->truncated = 1;
        return -1;
    }

    if (usz <= EPUB_CHUNK) {
        EpubUnit *u = &d->units[d->nunits++];
        u->zi = zi;  u->from = 0;  u->to = usz;
        u->first = 1;  u->last = 1;
        return 0;
    }

    ZipReader r;
    if (zip_reader_open(&d->zip, zi, &r) != 0) {
        /* Nao da para varrer: entrega o capitulo inteiro numa unidade so e
         * deixa o truncamento aparecer na leitura, que e melhor que perder o
         * capitulo. */
        EpubUnit *u = &d->units[d->nunits++];
        u->zi = zi;  u->from = 0;  u->to = usz;
        u->first = 1;  u->last = 1;
        return 0;
    }

    /* Teto de paragrafos com folga: o conversor cria paragrafo tambem para
     * blocos que acabam vazios, e uma margem evita depender dessa contagem
     * bater exatamente. */
    const int para_cap = RF_MAX_PARAS - 16;

    unsigned char buf[4096];
    long long pos = 0;          /* byte absoluto no membro descomprimido */
    long long cut_from = 0;     /* inicio da unidade em construcao */
    long long since = 0;        /* bytes desde o corte anterior */
    int paras = 0;
    int first = 1;
    int over = 0;               /* ja passou de algum limite; corta no proximo '>' */

    /* Estado de tag atravessando a fronteira do buffer. */
    char tagname[16];
    int  tagn = -1;             /* -1 = fora de tag; >=0 = coletando o nome */

    for (;;) {
        int got = zip_reader_read(&r, buf, (int)sizeof(buf));
        if (got <= 0)
            break;

        for (int i = 0; i < got; ++i) {
            unsigned char c = buf[i];
            pos++;
            since++;

            if (c == '<') {
                tagn = 0;
                continue;
            }
            if (tagn >= 0) {
                if (c == '>') {
                    if (tagn > 0 && is_block(tagname, tagn))
                        paras++;
                    tagn = -1;
                } else if (c == '/' && tagn == 0) {
                    /* barra de fechamento nao entra no nome */
                } else if (c == ':') {
                    /* Prefixo de namespace fora, como em is_block. */
                    tagn = 0;
                } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                    /* nome terminou; o resto sao atributos */
                    if (tagn == 0)
                        tagn = -1;
                } else if (tagn < (int)sizeof(tagname)) {
                    tagname[tagn++] = (char)c;
                }
            }

            if (!over && (since >= EPUB_CHUNK || paras >= para_cap))
                over = 1;

            /* Corta logo DEPOIS do '>', que e o fim de uma tag completa. */
            if (over && c == '>' && tagn < 0) {
                if (d->nunits >= EPUB_MAX_UNITS) {
                    d->truncated = 1;
                    zip_reader_close(&r);
                    return -1;
                }
                EpubUnit *u = &d->units[d->nunits++];
                u->zi = zi;  u->from = cut_from;  u->to = pos;
                u->first = first;  u->last = 0;
                first = 0;
                cut_from = pos;
                since = 0;
                paras = 0;
                over = 0;
            }
        }
    }
    zip_reader_close(&r);

    /* O resto, que pode ser vazio se o ultimo corte caiu no fim exato. */
    if (cut_from < usz) {
        if (d->nunits >= EPUB_MAX_UNITS) {
            d->truncated = 1;
            return -1;
        }
        EpubUnit *u = &d->units[d->nunits++];
        u->zi = zi;  u->from = cut_from;  u->to = usz;
        u->first = first;  u->last = 1;
    } else if (d->nunits > 0) {
        d->units[d->nunits - 1].last = 1;
    }
    return 0;
}

int epub_open(EpubDoc *d, PdfArena *a, const PdfIo *io)
{
    memset(d, 0, sizeof(*d));

    if (zip_open(&d->zip, a, io) != 0) {
        snprintf(d->err, sizeof(d->err), "%s", d->zip.err);
        return -1;
    }

    /*
     * O indice de unidades e o unico resultado que sobrevive a esta funcao,
     * entao so ele sai da arena do documento. O container.xml, o OPF e a tabela
     * do manifesto sao andaimes: vivem numa arena temporaria que e devolvida ao
     * sistema no fim. Sem essa separacao, um OPF de 300 KB ficaria residente
     * pelo resto da leitura, sem nunca mais ser lido.
     */
    PdfArena tmp;
    pdf_arena_init(&tmp, 64 * 1024);

    d->units = (EpubUnit *)pdf_arena_alloc(a, sizeof(EpubUnit) * EPUB_MAX_UNITS);
    if (!d->units) {
        set_err(d, "sem memoria para o indice de unidades");
        pdf_arena_free(&tmp);
        return -1;
    }

    /* --- container.xml -> caminho do OPF ---------------------------------- */
    int ci = zip_find(&d->zip, "META-INF/container.xml");
    if (ci < 0) {
        set_err(d, "sem META-INF/container.xml: nao e um EPUB");
        pdf_arena_free(&tmp);
        return -1;
    }

    int clen = 0;
    unsigned char *cbuf = zip_read_all(&d->zip, &tmp, ci, 64 * 1024, &clen);
    if (!cbuf) {
        set_err(d, "container.xml ilegivel");
        pdf_arena_free(&tmp);
        return -1;
    }

    char opf_path[ZIP_NAME_MAX];
    opf_path[0] = '\0';
    {
        const char *s = (const char *)cbuf, *e = s + clen;
        const char *rf = find_tag(s, e, "rootfile");
        if (rf)
            attr(rf, e, "full-path", opf_path, sizeof(opf_path));
    }
    if (!opf_path[0]) {
        set_err(d, "container.xml sem rootfile");
        pdf_arena_free(&tmp);
        return -1;
    }

    int oi = zip_find(&d->zip, opf_path);
    if (oi < 0) {
        set_err(d, "OPF apontado pelo container nao existe no ZIP");
        pdf_arena_free(&tmp);
        return -1;
    }

    int olen = 0;
    unsigned char *obuf = zip_read_all(&d->zip, &tmp, oi, OPF_MAX, &olen);
    if (!obuf) {
        set_err(d, "OPF ilegivel ou grande demais");
        pdf_arena_free(&tmp);
        return -1;
    }

    const char *os = (const char *)obuf, *oe = os + olen;

    /* "title", nao "dc:title": find_tag ja descarta o prefixo de namespace,
     * entao um nome so cobre as duas formas que os arquivos usam. */
    tag_text(os, oe, "title", d->title, sizeof(d->title));
    tag_text(os, oe, "creator", d->author, sizeof(d->author));

    /* --- manifesto: id -> indice no ZIP ----------------------------------- */
    typedef struct { char id[64]; int zi; } ManItem;
    ManItem *man = (ManItem *)pdf_arena_alloc(&tmp, sizeof(ManItem) * EPUB_MAX_SPINE);
    if (!man) {
        set_err(d, "sem memoria para o manifesto");
        pdf_arena_free(&tmp);
        return -1;
    }
    int nman = 0;

    const char *p = os;
    while (nman < EPUB_MAX_SPINE) {
        const char *lt = find_tag(p, oe, "item");
        if (!lt)
            break;
        p = tag_end(lt, oe);

        char id[64], href[ZIP_NAME_MAX];
        if (attr(lt, oe, "id", id, sizeof(id)) <= 0)
            continue;
        if (attr(lt, oe, "href", href, sizeof(href)) <= 0)
            continue;

        /* Ancora nao faz parte do nome do arquivo. */
        char *hash = strchr(href, '#');
        if (hash)
            *hash = '\0';

        char full[ZIP_NAME_MAX];
        resolve(opf_path, href, full, sizeof(full));

        int zi = zip_find(&d->zip, full);
        if (zi < 0)
            continue;

        snprintf(man[nman].id, sizeof(man[nman].id), "%s", id);
        man[nman].zi = zi;
        nman++;
    }

    /* --- espinha: a ordem de leitura -------------------------------------- */
    const char *sp = find_tag(os, oe, "spine");
    if (!sp) {
        set_err(d, "OPF sem espinha");
        pdf_arena_free(&tmp);
        return -1;
    }
    p = tag_end(sp, oe);

    while (d->nspine < EPUB_MAX_SPINE) {
        const char *lt = find_tag(p, oe, "itemref");
        if (!lt)
            break;
        p = tag_end(lt, oe);

        char idref[64];
        if (attr(lt, oe, "idref", idref, sizeof(idref)) <= 0)
            continue;

        int zi = -1;
        for (int i = 0; i < nman; ++i)
            if (strcmp(man[i].id, idref) == 0) { zi = man[i].zi; break; }
        if (zi < 0)
            continue;

        d->nspine++;
        if (plan_chapter(d, zi) != 0)
            break;
    }

    pdf_arena_free(&tmp);

    if (d->nunits == 0) {
        set_err(d, "espinha vazia: nenhum capitulo legivel");
        return -1;
    }
    return 0;
}

void epub_close(EpubDoc *d)
{
    zip_close(&d->zip);
    memset(d, 0, sizeof(*d));
}

/* ------------------------------------------------------------------------- */
/* XHTML -> paragrafos                                                        */

/* Entidades nomeadas que aparecem em livro. As numericas sao tratadas a parte. */
static const struct { const char *name; unsigned int cp; } ENT[] = {
    { "amp",    '&'    }, { "lt",     '<'    }, { "gt",     '>'    },
    { "quot",   '"'    }, { "apos",   '\''   }, { "nbsp",   0x00A0 },
    { "mdash",  0x2014 }, { "ndash",  0x2013 }, { "hellip", 0x2026 },
    { "lsquo",  0x2018 }, { "rsquo",  0x2019 }, { "ldquo",  0x201C },
    { "rdquo",  0x201D }, { "laquo",  0x00AB }, { "raquo",  0x00BB },
    { "eacute", 0x00E9 }, { "egrave", 0x00E8 }, { "agrave", 0x00E0 },
    { "ccedil", 0x00E7 }, { "atilde", 0x00E3 }, { "otilde", 0x00F5 },
    { "aacute", 0x00E1 }, { "iacute", 0x00ED }, { "oacute", 0x00F3 },
    { "uacute", 0x00FA }, { "acirc",  0x00E2 }, { "ecirc",  0x00EA },
    { "ocirc",  0x00F4 }, { "uuml",   0x00FC }, { "deg",    0x00B0 },
    { "copy",   0x00A9 }, { "trade",  0x2122 }, { "bull",   0x2022 },
    { "dagger", 0x2020 }, { "sect",   0x00A7 }, { "middot", 0x00B7 }
};

/* Decodifica uma entidade que comeca em `p` (no '&'). Devolve os bytes UTF-8 em
 * `out` e avanca `*p`; 0 se nao for uma entidade reconhecivel. */
static int entity(const char **p, const char *end, char out[4])
{
    const char *q = *p + 1;
    if (q >= end)
        return 0;

    /* Ponto e virgula longe demais nao e entidade: e um '&' solto no texto, que
     * e comum em livro. */
    const char *semi = q;
    while (semi < end && semi - q < 10 && *semi != ';')
        semi++;
    if (semi >= end || *semi != ';')
        return 0;

    unsigned int cp = 0;
    if (*q == '#') {
        q++;
        if (q < semi && (*q == 'x' || *q == 'X')) {
            q++;
            while (q < semi) {
                char c = *q++;
                if (c >= '0' && c <= '9') cp = cp * 16 + (unsigned)(c - '0');
                else if (c >= 'a' && c <= 'f') cp = cp * 16 + (unsigned)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') cp = cp * 16 + (unsigned)(c - 'A' + 10);
                else return 0;
            }
        } else {
            while (q < semi) {
                char c = *q++;
                if (c < '0' || c > '9')
                    return 0;
                cp = cp * 10 + (unsigned)(c - '0');
            }
        }
    } else {
        int n = (int)(semi - q);
        for (size_t i = 0; i < sizeof(ENT) / sizeof(ENT[0]); ++i) {
            if ((int)strlen(ENT[i].name) == n && memcmp(q, ENT[i].name, (size_t)n) == 0) {
                cp = ENT[i].cp;
                break;
            }
        }
        if (cp == 0)
            return 0;
    }

    int n = utf8_encode(cp, out);
    if (n <= 0)
        return 0;
    *p = semi + 1;
    return n;
}

/* Tags que TERMINAM o bloco de texto corrente. Tudo que nao esta aqui e inline
 * (enfase, link, span) e simplesmente some, sem separar palavras. */
static int is_block(const char *name, int n)
{
    static const char *B[] = {
        "p", "div", "br", "li", "h1", "h2", "h3", "h4", "h5", "h6",
        "blockquote", "tr", "td", "th", "hr", "table", "ul", "ol",
        "dd", "dt", "dl", "section", "article", "aside", "figure",
        "figcaption", "pre", "header", "footer", "nav", "body", "title"
    };
    for (size_t i = 0; i < sizeof(B) / sizeof(B[0]); ++i) {
        int bl = (int)strlen(B[i]);
        if (bl == n && ci_eq(name, B[i], n))
            return 1;
    }
    return 0;
}

static int is_heading(const char *name, int n)
{
    return n == 2 && (name[0] == 'h' || name[0] == 'H') &&
           name[1] >= '1' && name[1] <= '6';
}

/* Tags cujo CONTEUDO nao e texto de leitura. */
static int is_skipped(const char *name, int n)
{
    return (n == 6 && ci_eq(name, "script", 6)) ||
           (n == 5 && ci_eq(name, "style", 5)) ||
           (n == 4 && ci_eq(name, "head", 4));
}

int epub_unit(EpubDoc *d, PdfArena *a, int unit, Reflow *out)
{
    memset(out, 0, sizeof(*out));
    out->columns = 1;
    out->body_size = 10.0f;
    out->leading = 12.0f;

    if (unit < 0 || unit >= d->nunits)
        return -1;

    const EpubUnit *u = &d->units[unit];

    /*
     * A faixa ja veio decidida do planejamento. Nao ha fronteira para
     * recalcular aqui - e essa a diferenca que impede unidades vizinhas de
     * discordarem sobre onde uma termina e a outra comeca.
     */
    long long span = u->to - u->from;
    if (span <= 0)
        return 0;
    if (span > EPUB_CHUNK * 2)
        span = EPUB_CHUNK * 2;      /* teto duro: o planejamento nao passa daqui */

    int cap = (int)span;
    char *raw = (char *)pdf_arena_alloc(a, (size_t)cap + 1);
    if (!raw)
        return -1;

    int got = zip_read_range(&d->zip, u->zi, u->from, raw, cap);
    if (got < 0)
        return -1;
    raw[got] = '\0';

    const char *s = raw;
    const char *e = raw + got;

    /* --- varredura -------------------------------------------------------- */
    int ocap = (int)(e - s) + 16;
    out->paras = (RfPara *)pdf_arena_alloc(a, sizeof(RfPara) * RF_MAX_PARAS);
    out->buf = (char *)pdf_arena_alloc(a, (size_t)ocap);
    if (!out->paras || !out->buf)
        return -1;

    int bl = 0;
    RfPara *cur = NULL;
    int pending_break = 1;      /* 1 = o proximo texto abre um paragrafo */
    unsigned pending_flags = 0;
    int space_owed = 0;
    char skip_name[16];
    int  skip_len = 0;          /* >0 = dentro de <script>/<style>/<head> */

    const char *p = s;

    while (p < e) {
        if (*p == '<') {
            const char *gt = (const char *)memchr(p, '>', (size_t)(e - p));
            if (!gt)
                break;

            const char *n = p + 1;
            int closing = 0;
            if (n < gt && *n == '/') { closing = 1; n++; }

            /* Comentario e declaracao passam batido. */
            if (n < gt && (*n == '!' || *n == '?')) {
                p = gt + 1;
                continue;
            }

            const char *ne = n;
            while (ne < gt && *ne != ' ' && *ne != '\t' && *ne != '\n' &&
                   *ne != '\r' && *ne != '/')
                ne++;

            /* <html:p> e <p> sao a mesma coisa para quem so quer o texto. */
            int nl;
            n = strip_ns(n, ne, &nl);

            if (skip_len > 0) {
                if (closing && nl == skip_len && ci_eq(n, skip_name, nl))
                    skip_len = 0;
                p = gt + 1;
                continue;
            }

            if (!closing && nl > 0 && nl < (int)sizeof(skip_name) &&
                is_skipped(n, nl)) {
                memcpy(skip_name, n, (size_t)nl);
                skip_len = nl;
                p = gt + 1;
                continue;
            }

            if (nl > 0 && is_block(n, nl)) {
                pending_break = 1;
                space_owed = 0;
                if (!closing && is_heading(n, nl))
                    pending_flags |= RF_HEADING;
            }

            p = gt + 1;
            continue;
        }

        if (skip_len > 0) {
            p++;
            continue;
        }

        /* --- texto -------------------------------------------------------- */
        char ch[4];
        int chn = 1;

        if (*p == '&') {
            int n = entity(&p, e, ch);
            if (n > 0) {
                chn = n;
            } else {
                ch[0] = *p++;
                chn = 1;
            }
        } else {
            unsigned char c = (unsigned char)*p;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                /*
                 * Espaco EM DIVIDA, e nao escrito na hora.
                 *
                 * XHTML e indentado: entre </p> e <p> ha quebra de linha e
                 * tabulacao que nao sao texto. Escrever espaco assim que
                 * aparece encheria o comeco de todo paragrafo com eles. Ficando
                 * em divida, o espaco so e pago se vier texto de verdade
                 * depois.
                 */
                space_owed = 1;
                p++;
                continue;
            }
            ch[0] = (char)c;
            chn = 1;
            p++;
        }

        /* U+00A0 e espaco tambem, so que inquebravel; para o reflow ele e um
         * espaco comum, e mante-lo faria a quebra de linha ignora-lo. */
        if (chn == 2 && (unsigned char)ch[0] == 0xC2 &&
            (unsigned char)ch[1] == 0xA0) {
            space_owed = 1;
            continue;
        }

        if (pending_break) {
            if (out->nparas >= RF_MAX_PARAS) {
                out->truncated = 1;
                break;
            }
            cur = &out->paras[out->nparas++];
            memset(cur, 0, sizeof(*cur));
            cur->off   = bl;
            cur->size  = (pending_flags & RF_HEADING) ? 13.0f : 10.0f;
            cur->flags = pending_flags;
            cur->nlines = 1;
            pending_break = 0;
            pending_flags = 0;
            space_owed = 0;
        }

        if (space_owed && bl > cur->off && bl < ocap)
            out->buf[bl++] = ' ';
        space_owed = 0;

        if (bl + chn <= ocap) {
            memcpy(out->buf + bl, ch, (size_t)chn);
            bl += chn;
        }
        cur->len = bl - cur->off;
    }

    /* Paragrafos que ficaram so com espaco nao entram: uma linha em branco no
     * meio do texto viraria uma tela quase vazia. */
    int w = 0;
    for (int i = 0; i < out->nparas; ++i) {
        if (out->paras[i].len > 0)
            out->paras[w++] = out->paras[i];
    }
    out->nparas = w;
    out->buflen = bl;
    out->lines_in = out->nparas;

    /* Emenda entre pedacos do MESMO capitulo. Entre capitulos nao ha emenda:
     * capitulo novo comeca paragrafo novo por definicao. */
    if (out->nparas > 0) {
        if (!u->first)
            out->paras[0].flags |= RF_CONT;
        if (!u->last)
            out->paras[out->nparas - 1].flags |= RF_OPEN;
    }

    return 0;
}
