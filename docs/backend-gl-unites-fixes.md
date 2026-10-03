# Backend GL sur un hôte NVIDIA : 4 unités au pipeline fixe (QGPU_CAP_FIXED4)

02/10/2026, PC Linux (RTX 4060 Ti, pilote 595.91.07, EGL).

## Symptôme

Sur ce PC, chaque démarrage disait « qgpu: backend gl refusé par l'auto-test : unités de
texture du pipeline fixe » : tout le GPU 3D retombait sur le backend logiciel (couloir de jeu de
`gltest` : 16 images/s au lieu de 626). C'était le cas depuis le protocole v17 (24/09, 8 unités
de texture, mis au point sur le M4).

## Cause

`gl_selftest` exigeait `GL_MAX_TEXTURE_UNITS ≥ QGPU_MAX_UNITS` (8). Le GL d'Apple en donne 8 ;
NVIDIA, en profil de compatibilité, **4** au pipeline fixe, mais 8 jeux de coordonnées et 32 unités
d'image (`/tmp/glunits` : `MAX_TEXTURE_UNITS=4 COORDS=8 IMAGE_UNITS=32`). Et il **ignore en
silence** les unités fixes 4..7 : `glEnable(GL_TEXTURE_2D)` et `glTexEnv` y passent sans erreur
GL, mais le texel ne compte pas (unité 0 blanche en REPLACE, unité N verte en MODULATE : vert pour
N = 1..3, blanc pour N = 4..7). Le refus était donc juste : accepter tel quel aurait faussé
l'image sans rien dire. Le refuser tout entier, en revanche, ne l'était pas.

Bug voisin : `gl_init` pose `COMBINE3`, `OCCLUSION`, `GL14`, `PROGRAMS` et `GLSL` avant l'auto-test
et ne les retirait pas en cas de refus ; le backend logiciel qui prenait la suite en héritait.

## Correction

- **Hôte** (`qgpu-gl.c`) : 4 unités fixes suffisent si les programmes ont 8 coordonnées et 8 unités
  d'image ; le backend annonce alors `QGPU_CAP_FIXED4` (`qgpu_proto.h`, sans changer
  `QGPU_PROTO_VERSION`) et le dit au démarrage. Un dessin du pipeline fixe qui allume une unité au
  delà (plugin plus ancien) est signalé une fois sur stderr. Le chemin d'échec de `gl_init` retire
  ses bits.
- **Invité** (plugin) : avec `QGPU_CAP_FIXED4`, `GL_MAX_TEXTURE_UNITS` annoncé = 4, comme un
  GeForce FX ; `GL_MAX_TEXTURE_COORDS` et `_IMAGE_UNITS` gardent 8 (DOOM 3 lit `texture[0..6]` par
  ses programmes). Un dessin du pipeline fixe qui allumerait une 5ᵉ unité va au rendu d'Apple
  (`UNIT_LIM` : `G.fixed_units` sans programme de fragments, `G.units` avec). Sur le M4 (8 unités
  fixes), rien ne change.

## Preuve (VM de dev 10.4.11, backend GL de la RTX 4060 Ti)

| épreuve | résultat |
|---|---|
| job `gpu` (51 scènes `gltest`, chacune comparée au rendu d'Apple) | **51 OK, 0 échec** ; couloir de jeu 625,96 img/s (16 par Apple) |
| scène `units` (nouvelle : toutes les unités annoncées en MODULATE) | plugin : `GL_MAX_TEXTURE_UNITS = 4`, vert attendu ; Apple seul : 8, vert |
| `prebuilt` régénéré sous 10.4.11 (plugin, kexts POMPPCGPU et POMPPCFsqrt), CD gravé par `xorriso` | monté par Tiger (HFS+), `install.sh` réussit, les deux kexts se chargent au redémarrage |
| Marble Blast sur le backend GL | rendu juste, aucun avertissement d'unité ignorée |

Les VM existantes doivent recevoir le nouveau plugin (CD POMPPCSRC : `install.sh` du dépôt, ou
CD invité) : avec l'ancien, le backend GL marche mais annonce 8 unités fixes à l'application.
