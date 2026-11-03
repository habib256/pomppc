// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 VERHILLE Arnaud
#include "LaunchPlan.h"

#include <algorithm>
#include <cstdlib>
#include <set>
#include <sstream>

namespace {

// [C] run_tiger.sh, branche Linux:x86_64 (l. 218-258) et argv final (l. 1407-1418) : ce qui suit en est
// le reflet. Une option retirée l'est pour une raison écrite ici ; docs/publier-windows.md §4 en fait
// le tableau de parité (docs/etat-des-hotes.md §1).
//
// RETIRÉES SOUS WINDOWS :
//   x-fp-native, x-fp-native64, x-fp-native-cmp, x-vfp-native, x-vfp-native-cmp, x-fp-known, x-fprf-lazy,
//   x-stfs-known, x-lfs-known : leurs émetteurs x86-64 demandent des registres que l'ABI Win64 ne laisse
//     pas libres (tcg/0017 : tcg-target-has.h ne les définit pas sous _WIN64, le talon de sortie passe six
//     arguments). Sans l'op TCG, x-fp-native retomberait sur le helper de x-fp-flat, chemin qui n'a
//     jamais été mesuré ici : x-fp-inline seul est plus court à défendre.
//   x-tlb-base (tcg/0052) : son émetteur n'emploie que L0/L1 et le code générique, rien de propre à
//     SysV à la relecture, mais [F] aucune partie vérifiée sous Win64. Tranché par : x-tlb-base-verify
//     en jeu sous Windows.
//   x-tb-fast (tcg/0015, 0018) : le chemin x86-64 lit la source d'horloge de Linux et exige un TSC
//     invariant déclaré par le noyau.
//   x-jit-near, x-jit-rel32 (tcg/0006, 0024) : placent le tampon par mmap ; le chemin Windows de QEMU
//     (VirtualAlloc) ne les lit pas.
//   x-tcg-cold (tcg/0056) : allumé sur Linux x86-64 seul, A/B fait là ; rien de mesuré ici.
//   qe=on (qgpu) : Quartz Extreme n'est recommandé que là où il est prouvé (run_tiger.sh l. 1247-1249).
//
// RETIRÉS DE LA LIGNE, AUTRE RAISON :
//   -monitor : aucun outil du paquet ne lit ce socket (matrice, killgame.py… restent dans le dépôt).
//   relais web (scripts/web-proxy.py) : pas de Python dans le paquet. Sans lui Safari sous Tiger ne joint
//     pas le web chiffré d'aujourd'hui ; écart écrit dans docs/publier-windows.md §4.
//   usb-host (manette) : le paquet n'a pas libusb, et le pilote WinUSB se pose à la main.
//   EGL_PLATFORM : sans objet sous WGL. Le verrou est tenu par le frontal, pas par un fd hérité.
const char* const kCpuTiger =
    "g4,x-fast-fp=on,x-sr-tlb=on,x-lfs-inline=on,x-vfp-fast=on,x-vperm-fast=on,x-vmx-inline=on,"
    "x-lmw-vector=on,x-fp-inline=on,x-ret-inline=on,x-jc-idx=on,x-icbi-sync=on,x-msr-nobql=on,"
    "x-tlb-precise=on,x-lmw-inline=on,x-dcbz-inline=on,x-dirty-skip=on,x-pteg-map=on";
const char* const kAccelTiger = ",x-jc-bits=14,x-jc-word=on,x-jc-2way=on";

// [D] QEMU hw/ppc/mac_newworld.c:519-525 @ v11.1.2 : deux canaux IDE, soit 4 emplacements.
const int kIdeSlots = 4;

std::string getOr(const LaunchHost& h, const char* name, const std::string& dflt) {
    std::string v;
    return h.env(name, &v) && !v.empty() ? v : dflt;
}

bool isSet(const LaunchHost& h, const char* name) {
    std::string v;
    return h.env(name, &v) && !v.empty();
}

bool isOff(const LaunchHost& h, const char* name) {   // VAR=0 ; absente, elle vaut allumé
    std::string v;
    return h.env(name, &v) && v == "0";
}

// -drive et -device lisent des listes séparées par des virgules : un chemin double les siennes.
std::string commas(std::string path) {
    for (size_t at = path.find(','); at != std::string::npos; at = path.find(',', at + 2)) path.insert(at, ",");
    return path;
}

std::vector<std::string> splitOn(const std::string& text, char sep) {
    std::vector<std::string> out;
    std::stringstream in(text);
    std::string item;
    while (std::getline(in, item, sep))
        if (!item.empty()) out.push_back(item);
    return out;
}

bool cdArg(const LaunchHost& h, const std::string& path, std::vector<std::string>* args, std::string* err) {
    const std::string q = h.qemuPath(path);
    if (q.empty()) {
        *err = "POMPPC can't open this path because of its characters: " + path;
        return false;
    }
    args->insert(args->end(), {"-drive", "id=gamecd,if=ide,media=cdrom,format=raw,readonly=on,file=" + commas(q)});
    return true;
}

// [C] scripts/disques.sh, disques_args : ordre, dédoublonnage et emplacements identiques.
bool extraDisks(const LaunchHost& h, const std::string& boot, int free, std::vector<std::string>* args,
                std::string* err) {
    const auto ssd = [&](const std::string& disk) { return h.firstLine(disk + ".type") == "ssd"; };
    if (ssd(boot)) {
        args->insert(args->end(), {"-global", "ide-hd.rotation_rate=1", "-global", "ide-hd.model=POMPPC SSD"});
    }
    std::string list;
    if (!h.env("POMPPC_EXTRA_DISKS", &list)) return true;
    std::set<std::string> seen;
    int n = 0, unit = 1;
    for (const std::string& f : splitOn(list, ';')) {
        if (f == boot || !seen.insert(f).second || !h.fileExists(f) || free <= 0) continue;
        const std::string q = h.qemuPath(f);
        if (q.empty()) {
            *err = "POMPPC can't open this path because of its characters: " + f;
            return false;
        }
        ++n;
        const std::string id = "disque" + std::to_string(n);
        args->insert(args->end(), {"-drive", "if=none,id=" + id + ",format=qcow2,file=" + commas(q), "-device",
                                   "ide-hd,drive=" + id + ",bus=ide.1,unit=" + std::to_string(unit) +
                                       ",rotation_rate=" + (ssd(f) ? "1" : "0") +
                                       ",model=" + (ssd(f) ? "POMPPC SSD" : "QEMU HARDDISK")});
        --unit;
        --free;
    }
    return true;
}

// Mêmes choix que tiger_audio.sh et hostcompat.sh : dsound est le seul pilote de ce QEMU. Les réglages
// de tampons du profil stable visent CoreAudio. [F] Bons réglages de dsound : à trouver à l'oreille.
void addAudio(const LaunchHost& h, std::vector<std::string>* args) {
    const std::string dev = getOr(h, "POMPPC_AUDIO_PROFILE", "stable") == "muet" ? "none,id=snd0" : "dsound,id=snd0";
    args->insert(args->end(), {"-audiodev", dev, "-global", "screamer.audiodev=snd0"});
}

bool planTiger(const LaunchInput& in, const LaunchHost& h, LaunchPlan* p) {
    std::vector<std::string>& a = p->args;
    const bool sound = !isSet(h, "NOSOUND");
    int ram = std::atoi(getOr(h, "RAM_MB", "1024").c_str());
    if (sound && ram > 768) ram = 768;   // run_tiger.sh : le Screamer exige moins de 1 Go
    const int smp = std::max(1, std::atoi(getOr(h, "SMP", "2").c_str()));

    const std::string disk = getOr(h, "DISK", in.dataRoot + "/disks/tiger.qcow2");
    if (!h.fileExists(disk)) {
        p->error = "This disk doesn't exist: " + disk;
        return false;
    }
    const std::string qdisk = h.qemuPath(disk);
    if (qdisk.empty()) {
        p->error = "POMPPC can't open this path because of its characters: " + disk;
        return false;
    }

    std::string cpu = kCpuTiger;
    std::string opts;
    if (h.env("CPU_OPTS", &opts) && !opts.empty()) cpu += "," + opts;
    // Le G4 sur la cible 32 bits, en MTTCG (tcg/0050). Sur Windows aussi, le binaire ppc64 reste au paquet
    // pour ne pas s'écarter de sa forme, mais la ligne n'y passe jamais.
    std::string accel = std::string("tcg,thread=") + (smp >= 2 ? "multi" : "single") + kAccelTiger;
    if (h.env("TCG_OPTS", &opts) && !opts.empty()) accel += "," + opts;

    a.insert(a.end(), {"-M", "mac99,via=pmu", "-cpu", cpu, "-m", std::to_string(ram), "-smp", std::to_string(smp),
                       "-display", "dbus,p2p=on", "-g", getOr(h, "RES", "1024x768x32"), "-drive",
                       "file=" + commas(qdisk) + ",format=qcow2,media=disk"});
    if (isSet(h, "SNAPSHOT")) a.push_back("-snapshot");

    // Le CD : celui que l'assistant demande, sinon le CD invité du paquet, sinon le lecteur vide.
    std::string cd = getOr(h, "POMPPC_INSTALL_CD", getOr(h, "POMPPC_CD", ""));
    const bool install = isSet(h, "POMPPC_INSTALL_CD");
    const bool gpu = !isOff(h, "GPU");
    const std::string guestIso = in.packageRoot + "/pomppc-guest.iso";
    int cdSlots = 0;
    if (!cd.empty() && h.fileExists(cd)) {
        if (!cdArg(h, cd, &a, &p->error)) return false;
        cdSlots = 1;
    } else if (gpu && !isOff(h, "GLISO") && h.fileExists(guestIso)) {
        if (!cdArg(h, guestIso, &a, &p->error)) return false;
        cdSlots = 1;
    } else if (!isSet(h, "NOCD")) {
        a.insert(a.end(), {"-drive", "id=gamecd,if=ide,media=cdrom"});
        cdSlots = 1;
    }
    if (!extraDisks(h, disk, 3 - cdSlots, &a, &p->error)) return false;
    if (isSet(h, "POMPPC_NO_REBOOT")) a.push_back("-no-reboot");

    if (isOff(h, "NET")) {
        a.insert(a.end(), {"-nic", "none"});
    } else {
        // run_tiger.sh sonde 2222 et les 9 suivants : un port pris fait échouer tout QEMU, et un
        // démarrage sans ssh vaut mieux qu'un refus de démarrer.
        std::string netdev = "user,id=net0";
        const int first = std::atoi(getOr(h, "SSH_FWD", "2222").c_str());
        if (first != 0) {
            for (int port = first; port <= first + 9 && !p->sshPort; ++port)
                if (h.portFree(port)) p->sshPort = port;
            if (p->sshPort) netdev += ",hostfwd=tcp:127.0.0.1:" + std::to_string(p->sshPort) + "-:22";
        }
        a.insert(a.end(), {"-netdev", netdev, "-device", "sungem,netdev=net0"});
    }

    const std::string bios = in.packageRoot + "/patches/smp-mac99/openbios-smp-screamer.elf";
    const std::string qbios = h.qemuPath(bios);
    if (qbios.empty() || !h.fileExists(bios)) {
        p->error = "This POMPPC folder is incomplete or in a path POMPPC can't open: " + bios;
        return false;
    }
    a.insert(a.end(), {"-bios", qbios, "-accel", accel});
    if (sound) addAudio(h, &a);

    // La marge de la tablette de Tiger 10.4.11 : par disque, sinon 15 (patches/usbhid/0001).
    std::string margin = getOr(h, "TABLET_MARGIN", "");
    if (margin.empty()) margin = h.firstLine(disk + ".tablet-margin");
    margin.erase(std::remove_if(margin.begin(), margin.end(), [](char c) { return c < '0' || c > '9'; }), margin.end());
    if (margin.empty()) margin = "15";
    std::string tablet = "usb-tablet,id=pointer0";
    if (margin != "0") tablet += ",x-abs-margin=" + margin;
    a.insert(a.end(), {"-device", tablet});

    if (isSet(h, "QFB")) {
        const std::string res = getOr(h, "QFB_RES", "1280x800");
        const size_t x = res.find('x');
        if (x != std::string::npos)
            a.insert(a.end(), {"-device", "qfb-pci,id=qfb0,width=" + res.substr(0, x) + ",height=" + res.substr(x + 1) + ",depth=8"});
    }
    if (gpu) {
        std::string dev = "qgpu-pci,id=gpu0,backend=" + getOr(h, "GPU_BACKEND", "auto");
        const std::string scanout = getOr(h, "GPU_SCANOUT", "");
        if (!scanout.empty()) dev += ",scanout=" + scanout;
        if (isSet(h, "GPU_TRACE")) dev += ",trace=on";
        a.insert(a.end(), {"-device", dev});
    }
    for (const std::string& extra : splitOn(getOr(h, "EXTRA_ARGS", ""), ' ')) a.push_back(extra);

    std::string bootArgs;   // défaut du paquet (AppRun) : pas de texte du noyau
    h.env("POMPPC_BOOT_ARGS", &bootArgs);
    a.insert(a.end(), {"-prom-env", "auto-boot?=true", "-prom-env",
                       install ? "boot-device=cd:,\\\\:tbxi" : "boot-device=hd:10,\\System\\Library\\CoreServices\\BootX",
                       "-prom-env", "boot-args=" + bootArgs, "-name", "Tiger", "-qmp",
                       "unix:" + in.qmpPath + ",server=on,wait=off", "-monitor", "none"});
    p->childEnv = {"QGPU_GPU_COPY=" + getOr(h, "QGPU_GPU_COPY", "1"), "QGPU_GLSL=" + getOr(h, "QGPU_GLSL", "1"),
                   "QGPU_GL_FLUSH=" + getOr(h, "QGPU_GL_FLUSH", "1")};
    if (!isSet(h, "SNAPSHOT")) p->lockFile = in.dataRoot + "/.run/tiger.lock";
    return true;
}

// [C] run_os9.sh : mono-cœur, profil de référence de scripts/os9_cpu.sh (g4 nu, tcg nu). Le partage
// virtio-9p n'existe pas sous Windows (have_virtfs, QEMU meson.build @ v11.1.2) et son chargeur
// (disks/extras/classicvirtio) n'est pas au paquet : le lanceur s'en passe déjà dans ce cas.
bool planOs9(const LaunchInput& in, const LaunchHost& h, LaunchPlan* p) {
    std::vector<std::string>& a = p->args;
    const std::string disk = getOr(h, "OS9_DISK", in.dataRoot + "/disks/os9.qcow2");
    const std::string qdisk = h.qemuPath(disk);
    if (!h.fileExists(disk) || qdisk.empty()) {
        p->error = "POMPPC can't open this disk: " + disk;
        return false;
    }
    const std::string mode = getOr(h, "POMPPC_OS9_MODE", "auto");
    const bool bootDisk = mode == "disk" || (mode != "install" && h.fileSize(disk) > 200ull * 1024 * 1024);

    a.insert(a.end(), {"-M", "mac99,via=pmu", "-cpu", "g4", "-accel", "tcg", "-m", getOr(h, "OS9_RAM", "512"),
                       "-smp", "1", "-display", "dbus,p2p=on", "-g", getOr(h, "RES", "1024x768x32")});
    const bool sound = !isSet(h, "NOSOUND");
    const std::string bios = in.packageRoot + "/patches/smp-mac99/openbios-smp-screamer.elf";
    const std::string qbios = h.qemuPath(bios);
    if (sound && !qbios.empty() && h.fileExists(bios)) a.insert(a.end(), {"-bios", qbios});
    if (isSet(h, "SNAPSHOT")) a.push_back("-snapshot");
    a.insert(a.end(), {"-drive", "file=" + commas(qdisk) + ",format=qcow2,media=disk"});

    int drives = 1;
    const std::string games = in.dataRoot + "/disks/games.iso";
    const auto addRawCd = [&](const std::string& path) {
        const std::string q = h.qemuPath(path);
        if (q.empty()) {
            p->error = "POMPPC can't open this path because of its characters: " + path;
            return false;
        }
        a.insert(a.end(), {"-drive", "file=" + commas(q) + ",format=raw,media=cdrom"});
        ++drives;
        return true;
    };
    if (bootDisk) {
        if (h.fileExists(games) && !addRawCd(games)) return false;
        a.insert(a.end(), {"-drive", "id=gamecd,if=ide,media=cdrom"});
        ++drives;
    } else {
        const std::string cd = getOr(h, "OS9_CD", in.dataRoot + "/disks/os9-install.iso");
        if (!h.fileExists(cd)) {
            p->error = "The Mac OS 9 install disc isn't there: " + cd;
            return false;
        }
        if (!addRawCd(cd)) return false;
    }
    if (!extraDisks(h, disk, kIdeSlots - drives, &a, &p->error)) return false;
    if (isSet(h, "POMPPC_NO_REBOOT")) a.push_back("-no-reboot");
    a.insert(a.end(), {"-nic", "none"});
    if (sound) addAudio(h, &a);
    for (const std::string& extra : splitOn(getOr(h, "EXTRA_ARGS", ""), ' ')) a.push_back(extra);
    a.insert(a.end(), {"-prom-env", "auto-boot?=true", "-prom-env",
                       bootDisk ? "boot-device=hd:,\\\\:tbxi" : "boot-device=cd:,\\\\:tbxi", "-name", "MacOS9", "-qmp",
                       "unix:" + in.qmpPath + ",server=on,wait=off", "-monitor", "none"});
    if (!isSet(h, "SNAPSHOT")) p->lockFile = in.dataRoot + "/.run/os9.lock";
    return true;
}

}  // namespace

LaunchPlan buildLaunchPlan(const LaunchInput& in, const LaunchHost& h) {
    LaunchPlan p;
    p.exe = in.packageRoot + "/qemu-system-ppc.exe";
    const std::string share = h.qemuPath(in.packageRoot + "/share/qemu");
    if (share.empty()) {
        p.error = "POMPPC is in a folder whose name it can't pass to QEMU: " + in.packageRoot +
                  ". Move the POMPPC folder to a path with plain letters.";
        return p;
    }
    // -L : QEMU n'a pas à deviner où est son firmware, quel que soit le préfixe de sa construction.
    p.args = {"-L", share};
    p.ok = in.tiger ? planTiger(in, h, &p) : planOs9(in, h, &p);
    return p;
}

std::string windowsCommandLine(const std::vector<std::string>& argv) {
    std::string out;
    for (const std::string& arg : argv) {
        if (!out.empty()) out += ' ';
        if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) {
            out += arg;
            continue;
        }
        out += '"';
        size_t backslashes = 0;
        for (char c : arg) {
            if (c == '\\') {
                ++backslashes;
                continue;
            }
            if (c == '"') out.append(backslashes * 2 + 1, '\\');
            else out.append(backslashes, '\\');
            backslashes = 0;
            out += c;
        }
        out.append(backslashes * 2, '\\');   // suivis du guillemet fermant : ils doublent
        out += '"';
    }
    return out;
}
