
#include "playerbot/playerbot.h"
#include "playerbot/DungeonRoute.h"
#include "DungeonRouteActions.h"

using namespace ai;

bool DungeonRouteMoveAction::isPossible()
{
    return dungeonroute::ShouldDrive(ai);
}

bool DungeonRouteMoveAction::Execute(Event& event)
{
    if (!dungeonroute::ShouldDrive(ai))
        return false;

    Map* map = bot->GetMap();
    dungeonroute::MapRoute const* route = dungeonroute::GetMapRoute(map->GetId());
    if (!route)
        return false;
    if (!route->verified && !dungeonrun::AllowUnverified(dungeonroute::RouteRuntimeSwitch()))
        return false;

    // [2026-10-18 站长口径] 拉怪记录已由核心统一采集（所有玩家/所有副本图），
    //   这里不再做"每 tick 收尾"；本文件只在真的起手时补一个 ord 提示。

    // 卡住保护：同一波超过 WAVE_GIVE_UP_MS 没有推进（且没在打）⇒ 放弃这波，下次瞄下一波
    uint16 ord = dungeonroute::CurrentOrd(bot, *route);
    if (ord && dungeonroute::WaveStuckMs(bot) > dungeonroute::WAVE_GIVE_UP_MS &&
        !dungeonroute::WaveInFight(bot, *route, ord))
    {
        dungeonroute::MarkSkipped(bot, ord, "wave stuck: no progress");
        return false;
    }

    dungeonroute::Target target;
    if (!dungeonroute::FindTargetNearest(bot, *route, target) || !target.hasPullPoint)
        return false;

    if (bot->GetDistance(target.px, target.py, target.pz) <= dungeonroute::AT_PULL_POINT_DIST)
        return false;    // 已到位 ⇒ 交给 DungeonRoutePullAction

    // 等人：主人（真人）没跟上就先别冲（返回 true 表示"本轮我处理了，别做别的移动"）
    if (!dungeonroute::ReadyToEngage(ai, 60.0f))
        return true;

    // [2026-10-18] 曾经加过"走 8 秒位置没动就放弃该波"的守卫，但实测在**正常赶路/游泳/等队友**时
    //   会误判（坦克跨 100 码移动途中被它跳过 3 个波）。不可达的波交给已有的 90 秒
    //   `WAVE_GIVE_UP_MS` + 仿真 stuck 兜底；这里只保留"到位后拉不到"的 5 秒跳过（PullAction）。

    // [2026-10-19 站长口径] boss 战后开关（蒸汽地窟门禁面板 / 毒蛇神殿桥控制台）：
    //   属于 boss 策略——对应 boss 打死后才去点，不是通用"卡住就开锁"。
    {
        dungeonroute::BossSwitch sw;
        if (dungeonroute::FindPendingBossSwitch(bot, *route, sw))
        {
            float const dist = bot->GetDistance(sw.x, sw.y, sw.z);
            if (dist > 4.0f)
                return MoveTo(map->GetId(), sw.x, sw.y, sw.z);

            GameObject* go = map->GetGameObject(sw.goGuid);
            if (go)
            {
                go->Use(bot);
                dungeonroute::StateOf(bot).switchUsed.insert(sw.goGuid);
                sLog.outString("[DUNGEONROUTE] %s 点了 boss 开关 %s（guid=%u goEntry=%u，boss=%u）%s",
                    bot->GetName(), go->GetGOInfo()->name, sw.goGuid, sw.goEntry, sw.bossEntry,
                    sw.note.c_str());
                return true;
            }

            // 网格还没加载到开关：等最多 10 秒，避免整队卡死在"等一个不存在的 GO"
            dungeonroute::BotState& st = dungeonroute::StateOf(bot);
            uint32 const nowMs = WorldTimer::getMSTime();
            if (!st.switchWaitAt)
                st.switchWaitAt = nowMs ? nowMs : 1;
            else if (WorldTimer::getMSTimeDiff(st.switchWaitAt, nowMs) > 10000)
            {
                st.switchUsed.insert(sw.goGuid);
                sLog.outString("[DUNGEONROUTE] %s 等不到 boss 开关 guid=%u（网格未加载）⇒ 先跳过",
                    bot->GetName(), sw.goGuid);
            }
            return true;
        }
    }

    // [2026-10-19] ① 跳点 / 升降台连接点（数据表 ai_playerbot_route_link）
    if (dungeonroute::TryRouteLink(bot, target.px, target.py, target.pz))
        return true;

    // [2026-10-19] 跳跃推进的"保护期"：跳跃样条还没走完时，不要再用 MoveTo 覆盖它
    //   （否则每 tick 都被无路径的 MoveTo 打断，跳跃原地不动——558 实测）。
    {
        dungeonroute::BotState& st = dungeonroute::StateOf(bot);
        uint32 const nowMs = WorldTimer::getMSTime();
        if (st.jumpUntil && nowMs < st.jumpUntil)
            return true;
    }

    // [2026-10-19] ② 卡住自救：位置 2.5 秒没动，且目标不远/同层 ⇒ 朝目标小跳一次（矮台/碎石/缺口）
    if (sPlayerbotAIConfig.routeJumpAssist)
    {
        dungeonroute::BotState& st = dungeonroute::StateOf(bot);
        uint32 const now = WorldTimer::getMSTime();
        float const bx = bot->GetPositionX();
        float const by = bot->GetPositionY();
        float const bz = bot->GetPositionZ();

        float const dx = bx - st.moveLastX;
        float const dy = by - st.moveLastY;
        float const dz = bz - st.moveLastZ;
        if (!st.moveProgressAt || (dx * dx + dy * dy + dz * dz) > 6.25f)   // 移动了 >2.5 码 = 有进展
        {
            st.moveLastX = bx;
            st.moveLastY = by;
            st.moveLastZ = bz;
            st.moveProgressAt = now ? now : 1;
        }
        else if (WorldTimer::getMSTimeDiff(st.moveProgressAt, now) > 2500)
        {
            st.moveProgressAt = now;
            float const dist = bot->GetDistance(target.px, target.py, target.pz);
            if (dist > 4.0f)
            {
                // [2026-10-19] 远处也自救：navmesh 缺块时朝目标"跳跃推进"（每次 ~8 码，2.5 秒一次），
                //   否则大距离（>25 码）会一直不算卡住、原地站到 90 秒超时。
                float vx = target.px - bx;
                float vy = target.py - by;
                float len = std::sqrt(vx * vx + vy * vy);
                if (len < 1.0f)
                    len = 1.0f;
                float const step = std::min(10.0f, std::max(2.0f, dist - 2.0f));
                float const jx = bx + vx / len * step;
                float const jy = by + vy / len * step;
                float const jz = (std::fabs(target.pz - bz) <= 8.0f) ? target.pz : bz;
                bot->GetMotionMaster()->MoveJump(jx, jy, jz, 9.0f, 4.0f);
                st.jumpUntil = now + 1500;
                sLog.outString("[DUNGEONROUTE] %s 卡住 ⇒ 朝目标跳跃推进 %.0f 码（dist=%.1f dz=%.1f）",
                    bot->GetName(), step, dist, target.pz - bz);
                return true;
            }
        }
    }

    return MoveTo(map->GetId(), target.px, target.py, target.pz);
}

bool DungeonRoutePullAction::isPossible()
{
    return dungeonroute::ShouldDrive(ai);
}

bool DungeonRoutePullAction::Execute(Event& event)
{
    if (!dungeonroute::ShouldDrive(ai))
        return false;

    Map* map = bot->GetMap();
    dungeonroute::MapRoute const* route = dungeonroute::GetMapRoute(map->GetId());
    if (!route)
        return false;
    if (!route->verified && !dungeonrun::AllowUnverified(dungeonroute::RouteRuntimeSwitch()))
        return false;

    // [2026-10-18 站长口径] 拉怪记录由核心统一采集（见 DungeonRoute.h §拉怪记录）

    dungeonroute::Target target;
    if (!dungeonroute::FindTargetNearest(bot, *route, target) || !target.hasPullPoint)
        return false;

    if (bot->GetDistance(target.px, target.py, target.pz) > dungeonroute::AT_PULL_POINT_DIST)
        return false;    // 还没到位 ⇒ 交给 DungeonRouteMoveAction

    Creature* creature = map->GetCreature(target.guid);
    if (!creature || !creature->IsAlive() || !creature->IsInWorld())
    {
        // [2026-10-18 站长口径] "打完最近一波不推进"：目标是**还没加载**的怪/boss
        //   （FindTarget 会先给波中心/走位点）⇒ 站在波中心等网格加载；等 WAVE_NOT_LOADED_MS 还没有就放弃这波。
        if (dungeonroute::WaveStuckMs(bot) > dungeonroute::WAVE_NOT_LOADED_MS)
        {
            if (target.boss)
                dungeonroute::MarkBossSkipped(bot, target.guid);
            else
            {
                uint16 const ord = dungeonroute::CurrentOrd(bot, *route);
                if (ord)
                    dungeonroute::MarkSkipped(bot, ord, "wave not loaded");
            }
        }
        return false;
    }

    // 等人：拉怪前主人至少要跟到附近（真人玩家的距离门控）
    if (!dungeonroute::ReadyToEngage(ai, 45.0f))
        return true;

    Event pullEvent("attack anything", creature->GetObjectGuid());
    if (ai->DoSpecificAction("pull my target", pullEvent, true))
    {
        // [2026-10-18 站长口径] 真的起手了 ⇒ 给核心记录器补一个 ord 提示（记录/收尾都在核心，见 DungeonRoute.h §拉怪记录）
        dungeonroute::PullBegin(bot, *route, target);
        return true;
    }

    // [仿真 2026-10-18] 拉怪链路可能整条不可用：
    //   `PullStrategy::Get(ai)` 只认引擎里的 "pull" 策略（真·防护坦的默认策略才有），
    //   或者坦克没有远程武器 ⇒ `CanDoPullAction()` false ⇒ `pull my target` 永远返回 false，
    //   表现就是"站在拉怪位一动不动、永远不开怪"。
    //   这里退回直接 "attack anything"（走过去打）；起手记录照样补 ord，路线推进不至于卡死。
    if (ai->DoSpecificAction("attack anything", pullEvent, true))
    {
        sLog.outString("[DUNGEONROUTE] %s 拉怪链路不可用，退回直接开怪（entry=%u）",
            bot->GetName(), creature->GetEntry());
        dungeonroute::PullBegin(bot, *route, target);
        return true;
    }

    return false;
}
