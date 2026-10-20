// Frame Assist (v0.9.5 - Android NDK FMT Macro Fix)
#define FMT_HEADER_ONLY

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <deque>
#include <vector>
#include <utility>
#include <algorithm>
#include <cstdlib>
#include <string>

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
    int g_curShift = 0;
    int g_rescues = 0;
    int g_ticksSaved = 0;
    std::string g_dbg = "esperando";
    std::deque<Snap> g_snaps;
    std::deque<std::pair<int, bool>> g_ground;
    std::vector<Ev> g_events;
    std::vector<Ev> g_replay;

    // --- Lecturas de los Ajustes de Geode ---
    int tol() {
        auto v = Mod::get()->getSettingValue<std::string>("stage");
        if (v == "Off") return 0;
        return std::atoi(v.c_str());
    }
    bool on() { return tol() > 0; }

    int maxTriesSetting() {
        return (int)Mod::get()->getSettingValue<int64_t>("max-tries");
    }

    bool allowShip() { return Mod::get()->getSettingValue<bool>("allow-ship"); }
    bool allowBall() { return Mod::get()->getSettingValue<bool>("allow-ball"); }
    bool allowUfo()  { return Mod::get()->getSettingValue<bool>("allow-ufo"); }
    bool allowWave() { return Mod::get()->getSettingValue<bool>("allow-wave"); }
    bool allowRobot() { return Mod::get()->getSettingValue<bool>("allow-robot"); }
    bool allowSpider() { return Mod::get()->getSettingValue<bool>("allow-spider"); }
    bool allowSwing() { return Mod::get()->getSettingValue<bool>("allow-swing"); }
    bool groundOnly() { return Mod::get()->getSettingValue<bool>("ground-only"); }
    bool showHud()    { return Mod::get()->getSettingValue<bool>("show-hud"); }

    int stride() {
        int t = tol();
        return t >= 16 ? std::max(1, t / 12) : 1;
    }
    int surviveTicks() { return std::max(120, tol() * 4); }

    std::vector<int> candidates() {
        std::vector<int> mags;
        int t = tol();
        for (int m = 1; m <= t; m++) {
            mags.push_back(m);
        }
        std::vector<int> out;
        for (int m : mags) { out.push_back(-m); out.push_back(m); }
        return out;
    }

    bool groundAt(int step) {
        for (auto& g : g_ground) if (g.first == step) return g.second;
        return true;
    }

    bool modeAllowed(PlayerObject* p) {
        if (!p) return true;
        if (p->m_isShip && !allowShip()) return false;
        if (p->m_isBall && !allowBall()) return false;
        if (p->m_isBird && !allowUfo()) return false;
        if (p->m_isDart && !allowWave()) return false;
        if (p->m_isRobot && !allowRobot()) return false;
        if (p->m_isSpider && !allowSpider()) return false;
        if (p->m_isSwing && !allowSwing()) return false;
        return true;
    }

    std::string labelText() {
        std::string s = "Frame Assist: ";
        s.append(std::to_string(tol()));
        s.append(" ticks | Rescates: ");
        s.append(std::to_string(g_rescues));
        s.append(" (");
        s.append(std::to_string(g_ticksSaved));
        s.append(" ticks)");
        return s;
    }

    void setDbg(const std::string& s) {
        g_dbg = s;
        auto pl = PlayLayer::get();
        if (!pl) return;
        if (auto lbl = typeinfo_cast<CCLabelBMFont*>(pl->getChildByID("frame-assist-dbg"))) {
            lbl->setString(showHud() ? s.c_str() : "");
        }
    }

    void updateLabel(bool flash) {
        auto pl = PlayLayer::get();
        if (!pl) return;
        auto lbl = typeinfo_cast<CCLabelBMFont*>(pl->getChildByID("frame-assist-label"));
        if (!lbl) return;
        lbl->setString(showHud() ? labelText().c_str() : "");
        if (flash && showHud()) {
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
    }
}

class $modify(FAGameLayer, GJBaseGameLayer) {
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto pl = PlayLayer::get();
        if (pl && on() && !pl->m_isPracticeMode && m_player1) {
            g_ground.push_back({g_step, (bool)m_player1->m_isOnGround});
            while (g_ground.size() > 600) g_ground.pop_front();

            if (g_replaying) {
                for (auto& e : g_replay)
                    if (e.step == g_step) GJBaseGameLayer::handleButton(e.down, 1, true);

                if (m_player1) {
                    m_player1->m_isDead = false;
                    m_player1->m_isDashing = false;
                }
                if (m_player2) {
                    m_player2->m_isDead = false;
                    m_player2->m_isDashing = false;
                }

                if (g_step >= g_origDeath) {
                    g_replaying = false;
                    g_watching = true;
                    setDbg("sobrevives... vigilando");
                }
            } else {
                if (g_step == 0 || g_step % stride() == 0) {
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

            auto lbl = CCLabelBMFont::create(showHud() ? labelText().c_str() : "", "bigFont.fnt");
            lbl->setID("frame-assist-label");
            lbl->setScale(0.3f);
            lbl->setOpacity(170);
            lbl->setAnchorPoint({0.5f, 1.f});
            lbl->setPosition({win.width / 2.f, win.height - 4.f});
            lbl->setZOrder(1000);
            this->addChild(lbl);

            auto dbg = CCLabelBMFont::create(showHud() ? g_dbg.c_str() : "", "bigFont.fnt");
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

        if (player && !modeAllowed(player)) {
            return PlayLayer::destroyPlayer(player, obj);
        }

        if (g_replaying && g_step < g_origDeath) {
            return;
        }

        if (g_snaps.empty()) {
            setDbg("sin snapshots guardados");
            return PlayLayer::destroyPlayer(player, obj);
        }

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
                if (deathStep - e.step <= 120) last = &e;
            }
            if (!last) {
                std::string errNear = "muerte sin click cercano (prox: ";
                errNear.append(std::to_string(nearest));
                errNear.append(")");
                setDbg(errNear);
                return PlayLayer::destroyPlayer(player, obj);
            }
            clickStep = last->step;
            g_origClick = clickStep;
            g_origDeath = deathStep;
            g_chain = true;
            g_tries = 0;
            g_useGround = groundOnly() || (m_player1 && !(m_player1->m_isShip || m_player1->m_isBird || m_player1->m_isDart || m_player1->m_isSwing) && groundAt(clickStep));
        }

        auto cands = candidates();
        int maxAllowedTries = maxTriesSetting();
        bool ok = false;
        int newClick = 0, snapStep = 0;

        while (g_tries < (int)cands.size() && g_tries < maxAllowedTries) {
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

        std::string dbgTry = "intento ";
        dbgTry.append(std::to_string(g_tries));
        dbgTry.append("/");
        dbgTry.append(std::to_string((int)cands.size()));
        dbgTry.append(": click ");
        if (g_curShift > 0) dbgTry.append("+");
        dbgTry.append(std::to_string(g_curShift));
        dbgTry.append(" ticks");
        setDbg(dbgTry);

        int origUp = -1, nextDown = -1;
        for (auto& e : g_events) {
            if (e.step <= clickStep) continue;
            if (!e.down && origUp < 0) origUp = e.step;
            if (e.down && nextDown < 0) nextDown = e.step;
        }

        CheckpointObject* target = nullptr;
        for (auto& s : g_snaps) if (s.step == snapStep) target = s.cp;
        if (!target) {
            setDbg("fallo: checkpoint no encontrado");
            return PlayLayer::destroyPlayer(player, obj);
        }

        target->retain();
        this->loadFromCheckpoint(target);
        
        while (!g_snaps.empty() && g_snaps.back().step >= snapStep) {
            g_snaps.back().cp->release();
            g_snaps.pop_back();
        }
        while (!g_ground.empty() && g_ground.back().first >= snapStep) g_ground.pop_back();
        target->release();

        g_step = snapStep;
        g_replaying = true;
        g_watching = false;

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

        g_replayEnd = g_origDeath + surviveTicks();

        if (m_player1) {
            m_player1->m_isDead = false;
            m_player1->m_isDashing = false;
            m_player1->setVisible(true);
        }
        if (m_player2) {
            m_player2->m_isDead = false;
            m_player2->m_isDashing = false;
            m_player2->setVisible(true);
        }
    }
};

class $modify(FAPlayerObject, PlayerObject) {
    bool collidedWithObject(float dt, GameObject* obj) {
        if (on() && g_replaying) {
            return PlayerObject::collidedWithObject(dt, obj);
        }
        return PlayerObject::collidedWithObject(dt, obj);
    }
};
