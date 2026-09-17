# Stencil — état GLEngine et tampon du GLDriver logiciel (Tiger 10.4.6)

Relevé le 17/09/2026 par la sonde `stencilprobe` de `guest/gltest` (un réglage GL par `glClear`,
vidage de l'état à chaque effacement par `POMPPC_GLTRACE_STATE=1`, diff des vidages), rendu
d'Apple seul (`POMPPC_GL_DISABLE=1`), contexte hors écran demandé avec `kCGLPFAStencilSize 8`
(`GLTEST_STENCIL=1`). Tout ce qui suit est **observé**, pas déduit.

## État GL de GLEngine (bloc pointé par `ctx+0x0c`)

| Offset | Type | Champ | Appel qui l'a fait bouger |
|---|---|---|---|
| `0x31c0` | u32, bit 0 | test de stencil actif | `glEnable(GL_STENCIL_TEST)` : `31c2: 0000→0001` |
| `0x31a0` | u16 | fonction (face avant) | `glStencilFunc(GL_EQUAL, …)` : `0207→0202` |
| `0x31b8` | u16 | fonction (face arrière, même valeur en 1.x) | idem |
| `0x319c` | u32 | référence | réf. `0x5A` : `319e: 0000→005a` |
| `0x3198` | u32 | masque de valeur | masque `0x3C` : `ffffffff→0000003c` |
| `0x2e38` | u32 | masque d'écriture | `glStencilMask(0xA5)` : `ffffffff→000000a5` |
| `0x31a2` | u16 | opération *fail* (avant ; arrière à `0x31ba`) | `1e00→1e01` |
| `0x31a4` | u16 | opération *zfail* (arrière à `0x31bc`) | `1e00→1e02`, puis `8507` (INCR_WRAP) |
| `0x31a6` | u16 | opération *zpass* (arrière à `0x31be`) | `1e00→150a`, puis `8508` (DECR_WRAP) |
| `0x2db4` | u32 | valeur d'effacement | `glClearStencil(0x77)` : `2db6: 0000→0077` |

GLEngine tient deux jeux (avant/arrière, écart `0x18`) alimentés ensemble par les appels 1.x ;
le plugin lit le jeu avant.

## Contexte du GLDriver logiciel : où est le tampon

Diff du contexte (`ctx`, 0x704 octets) entre un pixel format sans stencil et avec stencil 8 bits :

| Offset | Sans | Avec | Sens |
|---|---|---|---|
| `+0xd0` | 0 | 8 | bits de stencil (à côté de `+0xc0` bits couleur, `+0xcc` bits profondeur) |
| `+0x258` | 0 | `0x000000ff` | masque du stencil dans le mot de profondeur |
| `+0x260` | `0xffffffff` | `0xffffff00` | masque de la profondeur dans ce même mot |
| `+0x2a4` | 0 | `0x000000ff` | masque d'écriture effectif du stencil |

**Il n'y a pas de tampon de stencil séparé** : le stencil occupe les **8 bits bas de chaque mot
de 32 bits du tampon de profondeur** (`ctx+0x88`), la profondeur les 24 bits hauts. L'échelle de
profondeur (`ctx+0x14`) ne change pas.

Conséquences pour le plugin :

- téléversement : `profondeur = (mot & 0xffffff00) / échelle`, `stencil = mot & 0xff` ;
- relecture : `mot = (profondeur·échelle & 0xffffff00) | stencil` — relire la profondeur seule
  écraserait le stencil de l'invité, et inversement : les deux se synchronisent ensemble, et le
  drapeau de fraîcheur de la profondeur couvre aussi le stencil ;
- sans stencil (`ctx+0xd0 == 0`), rien ne change par rapport à aujourd'hui.

## À vérifier encore

- Que le GLDriver d'Apple écrit bien le stencil dans ces 8 bits pendant un dessin (scène de rendu
  `stencil` à écrire : masque puis test EQUAL, comparer l'image au rendu accéléré).
- Le comportement avec une profondeur demandée à 32 bits et un stencil de 8 (le pixel format
  retenu garde-t-il 24+8 ?).
