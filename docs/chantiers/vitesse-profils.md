# Vitesse : ce que les profils ont établi

> Fiche de chantier ; index et priorités : [`TODO.md`](../../TODO.md).

Synthèse commune aux chantiers de vitesse. Les chiffres courants sont en tête du `TODO.md`.

**Ce que les profils ont établi** (`docs/vitesse-doom3-x86.md` §18-§25, `docs/tcg-g4.md` §44-§47) :
- Sur le PC, le goulot du code généré est le **chargement des instructions** (frontend bound
  40 %, 100 octets de x86 par instruction PowerPC) et la latence mémoire, pas le nombre
  d'instructions : retirer des tests hors du chemin critique ne rapporte plus rien (plafond
  mesuré 3,5 %). Les leviers restants du JIT x86 sont petits et génériques : recherche de
  blocs (mb 10 %, zen 14 %, cmr 6 % du vCPU), base de temps (ut 14 %), PGO.
- **Le plugin GL de l'invité est le levier commun aux deux hôtes** : 28 % du vCPU de cmr,
  22 % de d3, 16 % de mb, 12 % de zen, 11 % d'ut sur le PC. Chaque pourcent gagné là vaut sur
  x86 et sur arm64 sans portage, et c'est le seul levier qui agit sur cmr (PACKBITS −3,5 %
  quand les leviers TCG le laissent neutre ou le ralentissent).
- Le fil de rendu de l'hôte (`qgpu-render`) occupe 55 % d'un cœur sur mb, 41 % sur zen, 25 %
  sur d3 (≈ 20 ms par image). Sur le PC, l'invité ne l'attend pas (0,1-3 ms/image) ; sur le M4,
  où d3 est à 29 ms, il peut devenir le chemin critique : à mesurer.
