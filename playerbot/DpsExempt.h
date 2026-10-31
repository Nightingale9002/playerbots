/*
 * This file is part of CMaNGOS PlayerBots.
 *
 * [2026-10-26 站长口径] **DPS 仇恨门豁免表**（`ai_playerbot_dps_exempt`）
 *   背景：副本里 DPS 默认必须等坦克把仇恨建立到门槛才许开打（AttackAction::IsWaitingForTankThreat）。
 *   但有些怪"必须立刻处理"，等坦克仇恨会出大事，典型如禁魔监狱(554) 的 Protean Spawn(21395)：
 *   它会不断分裂/刷新，等坦克建立仇恨的时间窗里队伍就被淹了。
 *   所以这里提供一张**按图 + entry 的数据表**来豁免仇恨门（数据驱动，改表不用改代码）：
 *     CREATE TABLE ai_playerbot_dps_exempt (`map` INT UNSIGNED, `entry` INT UNSIGNED, `note` VARCHAR(120));
 *   命中 ⇒ IsWaitingForTankThreat 直接返回 false（DPS 可以开打），
 *   但**不改变击杀优先级**（那由 ai_playerbot_priority_target 决定）、也不改变停手逻辑。
 */

#ifndef _PLAYERBOT_DPS_EXEMPT_H
#define _PLAYERBOT_DPS_EXEMPT_H

#include <set>
#include <memory>
#include <utility>

#include "Database/DatabaseEnv.h"

namespace dpsexempt
{
    inline std::set<std::pair<uint32, uint32> >& Table()
    {
        static std::set<std::pair<uint32, uint32> > t;
        return t;
    }

    inline void Load()
    {
        Table().clear();
        std::unique_ptr<QueryResult> r(WorldDatabase.PQuery("SELECT `map`,`entry` FROM ai_playerbot_dps_exempt"));
        if (!r)
            return;
        do
        {
            Field* f = r->Fetch();
            Table().insert(std::make_pair(f[0].GetUInt32(), f[1].GetUInt32()));
        } while (r->NextRow());
        sLog.outString("[DPSX] 仇恨门豁免表载入 %u 条", uint32(Table().size()));
    }

    inline void EnsureLoaded()
    {
        static bool loaded = false;
        if (loaded)
            return;
        loaded = true;
        Load();
    }

    // GM 命令改写表之后调用（原地刷新内存）
    inline void Reload() { Load(); }

    // 这个 (地图, entry) 是否豁免"等坦克仇恨"这道门
    inline bool Is(uint32 mapId, uint32 entry)
    {
        if (!entry)
            return false;
        EnsureLoaded();
        return Table().count(std::make_pair(mapId, entry)) != 0;
    }
}

#endif
