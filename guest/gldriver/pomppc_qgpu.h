/*
 * pomppc_qgpu.h — client userland du kext POMPPCGPU (GPU paravirtuel qgpu).
 */
#ifndef POMPPC_QGPU_H
#define POMPPC_QGPU_H

#include <IOKit/IOKitLib.h>

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
/* Soumet [off, off+len) de la tranche. Renvoie le statut QGPU_ST_* (ou -1
   si l'appel au kext lui-même a échoué) ; *pc = commande fautive. */
long qgpu_submit(QgpuClient *q, unsigned long off, unsigned long len, unsigned long *pc);

#endif
