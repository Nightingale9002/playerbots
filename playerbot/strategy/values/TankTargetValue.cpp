
#include "playerbot/playerbot.h"
#include "TankTargetValue.h"
#include "PossibleAttackTargetsValue.h"
#include "LeastHpTargetValue.h"

using namespace ai;

// [pull 2026-10-18 站长口径] 坦克"拿正在打队友的怪"的排序（FindTargetForTankStrategy::CheckAttacker）：
//   1) 真正在交战（有仇恨 / 正在打自己或队友）的怪优先于"只是被塞进 attackers 的远处目标"
//      （AttackersValue::AddTargetsOf 会把 current target / attack target 也塞进 "attackers"，
//        那些还没走到的远处目标对坦克仇恨是 0 ⇒ 只看 minThreat 时坦克会一直选回那个远处目标，
//        路上打它的怪永远轮不到，表现就是"无视路上的怪直接跑过去"）；
//   2) 近战怪优先于远程/法系怪（PossibleAttackTargetsValue::IsMeleeAttacker()）；
//   3) 距离自己（战斗触及距离）更近的优先；
//   4) 其余保持原判据：同层/可达（CanReachTarget）、优先级最低仇恨（minThreat）作为最后 tie-break。
//
// [aggro 2026-10-17 站长口径] 判据 (1)(2)(3) 现在与 DPS 链共用
//   PossibleAttackTargetsValue::IsEngagedAttacker() / IsMeleeAttacker()，两条链排序口径一致。
class FindTargetForTankStrategy : public FindNonCcTargetStrategy
{
public:
    FindTargetForTankStrategy(PlayerbotAI* ai) : FindNonCcTargetStrategy(ai)
    {
        minThreat = 0;
        resultTier = -1;
        resultMelee = false;
        resultDistance = 0.0f;
    }

public:
    virtual void CheckAttacker(Unit* creature, ThreatManager* threatManager) override
    {
        Player* bot = ai->GetBot();
        AiObjectContext* context = ai->GetAiObjectContext();

        if (IsCcTarget(creature)) return;

        if (!PossibleAttackTargetsValue::IsValid(creature, bot))
        {
            std::list<ObjectGuid> attackers = AI_VALUE(std::list<ObjectGuid>, "possible attack targets");
            if (std::find(attackers.begin(), attackers.end(), creature->GetObjectGuid()) == attackers.end())
                return;
        }

        // [pull 2026-10-18 站长口径] 不在同一层 / 走不到的怪不当坦克目标（它只会让坦克一路跑过去）。
        //   requireLos=false：战斗中目标被柱子暂时挡住不该丢，交给路网判据。
        if (!PossibleAttackTargetsValue::CanReachTarget(creature, bot, false))
            return;

        // [aggro 2026-10-17] 与 DPS 链共用同一套威胁分级（见 LeastHpTargetValue.h → FindDpsThreatTargetStrategy）：
        //   坦克也要优先去把"会加血 / 能打断可减输出"的怪**建立基础仇恨**，否则 DPS 一打它就 OT。
        const int tier = FindDpsThreatTargetStrategy::ThreatTier(creature, bot);
        const bool melee = PossibleAttackTargetsValue::IsMeleeAttacker(creature);
        const float distance = bot->GetDistance(creature, true, DIST_CALC_COMBAT_REACH);
        const float threat = threatManager->getThreat(bot);

        if (IsBetter(tier, melee, distance, threat))
        {
            result = creature;
            resultTier = tier;
            resultMelee = melee;
            resultDistance = distance;
            minThreat = threat;
        }
    }

protected:
    bool IsBetter(int tier, bool melee, float distance, float threat) const
    {
        if (!result)
            return true;

        // 0 级 = 未交战（只是被塞进候选的远处目标）；1 级以上 = 已交战。分级已含此语义。
        if (tier != resultTier)
            return tier > resultTier;

        if (melee != resultMelee)
            return melee;

        if (fabs(resultDistance - distance) > 0.5f)
            return distance < resultDistance;

        return (minThreat - threat) > 0.1f;
    }

protected:
    float minThreat;
    int resultTier;
    bool resultMelee;
    float resultDistance;
};


ObjectGuid TankTargetValue::Calculate()
{
    ObjectGuid rti = RtiTargetValue::Calculate();
    if (rti) return rti;

    FindTargetForTankStrategy strategy(ai);
    Unit* target = FindTarget(&strategy);
    return target ? target->GetObjectGuid() : ObjectGuid();
}
