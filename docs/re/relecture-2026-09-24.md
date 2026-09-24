# Relecture adversariale des commits du 24/09/2026

Commits relus : `68a2d6c` (v16/v17), `cc1b0df` (chemin tableaux, crochet de plantage, LAYOUT),
`bc7ed62` (COPY_TEX, gardes de niveaux), `4754dd4` (gardes des procédures à pointeurs).
Les numéros de ligne renvoient à `git show 6bf2563:guest/gldriver/pomppc_accel.c`, sauf mention
contraire. Chaque défaut a été vérifié dans le code. Les hypothèses écartées sont listées à la fin.

Classement : **P** = plantage ou corruption, **I** = image fausse, **R** = performance,
**S** = style ou comportement indéfini sans effet observé.

---

## P1. Les trois gardes ne sont posées qu'avec `POMPPC_GL_NOTE`

`crash_hook_install` (9231-9236) sort tout de suite si `POMPPC_GL_NOTE` n'est pas défini.
Sans ce journal, aucun gestionnaire n'est installé : `pack_jmp_on`, `sig_jmp_on` et
`proc_jmp_on` sont posés pour rien.

**Scénario** : on lance DOOM 3 ou Prey sans le journal (cas normal, seul `UT2004-trace.sh` le
définit). Une lecture de niveau illisible (`tex_lv0_sig`, `_currentRender`), un lot de tableaux
qui déborde sur une page non mappée ou un pointeur mort dans `a_polygon_ptr` envoie SIGSEGV au
gestionnaire du jeu. Celui-ci appelle `Quit()`, puis `gldDeleteTexture`, qui attend `G.mu`,
toujours tenu : **le processus se fige**, comme avant les trois commits. Les résultats « Prey
7 min, DOOM 3 10 min sans plantage » ne valent que journal activé.

**Correctif** : installer le crochet sans condition dans `pomppc_backend_init`, et garder
l'écriture du rapport sous condition (le chemin sans journal existe déjà, `fd = -1`) :
```c
static void crash_hook_install(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGBUS, &sa, &crash_prev[0]);
    sigaction(SIGSEGV, &sa, &crash_prev[1]);
}
```
Le plugin tourne aussi dans le WindowServer et `loginwindow`. Le crochet y est inerte hors des
gardes : il rend la main au gestionnaire précédent et la faute se reproduit. Deux compléments :
- Un jeu peut réinstaller son propre gestionnaire après nous, et les gardes seraient alors mortes
  en silence. Il faut vérifier une fois par image (`PROC_Swap60`) avec
  `sigaction(SIGSEGV, NULL, &cur)` et réinstaller si `cur.sa_sigaction != crash_handler`.
- Après une faute hors garde, le crochet remet le gestionnaire précédent **pour toujours**
  (9229). Il faut l'écrire dans le journal : les gardes sont mortes à partir de là.

## P2. `PTR_PROC_GUARD` : la garde couvre le rendu d'Apple et libère un verrou qu'elle ne tient pas

Lignes 8843-8863, avec `a_polygon_ptr_unsafe` (8865-8886), `LP_PROC` (8893-8918) et `apple_guard`
(8674-8680). La garde est armée sur **tout** l'appel intérieur. Cet appel lâche `G.mu` puis
appelle `real(ctx, …)` (le rendu d'Apple), ou `apple_guard`, qui lâche le verrou puis fait
`malloc`, `real()` et `free`. Sur une faute, le gestionnaire exécute
`pthread_mutex_unlock(&G.mu)` sans condition.

**Scénario A** (une seule fille d'exécution) : Prey, un repli `RenderPolygonPtr` vers Apple
lit la même page morte que celle qui a fait fauter `a_quads`. La garde saute hors du
rastériseur d'Apple, en plein travail, et déverrouille un `G.mu` déjà libre. POSIX ne définit pas
ce cas. La libpthread de Tiger ne vérifie pas le propriétaire d'un mutex NORMAL : l'état du
rendu logiciel d'Apple est incohérent, et si la faute survient dans `malloc`, son verrou reste
pris et le `malloc` suivant se bloque.

**Scénario B** (deux fils d'exécution et deux contextes) : `sigsetjmp(proc_jmp)` et
`proc_jmp_on = 1` s'exécutent **avant** la prise du verrou. Le fil B écrase `proc_jmp` pendant
que le fil A est dans le corps. Si A faute, il saute dans le cadre de pile de B, sur la pile de
A. Le `proc_jmp_on = 0` de B désarme aussi A.

**Correctif** : armer la garde seulement verrou tenu, et la désarmer avant tout déverrouillage.
Le correctif minimal ajoute `proc_jmp_on = 0;` juste avant **chaque**
`pthread_mutex_unlock(&G.mu)` des trois corps et d'`apple_guard` (sans effet quand la garde n'est
pas armée). Le correctif propre se fait dans l'enveloppe :
```c
pthread_mutex_lock(&G.mu);
if (sigsetjmp(proc_jmp, 0)) { proc_jmp_on = 0; /* p->color = HOST_NEWER (I8) */
                              pthread_mutex_unlock(&G.mu); return 0; }
proc_jmp_on = 1;
r = inner_locked(ctx, verts, n, flags, &real);   /* ne verrouille plus, ne rend pas à Apple */
proc_jmp_on = 0;
pthread_mutex_unlock(&G.mu);
return real ? real(ctx, verts, n, flags) : r;
```
Il faut alors qu'`apple_guard` rende son tampon au lieu d'appeler `real` lui-même. Une faute
dans le rendu d'Apple doit rester **celle d'Apple**.

## P3. Le gestionnaire ignore quel fil d'exécution a fauté

Lignes 9137-9152. Les drapeaux `*_jmp_on` sont globaux au processus, et le gestionnaire ne
compare pas le fil fautif au fil qui a armé la garde.

**Scénario** : pendant qu'un gros lot est empaqueté (`pack_jmp_on = 1`), un autre fil fait une
faute. Ce peut être le son de DOOM 3, un fil du WindowServer, ou une faute voulue (JIT, Java).
Le gestionnaire fait `siglongjmp(pack_jmp)` **sur l'autre fil**, qui reprend dans le cadre de
`geom_draw_client` du premier : deux fils sur la même pile, corruption immédiate.

**Correctif** : noter le fil à l'armement et le comparer dans le gestionnaire.
`pthread_self()` est sans verrou sur Darwin.
```c
static pthread_t guard_thr;              /* posé à chaque armement : guard_thr = pthread_self(); */
...
if (!pthread_equal(pthread_self(), guard_thr)) goto not_ours;   /* en tête de crash_handler */
```

## P4. Crochet de plantage : récursion possible, et appels à dyld

Lignes 9134-9229.
- **Récursion** : avec `SA_NODEFER` (ajouté en `bc7ed62`), une faute **dans** le gestionnaire le
  relance. Le parcours de pile (9190-9200) lit `*(sp+8)` et `*sp` sur une chaîne qui n'est
  vérifiée que par `0x1000 < sp < 0xc0000000`, sans alignement ni ordre croissant. Une pile
  corrompue, souvent la cause même de la faute, fait fauter la lecture. Le gestionnaire se
  relance, les drapeaux sont à 0, il rouvre le fichier (un descripteur de plus à chaque tour),
  réécrit « CRASH signal » et refaute, jusqu'à épuisement de la pile. Le gestionnaire du jeu ne
  s'exécute jamais et le fichier `.crash` se remplit de doublons.
- **`_dyld_image_count` et `_dyld_get_image_name`** (9163-9170) ne sont pas sûres en signal. Si
  un autre fil charge une image, le vecteur de dyld peut être réalloué pendant la lecture, et
  l'on retombe dans la récursion.

**Correctif** :
```c
static volatile int in_crash;
static unsigned long plugin_base;        /* à l'installation : dladdr(crash_handler, &di) */
...
if (in_crash) { sigaction(sig, &crash_prev[sig == SIGBUS ? 0 : 1], 0); return; }
in_crash = 1;
sigaction(sig, &crash_prev[sig == SIGBUS ? 0 : 1], 0);   /* D'ABORD : une faute ici ne boucle plus */
...
/* parcours de pile : (sp & 15) == 0 et nouveau sp > ancien sp, sinon arrêt */
```
Le fichier `.crash` doit aussi s'ouvrir avec `O_NOFOLLOW` (règle P5 du README : le plugin peut
tourner en root).

## P5. `va_src` : un VBO dont la copie cliente est nulle donne un pointeur minuscule

Lignes 7085-7087. `base = GLD_U32(vbo, 0x30)` puis `return base + raw`, sans tester `base`.
`va_probe` (commentaire au-dessus) documente pourtant « VBO dont la copie cliente à +0x30 est
nulle » (Colin McRae).

**Scénario** : VBO lié, copie cliente nulle ou paginée par le memory plugin, décalage non nul
(`glVertexPointer(…, (void*)12)`). `va_src` rend `0x0000000c`, et `va_sources_ok` ne l'écarte pas
puisque le pointeur est non nul. L'empaqueteur lit la page 0 : SIGSEGV. Avec le journal, le lot
est jeté (géométrie manquante). Sans le journal (P1), le jeu plante. Avant `cc1b0df`, le cache
résolu de GLEngine était lu d'abord et servait ce cas. Tous les jeux sans programme qui utilisent
des VBO sont exposés (UT2004, Marble Blast si VBO).

**Correctif** :
```c
if (vbo) {
    base = GLD_U32((void *)vbo, 0x30);
    return base ? (const unsigned char *)(base + raw) : 0;   /* 0 → source nulle → refus */
}
```
Pour un tableau client (sans VBO), le cache vaut `raw` quand il est à jour, d'après
`_gleResetVACachePointers` (tableaux-de-sommets.md §2.3). Le préférer n'apporte donc que le
risque d'une valeur périmée, celui-là même qui a planté DOOM 3. Rendre `raw` directement, et
garder le cache pour la sonde.

---

## I1. COPY_TEX : l'empreinte calculée sous `upload_blank` fait écraser la copie

Lignes 2948-2949 (`if (upload_blank) return sig;`), 3249 et 9892-9897. Pendant `try_copy_tex`,
`upload_texture` range `t->lv0_sig = tex_lv0_sig(t)` avec `upload_blank = 1`, c'est-à-dire une
empreinte **sans échantillon de texels**. Au dessin suivant, `texture_ok` (3008) recalcule
l'empreinte avec les échantillons ; si la page est illisible, elle vaut `sig ^ 0x5a5a5a5a`.
L'empreinte diffère, donc `dirty = 1`, et le niveau invité est retéléversé **par-dessus la copie
hôte**. `host_only` repasse aussi à 0.

**Scénarios** :
- DOOM 3, `_currentRender` illisible : à la première image après chaque (re)création, l'effet
  (chaleur, verre) échantillonne du noir.
- Texture à contenu réel, rendue sale **avant** chaque copie (`glTexImage2D(data)` ou
  `glTexSubImage2D`, puis `glCopyTexSubImage2D`) : **à chaque image**, la copie est remplacée par
  le contenu invité.
- De plus, `upload_blank` noircit **toute** la texture et **tous** ses niveaux, même lisibles.
  Une copie partielle (bords de DOOM 3 : `w = 1`, atlas, mipmaps) laisse du noir là où le
  contenu invité était juste. Avant `bc7ed62`, les niveaux lisibles montaient tels quels.

**Correctif** :
1. Dans `upload_texture`, `upload_blank` ne doit noircir que les niveaux **sans données**
   (`!d`). Les niveaux lisibles passent par la copie gardée, et le chemin de faute les envoie
   déjà noirs. `texture_uploadable` ne doit relâcher le test `LV_ROWPIX` que pour `!LV_DATA`.
2. Dans `try_copy_tex`, après `upload_blank = 0;`, recalculer
   `t->lv0_sig = tex_lv0_sig(t);`. L'empreinte est alors la même qu'au dessin, variante de faute
   comprise.

## I2. COPY_TEX, nouvelle disposition : cible jamais vérifiée

Lignes 9860-9863. Quand `a[2] == 0`, la cible est forcée à `GL_TEXTURE_2D` et `a[6]` (zoff) est
ignoré. Rien n'établit que `a[2]` vaut toujours 0 pour une texture 2D seulement. Ce pourrait être
un indice de face ou de tranche, puisque la disposition est celle de `CopyTexSubImage3D`.

**Scénario** : `glCopyTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X, …)` (face 0, reflets
dynamiques) ou `glCopyTexSubImage3D` à zoff = 0. Le plugin émet `COPY_TEX` avec `QGPU_TT_2D`
sur une texture créée CUBE ou 3D sur l'hôte. L'hôte répond BAD_ARG, puis fait `break` (H4) :
**tout le reste de la soumission est perdu**, et les miroirs d'état sont faux durablement
(P9 : il faut une image entière d'état renvoyé).

**Correctif** :
```c
if (target == 0) {
    if (a[6] != 0 || tex_is_cube(t) || tex_is_3d(t)) COPYTEX_NO("disposition longue non 2D");
    ...
}
```
Ce test se place après `intern_tex`, ou bien on déplace la résolution de `t` plus haut.
Accessoirement, garder `a[2]` d'origine dans le journal `COPYTEX_NO`.

## I3. `raw_bad` est écrasé entre le marquage et le filtre des triangles

`raw_scan_nan` marque `raw_bad[]` (7937). Ensuite, dans la branche du tampon hôte (7968),
`close_raw()` appelle `raw_fix_nan()` (1485), qui rappelle `raw_scan_nan` sur le lot Begin/End
en attente et **réécrit** `raw_bad[0..nv_raw)`. Enfin, `va_copy_idx_tri` (8006) lit ce tableau
modifié.

**Scénario** : un dessin en mode immédiat (lot brut ouvert, rien ne l'a fermé : `send_state` et
`geom_send_all` n'ont rien émis), puis un `glDrawElements(GL_TRIANGLES)` adossé à un VBO avec
des sommets fous. Les drapeaux sont ceux du lot Begin/End : des triangles sains sont retirés
(trous) et des triangles fous restent. Leurs mots insensés valent 0 : ce sont des pointes vers
le plan x=0 ou vers l'origine.

**Correctif** : appeler `close_raw();` **avant** l'empaquetage (juste après
`geom_send_all(p, fmt)`), ou donner au chemin tableaux son propre tableau `arr_bad[]`.

## I4. Réutilisation du tampon hôte après un filtrage de triangles

Lignes 7955-7988. Avec `filter_bad`, les `nverts` sommets (mots fous remis à 0) montent dans le
tampon hôte, et `pack_key`, `pack_vmin` et `pack_nverts` sont enregistrés comme pour un paquet
sain. Au dessin suivant avec la même clé, `reuse = 1` : `raw_scan_nan` ne tourne pas,
`filter_bad = 0`, et **tous** les indices sont copiés.

**Scénario** : un maillage statique en VBO (propre, `host == 1`) avec un sommet fou. À la
première image, les triangles sont retirés. À partir de la deuxième, ils reviennent avec la
composante insensée remplacée par 0 : des pointes. `POMPPC_GL_TRIFILTER=0` ne les voyait pas,
puisque le lot entier partait alors chez Apple.

**Correctif** : si `filter_bad` est posé, ne pas enregistrer de clé réutilisable
(`hb->pack_nverts = 0;` après l'émission du `BUF_SUBDATA`), ou mémoriser un drapeau
`hb->pack_filtered` qui interdit `reuse`.

## I5. (68a2d6c) La clé de réutilisation ignore le générique 0, les génériques et les unités 4 à 7

`va_pack_key` (7611) et `va_host_ready` (7645-7672) ne parcourent que les emplacements
conventionnels `{0,1,2,4,3,8,9,10,11}`. Sous programme, la position vient de l'emplacement 16
(générique 0). L'emplacement 0 est inactif, donc la clé n'y voit qu'une valeur courante
constante. `va_host_ready` prend le VBO de `VA_VBO(V, 0)`, c'est-à-dire le **dernier** VBO lié
au tableau conventionnel, qui peut être périmé. Les génériques `QGPU_VF_GEN(k)` et les
texcoords 4 à 7 (v17) ne comptent ni dans la clé ni dans le test de propreté.

**Scénario** (Colin McRae, ou tout programme dont la position est `vertex.attrib[0]`) : deux
maillages différents ont le même `vmin` et le même `nverts`, et un VBO propre traîne sur
l'emplacement 0. Le second dessin réutilise les sommets **du premier**. Autre cas : des
génériques 8 à 11 lus dans un VBO dynamique distinct de celui de la position ; il est modifié,
mais la position reste propre, donc on réutilise des génériques périmés.

**Correctif** : dans `va_pack_key`, `va_host_ready` et `va_host_clean`, remplacer la liste fixe
par la liste que construit `va_plan_build` : position = 16 si la bascule s'applique, puis chaque
`QGPU_VF_TEX(u)` (u < 8) et chaque `QGPU_VF_GEN(k)` du format. Le plus simple est de construire
le plan **avant** la clé, et de hacher et tester propreté à partir de `plan.a[j].src`, du pas et
du type, en plus des VBO de ces emplacements.

## I6. Tampon hôte recréé sans invalider sa clé

`buf_host_ensure` (4586) détruit puis recrée le tampon hôte, avec un nouvel identifiant et un
contenu indéfini, sans toucher à `pack_*`. Ensuite, plusieurs sorties ne ré-émettent pas de
`BUF_SUBDATA` : la faute d'empaquetage (garde `pack_jmp`, nouvelle), `nverts > RAW_NAN_MAX`, et
un lot ruban ou éventail avec un sommet fou. Le tampon reste neuf, mais `pack_*` décrit l'ancien
contenu.

**Scénario** : cache de sommets d'idTech4, un grand VBO dessiné par plages. Un lot plus grand
que `qsize` force la recréation, puis faute ou est refusé. Le dessin suivant reprend la plage
précédente, avec `reuse = 1` : il dessine le contenu vide du nouveau tampon, donc du vide ou des
sommets à zéro.

**Correctif** : dans `buf_host_ensure`, lors d'une (re)création, ajouter
`b->pack_nverts = 0; b->pack_key = 0;`.

## I7. `text_vp_inputs` : lecture du texte et correspondance d'alias avec l'hôte

Lignes 5145-5188 et masquage 5692-5706.
- **Lecture** : la grammaire ARB autorise des blancs entre les éléments. `vertex.attrib [8]`
  (espace avant `[`) ne correspond pas à `"attrib["` : l'entrée n'est pas marquée, et
  `QGPU_VF_GEN(8)` est **retiré** du format, donc le programme reçoit la valeur courante.
  L'erreur va dans le mauvais sens (on retire). Il faut sauter les blancs entre `attrib` et `[`,
  et après `[`. Par prudence, tout prendre (`need | 0x0FFFFFFF`) dès qu'une forme n'est pas
  reconnue après `vertex.`.
- **Alias à sens unique** : l'hôte (`patches/qgpu/qgpu-gl.c:2203-2295`) ne fait l'alias que pour
  0 = position, par `glVertexPointer`. La normale, la couleur et les texcoords passent par les
  tableaux conventionnels, et les génériques 1 à 15 par `glVertexAttribPointerARB(k)`. Or
  `text_vp_inputs` traduit `attrib[2]` en `NORMAL | GEN(2)`, mais `vertex.normal` en `NORMAL`
  seul, et `attrib[8+u]` en `GEN(8+u)` sans `TEX(u)`. Un programme qui lit `vertex.attrib[3]`
  pendant que l'application fournit `glColorPointer` (alias valide chez NVIDIA et, d'après
  « code 0 inactif → 16 », chez GLEngine) reçoit la couleur en `glColorPointer` sur l'hôte, alors
  que le programme hôte lit le générique 3 : la valeur courante. Le cas inverse
  (`vertex.texcoord[0]` lu, données en `glVertexAttribPointerARB(8)`) masque le générique 8. Ce
  n'est pas une régression du 24/09, sauf ce dernier cas, qui est nouveau (masque `VPN_GEN`).
  **Correctif** : rendre l'alias symétrique dans `text_vp_inputs`, où `normal` pose aussi
  `GEN(2)`, `color` pose `GEN(3)` et `texcoord[u]` pose `GEN(8+u)`. À l'empaquetage, si le
  programme lit l'entrée k mais que seul le tableau aliasé est actif, empaqueter ce tableau dans
  l'emplacement que l'hôte donne au programme.
- **Limite de l'hôte, antérieure (b0bfb52)** : `qgpu-gl.c:2267` `if (!tex[u]) continue;` ne
  fournit aucun texcoord pour une unité sans texture. Côté plugin, `geom_format` ne porte
  `TEX(u)` que si l'unité est texturée. Sous programme, un `vertex.texcoord[u]` utilisé comme
  simple donnée (courant chez idTech4) n'est jamais fourni, et l'hôte garde l'état du dessin
  précédent.
- `POMPPC_GL_VPNEED=0` fonctionne, mais voir R1.

## I8. Faute dans une procédure à pointeurs : `p->color` n'est pas marqué

Une faute au milieu de `a_polygon_ptr` ou `LP_PROC` a pu laisser des primitives déjà émises dans
la série, mais `end_tris` et `p->color = HOST_NEWER` ne sont pas exécutés. Un repli suivant
(`sync_to_sw_locked`) ne relit pas l'hôte. Le logiciel dessine sur un tampon invité périmé, puis
ce tampon remonte et efface les triangles hôte. Correctif : dans la branche de faute, verrou
encore tenu, faire `if ((p = find_ctx(ctx))) p->color = HOST_NEWER;`.

## I9. Niveau illisible envoyé noir, pour une texture ordinaire

Lignes 3204-3221. Le choix est défendable : l'alternative est Apple, qui lit la même page et
meurt. Une faute de lecture sur un niveau d'une texture ordinaire veut dire que `LV_DATA` pointe
vers de la mémoire libérée, puisque des pages simplement non engagées se lisent à zéro sous Mach.
Le noir reste en place tant que `LV_DATA` ne change pas, ce qui est cohérent. Mais le cas n'est
**détectable** que par trois lignes de journal. Il faut ajouter un compteur (`G.n_texblack`)
aux statistiques par image, pour qu'une texture noire se voie dans le bilan au lieu de passer
pour un défaut de rendu.

---

## R1. `getenv` à chaque dessin

`geom_format` appelle `getenv("POMPPC_GL_VPNEED")` jusqu'à deux fois (5693). Il sert à chaque
`geom_publish` et chaque dessin de tableaux. `POMPPC_GL_TRIFILTER` est lu à chaque lot fou
(7953). Sur DOOM 3, avec 1 280 dessins par image, c'est un parcours de `environ` avec `strncmp`
à chaque fois, du même ordre que les 5 % économisés sur `sigprocmask`. Correctif : mettre en
cache dans un `static int` (−1 = pas encore lu), comme `pendclose`.

`dbg_vtx_i` est une écriture `volatile` à chaque sommet (7922). C'est négligeable devant la
conversion, mais on pourrait ne l'écrire que toutes les 64 itérations.

---

## S1. `tex_lv0_sig` : `sigsetjmp` dans un `&&`, et un local modifié après

Ligne 2950. `if (d && w && h && sigsetjmp(sig_jmp, 0) != 0)` n'est pas une forme d'appel
autorisée par C99 7.13.1.1, et `sig` est modifié après `sigsetjmp` (`sig ^= …`) puis lu dans la
branche de retour. Sa valeur y est indéterminée (7.13.2.1). En pratique, c'est stable : le même
point faute à chaque fois. Correctif :
```c
volatile unsigned long vsig = sig;
if (d && w && h) {
    if (sigsetjmp(sig_jmp, 0)) { sig_jmp_on = 0; return vsig ^ 0x5a5a5a5aUL; }
    ...
```

## S2. Chemin direct des flottants : `bpc` ignoré

Ligne 7396. Pour `GL_FLOAT`, `f[k]` suppose 4 octets par composante. `va_fetch` utilisait
`src + k*bpc` avec `bpc = va_bpc(type, ent[0xc])`. Les deux n'ont divergé sur aucun cas relevé
(`ent[0xc]` vaut 4 pour les flottants), mais la condition du chemin direct devrait être
`at->type == VA_GL_FLOAT && at->bpc == 4`.

## S3. Chemin Begin/End sous programme : `w = 0` toujours jeté

`raw_fix_nan` (1063) passe `keep_w0 = 0` même sous programme. Un volume d'ombre dessiné par
Begin/End (ou par le déroulage de GLEngine) sous `shadow.vp` perd encore ses sommets à l'infini.
C'est incohérent avec le chemin tableaux ; passer `G.raw_ctx && G.prog && G.raw_ctx->vp_on`.

---

## Points vérifiés et écartés

- **Kext ↔ plugin (`POMPPC_SUB_LAYOUT`)** : un kext ancien masque `len` avec son **ancien**
  `POMPPC_SUB_FLAGS`, sans le bit `0x10000000`. Il reste donc `len = 0x10000000` (256 Mio) et
  `off = 0`. Le test `len > fSlotSize - off` refuse l'appel (`kIOReturnBadArgument`) tant que la
  fenêtre fait moins de 1 Gio (4 tranches), ce qui est toujours le cas : le verdict du plugin est
  juste. Seules les constantes de tranches sont comparées, et `QGPU_MAX_PROG` ne concerne pas le
  kext.
- **Imbrication des gardes** : `tex_lv0_sig` et `upload_texture` (garde `sig`) sont appelées dans
  les régions `pack` et `proc`. `pack` et `proc` ne s'imbriquent jamais : `geom_draw_client` n'est
  appelé que par `geom_render_array` et `geom_render_vb`. Le gestionnaire teste la garde la plus
  intérieure (`sig`) en premier, c'est correct. Aucun drapeau ne peut rester posé après un retour
  normal.
- **État après un saut `pack_jmp`** : les lectures de mémoire de l'application précèdent toujours
  `reserve()` (`upload_texture`, `prog_upload`, `prog_send_*`, `prim`/`put_vertex`) et
  `G.vtx += packed`. Aucune commande n'est donc laissée à moitié écrite et aucun `DRAW` n'est
  orphelin. Le verrou est rendu par l'appelant de `geom_draw_client`. Seules réserves : I6
  (tampon recréé) et quelques octets d'arène perdus jusqu'au vidage suivant, sans conséquence.
- **`sigsetjmp(…, 0)` + `SA_NODEFER`** : c'est correct sans pile de signal alternative. Sans
  `SA_NODEFER`, SIGSEGV resterait bloqué après le saut, et la faute suivante tuerait le
  processus.
- **Empaqueteur planifié et `va_fetch`** : l'équivalence tient pour la couleur morte (valeur
  courante calculée une fois par dessin, invariante dans le dessin), le tableau désactivé, la
  source nulle, la normalisation (bit `0x8000`, normale et couleur entières), `src_n > dst_n`
  (borné), une position à 2 ou 3 composantes (bourrage 0 puis 1), les doubles (par `va_comp`),
  le générique 0 comme position, et NaN → 0 / ±Inf → ±1e9 (déjà le cas avant). `VaPlan` compte
  `GEN_MAX + MAX_UNITS + 5` entrées pour `GEN_MAX + MAX_UNITS + 4` au plus.
- **`va_copy_idx_tri`** : aucun indice n'est inférieur à `vmin`, car `va_scan_idx` a lu les mêmes
  indices. `nverts ≤ RAW_NAN_MAX` est vérifié avant. `need` est recalculé après la réduction de
  `nidx`, et le test de place utilisait la borne haute.
- **Cœur et `w = 0`** : la validation de `DRAW_RAW` (`qgpu-core.c:343` et `961`) ne refuse que
  NaN et |v| > 1e9 ; `w = 0` passe. Sous programme, `w` est une donnée d'entrée du programme, le
  découpage homogène du GPU hôte s'en charge. Le backend logiciel découpe aussi en homogène
  (`qgpu-soft.c` : `clip_poly`, `div_w`), mais il ne prend pas les programmes : la question ne se
  pose pas.
- **`upload_blank` oublié** : aucun chemin de sortie de `try_copy_tex` ne le laisse posé. Aucune
  garde n'entoure `pomppc_proc_pre`, et `upload_texture` ne sort de sa propre garde `sig` que par
  un retour normal.
- **`static VaPlan plan`, `told`** : ils ne sont utilisés que verrou tenu. Le crochet les lit
  seulement. Avec P3 corrigé, il n'y a pas de course.
