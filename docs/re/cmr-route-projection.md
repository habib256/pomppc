# Colin McRae : projection de la texture de route humide (04/11/2026)

Le correctif porte sur **une copie de l'archive de shaders du jeu**, pas sur le plugin ou
l'émulateur. Il remplace `TEX` par `TXP` pour le seul échantillonnage **t2** de `Track.xpu`.
Le diagnostic au rejeu sur le M4 supprime les bandes de la route humide. **L'archive corrigée
est installée et son programme vérifié dans une VM de diagnostic** (preuve du 05/11 ci-dessous).
Aucun vrai PowerMac G4 n'a été testé ; le comportement de son pilote d'origine reste inconnu.

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

Dans une **copie du vidage**, remplacer l’opcode complet `TEX t2` par `TXP t2` supprime les bandes.
Cela change deux octets : E → X puis X → P.
127 346 pixels diffèrent. Changer aussi t3 donne une image proche mais différente : aucun
besoin de ce second changement n'est établi, **t3 reste exact**. Le backend envoie les programmes
ARB au GPU hôte ; il n'ajoute pas de division générale ni de conversion propre à Colin McRae.

| Avant : archive/programme d'origine | Après : diagnostic, seul t2 projeté |
|---|---|
| ![Bandes sur la route](../../bench/plugin/ombres-0411/cmr-shaders/diagnostic-800/base.png) | ![Route avec t2 projeté](../../bench/plugin/ombres-0411/cmr-shaders/diagnostic-800/txp2.png) |

Le [rapport local](../../bench/plugin/ombres-0411/cmr-shaders/diagnostic-800/report.json) contient
les empreintes du manifeste du vidage et des sources qgpu/rejeu utilisées. Il rapporte un rejeu
initial sans erreur. Les images et l'archive propriétaire restent hors Git.
La [comparaison binaire indépendante](../../bench/plugin/ombres-0411/cmr-shaders/diagnostic-800/binary-change-proof.json)
confirme les deux changements dans la variante diagnostique : positions 1555/1556 du fichier
`000028.bin`, 1467/1468 dans le programme ; `TXP t2` présent, `TEP` absent et t3 toujours `TEX`.

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
| Deux octets changés | offsets absolus **163 261** (`0x27dbd`), E → X, et **163 262** (`0x27dbe`), X → P |
| Instruction | `TEX t2, fragment.texcoord[2], texture[2], %zt2;` → même instruction `TXP` |
| Archive après | `f9fe55f4310998c1bb548d9abf633a171073e9950315621b627758b4e813e4f6` |
| Track.xpu avant | `993f14fb517fcc7806cf2bf23c4574f2d83aa35d815bfdcbf8cfa486488cea8d` |
| Track.xpu après | `56af056d75ef1255e2f205affa9644b92ab4efe720f5513d16ad8d87d9e1404e` |

La sortie est relue comme une instruction `TXP t2` exacte avec les mêmes opérandes.
Un résultat `TEP` est rejeté avant les contrôles d’empreinte, même si une empreinte attendue
reproduisait cette erreur. Le répertoire, les tailles, t0/t1/t3, les autres programmes et tous les octets de remplissage
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
vérifient les deux octets exacts et l'opcode final `TXP`, tous les autres programmes et t3, les espaces/templates, les refus
d'archives inconnues/tronquées, les noms dupliqués, chevauchements, offsets hors limites,
instruction absente/multiple, mauvaise empreinte après, sortie existante et lien symbolique.
Le test facultatif sur l'original local a aussi passé : empreinte finale exacte, exactement deux octets,
original inchangé, double application refusée et restauration identique.

[F] Plusieurs routes/conditions et le rendu d'origine sur matériel réel restent à observer.

Un essai sur vrai PowerMac G4 avec l'original puis cette copie permettrait de trancher le
comportement du pilote d'origine. Aucun résultat de ce matériel n'est revendiqué.

## Correction de l'outil initial (05/11/2026)

Le premier outil et ses tests ont reproduit une erreur de construction : changer seulement
X → P dans `TEX` produit **`TEP`**, une instruction invalide. Cette archive (SHA-256
`dd5a9ace7e0665a3c6f8eedd2e375042496cb9fdb48e987c07f2d0453fe88adc`) a été refusée par
le compilateur ARB en VM, puis le jeu a quitté. Elle est conservée comme preuve négative et
ne doit pas être installée. L'original reste intact. Le champ `byte_changes: 1` du rapport
diagnostique initial était également erroné ; la transformation valide TEX → TXP en exige deux.
Le rapport a depuis été rectifié et complété par la comparaison binaire ci-dessus.

Les tests corrigés vérifient l'opcode complet, ses opérandes, les deux positions modifiées et
tous les autres octets. Un mutant reproduit l'ancien remplacement X → P et les empreintes
correspondantes : il est rejeté **par la vérification TXP**, pas seulement par une empreinte
différente. Le test de l'archive réelle vérifie explicitement `TEX` avant, `TXP` après,
aucun `TEP`, différences exactement `[163261, 163262]`, longueur et autres programmes inchangés.
La validation de cette archive valide dans une nouvelle ombre est décrite ci-dessous.


## Validation dans Tiger, 05/11/2026

L'essai valide est séparé du premier :
`bench/plugin/ombres-0411/cmr-resolution/track-projection-v2-800/`.
L'archive originale du DMG est conservée ; la nouvelle ombre
`/Users/tiger/cmr-track-projection-v2-0411.shadow` contient seulement la copie corrigée.
`installation-ombre-v2-clone.json` conserve empreintes et différences des deux octets.
Aucun patch n'est encore installé sur le disque quotidien à ce stade.

- Relecture du fichier dans Tiger : SHA-256 **f9fe55f4310998c1bb548d9abf633a171073e9950315621b627758b4e813e4f6**.
- Le GPU accepte `PROG 3 cible 8804 : 2379 octets, env 2 local 35 compilé` ; aucun refus
  du programme ni assertion GL. Le titre continue, une capture synchronisée est conservée.
- Capture du titre image **49132**, rejeu **0,01 / 0,00 %**, sans erreur. Ce passage est un
  autre circuit, sec ; il ne sert pas de capture directe de la route humide précédente.
- Le programme Track extrait de ce nouveau flux est **identique** à celui du rejeu humide,
  sauf les deux octets TEX → TXP ; SHA-256 du programme final
  `1d883496b68e7988abb607f53409cd95caef342637194ee5cb36d62800992b86`.
  `preuve-shader-charge.json` consigne cette égalité. Ainsi le shader effectivement chargé
  dans le jeu est bien celui qui supprime les bandes sur le flux humide conservé.
- Huit tests ciblés avec l'archive réelle passent. Contrôle général final depuis Terminal :
  **527 OK, 0 échec, 5 ignorés** (`.run/tests-texblank-v2-terminal-0411.log`).

L'outil et le jeu original restent séparés du pilote : aucune instruction ARB TEX n'est
réinterprétée par POMPPC et aucun nom de shader n'entre dans l'émulateur.

## Lanceur Tiger pour l'ombre déjà installée

Copier `tools/guest/cmr-track-projection.command` sur le Bureau de Tiger, le rendre exécutable
(`chmod +x ...`), puis le lancer par double clic. Il attend l'original
`$HOME/Desktop/Colin_McRae_Ready_Mac-1.dmg` et une ombre **déjà préparée et vérifiée** à
`$HOME/Library/Application Support/POMPPC/cmr-track-projection.shadow`.
Il n'installe aucun asset et ne crée pas d'ombre. Il ouvre le jeu uniquement si `shaders.big`
porte le MD5 **5761b6eb3081cf0f10d7934df4a98045**, calculé sur l'archive TXP valide de SHA-256
f9fe55f4…813e4f6. Un volume déjà correct est directement ouvert ; seul l'original connu
(MD5 51a6cd53223114f59bd8c81ecff86938) peut être démonté, sans forçage. Une archive montée
inconnue est laissée en place et refusée. Si nécessaire, ce démontage normal
précède `hdiutil attach -shadow` au point `/Volumes/Colin McRae Ready Mac`, puis le fichier
est vérifié à nouveau. Jeu lancé sur une autre archive ou volume occupé : message et arrêt,
jamais de processus tué ni de démontage forcé. Le DMG original, le pilote et l'émulateur
restent inchangés. Syntaxe `/bin/sh` et Bash vérifiée, ShellCheck propre ; essai du lanceur
dans Tiger à faire après installation de l'ombre stable.
