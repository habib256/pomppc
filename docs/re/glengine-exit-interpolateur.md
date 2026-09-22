# GLEngine : `exit(1)` dans `_gleBuildInterpolateFunc` (relevé du 22/09/2026)

**Symptôme** : UT2004 « retourne au bureau » sans message, sans signal, sans rapport
CrashReporter ; nos statistiques `atexit` s'impriment, le journal UT se ferme proprement
(`Error: Leak detected using FMemStack`, puis `Log file closed`). Reproduit trois fois
(20:34, 21:14, 21:20), chemin brut actif ou coupé, avec 8 comme avec 4 unités annoncées.

**Preuve** : mouchard `tools/guest/exitwatch/exitwatch.c` (interposition dyld de `exit`,
`_exit`, `abort` ; remontée de la chaîne des cadres PPC + table des images). Pile de
`exit(1)`, symbolisée avec `nm -n` (GLEngine = base `0x0a70e000`, UT = base `0x1000`) :

```
GLEngine  _gleBuildInterpolateFunc + 0x6a0      ← bl _exit (li r3,1)
GLEngine  _gleSetClipInterpFunc + 0x2c
GLEngine  _gleClipPoly + 0xd4
GLEngine  _gleRenderSmoothTrianglesFunc + 0x398
GLEngine  _gleFlushAtomicFunc + 0x20
GLEngine  _gleDrawArraysOrElements_Core + 0xe4
GLEngine  _gleDrawArraysOrElements_IMM_Exec + 0x3bc
UT2004    FOpenGLRenderInterface::DrawPrimitive + 0x488
UT2004    AxProcMesh::Render + 0x90c   (maillage procédural : effets, projecteurs)
```

**Mécanisme** (`otool -tV -p _gleBuildInterpolateFunc`) : GLEngine **génère du code** à la
volée (`_gleRTCAddAChunk`, `_gleRTCBuildABranch`) pour interpoler les attributs d'un
polygone découpé contre les plans de clipping. Les drapeaux `r27` : bit 0/4 selon le mode,
bit 1 si `GL_SMOOTH` (`gctx+0x34f4 == 0x1d01`), bit 2 si `cfg+0x94` n'a pas le bit 2, puis
un bit par unité de texture (`r28 = 0..7`, bits 16..23). En fin de construction :

```
00051d30  subf  r2,r29,r30 ; addi r2,r2,-0x20 ; srawi r30,r2,2   ; taille en mots
00051d3c  cmpwi cr7,r30,0xa0
00051d40  ble+  cr7,0x51d4c
00051d44  li    r3,0x1
00051d48  bl    _exit                                            ; > 160 mots : suicide
```

Aucun message : c'est un « ne devrait jamais arriver » d'Apple, dimensionné pour SES pilotes.
Ce découpage logiciel n'est pas un repli du plugin (0 « synced software calls » dans les
parties concernées) : c'est la T&L de GLEngine lui-même sur `glDrawElements`
(`_gleDrawArraysOrElements_IMM_Exec`), qui découpe avant d'appeler `RenderTriangles`.

**Pourquoi chez nous** : le plugin pose `cfg+0x7a = 1` (« comme le GeForce3 », pour que
les coordonnées r/q données entre `glBegin`/`glEnd` ne fassent pas basculer la primitive
vers `_gleForceToSoftwareTCL`, `docs/re/opengl-1.4.md` §3) — et à 1, chaque unité porte
s, t, r, q dans l'interpolateur, soit deux fois plus de chunks par unité. Un maillage
d'UT2004 avec plusieurs unités, couleur, couleur secondaire et brouillard dépasse alors
les 160 mots. Le GeForce3 réel n'y tombait pas parce que sa T&L matérielle découpe
elle-même ; nous, nous laissons GLEngine découper ce que nous ne prenons pas en brut.

**Expérience en cours** : `POMPPC_GL_RDIRTY=0` (`cfg+0x7a = 0`) avec le chemin brut coupé.
Si la sortie disparaît, le compromis est à revoir : soit `cfg+0x7a = 0` et gérer le
repli `_gleForceToSoftwareTCL` (F1 sait maintenant refermer un `pend` orphelin), soit
prendre en brut *aussi* les `glDrawElements` (canal tableaux, `POMPPC_GL_ARRAY=1`) pour
que GLEngine ne découpe plus rien.
