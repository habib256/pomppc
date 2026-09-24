/*
 * pomppc_qgpu.h — client userland du kext POMPPCGPU (GPU paravirtuel qgpu).
 */
#ifndef POMPPC_QGPU_H
#define POMPPC_QGPU_H

#include <IOKit/IOKitLib.h>

/* Les drapeaux POMPPC_SUB_* passés dans `len` de QGPU_UC_SUBMIT, les
   sélecteurs QGPU_UC_* et les registres sont dans qgpu_abi.h (inclus par
   qgpu_proto.h) : une seule copie, la même que celle du kext. */

typedef struct QgpuClient {
    io_connect_t   conn;
    unsigned char *win;          /* notre tranche de BAR0, mappée */
    unsigned long  size;         /* taille de la tranche */
    unsigned long  base;         /* début de la tranche dans BAR0 (offsets du flux) */
    unsigned long  ctx_base;     /* premiers identifiants de nos plages, calculés
                                    depuis la table QGPU_REG_LAYOUT du device (v19) */
    unsigned long  surf_base;
    unsigned long  tex_base;
    unsigned long  query_base;
    unsigned long  buf_base;
    unsigned long  index;
    unsigned long  nclients;     /* QGPU_REG_CLIENTS */
    unsigned long  version;
    unsigned long  caps;
} QgpuClient;

/* 0 si le device est utilisable ; sinon un message dans *why. Vérifie, dans
   cet ordre : kext v19 (QGPU_UC_READ_REG répond), device v19
   (QGPU_CAP_CLIENTS), et disposition des identifiants du device identique à
   celle de ce qgpu_proto.h (sinon : QEMU d'un autre en-tête). */
int  qgpu_open(QgpuClient *q, const char **why);
/* v19 : lecture d'un registre de BAR1 par le kext (sans effet de bord).
   -1 si le kext ne sait pas (d'avant la v19) ou refuse l'offset. */
int  qgpu_read_reg(QgpuClient *q, unsigned long offset, unsigned long *value);
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
