/* exitwatch.c — mouchard sur exit()/abort() pour Tiger PPC.
 *
 * Injecté par DYLD_INSERT_LIBRARIES=libexitwatch.dylib (le lanceur « UT2004
 * trace.app » le fait). INTERPOSITION dyld (section __DATA,__interpose) et non
 * espace de noms plat : DYLD_FORCE_FLAT_NAMESPACE faisait planter Carbon au
 * lancement (SetSystemUIMode, 22/09 20:46). Quand le processus décide de
 * sortir, on remonte la chaîne des cadres PowerPC (0(r1) = cadre précédent,
 * 8(r1) = LR sauvegardé) et on journalise les adresses de retour avec la table
 * des images chargées (Tiger n'a pas d'ASLR : une adresse se lit ensuite avec
 * `nm` du binaire ou de la bibliothèque, moins son adresse de chargement).
 * Journal : $POMPPC_EXITWATCH (défaut /tmp/exitwatch.txt), en ajout.
 *
 *   /usr/bin/gcc-4.0 -arch ppc -O1 -dynamiclib -o libexitwatch.dylib exitwatch.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach-o/dyld.h>

static void dump(const char *who, int code)
{
    const char *path = getenv("POMPPC_EXITWATCH");
    FILE *f = fopen(path && *path ? path : "/tmp/exitwatch.txt", "a");
    unsigned long *sp;
    unsigned long i, n;
    if (!f)
        return;
    fprintf(f, "== %s(%d) pid %d\n", who, code, (int)getpid());
    __asm__ volatile ("mr %0, r1" : "=r" (sp));
    for (i = 0; i < 40 && sp; i++) {
        unsigned long *prev = (unsigned long *)sp[0];
        if (!prev || prev <= sp || ((unsigned long)prev & 3))
            break;
        fprintf(f, "  #%02lu ra=%08lx sp=%08lx\n", i, prev[2], (unsigned long)prev);
        sp = prev;
    }
    n = _dyld_image_count();
    for (i = 0; i < n; i++) {
        const char *nm = _dyld_get_image_name(i);
        const struct mach_header *h = _dyld_get_image_header(i);
        fprintf(f, "  img %08lx %s\n", (unsigned long)h, nm ? nm : "?");
    }
    fclose(f);
}

static void my_exit(int code)
{
    dump("exit", code);
    exit(code);                         /* l'original, par l'interposition */
}

static void my_abort(void)
{
    dump("abort", 0);
    abort();
}

static void my__exit(int code)
{
    dump("_exit", code);
    _exit(code);
}

/* Table d'interposition dyld : { remplaçant, original }. */
typedef struct { const void *replacement, *replacee; } interpose_t;
__attribute__((used)) static const interpose_t interposers[]
    __attribute__((section("__DATA,__interpose"))) = {
    { (const void *)my_exit,  (const void *)exit  },
    { (const void *)my_abort, (const void *)abort },
    { (const void *)my__exit, (const void *)_exit },
};
