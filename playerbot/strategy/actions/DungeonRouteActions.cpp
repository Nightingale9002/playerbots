
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

    // 卡住保护：同一波超过 WAVE_GIVE_UP_MS 没有推进（且没在打）⇒ 放弃这波，下次瞄下一波
    // [2026-10-22 站长口径] 只要还有怪在和我们战斗（包括**没登记进路线表**的怪）就不许跳过。
    //   实测日志里出现「跳过 ord=N（wave stuck: no progress）」时，队伍其实正被一波未登记怪缠着。
    uint16 ord = dungeonroute::CurrentOrd(bot, *route);
    if (ord && dungeonroute::WaveStuckMs(bot) > dungeonroute::WAVE_GIVE_UP_MS &&
        !dungeonroute::WaveInFight(bot, *route, ord) &&
        !dungeonroute::UnknownFighterInCombat(bot, *route))
    {
        dungeonroute::MarkSkipped(bot, ord, "wave stuck: no progress");
        return false;
    }

    dungeonroute::Target target;
    if (!dungeonroute::FindTargetNearest(bot, *route, target, true) || !target.hasPullPoint)
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
    if (!dungeonroute::ShouldDrive(ai))
        return false;

    Map* map = bot->GetMap();
    dungeonroute::MapRoute const* route = dungeonroute::GetMapRoute(map->GetId());
    if (!route)
        return false;
    if (!route->verified && !dungeonrun::AllowUnverified(dungeonroute::RouteRuntimeSwitch()))
        return false;

    // [2026-10-23 站长口径] 同 MoveAction：把"要躲的怪"灌进 avoid creature list（躲地板链路）
    if (!route->avoidEntries.empty())
    {
        std::set<uint32>& avoid = AI_VALUE(std::set<uint32>&, "avoid creature list");
        for (uint32 e : route->avoidEntries)
            avoid.insert(e);
    }

    // [2026-10-18 站长口径] 拉怪记录由核心统一采集（见 DungeonRoute.h §拉怪记录）

    dungeonroute::Target target;
    if (!dungeonroute::FindTargetNearest(bot, *route, target, true) || !target.hasPullPoint)
        return false;

    if (bot->GetDistance(target.px, target.py, target.pz) > dungeonroute::AT_PULL_POINT_DIST)
        return false;    // 还没到位 ⇒ 交给 DungeonRouteMoveAction

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
                return false;
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
