# Profils du plugin : résoudre les noms avant de choisir un levier

Relevé du 01/11/2026, vers 22 h 47, dans la VM quotidienne du M4 (Tiger 10.4.11).
Ce travail identifie du code ; il ne mesure aucun gain de vitesse. Fichiers locaux :
`bench/plugin-symboles-0111n/`. Les profils du PC du matin ne sont plus présents sur ce M4.

## `___stub_getrealaddr` n'est pas le nom générique des appels indirects

[C] `GLDriver-POMPPC`, binaire installé, MD5 `860690d582e1ac383f8c4dc0c12e0fee` :
`nm -nm` et `otool -tvV` placent `___stub_getrealaddr` à `0x0003c734`, jusqu'au `blr`
à `0x0003c7d4`. La section `__TEXT,__text` finit à `0x0003c7d8`. Le seul appel à ce
résolveur visible dans cette fin du texte est celui de `_fprintf$LDBLStub`, à `0x0003c6e8`.
Le stub lit un pointeur en cache, appelle le résolveur si ce pointeur est nul, puis le conserve
et saute vers lui. Le résolveur construit un nom et appelle
`NSIsSymbolNameDefinedWithHint`, `NSLookupAndBindSymbolWithHint`, puis `NSAddressOfSymbol`.

[C] Dans ce même binaire, les **82 stubs d'importation** sont dans
`__TEXT,__picsymbolstub1`, de `0x0003c7e0` à `0x0003d220` (borne exclue), par entrées de
32 octets. Ils suivent donc immédiatement le dernier symbole nommé de `__text`.
Exemples, donnés par `otool -Iv` :

| adresse relative au bundle | importation |
|---|---|
| `0x0003c880` | `pthread_mutex_lock` |
| `0x0003ca00` | `getenv` |
| `0x0003ca40` | `pthread_mutex_unlock` |
| `0x0003cb60` | `memcpy` |
| `0x0003cea0` | `memcmp` |

[F] L'étiquette `__stub_getrealaddr` à 1,0 % dans le profil PC du matin **ne suffit pas** à
en déduire un coût d'appels internes par pointeur, ni à recommander `static` ou une visibilité
cachée. Il faut les adresses ou décalages des échantillons et le bundle exact du profil pour
distinguer le résolveur des stubs voisins. À trancher avec les nouveaux profils invités,
puis à refaire avec les fichiers exacts du PC. Aucun pourcentage du matin n'est réattribué
sur la seule base de ce binaire M4.

## Le bloc libSystem `0x90131200`

[C] `/usr/lib/libSystem.B.dylib` du M4, MD5 `807198001a02bd6c3cc9413c80bbd312` :
`nm -n` donne `_bcmp` et `_memcmp` à **`0x901311a0`**, puis `_dprintf` à `0x901312b8`.
L'adresse `0x90131200` appartient au corps commun de comparaison mémoire (`+0x60`).
Le désassemblage confirme la boucle de comparaison ; ce n'est pas une identification de
`memcpy` ni d'un verrou. Reste à identifier ses appelants dans les profils, et à contrôler
l'empreinte de libSystem sur le PC avant de lui appliquer cette correspondance.

## Reproduire sans les outils de développement de Tiger

`tools/re/machonm.py --stubs BINAIRE` lit `LC_DYSYMTAB` et les sections `S_SYMBOL_STUBS`
du Mach-O ppc, universel compris. Il imprime adresse, taille et importation de chaque stub.
Il ne rajoute pas les importations aux symboles définis rendus par `-n`, utilisés par
`partfil.py` pour attribuer les piles au plugin.

Validation : les 82 adresses et noms du bundle installé sont identiques à `otool -Iv` ; trois
tests synthétiques couvrent la tranche ppc, un premier index indirect non nul, les marqueurs
LOCAL/ABS, le refus d'un index hors table et la conservation des seuls symboles définis.
