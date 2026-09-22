/* exitwatch.c — mouchard sur exit()/_exit()/abort() pour Tiger PPC.
 *
 * Injecté par DYLD_INSERT_LIBRARIES=libexitwatch.dylib DYLD_FORCE_FLAT_NAMESPACE=1
 * (le lanceur « UT2004 trace.app » le fait) : quand le processus décide de
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
#include <dlfcn.h>
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

void exit(int code)
{
    void (*real)(int) = (void (*)(int))dlsym(RTLD_NEXT, "exit");
    dump("exit", code);
    if (real)
        real(code);
    _exit(code);
}

void abort(void)
{
    void (*real)(void) = (void (*)(void))dlsym(RTLD_NEXT, "abort");
    dump("abort", 0);
    if (real)
        real();
    _exit(134);
}
