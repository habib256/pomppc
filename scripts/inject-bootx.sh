#!/usr/bin/env bash
# Réinjecte BootX (extrait du CD d'install) dans le volume système de tiger.qcow2.
# Le boot échoue parce que /System/Library/CoreServices/BootX manque sur le disque installé.
# Nécessite root (qemu-nbd + montage HFS+ en écriture). À lancer: sudo bash scripts/inject-bootx.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DISK="$ROOT/disks/tiger.qcow2"
BOOTX="$ROOT/work/BootX"
NBD="/dev/nbd0"
MP="$(mktemp -d)"

[[ $EUID -eq 0 ]]        || { echo "Lance-moi avec sudo."; exit 1; }
[[ -f "$DISK" ]]         || { echo "Disque introuvable: $DISK"; exit 1; }
[[ -f "$BOOTX" ]]        || { echo "BootX introuvable: $BOOTX"; exit 1; }
pgrep -f "qemu-system-ppc" >/dev/null && { echo "QEMU tourne encore — coupe-le d'abord."; exit 1; } || true

cleanup() {
  mountpoint -q "$MP" && umount "$MP" || true
  qemu-nbd -d "$NBD" >/dev/null 2>&1 || true
  rmdir "$MP" 2>/dev/null || true
}
trap cleanup EXIT

echo "1) module nbd + hfsplus"
modprobe nbd max_part=16
modprobe hfsplus

echo "2) exposition du qcow2 sur $NBD"
qemu-nbd -c "$NBD" "$DISK"
sleep 1
partprobe "$NBD" 2>/dev/null || true
sleep 1

echo "3) recherche de la partition système (montage-test)"
HFSDEV=""
# On teste p10 en premier (connu via la table de partitions Apple), puis les autres.
for dev in "${NBD}p10" ${NBD}p*; do
  [[ -b "$dev" ]] || continue
  if mount -t hfsplus -o ro,force "$dev" "$MP" 2>/dev/null; then
    if [[ -d "$MP/System/Library/CoreServices" ]]; then
      HFSDEV="$dev"; umount "$MP"; echo "   -> volume système trouvé sur $dev"; break
    fi
    umount "$MP"
  fi
done
[[ -n "$HFSDEV" ]] || { echo "Volume système introuvable sur $NBD (partitions: $(ls ${NBD}p* 2>/dev/null | tr '\n' ' '))"; exit 1; }

echo "4) désactivation du bit 'journalisé' (le noyau refuse l'écriture sinon)"
python3 - "$HFSDEV" <<'PY'
import sys, struct, subprocess
dev = sys.argv[1]
size = int(subprocess.check_output(["blockdev","--getsize64",dev]).strip())
UNMNT     = 1 << 8    # kHFSVolumeUnmountedBit  (1 = démonté proprement)
INCNSTNT  = 1 << 11   # kHFSBootVolumeInconsistentBit
JOURNALED = 1 << 13   # kHFSVolumeJournaledBit
SOFTLOCK  = 1 << 15   # kHFSVolumeSoftwareLockBit

def clear_journal(f, vh, size):
    """Force les attributs pour que le noyau accepte le montage rw:
       propre (UNMNT=1), cohérent (INCNSTNT=0), non journalisé, non verrouillé."""
    f.seek(vh); sig = f.read(2)
    if sig not in (b"H+", b"HX"):
        print(f"   VH à {vh}: signature {sig!r} inattendue"); return
    for name, off in (("primaire", vh), ("alternatif", size - 1024)):
        f.seek(off); s = f.read(2)
        if s not in (b"H+", b"HX"): continue
        f.seek(off+4); attr = struct.unpack(">I", f.read(4))[0]
        new = (attr | UNMNT) & ~(INCNSTNT | JOURNALED | SOFTLOCK)
        f.seek(off+4); f.write(struct.pack(">I", new))
        print(f"   VH {name}: attributs {attr:#010x} -> {new:#010x}")

with open(dev, "r+b") as f:
    f.seek(1024); sig = f.read(2)
    if sig in (b"H+", b"HX"):
        print("   HFS+ pur (H+ à 1024)")
        clear_journal(f, 1024, size)
    elif sig == b"BD":
        print("   wrapper HFS détecté — recherche du volume HFS+ embarqué")
        f.seek(1024+20); alBlkSiz = struct.unpack(">I", f.read(4))[0]
        f.seek(1024+28); alBlSt   = struct.unpack(">H", f.read(2))[0]
        f.seek(1024+124); embSig  = f.read(2)
        f.seek(1024+126); embStart, embCount = struct.unpack(">HH", f.read(4))
        if embSig != b"H+":
            print(f"   pas de HFS+ embarqué (embSig={embSig!r})"); sys.exit(1)
        emb_off = alBlSt*512 + embStart*alBlkSiz
        emb_size = embCount*alBlkSiz
        print(f"   HFS+ embarqué @ {emb_off} (taille {emb_size})")
        clear_journal(f, emb_off+1024, emb_off+emb_size)
    else:
        print(f"   signature {sig!r} inconnue"); sys.exit(1)
    f.flush()
PY

echo "5) montage en écriture"
mount -t hfsplus -o rw,force "$HFSDEV" "$MP"
MODE=$(awk -v m="$MP" '$2==m{print $4}' /proc/mounts | cut -d, -f1)
echo "   monté en: ${MODE:-?}"
if [[ "$MODE" != "rw" ]]; then
  echo "   !! toujours read-only. Raison côté noyau :"
  dmesg 2>/dev/null | grep -i hfs | tail -5 | sed 's/^/     /'
fi

DEST="$MP/System/Library/CoreServices"
[[ -d "$DEST" ]] || { echo "Dossier CoreServices absent sur le volume — mauvaise partition ?"; exit 1; }
if [[ -f "$DEST/BootX" ]]; then
  echo "   BootX est DÉJÀ présent ($(stat -c%s "$DEST/BootX") octets) — diagnostic à revoir."
else
  echo "   BootX absent, confirmé. Copie en cours…"
fi

echo "6) copie de BootX"
cp "$BOOTX" "$DEST/BootX"
sync
echo "   OK: $(ls -l "$DEST/BootX")"

echo "7) démontage propre"
# (le trap cleanup s'en charge aussi)
umount "$MP"
qemu-nbd -d "$NBD"
trap - EXIT
rmdir "$MP" 2>/dev/null || true

echo
echo "TERMINÉ. BootX injecté. Relance le boot avec boot-device=hd:10,\\System\\Library\\CoreServices\\BootX"
