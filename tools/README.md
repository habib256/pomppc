# tools/ — outils de développement du GPU paravirtuel

| Outil | Rôle |
|---|---|
| `re/ppcanno.py` | Désassembleur PowerPC annoté pour les Mach-O de Tiger (capstone) : résout l'adressage PIC (`bcl`/`mflr`/`addis`), les stubs d'import et les chaînes. `--xref chaîne`, `--callers symbole`, ou le nom d'une fonction. C'est l'outil qui a servi à lire `GLEngine`, `OpenGL` et le `GLDriver` d'Apple. |
| `gld/gen_tramp.py` | Génère `guest/gldriver/gld_tramp.s`, les trampolines du plugin OpenGL (l'invité n'a pas Python). `tests/run-all.sh` vérifie que le fichier livré est à jour. |
| `guest/devloop.py` | Boucle de développement dans une VM Tiger allumée : un job (dossier avec `job.sh`) part par une boîte aux lettres sur le disque brut et revient en quelques secondes. `prepare` (par `hdiutil` sur macOS, **dans l'invité** ailleurs), `start [--gui]`, `run DOSSIER`, `shot`, `type`, `stop`. Disque : `DEVDISK=` (image raw) ; `CDROM=` ajoute des lecteurs. |
| `guest/agent.sh` | L'agent invité de `devloop` (lecture brute de la boîte d'entrée, écriture de la sortie par son fichier). |
| `guest/POMPPCAgent/` | StartupItem qui lance l'agent au démarrage du bureau. |
| `guest/POMPPCGuiRunner.app` | Élément d'ouverture de session : exécute les jobs graphiques dans la session de l'utilisateur (un processus racine ne peut ouvrir ni fenêtre ni contexte CGL). |
| `guest/guilib.sh` | `gui_run 'commande'` pour les `job.sh`, livré automatiquement avec chaque job. |
| `guest/jobs/` | Jobs réutilisables : `xcode` (Xcode Tools du DVD dans la VM de dev), `gpu` (chaîne complète et `gltest`), `texup` (débit de téléversement), `glwin` (mode bureau), `diag`, `scene` (une ou plusieurs scènes `gltest` par les deux chemins et sous Apple), et les sondes de relevé `t3dprobe`, `cubeprobe`, `wrapprobe`, `v14probe` (`docs/re/textures-3d.md`, `cartes-de-cube.md`, `bordure-et-compression.md`, `opengl-1.4.md`). `stage.sh NOM DOSSIER` y joint les sources invité du dépôt. |

Détails et pièges : `docs/gpu-3d-tiger.md` §4 (rétro-ingénierie) et §5.1 (boucle de
développement).

⚠ `devloop prepare` installe l'agent, le StartupItem et l'élément d'ouverture dans l'image
passée en `DEVDISK`. Ne l'utiliser que sur un disque de développement, jamais sur le disque
Tiger de tous les jours.
