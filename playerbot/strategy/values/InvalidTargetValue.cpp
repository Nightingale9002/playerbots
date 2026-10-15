
#include "playerbot/playerbot.h"
#include "InvalidTargetValue.h"
#include "PossibleAttackTargetsValue.h"
#include "EnemyPlayerValue.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/ServerFacade.h"

using namespace ai;

bool InvalidTargetValue::Calculate()
{
    Unit* target = ai->GetUnit(AI_VALUE(ObjectGuid, qualifier));
    if (!target || !target->IsInWorld() || target->GetMapId() != bot->GetMapId())
    {
        return true;
    }

    Unit* duelTarget = ai->GetUnit(AI_VALUE(ObjectGuid, "duel target"));
    if (duelTarget && duelTarget == target)
    {
        return false;
    }

    if (qualifier == "current target")
    {
        if (target->GetObjectGuid() != bot->GetSelectionGuid())
        {
            return true;
        }

        // [pull 2026-10-18 站长口径] 坦克的目标如果"明显在另一层"就当无效目标丢掉（InvalidTargetTrigger
        //   ⇒ select new target ⇒ tank assist 重新挑，见 TankTargetValue.cpp）。不丢的话它会一直"直奔"
        //   那个走不到的目标（多楼层副本：卡拉赞 / 黑石深渊 / 厄运之槌）。
        //   阈值取 2 倍同层阈值：上下楼梯的高度抖动不该让坦克丢目标。
        const float abandonZDistance = sPlayerbotAIConfig.pullSameLevelZDistance * 2.0f;
        if (ai->IsTank(bot) && abandonZDistance > 0.0f && target->GetMapId() == bot->GetMapId() &&
            fabs(bot->GetPositionZ() - target->GetPositionZ()) > abandonZDistance)
        {
            return true;
        }
    }

    const bool validTarget = PossibleAttackTargetsValue::IsValid(target, bot);
    if (!validTarget)
    {
        std::list<ObjectGuid> attackers = AI_VALUE(std::list<ObjectGuid>, "possible attack targets");
        if (std::find(attackers.begin(), attackers.end(), target->GetObjectGuid()) != attackers.end())
        {
            return false;
        }
    }

    return !validTarget;
}