// GPL3 - Copyleft VERHILLE Arnaud
// FrontendSettings — réglages d'ImGuiDock gardés d'un lancement à l'autre.
//
// Écrits à l'arrêt dans .run/imguidock.conf (à côté d'imgui.ini, qui garde la
// disposition des fenêtres, et de chime.conf), relus au lancement. Format
// texte « clé = valeur », une par ligne, lisible et modifiable à la main : une
// clé inconnue ou une valeur illisible est ignorée (la valeur par défaut
// reste), jamais une erreur — un fichier d'une autre version ne doit pas
// empêcher le frontend de démarrer.
#pragma once

#include <string>

struct FrontendSettings {
    std::string os = "os9";      // "os9" ou "tiger" ; ignoré si un lanceur est passé en argument
    bool sound = true;
    bool pad = true;
    bool grabbed = true;         // clavier → invité
    bool preferTablet = true;    // souris absolue en fenêtre
    bool fitView = true;
    float zoom = 1.0f;           // 0,5 à 2 quand fitView est faux
    bool showLibrary = true;
    bool showBilan = true;
    bool showJournal = true;
    bool fullscreen = false;
    // Géométrie de la fenêtre (mode fenêtré), valide si winW/winH > 0.
    int winX = 0, winY = 0, winW = 0, winH = 0;

    // Rend faux si le fichier est absent ou illisible (réglages par défaut gardés).
    bool load(const std::string& path);
    // Écriture atomique (fichier .tmp puis rename). Rend faux en cas d'échec.
    bool save(const std::string& path) const;
};
