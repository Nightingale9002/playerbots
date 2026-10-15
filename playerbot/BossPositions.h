#pragma once

#include "playerbot/playerbot.h"

#include "Database/DatabaseEnv.h"
#include "Maps/Map.h"
#include "Globals/ObjectMgr.h"

#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// [boss-pos 2026-10-18 站长口径] **BOSS 战站位按 BOSS 单独存**（不用 conf 里的全局名单）：
//   表：tbcworld.ai_playerbot_boss_position(`entry`, `map`, `bot_role`, `x`, `y`, `z`, `radius`)
//   粒度：只按角色 —— tank / healer / melee / ranged（站长 2026-10-18 拍板）。
//   录入：游戏内 `.bot bosspos ...`（见 PlayerbotHolder::HandleBosspos），写库后**立即生效**，不用重启。
//   使用：StandBossPositionAction（动作名 "boss position"）—— 开战命中登记过的 BOSS 后，
//         各角色站到自己的点；`radius` 同时是"不许走出 BOSS 战范围"的护栏。
//   缓存：300 秒重读一次（数据是静态的）；命令写完强制失效 ⇒ 下一次判定就吃到新值。
// ---------------------------------------------------------------------------
namespace bossposition
{
    enum Role : uint32
    {
        ROLE_TANK = 0,
        ROLE_HEALER = 1,
        ROLE_MELEE = 2,
        ROLE_RANGED = 3,
        ROLE_COUNT = 4
    };

    const uint32 kRoleCount = 4;
    const uint32 kRefreshSeconds = 300;
    const float kDefaultRadius = 8.0f;

    struct Spot
    {
        uint32 mapId = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float radius = kDefaultRadius;
    };

    struct Row
    {
        uint32 entry = 0;
        uint32 role = 0;
        Spot spot;
    };

    inline char const* RoleName(uint32 role)
    {
        switch (role)
        {
            case ROLE_TANK:   return "tank";
            case ROLE_HEALER: return "healer";
            case ROLE_MELEE:  return "melee";
            case ROLE_RANGED: return "ranged";
            default:          return "";
        }
    }

    // 不认识 ⇒ -1（宁缺勿滥）
    inline int32 RoleFromName(std::string name)
    {
        for (char& c : name)
            c = (char)tolower(c);

        for (uint32 role = 0; role < kRoleCount; ++role)
        {
            if (name == RoleName(role))
                return (int32)role;
        }

        return -1;
    }

    // 这个 bot 在这场战斗里按哪个角色站位（坦克优先；治疗其次；其余按近战/远程分）
    inline uint32 RoleOf(PlayerbotAI* ai, Player* bot)
    {
        if (!ai || !bot)
            return ROLE_RANGED;

        if (PlayerbotAI::IsTank(bot))
            return ROLE_TANK;
        if (PlayerbotAI::IsHeal(bot))
            return ROLE_HEALER;
        if (ai->IsMelee(bot))
            return ROLE_MELEE;

        return ROLE_RANGED;
    }

    // ---- 数据（函数内静态量：头文件被多个 TU 包含也只有一份） ----
    inline std::mutex& Mutex() { static std::mutex m; return m; }
    inline std::map<uint32, Spot>& Table() { static std::map<uint32, Spot> t; return t; }   // key = entry * kRoleCount + role
    inline time_t& LoadedAt() { static time_t t = 0; return t; }
    inline bool& Loaded() { static bool b = false; return b; }

    // 每个 bot 的"正在打的 BOSS entry"缓存（1 秒），键 = PlayerbotAI*，析构里 Forget() 清掉
    inline std::map<PlayerbotAI*, std::pair<uint32, time_t> >& FightCache() { static std::map<PlayerbotAI*, std::pair<uint32, time_t> > c; return c; }

    inline uint32 Key(uint32 entry, uint32 role) { return entry * kRoleCount + role; }

    inline void Load()
    {
        std::map<uint32, Spot> loaded;

        auto results = WorldDatabase.Query(
            "SELECT `entry`, `map`, `bot_role`, `x`, `y`, `z`, `radius` FROM `ai_playerbot_boss_position`");
        if (results)
        {
            do
            {
                Field* fields = results->Fetch();
                uint32 entry = fields[0].GetUInt32();
                if (!entry)
                    continue;

                int32 role = RoleFromName(fields[2].GetString());
                if (role < 0)
                    continue;

                Spot spot;
                spot.mapId = fields[1].GetUInt32();
                spot.x = fields[3].GetFloat();
                spot.y = fields[4].GetFloat();
                spot.z = fields[5].GetFloat();
                spot.radius = fields[6].GetFloat();
                if (spot.radius <= 0.0f)
                    spot.radius = kDefaultRadius;

                loaded[Key(entry, (uint32)role)] = spot;
            } while (results->NextRow());
        }

        std::scoped_lock lock(Mutex());
        Table().swap(loaded);
        LoadedAt() = time(nullptr);
        Loaded() = true;

        if (!Table().empty())
            sLog.outString("[BOSSPOS] ai_playerbot_boss_position: %u rows", (uint32)Table().size());
    }

    inline void EnsureLoaded()
    {
        {
            std::scoped_lock lock(Mutex());
            if (Loaded() && (time(nullptr) - LoadedAt()) < (time_t)kRefreshSeconds)
                return;
        }

        Load();
    }

    // 强制下一次判定重读（命令写完调用）
    inline void Invalidate()
    {
        std::scoped_lock lock(Mutex());
        Loaded() = false;
    }

    inline bool Get(uint32 entry, uint32 mapId, uint32 role, Spot& out)
    {
        if (!entry)
            return false;

        EnsureLoaded();

        std::scoped_lock lock(Mutex());
        auto it = Table().find(Key(entry, role));
        if (it == Table().end())
            return false;

        // 表里写了 map 就只在该 map 生效（0 = 不限）
        if (mapId && it->second.mapId && it->second.mapId != mapId)
            return false;

        out = it->second;
        return true;
    }

    inline bool Has(uint32 entry, uint32 mapId)
    {
        if (!entry)
            return false;

        EnsureLoaded();

        std::scoped_lock lock(Mutex());
        for (uint32 role = 0; role < kRoleCount; ++role)
        {
            auto it = Table().find(Key(entry, role));
            if (it == Table().end())
                continue;
            if (mapId && it->second.mapId && it->second.mapId != mapId)
                continue;

            return true;
        }

        return false;
    }

    // entry = 0 ⇒ 全部
    inline std::vector<Row> List(uint32 entry)
    {
        EnsureLoaded();

        std::vector<Row> rows;
        std::scoped_lock lock(Mutex());
        for (auto const& [key, spot] : Table())
        {
            uint32 e = key / kRoleCount;
            if (entry && e != entry)
                continue;

            Row row;
            row.entry = e;
            row.role = key % kRoleCount;
            row.spot = spot;
            rows.push_back(row);
        }

        return rows;
    }

    inline bool Set(uint32 entry, uint32 mapId, uint32 role, float x, float y, float z, float radius)
    {
        if (!entry || role >= kRoleCount)
            return false;

        if (radius <= 0.0f)
            radius = kDefaultRadius;

        WorldDatabase.PExecute(
            "REPLACE INTO `ai_playerbot_boss_position` (`entry`, `map`, `bot_role`, `x`, `y`, `z`, `radius`) "
            "VALUES (%u, %u, '%s', %f, %f, %f, %f)",
            entry, mapId, RoleName(role), x, y, z, radius);

        Invalidate();
        return true;
    }

    // role = -1 ⇒ 删这个 BOSS 的全部角色
    inline void Delete(uint32 entry, int32 role)
    {
        if (!entry)
            return;

        if (role >= 0 && role < (int32)kRoleCount)
            WorldDatabase.PExecute("DELETE FROM `ai_playerbot_boss_position` WHERE `entry` = %u AND `bot_role` = '%s'", entry, RoleName((uint32)role));
        else
            WorldDatabase.PExecute("DELETE FROM `ai_playerbot_boss_position` WHERE `entry` = %u", entry);

        Invalidate();
    }

    // ---- "现在在打哪个 BOSS" ----
    inline uint32 RegisteredEntry(Unit* unit, uint32 mapId)
    {
        if (!unit || unit->GetTypeId() != TYPEID_UNIT || !unit->IsAlive())
            return 0;

        uint32 entry = unit->GetEntry();
        return Has(entry, mapId) ? entry : 0;
    }

    // 队伍（含真人）正在打的、登记过站位的 BOSS entry；没有就 0。每个 bot 缓存 1 秒。
    inline uint32 FightEntry(PlayerbotAI* ai, Player* bot)
    {
        if (!ai || !bot)
            return 0;

        time_t now = time(nullptr);

        {
            std::scoped_lock lock(Mutex());
            auto it = FightCache().find(ai);
            if (it != FightCache().end() && it->second.second && (now - it->second.second) < 1)
                return it->second.first;
        }

        uint32 mapId = bot->GetMapId();
        uint32 entry = RegisteredEntry(bot->GetVictim(), mapId);
        if (!entry)
            entry = RegisteredEntry(bot->GetMap()->GetCreature(bot->GetSelectionGuid()), mapId);

        if (!entry && bot->IsInCombat())
        {
            if (Group* group = bot->GetGroup())
            {
                for (GroupReference* ref = group->GetFirstMember(); ref && !entry; ref = ref->next())
                {
                    Player* member = ref->getSource();
                    if (!member || member == bot || !member->IsInWorld())
                        continue;

                    entry = RegisteredEntry(member->GetVictim(), mapId);
                    if (!entry)
                        entry = RegisteredEntry(member->GetMap()->GetCreature(member->GetSelectionGuid()), mapId);
                }
            }
        }

        {
            std::scoped_lock lock(Mutex());
            FightCache()[ai] = std::make_pair(entry, now);
        }

        return entry;
    }

    // 由 ~PlayerbotAI() 调用
    inline void Forget(PlayerbotAI* ai)
    {
        std::scoped_lock lock(Mutex());
        FightCache().erase(ai);
    }
}
