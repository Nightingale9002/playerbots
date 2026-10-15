#pragma once
#include "playerbot/strategy/Value.h"
#include "TargetValue.h"
#include "PossibleAttackTargetsValue.h"

namespace ai
{
    // [aggro 2026-10-17 站长口径] **威胁分级**：DPS 与坦克共用同一套，避免两条链口径漂移。
    //   站长口径原话："dps 应该是谁仇恨足够打谁，并且 t 要保证每只怪的基础仇恨，或者是谁威胁高打谁；
    //   有些怪物是威胁更高的，比如有强力技能、会加血，或者单纯的输出更高，或者有些法系怪通过打断能减少
    //   输出，这些应该考虑到" / "实际上更应该强调的是高威胁技能，比如加血、击晕、召唤等，其中有些是
    //   可以打断的，我们可以用'有高威胁技能'作为高威胁怪物的判断规则" / "我们应该加一个优先打带骷髅标记的"。
    //
    //   分级（数字越大越优先）：
    //     5 = 带**骷髅标记**（全队约定的第一集火目标，人工指定优先于一切自动判断）
    //     4 = 有高威胁技能（HasHighThreatAbility：治疗/击晕/定身/恐惧/召唤/高额伤害 —— **怪的属性**）
    //     3 = 正在交战（在打自己/队友）—— 还没交战的怪不该抢
    //     2 = 正在施放可打断的法术（打断即减输出）
    //     1 = 正在施放治疗法术（此刻就在回血，最紧急的瞬时状态）
    //     0 = 其余
    //   关键设计（站长给的约束）：**残血不作为主判据**。残血是"结果"，拿它当平级判据会让目标在两 tick
    //   之间来回跳（"一直残血优先"）。这里残血只在**最后**、且距离/仇恨都相同时才用。
    class FindDpsThreatTargetStrategy : public FindNonCcTargetStrategy
    {
    public:
        FindDpsThreatTargetStrategy(PlayerbotAI* ai) : FindNonCcTargetStrategy(ai)
        {
            minHealth = 0;
            resultTier = -1;
            resultMelee = false;
            resultDistance = 0.0f;
            resultThreat = 0.0f;
        }

        static int ThreatTier(Unit* target, Player* bot)
        {
            // 分级本体在 PossibleAttackTargetsValue::ThreatTier()（避免循环包含）
            return PossibleAttackTargetsValue::ThreatTier(target, bot);
        }

        virtual void CheckAttacker(Unit* attacker, ThreatManager* threatManager) override
        {
            // do not use this logic for pvp
            if (attacker->IsPlayer())
                return;

            if (IsCcTarget(attacker))
                return;

            Player* bot = ai->GetBot();
            const int tier = ThreatTier(attacker, bot);
            const bool melee = PossibleAttackTargetsValue::IsMeleeAttacker(attacker);
            const float distance = bot->GetDistance(attacker, true, DIST_CALC_COMBAT_REACH);
            const float threat = threatManager->getThreat(bot);
            const float health = attacker->GetHealth();

            if (!result || IsBetter(tier, melee, distance, threat, health))
            {
                result = attacker;
                resultTier = tier;
                resultMelee = melee;
                resultDistance = distance;
                resultThreat = threat;
                minHealth = health;
            }
        }

    protected:
        bool IsBetter(int tier, bool melee, float distance, float threat, float health) const
        {
            if (tier != resultTier)
                return tier > resultTier;

            if (melee != resultMelee)
                return melee;

            if (fabs(resultDistance - distance) > 0.5f)
                return distance < resultDistance;

            if (fabs(resultThreat - threat) > 0.1f)
                return threat > resultThreat;

            return health < minHealth;
        }

    protected:
        float minHealth;
        int resultTier;
        bool resultMelee;
        float resultDistance;
        float resultThreat;
    };

    // 兼容旧用法：`least hp target` 现在等同于上面的威胁分级链（残血只是最后的 tie-break）。
    class FindLeastHpTargetStrategy : public FindDpsThreatTargetStrategy
    {
    public:
        FindLeastHpTargetStrategy(PlayerbotAI* ai) : FindDpsThreatTargetStrategy(ai) {}
    };

    class LeastHpTargetValue : public TargetValue
	{
	public:
        LeastHpTargetValue(PlayerbotAI* ai, std::string name = "least hp target") : TargetValue(ai, name) {}

    public:
        ObjectGuid Calculate() override;
    };
}
