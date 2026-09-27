# Architecture — propriété et synchronisation

État relu le 27/09/2026 : protocole qgpu v22, ABI de transport v19.
Ce document décrit principalement la chaîne graphique Tiger, où se croisent
mémoire partagée, clients IOKit, vCPU et rendu hôte. Le contrat binaire reste
dans [protocole.md](protocole.md) et les en-têtes qu'il référence.

**Lecture des invariants :** une règle « doit » exprime le contrat à préserver.
Les mécanismes cités décrivent le code actuel. Les écarts identifiés par
**L1…L6** sont des limites actuelles, pas des garanties ni des corrections.
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
Son attente est bornée par `QGPU_SYNC_WAIT_MS` ; cela ne rend pas toutes les
autres attentes bornées (L3). L'attente asynchrone invitée utilise IRQ et timer
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
`switch_half` attend avant réutilisation. **L1 : le chemin timeout de
`wait_half_ex` efface pourtant `h->busy` et `h->npost` sans barrière atteinte.**
La désactivation de l'asynchrone et le marquage ultérieur des contextes cassés
ne prouvent pas l'arrêt du lecteur hôte. M1 n'est donc pas garanti sur ce chemin.
Correction attendue : conserver la zone en quarantaine tant que son achèvement
ou l'arrêt effectif du device n'est pas établi. Épreuve : retarder le backend
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

**L6 : `sync_to_host`, branche couleur, initialise `done = !can`** puis peut
poser `SYNCED` lorsque la copie est impossible (source absente, format non
pris en charge ou arène trop petite). Ne pas déduire une égalité des pixels
du seul drapeau. Il reste à distinguer explicitement « contenu inutile »,
« transfert possible/ordonné » et « transfert impossible », avec épreuves
de format et de taille aux limites.

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
reset ne prouve donc pas que le nettoyage a terminé. Correction attendue :
propager le résultat, conserver un créneau non réattribuable en cas d'échec,
et définir sa récupération. Épreuves : file pleine, reset refusé et reset
accepté mais retardé, puis tentative d'ouverture d'un nouveau client.

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
| Arrêt du kext | Pose `fStopping`, réveille les dormeurs, puis retire IRQ, timer et gate. Les nouveaux appels doivent être refusés. |
| `POMPPCGPUUserClient::stop` | Efface propriétaire et créneau, utilise `forgetSlot` pour la seule comptabilité ; aucun MMIO ni entrée dans une gate potentiellement retirée. Ce chemin n'est pas un nettoyage hôte confirmé. |
| Changement de cible / géométrie | Drainage avant modification du pointeur et du pas lus sans verrou par le rendu ; à l'expiration du drainage, conserve l'ancienne cible, avec possible défaut visuel. |

**R1 — Un reset global termine une époque de barrières.** Les jobs abandonnés
n'avancent jamais leur ancienne barrière. Les compteurs à zéro et l'absence
de numéro d'époque ne permettent pas de reprendre un client vivant comme si
rien n'avait changé. Ses objets et attentes doivent être réinitialisés ; le
reset global n'est pas une réparation transparente d'une seule application.

**L3 — La disponibilité n'est pas bornée partout.** `qgpu_soft_reset` et
`qgpu_stop_thread` utilisent un drainage non borné du job courant. Un backend
bloqué peut donc empêcher reset/arrêt d'aboutir malgré le timeout du doorbell.
Ne jamais libérer de force ses ressources pendant qu'il les utilise encore.

**L4 — Arrêt du kext avec dormeurs restants.** `POMPPCGPU::stop` attend au plus
environ une seconde ; s'il reste des dormeurs, il journalise puis retire quand
même les sources. Le code ne démontre donc pas l'invariant « aucun dormeur au
retrait de la gate » sur ce chemin. Épreuve encore nécessaire : arrêt pendant
`WAIT_FENCE`/`SUBMIT`, y compris dépassement du délai de sortie.

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
| Exécution cœur/backend | `STATUS`, `STATUS_PC`, `ERRORS` | Résultat publié globalement ; les commandes précédemment exécutées ne sont pas annulées. |
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

Évolution attendue : attribution des erreurs au client / à la soumission,
avec une durée de conservation définie. Ne pas déduire une cause précise d'un
simple rapprochement entre une barrière et le dernier STATUS lu.

## 8. Preuves et limites de couverture

| Invariant | Preuve existante / contrôle | Reste à établir |
|---|---|---|
| Kext indépendant des opcodes | `tests/run-all.sh` : ABI identique et absence de symboles sémantiques ; `scripts/qgpu_contract.py --check` | Ces contrôles ne prouvent pas les cycles de vie. |
| Objets et reset de tranche | `tests/qgpu_core_test.c` : bornes, reset, destruction, déliaison des surfaces, conservation d'une autre tranche | Les tests natifs ne traversent ni IOKit ni la file du device. |
| Admission, barrières et transport | `tests/qgpu_smoke.py`, à lancer explicitement | Injection de retard et non-réutilisation après timeout (L1/L2). |
| Cohérence de rendu | `guest/gltest`, tests croisés des backends, matrice et rejeu | Transferts impossibles (L6), scénarios de panne ; rejeu et VM partagent le même moteur. |
| Fin de session | Mécanismes de gate, busy, arrêt et fork relus dans les sources | Mort pendant soumission, double fermeture, réouverture, kext arrêté avec dormeurs (L2/L4), backend bloqué (L3). |

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
