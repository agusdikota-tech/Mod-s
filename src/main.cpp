// Frame Assist (Stable + Show/Hide HUD + Android NDK Fix)
#ifndef FMT_ENFORCE_COMPILE_STRING
#define FMT_ENFORCE_COMPILE_STRING 0
#endif
#define FMT_NO_FMT_STRING

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
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
    int g_curShift = 0;
    int g_rescues = 0;
    int g_ticksSaved = 0;
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

            auto lbl = CCLabelBMFont::create("Frame Assist Active", "bigFont.fnt");
            lbl->setID("frame-assist-label");
            lbl->setScale(0.3f);
            lbl->setOpacity(170);
            lbl->setAnchorPoint({0.5f, 1.f});
            lbl->setPosition({win.width / 2.f, win.height - 4.f});
            lbl->setZOrder(1000);
            lbl->setVisible(showHud());
            this->addChild(lbl);
        }
    }

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        if (!on() || obj == m_anticheatSp
            
