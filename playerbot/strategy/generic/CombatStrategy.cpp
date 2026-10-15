
#include "playerbot/playerbot.h"
#include "CombatStrategy.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/strategy/values/ThreatValues.h"
#include "playerbot/strategy/values/PossibleAttackTargetsValue.h"
#include "playerbot/strategy/actions/GenericSpellActions.h"
#include "playerbot/strategy/values/AoeValues.h"

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

    // [corner pull 2026-10-18 站长口径] **非坦克也卡视野**：远处有远程怪在打我们、近战够不到时，
    //   跟到坦克的卡视野点后面（动作 `use cover`），把它逼进近战。放在所有职业都会加载的通用战斗策略里，
    //   优先级取 ACTION_MOVE（比攻击高、比打断/治疗低）。
    //   [pullback 2026-10-18 站长口径] 开关：AiPlayerbot.UseCover（默认 1）。
    if (sPlayerbotAIConfig.useCoverEnabled)
    {
        triggers.push_back(new TriggerNode(
            "enemy ranged out of melee",
            NextAction::array(0, new NextAction("use cover", ACTION_MOVE), NULL)));
    }

    // [boss-pos 2026-10-18 站长口径] BOSS 战站位：**每个 BOSS 单独一套**（`ai_playerbot_boss_position`，
    //   游戏内 `.bot bosspos` 录入，写库即生效）。跑出该角色给的战场半径 ⇒ 走回去；没登记的 BOSS 不干预。
    triggers.push_back(new TriggerNode(
        "boss position",
        NextAction::array(0, new NextAction("boss position", ACTION_MOVE + 1), NULL)));

    // [killorder 2026-10-18 站长口径] 优先击杀：名单里的怪在场且当前目标不是它 ⇒ 切目标
    //   （`AiPlayerbot.PriorityKillTargets`，破碎大厅：碎手军团士兵 16700/20589）。
    triggers.push_back(new TriggerNode(
        "priority kill target available",
        NextAction::array(0, new NextAction("switch to priority kill target", ACTION_HIGH + 5), NULL)));
}

float AvoidAoeStrategyMultiplier::GetValue(Action* action)
{
    if (!action)
        return 1.0f;

    std::string name = action->getName();

    // [dodge 2026-10-18 站长口径] **任何情况优先躲地板**。
    //   现场反馈"在躲地板和其他策略之间来回冲突"：判断是 multipliers 只压了"读条法术"，
    //   移动/攻击/接近类动作照常执行 ⇒ 躲地板的走位被 `reach`/`attack` 立刻顶回去，形成来回。
    //   现在：只要"脚下有敌方 AOE"或"身上已挂 area debuff"，除少数几个协调动作外**一律压零**，
    //   只留躲地板动作自己能跑。
    if (AI_VALUE(bool, "hostile aoe incoming") || AI_VALUE2(bool, "has area debuff", "self target"))
    {
        // [dodge 2026-10-18 站长口径] 躲地板**优先级最高、不豁免**；唯一例外：
        //   "普通伤害地板 + 队里有人危急血"时允许先把治疗读完（致命地板不给例外 —— 先活下来）。
        bool const lethalAoe = HostileAoeValue::IsLethalIncoming(ai);
        if (!lethalAoe && AI_VALUE(bool, "party member critical health") &&
            (dynamic_cast<CastHealingSpellAction*>(action) || name == "heal"))
            return 1.0f;

        if (name == "move away from hazard" || name == "move away from magmadar lava bomb" ||
            name == "set facing" || name == "select new target" || name == "stop moving" ||
            name == "wait for attack keep safe distance")
            return 1.0f;

        sLog.outDetail("[dodge] %s: suppress '%s' (aoe incoming/standing in it)", bot->GetName(), name.c_str());
        return 0.0f;
    }

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

        // [heal 2026-10-18 站长口径] **有人需要治疗时，绝不因为"等坦克仇恨"而压着**。
        //   站长实测"牧师只在 T 血很低的时候才治疗"：治疗法术的 threatType 是 ACTION_THREAT_AOE，
        //   `WaitForAttackMultiplier` 会把它一起压成 0；叠加新加的仇恨基础值门禁，开怪前期治疗整段被压掉。
        if (AI_VALUE(bool, "party member critical health") || AI_VALUE(bool, "party member low health") ||
            AI_VALUE(bool, "party member medium health"))
            return false;

        // [threat 2026-10-16 站长口径] 看自己的仇恨：自己的仇恨 ≥ 坦克仇恨的 X% 时压着不出手
        //   （AiPlayerbot.WaitForAttackThreatPercent，0 = 关；"threat" 值 = 自己仇恨 ×100 / 坦克仇恨，
        //    坦克还没上仇恨而怪已进战斗时它会给 100 ⇒ 也顺带盖住"坦克还没拉住"的情况）
        if (sPlayerbotAIConfig.waitForAttackThreatPercent > 0 &&
            AI_VALUE2(uint8, "threat", "current target") >= sPlayerbotAIConfig.waitForAttackThreatPercent)
        {
            return true;
        }

        // [threat 2026-10-18 站长口径] **仇恨基础值**：队里最高仇恨 < WaitForAttackThreatBase 时，非坦克先压着。
        //   为什么比例判断不够：它比的是"坦克仇恨"（靠 IsTank() 认人）。真人当坦克 / 角色没被认出来时
        //   GetTankThreat() 返回 -1 ⇒ 比例判断直接失效 ⇒ 开怪瞬间 bot 就把仇恨拉满（站长实测"总是 OT"）。
        //   这里改用"队里对这个目标的最高仇恨"（真人/任何职业都算），并要求它先涨到 base 才允许出手。
        if (sPlayerbotAIConfig.waitForAttackThreatBase > 0)
        {
            Unit* unit = ai->GetUnit(context->GetValue<ObjectGuid>("current target")->Get());
            if (unit && !sServerFacade.IsFriendlyTo(bot, unit) && unit->IsInCombat())
            {
                float topThreat = ThreatValue::GetTopGroupThreat(ai, unit);
                if (topThreat >= 0.0f && topThreat < sPlayerbotAIConfig.waitForAttackThreatBase)
                    return true;
            }
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

    // [heal 2026-10-18 站长口径] 治疗类动作**永远不压**（治疗优先于仇恨，站长："牧师只在 T 血很低才治疗"）。
    if (dynamic_cast<CastHealingSpellAction*>(action) || actionName == "heal" || actionName == "interrupt current spell")
        return 1.0f;

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

void HealInterruptStrategy::InitReactionTriggers(std::list<TriggerNode*>& triggers)
{
    InitCombatTriggers(triggers);
}
