# Banc caméra UT2004 Demo : AS-Convoy

Le jeu livré dans la VM contient une cinématique d'introduction dans
`AS-Convoy`. `ASGameInfo.Login` déclenche `IntroScene` en solo si le joueur
n'est pas spectateur et si `QuickStart` est absent. **`QuickStart=False`
active quand même QuickStart** dans cette démo : le code teste la présence
de l'option. DM-Rankin ne contient pas de SceneManager de flyby.

Le dossier `Benchmark` de cette installation ne contient aucun lanceur :
seulement les sous-dossiers de résultats et un `Stuff/timedemo.txt` vide.
Le parcours utilisé ici est donc la cinématique livrée avec AS-Convoy,
pas un supposé benchmark accessible depuis le menu.

## Méthode

Le job `tools/guest/jobs/utflyby/` :

- ferme l'instance précédente, sauvegarde les préférences et les restaure ;
- lance AS-Convoy sans bots, en plein écran, quatre unités de texture ;
- fixe `MinDesiredFrameRate=0` pour éviter une qualité adaptée au débit,
  et désactive la synchronisation verticale ;
- utilise une bibliothèque temporaire de mesure, jamais installée dans le jeu ;
- mesure les intervalles entre les swaps 13 et 73, soit 60 intervalles ;
- capture l'écran au swap 74, après la fenêtre mesurée, puis ferme le jeu.

La capture lit directement le framebuffer (`frame-74.ppm`) : la capture
Quartz renvoie un tampon noir dans ce mode de présentation. Le rapport
vérifie les dimensions, la longueur et la présence de pixels non noirs ;
une inspection visuelle reste nécessaire.

Le temps de simulation avance de 0,2 seconde par appel `UGameEngine::Tick`.
Ce pas assez large permet d'échantillonner 12 secondes du parcours malgré
le rendu actuellement très lent. Ce n'est pas un test de latence ni une
simulation de gameplay à 30 Hz. L'horloge des mesures reste le temps réel.

Le drapeau exporté `GUseFixedTimeStep` ne suffit pas sur ce binaire Mac :
`CMainLoop::RunLoop` l'ignore. Le premier essai a été rejeté car `GDeltaTime`
restait variable. La bibliothèque résout les symboles de `UGameEngine`,
vérifie le pointeur original de `Tick` dans sa table virtuelle et remplace
cet appel **en mémoire dans le seul processus testé**. Elle fournit 0,2 à
`Tick` et à `GDeltaTime`. Si les symboles ou le pointeur manquent, elle refuse
de lancer le test. Aucune instruction ni aucun binaire sur disque n'est patché.

Le mode `-benchmark` reste désactivé : il avait planté dans
`FStats::UpdateString`. `clock.csv` relève chaque swap SDL, le pas réellement
observé et `GIsBenchmarking`. Le rapport refuse une capture incomplète, un
changement de contexte, des images manquantes ou un pas de temps incorrect.
La graine libc est fixée à zéro ; vérifier les captures et compteurs avant
d'affirmer une identité complète de tous les effets et particules.

## Reproduction

VM de développement déjà démarrée en mode bureau avec le pilote plein écran.
La copie doit inclure les trois fichiers du job, pas seulement `job.sh`.

```
mkdir -p /tmp/ut-flyby-run
cp tools/guest/jobs/utflyby/* /tmp/ut-flyby-run/
printf '800x600\n' > /tmp/ut-flyby-run/resolution.txt
DEVDISK=disks/tiger-dev.raw python3 tools/guest/devloop.py run /tmp/ut-flyby-run --timeout 640
python3 tools/guest/flyby_report.py bench/devloop/last/out
```

Remplacer `800x600` par `1024x768` pour le second format. Chaque sortie doit
être archivée avant le passage suivant (`bench/devloop/last` est un lien mobile).
Ne pas manipuler le jeu pendant le test. Le job sauvegarde les réglages effectifs,
les horloges, les statistiques du pilote et les captures.

Fermer Zenerchi avant le lancement. Un essai avec les deux jeux actifs a
laissé une image UT figée : le rapport de crash situe la faute dans
`CGDisplaySwitchToMode`, appelé par `SDL_VideoQuit` lors de la sortie du
plein écran. Le lien causal avec Zenerchi reste à confirmer. Cet essai est
écarté (`bench/ut-flyby/rejected-concurrent-zenerchi/`). Le job refuse
Zenerchi actif et ne marque plus comme complet un passage qui plante à la
fermeture.

## Conditions et premiers diagnostics

### Référence du 20 septembre 2026

Deux passages terminés avec code de sortie 0, préférences restaurées,
sans Zenerchi. Les captures au swap 74 montrent le même point de la
cinématique ; les défauts de rendu restent présents.

| Résolution | Images/s | Médiane | P95 | Durée des 60 intervalles |
| --- | ---: | ---: | ---: | ---: |
| 800×600 | 1,077 | 1 089 ms | 1 269 ms | 55,71 s |
| 1024×768 | 0,691 | 1 703 ms | 1 978 ms | 86,80 s |

Preuves : `bench/ut-flyby/800x600/` et `bench/ut-flyby/1024x768/`
(`report.json`, traces CSV, `frame-74.ppm`, `exit-status.txt`).
Ce sont des mesures uniques, pas des moyennes de répétitions. Les deux
passages comptent environ 2 242 appels logiciels et 29,43 relectures par
image ; le nombre de sommets varie de moins de 0,2 % entre résolutions.

### Diagnostic

Tiger, G4, un CPU, 1 Gio, QEMU `x-fast-fp=on`, QGPU `backend=auto`, Cocoa.
La quotidienne `disks/tiger.qcow2` n'est pas modifiée.

La découverte du parcours a révélé des milliers de refus `id-texture`, puis
un rendu principalement logiciel. Le protocole réserve actuellement 128
identifiants de texture par client (`QGPU_CLIENT_TEX_IDS`). Les fichiers de
preuve sont dans `bench/ut-flyby/`. Les images présentent encore des défauts :
ce banc mesure l'état actuel du pilote, pas un rendu validé comme correct.

Cette référence précède le cache avec éviction : `texture_uploadable()`
refusait les nouvelles textures dès que les 128 identifiants étaient pris,
jusqu'à la destruction d'un objet GL. Le cache réutilise désormais les
identifiants après avoir terminé les commandes en vol, protège les unités
actives du dessin et invalide les états des contextes. `gltest texcache`
vérifie 257 textures vivantes, leur modification après éviction et deux
contextes partagés. Résultats : [cache de textures](ut2004-texture-cache.md).
