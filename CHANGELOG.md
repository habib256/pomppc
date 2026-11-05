# Changelog

Ce que chaque version publiée de POMPPC a changé, de la plus récente à la plus ancienne
(publications : https://github.com/habib256/pomppc/releases). Le **protocole qgpu**
(`QGPU_PROTO_VERSION`, v22 de la 0.3 à la 0.4.1) a son propre numéro : chaque changement sémantique exige de
reconstruire QEMU et le plugin ; le kext ne change que si l'ABI de transport change. Les mesures
sont celles prises le jour même, sur l'hôte indiqué. Le détail de chaque lot est dans le message
de commit et dans `docs/`.

## 0.4.2 (publiée le 05/11/2026)

**STPATCH abandonné (05/11, M4)** : d3 −2,16 % mais Prey +6,3 % ; code laissé éteint (`docs/plugin-stpatch.md`). La matrice ne mesure plus la rafale d'images vides du démarrage de Prey.

**Deux défauts visuels confirmés corrigés à l'œil le 05/11 sur le M4.** L'ombre de la barre de menus (ciseau du
WindowServer dans le bloc d'état, plugin `20261105-ws-scissor`, [preuve](docs/re/qe-region-state.md)) et la route
humide de Colin McRae (`TXP` dans une copie de l'archive de shaders, [preuve](docs/re/cmr-route-projection.md)).
Le CD invité de la 0.4.2, regravé sur le M4 le 05/11, contient ce plugin ; l'ISO gravée le 04/11 sur le PC est remplacée.

**Contrôle du paquet Windows sur le Mac.** Le script accepte Bash 3.2, livré avec macOS :
conversion des noms de DLL par `tr` plutôt que `${nom,,}` (Bash 4). Les onze témoins positifs
et négatifs passent sur le M4 ; règles du paquet inchangées.

**M4, écart de vitesse de DOOM 3 expliqué le 04/11.** Les 29 ms du 02/11 étaient sans ombres,
les 37 ms du 03/11 avec `r_shadows=1`. Copie du disque du 01/11 sous macOS 27.2 : 29,36 ms ;
sur le disque actuel et le même QEMU reconstruit, seul ce réglage donne 29,22 puis 37,04 ms,
zéro repli. Référence courante : 37 ms avec ombres ; aucun réglage de production modifié.
Le soupçon macOS et l'exclusion initiale des réglages sont retirés
([preuve](docs/chantiers/d3-macos272.md)).

**AppImage Linux x86-64, construite en CI.** `POMPPC-0.4.2-x86_64.AppImage`, plancher glibc 2.31, dans une image
Debian 11 épinglée par digest (`packaging/linux/`, `.github/workflows/release.yml`, `docs/publier-linux.md`). Données dans
`~/POMPPC`, pilote GL de l'hôte, RUNPATH et pas de `LD_LIBRARY_PATH`. L'assistant de premier lancement est porté sous
Linux (`md5sum`, `sha1sum`, `zenity` ou `kdialog`, `xdg-open`, `qemu-img` pour les `.dmg`) ; déposer des fichiers et
partager un dossier restent propres à macOS. Binaire générique, plus lent que le binaire rapide du PC, et le PC reste
derrière le M4 : les notes le chiffrent (générique +7 à +12 % en ms/image, A/B du 04/11). Construite et contrôlée en CI ;
essayée sur le PC depuis le paquet (Tiger, `backend gl`, QE, `gltest`). Hôte ancien, sans NVIDIA, assistant et son à prouver.

**Paquet Windows x64, expérimental, écrit sans machine Windows.** `POMPPC-0.4.2-windows-x64.zip`, MSYS2 UCRT64 en CI
(`packaging/windows/`, job `windows`, `docs/publier-windows.md`). QEMU natif, profil `paquet-windows` ; backend WGL de qgpu (fenêtre
cachée, compatibilité 3.2, repli logiciel) ; frontal sans bash : `LaunchPlan` écrit la ligne de QEMU, paire AF_UNIX et
`get-win32-socket`, `Listener.Win32.Map`, Job Object, et **verrou `.run/tiger.lock` tenu par le frontal**, QEMU ne verrouillant rien
sous Windows. Sans flottant natif, rel32 ni TSC. Le comportement Linux et macOS ne change pas. Compilé et éprouvé sur le runner de CI (Open
Firmware, bus D-Bus, `Win32.Map`, image, verrou). **Rien n'a tourné sur un vrai PC : le rendu GL, Tiger, MTTCG sous Windows, la vitesse
et le son restent à prouver.**

**Endurance du PC avec le bundle du 02/11.** 78 démarrages et 16 cycles de jeu, 0 incident, binaire rapide (`endurance.md` §18).

**PC, PGO réentraîné : neutre, profil du 31/10 gardé.** `pgo-train.sh` refuse un entraînement sans jeu mesuré et recompile avant `use` (`vitesse-doom3-x86.md` §26).

**Quartz Extreme aux résolutions, preuve close sur les deux hôtes.** Sur le PC (EGL/NVIDIA, 03-04/11) : 66 changements de mode sans écart, matrice 8 cellules sur 8, Colin McRae depuis quatre bureaux, A/B du plugin `20261102-modes` sans perte prouvée (`docs/chantiers/qe-resolutions.md`). `POMPPC_GL_PRESENTQ` écarté sur chiffres (`docs/chantiers/presentq.md`).

**Une seule source pour la version** : `project(POMPPC VERSION …)` de `frontend/CMakeLists.txt`, resté à 0.3 jusqu'ici ;
`tools/check_version_strings.sh` et `tools/check_workflow_pins.sh` tournent dans `tests/run-all.sh`.

**Plantage du frontal écran éteint.** Lancé alors que l'écran du Mac est en veille, le frontal plantait dès la première image (0.4.1 touchée) ; la fenêtre suit maintenant l'invité sans borne tant qu'aucun écran n'est connu.

**Preferences.** Son et manette USB s'y règlent aussi, sous le même bouton, et la fenêtre dit la définition
courante et où la changer dans Mac OS. Une Library vide a un bouton **Show Folder**.

## 0.4.1 (03/11/2026)

**Une vraie application macOS.** `POMPPC.app` dans un `.dmg`, à glisser dans Applications, sans
Terminal sur le Mac (`scripts/package_app_macos.sh`). Les données vivent dans `~/POMPPC`. Signature ad hoc,
faute de certificat Apple Developer : macOS demande un accord au premier lancement. Le frontal passe en
anglais ; `run_os9.sh` ne dépend plus de python3 au démarrage. README refondu autour de l'app. Binaires
invités et QEMU de la 0.4, plus `guest-driver` (ci-dessous).

**Un seul assistant, dans la fenêtre.** Système, disque, CD, installation, combo 10.4.11 et pilote
s'enchaînent sans rouvrir l'app, et reprennent si on quitte (`SetupFlow`, `SetupUi`). `-no-reboot` termine
les étapes d'installation quand l'invité redémarre. Le CD vient du média de l'utilisateur, ou de la page de
Macintosh Garden ouverte dans le navigateur, le fichier étant repris dans Téléchargements et contrôlé par MD5 :
l'app ne télécharge pas le système elle-même. Elle télécharge le combo chez Apple, qui le distribue
gratuitement (SHA-1 contrôlé). Installateur **Install POMPPC** à double-cliquer sur le CD de l'invité, sans
Terminal. Propriété QOM `guest-driver` du device (IRQ_MASK armé par le kext, sans changer le protocole) : la fin
de l'étape du pilote et un rappel dans l'app s'y appuient. **OS ▸ Set Up Another System...** ajoute l'autre système.

**Le contrôleur USB ne meurt plus.** `usbhid/0002` : le garde-fou de QEMU contre une liste de descripteurs en
boucle comptait les paquets. Un seul transfert de plus de 32 paquets, ici 2112 octets vers un disque USB sous
Tiger, tuait l'OHCI, clavier et tablette compris. **QEMU change : le PC reconstruit.**

**Un dossier du Mac comme disque.** Panneau *Disks*, *Shared Folder* : le dossier choisi apparaît en disque USB
HFS+ dans Tiger et OS 9. Ce n'est pas un partage en direct : ce que l'invité change revient à l'éjection ou à
l'arrêt, les suppressions vont à la Corbeille, un conflit garde les deux versions, et un arrêt brutal ne
supprime rien (`SharedFolder`). `vvfat` écarté : 503 Mo au plus, fichiers vus vides sous Tiger.

**Échanger des fichiers.** Déposer des fichiers sur la fenêtre branche à chaud un disque USB *From Mac*,
lisible et écrivable par Mac OS sans pilote, repris sur le Mac à l'éjection. Une image de disque se branche en
lecture seule. **Confort.** Police vectorielle Roboto, rappel de sortie du plein écran et de la capture, menu
*Help* (guide, données, journal, problème, mise à jour, à propos), *Preferences* (mémoire, processeurs), image
*Sharp* ou *Smooth*, *Try Without Saving*, capture d'écran.

**Ne plus perdre de travail.** Fermer POMPPC propose d'éteindre le système : **Shut Down** envoie la
touche d'alimentation, Mac OS pose sa question, et POMPPC se ferme quand il s'est arrêté. Son, manette,
changement de système et redémarrage demandent confirmation. QEMU arrêté ou impossible à lancer s'affiche
dans la fenêtre, en quelques secondes, au lieu d'une attente sans fin.

**L'invité d'abord.** Panneaux fermés par défaut, fenêtre à la taille de l'invité et image nette à 100 %,
pomme d'Apple au lieu du texte du noyau dans l'app, titre au nom du système.

**Choix des disques.** Panneau **View ▸ Disks** du frontal, par système : disque de démarrage, jusqu'à deux
autres en même temps (un sous OS 9 avec `disks/games.iso` : 4 emplacements IDE, partagés avec les
CD), création de disques, type HD ou SSD. Tiger n'a pas de notion de SSD : le type pose
`rotation_rate=1` et un modèle lisible, sans changer la vitesse. Les fenêtres ayant changé de nom, la
disposition vit dans `imgui2.ini` et repart une fois par défaut.

## 0.4 (02/11/2026)

Paquet macOS Apple Silicon (`scripts/package_release_macos.sh 0.4`) au commit de la version, construit
pour **macOS 15.0**, sans Homebrew à l'exécution : QEMU, le frontal et leurs bibliothèques sont
recompilés pour cette cible (`scripts/build_release_deps_macos.sh`) et l'empaquetage refuse tout
binaire au-dessus (`MIN_MACOS`). Mis au point sur un M4 sous macOS 27. README : vérification, résolutions,
mise à jour et dépannage. Principaux changements
pour ce paquet : Quartz Extreme allumé et valable à toutes les résolutions, série TCG du PC
allumée sur arm64, plugin allégé, Tiger 10.4.11. Temps du M4 au 02/11 contre la 0.3, en ms/image :
DOOM 3 28,9 contre 48,0, UT2004 19,3 contre 25,3, Zenerchi 2,6 contre 4,3, Colin McRae 41,7 en
1920×1080 contre 50,5 en 1024×768. Dernier tour complet de la matrice : 17 vertes sur 17 le 28/10 ;
cinq jeux le 02/11 au matin, sans le correctif des résolutions du soir (journal de
`docs/quartz-extreme.md`). Détail ci-dessous.

Précision du 04/11 : le relevé DOOM 3 du 02/11 ci-dessus avait `r_shadows=0` ; le réglage
courant `r_shadows=1` donne 37 ms. Voir le diagnostic dans la section 0.4.2.

- **Banc : PID du profil hôte, 05/11 (M4).** Avec un `DISK` explicite, le `pgrep -f` de `matrice.py` et de
  `matab.sh` pouvait retenir le shell parent. `tools/matrice/qemupid.py` lit le nom de l'exécutable,
  et un profil `sample` qui n'est pas celui de QEMU est écarté (`.rejete`). Épreuve réelle : shell parent
  écarté à côté du vrai QEMU, ancien profil de bash refusé, profil de QEMU accepté ; tests natifs dans
  `tests/qemupid_test.py`. `matab.sh` ferme aussi un frontal orphelin, seulement si plus aucun QEMU ne tourne
  (campagne `m4-banc-preuve-0511`, `DISK` explicite : frontal fermé, profil de `qemu-system-ppc`).
- **Ombre de la barre de menus, 05/11 (M4).** Plugin `20261105-ws-scissor` : le bloc d'état perdait
  le ciseau de la région du WindowServer, et l'ombre manquait sous la barre jusqu'au premier changement
  de mode. Comparé à l'oracle scalaire, plugmd5 69 images, cinq jeux verts
  (`docs/re/qe-region-state.md`). Aucun changement de QEMU, de kext ni de protocole.
- **Quartz Extreme à toutes les résolutions, 02/11 soir.** Passer en 1920×1080 donnait un écran
  rayé, et le défaut revenait à chaque démarrage : la VRAM restait mappée à la taille du mode de
  démarrage (kext). En 2560×1080, bureau comprimé à 2048 de large (`GL_MAX_VIEWPORT_DIMS`) et
  fenêtre GL de plus de 2048 blanche (texture d'export, cœur et plugin `20261102-modes`). 44
  changements à chaud justes, 11 définitions en 32, 16 et 8 bits ; `glwin`, Marble Blast et Zenerchi
  justes ; 467 OK. Matrice et A/B non rejoués, PC non essayé (`docs/quartz-extreme.md`, journal).
- **Contrôle général remis au vert, 02/11 matin.** Les 49 scripts en échec
  ShellCheck sont corrigés : retours de `cd`, arguments littéraux, variables
  réellement inutilisées et nettoyages par noms complets. Trois nettoyages
  testés avec espaces, retour à la ligne et tiret initial. Réactivation du
  différentiel softfloat sur l'objet QEMU 11, avec ses règles PPC de NaN :
  1 673 079 cas sans divergence. Harnais isolé `--slow` : **470 OK, zéro échec,
  une exclusion Linux sur le M4** ; QFB, QGPU et pont D-Bus passent.
- **Zone froide arm64 implantée, 02/11 matin.** `tcg/0066` : cinq callbacks,
  relais chauds conservant le chemin rapide, régions bornées en SMP=1/2,
  pools froids séparés et état chaud restauré sur erreur. 53 cas exécutés
  nativement sur le M4, cinq mutants ; tests invités mémoire/FP/vecteurs,
  fautes PC/DAR, tlbb et code réécrit, sorties identiques au témoin ; **2 560
  exceptions FP au PC exact**. Six preuves d'image vertes avec l'attente progressive.
  Matrice des cinq jeux close, aucun gain générique établi : disponible,
  **éteinte par défaut sur le M4** (`tcg-zone-froide-arm64.md` §6–7).
- **Attente courte bornée, 02/11 matin.** `tcg/0067`, `POLLNS=3` : budget
  par fil, retour progressif au poll courant en rafale (250 ms à 2 s),
  initialisation synchronisée ; 1 635 contrôles et neuf mutants. Matrice finale
  à trois passages et témoin identique, **60 cellules sans anomalie/repli** :
  attente seule zen −9,08 %, UT +1,01 % ; avec froid zen −10,06 %, UT +0,23 %,
  d3 +1,09 %, mb −0,76 %, cmr +0,65 %. La grande perte UT du mode 2 est retirée,
  aucun gain générique revendiqué ; **éteinte par défaut**. Trois prototypes
  rejetés archivés ; détails et limites dans `re/ut-attente-m4.md` §5–7.

- **Restauration : la VM survit à son lanceur, 02/11.** `matab.sh` utilise une
  vraie session détachée ; sur macOS sans `setsid(1)`, `host_detach` passe par
  `os.setsid` et `exec`, car `nohup` gardait le groupe parent. Deux tests échouent
  avant et passent après, restauration ImGuiDock/SSH vérifiée (`endurance.md` §17).
- **M4 : endurance finale, 02/11.** Binaire courant : 50 froids, puis 5 démarrages
  et quatre jeux, aucun incident. Nuit entière : 70 démarrages de contrôle et
  huit jeux sains, bases vérifiées (`endurance.md` §16–17).
- **M4 : preuve préparatoire des sauts longs de la zone froide, 02/11.** 36 portes
  exécutées sur le M4 avec les relocations réelles de QEMU, destinations à ±48/96 Mio,
  deux mutants de condition/largeur détectés. Revue : couvrir aussi FP/tlbb, la taille
  des régions en SMP=1 et les pools littéraux. Portage encore à implanter et mesurer,
  aucun allumage (`tcg-zone-froide-arm64.md`).
- **M4 : l'attente d'UT identifiée dans `delay_idle`, 02/11.** Mappage des piles arm64
  vers les instructions invitées, puis registre LR à `mach_absolute_time` : boucle
  d'attente active SMP du noyau xnu-792.24.17, source Apple concordante. Pas de raccourci
  invité ajouté ; identité du noyau à vérifier sur le PC (`re/ut-attente-m4.md`).
  `samplejit.py` conserve les ambiguïtés, six tests et deux mutants détectés.
- **M4 : variante d'attente sous la milliseconde mesurée et écartée, 02/11.** `tcg/0065`,
  `POLLNS=2` : `select` uniquement pour 0 < délai < 1 ms ; le mode 1 reste entier,
  aucun allumage. 56 contrôles du contrat/routage, deux mutants de frontière détectés ;
  A/B neuf des cinq jeux : zen −8,20 %, **UT +10,06 %**, autres écarts inconclusifs,
  zéro incident/repli. `archive/m4-pollns-court-0211`, `tcg-g4.md` §42.2.
- **Matrice : rapports A/B sans arrondi prématuré, 02/11.** Le CSV garde le temps précis
  en plus de l'affichage à 0,1 ms ; le bilan utilise cette précision, anciennes archives
  acceptées. Évite de prendre zen 2,7→2,6 pour −3,7 % quand le gain réel est −1,78 %.
  Tests hors VM : rapport, ancien format, facteur échanges/image et fenêtre absente.
- **M4 : profils croisés des cinq jeux renouvelés, 02/11.** d3 : rendu hôte 0,178 cœur,
  82,2 % d'attente de condition ; la piste de conversion d'attributs n'est pas prioritaire
  sur cette mesure. Recherche de blocs 4,8–8,1 % des vCPU actifs (`profil-m4-0211.md` §2).
- **M4 : PACKBITS mesuré au repos sur les cinq jeux, 02/11.** A/B neuf à trois bras,
  trois parties par bras, mb aussi en plein écran : UT −3,19 %, Colin en course −7,08 % ;
  autres écarts sous 3 %, inconclusifs. Aucun incident ni repli, aucun réglage changé.
  Le bras non-simple ne compte pas les avis actifs ; ne pas l'assimiler à une mesure de
  leur coût sous une fenêtre posée sur le jeu (`docs/profil-m4-0211.md` §1).
- **M4 : contrat du chemin d'attente expérimental corrigé, 01/11 nuit.** `tcg/fixes/0058`
  garde le compte de `select` et préserve `revents` au retour nul/erreur, comme `g_poll`
  de l'hôte. Reproduction de quatre écarts avant, 42 cas sans écart après, deux mutants
  détectés (`tools/tcg/pollnsproof.*`, `docs/tcg-g4.md` §42.1). Aucun allumage.
- **Endurance : Ctrl-C pendant les lancements ferme les VM, 01/11 nuit.** L'attente
  d'échelonnement était hors du gestionnaire d'interruption ; une VM restait vivante.
  Lancements et nettoyage protégés, attente seulement entre deux départs, travailleurs
  démarrés joints. Quatre tests et une interruption réelle à deux instances prévues,
  zéro QEMU résiduel (`docs/endurance.md` §16).
- **M4 : six preuves d'image renouvelées, 01/11 nuit.** Après constat de l'absence des
  artefacts locaux, références neuves regardées et validées : mb/zen/d3/ut en fenêtre,
  mb/cmr en plein écran. Six cellules vertes, zéro repli ; cmr en course en 1920×1080 sans
  le faux reflet. Ombre de DOOM 3 avec la correction active confirmée par l'utilisateur.
  Temps sous activité externe, non retenus comme A/B (`docs/etat-des-hotes.md`).
- **Profils du plugin : symboles indirects Mach-O, 01/11.** `machonm.py --stubs` lit les
  importations du bundle ppc (82 correspondances avec `otool -Iv`, trois tests hors VM).
  Relevé du M4 : `___stub_getrealaddr` est un résolveur du stub de `fprintf`, suivi des stubs
  d'importation ; le bloc libSystem `0x90131200` est dans `memcmp`/`bcmp`. Les appelants et
  les adresses du profil PC restent à qualifier (`docs/re/plugin-symboles-tiger.md`).
- **Profils M4 : coûts propres par fil, 01/11.** `threadbusy.py --fonctions N` sépare les
  fonctions actives des attentes noyau, notamment sur `qgpu-render`. Corrige aussi le
  dénominateur lorsqu'un fil apparaît dans plusieurs blocs et la dernière pile sans pied de
  page. Quatre tests hors VM ; les profils ne sont pas assimilés à un relevé de temps GPU.
- **`tcg/0064` x-tb-head fusionné, 01/11 (PC).** Tête de bloc allégée (`can_do_io` déduit de
  l'adresse de retour, `goto_tb` vers l'avant chaîné après la vérification d'interruption ;
  `docs/anatomie-ppc32.md`, outils `tools/tcg/codeanat32.py`, `tbhproof.*`), écrit le 23-24/10
  sur une branche sous le numéro 0051, renuméroté 0064 à la fusion. Propriété de l'accélérateur,
  **éteinte par défaut** (`TBHEAD=1|2|3`, `7` = preuve) ; l'empreinte de la série change : binaires
  à reconstruire (M4 fait le 01/11, `~/src/qemu/build` sur `a0e028e` ; PC à faire). Mesuré sur banc (−5,0 / −3,1 % d'instructions, −2 % de
  cycles), pas encore en jeu.
- **Quartz Extreme : plus d'image ancienne du jeu sous l'ombre d'une fenêtre posée sur un jeu
  en fenêtre, 01/11 (PC).** Sur le chemin direct, le jeu présentait par le noyau sans aviser le
  WindowServer, qui ne recomposait la zone d'ombre qu'à ses propres dommages. Kext : méthode 6
  (`POMPPC_GA_PRESENT2`, dit si la surface est « non simple ») ; plugin `20261101-ombre` : avis
  cadencé (`POMPPC_GL_QE_NONSIMPLE_HZ`, 20) par le fil des avis, qui garde le dernier en
  attente. Protocole qgpu et QEMU inchangés ; **le kext change : réinstaller et redémarrer
  l'invité sur les deux hôtes** (fait le 01/11 sur le PC et sur le M4). Coût seulement sous une fenêtre posée : mb-fen +6,2 %, d3
  +0,9 % (`4033e94`). `docs/quartz-extreme.md`, journal du 01/11.
- **Frontal ImGuiDock : fin du double curseur, 01/11 (PC).** Le curseur de l'hôte est masqué
  au-dessus de l'image de l'invité en souris absolue (Tiger dessine le sien dans le framebuffer) ;
  il revient hors de l'image, en pause et VM arrêtée (`wantsHiddenHostCursor`, `PointerPolicy.h`,
  testé dans `pointer_test`). Signalé par l'utilisateur ; à voir à l'œil au prochain `run_tiger.sh`.
- **`pgo-train.sh` : cible `ppc32mt` par défaut, 01/11 (PC, `cf6cf61`).** C'est la cible de
  production des deux hôtes ; le bras `ppc64` pose `PPC32MT=0` (avant, le défaut « ppc64 »
  héritait du `PPC32MT=1` de `run_tiger.sh`). Les profils déjà construits ne changent pas.
  `docs/binaire-rapide-x86.md` §7.
- **Plugin `20261101-packbits` : `POMPPC_GL_PACKBITS` allumé, 01/11 (PC ; ajouté au journal le
  01/11 d'après `fd5990d`).** Bornage des flottants sur leurs bits, octets non signés par table ;
  plugmd5 69 scènes 0 écart ; A/B au repos d3 −3,9 %, ut −4,5 %, zen −4,2 %, mb −1,2 %, cmr en
  course −3,5 %. Code de l'invité : sur le M4 depuis le 01/11 (plugin `20261101-ombre`). `docs/plugin-allegement.md` §8.
- **Matrice : Colin McRae mesuré en course, 31/10 (PC, `6054367`).** Fenêtre de mesure à
  ≥ 1 700 sommets bruts par échange, comptés dans l'invité : depuis le 28/10 le PC mesurait la
  transition d'avant la course (44-48 ms/image) au lieu de la course (82,1 ms/image) ;
  `plancher_ms_linux` 105. `docs/vitesse-doom3-x86.md` §25.
- **qgpu : plus de faux reflet sur la carrosserie de Colin McRae, 31/10 (PC, `928a5f0`).** Sous
  programme de fragments, une unité éteinte par le plugin n'a plus de texture
  (`unit_texture_bound`, `qgpu-core.c`) : QEMU change sur les deux hôtes, pas le protocole.
  Nouvelle référence cmr-pe du PC, validée le 01/11. `docs/re/cmr-rendu-vers-texture.md` §7.
- **Zone froide (`tcg/0056`, `x-tcg-cold`) allumée sur le PC x86-64, 31/10 (`80aae4e`).** 6
  parties par bras sans gel de cmr ; A/B d3 −1,8 %, mb −1,2 %, zen −2,2 %, cmr −2,2 %, ut neutre ;
  endurance §15 : 10 démarrages et 8 cycles de jeu, 0 incident. Sans effet sur arm64 (saut long
  à écrire). `docs/vitesse-doom3-x86.md` §23.
- **vperm en PSHUFB, 30/10 (PC).** `tcg/0059` (`VPERMINLINE=1`, `VPERMVERIFY=1`) traduit
  `vperm` en deux `PSHUFB`. Exact : 2,56 milliards vérifiés, 0 divergence. Neutre en vitesse
  (d3 −0,1 %, mb +2,4 %, ut +0,3 %) : éteint par défaut. `docs/tcg-g4.md` §43.

- **Quartz Extreme allumé sur le PC x86-64, 30/10.** Parité avec le M4 : ombre juste sur
  Marble Blast et DOOM 3, matrice et A/B au repos. Deux lenteurs propres au PC ont été
  corrigées. Le **kext** présente en asynchrone la région moins un curseur immobile, au lieu
  de tout présenter en synchrone (Marble Blast plein écran 15,2 → 8,6 ms, −7,5 % contre sans
  QE). Le **plugin** ne relit plus, et ne réassigne plus, une fenêtre cachée qu'on rattache
  à l'identique (Colin McRae 59,0 → 44,5 ms). Endurance : 48 démarrages et 16 cycles de jeu
  sans incident. `run_tiger.sh` pose `qe=on` sur Linux x86-64 ; `QE=0` l'éteint. Le plugin
  GA est construit par le job `prebuilt`, pour les VM sans gcc. `docs/quartz-extreme.md`,
  `docs/endurance.md` §14.

- **Quartz Extreme, ombre sur une fenêtre GL, 30/10 (M4).** Sous l'ombre d'une fenêtre posée
  sur un jeu en fenêtre, le compositeur montrait du blanc (Zenerchi), du noir (Marble
  Blast) ou l'image d'un ancien jeu (DOOM 3). Il liait sa propre texture de surface, vide,
  au lieu de la texture d'export du jeu sur le chemin rapide des clés d'état. Toutes les
  liaisons passent maintenant par `tex_bind_id`, et le jeu apparaît sous l'ombre avec son
  dégradé (une seconde cause, l'ancien jeu sous DOOM 3, est corrigée le 01/11 : entrée plus
  haut). Les coins noirs des fenêtres de DOOM 3 et Prey viennent du jeu lui-même
  (`NSBackingStoreRetained`, sans alpha), comme sur un vrai Mac. `docs/quartz-extreme.md`.

- **Émulateurs voisins, 30/10 (M4).** `tcg/0057` (`VCPUQOS=1`, fils des vCPU en QoS
  interactive) neutre au repos ; `tcg/0058` (`POLLNS=1`, attente à la microseconde) :
  Zenerchi −18 %, UT2004 +6,6 %. Les deux restent éteints. `docs/tcg-g4.md` §42.

- **Quartz Extreme : curseur visible sur une fenêtre GL, 29/10 (M4, `71f9f76`).** Le kext retire
  le curseur (`hideCursor`), présente en synchrone, puis le remet, au lieu de renvoyer au
  WindowServer (dont l'avis peignait en blanc une fenêtre qui n'est pas au premier plan).
  Matrice mb/zen/d3 en fenêtre vertes. `docs/quartz-extreme.md`.
- **Quartz Extreme allumé par défaut sur le M4, 29/10 (`c792464`).** `QGPU_CAP_QE_DEFAULT`
  (propriété `qe` du device, posée par `run_tiger.sh` ; `QE=0` l'éteint), kext en personnalité
  « auto » (`install.sh` sans `QE`). Le jeu présente son échange par le noyau (méthode 5).
  Endurance §12-§13 sans incident. `docs/quartz-extreme.md`.
- **M4, 28-29/10 : vidage par bandes, matrice 17/17, endurance de nuit.** `TEX_READBACK` à fenêtre
  de lignes (`QGPU_CAP_TEX_READBACK_ROWS`, `403593e`, sans changer `QGPU_PROTO_VERSION`) : cmr-pe en
  1920×1080 vert ; matrice entière `20261028-1854` 17/17 vertes (`17903f5`) ; nuit d'endurance :
  342 démarrages et 40 cycles de jeu, aucun incident (`docs/endurance.md` §11).
- **PC, 28/10 : endurance et Colin McRae.** Nuit du 27-28/10 : 683 démarrages (400 à froid, 283
  en campagne `reboot` dont 281 reboots), 0 incident (`9d1c921`, `docs/endurance.md` §10). Colin McRae ne quitte plus en entrant
  en course : qgpu réécrit les `ALIAS` des programmes ARB, refusés par NVIDIA (`5c2ea82`).
- **Auto-profileur sans `perf`, 28/10 (PC, `c1105f7`).** `patches/diag`, `tools/tcg/selfprof.py`,
  `POMPPC_SELFPROF` dans la matrice : part du code généré, de l'attente mémoire et des aides par
  jeu. `docs/vitesse-doom3-x86.md` §18.

- **Profils CPU OS 9 expérimentaux, 27/10.** `OS9_PROFILE=memoire|flottant|complet|verifie`
  reprend les optimisations CPU de Tiger, un seul cœur, périphériques classiques.
  Sept passages DOOM II : écran fixe exact et PCM présent ; profil vérifié : 1,06 milliard
  d'opérations flottantes et 3,27 milliards de chargements, aucune divergence. Barrack
  contrôlé en partie sur témoin et complet. Défaut `g4` conservé ; AltiVec et gains en jeu
  à compléter. `docs/os9-optimisations-cpu.md` ; suite du dépôt : 287 OK, 0 échec.

- **Endurance M4, 27/10.** Nouveaux défauts sur Tiger 10.4.11 isolé : **78 démarrages
  sains** (24 froids, 54 reboots), **16 cycles de jeux sains**, quatre par jeu. Aucun
  incident ; la nuit plus longue a été jouée le 28-29/10 (entrée plus haut). Compteur QEMU du banc corrigé sur macOS
  (`ps ucomm`, au lieu du chemin tronqué de `comm`). Base vérifiée avant/après, VM
  quotidienne restaurée dans ImGuiDock. `docs/endurance.md` §9.

- **Traces expérimentales sur le M4, preuves du 26/10.** Banc nu et mutants portés sur
  macOS ; `split-wx=on` choisi quand `x-tier2` est demandé, pour permettre les écritures
  de ses compteurs. `tcg/0055` corrige le vérificateur : rejeu du nombre exact d'instructions
  exécutées et comparaison des seules données écrites par les stores étroits. Marble Blast
  passe de 3 349 divergences à zéro (32,6 M traces vérifiées), image exacte. DOOM 3 :
  293,5 M contrôles sans divergence et image synchronisée du niveau exacte. A/B M4
  entrelacé 3×3 : **traces +5,74 %**, 33,929 contre 32,086 ms/image au défaut ; elles
  restent éteintes. Split-wx seul neutre, défaut mémoire conservé. Preuves natives PC
  de 0055 jouées le 27/10 (`c92dacc` : t2proof conforme, Tiger et Marble Blast à 0 divergence). `docs/plan-traducteur-rapide.md` §9.8–9.9 ; suite du dépôt :
  283 OK, 0 échec.

- **UT2004 sur le M4 : éclairage des spots calculé par le device, 26/10 (`5f25498`).** Sur le GL
  d'Apple (Metal), une lumière spot allumée retirait aux points hors de son cône l'éclairage des
  autres lumières (cargaison et objets noirs). Pour un dessin fixe éclairé portant un spot,
  `qgpu-gl.c` calcule les couleurs par sommet (`qgpu-light.h`, l'éclairage du backend soft ;
  `QGPU_GL_CPULIGHT=0/1`) ; rejeu de la démo : 62 images faussées sur 445 avant, 24 après (écarts
  de filtrage). QEMU change, pas le protocole. `docs/re/ut2004-arme-noire.md`.
- **ImGuiDock démarre toujours en fenêtre, 26/10 (`d91581a`).** Le plein écran de la session
  précédente n'est plus rétabli ; Alt+Entrée bascule fenêtre ↔ plein écran comme F11 et
  Ctrl+Cmd+F, sans être transmis à l'invité. `matab.sh --restore` relance la VM dans ImGuiDock.
  `frontend/README.md`.
- **Parité M4 : la série du PC allumée sur Darwin:arm64, 26/10 (`550b2a8`).** `PPC32MT TLBPRECISE
  LMWINLINE DCBZINLINE FPNATIVECMP VFPNATIVECMP DIRTYSKIP PTEGMAP JC2WAY FPKNOWN FPRFLAZY STFSKNOWN
  LFSKNOWN TLBBASE` ; A/B 6 parties par bras : DOOM 3 −23,9 %, UT2004 −18,6 %, Marble Blast
  −22,7 % ; laissés éteints sur un binaire sans `tcg/0037`-`0039`. `docs/parite-m4-2510.md`.
- **PC x86-64, 20-25/10 : leviers TCG allumés par défaut** (`run_tiger.sh`, `docs/tcg-g4.md`
  §36-§41, `docs/ppc32-mttcg.md`). 20/10 (`b8484d5`) : `tcg/0040`, `0041`, `0043` (DOOM 3
  −4,4 %) et `0045`-`0047` (−6,3 %). 23/10 (`623893c`) : `PPC32MT` (`tcg/0049`-`0050`, G4 en
  MTTCG sur la cible 32 bits : d3 −7,7 %, ut −5,1 %, mb −7 %, zen −6 %). 24/10 (`4b5e3e8`) :
  `LFSKNOWN` (`tcg/0051`, d3 −9,5 %, ut −2 %). 25/10 (`c3d0c1a`) : `TLBBASE` (`tcg/0052`, d3
  −2,2 %) ; `tcg/0053` neutre, `archive/0053-vfp-known`.
- **Plugin de l'invité, 20-23/10 : leviers d'allègement allumés** (`docs/plugin-allegement.md`,
  `docs/d3-plugin-x86.md` §10). 20/10 : `ZONES` (d3 −6,1 %). 21/10 (`3ae61df`) : `DISPFAST`,
  `PROGMEMO`, `MTXDIRTY`, `STUNIT`, `DISPLOCK` (d3 −3,8 %, mb −5,6 %). 22/10 (`48f6fa4`,
  `20261022-abon`) : `BEGINVD`, `LIGHTDIRTY`, `WINSEED`, `TEXDEFER`, `DISPSEED`, `SWAPLITE`
  (`PRESENTQ` éteint). 23/10 (`94f89ef`, `20261023-uvwon`) : `UVWIDE` (ut −1,6 %, d3 −1,4 %).

- **Collecte d'endurance : RAM physique conservée, 20/10.** `ram.bin` est écrit dès l'arrêt
  de la VM, avant les diagnostics et tout redémarrage, pour permettre l'analyse du savearea
  et du tas des paniques rares (§8.4 de `docs/endurance.md`). Plage issue de `info mtree`,
  espace libre et taille écrite vérifiés, échec documenté sans interrompre les diagnostics.
  Test de transfert réel sur QEMU mac99 sans Tiger, avec motif mémoire relu et cas d'erreur.
  Décodage du savearea et reproduction de la panique encore à faire.

- **`tex14` expliqué, PC, 14/10 : le job `gpu` tournait sur llvmpipe.** Les deux exécutions en
  échec (55/56) ont `QGPUCaps` = 720894, sans `QGPU_CAP_FIXED4` (0x40000) que le backend gl pose
  sur NVIDIA : leur VM était partie sans `EGL_PLATFORM=device`, que `devloop.py` ne posait pas,
  et Mesa llvmpipe rendait. Mesa mesure la distance d'atténuation des points sur **|z œil|**
  (point à z = 0 : taille bornée à `POINT_SIZE_MAX` = 64), d'où « dehors (+5) » allumé ;
  reproduit à volonté (VM sans `EGL_PLATFORM` : même échec, mêmes caps). L'essai
  `EGL_PLATFORM=x11` n'avait pas tourné (frappe de `start` perdue, `TIMEOUT`, `last` périmé).
  Sur la RTX (pilote 595.99) : `tex14` juste, job complet **56/56**. `devloop.py` pose
  désormais `EGL_PLATFORM=device` sous Linux, dit sur quel GL rend qgpu (⚠ sinon), attend une
  vraie console avant de taper, et signale `last` périmé après un `TIMEOUT`
  (`docs/gpu-3d-tiger.md` §5.1). Device et plugin inchangés.
- **Paquet invité `20261014` (main dcac35f), PC, 14/10 06:28** : `disks/prebuilt` régénéré dans
  la VM de dev (ancien en `bench/devloop/prebuilt-20261007c`) — plugin `20261014-r5` (pagination
  des textures), kext POMPPCGPU A6 L2 (ABI inchangée), gltest ; ISO
  `disks/pomppc-guest-20261014.iso` avec en plus le StartupItem `POMPPCSshGarde`. Job `gpu` : 55/56 au premier passage (`tex14`), qui tournait en fait sur
  llvmpipe (entrée ci-dessus) ; sur la RTX, **56/56** (job 1791947001974). **Installé** dans la VM quotidienne le 14/10 à 07:50
  (sauvegarde `~/pomppc-sauvegarde-20261014` dans l'invité, redémarré) : kexts chargés, caps
  983038, ssh à l'écoute ; matrice `20261014-0752` mb et ut en fenêtre vertes, images justes
  (16,4 et 50,3 ms/image).
- **PC, 13-14/10 : gardien ssh du paquet invité, plancher d'UT2004.** `POMPPCSshGarde`
  (StartupItem) recharge `ssh.plist` quand launchd l'a chargé sans socket (lo0 sans IPv6 au
  démarrage, `docs/endurance.md` §8.3-8.5) ; défaut reproduit à la demande et réparé dans la
  VM. Panique #76 relue (pendant un relevé série ; kalloc.16 corrompue ; `kpanic.py` lit le
  format de chaque CPU). Plancher Linux d'UT2004 104 → 63 (3 + 3 parties du binaire rapide sur
  la RTX, fenêtre 49,6, plein écran 47,4 ms/image, `docs/matrice-jeux.md` §6 quater). Backend gl
  de qgpu : sous la session Wayland d'Ubuntu 26.04 il tombait sur llvmpipe ; `EGL_PLATFORM=device`
  dans `run_tiger.sh` et `matrice.py` lui rend la RTX.
- **GPU hôte bloqué à la demande, et A6 (L2) fermée, PC, 14/10** (`docs/epreuve-gpu-bloque.md`).
  Device : propriétés de test `x-test-stall-ms` (le thread de rendu dort avant chaque job,
  réglable à chaud par `qom-set`, relu toutes les 10 ms) et `x-test-refuse-reset` (CLIENT_RESET
  refusé par `QUEUE_FULL`), éteintes par défaut et sans coût ; correctif trouvé par l'épreuve :
  une géométrie qfb abandonnée faute de drainage (device encore sain, `system_reset` avec un job
  bloqué) était perdue pour de bon, elle est retentée au prochain doorbell et par le reset qui
  aboutit. Kext (ABI inchangée) : `destroyClientObjects` rend son verdict, une tranche au
  nettoyage échoué reste « sale » et `allocSlot` ne la redonne qu'après avoir relancé un
  CLIENT_RESET jamais parti et vu sa barrière atteinte ; `resetSlot` rend l'échec. Épreuves en
  VM (copie de la VM de dev, QEMU construit dans `~/src/qemu-stall`) : GL3, KT4, K6 et KT5
  tenus (reset borné à 5 s, device « cassé » puis réparé par `kextload` ou après
  `system_reset`, mode d'écran changé pendant la panne sans BQL tenu, fenêtre qfb reliée à la
  réparation, `quit` bloqué en 5,06 s) ; A6 : client tué avec ses soumissions en file, 4
  `QGPU_ST_LIMIT` sur 5 scénarios avec le kext d'avant, 0 sur 5 avec le nouveau (autre tranche
  ou attente, journal qui nomme la tranche sale puis sa libération). Outils : `slotpeer`,
  `qfbmode` (guest/qgpu-test), job `stall`, `tools/guest/epreuve_gpu_bloque.py`.
  `tests/qgpu_smoke.py` vert sur le nouveau binaire. **À faire :** reconstruire les binaires de
  référence (PC et M4) et installer le kext dans les VM. (01/11 : fait, QEMU et kext reconstruits
  depuis sur les deux hôtes : M4 le 29/10, PC le 31/10 et le 01/11.)

- **Paquet invité `20261007c` (main 97fad0a) et revue de DOOM 3 sous programme ARB, PC, 07/10
  09:05** : job `gpu` **56 OK, 0 échec** (`vpimm` 44/44, `pixstore`, `matbegin`, `matsonde`) ;
  `disks/prebuilt` régénéré (ancien en `bench/devloop/prebuilt-20261007b`), paquet
  `disks/pomppc-guest-20261007c.iso` installé dans la VM quotidienne au repos (sauvegarde
  `~/pomppc-sauvegarde-20261007c`, invité redémarré) : `caps 0xefffe`, les quatre scènes vertes
  en session. Tour `bench/matrice/20261007-vpimm2` (`-j d3,ut -m fen`, avec vidage) : images
  justes (DOOM 3 rejeu = VM 0,04, référence 0,00 encore à valider ; UT2004 0,00 / 0,00), 15 et 3
  replis, pas de plantage. Vitesse hors norme (232,7 et 167,0 ms/image) sous trois VM
  d'endurance d'un autre agent : un A/B UT2004 sans vidage sous la même charge (défaut 60,2 puis
  101,5 ; `POMPPC_GL_VPIMM=0 POMPPC_GL_TCLLOST=0` 56,4) l'attribue à la charge, pas au plugin.
- **Paquet invité refait après le correctif `glPixelStore` (77f9092), PC, 07/10 08:25**
  (`docs/matrice-jeux.md` §6 quater) : sur une nouvelle copie de la VM de dev, binaire de
  référence reconstruit le matin, job `gpu` **54 OK, 0 échec** (`pixstore` et `vpimm` compris,
  `qgpu_test` 0 échec, caps 0xefffe) ; job `prebuilt` → `disks/prebuilt` (ancien en
  `bench/devloop/prebuilt-20261007-0709`), paquet `disks/pomppc-guest-20261007b.iso`. Installé
  dans la VM quotidienne, au repos (aucune application, ~49 min sans entrée) : ancien plugin dans
  `~/pomppc-sauvegarde-20261007b`, `install.sh`, invité redémarré ; kext `caps 0xefffe`, plugin
  chargé (chaîne `20261004-d3x`, révision laissée en attente de l'utilisateur), `gltest pixstore`
  et `gltest vpimm` verts dans la session.
- **Mode immédiat sous programme de sommets : normale, couleur secondaire, brouillard,
  génériques, 07/10** (`docs/protocole-v16-programmes.md` §7 bis) : `glNormal`,
  `glSecondaryColor`, `glFogCoord` et `glVertexAttrib4fARB` entre `glBegin` et `glEnd` sous
  programme ARB sortaient à la valeur courante de `glBegin`, et un générique de tableau
  désactivé à la valeur de l'hôte : `geom_format` ne demandait ces attributs que pour l'état
  fixe (éclairage, `COLOR_SUM`, `GL_FOG_COORDINATE`) ou un tableau actif. Texte précis : ce
  qu'il lit est porté, les génériques seulement s'ils sont écrits `vertex.attrib[k]` (masque
  `vp_attr`). Plugin seul, sans changement de protocole. Scène `vpimm` étendue (f)-(m) : 9
  témoins faux sur 18 avant, 44/44 et écart 0 à Apple après ; `POMPPC_GL_VPIMM=0` refait les
  9 (A/B des jeux). Job `gpu` 54 OK, 0 échec ; `tests/run-all.sh` 239 OK, 0 échec. Revue de
  DOOM 3 et Colin McRae en jeu : reste au TODO.
- **`glMaterial` entre `glBegin` et `glEnd` : le « T&L perdu » de GLEngine honoré, 07/10**
  (`docs/re/opengl-1.4.md` §3.3) : `_glMaterial*_Exec` appelle `_gleForceToSoftwareTCL`, qui
  pose `0x10000000` dans le mot `+0x0c` du bloc de changements puis appelle le dispatch ; le
  plugin rendait toujours « T&L gardé », GLEngine relançait notre `BeginPrimitiveBuffer` avec
  son curseur dans son propre tampon (primitive perdue) et l'objet matériau de `gctx` ne bougeait
  plus (primitives suivantes à l'ancien matériau, même après un `glMaterial` hors `glBegin`).
  Le dispatch qui porte ce bit rend désormais le pilote sans T&L, au pipeline fixe seulement
  (pas sous programme : `_gleBuildInterpolateFunc` → `exit`) : la primitive est éclairée par
  GLEngine et rastérisée par le chemin hérité ; le dispatch suivant rend le T&L.
  `POMPPC_GL_TCLLOST=0` pour l'A/B. Nouvelle scène `gltest matsonde` (matériau avant le premier
  sommet, au milieu d'une liste, primitive suivante, hors `glBegin`). Job `gpu` **56 OK, 0 échec**
  (`matbegin` et `matsonde` ajoutées, `pixstore` comprise) ; `gtgeo.sh` : `matbegin` identique
  à l'octet au rendu d'Apple dans les quatre modes, toutes les autres scènes au même md5
  qu'avant ; `POMPPC_GL_RDIRTY=0` : `tcprobe` passe aussi. `tests/run-all.sh` 239 OK, 0 échec,
  7 ignorés (aucun code hôte touché).
- **Endurance : console série et cause des « pas de ssh », PC, 07/10** (`docs/endurance.md` §8) :
  second canal vers l'invité par un UART 16550 PCI (`-device pci-serial,addr=0x12`,
  Apple16X50Serial, getty sur `tty.pci-serial18` dans la nouvelle base
  `disks/tiger-endurance-serie.qcow2`), `tools/endurance/serie.py`, `endurance.py --serie`
  (relevé par la console avant tout reset, même relevé par ssh après `system_reset`,
  `--releve-sain`). Campagne `pc-reboot-serie` (168 démarrages, binaire rapide du 07/10) :
  1 « pas de ssh » reproduit et lu — lo0 sans IPv6 (`in6_ifattach_loopback` en échec),
  `launchctl` échoue au `bind()` IPv6 de `ssh.plist` et n'ouvre pas non plus l'IPv4 ; course
  dans l'invité, pas QEMU — et 1 panique au démarrage (§8.4), non analysée.

- **Binaires, paquet invité et matrice après la nuit du 06-07/10, PC, 07/10 au matin**
  (`docs/binaire-rapide-x86.md` §6, `docs/matrice-jeux.md` §6 quater) :
  `disks/prebuilt` régénéré par le job `prebuilt` sur une copie de la VM de dev (plugin de la
  nuit : erreurs par client, `vpimm`, garde `unpack_trusted` ; `gltest` à 256×256 et `vpimm` ;
  kext POMPPCGPU recompilé, octets identiques ; ancien en `bench/devloop/prebuilt-20261004`) ;
  job `gpu` avant reconstruction 52 OK, 1 échec (`vpimm`, 7 témoins : device du 06/10, attendu).
  Paquet `disks/pomppc-guest-20261007.iso` (`scripts/make_guest_iso.sh`) installé dans la VM
  quotidienne par son `install.sh` (ancien plugin en `~/pomppc-sauvegarde-20261007`) : kext
  `caps 0xefffe`, plugin chargé (`plugin 20261004-d3x qgpu v22` : la chaîne de révision n'a pas
  été montée cette nuit), `gltest vpimm` 26/26 dans la session. QEMU reconstruit
  (`QEMU_FAST=1`, `build/` et `build-fast/`, anciens en `*.avant-0707`) : série
  `3d7534b0f2217d54`, le lanceur reprend le binaire rapide ; `tests/run-all.sh` 239 OK,
  1 échec (binaire rapide écarté), 5 ignorés → **240 OK, 0 échec, 5 ignorés** (deux tests du
  frontend désormais construits). Tour `bench/matrice/20261007-0709` avec vidage : Marble Blast
  15,5 / 15,2, Zenerchi 7,4, UT2004 48,8 / 48,5 vertes, images justes ; DOOM 3 123,4 / 121,5
  rouge faute de référence validée (`d3-fen`, et `d3-pe` créée par ce tour dans
  `references-linux.csv`, `validee=non`), images rejeu = VM (0,03 et 0,04). Vitesse peu probante
  (un QEMU d'un autre agent sur l'hôte) ; `POMPPC_GL_VPIMM=0` ne change pas DOOM 3 (118,6
  contre 119,4 sans vidage).
- **État de `glPixelStorei` : la base était fausse, pas les offsets ; garde remplacée par une
  vérification, 07/10** (`docs/re/pixelstore.md`, `docs/backend-gl-unites-fixes.md`, « Cause :
  la mauvaise base ») : le plugin lisait `CTX_UNPACK_*` (`+0x31cc..0x31e5`) sur le contexte du
  GLDriver (un bloc de 0x800 octets) au lieu du contexte de GLEngine (`gctx = GS − 0x360`) :
  lecture ~10 Kio hors du bloc, d'où les 0 de 10.4.11, le 4 du 22/09 et le plantage de
  `try_draw_pixels` dans la session bureau. Relevé par diff de vidages (scène `gltest
  pixsonde`) : `GC_PACK_*` en `gctx+0x31b0..0x31c9`, `GC_UNPACK_*` en `gctx+0x31cc..0x31e5`,
  GLEngine identique en 10.4.6 et 10.4.11. `pixstore_read()` remplace `unpack_trusted()`
  (cohérence seulement, compteur `POMPPC_GL_STATS`, 0 relevé) ; restrictions aux lignes
  serrées levées pour `DrawPixels`, `Bitmap` et stencil, `ReadPixels` suit `GL_PACK_*` ;
  couleur raster de `glBitmap` lue sur `gctx`. Nouvelle scène `pixstore` (64 témoins, dans le
  job `gpu`) : verte, écart 0 à Apple, fausse avec le plugin d'avant ; job `gpu` 54/54,
  8 × `mixte`/`v15` par le relais sans plantage ; job `tools/guest/jobs/pixgui`.
- **Correctifs des bug hunts éprouvés dans la VM quotidienne, PC, 07/10**
  (`docs/bug-hunt-2026-09-29-passe4.md`, dernière section) : `kextunload` avec Marble Blast
  et un client tenu ouvert (K4, KG4, KT1 : dormeur réveillé en 0,9 s, aucune panique, le
  jeu repasse en rendu d'Apple), appels après déchargement refusés proprement
  (`kIOReturnBadArgument` par IOKit sous 10.4.11), rechargement et jeu relancé en
  asynchrone sans `LIMIT` (KT2), aucune tranche perdue après rafales et `kill -9`, `QFB=1`
  avec Marble Blast (614 présentations sur VGA), `system_reset` SMP=2 (CPU 1 parqué jusqu'à
  l'entrée du CPU 0 dans le noyau), son lecture/pause/arrêt/relance (silence numérique
  exact en pause, avant et après `system_reset`), `run-all.sh` rapide et `--slow`.
  **Échoué** : DOOM 3 sous `QGPU_MEM_MB=128` (201 `NO_MEM` propres, puis `exit()` de
  GLEngine dans le repli logiciel) ; à 512 Mio aucun refus n'arrive. **Non éprouvable** :
  GPU hôte bloqué (pas de propriété de test au device). Nouvel outil invité
  `guest/qgpu-test/unloadpeer`.
- **`tcg/0010` (code réécrit par l'autre vCPU) : preuve close, PC, 07/10**
  (`docs/tcg-g4.md` §17.4, §17.5) : `smctest` E × 100 sur la VM quotidienne (10.4.11,
  session bureau, SMP=2, binaire de référence) : 100 fois l'empreinte `aa4d72ebdba6b6eb`,
  0 erreur, 189 s ; A-F une fois, empreintes du §17.4. Compteurs de
  `patches/tcg/essais/0010-smcstat.patch` (porté sur l'arbre du 02/10, relevé périodique
  horodaté) pendant une partie de DOOM 3 (cellule `d3-fen`, 19 min, ~15 min dans le
  niveau) : **0 course** au démarrage, au chargement et en jeu (34 protections de page en
  jeu, 4,2 M `icbi` tous par le chemin court). Les 5 « courses 2 » relevées après la partie
  sont des bits `VGA` effacés par l'affichage, pas du code (compteur trop large, noté).
  Aucun changement de code livré.
- **`glDrawPixels` : plus de lecture guidée par des mots d'état non prouvés, 07/10**
  (`docs/backend-gl-unites-fixes.md`, « Scènes gltest sur le PC ») : sur la VM 10.4.11 du PC,
  dans la session de l'utilisateur, `try_draw_pixels` a planté trois fois (`EXC_BAD_ACCESS`
  dans `memcpy`, scènes `mixte` et `v15`) : les mots `CTX_UNPACK_*` (relevés sur 10.4.6, déjà
  pris en défaut le 22/09) valent 0 à chaque appel relevé — `ALIGNMENT` compris, dont le défaut
  GL est 4 — et ont valu autre chose ces trois fois-là, d'où une adresse source à
  `pixels + 0x5d410000`. Le plugin n'accepte plus le chemin hôte de `DrawPixels` (couleur,
  profondeur, stencil) et de `Bitmap` que si ces mots ont leur valeur par défaut ; sinon rendu
  logiciel, exact. `gltest` : les neuf scènes dont les témoins sortent de 64×64 se jouent en
  256×256 sans taille donnée, et la scène `stencil` demande toujours son tampon de stencil
  (neuf faux rouges d'une passe de nuit). Job `gpu` 52/53 (seul `vpimm`, qui attend un QEMU
  reconstruit) ; même verdict en session bureau, par l'agent racine et par le relais, sans
  plantage du plugin. Plugin à réinstaller ; le `gltest` précompilé (`disks/prebuilt`) est à
  régénérer.
- **Mode immédiat sous programme de sommets : coordonnées de texture et couleurs, 07/10**
  (`docs/protocole-v16-programmes.md` §7) : `glTexCoord`/`glMultiTexCoord`/`glColor` entre
  `glBegin` et `glEnd` sous un programme ARB se perdaient (quad entier au texel (0,0), à la
  couleur courante de `glBegin`) : `geom_format` ne portait, sous programme, un attribut
  conventionnel que si son **tableau** était actif — jamais en mode immédiat — et
  `vertex.texcoord[u]` d'une unité sans texture n'était porté (plugin) ni lié (hôte) que
  sous GLSL. Le texte du programme décide désormais seul quand il dit ce qu'il lit
  (`POMPPC_GL_VPIMM=0` : ancien filtre) ; la crainte des pointeurs résolus périmés (23/09)
  est réfutée sur le chemin des tableaux. Nouvelle scène **`gltest vpimm`** (26 témoins,
  dans le job `gpu`) : 24 faux avant, 26/26 et identique au rendu d'Apple après ; job `gpu`
  53 OK, 0 échec ; `tests/run-all.sh` inchangé (237 OK, 1 échec binaire rapide, 7 ignorés).
  **QEMU à reconstruire** (`patches/qgpu/qgpu-gl.c`) et plugin à réinstaller.
- **UT2004 : armes noires dans la démo de test, PC NVIDIA, 07/10**
  (`docs/re/ut2004-arme-noire.md`, `docs/protocole.md` « Combineurs ATI ») : le backend GL
  n'annonçait `QGPU_CAP_COMBINE3` qu'avec `GL_ATI_texture_env_combine3`, absent chez
  NVIDIA ; UT2004 prenait alors son repli `REPLACE(PREVIOUS)` et les présentoirs d'armes de
  la démo (images ~166-195) sortaient noirs. Le backend l'annonce aussi avec
  `GL_NV_texture_env_combine4` et y traduit les fonctions ATI et les sources ZERO/ONE
  (exact pour MODULATE_ADD et MODULATE_SIGNED_ADD, MODULATE_SUBTRACT approché). Démo
  entière revidée sur la VM quotidienne avec un QEMU reconstruit : armes texturées, rejeu
  `gl` = `soft`, cellule `ut-fen` juste, `gltest` sans régression. Rejoueur :
  `QGPU_REPLAY_STATE`, `QGPU_REPLAY_TEXOUT`, `QGPU_REPLAY_UNUSED`. Reste le M4, qui a
  l'extension ATI et où l'utilisateur avait vu le défaut le 01/10.

- **`tests/run-all.sh` sur le PC : deux échecs hors binaire rapide levés, 07/10**
  (`docs/backend-gl-unites-fixes.md` « Test natif », `docs/audio-stabilite.md`) :
  `qgpu_core_test` v17 (a)/(b) exigeait les unités 5 et 6 **au pipeline fixe** du backend GL,
  que `QGPU_CAP_FIXED4` (NVIDIA, 02/10) ne promet plus et que le plugin n'envoie pas (rendu
  d'Apple) ; sous `FIXED4`, ils sont refaits sous programme de fragments (mêmes pixels, obtenus
  sur la RTX 4060 Ti), le backend logiciel garde le pipeline fixe. `screamer_audio_test.py` ne
  compilait plus depuis le portage à QEMU 11.1.2 (02/10 : `audio_be_write`,
  `audio_be_set_volume_out_lr`, membre `audio_be`) ; ses bouchons suivent le device et
  vérifient que PCM et volume vont au backend du device. PC : 235 OK, 3 échecs, 7 ignorés →
  **237 OK, 1 échec (binaire rapide écarté), 7 ignorés**.
- **Architecture, suites A6 : L2 à L5 relues contre le code, 07/10**
  (`docs/architecture.md` §5-§8) : **L3** (attentes non bornées du reset et de l'arrêt du
  thread) et **L4** (arrêt du kext avec dormeurs) sont **fermées dans le code** par les
  correctifs du 29/09 (GL3, KT5 ; KG4, KT1) — seul reste non borné le drainage du retrait du
  device, inatteignable (`hotpluggable = false`) ; leurs épreuves en VM restent dans l'entrée
  « Validation ». **L2** (créneau rendu malgré un nettoyage échoué) et **L5** (verdict global)
  tiennent : correction du kext et ABI par tranche décrites, avec leurs épreuves. Nouveau test
  natif `run_a6` (`tests/qgpu_core_test.c`) : ce que le cœur fait d'un nettoyage manqué
  (`QGPU_ST_LIMIT` fatal), retardé (sans effet, FIFO) ou rejoué après réattribution.
- **Rejeu : surface lue sans être liée dans le vidage, 07/10** (`docs/matrice-jeux.md` §5) :
  `tests/qgpu_replay.c` ne créait une surface d'avant le vidage qu'au `SURF_BIND` ou d'après
  la présentation ; un `SURF_READBACK`/`UPLOAD` (ou `DEPTH_*`, `STENCIL_*`, `SURF_TEX`) d'une
  surface jamais liée donnait `NO_SURF` (DOOM 3 fenêtre, tour `20260926-2156` du M4, vidage
  sans `surfaces.txt`). Le prologue la crée comme le device l'avait (relue noire si jamais
  dessinée) et relie le contexte à la surface de ses transferts quand sa liaison était
  devinée. DOOM 3 fenêtre du PC sans `surfaces.txt` : 2 `NO_SURF` → 0, 20 images identiques
  au rejeu avec `surfaces.txt` ; rejeux des quatre cellules du PC (avec `surfaces.txt`)
  inchangés à l'octet.
- **`QGPU_REG_ERRORS` par client, 07/10** (`docs/protocole-v19-transport.md`, « Erreurs par
  client ») : le compteur d'erreurs du device était global, et la sonde attendue ou le flux fautif
  d'un **autre** processus faisait repasser le plugin en synchrone 120 images, miroirs invalidés.
  Le device tient maintenant un compteur par tranche de BAR0 (`QGPU_CAP_CLIENT_ERRORS`,
  `QGPU_REG_CLIENT_ERRORS(i)` en 0xC8 + 4·i), attribué par l'offset du flux ; le plugin garde le
  global comme déclencheur et ne relit le sien (`QGPU_UC_READ_REG`) que quand il bouge. Ni version
  du protocole ni `qgpu_abi.h` : **QEMU et plugin, kext inchangé**. Épreuve à deux clients dans
  l'invité (`tools/guest/jobs/regerr`, client fautif `guest/qgpu-test/errpeer` toutes les 10 ms
  pendant `gltest game`) : plugin d'avant **1 repli, fin en synchrone** ; d'après **0 repli, 71
  mouvements étrangers ignorés**. `gltest` 52 OK / 0 échec, `qgpu_smoke.py` 61/61 (dont l'attente
  périmée de 4 contextes par client, 32 depuis le 26/09, corrigée).
- **Binaire rapide du PC mesuré hôte au repos, matrice finale, planchers du PC, 06-07/10**
  (`docs/vitesse-doom3-x86.md` §13.5-13.7) : A/B entrelacé DOOM 3 `d3-fen`, 3 parties par bras,
  défauts du 06/10 : binaire de référence **122,9** ms/image (121,8..124,6), **binaire rapide
  111,5 (108,9..112,9), −9,3 %**. Matrice avec vidage, anciens défauts du 04/10 sur le binaire
  de référence contre binaire rapide et nouveaux défauts : **Marble Blast 20,9 → 14,9 (−29 %),
  Zenerchi 7,6 → 5,9 (−22 %), UT2004 58,1 → 48,7 (−16 %), DOOM 3 136,3 → 111,9 (−18 %)**,
  images justes (rejeu = VM et référence 0,00/0,00 % ; DOOM 3 0,03/0,00 % contre la VM dans
  les deux bras, comme le 04/10). Planchers `plancher_ms_linux` : **DOOM 3 140** (il prenait
  les 76 du M4), **Marble Blast 26 → 19**, **Zenerchi 10 → 8** ; UT2004 garde 104 (une partie).
  L'autre session qui avait pollué le premier essai travaillait encore : une barrière avant
  chaque partie (`MATAB_AVANT` de `matab.sh`, `tools/tcg/chargehote.py attendre`) et un relevé
  de la charge étrangère toutes les 5 s (`chargehote.py releve`, `chargeparties.py`) ont tenu
  ses rafales de 10 à 13 cœurs hors des 8 parties, toutes valides. La référence `d3-fen` du
  PC attend la validation à l'œil de l'utilisateur.
- **Le cas « sans traduction » de `x-ret-verify` expliqué, 06/10** (`docs/tcg-g4.md` §35) :
  ce n'est pas le noyau mais **BootX sous OpenBIOS**. Quand un PTEG est plein, le gestionnaire
  de fautes d'OpenBIOS (`hash_page`, `tlbie` en `0xfff08a5c`) évince une PTE et ne fait
  `tlbie` que de la page qui entre ; `x-tlb-precise` retire bien la classe du TLB mais garde
  l'entrée du cache de sauts de la page évincée, que l'architecture permet de garder. Prouvé
  par une instrumentation (`patches/tcg/essais/0031-retv-diag.patch`, non appliquée) et par
  les deux bras : 768 Mo, 4 cas à chaque démarrage SMP=2 (1 en SMP=1) avec `x-tlb-precise`,
  **0 sans**. Sans gravité : les traductions d'OpenBIOS ne changent jamais, le bloc exécuté
  est celui que l'ISI ferait retrouver. Correction du §32.5 : un 7400 ne garderait pas cette
  traduction (même classe), il prendrait l'ISI. Aucun patch de la série modifié.
  `tools/tcg/retvboot.sh` démarre la VM de dev en `-snapshot` pour ces essais.
- **Bilan de la phase 2 sur le PC, 06/10** (`docs/vitesse-doom3-x86.md` §13) : une partie
  DOOM 3 sous **tous** les vérificateurs (TLB précis, `lmw`/`stmw` et `dcbz` en ligne, flottant
  et AltiVec natifs avec leurs comparaisons, sorties) : 0 divergence sur 2,9 G accès
  retraduits, 2,2 G `lmw`/`stmw`, 837 M `dcbz`, 810 M opérations flottantes et 2,9 G
  vectorielles ; `x-ret-verify` relève 2 cas « sans traduction » sur 12,8 G sorties, au même
  pc du noyau (`0x5605d88`), au démarrage et au redémarrage de l'invité seulement (à
  comprendre). A/B entrelacés, 3 parties par bras, binaire de référence : le côté mémoire
  (`TLBPRECISE LMWINLINE DCBZINLINE`) fait **DOOM 3 144,3 → 132,0 ms/image (−8,5 %)**, **Marble
  Blast 20,9 → 16,5 (−21 %)**, **UT2004 58,9 → 54,7 (−7 %)** (aucune rafale dans la fenêtre) ;
  par-dessus, `JITREL32 FPNATIVECMP VFPNATIVECMP LMWVEC JCWORD` ensemble **−2,6 %** sur DOOM 3.
  **Tous allumés par défaut sur Linux x86-64** (`=0` pour couper ; macOS arm64 inchangé). Le
  profil PGO du binaire rapide est entraîné sur cette configuration (mb, zen, ut, d3 ;
  1 356 `.gcda`) et `build-fast/` reconstruit (complet, même série) : `run_tiger.sh` le prend.
  Départ du 04/10 (référence) : DOOM 3 138, Marble Blast 21, Zenerchi 7,6, UT2004 56
  ms/image ; DOOM 3 sur le binaire rapide : 124,1 sur une partie. L'A/B du binaire rapide et la
  matrice finale avec vidage restent à faire hôte au repos (une autre session occupait le PC ;
  commandes dans TODO.md). `tools/tcg/utrafales.py` lit les enregistrements courts d'UT2004.
- **Le côté mémoire du traducteur sur le PC, 06/10** (`docs/tcg-g4.md` §32) : trois
  propriétés éteintes par défaut. `x-tlb-precise` (`tcg/0031`, `TLBPRECISE=1`) : `tlbie`, les
  changements de registre de segment (avec `x-sr-tlb`) et les écritures de BAT ne retirent plus
  du TLB que les entrées concernées (classe de `tlbie` élargie, pages journalisées par segment,
  balayage), et le TLB grandit enfin (**−90 % de remplissages**). `x-lmw-inline`
  (`tcg/0032`, `LMWINLINE=1`) : `lmw`/`stmw` dans une page en accès mot ; `x-dcbz-inline`
  (`tcg/0035`, `DCBZINLINE=1`) : `dcbz` en quatre rangements. Les trois ensemble, A/B ABBA dans
  la VM de dev : **Marble Blast 19,5 → 35 img/s (+80 %)**, softmmu 24 → 7,5 % du temps vCPU,
  allers-retours par tubes −74 %, `dcbz` −65 %, `lmw`/`stmw` −9 à −47 %. Vérificateurs
  (`x-tlb-precise-verify=N`, `x-lmw-inline-verify`, `x-dcbz-inline-verify`) à zéro divergence
  sur des centaines de millions d'opérations, empreintes invité identiques (`tlbtest`,
  `dcbztest`, `lmwtest`, `smctest`), 10 mutants sur 11 détectés ; compteurs `x-mem-stats`. Défaut
  corrigé en route : le vérificateur de `x-sr-tlb` lisait la mauvaise table depuis QEMU 11.
  A/B en jeu (DOOM 3, Marble Blast, UT2004) à faire sur la VM quotidienne.
- **Flottant AltiVec restant sur le PC, 06/10** (`docs/tcg-g4.md` §34,
  `patches/tcg/0034-ppc-vfp-native-cmp.patch`) : `vcmpeqfp`, `vcmpgefp`, `vcmpgtfp`,
  `vcmpbfp` (et formes Rc, CR6 calculé en ops TCG), `vcfsx`/`vcfux` et `vctsxs`/`vctuxs`
  (saturation et VSCR[SAT]) par l'op native de `x-vfp-native` (émetteur x86-64, sans
  effet sur arm64). Propriété `x-vfp-native-cmp`, **éteinte par défaut**
  (`VFPNATIVECMP=1`), vérificateur `x-vfp-native-cmp-verify` (`VFPNCMPVERIFY=1`). Exact
  au bit près : 422,7 M vecteurs contre les helpers et le softfloat de QEMU, émetteur
  réel sous `qemu-ppc` linux-user (10 mutants sur 10), `vfptest c` identique dans Tiger
  (SMP=1 et 2), Marble Blast sous vérificateur sans divergence. Banc invité :
  comparaisons −40 à −71 %, conversions −94 à −96 %. Le vérificateur montre que Tiger
  tourne avec VSCR[NJ] = 1 et que ~42 % des `vmaddfp`/`vsubfp` de Marble Blast
  retombent au logiciel pour des dénormaux mis à zéro par NJ : prochaine cible.
  Gain attendu en jeu ~0,5 % sur DOOM 3, ~1 % sur UT2004, A/B à jouer.

- **La série 0025-0028 sur le PC x86-64, 06/10** (`docs/tcg-g4.md` §30) : binaire de référence
  du PC reconstruit avec `tcg/fixes/` et 0025-0028, plus **`tcg/0029`**, le pendant x86-64 de la
  copie `lmw`/`stmw` de 0025 (gcc 13 laisse la boucle d'origine scalaire, un `bswap` par mot) :
  quatre mots à la fois en SSE2, inlinés, sous la même propriété `x-lmw-vector` ; microbanc de
  la copie −40 % sur 19 registres. Preuves hôte : 1 024 064 cas en 32 et 64 bits, 12 mutants sur
  12 ; `vfpproof` helpers et modèle natif x86 648 M vecteurs chacun sans divergence (le zéro
  signé de `vnmsubfp` corrigé sur le M4 n'existe pas sur le chemin FMA3 x86, qui nie après
  l'arrondi par un `xor`) ; `jcwordproof` 573 440 cas. VM de dev SMP=1 et SMP=2, options de
  production et vérificateurs : empreintes VFP/VMX/LMW identiques à celles du M4, 0 divergence.
  `jitcheck.py` et `jit-m4-proof.sh` portables sous Linux. `LMWVEC`/`JCWORD` restent éteints par
  défaut sur le PC en attendant l'A/B en jeu.
- **Binaire rapide du PC, 06/10** (`docs/binaire-rapide-x86.md`) : la compilation
  `-O3 -march=native`, sans durcissements, LTO et PGO (DOOM 3 −23 %, autres jeux −18-19 % le
  04/10) quitte l'arbre d'essai : `QEMU_FAST=1 ./scripts/build_qemu_qfb.sh` construit
  `~/src/qemu/build-fast/` à côté de la référence `build/` (même série, capacités sondées,
  relevé `pomppc-build.txt`), et `run_tiger.sh` le préfère sur Linux x86-64 quand il est
  complet, construit sur ce processeur et sur la série du dépôt (`QEMU_FAST=0` : référence ;
  `QEMU_BIN` prime ; la bannière dit lequel). Profil entraîné sur une charge mixte (Marble Blast,
  Zenerchi, UT2004, DOOM 3) par `tools/tcg/pgo-train.sh`, constructions sous plafond mémoire ;
  les bras de `matab.sh` et le paquet publié restent sur la référence générique. Mesures sur la
  VM quotidienne : à venir.

- **`x-jit-rel32` sur le PC, 06/10** (`docs/tcg-g4.md` §31) : ses « deux régimes »
  sur UT2004 (57 ou 64-67 ms/image) ne viennent pas de QEMU mais de la mesure. La fenêtre
  `ut-fen` (images 13..73, ~4 s) attrape ou non la rafale de 1 à 3 s de processeur invité
  que coûte l'`osascript` de `premier_plan()` de la matrice, selon la phase de ses relevés ;
  QEMU plus rapide, la phase glisse et la rafale y tombe (8 parties sur 9 avec `JITREL32`).
  À contenu égal (pas de simulation fixe, image rapportée à la même image des 21 parties),
  `x-jit-rel32` est **−2,9 %** sur UT2004. Placement prouvé stable (20/20 lancements :
  aligné 2 Mio, `MADV_HUGEPAGE`, 64 Mio sous le texte), appels réellement directs
  (`-d out_asm`). Outils : `tools/tcg/utrafales.py` (lecture d'une campagne `ut-fen` sans
  les rafales), `tools/tcg/jitcheck.py --rel32` (placement et THP sous Linux). `JITREL32`
  reste à 0 jusqu'à l'A/B de phase 2.
- **Matrice : plus d'osascript dans la fenêtre de mesure, ssh multiplexé** (06/10,
  `docs/matrice-jeux.md` §5) : les jeux à fenêtre fixe la déclarent (`images_fenetre`) ;
  `premier_plan()` part pendant le chargement, jamais à moins de 6 s de la fenêtre ni dedans
  (replis notés, remise au premier plan après) ; System Events lancé avant le jeu.
  `tools/guest/tssh.sh` multiplexe ses connexions (`ControlMaster`, `.run/tssh-<port>`,
  `TSSH_MUX=0` pour revenir) : 0,04 s par commande au lieu de 0,9-2,2 s.
- **Flottant scalaire restant sur le PC, 06/10** (`docs/tcg-g4.md` §33,
  `patches/tcg/0033-ppc-fp-native-cmp.patch`) : `frsp`, `fctiw`, `fctiwz`,
  `fcmpo`, `fdivs` et `fdiv` passent par l'op native de `x-fp-native` (émetteur
  x86-64 ; sans effet sur arm64 en attendant le pendant NEON), `fsel` en ops TCG.
  Propriété `x-fp-native-cmp`, **éteinte par défaut** (`FPNATIVECMP=1`), vérificateur
  `x-fp-native-cmp-verify` (`FPNCMPVERIFY=1`). Exact au bit près hors FI/FX déjà
  admis par `x-fast-fp` : 461,7 M vecteurs contre le softfloat de QEMU sans
  divergence, émetteur réel éprouvé sous `qemu-ppc` linux-user (9 mutants sur 9
  détectés), `fptest c` identique dans Tiger (SMP=1 et 2), Marble Blast sous le
  vérificateur : 70 M opérations, 0 divergence. Banc invité : `frsp` −57 %, `fctiwz` −72 %.
  L'inventaire montre que le reste du « softfloat » de DOOM 3 et d'UT2004 est surtout
  de l'AltiVec (`vcmp*fp`, `vcfsx`). Gain attendu en jeu ~1 % sur DOOM 3, A/B à jouer.

- **JIT M4, 05/10** (`docs/jit-m4-2026-10-05.md`) : modèle natif NEON enfin
  sélectionné par `VFPPROOF_NATIVE=1`, 648 M vecteurs sans divergence, huit mutants
  détectés ; correction du zéro signé `vnmsubfp` et du helper lent aarch64 (0028).
  Vérification en VM SMP=1 et SMP=2, instructions et invalidations sans divergence.
  Flottant AltiVec natif : **1 336 → 362 ms (−73 %) sur microbanc**.
  Variantes `LMWVEC`/`JCWORD` exactes mais sans gain mesuré sur microbanc. À la demande de
  l’utilisateur, `VFPNATIVE`, `LMWVEC` et `JCWORD` sont allumés dans `run_tiger.sh`
  sur macOS arm64 ; chaque variable mise à `0` coupe son option. Rebuild QEMU
  avec 0025–0028 effectué sur le M4 le 05/10 à 21:12 (ppc et ppc64,
  capacités complètes, backend GL compris). A/B DOOM 3 du 06/10, six parties
  par bras entrelacées : **44,9 → 42,2 ms/image (−6,0 %)** pour les trois options
  ensemble. Matrice arrêtée à la demande de l’utilisateur : **7/15 cellules
  terminées, 6 vertes** ; les sept images sont justes, Zenerchi seul dépasse
  le seuil de vitesse (6,3 ms/image pour 6). Bilan partiel conservé dans
  `docs/mesures/m4-20261006/`, vérification de Zenerchi à reprendre.
  Profil des blocs et du code ARM (`hotblocks`, `jitblocks`) ; correction du
  placement RX sous `split-wx` (0027), 20/20 lancements dans la fenêtre du texte.

- **DOOM 3 sur le PC : 150 → 115 ms/image (−23 %), allumé par défaut le 04/10** (nuit du 03
  au 04/10, `docs/vitesse-doom3-x86.md`). Le surcoût propre à DOOM 3 sur x86 (2,5× le M4 au lieu
  de 2×) vient de sa logique à 60 tics/s, payée ~9 fois par image à 150 ms (§3.1) : tout gain
  brut y est amplifié. Leviers, chacun en A/B entrelacé (3 parties par bras) :
  `QEMU_OPT=native,nohard,lto,pgo` (variante de `build_qemu_qfb.sh`, arbre séparé) **−12,9 %** ;
  `tcg/0024` `x-jit-rel32` (tampon du JIT à < 2 Gio du texte sous Linux : appels de helpers
  directs) −2,3 % ; `tcg/0021` `x-vmx-inline` + `tcg/0022` `x-vfp-native` (vsldoi, vmrg[hl]w,
  stve*x et le flottant AltiVec dans le code généré) −3,8 % ; plugin `20261003-d3x` (4 leviers)
  −3,3 % ; `tcg/0023` (mftb sans `div`) dans le bruit. Épreuves : vfptest et vmxtest à
  l'empreinte identique dans 4 modes ; une partie de DOOM 3 sous tous les vérificateurs
  (2,35 G opérations vfp, 2,7 G vmx, 2,1 G lectures de base de temps, 0 divergence ; plugin en
  contrôle, 0 écart) ; 5 mutants de l'émetteur réel sur 5 détectés ; gltest 59 scènes
  identiques dans 7 modes. Matrice du PC tout allumé, avec vidage : mb 17,5, zen 6,3, ut 47,9
  ms/image (−18-19 %), images justes. **Allumés par défaut le 04/10** (à la demande de
  l'utilisateur) : `VMXINLINE` partout, `VFPNATIVE` sur Linux x86-64 seulement
  (émetteur aarch64 de 0022 jamais compilé : à prouver sur le M4), les 4 leviers du plugin
  (révision `20261004-d3x`). **Reconstruire QEMU** (`scripts/build_qemu_qfb.sh`, patches
  0021-0024) **et réinstaller le plugin** ; le binaire PGO reste une variante (`QEMU_OPT`).
  **`JITREL32` reste éteint** : sur UT2004 il a deux régimes selon la place du tampon du JIT
  (57 ou 64-67 ms/image contre 58 sans lui, `bench/tcg/ab/x86-ut-tcg`), cause à trouver. `r_useIndexBuffers 1` : +8 %, à éviter.
  Le vrai `-O3` (`-Doptimization=3`, posé par `native` ; un `-O3` en CFLAGS était écrasé par
  meson) : encore −2,4 %. Piège : la liaison LTO+PGO simultanée des deux QEMU a gelé le PC (swap) ;
  `build_qemu_qfb.sh` lie désormais un binaire à la fois, 6 ltrans.
- **Plugin : trois leviers pour DOOM 3 sur le PC, éteints** (03/10, `docs/d3-plugin-x86.md`,
  révision `20261003-d3x`). Diagnostic : DOOM 3 prend déjà `DRAW_NATIVE` pour tous ses dessins
  (`GEOMHOST 0` n'est pas un refus : il n'a pas de tableaux clients) ; restent au G4 le verdict
  des ~46 % de dispatches qui relient des textures, le balayage et la recopie de 456 000
  indices par image, et trois verrous par dispatch. `POMPPC_GL_IDXLAZY` (pas de balayage quand
  les miroirs sont propres, commande identique), `POMPPC_GL_IDXVEC` (balayage AltiVec,
  `pomppc_vec.c` compilé avec `-faltivec`), `POMPPC_GL_UNITVD` (verdict refait par unité,
  contrôlé par `VERDICTCHECK`), `POMPPC_GL_DISPONE` (dispatch en une passe). Ni protocole ni
  kext : **plugin à recompiler et réinstaller seulement**. Prouvé et mesuré la nuit suivante
  (entrée ci-dessus) ; installé dans la VM quotidienne du PC, leviers éteints.
- **Carillon de démarrage sous Linux** (03/10) : le frontend n'avait de lecteur que pour macOS
  (`NSSound`) ; sous Linux un bouchon refusait de jouer. `StartupChimeLinux.cpp` joue le WAV ou
  l'AIFF par PulseAudio (PipeWire compris), volume réglable pendant la lecture. Le son par défaut
  (`disks/chimes/powermac3-1-4.2.8.wav`, extrait du firmware G4, local) reste à copier sur chaque
  hôte ; `POMPPC_CHIME_FILE` en désigne un autre.
- **VM quotidienne du PC en Tiger 10.4.11, DOOM 3 Demo installé** (03/10). Marble Blast 20,7,
  Zenerchi 6,8, UT2004 57,6 ms/image (10.4.6 : 21,0 / 7,0 / 60,0) ; DOOM 3 150,5 ms/image en
  fenêtre. Le combo remet la mise en veille à 10 min : l'invité endormi coupe ssh et fait
  échouer DOOM 3 (`kCGLBadDisplay`) ; à couper par `pmset`. Lanceur `Doom 3 trace.app`.
- **`x-sr-tlb` validé sur l'hôte x86-64** (03/10, `docs/tcg-g4.md` §27) : `x-sr-tlb-verify`
  sans divergence en SMP=1 (6,6 M contrôles) et en SMP=2 sous Marble Blast (2,2 M). Le PC est
  désormais au niveau du M4 sur toutes les accélérations TCG. La matrice sait profiler QEMU
  sous Linux (`--sample-hote` par `perf`, code JIT nommé par `EXTRA_ARGS=-perfmap`).
- **Flottant AltiVec à 4 voies sur hôte x86-64** (03/10, `patches/tcg/0020`, `docs/tcg-g4.md`
  §26). `x-vfp-fast` laissait `vmaddfp`/`vnmsubfp` au logiciel sur x86 ; ils passent maintenant
  par un `vfmadd231ps` (FMA3, sondé à l'exécution) et `vaddfp`/`vsubfp` par AVX, avec les mêmes
  décisions par voie que l'arm64. `vfpproof.sh` (porté sur QEMU 11) : 648 M vecteurs, 0
  divergence, même nombre de passages rapides que le M4 ; 9 mutants sur 9 détectés ; `vfptest`
  invité à l'empreinte identique. Banc invité (i7-10700F) : 4 054 → 1 492 ms (−63 %).
- **Matrice de jeux sur le PC Linux** (nuit du 02 au 03/10, `docs/matrice-jeux.md` §6 quater).
  Portage (rejoueur EGL, ImageMagick, `/proc/loadavg`, `matab.sh` sans chemin du M4), références
  et planchers par hôte (`references-linux.csv`, `plancher_ms_linux`). Cinq cellules (Marble Blast,
  Zenerchi, UT2004 : les seuls jeux de ce PC) : 4 vertes sur 5 au tour de référence, images
  justes ; un « rejeu ≠ VM » d'UT2004 plein écran (1 sur 7), déjà vu sur le M4, montré cette fois
  hors de tout le vidage. A/B en jeu de tcg/0017-0019 : UT2004 −4,3 %, Marble Blast −3,7 %,
  Zenerchi −6,7 % (`docs/tcg-g4.md` §25.6). UT2004 y est limité par l'émulation (vCPU à 96 %).
- **Marge de tablette par disque** : `<disque>.tablet-margin` (lu par `run_tiger.sh` et
  `devloop.py`) ; le `tiger.qcow2` du PC est en 10.4.6 : 0.
- **GPU 3D sur l'hôte NVIDIA, de nouveau** (02/10, `docs/backend-gl-unites-fixes.md`). Depuis
  la v17 (24/09), l'auto-test du backend GL exigeait 8 unités au pipeline fixe ; NVIDIA en annonce
  4 (et ignore en silence les suivantes) : le PC rendait tout en logiciel. Le backend accepte 4
  unités fixes avec 8 coordonnées et 8 unités d'image et annonce `QGPU_CAP_FIXED4` (sans changer
  la version du protocole) ; le plugin annonce alors `GL_MAX_TEXTURE_UNITS` = 4 et garde 8 sous
  programme. `gltest` 51 scènes sur 51 sur la RTX 4060 Ti, couloir 626 img/s (16 en logiciel).
  Le backend logiciel n'hérite plus des capacités d'un backend GL refusé. **Plugin à
  réinstaller** dans les VM. `stage.sh` joint le contrat `qgpu_proto.h` aux jobs invités.
- **Tablette USB juste sous Tiger 10.4.11** (02/10, `patches/usbhid/0001`,
  `docs/tablette-tiger-10.4.11.md`). L'IOHIDEventDriver de 10.4.11 retire 7,5 % de chaque bout
  des axes absolus : le pointeur s'écartait du clic (×1,18 depuis le centre). `usb-tablet` gagne
  `x-abs-margin`, que `run_tiger.sh` et `devloop.py` règlent à 15 (`TABLET_MARGIN=0` pour
  ≤ 10.4.10).
- **`run_tiger.sh` lance bien ImGuiDock et QEMU 11.1.2** (02/10). La version du QEMU est
  vérifiée avant d'ouvrir le frontend (sa sortie s'y perdait) et un autre binaire est refusé,
  sauf `QEMU_BIN=` explicite ou `POMPPC_QEMU_ANY=1` ; `run_frontend.sh` reconstruit un frontend
  plus vieux que ses sources (celui du PC datait du 22/08, compilé contre une ImGui sans docking).
- **Hôte x86-64 au niveau de l'arm64** (02/10, `patches/tcg/0017`-`0019`, `docs/tcg-g4.md`
  §25). `x-fp-native` et `x-fp-native64` ont leur émetteur x86_64 (VEX et FMA3, sondés à
  l'exécution ; `x-fp-flat` sans eux), `x-tb-fast` lit le TSC invariant sous Linux (source
  d'horloge `tsc` du noyau), `vperm` passe par `pshufb` (AVX). `fptest` simple et double
  identiques octet pour octet, 486 M opérations sous `x-fp-verify` sans divergence, 1,2 G
  lectures de la base de temps sans écart positif, `vperm` 50,7 M cas sans divergence. Bancs
  invités (i7-10700F) contre l'x86 d'avant, options par défaut : chaîne simple 287 → 180 ms,
  sommets double 1 227 → 749 ms, `mftb` 31,8 → 17,5 ns, `vperm` 452 → 95 ms ; mutants de
  l'émetteur 5/5 détectés. Le vérificateur ignore `float_flag_input_denormal_used` (QEMU 10+, jamais lu
  par la cible) : faux positif de `fcmpu` sur opérande dénormal, commun aux deux hôtes.
- **Paquet Linux x86-64** : `scripts/package_release_linux.sh` (dépôt, QEMU, frontend, CD
  invité repris de la publication ou gravé par `xorriso`) ; les bibliothèques viennent du
  système, `DEPENDS.txt` liste les paquets apt. `make_guest_iso.sh` grave aussi sous Linux.

## 0.3 (02/10/2026)

Paquet macOS Apple Silicon (`scripts/package_release_macos.sh 0.3`) au commit de la version.

- **QEMU 11.1.2** (01-02/10). Toute la série `patches/` reposée sur QEMU v11.1.2 (elle visait
  9.2.0), un commit par patch, appliquée sans fuzz (`patches/README.md`, « Base : QEMU 11.1.2 »).
  Gel au démarrage de Tiger après « BSD root » corrigé : en 11.x, `tcg_optimize` n'appelle plus
  `finish_folding` pour un `case` resté `!done`, et le `case` de `ppc_fp32` (tcg/0014) gardait
  les listes de copies de son temporaire de sortie, d'où une boucle sans fin de l'optimiseur.
  Clone neuf : 25 capacités sur 25 ; matrice `bench/matrice/20261001-qemu11` 15 vertes sur 15,
  images justes (Warcraft III au second passage, échec de lancement intermittent déjà vu sous
  9.2.0) : DOOM 3 48,0 / 47,9, Prey 53,8 / 50,4, UT2004 25,3 / 25,4, Colin McRae 50,5,
  Nexuiz ARB 74,4 / 74,2, Nexuiz GLSL 35,0 / 34,5 ms/image (fenêtre / plein écran).
  `config.env` porte la version visée (`POMPPC_QEMU_VERSION`) : `build_qemu_qfb.sh` en tire le
  tag et refuse un arbre d'une autre version, `run_tiger.sh` l'affiche et prévient si le
  binaire diffère ; le titre du frontend montre la version du QEMU lancé (accueil QMP).
- **UT2004 ne gèle plus en quittant** (02/10). `clearDrawable` → `gldAttachDrawable` → relecture
  différée (`run_posts`) dans le tampon du drawable déjà rendu : faute, verrou du plugin tenu ;
  le gestionnaire du jeu appelait `exit()`, dont le nettoyage SDL revenait dans le plugin et
  attendait ce verrou. Garde de faute dédiée (`post_jmp`) : les copies sans destination sont
  jetées. Gel reproduit avec l'ancien plugin, sortie propre confirmée par l'utilisateur.
- **Paquet : `x-tb-fast` réellement actif** (02/10). Le script d'empaquetage signait QEMU avec
  l'entitlement Hypervisor.framework ; un processus qui le porte voit `cntfrq_el0` à 24 MHz
  (Apple M4, macOS 26) au lieu d'1 GHz, et la base de temps rapide (tcg/0015) retombait sur
  l'horloge d'origine (« x-tb-fast : cntfrq 24000000 Hz »). C'était le cas du paquet v0.1.0.
  Signature ad hoc sans entitlement : HVF ne sert jamais à un invité PowerPC.
- **gltest et POMPPCFsqrt sous Tiger 10.4.11** (01/10) : `gl15`, `tex13`, `tex14`, `texlod` en
  256×256 par défaut ; la dépendance de `POMPPCFsqrt` porte la version exacte du noyau
  (`uname -r`, écrite par les installateurs), sans quoi le kext ne se chargeait pas sous 10.4.11.
