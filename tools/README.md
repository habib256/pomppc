# tools/ — outils de développement

Index de tout ce que `tools/` versionne (relu contre `git ls-files tools/` le 01/11/2026). Les
mots « VM quotidienne » et « VM de dev » ont leur sens de `docs/etat-des-hotes.md` : la
première est le Tiger de tous les jours (`run_tiger.sh`, `.run/tiger.lock`), la seconde une
image raw de développement passée en `DEVDISK`.

Détails et pièges : `docs/gpu-3d-tiger.md` §4 (rétro-ingénierie) et §5.1 (boucle de
développement).

⚠ `devloop prepare` installe l'agent, le StartupItem et l'élément d'ouverture dans l'image
passée en `DEVDISK`. Ne l'utiliser que sur un disque de développement, jamais sur le disque
Tiger de tous les jours.

## guest/ — boucle de développement et gestes dans l'invité

| Outil | Rôle |
|---|---|
| `guest/devloop.py` | Boucle de développement dans l'invité Tiger allumé : un job (dossier avec `job.sh`) part par une boîte aux lettres sur le disque brut et revient en quelques secondes. `prepare` (par `hdiutil` sur macOS, **dans l'invité** ailleurs), `start [--gui]`, `run DOSSIER [--timeout S]`, `shot`, `type`, `click`, `shutdown [--gui]` (arrêt sûr), `stop` (coupe le courant). Disque : `DEVDISK=` (image raw, sans défaut) ; `CDROM=` ajoute des lecteurs. |
| `guest/agent.sh` | L'agent invité de `devloop` (lecture brute de la boîte d'entrée, écriture de la sortie par son fichier). |
| `guest/POMPPCAgent/` | StartupItem qui lance l'agent au démarrage du bureau. |
| `guest/POMPPCGuiRunner.app` | Élément d'ouverture de session : exécute les jobs graphiques dans la session de l'utilisateur (un processus racine ne peut ouvrir ni fenêtre ni contexte CGL). |
| `guest/guilib.sh` | `gui_run 'commande'` pour les `job.sh`, livré automatiquement avec chaque job. |
| `guest/tssh.sh`, `guest/tssh.exp` | ssh vers le Tiger quotidien (algorithmes anciens ; clé hors dépôt, `.run/cmr/id_rsa` du dépôt principal ou `TSSH_KEY` ; port publié dans `.run/tiger.sshport`) ; `-p` par mot de passe (`tssh.exp`). |
| `guest/cycle.sh`, `guest/killgame.py` | Recompiler et réinstaller le plugin dans l'invité (`NORUN=1`), cycle Colin McRae ; tuer Colin McRae par le stub GDB de QEMU. Déplacés de `.run/cmr/` (A5). |
| `guest/frame_report.py`, `guest/flyby_report.py` | Résumé des intervalles d'échange d'un `frames.csv` (`POMPPC_GL_FRAMES`) ; validation et bilan d'un job `utflyby` terminé. Testés par `tests/frame_report_test.py` et `tests/flyby_report_test.py`. |
| `guest/epreuve_gpu_bloque.py` | Côté hôte des épreuves « GPU hôte bloqué » (GL3, KT4, K6, A6) sur une VM de dev, avec le job `stall` (`docs/epreuve-gpu-bloque.md`). |
| `guest/exitwatch/exitwatch.c` | Mouchard `exit()`/`abort()` injecté par `DYLD_INSERT_LIBRARIES` (interposition dyld) : pile PowerPC du processus qui sort. |
| `guest/gltrap/gltrap.c` | Mouchard des appels OpenGL d'une application Tiger (programmes ARB, attributs génériques, erreurs GL), même interposition ; écrit pour Colin McRae. |
| `guest/launchers/` | Lanceurs à copier dans l'invité : `Doom3-trace.sh`, `UT2004-trace.sh` (jeu sous la trace du plugin, bundle d'après `Info.plist.example`), `nexuiz.command` (banc Nexuiz) et `nexuiz-test.command` (raccourci interactif), `premierplan.c` (bibliothèque qui met au premier plan un jeu lancé par son exécutable). |

### guest/jobs/ — jobs `devloop`

`jobs/stage.sh NOM DOSSIER` assemble un job : son `job.sh` et, dans `src/`, les sources invité
du dépôt ; `jobs/lib.sh` (étapes communes des jobs GPU) y est joint. Les dossiers sans `job.sh`
(`iotrace`, `sqrttest`, `stallmeter`, `tbtest`) ne contiennent que des sources à compiler dans
l'invité ; `jit-bench` et `jit-suite` reçoivent leurs sources (`vfptest.c`, `lmwtest.c`,
`jctest.c`…) de leur pilote (`tools/tcg/jit-m4-proof.sh`, `ppc32mt-bench.sh`, `tlbbdev.sh`).

| Famille | Jobs |
|---|---|
| Installation de la VM de dev | `xcode` (Xcode Tools du DVD), `combo` (passage de 10.4.6 à 10.4.11 par la mise à jour combinée), `install` (installation réelle par `guest/gldriver/install.sh`, puis redémarrage), `prebuilt` (kext, plugins, `gltest`, `glwin`, `accelprobe` compilés pour un Tiger sans Xcode Tools) |
| GPU et plugin | `gpu` (chaîne complète et `gltest`), `accel` (accélérateur IOKit, tâche 4.2), `diag` (chargement du plugin par GLEngine), `glwin` (mode bureau), `pixgui` (état de `glPixelStorei` dans la session), `regerr` (`QGPU_CAP_CLIENT_ERRORS`), `scene` (une ou plusieurs scènes `gltest` par les deux chemins et sous Apple), `tex3d`, `texup` (débit de téléversement), `plugmd5` (scènes sous plusieurs modes du plugin, images comparées à l'octet), `plugprof` (profil d'instructions du plugin sous le greffon `hotblocks`), `swapwin` (l'échange dans la session), `stall` (côté invité de `epreuve_gpu_bloque.py`) ; scripts de scènes à lancer dans l'invité : `gt5.sh` (lots du verdict unique), `gta4.sh` (bloc d'état A4), `gtgeo.sh` (géométrie A4), `gtglsl.sh` (GLSL) |
| Relevés de rétro-ingénierie | `t3dprobe`, `cubeprobe`, `wrapprobe`, `v14probe` (`docs/re/textures-3d.md`, `cartes-de-cube.md`, `bordure-et-compression.md`, `opengl-1.4.md`) |
| Jeux | `games` (jeux réels sur le bureau, bilan du plugin), `fpgames` (A/B du flottant rapide sur les jeux), `tcgmb` (Marble Blast pour les A/B du processeur émulé), `utflyby`, `utfullscreen`, `utprofile` (UT2004) |
| Processeur émulé : preuves dans l'invité | `fpbench`, `fptest`, `lfstest`, `lmwtest`, `dcbztest`, `vfptest`, `vmxtest`, `smctest`, `tlbtest`, `tlbbtest`, `jctest` (branches indirectes), `tbtest` (base de temps `mftb`), `sqrttest` (racine de la libm contre `fsqrt`), `fpk-suite` (preuves de tcg/0045-0047, 0051, 0052 en un démarrage, pour un hôte sans `qemu-ppc` linux-user) |
| Processeur émulé : bancs | `regime` (régime de vitesse d'un démarrage), `mtbench` et `mtcharge` (G4 sur la cible 32 bits en MTTCG), `t2bench` (traducteur de second niveau, scène `d3inter`), `jit-bench`, `jit-suite` |
| Son et gels | `iotrace` (trace de l'IOProc CoreAudio, lanceur `d3son.command` de `tools/son/partie.py`), `stallmeter` (arrêts des vCPU vus par la base de temps ; `qhold.c` tient N clients du kext) |

## matrice/ — matrice de jeux (A3)

| Outil | Rôle |
|---|---|
| `matrice/matrice.py` | **Matrice de jeux** : joue chaque jeu en fenêtre et en plein écran sur la VM quotidienne, vide, rejoue en natif, compare à la capture de la VM et à une référence rangée, mesure ms/image et replis ; tableau vert/rouge dans `bench/matrice/<tour>/`. `docs/matrice-jeux.md`. |
| `matrice/jeu.py`, `matrice/jeux/` | Classe de base et un module par jeu : `cmr`, `d3`, `mb`, `nx`, `prey`, `ut`, `wc3`, `zen` ; `rtcw` (non automatisé, jeu absent du disque quotidien). |
| `matrice/hote.py` | Gestes de l'hôte sur la VM quotidienne (ssh, moniteur HMP de `.run/tiger.mon`, capture, redémarrage, rapatriement). |
| `matrice/guest/lance.command` | Lanceur unique dans l'invité (lit `~/matrice/cellule.sh` écrit par l'hôte). |
| `matrice/ppmcmp.c` | Comparateur d'images PPM. |
| `matrice/references.csv`, `matrice/references-linux.csv` | Empreintes des références : M4 et PC (`docs/matrice-jeux.md` §6 quater). |
| `matrice/ab-attente.sh`, `ab-copie-gpu.sh`, `ab-fsqrt.sh`, `ab-geometrie.sh`, `ab-geometrie-bilan.py` | A/B en VM : attente de l'invité sur l'hôte, copie GPU surface → texture, kext `POMPPCFsqrt`, volet géométrie d'A4 (et son bilan). |

## re/ — rétro-ingénierie, vidages, profils

| Outil | Rôle |
|---|---|
| `re/ppcanno.py` | Désassembleur PowerPC annoté pour les Mach-O de Tiger (capstone) : résout l'adressage PIC (`bcl`/`mflr`/`addis`), les stubs d'import et les chaînes. `--xref chaîne`, `--callers symbole`, ou le nom d'une fonction. C'est l'outil qui a servi à lire `GLEngine`, `OpenGL` et le `GLDriver` d'Apple. |
| `re/machonm.py` | Table de symboles d'un Mach-O PowerPC (tranche ppc d'un binaire universel), sans `nm`. |
| `re/sym.py` | Décalage → symbole dans le bundle du plugin (`nm -n` sur l'hôte) : `sym.py GLDriver-POMPPC 0x1234 …`, pour lire les `CRASH pc/lr` de `<note>.crash`. |
| `re/diffstate.py` | Diff des vidages d'état de GLEngine du traceur du plugin (`POMPPC_GLTRACE`), recherche de flottants. |
| `re/dumpdec.py` | Décode les commandes d'un vidage `POMPPC_GL_DUMP` (en-tête 64 octets puis flux de commandes) avec les noms `QGPU_OP_*`/`QGPU_SK_*` lus dans `patches/qgpu/qgpu_proto.h` : `dumpdec.py VIDAGE [pc|index] [tout]`. |
| `re/dumpstat.py`, `re/dumpvidages.py` | Volumes par image d'un vidage (textures, sommets, indices, arène) ; pourquoi chaque soumission d'un vidage est partie. |
| `re/frames.py` | Résume `frames.csv` d'un vidage par fenêtre de N images : ms/image, dessins/image, replis. |
| `re/attente.py`, `re/a4attente.py`, `re/a4tex.py` | Attente de l'invité sur l'hôte par image (fenêtre de mesure de la matrice ; fenêtre du vidage) ; trafic de texels par image d'un vidage (A4). |
| `re/sampleplug.py`, `re/partfil.py`, `re/hotplug.py` | Profils du plugin : `sample` pris dans Tiger (postes du plugin ; fil principal partagé entre plugin, GLEngine, kext…) ; profil d'instructions entre deux instantanés du greffon `hotblocks` (job `plugprof`). |
| `re/perfpart.py` | Répartition d'un `perf record` du QEMU par fil et par poste (pendant Linux des outils `sample` du M4). |
| `re/kpanic.py` | Texte et registres d'une panique du noyau lus dans la VM gelée (à lancer **avant** tout reset). |
| `re/epreuve_reset.py` | Épreuve « panique puis `system_reset` » sur la VM quotidienne. |

## tcg/ — processeur émulé : mesures, preuves, bancs

`docs/tcg-g4.md`, `docs/vitesse-doom3-x86.md`, `docs/parite-m4-2510.md`. Les preuves hôte
extraient le code TEL QUEL d'un arbre QEMU patché ; chaque `*-mut.*` est la contre-épreuve
(chaque mutation doit être détectée).

| Famille | Outils |
|---|---|
| A/B en jeu sur la VM quotidienne | `matab.sh` (A/B entrelacé par la matrice ; `--restore` rend la VM quotidienne), `matabsum.py` (bilan), `chargehote.py` (hôte au repos avant une mesure) et `chargeparties.py` (charge étrangère par partie), `utrafales.py` (rafales d'images lentes d'UT2004), `jcmiss.py` (ratés du cache de sauts, `RETVERIFY=1`), `vmwith.sh` (relance la VM quotidienne avec des variables), `d3run.sh`, `retd3.sh`, `d3win.py` (parties de DOOM 3 et leurs fenêtres), `guest/meas-tcg.sh` (le pendant dans l'invité) |
| A/B et bancs sur une VM de dev | `fpnatab.sh`, `fpnatsum.py`, `fpnatd3.py`, `fpnatd3sum.py`, `mbab.sh`, `mbpair.py`, `mbreport.sh`, `regab.sh`, `regidx.py`, `regreport.py`, `smpab.sh`, `psmsum.py`, `threadbusy.py`, `ppc32mt-bench.sh`, `ppc32mtsum.py`, `lfskdev.sh`, `tlbbdev.sh`, `tlbbbanc.py`, `t2dev.sh`, `retproof.sh`, `retvboot.sh`, `jit-m4-proof.sh`, `clearbox.py` (efface la boîte d'entrée de `devloop`) |
| Preuves hôte et contre-épreuves | `lfsproof.sh`/`.c` (tcg/0002), `vfpproof.sh`/`.c`, `vfpproof-x86-mut.sh`, `vfpproof-native-mut.sh` (tcg/0003, 0020, 0022), `vpermproof.sh`/`.c` (tcg/0004), `fpproof.sh`/`.c`, `fpproof-mut.sh` (tcg/0007), `fpflat-mut.sh` (tcg/0013), `fpnat-mut.sh`, `fpnatbench.c` (tcg/0014), `vmxproof.sh`/`.c` (tcg/0021), `lmwvectorproof.sh`/`.c`, `lmwvectorproof-x86-mut.sh` (tcg/0025, 0029), `tlbpproof.py` (tcg/0031), `fpnatcmp-user.sh`, `fpnatcmp-userhack.py`, `fpnatcmp-mut.sh` (tcg/0033), `vfpcmp-user.sh`, `vfpcmp-mut.sh`, `vfpcmpproof.sh`/`.c`, `vfpcmpproof-mut.sh` (tcg/0034), `vfncxcheck.sh`/`.c`, `vfncxcheck-ref.txt` (tcg/0034 contre 0038), `jcwordproof.py` (tcg/0026, tcg/0060), `t2proof.sh`, `t2proof.S`, `t2proof-assemble.py` (tcg/0048, banc nu), `lfskuser.sh`, `lfsk-mut.py`, `lfsksize.sh` (tcg/0051), `tlbbproof.c`, `tlbb-mut.py`, `tlbbnu.sh`, `tlbbnu.S`, `tlbbnu-mut.sh`, `tlbbsize.sh`, `tlbbcover.py`, `tlbbase.py` (tcg/0052), `tbhproof.sh`, `tbhproof.S` (tcg/0064, banc nu), `pollnsproof.py`/`.c` (contrat de tcg/0058 contre `g_poll`, macOS), `coldbranchproof.py`/`.c` (sauts longs de la zone froide exécutés sur arm64) et `coldzoneproof.py`/`.c` (relais et pools de tcg/0066), `m4-fpk-mut.sh` (émetteur aarch64 de 0037, 0045-0046, 0051, 0052), `pc-serie-proofs.sh` et `m4-serie-0031-0035-proofs.sh` (séances de preuves des séries), `a64enc-check.py`, `a64syntax.sh` (encodages et syntaxe A64 relus depuis le PC), `cc1.py` (compile un objet d'un arbre modifié avec la ligne d'un arbre construit) |
| Greffons TCG (construits par `build.sh` en `libNOM.dylib`/`.so`) | `ppcmix.c` + `ppcmix.py` (opcodes exécutés), `fprun.c` + `fprun.py` (suites de flottant scalaire), `hotblocks.c` + `jitblocks.py` (exécutions par traduction, coût du code émis), `tbedges.c` (blocs et enchaînements) |
| Anatomie du code émis et profils | `codeanat.py`, `codeanat32.py`, `codeop.py`, `codeop2.py`, `fpsrc.py`, `tcgsize.py`, `pmc5count.py`, `regions.py`, `tbreplay.py` + `tbreplay.c` (retraduction des blocs d'un journal par `qemu-ppc` linux-user), `samplesum.py` (`sample` macOS), `samplejit.py` (`sample` macOS rendu aux PC invités d'un journal `out_asm`), `selfprof.py` (auto-profileur `POMPPC_SELFPROF`), `perfhw.sh` (compteurs Intel), `jitpoll.py` (`info jit` périodique), `hmp.py` |
| Hôte et JIT | `pgo-train.sh` (profil PGO du binaire rapide du PC, `docs/binaire-rapide-x86.md`), `jitwhere.sh` et `jitcheck.py` (où et comment le tampon du JIT est projeté), `farcall.c` (appel lointain depuis le code généré, Apple Silicon), `tbclock.c` (coût de la base de temps de l'hôte arm64) |

## Autres dossiers

| Outil | Rôle |
|---|---|
| `endurance/endurance.py` | Banc d'endurance : démarrages et cycles de jeu en série, collecte et symbolisation des paniques (`docs/endurance.md`). Modules : `hmp.py` (client HMP), `memoire.py` (vidage de la RAM), `serie.py` (console série sans ssh), `symbolise.py` (adresse → symbole), `invite-syms.sh` (symboles des kexts, exécuté dans l'invité) ; `devdemarrages.py` : démarrages répétés sur un disque de dev, sans ssh. |
| `son/` | Relevé du son (`docs/audio-stabilite.md`) : `partie.py` (une partie de DOOM 3, Prey ou Marble Blast), `resume.py`, `bilan.py`, `zpos.py` (journal `SCREAMER_DIAG`), `pcm.py` (PCM enregistré), `iot.py` (`iotrace.csv` du job `iotrace`). |
| `gld/gen_tramp.py` | Génère `guest/gldriver/gld_tramp.s`, les trampolines du plugin OpenGL (l'invité n'a pas Python). `tests/run-all.sh` vérifie que le fichier livré est à jour. |
| `gl/gl_fixed_units.c` | Unités de texture du pipeline fixe de l'hôte (EGL, Linux) réellement appliquées (`docs/backend-gl-unites-fixes.md`). |
| `web/banc.py` | Banc de sites pour le relais web des vieux navigateurs (`scripts/web-proxy.py`). |

Hors de `tools/` mais du même usage : `scripts/moncmd.py`, commande HMP sur un moniteur en
socket UNIX (`.run/mon.sock` pour la VM quotidienne, chemin publié dans `.run/tiger.mon` ;
`.run/os9-mon.sock` pour Mac OS 9). La VM de dev de `devloop` n'expose que QMP
(`bench/devloop/qmp.sock`) ; `tcg/hmp.py` et `endurance/hmp.py` parlent aux moniteurs HMP des
bancs. Contrôle AZERTY / CD / UT : `docs/re/ut2004-demo.md` §5.
