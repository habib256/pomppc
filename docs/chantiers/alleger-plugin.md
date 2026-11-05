# Alléger le plugin de l'invité

> Fiche de chantier ; index et priorités : [`TODO.md`](../../TODO.md).

Actif sur les deux hôtes : chaque pourcent gagné vaut sur x86 et sur arm64 sans portage.

- [ ] **[Plugin — les deux hôtes] Alléger le plugin de l'invité, suite de PACKBITS**
  (`docs/plugin-allegement.md` §8, profil `bench/tcg/ab/xprof-0111-cmr-pb`). Le coût est
  maintenant étalé : `geom_draw_client` 3,7 % du vCPU de cmr, `geom_draw_native` 1,5,
  `send_state` 1,3, `prog_sync` 1,1, `geom_dispatch_locked` 1,1, **`___stub_getrealaddr` 1,0**
  (étiquette à résoudre avant d'en tirer un levier : sur le M4, résolveur du stub de
  `fprintf`, suivi des 82 stubs d'importation ; `docs/re/plugin-symboles-tiger.md`),
  `texture_unit_ok` 0,9, `geom_format` 0,8, `us_fill`/`us_get` 1,3,
  `prog_params_arena`/`prog_state` 1,4 ; dans libSystem, le bloc `0x90131200` fait 1,8 % sur
  cmr et 0,7 % sur d3/mb (identifié sur le M4 dans `memcmp`/`bcmp`, libSystem MD5
  `80719800…` ; appelants et empreinte du PC à vérifier). Méthode du §2 : un levier par
  variable `POMPPC_GL_*`, mode `=2` de contrôle, `plugmd5` 0 écart, A/B des cinq jeux sur le
  PC ; le M4 a le bundle depuis le 01/11 (VM quotidienne), A/B PACKBITS neuf terminé le
  02/11 : UT −3,19 %, cmr −7,08 %, autres écarts inconclusifs (`profil-m4-0211.md` §1).
  S'y rattache l'**avis borné** des
  surfaces non simples (QE, 01/11) : le
  coût de l'avis d'ombre (mb-fen +6 % mesuré sur le PC quand une fenêtre est posée sur le jeu)
  vient d'un avis sur toute la surface ; le relevé M4 du 02/11 n'a pas d'avis actifs en régime
  stable, son coût reste à mesurer sous une fenêtre posée. Le kext connaît le rectangle
  englobant de la partie non visible : le renvoyer par la méthode 6 et n'aviser que lui, puis
  remonter la cadence (fermeture : mb-fen sous fenêtre posée au niveau de
  `POMPPC_GL_QE_NONSIMPLE=0`, bande toujours juste). **Fermeture :** plugin sous 20 % du vCPU
  sur cmr et d3, aucun jeu en perte, matrice verte.

## Ombres de DOOM 3 : piste mesurée le 04/11 sur le M4

[A] À réglage unique sur la copie actuelle : `r_shadows=0` → `1`, **29,221 → 37,039 ms/image**,
zéro repli (`bench/tcg/ab/m4-d3-ombres-0411`, une partie par bras). Les anciens 29 ms étaient
sans ombres ; l'écart est expliqué, ce n'est pas une accélération obtenue en les désactivant.
La qualité courante est conservée avec `r_shadows=1`.

Le profil invité avec ombres montre `RB_StencilShadowPass` dans 114/863 échantillons du fil
principal (13,2 %), dont **65/114 via `geom_draw_client`**. On y voit `geom_draw_native`,
`va_scan_idx`, copies, `send_state`, `sync_to_host` et `geom_send_all`. Piste : alléger ce
chemin géométrique **générique**, sans modifier le rendu des ombres. Pas de gain récupérable
encore prouvé ; les obligations restent `plugmd5`, images et A/B de toute la matrice.
Profils hôte et limites des estimations dans [`d3-macos272.md`](d3-macos272.md).
Les compteurs du même niveau donnent aussi 2,7 fois plus de mots de commandes et deux vidages
par image au lieu d'un, tampon plein ; attente des barrières +0,73 ms/image sur le bloc observé.
À distinguer du travail de préparation des lots : aucun gain d'un tampon plus grand n'est encore mesuré.

## STPATCH expérimental (04/11, éteint)

Le prototype remplace les blocs froids ne changeant que cull/stencil par les clés canoniques
existantes ; ni protocole ni QEMU modifiés. Preuve native différentielle et mutants,
contrôles VM partiels du premier prototype dans [`plugin-stpatch.md`](../plugin-stpatch.md).
La révision b1 rétablit la réservation même sans clé ; à reconstruire et valider dans Tiger.
**A/B du 05/11 : d3 −2,16 % (0,8 ms sur les 8 ms des ombres), ut, cmr, mb, zen sans écart prouvé.
Toujours éteint** : détail et reste à faire à la fin de [`plugin-stpatch.md`](../plugin-stpatch.md).
Les aplats blancs du titre de CMR étaient un autre défaut, corrigé (`20261104-texblank`).

## Piste proposée le 01/11 (à arbitrer par l'utilisateur)

2. **Régime de compilation du plugin de l'invité.** Le plugin fait 11-28 % du vCPU selon le jeu :
   10 % de mieux sur lui vaut 1-3 % sur chaque jeu, sur les deux hôtes, sans portage. Leviers :
   PGO et LTO du plugin lui-même (le gcc 4.0 de Tiger a `-fprofile-generate/-fprofile-use` ;
   profil pris en jouant la matrice dans la VM de dev), appels internes directs (`static`,
   visibilité cachée, gain à mesurer ; l'étiquette `___stub_getrealaddr` à 1 % sur cmr doit
   d'abord être résolue sur ses adresses, `docs/re/plugin-symboles-tiger.md`),
   configuration lue une fois (A5 : plus de `getenv` dans le chemin chaud). **Fermeture :**
   plugmd5 0 écart, A/B des cinq jeux, tous en gain. Coût : deux à trois jours (agent Opus).
   Risque : la maturité du PGO de gcc 4.0 sur PowerPC.
