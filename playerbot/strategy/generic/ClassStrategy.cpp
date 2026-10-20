
#include "playerbot/playerbot.h"
#include "ClassStrategy.h"
#include "playerbot/DecisionWeights.h"

#include <algorithm>
#include <set>
#include <string>

using namespace ai;

// ===========================================================================
// [2026-10-20 站长口径] 控制技能的优先级**按战斗情况动态调整**。
//   站长原话："（变羊/放逐这类控制的优先级）应该能根据战斗情况动态调整"。
//
// 为什么用 multiplier 而不是改各职业策略里的固定 relevance：
//   ① 固定值只能一刀切；同一个 bot 在"单怪"和"5 只怪围上来"时该不该控完全不同；
//   ② multiplier 每 tick 参与 action 选优 ⇒ 改 `ai_playerbot_decision_weight` 表即可生效，
//      不用重编译/重启（符合 2026-10-18 "决策参数化" 的口径）。
//      表缺行 ⇒ 用 DecisionWeights.h 里的默认值。
//
// 判据：
//   先看**人工标记**：`.bot rti` 的 cc 目标、星星标记（index 0 = 控制标记）⇒ **不降级**，
//     保持策略里写的原优先级（人工指令随时生效，压过自动判断）；
//   否则按场面压力给**绝对优先级**：
//     base + per_enemy * (同时交战的怪数 - 1) + hurt_ally * (队内血量 < hurt_health% 的成员数)
//   再夹到 [1, max] ⇒ 单怪/队伍安全时排在普通输出后面（基本不控）；
//     多怪同时打、队里有人快死时抬到打断/驱散附近（积极控场）。
//
// 返回值是**倍率**：Engine.cpp 里是 `relevance *= multiplier->GetValue(action)`，
//   所以返回 target/current 把 relevance 精确设成 target。多次注册也幂等
//   （第二次调用时 current == target ⇒ 返回 1.0）。
// ===========================================================================
namespace
{
    // 参与动态调整的控制动作（= 各职业 cc 策略里的 NextAction 名）。
    //   注意：hunter 的冰冻陷阱**不在**名单里 —— HunterStrategy 的注释明确要求陷阱必须
    //   >= ACTION_INTERRUPT（陷阱有前后置移动/连招），降级会破坏它的动作链。
    bool IsDynamicCcAction(std::string const& name)
    {
        static std::set<std::string> const names =
        {
            "polymorph",
            "banish on cc",
            "fear on cc",
            "hibernate on cc",
            "entangling roots",
            "sap",
            "turn undead",
            "scare beast on cc"
        };
        return names.find(name) != names.end();
    }

    // 人工控制标记：rti 的 cc 目标 / 星星标记（index 0）
    bool HasManualCcMark(PlayerbotAI* ai, Player* bot)
    {
        if (Value<ObjectGuid>* ccMark = ai->GetAiObjectContext()->GetValue<ObjectGuid>("rti cc target"))
        {
            if (!ccMark->Get().IsEmpty())
                return true;
        }

        if (bot)
        {
            if (Group* group = bot->GetGroup())
            {
                if (!group->GetTargetIcon(0).IsEmpty())
                    return true;
            }
        }

        return false;
    }

    uint32 CountHurtMembers(Player* bot, float healthPct)
    {
        if (!bot)
            return 0;

        uint32 count = 0;
        if (bot->IsAlive() && bot->GetHealthPercent() < healthPct)
            ++count;

        Group* group = bot->GetGroup();
        if (!group)
            return count;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->getSource();
            if (!member || member == bot || !member->IsInWorld() || !member->IsAlive())
                continue;

            if (member->GetHealthPercent() < healthPct)
                ++count;
        }

        return count;
    }

    uint32 CountEngagedEnemies(PlayerbotAI* ai)
    {
        if (Value<uint8>* count = ai->GetAiObjectContext()->GetValue<uint8>("possible attack targets count"))
            return count->Get();

        return 0;
    }
}

class CcRelevanceMultiplier : public Multiplier
{
public:
    CcRelevanceMultiplier(PlayerbotAI* ai) : Multiplier(ai, "cc relevance") {}

    float GetValue(Action* action) override
    {
        if (!action || !IsDynamicCcAction(action->getName()))
            return 1.0f;

        float const current = action->getRelevance();
        if (current <= 0.01f)
            return 1.0f;

        Player* bot = ai->GetBot();
        if (!bot)
            return 1.0f;

        // 人工标记 ⇒ 保持原优先级（"随时生效"）
        if (HasManualCcMark(ai, bot))
            return 1.0f;

        uint32 const enemies = CountEngagedEnemies(ai);
        uint32 const hurt = CountHurtMembers(bot, dw::Get("cc.dyn.hurt_health"));

        float target = dw::Get("cc.dyn.base")
                     + dw::Get("cc.dyn.per_enemy") * float(enemies > 0 ? enemies - 1 : 0)
                     + dw::Get("cc.dyn.hurt_ally") * float(hurt);

        float const cap = dw::Get("cc.dyn.max");
        if (cap > 0.0f)
            target = std::min(target, cap);

        target = std::max(target, 1.0f);

        return target / current;
    }
};

void CcStrategy::InitCombatMultipliers(std::list<Multiplier*>& multipliers)
{
    multipliers.push_back(new CcRelevanceMultiplier(ai));
}


#ifdef MANGOSBOT_ZERO // Vanilla

void ClassStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "invalid target",
        NextAction::array(0, new NextAction("select new target", ACTION_EMERGENCY), NULL)));

    triggers.push_back(new TriggerNode(
        "mounted",
        NextAction::array(0, new NextAction("check mount state", ACTION_EMERGENCY), NULL)));

    triggers.push_back(new TriggerNode(
        "very often",
        NextAction::array(0, new NextAction("use lightwell", ACTION_LIGHT_HEAL), NULL)));

    triggers.push_back(new TriggerNode(
        "combat long stuck",
        NextAction::array(0, new NextAction("unstuck", ACTION_IDLE), NULL)));

    triggers.push_back(new TriggerNode(
        "combat stuck",
        NextAction::array(0, new NextAction("unstuck", ACTION_IDLE), NULL)));
}

void ClassStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{
    /*
    triggers.push_back(new TriggerNode(
        "vehicle near",
        NextAction::array(0, new NextAction("enter vehicle", 10.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "very often",
        NextAction::array(0, new NextAction("use lightwell", 80.0f), NULL)));
    */

    triggers.push_back(new TriggerNode(
        "very often",
        NextAction::array(0, new NextAction("check mount state", ACTION_IDLE), new NextAction("check values", ACTION_IDLE), NULL)));

    triggers.push_back(new TriggerNode(
        "near dark portal",
        NextAction::array(0, new NextAction("move to dark portal", ACTION_IDLE), NULL)));

    triggers.push_back(new TriggerNode(
        "at dark portal azeroth",
        NextAction::array(0, new NextAction("use dark portal azeroth", ACTION_IDLE), NULL)));

    triggers.push_back(new TriggerNode(
        "at dark portal outland",
        NextAction::array(0, new NextAction("move from dark portal", ACTION_IDLE), NULL)));
}

void ClassStrategy::InitReactionTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "combat start",
        NextAction::array(0, new NextAction("set combat state", ACTION_PASSTROUGH + 10), NULL)));

    triggers.push_back(new TriggerNode(
        "combat end",
        NextAction::array(0, new NextAction("set non combat state", ACTION_PASSTROUGH + 10), NULL)));

    triggers.push_back(new TriggerNode(
        "death",
        NextAction::array(0, new NextAction("set dead state", ACTION_PASSTROUGH + 10), NULL)));

    triggers.push_back(new TriggerNode(
        "resurrect",
        NextAction::array(0, new NextAction("set non combat state", ACTION_PASSTROUGH + 10), NULL)));
}

void ClassStrategy::InitDeadTriggers(std::list<TriggerNode*>& triggers)
{

}

void ClassPvpStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "random",
        NextAction::array(0, new NextAction("free action potion", ACTION_HIGH), NULL)));
}

void ClassPvpStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void ClassPvpStrategy::InitReactionTriggers(std::list<TriggerNode*>& triggers)
{

}

void ClassPvpStrategy::InitDeadTriggers(std::list<TriggerNode*>& triggers)
{

}

void ClassPveStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void ClassPveStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void ClassPveStrategy::InitReactionTriggers(std::list<TriggerNode*>& triggers)
{

}

void ClassPveStrategy::InitDeadTriggers(std::list<TriggerNode*>& triggers)
{

}

void ClassRaidStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void ClassRaidStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void ClassRaidStrategy::InitReactionTriggers(std::list<TriggerNode*>& triggers)
{

}

void ClassRaidStrategy::InitDeadTriggers(std::list<TriggerNode*>& triggers)
{

}

void AoeStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "melee very high aoe",
        NextAction::array(0, new NextAction("goblin sapper", ACTION_HIGH + 1), NULL)));

    triggers.push_back(new TriggerNode(
        "ranged high aoe",
        NextAction::array(0, new NextAction("throw grenade", ACTION_HIGH), NULL)));
}

void AoeStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void AoePvpStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void AoePvpStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void AoePveStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void AoePveStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void AoeRaidStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void AoeRaidStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CureStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CureStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CurePvpStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CurePvpStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CurePveStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CurePveStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CureRaidStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CureRaidStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CcStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CcStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CcPvpStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CcPvpStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CcPveStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CcPveStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CcRaidStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CcRaidStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BuffStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BuffStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BuffPvpStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BuffPvpStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BuffPveStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BuffPveStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BuffRaidStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BuffRaidStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BoostStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "often",
        NextAction::array(0, new NextAction("use trinket", ACTION_HIGH), NULL)));
}

void BoostStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BoostPvpStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BoostPvpStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BoostPveStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BoostPveStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BoostRaidStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BoostRaidStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void StealthStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void StealthStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void StealthPvpStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void StealthPvpStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void StealthPveStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void StealthPveStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void StealthRaidStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void StealthRaidStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffhealStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffhealStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffhealPvpStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffhealPvpStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffhealPveStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffhealPveStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffhealRaidStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffhealRaidStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffdpsStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffdpsStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffdpsPvpStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffdpsPvpStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffdpsPveStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffdpsPveStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffdpsRaidStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffdpsRaidStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}


#endif
#ifdef MANGOSBOT_ONE // TBC

void ClassStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "invalid target",
        NextAction::array(0, new NextAction("select new target", ACTION_EMERGENCY), NULL)));

    triggers.push_back(new TriggerNode(
        "mounted",
        NextAction::array(0, new NextAction("check mount state", ACTION_EMERGENCY), NULL)));

    triggers.push_back(new TriggerNode(
        "very often",
        NextAction::array(0, new NextAction("use lightwell", ACTION_LIGHT_HEAL), NULL)));

    triggers.push_back(new TriggerNode(
        "combat long stuck",
        NextAction::array(0, new NextAction("unstuck", ACTION_IDLE), NULL)));

    triggers.push_back(new TriggerNode(
        "combat stuck",
        NextAction::array(0, new NextAction("unstuck", ACTION_IDLE), NULL)));
}

void ClassStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{
    /*
    triggers.push_back(new TriggerNode(
        "vehicle near",
        NextAction::array(0, new NextAction("enter vehicle", 10.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "very often",
        NextAction::array(0, new NextAction("use lightwell", 80.0f), NULL)));
    */

    triggers.push_back(new TriggerNode(
        "very often",
        NextAction::array(0, new NextAction("check mount state", ACTION_IDLE), new NextAction("check values", ACTION_IDLE), NULL)));

    triggers.push_back(new TriggerNode(
        "near dark portal",
        NextAction::array(0, new NextAction("move to dark portal", ACTION_IDLE), NULL)));

    triggers.push_back(new TriggerNode(
        "at dark portal azeroth",
        NextAction::array(0, new NextAction("use dark portal azeroth", ACTION_IDLE), NULL)));

    triggers.push_back(new TriggerNode(
        "at dark portal outland",
        NextAction::array(0, new NextAction("move from dark portal", ACTION_IDLE), NULL)));
}

void ClassStrategy::InitReactionTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "combat start",
        NextAction::array(0, new NextAction("set combat state", ACTION_PASSTROUGH + 10), NULL)));

    triggers.push_back(new TriggerNode(
        "combat end",
        NextAction::array(0, new NextAction("set non combat state", ACTION_PASSTROUGH + 10), NULL)));

    triggers.push_back(new TriggerNode(
        "death",
        NextAction::array(0, new NextAction("set dead state", ACTION_PASSTROUGH + 10), NULL)));

    triggers.push_back(new TriggerNode(
        "resurrect",
        NextAction::array(0, new NextAction("set non combat state", ACTION_PASSTROUGH + 10), NULL)));
}

void ClassStrategy::InitDeadTriggers(std::list<TriggerNode*>& triggers)
{

}

void ClassPvpStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void ClassPvpStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void ClassPvpStrategy::InitReactionTriggers(std::list<TriggerNode*>& triggers)
{

}

void ClassPvpStrategy::InitDeadTriggers(std::list<TriggerNode*>& triggers)
{

}

void ClassPveStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void ClassPveStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void ClassPveStrategy::InitReactionTriggers(std::list<TriggerNode*>& triggers)
{

}

void ClassPveStrategy::InitDeadTriggers(std::list<TriggerNode*>& triggers)
{

}

void ClassRaidStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void ClassRaidStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void ClassRaidStrategy::InitReactionTriggers(std::list<TriggerNode*>& triggers)
{

}

void ClassRaidStrategy::InitDeadTriggers(std::list<TriggerNode*>& triggers)
{

}

void AoeStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "melee very high aoe",
        NextAction::array(0, new NextAction("goblin sapper", ACTION_HIGH + 1), NULL)));

    triggers.push_back(new TriggerNode(
        "ranged high aoe",
        NextAction::array(0, new NextAction("throw grenade", ACTION_HIGH), NULL)));
}

void AoeStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void AoePvpStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void AoePvpStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void AoePveStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void AoePveStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void AoeRaidStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void AoeRaidStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CureStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CureStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CurePvpStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CurePvpStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CurePveStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CurePveStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CureRaidStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CureRaidStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CcStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CcStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CcPvpStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CcPvpStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CcPveStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CcPveStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CcRaidStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CcRaidStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BuffStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BuffStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BuffPvpStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BuffPvpStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BuffPveStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BuffPveStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BuffRaidStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BuffRaidStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BoostStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "often",
        NextAction::array(0, new NextAction("use trinket", ACTION_HIGH), NULL)));
}

void BoostStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BoostPvpStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BoostPvpStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BoostPveStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BoostPveStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BoostRaidStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BoostRaidStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void StealthStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void StealthStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void StealthPvpStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void StealthPvpStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void StealthPveStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void StealthPveStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void StealthRaidStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void StealthRaidStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffhealStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffhealStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffhealPvpStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffhealPvpStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffhealPveStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffhealPveStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffhealRaidStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffhealRaidStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffdpsStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffdpsStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffdpsPvpStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffdpsPvpStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffdpsPveStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffdpsPveStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffdpsRaidStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffdpsRaidStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

#endif
#ifdef MANGOSBOT_TWO // WOTLK

void ClassStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "invalid target",
        NextAction::array(0, new NextAction("select new target", ACTION_EMERGENCY), NULL)));

    triggers.push_back(new TriggerNode(
        "mounted",
        NextAction::array(0, new NextAction("check mount state", ACTION_EMERGENCY), NULL)));

    triggers.push_back(new TriggerNode(
        "very often",
        NextAction::array(0, new NextAction("use lightwell", ACTION_LIGHT_HEAL), NULL)));

    triggers.push_back(new TriggerNode(
        "often",
        NextAction::array(0, new NextAction("use trinket", ACTION_HIGH), NULL)));

    triggers.push_back(new TriggerNode(
        "combat long stuck",
        NextAction::array(0, new NextAction("unstuck", static_cast<float>(ACTION_IDLE) - 0.1f), NULL)));

    triggers.push_back(new TriggerNode(
        "combat stuck",
        NextAction::array(0, new NextAction("unstuck", static_cast<float>(ACTION_IDLE) - 0.3f), NULL)));
}

void ClassStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{
    /*
    triggers.push_back(new TriggerNode(
        "vehicle near",
        NextAction::array(0, new NextAction("enter vehicle", 10.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "very often",
        NextAction::array(0, new NextAction("use lightwell", 80.0f), NULL)));
    */

    triggers.push_back(new TriggerNode(
        "very often",
        NextAction::array(0, new NextAction("check mount state", ACTION_IDLE), new NextAction("check values", ACTION_IDLE), NULL)));

    triggers.push_back(new TriggerNode(
        "near dark portal",
        NextAction::array(0, new NextAction("move to dark portal", ACTION_IDLE), NULL)));

    triggers.push_back(new TriggerNode(
        "at dark portal azeroth",
        NextAction::array(0, new NextAction("use dark portal azeroth", ACTION_IDLE), NULL)));

    triggers.push_back(new TriggerNode(
        "at dark portal outland",
        NextAction::array(0, new NextAction("move from dark portal", ACTION_IDLE), NULL)));
}

void ClassStrategy::InitReactionTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "combat start",
        NextAction::array(0, new NextAction("set combat state", ACTION_PASSTROUGH + 10), NULL)));

    triggers.push_back(new TriggerNode(
        "combat end",
        NextAction::array(0, new NextAction("set non combat state", ACTION_PASSTROUGH + 10), NULL)));

    triggers.push_back(new TriggerNode(
        "death",
        NextAction::array(0, new NextAction("set dead state", ACTION_PASSTROUGH + 10), NULL)));

    triggers.push_back(new TriggerNode(
        "resurrect",
        NextAction::array(0, new NextAction("set non combat state", ACTION_PASSTROUGH + 10), NULL)));
}

void ClassStrategy::InitDeadTriggers(std::list<TriggerNode*>& triggers)
{

}

void ClassPvpStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void ClassPvpStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void ClassPvpStrategy::InitReactionTriggers(std::list<TriggerNode*>& triggers)
{

}

void ClassPvpStrategy::InitDeadTriggers(std::list<TriggerNode*>& triggers)
{

}

void ClassPveStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void ClassPveStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void ClassPveStrategy::InitReactionTriggers(std::list<TriggerNode*>& triggers)
{

}

void ClassPveStrategy::InitDeadTriggers(std::list<TriggerNode*>& triggers)
{

}

void ClassRaidStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void ClassRaidStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void ClassRaidStrategy::InitReactionTriggers(std::list<TriggerNode*>& triggers)
{

}

void ClassRaidStrategy::InitDeadTriggers(std::list<TriggerNode*>& triggers)
{

}

void AoeStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "melee very high aoe",
        NextAction::array(0, new NextAction("goblin sapper", ACTION_HIGH + 1), NULL)));

    triggers.push_back(new TriggerNode(
        "ranged high aoe",
        NextAction::array(0, new NextAction("throw grenade", ACTION_HIGH), NULL)));
}

void AoeStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void AoePvpStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void AoePvpStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void AoePveStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void AoePveStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void AoeRaidStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void AoeRaidStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CureStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CureStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CurePvpStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CurePvpStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CurePveStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CurePveStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CureRaidStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CureRaidStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CcStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CcStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CcPvpStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CcPvpStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CcPveStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CcPveStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CcRaidStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void CcRaidStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BuffStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BuffStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BuffPvpStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BuffPvpStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BuffPveStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BuffPveStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BuffRaidStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BuffRaidStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BoostStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "often",
        NextAction::array(0, new NextAction("use trinket", ACTION_HIGH), NULL)));
}

void BoostStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BoostPvpStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BoostPvpStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BoostPveStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BoostPveStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BoostRaidStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void BoostRaidStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void StealthStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void StealthStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void StealthPvpStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void StealthPvpStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void StealthPveStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void StealthPveStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void StealthRaidStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void StealthRaidStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffhealStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffhealStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffhealPvpStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffhealPvpStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffhealPveStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffhealPveStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffhealRaidStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffhealRaidStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffdpsStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffdpsStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffdpsPvpStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffdpsPvpStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffdpsPveStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffdpsPveStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffdpsRaidStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{

}

void OffdpsRaidStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{

}


#endif

void SpecPlaceholderStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "in pve",
        NextAction::array(0, new NextAction("update pve strats", ACTION_PASSTROUGH), NULL)));

    triggers.push_back(new TriggerNode(
        "in pvp",
        NextAction::array(0, new NextAction("update pvp strats", ACTION_PASSTROUGH), NULL)));

    triggers.push_back(new TriggerNode(
        "in raid fight",
        NextAction::array(0, new NextAction("update raid strats", ACTION_PASSTROUGH), NULL)));
}

void SpecPlaceholderStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "in pve",
        NextAction::array(0, new NextAction("update pve strats", ACTION_PASSTROUGH), NULL)));

    triggers.push_back(new TriggerNode(
        "in pvp",
        NextAction::array(0, new NextAction("update pvp strats", ACTION_PASSTROUGH), NULL)));

    triggers.push_back(new TriggerNode(
        "in raid fight",
        NextAction::array(0, new NextAction("update raid strats", ACTION_PASSTROUGH), NULL)));
}
