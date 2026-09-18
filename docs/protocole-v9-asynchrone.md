# Protocole v9 — le doorbell asynchrone

*Tâche 2.2 de `docs/todo-gpu-3d.md`. Ce document décrit ce que le **device**
fait désormais, et ce que le **kext** et le **plugin** devront faire pour en
tirer le gain. Le contrat lui-même, qui fait foi, est dans
`patches/qgpu/qgpu_proto.h` (section « v9 »), recopié à l'identique dans
`kext/POMPPCGPU/qgpu_proto.h`.*

## 1. Pourquoi

Jusqu'à la v8 le device exécutait le flux **dans l'écriture MMIO du doorbell** :
le vCPU restait gelé pendant tout le rendu hôte. Le profil de Marble Blast
après la géométrie brute et la fusion des dessins le disait sans détour :
`__memcpy` 15,8 % (tâche 2.1) et **`mach_msg_trap` 13,9 %** — l'attente du
device et du WindowServer.

La v9 déplace l'exécution sur un **thread de rendu hôte** et met les
soumissions en **file**. L'invité dépose et repart ; le GPU de l'hôte dessine
pendant que le PowerPC émulé prépare l'image suivante.

**Rien n'est retiré.** Le mode synchrone reste le défaut, au bit près : un
invité v1–v8 qui écrit 1 dans `QGPU_REG_DOORBELL` puis lit `STATUS`/`FENCE`
juste après voit exactement ce qu'il voyait. C'est vérifié par
`tests/qgpu_smoke.py`, qui commence par le scénario v1 inchangé.

## 2. Sémantique

### Deux modes, choisis **par soumission**

| écriture dans `QGPU_REG_DOORBELL` | effet |
|---|---|
| `QGPU_DOORBELL_GO` (1) | exécution **synchrone** : l'écriture ne rend la main qu'une fois la soumission terminée. Si des soumissions asynchrones sont encore en file, elle passe **après** elles. Si la file est pleine, elle **attend** une place — un doorbell synchrone n'est jamais refusé. |
| `QGPU_DOORBELL_GO \| QGPU_DOORBELL_ASYNC` (3) | la soumission `(SUBMIT_OFF, SUBMIT_LEN)` est **mise en file** et l'écriture rend la main tout de suite. Si la file est pleine, **rien** n'est mis en file. |
| valeur sans le bit 0 | rien, comme en v1. |

La file fait `QGPU_QUEUE_DEPTH` = 16 places (le device publie la sienne dans
`QGPU_REG_QUEUE_DEPTH` : un invité prudent lit le registre).

### Registres ajoutés

| offset | nom | lecture |
|---|---|---|
| 0x18 | `QGPU_REG_DOORBELL` | (déjà là) **soumissions en attente ou en cours** ; 0 = l'hôte n'a plus rien à faire |
| 0x38 | `QGPU_REG_QUEUE_FREE` | places libres = `QUEUE_DEPTH − DOORBELL` |
| 0x3C | `QGPU_REG_FENCE_SUBMITTED` | soumissions **acceptées** |
| 0x40 | `QGPU_REG_SUBMIT_ST` | suite donnée à la **dernière écriture du doorbell** : `QGPU_ST_OK` ou `QGPU_ST_QUEUE_FULL` |
| 0x44 | `QGPU_REG_ERRORS` | soumissions **terminées** avec un statut ≠ OK depuis le reset |
| 0x48 | `QGPU_REG_QUEUE_DEPTH` | profondeur de la file de ce device |

`QGPU_CTRL_TOPADDR` passe de 0x40 à 0x50. `QGPU_REG_FENCE`, `QGPU_REG_STATUS`
et `QGPU_REG_STATUS_PC` gardent leur sens : nombre de soumissions **terminées**,
et statut de la **dernière terminée**.

**Ordre de lecture.** L'hôte publie `FENCE` **en dernier**, après le statut et
après les relectures. L'invité lit donc **`FENCE` d'abord**, `STATUS`/`STATUS_PC`
ensuite — dans l'autre sens il pourrait attribuer à la soumission *n* un statut
plus ancien qu'elle. (Avec plusieurs soumissions en vol le statut ainsi lu peut
au contraire venir d'une soumission plus récente : c'est inévitable, et c'est à
quoi sert `QGPU_REG_ERRORS`.)

Nouveau bit de capacité : `QGPU_CAP_ASYNC` (0x8). Sans lui, un device traite
`QGPU_DOORBELL_ASYNC` comme un doorbell synchrone — un invité v9 reste correct,
il est seulement aussi lent qu'en v8.

### Numéro de barrière d'une soumission

Frapper le doorbell asynchrone, puis lire `QGPU_REG_FENCE_SUBMITTED` : sa
valeur **est** le numéro de barrière de la soumission qu'on vient de déposer.
Attendre sa fin, c'est attendre `QGPU_REG_FENCE ≥ f`. Les compteurs sont des
entiers de 32 bits qui bouclent : comparer par `(SInt32)(fence − f) >= 0`.

### File pleine

Un doorbell asynchrone refusé (`QGPU_REG_SUBMIT_ST` = `QGPU_ST_QUEUE_FULL`) :

* ne met **rien** en file, rien ne sera exécuté ;
* n'avance **ni** `FENCE` **ni** `FENCE_SUBMITTED` ;
* n'incrémente **pas** `QGPU_REG_ERRORS` — aucune soumission n'a fini en erreur.

`QGPU_ST_QUEUE_FULL` (10) n'apparaît jamais dans `QGPU_REG_STATUS` : ce n'est
pas le résultat d'un flux, c'est un refus d'acceptation. L'invité réessaie
après avoir attendu `QUEUE_FREE > 0`, ou se replie sur le mode synchrone.

### Erreurs

Chaque soumission reste **indépendante**, comme en v8 : une soumission qui
échoue n'empêche pas les suivantes de la file de s'exécuter. Elle avance
`FENCE` comme les autres, pose son statut dans `STATUS`/`STATUS_PC`, et
incrémente `QGPU_REG_ERRORS`.

**Choix fait, et à retenir côté invité** : avec plusieurs soumissions en vol,
`STATUS` ne suffit plus à conclure « tout s'est bien passé » — il ne décrit que
la dernière terminée. C'est **`QGPU_REG_ERRORS` qui fait foi** : le lire avant
la rafale et après la barrière dit s'il y a eu une erreur ; `STATUS`/`STATUS_PC`
disent laquelle pour la dernière. (L'alternative — arrêter la file à la première
erreur — a été écartée : elle aurait changé la sémantique v8 de l'indépendance
des soumissions, et un flux fautif est de toute façon un bug du plugin, pas un
état à rattraper.)

### Interruption

`QGPU_IRQ_DONE` est levée à **chaque** soumission terminée, et se démasque
comme avant (`QGPU_REG_IRQ_MASK`). Elle est de **niveau** et se **coalesce** :
une seule interruption peut couvrir plusieurs soumissions terminées. Le
gestionnaire acquitte puis **relit `FENCE`** — il ne compte pas les
interruptions.

### Reset

Écrire `QGPU_REG_MAGIC` **vide la file** (les soumissions en attente sont
jetées, elles n'avanceront jamais `FENCE`), **attend la fin de celle qui est en
cours**, puis remet tous les compteurs à zéro. Au retour de l'écriture, plus
aucune soumission ne touche BAR0 : c'est ce qui rend le reset sûr avant de
rendre la mémoire d'un client.

## 3. Contrat mémoire

C'est le point à ne pas se tromper. Tant que `QGPU_REG_FENCE` n'a pas dépassé
une soumission, elle est **en vol** : l'hôte peut lire ou écrire à tout moment
les zones de BAR0 qu'elle désigne. Pour une soumission de barrière `n`, et
**jusqu'à ce que `FENCE ≥ n`**, l'invité doit :

1. **Ne pas modifier** le flux de commandes `[SUBMIT_OFF, SUBMIT_OFF+SUBMIT_LEN)`
   ni aucune zone qu'il désigne : sommets et indices de `DRAW_*` / `DRAW_RAW`,
   texels de `TEX_IMAGE`, pixels de `SURF_UPLOAD` / `DEPTH_UPLOAD` /
   `STENCIL_UPLOAD`.
2. **Ne pas lire** les zones de destination : les relectures
   (`SURF_READBACK`, `DEPTH_READBACK`, `STENCIL_READBACK`, `QUERY_RESULT`)
   n'apparaissent dans BAR0 qu'à l'avancement de `FENCE`. Avant, leur contenu
   est indéterminé — ancien, partiel, ou en cours d'écriture.
3. En revanche, **réécrire `SUBMIT_OFF` / `SUBMIT_LEN`** est sans danger : le
   device les a lus au moment du doorbell et n'y revient pas.

**Objets et ordre.** Contextes, surfaces, textures et requêtes sont partagés par
toutes les soumissions. Il n'y a qu'**une** file et **un** thread de rendu, et
l'exécution suit **l'ordre de soumission**, sans réordonnancement — y compris
entre clients différents du kext. Une soumission voit donc l'état laissé par
celle qui la précède, exactement comme en v8. Le device reste mono-contexte
courant : chaque soumission commence par un `CTX_BIND`.

**Hors périmètre.** La migration et le retrait à chaud ne sont pas tenus (ils ne
l'étaient pas davantage avant : les objets hôte ne migrent pas). Le device
draine sa file avant de sauver son état, et l'invité repart d'un device vide au
chargement. La version de `VMStateDescription` passe de 1 à 2, la fenêtre de
registres ayant changé de taille.

## 4. Ce que fait le device (`patches/qgpu/qgpu-pci.c`)

* Un **thread de rendu** (`qemu_thread_create`, nommé `qgpu-render`) créé au
  `realize`, arrêté au `exit` du device **et** par un `qemu_add_exit_notifier`
  à la sortie de QEMU. L'arrêt draine ce qui est en cours avant de joindre.
* Une **file circulaire** de `QGPU_QUEUE_DEPTH` couples `(off, len)`, protégée
  par un mutex et deux conditions (`cond_work` réveille le thread, `cond_done`
  réveille le vCPU qui attend).
* `qgpu_core_execute` s'exécute **hors verrou et hors BQL**.

### Pourquoi le mode synchrone passe AUSSI par le thread

Un contexte CGL peut être rendu courant sur des threads différents à des moments
différents — c'est déjà ce que fait `qgpu-gl.c`, qui le rend courant au début de
chaque opération — mais **jamais sur deux threads à la fois**. Garder
l'exécution en place sur le vCPU quand la file est vide obligerait à prouver
cette exclusion sur chaque chemin (et à la re-prouver à chaque évolution), pour
économiser deux réveils de condition — quelques microsecondes sur une opération
qui en coûte des centaines. **Un seul propriétaire du contexte GL, un seul ordre
d'exécution** : c'est aussi ce qui rend gratuite la garantie « l'ordre
d'exécution est l'ordre de soumission », y compris quand un invité mélange les
deux modes, ou quand plusieurs clients du kext soumettent.

Pendant l'attente d'un doorbell synchrone, le **BQL reste pris** : c'est
exactement ce que faisait la v8, donc aucune régression pour un invité
synchrone. Le thread de rendu, lui, n'a jamais besoin du BQL pour avancer.

### Sûreté vis-à-vis de QEMU

* Le thread de rendu ne prend **jamais** le BQL. Le vCPU prend le BQL puis le
  mutex de la file ; le thread ne prend que le mutex : **pas d'inversion**.
* `FENCE`, `STATUS`, `STATUS_PC` et `ERRORS` sont écrits par `qatomic_*`.
  **`FENCE` est écrit en dernier et en `qatomic_store_release`** ; il est lu en
  `qatomic_load_acquire`. C'est cette barrière qui publie, en même temps que le
  statut, **tout ce que la soumission a écrit dans BAR0** — voir FENCE avancer,
  c'est voir les relectures.
* L'interruption est levée par un **bottom half** (`qemu_bh_new` au `realize`,
  `qemu_bh_schedule` depuis le thread — sûr depuis n'importe quel thread).
  `pci_set_irq` n'est jamais appelé depuis le thread de rendu. Le BH est
  **annulé** au reset, pour qu'un `DONE` en retard ne survive pas à la remise à
  zéro des compteurs.
* BAR0 est de la RAM QEMU ordinaire : `qgpu_core_execute` peut la lire et
  l'écrire depuis n'importe quel thread. C'est l'**invité** qui doit respecter
  le contrat mémoire ci-dessus, pas l'hôte qui doit se protéger de lui.

## 5. Ce que le KEXT devra changer

*(Rien n'a été touché dans `kext/` hors la copie identique de `qgpu_proto.h`.)*

L'ABI du user client ne change pas ; sa **sémantique** change quand le kext
posera `QGPU_DOORBELL_ASYNC` :

* **`QGPU_UC_SUBMIT`** — écrire `3` au lieu de `1` dans `QGPU_REG_DOORBELL`
  quand `QGPU_REG_CAPS & QGPU_CAP_ASYNC` et que le client l'a demandé.
  Rendre alors **`QGPU_REG_FENCE_SUBMITTED`** (la barrière de *cette*
  soumission) au lieu de la fence déjà atteinte, et **`QGPU_REG_SUBMIT_ST`**
  comme `status` (l'acceptation : `QGPU_ST_OK` ou `QGPU_ST_QUEUE_FULL`) au lieu
  du résultat du rendu. Sur `QGPU_ST_QUEUE_FULL`, le client réessaie — le kext
  ne doit pas boucler dans la command gate, qui sérialise tous les clients.
  `status_pc` n'a plus de sens à la soumission : il se lit après la barrière.
* **`QGPU_UC_WAIT_FENCE`** — ne plus scruter à `IOSleep(1)`. Aujourd'hui la
  boucle est justifiée (« avec un device synchrone la fence est déjà
  atteinte »), mais elle coûterait jusqu'à une milliseconde par attente en v9.
  Dormir sur la command gate (`fGate->commandSleep(&fIRQCount, deadline)`) et se
  faire réveiller par `irqAction`, qui fait déjà le `commandWakeup` —
  l'interruption `DONE` est déjà démasquée au `start()`. Garder la scrutation en
  repli quand `fIRQSource` est nul, et **relire `FENCE` au réveil** (l'IRQ se
  coalesce, elle ne compte pas les soumissions).
* **`QGPU_UC_GET_INFO`** peut rendre `QGPU_REG_QUEUE_DEPTH` pour que le
  userland dimensionne son double tampon sans supposer 16.
* **Fermeture d'un client** (`destroyClientObjects`) : les soumissions de
  destruction partent déjà en synchrone, ce qui reste correct — elles passent
  après tout ce que le client avait en vol. **Ne pas** les passer en asynchrone
  sans les faire suivre d'une attente de barrière : la tranche de BAR0 est
  rendue juste après.
* Le reset (`QGPU_UC_RESET`) écrit `QGPU_REG_MAGIC`, qui vide déjà la file et
  attend la soumission en cours : rien à ajouter, mais il détruit **tous** les
  objets, y compris ceux des autres clients — inchangé, et toujours à surveiller.

## 6. Ce que le PLUGIN devra changer

*(Rien n'a été touché dans `guest/`.)*

Le gain n'existe que si le plugin **ne lit pas** tout de suite ce qu'il vient de
soumettre. Aujourd'hui `flush()` soumet puis parcourt `G.post[]` dans la même
foulée : chaque image attend l'hôte.

1. **Double tampon de la tranche.** Les zones *flux de commandes*, *sommets /
   indices* et *arène de relecture* de la tranche du client doivent exister en
   deux exemplaires, alternés à chaque soumission. Sans cela, écrire le flux de
   l'image *n+1* pendant que l'hôte lit celui de l'image *n* viole le point 1 du
   contrat mémoire — et le symptôme serait des triangles qui clignotent, pas un
   plantage. La tranche fait `shmem/QGPU_MAX_CLIENTS` : il y a la place.
2. **N'attendre la barrière qu'avant de lire une relecture.** `flush()` garde la
   barrière rendue par `QGPU_UC_SUBMIT` ; le parcours de `G.post[]` (recopie des
   pixels, de la profondeur, du stencil vers les tampons de GLEngine) se fait
   après un `QGPU_UC_WAIT_FENCE` sur cette barrière — et *seulement* si la
   soumission avait des relectures. Une soumission qui n'en a pas (l'immense
   majorité : dessins, changements d'état, téléversements) n'est jamais
   attendue.
3. **Présentation directe** (tâche 2.4) : l'écriture dans le rectangle de la
   fenêtre attend la barrière de l'image, et elle seule. C'est le point de
   synchronisation naturel — une image par attente, au lieu d'une attente par
   soumission.
4. **Détection d'erreur.** `broken_all()` ne peut plus se fier au `status` rendu
   par `QGPU_UC_SUBMIT` (qui ne dira plus que l'acceptation) : lire
   `QGPU_REG_ERRORS` au moment où l'on attend déjà la barrière — c'est-à-dire
   une fois par image — et couper l'accélération si le compteur a bougé.
   `STATUS`/`STATUS_PC` donnent alors la dernière fautive pour le journal.
5. **File pleine.** Sur `QGPU_ST_QUEUE_FULL`, attendre la barrière la plus
   ancienne en vol plutôt que de boucler à vide. Avec 16 places et une poignée
   de soumissions par image, le cas doit rester rare : s'il ne l'est pas, c'est
   que le plugin soumet trop souvent — la fusion des dessins l'a déjà ramené à
   65-85 `DRAW_RAW` par image.
6. **Interrupteur** `POMPPC_GL_ASYNC` (défaut à décider à la mesure), pour
   pouvoir comparer A/B sur Marble Blast sans reconstruire.

## 7. Vérification

* `tests/qgpu_core_test.c` — `run_v9` : carte des registres (offsets alignés,
  distincts, sous `QGPU_CTRL_TOPADDR`), bits du doorbell disjoints, statut et
  capacité qui ne recouvrent rien, puis l'invariant d'ordre et d'indépendance
  que la file promet (une soumission fautive au milieu n'empêche pas la suivante
  de voir l'état laissé par la première). **Les deux backends passent.**
* `tests/qgpu_smoke.py` — de bout en bout, par Open Firmware, sur un QEMU
  reconstruit : le scénario v1 inchangé, puis rafale de 40 doorbells
  asynchrones (16 acceptées, les autres refusées avec `QGPU_ST_QUEUE_FULL` —
  c'est **la preuve que le device n'exécute pas dans l'écriture MMIO**), la
  barrière qui rattrape, la relecture visible seulement après elle, trois
  soumissions mises en file d'un coup dont celle du milieu est fautive (les deux
  autres s'exécutent), l'`IRQ DONE` posée par le thread de rendu, et une
  soumission synchrone finale qui est terminée au retour du `stw`.
