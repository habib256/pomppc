# Écarté sur chiffres

> Fiche de chantier ; index et priorités : [`TODO.md`](../../TODO.md).

Ne pas rouvrir sans fait nouveau ; la mesure est dans la doc citée. Les « § » nus renvoient au document de la ligne précédente.

| piste | mesure | doc |
|---|---|---|
| Traducteur de second niveau par traces (`x-tier2`, `0048`, `0055`) ; mis de côté le 25/09, relancé le 21/10, arrêté le 22/10 | DOOM 3 +2,7 % (PC), +5,7 % (M4) ; reprise sur décision explicite de rouvrir ce chantier | `plan-traducteur-rapide.md` §9 |
| Régions chaudes recompilées par LLVM | +17,6 % sur DOOM 3 | idem, en-tête (§10 sous `archive/tier3-llvm`) ; arrêté le 22/10 |
| `POMPPC_GL_PRESENTQ`, image n mise en file avant d'attendre la n−1 (04/11) | plein écran, 3 parties : cmr +2,13 %, mb +0,20 %, ut −0,38 % ; aucun gain | `presentq.md` |
| Mémoire directe (« fastmem ») | net ≤ 5 % pour un chantier entier | `vitesse-doom3-x86.md` §21 |
| Vérification TLB compacte | extrapolé de la zone froide : 0-4 % | §21 |
| « Opérande flottant connu partout » | plafond 3,5 %, même sans aucun test | §18.1 |
| TLB logiciel en pages énormes | les deux bras exécutaient le même code | §18.3 |
| `tcg/0053` AltiVec flottant connu | neutre en jeu | `tcg-g4.md` §41, `archive/0053-vfp-known` |
| `tcg/0057` QoS des vCPU (M4) | neutre au repos | §42 |
| `tcg/0059` `vperm` en PSHUFB | exact, neutre | §43 |
| `tcg/0060` x8 tel quel | cmr +4,2 % en course | §46 (x8 + 0063 : `leviers-jit-x86.md`) |
| `tcg/0061` `x-bat-skip` | neutre partout en course, rafale non expliquée | §46 |
| `tcg/0063` `x-dbat-lite` tel quel | cmr neutre, ut +3,7 % | §47 (témoin sous vérificateurs et x8 + 0063 : `leviers-jit-x86.md`) |
| `tcg/0065` `POLLNS=2`, attente sous 1 ms (M4) | zen −8,20 % mais UT +10,06 % | `tcg-g4.md` §42.2, `archive/m4-pollns-court-0211` |
| Appel MMIO synchrone « plugin vers l'hôte » | natif ×0,6-1,5 sur le travail réel | `plan-plugin-hote.md` (code sous `archive/plugin-hote-phase0`) |
| 3-4 vCPU (Tiger démarre sur 3 et 4) | 0 gain sur les jeux ; reprise sur demande explicite d'un mode 3-4 vCPU | `patches/smp-mac99/essais/qemu-mac99-4cpus.patch` |
