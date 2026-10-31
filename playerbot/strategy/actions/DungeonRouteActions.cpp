
#include "playerbot/playerbot.h"
#include "playerbot/DungeonRoute.h"
#include "playerbot/CoverSpot.h"   // 离线预筛的卡视野点库
#include "DungeonRouteActions.h"
#include "DungeonTelemetry.h"           // [2026-11-01] 诊断日志节流 + 结构化（PULLWHY/PULLSAFE/PULLCOST/MOVETGT/PULLDIAG）
#include "MotionGenerators/MoveMap.h"   // [2026-10-25] 卡视野改用**导航网格**判墙面
#include "MotionGenerators/WaypointManager.h"        // [2026-10-26] 巡逻怪按路径推演
#include "MotionGenerators/WaypointMovementGenerator.h"

using namespace ai;

namespace dungeonroute
{
    // [2026-10-26 站长口径] 巡逻怪的位置预测**按巡逻路径**走，不按当前朝向直线外推：
    //   直线外推在拐角/折返处必然推错 ⇒ PullAdmissionDelayMs 永远能"预测"出一个更便宜的时刻，
    //   实际却不会发生（r100：整组在拉怪点等 1100s 再被兜底重置）。
    void PredictMobPosPath(Creature* c, float dtSec, float& x, float& y)
    {
        x = c->GetPositionX();
        y = c->GetPositionY();
        if (!c->GetMotionMaster() || c->IsStopped() || dtSec <= 0.0f)
            return;
        float const sp = c->GetSpeed(MOVE_RUN);
        if (sp <= 0.01f)
            return;
        uint8 const mt = c->GetMotionMaster()->GetCurrentMovementGeneratorType();
        if (mt != WAYPOINT_MOTION_TYPE && mt != RANDOM_MOTION_TYPE && mt != POINT_MOTION_TYPE)
            return;

        if (mt == WAYPOINT_MOTION_TYPE)
        {
            auto* wmg = dynamic_cast<WaypointMovementGenerator<Creature>*>(c->GetMotionMaster()->top());
            if (wmg)
            {
                uint32 pathId = 0;
                WaypointPathOrigin origin = PATH_NO_PATH;
                wmg->GetPathInformation(pathId, origin);
                if (WaypointPath* path = sWaypointMgr.GetPathFromOrigin(c->GetEntry(), c->GetDbGuid(),
                        int32(pathId), origin))
                {
                    if (!path->empty())
                    {
                        // 从当前节点往后，沿折线走 sp*dt 的路程；走到尾就回绕（循环巡逻）
                        float remain = sp * dtSec;
                        auto it = path->upper_bound(wmg->GetCurrentNode());
                        if (it == path->end())
                            it = path->begin();
                        float cx = x, cy = y;
                        size_t guard = 0;
                        while (remain > 0.05f && guard++ < path->size() * 2 + 4)
                        {
                            WaypointNode const& n = it->second;
                            float const dx = n.x - cx, dy = n.y - cy;
                            float const d = std::sqrt(dx * dx + dy * dy);
                            if (d <= 0.05f)
                            {
                                if (++it == path->end())
                                    it = path->begin();
                                continue;
                            }
                            if (d >= remain)
                            {
                                cx += dx / d * remain;
                                cy += dy / d * remain;
                                remain = 0.0f;
                            }
                            else
                            {
                                cx = n.x;
                                cy = n.y;
                                remain -= d;
                                if (++it == path->end())
                                    it = path->begin();
                            }
                        }
                        x = cx;
                        y = cy;
                        return;
                    }
                }
            }
        }

        // 兜底（RANDOM/POINT/取不到路径）：仍按当前朝向直线外推
        x += std::cos(c->GetOrientation()) * sp * dtSec;
        y += std::sin(c->GetOrientation()) * sp * dtSec;
    }
}


// [2026-10-25 站长口径] 用寻路网格的边缘找墙：一个点"走不出去"（离最近多边形 > 2 码）就算墙/障碍。
//   这比打视线射线准 —— 视觉上能看穿的门洞、栏杆不会误判成墙，而真正的墙一定不在网格上。
static bool OnNavMesh(Map* map, float x, float y, float z)
{
    if (!map)
        return false;
    dtNavMeshQuery const* q =
        MMAP::MMapFactory::createOrGetMMapManager()->GetNavMeshQuery(map->GetId(), map->GetInstanceId());
    if (!q)
        return false;

    // ⚠️ 坐标顺序：CMaNGOS 的 mmap 顶点按 (y, z, x) 存（同 `PathFinder::getArea` 的 `point[3]={y,z,x}`），
    //   传 {x,y,z} ⇒ findNearestPoly 永远返回 INVALID_POLYREF（实测 1256 个候选全"无多边形"）。
    dtQueryFilter filter;
    dtPolyRef ref = 0;
    float const pt[3] = {y, z, x};
    float closest[3] = {0.0f, 0.0f, 0.0f};
    float const extents[3] = {3.0f, 5.0f, 3.0f};
    q->findNearestPoly(pt, extents, &filter, &ref, closest);
    if (!ref)
        return false;
    float const gx = closest[2], gy = closest[0], gz = closest[1];   // 换回游戏坐标
    float const dx = gx - x;
    float const dy = gy - y;
    float const dz = gz - z;
    return (dx * dx + dy * dy + dz * dz) <= 4.0f;   // 2 码以内算"站在可行走面上"
}

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

    // [2026-10-23 站长口径] 本图"要躲的怪"（无敌+击飞类）灌进引擎自带的 avoid creature list，
    //   由 avoid specific creatures 策略负责"move away"（和躲地板同一条链路），不再去拉它。
    if (!route->avoidEntries.empty())
    {
        std::set<uint32>& avoid = AI_VALUE(std::set<uint32>&, "avoid creature list");
        for (uint32 e : route->avoidEntries)
            avoid.insert(e);
    }

    // [2026-10-18 站长口径] 拉怪记录已由核心统一采集（所有玩家/所有副本图），
    //   这里不再做"每 tick 收尾"；本文件只在真的起手时补一个 ord 提示。

    dungeonroute::Target target;
    if (!dungeonroute::FindTargetNearest(bot, *route, target, true) || !target.hasPullPoint)
        return false;

    // [2026-10-26 站长口径] 跳波前的"正在走近"豁免：
    //   实测 459 次跳波里有 49% 发生在离拉怪位 60+ 码的路上（中位 51 码），90 秒一到就把这一波扔了，
    //   然后级联跳完整张图、整组空转。这里把"离拉怪位的距离在缩短"当作活进度。
    bool approaching = false;
    {
        dungeonroute::BotState& wst = dungeonroute::StateOf(bot);
        uint32 const nowW = WorldTimer::getMSTime() ? WorldTimer::getMSTime() : 1;
        float const dNow = bot->GetDistance(target.px, target.py, target.pz);
        if (wst.waveLastDist >= 0.0f && wst.waveLastDistAt)
        {
            if (WorldTimer::getMSTimeDiff(wst.waveLastDistAt, nowW) >= 2000)
            {
                if (dNow < wst.waveLastDist - 3.0f)
                    wst.waveApproachAt = nowW;      // 真的在靠近
                wst.waveLastDist = dNow;
                wst.waveLastDistAt = nowW;
            }
        }
        else
        {
            wst.waveLastDist = dNow;
            wst.waveLastDistAt = nowW;
        }
        approaching = wst.waveApproachAt &&
            WorldTimer::getMSTimeDiff(wst.waveApproachAt, nowW) < 20000;
    }

    // [2026-10-26 站长指令] **删掉 90 秒跳波兜底**：不再"同一波 90 秒没推进就放弃"。
    //   理由：这是时间兜底，把"正在走过去 / 正在处理别的怪"统统当成卡死，一路级联跳完整张图
    //   （r111 实测 459 次跳波，其中 49% 发生在离拉怪位 60+ 码的路上），真正的根因被掩盖了。
    //   现在改成：只保留"在打/在靠近"的观测（下面 approaching 记着），**不再跳波**；
    //   卡死由外层的整轮 stuck（仿真 1800s）+ 日志诊断暴露，而不是在这里偷偷丢波。
    uint16 ord = dungeonroute::CurrentOrd(bot, *route);
    (void)approaching;

    // [2026-10-25 诊断] 越界：同 map 但人站在导航网格之外（被击飞掉出桥面 / 掉下地形 / 被传到无效坐标）。
    //   这种状态下 PathFinder 必然算不出路，表现是"整组原地不动、act=check mail"。
    //   这里**只写日志**；"击飞兜底"放在 SimScenario 里做（静止 + 离队长太远才救）。
    //   [2026-10-25 站长口径] **不用 z 判水域**：游泳状态取移动标志 MOVEFLAG_SWIMMING（不是液体高度/z 比较）。
    {
        dungeonroute::BotState& ost = dungeonroute::StateOf(bot);
        uint32 const nowO = WorldTimer::getMSTime();
        if (!ost.oobLogAt || WorldTimer::getMSTimeDiff(ost.oobLogAt, nowO ? nowO : 1) >= 10000)
        {
            ost.oobLogAt = nowO ? nowO : 1;
            dtNavMeshQuery const* oq = MMAP::MMapFactory::createOrGetMMapManager()
                ->GetNavMeshQuery(map->GetId(), map->GetInstanceId());
            if (!oq)
                ost.oobStrikes = 0;                    // mmaps 还没加载：不下结论
            else if (OnNavMesh(map, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()))
                ost.oobStrikes = 0;
            else if (++ost.oobStrikes >= 3)            // 连续 3 次(≈30s)在网格外才记，避开入口落地那几秒
            {
                bool const swimming = bot->m_movementInfo.HasMovementFlag(MOVEFLAG_SWIMMING);
                sLog.outError("[OOB] bot=%s map=%u inst=%u pos=(%.1f,%.1f,%.1f) curOrd=%u tgt=%u ord=%u "
                    "dist_tgt=%.0f inCombat=%d swim=%d onNavMesh=0 strikes=%u",
                    bot->GetName(), map->GetId(), map->GetInstanceId(),
                    bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                    uint32(dungeonroute::CurrentOrd(bot, *route)), target.guid, uint32(target.ord),
                    bot->GetDistance(target.px, target.py, target.pz), bot->IsInCombat() ? 1 : 0,
                    swimming ? 1 : 0, uint32(ost.oobStrikes));
            }
        }
    }

    {
        // [2026-10-25 修] PULLSAFE 的安全点也算"到位"：不这么判，MoveAction 会把坦克从安全点
        //   推回原拉怪位，到原拉怪位 PullAction 又送它回安全点 ⇒ 两个动作互顶、永远不开怪。
        float ex = target.px, ey = target.py, ez = target.pz;
        dungeonroute::SafePullPoint(bot, target, ex, ey, ez);
        if (bot->GetDistance(ex, ey, ez) <= dungeonroute::AT_PULL_POINT_DIST ||
            bot->GetDistance(target.px, target.py, target.pz) <= dungeonroute::AT_PULL_POINT_DIST)
            return false;    // 已到位 ⇒ 交给 DungeonRoutePullAction
    }

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
                sLog.outError("[DUNGEONROUTE] %s 点了 boss 开关 %s（guid=%u goEntry=%u，boss=%u）%s",
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
                sLog.outError("[DUNGEONROUTE] %s 等不到 boss 开关 guid=%u（网格未加载）⇒ 先跳过",
                    bot->GetName(), sw.goGuid);
            }
            return true;
        }
    }

    // [2026-10-24 诊断] 队伍到底在往哪一波走（553/557 卡"目标很远"时看这一行）
    {
        dungeonroute::BotState& st2 = dungeonroute::StateOf(bot);
        uint32 const n2 = WorldTimer::getMSTime();
        if (!st2.moveTgtLogAt || WorldTimer::getMSTimeDiff(st2.moveTgtLogAt, n2 ? n2 : 1) >= 5000)
        {
            st2.moveTgtLogAt = n2 ? n2 : 1;
            dtelemetry::Event("MOVETGT", dungeonroute::TeleKeyWave(map->GetId(), target.ord, bot),
                "%s ord=%u guid=%u boss=%d dist=%.1f lockOrd=%u curOrd=%u",
                bot->GetName(), uint32(target.ord), target.guid, target.boss ? 1 : 0,
                bot->GetDistance(target.px, target.py, target.pz), uint32(st2.lockOrd),
                uint32(dungeonroute::CurrentOrd(bot, *route)));
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

    // [2026-10-19 站长口径] 完全取消自动“跳跃推进”，统一 mmap 寻路：
    //   目标太远/不同层时不跳，交给 mmap；真走不过去由卡住保护/波次放弃处理。
    // [2026-10-23 站长口径] 不完整路径的收束：
    //   以前"走到不完整路径末端"就到头了，没有后续判定 ⇒ 队伍会朝一个永远到不了的目标无限走
    //   （实测 552 三组追 140–170 码外目标 20 分钟）。
    //   现在：每 1.5 秒算一次路径；
    //     · 路径不完整（末端离目标 > 容差）且**还没到末端** ⇒ 先走到末端；
    //     · 已在末端（<=8 码）再算一次仍不完整 ⇒ 判定"走到头也不可达"，把该波/boss 跳过。
    {
        dungeonroute::BotState& st = dungeonroute::StateOf(bot);
        uint32 const nowMs = WorldTimer::getMSTime();
    bool const needCheck = (st.pathTargetGuid != target.guid) || !st.pathCheckAt ||
            WorldTimer::getMSTimeDiff(st.pathCheckAt, nowMs ? nowMs : 1) >= 1500;
        if (needCheck)
        {
            st.pathTargetGuid = target.guid;
            st.pathCheckAt = nowMs ? nowMs : 1;

            PathFinder pf(bot);
            bool const ok = pf.calculate(target.px, target.py, target.pz);
            float endDist = 1e9f, ex = target.px, ey = target.py, ez = target.pz;
            if (ok)
            {
                auto& pts = pf.getPath();
                if (!pts.empty())
                {
                    Vector3 const& e = pts.back();
                    ex = e.x; ey = e.y; ez = e.z;
                    float const dx = e.x - target.px, dy = e.y - target.py, dz = e.z - target.pz;
                    endDist = std::sqrt(dx * dx + dy * dy + dz * dz);
                }
            }
            float const tol = dw::Get("boss.path_tolerance_yd");
            if (!ok || endDist > tol)
            {
                float const toEnd = bot->GetDistance(ex, ey, ez);
                if (toEnd > 8.0f)
                    return MoveTo(map->GetId(), ex, ey, ez);      // 先走到不完整路径的末端

                // 已在末端：再确认一次（从末端能不能走到目标）
                PathFinder pf2(bot);
                bool const ok2 = pf2.calculate(target.px, target.py, target.pz);
                float end2 = 1e9f;
                if (ok2)
                {
                    auto& p2 = pf2.getPath();
                    if (!p2.empty())
                    {
                        Vector3 const& e2 = p2.back();
                        float const dx = e2.x - target.px, dy = e2.y - target.py, dz = e2.z - target.pz;
                        end2 = std::sqrt(dx * dx + dy * dy + dz * dz);
                    }
                }
                if (ok2 && end2 <= tol)
                    return MoveTo(map->GetId(), target.px, target.py, target.pz);

                // [2026-10-24 站长口径] 已删除 DIRECTMOVE 兜底：直线 MovePoint 只是掩盖问题，不解决"
                //   "mmap 不可达"这个根因。不可达的怪应该在数据层**一开始就跳过**（ai_playerbot_unreachable）。

                if (target.boss)
                {
                    dungeonroute::MarkBossSkipped(bot, target.guid);
                    sLog.outError("[DUNGEONROUTE] %s boss=%u 走到路径末端仍不可达（endDist=%.0f）⇒ 跳过",
                        bot->GetName(), target.guid, endDist);
                }
                else if (target.ord)
                {
                    dungeonroute::MarkSkipped(bot, target.ord, "path dead end");
                    sLog.outError("[DUNGEONROUTE] %s 波 ord=%u 走到路径末端仍不可达（endDist=%.0f）⇒ 跳过",
                        bot->GetName(), uint32(target.ord), endDist);
                }
                else
                {
                    // [2026-10-25 修] target.guid == 0 ⇒ 这是"固化路线的路点"（不是怪本身）。
                    //   走到路径末端还到不了 ⇒ 把这个路点拉黑跳过，下次换下一个点，
                    //   否则坦克会每 tick 盯同一个到不了的点、原地钉死（实测 dist 一直不变）。
                    if (target.guid == 0)
                    {
                        dungeonroute::SkipPathNodeAt(map->GetId(), target.px, target.py, target.pz);
                        return false;
                    }
                    if (Creature* cc = map->GetCreature(target.guid))
                    {
                        if (!st.deadEndLogAt ||
                            WorldTimer::getMSTimeDiff(st.deadEndLogAt, WorldTimer::getMSTime()) >= 5000)
                        {
                            st.deadEndLogAt = WorldTimer::getMSTime() ? WorldTimer::getMSTime() : 1;
                            sLog.outError("[DUNGEONROUTE] %s 拉怪位不可达（endDist=%.0f）⇒ 改为朝怪本体走 %s(entry=%u guid=%u)",
                                bot->GetName(), endDist, cc->GetName(), cc->GetEntry(), target.guid);
                        }
                        return MoveTo(map->GetId(), cc->GetPositionX(), cc->GetPositionY(), cc->GetPositionZ());
                    }
                    sLog.outError("[DUNGEONROUTE] %s 拉怪位不可达且怪已不在网格 guid=%u ⇒ 换目标",
                        bot->GetName(), target.guid);
                }
                return false;
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
    // [2026-10-23 站长指令] 只读诊断：为什么"一次都没开怪"（555 影子迷宫实测 0 次起手）。
    //   每个早退点打一个 reason（每 3 秒最多一条），不改行为。
    dungeonroute::BotState& dbgSt = dungeonroute::StateOf(bot);
    uint32 const dbgNow = WorldTimer::getMSTime();
    bool const dbgOk = !dbgSt.pullDbgAt || WorldTimer::getMSTimeDiff(dbgSt.pullDbgAt, dbgNow ? dbgNow : 1) >= 3000;
    auto dbgReason = [&](const char* why, float d = -1.0f, uint32 ord = 0)
    {
        if (!dbgOk)
            return;
        dbgSt.pullDbgAt = dbgNow ? dbgNow : 1;
        dtelemetry::Event("PULLWHY", dungeonroute::TeleKeyBot(bot),
            "bot=%s reason=%s dist=%.1f ord=%u combat=%d",
            bot->GetName(), why, d, ord, bot->IsInCombat() ? 1 : 0);
    };

    if (!dungeonroute::ShouldDrive(ai))
    {
        dbgReason("not_driver");
        return false;
    }

    Map* map = bot->GetMap();
    dungeonroute::MapRoute const* route = dungeonroute::GetMapRoute(map->GetId());
    if (!route)
    {
        dbgReason("no_route");
        return false;
    }
    if (!route->verified && !dungeonrun::AllowUnverified(dungeonroute::RouteRuntimeSwitch()))
    {
        dbgReason("route_unverified");
        return false;
    }

    // [2026-10-23 站长口径] 同 MoveAction：把"要躲的怪"灌进 avoid creature list（躲地板链路）
    if (!route->avoidEntries.empty())
    {
        std::set<uint32>& avoid = AI_VALUE(std::set<uint32>&, "avoid creature list");
        for (uint32 e : route->avoidEntries)
            avoid.insert(e);
    }

    // [2026-10-18 站长口径] 拉怪记录由核心统一采集（见 DungeonRoute.h §拉怪记录）

    // [2026-10-24 站长口径] 拉完/挪位后的站位优先：生效期间**不再回原来的拉怪位**
    // [2026-10-25 修] 拆分两种情形：
    //   ① 战斗中：保持原行为（我方的挪位点优先，别跑回旧拉怪位）；
    //   ② 非战斗 + PULLSAFE 安全点：先走到安全点，**站到了就往下正常拉怪**（安全点=本次拉怪位）。
    //   旧逻辑在安全点上直接 return false，而"离安全点>3 码"又送回安全点 ⇒ 和 MoveAction 的
    //   "回原拉怪位"互相顶死（r92 557 Pandemonius 单组 472s 没开一次怪）。
    {
        dungeonroute::BotState& ost = dungeonroute::StateOf(bot);
        uint32 const nowO = WorldTimer::getMSTime();
        if (ost.standOverrideUntil && nowO < ost.standOverrideUntil)
        {
            float const dOverride =
                bot->GetDistance(ost.standOverrideX, ost.standOverrideY, ost.standOverrideZ);
            if (dOverride >= 80.0f)
            {
                // 离得太远（通常是被兜底重置传回入口、或已经换了波）：这个点作废，继续正常流程。
                //   旧代码在这里 return false，等于让坦克站满 2 分钟不动。
                ost.standOverrideUntil = 0;
                ost.safePullPoint = false;
            }
            else if (dOverride > 3.0f)
            {
                // 还没站上去：先走到安全点/挪位点
                bot->GetMotionMaster()->MovePoint(0, ost.standOverrideX, ost.standOverrideY,
                    ost.standOverrideZ);
                if (Unit* v = bot->GetVictim())
                    bot->SetFacingToObject(v);
                return true;
            }
            else if (!ost.safePullPoint || bot->IsInCombat())
            {
                // 已站上"拉完之后的站位点"：保持不动（原行为）
                dbgReason("stand_override_hold", dOverride);
                return false;
            }
            // 已站上 PULLSAFE 安全点且不在战斗 ⇒ 往下正常拉怪（安全点=本次拉怪位）
        }
        else if (ost.standOverrideUntil)
        {
            ost.standOverrideUntil = 0;
            ost.safePullPoint = false;
        }
    }

    dungeonroute::Target target;
    // [2026-10-23 站长指令] 先拉指定怪（例：540 军团士兵），且不让它周围一堆怪一起进战斗：
    //   有人多 ⇒ hold，什么都不拉（等巡逻走开/退）；否则用这一只作为唯一开怪目标。
    bool pfHold = false;
    if (dungeonroute::PullFirstTarget(bot, map, *route, target, pfHold))
    {
        if (pfHold)
        {
            dbgReason("pull_first_hold_too_many_adds");
            return true;
        }
    }
    else if (!dungeonroute::FindTargetNearest(bot, *route, target, true) || !target.hasPullPoint)
    {
        dbgReason("no_target_or_no_pullpoint");
        return false;
    }

    // [2026-10-24 站长口径] 拉怪位不能踩进"未进战斗怪"的仇恨圈（30 码内 + LOS 通过）
    //   有合法点 ⇒ 挪到**最近**的合法点；一个都没有 ⇒ 先拉（打起来再 pullback）
    // [2026-10-25 修] 本次拉怪已经在用 PULLSAFE 的安全点 ⇒ 不要再拿**原拉怪位**判"安不安全"，
    //   否则每 tick 都重新挪一次安全点、永远走不到"开怪"这一步（557 Pandemonius 实测）。
    float ex = target.px, ey = target.py, ez = target.pz;
    bool const safeStand = dungeonroute::SafePullPoint(bot, target, ex, ey, ez);
    if (!safeStand)
    {
        // [2026-10-26 修] 判"会不会引到别的怪"时排除**本次要拉的这一波**：
        //   拉怪位天生就在目标波的仇恨圈边上，不排除 ⇒ 48 个候选点全被判不安全
        //   （实测 2 小时 1789 条 `附近没有安全拉怪点`，offender 全是待拉的那只怪/boss）。
        std::set<uint32> pullWave;
        if (route)
        {
            auto wit = route->waves.find(target.ord);
            if (wit != route->waves.end())
                for (auto const& m : wit->second.members)
                    pullWave.insert(m.guid);
        }
        if (Creature* bad = dungeonroute::UnsafeStandOffender(bot, target.px, target.py, target.pz, 5.0f,
                                                              pullWave.empty() ? nullptr : &pullWave))
        {
            float bestX = target.px, bestY = target.py;
            float bestD = 1e18f;
            bool found = false;
            for (int i = 0; i < 12; ++i)
            {
                float const ang = float(i) * (2.0f * M_PI_F / 12.0f);
                for (int step = 1; step <= 4; ++step)      // 4, 8, 12, 16 码
                {
                    float const d = float(step) * 4.0f;
                    float const cx = target.px + std::cos(ang) * d;
                    float const cy = target.py + std::sin(ang) * d;
                    if (dungeonroute::UnsafeStandOffender(bot, cx, cy, bot->GetPositionZ(), 5.0f,
                                                          pullWave.empty() ? nullptr : &pullWave))
                        continue;
                    if (d < bestD)
                    {
                        bestD = d;
                        bestX = cx;
                        bestY = cy;
                        found = true;
                    }
                }
            }
            if (found && bot->GetDistance(bestX, bestY, bot->GetPositionZ()) > dungeonroute::AT_PULL_POINT_DIST)
            {
                dungeonroute::BotState& pst = dungeonroute::StateOf(bot);
                pst.standOverrideX = bestX;
                pst.standOverrideY = bestY;
                pst.standOverrideZ = bot->GetPositionZ();
                pst.standOverrideUntil = WorldTimer::getMSTime() + 120000;   // 2 分钟内不回原拉怪位
                pst.safePullPoint = true;            // [2026-10-25 修] 安全点=本次拉怪的站位点
                bot->GetMotionMaster()->MovePoint(0, bestX, bestY, bot->GetPositionZ());
                if (Unit* v = bot->GetVictim())
                    bot->SetFacingToObject(v);
                dtelemetry::Event("PULLSAFE", dungeonroute::TeleKeyBot(bot),
                    "%s 拉怪位会引到 %s(entry=%u) => 挪到最近的安全点 (%.0f,%.0f) dist=%.0f",
                    bot->GetName(), bad->GetName(), bad->GetEntry(), bestX, bestY, bestD);
                return true;
            }
            if (!found)
            {
                dungeonroute::StateOf(bot).pullBackPending = 1;
                dtelemetry::Event("PULLSAFE", dungeonroute::TeleKeyBot(bot),
                    "%s 附近没有安全拉怪点（%s entry=%u）=> 先拉，之后 pullback",
                    bot->GetName(), bad->GetName(), bad->GetEntry());
            }
        }
    }

    if (bot->GetDistance(ex, ey, ez) > dungeonroute::AT_PULL_POINT_DIST)
    {
        dbgReason("not_at_pull_point", bot->GetDistance(ex, ey, ez), uint32(target.ord));
        return false;    // 还没到位 ⇒ 交给 DungeonRouteMoveAction
    }

    // [2026-10-22 寻路修] "等网格加载"只从**站到拉怪位那一刻**开始计时。
    //   旧代码用 WaveStuckMs（从进入本波开始算）：走路本身就要 5s 以上，一到点就满足
    //   `> WAVE_NOT_LOADED_MS` ⇒ 没有立刻瞄到活怪的波被整波误判成"未加载"跳过。
    dungeonroute::BotState& st = dungeonroute::StateOf(bot);
    uint32 const nowMs = WorldTimer::getMSTime();
    uint16 const waitOrd = target.boss ? 0 : target.ord;
    if (st.atPullOrd != waitOrd)
    {
        st.atPullOrd = waitOrd;
        st.atPullSince = nowMs ? nowMs : 1;
    }
    uint32 const atPullMs = st.atPullSince ? WorldTimer::getMSTimeDiff(st.atPullSince, nowMs ? nowMs : 1) : 0;

    Creature* creature = map->GetCreature(target.guid);
    if (!creature || !creature->IsAlive() || !creature->IsInWorld())
    {
        // [2026-10-24 站长指令] 这只怪**消失了**（Protean Horror 这种有可打也有会消失的）：
        //   在拉怪点等 3 秒还找不到 ⇒ 只跳过**这个 guid** 60 秒，换下一个目标；不按 entry 拉黑。
        if (atPullMs > 3000 && nowMs)
        {
            st.guidSkipUntil[target.guid] = nowMs + 60000;
            sLog.outError("[DUNGEONROUTE] %s 目标 %u 已消失/不在网格 ⇒ 跳过这只 60s，换下一个",
                bot->GetName(), target.guid);
            return false;
        }
        // [2026-10-19 543 实测] PULL 这一组打完了：整波没有活怪 ⇒ 立刻标记本波已清，
        //   不要等 seenAlive 的下一次刷新（否则会先被 15s 无目标守卫跳掉）。
        if (!target.boss && target.ord)
        {
            auto wit = route->waves.find(target.ord);
            if (wit != route->waves.end() && !dungeonroute::WaveHasAlive(map, wit->second))
            {
                if (st.seenAlive.count(target.ord) || st.lockOrd == target.ord)
                    dungeonroute::MarkCleared(bot, target.ord, "pull cleared");
            }
            // [2026-10-22 寻路修] 本波还有活怪，只是暂时选不中（在水里/被挡/视线外/会 ADD）⇒
            //   这**不是**"未加载"，不能整波跳过；继续交给 MoveAction 接近，卡满 90s 有 WAVE_GIVE_UP 兜底。
            else if (wit != route->waves.end())
            {
                dbgReason("target_not_loaded_in_grid", -1.0f, uint32(target.ord));
                return false;
            }
        }

        // [2026-10-18 站长口径] "打完最近一波不推进"：目标是**还没加载**的怪/boss
        //   （FindTarget 会先给波中心/走位点）⇒ 站在波中心等网格加载；等 WAVE_NOT_LOADED_MS 还没有就放弃这波。
        if (atPullMs > dungeonroute::WAVE_NOT_LOADED_MS)
        {
            if (target.boss)
                dungeonroute::MarkBossSkipped(bot, target.guid);
            else if (target.ord)
                dungeonroute::MarkSkipped(bot, target.ord, "wave not loaded");
        }
        return false;
    }

    // 等人：拉怪前主人至少要跟到附近（真人玩家的距离门控）
    if (!dungeonroute::ReadyToEngage(ai, 45.0f))
    {
        dbgReason("not_ready_to_engage", bot->GetDistance(creature), uint32(target.ord));
        return true;
    }

    // [2026-10-25 站长口径] 开怪准入：等"代价最小的那一刻"再拉。
    //   代价 = 这次开怪会让多少**不属于目标组**的怪一起进战斗（组 = 刷怪系统分的 CreatureGroup，
    //   见 dungeonroute::PullGroupKey；巡逻按朝向+移速外推，不只看此刻位置）。
    //   ① 现在没有额外怪 ⇒ 立刻拉；② 往后几秒更便宜 ⇒ 等那一刻；③ 等也没用 ⇒ 现在就拉，
    //   不可避免的那一只接受（例：540 门口必须先开的军团士兵，它离入口最近那一刻仍会带一只静态怪）。
    if (st.pullWaitUntil && nowMs < st.pullWaitUntil)
    {
        dbgReason("waiting_best_window", bot->GetDistance(creature), uint32(target.ord));
        return true;
    }
    if (st.pullWaitUntil)
        st.pullWaitUntil = 0;
    {
        dungeonroute::PullCostSample cost;
        uint32 const delayMs = dungeonroute::PullAdmissionDelayMs(bot, creature, &cost);
        // [2026-10-26 站长口径] **当前代价已经是 0 ⇒ 直接拉**，不为"以后更便宜"再等；
        //   另外总等待上限 10 秒：巡逻外推会一直给出"2~8 秒后更便宜" ⇒ 无限推迟、整组空转（r100 实测 1100s）。
        uint32 const waitMs = cost.adds ? delayMs : 0;
        bool const changed = (st.pullCostAdds != cost.adds) || (waitMs != 0);
        st.pullCostAdds = cost.adds;
        st.pullCostPatrol = cost.patrolAdds;
        if (changed)
        {
            dtelemetry::Event("PULLCOST", dungeonroute::TeleKeyBot(bot),
                "%s 目标 %s entry=%u 当前代价=%u（巡逻 %u）%s", bot->GetName(),
                creature->GetName(), creature->GetEntry(), cost.adds, cost.patrolAdds,
                waitMs ? "⇒ 等更便宜的时刻" : "⇒ 现在开");
        }
        if (waitMs)
        {
            if (!st.pullCostWaitSince)
                st.pullCostWaitSince = nowMs ? nowMs : 1;
            uint32 const waited = WorldTimer::getMSTimeDiff(st.pullCostWaitSince, nowMs ? nowMs : 1);
            if (waited < 10000)
            {
                st.pullWaitUntil = nowMs + waitMs;
                dbgReason("wait_best_window", bot->GetDistance(creature), uint32(target.ord));
                return true;
            }
            dtelemetry::Event("PULLCOST", dungeonroute::TeleKeyBot(bot),
                "%s 等窗口已 %us（上限 10s）⇒ 现在开（当前代价=%u）",
                bot->GetName(), waited / 1000, cost.adds);
            st.pullCostWaitSince = 0;
            st.pullWaitUntil = 0;
        }
        else
            st.pullCostWaitSince = 0;
    }

    // [2026-10-24 站长指令] 552 类场景：等怪之间打完再开怪；一次不要拉太多（按核心求援判定）
    if (!route->pullWaits.empty())
    {
        for (auto const& w : route->pullWaits)
        {
            if (w.waitEnemyFight)
            {
                bool enemyFighting = false;
                for (Unit* a : creature->getAttackers())
                {
                    if (!a || a->GetTypeId() != TYPEID_UNIT)
                        continue;
                    if (bot->GetDistance(a) > w.radius)
                        continue;
                    enemyFighting = true;
                    break;
                }
                if (enemyFighting)
                {
                    dbgReason("wait_enemy_fight", bot->GetDistance(creature), uint32(target.ord));
                    return true;
                }
            }
            if (w.maxAssist != 0)
            {
                float const chainR = creature->GetCreatureInfo() && creature->GetCreatureInfo()->CallForHelp > 0
                    ? float(creature->GetCreatureInfo()->CallForHelp) : 5.0f;
                CreatureList receivers;
                MaNGOS::AnyAssistCreatureInRangeCheck u_check(creature, bot, chainR);
                MaNGOS::CreatureListSearcher<MaNGOS::AnyAssistCreatureInRangeCheck> searcher(receivers, u_check);
                Cell::VisitAllObjects(creature, searcher, chainR);
                if (receivers.size() > w.maxAssist)
                {
                    dbgReason("wait_too_many_assist", bot->GetDistance(creature), uint32(target.ord));
                    return true;
                }
            }
        }
    }

    // [2026-10-24 站长指令·解释A] 弓箭手类：
    //   站位要让它"面朝怪堆、后退方向没有别的怪" —— 即：在它周围一圈里，选一个站位，
    //   使"它后退落点（背离坦克的方向 35 码处）"附近敌对怪数量最少；同时要求坦克能看到它（能拉）。
    if (!route->pullBacks.empty())
    {
        for (auto const& pb : route->pullBacks)
        {
            if (creature->GetEntry() != pb.entry)
                continue;
            float const dNow = bot->GetDistance(creature);
            if (dNow >= pb.dist)
                break;

            float const cx = creature->GetPositionX();
            float const cy = creature->GetPositionY();
            float bestX = bot->GetPositionX(), bestY = bot->GetPositionY();
            uint32 bestAdds = 0xFFFFFFFF;
            bool found = false;
            for (int i = 0; i < 16; ++i)
            {
                float const ang = float(i) * (2.0f * M_PI_F / 16.0f);
                float const px = cx + std::cos(ang) * (pb.dist + 5.0f);
                float const py = cy + std::sin(ang) * (pb.dist + 5.0f);
                float const pz = bot->GetPositionZ();
                // 坦克要看得到弓手（否则拉不到）
                if (!bot->GetMap()->IsInLineOfSight(px, py, pz + 1.5f, cx, cy, creature->GetPositionZ() + 1.0f, false))
                    continue;
                // 弓手后退方向 = 背离坦克；落点 = 弓手 + 反方向 * 35
                float const rx = cx + (cx - px) / (pb.dist + 5.0f) * 35.0f;
                float const ry = cy + (cy - py) / (pb.dist + 5.0f) * 35.0f;
                uint32 adds = 0;
                uint32 seenAdds = 0;   // [2026-10-24 站长口径] "卡视野"：候选站位还能看到多少其它怪
                auto countOne = [&](uint32 g)
                {
                    Creature* c = bot->GetMap()->GetCreature(g);
                    if (!c || !c->IsAlive() || !c->IsInWorld() || c == creature)
                        return;
                    float const dx = c->GetPositionX() - rx, dy = c->GetPositionY() - ry;
                    if (dx * dx + dy * dy <= 25.0f * 25.0f)
                        ++adds;
                    float const mx = c->GetPositionX() - creature->GetPositionX();
                    float const my = c->GetPositionY() - creature->GetPositionY();
                    if (mx * mx + my * my <= 30.0f * 30.0f &&
                        bot->GetMap()->IsInLineOfSight(px, py, pz + 1.5f,
                            c->GetPositionX(), c->GetPositionY(), c->GetPositionZ() + 1.0f, false))
                        ++seenAdds;
                };
                for (auto const& kv : route->guidOrd)
                    countOne(kv.first);
                for (uint32 pg : route->patrolGuids)
                    countOne(pg);
                uint32 const score = adds * 10 + seenAdds;   // 先少引怪，其次卡视野
                if (score < bestAdds)
                {
                    bestAdds = score;
                    bestX = px; bestY = py;
                    found = true;
                }
            }
            if (found && bot->GetDistance(bestX, bestY, bot->GetPositionZ()) > 4.0f)
            {
                bot->GetMotionMaster()->MovePoint(0, bestX, bestY, bot->GetPositionZ());
                sLog.outError("[PULLBACK] %s 弓箭手 %s 距离 %.1f < %.0f ⇒ 绕到后退落点最少怪的一侧 (%.0f,%.0f) 再打（该方向落点附近怪=%u）",
                    bot->GetName(), creature->GetName(), dNow, pb.dist, bestX, bestY, bestAdds);
                return true;
            }
            break;
        }
    }

    Event pullEvent("attack anything", creature->GetObjectGuid());
    if (ai->DoSpecificAction("pull my target", pullEvent, true))
    {
        // [2026-10-24 修] `pull my target` 在"找不到可拉位置"时会**先移动**但返回 true（见 PullRequestAction），
        //   以前这里直接当"起手成功"，于是 553/557 站在拉怪点 act=pull my target 却永远不进战斗。
        //   现在确认真的打起来了才算起手。
        bool const engaged = creature->IsInCombat() ||
            (bot->GetVictim() && bot->GetVictim()->GetObjectGuid() == creature->GetObjectGuid());
        if (engaged)
        {
            dungeonroute::PullBegin(bot, *route, target);
            return true;
        }
        // 还没打起来：如果已经很近了，直接强制开怪，别在"拉怪动作"里空转
        if (bot->GetDistance(creature) <= 10.0f)
        {
            bot->Attack(creature, true);
            sLog.outError("[ENGAGE] %s 近距离强制开怪 target=%s(entry=%u) dist=%.1f（pull 只移动没开怪）",
                bot->GetName(), creature->GetName(), creature->GetEntry(), bot->GetDistance(creature));
            dungeonroute::PullBegin(bot, *route, target);
            return true;
        }
        if (!st.pullDbgAt || WorldTimer::getMSTimeDiff(st.pullDbgAt, nowMs ? nowMs : 1) >= 3000)
        {
            st.pullDbgAt = nowMs ? nowMs : 1;
            sLog.outError("[PULLWAIT] %s pull my target 只移动没开怪 target=%s dist=%.1f ⇒ 继续接近",
                bot->GetName(), creature->GetName(), bot->GetDistance(creature));
        }
        return true;
    }

    // [仿真 2026-10-18] 拉怪链路可能整条不可用：
    //   `PullStrategy::Get(ai)` 只认引擎里的 "pull" 策略（真·防护坦的默认策略才有），
    //   或者坦克没有远程武器 ⇒ `CanDoPullAction()` false ⇒ `pull my target` 永远返回 false，
    //   表现就是"站在拉怪位一动不动、永远不开怪"。
    //   这里退回直接 "attack anything"（走过去打）；起手记录照样补 ord，路线推进不至于卡死。
    if (ai->DoSpecificAction("attack anything", pullEvent, true))
    {
        sLog.outError("[DUNGEONROUTE] %s 拉怪链路不可用，退回直接开怪（entry=%u）",
            bot->GetName(), creature->GetEntry());
        dungeonroute::PullBegin(bot, *route, target);
        return true;
    }

    // [2026-10-23 站长口径] 两个动作都失败 ⇒ 必须留日志（以前静默 return false，表现就是"整队站在拉怪位发呆"）：
    //   ① 目标不可攻击 / 永远无敌（召唤物、无敌机关）⇒ 拉黑，继续推进（"应该逃跑"）；
    //   ② 目标可攻击但拉不到 ⇒ 有些怪要**贴近**才进入可打状态 ⇒ 主动走近再试（2 秒节流）。
    bool const immune = !bot->CanAttack(creature) || creature->IsImmuneToPlayer() ||
        creature->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_IMMUNE) ||
        creature->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_UNTARGETABLE);

    if (immune)
    {
        if (st.unattackable.insert(target.guid).second)
            sLog.outError("[DUNGEONROUTE] %s 拉怪失败：目标不可攻击/永远无敌（entry=%u guid=%u name=%s）⇒ 拉黑，继续推进",
                bot->GetName(), creature->GetEntry(), target.guid, creature->GetName());
        return false;
    }

    float const distNow = bot->GetDistance(creature);
    uint32 const failNowMs = WorldTimer::getMSTime();

    // [2026-10-23 站长指令] 只读诊断：把"拉不动"的四种可能分开记录（不改任何行为）：
    //   卡视野(los) / 走不到(path_ok,end_dist) / 免疫(imm_*) / 不可选中(untargetable,not_attackable1)
    {
        PathFinder pfDiag(bot);
        bool const pOk = pfDiag.calculate(creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ());
        float endDist = -1.0f;
        uint32 pts = 0;
        if (pOk)
        {
            auto& pp = pfDiag.getPath();
            pts = uint32(pp.size());
            if (!pp.empty())
            {
                Vector3 const& e = pp.back();
                float const dx = e.x - creature->GetPositionX();
                float const dy = e.y - creature->GetPositionY();
                float const dz = e.z - creature->GetPositionZ();
                endDist = std::sqrt(dx * dx + dy * dy + dz * dz);
            }
        }
        bool const los = bot->IsWithinLOS(creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ());
        uint8 mtype = 0;
        if (creature->GetMotionMaster())
            mtype = creature->GetMotionMaster()->GetCurrentMovementGeneratorType();
        dtelemetry::Event("PULLDIAG", dungeonroute::TeleKeyWave(map->GetId(), target.ord, bot),
            "bot=%s target=%s(entry=%u guid=%u) ord=%u boss=%d dist=%.1f dz=%.1f los=%d canattack=%d imm_player=%d imm_npc=%d untargetable=%d not_attackable1=%d path_ok=%d path_type=%d end_dist=%.1f pts=%u motion=%u",
            bot->GetName(), creature->GetName(), creature->GetEntry(), target.guid,
            uint32(target.ord), target.boss ? 1 : 0, distNow,
            creature->GetPositionZ() - bot->GetPositionZ(), los ? 1 : 0,
            bot->CanAttack(creature) ? 1 : 0, creature->IsImmuneToPlayer() ? 1 : 0,
            creature->IsImmuneToNPC() ? 1 : 0,
            creature->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_UNTARGETABLE) ? 1 : 0,
            creature->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NOT_ATTACKABLE_1) ? 1 : 0,
            pOk ? 1 : 0, int(pfDiag.getPathType()), endDist, pts, uint32(mtype));
    }
    bool const canRetry = (st.approachGuid != target.guid) || !st.approachAt ||
        WorldTimer::getMSTimeDiff(st.approachAt, failNowMs ? failNowMs : 1) >= 2000;

    sLog.outError("[DUNGEONROUTE] %s 拉怪失败：pull/attack 都不成立 target=%s(entry=%u guid=%u) dist=%.1f ord=%u boss=%d ⇒ %s",
        bot->GetName(), creature->GetName(), creature->GetEntry(), target.guid, distNow,
        uint32(target.ord), target.boss ? 1 : 0,
        distNow > 4.0f ? "走近再试" : "贴脸仍失败");

    // [2026-10-23] 同一只怪连拉 3 次都不成立 ⇒ 拉黑（不管原因是不可攻击、AI 感知不到还是脚本无敌），
    //   避免整队死磕一只怪（实测 558 Raging Soul 被重试 175 次、整图 0 进度）。
    uint8& fails = st.pullFailCount[target.guid];
    if (fails < 255)
        ++fails;
    if (fails >= 3 && st.unattackable.insert(target.guid).second)
        sLog.outError("[DUNGEONROUTE] %s 拉怪失败 %u 次 ⇒ 拉黑 target=%s(entry=%u guid=%u)，继续推进",
            bot->GetName(), uint32(fails), creature->GetName(), creature->GetEntry(), target.guid);

    if (fails >= 3)
        return false;

    // [2026-10-23 站长口径] "为了保证进入战斗，把开怪本身加上奖励"：
    //   pull/attack 两个动作都不成立时，**直接强制开怪**（等价玩家右键攻击），
    //   保证战斗样本一定产生；40 码内先贴上去再打。这一步不做任何"等/看"判断。
    if (distNow <= 40.0f)
    {
        bot->Attack(creature, true);
        if (distNow > 5.0f)
            bot->GetMotionMaster()->MovePoint(0, creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ());
        sLog.outError("[ENGAGE] %s 强制开怪 target=%s(entry=%u guid=%u) dist=%.1f ord=%u",
            bot->GetName(), creature->GetName(), creature->GetEntry(), target.guid, distNow, uint32(target.ord));
        dungeonroute::PullBegin(bot, *route, target);
        return true;
    }

    if (canRetry)
    {
        st.approachGuid = target.guid;
        st.approachAt = failNowMs ? failNowMs : 1;
        bot->GetMotionMaster()->MovePoint(0, creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ());
        return true;
    }

    return false;
}


// [2026-10-24 站长口径] 漏怪补拉（策略动作，不是仿真强制）：
//   在"正在打队友的怪"里找最近的一只，且自己当前没有目标 ⇒ 攻击它。
bool DungeonAssistLooseAction::Execute(Event& event)
{
    if (!bot || !bot->IsInCombat() || !bot->GetMap())
        return false;

    Group* grp = bot->GetGroup();
    if (!grp)
        return false;

    // 队伍的主坦克（仇恨基准）
    Player* tank = nullptr;
    for (GroupReference* ref = grp->GetFirstMember(); ref; ref = ref->next())
    {
        Player* m = ref->getSource();
        if (m && m->IsAlive() && PlayerbotAI::IsTank(m))
        {
            tank = m;
            break;
        }
    }
    if (!tank)
        return false;

    dungeonroute::MapRoute const* route = dungeonroute::GetMapRoute(bot->GetMapId());
    dungeonroute::BotState& st = dungeonroute::StateOf(bot);

    // [2026-10-24 站长口径] 仇恨判据：坦克仇恨必须 ≥ 第 N 仇恨 × pct/100（默认 rank=1 / pct=100）。
    //   rank>1 的特殊 boss 走 ai_playerbot_threat_rule；没行就是默认。
    auto ruleFor = [&](uint32 entry) -> dungeonroute::MapRoute::ThreatRule
    {
        dungeonroute::MapRoute::ThreatRule r;
        if (route)
            for (auto const& x : route->threatRules)
                if (x.entry == entry)
                    return x;
        return r;
    };

    auto tankHoldsThreat = [&](Creature* c) -> bool
    {
        if (!c || !c->IsAlive())
            return false;
        auto const rule = ruleFor(c->GetEntry());
        float const pct = rule.pct > 0.0f ? rule.pct : 100.0f;

        float tankTh = c->getThreatManager().getThreat(tank);
        // 取"除坦克外"的第 rank 高仇恨
        std::vector<float> others;
        for (GroupReference* ref = grp->GetFirstMember(); ref; ref = ref->next())
        {
            Player* m = ref->getSource();
            if (!m || m == tank)
                continue;
            others.push_back(c->getThreatManager().getThreat(m));
        }
        if (others.empty())
            return tankTh > 0.0f;
        std::sort(others.begin(), others.end(), std::greater<float>());
        uint8 const rank = rule.rank ? rule.rank : 1;
        size_t const idx = std::min<size_t>(rank - 1, others.size() - 1);
        float const refThreat = others[idx];
        return tankTh * 100.0f >= refThreat * pct;
    };

    // ① 有"在手的怪"：拉回到坦克（仇恨达标）或死了 ⇒ 清空，回退正常拉怪
    //    [2026-10-24 修] 关键：在手的怪**还没拉回来时必须继续动作**（嘲讽/审判/Attack），
    //    之前这里直接 return false ⇒ 坦克站着不动、永远拉不回来（就是"坦克空转"的直接原因）。
    if (st.assistGuid)
    {
        Creature* cur = bot->GetMap()->GetCreature(ObjectGuid(HIGHGUID_UNIT, 0, st.assistGuid));
        if (!cur || !cur->IsAlive() || !cur->IsInWorld() || tankHoldsThreat(cur))
        {
            st.assistGuid = 0;      // 拉回来了 / 死了 ⇒ 回退正常拉怪
            return false;
        }

        // 还在处理这一只：继续做仇恨（不重新挑目标，但必须动作）
        uint32 const tauntId = (bot->getClass() == CLASS_WARRIOR) ? 355
            : (bot->getClass() == CLASS_DRUID ? 6795 : 0);
        if (tauntId && cur->GetVictim() != bot && bot->HasSpell(tauntId) && sServerFacade.IsSpellReady(bot, tauntId))
            ai->CastSpell(tauntId, cur);

        if (bot->getClass() == CLASS_PALADIN && cur->GetVictim() && cur->GetVictim() != bot &&
            bot->HasSpell(31789) && sServerFacade.IsSpellReady(bot, 31789))
        {
            Unit* ally = cur->GetVictim();
            if (ally->GetTypeId() == TYPEID_PLAYER && ally->GetMapId() == bot->GetMapId() &&
                bot->GetDistance(ally) <= 30.0f)
                ai->CastSpell(31789, ally);
        }

        if (bot->GetVictim() != cur)
            bot->Attack(cur, true);
        return true;
    }

    // ② 找下一只"第一仇恨不是坦克"的怪（法术临时锁目标不算），挑最近的
    Creature* best = nullptr;
    float bestD = 1e18f;
    std::set<ObjectGuid> seen;
    for (GroupReference* ref = grp->GetFirstMember(); ref; ref = ref->next())
    {
        Player* m = ref->getSource();
        if (!m || !m->IsInWorld())
            continue;
        for (Unit* a : m->getAttackers())
        {
            if (!a || !a->IsAlive() || a->GetTypeId() != TYPEID_UNIT || a->GetMapId() != bot->GetMapId())
                continue;
            if (!seen.insert(a->GetObjectGuid()).second)
                continue;
            if (tankHoldsThreat((Creature*)a))
                continue;                 // 仇恨还在坦克手里
            float const d = bot->GetDistance(a);
            if (d > 40.0f)
                continue;
            if (d < bestD)
            {
                bestD = d;
                best = (Creature*)a;
            }
        }
    }
    if (!best)
        return false;

    st.assistGuid = best->GetGUIDLow();
    sLog.outError("[ASSIST] %s 补拉仇恨 %s(entry=%u) dist=%.1f（拉到达标为止）", bot->GetName(),
        best->GetName(), best->GetEntry(), bestD);

    // ③ 做仇恨：嘲讽/正义防御/审判
    uint32 tauntId = 0;
    switch (bot->getClass())
    {
        case CLASS_WARRIOR: tauntId = 355; break;      // Taunt
        case CLASS_DRUID:   tauntId = 6795; break;     // Growl
        default: break;
    }
    if (tauntId && best->GetVictim() != bot && bot->HasSpell(tauntId) && sServerFacade.IsSpellReady(bot, tauntId))
        ai->CastSpell(tauntId, best);

    if (bot->getClass() == CLASS_PALADIN && best->GetVictim() && best->GetVictim() != bot &&
        bot->HasSpell(31789) && sServerFacade.IsSpellReady(bot, 31789))
    {
        Unit* ally = best->GetVictim();
        if (ally->GetTypeId() == TYPEID_PLAYER && ally->GetMapId() == bot->GetMapId() &&
            bot->GetDistance(ally) <= 30.0f)
            ai->CastSpell(31789, ally);
    }

    if (bot->GetVictim() != best)
        bot->Attack(best, true);
    return true;
}

// [2026-10-24 站长口径] 低蓝分阶段：
//   阶段1（战斗中且蓝 < 40%）：职业省蓝手段 —— 骑士：智慧审判；术士：生命分流
//   阶段2（战斗中且蓝 < 8%）：才喝蓝药水
bool DungeonManaSustainAction::Execute(Event& event)
{
    if (!bot || !bot->IsAlive() || !bot->GetMaxPower(POWER_MANA))
        return false;
    if (!bot->IsInCombat())
        return false;                       // 不在战斗：交给喝水

    float const mp = bot->GetPowerPercent(POWER_MANA);
    Unit* tgt = bot->GetVictim();

    // 阶段2：极低蓝 ⇒ 药水
    if (mp < 8.0f)
        return ai->DoSpecificAction("mana potion", event, false);

    if (mp >= 40.0f || !tgt)
        return false;

    // 阶段1：职业省蓝
    if (bot->getClass() == CLASS_PALADIN)
    {
        static uint32 const wisJudges[] = { 20354, 20353, 20352, 20268, 20186 };
        for (uint32 id : wisJudges)
            if (bot->HasSpell(id) && sServerFacade.IsSpellReady(bot, id))
            {
                sLog.outError("[MANA] %s 智慧审判(过渡) mp=%.0f%%", bot->GetName(), mp);
                ai->CastSpell(id, tgt);
                return true;
            }
    }
    else if (bot->getClass() == CLASS_WARLOCK && bot->GetHealthPercent() > 50.0f)
    {
        static uint32 const taps[] = { 11689, 11688, 11687, 4090, 1456, 1455, 1454 };
        for (uint32 id : taps)
            if (bot->HasSpell(id) && sServerFacade.IsSpellReady(bot, id))
            {
                sLog.outError("[MANA] %s 生命分流(过渡) mp=%.0f%%", bot->GetName(), mp);
                ai->CastSpell(id, bot);
                return true;
            }
    }
    return false;
}


// [2026-10-24 站长口径] 坦克抓 OT：挑"仇恨缺口最大"的一只，嘲讽/正义防御/高仇恨技能
bool DungeonTankTauntAction::Execute(Event& event)
{
    if (!bot || !PlayerbotAI::IsTank(bot) || !bot->IsInCombat() || !bot->GetMap())
        return false;
    Group* grp = bot->GetGroup();
    if (!grp)
        return false;

    Creature* best = nullptr;
    float bestDeficit = 0.0f;
    std::set<ObjectGuid> seen;
    for (GroupReference* ref = grp->GetFirstMember(); ref; ref = ref->next())
    {
        Player* m = ref->getSource();
        if (!m || !m->IsInWorld())
            continue;
        for (Unit* a : m->getAttackers())
        {
            if (!a || !a->IsAlive() || a->GetTypeId() != TYPEID_UNIT || a->GetMapId() != bot->GetMapId())
                continue;
            if (!seen.insert(a->GetObjectGuid()).second)
                continue;
            if (bot->GetDistance(a) > 30.0f)
                continue;
            float tankTh = a->getThreatManager().getThreat(bot);
            float bestOther = 0.0f;
            for (GroupReference* r2 = grp->GetFirstMember(); r2; r2 = r2->next())
            {
                Player* o = r2->getSource();
                if (!o || o == bot)
                    continue;
                bestOther = std::max(bestOther, a->getThreatManager().getThreat(o));
            }
            float const deficit = bestOther - tankTh;
            if (deficit > bestDeficit)
            {
                bestDeficit = deficit;
                best = (Creature*)a;
            }
        }
    }
    if (!best)
        return false;

    static std::map<ObjectGuid, uint32> s_lastLog;
    static std::map<ObjectGuid, uint32> s_lastAct;
    uint32 const nowMs = WorldTimer::getMSTime();
    uint32& lastAct = s_lastAct[bot->GetObjectGuid()];
    if (lastAct && WorldTimer::getMSTimeDiff(lastAct, nowMs ? nowMs : 1) < 1000)
        return false;                 // 每秒最多动作一次（嘲讽有 CD，避免刷）
    lastAct = nowMs ? nowMs : 1;

    // [2026-10-26 站长口径·修正] **移动不影响坦克出刀**（进近战范围就平砍）；
    //   射程按法术数据走：嘲讽(355)/低吼(6795) 是**近战范围**，正义防御(31789) 才是 30 码。
    //   顺序：① 够得着就先施法抢仇恨；② 不在近战范围就一边 Attack 一边走过去。
    float const distToBest = bot->GetDistance(best);
    // [2026-10-26 站长口径·修正] 射程别写死：**嘲讽(355)/低吼(6795) 是近战范围，只有正义防御(31789) 是 30 码**。
    //   一律按法术数据里的 maxRange 判（读不到就退回近战范围）。移动不影响出刀，
    //   所以顺序是：① 够得着就先施法抢仇恨；② 不在近战范围就一边 Attack 一边走过去。
    auto spellMaxRange = [](uint32 id) -> float
    {
        SpellEntry const* si = sSpellTemplate.LookupEntry<SpellEntry>(id);
        if (!si)
            return ATTACK_DISTANCE;
        SpellRangeEntry const* r = sServerFacade.LookupSpellRangeEntry(si->rangeIndex);
        if (!r || r->maxRange <= 0.0f)
            return ATTACK_DISTANCE;
        return r->maxRange;
    };
    bool moved = false;

    uint32 const tauntId = (bot->getClass() == CLASS_WARRIOR) ? 355
        : (bot->getClass() == CLASS_DRUID ? 6795 : 0);
    bool acted = false;
    if (tauntId && best->GetVictim() != bot && bot->HasSpell(tauntId) &&
        sServerFacade.IsSpellReady(bot, tauntId) && distToBest <= spellMaxRange(tauntId))
    {
        acted = ai->CastSpell(tauntId, best);
    }
    if (!acted && bot->getClass() == CLASS_PALADIN)
    {
        if (best->GetVictim() && best->GetVictim() != bot && bot->HasSpell(31789) &&
            sServerFacade.IsSpellReady(bot, 31789))
        {
            Unit* ally = best->GetVictim();
            if (ally->GetTypeId() == TYPEID_PLAYER && bot->GetDistance(ally) <= spellMaxRange(31789))
                acted = ai->CastSpell(31789, ally);
        }
        if (!acted)
        {
            static uint32 const judges[] = { 20354, 20353, 20352, 20268, 20186, 20271 };
            for (uint32 id : judges)
                if (bot->HasSpell(id) && sServerFacade.IsSpellReady(bot, id))
                {
                    acted = ai->CastSpell(id, best);
                    break;
                }
        }
    }
    // ② 不在近战范围就走过去（Attack 已经挂着，进范围立刻打）
    if (!bot->CanReachWithMeleeAttack(best))
    {
        bot->GetMotionMaster()->MovePoint(0, best->GetPositionX(), best->GetPositionY(),
            best->GetPositionZ());
        moved = true;
    }
    if (bot->GetVictim() != best)
        bot->Attack(best, true);

    uint32& lastLog = s_lastLog[bot->GetObjectGuid()];
    if (!lastLog || WorldTimer::getMSTimeDiff(lastLog, nowMs ? nowMs : 1) >= 5000)
    {
        lastLog = nowMs ? nowMs : 1;
        sLog.outError("[TANKOT] %s %s %s(entry=%u) 缺口=%.0f dist=%.1f 施法=%d 走近=%d", bot->GetName(),
            (moved || !acted) ? "接战" : "抓 OT", best->GetName(), best->GetEntry(), bestDeficit,
            distToBest, acted ? 1 : 0, moved ? 1 : 0);
    }
    return true;
}




// [2026-10-24 站长口径] 群拉：复用模块自带 "aoe" 策略（不手写技能表）
//   怪多 ⇒ 打开 aoe；怪少 ⇒ 关掉，回到单体仇恨
bool DungeonTankAoeAction::Execute(Event& event)
{
    // [2026-10-24 挂死修] 不再在动作执行期间 ChangeStrategy（会改引擎策略表 ⇒ 实测把世界线程挂死）。
    //   群拉改为在策略层面常开 "aoe"（见 AiFactory::AddDefaultCombatStrategies），由它自己的触发器决定何时放 AoE。
    static std::map<ObjectGuid, uint32> s_last;
    uint32 const nowMs = WorldTimer::getMSTime();
    uint32& l = s_last[bot->GetObjectGuid()];
    if (!l || WorldTimer::getMSTimeDiff(l, nowMs ? nowMs : 1) >= 10000)
    {
        l = nowMs ? nowMs : 1;
        sLog.outError("[AOEPULL] %s 怪多（>=3 未受控）⇒ 群拉（aoe 策略常开）", bot->GetName());
    }
    return false;      // 不抢占，交给 aoe 策略自己的动作
}


// [2026-10-24 站长口径] 战斗中挪位：离开"未进战斗怪"的仇恨圈，走过去；每 tick 面朝当前目标
// [2026-10-25 修复] 挪位点由**几何**算出，不再在 5/10/15/20 码里挑"最近"的合法点：
//   原来的做法每次最多挪 5 码，而安全带是 24~27 码的一圈 ⇒ 走不出去，每 tick 重算、
//   在 1~5 码里来回抖（实测一轮 REPOS 动作执行 3.9 万次、坐标只在 1~2 码里变）。
//   现在沿"怪 → 我"的方向退到威胁圈外，退的距离 = 阈值距离 - 当前距离，一次决策退到位。
// [2026-10-27 站长口径] 施法怪/够不着的目标：安全就冲上去，不安全就卡视野或往后拉。
namespace { bool RepositionUnreachableTarget(Player* bot, dungeonroute::BotState& rst, uint32 nowMs); }

bool DungeonTankRepositionAction::Execute(Event& event)
{    if (!bot || !PlayerbotAI::IsTank(bot) || !bot->GetMap())
        return false;

    dungeonroute::BotState& rst = dungeonroute::StateOf(bot);
    bool const pending = (rst.pullBackPending != 0);
    float const margin = bot->IsInCombat() ? 2.0f : 5.0f;
    uint32 const nowMs = WorldTimer::getMSTime();

    // [2026-10-26 站长口径] **有怪在近战砍队友 ⇒ 立刻取消拉回**，坦克上去接战。
    //   否则坦克被钉在拉怪点（约 23 码外）等一个不会过来的怪：实测 555 坦克在 23 码外只放 buff，
    //   仇恨 2502 vs DPS 17~20k，奶妈被砍死。
    if (bot->IsInCombat())
    {
        bool mateMeleed = false;
        if (Group* g = bot->GetGroup())
        {
            for (GroupReference* ref = g->GetFirstMember(); ref && !mateMeleed; ref = ref->next())
            {
                Player* m = ref->getSource();
                if (!m || !m->IsInWorld() || !m->IsAlive() || m == bot)
                    continue;
                for (Unit* a : m->getAttackers())
                    if (a && a->IsAlive() && a->GetTypeId() == TYPEID_UNIT && a->CanReachWithMeleeAttack(m))
                    {
                        mateMeleed = true;
                        break;
                    }
            }
        }
        if (mateMeleed && (rst.pullBackPending || rst.standOverrideUntil))
        {
            rst.pullBackPending = 0;
            rst.standOverrideUntil = 0;
        }
    }

    // [2026-10-25 站长口径] **拉回**：起手后先退回拉怪位（怪会跟过来）；到了才允许原地打。
    //   原来起手后直接走进房间开打，隔壁的怪被一起引进来（552：拉 2 只变 6 个攻击者）。
    if (rst.pullBackPending && rst.standOverrideUntil && nowMs < rst.standOverrideUntil)
    {
        float const dPull = bot->GetDistance(rst.standOverrideX, rst.standOverrideY, rst.standOverrideZ);
        if (dPull > 4.0f)
        {
            bot->GetMotionMaster()->MovePoint(0, rst.standOverrideX, rst.standOverrideY,
                rst.standOverrideZ);
            if (!rst.reposFailAt || WorldTimer::getMSTimeDiff(rst.reposFailAt, nowMs ? nowMs : 1) >= 5000)
            {
                rst.reposFailAt = nowMs ? nowMs : 1;
                sLog.outError("[PULLBACK] %s 退回拉怪位 (%.0f,%.0f) 还差 %.0f 码",
                    bot->GetName(), rst.standOverrideX, rst.standOverrideY, dPull);
            }
            return true;
        }
        rst.pullBackPending = 0;
        rst.standOverrideUntil = 0;
    }
    else if (rst.pullBackPending)
        rst.pullBackPending = 0;

    // [2026-10-27 站长口径] **施法怪处理**：位置安全就冲上去打；不安全就卡视野 / 往后拉。
    //   555/558 实测：怪在坑对面/高台上，d=5~6、reach=0，坦克站在边上不动，怪 hp 8 分钟不掉。
    if (bot->IsInCombat() && RepositionUnreachableTarget(bot, rst, nowMs))
        return true;

    // 正在走向上一次决策的点 ⇒ 这次挪位还没做完，不重新决策（状态判断，不是时间节流）
    if (rst.standOverrideUntil && nowMs < rst.standOverrideUntil &&
        bot->GetDistance(rst.standOverrideX, rst.standOverrideY, rst.standOverrideZ) >
            dungeonroute::AT_PULL_POINT_DIST)
        return false;

    Creature* bad = dungeonroute::UnsafeStandOffender(bot, bot->GetPositionX(), bot->GetPositionY(),
        bot->GetPositionZ(), pending ? 5.0f : margin);
    if (!bad)
    {
        // 已经安全：pending 的用途到此为止，清掉，别让它反复触发
        if (pending)
            rst.pullBackPending = 0;
        return false;
    }

    // 判"不安全"的阈值是 威胁圈 + margin。退到阈值之外再多留一个 margin，
    //   这样新位置明确在圈外、不是擦着边界（否则 3D/浮点判定会把它重新判回圈内）。
    float const ring = dungeonroute::ThreatRingRadius(bot, bad);
    float const target = ring + 2.0f * margin;
    float const cur = bot->GetDistance(bad);
    float const need = target - cur;
    float const dx = bot->GetPositionX() - bad->GetPositionX();
    float const dy = bot->GetPositionY() - bad->GetPositionY();
    float const len = std::sqrt(dx * dx + dy * dy);
    // [2026-10-26 站长口径] 位移需求太小就不动：实战里这个动作 25 分钟触发 15,778 次，
    //   全是 1~2 码的抖动 —— 坦克一直在挪，等于一直不打，仇恨/输出全丢。
    //   要求"真的差得远"（>=4 码）才挪；否则原地打，等这一波打完再说。
    if (need < 4.0f || len < 1.0f)
        return false;                       // 已经在外（判定竞态），或怪就贴在身上、方向无从谈起

    float const awayAng = std::atan2(dy, dx);
    // 先试正对"背离方向"，被别的怪挡住再向两侧张：±15° / ±30° / ±45° / ±60° / ±75° / ±90°
    static float const spread[] = {0.0f, 0.2618f, -0.2618f, 0.5236f, -0.5236f, 0.7854f, -0.7854f,
        1.0472f, -1.0472f, 1.3090f, -1.3090f, 1.5708f, -1.5708f};

    float bestX = 0.0f, bestY = 0.0f;
    bool clear = false;
    for (size_t i = 0; i < sizeof(spread) / sizeof(spread[0]); ++i)
    {
        float const ang = awayAng + spread[i];
        float const cx = bot->GetPositionX() + std::cos(ang) * need;
        float const cy = bot->GetPositionY() + std::sin(ang) * need;
        if (dungeonroute::UnsafeStandOffender(bot, cx, cy, bot->GetPositionZ(), margin))
            continue;
        bestX = cx;
        bestY = cy;
        clear = true;
        break;
    }
    if (!clear)
    {
        // 整片扇形都落在别的怪圈里：仍然沿背离方向退（离开当前这只总比钉在原地强），
        //   下一轮决策处理新挡路的那只；这里不再"找不到点就返回 false"，
        //   否则就是原来那个死循环：条件为真 → 动作什么都不做 → 下一 tick 再来。
        bestX = bot->GetPositionX() + std::cos(awayAng) * need;
        bestY = bot->GetPositionY() + std::sin(awayAng) * need;
    }

    // [2026-10-25 站长口径] **找不到安全点就不许动**：
    //   原来这里 clear=false 时仍按"背离方向"硬挪，结果把坦克推到更不安全的地方
    //   （547 实测：每秒"无理想点"却退 6 码，坦克零仇恨、队伍卡死到整轮超时）。
    //   正确做法：原地不动，等巡逻怪走到位、或者等这一波打完。
    if (!clear)
    {
        if (!rst.reposFailAt || WorldTimer::getMSTimeDiff(rst.reposFailAt, nowMs ? nowMs : 1) >= 5000)
        {
            rst.reposFailAt = nowMs ? nowMs : 1;
            sLog.outError("[REPOS] %s 找不到安全点（避 %s entry=%u，距 %.1f / 目标 %.1f）⇒ 原地不动，等巡逻怪走过来",
                bot->GetName(), bad->GetName(), bad->GetEntry(), cur, target);
        }
        return true;
    }

    rst.standOverrideX = bestX;
    rst.standOverrideY = bestY;
    rst.standOverrideZ = bot->GetPositionZ();
    rst.standOverrideUntil = nowMs + 120000;

    bot->GetMotionMaster()->MovePoint(0, bestX, bestY, bot->GetPositionZ());
    if (Unit* tgt = bot->GetVictim())
        bot->SetFacingToObject(tgt);

    sLog.outError("[REPOS] %s 退 %.1f 码 -> (%.0f,%.0f)（避 %s entry=%u，距 %.1f / 目标 %.1f，%s，pending=%d）",
        bot->GetName(), need, bestX, bestY, bad->GetName(), bad->GetEntry(), cur, target,
        clear ? "圈外" : "无理想点", pending ? 1 : 0);
    if (pending)
        rst.pullBackPending = 0;
    return true;
}


// [2026-10-26 站长口径] **拉回和卡视野是一件事**：给坦克选一个"该站的点"再沿路走过去。
//   三种用途共用这一个引擎（区别只在约束）：
//     SPOT_SAFE   —— 拉回：不在别的怪仇恨圈里（目的：不引怪）
//     SPOT_LOS    —— 卡视野：上面 + 对目标怪断视线 + 环境掩护（手段：把远程/站桩怪逼出来）
//     SPOT_COMBAT —— 战斗中动态挪位：保持当前目标在近战内、位移最小（目的：躲巡逻怪）
namespace
{
    enum TankSpotMode
    {
        SPOT_SAFE = 0,
        SPOT_LOS = 1,
        SPOT_COMBAT = 2,
    };

    struct TankSpot
    {
        bool ok = false;
        float x = 0.0f, y = 0.0f, z = 0.0f;
    };

    // [2026-10-26 站长口径] 候选点的 Z 要**从导航网格取**。
    //   ⚠️ 坐标顺序：mmap 顶点按 (y, z, x) 存，传 {x,y,z} 一个多边形都找不到。
    inline bool NavMeshZ(Map* map, float x, float y, float refZ, float& outZ,
                         uint32* nmNull = nullptr, uint32* nmNoRef = nullptr, uint32* nmFar = nullptr)
    {
        if (!map)
        {
            if (nmNull) ++*nmNull;
            return false;
        }
        dtNavMeshQuery const* q =
            MMAP::MMapFactory::createOrGetMMapManager()->GetNavMeshQuery(map->GetId(), map->GetInstanceId());
        if (!q)
        {
            if (nmNull) ++*nmNull;
            return false;
        }
        dtQueryFilter filter;
        dtPolyRef ref = 0;
        float const pt[3] = { y, refZ, x };
        float closest[3] = { 0.0f, 0.0f, 0.0f };
        float const extents[3] = { 5.0f, 5.0f, 5.0f };
        q->findNearestPoly(pt, extents, &filter, &ref, closest);
        if (!ref)
        {
            if (nmNoRef) ++*nmNoRef;
            return false;
        }
        float const gx = closest[2], gy = closest[0], gz = closest[1];
        float const dx = gx - x;
        float const dy = gy - y;
        float const dz = gz - refZ;
        if (dx * dx + dy * dy + dz * dz > 4.0f)   // 3D 2 码内算同一块可行走面
        {
            if (nmFar) ++*nmFar;
            return false;
        }
        outZ = gz;
        return true;
    }

    // [2026-10-26 站长口径] 失败要能看出是哪条过滤吃掉了候选点（否则永远是黑盒）。
    struct TankSpotStats
    {
        uint32 navmesh = 0, unsafe = 0, los = 0, cover = 0, melee = 0, ok = 0;
        uint32 nmNull = 0, nmNoRef = 0, nmFar = 0;   // 网格过滤的三个细分原因（诊断用）
    };

    // [2026-10-26 站长口径] 卡视野点的成色：怪看不见 + 实心遮挡 >= 180 度（人贴墙 ⇒ 保底半球）
    //   + 离墙距离越小越好（离墙远 = 挡不干净）。
    inline bool CoverOk(Map* map, float cx, float cy, float cz, Creature* danger,
                        float& wallDist, float& coverScore, uint32* statsCover = nullptr)
    {
        if (!map || !danger)
            return false;
        if (map->IsInLineOfSight(cx, cy, cz + 1.5f, danger->GetPositionX(), danger->GetPositionY(),
                                 danger->GetPositionZ() + 1.0f, false))
            return false;                       // 怪还看得见 ⇒ 不算卡视野
        int blocked = 0;
        for (int i = 0; i < 16; ++i)
        {
            float const a = float(i) * (2.0f * M_PI_F / 16.0f);
            if (!map->IsInLineOfSight(cx, cy, cz + 1.0f, cx + std::cos(a) * 1.5f,
                                      cy + std::sin(a) * 1.5f, cz + 1.0f, false))
                ++blocked;
        }
        if (blocked < 8)                        // 遮挡不足 180 度 ⇒ 挡不干净
        {
            if (statsCover) ++*statsCover;
            return false;
        }
        wallDist = 99.0f;
        for (int i = 0; i < 8 && wallDist > 0.4f; ++i)
        {
            float const a = float(i) * (2.0f * M_PI_F / 8.0f);
            for (float r = 0.4f; r <= 2.4f; r += 0.2f)
            {
                if (!map->IsInLineOfSight(cx, cy, cz + 1.0f, cx + std::cos(a) * r,
                                          cy + std::sin(a) * r, cz + 1.0f, false))
                {
                    if (r < wallDist)
                        wallDist = r;
                    break;
                }
            }
        }
        if (wallDist > 1.5f)                    // 离墙太远 ⇒ 贴不住墙，挡不住半球
        {
            if (statsCover) ++*statsCover;
            return false;
        }
        coverScore = -wallDist * 100.0f;
        return true;
    }

    // [2026-10-26 站长口径] **选点靠几何，不靠 40 码盲扫**：
    //   ① 先在"怪眼 -> 坦克"射线上打出真正的遮挡面，再从墙面往坦克这一侧退半步（人贴墙站）；
    //   ② 线上没有遮挡（怪看得见你）⇒ 从坦克脚下沿 32 个方向走到"走出导航网格"的地方（那就是墙），
    //      把墙面前 0.4~0.8 码的位置当候选点。
    inline TankSpot PickCoverSpot(Player* bot, Creature* danger, TankSpotStats* stats = nullptr)
    {
        TankSpot best;
        if (!bot || !bot->GetMap() || !danger)
            return best;
        Map* map = bot->GetMap();
        float bestScore = -1e18f;

        // ① 怪眼 -> 坦克：找墙面
        float const ex = danger->GetPositionX(), ey = danger->GetPositionY();
        float const ez = danger->GetPositionZ() + 1.2f;
        float hx = bot->GetPositionX(), hy = bot->GetPositionY(), hz = bot->GetPositionZ() + 1.0f;
        if (map->GetHitPosition(ex, ey, ez, hx, hy, hz, 0.0f))
        {
            float dx = hx - ex, dy = hy - ey;
            float const len = std::sqrt(dx * dx + dy * dy);
            if (len > 0.1f)
            {
                dx /= len;
                dy /= len;
                float const px = -dy, py = dx;          // 侧向
                static float const back[] = { 0.0f, 0.5f, 1.0f, 1.6f, 2.2f };   // 往坦克侧退（贴墙）
                static float const side[] = { 0.0f, -0.8f, 0.8f, -1.6f, 1.6f };
                for (float b : back)
                {
                    for (float sg : side)
                    {
                        float const cx = hx + dx * b + px * sg;
                        float const cy = hy + dy * b + py * sg;
                        float nz = 0.0f;
                        if (!NavMeshZ(map, cx, cy, bot->GetPositionZ(), nz,
                                      stats ? &stats->nmNull : nullptr, stats ? &stats->nmNoRef : nullptr,
                                      stats ? &stats->nmFar : nullptr))
                        {
                            if (stats) ++stats->navmesh;
                            continue;
                        }
                        if (dungeonroute::UnsafeStandOffender(bot, cx, cy, nz, 2.0f))
                        {
                            if (stats) ++stats->unsafe;
                            continue;
                        }
                        float wallDist = 99.0f, coverScore = 0.0f;
                        if (!CoverOk(map, cx, cy, nz, danger, wallDist, coverScore,
                                     stats ? &stats->cover : nullptr))
                            continue;
                        if (stats) ++stats->ok;
                        float const score = coverScore - std::fabs(b) * 2.0f - std::fabs(sg) * 3.0f;
                        if (score > bestScore)
                        {
                            bestScore = score;
                            best.ok = true;
                            best.x = cx;
                            best.y = cy;
                            best.z = nz;
                        }
                    }
                }
            }
        }

        // ② 坦克周围打一圈 vmaps 射线找墙面（怪看得见你的时候）。
        //   ⚠️ 不能用"走出导航网格"当墙：副本里网格铺到墙根，32 个方向 12 码内几乎都不会出格。
        if (!best.ok)
        {
            float const bx = bot->GetPositionX(), by = bot->GetPositionY();
            float const bz = bot->GetPositionZ();
            static float const backOff[] = { 0.5f, 0.9f, 1.4f };   // 从墙面往坦克侧退（贴墙）
            for (int i = 0; i < 32; ++i)
            {
                float const a = float(i) * (2.0f * M_PI_F / 32.0f);
                float const ux = std::cos(a), uy = std::sin(a);
                float wx = bx + ux * 12.0f, wy = by + uy * 12.0f, wz = bz + 1.0f;
                if (!map->GetHitPosition(bx, by, bz + 1.0f, wx, wy, wz, 0.0f))
                    continue;                       // 这个方向 12 码内没墙
                float const wallGap = std::sqrt((wx - bx) * (wx - bx) + (wy - by) * (wy - by));
                if (wallGap < 1.2f)
                    continue;                       // 贴着别的东西，跳过
                for (float bo : backOff)
                {
                    float const cx = wx - ux * bo;
                    float const cy = wy - uy * bo;
                    float nz = 0.0f;
                    if (!NavMeshZ(map, cx, cy, bz, nz,
                                  stats ? &stats->nmNull : nullptr, stats ? &stats->nmNoRef : nullptr,
                                  stats ? &stats->nmFar : nullptr))
                    {
                        if (stats) ++stats->navmesh;
                        continue;
                    }
                    if (dungeonroute::UnsafeStandOffender(bot, cx, cy, nz, 2.0f))
                    {
                        if (stats) ++stats->unsafe;
                        continue;
                    }
                    float wallDist = 99.0f, coverScore = 0.0f;
                    if (!CoverOk(map, cx, cy, nz, danger, wallDist, coverScore,
                                 stats ? &stats->cover : nullptr))
                        continue;
                    if (stats) ++stats->ok;
                    float const dd = bot->GetDistance(cx, cy, nz);
                    float const score = coverScore - dd * 2.0f;
                    if (score > bestScore)
                    {
                        bestScore = score;
                        best.ok = true;
                        best.x = cx;
                        best.y = cy;
                        best.z = nz;
                    }
                }
            }
        }
        return best;
    }

    // anchor：以哪个点为中心扫（拉回=拉怪位；卡视野走 PickCoverSpot）
    inline TankSpot PickTankSpot(Player* bot, Creature* danger, TankSpotMode mode,
                                 float anchorX, float anchorY, float anchorZ,
                                 TankSpotStats* stats = nullptr)
    {
        TankSpot best;
        if (!bot || !bot->GetMap())
            return best;
        if (mode == SPOT_LOS)
            return PickCoverSpot(bot, danger, stats);     // 卡视野只走几何选点

        Map* map = bot->GetMap();
        float bestScore = -1e18f;

        for (int ox = -40; ox <= 40; ox += 2)
        {
            for (int oy = -40; oy <= 40; oy += 2)
            {
                float const cx = anchorX + float(ox);
                float const cy = anchorY + float(oy);
                float const d = std::sqrt(float(ox * ox + oy * oy));
                if (d < 2.0f || d > 40.0f)
                    continue;
                float nz = 0.0f;
                if (!NavMeshZ(map, cx, cy, anchorZ, nz,
                             stats ? &stats->nmNull : nullptr,
                             stats ? &stats->nmNoRef : nullptr,
                             stats ? &stats->nmFar : nullptr))
                {
                    if (stats) ++stats->navmesh;
                    continue;
                }
                // 仇恨圈只算**没参战**的怪（已经在打的归战斗逻辑管）—— SafePointVisitor 里已如此
                if (dungeonroute::UnsafeStandOffender(bot, cx, cy, nz, 2.0f))
                {
                    if (stats) ++stats->unsafe;
                    continue;
                }
                if (mode == SPOT_COMBAT)
                {
                    if (!danger)
                        continue;
                    float const ddx = danger->GetPositionX() - cx;
                    float const ddy = danger->GetPositionY() - cy;
                    float const ddz = danger->GetPositionZ() - nz;
                    if (std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz) > 8.0f)
                    {
                        if (stats) ++stats->melee;
                        continue;
                    }
                }

                if (stats) ++stats->ok;
                float const score = -d;             // 越近越好
                if (score > bestScore)
                {
                    bestScore = score;
                    best.ok = true;
                    best.x = cx;
                    best.y = cy;
                    best.z = nz;
                }
            }
        }
        return best;
    }
}

// [2026-10-27 站长口径] 施法怪/够不着的目标（坦克视角）：
//   ① 目标在近战外、但**完整路径**走得到（NORMAL/SHORTCUT）⇒ 安全，直接冲上去近战；
//   ② 路径不完整（隔坑 / 高台 / 断桥）⇒ 先找卡视野点把它逼过来；
//   ③ 连卡视野点都没有 ⇒ 沿"目标→自己"方向再退 10 码（往后拉）。
//   节流 3 秒，只在目标 ≤30 码且确实够不着时触发，不影响正常追击。
namespace
{
    bool RepositionUnreachableTarget(Player* bot, dungeonroute::BotState& rst, uint32 nowMs)
    {
        if (!bot || !bot->IsInCombat() || !bot->GetMap())
            return false;
        Unit* v = bot->GetVictim();
        if (!v || v->GetTypeId() != TYPEID_UNIT || !v->IsAlive())
            return false;
        if (bot->CanReachWithMeleeAttack(v))
            return false;
        float const d = bot->GetDistance(v);
        if (d > 30.0f)
            return false;
        if (rst.casterPosAt && WorldTimer::getMSTimeDiff(rst.casterPosAt, nowMs ? nowMs : 1) < 3000)
            return false;
        rst.casterPosAt = nowMs ? nowMs : 1;

        PathFinder pf(bot);
        pf.calculate(v->GetPositionX(), v->GetPositionY(), v->GetPositionZ());
        PathType const pt = pf.getPathType();
        if (pt == PATHFIND_NORMAL || pt == PATHFIND_SHORTCUT)
        {
            bot->GetMotionMaster()->MoveChase(v);
            sLog.outError("[CASTERPOS] %s 目标 %s(entry=%u) 近战够不着但路径可通（d=%.1f type=%u）=> 冲上去",
                bot->GetName(), v->GetName(), v->GetEntry(), d, uint32(pt));
            return true;
        }

        TankSpotStats stats;
        TankSpot spot = PickTankSpot(bot, static_cast<Creature*>(v), SPOT_LOS,
            bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), &stats);
        if (spot.ok)
        {
            bot->GetMotionMaster()->MovePoint(0, spot.x, spot.y, spot.z);
            sLog.outError("[CASTERPOS] %s 目标 %s(entry=%u) 路径不完整（d=%.1f type=%u）=> 卡视野退到 (%.0f,%.0f)",
                bot->GetName(), v->GetName(), v->GetEntry(), d, uint32(pt), spot.x, spot.y);
            return true;
        }

        // 往后拉：沿"目标 -> 自己"方向再退 10 码（落到导航网格上才走）
        float dx = bot->GetPositionX() - v->GetPositionX();
        float dy = bot->GetPositionY() - v->GetPositionY();
        float len = std::sqrt(dx * dx + dy * dy);
        if (len < 0.5f) { dx = 1.0f; dy = 0.0f; len = 1.0f; }
        float const tx = bot->GetPositionX() + dx / len * 10.0f;
        float const ty = bot->GetPositionY() + dy / len * 10.0f;
        float nz = 0.0f;
        if (NavMeshZ(bot->GetMap(), tx, ty, bot->GetPositionZ(), nz))
        {
            bot->GetMotionMaster()->MovePoint(0, tx, ty, nz);
            sLog.outError("[CASTERPOS] %s 目标 %s(entry=%u) 走不通且无卡视野点（d=%.1f）=> 往后拉 10 码到 (%.0f,%.0f)",
                bot->GetName(), v->GetName(), v->GetEntry(), d, tx, ty);
            return true;
        }

        sLog.outError("[CASTERPOS] %s 目标 %s(entry=%u) 走不通 / 无卡视野点 / 后退点不在网格（d=%.1f）=> 放弃挪位",
            bot->GetName(), v->GetName(), v->GetEntry(), d);
        return false;
    }
}

// [2026-10-25 站长口径] **坦克是队伍核心**：任意队友被敌对怪攻击 ⇒ 立刻救场。
//   规则：不管坦克有没有目标、正在打谁、正在走哪条路 —— 全部让位。
//   如果那只怪不朝坦克走（远程/法系/站桩）⇒ 坦克负责**卡视野**把它逼过来。
bool DungeonTankEngageAction::Execute(Event& event)
{
    if (!bot || !PlayerbotAI::IsTank(bot) || !bot->GetMap())
        return false;
    Group* grp = bot->GetGroup();
    if (!grp)
        return false;

    // [2026-10-25 站长口径] **坦克优先锁集火目标**（priority5 > 低血精英），
    //   其它怪只维持"基础仇恨"：治疗仇恨接近(≥pct%)就补一次。
    {
        Creature* focus = dungeonroute::FindTankFocusTarget(bot);
        if (focus && (!bot->GetVictim() || bot->GetVictim()->GetObjectGuid() != focus->GetObjectGuid()))
            bot->Attack(focus, true);
        Creature* pressure = dungeonroute::FindHealerThreatPressure(bot);
        if (pressure && (!bot->GetVictim() || bot->GetVictim()->GetObjectGuid() != pressure->GetObjectGuid()))
        {
            bot->Attack(pressure, true);
            sLog.outError("[BASET] %s 补基础仇恨：%s(entry=%u) 的治疗仇恨已接近坦克",
                bot->GetName(), pressure->GetName(), pressure->GetEntry());
        }
    }

    Creature* best = nullptr;
    uint32 worstDeficit = 0;
    for (GroupReference* ref = grp->GetFirstMember(); ref; ref = ref->next())
    {
        Player* m = ref->getSource();
        if (!m || m == bot || !m->IsInWorld())
            continue;
        for (Unit* a : m->getAttackers())
        {
            if (!a || !a->IsAlive() || a->GetTypeId() != TYPEID_UNIT || a->GetMapId() != bot->GetMapId())
                continue;
            Creature* c = static_cast<Creature*>(a);
            if (c->IsCritter() || !bot->CanAttack(c))
                continue;
            // [2026-10-26 站长口径] 不再因为"坦克已经把它设为目标"就跳过 —— 实测那种情况坦克站在
            //   23 码外不动（有目标但不近战），怪照样在砍队友。改成：只要它在砍队友就得接手，
            //   下面 ① 分支（victim != bot）里的"近战砍队友 ⇒ 上去接"会处理。
            if (bot->GetVictim() && bot->GetVictim()->GetObjectGuid() == c->GetObjectGuid() &&
                c->GetVictim() == bot)
                continue;   // 它本来就在打我 ⇒ 正常战斗，不用救场
            float const selfThreat = c->getThreatManager().getThreat(bot);
            float const mateThreat = c->getThreatManager().getThreat(m);
            uint32 const deficit = uint32(mateThreat > selfThreat ? (mateThreat - selfThreat) : 0.0f);
            if (!best || deficit > worstDeficit)
            {
                worstDeficit = deficit;
                best = c;
            }
        }
    }
    if (!best)
        return false;

    // ① 它不朝坦克走（victim 不是坦克）
    if (best->GetVictim() != bot)
    {
        // [2026-10-26 站长口径] **它正在近战砍队友 ⇒ 直接上去接手**（走位 + 嘲讽/接战），不是卡视野。
        //   卡视野只对"远程/站桩不跟过来"的怪有用；对已经在砍队友的怪，坦克退开只是把队友留在原地挨打。
        //   实测 555：坦克站在 23 码外，DPS 仇恨 1.7~2 万 vs 坦克 2500，奶妈被砍死。
        {
            Unit* mate = best->GetVictim();
            bool const meleeOnMate = mate && mate->GetTypeId() == TYPEID_PLAYER &&
                best->CanReachWithMeleeAttack(mate);
            if (meleeOnMate)
            {
                float const dm = bot->GetDistance(best);
                if (!bot->CanReachWithMeleeAttack(best))
                    bot->GetMotionMaster()->MovePoint(0, best->GetPositionX(), best->GetPositionY(),
                        best->GetPositionZ());
                if (bot->GetVictim() != best)
                    bot->Attack(best, true);
                uint32 const tauntId2 = (bot->getClass() == CLASS_WARRIOR) ? 355
                    : (bot->getClass() == CLASS_DRUID ? 6795 : 0);
                if (tauntId2 && dm <= 8.0f && bot->HasSpell(tauntId2) && sServerFacade.IsSpellReady(bot, tauntId2) &&
                    best->GetVictim() != bot)
                    ai->CastSpell(tauntId2, best);

                static std::map<ObjectGuid, uint32> s_lastTake;
                uint32& lastTake = s_lastTake[bot->GetObjectGuid()];
                uint32 const nowTake = WorldTimer::getMSTime() ? WorldTimer::getMSTime() : 1;
                if (!lastTake || WorldTimer::getMSTimeDiff(lastTake, nowTake) >= 5000)
                {
                    lastTake = nowTake;
                    sLog.outError("[ENGAGE2] %s 接手：%s(entry=%u) 正在近战砍 %s ⇒ 上去接战（距 %.0f 码）",
                        bot->GetName(), best->GetName(), best->GetEntry(),
                        mate->GetTypeId() == TYPEID_PLAYER ? mate->GetName() : "?",
                        dm);
                }
                return true;
            }
        }

        float const d = bot->GetDistance(best);
        if (d > dungeonroute::GATHER_RADIUS)
        {
            TankSpotStats stats;
            // [2026-10-26 站长口径] 先查离线点库：取最近一个"本波怪看不见"的贴墙点，现场只做复核
            TankSpot spot;
            {
                dungeonroute::MapRoute const* prt = dungeonroute::GetMapRoute(bot->GetMapId());
                uint16 ord = 0;
                if (prt)
                {
                    auto git = prt->guidOrd.find(best->GetGUIDLow());
                    if (git != prt->guidOrd.end())
                        ord = git->second;
                }
                float px = 0.0f, py = 0.0f, pz = 0.0f;
                if (coverspot::Find(bot->GetMapId(), ord, bot->GetPositionX(), bot->GetPositionY(),
                                    35.0f, px, py, pz))
                {
                    float nz = 0.0f;
                    Map* bmap = bot->GetMap();
                    if (NavMeshZ(bmap, px, py, bot->GetPositionZ(), nz) &&
                        !bmap->IsInLineOfSight(px, py, nz + 1.5f, best->GetPositionX(), best->GetPositionY(),
                                               best->GetPositionZ() + 1.0f, false))
                    {
                        spot.ok = true;
                        spot.x = px;
                        spot.y = py;
                        spot.z = nz;
                        if (stats.ok == 0)
                            ++stats.ok;
                        sLog.outError("[ENGAGE2] %s 点库命中：往 (%.0f,%.0f) 卡 %s(entry=%u) 的视线（ord=%u）",
                            bot->GetName(), px, py, best->GetName(), best->GetEntry(), uint32(ord));
                    }
                }
            }
            if (!spot.ok)
                spot = PickTankSpot(bot, best, SPOT_LOS,
                    bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), &stats);
            if (spot.ok)
            {
                bot->GetMotionMaster()->MovePoint(0, spot.x, spot.y, spot.z);
                sLog.outError("[ENGAGE2] %s 卡视野：%s(entry=%u) 距 %.0f 码不跟过来 => 退到 (%.0f,%.0f) 断视线",
                    bot->GetName(), best->GetName(), best->GetEntry(), d, spot.x, spot.y);
                return true;
            }
            sLog.outError("[ENGAGE2] %s 卡视野失败：%s(entry=%u) 距 %.0f 码 => 先原地等（候选拒绝：网格 %u / 仇恨圈 %u / 还看得见 %u / 掩护不足 %u / 通过 %u；网格细分 空查询 %u / 无多边形 %u / 离面太远 %u）",
                bot->GetName(), best->GetName(), best->GetEntry(), d,
                stats.navmesh, stats.unsafe, stats.los, stats.cover, stats.ok,
                stats.nmNull, stats.nmNoRef, stats.nmFar);
            return false;
        }
    }

    // [2026-10-26 站长口径] 拉回挂点补到这条"救场/强制开怪"路径：先选一个安全站位记下来，
    //   之后由 DungeonTankRepositionAction 把坦克带回该点（战斗中怪会跟过来）。
    {
        dungeonroute::BotState& rst = dungeonroute::StateOf(bot);
        if (!rst.pullBackPending)
        {
            TankSpot back = PickTankSpot(bot, best, SPOT_SAFE,
                bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
            if (back.ok)
            {
                rst.pullBackPending = 1;
                rst.standOverrideX = back.x;
                rst.standOverrideY = back.y;
                rst.standOverrideZ = back.z;
                rst.standOverrideUntil = WorldTimer::getMSTime() + 20000;
                sLog.outError("[PULLBACK] %s 救场前挂拉回点 (%.0f,%.0f)（避 %s entry=%u）",
                    bot->GetName(), back.x, back.y, best->GetName(), best->GetEntry());
            }
        }
    }

    // ② 能拉就拉：嘲讽 + 攻击
    static uint32 const taunts[] = {355, 6795, 31789};
    for (size_t i = 0; i < sizeof(taunts) / sizeof(taunts[0]); ++i)
    {
        if (!sServerFacade.IsSpellReady(bot, taunts[i]))
            continue;
        if (ai->CastSpell(taunts[i], best))
            break;
    }
    bot->Attack(best, true);
    if (bot->GetDistance(best) > 5.0f)
        bot->GetMotionMaster()->MovePoint(0, best->GetPositionX(), best->GetPositionY(), best->GetPositionZ());
    sLog.outError("[ENGAGE2] %s 救场：%s(entry=%u) 在打队友，坦克接手（距 %.1f，威胁差 %u）",
        bot->GetName(), best->GetName(), best->GetEntry(), bot->GetDistance(best), worstDeficit);
    return true;
}


// [2026-10-25 站长口径] 控制模式：每个有 CC 的职业控一只高威胁怪，**不重复**；排除 priority 5；
//   坦克不参与（坦克负责把怪拉离被控目标）。
bool DungeonCcCastAction::Execute(Event& event)
{
    if (!bot || !bot->GetMap() || !bot->IsInCombat() || !bot->GetGroup())
        return false;
    if (PlayerbotAI::IsTank(bot))
        return false;

    // 我会的最高级那个控制法术
    std::vector<uint32> const spells = dungeonroute::CcSpellsFor(bot);
    uint32 spellId = 0;
    for (uint32 id : spells)
        if (bot->HasSpell(id) && sServerFacade.IsSpellReady(bot, id))
        {
            spellId = id;
            break;
        }
    if (!spellId)
        return false;

    // 收集本队战斗中的敌对活怪（排除 priority 5 / 已到 / 已分配），按威胁等级 + 距离排序
    struct Cand { Creature* c; int tier; float dist; };
    std::vector<Cand> cands;
    std::set<ObjectGuid> seen;
    uint32 const now = WorldTimer::getMSTime();
    for (GroupReference* ref = bot->GetGroup()->GetFirstMember(); ref; ref = ref->next())
    {
        Player* m = ref->getSource();
        if (!m || !m->IsInWorld())
            continue;
        for (Unit* a : m->getAttackers())
        {
            if (!a || !a->IsAlive() || a->GetTypeId() != TYPEID_UNIT || a->GetMapId() != bot->GetMapId())
                continue;
            if (!seen.insert(a->GetObjectGuid()).second)
                continue;
            Creature* c = static_cast<Creature*>(a);
            if (c->IsCritter() || !bot->CanAttack(c))
                continue;
            int const tier = PossibleAttackTargetsValue::ThreatTier(c, bot);
            if (tier >= 5)                                  // priority 5：优先击杀，不控
                continue;
            float const d = bot->GetDistance(c);
            if (d > 28.0f || d < 2.0f)                      // 太远控不到、太近控了也在挨打
                continue;
            if (!bot->IsWithinLOSInMap(c))
                continue;
            // 已经有人（含自己上一次）控住/分配了 ⇒ 不重复
            {
                std::lock_guard<std::mutex> lock(dungeonroute::CcMutex());
                auto it = dungeonroute::CcAssign().find(c->GetObjectGuid().GetRawValue());
                if (it != dungeonroute::CcAssign().end() && it->second > now)
                    continue;
            }
            cands.push_back(Cand{c, tier, d});
        }
    }
    if (cands.empty())
        return false;

    // 威胁最高优先；同级取近的
    std::sort(cands.begin(), cands.end(), [](Cand const& a, Cand const& b)
    {
        if (a.tier != b.tier)
            return a.tier > b.tier;
        return a.dist < b.dist;
    });

    // 每人一只：按"队伍里有 CC 的人"的 guid 排序，取自己的下标
    std::vector<uint32> ccers;
    for (GroupReference* ref = bot->GetGroup()->GetFirstMember(); ref; ref = ref->next())
    {
        Player* m = ref->getSource();
        if (!m || !m->IsInWorld() || PlayerbotAI::IsTank(m) || PlayerbotAI::IsHeal(m))
            continue;
        std::vector<uint32> const s2 = dungeonroute::CcSpellsFor(m);
        for (uint32 id : s2)
            if (m->HasSpell(id))
            {
                ccers.push_back(m->GetGUIDLow());
                break;
            }
    }
    std::sort(ccers.begin(), ccers.end());
    uint32 myIdx = 0;
    for (size_t i = 0; i < ccers.size(); ++i)
        if (ccers[i] == bot->GetGUIDLow())
            myIdx = uint32(i);

    if (myIdx >= cands.size())
        return false;                                       // 人比怪多 ⇒ 我没分到
    Creature* target = cands[myIdx].c;

    {
        std::lock_guard<std::mutex> lock(dungeonroute::CcMutex());
        dungeonroute::CcAssign()[target->GetObjectGuid().GetRawValue()] = now + 20000;
    }
    if (ai->CastSpell(spellId, target))
    {
        sLog.outError("[CC] %s 控住 %s(entry=%u)：法术 %u（威胁级 %d，距 %.0f，第 %u 个控制者）",
            bot->GetName(), target->GetName(), target->GetEntry(), spellId, cands[myIdx].tier,
            cands[myIdx].dist, myIdx + 1);
        return true;
    }
    return false;
}


// [2026-10-25 站长口径] 怪没聚齐 / 坦克仇恨不够 ⇒ DPS 与法系停手（坦克/治疗不受影响）。
// [2026-10-26 站长口径·修正] **放行判据必须和停手门（DungeonDpsHoldFireTrigger）一致**：
//   以前这里是"坦克在这只上有 >0 仇恨就 Attack"，等于把停手门自己废掉 ——
//   DPS 一路把仇恨打到 1.7~2 万，怪转头砍 DPS/奶（555 实测：坦克 2502 vs DPS 19877）。
bool DungeonDpsHoldFireAction::Execute(Event& event)
{
    if (!bot)
        return false;

    // 与触发器同一套阈值（绝对仇恨下限 + 相对百分比），两者改一处就够。
    auto tankLeadOk = [&](Creature* tgt, float& tankT, float& selfT, uint32& needAbs, uint32& needPct) -> bool
    {
        if (!tgt)
            return false;
        tankT = dungeonroute::ThreatOfTankOn(bot, tgt);
        selfT = tgt->getThreatManager().getThreat(bot);
        needAbs = sPlayerbotAIConfig.waitForAttackThreatLead;
        if (dungeonroute::IsRouteBoss(tgt->GetMapId(), tgt->GetGUIDLow()))
            needAbs = sPlayerbotAIConfig.waitForAttackThreatLeadBoss;
        if (dungeonroute::IsImmuneToTaunt(tgt))
            needAbs = sPlayerbotAIConfig.waitForAttackThreatLeadImmune;
        needPct = sPlayerbotAIConfig.waitForAttackThreatPercent ? sPlayerbotAIConfig.waitForAttackThreatPercent : 110;
        if (needAbs && tankT < float(needAbs))
            return false;                                   // 坦克还没建立最低仇恨
        if (selfT > 1.0f && tankT * 100.0f < selfT * float(needPct))
            return false;                                   // 坦克的领先不够（DPS 会 OT）
        return true;
    };

    // 全队集火坦克选定的那一只（杀一个减一个）
    Creature* focus = dungeonroute::FindTankFocusTarget(dungeonroute::GroupTank(bot));
    if (!focus)
        focus = dungeonroute::FindTankFocusTarget(bot);

    float tankT = 0.0f, selfT = 0.0f;
    uint32 needAbs = 0, needPct = 0;
    if (focus && tankLeadOk(focus, tankT, selfT, needAbs, needPct))
    {
        if (!bot->GetVictim() || bot->GetVictim()->GetObjectGuid() != focus->GetObjectGuid())
        {
            bot->Attack(focus, true);
            sLog.outError("[FOCUS] %s 集火 %s(entry=%u)：坦克仇恨 %.0f / 自己 %.0f（阈值 绝对 %u、百分比 %u%%）",
                bot->GetName(), focus->GetName(), focus->GetEntry(), tankT, selfT, needAbs, needPct);
        }
        return true;
    }

    if (bot->GetVictim())
    {
        dungeonroute::GatherState const g = dungeonroute::ComputeGather(dungeonroute::GroupTank(bot));
        bot->AttackStop();
        static std::map<ObjectGuid, uint32> s_lastHold;
        uint32& lastHold = s_lastHold[bot->GetObjectGuid()];
        uint32 const nowH = WorldTimer::getMSTime() ? WorldTimer::getMSTime() : 1;
        if (!lastHold || WorldTimer::getMSTimeDiff(lastHold, nowH) >= 5000)
        {
            lastHold = nowH;
            sLog.outError("[HOLDFIRE] %s 停手：坦克仇恨 %.0f / 自己 %.0f（阈值 绝对 %u、百分比 %u%%），"
                          "另有 %u 只没聚到 5 码内",
                bot->GetName(), tankT, selfT, needAbs, needPct, g.pending);
        }
    }
    return true;
}
