# État de `glPixelStorei` : `GC_PACK_*` / `GC_UNPACK_*` (relevé du 07/10/2026)

## 1. Le défaut : une base fausse, pas des offsets faux

Le plugin lisait l'état de `glPixelStorei` à `p->ctx + 0x31cc..0x31e5` pour `glDrawPixels`,
`glBitmap` et le `glDrawPixels` de profondeur et de stencil (`CTX_UNPACK_*`, Phase A du
20/09). Or `p->ctx` est le **contexte du GLDriver** (argument r3 des procédures, environ
`0x704` octets : `CTX_GLSTATE` en `+0x0c`, largeur en `+0x1c`…), pas le **contexte de
GLEngine** (`gctx = GS − 0x360`, `gctx_of()`), où `_glPixelStorei_Exec` range ces mots. Le contexte du
GLDriver est un bloc de `malloc_size` = 0x800 octets (relevé dans la session bureau) : le
plugin lisait donc environ 10 Kio au-delà de sa fin, dans un autre objet du tas :

- le 22/09 (10.4.6, scène `drawpack`) : « ALIGNMENT vaut 4 quand l'application a posé 1 » ;
- le 07/10 (10.4.11, 64 appels relevés) : les quatre mots à 0, `ALIGNMENT` compris ;
- le 07/10, dans la session bureau par le relais `POMPPCGuiRunner` : des valeurs folles,
  `try_draw_pixels` en a tiré une source à `pixels + 0x5d410000` et `memcpy` a planté
  (`docs/backend-gl-unites-fixes.md`, « Scènes gltest sur le PC »).

Ce qu'il y a à cet endroit dépend de la disposition du tas (bibliothèques chargées, session),
d'où un défaut muet en single-user et un plantage dans la session de l'utilisateur. Le
même défaut de base touchait la couleur raster de `try_bitmap` (`p->ctx + 0x4858`,
`p->ctx + 0x2a0`), lue désormais sur `gctx`.

## 2. Relevé

Méthode de `docs/re/README.md` : un réglage par étape, un `glClear` après chacun, vidage du
bloc d'état à chaque `glClear` et diff des vidages successifs.

```sh
# dans l'invité, single-user (VM de dev 10.4.11, 8S165)
env GLTEST_NOWS=1 POMPPC_GL_DISABLE=1 POMPPC_GLTRACE=/tmp/tr POMPPC_GLTRACE_STATE=1 \
    ./gltest pixsonde 64 64 /tmp/p.ppm
# sur l'hôte, une fois /tmp/tr rapatrié
tools/re/diffstate.py diff tr --tag clear-glstate
```

La scène `pixsonde` de `guest/gltest` pose dans l'ordre chaque paramètre `GL_UNPACK_*` puis
`GL_PACK_*` à une valeur distinctive (ALIGNMENT 1 puis 8, ROW_LENGTH 17, SKIP_ROWS 3…). Chaque
étape bouge **un seul octet** de tout le bloc de `0x5400` octets (offsets `GS+…` ci-dessous,
`gctx = GS − 0x360`). Contre-épreuve sous le plugin : une sonde temporaire notait, à chaque
`DrawPixels`/`Bitmap`/`ReadPixels`, les mots `gctx+0x3180..0x3240` (mêmes mouvements, mêmes
valeurs) et `p->ctx+0x31cc..` (toujours 0, quel que soit le réglage).

| paramètre | GS+ | **gctx+** | type | défaut | ancien `CTX_*` (sur `p->ctx`) |
|---|---|---|---|---|---|
| `GL_PACK_ROW_LENGTH` | 0x2e50 | **0x31b0** | u32 | 0 | — |
| `GL_PACK_IMAGE_HEIGHT` | 0x2e54 | **0x31b4** | u32 | 0 | — |
| `GL_PACK_SKIP_ROWS` | 0x2e58 | **0x31b8** | u32 | 0 | — |
| `GL_PACK_SKIP_PIXELS` | 0x2e5c | **0x31bc** | u32 | 0 | — |
| `GL_PACK_SKIP_IMAGES` | 0x2e60 | **0x31c0** | u32 | 0 | — |
| `GL_PACK_ALIGNMENT` | 0x2e64 | **0x31c4** | u32 | 4 | — |
| `GL_PACK_SWAP_BYTES` | 0x2e68 | **0x31c8** | u8 | 0 | — |
| `GL_PACK_LSB_FIRST` | 0x2e69 | **0x31c9** | u8 | 0 | — |
| `GL_UNPACK_ROW_LENGTH` | 0x2e6c | **0x31cc** | u32 | 0 | 0x31cc |
| `GL_UNPACK_IMAGE_HEIGHT` | 0x2e70 | **0x31d0** | u32 | 0 | — |
| `GL_UNPACK_SKIP_ROWS` | 0x2e74 | **0x31d4** | u32 | 0 | 0x31d4 |
| `GL_UNPACK_SKIP_PIXELS` | 0x2e78 | **0x31d8** | u32 | 0 | 0x31d8 |
| `GL_UNPACK_SKIP_IMAGES` | 0x2e7c | **0x31dc** | u32 | 0 | — |
| `GL_UNPACK_ALIGNMENT` | 0x2e80 | **0x31e0** | u32 | 4 | 0x31e0 |
| `GL_UNPACK_SWAP_BYTES` | 0x2e84 | **0x31e4** | u8 | 0 | — |
| `GL_UNPACK_LSB_FIRST` | 0x2e85 | **0x31e5** | u8 | 0 | 0x31e5 |

Les offsets `UNPACK` du 20/09 étaient donc justes ; seule leur base était fausse. Les deux
blocs ont la même forme (`ROW_LENGTH, IMAGE_HEIGHT, SKIP_ROWS, SKIP_PIXELS, SKIP_IMAGES,
ALIGNMENT`, puis deux octets `SWAP_BYTES, LSB_FIRST`), `PACK` d'abord.

## 3. 10.4.6 et 10.4.11 : le même GLEngine

`GLEngine.bundle/GLEngine` de la VM 10.4.11 : 1 295 708 octets, daté du 30/01/2006, MD5
`02f4a2854a95be1f2853baeb99ae22b3`. Ses 316 blocs de 4 Kio non vides se retrouvent **tous**
dans l'image de la sauvegarde 10.4.6 (`disks/tiger-dev-10.4.6.raw`, recherche bloc à bloc
alignée sur les blocs d'allocation HFS+) : le combo 10.4.11 n'a pas remplacé GLEngine, et une
table unique vaut pour les deux systèmes. Le plugin n'a pas de table par version ; si un
autre GLEngine apparaît, la vérification de cohérence ci-dessous est le premier signal.

## 4. Ce qu'en fait le plugin

`pixstore_read(p, pack, …)` lit le bloc sur `gctx_of(p)`. Ce n'est plus une garde
(`unpack_trusted`, 07/10, qui envoyait tout réglage non défaut au logiciel) mais une
**vérification de cohérence** : alignement hors `{1, 2, 4, 8}`, longueur ou décalage
≥ 2²⁴, octet `SWAP`/`LSB` ni 0 ni 1 — tout ce que GLEngine refuse lui-même par
`GL_INVALID_VALUE` — envoie l'appel au rendu logiciel, compte dans `POMPPC_GL_STATS`
(« N incoherent glPixelStorei state(s) ») et le note (8 notes au plus). Zéro attendu.

Avec la bonne base, les restrictions qui couvraient le défaut sont levées : lignes serrées
multiples de 4 seulement pour `DrawPixels` couleur, stencil, `Bitmap` et `ReadPixels`
(P1/P2 du bug hunt du 22/09, Finding 8 du 29/09). Le pas de l'application est calculé par
`pixstore_stride()` (`ROW_LENGTH` ou `w`, arrondi à `ALIGNMENT`), les `SKIP_*` décalent la
source (ou la destination de `ReadPixels`). Restent au logiciel : `ROW_LENGTH < w`, et
`SWAP_BYTES` sur la profondeur flottante (sans effet sur les octets).

## 5. Épreuve

Scène `gltest pixstore` (256×256) : `glDrawPixels` GL_RGB 13×5 en ALIGNMENT 1/2/4/8,
GL_RGBA avec ROW_LENGTH 10 / SKIP_ROWS 2 / SKIP_PIXELS 3, GL_RGB ALIGNMENT 2 avec ROW_LENGTH
et SKIP ; `glBitmap` 9×6 en ALIGNMENT 1 et 8 et 40×4 en ROW_LENGTH 48 / SKIP / LSB_FIRST
(couleur raster distincte de la couleur courante) ; `glReadPixels` GL_RGB en PACK_ALIGNMENT
1/2/8, GL_RGBA en PACK_ROW_LENGTH 17 / SKIP ; profondeur flottante écrite en ALIGNMENT 8 /
ROW_LENGTH 9 / SKIP et relue en PACK_ROW_LENGTH 11 / SKIP. Remplissage source magenta (un
octet de remplissage lu se voit), destination à 0xA5 (un octet écrit hors du rectangle se
voit). Résultats : `docs/backend-gl-unites-fixes.md`, « Cause : la mauvaise base ».
