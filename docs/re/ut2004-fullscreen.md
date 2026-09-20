# UT2004 : plein écran 800×600 et 1024×768

Correctif du 20 septembre 2026, validé dans la VM de développement Tiger
`disks/tiger-dev.raw`. Le pilote compilé est installé dans cette VM.

## Cause et correction

`CGLSetFullScreen` appelle `gldAttachDrawable` avec le type 54. Le
`glsAssignDrawable` de Tiger rejette ce type sans condition (`10005`, invalid
drawable). Le pixel format et le changement de mode du framebuffer fonctionnaient.

Le plugin traduit cette demande en drawable mémoire (type 53, descripteur
`{largeur, hauteur, pas, adresse}`), dimensionné sur l'écran principal 32 bits.
Un tampon arrière appartient au contexte ; Apple gère toujours profondeur,
stencil et rendu logiciel. Le verrou logiciel interdisant un drawable mémoire
avec double buffering (`ctx+0xe1`) est levé uniquement pendant l'attachement,
puis restauré. Le pixel format public garde le double buffering.

À l'échange, la présentation accélérée existante écrit en mémoire vidéo.
Si l'image vient du rendu logiciel, ou si `POMPPC_GL_DIRECT=0`, elle est
synchronisée puis copiée depuis le tampon arrière. Les transferts en vol sont
terminés avant libération du tampon lors d'un détachement ou d'une destruction.
Ce chemin vise l'écran principal de la VM, en 32 bits ; le multi-écran et les
formats 16 bits ne sont pas couverts.

## Lancer le jeu

Le job conserve une sauvegarde `UT2004.ini.before-fullscreen`, fixe
`StartupFullscreen=True` et `FullscreenViewportX/Y`, puis lance la démo dans
la session graphique. Le réglage persiste aussi pour les prochains lancements
par le Finder. Il ferme l'instance précédente d'UT2004.

```
mkdir -p /tmp/ut-launch-fullscreen
cp tools/guest/jobs/utfullscreen/job.sh /tmp/ut-launch-fullscreen/
printf '800x600\n' > /tmp/ut-launch-fullscreen/resolution.txt
DEVDISK=disks/tiger-dev.raw python3 tools/guest/devloop.py run /tmp/ut-launch-fullscreen --timeout 90
```

Pour la seconde résolution, écrire `1024x768` dans `resolution.txt` et rejouer
le job. Le fichier absent choisit 800×600. Aucun `-windowed` n'est utilisé.
Le job de profil stationnaire `utprofile` garde ses conditions de mesure.

## Validation

Sonde `guest/gltest/fullscreen.c`, compilée dans Tiger avec le SDK 10.4u :
800×600 et 1024×768, chacun avec présentation directe, copie forcée, et
soumissions synchrones. Pour chaque combinaison : trois attachements et
détachements, contrôle rouge/vert/bleu dans la mémoire vidéo et pixel jaune
produit par `glDrawPixels` (repli Apple). **6/6 combinaisons réussies**.

Non-régression hors écran : `tri`, `varray`, `varrayvbo`, `mixte`, `lit`,
`texgen`, `game`, en 256×256 : **7/7 contrôles de pixels réussis**.

Preuves locales : `bench/ut-fullscreen/` (journaux et captures QMP).
Le débit du menu ne constitue pas une mesure de jouabilité en partie.
