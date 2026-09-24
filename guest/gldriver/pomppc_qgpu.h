/*
 * pomppc_qgpu.h — client userland du kext POMPPCGPU (GPU paravirtuel qgpu).
 */
#ifndef POMPPC_QGPU_H
#define POMPPC_QGPU_H

#include <IOKit/IOKitLib.h>

/* ── v9 : drapeaux passés DANS `len` à QGPU_UC_SUBMIT ────────────────────────
 *
 *   COPIE IDENTIQUE dans kext/POMPPCGPU/POMPPCGPU.h. Les deux doivent rester
 *   au bit près, comme les deux copies de qgpu_proto.h.
 *
 *   POURQUOI DANS `len` ET PAS UN SCALAIRE DE PLUS. qgpu_proto.h fige
 *   QGPU_UC_METHOD_COUNT et les sélecteurs : on ne peut pas ajouter de
 *   méthode. Et on ne peut pas non plus ajouter un argument à QGPU_UC_SUBMIT :
 *   l'ABI de Darwin 8 compare le NOMBRE d'arguments scalaires au bit près
 *   (is_io_connect_method_scalarI_scalarO refuse l'appel dès que inputCount ≠
 *   IOExternalMethod::count0), donc passer count0 de 2 à 3 ferait rendre
 *   kIOReturnBadArgument à TOUS les appelants existants. `len` est un multiple
 *   de 4 borné par la tranche (16 Mio sur une fenêtre de 64) : ses bits hauts
 *   sont libres, et un appelant qui ne les connaît pas les laisse à zéro —
 *   c'est-à-dire exactement le comportement v8. LES DEUX FORMES D'APPEL
 *   RESTENT DONC LE MÊME APPEL.
 */
#define POMPPC_SUB_ASYNC        0x80000000UL
#define POMPPC_SUB_PEEK         0x40000000UL
#define POMPPC_SUB_QUEUE        0x20000000UL
/* 24/09/2026 : LAYOUT — le kext répond, SANS rien soumettre, avec les constantes
   de tranches qu'il a compilées : fence = QGPU_CLIENT_TEX_IDS | BUF_IDS << 16,
   status = QGPU_CLIENT_SURF_IDS | CTX_IDS << 16, pc = QGPU_MAX_TEX. Un kext qui
   ignore ce bit le laisse dans `len`, qui déborde alors la tranche : appel
   refusé — c'est le signal « kext d'un autre en-tête ». Le 23/09, un kext à
   128 textures par client sous un plugin à 1024 a coûté deux plantages et des
   créneaux de clients perdus. */
#define POMPPC_SUB_LAYOUT       0x10000000UL
#define POMPPC_SUB_FLAGS        (POMPPC_SUB_ASYNC | POMPPC_SUB_PEEK | POMPPC_SUB_QUEUE | \
                                 POMPPC_SUB_LAYOUT)

typedef struct QgpuClient {
    io_connect_t   conn;
    unsigned char *win;          /* notre tranche de BAR0, mappée */
    unsigned long  size;         /* taille de la tranche */
    unsigned long  base;         /* début de la tranche dans BAR0 (offsets du flux) */
    unsigned long  ctx_base;     /* premiers identifiants de notre plage */
    unsigned long  surf_base;
    unsigned long  tex_base;
    unsigned long  index;
    unsigned long  version;
    unsigned long  caps;
} QgpuClient;

/* 0 si le device est utilisable ; sinon un message dans *why. */
int  qgpu_open(QgpuClient *q, const char **why);
void qgpu_close(QgpuClient *q);
/* Après fork() SANS exec, dans l'ENFANT seulement : oublier le port Mach et la
   tranche mappée SANS un seul appel au noyau. Fermer le user client ici
   détruirait les objets du PÈRE (c'est le même user client), et écrire dans la
   tranche — qui est de la mémoire de device, partagée et non copiée — ferait
   exécuter deux fois la trame du père. Voir pomppc_backend_forget (P8). */
void qgpu_forget(QgpuClient *q);
/* Soumet [off, off+len) de la tranche. Renvoie le statut QGPU_ST_* (ou -1
   si l'appel au kext lui-même a échoué) ; *pc = commande fautive. */
long qgpu_submit(QgpuClient *q, unsigned long off, unsigned long len, unsigned long *pc);

/* ── v9 ──────────────────────────────────────────────────────────────────────
 * Le kext sait-il poser le doorbell asynchrone ? Sonde UNE fois (un
 * QGPU_UC_SUBMIT « peek », qu'un kext d'avant la v9 refuse proprement) et
 * vérifie aussi la version et QGPU_CAP_ASYNC du device. */
int  qgpu_async_ok(QgpuClient *q);
/* Met [off, off+len) en file et rend la main tout de suite. Renvoie le statut
   d'ACCEPTATION (QGPU_ST_OK, QGPU_ST_QUEUE_FULL, ou -1 si l'appel a échoué) ;
   *fence = barrière de CETTE soumission, *errors = QGPU_REG_ERRORS.
   Sur QGPU_ST_QUEUE_FULL rien n'a été mis en file : *fence ne vaut rien. */
long qgpu_submit_async(QgpuClient *q, unsigned long off, unsigned long len,
                       unsigned long *fence, unsigned long *errors);
/* Attend QGPU_REG_FENCE ≥ fence. 0 = atteinte, -1 = délai dépassé ou erreur. */
int  qgpu_wait(QgpuClient *q, unsigned long fence, unsigned long ms);
/* Sans rien soumettre : compteur d'erreurs, statut et pc de la dernière
   soumission TERMINÉE. */
int  qgpu_peek(QgpuClient *q, unsigned long *errors, unsigned long *status,
               unsigned long *pc);
/* Sans rien soumettre : soumissions en vol, places libres, profondeur. */
int  qgpu_queue(QgpuClient *q, unsigned long *inflight, unsigned long *freeslots,
                unsigned long *depth);

#endif
