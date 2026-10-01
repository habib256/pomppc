#!/usr/bin/env bash
# GPL3 - Copyleft VERHILLE Arnaud
# run_tiger.sh — lance Mac OS X 10.4 (Tiger) dans une fenêtre.
#
#   ./run_tiger.sh            # ImGuiDock + SMP 2 + son + toutes les optimisations + GPU qgpu
#   POMPPC_FRONTEND=native ./run_tiger.sh  # fenêtre QEMU native (diagnostic)
#   SMP=1 ./run_tiger.sh      # mono-cœur + SON (chemin stable d'origine)
#   NOSOUND=1 ./run_tiger.sh  # coupe l'audio (rend la RAM pleine : 1024 au lieu de 768)
#   WIDE=1 ./run_tiger.sh     # 16:9 plein écran 1920x1080 (sinon RES=WxHxD au choix)
#   SNAPSHOT=1 ./run_tiger.sh # disque jetable (writes annulés -> boot toujours propre, pas de fsck)
#   NET=0 ./run_tiger.sh      # coupe le réseau (actif par défaut si QEMU a slirp :
#                             # NAT, DHCP 10.0.2.15, passerelle 10.0.2.2, DNS 10.0.2.3)
#   SSH_FWD=0                 # coupe la redirection SSH localhost:2222 → invité:22
#                             # (10.0.2.15 n'est pas joignable depuis l'hôte)
#   WEBPROXY=0                # ne lance pas le relais web HTTPS→HTTP (10.0.2.2:8080)
#   HEADLESS=1 ./run_tiger.sh # sans fenêtre (moniteur seul, pour scripting)
#   POMPPC_DISPLAY=sdl        # affichage QEMU explicite, sans ImGuiDock
#   QFB=1 ./run_tiger.sh      # + écran paravirtuel qfb-pci (kext POMPPCQFB)
#   QFB_RES=1280x800          # mode par défaut proposé par l'écran QFB (avec QFB=1)
#   GPU=0 ./run_tiger.sh      # sans GPU paravirtuel qgpu-pci (allumé par défaut)
#   GPU_BACKEND=soft|gl|auto  # backend de rendu hôte du qgpu (défaut : auto ;
#                             # le backend RÉELLEMENT pris est sondé et annoncé)
#   GPU_SCANOUT=auto|qfb|vga|none  # cible de présentation de SURF_PRESENT
#                             # (vide par défaut : rien n'est passé au device)
#   FASTFP=0 ./run_tiger.sh   # flottant exact ; le rapide (FPU hôte) est le défaut
#   SRTLB=0 ./run_tiger.sh    # coupe le TLB gardé d'un jeu de segments à l'autre (x-sr-tlb, défaut allumé,
#                             # patches/tcg/, docs/tcg-g4.md) ; CPU_OPTS=… propriétés brutes
#   LFSINLINE=0 VFPFAST=0 VPERMFAST=0 ./run_tiger.sh  # coupe lfs/stfs sans helper, flottant AltiVec
#                             # à 4 voies, vperm par table (tcg/0002-0004, allumés par défaut)
#                             # docs/flottant-rapide.md
#   FPINLINE=0 ./run_tiger.sh # coupe le flottant scalaire simple (fmuls, fmadds, fcmpu…) sans ses
#                             # deux helpers dans le cas courant (x-fp-inline, tcg/0007, allumé
#                             # par défaut, docs/tcg-g4.md §15) ; FPVERIFY=1 : mode preuve
#   FPFLAT=1 ./run_tiger.sh   # les mêmes en UN appel, sans branchement dans le code généré
#                             # (x-fp-flat, tcg/0013, éteint par défaut, docs/tcg-g4.md §22)
#   FPNATIVE=0 ./run_tiger.sh # coupe leur chemin court en instructions flottantes de l'hôte
#                             # (arm64), le helper de x-fp-flat hors ligne sinon (x-fp-native,
#                             # tcg/0014, allumé par défaut depuis le 01/10, docs/tcg-g4.md §22)
#   FPNATIVE64=0 ./run_tiger.sh # coupe le même pour le flottant DOUBLE (fadd fmul fmadd… : x-fp-native64,
#                             # tcg/0016, allumé par défaut, docs/tcg-g4.md §24) ; FPVERIFY=1 : preuve
#   TBFAST=0 ./run_tiger.sh   # coupe mftb/mftbu calculés depuis le compteur de l'hôte (cntvct, 1 GHz)
#                             # et l'horloge de QEMU linéaire à la ns (x-tb-fast, tcg/0015, allumé
#                             # par défaut, docs/tcg-g4.md §23) ; TBVERIFY=1 : mode preuve
#   RETINLINE=0 JCIDX=0 ./run_tiger.sh  # coupe les sorties indirectes (blr, bctr…) cherchées en
#                             # ligne et le cache de sauts vidé par mmu_idx (tcg/0008, allumés par
#                             # défaut, docs/tcg-g4.md §16) ; RETVERIFY=1 : mode preuve
#   ICBISYNC=0 ./run_tiger.sh # coupe icbi qui invalide les blocs de sa ligne (x-icbi-sync, tcg/0010,
#                             # allumé par défaut depuis le 30/09 : docs/tcg-g4.md §17, §20)
#   MSRNOBQL=0 ./run_tiger.sh # coupe mtmsr/rfi sans verrou global quand la ligne d'interruption ne
#                             # change pas (x-msr-nobql, tcg/0012, allumé par défaut, docs/tcg-g4.md §19) ;
#                             # MSRVERIFY=1 : mode preuve
#   JCBITS=12 ./run_tiger.sh  # cache de sauts d'origine (2^12 entrées par vCPU) ; défaut 14 (x-jc-bits,
#                             # tcg/0011, propriété de l'accélérateur, 12 à 16 ; docs/tcg-g4.md §18)
#   JITNEAR=0 ./run_tiger.sh  # laisse macOS placer le tampon du JIT (défaut : dans la fenêtre de 4 Gio
#                             # du texte de QEMU, x-jit-near, tcg/0006 : supprime le régime lent, docs/tcg-g4.md §14) ;
#                             # TCG_OPTS=… propriétés brutes de l'accélérateur
#   NOPAD=1 ./run_tiger.sh    # coupe le passthrough de la manette USB
#   TABLET=1 ./run_tiger.sh   # + usb-tablet (défaut avec ImGuiDock).
#                             # TABLET=0 garde seulement la souris relative.
#                             # ImGuiDock sélectionne explicitement la tablette
#                             # pour éviter le repli vers la souris relative.
#   NOCD=1 ./run_tiger.sh     # omet le lecteur CD amovible vide 'gamecd'
#   GLISO=0 ./run_tiger.sh    # omet l'ISO des sources du plugin GL (disks/pomppc-src.iso)
#                             # dans 'gamecd' ; implicite avec le GPU (GLISO=1 pour forcer)
#   EXTRA_ARGS="-device ..."  # arguments QEMU supplémentaires
#
# Piloté par le frontend ImGui : DBUS_DISPLAY=1 (sortie -display dbus,p2p=on) et
# QMP_SOCK=<chemin> (socket QMP). POMPPC_SCRATCH déplace .run/ (socket moniteur).
#
# Build UNIFIÉ : QEMU 9.2 (device Screamer, patches/screamer/) + OpenBIOS fusionné
# (bring-up SMP balaton + nœud audio screamer). SMP *et* son ensemble, produits
# tous les deux par scripts/build_qemu_qfb.sh.
# Le son est SONDÉ, pas supposé : si le binaire n'a pas la classe 'screamer', le
# lanceur le dit, coupe l'audio et NE rabote PAS la RAM. (Un -global sur une
# classe absente n'est qu'un warning côté QEMU : rien ne signalait la panne.)
# SMP >= 2 : qemu-system-ppc64 (target MTTCG-safe) + réveil CPU secondaire via
# GPIO KeyLargo. Vérifie le nb de CPU dans « À propos de ce Mac ». Son via
# PulseAudio sous Linux, CoreAudio sous macOS (activer la Mémoire Virtuelle
# côté invité aide). Fenêtre fermée -> quitte.
set -euo pipefail
USER_SMP="${SMP:-}"                        # intention user AVANT que config.env n'impose SMP=1
USER_RES="${RES:-}"                        # RES explicite de l'utilisateur, prioritaire
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Profil maximal des optimisations disponibles et éprouvées. Les sondes plus
# bas restent obligatoires ; 0 explicite conserve sa valeur pour les A/B.
# Les modes VERIFY ne sont pas des optimisations et restent éteints.
# Depuis le 30/09/2026 (docs/tcg-g4.md §18-§21) : ICBISYNC (x-icbi-sync, tcg/0010)
# et MSRNOBQL (x-msr-nobql, tcg/0012) en font partie ; depuis le 01/10/2026 (§22),
# FPNATIVE (x-fp-native, tcg/0014) ; depuis le soir du 01/10 (§23-§24, à la demande de
# l'utilisateur, sans tour de matrice), TBFAST (x-tb-fast, tcg/0015) et FPNATIVE64
# (x-fp-native64, tcg/0016).
for _opt in FASTFP SRTLB LFSINLINE VFPFAST VPERMFAST FPINLINE RETINLINE JCIDX ICBISYNC MSRNOBQL JITNEAR FPNATIVE TBFAST FPNATIVE64; do
  export "$_opt=${!_opt:-1}"
done
# x-jc-bits (tcg/0011) : 2^14 entrées de cache de sauts (12 = QEMU d'origine).
export JCBITS="${JCBITS:-14}"
export QGPU_GPU_COPY="${QGPU_GPU_COPY:-1}" QGPU_GLSL="${QGPU_GLSL:-1}"
# glFlush du device en fin de soumission (docs/backend-gl-attente.md §5) : allumé
# par défaut depuis le 01/10/2026 avec la soumission anticipée des requêtes du
# plugin (POMPPC_GL_QFLUSH) ; QGPU_GL_FLUSH=0 l'éteint.
export QGPU_GL_FLUSH="${QGPU_GL_FLUSH:-1}"

# Avant le verrou disque : ImGui relance CE script avec DBUS_DISPLAY=1.
# Cette garde évite la récursion et laisse les outils sans fenêtre inchangés.
case "${POMPPC_FRONTEND:-imgui}" in
  imgui|native) ;;
  *) echo "POMPPC_FRONTEND attendu : imgui ou native" >&2; exit 2 ;;
esac
if [ "${POMPPC_FRONTEND:-imgui}" = imgui ] && [ -z "${DBUS_DISPLAY:-}" ] &&
   [ -z "${HEADLESS:-}" ] && [ -z "${POMPPC_DISPLAY:-}" ]; then
  echo "▶ Tiger : ImGuiDock, profil optimisations maximales (réglages surchargeables)"
  exec "$ROOT/run_frontend.sh" "$ROOT/run_tiger.sh"
fi
source "$ROOT/config.env"
source "$ROOT/scripts/caps.sh"
source "$ROOT/scripts/hostcompat.sh"

# WIDE=1 -> 16:9 plein écran (1920x1080). Le ndrv stock offre nativement ce mode
# (contrairement à 1440x900/1600x900 qui retombent en 800x600). RES= reste prioritaire.
[ -n "${WIDE:-}" ] && [ -z "$USER_RES" ] && RES="1920x1080x32"
[ -n "$USER_RES" ] && RES="$USER_RES"

SMP_N="${USER_SMP:-2}"                     # défaut = 2 cœurs (la nouveauté)
# OpenBIOS UNIFIÉ : bring-up SMP (balaton) + nœud audio screamer (mcayland),
# buildé en -O1 (gcc-13 miscompile ce code OpenBIOS à -Os). Marche mono ET SMP.
UNI_OBIOS="$ROOT/patches/smp-mac99/openbios-smp-screamer.elf"
QEMU_BIN64="${QEMU_BIN}64"                # qemu-system-ppc -> qemu-system-ppc64

# --- Verrou disque ---
# Verrou sur un fd plutôt que fuser : fuser vient de psmisc, et s'il manque le
# test échouait en silence — plus aucun garde-fou, deux QEMU sur le même qcow2,
# corruption. Le fd 9 reste ouvert à travers l'exec final : le verrou vit donc
# aussi longtemps que QEMU. En SNAPSHOT=1 les écritures sont jetées, plusieurs
# instances sont légitimes : pas de verrou. (host_lock_fd : flock sous Linux,
# perl sous macOS, qui n'a pas flock(1).)
SCR="${POMPPC_SCRATCH:-$ROOT/.run}"; mkdir -p "$SCR"
if [ -z "${SNAPSHOT:-}" ]; then
  exec 9>"$SCR/tiger.lock"
  if ! host_lock_fd 9; then
    echo "⚠  Tiger tourne déjà (verrou $SCR/tiger.lock). Ferme-le d'abord," >&2
    echo "   ou lance en disque jetable : SNAPSHOT=1 ./run_tiger.sh" >&2
    exit 1
  fi
fi

# --- Sélection binaire / accélérateur / firmware ---
# Build UNIFIÉ (QEMU 9.2 + device Screamer porté) : SMP *et* son ensemble.
EXTRA=(); AUDIO=(); RAM="$RAM_MB"
[ -f "$UNI_OBIOS" ] || { echo "⚠  OpenBIOS unifié introuvable ($UNI_OBIOS)." >&2; exit 1; }
EXTRA+=(-bios "$UNI_OBIOS")

# --- Choix du binaire AVANT tout sondage ---
# L'ordre compte : dès SMP >= 2 la VM tourne sur qemu-system-ppc64, et sonder
# les capacités sur $QEMU_BIN (32 bits) revenait à interroger un binaire que
# personne ne lance. Le Screamer en particulier : une réponse « oui » venue du
# 32 bits rabotait la RAM de 1024 à 768 Mo sur un 64 bits peut-être muet.
# Tout ce qui suit sonde donc $BIN, le binaire réellement exécuté.
#
# SMP est sondé, plus seulement « le fichier existe » : un qemu-system-ppc64 de
# distribution est bien exécutable et n'a pas le bring-up SMP mac99
# (patches/smp-mac99/) — QEMU mourait alors à l'exec final sur « Invalid SMP
# CPUs 2 ». ⚠ -smp >= 2 exige aussi via=pmu (sans PMU, pas de device 'gpio',
# donc pas de ligne de reset du CPU1) : la garde est explicite.
SMP_TAG=""
if [ "$SMP_N" -ge 2 ]; then
  [ -x "$QEMU_BIN64" ] || { echo "⚠  SMP demandé mais $QEMU_BIN64 introuvable." >&2; exit 1; }
  case "$MACHINE" in
    *via=pmu*) ;;
    *) echo "⚠  -smp $SMP_N exige '$MACHINE,via=pmu' (le reset du CPU1 passe par" >&2
       echo "   le GPIO 4 de KeyLargo, qui n'existe qu'avec le PMU) — mono-cœur." >&2
       SMP_N=1; SMP_TAG=" (via=pmu absent)" ;;
  esac
fi
if [ "$SMP_N" -ge 2 ]; then
  smp_rc=0; qemu_machine_smp_ok "$QEMU_BIN64" "$MACHINE" "$SMP_N" || smp_rc=$?
  case "$smp_rc" in
    0) ;;
    1) echo "⚠  SMP demandé mais ce QEMU refuse -smp $SMP_N sur $MACHINE" >&2
       echo "   (bring-up SMP mac99 absent). Reconstruis le binaire de référence :" >&2
       echo "   ./scripts/build_qemu_qfb.sh   (lancement mono-cœur.)" >&2
       SMP_N=1; SMP_TAG=" (SMP indisponible)" ;;
    *) echo "⚠  sondage SMP impossible sur $QEMU_BIN64 : on tente quand même." >&2 ;;
  esac
fi
if [ "$SMP_N" -ge 2 ]; then
  BIN="$QEMU_BIN64"
  TCG_ACCEL="tcg,thread=multi"
  MODE="SMP ${SMP_N} cœurs (MTTCG, ppc64)"
else
  BIN="$QEMU_BIN"
  MODE="mono-cœur${SMP_TAG}"
fi

# Son ON par défaut ; NOSOUND=1 pour couper. On SONDE le binaire : sans la
# classe 'screamer' QEMU se contente d'un warning sur le -global, et on se
# retrouvait avec 768 Mo de RAM et zéro son sans le savoir.
SND_ON=0
if [ -z "${NOSOUND:-}" ]; then
  if qemu_machine_has "$BIN" "$MACHINE" screamer; then
    source "$ROOT/scripts/tiger_audio.sh"
    AUDIO_DEVICE=$(tiger_audio_device) || exit $?
    AUDIO=(-audiodev "$AUDIO_DEVICE" -global screamer.audiodev=snd0)
    echo "  Audio : $AUDIO_DEVICE (POMPPC_AUDIO_PROFILE=default pour revenir au défaut QEMU)"
    host_audio_env
    [ "$RAM" -gt 768 ] && RAM=768        # le Screamer exige < 1 Go
    SND_ON=1
  else
    echo "⚠  son indisponible : ce QEMU n'a pas le device 'screamer'." >&2
    echo "   Reconstruis le binaire de référence : ./scripts/build_qemu_qfb.sh" >&2
    echo "   (RAM laissée à ${RAM} Mo — pas de plafond 768 sans Screamer.)" >&2
  fi
fi

[ "$SND_ON" = 1 ] && MODE="$MODE + SON"   # reflète le sondage, pas l'intention

# --- Flottant rapide (propriété de CPU x-fast-fp, patches/fastfp/) ---
# Laisse softfloat confier les opérations flottantes courantes au FPU de l'hôte.
# Résultats identiques au bit près ; seuls FPSCR[FI] (constant à 1) et la règle
# de transition de FX dévient. Voir docs/flottant-rapide.md.
# Allumé par défaut (jeux : Vorbis, physique). FASTFP=0 pour le flottant exact.
#
# SONDÉ, jamais supposé — et pour une raison plus dure que d'habitude : une
# propriété absente dans -cpu ne produit pas un warning, elle fait QUITTER QEMU.
# Poser ,x-fast-fp=on à l'aveugle sur un binaire non patché ne dégraderait pas
# le lanceur, il l'empêcherait de démarrer.
CPU_SPEC="$CPU"
if [ "${FASTFP:-1}" != 0 ]; then
  ffp_rc=0; qemu_cpu_has_fastfp "$BIN" "$MACHINE" "$CPU" || ffp_rc=$?
  case "$ffp_rc" in
    0) CPU_SPEC="$CPU,x-fast-fp=on"
       MODE="$MODE + FLOTTANT RAPIDE" ;;
    # La bannière DIT l'écart entre l'intention et le binaire : sans ça, la
    # seule trace d'un flottant exact était un warning noyé dans le défilement,
    # et une mesure A/B « sans gain » restait inexplicable.
    1) echo "⚠  flottant rapide demandé mais ce QEMU n'a pas la propriété 'x-fast-fp'." >&2
       echo "   Reconstruis le binaire de référence : ./scripts/build_qemu_qfb.sh" >&2
       echo "   (lancement en flottant exact.)" >&2
       MODE="$MODE + flottant rapide DEMANDÉ MAIS INDISPONIBLE" ;;
    *) echo "⚠  sondage x-fast-fp impossible : lancement en flottant exact." >&2
       MODE="$MODE + flottant rapide DEMANDÉ, SONDAGE IMPOSSIBLE" ;;
  esac
fi

# --- TLB gardé d'un jeu de segments à l'autre (x-sr-tlb, patches/tcg/) ---
# Un changement de registre de segment ne vide plus tout le TLB de QEMU : chaque
# mmu_idx traduit est vidé seulement s'il sert sous un autre jeu de segments.
# Voir docs/tcg-g4.md. Allumé par défaut depuis le 25/09/2026 (DOOM 3 SMP=2 : médiane
# 89,0 → 80,1 ms/image) ; SRTLB=0 pour l'éteindre. Sondé comme x-fast-fp : un QEMU
# sans la propriété tourne sans (avertissement seulement si SRTLB=1 est explicite).
# CPU_OPTS=… ajoute des propriétés brutes au modèle (p. ex. x-sr-tlb-verify=64).
if [ "${SRTLB:-1}" != 0 ]; then
  if qemu_cpu_has_prop "$BIN" "$MACHINE" "$CPU" "x-sr-tlb=on"; then
    CPU_SPEC="$CPU_SPEC,x-sr-tlb=on"
    MODE="$MODE + TLB PAR SEGMENTS"
  elif [ -n "${SRTLB:-}" ]; then
    echo "⚠  SRTLB=1 demandé mais ce QEMU n'a pas la propriété 'x-sr-tlb' (patches/tcg/)." >&2
    MODE="$MODE + TLB par segments DEMANDÉ MAIS INDISPONIBLE"
  fi
fi
# --- lfs/stfs sans helper (x-lfs-inline, patches/tcg/0002) ---
# Les conversions simple ↔ double de lfs/stfs en ops TCG entières au lieu d'un
# appel de helper ; mêmes bits pour toutes les entrées (docs/tcg-g4.md §8).
# Allumé par défaut depuis le 25/09/2026 (DOOM 3, docs/tcg-g4.md §13) : LFSINLINE=0 l'éteint.
if [ "${LFSINLINE:-1}" != 0 ]; then
  if qemu_cpu_has_prop "$BIN" "$MACHINE" "$CPU" "x-lfs-inline=on"; then
    CPU_SPEC="$CPU_SPEC,x-lfs-inline=on"
    MODE="$MODE + LFS EN LIGNE"
  elif [ -n "${LFSINLINE:-}" ]; then
    echo "⚠  LFSINLINE=1 demandé mais ce QEMU n'a pas la propriété 'x-lfs-inline' (patches/tcg/0002)." >&2
    MODE="$MODE + lfs en ligne DEMANDÉ MAIS INDISPONIBLE"
  fi
fi
# --- AltiVec : flottant à 4 voies (x-vfp-fast, tcg/0003), vperm par table
# (x-vperm-fast, tcg/0004) --- mêmes résultats au bit près (docs/tcg-g4.md §9-10).
# Allumés par défaut depuis le 25/09/2026 (DOOM 3, docs/tcg-g4.md §13) : VFPFAST=0, VPERMFAST=0.
for _p in "VFPFAST x-vfp-fast VFP-4-VOIES" "VPERMFAST x-vperm-fast VPERM-TABLE"; do
  set -- $_p
  if [ "${!1:-1}" != 0 ]; then
    if qemu_cpu_has_prop "$BIN" "$MACHINE" "$CPU" "$2=on"; then
      CPU_SPEC="$CPU_SPEC,$2=on"
      MODE="$MODE + $3"
    elif [ -n "${!1:-}" ]; then
      echo "⚠  $1=1 demandé mais ce QEMU n'a pas la propriété '$2' (patches/tcg/)." >&2
      MODE="$MODE + $3 DEMANDÉ MAIS INDISPONIBLE"
    fi
  fi
done
# --- Flottant scalaire sans helper (x-fp-inline, patches/tcg/0007) ---
# fadds fsubs fmuls fmadds fmsubs fnmadds fnmsubs fcmpu : un chemin court (un
# appel pur + FPRF/FI/FPCC en ligne) quand le FPSCR est amorcé sans trappe, en
# arrondi au plus proche, et que les opérandes sont des simples normaux ; sinon
# les helpers d'origine. Mêmes résultats, même FPSCR au bit près (docs/tcg-g4.md
# §15). N'agit qu'avec x-fast-fp. Allumé par défaut depuis le 26/09/2026 (DOOM 3
# 74,4 → 65,1 ms/image, docs/tcg-g4.md §15.9) : FPINLINE=0 l'éteint ; FPVERIFY=1
# ajoute le mode preuve (x-fp-verify : chaque passage par le chemin court refait
# par les helpers, bilan sur stderr).
if [ "${FPINLINE:-1}" != 0 ]; then
  if qemu_cpu_has_prop "$BIN" "$MACHINE" "$CPU" "x-fp-inline=on"; then
    case "$CPU_SPEC" in
      *x-fast-fp=on*) CPU_SPEC="$CPU_SPEC,x-fp-inline=on"; MODE="$MODE + FLOTTANT SCALAIRE EN LIGNE"
                      if [ "${FPVERIFY:-0}" != 0 ]; then
                        CPU_SPEC="$CPU_SPEC,x-fp-verify=on"; MODE="$MODE (VÉRIFIÉ)"
                      fi ;;
      *) if [ -n "${FPINLINE:-}" ]; then
           echo "⚠  FPINLINE=1 sans flottant rapide : x-fp-inline n'agit qu'avec x-fast-fp (FASTFP=1)." >&2
           MODE="$MODE + flottant scalaire en ligne SANS EFFET (flottant exact)"
         fi ;;
    esac
  elif [ -n "${FPINLINE:-}" ]; then
    echo "⚠  FPINLINE=1 demandé mais ce QEMU n'a pas la propriété 'x-fp-inline' (patches/tcg/0007)." >&2
    MODE="$MODE + flottant scalaire en ligne DEMANDÉ MAIS INDISPONIBLE"
  fi
fi
# --- Flottant scalaire en un appel, sans branchement (x-fp-flat, patches/tcg/0013,
# docs/tcg-g4.md §22) --- les mêmes instructions que x-fp-inline, traduites en UN
# appel et aucun branchement : le choix « chemin court de x-fp-inline ou séquence
# d'origine » est fait en C, le code généré n'est plus coupé en blocs de base à
# chaque instruction. Mêmes résultats, même FPSCR au bit près. N'agit qu'avec
# x-fast-fp ; prime sur x-fp-inline. Éteint par défaut (A/B DOOM 3 à jouer) :
# FPFLAT=1 l'allume ; FPVERIFY=1 vérifie aussi son chemin court.
if [ "${FPFLAT:-0}" != 0 ]; then
  if qemu_cpu_has_prop "$BIN" "$MACHINE" "$CPU" "x-fp-flat=on"; then
    case "$CPU_SPEC" in
      *x-fast-fp=on*) CPU_SPEC="$CPU_SPEC,x-fp-flat=on"; MODE="$MODE + FLOTTANT SCALAIRE SANS BRANCHEMENT"
                      case "$CPU_SPEC" in
                        *x-fp-verify=on*) ;;
                        *) if [ "${FPVERIFY:-0}" != 0 ]; then
                             CPU_SPEC="$CPU_SPEC,x-fp-verify=on"; MODE="$MODE (VÉRIFIÉ)"
                           fi ;;
                      esac ;;
      *) echo "⚠  FPFLAT=1 sans flottant rapide : x-fp-flat n'agit qu'avec x-fast-fp (FASTFP=1)." >&2
         MODE="$MODE + flottant scalaire sans branchement SANS EFFET (flottant exact)" ;;
    esac
  else
    echo "⚠  FPFLAT=1 demandé mais ce QEMU n'a pas la propriété 'x-fp-flat' (patches/tcg/0013)." >&2
    MODE="$MODE + flottant scalaire sans branchement DEMANDÉ MAIS INDISPONIBLE"
  fi
fi
# --- Flottant scalaire par le FPU de l'hôte dans le code généré (x-fp-native,
# patches/tcg/0014, docs/tcg-g4.md §22) --- le chemin court de x-fp-inline en
# instructions flottantes arm64 (op TCG ppc_fp32), le helper de x-fp-flat appelé
# hors ligne quand il ne s'applique pas. Mêmes résultats, même FPSCR au bit près.
# N'agit qu'avec x-fast-fp ; x-fp-flat sur un hôte qui n'est pas arm64. Allumé par
# défaut depuis le 01/10/2026 (A/B DOOM 3 60,5 -> 58,3 ms/image, matrice verte,
# §22) : FPNATIVE=0 l'éteint ; FPVERIFY=1 le vérifie.
if [ "${FPNATIVE:-0}" != 0 ]; then
  if qemu_cpu_has_prop "$BIN" "$MACHINE" "$CPU" "x-fp-native=on"; then
    case "$CPU_SPEC" in
      *x-fast-fp=on*) CPU_SPEC="$CPU_SPEC,x-fp-native=on"; MODE="$MODE + FLOTTANT SCALAIRE NATIF"
                      case "$CPU_SPEC" in
                        *x-fp-verify=on*) ;;
                        *) if [ "${FPVERIFY:-0}" != 0 ]; then
                             CPU_SPEC="$CPU_SPEC,x-fp-verify=on"; MODE="$MODE (VÉRIFIÉ)"
                           fi ;;
                      esac ;;
      *) echo "⚠  FPNATIVE=1 sans flottant rapide : x-fp-native n'agit qu'avec x-fast-fp (FASTFP=1)." >&2
         MODE="$MODE + flottant scalaire natif SANS EFFET (flottant exact)" ;;
    esac
  else
    echo "⚠  FPNATIVE=1 demandé mais ce QEMU n'a pas la propriété 'x-fp-native' (patches/tcg/0014)." >&2
    MODE="$MODE + flottant scalaire natif DEMANDÉ MAIS INDISPONIBLE"
  fi
fi
# --- Flottant DOUBLE par le FPU de l'hôte (x-fp-native64, patches/tcg/0016,
# docs/tcg-g4.md §24) --- fadd fsub fmul fmadd fmsub fnmadd fnmsub par la même op
# TCG que x-fp-native (formes double). N'agit qu'avec x-fast-fp. Allumé par défaut
# depuis le 01/10 (A/B Prey −1,7 %) : FPNATIVE64=0 l'éteint ; FPVERIFY=1 le vérifie.
if [ "${FPNATIVE64:-0}" != 0 ]; then
  if qemu_cpu_has_prop "$BIN" "$MACHINE" "$CPU" "x-fp-native64=on"; then
    case "$CPU_SPEC" in
      *x-fast-fp=on*) CPU_SPEC="$CPU_SPEC,x-fp-native64=on"; MODE="$MODE + FLOTTANT DOUBLE NATIF"
                      case "$CPU_SPEC" in
                        *x-fp-verify=on*) ;;
                        *) if [ "${FPVERIFY:-0}" != 0 ]; then
                             CPU_SPEC="$CPU_SPEC,x-fp-verify=on"; MODE="$MODE (VÉRIFIÉ)"
                           fi ;;
                      esac ;;
      *) echo "⚠  FPNATIVE64=1 sans flottant rapide : x-fp-native64 n'agit qu'avec x-fast-fp (FASTFP=1)." >&2
         MODE="$MODE + flottant double natif SANS EFFET (flottant exact)" ;;
    esac
  else
    echo "⚠  FPNATIVE64=1 demandé mais ce QEMU n'a pas la propriété 'x-fp-native64' (patches/tcg/0016)." >&2
    MODE="$MODE + flottant double natif DEMANDÉ MAIS INDISPONIBLE"
  fi
fi
# --- Base de temps par le compteur de l'hôte (x-tb-fast, patches/tcg/0015,
# docs/tcg-g4.md §23) --- mftb/mftbu sans clock_gettime ni division 128 bits ;
# l'horloge de QEMU devient cntvct_el0 + K (linéaire, à la ns). Hôtes arm64 dont
# le compteur bat à 1 GHz (sinon sans effet, QEMU le dit). Allumé par défaut depuis
# le 01/10 (A/B DOOM 3 −0,6 %, Nexuiz −0,8 %) : TBFAST=0 l'éteint ;
# TBVERIFY=1 compare chaque lecture à la valeur d'origine (bilan sur stderr).
if [ "${TBFAST:-0}" != 0 ]; then
  if qemu_cpu_has_prop "$BIN" "$MACHINE" "$CPU" "x-tb-fast=on"; then
    CPU_SPEC="$CPU_SPEC,x-tb-fast=on"; MODE="$MODE + BASE-DE-TEMPS-RAPIDE"
    if [ "${TBVERIFY:-0}" != 0 ]; then
      CPU_SPEC="$CPU_SPEC,x-tb-verify=on"; MODE="$MODE (VÉRIFIÉE)"
    fi
  else
    echo "⚠  TBFAST=1 demandé mais ce QEMU n'a pas la propriété 'x-tb-fast' (patches/tcg/0015)." >&2
    MODE="$MODE + base de temps rapide DEMANDÉE MAIS INDISPONIBLE"
  fi
fi
# --- Sorties indirectes des blocs (blr, bctr…) : recherche du bloc suivant en
# ligne (x-ret-inline) et cache de sauts vidé par mmu_idx (x-jc-idx),
# patches/tcg/0008, docs/tcg-g4.md §16 --- mêmes blocs choisis que
# helper_lookup_tb_ptr. Allumés par défaut depuis le 26/09/2026 (DOOM 3 65,7 →
# 61,2 ms/image, docs/tcg-g4.md §16) : RETINLINE=0, JCIDX=0 les éteignent ;
# RETVERIFY=1 ajoute le mode preuve (x-ret-verify : chaque bloc pris dans le
# cache de sauts comparé à une recherche physique complète, bilan sur stderr).
for _p in "RETINLINE x-ret-inline SORTIES-EN-LIGNE 1" "JCIDX x-jc-idx CACHE-DE-SAUTS-PAR-MMU 1" \
          "RETVERIFY x-ret-verify SORTIES-VÉRIFIÉES 0"; do
  set -- $_p
  _v="${!1:-$4}"
  if [ "$_v" != 0 ]; then
    if qemu_cpu_has_prop "$BIN" "$MACHINE" "$CPU" "$2=on"; then
      CPU_SPEC="$CPU_SPEC,$2=on"
      MODE="$MODE + $3"
    elif [ -n "${!1:-}" ]; then
      echo "⚠  $1=1 demandé mais ce QEMU n'a pas la propriété '$2' (patches/tcg/0008)." >&2
      MODE="$MODE + $3 DEMANDÉ MAIS INDISPONIBLE"
    fi
  fi
done
# --- icbi invalide les blocs de sa ligne de cache (x-icbi-sync, patches/tcg/0010,
# docs/tcg-g4.md §17) --- en SMP, une écriture dans du code invalide les blocs
# AVANT d'être faite : l'autre vCPU peut retraduire l'ancien code entre les deux,
# et le protocole de l'invité (dcbst, sync, icbi, isync) ne le rattrapait pas.
# Allumé par défaut depuis le 30/09/2026 (DOOM 3 sans coût mesurable, docs/tcg-g4.md
# §20) ; ICBISYNC=0 l'éteint.
if [ "${ICBISYNC:-1}" != 0 ]; then
  if qemu_cpu_has_prop "$BIN" "$MACHINE" "$CPU" "x-icbi-sync=on"; then
    CPU_SPEC="$CPU_SPEC,x-icbi-sync=on"
    MODE="$MODE + ICBI-SYNCHRONE"
  elif [ -n "${ICBISYNC:-}" ]; then
    echo "⚠  ICBISYNC=1 demandé mais ce QEMU n'a pas la propriété 'x-icbi-sync' (patches/tcg/0010)." >&2
    MODE="$MODE + ICBI-SYNCHRONE DEMANDÉ MAIS INDISPONIBLE"
  fi
fi
# --- mtmsr/rfi sans verrou global quand la ligne d'interruption ne change pas
# (x-msr-nobql, patches/tcg/0012, docs/tcg-g4.md §19) --- compteur de séquence
# contre les mises à jour faites sous le verrou ; MSRVERIFY=1 ajoute le mode
# preuve (chaque décision sans verrou refaite sous le verrou, bilan sur stderr).
# Allumé par défaut depuis le 30/09/2026 (DOOM 3 −0,6 %, docs/tcg-g4.md §19) ;
# MSRNOBQL=0 l'éteint.
for _p in "MSRNOBQL x-msr-nobql MSR-SANS-VERROU" "MSRVERIFY x-msr-nobql-verify MSR-VÉRIFIÉ"; do
  set -- $_p
  if [ "${!1:-0}" != 0 ]; then
    if qemu_cpu_has_prop "$BIN" "$MACHINE" "$CPU" "$2=on"; then
      CPU_SPEC="$CPU_SPEC,$2=on"
      MODE="$MODE + $3"
    else
      echo "⚠  $1=1 demandé mais ce QEMU n'a pas la propriété '$2' (patches/tcg/0012)." >&2
      MODE="$MODE + $3 DEMANDÉ MAIS INDISPONIBLE"
    fi
  fi
done
[ -n "${CPU_OPTS:-}" ] && CPU_SPEC="$CPU_SPEC,${CPU_OPTS#,}"
# --- Tampon du JIT dans la fenêtre de 4 Gio du texte de QEMU (x-jit-near,
# patches/tcg/0006) --- propriété de l'ACCÉLÉRATEUR. Sur Apple M4, le noyau pose
# le tampon hors de cette fenêtre un lancement sur deux environ, et tout le
# processus tourne alors ~10 % moins vite (« les deux régimes », docs/tcg-g4.md
# §14). Allumé par défaut depuis le 25/09/2026 (DOOM 3 confirmé, §14.7) : JITNEAR=0
# l'éteint. Sondé.
if [ "${JITNEAR:-1}" != 0 ]; then
  if qemu_tcg_has_prop "$BIN" "$MACHINE" "x-jit-near=on"; then
    TCG_ACCEL="${TCG_ACCEL:-tcg},x-jit-near=on"
    MODE="$MODE + JIT PRÈS DU TEXTE"
  elif [ -n "${JITNEAR:-}" ]; then
    echo "⚠  JITNEAR=1 demandé mais ce QEMU n'a pas la propriété 'x-jit-near' (patches/tcg/0006)." >&2
    MODE="$MODE + JIT près du texte DEMANDÉ MAIS INDISPONIBLE"
  fi
fi
# --- Taille du cache de sauts (x-jc-bits, patches/tcg/0011, docs/tcg-g4.md §18)
# --- propriété de l'ACCÉLÉRATEUR (les blocs sont partagés entre vCPU et la
# sonde en ligne de x-ret-inline fige le hachage dans le code généré). 12 =
# QEMU d'origine (rien n'est passé), 13..16 l'agrandit ; 14 par défaut depuis le
# 30/09/2026 (DOOM 3 −1,9 %, docs/tcg-g4.md §18). Sondé.
if [ -n "${JCBITS:-}" ] && [ "${JCBITS}" != 12 ]; then
  if qemu_tcg_has_prop "$BIN" "$MACHINE" "x-jc-bits=$JCBITS"; then
    TCG_ACCEL="${TCG_ACCEL:-tcg},x-jc-bits=$JCBITS"
    MODE="$MODE + CACHE DE SAUTS 2^$JCBITS"
  else
    echo "⚠  JCBITS=$JCBITS demandé mais ce QEMU n'a pas la propriété 'x-jc-bits' (patches/tcg/0011) ou refuse la valeur." >&2
    MODE="$MODE + cache de sauts 2^$JCBITS DEMANDÉ MAIS INDISPONIBLE"
  fi
fi
[ -n "${TCG_OPTS:-}" ] && TCG_ACCEL="${TCG_ACCEL:-tcg},${TCG_OPTS#,}"
[ -n "${TCG_ACCEL:-}" ] && EXTRA+=(-accel "$TCG_ACCEL")

# --- Affichage ---
# DBUS_DISPLAY=1 : sortie via -display dbus,p2p=on pour le frontend ImGui
# (POMPPC/frontend). Le frontend fournit QMP_SOCK et se branche en add_client.
if [ -n "${DBUS_DISPLAY:-}" ]; then DISP="dbus,p2p=on";
elif [ -n "${HEADLESS:-}" ]; then DISP="none";
else DISP="${POMPPC_DISPLAY:-$HOST_DISPLAY}"; host_audio_env; fi

# QMP pour pilotage par le frontend (add_client @dbus-display, reset, etc.).
QMP_ARGS=()
[ -n "${QMP_SOCK:-}" ] && QMP_ARGS=(-qmp "unix:$QMP_SOCK,server=on,wait=off")

# --- Réseau ---
# Actif par défaut dès que le binaire a slirp (NAT utilisateur, aucun droit
# requis) ; NET=0 le coupe. Une demande explicite NET=1 sur un binaire sans
# slirp est signalée ; le défaut, lui, se replie en silence.
NET_WANT="${NET:-auto}"
NET=""
NET_ARGS=(-nic none)
if [ "$NET_WANT" != 0 ]; then
  if qemu_has_netdev "$BIN" user; then
    # 10.0.2.15 n'est pas joignable depuis l'hôte (NAT slirp). On ouvre
    # localhost:2222 → sshd de l'invité (Session à distance). SSH_FWD=0 pour couper.
    # Un port déjà pris (une autre VM, une VM de dev restée debout) faisait
    # ÉCHOUER tout QEMU (« Could not set up host forwarding rule ») : on cherche
    # donc le premier port libre à partir de celui demandé, et si aucun ne l'est
    # on démarre SANS redirection ssh plutôt que de refuser de booter.
    SSH_PORT="${SSH_FWD:-2222}"
    if [ "$SSH_PORT" != 0 ]; then
      ssh_port=""
      for p in $(seq "$SSH_PORT" $((SSH_PORT + 9))); do
        if ! (exec 3<>"/dev/tcp/127.0.0.1/$p") 2>/dev/null; then
          ssh_port="$p"; break
        fi
        # Pas de « exec 3>&- 2>/dev/null » ici : le sous-shell ferme déjà son
        # fd 3, et un exec avec redirection s'applique à TOUT le script — dès
        # que 2222 était pris (une autre VM), la sortie d'erreur du lanceur et
        # de QEMU partait dans /dev/null : QEMU mourait sans un mot.
      done
      if [ -n "$ssh_port" ]; then
        NETDEV="$NETDEV,hostfwd=tcp:127.0.0.1:${ssh_port}-:22"
        SSH_PORT="$ssh_port"
      else
        echo "⚠  ports ${SSH_FWD:-2222}..$((${SSH_FWD:-2222}+9)) tous occupés : démarrage SANS redirection ssh" >&2
        echo "   (une autre VM tourne ? \`pkill -f qemu-system-ppc\` ou SSH_FWD=<port> ./run_tiger.sh)" >&2
        SSH_PORT=0
      fi
    fi
    NET_ARGS=(-netdev "$NETDEV" -device "$NETNIC")
    NET=1
  elif [ "$NET_WANT" = 1 ]; then
    echo "⚠  NET=1 demandé mais ce QEMU n'a pas slirp — réseau coupé." >&2
  fi
fi

# --- Relais web pour Safari/curl de Tiger (scripts/web-proxy.py) ---
#     Tiger ne parle plus le TLS des sites actuels : le relais, sur l'hôte, les
#     sert en HTTP. Il n'écoute que sur 127.0.0.1 (l'invité le voit en
#     10.0.2.2) et s'arrête avec QEMU : $$ devient le PID de QEMU à l'exec final.
WEBPROXY_PORT="${WEBPROXY_PORT:-8080}"
WEBPROXY_ON=""
if [ -n "$NET" ] && [ "${WEBPROXY:-1}" != 0 ] && command -v python3 >/dev/null 2>&1; then
  mkdir -p "${POMPPC_SCRATCH:-$ROOT/.run}"
  # 9>&- : le relais ne doit pas hériter du verrou tiger.lock (un flock suit
  # la description de fichier) — sinon il le tient encore après la mort de QEMU
  # et la relance suivante croit Tiger toujours lancé.
  nohup python3 "$ROOT/scripts/web-proxy.py" --port "$WEBPROXY_PORT" --parent-pid $$ 9>&- \
    >> "${POMPPC_SCRATCH:-$ROOT/.run}/web-proxy.log" 2>&1 &
  WEBPROXY_ON=1
fi

# --- Disque jetable optionnel ---
SNAP_ARGS=(); [ -n "${SNAPSHOT:-}" ] && SNAP_ARGS=(-snapshot)

# --- Lecteur CD amovible 'gamecd' (vide) : insertion à chaud via le frontend
#     (QMP blockdev-change-medium) ou ./mount. NOCD=1 pour l'omettre. ---
CD_ARGS=(); [ -z "${NOCD:-}" ] && CD_ARGS=(-drive "id=gamecd,if=ide,media=cdrom")

# --- Écran paravirtuel QFB (device qfb-pci + kext POMPPCQFB) ---
#     QFB=1 ajoute un second écran piloté par notre kext ; l'écran VGA reste la
#     console Open Firmware. QFB_RES=LxH choisit le mode par défaut proposé.
QFB_ARGS=()
if [ -n "${QFB:-}" ]; then
  if qemu_has_device "$BIN" qfb-pci; then
    QFB_RES="${QFB_RES:-1280x800}"
    QFB_ARGS=(-device "qfb-pci,id=qfb0,width=${QFB_RES%x*},height=${QFB_RES#*x},depth=8")
    echo "  🖵  écran QFB ${QFB_RES} (second moniteur)"
  else
    echo "⚠  QFB=1 demandé mais ce QEMU n'a pas le device qfb-pci." >&2
    echo "   Reconstruis-le : ./scripts/build_qemu_qfb.sh" >&2
    exit 1
  fi
fi

# --- GPU paravirtuel qgpu (device qgpu-pci + kext POMPPCGPU) ---
#     Allumé par défaut : l'invité y soumet des flux de commandes, l'hôte les
#     rend en OpenGL (docs/gpu-3d-tiger.md). GPU=0 pour l'omettre.
#     GPU_BACKEND choisit le backend hôte (défaut auto) ; GPU_SCANOUT choisit la
#     cible de présentation ; GPU_TRACE=1 journalise chaque commande sur stderr.
#
#     Défaut 'auto' et PAS 'gl' : le cas 'gl' du cœur n'a aucun repli, et
#     realize() fait error_setg si EGL manque — le lanceur imposait donc un
#     backend que personne n'avait sondé, et QEMU refusait de démarrer sur un
#     hôte sans EGL (binaire sans les en-têtes, session sans DRI, conteneur).
#     'auto' essaie 'gl' puis retombe sur 'soft'. Comme les deux se ressemblent
#     sur la ligne de commande et pas du tout à l'écran, on SONDE quel backend
#     le device prend réellement et c'est CELUI-LÀ qui est annoncé.
GPU_ARGS=()
GPU_ON=""
if [ "${GPU:-1}" != 0 ]; then
  if qemu_has_device "$BIN" qgpu-pci; then
    GPU_BE_WANT="${GPU_BACKEND:-auto}"
    GPU_BE=""                       # backend réellement pris (sondé), si connu
    gpu_rc=0
    GPU_BE="$(qemu_qgpu_backend_taken "$BIN" "$MACHINE" "$GPU_BE_WANT")" || gpu_rc=$?
    if [ "$gpu_rc" = 1 ] && [ "$GPU_BE_WANT" != auto ]; then
      echo "⚠  backend qgpu '$GPU_BE_WANT' refusé par ce QEMU (pas d'EGL/GL sur cet" >&2
      echo "   hôte ?) — repli sur 'auto'." >&2
      GPU_BE_WANT="auto"; gpu_rc=0
      GPU_BE="$(qemu_qgpu_backend_taken "$BIN" "$MACHINE" auto)" || gpu_rc=$?
    fi
    # Pas de $([ … ] && echo …) ici : sous set -e, l'affectation prend le code
    # de la substitution (1 sans GPU_TRACE) et le lanceur s'arrêtait en silence.
    GPU_OPTS="backend=$GPU_BE_WANT"
    # GPU_SCANOUT (scanout=auto|qfb|vga|none) : quelle cible SURF_PRESENT écrit.
    # Vide par défaut = on ne passe rien, le device décide. Demandée
    # explicitement, la propriété est sondée : l'ignorer en silence sur un
    # binaire qui ne l'a pas ferait présenter dans le mauvais écran, et c'est
    # précisément le bug qu'elle existe pour trancher.
    if [ -n "${GPU_SCANOUT:-}" ]; then
      scan_rc=0
      qemu_qgpu_device_ok "$BIN" "$MACHINE" "scanout=$GPU_SCANOUT" || scan_rc=$?
      case "$scan_rc" in
        0) GPU_OPTS="$GPU_OPTS,scanout=$GPU_SCANOUT" ;;
        1) echo "⚠  GPU_SCANOUT=$GPU_SCANOUT refusé : ce qgpu-pci n'a pas la propriété" >&2
           echo "   'scanout' (ou pas cette valeur là)." >&2
           echo "   Reconstruis-le : ./scripts/build_qemu_qfb.sh" >&2
           exit 1 ;;
        # Sondage impossible n'est pas « absente » : on passe l'option demandée.
        *) echo "⚠  GPU_SCANOUT=$GPU_SCANOUT : sondage impossible, passé tel quel." >&2
           GPU_OPTS="$GPU_OPTS,scanout=$GPU_SCANOUT" ;;
      esac
    fi
    [ -n "${GPU_TRACE:-}" ] && GPU_OPTS="$GPU_OPTS,trace=on"
    GPU_ARGS=(-device "qgpu-pci,id=gpu0,$GPU_OPTS")
    GPU_ON=1
    if [ -n "$GPU_BE" ]; then
      echo "  🎨 GPU paravirtuel qgpu (backend $GPU_BE, demandé : $GPU_BE_WANT)"
    else
      echo "  🎨 GPU paravirtuel qgpu (backend demandé : $GPU_BE_WANT ; backend pris" \
           "non sondable sur ce binaire)"
    fi
  else
    echo "⚠  GPU allumé par défaut mais ce QEMU n'a pas le device qgpu-pci." >&2
    echo "   Reconstruis-le : ./scripts/build_qemu_qfb.sh" >&2
    echo "   (lancement sans accélération 3D.)" >&2
  fi
fi

# --- CD des sources du plugin GL (kext POMPPCGPU + GLDriver-POMPPC) ---
#     Inséré dans le lecteur 'gamecd' (donc éjectable/remplaçable à chaud comme
#     avant). Régénéré si absent ou plus vieux qu'une source : le CD monté
#     correspond toujours au dépôt. Dans l'invité, il apparaît comme POMPPCSRC.
GLISO_WANT="${GLISO:-}"
[ -z "$GLISO_WANT" ] && [ -n "$GPU_ON" ] && GLISO_WANT=1
if [ "$GLISO_WANT" = 1 ]; then
  GL_ISO="$ROOT/disks/pomppc-src.iso"
  GL_SRCS=("$ROOT/kext/POMPPCGPU" "$ROOT/kext/POMPPCQFB" "$ROOT/guest/gldriver"
           "$ROOT/guest/gltest" "$ROOT/guest/qgpu-test" "$ROOT/guest/net")
  if [ ! -f "$GL_ISO" ] || [ -n "$(find "${GL_SRCS[@]}" -type f -newer "$GL_ISO" 2>/dev/null | head -1)" ]; then
    echo "  💿 (re)génération de $(basename "$GL_ISO")…"
    "$ROOT/scripts/make_kext_iso.sh" "$GL_ISO" >/dev/null || {
      echo "⚠  impossible de graver l'ISO du plugin GL (scripts/make_kext_iso.sh)." >&2; exit 1; }
  fi
  CD_ARGS=(-drive "id=gamecd,if=ide,media=cdrom,format=raw,readonly=on,file=$GL_ISO")
  echo "  💿 CD POMPPCSRC inséré. Dans Tiger (une seule fois, Terminal) :"
  echo "       cp -R /Volumes/POMPPCSRC /tmp/src && sudo sh /tmp/src/guest/gldriver/install.sh"
  echo "       sudo sh /Volumes/POMPPCSRC/guest/net/proxy.sh on     # web via le relais"
fi

# --- Arguments QEMU ad hoc : EXTRA_ARGS="-device ..." ./run_tiger.sh ---
read -r -a USER_EXTRA <<< "${EXTRA_ARGS:-}"

BOOTDEV='hd:10,\System\Library\CoreServices\BootX'
MON=$(host_mon_path "$SCR/mon.sock")   # mon.sock pour la VM verrouillée, mon-PID.sock en SNAPSHOT=1
# .run/tiger.mon et .run/tiger.sshport : ce que lisent la matrice, killgame.py,
# cycle.sh et tools/guest/tssh.sh (rien en SNAPSHOT=1 : ils visent la VM quotidienne)
host_publish_vm tiger "$MON" "$([ -n "${NET:-}" ] && echo "${SSH_PORT:-0}" || echo 0)"

echo "▶ Tiger : $MODE | cpu=$CPU_SPEC ram=${RAM}Mo affichage=$DISP \
réseau=$([ -n "${NET:-}" ] && echo on || echo off) \
disque=$([ -n "${SNAPSHOT:-}" ] && echo jetable || echo persistant)"
echo "  moniteur QEMU : $MON"
[ -n "$WEBPROXY_ON" ] && echo "  🌐 relais web : 10.0.2.2:$WEBPROXY_PORT (proxy HTTP de Tiger ; journal .run/web-proxy.log)"
[ -n "$NET" ] && [ "${SSH_PORT:-0}" != 0 ] && echo "  🔑 SSH : ssh -p ${SSH_PORT} tiger@127.0.0.1  (invité 10.0.2.15:22)"
[ "$SMP_N" -ge 2 ] && echo "  (1er boot en persistant = fsck possible ~1min ; ensuite rapide)"

# --- Manette USB : auto-passthrough (idem run_os9.sh) ; NOPAD=1 pour couper ---
PAD_ARGS=()
if [ -z "${NOPAD:-}" ] && [ -e /dev/input/js0 ] \
   && qemu_has_device "$BIN" usb-host; then
  PAD_VID=$(udevadm info -q property -n /dev/input/js0 2>/dev/null | sed -n 's/^ID_VENDOR_ID=//p')
  PAD_PID=$(udevadm info -q property -n /dev/input/js0 2>/dev/null | sed -n 's/^ID_MODEL_ID=//p')
  PAD_USB="/dev/bus/usb/$(lsusb 2>/dev/null | awk -v v="$PAD_VID" -v p="$PAD_PID" 'tolower($6)==v":"p{printf "%s/%s", $2, substr($4,1,3)}')"
  if [ -n "$PAD_VID" ] && [ -n "$PAD_PID" ] && [ -w "$PAD_USB" ]; then
    PAD_ARGS=(-device "usb-host,vendorid=0x$PAD_VID,productid=0x$PAD_PID")
    echo "  🎮 manette $PAD_VID:$PAD_PID → passthrough"
  fi
fi

# Le pointeur relatif via=pmu subit l'accélération de Tiger : il ne peut pas
# rester aligné sur celui de l'hôte. ImGuiDock sélectionne la tablette absolue.
# Le frontend natif conserve son ancien défaut ; TABLET=0/1 reste explicite.
TABLET_ARGS=()
TABLET_DEFAULT=0
[ -n "${DBUS_DISPLAY:-}" ] && TABLET_DEFAULT=1
if [ "${TABLET:-$TABLET_DEFAULT}" = 1 ]; then
  TABLET_ARGS=(-device usb-tablet,id=pointer0)
  echo "  Souris absolue : tablette USB (sélectionnée par ImGuiDock)"
fi

exec "$BIN" -M "$MACHINE" -cpu "$CPU_SPEC" -m "$RAM" -smp "$SMP_N" \
  -display "$DISP" -g "$RES" \
  -drive "file=$DISK,format=qcow2,media=disk" ${SNAP_ARGS[@]+"${SNAP_ARGS[@]}"} ${CD_ARGS[@]+"${CD_ARGS[@]}"} \
  ${NET_ARGS[@]+"${NET_ARGS[@]}"} ${EXTRA[@]+"${EXTRA[@]}"} ${AUDIO[@]+"${AUDIO[@]}"} \
  ${TABLET_ARGS[@]+"${TABLET_ARGS[@]}"} ${PAD_ARGS[@]+"${PAD_ARGS[@]}"} ${QFB_ARGS[@]+"${QFB_ARGS[@]}"} ${GPU_ARGS[@]+"${GPU_ARGS[@]}"} ${USER_EXTRA[@]+"${USER_EXTRA[@]}"} \
  -prom-env 'auto-boot?=true' \
  -prom-env "boot-device=$BOOTDEV" \
  -prom-env 'boot-args=-v' \
  -name "Tiger" \
  ${QMP_ARGS[@]+"${QMP_ARGS[@]}"} \
  -monitor "unix:$MON,server,nowait"
