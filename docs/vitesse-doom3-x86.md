# DOOM 3 sur le PC x86-64 : où passe le temps (03/10/2026)

PC Linux (i7-10700F, 8 cœurs / 16 fils, RTX 4060 Ti, Ubuntu 24.04, noyau 6.14), VM quotidienne
`disks/tiger.qcow2` (**Tiger 10.4.11**, SMP 2, 768 Mio, fenêtre QEMU native), QEMU de référence
11.1.2 (`~/src/qemu`, série 0001-0020), paquet invité du 02/10 (plugin `20261001-tout`, kexts
POMPPCGPU et POMPPCFsqrt). Cellule `d3-fen` de la matrice (`demo_mars_city1`, joueur immobile,
fenêtre T+50..T+280), sans vidage. But : comprendre pourquoi DOOM 3 est 2,5 à 2,7 fois plus lent
que sur le Mac M4 (56-61 ms/image) quand les autres jeux ne le sont que 2 à 2,3 fois, et chiffrer
les leviers de l'hôte. Preuves : `bench/vitesse/d3-x86/` et `bench/tcg/ab/x86-*` (hors git).

## 1. État de la VM

| contrôle | résultat |
|---|---|
| Économiseur d'énergie de l'invité (`pmset -g`) | coupé (`sleep 0`, `displaysleep 0`, `disksleep 0`) |
| `POMPPCFsqrt` (`kextstat`) | chargé (~30 s après le démarrage, par son StartupItem ; `Info.plist` déjà réécrit en 8.11.0) |
| options de `run_tiger.sh` | toutes actives : `x-fast-fp x-sr-tlb x-lfs-inline x-vfp-fast x-vperm-fast x-fp-inline x-fp-native x-fp-native64 x-tb-fast x-ret-inline x-jc-idx x-icbi-sync x-msr-nobql`, `x-jit-near`, `x-jc-bits=14` |
| hôte | AVX2 et FMA3 (donc `x-fp-native*`, `x-vfp-fast` par FMA3 actifs), source d'horloge `tsc` (`x-tb-fast` actif), THP `madvise` |

**Inactive malgré l'annonce : `x-jit-near`.** La trace de démarrage dit
`tcg: tampon JIT 0x765ee4000000-0x765f24000000 (x-jit-near : pas de place, noyau), texte
0x58957d2646f0 : AUTRE fenêtre de 4 Gio`. Sous Linux, `mmap(NULL, …)` place le tampon en haut de
l'espace (0x76…/0x7f…), à ~30 Tio du texte de QEMU (exécutable PIE en 0x55…-0x59…) ; la
recherche du patch 0006 (réductions successives de 64 Mio) ne donne jamais d'indication
d'adresse et échoue toujours. Conséquence sur x86-64 : `tcg_out_branch` ne peut pas émettre
`call rel32` (±2 Gio) et émet `call *[rip+pool]` pour **tous** les appels du code généré
(helpers, chemins lents ld/st, `helper_lookup_tb_ptr`). Mesure : §5.3.

Pas d'`x-lmw-inline` : il n'existe que dans `patches/tcg/essais/0002-ppc-lmw-inline.patch`, hors
série.

## 2. Mesure de départ

`tools/tcg/matab.sh x86-pin 3 "ref:" "pin:PIN=1" d3 fen` (bras `ref` = configuration par défaut,
QEMU relancé à chaque partie, hôte au repos, 0 autre QEMU) et le tour de profil :

| partie | ms/image | fenêtre |
|---|---|---|
| `10411-d3c` (tour précédent) | 150,5 | — |
| profil (`-perfmap`, `--sample`) | 151,4 | 2606..2836 |
| `ref-1` / `ref-2` / `ref-3` | 152,0 / 144,6 / 148,7 | 2626.. / 2645.. / 2700.. |

**Médiane 148,7 ms/image (144,6..152,0, dispersion 5 %)**, contre 56,1 sur le M4.
Plein écran : non mesuré (voir §6).

## 3. Le fil principal du jeu (dans l'invité)

`sample` de 10 s après la fenêtre, partagé par `tools/re/partfil.py` ; les listes de symboles de
l'invité sont tirées des binaires de la VM par le nouveau `tools/re/machonm.py` (ni la VM
quotidienne ni l'hôte n'ont `nm` ; les symboles importés, `N_UNDF` et `N_PBUD`, sont exclus),
`bench/vitesse/d3-x86/syms/`.

| propriétaire | PC (151,4 ms/image) | M4 (56,1, `vitesse-profil-2026-10-01.md`) | rapport |
|---|---|---|---|
| jeu | 61,9 % · 93,8 ms | 57,5 % · 32,2 ms | **2,91** |
| GLEngine | 6,3 % · 9,5 ms | 7,5 % · 4,2 ms | 2,26 |
| plugin | 31,7 % · 48,0 ms | 35,0 % · 19,6 ms | 2,45 |
| dont appels au kext | 2,3 ms | 1,3 ms | |
| attente | 0,1 % · 0,2 ms | 0,0 % | |

Le plugin et GLEngine sont 2,3-2,5 fois plus lents, le rapport des autres jeux ; **le jeu l'est
2,9 fois**. Arbre d'appels (parts inclusives du fil) :

| sous-arbre | part | ms/image |
|---|---|---|
| `idSessionLocal::UpdateScreen` (rendu : avant + arrière) | 67,2 % | 101,7 |
| dont `RB_ExecuteBackEndCommands` (dessins, plugin) | 40,4 % | 61,1 |
| dont `idGameLocal::Draw` (`R_RenderView`, interactions, ombres) | 26,2 % | 39,6 |
| **`idSessionLocal::RunGameTic` (logique du jeu)** | **32,4 %** | **49,1** |
| dont `idStaticEntity::Think` → GUI du décor (`idWindow::Redraw`, `DrawText`) | 10,0 % | 15,1 |
| dont `idPlayer::Think`, `idAI::Think` | 7,3 % + 4,7 % | 11,0 + 7,2 |

### 3.1 L'explication du « 2,5× au lieu de 2× » : les tics du jeu

DOOM 3 simule le monde à **60 tics par seconde, quel que soit le rythme des images** :
`idSessionLocal::Frame` joue d'un coup tous les tics échus depuis l'image précédente. À
151 ms/image, cela fait **~9 tics par image** (5,4 ms chacun) ; le M4, à 56 ms/image, n'en joue
que ~3,4. Le coût d'une image n'est donc pas proportionnel à la vitesse du processeur émulé :

  T = R / (1 − g / 16,7 ms)    (R : rendu par image, g : coût d'un tic)

Ici R = 102,3 ms et g = 5,4 ms (T = 151,4). Si le PC n'était qu'un M4 k fois plus lent en tout,
le M4 ferait (102,3 + 3,37 × 5,4) / k = 56,1 ms, soit **k = 2,15** : le rapport des autres jeux.
**Le surcoût propre à DOOM 3 sur x86 n'est pas une pathologie de l'hôte : c'est la logique à
60 Hz qui grossit quand l'image ralentit.** Deux conséquences :

1. **Tout gain de vitesse brute est amplifié** : 10 % de vitesse en plus sur le G4 émulé
   donnent 12,9 % de temps d'image en moins (T passe de 151,4 à 131,9 ms) ; 20 %, 23,6 %.
2. **Le coût d'un tic compte double** : il est payé ~9 fois par image. La GUI du décor
   (`idStaticEntity::Think` → `idWindow::Redraw` → `idDeviceContext::DrawText`, 15 ms/image)
   est redessinée à **chaque tic**, pas à chaque image.

Le son du jeu (décodage Vorbis, `mdct_*`, `vorbis_book_decodev_add`) tourne sur un autre fil
de l'invité, sur le second vCPU : ~4 % du code généré exécuté.

## 4. Le processus QEMU (hôte)

`perf record -F 999 -p <QEMU>` 10 s juste après la fenêtre (`matrice.py --sample-hote 10`),
QEMU lancé avec `EXTRA_ARGS="-perfmap -name Tiger,debug-threads=on"` (code JIT nommé par
`/tmp/perf-<pid>.map`, fils nommés), réparti par le nouveau `tools/re/perfpart.py`
(`bench/vitesse/d3-x86/profil-1/d3-fen/perf-hote-mesure.data`).

| fil | part d'un cœur | fréquence pendant l'occupation |
|---|---|---|
| CPU 0/TCG | 60 % | 4,65 GHz |
| CPU 1/TCG | 51 % | 4,64 GHz |
| boucle principale | 38 % (beaucoup de réveils courts) | 0,76 GHz effectifs |
| qgpu-render | 15 % | 4,57 GHz |

Les deux vCPU se partagent le fil du jeu (60 + 51 %, comme sur le M4 : 52 + 55 %) ; **1,12 cœur
occupé, 169 ms de vCPU par image**. Les cœurs tournent à 4,65 GHz sous le gouverneur
`powersave` (intel_pstate, HWP, `balance_performance`) : le turbo tous cœurs du 10700F est
4,6 GHz, le maximum 4,8.

| poste (fils vCPU) | part du temps vCPU | ms vCPU/image | M4 (part, `vitesse-profil-2026-10-01.md`) |
|---|---|---|---|
| code généré (JIT) | 68,3 % | 115,4 | ~2/3 |
| softmmu : TLB, remplissages (`ppc_hash32_xlate`, `tlb_set_page_full`), accès lents, `probe_access*` | 11,2 % | 18,8 | `helper_ldul_mmu` 2,2 % (appels directs seuls) |
| AltiVec (`vfp_fma4_fma3`, `helper_vmaddfp`, `vsldoi`, `vperm_fast_avx`…) | 5,3 % | 9,0 | 6,6 % |
| recherche de blocs (`qht_lookup_custom`, `tb_lookup`, `helper_lookup_tb_ptr`) | 5,0 % | 8,5 | 4,1 % |
| flottant (softfloat : `float32_compare_quiet`, `float64_unpack_canonical`, FPRF…) | 3,3 % | 5,5 | ~1 % (double) |
| noyau Linux | 1,6 % | 2,7 | — |
| `lmw`/`stmw` | 1,5 % | 2,6 | 5,4 % |
| boucle d'exécution | 1,1 % | 1,8 | — |
| base de temps (`tbf_load`) | 1,0 % | 1,7 | 2,0 % |
| autres | 1,6 % | 2,6 | — |

**Même forme que sur le M4** : deux tiers de code généré, un tiers de helpers. Aucun poste propre
à x86 ne ressort ; le surcoût est uniforme (vitesse brute d'un cœur, ×2,15) puis amplifié par
les tics (§3.1). Fonctions les plus lourdes : `qht_lookup_custom` 2,0 %, `probe_access_internal`
1,9 %, `tb_lookup` 1,7 %, `vfp_fma4_fma3` 1,3 %, `probe_access` 1,0 %, `address_space_ldm_internal`
0,95 % (lectures des PTEG au remplissage du TLB), `tbf_load` 0,8 %, `helper_vmaddfp` 0,8 %,
`helper_lmw`/`stmw` 0,8 + 0,75 %.

Code généré par région de l'invité : **DOOM 3 lui-même 54 %** (`idSIMD_AltiVec::DeriveTangents`
3,9 %, `R_LocalPointToGlobal` 2,3 %, `PushVolumeIntoTree_r` 1,8 %, `idDeviceContext::DrawStretchPic`
1,6 %, `ClippedCoords` 1,2 %, `DrawText` 1,1 %, Vorbis ~4 %), 0x08xxxxxx (bibliothèques chargées,
plugin) 23 %, 0x0axxxxxx 10 %, bibliothèques système 5 %, commpage 3 %.

Boucle principale : 14 % de son temps dans `tlb_reset_dirty` (suivi des pages sales de la VRAM
pour l'affichage VGA, qui repasse les TLB des deux vCPU), le reste en noyau (poll), pixman et
glib. Fil de rendu : pilote NVIDIA 39 %, `do_draw_native` 12 % ; `nat_conv_attr` n'apparaît pas
dans ce relevé (le rendu ne pèse que 15 % d'un cœur et le jeu ne l'attend pas : attente GL
0,2 ms/image).

## 5. Leviers de l'hôte et de la compilation (étape 1)

Chaque levier en A/B entrelacé (`tools/tcg/matab.sh`, désormais à N bras), QEMU relancé à chaque
partie, hôte au repos.

### 5.1 Épinglage des vCPU (`PIN=1`)

`bench/tcg/ab/x86-pin` : vCPU sur les cœurs physiques 2 et 4 (leurs frères SMT 10 et 12
laissés libres), les autres fils de QEMU ailleurs.

| bras | parties (ms/image) | médiane |
|---|---|---|
| `ref` | 152,0 / 144,6 / 148,7 | 148,7 |
| `pin` | 146,4 / ~~58,7~~ / 144,6 | 145,5 (deux parties) |

**`pin-2` n'est pas une mesure** : vers l'image 3200 la vue du jeu change (50-60 ms/image dès
3350, la scène n'est plus celle des autres parties), et la fenêtre est tombée à 3313..3543 au
lieu de ~2650. Sur les deux parties valables, l'écart (−2 %) est dans le bruit de `ref` (5 %).
**Pas de gain mesurable** : le noyau place déjà les deux fils occupés sur des cœurs distincts
(16 fils logiques pour ~2,5 cœurs occupés). `PIN=1` reste dans `run_tiger.sh`, éteint.

### 5.2 Pages énormes

Sans objet : avec THP en `madvise`, QEMU marque la RAM de l'invité `MADV_HUGEPAGE` ; relevé
après une partie, la zone de 768 Mio a **669 696 kio résidents, tous en pages énormes**, et les
tranches de 64 Mio du tampon JIT aussi (63 488 kio sur 65 532 chacune). `-mem-path` n'apporterait
rien.

### 5.3 Appels directs du code généré (`x-jit-near` sous Linux)

Campagne `bench/tcg/ab/x86-c1` (six bras entrelacés, 3 parties chacun, QEMU relancé à chaque
partie, hôte au repos ; médianes, entre parenthèses min..max) :

| bras | binaire, réglage | ms/image | contre `ref` |
|---|---|---|---|
| `ref` | référence `~/src/qemu` | 150,2 (146,0..151,9) | — |
| `t0` | `~/src/qemu-d3tcg` (tcg/0021-0024), propriétés éteintes (0023 seul actif) | 148,7 (147,8..148,8) | −1,0 % (bruit) |
| `r32` | idem + `x-jit-rel32` (`JITREL32=1`) | 144,5 (144,0..147,9) | −3,8 % ; **−2,8 % contre `t0`** |

`x-jit-rel32` (tcg/0024) pose le tampon du JIT 1,1 Gio sous le texte de QEMU (« appels des
helpers directs (rel32) », vérifié dans chaque `run_tiger.log`). Le gain est réel mais modeste,
loin des 5 % espérés : le prédicteur de l'i7 résout bien les `call *[rip+pool]`, dont la cible
est fixe ; le coût restant d'un appel est celui du helper lui-même.

### 5.4 Gouverneur du processeur

Non mesuré : changer de gouverneur demande la racine. Sous `powersave` (intel_pstate actif,
HWP, préférence `balance_performance`), les fils vCPU tournent déjà à **4,65 GHz pendant leur
occupation** (§4), le turbo tous cœurs du 10700F (4,6 GHz) ; le maximum à un cœur est 4,8 GHz.
Le gain possible est donc au plus de quelques pour cent, surtout sur la latence de réveil des
fils peu occupés (boucle principale, rendu). Pour l'essayer (à défaire au redémarrage) :

    echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
    tools/tcg/matab.sh x86-gov 3 "perf:" "perf2:" d3 fen    # puis le même en powersave

(A/B à jouer en deux campagnes successives, le gouverneur étant global à l'hôte.)

### 5.5 Compilation de QEMU

Variantes construites par `QEMU_OPT` (`scripts/build_qemu_qfb.sh`) dans l'arbre séparé
`~/src/qemu-opt` (source identique à la référence), même campagne `x86-c1` :

| bras | `QEMU_OPT` | ms/image | contre `ref` |
|---|---|---|---|
| `ref` | (aucune : `-O2`, durcissements de QEMU) | 150,2 (146,0..151,9) | — |
| `lto` | `native,nohard,lto` : `-march=native`, sans `-fzero-call-used-regs` ni `-ftrivial-auto-var-init`, sans protection de pile, LTO | 138,1 (137,7..140,2) | **−8,1 %** |
| `pgo` | `native,nohard,lto,pgo` : idem + profil d'une partie de DOOM 3 (`pgo-gen`, 25 min de jeu, 1 356 `.gcda`) | **130,8 (129,1..131,1)** | **−12,9 %** |

**`-O3` dans `--extra-cflags` n'agit pas** : meson ajoute `-g -O2` après (ligne `CFLAGS` du
configure : `-O3 -march=native … -g -O2`), le dernier `-O` l'emporte ; les binaires de ce tableau
sont donc en `-O2 -march=native`. Le vrai `-O3` (`-Doptimization=3`, que `native` pose
désormais) a été mesuré à la fin, sur la configuration complète (`bench/tcg/ab/x86-o3`, copie TCG
en PGO réentraîné, tout allumé) : **115,3 (115,1..117,3) contre 118,1 (116,8..118,4) ms/image,
−2,4 %**. Les étendues ne se recouvrent pas : c'est le plus gros levier de l'hôte, sans une ligne de code
de l'émulateur. Un binaire `native` n'est valable que sur CE processeur (AVX2, FMA3 ; il ne
démarrerait pas sur un x86 plus ancien). La justesse ne dépend pas des options (ni
`-ffast-math` ni rien qui touche au flottant ; softfloat et les chemins natifs gardent leurs
tests) ; les épreuves du §7 ont tourné sur la référence et sur la copie TCG.

**Piège de la construction** (gel du PC le 03/10 à 19:39) : sous `lto`, ninja liait
`qemu-system-ppc` et `qemu-system-ppc64` en même temps, chacun avec 16 processus `ltrans`
(`-flto=auto`), et le profil PGO les rend bien plus gros : 40 Go de RAM et le swap saturés, plus
rien ne répond, journal coupé net. Corrigé : `-Dbackend_max_links=1 -Db_lto_threads=$LTO_JOBS`
(6 par défaut) ; la reconstruction PGO a culminé à 4,2 Go. Construire sous plafond reste prudent :
`systemd-run --user --scope -p MemoryMax=28G -p MemorySwapMax=2G …` (le noyau tue la construction
au lieu de geler la machine).

Le profil peut resservir à un arbre voisin (la copie TCG) : renommer les `.gcda`
(`#qemu-opt#` → `#qemu-d3tcg#`) ; les fichiers qu'un patch a changés sont alors compilés sans
profil (`-Wno-error=coverage-mismatch`, posé par `QEMU_OPT=…,pgo`).

### 5.6 `r_useIndexBuffers 1` (demande de l'agent plugin)

Bras `ib` de `x86-c1` (`D3_SET=r_useIndexBuffers=1`, binaire de référence) : **162,5 ms/image
(160,6..166,3), +8 % : plus lent**. Les indices passent alors par un VBO d'éléments que le jeu
réécrit à chaque image ; la note montre des `NATSHM repli ligne 12201` (`raw_ensure`/`raw_sync`
du tampon d'indices ratés : le dessin retombe sur l'empaquetage). À ne pas utiliser ; les
indices en mémoire cliente restent le meilleur cas pour le plugin d'aujourd'hui (§8).

## 6. Le traducteur sur x86 (étape 2 : tcg/0021-0024)

Conception : `docs/tcg-g4.md` §28. Copie `~/src/qemu-d3tcg` (référence + 0021-0024), construite
comme la référence (`build/`) et en `native,nohard,lto,pgo` (`build-native-nohard-lto-pgo/`,
profil de `~/src/qemu-opt` renommé : les fichiers patchés, dont `translate.c` et `int_helper.c`,
sans profil). Mesures : §6.2.

### 6.1 Ce que valent les patches, isolément (bancs invités, VM de dev)

| banc (2 M itérations, `bench/vitesse/d3-x86/preuves-tcg2`) | référence | copie, propriétés éteintes | `x-vmx-inline` + `x-vfp-native` |
|---|---|---|---|
| `vfptest` : 2 `vmaddfp` + `vaddfp` + `vsubfp` | 61 ms | 61 ms | **21 ms** (×2,9) |
| `vmxtest` : `vsldoi` + `vmrghw` + `vmrglw` + `stvewx` | 79 ms | 78 ms | **19 ms** (×4,1) |
| `vfptest` : 4 `vperm` (témoin, inchangé) | 19 ms | 19 ms | 18 ms |

**Piège de la preuve** : le chemin court de `x-vfp-fast` comme de `x-vfp-native` exige
`!vec_status.no_hardfloat`, que `cpu_init.c` pose à `!fast_fp`. Sans `x-fast-fp=on` (que
`run_tiger.sh` met toujours), la porte est fermée : la première série d'épreuves
(`preuves-tcg`, `-cpu g4,x-vfp-fast=on,x-tb-fast=on`) a vérifié 42 M opérations dont **0** par
le chemin court, et le banc ne gagnait rien. Les épreuves se jouent avec les options de
production complètes (`preuves-tcg2`).

### 6.2 En jeu : DOOM 3, poste par poste (campagne `x86-c2`)

Binaire TCG en `native,nohard,lto,pgo` pour tous les bras `q*`, propriétés ajoutées une à une ;
`pgo` = la variante PGO sans les patches (meilleur bras de `x86-c1`). 3 parties par bras,
entrelacées, QEMU relancé à chaque partie, hôte au repos (charge < 1,8, 0 autre QEMU).

| bras | réglage | ms/image (min..max) | pas | contre `ref` (150,2, `x86-c1`) |
|---|---|---|---|---|
| `pgo` | `~/src/qemu-opt`, PGO | 130,2 (129,3..134,2) | — | −13,3 % |
| `q0` | copie TCG en PGO, tout éteint | 130,8 (129,7..134,0) | ≈ (profil incomplet, §6) | −12,9 % |
| `q1` | + `JITREL32=1` | 127,8 (124,6..130,7) | −2,3 % | −14,9 % |
| `q2` | + `VMXINLINE=1 VFPNATIVE=1` | 122,9 (121,4..126,2) | −3,8 % | −18,2 % |
| **`q3`** | + plugin `IDXLAZY=1 IDXVEC=1 DISPONE=1 UNITVD=1` (§8) | **118,8 (117,7..121,4)** | −3,3 % | **−20,9 %** |

Les pas sont du même ordre que la dispersion d'un bras (3-5 %) mais vont tous dans le même sens
et les médianes s'ordonnent comme l'empilement : la somme est nette (`q3` contre `pgo` : −8,8 %,
étendues disjointes). Avec l'amplification des tics (§3.1), ~5 % de vitesse brute du G4 émulé
donnent ces ~6 % d'image.

## 7. Épreuves d'exactitude

Toutes ont tourné cette nuit, sur le PC.

| épreuve | où | résultat |
|---|---|---|
| `vfptest` (catalogue 40³ × NJ 0/1, 2²² vecteurs aléatoires × 4 instructions, `vperm`) | VM de dev, 4 modes : référence ; copie éteinte ; `x-vfp-native` + `x-vmx-inline` ; idem + `-verify` + `x-tb-verify` | **empreinte identique** dans les 4 (`a4831c30fd7c` hors lignes de banc) |
| `vmxtest` (`vsldoi` aux 16 décalages, `vmrg[hl]w`, recouvrements, `stve[bhw]x` aux 16 adresses, fautes) | idem | **empreinte identique** (`d524863a5ee3`) |
| vérificateurs, VM de dev | mode `-verify` | `vfp-native-verify` 42 066 433 opérations (16 573 258 par le chemin court), `vmx-verify` 81 M permutations + 14,6 M rangements, `tb-verify` 341 M lectures : **0 divergence**, 0 lecture fausse |
| **une partie de DOOM 3 entièrement vérifiée** (`bench/tcg/ab/x86-chk`, binaire TCG en PGO, `JITREL32 VMXINLINE VFPNATIVE VMXVERIFY VFPNVERIFY TBVERIFY`, plugin `IDXLAZY=2 IDXVEC=2 DISPONE=1 UNITVD=1 VERDICTCHECK=1`, 5 000 images) | VM quotidienne | `vfp-native-verify` **2 349 520 191** opérations (2 328 050 594 par le chemin court), `vmx-verify` **2 554 622 690** permutations + 174 751 488 rangements, `tb-verify` 2 098 215 787 lectures : **0 divergence** ; plugin : 2 164 116 dessins sans balayage contrôlés, 3 138 657 balayages AltiVec contrôlés, 1 483 536 verdicts par unité comparés au complet, 641 288 verdicts gardés contrôlés : **0 écart** |
| binaire final `build-pgo3` (`-O3`, PGO), `vfptest` + `vmxtest` (`preuves-o3`) | VM de dev, propriétés allumées sans puis avec vérificateurs | empreintes **identiques** (`a4831c30fd7c`, `d524863a5ee3`) ; `vfp-native-verify` 42 M (16,6 M par le chemin court), `vmx-verify` 96 M, `tb-verify` 1,7 G : **0 divergence** |
| `gltest`, 59 scènes, plugin `20261003-d3x` (job `plugd3x`) | VM de dev (référence) | 7 modes (défaut ; tout en contrôle ; tout allumé ; chaque levier seul) : **md5 identiques au mode défaut pour les 59 scènes** ; les échecs connus (ARB, GLSL en single-user) les mêmes partout |

**Mutants de l'émetteur réel** (`bench/vitesse/d3-x86/preuves-mutants`) : chacun construit dans
la copie TCG (`ninja` incrémental, binaires dans `~/src/qemu-d3mut/` avec un lien `qemu-bundle`),
joué dans la VM de dev avec les options de production, propriétés allumées sans puis avec
vérificateur :

| mutant | patch | empreinte (`-verify` éteint) | vérificateur |
|---|---|---|---|
| m1 : `vnmsubfp` par `vfmadd231ps` | 0022 | **changée** (`7f0d7e2167d3`) | 1 492 011 divergences |
| m2 : borne du résultat jusqu'à l'infini (`0xfe7fffff`) | 0022 | identique — attendu : seul le drapeau overflow de `vec_status` diffère, invisible en AltiVec | **1** divergence |
| m3 : borne basse à 1 (dénormaux gardés) | 0022 | **changée** (`d0302f994ef7`) | 74 794 divergences |
| m4 : `stve*x` prend l'autre double mot (`movcond` EQ) | 0021 | **changée** (`258a04667b4e`) | 13 566 362 divergences (rangements) |
| m5 : `vsldoi` décale à l'envers (`extract2 … s`) | 0021 | **changée** (`2ed19ee170f4`) | 50 331 648 divergences (permutations) |

**5 mutants sur 5 détectés par les vérificateurs, 4 sur 5 par l'empreinte** (m2 n'a pas d'effet
visible par construction). Pour m1, m3 et m5, le vérificateur réécrit la valeur de référence :
l'empreinte redevient `a4831c30fd7c` / `d524863a5ee3` sous `-verify`.

Reste la **parité arm64** de 0022 : l'émetteur aarch64 n'a jamais été compilé (ni chaîne aarch64
ni clang sur le PC) ; à compiler et prouver sur le M4 (mêmes épreuves, mêmes mutants) avant
tout usage là-bas. 0021, 0023 (code commun) y valent tels quels ; 0024 est propre à Linux x86-64.

## 8. Le plugin (étape 3)

Conception : `docs/d3-plugin-x86.md` (branche `d3-x86-plugin`). Plugin `20261003-d3x` installé
dans la VM quotidienne le 04/10 vers 0 h 50, leviers éteints par défaut (l'ancien,
`20261001-tout`, est gardé dans `~tiger/GLDriver-POMPPC.bundle.20261001-tout` de l'invité).
Taux de reprise mesurés dans DOOM 3 (5 000 images) : IDXLAZY évite le balayage pour **32 %** des
dessins indexés (2,16 M sur 6,71 M ; les autres lisent un VBO au miroir sale), IDXVEC prend les
balayages restants de plus de 32 indices (3,14 M sur 4,54 M), DISPONE saute le crochetage à
**99,9 %** des dispatches, UNITVD remplace le verdict complet dans **73 %** des cas où il est
tenté. Mesure : `q3` contre `q2` au §6.2, **−3,3 %** (122,9 → 118,8 ms/image).

## 9. Bilan : tout ensemble (campagne `x86-c3`)

Dans une même campagne (3 parties par bras, entrelacées) :

| bras | binaire | réglages | ms/image (min..max) | contre `ref` |
|---|---|---|---|---|
| `ref` | référence `~/src/qemu/build` | défauts | 149,9 (149,4..151,4) | — |
| `q3` | copie TCG, PGO au profil renommé (`build-native-nohard-lto-pgo`) | `JITREL32 VMXINLINE VFPNATIVE` + plugin `IDXLAZY IDXVEC DISPONE UNITVD` | 117,5 (114,2..117,5) | **−21,6 %** |
| `r3` | copie TCG, PGO **réentraîné** sur elle-même avec tous ces réglages (`build-pgo2`, 25 min de DOOM 3, 1 421 `.gcda`) | idem | **116,9 (116,6..118,7)** | **−22,0 %** |

**DOOM 3 passe de ~150 à ~117 ms/image sur le PC** (8,5 images/s au lieu de 6,7), et à
**115,3 ms/image (−23 %) en vrai `-O3`** (§5.5), contre 56-61 sur le M4 : l'écart tombe de
2,5-2,7× à ~2,0×, le rapport des autres jeux (§3.1). Réentraîner le
profil sur la copie TCG n'apporte rien de mesurable (`r3` contre `q3` : −0,5 %, dans le bruit) :
le profil d'un arbre voisin suffit.

D'où vient le gain, en cumulé (`x86-c1`, `x86-c2`) : compilation (`-march=native`, sans
durcissements, LTO, PGO) ~13 %, `x-jit-rel32` ~2 %, `x-vmx-inline` + `x-vfp-native` ~4 %, plugin
~3 %.

## 10. Pour la suite

1. **Défauts — fait le 04/10** : `VMXINLINE=1` partout, `VFPNATIVE=1` sur Linux x86-64
   (**pas `JITREL32`**, §12), les quatre leviers du plugin à 1 (révision `20261004-d3x`), binaire de
   référence `~/src/qemu` reconstruit avec 0021-0024. Ce qui était prévu : `JITREL32`, `VMXINLINE` et `VFPNATIVE` peuvent passer à 1 dans
   `run_tiger.sh` sur Linux x86-64, les quatre leviers du plugin à 1 (`*_DEFAULT`) — sur le M4
   seulement après la parité arm64 de 0022 et un A/B là-bas.
2. **Binaire PGO du PC.** `QEMU_OPT=native,nohard,lto,pgo` reste une variante (binaire propre au
   processeur, profil à refaire à chaque changement notable de la série) : soit en faire le
   binaire quotidien du PC (`QEMU_BIN` des lanceurs), soit garder la référence et ne s'en servir
   que pour jouer. Le meilleur binaire mesuré : `~/src/qemu-d3tcg/build-pgo3/` (`-O3`, PGO
   réentraîné, `PGO_DIR=pgo-data3`). **06/10 : choisi la première voie**, sans toucher à la
   référence : `QEMU_FAST=1 ./scripts/build_qemu_qfb.sh` construit `~/src/qemu/build-fast/` à
   côté de `build/`, `run_tiger.sh` le prend s'il est à jour (`QEMU_FAST=0` : référence), le
   profil s'entraîne par `tools/tcg/pgo-train.sh` : `docs/binaire-rapide-x86.md`.
3. **Gouverneur** `performance` (§5.4) : un A/B à faire, il demande la racine.
4. **Plugin, indices en VBO** (§5.6) : `r_useIndexBuffers 1` serait le cas idéal pour IDXLAZY
   (indices déjà chez l'hôte) mais retombe aujourd'hui sur l'empaquetage ; comprendre le refus
   de `raw_ensure` pour le tampon d'éléments.
5. **Le reste du temps vCPU** (§4) : softmmu 11 % (`probe_access*`, remplissages du TLB), recherche
   de blocs 5 %, logique du jeu à 60 Hz (§3.1 : chaque gain est amplifié).
6. **Parité arm64** de 0022 sur le M4 (§7).

## 11. Non-régression : la matrice du PC, tout allumé (`bench/tcg/ab/x86-mat`)

Un tour de matrice **avec vidage** (preuve d'image : rejeu natif du vidage contre la capture de
la VM, rejeu de la référence) par configuration, `MATAB_VIDAGE=1 tools/tcg/matab.sh x86-mat 1
"ref:" "all:QEMU_BIN=…/build-pgo2/qemu-system-ppc JITREL32=1 VMXINLINE=1 VFPNATIVE=1
POMPPC_GL_IDXLAZY=1 POMPPC_GL_IDXVEC=1 POMPPC_GL_DISPONE=1 POMPPC_GL_UNITVD=1" mb,zen,ut,d3 fen`
(une partie par configuration : la vitesse est indicative, pas un A/B) :

| cellule | `ref` : verdict, ms/image | `all` : verdict, ms/image | image (`all`) |
|---|---|---|---|
| `mb-fen` | vert, 21,5 | vert, **17,5** | juste (0,00/0,00 %) |
| `zen-fen` | vert, 7,8 | vert, **6,3** | juste (0,00/0,00 %) |
| `ut-fen` | vert, 58,7 | vert, **47,9** | juste (0,00/0,00 %) |
| `d3-fen` | rouge (plancher 76), 151,3 | rouge (plancher 76), **116,4** | rejeu contre VM 0,03/0,00 % ; rejeu de la référence 0,00/0,00 % |

Aucune régression ; les trois autres jeux gagnent ~18-19 %, comme DOOM 3. `d3-fen` n'avait pas
de référence sur le PC : le tour `ref` l'a créée (`references-linux.csv`, `validee=non`), à
valider à l'œil (`matrice.py --valider d3-fen`) ; les deux captures montrent la même scène,
justement rendue (`ref-1/d3-fen/capture.png`, `all-1/d3-fen/capture.png`).

**Pour jouer à DOOM 3 au plus vite sur le PC** (depuis le 04/10, les réglages sont par défaut ; reste le binaire PGO) :

    QEMU_BIN=~/src/qemu-d3tcg/build-pgo3/qemu-system-ppc JITREL32=1 POMPPC_FRONTEND=native ./run_tiger.sh

(`JITREL32=1` pour DOOM 3 seulement : voir §12.)

## 12. Mise par défaut (04/10) et le piège de `x-jit-rel32`

Défauts allumés le 04/10 : `x-vmx-inline` partout, `x-vfp-native` sur Linux x86-64 (émetteur
aarch64 non prouvé), les quatre leviers du plugin (révision `20261004-d3x`, installée dans la VM
quotidienne et dans `disks/prebuilt`, CD regravé ; ancien `prebuilt` dans
`bench/devloop/prebuilt-20261002`) ; binaire de référence `~/src/qemu` reconstruit avec 0021-0024.

Le premier tour de vérification avec **tous** les nouveaux défauts (binaire de référence, sans
PGO) a montré UT2004 plus lent ; A/B à 3 parties par bras sur `ut-fen` :

| campagne | bras | ms/image (min..max) |
|---|---|---|
| `x86-ut-def` | nouveaux défauts (dont `JITREL32`) | 62,5 (60,5..63,9) |
| | tout éteint | 57,9 (57,2..58,6) |
| `x86-ut-split` | options TCG seules | 67,2 (65,9..67,4) |
| | plugin seul | 57,1 (55,8..62,1) |
| `x86-ut-tcg` | `JITREL32` seul | **64,1 (57,0..67,2)** |
| | `VMXINLINE` seul | 58,3 (56,7..58,7) |
| | `VFPNATIVE` seul | 58,2 (57,9..59,0) |

**`x-jit-rel32` a deux régimes sur UT2004** (57 ou 64-67 ms/image d'un lancement à l'autre, la
place du tampon changeant avec l'ASLR) ; sur DOOM 3 il gagnait 2-3 % dans les deux campagnes. Même
allure que les « deux régimes » de `x-jit-near` sur le M4 (`docs/tcg-g4.md` §14) : sans doute un
conflit d'adresses entre le code généré et le texte de QEMU (alias de cache ou de prédicteur), à
étudier (`perf stat` des deux régimes). **Il reste éteint par défaut** (`JITREL32=1` pour l'avoir).
Le gain total de DOOM 3 par défaut perd donc ses ~2 % ; le reste est inchangé.

**Tours de vérification avec les défauts définitifs** (sans `JITREL32`, binaire de référence) :
matrice avec vidage `x86-defauts2` : mb 21,0, zen 7,6, d3 138,5 ms/image, images justes (d3 :
référence à valider) ; UT2004 y sort à 76,6 ms/image, mais c'est un **transitoire** de début de
fenêtre (les ~20 premières images à 100-280 ms, puis 40-70 comme d'habitude) quand UT suit le
vidage de DOOM 3 (40 Mo) dans la même VM, vu aussi une fois avant. Sans vidage, DOOM 3 puis UT
dans la même VM (`x86-d3ut`, une partie par bras, campagne arrêtée là) : **défauts d3 137,7 /
ut 56,4 ; tout éteint d3 149,5 / ut 57,3**. Pas de régression d'UT ; DOOM 3 −8 % par défaut sur le
binaire de référence (−23 % avec le binaire PGO et `JITREL32=1`).

**Suite sur le M4** : compiler et prouver l'émetteur aarch64 de 0022 (vfptest, vfpproof,
mutants), A/B de `x-vmx-inline`/`x-vfp-native` et des leviers du plugin là-bas ; comprendre les
deux régimes de `x-jit-rel32` ; valider la référence `d3-fen` du PC.
