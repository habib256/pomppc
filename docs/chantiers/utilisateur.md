# Ce qui attend l'utilisateur

> Fiche de chantier ; index et priorités : [`TODO.md`](../../TODO.md).

Le travail est fait ; il ne manque que l'essai ou la décision de l'utilisateur.

- [ ] **[Paquet] Éprouver la 0.4.1 sur macOS 15 et M1** (publiée le 03/11, essayée sur M4 sous macOS 27
  seulement) : accord de Gatekeeper sur l'app réellement téléchargée, démarrage, `x-tb-fast`, un jeu, sur
  le M1 de l'utilisateur sous 15.8. Aussi, faute de média ou de geste réel ici : clics réels dans
  l'assistant, vrai installateur de Tiger et d'OS 9, combo sur une vraie 10.4.6, écran Retina ; pour le
  dossier partagé, accord de confidentialité sur Documents ou Bureau, très gros dossier, fichier avec
  resource fork écrit sous OS 9 (`docs/etat-des-hotes.md` §3). **Fermeture :** essai juste, ou correctif
  publié.
- [ ] **[Frontend] Curseur unique sous ImGuiDock** (`50a9801`, 01/11) : **réglé sur le M4,
  confirmé par l'utilisateur le 02/11** ; à voir à l'œil sur le PC au prochain
  `run_tiger.sh` en fenêtre (un seul curseur sur l'image, flèche de l'hôte de retour sur les
  menus et bandeaux, rien de collé après un aller-retour en plein écran).
- [ ] **[Plugin] Replis à retirer** : `POMPPC_GL_VERDICT=0`, `POMPPC_GL_WHITELIST=0`,
  `POMPPC_GL_TEXMEMO=0`, `POMPPC_GL_STSKIP=0`. **Reprise :** mesure en jeu par l'utilisateur ;
  ensuite retirer les options devenues inutiles et vérifier la matrice.
- [ ] **[Outils] Captures, dix tours consécutifs verts** : à jouer sur le banc d'endurance quand
  l'utilisateur le demande.
- [ ] **[TCG — PC] Gouverneur `performance`** (`docs/vitesse-doom3-x86.md` §5.4) : un A/B à
  faire, il demande la racine.
