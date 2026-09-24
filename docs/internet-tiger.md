# Internet sous Tiger (relais HTTPS→HTTP)

Extrait du README, déplacé le 24/09/2026. Relais : `scripts/web-proxy.py` ; réglage invité : `guest/net/proxy.sh`.

## Fonctionnement

`run_tiger.sh` active le réseau par défaut (NAT utilisateur de QEMU : Tiger reçoit
`10.0.2.15` par DHCP, l'hôte est `10.0.2.2`, le DNS `10.0.2.3`). HTTP fonctionne tel quel,
mais **HTTPS non** : Tiger n'a qu'OpenSSL 0.9.7 et des certificats de 2006, que les sites
actuels refusent.

D'où le relais `scripts/web-proxy.py`, lancé et arrêté avec QEMU : il n'écoute que sur
`127.0.0.1:8080` (vu de Tiger : `10.0.2.2:8080`), va chercher les pages en HTTPS moderne
avec les certificats de l'hôte et les rend en HTTP, liens réécrits. Dans Tiger, une fois :

```sh
sudo sh /Volumes/POMPPCSRC/guest/net/proxy.sh on    # CD présent avec GPU=1 ou GLISO=1
```

(ou Préférences Système → Réseau → Proxys → Proxy web (HTTP) : `10.0.2.2`, port `8080`),
puis taper les adresses en `http://` dans Safari. **Page d'accueil : `http://pomppc/`**
(recherche Google, sites légers, réglages). `WEBPROXY=0` ne lance pas le relais, `NET=0`
coupe le réseau.

Le relais ne s'arrête pas au TLS : Safari 2 ne comprend ni le JavaScript moderne, ni WebP,
AVIF ou SVG. Selon le **mode** du site, réglable par la barre jaune en tête de chaque page :

| mode | ce que reçoit le vieux navigateur |
|---|---|
| **auto** (défaut) | *sans JS*, et *rendu* si la page sort vide ou demande JavaScript (appris par site) |
| sans JS | la page sans ses scripts, `<noscript>` déplié — plus légère de moitié |
| rendu | Chrome, sans affichage, exécute la page sur l'hôte ; l'invité reçoit le document obtenu |
| brut | la page telle quelle |

Dans tous les modes : images WebP, AVIF et SVG converties, grandes images réduites à 1024 px,
bannières de cookies et éléments que Safari 2 afficherait à tort retirés. Des **adaptateurs**
servent les sites fermés à tout autre qu'un navigateur récent : **recherche Google** (rendue
par Chrome, résultats en HTML simple), **Reddit** (lu par ses flux), **Stack Overflow** et
Stack Exchange (par leur API). Dépendances facultatives, détectées au démarrage :
BeautifulSoup + lxml, Pillow, ImageMagick, Google Chrome.

Mesure : `tools/web/banc.py` passe 22 sites par une instance du relais en se présentant comme
Safari 2 — **21 lisibles** contre 15 avec le simple relais TLS, 131 images décodables sur 133,
plus aucun script envoyé (19/09/2026) ; archive.org reste fermé.

