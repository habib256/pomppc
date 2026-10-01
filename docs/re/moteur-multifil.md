# Le moteur OpenGL multifil (`kCGLCEMPEngine`) sous Tiger 10.4.6 : absent

Question (01/10/2026) : les jeux lents de la matrice (DOOM 3, Prey, Colin McRae, Nexuiz ARB)
n'occupent qu'un vCPU, le second reste au repos (`docs/smp-coeurs.md` §2.3). Si GLEngine savait
déjà travailler sur un fil à lui (`CGLEnable(ctx, kCGLCEMPEngine)`, valeur 313), GLEngine et notre
plugin passeraient sur le second vCPU sans rien écrire.

## Ce qui a été lu (VM quotidienne, 10.4.6, build 8I128)

1. **En-têtes du SDK** `MacOSX10.4u.sdk` (`CGLTypes.h`) : l'énumération `CGLContextEnable`
   s'arrête à `kCGLCEDisplayListOptimization = 307` ; pas de 313.
2. **Imports de GLEngine** (`OpenGL.framework/Resources/GLEngine.bundle/GLEngine`, 30/01/2006) :
   `nm -u` ne donne que `pthread_mutex_{init,destroy,lock,unlock}` ; **ni `pthread_create`, ni
   condition, ni sémaphore**. GLEngine ne crée aucun fil.
3. **À l'exécution** (programme de 15 lignes, contexte accéléré, plugin POMPPC) :

       CGLEnable(313)      -> 10010 (kCGLBadEnumeration)
       CGLIsEnabled(313)   -> 10010, 0
       CGLEnable(307)      -> 0        (contrôle)

## Et en 10.4.8 à 10.4.11 ? (recherche du 01/10)

- Apple, note technique TN2085 (« Enabling multi-threaded execution of the OpenGL framework »),
  section Availability : « Mac OS X 10.4.7 or later on Mac Pro Macintosh systems & 10.4.8 or
  later for other Intel-based Macintosh computers ». Rien pour PowerPC.
- Blizzard (Tigerclaw, forum WoW, 25/10/2006), à propos du MP engine de WoW 2.0 : « We're close to
  the limit of what we can accomplish on 10.4.x OpenGL on PowerPC (which is very different from
  10.4.x OpenGL on Intel presently) » ; « If Leopard brings multi-thread GL support back to
  PowerPC… ». MacRumors et Macworld (12/2006) : multithreaded GL pour les Mac Intel en 10.4.8,
  pas les PowerPC multiprocesseurs.

Le moteur multifil n'existe donc pas sur PowerPC dans toute la série 10.4 : passer en 10.4.11
pour lui serait vain. Leopard (10.5, G4 ≥ 867 MHz) annonçait le moteur multifil sans préciser
l'architecture ; ce serait une autre migration (GLEngine LLVM, plugin à refaire), non étudiée.

## Conclusion

Pas de moteur multifil dans Tiger 10.4.6. Le mettre à jour n'est pas une option : les offsets de
GLEngine dont dépend le plugin valent pour 10.4.6 (`docs/gpu-3d-tiger.md` §4). Occuper le second
vCPU demanderait un fil de travail **dans notre plugin** (encodage, verdict et envoi décalés d'un
dessin ou d'une image, état copié), chantier à chiffrer d'après le profil du 01/10
(`bench/vitesse/profil-20261001`).
