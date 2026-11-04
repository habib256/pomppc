# Colin McRae : projection de la texture de route humide (04/11/2026)

Le correctif porte sur **une copie de l'archive de shaders du jeu**, pas sur le plugin ou
l'émulateur. Il remplace `TEX` par `TXP` pour le seul échantillonnage **t2** de `Track.xpu`.
Le diagnostic au rejeu sur le M4 supprime les bandes de la route humide. **La validation en
VM de l'archive corrigée reste à faire ; aucun vrai PowerMac G4 n'a été testé.** Il n'est
pas établi comment le pilote d'origine traitait ce programme sur ce matériel.

## Contrat et diagnostic

[B] La spécification [ARB_fragment_program, révision 28, 08/10/2013, §3.11.6.1–2](https://registry.khronos.org/OpenGL/extensions/ARB/ARB_fragment_program.txt)
distingue les deux instructions : `TEX` utilise directement les coordonnées fournies ; `TXP`
divise leurs trois premières composantes par la quatrième avant l'échantillonnage. Pour une
texture 2D, seules les composantes s et t servent. Si q n'est pas strictement positif, la
spécification ne garantit pas le résultat de `TXP` ; cette correction n'ajoute aucun traitement
implicite dans le pilote.

[A, GPU hôte M4, contrat logiciel] Le flux capturé au titre 800×600 de
`bench/plugin/ombres-0411/cmr-resolution/f041-800-route/` porte la géométrie de l'image 56958,
présentée à l'image 56961. Les dessins 52–143 utilisent Track (VP2/FP3). Le dessin 67 produit
le pixel (400,400). Son échantillonnage t2 vise la texture 1034, 64×64, commentée « constant
specular » dans le programme, filtrée LINEAR et répétée. Les valeurs relevées de t vont de
25,21 à 37,99 ; t/q de 0,564 à 0,940. L'échantillonnage sans projection répète donc cette
texture sur la route. Cette observation et l'expérience suivante motivent le correctif ; elles
ne prouvent pas l'intention des auteurs sur tous les circuits et toutes les conditions.

Dans une **copie du vidage**, changer seulement le X de `TEX t2` en P supprime les bandes.
127 346 pixels diffèrent. Changer aussi t3 donne une image proche mais différente : aucun
besoin de ce second changement n'est établi, **t3 reste exact**. Le backend envoie les programmes
ARB au GPU hôte ; il n'ajoute pas de division générale ni de conversion propre à Colin McRae.

| Avant : archive/programme d'origine | Après : diagnostic, seul t2 projeté |
|---|---|
| ![Bandes sur la route](../../bench/plugin/ombres-0411/cmr-shaders/diagnostic-800/base.png) | ![Route avec t2 projeté](../../bench/plugin/ombres-0411/cmr-shaders/diagnostic-800/txp2.png) |

Le [rapport local](../../bench/plugin/ombres-0411/cmr-shaders/diagnostic-800/report.json) contient
les empreintes du manifeste du vidage et des sources qgpu/rejeu utilisées. Il rapporte un rejeu
initial sans erreur. Les images et l'archive propriétaire restent hors Git.

## Archive et modification exacte

L'original conservé localement est
`bench/plugin/ombres-0411/cmr-shaders/shaders.big.original` : **215 040 octets**,
SHA-256 `9825b610f58ab69b26dfe9c57d1423b373f20f0b0c6d89632a07d4750a9e5256`.
L'outil refuse toute autre empreinte, y compris une archive déjà corrigée.

[C, asset du jeu, empreinte ci-dessus] Cette variante BIGF porte 53 entrées : en-tête de
36 octets, répertoire à entrées de 24 octets (nom ASCII de 16 octets, taille et offset
little-endian), données à 2048, offsets relatifs alignés à 2048. `Track.xpu` est isolé par
son entrée, **pas par une recherche globale ni par un NUL entre programmes**. Sa source
contient le commentaire Track.arp et les templates `%zt0..%zt3` qu'IndirectX remplace ensuite
par les cibles telles que 2D/RECT. L'outil ne remplace aucun template.

| Élément | Valeur |
|---|---|
| Entrée | `Track.xpu`, offset absolu 161 792, taille 2 385 |
| Seul octet changé | offset absolu **163 262** (`0x27dbe`), X → P |
| Instruction | `TEX t2, fragment.texcoord[2], texture[2], %zt2;` → même instruction `TXP` |
| Archive après | `dd5a9ace7e0665a3c6f8eedd2e375042496cb9fdb48e987c07f2d0453fe88adc` |
| Track.xpu avant | `993f14fb517fcc7806cf2bf23c4574f2d83aa35d815bfdcbf8cfa486488cea8d` |
| Track.xpu après | `d4a7f1aee4e5082ee0e19722e592a7ba622f82f707d251b7fd146eebd6429690` |

Le répertoire, les tailles, t0/t1/t3, les autres programmes et tous les octets de remplissage
restent identiques. Observation distincte : `Water_DP3.xpu` emploie déjà `TXP` pour t2 et t3 ;
ce n'est pas une preuve de l'intention de `Track.xpu`.

## Utilisation locale et retour à l'original

Depuis la racine du dépôt, avec Python 3 :

```sh
python3 tools/guest/cmr-track-projection.py \
  bench/plugin/ombres-0411/cmr-shaders/shaders.big.original \
  /private/tmp/shaders-track-txp.big
```

L'entrée est seulement lue. La sortie doit être absente ; aucune sortie existante ni lien
symbolique n'est écrasé. Le JSON affiché indique les empreintes, l'entrée et l'offset changé.
Le programme valide les bornes, noms uniques, alignements et chevauchements du répertoire,
la source ciblée, l'unicité de l'instruction, puis les empreintes après et la relecture de sortie.
Aucun shader propriétaire complet n'est inclus dans le dépôt.

Pour restaurer, on repart de **l'original conservé**, jamais d'un patch inverse appliqué à
une archive inconnue. L'option suivante produit une nouvelle copie vérifiée de cet original :

```sh
python3 tools/guest/cmr-track-projection.py --restore \
  bench/plugin/ombres-0411/cmr-shaders/shaders.big.original \
  /private/tmp/shaders-original-restaure.big
```

L'outil ne monte pas de disque et n'installe rien dans l'invité. L'essai en VM doit utiliser
une copie/ombre du DMG du jeu ; conserver l'original et pouvoir réinstaller cette copie vérifiée.
Le shader se trouve dans
`Colin McRae Rally Mac.app/Contents/Resources/data/Rally_Shaders/shaders.big`.

## Épreuves

```sh
python3 tests/cmr_track_projection_test.py -v
POMPPC_CMR_ORIGINAL=/chemin/shaders.big.original \
  python3 tests/cmr_track_projection_test.py -v
```

Les tests ordinaires fabriquent une archive minimale synthétique, sans programme du jeu. Ils
vérifient l'octet unique, tous les autres programmes et t3, les espaces/templates, les refus
d'archives inconnues/tronquées, les noms dupliqués, chevauchements, offsets hors limites,
instruction absente/multiple, mauvaise empreinte après, sortie existante et lien symbolique.
Le test facultatif sur l'original local a aussi passé : empreinte finale exacte, un seul octet,
original inchangé, double application refusée et restauration identique.

[F] Reste à éprouver l'archive corrigée en VM et à observer plusieurs routes/conditions.
Un essai sur vrai PowerMac G4 avec l'original puis cette copie permettrait de trancher le
comportement du pilote d'origine. Aucun résultat de ce matériel n'est revendiqué.
