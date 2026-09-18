# Protocole qgpu v10 — ce qui manquait à OpenGL 1.2–1.4 côté hôte : textures, couleur secondaire, paramètres de point

Jusqu'à la v9, une texture qgpu était **2D**, ses texels arrivaient en ARGB **déjà convertis par
l'invité** sur le PowerPC émulé, et seuls trois modes de répétition existaient. La couleur
secondaire ne pouvait pas être envoyée sans allumer `GL_COLOR_SUM`, et la taille d'un point ne
dépendait pas de sa distance. C'est ce qui bloquait, côté hôte, l'annonce d'une version d'OpenGL
supérieure à 1.1 (`docs/re/version-extensions.md` §7) :

| Version | Ce qui manquait |
|---|---|
| 1.2 | **textures 3D** (le seul vrai verrou), niveaux de base et max, bornes de LOD |
| 1.3 | cartes de cube, compression (S3TC), `GL_CLAMP_TO_BORDER` |
| 1.4 | textures de profondeur et comparaison d'ombre, `GL_MIRRORED_REPEAT`, biais de LOD, mipmaps automatiques ; **couleur secondaire** ; **paramètres de point** |

La v10 les ajoute **côté hôte** (tâches 3.4 et 3.6, la moitié hôte de 2.5 et de 3.5 de
`docs/todo-gpu-3d.md`). Elle ajoute aussi les textures **rectangle**, hors OpenGL 1.5 strict mais
indispensables à Quartz Extreme et Core Image plus tard (4.4). Le multiéchantillonnage (3.8) et
les sprites de points restent hors de la v10.

Règle inchangée : **rien n'est retiré, aucune longueur de commande existante ne change**.
`TEX_CREATE` crée une texture 2D, `TEX_IMAGE` envoie des texels ARGB, exactement comme avant ; les
valeurs initiales des nouveaux paramètres sont celles d'OpenGL et elles sont neutres. Un flux v9
rend la même image sur un hôte v10.

Le contrat détaillé est dans `patches/qgpu/qgpu_proto.h`, section « v10 ». Ce document dit
pourquoi il a cette forme, et ce qu'il reste à faire dans l'invité.

---

## 1. Trois opcodes

| Opcode | Arguments | Rôle |
|---|---|---|
| `TEX_CREATE3` (0x44) | tex, cible | crée une texture de cible 1D, 2D, 3D, CUBE_MAP ou RECTANGLE |
| `TEX_IMAGE3` (0x45) | tex, cible d'image, niveau, w, h, d, format de base, **format, type**, off, octets par ligne, octets par tranche | définit un niveau (une face pour un cube) |
| `TEX_SUBIMAGE` (0x46) | tex, cible d'image, niveau, x, y, z, w, h, d, format, type, off, octets par ligne, octets par tranche | remplace une boîte d'un niveau défini |

### La cible est fixée à la création

C'est la règle d'OpenGL : le premier `glBindTexture` fixe la cible d'un objet texture. La mettre
dans `TEX_CREATE3` plutôt que de la déduire de la première image a deux raisons concrètes :

* les **paramètres** posés avant la première image doivent déjà être validés selon la cible —
  une texture rectangle refuse `GL_REPEAT` et les filtres avec mipmaps, et ses valeurs initiales
  ne sont pas celles des autres (`GL_LINEAR` / `GL_CLAMP_TO_EDGE`) ;
* l'invité a de quoi la connaître : la table des textures liées de GLEngine est indexée par
  unité **et par cible** (`CTX_TEXUNITS`, `unité·0x14 + cible·4`). Qu'elle soit déjà connue au
  moment de `gldCreateTexture` n'est **pas relevé** (cf. §7).

La priorité entre cibles d'une même unité (cube > 3D > rectangle > 2D > 1D) reste l'affaire de
l'invité : il envoie dans `QGPU_SK_TEX*_BIND` la texture de la cible effective. Le cœur n'a donc
**aucune clé d'état nouvelle** pour les cibles.

### Les données au format de l'application, converties par l'hôte

`TEX_IMAGE3` prend les texels **tels que l'application les a donnés** à `glTexImage` : le couple
(format, type) d'OpenGL est recopié, et la conversion en ARGB se fait dans le cœur, sur l'hôte.
Jusqu'ici `convert_level` (`guest/gldriver/pomppc_accel.c`) faisait ce travail texel par texel sur
le PowerPC émulé ; il n'aura plus qu'à recopier.

Couples acceptés : ceux que le plugin savait déjà convertir, plus `5_6_5_REV`, `4_4_4_4_REV` et
`5_5_5_1`, plus la profondeur et les quatre formats S3TC. La liste exacte est dans
`qgpu_proto.h`. `GL_BGRA` + `GL_UNSIGNED_INT_8_8_8_8_REV` est exactement le mot ARGB de
`TEX_IMAGE` : c'est ainsi que le cœur réécrit `TEX_IMAGE` sur le même chemin.

Deux pas (**octets par ligne**, **octets par tranche**) décrivent des données alignées
(`GL_UNPACK_ALIGNMENT`, `_ROW_LENGTH`, `_IMAGE_HEIGHT`, que l'invité a déjà appliqués) : le
plugin refusait jusqu'ici tout niveau dont le pas n'était pas la largeur exacte
(`S16(lv, LV_ROWPIX) != w`).

`off = QGPU_TEX_NO_DATA` définit un niveau à zéro, comme `glTexImage` avec un pointeur nul, pour
le remplir ensuite par sous-images.

### Sous-images

`TEX_SUBIMAGE` est l'opcode qui manquait pour les textures **qui changent** (vidéo, textures
dynamiques) : le plugin renvoie aujourd'hui la texture entière dès qu'elle est marquée modifiée.
La boîte doit tenir dans le niveau ; en S3TC, elle doit être alignée sur les blocs de 4×4.

## 2. Tout le contenu des textures est calculé par le cœur

Conversions de format, **décompression S3TC** et **génération des mipmaps** se font dans
`qgpu-core.c`, pas dans les backends. Un backend ne voit que des mots ARGB, ou des flottants de
profondeur. C'est le choix qui garde le backend de référence comme vérité terrain :

* **S3TC.** La spécification laisse libre l'arrondi des couleurs intermédiaires ; décompresser
  dans le cœur donne exactement les mêmes texels au backend logiciel et au GPU hôte. Le coût
  (mémoire hôte ×4 à ×8 pour ces textures) est sans importance sur l'hôte visé, et il n'y a
  aucun coût côté invité : c'est le PowerPC émulé qu'on décharge. Les blocs sont définis octet
  par octet, en **petit-boutiste** : l'invité les recopie sans rien échanger.
* **Mipmaps automatiques.** `glGenerateMipmap` et `GL_GENERATE_MIPMAP` laissent le filtre libre ;
  générés dans le cœur (moyenne de blocs 2×2, 2×2×2 en 3D), les niveaux sont identiques pour
  tous les backends. Ils sont recalculés à chaque image ou sous-image **du niveau de base**, et
  seulement pour lui (règle d'OpenGL 1.4).

## 3. Échantillonnage

### Répétition et bordure

`GL_MIRRORED_REPEAT` suit la formule des spécifications récentes, sur les indices de texels :
`(taille − 1) − miroir((i mod 2·taille) − taille)`. `GL_CLAMP` et `GL_CLAMP_TO_BORDER` prennent
la **couleur de bordure** (`QGPU_TP_BORDER_COLOR`), qui était le noir transparent jusqu'ici — et
qui l'est toujours par défaut.

### LOD

λ = log2 ρ, plus le biais de la texture et celui de l'unité (leur somme bornée à
±`QGPU_MAX_LOD_BIAS`), borné à [`MIN_LOD`, `MAX_LOD`]. Les niveaux employés vont du niveau de
base b à q = min(b + log2 de la plus grande dimension, `MAX_LEVEL`), et c'est cette chaîne-là
que la complétude exige. Le biais d'**unité** (`glTexEnv(GL_TEXTURE_FILTER_CONTROL, …)`) est une
clé d'état, `QGPU_SK_TEX_LOD_BIAS0 + u` ; celui de la **texture** est un paramètre.

### Cartes de cube

Face et coordonnées selon la table 3.21 d'OpenGL 1.3. **Le GPU hôte fait foi** : le test (e)
de `run_v10` pose une face +X de 2×2 texels distincts et quatre directions, et le backend de
référence doit rendre les mêmes quatre couleurs que le pilote NVIDIA. La complétude exige six
faces carrées, de même taille et de même format à chaque niveau ; sinon le texturage de l'unité
est coupé, comme en OpenGL.

### Textures de profondeur

Une texture de format de base `GL_DEPTH_COMPONENT` rend D, ou en mode
`GL_COMPARE_R_TO_TEXTURE` le résultat 0/1 de « r fonc D », r borné à [0,1]. La comparaison se
fait **texel par texel, avant le filtrage** : avec un filtre linéaire, le résultat est la moyenne
pondérée des comparaisons, ce que font les GPU. Le résultat devient luminance, intensité ou alpha
selon `QGPU_TP_DEPTH_MODE`, et l'environnement de texture le voit comme une texture de ce format.

### r n'est plus ignoré

La coordonnée r du sommet, « ignorée » depuis la v3, sert maintenant à trois choses : troisième
coordonnée des textures 3D, troisième composante de la direction d'un cube, valeur de référence
des textures de profondeur. Un invité v3–v9 y mettait 0 ; cela ne change rien à ses textures 2D.

## 3 bis. Couleur secondaire et paramètres de point

Deux groupes de clés d'état, qui ne servent **qu'à `DRAW_RAW`** comme les clés v7 : les sommets
des anciens opcodes n'ont pas de couleur secondaire, et leur taille de point est déjà calculée par
GLEngine.

**`QGPU_SK_COLOR_SUM`** a **trois** valeurs, pas deux, pour tenir la promesse de compatibilité.
Jusqu'à la v9, `DRAW_RAW` ajoutait la couleur secondaire dès que le format de sommet en portait
une : c'est ce qui empêchait le plugin de l'envoyer (`todo-gpu-3d.md`, « Ce que le lot 2 a laissé
derrière lui »). La valeur initiale, `QGPU_CSUM_FORMAT`, garde exactement cette règle ;
`QGPU_CSUM_OFF` et `QGPU_CSUM_ON` sont `glDisable` / `glEnable(GL_COLOR_SUM)`. Avec l'éclairage,
c'est lui qui produit la couleur secondaire (la spéculaire en `GL_SEPARATE_SPECULAR_COLOR`, zéro
sinon), et elle s'ajoute toujours : règle d'OpenGL, suivie à l'identique par les deux backends.

**Paramètres de point** : `QGPU_SK_POINT_SIZE_MIN`, `_MAX`, `QGPU_SK_POINT_FADE` et les trois
coefficients a, b, c de `GL_POINT_DISTANCE_ATTENUATION`. Taille dérivée
`taille · sqrt(1 / (a + b·d + c·d²))`, bornée à [MIN, MAX], d étant la distance à l'œil en
coordonnées œil, `sqrt(x² + y² + z²)`. La formule est écrite **une fois**, dans
`qgpu_point_size()` du cœur, et le backend de référence l'emploie pour `GL_POINTS` comme pour les
sommets d'un polygone en mode `GL_POINT` ; le backend OpenGL passe les mêmes valeurs à
`glPointParameter`. Le **seuil de fondu** est accepté et sans effet : OpenGL 1.4 (§3.3) ne l'emploie
qu'en multiéchantillonnage, que qgpu ne fait pas — et le test (t) vérifie que le GPU hôte ne fond
pas l'alpha non plus.

## 4. Écarts assumés du backend de référence

* **λ reste calculé par triangle**, sur les aires, comme depuis la v3. En 3D, ρ ne tient compte
  que de s et t. Pour un cube, les trois sommets sont rapportés à la face du centre du triangle ;
  un triangle à cheval sur deux faces est grossi (λ = −∞). En 1D, ρ vient du gradient de s seul :
  une aire (s, t) serait nulle dès que t est constant.
* **Profondeur** : le cœur garde des flottants, le GPU hôte des entiers de 24 bits. Les
  comparaisons ne peuvent différer qu'à moins de 2⁻²⁴ de l'égalité.
* **Rectangle** : `MIN_LOD` et `MAX_LOD` sont **refusés** (`QGPU_ST_BAD_ARG`). Une texture
  rectangle n'a pas de mipmaps, et le pilote NVIDIA les rejette (`GL_INVALID_OPERATION`, vu en
  vrai) : les accepter aurait fait diverger les deux backends.

## 5. Capacité

`QGPU_CAP_GL14` (0x10) dit que le backend actif sait faire ce que la v10 demande au GPU :
échantillonner les nouvelles cibles et appliquer les nouveaux paramètres de texture, et
l'atténuation des points. Le backend de référence l'annonce toujours. Le backend OpenGL l'annonce
si l'hôte est en OpenGL 1.4 au moins et que `glTexImage3D` et `glPointParameterf(v)` se
résolvent. Sans ce bit, `TEX_CREATE3` d'une autre cible que 2D, une image de profondeur, les
paramètres de texture propres à la v10 et des paramètres de point autres qu'initiaux répondent
`QGPU_ST_BACKEND` — refusés au moment où ils sont posés, sans rien écrire : l'invité se replie, il
ne plante pas. Conversions, S3TC, sous-images et mipmaps, faits par le cœur, n'en dépendent pas,
pas plus que `QGPU_SK_COLOR_SUM` (`GL_COLOR_SUM` sert depuis la v7).

**`QGPU_REG_CAPS` publie désormais ces bits.** Jusqu'à la v9, `qgpu-pci.c` publiait
`core.be->cap`, c'est-à-dire les bits *fixes* du backend : `QGPU_CAP_OCCLUSION`, résolu à chaud
par `init()`, n'atteignait jamais l'invité (`docs/protocole-v8-pipeline-fixe.md` §5 le notait).
Le device publie maintenant `core.caps`, et `tests/qgpu_smoke.py` le vérifie : sur l'hôte Linux,
`caps = 0x1e` (GL, occlusion, asynchrone, textures).

## 6. Preuve

`run_v10` de `tests/qgpu_core_test.c`, sur les deux backends. Chaque valeur attendue est écrite à
la main, et elle doit sortir **identique** du backend logiciel et du GPU hôte (RTX 4060 Ti,
pilote NVIDIA, EGL), aux points testés, qui sont au centre de texels ou de bandes à couleur
constante :

| Cas | Ce qui est vérifié |
|---|---|
| (a)–(c) | 3D au plus proche (une tranche par bande), trilinéaire entre deux tranches (`007f7f` sur GPU, `008080` en référence : tolérance de 2), `WRAP_R` en `REPEAT` et `CLAMP_TO_EDGE` |
| (d)–(f) | cube : une face par axe ; **orientation** d'une face (quatre directions, quatre texels) ; cube incomplet → texturage coupé |
| (g) | rectangle 3×2 en coordonnées de texels ; refus de `REPEAT`, des mipmaps, d'un niveau de base ≠ 0 |
| (h) | 1D : t ignoré, même en `GL_CLAMP` linéaire, où une texture 2D d'une ligne mêlerait la bordure |
| (i), (j) | `MIRRORED_REPEAT` ; `CLAMP_TO_BORDER` avec une couleur de bordure à alpha 0x80 |
| (k) | profondeur : `LEQUAL`, `GEQUAL` en alpha, D en luminance sans comparaison, données `UNSIGNED_SHORT` |
| (l) | LOD : λ = 0, 1, 2 ; `BASE_LEVEL`, `MAX_LEVEL`, `MIN_LOD`, `MAX_LOD`, biais de texture, biais de texture + d'unité |
| (m) | mipmaps générés : niveau 2 = moyenne `800080`, niveau 1 en damier, régénération après sous-image du niveau de base |
| (n) | les 18 couples (format, type), chacun sur deux texels, alpha compris ; pas de ligne et pas de tranche |
| (o) | S3TC : DXT1 à 4 couleurs, DXT1 à 3 couleurs et noir transparent, DXT3, DXT5 à 8 alphas |
| (p) | niveau sans données puis sous-image |
| (q) | **chemin brut** (v7) : 3D par coordonnées brutes, cube par texgen `GL_NORMAL_MAP` |
| (s) | `GL_COLOR_SUM` : règle v7–v9 par défaut, coupée malgré une couleur secondaire dans le format, allumée avec la valeur courante |
| (t) | paramètres de point : atténuation c·d² (tailles 8 et 4 à 20 et 40 de l'œil), `POINT_SIZE_MAX`, `POINT_SIZE_MIN`, seuil de fondu sans effet, polygone en `GL_POINT`, et `DRAW_POINTS` (v4) indifférent à tout cela |
| (r) | 20 refus, chacun avec son statut, et le cœur intact après eux |

Résultat : **0 échec** sur les deux backends, et `tests/qgpu_smoke.py` vert à travers QEMU.

## 7. Ce que le plugin invité devra faire

Rien de ceci n'est visible par une application tant que le plugin n'a pas suivi. Il faut une VM
de développement ; rien n'a donc été touché côté invité. Dans l'ordre :

1. **Formats convertis par l'hôte** (gain de vitesse immédiat, sans relevé nouveau) : envoyer
   `TEX_IMAGE3` avec le `LV_FORMAT` / `LV_TYPE` du niveau et son pas réel (`LV_ROWPIX`), au lieu de
   `convert_level` suivi de `TEX_IMAGE`. Les niveaux à pas aligné cessent d'être refusés.
2. **Textures 3D** : poser `cfg+0xbe` (`GL_MAX_3D_TEXTURE_SIZE`) à `QGPU_MAX_TEX_3D_DIM`. La sonde
   `POMPPC_GL_TRY3D` a montré que GLEngine accepte alors `glTexImage3D` et remet les niveaux par
   `gldCreateTextureLevel`. **À relever** : où le niveau porte sa profondeur (la structure de
   0x74 octets ne connaît que `LV_W`, `LV_H`), et comment le plugin connaît la cible d'un objet
   texture. Il n'y a **aucun repli possible**, le rendu d'Apple ne sait pas échantillonner la 3D :
   tout dessin qui emploie une texture 3D doit passer par l'hôte.
3. **Cartes de cube** : même mécanique, `cfg+0xc2` (`GL_MAX_CUBE_MAP_TEXTURE_SIZE`) et les six
   faces. À relever : l'endroit où GLEngine range les faces d'un objet cube.
4. **Compression** : annoncer `GL_ARB_texture_compression` et `_s3tc` seulement après avoir
   vérifié que `glCompressedTexImage2D` arrive au pilote avec les blocs bruts. Aujourd'hui la
   compression est **silencieusement fausse** sous le rendu d'Apple (`version-extensions.md` §2) :
   c'est la case la plus urgente à fermer, même avant de l'annoncer.
5. **Profondeur et comparaison**, `MIRRORED_REPEAT`, `CLAMP_TO_BORDER`, bornes et biais de LOD :
   relever les offsets des nouveaux paramètres dans `DT_PARAMS` (seuls `TP_WRAP_S/T`, `TP_MIN`,
   `TP_MAG` le sont), par sonde comme d'habitude, puis les recopier.
6. **Couleur secondaire** : trouver l'octet de `GL_COLOR_SUM` dans le bloc d'état de GLEngine
   (toujours pas relevé), puis mettre la couleur secondaire dans le format de `DRAW_RAW` et poser
   `QGPU_SK_COLOR_SUM` explicitement. Sous le rendu d'Apple, elle n'est **pas** ajoutée du tout
   (`version-extensions.md` §2) : ce sera une fonction tenue par l'hôte seul, comme les requêtes
   d'occlusion.
7. **Paramètres de point** : les offsets sont déjà relevés (`docs/re/etat-tcl.md` §9,
   `GS+0x30c0` à `GS+0x30cc`) ; le motif de refus `taille-de-point-attenuee` peut tomber sur le
   chemin brut. **Attention** : GLEngine part d'un `GL_POINT_SIZE_MAX` de **1**, observé par la
   sonde, et non de la taille maximale. Le recopier tel quel ramènerait tous les points du chemin
   brut à 1 pixel. Il faut vérifier dans l'invité comment GLEngine l'applique, et ne pas envoyer
   de borne plus stricte que ce qu'il applique vraiment.

Avec 2 et les vérifications [H] de `version-extensions.md` (couleur spéculaire séparée, niveaux et
LOD), l'annonce de **1.2** devient possible. 1.3 demande encore le multiéchantillonnage (3.8) ;
1.4, la couleur secondaire et les paramètres de point.
