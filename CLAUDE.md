# POMPPC : consignes pour les agents IA

POMPPC fait tourner Mac OS X Tiger (PowerMac G4) sous QEMU, avec un GPU paravirtuel (qgpu) qui
rend les jeux OpenGL sur le GPU de l'hôte. Le projet est mené en français.

**Le travail alterne entre deux machines : un PC Linux x86-64 et un Mac M4 (arm64).**
Avant toute action :

1. `uname -sm` dit sur quel hôte la séance se déroule.
2. `git pull`.
3. Lire **[docs/etat-des-hotes.md](docs/etat-des-hotes.md)** : où en est chaque hôte, ce qui
   reste à faire sur celui-ci, et les règles communes (parité, vitesse générique, A/B au repos,
   ne jamais supprimer `.run/tiger.lock`).

En fin de séance, mettre à jour `docs/etat-des-hotes.md` (section de l'hôte, avec date et
heure), puis commit sur `main` et push.

Autres repères : `TODO.md` (priorités), `CHANGELOG.md` (travaux clos), `docs/` (études),
`docs/vitesse-doom3-x86.md` (vitesse sur le PC), `docs/parite-m4-2510.md` (parité sur le M4).

## Les grades de preuve

**Le silicium est la référence.** POMPPC s'appuie sur QEMU, mais QEMU n'est pas
l'oracle : c'est une lecture du matériel faite par d'autres, utile et faillible.
Toute affirmation sur le matériel porte **son grade et sa source**, dans un
crochet en tête de commentaire, pour qu'un lecteur et un `grep` s'accordent sur
ce qui soutient le code :

| Grade | Preuve | Pour POMPPC |
|---|---|---|
| **A** | Mesure sur **machine réelle** | Un vrai PowerMac G4, ou une mesure sur l'hôte quand le point porte sur lui. |
| **B** | **Document primaire** | *PowerPC Programming Environments*, fiches techniques G4 / MPC107, Apple Developer Notes, Open Firmware 1275. |
| **C** | **Désassemblage de ROM** | Mac OS ROM, Open Firmware, pilotes Tiger. Preuve du *contrat* attendu par le logiciel, pas du silicium. |
| **D** | **Un autre émulateur** | QEMU, MAME, SheepShaver, PearPC — à égalité. |
| **E** | **Affirmé sans source.** | |
| **F** | **Inconnu assumé** | deviné, ajusté, approximatif, non vérifié. |

Trois règles. **Une citation de grade D nomme sa révision** (tag ou commit) :
les numéros de ligne amont bougent à chaque commit, et une plage sans révision
ne désigne rien. **Un grade inférieur ne l'emporte jamais sur un grade
supérieur** — là où POMPPC s'écarte de QEMU parce qu'un document, une ROM ou
une mesure dit autre chose, c'est correct, et le commentaire le dit ; cela
protège le code d'un futur « alignement sur QEMU » qui serait une régression.
**Un F se garde, il ne se cache pas** : écris ce qui le trancherait.

```
// [B] MPC107 User's Manual § 4.2 — le registre se relit après le POST.
// [D] QEMU hw/ppc/mac_newworld.c:312-330 @ v9.1.0 — idem, tel que QEMU le lit.
// [C] Mac OS ROM $FFC02A40 — le pilote attend l'ordre inverse.
// [A] Mesuré sur un vrai G4 : 133 MHz de bus.
// [F] Inconnu : ajusté jusqu'à ce que Doom 3 tienne. Tranché par : un profil.
```

Cette hiérarchie est commune à tous les émulateurs du même auteur (POM2, POM1,
POMIIGS, POM68K, POMPPC, NeoST, RasterSystemGG).
