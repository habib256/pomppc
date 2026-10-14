# R5 : pagination des textures sous le plafond mémoire de l'hôte (14/10/2026, PC)

Reste (1) de « [Validation] Restes des épreuves en VM des bug hunts » (`TODO.md`) :
avec `QGPU_MEM_MB=128`, DOOM 3 essuyait 201 `NO_MEM` proprement puis mourait par l'`exit()`
de GLEngine (`docs/bug-hunt-2026-09-29-passe4.md`, dernière section ;
`docs/re/glengine-exit-interpolateur.md`). Plugin `20261014-r5`.

## 1. Cause

Le repli du 29/09 rendait à Apple, pendant `NOMEM_BACKOFF` (120) images, tout objet que l'hôte
avait refusé (`NO_TEX_NOMEM` dans `texture_uploadable_full`, et pendant la même fenêtre toute
texture neuve, toute surface, tout texte de programme). Sous programme ARB, un dessin rendu à
Apple passe par le T&L logiciel de GLEngine (`gleVPRenderTriangles` → `gleClipPoly` →
`gleBuildInterpolateFunc`), qui sort par `exit(1)` dès que l'interpolateur de découpage
dépasse 160 mots — ce qu'une interaction de DOOM 3 (7 unités, r/q portés) fait toujours. Le
repli « logiciel » n'en était donc pas un : il tuait le jeu.

## 2. Ce qui change (plugin seul, protocole inchangé)

Règle : on ne rend plus un dessin à Apple parce que l'hôte manque de mémoire ; on fait ce que
fait un pilote dont la VRAM est pleine — on **évince**. `POMPPC_GL_TEXPAGE=0` rend l'ancien
comportement (pour comparer) ; défaut 1.

- **Estimation** (`tex_host_est`) : ce que le cœur alloue pour une texture — chaque niveau de
  chaque face en 32 bits par texel, S3TC décompressé (`tex_alloc_level`). Tenue dans
  `PTex.host_bytes` à chaque téléversement ; `tex_resident_bytes` en fait la somme (une marche
  de la liste par image au plus, `G.held` entre deux).
- **Budget appris** (`tex_budget_learn`, `G.tex_budget`, 0 = rien appris, pas de limite) :
  au refus d'une texture nommée (`nomem_seen`), ce que nos autres textures tenaient ; au refus
  d'un `BUF_CREATE`, cela moins la taille du tampon ; à un refus asynchrone (objets inconnus,
  `check_errors`) ou d'une surface ou d'un programme, 15/16 de ce que nous tenions. Il ne fait
  que baisser, plancher 8 Mio.
- **Éviction** (`tex_make_room`, avant le `TEX_CREATE`/`TEX_IMAGE3` d'`upload_texture` et la
  `SURF_TEX` d'`upload_surftex`) : `TEX_DESTROY` des textures choisies par `evict_lru` (extrait
  d'`alloc_tex_id`, mêmes protections : unités du contexte qui dessine, textures « hôte
  seulement » de `COPY_TEX`, la texture en cours) jusqu'à ce que le téléversement tienne. Le
  flux est FIFO : les dessins déjà écrits qui échantillonnaient la victime passent avant ; les
  niveaux restent dans l'invité, la texture revient au prochain dessin qui la lie.
- **Accès cyclique** : si même la plus ancienne candidate a servi à l'image en cours
  (`G.tex_clock_frame`), l'image entière ne tient pas — le LRU évincerait chaque texture avant
  son retour. On prend alors la plus récente (MRU) : le début de l'image reste résident.
- **Plus de repli Apple** sous `G.texpage` : `texture_uploadable_full` ne refuse plus pour
  `nomem_until` ; `ensure_surface` et `prog_ensure` ne se ferment plus pendant la fenêtre de
  pression et, refusés, font de la place (8 octets par pixel ; 8 × le texte, 1 Mio au moins)
  puis redemandent **une** fois. Ce que l'hôte refuse encore est dessiné par l'hôte unité
  coupée (texture incomplète, comme en OpenGL), jamais par Apple.
- **Pas** d'éviction pour une réserve `DRAW_NATIVE` de plus (`rp_create`) : essayé — à 128 Mio
  DOOM 3 en demandait deux autres (16 Mio chacune), le budget des textures tombait de 77 à
  42 Mio et chaque image retéléversait ~60 Mio (970 000 évictions en 4 000 images).
  L'empaquetage reste rendu par l'hôte.
- Compteurs : ligne `TEXPAGE image N` de la note toutes les 500 images (budget, tenu,
  évictions, Kio évincés, niveaux téléversés, reprises, `NO_MEM` nommés), lignes
  `TEXPAGE budget …` à chaque apprentissage, bilan `texture paging` de `POMPPC_GL_STATS`.

## 3. Épreuves

VM de l'agent : copie `disks/tiger-r5.qcow2` du disque quotidien (10.4.11, DOOM 3 Demo),
`run_tiger.sh` en `HEADLESS=1`, SMP 2, binaire de référence `~/src/qemu/build`, port 2300,
cellule `d3-fen` de `tools/matrice/matrice.py` pointée sur cette VM (`MATRICE_MON`,
`TSSH_PORT`, `MATRICE_BIN`). Relevés dans `bench/r5/` (hors dépôt). Hôte chargé (3 VM
d'endurance) : aucune conclusion de vitesse.

| Épreuve | Verdict | Preuve |
|---|---|---|
| 128 Mio, plugin `20261014-r5` (final) | **survit** | 833 refus au chargement (image 249), puis 6 `NO_MEM` nommés ; budget appris 90,8 → 78,5 Mio, convergé à l'image 1421 ; 3 396 évictions en tout (1 Go), ~0,2 par image après l'image 2000 ; jeu en marche jusqu'au `kill` de la matrice (image 5 055), aucun `exit`, aucune faute. `bench/r5/m128-b7/d3-fen` |
| 128 Mio, image | **juste** | rejeu du vidage contre la capture de la VM : 0,00 / 0,00 % ; rejeu de la référence : 0,42 / 0,01 % (tolérances 0,3/0,2 % et 0,5/0,1 %) ; replis dans la fenêtre : 33 (admis). `capture-zone.png` : Mars City Hangar, identique à celle de 512 Mio |
| 128 Mio, `POMPPC_GL_TEXPAGE=0` (même binaire) | **meurt** comme le 07/10 | « signal caught: Segmentation fault … double fault » à l'image 288 ; CrashReporter : `exit` ← `gleBuildInterpolateFunc` ← `gleClipPoly` ← `gleVPRenderTriangles` ← `RB_ARB2_CreateDrawInteractions`. `bench/r5/m128-texpage0/d3-fen` |
| 512 Mio, plugin `20261014-r5` | **rien ne change** | 0 ligne `NOMEM`/`TEXPAGE` dans la note (la pagination n'agit pas) ; image : rejeu/VM 0,00 / 0,00 %, référence 0,42 / 0,01 % ; replis 37 (admis). `bench/r5/m512-b7/d3-fen` |

Les cellules sont « rouges » pour des raisons hors R5 : vitesse (hôte chargé et backend sur
llvmpipe, §4), référence jamais validée (`--valider d3-fen`, déjà le cas avant), et
« rapatriement en échec » parce que QEMU meurt à la fin de la cellule (§4) — les données ont
été rapatriées à la main après redémarrage puis analysées par `matrice.py --analyse`.

Mises au point en route (chacune vue en VM) : la réserve `DRAW_NATIVE` évinçant les textures
(retirée, §2) ; le LRU seul en accès cyclique (MRU, §2) ; l'apprentissage qui retranchait toute
la taille d'une texture refusée alors qu'elle tenait dans le budget (78 → 67 Mio d'un coup, puis
38 évictions par image) — désormais un quart, 1 Mio au moins ; et la destination de `COPY_TEX`
(`_currentRender`, 1024×512) relue dans l'invité après un refus : faute rattrapée par le plugin,
mais CrashReporter ouvrait « Doom 3 Demo a quitté inopinément » par-dessus la fenêtre et toute la
présentation directe tombait (un Swap60 chez Apple par image) — elle repart désormais noire
(`copy_dst`), la copie suivante la remplit.

## 4. Ce que l'épreuve a mis au jour (hors R5)

- **QEMU meurt à la fin de toute cellule DOOM 3 avec vidage** : `double free or corruption`
  dans `gl_prog_destroy` → `DeleteProgramsARB` (Mesa `libgallium` 26.0.8), appelé par
  `qgpu_core_client_reset` → `prog_set_free` quand le jeu est tué après le vidage. Pile prise
  sous gdb (`bench/r5/qemu-crash-*.log`). **Reproduit avec l'ancien plugin `20261007c` à
  512 Mio** (`bench/r5/qemu-crash-512-ancien-gdb.log`) : ce n'est pas la pagination. Pas
  reproduit par `qgpu_replay` sous ASan (vidage rejoué puis `qgpu_core_client_reset`).
- **Le backend GL de qgpu tourne sur llvmpipe** dans ces VM : 16 fils `llvmpipe-N` dans le
  processus QEMU, `libgallium` dans la pile ; `eglGetDisplay(EGL_DEFAULT_DISPLAY)` sous la
  session Wayland d'Ubuntu 26.04 ne prend pas l'EGL de NVIDIA (« pci id … driver (null) »,
  « failed to create dri2 screen »), sans `WAYLAND_DISPLAY` ni `DISPLAY` l'init échoue.
  L'image reste dans les tolérances de la référence NVIDIA (0,42 / 0,01 %).
- **QEMU n'est plus reconstructible sur le PC** : `configure` échoue (`glib-2.0` introuvable,
  en-têtes de développement retirés par la mise à niveau) — un build ASan était prévu dans
  `~/src/qemu-r5` (copie de `~/src/qemu` sans `build*`).
- Pour une VM copiée du disque quotidien : couper l'économiseur d'écran
  (`defaults -currentHost write com.apple.screensaver idleTime -int 0`) — passé 20 min,
  ScreenSaverEngine cache la fenêtre, prend une tranche du kext et fait redémarrer l'invité par
  la matrice (« 1 client du kext restant »).

## 5. Limites

- Le budget ne remonte jamais (un niveau plus léger chargé ensuite reste sous l'ancien budget).
- Les `SURF_TEX` et les programmes GLSL refusés (`GLSL_LINK`) gardent l'ancien repli.
- À 128 Mio la réserve `DRAW_NATIVE` n° 1 est refusée toutes les 120 images (empaquetage, hôte).
