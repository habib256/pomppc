# docs/re — relevés de rétro-ingénierie d'OpenGL.framework (Tiger 10.4.6)

Un fichier par sujet. Chaque relevé donne : le binaire, l'adresse, ce qui a été lu, et **comment
le vérifier** (sonde `guest/gltest`, trace `POMPPC_GLTRACE`, diff de vidages). Rien de deviné.

Les désassemblages annotés sont produits par `tools/re/ppcanno.py` à partir des binaires du
système installé ; ils ne sont pas dans le dépôt (code d'Apple). Une copie de travail est gardée
hors dépôt (disque USB : `pomppc/re/`).
