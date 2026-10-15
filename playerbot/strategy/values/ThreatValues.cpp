
#include "playerbot/playerbot.h"
#include "ThreatValues.h"

#include "playerbot/ServerFacade.h"
#include "Combat/ThreatManager.h"

using namespace ai;

float MyThreatValue::Calculate()
{
    Unit* target = ai->GetUnit(AI_VALUE(ObjectGuid, qualifier));
    
    if (target->GetObjectGuid() != lastTarget) //Reset history if we switched target.
        LogCalculatedValue::Reset();

    lastTarget = target->GetObjectGuid();
      
    return ThreatValue::GetThreat(bot, target);
}

float TankThreatValue::Calculate()
{
    Unit* target = ai->GetUnit(AI_VALUE(ObjectGuid, qualifier));

    return ThreatValue::GetTankThreat(ai, target);
}

uint8 ThreatValue::Calculate()
{
    if (qualifier == "aoe")
    {
        uint8 maxThreat = 0;
        std::list<ObjectGuid> attackers = context->GetValue<std::list<ObjectGuid>>("possible attack targets")->Get();
        for (std::list<ObjectGuid>::iterator i = attackers.begin(); i != attackers.end(); i++)
        {
            Unit* unit = ai->GetUnit(*i);
            if (!unit || !sServerFacade.IsAlive(unit))
                continue;

            uint8 threat = Calculate(unit);
            if (!maxThreat || threat > maxThreat)
                maxThreat = threat;
        }

        return maxThreat;
    }

    Unit* target = ai->GetUnit(AI_VALUE(ObjectGuid, qualifier));

    if (target && target->IsFriend(bot))
        target = target->GetTarget();

    return Calculate(target);
}

float ThreatValue::GetThreat(Player* player, Unit* target)
{
    if (!target || target->GetMapId() != player->GetMapId())
        return 0;

    if (dynamic_cast<Player*>(target) && (dynamic_cast<Player*>(target))->IsBeingTeleported())
        return 0;

    if (target->IsFriend(player))
        target = target->GetTarget();

    if (target->GetObjectGuid().IsPlayer())
        return 0;

    float botThreat = sServerFacade.GetThreatManager(target).getThreat(player);

    return botThreat;
}

float ThreatValue::GetTankThreat(PlayerbotAI* ai, Unit* target)
{
    if (!target)
        return 0;

    if (target->IsFriend(ai->GetBot()))
        target = target->GetTarget();

    // [threat 2026-10-17] 友好目标换成它的当前目标后可能为空（例：治疗 bot 的 current target 是一个没选目标的队友）
    //   ⇒ 这里补一道空指针保护，避免下面 target->GetObjectGuid() 解引用空指针。
    if (!target)
        return 0;

    if (target->GetObjectGuid().IsPlayer())
        return 0;

    Group* group = ai->GetBot()->GetGroup();
    if (!group)
        return 0;

    float maxThreat = -1.0f;

    for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
    {
        Player* player = gref->getSource();
        if (!player || !sServerFacade.IsAlive(player) || !ai->IsSafe(player) || player == ai->GetBot())
            continue;

        if (ai->IsTank(player))
        {
            float threat = sServerFacade.GetThreatManager(target).getThreat(player);
            if (maxThreat < threat)
                maxThreat = threat;
        }
    }

    return maxThreat;
}

// [threat 2026-10-18 站长口径] 队里对这个目标的最高仇恨（**不限坦克角色**，真人玩家也算）。
//   为什么需要：比例判断（"自己仇恨 ≥ 坦克仇恨的 X%"）依赖 `IsTank()` 认坦克；
//   真人玩家当坦克、或角色没被认出来时 `GetTankThreat()` 返回 -1 ⇒ 比例判断直接失效 ⇒ 开怪瞬间就 OT。
float ThreatValue::GetTopGroupThreat(PlayerbotAI* ai, Unit* target)
{
    if (!ai || !target)
        return -1.0f;

    if (target->IsFriend(ai->GetBot()))
        target = target->GetTarget();

    if (!target)
        return -1.0f;

    if (target->GetObjectGuid().IsPlayer())
        return -1.0f;

    Player* bot = ai->GetBot();
    Group* group = bot ? bot->GetGroup() : nullptr;
    if (!group)
        return -1.0f;

    float topThreat = -1.0f;
    for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
    {
        Player* player = gref->getSource();
        if (!player || !sServerFacade.IsAlive(player) || player == bot)
            continue;

        float threat = sServerFacade.GetThreatManager(target).getThreat(player);
        if (topThreat < threat)
            topThreat = threat;
    }

    return topThreat;
}

uint8 ThreatValue::Calculate(Unit* target){
    if (!target)
        return 0;

    if (target->IsFriend(bot))
        target = target->GetTarget();

    if (!target)
        return 0;

    if (target->GetObjectGuid().IsPlayer())
        return 0;

    Group* group = bot->GetGroup();
    if (!group)
        return 0;

    float botThreat, maxThreat;
    if (qualifier == "aoe")
    {
        botThreat = GetThreat(bot, target);
        maxThreat = GetTankThreat(ai, target);
    }
    else
    {
        botThreat = AI_VALUE2(float, "my threat", qualifier);
        maxThreat = AI_VALUE2(float, "tank threat", qualifier);
    }

    if (maxThreat < 0)
        return 0;

    // calculate normal threat for fleeing targets
    bool fleeing = target->GetMotionMaster()->GetCurrentMovementGeneratorType() == FLEEING_MOTION_TYPE ||
        target->GetMotionMaster()->GetCurrentMovementGeneratorType() == TIMED_FLEEING_MOTION_TYPE;

    // [threat 2026-10-17 站长口径] "坦克还没建立仇恨"= maxThreat == 0（队里**确实有坦克**；
    //   没有坦克时 GetTankThreat() 返回 -1，上面已 return 0 ⇒ 同样"不压"，纯机器人小队不受影响）。
    //   口径分两种，行为与修复前**一致**，只是不再是未定义行为：
    //     * 目标**已进战斗**（且没在逃跑）⇒ 返回 100 = "该压"（原有口径，保留：坦克还没上仇恨就先别打）；
    //     * 目标**还没进战斗** ⇒ 明确返回 0 = "不压"（野外与仇恨反应动作零影响）。
    //       原代码这种情况没有分支，会掉到下面 botThreat * 100 / maxThreat 的 **0/0**：
    //       float 除零得 NaN（botThreat>0 时是 ±inf），隐式转 uint8 是未定义行为，实测取到 0；
    //       现在把结果**写死成 0**，与实测行为相同但已是明确定义。
    //   副本里"非坦克开场不许打坦克还没拉的怪"不再依赖这个值，而是两道**显式、与地图绑定**的门禁：
    //     * AttackAction::IsWaitingForTankThreat() —— 出手/选目标（所有 AttackAction 派生动作）
    //     * WaitForAttackStrategy::ShouldWait() 的副本门禁 —— 法系读条等"威胁类"动作
    if (maxThreat == 0)
        return (target->IsInCombat() && !fleeing) ? 100 : 0;

    // return low threat if mob if fleeing
    if (fleeing)
        return 0;

    return botThreat * 100 / maxThreat;
}