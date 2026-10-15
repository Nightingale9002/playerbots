#include "playerbot/playerbot.h"
#include "playerbot/DungeonRoute.h"

#include "playerbot/strategy/values/PossibleAttackTargetsValue.h"

#include "Maps/Map.h"
#include "Database/DatabaseEnv.h"
#include "Globals/ObjectMgr.h"

#include <mutex>
#include <ctime>
#include <map>

using namespace ai;

namespace
{
    // 路线表重载间隔（表是静态的，隔一会儿重读一次即可）
    const uint32 kTableRefreshSeconds = 300;

    // mapId -> (spawn guid -> 波次序)
    std::mutex g_tableMutex;
    std::map<uint32, std::map<uint32, uint16> > g_byMap;
    time_t g_tableAt = 0;
    bool g_tableLoaded = false;

    void EnsureTable()
    {
        {
            std::lock_guard<std::mutex> lock(g_tableMutex);
            if (g_tableLoaded && (time(nullptr) - g_tableAt) < (time_t)kTableRefreshSeconds)
                return;
        }

        std::map<uint32, std::map<uint32, uint16> > loaded;
        auto results = WorldDatabase.Query("SELECT `map`, `guid`, `ord` FROM `ai_playerbot_dungeon_route`");
        if (results)
        {
            do
            {
                Field* fields = results->Fetch();
                uint32 map = fields[0].GetUInt32();
                uint32 guid = fields[1].GetUInt32();
                uint16 ord = (uint16)fields[2].GetUInt32();
                if (map && guid)
                    loaded[map][guid] = ord;
            } while (results->NextRow());
        }

        std::lock_guard<std::mutex> lock(g_tableMutex);
        g_byMap.swap(loaded);
        g_tableAt = time(nullptr);
        g_tableLoaded = true;
    }
}

namespace ai
{
    namespace dungeonroute
    {
        uint16 GetOrder(PlayerbotAI* ai, Unit* unit)
        {
            if (!ai || !unit)
                return 0;

            Player* bot = ai->GetBot();
            if (!bot || !bot->IsInWorld())
                return 0;

            Map* map = bot->GetMap();
            if (!map || !(map->IsDungeon() || map->IsRaid()))
                return 0;

            EnsureTable();

            const uint32 mapId = bot->GetMapId();
            const uint32 spawnGuid = unit->GetObjectGuid().GetCounter();

            std::lock_guard<std::mutex> lock(g_tableMutex);
            auto it = g_byMap.find(mapId);
            if (it == g_byMap.end())
                return 0;

            auto found = it->second.find(spawnGuid);
            return found != it->second.end() ? found->second : 0;
        }

        Unit* GetNextOrderedTarget(PlayerbotAI* ai)
        {
            if (!ai)
                return nullptr;

            Player* bot = ai->GetBot();
            if (!bot || !bot->IsInWorld())
                return nullptr;

            Map* map = bot->GetMap();
            if (!map || !(map->IsDungeon() || map->IsRaid()))
                return nullptr;

            std::list<ObjectGuid> targets = ai->GetAiObjectContext()->
                GetValue<std::list<ObjectGuid> >("possible attack targets")->Get();

            ObjectGuid best;
            uint16 bestOrder = 0;
            for (std::list<ObjectGuid>::iterator i = targets.begin(); i != targets.end(); ++i)
            {
                Unit* unit = ai->GetUnit(*i);
                if (!unit || !unit->IsAlive() || !bot->CanAttack(unit))
                    continue;

                uint16 order = GetOrder(ai, unit);
                if (!order)   // 没被标记的怪不参与排序（由调用方回落原逻辑处理）
                    continue;

                if (!PossibleAttackTargetsValue::CanReachTarget(unit, bot))
                    continue;

                if (!bestOrder || order < bestOrder)
                {
                    bestOrder = order;
                    best = *i;
                }
            }

            return best ? ai->GetUnit(best) : nullptr;
        }
    }

        // ---------------------------------------------------------------------------
        // [wave 2026-10-18 站长口径] 波次推进（简化版）
        //   质心/距离都从既有表 + 刷点坐标现算，按 ord 缓存一次，不引入新表、不做聚类。
        // ---------------------------------------------------------------------------
        namespace
        {
            struct WaveInfo
            {
                WorldPosition center;
                uint32 total = 0;
                std::vector<WorldPosition> points;
            };

            std::mutex g_waveMutex;
            std::map<uint32, std::map<uint16, WaveInfo> > g_waveCache;
            time_t g_waveAt = 0;
            const uint32 kWaveCacheSeconds = 300;

            void BuildWaveCache()
            {
                std::map<uint32, std::map<uint16, WaveInfo> > built;

                auto results = WorldDatabase.Query("SELECT `map`, `guid`, `ord` FROM `ai_playerbot_dungeon_route`");
                if (!results)
                {
                    std::lock_guard<std::mutex> lock(g_waveMutex);
                    g_waveCache.swap(built);
                    g_waveAt = time(nullptr);
                    return;
                }

                do
                {
                    Field* fields = results->Fetch();
                    uint32 mapId = fields[0].GetUInt32();
                    uint32 guid = fields[1].GetUInt32();
                    uint16 ord = (uint16)fields[2].GetUInt32();
                    if (!mapId || !guid || !ord)
                        continue;

                    CreatureData const* data = sObjectMgr.GetCreatureData(guid);
                    if (!data)
                        continue;

                    WaveInfo& wave = built[mapId][ord];
                    wave.points.push_back(WorldPosition(mapId, data->posX, data->posY, data->posZ, 0.0f));
                    wave.total = (uint32)wave.points.size();
                } while (results->NextRow());

                for (auto& byMap : built)
                {
                    for (auto& byOrd : byMap.second)
                    {
                        WaveInfo& wave = byOrd.second;
                        if (wave.points.empty())
                            continue;

                        float x = 0.0f, y = 0.0f, z = 0.0f;
                        for (auto const& p : wave.points)
                        {
                            x += p.getX();
                            y += p.getY();
                            z += p.getZ();
                        }
                        float const n = (float)wave.points.size();
                        wave.center = WorldPosition(byMap.first, x / n, y / n, z / n, 0.0f);
                    }
                }

                std::lock_guard<std::mutex> lock(g_waveMutex);
                g_waveCache.swap(built);
                g_waveAt = time(nullptr);
            }

            void EnsureWaveCache()
            {
                {
                    std::lock_guard<std::mutex> lock(g_waveMutex);
                    if (g_waveAt && (time(nullptr) - g_waveAt) < (time_t)kWaveCacheSeconds && !g_waveCache.empty())
                        return;
                }

                BuildWaveCache();
            }
        }

        namespace dungeonroute
        {
        bool GetWaveCenter(PlayerbotAI* ai, uint16 ord, WorldPosition& out)
        {
            if (!ai || !ord)
                return false;

            Player* bot = ai->GetBot();
            if (!bot || !bot->IsInWorld())
                return false;

            EnsureWaveCache();

            std::lock_guard<std::mutex> lock(g_waveMutex);
            auto byMap = g_waveCache.find(bot->GetMapId());
            if (byMap == g_waveCache.end())
                return false;

            auto it = byMap->second.find(ord);
            if (it == byMap->second.end() || !it->second.center)
                return false;

            out = it->second.center;
            return true;
        }

        uint16 GetNextOrder(PlayerbotAI* ai, uint16 afterOrd)
        {
            if (!ai)
                return 0;

            Player* bot = ai->GetBot();
            if (!bot || !bot->IsInWorld())
                return 0;

            EnsureWaveCache();

            std::lock_guard<std::mutex> lock(g_waveMutex);
            auto byMap = g_waveCache.find(bot->GetMapId());
            if (byMap == g_waveCache.end())
                return 0;

            uint16 next = 0;
            for (auto const& byOrd : byMap->second)
            {
                if (byOrd.first > afterOrd && (!next || byOrd.first < next))
                    next = byOrd.first;
            }

            return next;
        }

        uint32 GetWaveCount(PlayerbotAI* ai, uint16 ord)
        {
            if (!ai || !ord)
                return 0;

            Player* bot = ai->GetBot();
            if (!bot || !bot->IsInWorld())
                return 0;

            EnsureWaveCache();

            std::lock_guard<std::mutex> lock(g_waveMutex);
            auto byMap = g_waveCache.find(bot->GetMapId());
            if (byMap == g_waveCache.end())
                return 0;

            auto it = byMap->second.find(ord);
            return it != byMap->second.end() ? it->second.total : 0;
        }

        float GetWaveDistance(PlayerbotAI* ai, uint16 ord)
        {
            if (!ai || !ord)
                return -1.0f;

            Player* bot = ai->GetBot();
            if (!bot || !bot->IsInWorld())
                return -1.0f;

            EnsureWaveCache();

            std::lock_guard<std::mutex> lock(g_waveMutex);
            auto byMap = g_waveCache.find(bot->GetMapId());
            if (byMap == g_waveCache.end())
                return -1.0f;

            auto it = byMap->second.find(ord);
            if (it == byMap->second.end() || it->second.points.empty())
                return -1.0f;

            float best = -1.0f;
            for (auto const& p : it->second.points)
            {
                float const d = bot->GetDistance(p.getX(), p.getY(), p.getZ());
                if (best < 0.0f || d < best)
                    best = d;
            }

            return best;
        }
        } // namespace dungeonroute
}
