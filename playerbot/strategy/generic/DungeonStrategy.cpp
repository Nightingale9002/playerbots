
#include "playerbot/playerbot.h"
#include "DungeonStrategy.h"

using namespace ai;

void DungeonStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{
    // Add this combat triggers in case the bot gets summoned into the dungeon and goes straight into combat
    triggers.push_back(new TriggerNode(
        "enter onyxias lair",
        NextAction::array(0, new NextAction("enable onyxias lair strategy", 100.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "enter molten core",
        NextAction::array(0, new NextAction("enable molten core strategy", 100.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "enter blackwing lair",
        NextAction::array(0, new NextAction("enable blackwing lair strategy", 100.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "enter karazhan",
        NextAction::array(0, new NextAction("enable karazhan strategy", 100.0f), NULL)));
    
    triggers.push_back(new TriggerNode(
        "enter mechanar",
        NextAction::array(0, new NextAction("enable mechanar strategy", 100.0f), NULL)));
}

// [dodge 2026-10-17 方案A] 通用「躲技能」。
//
// 链路（全用既有零件，不新造动作）：
//   `hostile aoe incoming`（触发器）→ 触发时把敌方地面 AOE 登记进 `stored hazards`
//   → `hazards` 有值 → `move away from hazard`（`DungeonActions.h → MoveAwayFromHazard`，成熟寻路）把 bot 挪出圈。
//
// 为什么原来躲不掉：`HazardsValue::Calculate()` 只读 `stored hazards`、自己不检测，
//   而 `stored hazards` 原先只有 `DungeonTriggers.cpp → CloseToHazardTrigger`（按**预置 BOSS 危险物 GUID**）
//   会写 —— 通用五人本里那个列表恒为空 ⇒ `MoveAwayFromHazard` 永远没东西可躲。
//   现在把"敌方动态对象（地板 AOE）"接进同一条链，于是所有副本自动具备躲地板能力，
//   不必逐个 BOSS 配 hazard 列表。
//
// 优先级用 `ACTION_EMERGENCY + 5`，与 `AvoidAoeStrategy`（"身上已挂 area debuff 就跑"）同基准：
//   本触发器管"**即将**踩到"，那个管"已经中招"，两者是同一紧急度的一前一后。
void DungeonStrategy::InitReactionTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "hostile aoe incoming",
        NextAction::array(0, new NextAction("move away from hazard", ACTION_EMERGENCY + 5), NULL)));

    // [跑位 2026-10-19 站长口径] **跑位是 BOSS 战第一优先级**：
    //   "进入 boss 战后首先应该移动到安全位置（不会引新的怪的位置），按照策略站好队形，然后才是其他的"。
    //   ⇒ 比紧急躲圈（ACTION_EMERGENCY+5）和所有自定义策略权重（最高 99）都高；
    //     到槽位（1.5 码内）后动作返回 false，自动恢复其它动作（加血/驱散/输出）。
    triggers.push_back(new TriggerNode(
        "party position active",
        NextAction::array(0, new NextAction("move to party slot", ACTION_PASSTROUGH + 20), NULL)));

    // [BOSS 战策略 2026-10-18 站长口径] 进攻驱散：敌人身上的魔法增益（护盾/增益/回血）优先驱掉
    triggers.push_back(new TriggerNode(
        "enemy has dispellable buff",
        NextAction::array(0, new NextAction("dispel enemy", ACTION_DISPEL + 5), NULL)));
}

void DungeonStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "enter onyxias lair",
        NextAction::array(0, new NextAction("enable onyxias lair strategy", 100.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "leave onyxias lair",
        NextAction::array(0, new NextAction("disable onyxias lair strategy", 100.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "enter molten core",
        NextAction::array(0, new NextAction("enable molten core strategy", 100.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "leave molten core",
        NextAction::array(0, new NextAction("disable molten core strategy", 100.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "enter blackwing lair",
        NextAction::array(0, new NextAction("enable blackwing lair strategy", 100.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "leave blackwing lair",
        NextAction::array(0, new NextAction("disable blackwing lair strategy", 100.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "enter karazhan",
        NextAction::array(0, new NextAction("enable karazhan strategy", 100.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "leave karazhan",
        NextAction::array(0, new NextAction("disable karazhan strategy", 100.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "enter mechanar",
        NextAction::array(0, new NextAction("enable mechanar strategy", 100.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "leave mechanar",
        NextAction::array(0, new NextAction("disable mechanar strategy", 100.0f), NULL)));
}