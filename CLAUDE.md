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
