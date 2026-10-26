#pragma once

// ---------------------------------------------------------------------------
// [卡视野点库 2026-10-26 站长口径] 离线预筛一批"贴着墙、怪从别的方向看不见"的点，
//   存进 ai_playerbot_cover_spot；战斗时不需要现场盲扫，直接从库里挑最合适的点。
//
// 生成：控制台 `.rndbot sim spots <mapId|all>`（或 `spots rebuild <mapId>`）
//   ① 用本图路线怪（ai_playerbot_dungeon_route + creature 刷点）的包围盒当扫描范围；
//   ② 2 码一格稠密取点：必须在导航网格上，且离墙 <= 0.8 码（贴墙站）；
//   ③ 站上去 1.2 码半径的实心遮挡 >= 180 度（贴墙 ⇒ 保底半球）；
//   ④ 视线到本图每只路线怪都**被挡住**（seen_max_ord 记录"能看到这个点的最大波次"，
//      运行时只有 ord > seen_max_ord 才当候选，前面波次的怪已经清了看不见它了）。
//
// 运行期：`coverspot::Find(mapId, ord, 附近坐标, 半径)` 直接取最近可用点，再现场复核一次。
// ---------------------------------------------------------------------------

#include "playerbot/playerbot.h"
#include "playerbot/DungeonRoute.h"
#include "Globals/ObjectMgr.h"
#include "vmap/VMapFactory.h"
#include "World/World.h"
#include "Maps/GridDefines.h"
#include "MotionGenerators/MoveMap.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <vector>

namespace coverspot
{
    struct Row
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
        float wallDist = 0.0f;
        uint16 seenMaxOrd = 0;      // 能看到这个点的最大波次；0 = 谁都看不见
        uint16 seenCount = 0;
    };

    inline std::map<uint32, std::vector<Row>>& Cache()
    {
        static std::map<uint32, std::vector<Row>> c;
        return c;
    }
    inline std::mutex& CacheMutex()
    {
        static std::mutex m;
        return m;
    }
    inline std::map<uint32, uint32>& LoadedAt()
    {
        static std::map<uint32, uint32> m;
        return m;
    }

    // mmap 顶点按 (y, z, x) 存（同 `PathFinder::getArea`），传 {x,y,z} 一个多边形都找不到。
    inline bool NavZ(uint32 mapId, float x, float y, float refZ, float& outZ)
    {
        MMAP::MMapManager* mm = MMAP::MMapFactory::createOrGetMMapManager();
        dtNavMeshQuery const* q = nullptr;
        for (uint32 inst = 0; inst <= 3 && !q; ++inst)
            q = mm->GetNavMeshQuery(mapId, inst);
        if (!q)
            return false;
        dtQueryFilter filter;
        dtPolyRef ref = 0;
        float const pt[3] = { y, refZ, x };
        float closest[3] = { 0.0f, 0.0f, 0.0f };
        float const extents[3] = { 5.0f, 5.0f, 5.0f };
        dtStatus const st = q->findNearestPoly(pt, extents, &filter, &ref, closest);
        static bool diagDone = false;
        if (!diagDone)
        {
            diagDone = true;
            sLog.outError("[COVERSPOT] NavZ 诊断 map=%u pt=(y=%.2f,z=%.2f,x=%.2f) status=%u ref=%llu closest=(%.2f,%.2f,%.2f)",
                mapId, pt[0], pt[1], pt[2], uint32(st), (unsigned long long)ref,
                closest[0], closest[1], closest[2]);
        }
        if (!ref)
            return false;
        float const gx = closest[2], gy = closest[0], gz = closest[1];
        float const dx = gx - x, dy = gy - y, dz = gz - refZ;
        if (dx * dx + dy * dy + dz * dz > 4.0f)
            return false;
        outZ = gz;
        return true;
    }

    inline VMAP::IVMapManager* Vm()
    {
        return VMAP::VMapFactory::createOrGetVMapManager();
    }

    inline bool LosClear(uint32 mapId, float x1, float y1, float z1, float x2, float y2, float z2)
    {
        return Vm()->isInLineOfSight(mapId, x1, y1, z1, x2, y2, z2, false);
    }

    // 从 (x,y) 所在高度向四周打 8 条 1.6 码射线，返回最近墙面距离（没有墙返回 99）
    inline float WallDist(uint32 mapId, float x, float y, float z)
    {
        float best = 99.0f;
        for (int i = 0; i < 8; ++i)
        {
            float const a = float(i) * (2.0f * M_PI_F / 8.0f);
            for (float r = 0.4f; r <= 1.6f; r += 0.2f)
            {
                if (!LosClear(mapId, x, y, z + 1.0f, x + std::cos(a) * r, y + std::sin(a) * r, z + 1.0f))
                {
                    if (r < best)
                        best = r;
                    break;
                }
            }
        }
        return best;
    }

    // 站上去 1.2 码半径内被实心挡住的射线数（16 条）
    inline uint32 BlockedRays(uint32 mapId, float x, float y, float z)
    {
        uint32 n = 0;
        for (int i = 0; i < 16; ++i)
        {
            float const a = float(i) * (2.0f * M_PI_F / 16.0f);
            if (!LosClear(mapId, x, y, z + 1.0f, x + std::cos(a) * 1.2f, y + std::sin(a) * 1.2f, z + 1.0f))
                ++n;
        }
        return n;
    }

    struct MobRef
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
        uint16 ord = 0;      // 0 = 巡逻/兜底 ⇒ 永远算"看得见"
    };

    inline void CollectMobs(uint32 mapId, std::vector<MobRef>& out)
    {
        dungeonroute::MapRoute const* route = dungeonroute::GetMapRoute(mapId);
        if (!route)
            return;
        for (auto const& kv : route->waves)
        {
            for (auto const& m : kv.second.members)
            {
                CreatureData const* d = sObjectMgr.GetCreatureData(m.guid);
                if (!d || d->id == 0)
                    continue;                       // 动态刷位没有静态坐标
                MobRef r;
                r.x = d->posX;
                r.y = d->posY;
                r.z = d->posZ;
                r.ord = kv.first;
                out.push_back(r);
            }
        }
        for (uint32 pg : route->patrolGuids)
        {
            CreatureData const* d = sObjectMgr.GetCreatureData(pg);
            if (!d || d->id == 0)
                continue;
            MobRef r;
            r.x = d->posX;
            r.y = d->posY;
            r.z = d->posZ;
            r.ord = 0;
            out.push_back(r);
        }
    }

    // 离线扫描并写库。返回写入点数 / 扫描点数。
    inline void ScanMap(uint32 mapId, uint32& scanned, uint32& kept, std::string& msg)
    {
        scanned = kept = 0;
        std::vector<MobRef> mobs;
        CollectMobs(mapId, mobs);
        if (mobs.empty())
        {
            msg = "没有路线怪，跳过";
            return;
        }
        float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f, sumZ = 0.0f;
        for (auto const& m : mobs)
        {
            minX = std::min(minX, m.x); maxX = std::max(maxX, m.x);
            minY = std::min(minY, m.y); maxY = std::max(maxY, m.y);
            sumZ += m.z;
        }
        float const refZ = sumZ / float(mobs.size());
        minX -= 20.0f; maxX += 20.0f; minY -= 20.0f; maxY += 20.0f;

        // 离线跑的时候没有活着的 Map 实例 ⇒ mmap 查询和 vmap 图块都得自己加载，
        //   否则 NavMeshZ 永远拿不到 query、IsInLineOfSight 永远"看得见"（没有几何）。
        {
            MMAP::MMapManager* mm = MMAP::MMapFactory::createOrGetMMapManager();
            bool const i0 = mm->loadMapInstance(sWorld.GetDataPath(), mapId, 0);
            // 关键：loadMapData 只 init 网格参数，瓦片得自己按 grid 加载，否则查询里一个多边形都没有。
            //   （不再用 loadAllMapTiles：它在 boost::filesystem::path::filename() 里会崩）
            {
                GridPair const m1 = MaNGOS::ComputeGridPair(minX, minY);
                GridPair const m2 = MaNGOS::ComputeGridPair(maxX, maxY);
                int const mx1 = 63 - int(m1.x_coord), mx2 = 63 - int(m2.x_coord);
                int const my1 = 63 - int(m1.y_coord), my2 = 63 - int(m2.y_coord);
                uint32 tiles = 0;
                for (int gx = std::min(mx1, mx2); gx <= std::max(mx1, mx2); ++gx)
                    for (int gy = std::min(my1, my2); gy <= std::max(my1, my2); ++gy)
                        if (mm->loadMap(sWorld.GetDataPath(), mapId, gx, gy))
                            ++tiles;
                sLog.outError("[COVERSPOT] map %u mmap 瓦片加载 %u 个", mapId, tiles);
            }
            bool const i1 = mm->loadMapInstance(sWorld.GetDataPath(), mapId, 1);
            dtNavMeshQuery const* q0 = mm->GetNavMeshQuery(mapId, 0);
            dtNavMeshQuery const* q1 = mm->GetNavMeshQuery(mapId, 1);
            float sz = 0.0f;
            bool const selfOk = NavZ(mapId, mobs[0].x, mobs[0].y, mobs[0].z, sz);
            sLog.outError("[COVERSPOT] map %u 加载: inst0=%d query0=%d / inst1=%d query1=%d；首怪自检 NavZ=%d (z %.1f -> %.1f)",
                mapId, i0 ? 1 : 0, q0 ? 1 : 0, i1 ? 1 : 0, q1 ? 1 : 0, selfOk ? 1 : 0, mobs[0].z, sz);
            // 顺序对比：{y,z,x} vs {x,y,z}
            for (int perm = 0; perm < 2 && q0; ++perm)
            {
                float const pq[3] = { perm == 0 ? float(mobs[0].y) : float(mobs[0].x),
                                      perm == 0 ? float(mobs[0].z) : float(mobs[0].y),
                                      perm == 0 ? float(mobs[0].x) : float(mobs[0].z) };
                dtQueryFilter f2;
                dtPolyRef r2 = 0;
                float c2[3] = { 0.0f, 0.0f, 0.0f };
                float const ex2[3] = { 5.0f, 5.0f, 5.0f };
                dtStatus const s2 = q0->findNearestPoly(pq, ex2, &f2, &r2, c2);
                sLog.outError("[COVERSPOT] 顺序%d pt=(%.2f,%.2f,%.2f) status=%u ref=%llu closest=(%.2f,%.2f,%.2f)",
                    perm, pq[0], pq[1], pq[2], uint32(s2), (unsigned long long)r2, c2[0], c2[1], c2[2]);
            }
        }
        {
            std::string const vbase = sWorld.GetDataPath() + "vmaps/";
            GridPair const g1 = MaNGOS::ComputeGridPair(minX, minY);
            GridPair const g2 = MaNGOS::ComputeGridPair(maxX, maxY);
            int const x1 = 63 - int(g1.x_coord), x2 = 63 - int(g2.x_coord);
            int const y1 = 63 - int(g1.y_coord), y2 = 63 - int(g2.y_coord);
            for (int gx = std::min(x1, x2); gx <= std::max(x1, x2); ++gx)
                for (int gy = std::min(y1, y2); gy <= std::max(y1, y2); ++gy)
                    Vm()->loadMap(vbase.c_str(), mapId, gx, gy);
        }

        WorldDatabase.DirectPExecute("DELETE FROM ai_playerbot_cover_spot WHERE `map`=%u", mapId);

        std::string batch;
        uint32 inBatch = 0;
        auto flush = [&]()
        {
            if (!inBatch)
                return;
            batch.pop_back();                     // 去掉末尾逗号
            WorldDatabase.DirectPExecute("INSERT INTO ai_playerbot_cover_spot "
                "(`map`,`x`,`y`,`z`,`wall_dist`,`block_rays`,`seen_max_ord`,`seen_count`) VALUES %s",
                batch.c_str());
            batch.clear();
            inBatch = 0;
        };

        uint32 nmFail = 0, wallFail = 0, blockFail = 0;
        float const step = 2.0f;
        for (float x = minX; x <= maxX; x += step)
        {
            for (float y = minY; y <= maxY; y += step)
            {
                ++scanned;
                if (scanned % 20000 == 0)
                    sLog.outError("[COVERSPOT] map %u 扫描进度 %u（已入库 %u）", mapId, scanned, kept);
                // 参考高度取"最近一只路线怪的 z"：全图平均 z 在多层/斜坡上会把大量点判成离面太远
                float nearZ = refZ;
                {
                    float bestD2 = 1e18f;
                    for (auto const& m : mobs)
                    {
                        float const dx = m.x - x, dy = m.y - y;
                        float const d2 = dx * dx + dy * dy;
                        if (d2 < bestD2)
                        {
                            bestD2 = d2;
                            nearZ = m.z;
                        }
                    }
                }
                float z = 0.0f;
                if (!NavZ(mapId, x, y, nearZ, z))
                {
                    ++nmFail;
                    continue;
                }
                float const wd = WallDist(mapId, x, y, z);
                if (wd > 0.8f)                    // 要求贴墙
                {
                    ++wallFail;
                    continue;
                }
                uint32 const blk = BlockedRays(mapId, x, y, z);
                if (blk < 8)                      // 遮挡不足 180 度
                {
                    ++blockFail;
                    continue;
                }
                uint16 seenMax = 0, seenCnt = 0;
                for (auto const& m : mobs)
                {
                    float const dx = m.x - x, dy = m.y - y;
                    if (dx * dx + dy * dy > 45.0f * 45.0f)
                        continue;
                    if (!LosClear(mapId, x, y, z + 1.6f, m.x, m.y, m.z + 1.6f))
                        continue;
                    ++seenCnt;
                    uint16 const ord = m.ord ? m.ord : 9999;    // 巡逻怪 ⇒ 一律算看得见到底
                    if (ord > seenMax)
                        seenMax = ord;
                }
                char buf[256];
                snprintf(buf, sizeof(buf), "(%u,%.2f,%.2f,%.2f,%.2f,%u,%u,%u),",
                    mapId, x, y, z, wd, blk, uint32(seenMax), uint32(seenCnt));
                batch += buf;
                if (++inBatch >= 200)
                    flush();
                ++kept;
            }
        }
        flush();
        char m2[128];
        snprintf(m2, sizeof(m2), "扫描 %u 点 / 入库 %u 点（网格外 %u / 离墙远 %u / 遮挡不足 %u）",
            scanned, kept, nmFail, wallFail, blockFail);
        msg = m2;

        std::scoped_lock lock(CacheMutex());
        Cache().erase(mapId);
        LoadedAt().erase(mapId);
    }

    inline void EnsureLoaded(uint32 mapId)
    {
        std::scoped_lock lock(CacheMutex());
        if (LoadedAt().count(mapId))
            return;
        std::vector<Row>& rows = Cache()[mapId];
        rows.clear();
        std::string const q = "SELECT `x`,`y`,`z`,`wall_dist`,`seen_max_ord`,`seen_count` FROM ai_playerbot_cover_spot WHERE `map`="
            + std::to_string(mapId);
        auto res = WorldDatabase.Query(q.c_str());
        if (res)
        {
            do
            {
                Field* f = res->Fetch();
                Row r;
                r.x = f[0].GetFloat();
                r.y = f[1].GetFloat();
                r.z = f[2].GetFloat();
                r.wallDist = f[3].GetFloat();
                r.seenMaxOrd = f[4].GetUInt16();
                r.seenCount = f[5].GetUInt16();
                rows.push_back(r);
            } while (res->NextRow());
        }
        LoadedAt()[mapId] = 1;
        sLog.outError("[COVERSPOT] map %u 点库已加载：%u 个候选点", mapId, uint32(rows.size()));
    }

    // 战斗时挑点：ord 之后的波次（ord > seen_max_ord）不会看见它，取离 needX/needY 最近的。
    inline bool Find(uint32 mapId, uint16 ord, float nearX, float nearY, float maxDist,
                     float& outX, float& outY, float& outZ)
    {
        EnsureLoaded(mapId);
        std::scoped_lock lock(CacheMutex());
        auto it = Cache().find(mapId);
        if (it == Cache().end())
            return false;
        float bestD2 = maxDist * maxDist;
        Row const* best = nullptr;
        for (auto const& r : it->second)
        {
            if (ord && r.seenMaxOrd && r.seenMaxOrd >= ord)   // ord=0 ⇒ 不知道该怪在第几波，不做波次过滤
                continue;
            float const dx = r.x - nearX, dy = r.y - nearY;
            float const d2 = dx * dx + dy * dy;
            if (d2 < bestD2)
            {
                bestD2 = d2;
                best = &r;
            }
        }
        if (!best)
            return false;
        outX = best->x;
        outY = best->y;
        outZ = best->z;
        return true;
    }
}
