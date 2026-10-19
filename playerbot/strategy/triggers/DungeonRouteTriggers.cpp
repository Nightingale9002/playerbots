
#include "playerbot/playerbot.h"
#include "playerbot/DungeonRoute.h"
#include "DungeonRouteTriggers.h"

using namespace ai;

// [2026-10-18 站长口径] "打完最近的一波之后就不会继续推进"：
//   如果当前波**没有任何可打目标**（友好 NPC / 小动物 / 怪全在远处没加载），
//   旧逻辑是 FindTarget 直接 false ⇒ 两个触发器都不激活 ⇒ 坦克原地站着。
//   这里加"自动跳过"：等 WAVE_NOT_LOADED_MS 就把这一波标记为放弃，ord 自然推进到下一波。
static bool AutoSkipIfNoTarget(Player* bot, dungeonroute::MapRoute const& route, bool hasTarget)
{
    if (hasTarget)
        return false;

    uint16 const ord = dungeonroute::CurrentOrd(bot, route);
    if (ord && dungeonroute::WaveStuckMs(bot) > dungeonroute::WAVE_NOT_LOADED_MS)
        dungeonroute::MarkSkipped(bot, ord, "wave no valid target");
    return true;
}

bool DungeonRouteMoveTrigger::IsActive()
{
    if (!dungeonroute::ShouldDrive(ai))
        return false;

    Map* map = bot->GetMap();
    dungeonroute::MapRoute const* route = dungeonroute::GetMapRoute(map->GetId());
    if (!route || !route->verified)
        return false;

    dungeonroute::Target target;
    bool const hasTarget = dungeonroute::FindTargetNearest(bot, *route, target) && target.hasPullPoint;
    if (AutoSkipIfNoTarget(bot, *route, hasTarget))
        return false;

    return bot->GetDistance(target.px, target.py, target.pz) > dungeonroute::AT_PULL_POINT_DIST;
}

bool DungeonRoutePullTrigger::IsActive()
{
    if (!dungeonroute::ShouldDrive(ai))
        return false;

    Map* map = bot->GetMap();
    dungeonroute::MapRoute const* route = dungeonroute::GetMapRoute(map->GetId());
    if (!route || !route->verified)
        return false;

    dungeonroute::Target target;
    bool const hasTarget = dungeonroute::FindTargetNearest(bot, *route, target) && target.hasPullPoint;
    if (AutoSkipIfNoTarget(bot, *route, hasTarget))
        return false;

    return bot->GetDistance(target.px, target.py, target.pz) <= dungeonroute::AT_PULL_POINT_DIST;
}
