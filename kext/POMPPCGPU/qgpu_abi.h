/*
 * qgpu_abi.h — ABI DE TRANSPORT du GPU paravirtuel « qgpu » (POMPPC).
 *
 * C'est la moitié du contrat que le KEXT connaît, et la seule : identité PCI,
 * registres de BAR1, doorbell, barrières, interruption, statuts de soumission,
 * tranches de clients, interface du user client. Rien ici ne parle d'OpenGL :
 * les opcodes, les clés d'état, les formats et les limites d'objets sont dans
 * qgpu_proto.h, que le kext N'INCLUT PAS. Un changement de qgpu_proto.h ne
 * demande donc jamais de reconstruire le kext ; un changement de CE fichier,
 * si (v19, chantier A1).
 *
 * UNE SEULE SOURCE DE VÉRITÉ, copiée à l'identique à deux endroits :
 *   patches/qgpu/qgpu_abi.h     (hôte : device QEMU + tests ; inclus par qgpu_proto.h)
 *   kext/POMPPCGPU/qgpu_abi.h   (invité : le kext, et lui seul)
 * tests/run-all.sh vérifie que les deux copies sont identiques au bit près, et
 * que le kext ne nomme aucun symbole de qgpu_proto.h.
 *
 * Ce fichier ne contient QUE des macros : il doit se compiler tel quel dans
 * le noyau Tiger (gcc 4.0, C++, -nostdinc), dans QEMU (C11) et dans un
 * programme Tiger userland (gcc 4.0, C89). Aucun include, aucun typedef.
 *
 * ── Ce que le transport promet ──────────────────────────────────────────────
 *
 *   Le device est un COPROCESSEUR DE COMMANDES : l'invité écrit un flux dans
 *   la fenêtre partagée (BAR0, de la RAM côté QEMU), déclare (offset,
 *   longueur) dans les registres (BAR1), frappe le doorbell ; l'hôte exécute,
 *   avance FENCE, pose STATUS, lève DONE si elle est démasquée. Le CONTENU du
 *   flux ne regarde que l'invité userland et le device (qgpu_proto.h).
 *
 *   TOUTE LECTURE DE BAR1 EST SANS EFFET DE BORD. C'est ce qui autorise le kext
 *   à laisser le userland lire n'importe quel registre (QGPU_UC_READ_REG) sans
 *   en connaître le sens : le device publie ce qu'il veut dans BAR1 (version,
 *   capacités, disposition des clients, et tout registre à venir), le plugin
 *   le lit, le kext ne fait que passer le mot.
 *
 *   CLIENTS. Plusieurs processus invités partagent le device. Le kext découpe
 *   BAR0 en QGPU_REG_CLIENTS tranches égales (une par client, mappée seule
 *   dans le processus) ; le DEVICE découpe de son côté ses identifiants
 *   d'objets en autant de plages (table QGPU_REG_LAYOUT, lue par le plugin) et
 *   sait DÉTRUIRE lui-même tout ce que possède une plage (QGPU_REG_CLIENT_RESET)
 *   — le kext ne produit plus aucun flux, il ne connaît aucun opcode.
 *
 * ── Endianness ──────────────────────────────────────────────────────────────
 *
 *   L'invité est un PowerPC big-endian ; les registres sont déclarés
 *   DEVICE_BIG_ENDIAN côté QEMU : l'invité ne fait AUCUN échange d'octets.
 */

#ifndef QGPU_ABI_H
#define QGPU_ABI_H

/* ── Identité PCI ────────────────────────────────────────────────────────── */
#define QGPU_PCI_VENDOR_ID      0x1234      /* vendor QEMU */
#define QGPU_PCI_DEVICE_ID      0x0fb2      /* local, à côté de qfb-pci (0x0fb1) */
/* IOPCIPrimaryMatch attend 0xDDDDVVVV : device en poids fort. */
#define QGPU_IOPCI_PRIMARY_MATCH 0x0fb21234

#define QGPU_MAGIC              0x71677031  /* 'qgp1' */

/* ── BAR0 : fenêtre partagée (RAM) ───────────────────────────────────────── */
#define QGPU_SHMEM_DEFAULT_MB   64
#define QGPU_SHMEM_MIN_MB       16
#define QGPU_SHMEM_MAX_MB       256

/* ── BAR1 : registres (4 Kio, accès 32 bits, big-endian) ─────────────────── */
#define QGPU_CTRL_BAR_SIZE      4096
/* v9 : la fenêtre de registres passait de 0x40 à 0x50 octets ; v19 : 0x100
 * (table de disposition des clients). Au-delà, la lecture rend 0xFFFFFFFF et
 * l'écriture est ignorée, comme avant. */
#define QGPU_CTRL_TOPADDR       0x100

#define QGPU_REG_MAGIC          0x00  /* r  : QGPU_MAGIC ; w : reset complet */
#define QGPU_REG_VERSION        0x04  /* r  : QGPU_PROTO_VERSION (qgpu_proto.h) */
#define QGPU_REG_CAPS           0x08  /* r  : QGPU_CAP_* du backend actif */
#define QGPU_REG_SHMEM_SIZE     0x0C  /* r  : taille de BAR0 en octets */
#define QGPU_REG_SUBMIT_OFF     0x10  /* rw : offset du flux dans BAR0 (mult. de 4) */
#define QGPU_REG_SUBMIT_LEN     0x14  /* rw : longueur du flux en octets (mult. de 4) */
#define QGPU_REG_DOORBELL       0x18  /* w  : QGPU_DOORBELL_* ; r : soumissions en
                                              attente ou en cours (0 = tout est fini) */
#define QGPU_REG_FENCE          0x1C  /* r  : nombre de soumissions TERMINÉES */
#define QGPU_REG_STATUS         0x20  /* r  : QGPU_ST_* de la dernière TERMINÉE */
#define QGPU_REG_STATUS_PC      0x24  /* r  : index (en mots) de la commande fautive */
#define QGPU_REG_IRQ_MASK       0x28  /* rw : QGPU_IRQ_* démasquées */
#define QGPU_REG_IRQ            0x2C  /* r  : en attente ; w : acquitte les bits écrits */
#define QGPU_REG_DEBUG          0x30  /* w  : un octet vers stderr de QEMU (trace invité) */
#define QGPU_REG_BACKEND_NAME   0x34  /* r  : 4 premiers caractères du backend ('soft'/'gl  ') */
/* v9 — file de soumissions. Détail et contrat mémoire : qgpu_proto.h, « v9 ». */
#define QGPU_REG_QUEUE_FREE     0x38  /* r  : places libres dans la file
                                              (QGPU_QUEUE_DEPTH − DOORBELL) */
#define QGPU_REG_FENCE_SUBMITTED 0x3C /* r  : nombre de soumissions ACCEPTÉES ; la
                                              barrière de celle qu'on vient de
                                              soumettre est sa valeur juste après
                                              l'écriture du doorbell */
#define QGPU_REG_SUBMIT_ST      0x40  /* r  : QGPU_ST_OK ou QGPU_ST_QUEUE_FULL —
                                              suite donnée à la DERNIÈRE écriture
                                              du doorbell (acceptation, pas rendu).
                                              v16 : un doorbell SYNCHRONE dont
                                              l'attente hôte expire (2 s, D2) rend
                                              QGPU_ST_BACKEND ici et dans STATUS.
                                              v19 : QGPU_ST_BAD_ARG après un
                                              CLIENT_RESET d'index hors table */
#define QGPU_REG_ERRORS         0x44  /* r  : nombre de soumissions TERMINÉES avec un
                                              statut ≠ QGPU_ST_OK depuis le reset */
#define QGPU_REG_QUEUE_DEPTH    0x48  /* r  : profondeur de la file de CE device */

/* ── v19 : clients (QGPU_CAP_CLIENTS) ────────────────────────────────────────
 *
 *   Jusqu'à la v18, le kext COMPILAIT le nombre de clients et la taille de
 *   chaque plage d'identifiants (QGPU_CLIENT_*_IDS de qgpu_proto.h), et
 *   détruisait les objets d'un client mort en émettant lui-même un flux de
 *   destruction — un doorbell par identifiant, 1 108 par client. Le 23/09/2026,
 *   un kext compilé à 128 textures par client sous un device et un plugin à
 *   1 024 n'a détruit qu'un huitième des textures de Prey : identifiants encore
 *   vivants, BAD_ARG en cascade, créneaux perdus. Depuis la v19 c'est le DEVICE
 *   qui publie la disposition et qui détruit.
 */
#define QGPU_REG_CLIENTS        0x80  /* r  : nombre de tranches de clients (≥ 1) ;
                                              0 = device d'avant la v19 */
#define QGPU_REG_CLIENT_RESET   0x84  /* w  : index de tranche → le device détruit
                                              TOUS les objets des plages de cette
                                              tranche, comme une soumission : mise
                                              en file (FIFO, donc APRÈS tout ce qui
                                              est en vol), FENCE_SUBMITTED avance,
                                              SUBMIT_ST dit OK ou QUEUE_FULL (rien
                                              n'est fait, réessayer), FENCE avance
                                              à la fin, STATUS = OK, DONE levée.
                                              Index ≥ CLIENTS : SUBMIT_ST =
                                              QGPU_ST_BAD_ARG, rien en file.
                                              r  : 0 */
#define QGPU_REG_LAYOUT         0x88  /* r  : QGPU_REG_LAYOUT + 4·k = nombre
                                              d'identifiants PAR CLIENT de la classe
                                              d'objets k (QGPU_CLASS_* de
                                              qgpu_proto.h ; le plugin de la tranche
                                              i possède [i·n, (i+1)·n)). 0 = classe
                                              inconnue de ce device. Par convention
                                              d'ABI, k = 0 est la classe des
                                              contextes et k = 1 celle des surfaces
                                              (sorties historiques de GET_SLOT) */
#define QGPU_REG_LAYOUT_CLASSES 16    /* entrées de la table : 0x88..0xC4 */
#define QGPU_REG_LAYOUT_CLASS(k) (QGPU_REG_LAYOUT + 4 * (k))

/* v9 : valeurs écrites dans QGPU_REG_DOORBELL. */
#define QGPU_DOORBELL_GO        0x00000001  /* v1 : exécuter, synchrone */
#define QGPU_DOORBELL_ASYNC     0x00000002  /* v9 : … mais en file (écrire GO|ASYNC = 3) */

/* v9 : profondeur de la file du device de référence. Le device publie la
 * sienne dans QGPU_REG_QUEUE_DEPTH — un invité prudent lit le registre
 * plutôt que cette macro. */
#define QGPU_QUEUE_DEPTH        16

/* ── Capacités DE TRANSPORT (bits de QGPU_REG_CAPS que le kext lit) ──────────
 * Les autres bits (backend, OpenGL 1.4, programmes…) sont dans qgpu_proto.h et
 * ne regardent que le plugin. Les valeurs sont dans le même espace de bits. */
/* v9 : le device sait mettre les soumissions en file et les exécuter sur un
 * thread de rendu. Sans ce bit, QGPU_DOORBELL_ASYNC est traité comme
 * QGPU_DOORBELL_GO (exécution synchrone) : un invité v9 reste correct sur un
 * device qui n'a pas le thread, il est seulement aussi lent qu'en v8. */
#define QGPU_CAP_ASYNC          0x00000008
/* v19 : le device publie QGPU_REG_CLIENTS et la table QGPU_REG_LAYOUT, et
 * tient QGPU_REG_CLIENT_RESET. Le kext v19 EXIGE ce bit (il n'a plus de quoi
 * détruire les objets lui-même) ; le plugin v19 aussi. */
#define QGPU_CAP_CLIENTS        0x00000200

#define QGPU_IRQ_DONE           0x00000001

/* ── Statuts ─────────────────────────────────────────────────────────────── */
#define QGPU_ST_OK              0
#define QGPU_ST_BAD_SUBMIT      1   /* offset/longueur hors de BAR0 ou non alignés */
#define QGPU_ST_BAD_HEADER      2   /* longueur de commande nulle ou dépassant le flux */
#define QGPU_ST_BAD_OPCODE      3
#define QGPU_ST_BAD_ARG         4   /* argument hors bornes (id, dimensions, format…) */
#define QGPU_ST_OOB             5   /* accès BAR0 hors de la fenêtre partagée */
#define QGPU_ST_NO_CTX          6   /* aucun contexte lié */
#define QGPU_ST_NO_SURF         7   /* surface inexistante ou aucune surface liée */
#define QGPU_ST_LIMIT           8   /* trop d'objets (QGPU_MAX_*) */
#define QGPU_ST_BACKEND         9   /* erreur du backend hôte */
/* v9 : file pleine. Ce statut ne décrit PAS un flux : il dit qu'une écriture
 * de QGPU_DOORBELL_ASYNC a été REFUSÉE. Rien n'a été mis en file, rien ne sera
 * exécuté, ni FENCE ni QGPU_REG_FENCE_SUBMITTED n'avancent, et QGPU_REG_ERRORS
 * ne bouge pas non plus (aucune soumission n'a fini en erreur). Il n'apparaît
 * que dans QGPU_REG_SUBMIT_ST, jamais dans QGPU_REG_STATUS. Le doorbell
 * SYNCHRONE ne le rend jamais : il attend une place. */
#define QGPU_ST_QUEUE_FULL      10

/* ── Interface du kext POMPPCGPU (IOUserClient) ──────────────────────────────
 *
 *   Sélecteurs de IOConnectMethodScalarIScalarO, et types de
 *   IOConnectMapMemory. Partagés entre le kext et les programmes invités.
 *
 *   Le mapping (IOConnectMapMemory) ne couvre QUE la tranche du client, et les
 *   (off, len) de SUBMIT sont relatifs à la tranche. En revanche, les offsets
 *   écrits DANS le flux sont absolus dans BAR0 : le client y ajoute slot_base.
 *   À la fermeture d'un client, le kext demande au device de détruire les
 *   objets de sa tranche (QGPU_REG_CLIENT_RESET). Le device reste
 *   mono-contexte courant : chaque soumission doit commencer par un CTX_BIND
 *   (un autre client a pu en changer entre-temps).
 *
 *   ABI DE DARWIN 8 : le nombre d'arguments scalaires d'un sélecteur est
 *   comparé au bit près (is_io_connect_method_scalarI_scalarO refuse dès que
 *   inputCount ≠ IOExternalMethod::count0). On n'ajoute donc jamais un
 *   argument à un sélecteur existant ; on ajoute un SÉLECTEUR, qu'un kext plus
 *   ancien refuse proprement (index ≥ QGPU_UC_METHOD_COUNT) — c'est la sonde.
 */
#define QGPU_UC_GET_INFO        0   /* in : —              out : version, caps, taille de tranche, fence */
#define QGPU_UC_SUBMIT          1   /* in : off, len       out : fence, status, status_pc (off relatif à la tranche) */
#define QGPU_UC_WAIT_FENCE      2   /* in : fence, ms      out : fence courante */
/* v9 : l'ABI du user client ne change pas, sa SÉMANTIQUE peut changer quand le
 * kext pose le doorbell asynchrone (QGPU_CAP_ASYNC) : QGPU_UC_SUBMIT rend
 * alors la BARRIÈRE DE LA SOUMISSION (QGPU_REG_FENCE_SUBMITTED) au lieu de la
 * fence déjà atteinte, et `status` dit l'acceptation (QGPU_ST_OK ou
 * QGPU_ST_QUEUE_FULL) et non le résultat du rendu — le résultat se lit après
 * QGPU_UC_WAIT_FENCE, qui dort sur l'interruption DONE au lieu de scruter.
 * Détail : docs/protocole-v9-asynchrone.md. */
#define QGPU_UC_RESET           3   /* in : —              out : — (détruit les objets du client) */
#define QGPU_UC_GET_SLOT        4   /* in : —              out : index, slot_base, ctx_base, surf_base
                                       (v19 : ctx_base = index × LAYOUT[0],
                                        surf_base = index × LAYOUT[1], recopiés
                                        des registres ; les autres plages se
                                        calculent avec QGPU_UC_READ_REG) */
#define QGPU_UC_READ_REG        5   /* v19. in : offset    out : valeur du registre
                                       de BAR1 (offset multiple de 4, dans le BAR ;
                                       sinon kIOReturnBadArgument). Sans effet de
                                       bord, par contrat. Un kext d'avant la v19
                                       refuse le sélecteur : c'est la sonde. */
#define QGPU_UC_METHOD_COUNT    6

#define QGPU_UC_MEM_SHMEM       0   /* IOConnectMapMemory : la tranche du client */

/* ── v9 : drapeaux passés DANS `len` à QGPU_UC_SUBMIT ────────────────────────
 *
 *   `len` est un multiple de 4 borné par la tranche (16 Mio sur une fenêtre
 *   de 64) : ses bits hauts sont libres, et un appelant qui ne les connaît pas
 *   les laisse à zéro — c'est-à-dire exactement le comportement v8. Un bit que
 *   le kext ne connaît pas reste dans `len`, qui déborde alors la tranche :
 *   appel refusé (kIOReturnBadArgument). C'est aussi ce qui arrive au drapeau
 *   LAYOUT (0x10000000) du 24/09/2026, retiré en v19 : le kext ne compile
 *   plus de disposition, le plugin lit celle du device.
 *
 *   ASYNC : doorbell asynchrone. Rend alors, au lieu de (fence, statut, pc) :
 *     out0 = QGPU_REG_FENCE_SUBMITTED  barrière de CETTE soumission ;
 *     out1 = QGPU_REG_SUBMIT_ST        acceptation (OK ou QGPU_ST_QUEUE_FULL) ;
 *     out2 = QGPU_REG_ERRORS           compteur d'erreurs — status_pc n'a pas
 *                                      de sens à la soumission, et c'est
 *                                      ERRORS qui fait foi quand plusieurs
 *                                      soumissions sont en vol.
 *   PEEK  : ne soumet RIEN ; rend (ERRORS, STATUS, STATUS_PC) — de quoi
 *           nommer la dernière soumission terminée en erreur.
 *   QUEUE : ne soumet RIEN ; rend (DOORBELL, QUEUE_FREE, QUEUE_DEPTH) — pour
 *           le bilan du plugin (profondeur de file moyenne).
 */
#define POMPPC_SUB_ASYNC        0x80000000UL
#define POMPPC_SUB_PEEK         0x40000000UL
#define POMPPC_SUB_QUEUE        0x20000000UL
#define POMPPC_SUB_FLAGS        (POMPPC_SUB_ASYNC | POMPPC_SUB_PEEK | POMPPC_SUB_QUEUE)

#endif /* QGPU_ABI_H */
