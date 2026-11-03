# `POMPPC_GL_PRESENTQ` rouvert (PC)

> Fiche de chantier ; index et priorités : [`TODO.md`](../../TODO.md).

A/B à rejouer sur le PC.

- [ ] **[Plugin] `POMPPC_GL_PRESENTQ` rouvert (01/11, demande de l'utilisateur)** : image n mise
  en file avant d'attendre la n−1 quand l'hôte présente (`SURF_PRESENT`) ; « non tranché » depuis
  le 22/10 (neutre en VM de dev, mb −0,2 ms). Sous QE, les jeux en fenêtre présentent par le
  kext (`present_qe`) : le levier n'agit qu'en **plein écran** (`present_direct`). A/B au repos
  lancé le 01/11 à 13 h 28 (`bench/tcg/ab/presentq-0111-{cmr,mb,ut}`, 3 parties par bras, pe) :
  **raté**, toutes les parties « pas de ssh » (binaire rapide refusé, empreinte de la série
  changée par un commentaire de `qgpu_proto.h`, remis à l'identique) ; à rejouer entièrement
  au retour sur le PC par `bench/tcg/ab/presentq-0111-chaine.sh`, **après** la reconstruction
  de `build/` et `build-fast/` qu'impose `tcg/0064` (`etat-des-hotes.md` §2).
  **Fermeture :** allumé si aucune cellule pe ne perd et qu'une gagne, sinon classé « écarté sur
  chiffres » ; variante pour le chemin QE (ne pas attendre la n−1 avant la présentation par le
  kext) à instruire si le gain en plein écran est net.
