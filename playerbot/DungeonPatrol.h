#pragma once

#include "playerbot/playerbot.h"
#include "playerbot/WorldPosition.h"

// ---------------------------------------------------------------------------
// [patrol 2026-10-18 站长口径] 副本内"巡逻怪"识别。
//
// **v2 口径（现行）：类型B 巡逻怪不躲，战斗中看情况清掉。**
//   离线生成器把巡逻怪分成两类（见 dev/tools/patrol.py）：
//     · 类型A：整条路线都在同一个遭遇组领地内 ⇒ 已经并进 `ai_playerbot_dungeon_route`，
//              `kind=1`，跟那一波一起拉、一起清，本表里**没有它**。
//     · 类型B：跨组 / 无组 ⇒ 留在本表，`patrol_type='B'`。它自己会走，位置没法预先排进 ord，
//              所以**不进波次**；运行时"遇上了就清"，不再等待、不再换战斗点。
//
// 于是本模块现在只干两件事：
//   ① 主动开怪时：**把巡逻怪当目标**要求它孤立（它会走开，主动开容易白跑）；
//      但"拉静态怪会顺手把巡逻怪拖进来"**不再拒绝**（旧版拒绝 ⇒ 坦克原地不动）。
//   ② 判断某个坐标是不是贴着巡逻路线（`RouteNear`），给"选卡视野点/换点"当**偏好**用，
//      不再当作硬拒。
//
// 数据**不在运行时现算**，而是**离线预生成**后存世界库表
//   `ai_playerbot_patrol(map, guid, entry, point, x, y, z, detection, patrol_type)`
//   （guid = creature 的 spawn guid）。运行时只读（每 kTableRefreshSeconds 重载）。
//   表里没有该副本 ⇒ 无巡逻数据 ⇒ 所有判据都按"安全"返回（行为等同改动前）。
//
// ⚠️ 现状：`DungeonPatrol.cpp → EnsureTable()` 的 SQL 还没取 `patrol_type`，
//   等于把所有行都当 B（正好符合 v2 口径：表里现在也只写 B 类）。
//   以后要按类型区分时再加这一列，别只改一半。
//
// 预生成：`dev/tools/route_v2.py --map <mapId> --emit`（v2；旧的 dungeon_route_gen.py 是 v1）。
//
// 适用面：仅副本 / 团本（`Map::IsDungeon()` / `IsRaid()`）。非副本一律"无巡逻数据"。
// ---------------------------------------------------------------------------

namespace ai
{
    namespace dungeonpatrol
    {
        // 这只怪是否登记为"巡逻怪"（非副本 / 表里没有 ⇒ false）
        bool IsPatrol(PlayerbotAI* ai, Unit* unit);

        // 以 pos 为圆心、radius 内是否有巡逻怪的路线点（radius 已含 Detection，调用方再叠加余量）
        bool RouteNear(PlayerbotAI* ai, const WorldPosition& pos, float extraMargin);

        // 这只巡逻怪是否"孤立"：radius 内没有别的可攻击怪（不含它自己）
        bool IsIsolated(PlayerbotAI* ai, Creature* patrol, float radius);
    }
}
