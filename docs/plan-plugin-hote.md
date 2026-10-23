# Déplacer le travail du plugin vers l'hôte (23/10/2026)

Choix de l'utilisateur après le levier E (`docs/plugin-allegement.md` §7) : les petits leviers
du plugin sont épuisés (d3q neutre en jeu, `d3-plugin-x86.md` §10.6 ; UVWIDE −1,5 %), alors que
le plugin fait encore **17 à 31 % du fil du jeu** (profil `bench/tcg/ab/gprof-2210b-*`, six
leviers A+B allumés) :

| jeu (ms/image) | plugin | dont dessin des tableaux | dont verdict au dispatch |
|---|---|---|---|
| Marble Blast (9,8) | 25 % | — (`geom_begin` 10,6 %) | 4,8 % |
| Zen (5,1) | 17 % | — | 5,2 % |
| UT2004 (42,6) | 30 % | 13,5 % | 10,2 % |
| DOOM 3 (94,9) | 31 % | 17,3 % | 8,8 % |

Dans DOOM 3, c'est ~28 ms par image, soit ~23 µs de code PowerPC traduit par TCG pour chacun
des ~1 250 dessins. Le même travail, en code natif de l'hôte, coûterait de l'ordre de 20 à
50 fois moins.

## 1. L'idée : un appel synchrone, pas un DMA

`d3-plugin-x86.md` §1 a écarté la lecture des tableaux de l'invité par l'hôte, mais **en
asynchrone** : quand le fil de rendu traite le dessin, le jeu a déjà réécrit sa mémoire, et
lire en place exigerait un instantané. Ici, l'appel est **synchrone**, dans le fil du vCPU,
au moment où le plugin l'aurait fait lui-même :

1. le plugin écrit les arguments dans BAR0 (déjà projeté chez lui), puis une écriture 32 bits
   dans une **page d'appel** MMIO, projetée elle aussi dans son espace par le kext ;
2. sous TCG, cette écriture n'est **pas une sortie de VM** : c'est un appel au gestionnaire
   MMIO de QEMU, dans le fil du vCPU, sans noyau invité ni `mach_msg` (la sonnette actuelle
   passe par le kext, `IOConnectMethod*`, et prend le BQL : `qgpu-pci.c` l. 604) ;
3. le gestionnaire lit l'état de GLEngine et les tableaux **dans la mémoire virtuelle du
   processus courant**, par la MMU du vCPU (segments et table de hachage du G4, ceux de
   l'instant), fait le travail en natif, écrit le résultat (verdict, commandes du flux) dans
   BAR0 et rend la main. L'état lu est exactement celui que le plugin aurait lu.

Ce qui tombe des objections du §1 : pas d'instantané (synchrone) ; pas d'épinglage par le
kext (QEMU traduit l'adresse virtuelle à l'appel ; une page absente rend « repli » et le
plugin fait le travail lui-même, comme aujourd'hui) ; la parité arm64 est presque gratuite
(du C du device, pas du code du JIT ; `pomppc-parite-hotes`).

## 2. Ce qui pourrait passer à l'hôte

Tout ce qui **lit** l'état de GLEngine et **écrit** dans le flux, sans appeler GLEngine :
verdict au dispatch (`geom_ok`, `texture_ok`, `geom_format`, clé), `send_state`,
`geom_send_all` (comparaisons de matrices et de lumières), plan d'attributs, balayage et
recopie des indices (le `memcpy` natif lit la mémoire invitée directement : 1,8 Mio/image
dans DOOM 3). Reste dans l'invité : ce qui appelle GLEngine ou le kext (téléversement des
textures par `gle*`, crochetage, soumission), et tout ce que l'hôte refuse (repli).

Coût caché à surveiller : la **traduction des adresses**. Chaque lecture d'une structure de
GLEngine est une adresse virtuelle invitée ; il faut un petit cache de pages (VA → pointeur
hôte) valide le temps d'un appel, invalidé par tout changement de segment ou de table.

## 3. Phase 0 : la porte (mesures, pas de levier)

Trois questions, chiffrées sur le PC (binaire rapide, VM de dev) :

1. **Coût d'un appel vide** depuis l'espace utilisateur de l'invité : écriture dans la page
   d'appel → gestionnaire → retour, avec la région MMIO **sans BQL**
   (`memory_region_enable_lockless_io` ou équivalent de QEMU 11.1.2). À comparer à un appel
   de fonction de ~200 instructions dans l'invité, et à la sonnette actuelle par le kext.
2. **Coût de lecture de la mémoire virtuelle invitée** depuis le gestionnaire : traduction
   par la MMU du vCPU (`ppc_xlate`/`cpu_get_phys_page_debug` ou le TLB de TCG), avec un
   cache de pages par appel ; débit pour 64 o, 1 Kio et 64 Kio lus en plusieurs pages, contre
   un `memcpy` de l'invité.
3. **Un travail réel** : `geom_send_all` (ou la clé du verdict, `vd_key_of`) refait en natif
   sur l'état de `d3inter`, comparé à la version invitée, en instructions invitées retirées et
   en µs par dessin.

**Porte** : un appel vide sous ~1 µs, et la lecture d'état native au moins 10× plus rapide que
le code invité. Sinon, le projet s'arrête là (comme le second niveau du traducteur,
`plan-traducteur-rapide.md` §10.8).

Coûts d'entrée connus : un type de mémoire de plus dans le user client (projection de la page
d'appel) — c'est `qgpu_abi.h`, donc **kext reconstruit et redémarrage** (`pomppc-proto-kext-rebuild`) ;
QEMU à reconstruire (les deux cibles, `run_tiger.sh` lance `qemu-system-ppc64`).

## 4. Suite si la porte passe

Phase 1 : le verdict au dispatch et `geom_send_all` en natif derrière `POMPPC_GL_HOSTCALL`
(éteint, `=2` contrôle : les deux calculs, comparés), preuves `plugmd5`/`gpu`, A/B des quatre
jeux. Phase 2 : le dessin des tableaux (plan, indices, `send_state`).
