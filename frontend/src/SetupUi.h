// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 VERHILLE Arnaud
// SetupUi — l'assistant de premier lancement, à l'écran : des pages tant qu'il n'y a pas de VM, puis
// une bande au-dessus de l'invité qui dit quoi faire dedans.
#pragma once

#include <functional>

class SetupFlow;

struct SetupActions {
    std::function<void()> quit;
    std::function<void()> cancel;    // revenir au système en cours ; absent au tout premier lancement
};

void drawSetupPage(SetupFlow& setup, const SetupActions& actions);

// 0 s'il n'y a rien à afficher à cette étape.
float setupBarHeight(const SetupFlow& setup);
void drawSetupBar(SetupFlow& setup);
