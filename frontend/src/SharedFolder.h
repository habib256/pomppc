// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 VERHILLE Arnaud
// SharedFolder — un dossier du Mac présenté à l'invité comme un disque.
//
// Sans ImGui ni QEMU, pour être testé seul (frontend/tests/shared_test.cpp). Le disque est une image
// HFS+ construite depuis le dossier, branchée en USB. Ce que l'invité y a changé revient sur le Mac
// quand il éjecte le disque ou s'arrête. Ce n'est pas un partage en direct.
//
// [A] Mesuré sous Tiger 10.4.11 (M4, 03/11) : vvfat (fat:rw, FAT32) expose 503 Mo au plus et montre
//     vides des fichiers qui ne le sont pas ; QEMU avertit lui-même que ce mode n'est pas testé.
//     D'où l'image HFS+, qui garde aussi les resource forks.
#pragma once

#include <string>

// Ce qu'on fait d'une entrée en comparant le manifeste (état à la construction), l'image et le Mac.
enum class SyncAction {
    None,        // rien n'a bougé, ou le Mac a seul changé : le Mac a raison
    CopyToMac,   // l'invité a créé ou changé ce fichier
    KeepBoth,    // les deux ont changé : la version de l'invité arrive sous un autre nom
    MoveToTrash  // l'invité l'a supprimé, le Mac ne l'a pas touché : à la Corbeille, jamais effacé
};
SyncAction decideSync(bool inManifest, bool inImage, bool imageChanged, bool onMac, bool macChanged,
                      bool allowDeletes);

struct SharedSyncReport {
    int copied = 0;     // arrivés ou mis à jour sur le Mac
    int conflicts = 0;  // arrivés sous un autre nom
    int trashed = 0;    // partis à la Corbeille
    int failed = 0;     // restés dans l'image : elle est alors conservée
    std::string summary() const;
};

// Le nom du disque dans l'invité : le nom du dossier, dans la limite de HFS+.
std::string sharedVolumeName(const std::string& folder);

// Vide si ce dossier convient, sinon la raison. La racine du disque et un dossier contenant les
// données de POMPPC (l'image finirait dans elle-même) sont refusés.
std::string sharedFolderProblem(const std::string& folder, const std::string& dataRoot);

// Construit image + manifeste. Le dossier n'est pas modifié. Rend faux et remplit err en cas d'échec,
// sans laisser d'image.
bool buildSharedDisk(const std::string& folder, const std::string& mountDir, const std::string& image,
                     const std::string& manifest, std::string* err);

// Le dossier inscrit dans un manifeste : pour reprendre une image restée après un arrêt brutal.
std::string sharedManifestFolder(const std::string& manifest);

// Ramène sur le Mac ce que l'invité a changé. L'image doit être libre (QEMU ne la tient plus).
// allowDeletes = faux après un arrêt brutal : l'image peut être incomplète, et un fichier qui y
// manque n'a alors pas forcément été supprimé.
// Vrai si tout est revenu : image et manifeste sont alors retirés. Faux : ils restent, rien n'est perdu.
bool syncSharedDisk(const std::string& folder, const std::string& mountDir, const std::string& image,
                    const std::string& manifest, bool allowDeletes, const std::string& guestName,
                    SharedSyncReport* report, std::string* err);

// Dans une réponse de query-block : l'invité a-t-il éjecté le périphérique USB `id` ?
// [A] Mesuré sous Tiger 10.4.11 (M4, 03/11) : diskutil eject passe tray_open à true, locked à false.
bool usbDiskEjected(const std::string& queryBlockReply, const std::string& id);
