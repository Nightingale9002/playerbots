
#include "playerbot/playerbot.h"
#include "PullStrategy.h"
#include "CombatStrategy.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/strategy/values/ThreatValues.h"
#include "playerbot/strategy/values/PossibleAttackTargetsValue.h"
#include "playerbot/DecisionWeights.h"
#include "playerbot/DungeonRoute.h"
#include "Grids/GridNotifiers.h"
#include "Grids/GridNotifiersImpl.h"
#include "Grids/CellImpl.h"

using namespace ai;

void CombatStrategy::InitCombatTriggers(std::list<TriggerNode*> &triggers)
{
    triggers.push_back(new TriggerNode(
        "invalid target",
        NextAction::array(0, new NextAction("select new target", 89.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "mounted",
        NextAction::array(0, new NextAction("check mount state", 88.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "combat stuck",
        NextAction::array(0, new NextAction("unstuck", 0.7f), NULL)));

    triggers.push_back(new TriggerNode(
        "combat long stuck",
        NextAction::array(0, new NextAction("unstuck", 0.9f), NULL)));

    triggers.push_back(new TriggerNode(
        "often",
        NextAction::array(0, new NextAction("use trinket", 50.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "very often",
        NextAction::array(0, new NextAction("use lightwell", 80.0f), NULL)));
}

float AvoidAoeStrategyMultiplier::GetValue(Action* action)
{
    if (!action)
        return 1.0f;

    std::string name = action->getName();
    if (name == "follow" || name == "co" || name == "nc" || name == "react" || name == "select new target" || name == "flee")
        return 1.0f;

    uint32 spellId = AI_VALUE2(uint32, "spell id", name);
    const SpellEntry* const pSpellInfo = sServerFacade.LookupSpellInfo(spellId);
    if (!pSpellInfo) return 1.0f;

    if (spellId && pSpellInfo->Targets & TARGET_FLAG_DEST_LOCATION)
        return 1.0f;
    else if (spellId && pSpellInfo->Targets & TARGET_FLAG_SOURCE_LOCATION)
        return 1.0f;

    uint32 CastingTime = !IsChanneledSpell(pSpellInfo) ? GetSpellCastTime(pSpellInfo, bot) : GetSpellDuration(pSpellInfo);

    if (AI_VALUE2(bool, "has area debuff", "self target") && spellId && CastingTime > 0)
    {
        return 0.0f;
    }

    return 1.0f;
}

void AvoidAoeStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "has area debuff",
        NextAction::array(0, new NextAction("flee", ACTION_EMERGENCY + 5), NULL)));
}

void AvoidAoeStrategy::InitReactionTriggers(std::list<TriggerNode*>& triggers)
{
    InitCombatTriggers(triggers);
}

void AvoidAoeStrategy::InitCombatMultipliers(std::list<Multiplier*>& multipliers)
{
    multipliers.push_back(new AvoidAoeStrategyMultiplier(ai));
}

void AvoidAoeStrategy::InitReactionMultipliers(std::list<Multiplier*>& multipliers)
{
    InitCombatMultipliers(multipliers);
}

void AvoidSpecificCreaturesStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "specific creature too close",
        NextAction::array(0, new NextAction("move away from specific creatures", ACTION_EMERGENCY + 5), NULL)));
}

void AvoidSpecificCreaturesStrategy::InitReactionTriggers(std::list<TriggerNode*>& triggers)
{
    InitCombatTriggers(triggers);
}

void AvoidSpecificCreaturesStrategy::InitCombatMultipliers(std::list<Multiplier*>& multipliers)
{
    multipliers.push_back(new AvoidSpecificCreaturesStrategyMultiplier(ai));
}

void AvoidSpecificCreaturesStrategy::InitReactionMultipliers(std::list<Multiplier*>& multipliers)
{
    InitCombatMultipliers(multipliers);
}

void WaitForAttackStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "wait for attack safe distance",
        NextAction::array(0, new NextAction("wait for attack keep safe distance", 60.0f), NULL)));

    // [threat margin 2026-10-19] 预期仇恨闸门：**这一发打完**就会超过坦克仇恨 ⇒ 先压着。
    //   与上面那条的区别：上面是"现在的仇恨比例已经过高"（静态），这条是"再打一发就过高"（预测），
    //   判据在值 `will overaggro`（自己仇恨 + 窗口内单发最大增量×1.15 vs 坦克仇恨）。
    //   优先级 20：压过普通攻击(10)/高优先技能(20 及以下)，但低于走位(30)与所有治疗/打断，
    //   也就是"宁可少打一下，也不 OT"；坦克自己在该值里恒为 false，不受影响。
    triggers.push_back(new TriggerNode(
        "will overaggro",
        NextAction::array(0, new NextAction("wait for attack keep safe distance", 20.0f), NULL)));

    // [threat-ot 2026-10-19] DPS 即将 OT：优先跑向坦克（比"保安全距离"高，但仍低于真正走位/治疗）
    triggers.push_back(new TriggerNode(
        "dps will overaggro",
        NextAction::array(0, new NextAction("move toward tank", 25.0f), NULL)));
}

void WaitForAttackStrategy::InitCombatMultipliers(std::list<Multiplier*>& multipliers)
{
    multipliers.push_back(new WaitForAttackMultiplier(ai));
}

WaitForAttackStrategy* WaitForAttackStrategy::Get(PlayerbotAI* ai)
{
    return ai ? ai->GetStrategy<WaitForAttackStrategy>("wait for attack", BotState::BOT_STATE_COMBAT) : nullptr;
}

bool WaitForAttackStrategy::ShouldWait(PlayerbotAI* ai)
{
    // Only check if the bot has the strategy enabled
    if (ai->HasStrategy("wait for attack", BotState::BOT_STATE_COMBAT))
    {
        Player* bot = ai->GetBot();
        AiObjectContext* context = ai->GetAiObjectContext();

        // [threat 2026-10-16 站长口径] 坦克不压（要立刻开怪/接怪）
        if (ai->IsTank(bot))
            return false;

        // [2026-10-18 站长口径] "现在 t 还在 pullback 的时候，近战已经走上去了"
        //   ⇒ 只要队里坦克还在**拉怪/回拉流程**里（PullStrategy 有目标），其他人一律先压着不动：
        //     不再按"我自己的仇恨/距离"判断（那些在坦克回拉途中全是放行条件）。
        if (Group* pullGroup = bot->GetGroup())
        {
            for (GroupReference* ref = pullGroup->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->getSource();
                if (!member || member == bot || !member->IsInWorld())
                    continue;

                PlayerbotAI* memberAi = member->GetPlayerbotAI();
                if (!memberAi || !ai->IsTank(member))
                    continue;

                PullStrategy* pulling = PullStrategy::Get(memberAi);
                if (pulling && pulling->HasTarget())
                    return true;
            }
        }

        // [threat 2026-10-16 站长口径] 看自己的仇恨：自己的仇恨 ≥ 坦克仇恨的 X% 时压着不出手
        //   （AiPlayerbot.WaitForAttackThreatPercent，0 = 关；"threat" 值 = 自己仇恨 ×100 / 坦克仇恨，
        //    坦克还没上仇恨而怪已进战斗时它会给 100 ⇒ 也顺带盖住"坦克还没拉住"的情况）
        if (sPlayerbotAIConfig.waitForAttackThreatPercent > 0 &&
            AI_VALUE2(uint8, "threat", "current target") >= sPlayerbotAIConfig.waitForAttackThreatPercent)
        {
            return true;
        }

        // [threat 2026-10-16 站长口径] 纯机器人队伍也按同样规则（不再要求"真人是 master"）：
        //   但"开场等坦克"只在**队里真的有坦克**时才等 —— 没有坦克的小队没得等，
        //   否则野外机器人小队每次开怪都集体发呆 WaitForAttackTime 秒（实测观感就是"放完技能要等一下"）。
        bool groupHasTank = false;
        if (Group* group = bot->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->getSource();
                // [threat 2026-10-17] "队里有坦克"必须是**活着的**坦克（坦克阵亡时不要再等它）
                if (member && member != bot && sServerFacade.IsAlive(member) && ai->IsTank(member))
                {
                    groupHasTank = true;
                    break;
                }
            }
        }

        // [threat 2026-10-16 站长口径] "开场等坦克 pull back"不能是死等固定秒数（实测 10 秒太长、
        //   跟得太近时每次开怪都要干等）。改成**事件驱动**：
        //     * 队里没有坦克 ⇒ 不等
        //     * 坦克已经把怪拉回来（坦克离我 ≤ WaitForAttackTankBackDistance 且对该目标有仇恨）⇒ 立刻打
        //     * 其余情况才等，上限 WaitForAttackTime 秒（默认 5）
        if (groupHasTank)
        {
            // 找队里的坦克
            Player* tank = nullptr;
            for (GroupReference* ref = bot->GetGroup()->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->getSource();
                if (member && member != bot && sServerFacade.IsAlive(member) && ai->IsTank(member))
                {
                    tank = member;
                    break;
                }
            }

            // [threat 2026-10-17 站长口径] **副本/团本专用**门禁（野外一律不生效）：
            //   非坦克 + 队里有活着的坦克 + 当前目标"坦克还没有仇恨"（GetTankThreat <= 0）
            //   且这只怪**也没在打队伍成员** ⇒ 压着不出手（返回 true）。
            //   为什么需要它：ThreatValue::Calculate() 把"怪未进战斗 + 坦克 0 仇恨"明确成返回 0（不压，
            //   见 ThreatValues.cpp），这个缺口以前是靠 0/0 的未定义行为顺带盖住的。
            //   [aggro 2026-10-17 修正] 原来只拦"怪还没进战斗"（`!unit->IsInCombat()`）。那只盖住了
            //   "坦克还没碰过"的**一部分**：怪一旦被别人（宠物/自己/路人）带进战斗，`IsInCombat()` 变真，
            //   这道门禁就放行了 —— 而它身上依然没有坦克的仇恨 ⇒ 正是站长说的
            //   "会打 t 还没建立仇恨的怪"。现在改成只看"坦克有没有仇恨 + 是不是在打队友"。
            if (bot->GetMap() && (bot->GetMap()->IsDungeon() || bot->GetMap()->IsRaid()))
            {
                Unit* unit = ai->GetUnit(context->GetValue<ObjectGuid>("current target")->Get());
                if (unit && !sServerFacade.IsFriendlyTo(bot, unit) &&
                    ThreatValue::GetTankThreat(ai, unit) <= 0 &&
                    !PossibleAttackTargetsValue::IsEngagedAttacker(unit, bot))
                {
                    return true;
                }
            }

            // 坦克是否已经把怪拉回来了：离我够近 + 对我当前目标有仇恨
            bool tankPulledBack = false;
            if (tank)
            {
                float const backDistance = sPlayerbotAIConfig.waitForAttackTankBackDistance;
                Unit* target = ai->GetUnit(context->GetValue<ObjectGuid>("current target")->Get());
                float const tankDistance = sServerFacade.GetDistance2d(bot, tank);
                bool const tankHasThreat = target && target->GetObjectGuid().IsCreature() &&
                    sServerFacade.GetThreatManager(target).getThreat(tank) > 0;

                tankPulledBack = tankHasThreat && tankDistance <= backDistance;
            }

            if (tankPulledBack)
                return false;

            // 还没拉回来 ⇒ 等，但最多等 WaitForAttackTime 秒
            const time_t combatStartTime = AI_VALUE(time_t, "combat start time");
            if (combatStartTime > 0)
                return (time(0) - combatStartTime) < GetWaitTime(ai);
        }
    }

    return false;
}

uint8 WaitForAttackStrategy::GetWaitTime(PlayerbotAI* ai)
{
    AiObjectContext* context = ai->GetAiObjectContext();
    return AI_VALUE(uint8, "wait for attack time");
}

float WaitForAttackMultiplier::GetValue(Action* action)
{
    // Allow some movement and targeting actions and non threat actions (like cc!)
    const std::string& actionName = action->getName();
    if ((actionName != "wait for attack keep safe distance") && 
        (actionName != "dps assist") && 
        (actionName != "set facing") &&
        (actionName != "pull my target") &&
        (actionName != "pull rti target") &&
        (actionName != "pull start") &&
        (actionName != "pull action") &&
        (actionName != "pull end") &&
        (action->getThreatType() != ActionThreatType::ACTION_THREAT_NONE))
    {
        return WaitForAttackStrategy::ShouldWait(ai) ? 0.0f : 1.0f;
    }

    return 1.0f;
}

void HealInterruptStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "heal target full health",
        NextAction::array(0, new NextAction("interrupt current spell", ACTION_EMERGENCY), NULL)));
}

// [pull-gate 2026-10-19] 怪堆闸门：附近敌对达到 PullHostileLimit 时压住"开怪"这个动作。
//   优先级 20：压过普通攻击(10)/低优先技能，低于走位(30)、打断(40)、治疗(60+)
//   —— 也就是"宁可先不打，也不要一头扎进一堆怪"，但绝不影响救命动作。
void PullGateStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "too many hostiles",
        NextAction::array(0, new NextAction("wait for attack keep safe distance", 20.0f), NULL)));
}

// [defend-self 2026-10-19 站长口径] 残血且被近战贴脸 ⇒ 先脱战近战，再谈输出/治疗。
//   优先级按角色分档：治疗 92（高于 CRITICAL_HEAL 80：先自救再奶人）/ 其它 88（低于躲地板 95）。
//   动作用现有的 `flee`（受 AiPlayerbot.FleeingEnabled 控制），**不是** runaway/kite。
void DefendSelfStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{
    float const priority = ai->IsHeal(ai->GetBot()) ? 92.0f : 88.0f;
    triggers.push_back(new TriggerNode(
        "being meleed low health",
        NextAction::array(0, new NextAction("flee", priority), NULL)));
}

// [tank 2026-10-19] 坦克救场：优先级 85 —— 高于普通仇恨循环与开怪(10/20)，
//   低于躲地板(95)、非坦克自保(88/92)；坦克自己不会触发 defend self，所以两者不冲突。
void TankPickUpStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "has loose mobs",
        NextAction::array(0, new NextAction("pick up loose mob", 85.0f), NULL)));

    // [tank 2026-10-19 站长口径] "target attacking healer 应该是 t 策略" ⇒ 优先级最高的一条：
    //   治疗正在被当前目标追，坦克立刻接（86，高于普通漏怪 85）。
    triggers.push_back(new TriggerNode(
        "target attacking healer",
        NextAction::array(0, new NextAction("pick up loose mob", 86.0f), NULL)));
}

void HealInterruptStrategy::InitReactionTriggers(std::list<TriggerNode*>& triggers)
{
    InitCombatTriggers(triggers);
}

// [2026-10-22 站长口径] ① AoE 破控守卫
void AoeCcGuardStrategy::InitCombatMultipliers(std::list<Multiplier*>& multipliers)
{
    multipliers.push_back(new AoeCcGuardMultiplier(ai));
}

float AoeCcGuardMultiplier::GetValue(Action* action)
{
    if (!action)
        return 1.0f;

    std::string const name = action->getName();
    if (name == "follow" || name == "flee" || name == "co" || name == "nc" || name == "select new target")
        return 1.0f;

    float const radius = dw::Get("aoe.cc_guard.radius");
    if (radius <= 0.0f)
        return 1.0f;

    uint32 const spellId = AI_VALUE2(uint32, "spell id", name);
    if (!spellId)
        return 1.0f;

    SpellEntry const* si = sServerFacade.LookupSpellInfo(spellId);
    if (!si)
        return 1.0f;

    bool isArea = false;
    for (int i = 0; i < 3; ++i)
    {
        if (si->Effect[i] && si->EffectRadiusIndex[i] > 0)
        {
            isArea = true;
            break;
        }
    }
    if (!isArea)
        return 1.0f;

    std::list<Unit*> units;
    MaNGOS::AnyUnitFulfillingConditionInRangeCheck check(bot,
        [this](Unit* u) -> bool
        {
            if (!u || !u->IsAlive() || u->GetTypeId() != TYPEID_UNIT)
                return false;
            if (!bot->IsEnemy(u))
                return false;
            return PossibleAttackTargetsValue::HasBreakableCC(u, bot) ||
                   PossibleAttackTargetsValue::HasUnBreakableCC(u, bot);
        }, radius);
    MaNGOS::UnitListSearcher<MaNGOS::AnyUnitFulfillingConditionInRangeCheck> searcher(units, check);
    Cell::VisitAllObjects(bot, searcher, radius);

    return units.empty() ? 1.0f : dw::Get("aoe.cc_guard.penalty");
}

// [2026-10-22 站长口径] ② 目标重选惩罚（等坦克仇恨期间别每 tick 清目标重选）
void ReselectPenaltyStrategy::InitCombatMultipliers(std::list<Multiplier*>& multipliers)
{
    multipliers.push_back(new ReselectPenaltyMultiplier(ai));
}

float ReselectPenaltyMultiplier::GetValue(Action* action)
{
    if (!action || action->getName() != "select new target")
        return 1.0f;
    if (!bot || PlayerbotAI::IsTank(bot))
        return 1.0f;
    if (!WaitForAttackStrategy::ShouldWait(ai))
        return 1.0f;
    return dw::Get("target.reselect.waiting_penalty");
}

// [2026-10-22 站长口径] ③ 路上遇到安全可拉的怪 ⇒ pull 权重上调（不加触发器）
void PullEncounterBoostStrategy::InitCombatMultipliers(std::list<Multiplier*>& multipliers)
{
    multipliers.push_back(new PullEncounterBoostMultiplier(ai));
}

float PullEncounterBoostMultiplier::GetValue(Action* action)
{
    if (!action)
        return 1.0f;

    std::string const name = action->getName();
    if (name != "dungeon route pull" && name != "pull my target")
        return 1.0f;

    if (!bot || !bot->IsInWorld() || !bot->GetMap())
        return 1.0f;
    if (!dungeonroute::BossFirstMode() || !dungeonroute::RouteEnabled())
        return 1.0f;

    dungeonroute::MapRoute const* route = dungeonroute::GetMapRoute(bot->GetMap()->GetId());
    if (!route)
        return 1.0f;

    dungeonroute::Target t;
    if (!dungeonroute::FindTargetNearest(bot, *route, t, true) || !t.hasPullPoint || !t.guid)
        return 1.0f;

    Creature* c = bot->GetMap()->GetCreature(t.guid);
    if (!c || !c->IsAlive())
        return 1.0f;
    if (bot->GetDistance(c) > dw::Get("pull.encounter.range"))
        return 1.0f;
    if (!dungeonroute::BossModePullSafe(bot, bot->GetMap(), *route, c))
        return 1.0f;

    return dw::Get("pull.encounter_boost");
}
