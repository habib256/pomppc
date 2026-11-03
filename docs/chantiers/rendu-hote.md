# Fil de rendu de l'hôte : convertir les attributs une fois

> Fiche de chantier ; index et priorités : [`TODO.md`](../../TODO.md).

Piste non lancée.

## Piste proposée le 01/11 (à arbitrer par l'utilisateur)

3. **Fil de rendu de l'hôte : convertir les attributs une fois.** `nat_conv_attr` fait 27 % de
   `qgpu-render` (attributs reconvertis à chaque dessin natif ; à convertir au `BUF_SUBDATA`).
   Sur le PC le fil prend 20 ms par image de d3 sans bloquer l'invité ; il dispute aussi les
   cœurs rapides aux vCPU sur mb et zen. **Profil M4 du 02/11 fait** : sur d3, 0,178 cœur de
   travail estimé et 82,2 % d'attente de condition (`docs/profil-m4-0211.md` §2), ce qui ne
   justifie pas de placer ce levier en tête sur le M4. **Fermeture :** A/B des cinq jeux sur
   les deux hôtes. Gain utile à chiffrer selon le jeu ; réduction de CPU hôte attendue.
   Coût : deux jours. Aucune optimisation de cette piste lancée.
