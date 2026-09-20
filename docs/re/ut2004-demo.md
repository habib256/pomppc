# UT2004 Demo sur Tiger quotidien — relevé du 20/09/2026

Premier passage réel d’**Unreal Tournament 2004 Demo Mac PPC** sous Tiger 10.4.6
(QEMU mac99 G4, qgpu-pci). Vérifié dans l’invité, VM quotidienne
`disks/tiger.qcow2`. Ce fichier sert à **reprendre** : ce qui marche, ce qui
bloque, comment piloter Tiger sans clavier AZERTY sous la main.

Le critère du projet est UT2004 « à fond » (`docs/todo-gpu-3d.md`). Marble Blast
et Zenerchi restent les témoins de non-régression.

## 1. Verdict

L’image est **juste** en fenêtre : textures, polices, curseur, HUD, décor 3D
(DM-Rankin). Le moteur tourne. **C’est inutilisable** : trop lent, même après
le T&L hôte. La démo n’est pas un plantage — c’est le G4 émulé (et le flottant
exact de QEMU) qui n’arrive pas à suivre Unreal Engine 2.

Plein écran : pixel format OK, `CGLSetFullScreen` → `invalid drawable`.

## 2. Ce qui a été levé (ne pas refaire)

Ordre chronologique, chaque porte fermait la suivante.

| Porte | Symptôme | Cause | Correctif (plugin) |
|---|---|---|---|
| Identité CGL | « Failed creating OpenGL pixel format » (SDL 1.2 Quartz) | le logiciel Apple n’a pas les drapeaux `0x2` (plein écran) / `0x2000` (fenêtre) / `0x100` (accéléré) ; attributs 54/72/73 refusés | `RI_OURS` posé, attributs retirés de la copie Apple (`pomppc_accel.c`, `docs/gpu-3d-tiger.md` § RendererInfo) |
| UI blanche | menus et curseur = carrés blancs/jaunes ; le hit-test marchait | police/curseur = texture **un niveau**, `MIN_FILTER` défaut `NEAREST_MIPMAP_LINEAR` (0x2702) → texture incomplète, hôte `qgpu_texture_levels` = 0, texturing sauté | si base OK et filtre mipmap : rabattre en `NEAREST`/`LINEAR` ; `tex_base_ok()` |
| Pavés jaunes | après un premier refus, tout le GPU tombait | `broken=1` sur tout `BAD_ARG` | `broken_all` : `BAD_ARG` jette **ce** lot, accélération gardée (8 lignes stderr puis omit) |
| Opcode 0x31 | spam `DRAW_TRIANGLES_TEX`, nverts 105 | hôte `do_draw` refuse NaN / `\|v\|≥1e9` pour **toute** la soumission | `sane_f` sur les sommets rastérisés ; `prim()` saute la primitive dont x/y n’est pas fini |
| T&L coupé | « géométrie brute refusée (statut 4), repli sur la rastérisation » | premier `DRAW_RAW` NaN → `geom_lost=1`, `G.v7=0` **pour la session** | ne plus tuer le T&L pour un NaN : assainir / jeter la primitive |
| Fillrate | image plus belle, **beaucoup** plus lente que la rastérisation | NaN remis à 0 → triangle jusqu’à l’origine, remplissage plein écran | `raw_fix_nan()` : bits IEEE (pas de FPU), jette la primitive, pose 0 sur le mot pour que l’hôte accepte le reste |

Autres vus, pas corrigés :

- `CGLSetFullScreen` / `gldAttachDrawable` → Apple logiciel, drawable invalide.
- `TEX_DESTROY` (`0x41`) `BAD_ARG` à la sortie.
- `malloc: double free` dans `ut2004-bin` à la fermeture.
- Packs démo absents (`SkaarjPack.u`, `BonusPack.u`, `SampleSkin.u`) : bruits
  normaux, pas des plantages.
- FASTFP **éteint** sur le QEMU quotidien (`~/src/qemu/build`, pas
  `qemu-fastfp`). `FASTFP=1 ./run_tiger.sh` est le levier de vitesse déjà
  prouvé (Marble ×1,2, Zenerchi −12 s, `docs/flottant-rapide.md`).

## 3. Où est le code

- Plugin : `guest/gldriver/pomppc_accel.c` (`tex_base_ok`, `sane_f`, `u_insane`,
  `raw_fix_nan`, `prim`, `broken_all`, drapeaux `RI_*`).
- Identité CGL : `guest/gldriver/pomppc_gld.c` (`gldChoosePixelFormat` copie
  traduite). `gldAttachDrawable` est encore un `FWD8` vers Apple.
- Sonde : `guest/gltest/accelprobe.c` (plein écran / fenêtre / liste SDL).
- Job jeux : `tools/guest/jobs/games/job.sh`, `GAMES=… ut` (pas dans le défaut).
- Hôte (inchangé ce jour) : `patches/qgpu/qgpu-core.c` `do_draw` / `do_draw_raw`
  rejettent encore NaN (`QGPU_ST_BAD_ARG`). Le plugin compense. Le bon endroit
  long terme est l’hôte : sauter la primitive **sans** balayer le G4.

Compiler le plugin : VM **dev** seulement.

```
export DEVDISK=/home/gistarcade/src/pomppc/disks/tiger-dev.raw
./tools/guest/jobs/stage.sh prebuilt /tmp/job
python3 tools/guest/devloop.py run /tmp/job --timeout 300
# puis copier bench/devloop/last/out/prebuilt → disks/prebuilt + VERSION
./scripts/make_kext_iso.sh disks/pomppc-src-<étiquette>.iso
```

La quotidienne n’a **pas** `gcc-4.0` : `install.sh` prend `prebuilt/` du CD.

## 4. Attaque suivante (M4)

1. **Mesurer** : relancer `./ut2004-bin -windowed` avec `POMPPC_GL_STATS=/tmp/ut.txt`
   (le `_` AZERTY, voir §5). Savoir si `DRAW_RAW` reste vivant (`sommets bruts` /
   `DRAW_RAW/img`) ou si on est encore en rastérisation. Sans ça on devine.
2. **Allumer FASTFP** sur la quotidienne (`FASTFP=1 ./run_tiger.sh`, binaire qui
   a `x-fast-fp`). C’est le gain déjà chiffré sur Marble/Zenerchi. Arrêt **propre**
   avant (`shutdown`, pas `quit` à chaud sur `tiger.qcow2`).
3. **NaN côté hôte** : dans `do_draw_raw` / `do_draw`, sauter la primitive
   au lieu de `BAD_ARG` sur toute la soumission — et retirer `raw_fix_nan` du
   plugin (scan G4 inutile).
4. Plein écran : `gldAttachDrawable` / `CGLSetFullScreen` (ne plus forward
   aveugle à Apple). Pixel format déjà bon.
5. `malloc` double-free à la sortie ; `TEX_DESTROY` 0x41.
6. Ne **pas** re-découper `tiger.qcow2` vers TEST tant que la VM n’est pas
   éteinte proprement.

Hors UT, ne pas casser Marble/Zenerchi (`gltest` 40/40, chemin brut).

## 5. Contrôle de TigerOSX (quotidienne)

Deux VM, **ne pas les confondre**.

| | Quotidienne | Dev (mailbox) |
|---|---|---|
| Disque | `disks/tiger.qcow2` (~6 G, **pas** dans git) | `disks/tiger-dev.raw` |
| QEMU | `~/src/qemu/build/qemu-system-ppc64`, GTK 1024×768, 768 Mo, SMP 2 | `qemu-system-ppc` headless, 1 Gio, mailbox |
| Sockets | `.run/qmp.sock`, `.run/mon.sock` | `bench/devloop/qmp.sock` |
| GPU | `qgpu-pci,backend=auto` | idem |
| Compte | `tiger` / `tiger974` (sudo) | root single-user + agent |

`pgrep qemu-system-ppc` tronque à 15 caractères : un faux « mort » est fréquent.
Lire la ligne complète (`pgrep -af qemu-system-ppc`).

### Clavier AZERTY dans l’invité

`send-key` parle QEMU **qcode US**. L’invité est AZERTY. Table utilisée :

| On veut | qcode |
|---|---|
| a / q | `q` / `a` |
| z / w | `w` / `z` |
| m | `semicolon` |
| / | `kp_divide` |
| - | `equal` |
| . | `shift`+`comma` |
| chiffre | `shift`+chiffre |
| " | qcode `3` (pas testé partout) |

Hold 100 ms, **0,15 s entre chaque touche**, sinon le HID de Tiger lâche des
caractères. `devloop.py type` est trop rapide pour la quotidienne (40 ms).

Command-Q / Command-E et alt-tab sont **peu fiables** (SDL + AZERTY). Pour
quitter UT : clic souris sur **Exit UT2004**, ou `killall ut2004-bin` dans
Terminal. Le bouton rouge de fenêtre était ~`(231, 41)` quand la fenêtre était
centrée ; dès qu’on la déplace, recaler sur les pixels rouges du screendump
(`r>180, g<80, b<80`). Un clic ~`(500, 400)` au lancement tape **Instant Action**.

Souris : tablette USB, coordonnées **absolues** 0…32767 ramenées à 1024×768.

### CD POMPPCSRC

ISO 9660 **sensible à la casse** : `/Volumes/POMPPCSRC`, jamais `/volumes/pomppsrc`.

Changer l’ISO **sans éjecter** laisse le cache du volume : `cat VERSION` montre
l’ancienne. Séquence :

```
# dans l’invité (AZERTY, lent)
hdiutil eject /Volumes/POMPPCSRC

# hôte
python3 scripts/moncmd.py .run/mon.sock "eject -f gamecd"
python3 scripts/moncmd.py .run/mon.sock \
  'change gamecd -f /home/gistarcade/src/pomppc/disks/pomppc-src-<étiquette>.iso raw'
# attendre le remontage, icône CD, puis
cat /Volumes/POMPPCSRC/prebuilt/VERSION
sudo sh /Volumes/POMPPCSRC/guest/gldriver/install.sh   # mot de passe tiger974
```

`install.sh` n’a pas besoin de redémarrer le bureau pour **recharger le plugin**
(le kext est `kextunload`/`kextload`). Relancer l’appli GL suffit.

Dernier plugin installé le 20/09 ~13:13 : *« DRAW_RAW jette primitive NaN (pas
fillrate) »*. Les ISO annexes (`pomppc-src-nan.iso`, `-keep.iso`, `-drop.iso`,
`-uttex.iso`) sont des gravures de travail, hors git.

### Lancer la démo

Installée sur le bureau de `tiger` :

```
cd "/Users/tiger/Desktop/Unreal Tournament 2004 Demo.app/System"
./ut2004-bin -windowed
```

Fenêtré d’abord. Plein écran casse encore. Captures QMP :
`screendump` → PPM, convertir en PNG (`PIL`).

### Pièges

- `sudo -n` est illégal sous ce sudoers : toujours le mot de passe.
- Finder : Return **renomme**, n’ouvre pas.
- Ne pas `change` un ISO encore monté (ci-dessus).
- Ne pas `quit` QEMU pendant une écriture HFS+ (`tiger.qcow2` / `tiger-dev.raw`) :
  `devloop.py shutdown` (dev) ; quotidienne : arrêt propre du bureau.
- Archive M4 / TEST : `/media/gistarcade/TEST/pomppc/input/ut2004/` (installeur).
  Ne pas re-splitter le qcow2 tant que la VM tourne.

## 6. Preuves visuelles du jour

- Menu lisible (libellés, curseur flèche, fond 3D) après mipmap + `BAD_ARG` non fatal.
- DM-Rankin : HUD (vie 100, armes, viseur), briques / sol / arme texturés — plugin
  12:55 (rastérisation, lent mais lisible).
- Plugin 13:05 (NaN→0, T&L) : plus bel éclairage, fillrate, **plus lent** que 12:55.
- Plugin 13:13 (jette la primitive) : T&L sans triangles à l’origine ; toujours
  trop lent pour jouer (FASTFP off, pas de bilan `POMPPC_GL_STATS`).
