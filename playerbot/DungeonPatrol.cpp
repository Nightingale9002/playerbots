#include "playerbot/playerbot.h"
#include "playerbot/DungeonPatrol.h"

#include "Grids/GridNotifiers.h"
#include "Grids/GridNotifiersImpl.h"
#include "Grids/CellImpl.h"
#include "Database/DatabaseEnv.h"
#include "Maps/Map.h"

#include <mutex>
#include <ctime>
#include <map>
#include <set>
#include <vector>

using namespace ai;

namespace
{
    // 巡逻表重载间隔（表是静态的）
    const uint32 kTableRefreshSeconds = 300;

    struct PatrolPoint
    {
        float x, y, z;
        float radius;   // = 该怪 Detection（安全距离在此基础上再加余量）
    };

    struct PatrolData
    {
        std::set<uint32> guids;              // 巡逻怪 spawn guid 集合
        std::vector<PatrolPoint> points;     // 巡逻路线点（含 Detection）
    };

    std::mutex g_mutex;
    std::map<uint32, PatrolData> g_byMap;    // mapId -> 巡逻数据
    time_t g_at = 0;
    bool g_loaded = false;

    void EnsureTable()
    {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_loaded && (time(nullptr) - g_at) < (time_t)kTableRefreshSeconds)
                return;
        }

        std::map<uint32, PatrolData> loaded;
        auto results = WorldDatabase.Query(
            "SELECT `map`, `guid`, `x`, `y`, `z`, `detection` FROM `ai_playerbot_patrol`");
        if (results)
        {
            do
            {
                Field* fields = results->Fetch();
                uint32 map = fields[0].GetUInt32();
                uint32 guid = fields[1].GetUInt32();
                if (!map || !guid)
                    continue;

                PatrolPoint p;
                p.x = fields[2].GetFloat();
                p.y = fields[3].GetFloat();
                p.z = fields[4].GetFloat();
                p.radius = fields[5].GetFloat();

                PatrolData& data = loaded[map];
                data.guids.insert(guid);
                data.points.push_back(p);
            } while (results->NextRow());
        }

        std::lock_guard<std::mutex> lock(g_mutex);
        g_byMap.swap(loaded);
        g_at = time(nullptr);
        g_loaded = true;
    }

    // 取 bot 所在 mapId；非副本/团本返回 0
    uint32 DungeonMapId(PlayerbotAI* ai)
    {
        if (!ai)
            return 0;

        Player* bot = ai->GetBot();
        if (!bot || !bot->IsInWorld())
            return 0;

        Map* map = bot->GetMap();
        if (!map || !(map->IsDungeon() || map->IsRaid()))
            return 0;

        return bot->GetMapId();
    }
}

namespace ai
{
    namespace dungeonpatrol
    {
        bool IsPatrol(PlayerbotAI* ai, Unit* unit)
        {
            const uint32 mapId = DungeonMapId(ai);
            if (!mapId || !unit)
                return false;

            EnsureTable();

            const uint32 spawnGuid = unit->GetObjectGuid().GetCounter();

            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_byMap.find(mapId);
            if (it == g_byMap.end())
                return false;

            return it->second.guids.find(spawnGuid) != it->second.guids.end();
        }

        bool RouteNear(PlayerbotAI* ai, const WorldPosition& pos, float extraMargin)
        {
            const uint32 mapId = DungeonMapId(ai);
            if (!mapId)
                return false;

            EnsureTable();

            const float px = pos.getX();
            const float py = pos.getY();
            const float pz = pos.getZ();

            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_byMap.find(mapId);
            if (it == g_byMap.end())
                return false;

            for (auto const& p : it->second.points)
            {
                const float r = p.radius + extraMargin;
                const float dx = px - p.x;
                const float dy = py - p.y;
                const float dz = pz - p.z;
                if (dx * dx + dy * dy + dz * dz <= r * r)
                    return true;
            }

            return false;
        }

        bool IsIsolated(PlayerbotAI* ai, Creature* patrol, float radius)
        {
            const uint32 mapId = DungeonMapId(ai);
            if (!mapId || !patrol)
                return false;

            Player* bot = ai->GetBot();

            std::list<Unit*> nearby;
            MaNGOS::AnyUnitInObjectRangeCheck check(patrol, radius);
            MaNGOS::UnitListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(nearby, check);
            Cell::VisitAllObjects(patrol, searcher, radius);

            for (Unit* unit : nearby)
            {
                if (!unit || unit == patrol || !unit->IsCreature() || !unit->IsAlive())
                    continue;

                if (bot->CanAttack(unit))
                    return false;
            }

            return true;
        }
    }
}
