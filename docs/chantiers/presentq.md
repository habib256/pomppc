# `POMPPC_GL_PRESENTQ` rouvert (PC)

> Fiche de chantier ; index et priorités : [`TODO.md`](../../TODO.md).

**Clos le 04/11/2026 : écarté sur chiffres** ([`ecartes.md`](ecartes.md)). A/B rejoué au repos le
03-04/11 sur le PC (`bench/tcg/ab/presentq-0311-{cmr,mb,ut}`, plein écran, 3 parties par bras,
kext et plugin `20261102-modes`, aucune partie « pas de ssh »). Écart de `pq` vers `ref`, médianes :

| cellule | `ref` ms/image | `pq` ms/image | écart |
|---|---:|---:|---:|
| cmr-pe | 77,68 | 79,37 | `pq` plus lent de 2,13 % |
| mb-pe | 8,69 | 8,71 | `pq` plus lent de 0,20 % |
| ut-pe | 37,69 | 37,55 | `pq` plus rapide de 0,38 % |

Aucune cellule ne gagne et tout reste sous le seuil de 3 % sur trois parties : la règle de fermeture
ci-dessous n'allume pas le levier. `ut` `pq-1` a une partie à 51,7 ms [F : cause non cherchée, le
levier n'étant pas retenu]. La variante pour le chemin QE, subordonnée à un gain net en plein écran,
n'est pas instruite.

- [x] **[Plugin] `POMPPC_GL_PRESENTQ` rouvert (01/11, demande de l'utilisateur)** : image n mise
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
