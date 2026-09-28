/* GPL3 - Copyleft VERHILLE Arnaud */
/*
 * pomppc_glslpp.h — conditions du préprocesseur GLSL faites par le plugin
 * (v21), à la place de celui de GLEngine 10.4.6, qui compte mal les #if
 * imbriqués dans un groupe sauté. Fonctions statiques, incluses par
 * pomppc_accel.c et par tests/glsl_pp_test.c (épreuve native).
 */
#ifndef POMPPC_GLSLPP_H
#define POMPPC_GLSLPP_H
#include <stdlib.h>
#include <string.h>

/*
 * Le compilateur de Tiger (3Dlabs, libGLProgrammability) compte mal les
 * conditions imbriquées dans un groupe SAUTÉ dès deux niveaux sous le groupe
 * sauté (sonde /tmp/pp, 27/09 : « #ifdef F / #ifdef R / #ifdef S … #else …
 * #endif / #endif … » refusé pour « #else after a #else », puis, #else
 * réécrits, des lignes sautées prises pour actives). DarkPlaces imbrique tout
 * son texte ainsi (mode ▸ étage ▸ effet ▸ option) : chaque permutation
 * échouait dans GLEngine (+developer 1) et le jeu coupait son chemin GLSL.
 *
 * Le plugin (glsl_source_hook) fait LUI-MÊME les conditions (#if, #ifdef, #ifndef, #elif, #else, #endif) avant
 * que GLEngine ne range le texte : les lignes sautées et les directives de
 * condition deviennent des lignes vides (numéros de ligne inchangés), les
 * #define / #undef actifs sont suivis pour évaluer la suite, tout le reste est
 * recopié. Évaluation à la C : entiers, defined, ! ~ - + * / % << >> < <= > >=
 * == != & ^ | && ||, identificateur inconnu = 0 (GL_EXT_gpu_shader4,
 * GL_ARB_texture_gather… : absentes de Tiger, donc non définies — le texte
 * rangé, que l'hôte recompile, porte le même choix). __VERSION__ = 110. Au
 * moindre doute (macro à paramètres dans une condition, expression illisible,
 * #if non fermé), le texte part tel quel. POMPPC_GL_GLSLPP=0 l'éteint. */
#define PP_DEPTH   64
#define PP_MACROS  512

typedef struct PpMacro {
    const char *name, *val;             /* dans le texte source (non NUL-terminés) */
    unsigned short nl, vl;
    unsigned char  func;                /* macro à paramètres */
} PpMacro;
typedef struct PpState {
    PpMacro m[PP_MACROS];
    int     nm;
    const char *p, *e;                  /* expression en cours */
    int     bad;
    int     rec;
} PpState;

static int pp_id(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
                                  (c >= '0' && c <= '9'); }

static PpMacro *pp_find(PpState *st, const char *n, unsigned long l)
{
    int i;
    for (i = st->nm - 1; i >= 0; i--)
        if (st->m[i].nl == l && !memcmp(st->m[i].name, n, l))
            return &st->m[i];
    return 0;
}

static void pp_ws(PpState *st)
{
    while (st->p < st->e && (*st->p == ' ' || *st->p == '\t' || *st->p == '\r'))
        st->p++;
}

static long pp_expr(PpState *st, int prec);

static long pp_eval_text(PpState *st, const char *b, const char *e)
{
    const char *sp = st->p, *se = st->e;
    long v;
    if (++st->rec > 16) {
        st->bad = 1;
        return 0;
    }
    st->p = b;
    st->e = e;
    v = pp_expr(st, 0);
    pp_ws(st);
    if (st->p != st->e)
        st->bad = 1;
    st->p = sp;
    st->e = se;
    st->rec--;
    return v;
}

static long pp_unary(PpState *st)
{
    pp_ws(st);
    if (st->p >= st->e) {
        st->bad = 1;
        return 0;
    }
    if (*st->p == '(') {
        long v;
        st->p++;
        v = pp_expr(st, 0);
        pp_ws(st);
        if (st->p < st->e && *st->p == ')')
            st->p++;
        else
            st->bad = 1;
        return v;
    }
    if (*st->p == '!') { st->p++; return !pp_unary(st); }
    if (*st->p == '~') { st->p++; return ~pp_unary(st); }
    if (*st->p == '-') { st->p++; return -pp_unary(st); }
    if (*st->p == '+') { st->p++; return pp_unary(st); }
    if (*st->p >= '0' && *st->p <= '9') {
        char buf[32];
        int k = 0;
        while (st->p < st->e && pp_id(*st->p) && k < 31)
            buf[k++] = *st->p++;
        buf[k] = 0;
        return strtol(buf, 0, 0);
    }
    if (pp_id(*st->p)) {
        const char *n = st->p;
        unsigned long l;
        PpMacro *m;
        while (st->p < st->e && pp_id(*st->p))
            st->p++;
        l = (unsigned long)(st->p - n);
        if (l == 7 && !memcmp(n, "defined", 7)) {
            int par;
            const char *n2;
            pp_ws(st);
            par = st->p < st->e && *st->p == '(';
            if (par)
                st->p++;
            pp_ws(st);
            n2 = st->p;
            while (st->p < st->e && pp_id(*st->p))
                st->p++;
            if (st->p == n2) {
                st->bad = 1;
                return 0;
            }
            m = pp_find(st, n2, (unsigned long)(st->p - n2));
            if (par) {
                pp_ws(st);
                if (st->p < st->e && *st->p == ')')
                    st->p++;
                else
                    st->bad = 1;
            }
            return m != 0;
        }
        if (l == 11 && !memcmp(n, "__VERSION__", 11))
            return 110;
        m = pp_find(st, n, l);
        if (!m)
            return 0;                   /* identificateur inconnu : 0 */
        if (m->func) {
            st->bad = 1;
            return 0;
        }
        if (!m->vl)
            return 0;
        return pp_eval_text(st, m->val, m->val + m->vl);
    }
    st->bad = 1;
    return 0;
}

/* opérateur binaire en tête : précédence (1 ||, 2 &&, 3 |, 4 ^, 5 &, 6 == !=,
   7 < <= > >=, 8 << >>, 9 + -, 10 * / %), longueur ; 0 si aucun */
static int pp_binop(PpState *st, int *len, int *op)
{
    const char *p = st->p;
    char a = p < st->e ? p[0] : 0, b = p + 1 < st->e ? p[1] : 0;
    *op = a;
    *len = 1;
    if (a == '|' && b == '|') { *len = 2; *op = 'O'; return 1; }
    if (a == '&' && b == '&') { *len = 2; *op = 'A'; return 2; }
    if (a == '|') return 3;
    if (a == '^') return 4;
    if (a == '&') return 5;
    if (a == '=' && b == '=') { *len = 2; *op = 'E'; return 6; }
    if (a == '!' && b == '=') { *len = 2; *op = 'N'; return 6; }
    if (a == '<' && b == '<') { *len = 2; *op = 'L'; return 8; }
    if (a == '>' && b == '>') { *len = 2; *op = 'R'; return 8; }
    if (a == '<' && b == '=') { *len = 2; *op = 'l'; return 7; }
    if (a == '>' && b == '=') { *len = 2; *op = 'g'; return 7; }
    if (a == '<' || a == '>') return 7;
    if (a == '+' || a == '-') return 9;
    if (a == '*' || a == '/' || a == '%') return 10;
    return 0;
}

static long pp_expr(PpState *st, int prec)
{
    long l = pp_unary(st);
    for (;;) {
        int len, op, p;
        long r;
        pp_ws(st);
        p = pp_binop(st, &len, &op);
        if (!p || p <= prec)
            return l;
        st->p += len;
        r = pp_expr(st, p);
        switch (op) {
        case 'O': l = l || r; break;
        case 'A': l = l && r; break;
        case '|': l |= r; break;
        case '^': l ^= r; break;
        case '&': l &= r; break;
        case 'E': l = l == r; break;
        case 'N': l = l != r; break;
        case 'L': l <<= (r & 31); break;
        case 'R': l >>= (r & 31); break;
        case 'l': l = l <= r; break;
        case 'g': l = l >= r; break;
        case '<': l = l < r; break;
        case '>': l = l > r; break;
        case '+': l += r; break;
        case '-': l -= r; break;
        case '*': l *= r; break;
        case '/': l = r ? l / r : (st->bad = 1, 0); break;
        case '%': l = r ? l % r : (st->bad = 1, 0); break;
        }
    }
}

/* Fait les conditions de `s` (n octets). Rend un texte alloué de même nombre
   de lignes, ou 0 (rien à faire, ou doute : le texte part tel quel). */
static char *glsl_pp_fix(const char *s, unsigned long n, unsigned long *nfix)
{
    PpState *st = calloc(1, sizeof(*st));
    unsigned char act[PP_DEPTH], taken[PP_DEPTH];
    unsigned long i = 0, o = 0;
    int depth = 0, incomment = 0;
    char *out = malloc(n + 1);
    *nfix = 0;
    if (!st || !out)
        goto fail;
    while (i < n) {
        unsigned long e = i, j, d0, d1, a0, a1, k;
        int active = !depth || act[depth - 1], dir = 0, startcomment = incomment;
        while (e < n && s[e] != '\n')
            e++;
        /* commentaires de bloc : une ligne qui commence dans l'un n'est pas une directive */
        for (k = i; k < e; k++) {
            if (!incomment && s[k] == '/' && k + 1 < e && s[k + 1] == '/')
                break;
            if (!incomment && s[k] == '/' && k + 1 < e && s[k + 1] == '*') {
                incomment = 1;
                k++;
            } else if (incomment && s[k] == '*' && k + 1 < e && s[k + 1] == '/') {
                incomment = 0;
                k++;
            }
        }
        j = i;
        while (j < e && (s[j] == ' ' || s[j] == '\t'))
            j++;
        if (!startcomment && j < e && s[j] == '#') {
            d0 = j + 1;
            while (d0 < e && (s[d0] == ' ' || s[d0] == '\t'))
                d0++;
            d1 = d0;
            while (d1 < e && s[d1] >= 'a' && s[d1] <= 'z')
                d1++;
            a0 = d1;
            while (a0 < e && (s[a0] == ' ' || s[a0] == '\t'))
                a0++;
            a1 = e;
            for (k = a0; k + 1 < e; k++)
                if (s[k] == '/' && (s[k + 1] == '/' || s[k + 1] == '*')) {
                    a1 = k;
                    break;
                }
            while (a1 > a0 && (s[a1 - 1] == ' ' || s[a1 - 1] == '\t' || s[a1 - 1] == '\r'))
                a1--;
#define DIR(w) (d1 - d0 == sizeof(w) - 1 && !memcmp(s + d0, w, sizeof(w) - 1))
            if (DIR("if") || DIR("ifdef") || DIR("ifndef")) {
                int v = 0;
                dir = 1;
                if (depth >= PP_DEPTH)
                    goto fail;
                if (active) {
                    if (DIR("if")) {
                        v = pp_eval_text(st, s + a0, s + a1) != 0;
                    } else {
                        unsigned long l = a0;
                        while (l < a1 && pp_id(s[l]))
                            l++;
                        v = pp_find(st, s + a0, l - a0) != 0;
                        if (DIR("ifndef"))
                            v = !v;
                    }
                    if (st->bad)
                        goto fail;
                }
                act[depth] = active && v;
                taken[depth] = !active || v;
                depth++;
            } else if (DIR("elif")) {
                dir = 1;
                if (!depth)
                    goto fail;
                if (!taken[depth - 1] && (depth < 2 || act[depth - 2])) {
                    int v = pp_eval_text(st, s + a0, s + a1) != 0;
                    if (st->bad)
                        goto fail;
                    act[depth - 1] = v;
                    taken[depth - 1] = v;
                } else {
                    act[depth - 1] = 0;
                }
            } else if (DIR("else")) {
                dir = 1;
                if (!depth)
                    goto fail;
                act[depth - 1] = !taken[depth - 1] && (depth < 2 || act[depth - 2]);
                taken[depth - 1] = 1;
            } else if (DIR("endif")) {
                dir = 1;
                if (!depth)
                    goto fail;
                depth--;
            } else if (active && DIR("define")) {
                unsigned long l = a0;
                PpMacro *m;
                while (l < a1 && pp_id(s[l]))
                    l++;
                if (l == a0 || st->nm >= PP_MACROS)
                    goto fail;
                m = &st->m[st->nm++];
                m->name = s + a0;
                m->nl = (unsigned short)(l - a0);
                m->func = l < a1 && s[l] == '(';
                while (l < a1 && (s[l] == ' ' || s[l] == '\t'))
                    l++;
                m->val = s + l;
                m->vl = (unsigned short)(a1 - l);
            } else if (active && DIR("undef")) {
                unsigned long l = a0;
                PpMacro *m;
                while (l < a1 && pp_id(s[l]))
                    l++;
                m = pp_find(st, s + a0, l - a0);
                if (m)
                    m->nl = 0;          /* oubliée */
            }
#undef DIR
        }
        if (dir || !active) {
            if (dir)
                (*nfix)++;
        } else {
            memcpy(out + o, s + i, e - i);
            o += e - i;
        }
        if (e < n)
            out[o++] = '\n';
        i = e + 1;
    }
    if (depth || !*nfix)
        goto fail;
    out[o] = 0;
    free(st);
    return out;
fail:
    free(st);
    free(out);
    return 0;
}

#endif /* POMPPC_GLSLPP_H */
