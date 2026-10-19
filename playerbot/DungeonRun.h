#pragma once
#include "playerbot/playerbot.h"
#include "playerbot/PlayerbotAIConfig.h"

#include <map>
#include <mutex>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// [自动进本 2026-10-18 站长口径] "首先要让 bot 去自动进本打怪"。
//
// 做法：驱动坦克（DungeonRoute 的 ShouldDrive）发现队伍**不在副本里**时：
//   ① 从配置 AiPlayerbot.AutoDungeonMaps 里挑一个"等级够"的目标副本；
//   ② 用 areatrigger_teleport 的 target_map/target_position（服务端现成的副本入口数据）
//      把队里的 bot 全部传送进副本（真人玩家不传，混合队伍直接放弃自动进本）；
//   ③ 进本后由 DungeonRoute 的按波推进接管打怪（ord 拉怪 → 清波 → boss）。
//
// 开关：与 `.bot route on/off` 共用运行时开关（route on 同时打开自动进本）；
//       配置 AiPlayerbot.AutoDungeonRun（默认 0）作为默认值。
// 失败/没配置/数据缺失 ⇒ 全部静默返回 false，不影响原有行为。
// ---------------------------------------------------------------------------

namespace dungeonrun
{
    struct Entrance
    {
        float x, y, z, o;
        uint8 requiredLevel;
        bool has;

        Entrance() : x(0), y(0), z(0), o(0), requiredLevel(0), has(false) {}
    };

    inline std::mutex& EntranceMutex() { static std::mutex m; return m; }
    inline std::map<uint32, Entrance>& Entrances() { static std::map<uint32, Entrance> e; return e; }
    inline time_t& EntranceLoadedAt() { static time_t t = 0; return t; }

    // 与 .bot route on/off 共用开关；-1 跟配置
    inline bool Enabled(int routeSwitch)
    {
        if (routeSwitch == 0)
            return false;
        if (routeSwitch == 1)
            return true;
        return sPlayerbotAIConfig.autoDungeonRun;
    }

    // 自动进本模式下允许未 verified 的图（站长口径：先让 bot 能自己进本打）
    inline bool AllowUnverified(int routeSwitch) { return Enabled(routeSwitch); }

    // 配置里的目标副本列表（逗号分隔的 map id）
    inline std::vector<uint32> Targets()
    {
        std::vector<uint32> out;
        std::string const& list = sPlayerbotAIConfig.autoDungeonMaps;
        size_t start = 0;
        while (start < list.size())
        {
            size_t end = list.find(',', start);
            std::string token = list.substr(start, end == std::string::npos ? std::string::npos : end - start);
            while (!token.empty() && (token.front() == ' ' || token.front() == '\t' || token.front() == '\r'))
                token.erase(token.begin());
            while (!token.empty() && (token.back() == ' ' || token.back() == '\t' || token.back() == '\r'))
                token.pop_back();
            if (!token.empty())
            {
                int v = atoi(token.c_str());
                if (v > 0)
                    out.push_back(uint32(v));
            }
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
        return out;
    }

    // 从服务端现成数据加载"副本入口落点"（areatrigger_teleport）
    inline void EnsureEntrances()
    {
        std::scoped_lock lock(EntranceMutex());
        const time_t now = time(nullptr);
        if (EntranceLoadedAt() && now - EntranceLoadedAt() < 300)
            return;
        EntranceLoadedAt() = now;

        std::unique_ptr<QueryResult> res = WorldDatabase.Query(
            "SELECT `target_map`,`required_level`,`target_position_x`,`target_position_y`,"
            "`target_position_z`,`target_orientation` FROM `areatrigger_teleport`");
        if (!res)
            return;

        std::map<uint32, Entrance> loaded;
        do
        {
            Field* fields = res->Fetch();
            uint32 mapId = fields[0].GetUInt32();
            if (!mapId || loaded.count(mapId))
                continue;

            Entrance e;
            e.requiredLevel = uint8(fields[1].GetUInt32());
            e.x = fields[2].GetFloat();
            e.y = fields[3].GetFloat();
            e.z = fields[4].GetFloat();
            e.o = fields[5].GetFloat();
            e.has = true;
            loaded[mapId] = e;
        } while (res->NextRow());

        Entrances().swap(loaded);
        if (!Entrances().empty())
            sLog.outString("[DUNGEONRUN] 副本入口数据 %u 个", (uint32)Entrances().size());
    }

    // 返回 true = 已经在本里（交给路线推进）；false = 本次不推进（已发起进本/条件不满足）
    inline bool MaybeEnterDungeon(Player* bot, int routeSwitch)
    {
        if (!Enabled(routeSwitch) || !bot || !bot->IsInWorld() || bot->IsBeingTeleported())
            return false;

        Map* map = bot->GetMap();
        if (map && (map->IsDungeon() || map->IsRaid()))
            return true;

        if (!bot->IsAlive())
            return false;

        std::vector<uint32> targets = Targets();
        if (targets.empty())
            return false;

        Group* group = bot->GetGroup();
        if (!group)
            return false;

        // [2026-10-18 修正] 允许混合队伍：只把 bot 传送进本，真人玩家不动（GM 测试最常见的用法）

        // 节流：同一个 bot 5 秒最多尝试一次（避免传送失败时每 tick 刷）
        static std::mutex throttleMutex;
        static std::map<uint32, uint32> lastTry;
        const uint32 now = WorldTimer::getMSTime();
        {
            std::scoped_lock lock(throttleMutex);
            uint32& last = lastTry[bot->GetGUIDLow()];
            if (last && WorldTimer::getMSTimeDiff(last, now) < 5000)
                return false;
            last = now;
        }

        EnsureEntrances();
        if (Entrances().empty())
            return false;

        // 挑第一个"等级够"的目标本
        uint32 targetMap = 0;
        Entrance entrance;
        for (uint32 mapId : targets)
        {
            auto it = Entrances().find(mapId);
            if (it == Entrances().end() || !it->second.has)
                continue;
            if (it->second.requiredLevel && bot->GetLevel() < it->second.requiredLevel)
                continue;
            targetMap = mapId;
            entrance = it->second;
            break;
        }
        if (!targetMap)
            return false;

        uint32 moved = 0;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->getSource();
            if (!member || !member->IsInWorld() || !member->GetPlayerbotAI() || member->IsBeingTeleported())
                continue;

            member->TeleportTo(targetMap, entrance.x, entrance.y, entrance.z, entrance.o);
            ++moved;
        }

        if (moved)
        {
            sLog.outString("[DUNGEONRUN] group=%u 自动进本 map=%u members=%u（lead=%s）",
                group->GetId(), targetMap, moved, bot->GetName());
        }

        return false;
    }
}
