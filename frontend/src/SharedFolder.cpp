// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 VERHILLE Arnaud
#include "SharedFolder.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <vector>

#include "SetupFlow.h"

namespace fs = std::filesystem;

namespace {

struct Stamp {
    bool exists = false;
    bool dir = false;
    uint64_t size = 0;
    int64_t mtime = 0;
};

Stamp stampOf(const std::string& path) {
    Stamp s;
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) return s;
    s.exists = true;
    s.dir = S_ISDIR(st.st_mode);
    s.size = s.dir ? 0 : (uint64_t)st.st_size;
    s.mtime = (int64_t)st.st_mtime;
    return s;
}

bool sameFile(const Stamp& a, uint64_t size, int64_t mtime) {
    return a.dir || (a.size == size && a.mtime == mtime);
}

// Ce que les systèmes écrivent pour eux-mêmes : ni à ramener, ni à supprimer. Les noms de Mac OS 9 ne
// sont réservés qu'à la racine du disque : ailleurs, un dossier « Trash » est celui de l'utilisateur.
bool systemName(const std::string& name, bool atRoot) {
    static const std::set<std::string> everywhere = {".DS_Store"};
    static const std::set<std::string> root = {
        ".Trashes", ".fseventsd", ".Spotlight-V100", ".TemporaryItems", ".HFS+ Private Directory Data\r",
        "Desktop DB", "Desktop DF", "Desktop Folder", "Trash", "TheVolumeSettingsFolder", "TheFindByContentFolder",
        "Temporary Items", "Network Trash Folder", "Cleanup At Startup", "Shutdown Check", "VM Storage",
    };
    if (everywhere.count(name) || (atRoot && root.count(name))) return true;
    return name.size() >= 2 && name[0] == '.' && name[1] == '_';
}

struct Row {
    std::string rel;
    bool dir = false;
    uint64_t imageSize = 0, macSize = 0;
    int64_t imageTime = 0, macTime = 0;
};

// Un nom de fichier peut contenir une tabulation ou un retour à la ligne.
std::string esc(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '%') out += "%25";
        else if (c == '\t') out += "%09";
        else if (c == '\n') out += "%0A";
        else if (c == '\r') out += "%0D";
        else out += c;
    }
    return out;
}
std::string unesc(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const std::string h = s.substr(i + 1, 2);
            if (h == "25") { out += '%'; i += 2; continue; }
            if (h == "09") { out += '\t'; i += 2; continue; }
            if (h == "0A") { out += '\n'; i += 2; continue; }
            if (h == "0D") { out += '\r'; i += 2; continue; }
        }
        out += s[i];
    }
    return out;
}

// Tous les chemins de `base`, sans suivre les liens, sans les fichiers système. HFS+ décompose les
// accents : la clé est le nom vu dans l'image, et APFS comme HFS+ retrouvent ce nom côté Mac.
void walk(const std::string& base, const std::string& rel, std::vector<std::string>* out, std::string* err) {
    std::error_code ec;
    const std::string dir = rel.empty() ? base : base + "/" + rel;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (systemName(name, rel.empty())) continue;
        const std::string child = rel.empty() ? name : rel + "/" + name;
        out->push_back(child);
        if (stampOf(base + "/" + child).dir) walk(base, child, out, err);
    }
    if (ec && err->empty()) *err = "POMPPC couldn't read " + dir + ".";
}

bool detach(const std::string& mountDir) {
    if (!SetupFlow::runTool({"hdiutil", "detach", "-quiet", mountDir}) &&
        !SetupFlow::runTool({"hdiutil", "detach", "-quiet", "-force", mountDir})) {
        return false;
    }
    std::error_code ec;
    fs::remove(mountDir, ec);
    return true;
}

bool readManifest(const std::string& path, std::string* folder, std::map<std::string, Row>* rows) {
    std::ifstream in(path);
    std::string line;
    if (!std::getline(in, line)) return false;
    *folder = unesc(line);
    while (std::getline(in, line)) {
        std::stringstream ss(line);
        std::string rel, kind, a, b, c, d;
        if (!std::getline(ss, rel, '\t') || !std::getline(ss, kind, '\t') || !std::getline(ss, a, '\t') ||
            !std::getline(ss, b, '\t') || !std::getline(ss, c, '\t') || !std::getline(ss, d)) {
            return false;   // tronqué : ne rien en déduire
        }
        Row r;
        r.rel = unesc(rel);
        r.dir = kind == "d";
        r.imageSize = std::strtoull(a.c_str(), nullptr, 10);
        r.imageTime = std::strtoll(b.c_str(), nullptr, 10);
        r.macSize = std::strtoull(c.c_str(), nullptr, 10);
        r.macTime = std::strtoll(d.c_str(), nullptr, 10);
        (*rows)[r.rel] = r;
    }
    return true;
}

// « photo.jpg » → « photo (from Tiger).jpg », puis « photo (from Tiger 2).jpg ».
std::string conflictName(const std::string& path, const std::string& guest) {
    const fs::path p(path);
    const std::string dir = p.parent_path().string(), stem = p.stem().string(), ext = p.extension().string();
    for (int n = 1;; ++n) {
        const std::string tag = n == 1 ? " (from " + guest + ")" : " (from " + guest + " " + std::to_string(n) + ")";
        const std::string candidate = dir + "/" + stem + tag + ext;
        if (!stampOf(candidate).exists) return candidate;
    }
}

// ditto garde les resource forks et les dates. Copie à côté puis renomme : le fichier du Mac n'est
// jamais à moitié écrit.
bool copyIn(const std::string& from, const std::string& to) {
    std::error_code ec;
    fs::create_directories(fs::path(to).parent_path(), ec);
    const std::string tmp = fs::path(to).parent_path().string() + "/.pomppc-copie-" + std::to_string(getpid());
    fs::remove_all(tmp, ec);
    if (!SetupFlow::runTool({"ditto", from, tmp})) {
        fs::remove_all(tmp, ec);
        return false;
    }
    fs::rename(tmp, to, ec);
    if (ec) {
        fs::remove_all(tmp, ec);
        return false;
    }
    return true;
}

bool toTrash(const std::string& path) {
    // POMPPC_TRASH : le dossier d'une épreuve, pour ne pas remplir la vraie Corbeille.
    if (const char* forced = std::getenv("POMPPC_TRASH")) {
        std::error_code ec;
        fs::create_directories(forced, ec);
        std::string to = std::string(forced) + "/" + fs::path(path).filename().string();
        for (int n = 2; stampOf(to).exists; ++n)
            to = std::string(forced) + "/" + std::to_string(n) + "-" + fs::path(path).filename().string();
        fs::rename(path, to, ec);
        return !ec;
    }
    return SetupFlow::runTool({"/usr/bin/trash", path});   // macOS 15, la version visée
}

bool dirEmptyButSystem(const std::string& path) {
    std::error_code ec;
    for (fs::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec))
        if (!systemName(it->path().filename().string(), false)) return false;
    return !ec;
}

}  // namespace

SyncAction decideSync(bool inManifest, bool inImage, bool imageChanged, bool onMac, bool macChanged,
                      bool allowDeletes) {
    if (inImage && !inManifest) return onMac ? SyncAction::KeepBoth : SyncAction::CopyToMac;
    if (inImage) {
        if (!imageChanged) return SyncAction::None;
        return onMac && macChanged ? SyncAction::KeepBoth : SyncAction::CopyToMac;
    }
    if (inManifest && onMac && !macChanged && allowDeletes) return SyncAction::MoveToTrash;
    return SyncAction::None;
}

std::string SharedSyncReport::summary() const {
    auto count = [](int n, const char* what) {
        return std::to_string(n) + " " + what + (n > 1 ? "s" : "");
    };
    std::string s;
    auto add = [&](const std::string& part) { s += (s.empty() ? "" : ", ") + part; };
    if (copied) add(count(copied, "file") + " updated");
    if (conflicts) add(count(conflicts, "file") + " kept under a new name");
    if (trashed) add(count(trashed, "item") + " moved to the Trash");
    if (failed) add(count(failed, "file") + " couldn't be copied");
    return s.empty() ? "No changes." : s + ".";
}

std::string sharedVolumeName(const std::string& folder) {
    std::string name = fs::path(folder).filename().string();
    if (name.empty()) name = fs::path(folder).parent_path().filename().string();
    name.erase(std::remove(name.begin(), name.end(), ':'), name.end());
    // [E] Un nom de volume de plus de 27 caractères pose problème sous Mac OS 9 (limite de HFS).
    //     Tranché par : un essai sous OS 9 avec un nom plus long.
    if (name.size() > 27) {
        size_t cut = 27;
        while (cut > 0 && ((unsigned char)name[cut] & 0xC0) == 0x80) --cut;   // pas au milieu d'un caractère
        name.resize(cut);
    }
    return name.empty() ? "Mac Folder" : name;
}

std::string sharedFolderProblem(const std::string& folder, const std::string& dataRoot) {
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) return "This folder can't be found.";
    const fs::path f = fs::weakly_canonical(folder, ec);
    const fs::path d = fs::weakly_canonical(dataRoot, ec);
    if (f == f.root_path()) return "Choose a folder rather than a whole disk.";
    auto dIt = d.begin();
    bool contains = true;
    for (auto it = f.begin(); it != f.end(); ++it, ++dIt) {
        if (dIt == d.end() || *it != *dIt) { contains = false; break; }
    }
    if (contains) return "This folder contains POMPPC's own files. Choose another one.";
    return "";
}

bool buildSharedDisk(const std::string& folder, const std::string& mountDir, const std::string& image,
                     const std::string& manifest, std::string* err) {
#ifdef __APPLE__
    std::error_code ec;
    std::vector<std::string> paths;
    std::string walkErr;
    walk(folder, "", &paths, &walkErr);
    if (!walkErr.empty()) { *err = walkErr; return false; }
    uint64_t bytes = 0;
    for (const std::string& rel : paths) bytes += stampOf(folder + "/" + rel).size;

    // [A] Mesuré sur APFS (M4, 03/11) : hdiutil alloue l'image entière. Cette marge coûte donc sa
    //     taille sur le Mac tant que le disque est branché, et borne ce que l'invité peut ajouter.
    const uint64_t mb = bytes / (1024 * 1024) + bytes / (1024 * 1024) / 8 + 1024;
    fs::create_directories(fs::path(image).parent_path(), ec);
    const fs::space_info room = fs::space(fs::path(image).parent_path(), ec);
    if (!ec && room.available / (1024 * 1024) < mb + 512) {
        *err = "There isn't enough free space on your Mac: this folder needs " +
               std::to_string(mb / 1024 + 1) + " GB.";
        return false;
    }
    fs::remove(image, ec);
    fs::remove(manifest, ec);
    if (!SetupFlow::runTool({"hdiutil", "create", "-quiet", "-size", std::to_string(mb) + "m", "-fs", "HFS+",
                             "-layout", "NONE", "-volname", sharedVolumeName(folder), "-ov", image})) {
        *err = "POMPPC couldn't create the disk.";
        return false;
    }
    fs::create_directories(mountDir, ec);
    if (!SetupFlow::runTool({"hdiutil", "attach", "-quiet", "-nobrowse", "-noverify", "-mountpoint", mountDir, image})) {
        *err = "POMPPC couldn't prepare the disk.";
        fs::remove(image, ec);
        return false;
    }
    auto fail = [&](const std::string& why) {
        detach(mountDir);
        std::error_code e2;
        fs::remove(image, e2);
        fs::remove(manifest, e2);
        *err = why;
        return false;
    };
    if (!SetupFlow::runTool({"ditto", folder, mountDir}))
        return fail("POMPPC couldn't copy this folder. Is a file unreadable?");

    // Le manifeste se relève dans l'image : c'est elle qui fixe les noms et les dates de référence.
    std::vector<std::string> copied;
    walkErr.clear();
    walk(mountDir, "", &copied, &walkErr);
    if (!walkErr.empty()) return fail(walkErr);
    std::ofstream out(manifest + ".tmp");
    out << esc(folder) << '\n';
    for (const std::string& rel : copied) {
        const Stamp i = stampOf(mountDir + "/" + rel), m = stampOf(folder + "/" + rel);
        out << esc(rel) << '\t' << (i.dir ? 'd' : 'f') << '\t' << i.size << '\t' << i.mtime << '\t'
            << m.size << '\t' << m.mtime << '\n';
    }
    out.close();
    if (!out) return fail("POMPPC couldn't save the disk's list of files.");
    if (!detach(mountDir)) return fail("POMPPC couldn't prepare the disk.");
    fs::rename(manifest + ".tmp", manifest, ec);
    if (ec) {
        fs::remove(image, ec);
        *err = "POMPPC couldn't save the disk's list of files.";
        return false;
    }
    return true;
#else
    (void)folder; (void)mountDir; (void)image; (void)manifest;
    *err = "Shared folders aren't available on this host yet.";
    return false;
#endif
}

std::string sharedManifestFolder(const std::string& manifest) {
    std::string folder;
    std::map<std::string, Row> rows;
    return readManifest(manifest, &folder, &rows) ? folder : "";
}

bool syncSharedDisk(const std::string& folder, const std::string& mountDir, const std::string& image,
                    const std::string& manifest, bool allowDeletes, const std::string& guestName,
                    SharedSyncReport* report, std::string* err) {
#ifdef __APPLE__
    std::string recorded;
    std::map<std::string, Row> rows;
    if (!readManifest(manifest, &recorded, &rows)) {
        *err = "POMPPC couldn't read the disk's list of files.";
        return false;
    }
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) {
        *err = "The shared folder is gone. POMPPC kept the disk: " + image;
        return false;
    }
    fs::create_directories(mountDir, ec);
    if (!SetupFlow::runTool({"hdiutil", "attach", "-quiet", "-nobrowse", "-readonly", "-noverify", "-mountpoint",
                             mountDir, image})) {
        *err = "POMPPC couldn't read the disk. It's still here: " + image;
        return false;
    }
    std::vector<std::string> paths;
    std::string walkErr;
    walk(mountDir, "", &paths, &walkErr);
    if (!walkErr.empty()) {
        detach(mountDir);
        *err = walkErr;
        return false;
    }
    std::sort(paths.begin(), paths.end());   // un dossier avant ce qu'il contient

    std::set<std::string> seen;
    for (const std::string& rel : paths) {
        seen.insert(rel);
        const std::string from = mountDir + "/" + rel, to = folder + "/" + rel;
        const Stamp i = stampOf(from), m = stampOf(to);
        auto row = rows.find(rel);
        const bool known = row != rows.end();
        if (i.dir) {
            if (!m.exists) {
                fs::create_directories(to, ec);
                if (ec) report->failed++;
            }
            continue;
        }
        const bool imageChanged = known && !sameFile(i, row->second.imageSize, row->second.imageTime);
        const bool macChanged = known && m.exists && !sameFile(m, row->second.macSize, row->second.macTime);
        switch (decideSync(known, true, imageChanged, m.exists, macChanged, allowDeletes)) {
            case SyncAction::CopyToMac:
                if (copyIn(from, to)) report->copied++;
                else report->failed++;
                break;
            case SyncAction::KeepBoth:
                if (copyIn(from, conflictName(to, guestName))) report->conflicts++;
                else report->failed++;
                break;
            default:
                break;
        }
    }
    const bool detached = detach(mountDir);

    // Les suppressions : les fichiers d'abord, puis les dossiers restés vides, du plus profond au moins profond.
    std::vector<const Row*> gone;
    for (const auto& kv : rows) if (!seen.count(kv.first)) gone.push_back(&kv.second);
    std::sort(gone.begin(), gone.end(), [](const Row* a, const Row* b) { return a->rel.size() > b->rel.size(); });
    for (const Row* r : gone) {
        const std::string to = folder + "/" + r->rel;
        const Stamp m = stampOf(to);
        if (!m.exists) continue;
        if (r->dir) {
            if (allowDeletes && m.dir && dirEmptyButSystem(to)) {
                if (toTrash(to)) report->trashed++;
                else report->failed++;
            }
            continue;
        }
        const bool macChanged = !sameFile(m, r->macSize, r->macTime);
        if (decideSync(true, false, false, true, macChanged, allowDeletes) == SyncAction::MoveToTrash) {
            if (toTrash(to)) report->trashed++;
            else report->failed++;
        }
    }
    if (report->failed || !detached) {
        *err = "POMPPC kept the disk because some files couldn't be copied: " + image;
        return false;
    }
    fs::remove(image, ec);
    fs::remove(manifest, ec);
    return true;
#else
    (void)folder; (void)mountDir; (void)image; (void)manifest; (void)allowDeletes; (void)guestName; (void)report;
    *err = "Shared folders aren't available on this host yet.";
    return false;
#endif
}

bool usbDiskEjected(const std::string& reply, const std::string& id) {
    // Une entrée de la liste à la fois : les profondeurs d'accolades, hors des chaînes.
    const std::string needle = "/peripheral/" + id + "/";
    size_t start = std::string::npos;
    int depth = 0;
    bool inString = false, escaped = false;
    const size_t list = reply.find('[');
    if (list == std::string::npos) return false;
    for (size_t i = list + 1; i < reply.size(); ++i) {
        const char c = reply[i];
        if (inString) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') { inString = true; continue; }
        if (c == '{') {
            if (depth++ == 0) start = i;
        } else if (c == '}' && --depth == 0 && start != std::string::npos) {
            const std::string entry = reply.substr(start, i - start + 1);
            if (entry.find(needle) != std::string::npos)
                return entry.find("\"tray_open\": true") != std::string::npos ||
                       entry.find("\"tray_open\":true") != std::string::npos;
            start = std::string::npos;
        }
    }
    return false;
}
