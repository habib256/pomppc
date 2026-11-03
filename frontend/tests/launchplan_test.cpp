// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 VERHILLE Arnaud
// La ligne de commande de QEMU du paquet Windows, sans QEMU ni Windows. La ligne elle-même est lancée
// pour de bon par bridge_probe, dans packaging/windows/smoke_windows.sh.
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <map>
#include <set>
#include <string>

#include "LaunchPlan.h"

namespace {

struct Fake {
    std::map<std::string, std::string> env;
    std::set<std::string> files;
    std::map<std::string, std::string> lines;
    std::set<int> taken;
    bool badPaths = false;
    LaunchHost host() {
        LaunchHost h;
        h.env = [this](const std::string& n, std::string* v) {
            auto it = env.find(n);
            if (it == env.end()) return false;
            *v = it->second;
            return true;
        };
        h.fileExists = [this](const std::string& p) { return files.count(p) != 0; };
        h.firstLine = [this](const std::string& p) { return lines.count(p) ? lines[p] : std::string(); };
        h.fileSize = [this](const std::string& p) { return files.count(p) ? 300ull * 1024 * 1024 : 0ull; };
        h.portFree = [this](int port) { return taken.count(port) == 0; };
        h.qemuPath = [this](const std::string& p) { return badPaths ? std::string() : p; };
        return h;
    }
};

std::string line(const LaunchPlan& p) {
    std::string all;
    for (const std::string& a : p.args) all += a + "\n";
    return all;
}
bool has(const LaunchPlan& p, const std::string& text) { return line(p).find(text) != std::string::npos; }

Fake base() {
    Fake f;
    f.files = {"D/disks/tiger.qcow2", "D/disks/os9.qcow2", "P/patches/smp-mac99/openbios-smp-screamer.elf",
               "P/pomppc-guest.iso"};
    return f;
}
LaunchInput in(bool tiger) {
    LaunchInput i;
    i.packageRoot = "P";
    i.dataRoot = "D";
    i.qmpPath = "D/.run/qmp-1.sock";
    i.tiger = tiger;
    return i;
}

}  // namespace

int main() {
    {   // la ligne du jeu, celle de run_tiger.sh
        Fake f = base();
        LaunchPlan p = buildLaunchPlan(in(true), f.host());
        assert(p.ok);
        assert(p.exe == "P/qemu-system-ppc.exe");
        assert(p.lockFile == "D/.run/tiger.lock");
        assert(has(p, "-L\nP/share/qemu\n"));
        assert(has(p, "-M\nmac99,via=pmu\n"));
        assert(has(p, "-m\n768\n"));          // Screamer : moins de 1 Go
        assert(has(p, "-smp\n2\n"));
        assert(has(p, "tcg,thread=multi,x-jc-bits=14"));
        assert(has(p, "-display\ndbus,p2p=on\n"));
        assert(has(p, "dsound,id=snd0"));
        assert(has(p, "hostfwd=tcp:127.0.0.1:2222-:22"));
        assert(has(p, "usb-tablet,id=pointer0,x-abs-margin=15"));
        assert(has(p, "qgpu-pci,id=gpu0,backend=auto\n"));
        assert(has(p, "file=P/pomppc-guest.iso"));
        assert(has(p, "unix:D/.run/qmp-1.sock,server=on,wait=off"));
        assert(has(p, "boot-device=hd:10,\\System\\Library\\CoreServices\\BootX"));
        // Rien de ce que Win64 n'a pas : voir l'en-tête de LaunchPlan.cpp.
        for (const char* absent : {"x-fp-native", "x-vfp-native", "x-fp-known", "x-lfs-known", "x-tb-fast", "x-jit-",
                                   "x-tlb-base", "qe=on", "usb-host", "x-tcg-cold"})
            assert(!has(p, absent));
    }
    {   // SNAPSHOT : pas de verrou
        Fake f = base();
        f.env["SNAPSHOT"] = "1";
        LaunchPlan p = buildLaunchPlan(in(true), f.host());
        assert(p.ok && p.lockFile.empty() && has(p, "-snapshot\n"));
    }
    {   // un disque manquant se dit, et rien ne part
        Fake f = base();
        f.files.erase("D/disks/tiger.qcow2");
        assert(!buildLaunchPlan(in(true), f.host()).ok);
    }
    {   // un chemin que QEMU ne saurait pas lire se dit aussi
        Fake f = base();
        f.badPaths = true;
        assert(!buildLaunchPlan(in(true), f.host()).ok);
    }
    {   // l'assistant
        Fake f = base();
        f.files.insert("D/images/MacOSX.4.iso");
        f.env = {{"POMPPC_INSTALL_CD", "D/images/MacOSX.4.iso"}, {"POMPPC_NO_REBOOT", "1"}, {"SMP", "1"}};
        LaunchPlan p = buildLaunchPlan(in(true), f.host());
        assert(p.ok && has(p, "file=D/images/MacOSX.4.iso") && has(p, "-no-reboot\n"));
        assert(has(p, "boot-device=cd:,\\\\:tbxi") && has(p, "tcg,thread=single"));
    }
    {   // ports pris, marge par disque, virgule dans un chemin
        Fake f = base();
        f.taken = {2222, 2223};
        f.lines["D/disks/a,b.qcow2.tablet-margin"] = "0";
        f.files.insert("D/disks/a,b.qcow2");
        f.env["DISK"] = "D/disks/a,b.qcow2";
        LaunchPlan p = buildLaunchPlan(in(true), f.host());
        assert(p.ok && p.sshPort == 2224 && has(p, "file=D/disks/a,,b.qcow2"));
        assert(!has(p, "x-abs-margin"));
    }
    {   // disques en plus : ordre et emplacements de disques.sh
        Fake f = base();
        f.files.insert("D/disks/x.qcow2");
        f.lines["D/disks/x.qcow2.type"] = "ssd";
        f.env["POMPPC_EXTRA_DISKS"] = "D/disks/x.qcow2;D/disks/absent.qcow2;D/disks/tiger.qcow2";
        LaunchPlan p = buildLaunchPlan(in(true), f.host());
        assert(p.ok && has(p, "ide-hd,drive=disque1,bus=ide.1,unit=1,rotation_rate=1,model=POMPPC SSD"));
        assert(!has(p, "disque2"));
    }
    {   // Mac OS 9 sur le disque (300 Mo dans le faux) puis en installation
        Fake f = base();
        LaunchPlan p = buildLaunchPlan(in(false), f.host());
        assert(p.ok && p.lockFile == "D/.run/os9.lock" && has(p, "-smp\n1\n") && has(p, "-nic\nnone\n"));
        assert(has(p, "boot-device=hd:,\\\\:tbxi"));
        f.env["POMPPC_OS9_MODE"] = "install";
        assert(!buildLaunchPlan(in(false), f.host()).ok);   // pas de CD
        f.files.insert("D/disks/os9-install.iso");
        p = buildLaunchPlan(in(false), f.host());
        assert(p.ok && has(p, "boot-device=cd:,\\\\:tbxi"));
    }
    // Les règles du CRT de Windows
    assert(windowsCommandLine({"a", "b c"}) == "a \"b c\"");
    assert(windowsCommandLine({"C:\\x y\\"}) == "\"C:\\x y\\\\\"");
    assert(windowsCommandLine({"a\"b"}) == "\"a\\\"b\"");
    assert(windowsCommandLine({"hd:,\\\\:tbxi"}) == "hd:,\\\\:tbxi");
    assert(windowsCommandLine({""}) == "\"\"");
    std::puts("launchplan_test : OK");
    return 0;
}
