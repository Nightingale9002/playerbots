#pragma once

#include "playerbot/playerbot.h"
#include "playerbot/WorldPosition.h"

// ---------------------------------------------------------------------------
// [dungeon-route 2026-10-17 站长口径] 副本内"入口 → BOSS"推进路线的拉怪排序。
//
// 干什么：坦克拉怪时优先打"波次序"（order）最小的存活怪 ⇒ 前一波打完才拉后一波（不越波），
//   整体沿"入口 → BOSS1 → BOSS2 → ..."的推进线由近到远。
//
// 顺序是**固定**的：不在运行时现算，而是**离线预生成**后存到世界库表
//   `ai_playerbot_dungeon_route(map, guid, ord)`（guid = creature 的 spawn guid）。
//   运行时**只读**该表（每 kTableRefreshSeconds 秒重载一次），表里没有该副本 ⇒ 无路线
//   （坦克回落原逻辑，不越波约束失效）。
//
// 预生成（离线，见 dev/tools/dungeon_route_gen.py + dev/tools/README_mmtile_vis.md）：
//   `python dev/tools/dungeon_route_gen.py --map <mapId>` 用 mmaps 导航网格现算一遍，
//   导出 `dev/dungeon_route_<mapId>.sql`；人工执行该 SQL 入库。算法：
//     · 入口  = areatrigger_teleport 里 target_map == 本图的落点
//     · BOSS  = 登记表 tbccharacters.ai_playerbot_boss_strategy 的 entry（或
//               creature_template.ScriptName LIKE 'boss_%'），在本图的 spawn
//     · 顺序  = 各 BOSS"从入口沿 mmap 走过去的路径长度"升序 = 入口 → b1 → b2 → ...
//     · 路线  = 逐段 Detour A* 拼接（走楼梯/坡道，不穿层、天然多层）
//     · 标记  = 沿路线每个采样点周围（该怪 Detection + 5）内的怪依次赋 ord；
//               多层护栏 |Δz| ≤ 6；可达护栏 = 怪的最近 poly 在路线 poly 集合里。
//
// 适用面：仅副本 / 团本（`Map::IsDungeon()` / `IsRaid()`）。非副本一律 0。
// ---------------------------------------------------------------------------

namespace ai
{
    namespace dungeonroute
    {
        // 这只怪的"波次序"（0 = 未知 / 不适用：非副本、表里没这个副本、或不在推进路线覆盖范围）
        uint16 GetOrder(PlayerbotAI* ai, Unit* unit);

        // 坦克用：在"可攻击目标"里挑序号最小（最靠前）且存活的怪；没有路线时返回 nullptr（调用方回落原逻辑）
        Unit* GetNextOrderedTarget(PlayerbotAI* ai);
        // [wave 2026-10-18 站长口径] **波次推进（简化版：不聚类、不中心线、不新表）**。
        //   波次 = `ai_playerbot_dungeon_route.ord`；波次质心 = 同 ord 的刷点均值（按 ord 缓存）。
        //   全部从既有表 + `sObjectMgr.GetCreatureData(guid)` 现算，不需要 DBSCAN / 中心线 / 宽点表。
        bool GetWaveCenter(PlayerbotAI* ai, uint16 ord, WorldPosition& out);
        uint16 GetNextOrder(PlayerbotAI* ai, uint16 afterOrd);
        uint32 GetWaveCount(PlayerbotAI* ai, uint16 ord);
        // 我离"该波次最近的一只"有多远（码）；该波次不存在返回 -1
        float GetWaveDistance(PlayerbotAI* ai, uint16 ord);
    }
}
