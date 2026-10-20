// Frame Assist (v0.7, EXPERIMENTAL)
// Rebobinado diferido, intentos acotados sobre el mismo click original, y un rescate
// solo cuenta si sobrevives un rato despues del punto de la muerte.
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
    bool g_replaying = false;   // fase A: rejugando hasta el punto de la muerte
    bool g_watching = false;    // fase B: vigilando que sobrevives
    int g_replayEnd = 0;
    int g_tries = 0;
    bool g_chain = false;       // hay una cadena de intentos en curso
    int g_origClick = 0;        // paso del click original que se intenta corregir
    int g_origDeath = 0;        // paso de la muerte original
    bool g_pending = false;
    int g_pendingStep = 0;
    int g_curShift = 0;
    int g_rescues = 0;
    int g_ticksSaved = 0;
    int g_calls = 0;
    double g_rate = 240.0;
    std::deque<Snap> g_snaps;
    std::vector<Ev> g_events;   // clicks reales del jugador
    std::vector<Ev> g_replay;   // clicks a inyectar en la fase A

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
    int surviveTicks() { return std::max(24, tol() * 2 + 8); }

    // desplazamientos a probar, en orden: -1,+1,-2,+2,... (acotados)
    std::vector<int> candidates() {
        std::vector<int> mags;
        int t = tol();
        for (int m = 1; m <= std::min(t, 6); m++) mags.push_back(m);
        for (int m : {8, 12, 16, 24, 32, 50}) if (m <= t) mags.push_back(m);
        std::vector<int> out;
        for (int m : mags) { out.push_back(-m); out.push_back(m); }
        return out;
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
        g_chain = false;
        g_replaying = false;
        g_watching = false;
        g_pending = false;
    }

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
        g_watching = false;
        return true;
    }
}

class $modify(FAGameLayer, GJBaseGameLayer) {
    void update(float dt) {
        auto pl = PlayLayer::get();
        bool catchUp = false;
        if (pl && on() && g_pending && m_player1 && !m_player1->m_isDead && !pl->m_isPracticeMode) {
            g_pending = false;
            int span = g_origDeath - g_pendingStep;
            if (doRewind(pl) && smooth() && span > 0 && g_rate > 1.0) {
                float extra = (float)(span / g_rate);
                if (extra <= 0.3f) {
                    dt += extra;
                    catchUp = true;
                }
            }
        }
        g_calls = 0;
        GJBaseGameLayer::update(dt);
        if (!catchUp && dt > 0.f && g_calls > 0) {
            g_rate = g_rate * 0.95 + (g_calls / (double)dt) * 0.05;
        }
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        g_calls++;
        auto pl = PlayLayer::get();
        if (pl && on() && !pl->m_isPracticeMode && m_player1 && !m_player1->m_isDead) {
            if (g_replaying) {
                // fase A: inyectar el click desplazado hasta llegar al punto de la muerte
                for (auto& e : g_replay)
                    if (e.step == g_step) GJBaseGameLayer::handleButton(e.down, 1, true);
                if (g_step >= g_origDeath) {
                    g_replaying = false;
                    g_watching = true;
                }
            } else {
                if (g_step % stride() == 0) {
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
                // fase B: si sobrevives suficiente tiempo, el rescate cuenta
                if (g_watching && g_step > g_replayEnd) {
                    g_watching = false;
                    g_chain = false;
                    g_tries = 0;
                    g_rescues++;
                    g_ticksSaved += std::abs(g_curShift);
                    updateLabel(true);
                    log::info("Frame Assist: rescate #{} (+{} ticks, total {} ticks)",
                              g_rescues, std::abs(g_curShift), g_ticksSaved);
                }
            }
            g_step++;
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        // solo ignoramos el input real durante el rejugado (fase A)
        if (on() && g_replaying && PlayLayer::get()) {
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
        int clickStep;
        if (g_chain) {
            clickStep = g_origClick;   // seguimos con el mismo click original
        } else {
            const Ev* last = nullptr;
            for (auto& e : g_events)
                if (e.down && deathStep - e.step <= tol() * 2 + 2) last = &e;
            if (!last) return PlayLayer::destroyPlayer(player, obj);
            clickStep = last->step;
            g_origClick = clickStep;
            g_origDeath = deathStep;
            g_chain = true;
            g_tries = 0;
        }

        // elegir el siguiente desplazamiento que tenga snapshot disponible
        auto cands = candidates();
        bool ok = false;
        int newClick = 0, snapStep = 0;
        while (g_tries < (int)cands.size()) {
            int shift = cands[g_tries++];
            newClick = clickStep + shift;
            if (newClick >= g_origDeath) continue;
            int from = std::min(newClick, clickStep);
            const Snap* best = nullptr;
            for (auto& s : g_snaps)
                if (s.step <= from && (!best || s.step > best->step)) best = &s;
            if (!best) continue;
            snapStep = best->step;
            g_curShift = shift;
            ok = true;
            break;
        }
        if (!ok) {
            // ningun desplazamiento sirve: mueres de verdad
            clearAll();
            return PlayLayer::destroyPlayer(player, obj);
        }

        // replay: click desplazado + clicks reales desde el snapshot (sin el original)
        g_replay.clear();
        g_replay.push_back({newClick, true});
        for (auto& e : g_events) {
            if (e.down && e.step == clickStep) continue;
            if (e.step >= snapStep) g_replay.push_back(e);
        }

        g_pendingStep = snapStep;
        g_replayEnd = g_origDeath + surviveTicks();
        g_pending = true;
        // no llamamos al destroyPlayer original: se rebobina al inicio del siguiente fotograma
    }
};
