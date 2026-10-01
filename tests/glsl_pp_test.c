/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 VERHILLE Arnaud */
/*
 * glsl_pp_test.c — épreuve native des conditions du préprocesseur GLSL que le
 * plugin fait à la place de GLEngine 10.4.6 (guest/gldriver/pomppc_glslpp.h).
 *
 *   cc -std=gnu11 -O1 -I guest/gldriver tests/glsl_pp_test.c -o /tmp/t && /tmp/t
 */
#include <stdio.h>
#include <string.h>
#include "pomppc_glslpp.h"

static int failures;

/* `in` préprocessé doit donner `want` (0 = texte gardé tel quel). */
static void t(const char *what, const char *in, const char *want)
{
    unsigned long nf;
    char *r = glsl_pp_fix(in, strlen(in), &nf);
    int ok = want ? r && !strcmp(r, want) : !r;
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        printf("     obtenu :\n%s\n     attendu :\n%s\n", r ? r : "(tel quel)", want ? want : "(tel quel)");
        failures++;
    }
    free(r);
}

int main(void)
{
    /* le cas de Tiger : trois niveaux sous un groupe sauté */
    t("#else imbriqués dans un groupe sauté",
      "#ifdef F\n#ifdef R\n# ifdef S\nint a;\n# else\nint b;\n# endif\n#endif\n#endif\nvoid main(){}\n",
      "\n\n\n\n\n\n\n\n\nvoid main(){}\n");
    t("groupe actif, #else pris",
      "#define F\n#ifdef F\n#ifdef S\nint a;\n#else\nint b;\n#endif\n#endif\nx\n",
      "#define F\n\n\n\n\nint b;\n\n\nx\n");
    t("#ifndef",
      "#ifndef A\nint a;\n#endif\n", "\nint a;\n\n");
    t("#if expressions : defined, ||, !, comparaison, macro numérique",
      "#define P 2\n#if defined(A) || !defined(B)\nx\n#endif\n#if P > 1 && (P - 1) * 3 == 3\ny\n#endif\n"
      "#if P < 1\nz\n#endif\n",
      "#define P 2\n\nx\n\n\ny\n\n\n\n\n");
    t("#elif et #else",
      "#define M 3\n#if M == 1\na\n#elif M == 3\nb\n#elif M == 3\nc\n#else\nd\n#endif\n",
      "#define M 3\n\n\n\nb\n\n\n\n\n\n");
    t("#undef",
      "#define A\n#undef A\n#ifdef A\na\n#else\nb\n#endif\n", "#define A\n#undef A\n\n\n\nb\n\n");
    t("macro vide : defined vrai, valeur 0",
      "#define E\n#ifdef E\na\n#endif\n#if E\nb\n#endif\n", "#define E\n\na\n\n\n\n\n");
    t("identificateur inconnu (extension absente de Tiger) = 0",
      "#ifdef GL_EXT_gpu_shader4\n#extension GL_EXT_gpu_shader4 : enable\n#endif\n#if GL_ARB_texture_gather\ng\n#endif\n",
      "\n\n\n\n\n\n");
    t("#define dans un groupe sauté : ignoré",
      "#ifdef A\n#define B\n#endif\n#ifdef B\nb\n#endif\n", "\n\n\n\n\n\n");
    t("commentaires : // #if et /* #if */ ne sont pas des directives",
      "//#ifdef A\n/*\n#ifdef A\n*/\n#ifdef A\na\n#endif\n", "//#ifdef A\n/*\n#ifdef A\n*/\n\n\n\n");
    t("directive avec commentaire de fin de ligne",
      "#define V\n#ifdef V // oui\na\n#endif // fin\n", "#define V\n\na\n\n");
    t("#version et #extension gardés",
      "#version 110\n#extension GL_ARB_texture_rectangle : enable\n#ifdef A\na\n#endif\nm\n",
      "#version 110\n#extension GL_ARB_texture_rectangle : enable\n\n\n\nm\n");
    t("macro à paramètres dans une condition : tel quel",
      "#define F(x) x\n#if F(1)\na\n#endif\n", 0);
    t("#if non fermé : tel quel", "#ifdef A\na\n", 0);
    t("#endif orphelin : tel quel", "a\n#endif\n", 0);
    t("aucune condition : tel quel", "void main(){}\n", 0);
    t("__VERSION__", "#if __VERSION__ >= 110\na\n#endif\n", "\na\n\n");
    printf("%s (%d échec(s))\n", failures ? "ÉCHEC" : "OK", failures);
    return failures ? 1 : 0;
}
