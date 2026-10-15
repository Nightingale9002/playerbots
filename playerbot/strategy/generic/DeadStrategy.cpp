
#include "playerbot/playerbot.h"
#include "playerbot/strategy/Strategy.h"
#include "playerbot/strategy/values/DeadValues.h"
#include "DeadStrategy.h"

using namespace ai;

void DeadStrategy::InitDeadTriggers(std::list<TriggerNode*>& triggers)
{
    PassTroughStrategy::InitDeadTriggers(triggers);

    // [corpse-run 2026-10-18 站长口径] **副本/团本里不要自动释放灵魂**（站长："取消 bot 副本里自动释放尸体"）：
    //   副本里死了就原地躺着等真人复活，不自动变鬼、不自动跑尸、不去找天使姐姐。
    //   保留的例外（否则会变成"没人管的躺尸"）：
    //     · 野外 / 战场 / 竞技场：行为不变（原本就不受这条影响）；
    //     · 副本里"连续死了太多次"（`dead strategy` 里那条 `repop` 逃生线）仍然保留。
    //   实现：这两条副本动作外加 `val::not::in dungeon`（值 "in dungeon" 见 ValueContext）。
    triggers.push_back(new TriggerNode(
        "val::and::{very often,not::in dungeon}",
        NextAction::array(0, new NextAction("auto release", relevance), NULL)));

    // [corpse-run 2026-10-18 站长口径] 这条 `repop`（连续死太多次的逃生线）在副本里**也关掉**：
    //   站长要的是"副本里死了就躺着等复活"，不自动释放（释放灵魂/传送墓地都算走尸）。
    triggers.push_back(new TriggerNode(
        "val::and::{val::gt32::{death count," + std::to_string(DeadValueConstants::DEATH_COUNT_BEFORE_EVAC) + "},not::in dungeon}",
        NextAction::array(0, new NextAction("repop", relevance + 4.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "very often",
        NextAction::array(0, new NextAction("self resurrect", relevance + 3.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "val::should spirit healer",
        NextAction::array(0, new NextAction("spirit healer", relevance + 2.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "dead",
       NextAction::array(0, new NextAction("find corpse", relevance), NULL)));

    triggers.push_back(new TriggerNode(
        "corpse near",
        NextAction::array(0, new NextAction("revive from corpse", relevance-1.0f), NULL)));

    // The bot's own recovery (auto release / spirit healer / repop, relevance 100-104) must not win
    // the tick over an offered resurrection: the engine runs one action per tick, and a request that
    // keeps losing is never answered.
    triggers.push_back(new TriggerNode(
        "resurrect request",
        NextAction::array(0, new NextAction("accept resurrect", relevance + 10.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "falling far",
        NextAction::array(0, new NextAction("repop", relevance+1), NULL)));

    triggers.push_back(new TriggerNode(
        "move long stuck",
        NextAction::array(0, new NextAction("repop", relevance+1), NULL)));
}
