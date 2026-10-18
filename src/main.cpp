// Frame Assist (v0.1, EXPERIMENTAL, sin probar en dispositivo)
// Idea: guardamos snapshots de los ultimos pasos de fisica. Si mueres poco despues
// de un click, volvemos a un snapshot anterior y reintentamos el click desplazado
// (-1, +1, -2, +2... pasos). Si ninguno sobrevive, mueres normal.
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <deque>
#include <vector>

using namespace geode::prelude;

struct Ev { int step; bool down; };
struct Snap { int step; CheckpointObject* cp; };

namespace {
    int g_step = 0;
    bool g_replaying = false;
    int g_replayEnd = 0;
    int g_tries = 0;
    std::deque<Snap> g_snaps;
    std::vector<Ev> g_events;   // clicks reales del jugador (recientes)
    std::vector<Ev> g_replay;   // clicks a inyectar durante el reintento

    int tol() {
        auto v = Mod::get()->getSettingValue<std::string>("stage");
        if (v == "Off") return 0;
        return std::atoi(v.c_str());
    }
    bool on() { return tol() > 0; }

    void clearAll() {
        for (auto& s : g_snaps) if (s.cp) s.cp->release();
        g_snaps.clear(); g_events.clear(); g_replay.clear();
        g_step = 0; g_tries = 0; g_replaying = false;
    }
}

class $modify(FAGameLayer, GJBaseGameLayer) {
    void processCommands(float dt) {
        auto pl = PlayLayer::get();
        if (pl && on() && !m_player1->m_isDead) {
            if (g_replaying) {
                for (auto& e : g_replay)
                    if (e.step == g_step) GJBaseGameLayer::handleButton(e.down, 1, true);
                if (g_step > g_replayEnd) g_replaying = false;
            } else {
                // snapshot ANTES de simular este paso
                if (auto cp = pl->createCheckpoint()) {
                    cp->retain();
                    g_snaps.push_back({g_step, cp});
                    while ((int)g_snaps.size() > tol() * 2 + 4) {
                        g_snaps.front().cp->release();
                        g_snaps.pop_front();
                    }
                }
            }
            g_step++;
        }
        GJBaseGameLayer::processCommands(dt);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        // durante un reintento ignoramos el input real (lo inyectamos nosotros)
        if (on() && g_replaying && PlayLayer::get()) {
            // las inyecciones propias llaman a GJBaseGameLayer::handleButton directo
            // y no pasan por este hook, asi que aqui solo llegan clicks reales
            return;
        }
        if (on() && button == 1 && isPlayer1) g_events.push_back({g_step, down});
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

class $modify(FAPlayLayer, PlayLayer) {
    void resetLevel() {
        clearAll();
        PlayLayer::resetLevel();
    }

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        // indicador siempre visible: la ayuda nunca queda oculta
        if (on()) {
            auto lbl = CCLabelBMFont::create(fmt::format("Frame Assist: {} ticks", tol()).c_str(), "bigFont.fnt");
            lbl->setScale(0.35f);
            lbl->setOpacity(160);
            lbl->setAnchorPoint({0.f, 1.f});
            auto win = CCDirector::get()->getWinSize();
            lbl->setPosition({6.f, win.height - 6.f});
            lbl->setZOrder(1000);
            this->addChild(lbl);
        }
    }

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        if (!on() || obj == m_anticheatSpike || m_isPracticeMode || g_snaps.empty()) {
            return PlayLayer::destroyPlayer(player, obj);
        }

        // ultimo click (down) cercano a la muerte
        int deathStep = g_step;
        const Ev* last = nullptr;
        for (auto& e : g_events)
            if (e.down && deathStep - e.step <= tol() * 2 + 2) last = &e;
        if (!last) return PlayLayer::destroyPlayer(player, obj);

        // candidatos: -1,+1,-2,+2,... (indice = g_tries)
        int k = g_tries / 2 + 1;
        int shift = (g_tries % 2 == 0) ? -k : k;
        if (k > tol()) {
            clearAll();
            return PlayLayer::destroyPlayer(player, obj);
        }
        g_tries++;

        int newClick = last->step + shift;
        int from = std::min(newClick, last->step);

        // buscar snapshot en 'from'
        CheckpointObject* target = nullptr;
        int targetStep = 0;
        for (auto& s : g_snaps) if (s.step == from) { target = s.cp; targetStep = s.step; }
        if (!target || newClick >= deathStep) {
            return PlayLayer::destroyPlayer(player, obj);
        }

        // armar el replay: click desplazado + eventos originales posteriores
        g_replay.clear();
        g_replay.push_back({newClick, true});
        for (auto& e : g_events)
            if (e.step > last->step || (e.step == last->step && !e.down))
                g_replay.push_back(e);

        // descartar snapshots/eventos desde el punto de rebobinado
        while (!g_snaps.empty() && g_snaps.back().step >= targetStep) {
            g_snaps.back().cp->release();
            g_snaps.pop_back();
        }
        g_step = targetStep;
        g_replayEnd = deathStep + 2;
        g_replaying = true;

        PlayLayer::loadFromCheckpoint(target);
        // no llamamos al destroyPlayer original: reintentamos
    }
};
