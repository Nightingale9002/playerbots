
#include "playerbot/playerbot.h"
#include "DungeonStrategy.h"
#include "playerbot/strategy/actions/GenericSpellActions.h"
#include "playerbot/strategy/values/AoeValues.h"

using namespace ai;

void DungeonStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{
    // [dodge 2026-10-18 站长口径] **任何情况优先躲地板**：combat 引擎也挂一份
    //   （原来只有 reaction 引擎有，combat 的读条/走位会把它顶掉 ⇒ 来回冲突）。
    triggers.push_back(new TriggerNode(
        "hostile aoe incoming",
        NextAction::array(0, new NextAction("move away from hazard", ACTION_EMERGENCY + 10), NULL)));

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

// [dodge 2026-10-18 站长口径] 躲地板期间压制其它一切动作（见 DungeonStrategy.h 的说明）。
float AoeDodgePriorityMultiplier::GetValue(Action* action)
{
    if (!action)
        return 1.0f;

    if (!AI_VALUE(bool, "hostile aoe incoming") && !AI_VALUE2(bool, "has area debuff", "self target"))
        return 1.0f;

    const std::string name = action->getName();

    // [dodge 2026-10-18 站长口径] 躲地板最高优先、不豁免；例外只有"普通伤害地板 + 有人危急血"时先读完治疗。
    bool const lethalAoe = HostileAoeValue::IsLethalIncoming(ai);
    if (!lethalAoe && AI_VALUE(bool, "party member critical health") &&
        (dynamic_cast<CastHealingSpellAction*>(action) || name == "heal"))
        return 1.0f;

    if (name == "move away from hazard" || name == "move away from magmadar lava bomb" ||
        name == "set facing" || name == "select new target" || name == "stop moving" ||
        name == "wait for attack keep safe distance")
        return 1.0f;

    return 0.0f;
}

void DungeonStrategy::InitCombatMultipliers(std::list<Multiplier*>& multipliers)
{
    multipliers.push_back(new AoeDodgePriorityMultiplier(ai));
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
}

void DungeonStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{
    // [tank-pull 2026-10-18 站长口径] **坦克在副本里自己开怪**：没目标、脱战、看得见可拉的怪 ⇒ 自己去拉。
    //   不挂在 `grind` 策略下（那样只有野外刷怪的 bot 才有这条线），这里对所有职业的"坦克"都生效；
    //   非坦克不受影响（触发器里已限定 `IsTank`），非坦克仍由 tank assist / wait for attack 管。
    //   优先级 50：高于走路/跟随，低于战斗急救与打断。
    triggers.push_back(new TriggerNode(
        "dungeon tank pull",
        NextAction::array(0, new NextAction("attack anything", 50.0f), NULL)));
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