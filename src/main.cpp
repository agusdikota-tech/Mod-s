// Frame Assist (v0.6, EXPERIMENTAL)
// Rebobinado y rejugado en el MISMO fotograma (sin corte visible) + contador de rescates.
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <deque>
#include <vector>
#include <algorithm>
#include <cstdlib>

using namespace geode::prelude;

struct Ev { int step; bool down; };
struct Snap { int step; CheckpointObject* cp; };

namespace {
    int g_step = 0;
    bool g_replaying = false;
    int g_replayEnd = 0;
    int g_tries = 0;
    bool g_pending = false;
    int g_pendingStep = 0;     // paso del snapshot al que volver
    int g_pendingDeath = 0;    // paso en el que moriste
    int g_curShift = 0;
    int g_rescues = 0;
    int g_ticksSaved = 0;
    int g_calls = 0;           // llamadas a processCommands en este fotograma
    double g_rate = 240.0;     // llamadas por segundo (se mide solo)
    std::deque<Snap> g_snaps;
    std::vector<Ev> g_events;
    std::vector<Ev> g_replay;

    int tol() {
        auto v = Mod::get()->getSettingValue<std::string>("stage");
        if (v == "Off") return 0;
        return std::atoi(v.c_str());
    }
    bool on() { return tol() > 0; }
    bool smooth() { return Mod::get()->getSettingValue<bool>("smooth"); }
    int stride() {
        int t = tol();
        return t >= 16 ? std::max(1, t / 8) : 1;
    }

    std::string labelText() {
        return fmt::format("Frame Assist: {} ticks | Rescates: {} ({} ticks)",
                           tol(), g_rescues, g_ticksSaved);
    }

    void updateLabel(bool flash) {
        auto pl = PlayLayer::get();
        if (!pl) return;
        auto lbl = typeinfo_cast<CCLabelBMFont*>(pl->getChildByID("frame-assist-label"));
        if (!lbl) return;
        lbl->setString(labelText().c_str());
        if (flash) {
            lbl->stopAllActions();
            lbl->setColor({90, 255, 90});
            lbl->setScale(0.4f);
            lbl->runAction(CCScaleTo::create(0.3f, 0.3f));
            lbl->runAction(CCTintTo::create(0.6f, 255, 255, 255));
        }
    }

    void clearAll() {
        for (auto& s : g_snaps) if (s.cp) s.cp->release();
        g_snaps.clear();
        g_events.clear();
        g_replay.clear();
        g_step = 0;
        g_tries = 0;
        g_replaying = false;
        g_pending = false;
    }

    // vuelve al snapshot pendiente; devuelve false si no existe
    bool doRewind(PlayLayer* pl) {
        CheckpointObject* target = nullptr;
        for (auto& s : g_snaps) if (s.step == g_pendingStep) target = s.cp;
        if (!target) return false;
        target->retain();
        pl->loadFromCheckpoint(target);
        while (!g_snaps.empty() && g_snaps.back().step >= g_pendingStep) {
            g_snaps.back().cp->release();
            g_snaps.pop_back();
        }
        target->release();
        g_step = g_pendingStep;
        g_replaying = true;
        return true;
    }
}

class $modify(FAGameLayer, GJBaseGameLayer) {
    // Inicio del fotograma: aqui es seguro rebobinar y, si "suave" esta activo,
    // sumamos al dt el tiempo de los ticks rebobinados para rejugarlos ya mismo.
    void update(float dt) {
        auto pl = PlayLayer::get();
        bool catchUp = false;
        if (pl && on() && g_pending && m_player1 && !m_player1->m_isDead && !pl->m_isPracticeMode) {
            g_pending = false;
            int span = g_pendingDeath - g_pendingStep;
            if (doRewind(pl) && smooth() && span > 0 && g_rate > 1.0) {
                dt += (float)(span / g_rate);
                catchUp = true;
            }
        }
        g_calls = 0;
        GJBaseGameLayer::update(dt);
        // medir cuantas llamadas por segundo hace el juego (no en el fotograma de recuperacion)
        if (!catchUp && dt > 0.f && g_calls > 0) {
            g_rate = g_rate * 0.95 + (g_calls / (double)dt) * 0.05;
        }
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        g_calls++;
        auto pl = PlayLayer::get();
        if (pl && on() && !pl->m_isPracticeMode && m_player1 && !m_player1->m_isDead) {
            if (g_replaying) {
                for (auto& e : g_replay)
                    if (e.step == g_step) GJBaseGameLayer::handleButton(e.down, 1, true);
                if (g_step > g_replayEnd) {
                    // sobreviviste: rescate conseguido
                    g_replaying = false;
                    g_rescues++;
                    g_ticksSaved += std::abs(g_curShift);
                    g_tries = 0;
                    updateLabel(true);
                }
            } else if (g_step % stride() == 0) {
                if (auto cp = pl->createCheckpoint()) {
                    cp->retain();
                    g_snaps.push_back({g_step, cp});
                    size_t maxSnaps = (size_t)((tol() * 2 + 4) / stride() + 2);
                    while (g_snaps.size() > maxSnaps) {
                        g_snaps.front().cp->release();
                        g_snaps.pop_front();
                    }
                }
            }
            g_step++;
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (on() && g_replaying && PlayLayer::get()) {
            return; // durante el reintento inyectamos nosotros el input
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

    void onQuit() {
        clearAll();
        PlayLayer::onQuit();
    }

    void levelComplete() {
        clearAll();
        PlayLayer::levelComplete();
    }

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        g_rescues = 0;
        g_ticksSaved = 0;
        // indicador siempre visible, una sola vez
        if (on() && !this->getChildByID("frame-assist-label")) {
            auto lbl = CCLabelBMFont::create(labelText().c_str(), "bigFont.fnt");
            lbl->setID("frame-assist-label");
            lbl->setScale(0.3f);
            lbl->setOpacity(170);
            lbl->setAnchorPoint({0.5f, 1.f});
            auto win = CCDirector::get()->getWinSize();
            lbl->setPosition({win.width / 2.f, win.height - 4.f});
            lbl->setZOrder(1000);
            this->addChild(lbl);
        }
    }

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        if (!on() || obj == m_anticheatSpike || m_isPracticeMode || g_snaps.empty()) {
            return PlayLayer::destroyPlayer(player, obj);
        }
        if (g_pending) return;

        int deathStep = g_step;
        const Ev* last = nullptr;
        for (auto& e : g_events)
            if (e.down && deathStep - e.step <= tol() * 2 + 2) last = &e;
        if (!last) return PlayLayer::destroyPlayer(player, obj);

        // candidatos: -1,+1,-2,+2,...
        int k = g_tries / 2 + 1;
        int shift = (g_tries % 2 == 0) ? -k : k;
        if (k > tol()) {
            clearAll();
            return PlayLayer::destroyPlayer(player, obj);
        }
        g_tries++;

        int newClick = last->step + shift;
        int from = std::min(newClick, last->step);

        // snapshot mas cercano en o antes de 'from'
        const Snap* best = nullptr;
        for (auto& s : g_snaps)
            if (s.step <= from && (!best || s.step > best->step)) best = &s;
        if (!best || newClick >= deathStep) {
            return PlayLayer::destroyPlayer(player, obj);
        }
        int snapStep = best->step;

        // replay: click desplazado + clicks reales desde el snapshot (menos el original)
        g_replay.clear();
        g_replay.push_back({newClick, true});
        for (auto& e : g_events) {
            if (&e == last) continue;
            if (e.step >= snapStep) g_replay.push_back(e);
        }

        g_curShift = shift;
        g_pendingStep = snapStep;
        g_pendingDeath = deathStep;
        g_replayEnd = deathStep + 2;
        g_pending = true;
        // no llamamos al destroyPlayer original: se rebobina al inicio del siguiente fotograma
    }
};
