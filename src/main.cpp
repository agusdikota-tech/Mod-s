// Frame Assist (v0.8.4, EXPERIMENTAL)
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <deque>
#include <vector>
#include <utility>
#include <algorithm>
#include <cstdlib>

using namespace geode::prelude;

struct Ev { int step; bool down; };
struct Snap { int step; CheckpointObject* cp; };

namespace {
    int g_step = 0;
    bool g_replaying = false;
    bool g_watching = false;
    int g_replayEnd = 0;
    int g_tries = 0;
    bool g_chain = false;
    bool g_useGround = false;
    int g_origClick = 0;
    int g_origDeath = 0;
    bool g_pending = false;
    int g_pendingStep = 0;
    int g_curShift = 0;
    int g_rescues = 0;
    int g_ticksSaved = 0;
    std::string g_dbg = "esperando";
    std::deque<Snap> g_snaps;
    std::deque<std::pair<int, bool>> g_ground;
    std::vector<Ev> g_events;
    std::vector<Ev> g_replay;

    int tol() {
        auto v = Mod::get()->getSettingValue<std::string>("stage");
        if (v == "Off") return 0;
        return std::atoi(v.c_str());
    }
    bool on() { return tol() > 0; }
    int stride() {
        int t = tol();
        return t >= 16 ? std::max(1, t / 8) : 1;
    }
    int surviveTicks() { return std::max(120, tol() * 4); }

    std::vector<int> candidates() {
        std::vector<int> mags;
        int t = tol();
        for (int m = 1; m <= std::min(t, 6); m++) mags.push_back(m);
        for (int m : {8, 12, 16, 24, 32, 50}) if (m <= t) mags.push_back(m);
        std::vector<int> out;
        for (int m : mags) { out.push_back(-m); out.push_back(m); }
        return out;
    }

    bool groundAt(int step) {
        for (auto& g : g_ground) if (g.first == step) return g.second;
        return true;
    }
    bool groundMode(PlayerObject* p) {
        return !(p->m_isShip || p->m_isBird || p->m_isDart || p->m_isSwing);
    }

    std::string labelText() {
        return fmt::format("Frame Assist: {} ticks | Rescates: {} ({} ticks)",
                           tol(), g_rescues, g_ticksSaved);
    }

    void setDbg(const std::string& s) {
        g_dbg = s;
        auto pl = PlayLayer::get();
        if (!pl) return;
        if (auto lbl = typeinfo_cast<CCLabelBMFont*>(pl->getChildByID("frame-assist-dbg")))
            lbl->setString(s.c_str());
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
        g_ground.clear();
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
        while (!g_ground.empty() && g_ground.back().first >= g_pendingStep) g_ground.pop_back();
        target->release();
        g_step = g_pendingStep;
        g_replaying = true;
        g_watching = false;

        // Limpiar estado de muerte para evitar conflictos con las físicas
        if (pl->m_player1) pl->m_player1->m_isDead = false;
        if (pl->m_player2) pl->m_player2->m_isDead = false;

        return true;
    }
}

class $modify(FAGameLayer, GJBaseGameLayer) {
    void update(float dt) {
        auto pl = PlayLayer::get();
        if (pl && on() && g_pending && m_player1 && !m_player1->m_isDead && !pl->m_isPracticeMode) {
            g_pending = false;
            if (!doRewind(pl)) setDbg("fallo: no habia snapshot para volver");
        }
        GJBaseGameLayer::update(dt);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto pl = PlayLayer::get();
        if (pl && on() && !pl->m_isPracticeMode && m_player1 && !m_player1->m_isDead) {
            g_ground.push_back({g_step, (bool)m_player1->m_isOnGround});
            while (g_ground.size() > 600) g_ground.pop_front();

            if (g_replaying) {
                for (auto& e : g_replay)
                    if (e.step == g_step) GJBaseGameLayer::handleButton(e.down, 1, true);

                if (m_player1) m_player1->m_isDead = false;

                if (g_step >= g_origDeath) {
                    g_replaying = false;
                    g_watching = true;
                    setDbg("sobrevives... vigilando");
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
                if (g_watching && g_step > g_replayEnd) {
                    g_watching = false;
                    g_chain = false;
                    g_tries = 0;
                    g_rescues++;
                    g_ticksSaved += std::abs(g_curShift);
                    updateLabel(true);
                    setDbg("rescate!");
                }
            }
            g_step++;
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (on() && button == 1 && isPlayer1 && g_replaying && PlayLayer::get()) {
            g_replay.push_back({g_origDeath, down});
            g_events.push_back({g_origDeath, down});
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
            auto win = CCDirector::get()->getWinSize();

            auto lbl = CCLabelBMFont::create(labelText().c_str(), "bigFont.fnt");
            lbl->setID("frame-assist-label");
            lbl->setScale(0.3f);
            lbl->setOpacity(170);
            lbl->setAnchorPoint({0.5f, 1.f});
            lbl->setPosition({win.width / 2.f, win.height - 4.f});
            lbl->setZOrder(1000);
            this->addChild(lbl);

            auto dbg = CCLabelBMFont::create(g_dbg.c_str(), "bigFont.fnt");
            dbg->setID("frame-assist-dbg");
            dbg->setScale(0.25f);
            dbg->setOpacity(200);
            dbg->setColor({255, 220, 90});
            dbg->setAnchorPoint({0.5f, 1.f});
            dbg->setPosition({win.width / 2.f, win.height - 18.f});
            dbg->setZOrder(1000);
            this->addChild(dbg);
        }
    }

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        if (!on() || obj == m_anticheatSpike || m_isPracticeMode) {
            return PlayLayer::destroyPlayer(player, obj);
        }
        if (g_snaps.empty()) {
            setDbg("sin snapshots guardados");
            return PlayLayer::destroyPlayer(player, obj);
        }
        if (g_pending) return;

        int deathStep = g_step;
        int clickStep;
        if (g_chain) {
            clickStep = g_origClick;
        } else {
            const Ev* last = nullptr;
            int nearest = 99999;
            for (auto& e : g_events) {
                if (!e.down) continue;
                nearest = std::min(nearest, deathStep - e.step);
                if (deathStep - e.step <= tol() * 2 + 2) last = &e;
            }
            if (!last) {
                setDbg("muerte sin click cercano");
                return PlayLayer::destroyPlayer(player, obj);
            }
            clickStep = last->step;
            g_origClick = clickStep;
            g_origDeath = deathStep;
            g_chain = true;
            g_tries = 0;
            g_useGround = m_player1 && groundMode(m_player1) && groundAt(clickStep);
        }

        auto cands = candidates();
        bool ok = false;
        int newClick = 0, snapStep = 0;
        while (g_tries < (int)cands.size()) {
            int shift = cands[g_tries++];
            newClick = clickStep + shift;
            if (newClick >= g_origDeath) continue;
            if (g_useGround && !groundAt(newClick)) continue;
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
            setDbg("sin solucion tras intentos");
            clearAll();
            return PlayLayer::destroyPlayer(player, obj);
        }

        setDbg(fmt::format("intento {}/{}: click {:+} ticks", g_tries, (int)cands.size(), g_curShift));

        int origUp = -1, nextDown = -1;
        for (auto& e : g_events) {
            if (e.step <= clickStep) continue;
            if (!e.down && origUp < 0) origUp = e.step;
            if (e.down && nextDown < 0) nextDown = e.step;
        }

        g_replay.clear();
        g_replay.push_back({newClick, true});
        if (origUp >= 0) {
            int newUp = origUp + g_curShift;
            if (nextDown >= 0 && newUp >= nextDown) newUp = nextDown - 1;
            if (newUp <= newClick) newUp = newClick + 1;
            g_replay.push_back({newUp, false});
        }
        for (auto& e : g_events) {
            if (e.step < snapStep) continue;
            if (e.down && e.step == clickStep) continue;
            if (!e.down && e.step == origUp) continue;
            g_replay.push_back(e);
        }

        g_pendingStep = snapStep;
        g_replayEnd = g_origDeath + surviveTicks();
        g_pending = true;
    }
};
