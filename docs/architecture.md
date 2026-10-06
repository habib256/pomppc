# Architecture — propriété et synchronisation

État relu le 27/09/2026 : protocole qgpu v22, ABI de transport v19. Limites
L2 à L5 relues contre le code le 07/10/2026 (suites A6) : chacune porte son
état daté et ses preuves `fichier:ligne` (lignes de `954c452`, sauf le test `run_a6` ajouté ce jour-là).
Ce document décrit principalement la chaîne graphique Tiger, où se croisent
mémoire partagée, clients IOKit, vCPU et rendu hôte. Le contrat binaire reste
dans [protocole.md](protocole.md) et les en-têtes qu'il référence.

**Lecture des invariants :** une règle « doit » exprime le contrat à préserver.
Les mécanismes cités décrivent le code actuel. Les écarts identifiés par
**L1…L6** sont des limites relevées le 27/09, pas des garanties ni des
corrections ; chacune dit son état daté (au 07/10 : L2 et L5 tiennent, L1,
L3, L4 et L6 sont fermées dans le code, leurs épreuves en VM restent dues).
La relecture statique ne remplace pas une épreuve de concurrence en VM.

## 1. Frontières et propriétaires

| Ressource | Propriétaire et durée de vie | Accès et libération |
|---|---|---|
| État et tampons privés de GLEngine | Apple / application invitée | Le plugin emprunte les adresses ; il ne les libère pas. Leur validité doit couvrir les lectures et les copies différées. |
| État du plugin, `PCtx`, suivi des textures et tampons | Une instance du plugin dans un processus | `G.mu` sérialise les chemins partagés ; fermeture via `pomppc_backend_fini`. |
| Connexion IOKit et mapping `QgpuClient.win` | Client userland | `qgpu_close` démappe puis ferme la connexion ; le mapping n'est pas une allocation privée du plugin. |
| Attribution d'une tranche BAR0 | Kext, table `fClients` et `fSlotBusy` | Allocation, fermeture et reset passent par la command gate ; le device reste propriétaire de la RAM sous-jacente. |
| BAR0, registres BAR1, file `QgpuJob` | Device `qgpu-pci` | Registres de soumission sous BQL ; file sous `s->lock`. Un job copie les offsets et la longueur, **pas les octets du flux**. |
| Contextes, surfaces, textures, requêtes, tampons hôte | `QgpuCore`, objets du backend associés | Exécution et destruction par le thread de rendu en régime normal. Les plages d'identifiants appartiennent logiquement aux clients. |
| Programmes ARB/GLSL | Un contexte qgpu | Espace d'identifiants commun aux deux familles ; libérés avec le contexte. |
| Contexte graphique hôte | Backend GL | Un seul thread l'utilise à la fois. L'initialisation précède le lancement du thread de rendu ; ensuite celui-ci exécute, reset et libère le backend. |
| Cible de présentation VGA/QFB | Device d'écran | qgpu emprunte la VRAM et retient sa `MemoryRegion` ; le cœur ne la libère pas. Rebinding après drainage. |

Le kext ne compile que [qgpu_abi.h](../patches/qgpu/qgpu_abi.h), jamais les
opcodes ni les limites sémantiques de [qgpu_proto.h](../patches/qgpu/qgpu_proto.h).
Il attribue les tranches selon les registres du device ; le plugin vérifie la
compatibilité de leur disposition à l'ouverture. Ajouter une classe d'objet
ne doit pas introduire son nettoyage dans le kext.

**Frontière de confiance.** L'attribution des plages est une convention entre
clients coopératifs, pas une isolation de sécurité entre processus invités.
Le kext borne la plage du flux soumis à la tranche du client. Le cœur borne
les références contenues dans ce flux à BAR0 et à l'espace global des objets,
sans vérifier leur appartenance au client. Un client peut donc référencer ou
détruire les objets d'un autre. La validation des bornes ne constitue pas, à
elle seule, une preuve générale de sûreté de l'hôte.

Sources : [device](../patches/qgpu/qgpu-pci.c), `QgpuJob`, `QgpuPCIState` et
commentaire « ISOLATION ENTRE CLIENTS » ; [client](../guest/gldriver/pomppc_qgpu.c),
`qgpu_open` / `qgpu_close` ; [cœur](../patches/qgpu/qgpu-core.h).

## 2. Threads, verrous et attentes

| Domaine | Règle |
|---|---|
| Plugin | Modifier le flux sous `G.mu`. Si une attente relâche ce mutex, `G.halt` interdit aux autres threads de modifier ou de changer les moitiés du flux via `stream_ready`. `Half.waiting` interdit une double attente concurrente de la même moitié. |
| Kext | La command gate sérialise notamment la séquence OFF/LEN/DOORBELL et l'attribution des tranches. `commandSleep` relâche la gate pendant l'attente ; chaque état susceptible de changer doit être recontrôlé au réveil. |
| Device, vCPU / thread principal | Ordre de verrouillage : **BQL puis `s->lock`**. Les compteurs et indices de la file sont protégés par ce mutex. |
| Thread de rendu | Prend `s->lock` pour extraire/terminer un job, puis exécute hors mutex. **Ne prend jamais le BQL.** Il est enregistré auprès de RCU pour les accès au suivi de mémoire invitée. |
| IRQ | Le thread de rendu planifie un bottom half ; celui-ci publie l'IRQ sous BQL. Ne pas appeler directement la mise à jour de ligne PCI depuis le thread de rendu. |

Le job en cours reste compté dans `q_count` jusqu'à sa fin : « file vide »
signifie qu'aucun job n'est en attente **ni en cours**. L'ordre d'exécution
est l'ordre FIFO d'acceptation, tous clients confondus, resets de client compris.

Le doorbell synchrone conserve le BQL pendant son attente. Le relâcher
localement violerait les hypothèses de la garde de réentrance MMIO de ce device.
Son attente est bornée par `QGPU_SYNC_WAIT_MS` ; depuis le 29/09, les resets,
la sauvegarde et la sortie de QEMU le sont aussi par `QGPU_RESET_WAIT_MS`
(L3, fermée sauf au retrait du device). L'attente asynchrone invitée utilise IRQ et timer
de secours ; un réveil signifie « relire la barrière », pas « travail réussi ».

Sources : `qgpu_render_thread`, `qgpu_doorbell`, `qgpu_irq_bh` dans le device ;
`submitGated`, `sleepForFence` dans le [kext](../kext/POMPPCGPU/POMPPCGPU.cpp) ;
`wait_half_ex`, `stream_ready` dans le [plugin](../guest/gldriver/pomppc_accel.c).

## 3. Soumission, durée de vie des octets et barrières

**M1 — Une zone soumise est empruntée jusqu'à sa fin effective.** Commandes,
sommets, indices et sources de transfert dans BAR0 restent immuables jusqu'à
la barrière correspondante. Les destinations de relecture ne sont consommées
qu'après cette barrière. Réécrire OFF/LEN après acceptation est permis ; réécrire
les octets qu'ils désignaient ne l'est pas.

**M2 — Acceptation et achèvement sont deux événements différents.**

1. Le plugin remplit une moitié libre de sa tranche.
2. Le kext sérialise OFF/LEN/DOORBELL ; ses écritures de registres appellent
   `OSSynchronizeIO`. Préserver l'ordre entre préparation mémoire et notification.
3. En asynchrone, lire `SUBMIT_ST` avant d'utiliser `FENCE_SUBMITTED`.
   `QUEUE_FULL` signifie qu'aucun job n'a été accepté et qu'aucun compteur
   n'a avancé : la barrière lue ne désigne pas cette tentative.
4. Le thread de rendu exécute le job, publie statut et résultats, puis incrémente
   `FENCE` avec `qatomic_store_release`. La lecture MMIO de FENCE emploie
   `qatomic_load_acquire` : elle publie aussi les relectures écrites dans BAR0.
5. Après achèvement, le plugin effectue les copies différées (`run_posts`),
   puis peut réutiliser la mémoire. Il faut aussi que leurs pointeurs de
   destination invités soient encore valides.

`FENCE_SUBMITTED` et `FENCE` sont globaux au device et sur 32 bits. Comparer
par différence signée : `(int32_t)(fence - cible) >= 0`, dans la même époque
de reset et sans écart de 2^31 ou plus. Une IRQ peut coalescer plusieurs fins ;
elle n'est ni un compteur de jobs ni un verdict de rendu.

La barrière signifie fin de traitement du job par le backend et disponibilité
des résultats requis par ses commandes. **Ce n'est pas une preuve que l'image
a été affichée physiquement par le frontend, ni une synchronisation verticale.**

**M3 — Timeout n'est pas annulation.** Un job accepté peut encore lire BAR0 après
le retour d'une attente expirée, y compris après un doorbell synchrone expiré.
Ni le passage en mode synchrone ni un statut d'erreur ne rendent ces octets libres.
Ne resoumettre automatiquement que lorsque la non-acceptation est établie
(`QUEUE_FULL`), sous peine de rejouer deux fois des commandes.

Le plugin alterne deux moitiés suivies par `Half.busy` et `Half.fence` ;
`switch_half` attend avant réutilisation. **L1 (corrigé le 29/09/2026) :** sur
délai, `wait_half_ex` garde la moitié en quarantaine et réattend ; après
`WAIT_GIVEUP` délais consécutifs, `stream_dead()` la retire pour toujours
(flux détourné vers un tampon privé, `G.state = -1`), sans jamais la rendre à
l'écriture. Un seul fil attend verrou relâché à la fois : au retour de
`flush()`, `G.halt` est nul et le flux reste à l'appelant jusqu'à ce qu'il
relâche lui-même le verrou. Épreuve restant à faire : retarder le backend
au-delà de `WAIT_MS` et vérifier l'absence de réécriture de la zone en vol.

Sources : `qgpu_run_job`, `qgpu_ctrl_read`, `qgpu_sync_gave_up` ;
`submit_cur`, `wait_half_ex`, `switch_half` ;
[accès aux registres](../kext/POMPPCGPU/POMPPCGPU.h), `regWrite`.

## 4. Contextes et cohérence des copies

**C1 — Un contexte logique n'est pas le contexte GL hôte.** `CTX_CREATE`
initialise son état, sans surface ni requête active. `CTX_BIND` sélectionne
le contexte courant du cœur. Celui-ci étant global à l'exécuteur, chaque
soumission qui utilise un contexte doit le sélectionner explicitement ;
ne jamais dépendre du dernier client exécuté.

**C2 — Détruire un contexte ne détruit pas toute sa tranche.** `CTX_DESTROY`
termine sa requête active, libère ses programmes et invalide le contexte
courant s'il le désignait. Surfaces, textures, tampons et objets requêtes ont
leur propre durée de vie. `CLIENT_RESET` effectue le nettoyage de toutes ces
classes dans les plages du client. Une surface détruite est déliée de tous
les contextes qui la désignaient, y compris dans une autre tranche.

**C3 — La copie consommée doit être la copie à jour.** Le plugin suit
séparément couleur et profondeur :

| État logique | Obligation avant consommation |
|---|---|
| `SW_NEWER` | Téléverser les données nécessaires avant leur utilisation hôte. |
| `HOST_NEWER` | Relire, attendre et copier les données nécessaires avant leur utilisation par Apple / l'application. |
| `SYNCED` | Aucun transfert supplémentaire requis selon le suivi du plugin. Ce drapeau seul ne prouve pas l'achèvement d'un transfert déjà encodé. |

Un effacement complet peut rendre inutile le contenu précédent. Un transfert
partiel ou refusé ne doit pas valider des pixels qu'il n'a pas synchronisés.
Le retour vers Apple dépend de cette cohérence ; il n'est pas une récupération
universelle garantie pour tous les états, notamment sous programmes ARB.

**L6 : `sync_to_host` pose encore `SYNCED` quand il n'y a rien à porter**
(source absente, 16 bits sans la v15). Depuis la 3e passe du bug hunt du
29/09, la taille n'en fait plus partie : téléversements et relectures d'une
image plus grande que l'arène partent par bandes de lignes (`band_rows`,
`upload_bands`, `queue_readback_to`), et `SYNCED` n'est posé que si toutes
les bandes sont parties. Une relecture dont la soumission s'est arrêtée avant
elle (`posts_lost` : refus synchrone hors dessin, ou `ERRORS` qui a bougé en
asynchrone) repasse `SYNCED → HOST_NEWER`, et `sync_to_sw_locked` relit.
Depuis la 4e passe, seuls le contexte et le canal (couleur ou profondeur) de
la relecture perdue basculent (`Post.ctx`), `SYNCED` est posé AVANT les
bandes (un vidage entre deux bandes peut perdre les premières), et quand
`ERRORS` bouge, TOUTES les moitiés encore en vol perdent leurs relectures.
L'invalidation descend jusqu'à `err_floor`, la plus ancienne soumission que
la dernière lecture propre d'`ERRORS` ne couvrait pas.
Ne pas déduire une égalité des pixels du seul drapeau pour les deux cas
« rien à porter ».

Sources : `exec_one`, `qgpu_core_client_reset` dans le cœur ; `sync_to_host`,
`sync_to_sw_locked`, `run_posts` dans le plugin ;
[limites du repli](re/glengine-exit-interpolateur.md).

## 5. Clients morts, fermeture et reset de client

**V1 — Une tranche n'est réattribuable qu'après la fin de ses anciens lecteurs
et le nettoyage de ses objets.** Le chemin normal est :

`clientClose/clientDied → freeSlot → slotGated → CLIENT_RESET en FIFO
→ attente de sa barrière → libération du créneau`.

`slotGated` vérifie **sous la gate** `fClients[slot] == client`, puis pose
`fSlotBusy`. La gate peut être relâchée pendant l'attente ; busy empêche la
réattribution et un second nettoyage. Cette vérification de propriétaire
empêche une fermeture tardive de détruire les objets d'un nouvel occupant.
`clientDied` utilise le même chemin que `clientClose` ; aucun callback userland
du processus mort n'est requis pour nettoyer les objets hôte.

**L2 : cette garantie est conditionnelle au succès du nettoyage.**
`destroyClientObjects` retourne `void` ; un refus, un défaut de drainage ou
un timeout est seulement journalisé. `slotGated` efface ensuite busy et peut
libérer le créneau malgré cet échec. Le retour réussi de la fermeture ou du
reset ne prouve donc pas que le nettoyage a terminé.

*État au 07/10/2026 : **tient encore**, telle qu'écrite.* Aucun correctif
depuis le 27/09 n'y touche (KG3, KT3 et K7 règlent la propriété du créneau,
pas le résultat du nettoyage). Preuves : `destroyClientObjects` est `void`
(kext/POMPPCGPU/POMPPCGPU.cpp:1086) ; ses trois échecs se contentent de
`GPULog` puis rendent la main — CLIENT_RESET refusé ou file pleine au-delà
de `DESTROY_RETRIES` (:1105-1109), file non drainée avant (:1111-1114),
barrière non atteinte après (:1117-1119) ; `slotGated` efface busy et rend
le créneau sans regarder (:945-950) et pose `kIOReturnSuccess` d'office
(:898) ; `resetSlot` rend donc un succès au plugin même après échec
(:1014-1016). Les deux échecs n'ont pas la même gravité, ce que
`tests/qgpu_core_test.c` fixe désormais au niveau du cœur (`run_a6`,
tests/qgpu_core_test.c:9418) :

- **nettoyage jamais mis en file** (refus, file non drainée) : les objets
  de l'ancien occupant restent, et la première création du nouvel occupant
  sur le même identifiant est une faute **fatale** `QGPU_ST_LIMIT`
  (patches/qgpu/qgpu-core.c:3542-3543) qui arrête son flux : accélération
  perdue pour ce processus (cas (a) du test) ;
- **nettoyage accepté mais retardé** (le kext a cessé d'attendre) : la file
  est FIFO (patches/qgpu/qgpu-pci.c:684-686), la destruction passe donc
  avant toute soumission du nouvel occupant, qui crée ses objets sans
  erreur (cas (b)). Reste à risque la mémoire, pas les objets : les
  dernières soumissions de l'ancien occupant, devant le CLIENT_RESET dans
  la file, lisent encore la tranche de BAR0 que le nouveau commence à
  écrire (M1) ;
- un CLIENT_RESET mis en file **après** la réattribution détruit les objets
  du nouvel occupant (cas (c)) : un nettoyage manqué ne se rattrape
  qu'avant de rendre le créneau.

Correction à faire dans le kext (pas dans cette relecture : il se compile
dans l'invité) : `destroyClientObjects` rend un `IOReturn` et, en cas
d'acceptation, la barrière visée ; `slotGated` ne remet pas le créneau à
`allocSlot` sur échec mais le marque « sale » avec cette barrière
(`fSlotFence[slot]`) ; `allocSlot` saute un créneau sale tant que `FENCE`
n'a pas atteint sa barrière (FIFO : objets détruits ET tranche plus lue),
et relance d'abord, dans la gate, le CLIENT_RESET d'un créneau sale sans
barrière (refusé la première fois) ; `resetSlot` rend l'échec au plugin.
Épreuve de fermeture : en VM, avec un retard injecté dans le thread de
rendu de l'hôte (propriété de test du device à ajouter, par ex.
`x-test-stall-ms`, et un refus forcé de CLIENT_RESET), tuer un client
pendant une soumission, file pleine, puis ouvrir un nouveau client : il
doit recevoir un autre créneau (ou attendre), jamais `QGPU_ST_LIMIT` à sa
première création, et le journal du kext doit nommer le créneau sale puis
sa libération.

`QGPU_UC_RESET` nettoie les objets **sans rendre le créneau** : `resetSlot`
conserve son propriétaire. Le plugin doit abandonner ses anciens identifiants
et reconstruire l'état nécessaire après un reset de ses objets.

À la terminaison normale de la bibliothèque, `pomppc_backend_fini` pose
`G.state = -1` avant de vider/drainer puis de fermer la connexion : les
nouveaux appels doivent cesser d'émettre même si le mutex est relâché.
Cette procédure reste soumise aux limites de timeout L1/L2.

Après `fork` sans `exec`, l'enfant **oublie** le mapping, la connexion et
l'état hérités (`pomppc_backend_forget`, `qgpu_forget`) ; il ne ferme pas le
user client du père et n'écrit pas dans sa tranche. Le handler prépare le
fork en prenant `G.mu`, puis père et enfant le relâchent chacun de leur côté.

## 6. Reset global, arrêt et présentation

| Opération | Effet et obligation |
|---|---|
| `CLIENT_RESET` / reset user client | Job FIFO, barrière globale avancée à sa fin ; détruit une tranche d'objets, pas les compteurs globaux. |
| Reset global du device | Jette les jobs non commencés, attend le job courant, annule le BH IRQ en attente, remet registres et compteurs à zéro, demande au thread de rendre le cœur vide, abaisse l'IRQ. |
| Arrêt du thread | Jette les jobs non commencés, attend le courant puis joint le thread. Au retrait, libère le backend sur ce thread ; à la sortie de QEMU, évite les appels aux bibliothèques GL potentiellement déjà démontées. |
| Arrêt du kext | Pose `fStopping`, réveille les dormeurs et attend, sans borne, qu'il ne reste ni dormeur ni appel en vol (`fSleepers`, `fCallers`), puis retire IRQ, timer et gate. Les nouveaux appels sont refusés par `enterCall`. |
| `POMPPCGPUUserClient::stop` | Efface propriétaire et créneau, utilise `forgetSlot` pour la seule comptabilité ; aucun MMIO ni entrée dans une gate potentiellement retirée. Ce chemin n'est pas un nettoyage hôte confirmé. |
| Changement de cible / géométrie | Drainage avant modification du pointeur et du pas lus sans verrou par le rendu ; à l'expiration du drainage, conserve l'ancienne cible, avec possible défaut visuel. |

**R1 — Un reset global termine une époque de barrières.** Les jobs abandonnés
n'avancent jamais leur ancienne barrière. Les compteurs à zéro et l'absence
de numéro d'époque ne permettent pas de reprendre un client vivant comme si
rien n'avait changé. Ses objets et attentes doivent être réinitialisés ; le
reset global n'est pas une réparation transparente d'une seule application.

**L3 — La disponibilité n'est pas bornée partout.** Écrit le 27/09 :
`qgpu_soft_reset` et `qgpu_stop_thread` utilisaient un drainage non borné
du job courant ; un backend bloqué pouvait empêcher reset/arrêt d'aboutir
malgré le timeout du doorbell. Ne jamais libérer de force ses ressources
pendant qu'il les utilise encore.

*État au 07/10/2026 : **fermée par des correctifs**, sauf un chemin non
borné assumé.* GL3 (888f45d, bug hunt du 29/09) borne le reset :
`qgpu_soft_reset` draine au plus `QGPU_RESET_WAIT_MS` = 5 s le job courant
puis le reset du cœur par le thread (patches/qgpu/qgpu-pci.c:715-727,
764-779) ; à l'échéance le device passe « cassé » (:734, :775) — CAPS et
CLIENTS à 0, doorbells et CLIENT_RESET refusés (:548-553, :662-665,
:784-790) — sans rien libérer sous le thread, et le prochain reset qui
aboutit le répare. `qgpu_pre_save` est borné de même (:1412). KT5
(4e3ea68, bug hunt 3) borne l'arrêt à la sortie de QEMU : drainage borné
(:1224-1236), et le thread n'est pas joint s'il est encore occupé
(:1242-1248) — `q_fini` faux lui interdit de toucher au backend. Seul
reste non borné le drainage du **retrait du device** (`fini` vrai,
:1221-1222) : il est voulu, l'état va être libéré et le thread y écrit
encore. Ce chemin n'est atteint que par un retrait à chaud, refusé
(`hotpluggable = false`, :1492), ou par l'échec de `migrate_add_blocker`
au realize (:1363), où le thread vient de naître et n'a aucun job. Aucune
attente non bornée ne reste donc sous BQL dans un scénario pris en charge.
L'épreuve en VM (GPU hôte bloqué puis rechargement du kext et
`system_reset`, GL3/KT4/KT5) est suivie par l'entrée « Correctifs des bug
hunts jamais éprouvés dans la VM » de [TODO.md](../TODO.md) ; le device
n'a pas de harnais natif (`qgpu-pci.c` ne se compile qu'avec QEMU), ce qui
interdit de l'éprouver sur l'hôte seul.

**L4 — Arrêt du kext avec dormeurs restants.** Écrit le 27/09 :
`POMPPCGPU::stop` attendait au plus environ une seconde ; s'il restait des
dormeurs, il journalisait puis retirait quand même les sources.

*État au 07/10/2026 : **description périmée, limite fermée dans le code**.*
KG4 (888f45d) a rendu l'attente sans borne, journal chaque seconde ; KT1
(4e3ea68) attend aussi les appels déjà passés le test de `fStopping`
(compteur `fCallers`, protocole de Dekker `enterCall`/`leaveCall`,
kext/POMPPCGPU/POMPPCGPU.cpp:41-70) : `stop` ne retire IRQ, timer et gate
qu'une fois `fCallers == 0 && fSleepers == 0` (:275-294). L'invariant
« aucun dormeur au retrait de la gate » est donc démontré par lecture. La
boucle se termine parce que chaque chemin gated revoit `fStopping` :
`sleepForFence` en tête et à chaque tour (:742, :765), `waitFenceCounted`
(:828, :859), `waitDestroy` (:1075), `destroyClientObjects` (:1091),
`submitGated` (:623), `slotGated` à l'allocation (:901) ; le plus long
passage non interruptible est un doorbell synchrone, borné à 2 s par le
device (patches/qgpu/qgpu-pci.c:199, :612-641). Épreuve encore à faire, en
VM : `kextunload` pendant `WAIT_FENCE`/`SUBMIT` d'un jeu ouvert, et
`SUBMIT` après déchargement → erreur propre ; elle est suivie par la même
entrée « Validation » de TODO.md (K4, KG4, KT1).

La migration et la sauvegarde/restauration de l'état VM sont bloquées par
`migrate_add_blocker` lorsque qgpu est présent : les objets GL hôte ne sont
pas sérialisés. Le vieux code `post_load` n'est pas une garantie de reprise.
Le retrait à chaud n'est pas un scénario pris en charge.

Sources : `qgpu_soft_reset`, `qgpu_stop_thread`, `qgpu_bind_scanout` et
`qgpu_pci_realize` ; `POMPPCGPU::stop`, `POMPPCGPUUserClient::stop`.

## 7. Erreurs : qui refuse, et ce que le refus signifie

| Niveau | Verdict | Ce qu'il permet de conclure |
|---|---|---|
| Ouverture du plugin | Version, capacités, disposition incompatibles | Ne pas utiliser ce transport ; ne pas improviser des plages d'identifiants. |
| Kext | Arguments hors tranche, état arrêté, gate refusée | L'appel a échoué ; ne pas interpréter des paramètres de sortie comme un succès. |
| Admission device | `SUBMIT_ST`, notamment `QUEUE_FULL` | Acceptation ou refus de mise en file, pas résultat d'exécution. |
| Exécution cœur/backend | `STATUS`, `STATUS_PC`, `ERRORS` | Résultat publié globalement (L5) ; les commandes précédemment exécutées ne sont pas annulées. |
| Attente | Timeout / transport interrompu | Achèvement non établi ; aucune autorisation implicite de réutiliser la mémoire. |

Une erreur de flux n'est **pas transactionnelle**. `qgpu_core_execute`
s'arrête sur une faute fatale. Il continue toutefois après `BAD_ARG` pour
les opcodes de dessin et pour `PROG_STRING` / `GLSL_LINK` identifiés par
`draw_op`. Il retient le premier de ces refus, même si une faute fatale
survient ensuite. `STATUS_PC` est un index en mots dans la soumission,
pas une adresse mémoire ni nécessairement l'emplacement où elle s'est arrêtée.

**L5 — Pas de verdict individuel persistant.** `STATUS/STATUS_PC` sont des
registres globaux réécrits à chaque job ; `ERRORS` compte les jobs terminés
en erreur, pas chaque commande invalide. Ils ne constituent pas un historique
par client ou par barrière, ni un instantané atomique du triplet. Une barrière
atteinte prouve la fin du travail, pas sa réussite. `check_errors` désactive
temporairement l'asynchrone lorsqu'ERRORS évolue, même si un autre client est
responsable. Le signalement local de timeout synchrone est également distinct
du résultat final du job qui peut arriver plus tard.

*État au 07/10/2026 : **tient encore**, réduite dans ses effets.* Le device
publie un seul triplet global (patches/qgpu/qgpu-pci.c:353-358) ; le kext le
lit en trois MMIO séparés, après un doorbell synchrone
(kext/POMPPCGPU/POMPPCGPU.cpp:652-654) comme pour le `PEEK` du plugin
(:611-613), et un autre job peut finir entre deux lectures ; le plugin
repasse en synchrone à tout mouvement d'`ERRORS`, quel qu'en soit l'auteur
(guest/gldriver/pomppc_accel.c:3478-3501). Ce qui a changé depuis le
27/09 : GL4 (888f45d) publie STATUS, STATUS_PC, ERRORS et FENCE sous
`s->lock`, et le doorbell synchrone abandonné écrit son `QGPU_ST_BACKEND`
sous ce même verrou (qgpu-pci.c:350-366, :509-514) : plus de « barrière
atteinte, backend en panne » sur une image rendue. Côté plugin, les
miroirs ne sont plus invalidés en bloc mais à partir du plancher
`err_floor` (T1, PC2, P-I2 : pomppc_accel.c:3452-3460, :3514-3518), et les
relectures de toutes les moitiés en vol sont déclarées perdues (P-I5,
:3486-3488). L'effet d'une erreur d'un autre client est donc borné à
`ASYNC_RETRY` images synchrones et à un renvoi ciblé d'état, mais la cause
reste. La fermer change l'ABI de transport (`qgpu_abi.h`, donc kext et
plugin) : compteur d'erreurs et dernier verdict **par tranche** (le device
déduit la tranche d'un job de son offset, avec le même découpage que le
kext, `(SHMEM_SIZE / CLIENTS) & ~0xFFF`), publiés ensemble avec la barrière
du job fautif, et un `PEEK` du kext qui rend ceux de la tranche du client.
Épreuve de fermeture : deux processus en VM, l'un qui soumet des flux
fautifs, l'autre qui doit rester en asynchrone (`n_syncfall` à 0) sans
qu'aucune de ses relectures soit invalidée ; c'est l'entrée « [Protocole]
`QGPU_REG_ERRORS` par client » de [TODO.md](../TODO.md).

Ne pas déduire une cause précise d'un simple rapprochement entre une
barrière et le dernier STATUS lu.

## 8. Preuves et limites de couverture

| Invariant | Preuve existante / contrôle | Reste à établir |
|---|---|---|
| Kext indépendant des opcodes | `tests/run-all.sh` : ABI identique et absence de symboles sémantiques ; `scripts/qgpu_contract.py --check` | Ces contrôles ne prouvent pas les cycles de vie. |
| Objets et reset de tranche | `tests/qgpu_core_test.c` : bornes, reset, destruction, déliaison des surfaces, conservation d'une autre tranche | Les tests natifs ne traversent ni IOKit ni la file du device. |
| Admission, barrières et transport | `tests/qgpu_smoke.py`, à lancer explicitement ; `run_a6` de `tests/qgpu_core_test.c` (07/10) : conséquences au cœur d'un nettoyage manqué, retardé ou rejoué (L2) | Injection de retard et non-réutilisation après timeout (L1/L2) : demande une propriété de test du device et la correction du kext (L2). |
| Cohérence de rendu | `guest/gltest`, tests croisés des backends, matrice et rejeu | Transferts impossibles (L6), scénarios de panne ; rejeu et VM partagent le même moteur. |
| Fin de session | Mécanismes de gate, busy, arrêt et fork relus dans les sources ; L3 et L4 fermées par lecture le 07/10 (resets et sortie bornés, `stop` qui attend dormeurs et appels en vol) | Mort pendant soumission, double fermeture, réouverture (L2) ; en VM, kext arrêté pendant `WAIT_FENCE`/`SUBMIT` (L4) et backend bloqué puis reset (L3), suivis par l'entrée « Validation » de TODO.md. |

Les tests natifs peuvent ignorer OpenGL s'il est indisponible : vérifier le
backend effectivement exécuté avant de conclure. Une matrice de jeux verte
ne ferme pas à elle seule les limites de concurrence ci-dessus. Les incidents
`kCGLBadDisplay`, créneaux perdus et paniques restent ouverts dans
[TODO.md](../TODO.md) ; cette relecture ne leur attribue pas une cause sans
reproduction.

## 9. Frontend : une propriété distincte

Le frontend pilote un **processus** QEMU via QMP et D-Bus. Son thread GLib
traite D-Bus ; le framebuffer partagé avec le rendu UI est protégé par
`fbMtx_`. Le mapping `Unix.Map` est retiré après la jonction du thread D-Bus
dans `QemuBridge::stop`. Le mutex `qmpMtx_` protège les échanges QMP concernés.
Ces verrous ne remplacent ni les barrières qgpu ni le protocole de capture.
La fin de `SURF_PRESENT`, la réception d'une mise à jour D-Bus et l'affichage
UI sont trois événements distincts.

Source : [QemuBridge.cpp](../frontend/src/QemuBridge.cpp). Pour l'architecture
de présentation et d'entrée : [frontend/README.md](../frontend/README.md).

## 10. Extraction progressive du plugin

L'extraction a été interrompue à la demande de l'utilisateur. Le code du
plugin est conservé dans son état initial ; aucune extraction non validée
n'est intégrée. Le premier lot envisagé isole les décisions de formats de
texture : fonctions pures, capacités explicites, sans accès à `G`, `PCtx`,
GLEngine, allocations ou soumissions.

Un essai de cette extraction a compilé dans Tiger avec gcc 4.0 et donné
49 486 022 comparaisons différentielles identiques sur l'hôte. La comparaison
des scènes invitées avant/après n'a pas été menée à terme : ces résultats ne
valident donc pas son intégration. Les correctifs L1…L6 restent indépendants
du découpage et ne sont pas implémentés par cette documentation.

| Lot suivant | Frontière à établir avant déplacement | Protection nécessaire |
|---|---|---|
| Lecteur GLEngine | Vue empruntée, accès typés, table d'offsets et identification de la version ; aucun opcode émis | Sondes d'état et scènes comparées au rendu Apple ; version inconnue refusée proprement |
| Textures | Décodage séparé du suivi des objets et de la soumission ; propriétaire explicite des copies | Formats, niveaux, palettes, DXT, cubes, profondeur et scènes de jeux correspondantes |
| Géométrie | Descripteurs et empaquetage séparés de l'arène empruntée ; durée de vie des indices/VBO explicite | Dessins immédiats/tableaux/VBO, limites mémoire, scènes de référence et temps/image |
| Programmes | Objets par contexte, compilation et diagnostics ; dépendances de samplers explicites | Scènes ARB/GLSL, changements de contexte et références DOOM 3/Prey/Nexuiz |
| Transport | Soumissions, moitiés, copies différées, erreurs et fermeture derrière une interface de durée de vie | Fermer L1/L2 avant de déplacer les chemins concurrents ; injection de pannes |
| Diagnostic/configuration | Configuration immuable par instance, compteurs et sorties distincts du rendu | Même comportement options activées/désactivées, coût de l'instrumentation mesuré |

Un lot doit conserver ses entrées/sorties observables et ses références
**avant/après**. Ne pas régénérer une référence pour faire passer une extraction.
Le rejeu hôte ne suffit pas à valider une modification du plugin invité :
il consomme déjà les commandes produites. La matrice de jeux complète et les
`frames.csv` restent nécessaires avant de déclarer A2 terminé ou un gain de
performance ; les scènes ciblées protègent ce premier lot limité.
