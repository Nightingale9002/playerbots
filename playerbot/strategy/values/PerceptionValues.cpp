#include "playerbot/playerbot.h"
#include "PerceptionValues.h"

#include "MotionGenerators/PathFinder.h"
#include "playerbot/ServerFacade.h"

#include "Grids/GridNotifiers.h"
#include "Grids/GridNotifiersImpl.h"
#include "Grids/CellImpl.h"

using namespace ai;

namespace
{
    // 感知层统一的目标取法。
    //   注意两点：
    //   ① `current target` 存的是 ObjectGuid（UnitCalculatedValue），
    //      用 AI_VALUE(Unit*, "current target") 会 dynamic_cast<Value<Unit*>*> 失败 ⇒ 永远 nullptr；
    //   ② AI_VALUE 宏依赖 Action/Trigger 里的 `context` 成员，**Value 类里没有** ⇒ 直接用
    //      全局的 ::AiValueOr<ObjectGuid>()（自带判空，名字没注册也不会崩）。
    Unit* PerceptionCurrentTarget(PlayerbotAI* ai)
    {
        if (!ai)
            return nullptr;

        AiObjectContext* context = ai->GetAiObjectContext();
        if (!context)
            return nullptr;

        return ai->GetUnit(::AiValueOr<ObjectGuid>(context, "current target"));
    }

    bool PerceptionBotUsable(Player* bot)
    {
        return bot && bot->IsInWorld() && bot->GetMap();
    }
}

// ---------------------------------------------------------------------------------------------
uint8 NearbyHostilesValue::Calculate()
{
    if (!PerceptionBotUsable(bot))
        return 0;

    // 限定词 = 半径；上限 60 码是刻意的：这个值是给"近处有没有一群怪"用的，
    // 传更大的半径没有语义，只会让扫描越来越贵。
    float radius = 10.0f;
    if (!getQualifier().empty())
    {
        float const qualifierRadius = atof(getQualifier().c_str());
        if (qualifierRadius > 0.0f)
            radius = std::min(qualifierRadius, 60.0f);
    }

    std::list<Unit*> targets;
    MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck u_check(bot, radius);
    MaNGOS::UnitListSearcher<MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck> searcher(targets, u_check);
    Cell::VisitAllObjects(bot, searcher, radius);

    if (targets.size() > 255)
        return 255;

    return (uint8)targets.size();
}

// ---------------------------------------------------------------------------------------------
bool WallBetweenValue::Calculate()
{
    if (!PerceptionBotUsable(bot))
        return false;

    Unit* target = PerceptionCurrentTarget(ai);
    if (!target || !target->IsInWorld())
        return false;

    return !bot->IsWithinLOSInMap(target);
}

// ---------------------------------------------------------------------------------------------
bool TargetReachableValue::Calculate()
{
    if (!PerceptionBotUsable(bot))
        return true;

    Unit* target = PerceptionCurrentTarget(ai);
    if (!target || !target->IsInWorld())
        return true;

    // 只看近处：远距离的"可达性"由 travel 逻辑负责，这里算既贵又无意义。
    if (target->GetMapId() != bot->GetMapId() || target->GetInstanceId() != bot->GetInstanceId())
        return true;

    if (bot->GetDistance(target) > 60.0f)
        return true;

    PathFinder pathFinder(bot);
    pathFinder.calculate(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ());

    // 只把"完全没有路"当作不可达。PATHFIND_INCOMPLETE 是"能更近一步"，仍算可达。
    return !(pathFinder.getPathType() & PATHFIND_NOPATH);
}

// ---------------------------------------------------------------------------------------------
bool StandingInWaterValue::Calculate()
{
    if (!PerceptionBotUsable(bot))
        return false;

    return bot->IsInWater();
}

// ---------------------------------------------------------------------------------------------
float SlopeToTargetValue::Calculate()
{
    if (!PerceptionBotUsable(bot))
        return 0.0f;

    Unit* target = PerceptionCurrentTarget(ai);
    if (!target || !target->IsInWorld())
        return 0.0f;

    return target->GetPositionZ() - bot->GetPositionZ();
}
