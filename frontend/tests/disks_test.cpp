// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 VERHILLE Arnaud
// Le choix des disques : liste, limites, sélection et création, sans ImGui ni VM.
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "DiskConfig.h"

namespace fs = std::filesystem;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void qcow2(const fs::path& p, uint64_t bytes) {
    std::ofstream o(p, std::ios::binary);
    unsigned char h[32] = {'Q', 'F', 'I', 0xfb};
    for (int i = 0; i < 8; ++i) h[31 - i] = (unsigned char)(bytes >> (8 * i));
    o.write(reinterpret_cast<char*>(h), sizeof(h));
}

int main() {
    char tmpl[] = "/tmp/pomppc-disks-XXXXXX";
    const std::string root = mkdtemp(tmpl);
    fs::create_directories(root + "/disks");
    const uint64_t G = 1024ULL * 1024 * 1024;
    qcow2(root + "/disks/tiger.qcow2", 16 * G);
    qcow2(root + "/disks/data.qcow2", 2 * G);
    qcow2(root + "/disks/more.qcow2", 3 * G / 2);
    std::ofstream(root + "/disks/fake.qcow2") << "not a disk, long enough to read a full header";
    std::ofstream(root + "/disks/notes.txt") << "x";

    auto disks = listDisks(root);
    CHECK(disks.size() == 3);
    CHECK(disks[0].name == "data.qcow2" && disks[2].name == "tiger.qcow2");
    CHECK(disks[2].bytes == 16 * G);
    CHECK(formatDiskSize(16 * G) == "16 GB");
    CHECK(formatDiskSize(3 * G / 2) == "1.5 GB");

    CHECK(setDiskSsd(root, "data.qcow2", true));
    CHECK(listDisks(root)[0].ssd);
    CHECK(setDiskSsd(root, "data.qcow2", false));
    CHECK(!listDisks(root)[0].ssd);
    CHECK(setDiskSsd(root, "data.qcow2", false));  // déjà HD

    CHECK(maxExtraDisks(root, true) == 2);
    CHECK(maxExtraDisks(root, false) == 2);
    std::ofstream(root + "/disks/games.iso") << "x";
    CHECK(maxExtraDisks(root, false) == 1);
    CHECK(maxExtraDisks(root, true) == 2);

    DiskSelection s{"gone.qcow2", {"data.qcow2", "data.qcow2", "gone.qcow2", "tiger.qcow2", "more.qcow2"}};
    DiskSelection n = normalizeSelection(s, disks, "tiger.qcow2", 2);
    CHECK(n.boot == "tiger.qcow2");
    CHECK((n.extras == std::vector<std::string>{"data.qcow2", "more.qcow2"}));
    CHECK(normalizeSelection(s, disks, "tiger.qcow2", 1).extras.size() == 1);
    CHECK(normalizeSelection({}, disks, "absent.qcow2", 2).boot == "absent.qcow2");
    CHECK(normalizeSelection({"data.qcow2", {}}, disks, "tiger.qcow2", 2).boot == "data.qcow2");

    auto env = diskEnv("/r", true, {"a.qcow2", {"b.qcow2", "c d.qcow2"}});
    CHECK(env.size() == 2 && env[0] == "DISK=/r/disks/a.qcow2");
    CHECK(env[1] == "POMPPC_EXTRA_DISKS=/r/disks/b.qcow2;/r/disks/c d.qcow2");
    CHECK(diskEnv("/r", false, {"a.qcow2", {}})[0] == "OS9_DISK=/r/disks/a.qcow2");
    CHECK(diskEnv("/r", false, {"a.qcow2", {}})[1] == "POMPPC_EXTRA_DISKS=");
    CHECK(diskEnv("/r", true, {}).empty());

    CHECK(joinNames({"a", "b"}) == "a;b");
    CHECK((splitNames("a;;b;") == std::vector<std::string>{"a", "b"}));
    CHECK(splitNames("").empty());

    CHECK(validDiskName("My Data_2-a"));
    for (const char* bad : {"", " x", "x ", "-x", "a/b", "../a", "a;b", "a'b", "a\"b", "a$b", "é"})
        CHECK(!validDiskName(bad));

    // qemu-img factice : la création passe par config.env, sans QEMU installé.
    std::ofstream(root + "/config.env") << "qemu-img() { printf 'QFI\\373' > \"$5\"; dd if=/dev/zero bs=1 count=28 >> \"$5\" 2>/dev/null; }\n";
    std::string err;
    CHECK(createDisk(root, "New Disk", 8, true, &err));
    CHECK(fs::exists(root + "/disks/New Disk.qcow2"));
    CHECK(fs::exists(root + "/disks/New Disk.qcow2.type"));
    const auto stamp = fs::file_size(root + "/disks/tiger.qcow2");
    CHECK(!createDisk(root, "tiger", 8, false, &err));
    CHECK(fs::file_size(root + "/disks/tiger.qcow2") == stamp);
    CHECK(!createDisk(root, "../evil", 8, false, &err));
    CHECK(!createDisk(root, "ok", 0, false, &err));
    CHECK(!createDisk(root, "ok", 4096, false, &err));
    CHECK(!fs::exists(root + "/disks/ok.qcow2"));

    fs::remove_all(root);
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
