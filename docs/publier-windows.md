# Publier le paquet Windows x64

`POMPPC-<version>-windows-x64.zip`, construit par `.github/workflows/release.yml` (job `windows`), sur
`windows-latest`, dans MSYS2 UCRT64. **Paquet expérimental** : tout le code de ce lot a été écrit sans machine
Windows, sous une campagne de mesure qui interdisait de compiler. Le premier passage de la CI est la première
compilation. Recette de la CI commune et de la Release : [publier-linux.md](publier-linux.md).

## 1. Forme du paquet

Un dossier à décompresser où l'on veut, sans installation :

| Chemin | Rôle |
|---|---|
| `POMPPC.exe` | le frontal (sous-système GUI, manifeste `activeCodePage` UTF-8) |
| `qemu-system-ppc.exe`, `qemu-system-ppc64.exe`, `qemu-img.exe` | QEMU 11.1.2 et ses patchs, profil `paquet-windows` |
| `*.dll` | ce que demandent les `.exe`, pris dans `/ucrt64/bin` seulement |
| `share/qemu/` | firmware (`-L` le désigne) |
| `patches/smp-mac99/openbios-smp-screamer.elf` | l'OpenBIOS unifié, et son source |
| `pomppc-guest.iso` | le CD invité (`packaging/guest-iso.lock`) |
| `THIRD-PARTY-LICENSES/`, `LICENSE`, `README.txt` | licences, mode d'emploi court |

Données de l'utilisateur : `%USERPROFILE%\POMPPC` (`POMPPC_DATA` pour un autre dossier), à la forme de `~/POMPPC` :
`disks/`, `images/`, `.run/`. Le journal de QEMU est `.run/qemu.log`, celui du frontal `.run/app.log`.

**Jamais dans le paquet** : `opengl32.dll` ni un pilote (`packaging/windows/check_windows_package.sh`, liste blanche
de ce que Windows fournit, avec ses témoins dans `tests/run-all.sh`).

Plancher : **Windows 10 1903** (`activeCodePage`). AF_UNIX demande 1809, `curl.exe` et `tar.exe` 1803.

## 2. Construire

```bash
# Shell MSYS2 UCRT64 ; paquets : la liste du job `windows` de release.yml
packaging/linux/fetch_guest_iso.sh build-windows-iso/pomppc-guest.iso
GUEST_ISO=build-windows-iso/pomppc-guest.iso packaging/windows/build_windows.sh
packaging/windows/check_windows_package.sh build-windows/POMPPC-<version>-windows-x64
packaging/windows/smoke_windows.sh build-windows/POMPPC-<version>-windows-x64 <version> frontend/build-windows
```

QEMU est cloné par `scripts/build_qemu_qfb.sh` (`QEMU_SRC`, `/d/q` : un chemin court), profil `paquet-windows`.
`core.autocrlf` est coupé avant le checkout : un `.patch` ou un `.sh` en CRLF n'irait pas plus loin que
`patch_strict`. `MSYS=winsymlinks:native`, comme dans la CI de QEMU.

## 3. Ce qui remplace bash et POSIX

| Sous Linux, macOS | Sous Windows | Fichier |
|---|---|---|
| `run_tiger.sh`, `run_os9.sh` | `LaunchPlan` écrit la même ligne ; les `.sh` ne sont que des noms dans `main.cpp` | `frontend/src/LaunchPlan.cpp` |
| `socketpair`, `SCM_RIGHTS`, `getfd` | paire AF_UNIX, `WSADuplicateSocketW`, QMP `get-win32-socket` | `QemuBridge.cpp` |
| `Listener.Unix.Map`, `mmap` | `Listener.Win32.Map`, `MapViewOfFile` | `QemuBridge.cpp`, `dbus/dbus-display1-win32.xml` |
| `flock` sur `.run/tiger.lock` | **`CreateFile` partage 0, tenu par le frontal** | `WinHost.cpp` |
| `posix_spawn`, `SIGTERM`, `waitpid` | `CreateProcessW`, Job `KILL_ON_JOB_CLOSE`, QMP `quit` puis `TerminateProcess` | `WinHost.cpp`, `QemuBridge.cpp` |
| `md5sum`, `sha1sum`, `unzip`, `du`, `cp` | `certutil`, `tar`, `fs::file_size`, `fs::copy_file` | `SetupFlow.cpp` |
| `zenity`, `xdg-open` | `GetOpenFileNameW`, `ShellExecuteW` | `WinHost.cpp` |
| EGL, CGL | WGL sur une fenêtre cachée, profil de compatibilité 3.2 | `patches/qgpu/qgpu-gl.c` |

**Le verrou est obligatoire.** [D] QEMU `block/file-win32.c:331-335,365` @ v11.1.2 : QEMU y refuse `locking=on` et
ouvre l'image en partage. Sans le verrou du frontal, deux instances corrompent le qcow2. Le handle fait foi, pas le
fichier, qui n'est jamais supprimé. `smoke_windows.sh` le met à l'épreuve : un second lancement est refusé.

**Chemins non ASCII.** Le frontal est en UTF-8. QEMU lit `argv` en ANSI et ouvre par `CreateFileA` : un
`C:\Users\Émile\POMPPC` lui est passé par sa forme courte (`GetShortPathNameW`). Si les formes courtes sont coupées
sur ce volume, le frontal le dit et ne lance rien. [F] Non essayé.

## 4. Parité : ce que Windows n'a pas (docs/etat-des-hotes.md §1)

Allumé : `x-fast-fp`, `x-sr-tlb`, `x-lfs-inline`, `x-vfp-fast`, `x-vperm-fast`, `x-vmx-inline`, `x-lmw-vector`, `x-fp-inline`,
`x-ret-inline`, `x-jc-idx`, `x-icbi-sync`, `x-msr-nobql`, `x-tlb-precise`, `x-lmw-inline`, `x-dcbz-inline`, `x-dirty-skip`, `x-pteg-map`,
`x-jc-bits=14`, `x-jc-word`, `x-jc-2way`, MTTCG sur la cible 32 bits (tcg/0050).

| Levier | Pourquoi pas ici | Ce qui le trancherait |
|---|---|---|
| `x-fp-native`, `-native64`, `-native-cmp`, `x-vfp-native`, `-native-cmp` (tcg/0014-0017, 0022, 0033-0034) | Op TCG absente sous `_WIN64` : le talon de sortie passe six arguments en registre (tcg/0017). [D] Vérifié dans la série : `tcg_target_call_iarg_regs[4..5]` n'est compilé que sous `TCG_TARGET_PPC_FP32_IMPL`, jamais défini sous `_WIN64`. | Écrire le talon pour l'ABI Win64 : quatre registres, le reste en pile |
| `x-fp-known`, `x-fprf-lazy`, `x-stfs-known`, `x-lfs-known` (tcg/0045-0047, 0051) | S'appuient sur l'op précédente | idem |
| `x-tlb-base` (tcg/0052) | Rien de propre à SysV à la relecture (L0, L1 et le générique). [F] Aucune partie vérifiée sous Win64 : éteint plutôt que coupé à la compilation | `x-tlb-base-verify` en jeu sous Windows |
| `x-tb-fast` (tcg/0015, 0018) | Le chemin x86-64 est `__linux__` : source d'horloge du noyau | Un chemin `QueryPerformanceCounter`, et un TSC invariant prouvé |
| `x-jit-near`, `x-jit-rel32` (tcg/0006, 0024) | Sans effet : ils placent le tampon par `mmap`, Windows a sa propre `alloc_code_gen_buffer` | Un `VirtualAlloc` proche du texte |
| `x-tcg-cold` (tcg/0056) | Prouvé sur Linux x86-64 seul | Un A/B sous Windows |
| `qe=on` | Quartz Extreme n'est recommandé qu'après une preuve par hôte | Une preuve sous Windows |

Autres écarts : **pas de relais web** (`scripts/web-proxy.py` demande Python, et Safari de Tiger n'atteint donc pas le web
chiffré), **pas de manette** (ni libusb, ni pilote WinUSB), **pas de moniteur** (aucun outil du paquet ne le lit), pas de
carillon (talon de `StartupChime`, l'invité a son son), dossier partagé et dépôt de fichiers macOS seulement,
disque de démarrage OS 9 sans virtio-9p. Binaire générique : ni `-march=native`, ni LTO, ni PGO.

## 5. Ce qui n'est PAS prouvé

Deux pièges du runner, [A] CI du 04/11/2026 : `-qmp stdio` sur un tube ne répond rien sous Windows, et une sonde de ce
type peut ne jamais rendre la main, d'où QMP par TCP dans `smoke_windows.sh` et aucune sonde QMP dans
`build_qemu_qfb.sh` sous ce profil ([F] cause non établie) ; `timeout`, `find` et `sort` de `System32` passent avant ceux
de MSYS2 si `/usr/bin` n'est pas en tête du `PATH`.

Le runner n'a ni GPU, ni son, ni disque Tiger, ni personne devant l'écran. La CI prouve :

- la construction par MSYS2 UCRT64, la série de patchs sur le QEMU sous Windows ;
- le contenu du zip, les DLL en liste blanche, `opengl32` lié et pas embarqué ;
- un `PATH` de machine neuve : les `.exe` chargent sans MSYS2 ;
- `-display dbus`, `dsound`, `qemu-img`, Open Firmware en SMP, QMP ;
- la ligne de `LaunchPlan` lancée pour de bon, le bus D-Bus, le listener, `Win32.Map`, une image non vide hachée ;
- le verrou, et le repli logiciel de qgpu sur `opengl32` en GL 1.1 (dans `.run/qemu.log`).

Il reste à faire, **sur une vraie machine Windows, avant de dire que ce paquet marche** :

| Non prouvé | Comment |
|---|---|
| Rendu GL de qgpu sur NVIDIA, AMD, Intel | Un jeu, à l'œil, par pilote ; le refus de l'auto-test se lit dans `.run/qemu.log` |
| Démarrage de Tiger, assistant de bout en bout | Un disque neuf, de l'ISO au pilote |
| MTTCG sous le planificateur de Windows | Endurance (`tools/endurance/`, à porter), gel ou corruption ? |
| Vitesse | Sans flottant natif, sans rel32, sans TSC : une matrice, en attendant [F] nettement sous le PC Linux |
| Son `dsound` | À l'oreille ; les tampons du profil `stable` visent CoreAudio |
| Clavier, souris, DPI, plein écran | Une séance réelle ; preuve en fenêtre **et** en plein écran |
| SmartScreen | Un `.exe` non signé qui fait du JIT RWX : avertissement certain, blocage antivirus possible |
| Chemins non ASCII | Un compte `Émile` ; un volume sans formes courtes |

La vitesse du paquet Linux bloque déjà sa publication pour le PC (`docs/vitesse-doom3-x86.md`) : celle de Windows sera moindre.

## 6. Publier

Comme [publier-linux.md](publier-linux.md) : le tag publie, et `publish` attend les deux paquets (`EXPECTED: 2`). **Publier reste
une décision de l'utilisateur**, et le zip ne devrait pas partir sans au moins une ligne du §5 cochée sur un vrai PC.
