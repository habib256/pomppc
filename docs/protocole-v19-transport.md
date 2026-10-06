# Protocole qgpu v19 — transport séparé : le kext ne connaît plus le protocole (24/09/2026)

Chantier d'architecture **A1** (`TODO.md` §4) : *séparer l'ABI de transport de la sémantique GL*.

## Pourquoi

Jusqu'à la v18, `qgpu_proto.h` était **un seul fichier**, copié à l'identique dans le kext, et le
kext en compilait une partie sémantique : le nombre de clients, la taille de chaque plage
d'identifiants (`QGPU_CLIENT_TEX_IDS`…), et les quatre opcodes de destruction, avec lesquels il
fabriquait lui-même, dans une page de service de BAR0, le flux qui détruit les objets d'un client
mort — **un doorbell par identifiant, 1 108 par client**, et jamais les requêtes d'occlusion.

Conséquence : **tout changement de `qgpu_proto.h` exigeait de reconstruire le kext** (dans
l'invité, gcc 4.0, puis redémarrage), et rien ne le vérifiait. Le 23/09/2026, `QGPU_MAX_TEX` passe
de 512 à 4096 ; QEMU et le plugin sont reconstruits, pas le kext : à la mort de Prey, le kext ne
détruit que 128 textures sur 1 024, DOOM 3 recrée des identifiants encore vivants → `BAD_ARG` en
cascade → repli → `Bus error`, puis « créneaux de clients perdus ». Une heure et deux plantages.
Le lendemain, un pansement (`POMPPC_SUB_LAYOUT` : le plugin demande au kext ses constantes et
refuse en cas d'écart) rendait l'erreur visible, sans supprimer la cause.

## Ce qui change

**Deux en-têtes.**

| fichier | contenu | qui l'inclut |
|---|---|---|
| `patches/qgpu/qgpu_abi.h` | le **transport** : identité PCI, registres de BAR1, doorbell, barrières, interruption, statuts de soumission, tranches de clients, sélecteurs du user client, drapeaux `POMPPC_SUB_*` | le **kext** (copie identique `kext/POMPPCGPU/qgpu_abi.h`), le device, le plugin (via `qgpu_proto.h`) |
| `patches/qgpu/qgpu_proto.h` | la **sémantique** : opcodes, clés d'état, formats, limites, classes d'objets ; `#include "qgpu_abi.h"` | le device et ses tests, le plugin, `qgpu_test` — **pas le kext** |

Le harnais (`tests/run-all.sh` §5 bis) vérifie : les deux copies de `qgpu_abi.h` identiques ;
**aucune copie de `qgpu_proto.h` sous `kext/`** ; aucun symbole `QGPU_OP_*`, `QGPU_MAX_*`,
`QGPU_CLIENT_*`, `QGPU_CLASS_*`, `QGPU_PROTO_*`, `QGPU_LEN_*`, `QGPU_CMD_HDR` dans les sources du
kext (hors commentaires) ; aucun symbole sémantique dans `qgpu_abi.h`.

**Le device possède la disposition des clients et détruit lui-même.** Trois registres et une
capacité, `QGPU_CAP_CLIENTS` = `0x200` (annoncée par le cœur, quel que soit le backend) :

| registre | offset | sens |
|---|---|---|
| `QGPU_REG_CLIENTS` | `0x80` | r : nombre de tranches (4). 0 = device d'avant la v19 |
| `QGPU_REG_CLIENT_RESET` | `0x84` | w : index de tranche → le device détruit tous les objets de ses plages, **comme une soumission** : mise en file (FIFO, donc derrière tout ce qui est en vol, y compris les dernières soumissions du client mort), `FENCE_SUBMITTED` avance, `SUBMIT_ST` = OK ou `QUEUE_FULL` (rien n'est fait, réessayer), `FENCE` avance à la fin, `STATUS` = OK, `DONE` levée. Index hors table : `SUBMIT_ST` = `BAD_ARG`, rien en file |
| `QGPU_REG_LAYOUT` + 4·k | `0x88`…`0xC4` | r : identifiants **par client** de la classe k (`QGPU_CLASS_CTX` 0, `SURF` 1, `TEX` 2, `QUERY` 3, `BUF` 4 ; 16 entrées ; 0 = classe inconnue). Le client de la tranche i possède [i·n, (i+1)·n). Par convention d'ABI, k = 0 et 1 sont les contextes et les surfaces |

`QGPU_CTRL_TOPADDR` passe de `0x50` à `0x100`. Le cœur : `qgpu_core_client_ids(k)` et
`qgpu_core_client_reset(c, slot)` (contextes avec leur requête ouverte et leurs programmes,
surfaces déliées de **tout** contexte, textures, requêtes, tampons ; un identifiant inutilisé est
sauté ; `BAD_ARG` si `slot >= QGPU_MAX_CLIENTS`). Le device (`qgpu-pci.c`) : un `QgpuJob` de type
`CLIENT_RESET` dans la même file et sur le même thread de rendu que les soumissions.

**Le kext** (`kext/POMPPCGPU/`) :

- n'inclut que `qgpu_abi.h` ; exige `QGPU_CAP_CLIENTS` (sinon refuse de démarrer : « needs a
  device that owns the client layout ») ; ne regarde plus `QGPU_REG_VERSION` que pour l'afficher
  (`ioreg` : `QGPUVersion`), plus de `QGPU_PROTO_MIN` ;
- lit `QGPU_REG_CLIENTS` (borné par sa capacité compilée `POMPPC_KEXT_MAX_CLIENTS` = 8, qui n'est
  pas une constante du protocole) et découpe BAR0 en autant de tranches, **sans page de service**
  (il n'écrit plus un mot dans BAR0) ; `ioreg` : `QGPUClients` ;
- à la fermeture d'un client : écrit l'index dans `QGPU_REG_CLIENT_RESET`, réessaie sur
  `QUEUE_FULL` après avoir attendu `FENCE_SUBMITTED` (8 fois au plus), puis dort sur la barrière
  (`sleepForFence`, 2 s) — ou scrute `FENCE` sans chien de garde ;
- **nouveau sélecteur `QGPU_UC_READ_REG`** (5) : `in` offset, `out` valeur, borné sur le BAR (pas
  sur `TOPADDR` : un registre publié demain se lit sans kext nouveau). Le contrat de
  `qgpu_abi.h` : *toute lecture de BAR1 est sans effet de bord*. `QGPU_UC_METHOD_COUNT` = 6 ;
- `QGPU_UC_GET_SLOT` : `ctx_base` et `surf_base` sont recopiés de `LAYOUT[0]` et `LAYOUT[1]`
  (plus compilés) ;
- le drapeau `POMPPC_SUB_LAYOUT` (`0x10000000`) est retiré : un bit inconnu déborde la tranche
  et l'appel est refusé, comme tout bit inconnu.

**Le plugin** (`guest/gldriver/pomppc_qgpu.c`), à l'ouverture, dans l'ordre des reproches :

1. `READ_REG(QGPU_REG_CLIENTS)` répond ? Sinon **kext d'avant la v19** : « reconstruire et
   réinstaller le kext, redémarrer » ;
2. `QGPU_CAP_CLIENTS` et `CLIENTS ≠ 0` ? Sinon **QEMU d'avant la v19** ;
3. la table `LAYOUT` vaut ses `QGPU_CLIENT_*_IDS` ? Sinon **QEMU d'un autre `qgpu_proto.h` que
   le plugin** — le seul autre à compiler ce fichier ;
4. `GET_SLOT` cohérent avec la table.

Les bases `ctx/surf/tex/query/buf_base` viennent de la table (`QgpuClient`, `nclients` en plus) ;
`pomppc_accel.c` les prend là. Les Makefiles du plugin et de `qgpu_test` copient
`qgpu_proto.h` + `qgpu_abi.h` depuis `patches/qgpu/` ; `install.sh`, `make_kext_iso.sh` et
`build_qemu_qfb.sh` transportent les deux.

## La règle qui tombe

Avant : *toute modification de `qgpu_proto.h` ⇒ QEMU + kext (`install.sh`, redémarrage) +
plugin*. Après : **`qgpu_proto.h` ⇒ QEMU + plugin** (`.run/cmr/cycle.sh NORUN=1`, deux minutes,
sans redémarrer) ; **`qgpu_abi.h` ⇒ les trois**, et c'est rare — la disposition des clients
elle-même se change dans `qgpu_proto.h` (le kext la lit).

Une clé d'état, un opcode, une limite, une classe d'objets de plus : le kext installé reste bon.
Si un jour un kext et un device ne s'accordent pas, c'est le kext qui refuse de démarrer
(`QGPU_CAP_CLIENTS` absent) ou le plugin qui refuse le kext (`READ_REG` inconnu) : jamais plus
d'objets laissés vivants en silence.

## Épreuves

| épreuve | où | résultat 24/09/2026 |
|---|---|---|
| carte des registres v19, `CAP_CLIENTS`, table, destruction d'une tranche (toutes classes, tranche voisine intacte, surface déliée d'un contexte étranger, contexte courant rendu, `BAD_ARG` hors table, recréation des identifiants) | `tests/qgpu_core_test.c` `run_v19`, soft et GL | 913 OK |
| contrat structurel (copies identiques, kext sans sémantique, ABI sans sémantique) | `tests/run-all.sh` §5 bis | OK |
| `CLIENT_RESET` de bout en bout depuis Open Firmware (rejouer la scène : `LIMIT` avant, OK après ; `BAD_ARG` hors table ; barrière) | `tests/qgpu_smoke.py` (`--slow`) | OK (55 vérifications) |
| kext v19 chargé contre le device v19, `READ_REG` (MAGIC, VERSION, CLIENTS, table, classe inconnue, non aligné refusé, hors BAR refusé, au-delà de TOPADDR = `0xFFFFFFFF`), `GET_SLOT` = table, bit de `len` inconnu refusé, `QGPU_UC_RESET` → recréation par le device | `guest/qgpu-test` dans Tiger | 44/44, kext « protocol v19, 4 clients x 16384 KiB » |
| plugin v19 : scènes `gltest` inchangées | invité | `tri texup texcache texdelmid cube arbvp arbfp varray varrayvbo caps blendc logicop polymode stipple occl sepspec spin game` OK ; `tex3d tex13 tex14 gl15 texlod` cassées **avant** A1 (`TODO.md` §5) |
| **la preuve d'A1** : ajouter une clé fictive à `qgpu_proto.h`, ne reconstruire que QEMU et le plugin, `gltest` vert avec le kext installé | par construction (le kext n'inclut pas le fichier) + harnais §5 bis | — |

## Ce qui reste hors périmètre

- L'isolation entre clients (un processus peut toujours toucher les identifiants d'un autre) :
  choix assumé, `qgpu-pci.c` en tête.
- ~~`QGPU_REG_ERRORS` global au device~~ : un compteur par tranche depuis le 07/10/2026,
  ci-dessous.
- Le nombre de tranches reste 4 et fixé par le device ; le WindowServer comme cinquième client
  est le chantier A6.

## Erreurs par client (07/10/2026)

**Le défaut.** `QGPU_REG_ERRORS` est global au device. En asynchrone le plugin ne voit que lui
(le kext le rend à chaque doorbell, à la place de `status_pc`) : quand il bouge, il repassait en
synchrone pendant 120 images et invalidait ses miroirs (`check_errors`), que l'erreur soit la
sienne ou non. Observé le 24/09 (8 877 erreurs du balayage de fermeture du kext, un repli par
application GL fermée à côté) ; le balayage est fait par le device depuis la v19, sans erreur, mais
la sonde attendue d'un autre processus (`PROG`, `NO_MEM`) ou son flux fautif suffisaient encore.
Reproduit le 07/10 (ci-dessous) : un voisin fautif fait finir `gltest game` en synchrone.

**La voie.** Rien dans le kext. Le device sait déjà de quelle tranche vient un flux : son offset
absolu dans BAR0, découpé comme le kext le découpe (`(fenêtre / CLIENTS) & ~0xFFF`, `fSlotSize`) —
et un `CLIENT_RESET` porte l'index de sa tranche. Il tient donc un compteur par tranche, et le
plugin le lit par `QGPU_UC_READ_REG`, qui existe depuis la v19. Tout est dans `qgpu_proto.h` :

| symbole | valeur | sens |
|---|---|---|
| `QGPU_CAP_CLIENT_ERRORS` | `0x00080000` | le device tient les compteurs ci-dessous (bit posé par `qgpu-pci.c`) |
| `QGPU_REG_CLIENT_ERRORS(i)` | `0xC8 + 4·i` | soumissions terminées en erreur dont le flux est dans la tranche *i* ; publié avant `FENCE`, comme le global |
| `QGPU_CLIENT_SLOT_BYTES(shm, n)` | `(shm / n) & ~0xFFF` | le découpage supposé, partagé par le device et le plugin |

Même règle que le global (ni `QUEUE_FULL`, ni doorbell synchrone abandonné par l'échéance D2) ; la
somme des compteurs vaut le global, sauf un flux hors de toute tranche. Ni `QGPU_PROTO_VERSION` ni
`qgpu_abi.h` ne changent : QEMU et plugin, sans réinstaller le kext ni redémarrer.

**Le plugin.** Le global reste le **déclencheur** (gratuit, rendu à chaque doorbell) ; quand il a
bougé, `errors_foreign` relit le compteur de sa tranche — un appel au kext, seulement alors. Immobile :
mouvement étranger, on se recale, l'asynchrone et les miroirs restent. Le compteur est lu *après* le
global : une faute à nous plus récente est prise tout de suite (prudent). `qgpu_open` ne l'active
que si le bit est là **et** que sa tranche (`GET_INFO`, `GET_SLOT`) est celle que le device calcule
depuis `SHMEM_SIZE` et `CLIENTS` : un autre kext, qui découperait autrement, ferait sinon désigner
le compteur d'un voisin. Compatibilité : vieux plugin ou vieux device → règle globale d'avant (sur
un device v19 sans le bit, les registres valent 0).

**Épreuves (07/10/2026, PC Linux, VM de dev 10.4.11, single-user).**

| épreuve | où | résultat |
|---|---|---|
| carte des registres (0xC8..0xD4 sous `TOPADDR`, après `LAYOUT`), bit disjoint, `QGPU_CLIENT_SLOT_BYTES` | `tests/qgpu_core_test.c` `run_v9` | OK |
| depuis Open Firmware : tranche 0 = global ; un flux fautif en tranche 1 ne fait bouger que la tranche 1 | `tests/qgpu_smoke.py` | 61/61 (6 de plus) |
| par le vrai kext : refus `LIMIT` → global +1, sa tranche +1, les autres 0 | `guest/qgpu-test` | OK |
| deux clients : `errpeer` refuse un flux toutes les 10 ms pendant `gltest game` (60 images) | `tools/guest/jobs/regerr` | plugin d'**avant** : 1 repli, fin en **synchrone** ; d'**après** : **0 repli, 71 mouvements étrangers**, fin en asynchrone ; chacun seul : 0 repli |
| non-régression : `qgpu_test`, scènes `gltest` contre Apple, `TEX3`, `C16` | `tools/guest/jobs/gpu` | `gltest` **52 OK, 0 échec**, verdict 0 échec |
