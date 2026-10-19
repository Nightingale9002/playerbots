#pragma once

// ---------------------------------------------------------------------------
// [副本推进 2026-10-18 站长口径] 运行期"按波推进"（**只对 verified=1 的图生效**）
//
// 数据来自离线生成器（见 dev/tools/DESIGN_dungeon_route_gen.md，表结构见 dev/183_*.sql）：
//   ai_playerbot_dungeon_route : guid + ord（同一遭遇组共享一个 ord，1 = 入口第一波）
//   ai_playerbot_wave_anchor   : 每波的"拉怪位"（§4.7 离线算好的、不 ADD 的站位）
//   ai_playerbot_boss_spawn    : 本图 boss 落点（静态 boss 用 creature 刷点 guid 判存活）
//   ai_playerbot_patrol(_wave) : 类型B 巡逻（运行期"遇上就清"，这里只读来做日志）
//
// 行为（MVP，L3 用）：
//   ① 只让"驱动坦克"动：坦克 bot、非战斗状态、图 verified=1、且队伍里没有真人坦克；
//      多个 bot 坦克时只让 guid 最小的那个驱动（避免俩坦克同时冲）。
//   ② 当前波 = **还活着的最小 ord**（跳过已放弃的波）；先走到该波的拉怪位，再复用
//      现有 pull 链路（"pull my target"，它负责 posMap/OnCombatStarted）起手。
//   ③ 波全清完 ⇒ boss 阶段：拉 boss_spawn 里还活着的静态 boss（没有锚点，走到 22 码外拉）。
//   ④ 打谁：PossibleAttackTargetsValue 里过滤 —— 只打"当前波" + "已经在打我们的"
//      （后者保证被巡逻/ADD 打了必然还手 ⇒ 就是"遇上就清"）。
//   ⑤ 卡住保护：同一波连续 WAVE_GIVE_UP_MS 没有推进 ⇒ 记日志并跳过该波。
//
// 安全：verified 列不存在 / 没有 verified=1 的图 ⇒ 整个模块不生效，回落原 playerbots 逻辑。
// ---------------------------------------------------------------------------

#include "playerbot/playerbot.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/strategy/generic/PullStrategy.h"
// [2026-10-18 全图怪物清单] 与服务器共享 ObjectMgr 内存数据（CreatureData / CreatureInfo）
#include "Globals/ObjectMgr.h"
// [2026-10-18] 拉怪目标要按"威胁等级表"选（ThreatTier：表 priority 5 最高 > 启发式），
//   所以这里要能看到 PossibleAttackTargetsValue 的静态判据（纯 header，不反向包含本文件）。
#include "playerbot/strategy/values/PossibleAttackTargetsValue.h"
// [2026-10-18 站长口径] 拉怪记录改由核心统一采集"所有玩家 / 所有副本图"（见注释 §拉怪记录）；
//   本文件只在自动开怪时给记录器补一个 ord 提示。
#include "Combat/InstancePullRecorder.h"
// [决策参数化 2026-10-18] 目标选择权重从表 ai_playerbot_decision_weight 读（可离线调参）
#include "playerbot/DecisionWeights.h"
// [自动进本 2026-10-18] 不在副本里时先把队伍带进本，再交给按波推进
#include "playerbot/DungeonRun.h"

namespace dungeonroute
{
    // 同一波多久没有任何"成员死亡"的推进就放弃（毫秒）
    const uint32 WAVE_GIVE_UP_MS = 90000;
    // [2026-10-18] "这一波没有可打目标"（友好 NPC / 小动物 / 全在远处没加载）时，
    //   等这么久就跳过——否则坦克会站在拉怪位一直不动。
    const uint32 WAVE_NOT_LOADED_MS = 5000;
    // ---------------------------------------------------------------------------
    // [2026-10-18 站长口径] 全图怪物清单 —— **与服务器共享 ObjectMgr 内存数据**
    //   ObjectMgr 启动时已把 `creature` 全表加载进内存（与 grid 是否加载无关），
    //   这里每图只建一次 `guid -> (CreatureData*, CreatureInfo*)` 的**指针索引**：
    //   数据只有一份，不会出现"策略层清单"和服务器不一致；不需要额外读库存。
    //   用途：
    //     ① 选目标前先排除小动物/任务 NPC（青蛙那种漏网）；
    //     ② 发现"波里有运行期动态刷怪（9,000,xxx）"的数据缺口（ObjectMgr 里没有）；
    //     ③ 策略层要知道"副本里总共有什么怪物"的唯一数据源。
    //   hostile 口径：非小动物 + 无 NPC 旗标；真正能不能打仍以运行期 CanAttack 为准。
    // ---------------------------------------------------------------------------
    struct RosterEntity
    {
        CreatureData const* data = nullptr;
        CreatureInfo const* info = nullptr;
        bool hostile = false;
    };

    struct DungeonRoster
    {
        std::unordered_map<uint32, RosterEntity> byGuid;
        uint32 total = 0;          // 本图刷点总数
        uint32 hostileCount = 0;   // 敌对（非小动物 / 非 NPC）
        uint32 critters = 0;
        uint32 npcs = 0;
        uint32 noTemplate = 0;
    };

    inline DungeonRoster const& RosterOf(uint32 mapId)
    {
        static std::unordered_map<uint32, DungeonRoster> cache;
        static std::mutex cacheMutex;

        std::scoped_lock lock(cacheMutex);
        auto it = cache.find(mapId);
        if (it != cache.end())
            return it->second;

        DungeonRoster r;
        struct Worker
        {
            uint32 mapId;
            DungeonRoster* r;
            bool operator()(CreatureDataPair const& pair) const
            {
                if (pair.second.mapid != mapId)
                    return false;

                CreatureInfo const* info = sObjectMgr.GetCreatureTemplate(pair.second.id);
                RosterEntity e;
                e.data = &pair.second;
                e.info = info;
                if (!info)
                    ++r->noTemplate;
                else if (info->CreatureType == CREATURE_TYPE_CRITTER)
                    ++r->critters;
                else if (info->NpcFlags)
                    ++r->npcs;
                else
                {
                    e.hostile = true;
                    ++r->hostileCount;
                }
                ++r->total;
                r->byGuid[pair.first] = e;
                return false;
            }
        } worker{ mapId, &r };

        sObjectMgr.DoCreatureData(worker);
        sLog.outString("[DUNGEONROUTE] 全图怪物清单 map=%u：刷点 %u（敌对 %u / 小动物 %u / NPC %u / 无模板 %u）",
            mapId, r.total, r.hostileCount, r.critters, r.npcs, r.noTemplate);
        return cache.emplace(mapId, std::move(r)).first->second;
    }

    // 波成员是否在 ObjectMgr 清单里 + 是否敌对（动态刷怪不在清单 ⇒ 返回 false,false）
    inline bool RosterLookup(uint32 mapId, uint32 guid, bool& hostile)
    {
        DungeonRoster const& r = RosterOf(mapId);
        auto it = r.byGuid.find(guid);
        if (it == r.byGuid.end())
            return false;
        hostile = it->second.hostile;
        return true;
    }

    // 当前波缓存（毫秒）：PossibleAttackTargetsValue 每 tick 会查很多次
    const uint32 ORD_CACHE_MS = 400;
    // 锁定"当前推进波"的时长：多波同时可见时只清这一波（清完/放弃自动解锁）
    const uint32 LOCK_MS = 150000;
    // "朝这一波走 8 秒位置都没动"就放弃（mmap 不可达 / 卡门 / 卡几何）。
    //   注意用**位置变化**而不是"到目标距离"：绕路时距离可能先变大，但那不是卡住。
    const uint32 MOVE_STUCK_MS = 8000;
    // 认为"已经站在拉怪位"的距离（码）
    const float AT_PULL_POINT_DIST = 3.5f;
    // boss 阶段没有锚点：走到离它多远处拉
    const float BOSS_PULL_DIST = 22.0f;
    // [2026-10-18] 图**未放行**时的重查间隔：`verified` 是站长实测后手动 UPDATE 的，
    //   若把"未放行"永久缓存，改完库必须重启进程才生效（实测就踩过：库里 verified=1，
    //   但进程里缓存的是 0 ⇒ 驱动坦克完全不拉怪，看起来像"bot 不会开怪"）。
    const uint32 ROUTE_RECHECK_MS = 5000;

    // [2026-10-18 站长口径] "bot 自动开怪应该做成开关，需要手动打开"：
    //   三态运行时开关：
    //     -1 = 跟随配置项 `AiPlayerbot.DungeonRoute`（默认 false ⇒ 不开）
    //      0 = 强制关
    //      1 = 强制开
    //   用 GM 命令 `.bot route on|off|reset` 切，**不用重启进程**（reset = 回到跟配置）。
    inline int& RouteRuntimeSwitch()
    {
        static int sw = -1;
        return sw;
    }

    inline bool RouteEnabled()
    {
        const int sw = RouteRuntimeSwitch();
        return sw < 0 ? sPlayerbotAIConfig.dungeonRoute : (sw != 0);
    }

    struct Anchor { bool has = false; float x = 0, y = 0, z = 0; };
    struct Member { uint32 guid = 0; uint32 entry = 0; };
    struct Wave { uint16 ord = 0; uint32 gid = 0; std::vector<Member> members; Anchor pull;
                  float cx = 0, cy = 0, cz = 0; bool hasCenter = false;   // 波中心（刷点均值）
                  bool allNonHostile = false; };   // 全图清单判定：整波没有敌对怪（纯 NPC/小动物）
    struct BossSpawn { uint32 guid = 0; uint32 entry = 0; float x = 0, y = 0, z = 0; };

    // [2026-10-19] 路线"连接点"：跳点(kind=0) / 升降台·传送点(kind=1)
    //   数据表 `ai_playerbot_route_link`：src = 起跳/等电梯位置，dst = 落点/对岸位置。
    //   用途：navmesh 没有的"必须跳过去/必须坐电梯"的缺口；本地仿真/人工标注用。
    struct RouteLink { float sx = 0, sy = 0, sz = 0; float dx = 0, dy = 0, dz = 0; uint8 kind = 0; };

    // [2026-10-19 站长口径] "boss 战斗结束后要点开关才能继续"——boss 策略的一部分：
    //   数据表 ai_playerbot_boss_switch：某个 boss 打死后，去点指定 GO（蒸汽地窟的门禁面板、
    //   毒蛇神殿的桥控制台等）；挂在 boss 上，不是通用的"卡住就开锁"。
    struct BossSwitch
    {
        uint32 bossEntry = 0;
        uint32 goGuid = 0;
        uint32 goEntry = 0;
        float x = 0, y = 0, z = 0;
        uint8 kind = 0;
        std::string note;
    };

    struct MapRoute
    {
        uint32 mapId = 0;
        bool loaded = false;
        bool verified = false;
        uint32 checkedAt = 0;                    // 上次查 verified 的时间（未放行时用来限流重查）
        std::map<uint16, Wave> waves;            // ord -> 波（升序，1..N）
        std::map<uint32, uint16> guidOrd;        // 刷点 guid -> ord
        std::vector<BossSpawn> bosses;           // 静态 boss 刷点（可能同 entry 多刷点）
        std::vector<RouteLink> links;            // 跳点/升降台连接点
        std::vector<BossSwitch> switches;        // boss 战后开关（门禁面板/桥控制台）
        uint16 maxOrd = 0;
    };

    struct Target
    {
        uint32 guid = 0;
        uint32 entry = 0;
        uint16 ord = 0;              // 目标所属波次（0 = boss 阶段）
        bool boss = false;
        bool hasPullPoint = false;
        float px = 0, py = 0, pz = 0;
    };

    // ---- 每个 bot 的小状态（当前波缓存 + 放弃列表）--------------------------------
    struct BotState
    {
        uint32 calcedAt = 0;
        uint16 ord = 0;
        uint32 ordSince = 0;
        std::set<uint16> skip;        // 已放弃的波（不可达/卡住）
        std::set<uint16> seenAlive;   // 见过活怪的波（用来区分"已清"与"远处未加载"）
        std::set<uint16> cleared;     // 确认已清的波
        std::set<uint32> bossSeen;    // 见过活的 boss 刷点
        std::set<uint32> bossCleared; // 确认已死的 boss 刷点
        std::set<uint32> switchUsed;  // 已点过的 boss 开关（goGuid）
        uint32 switchWaitAt = 0;      // 开关没加载时的等待起点
        uint16 lockOrd = 0;           // 当前锁定的推进波（0 = 未锁定）
        uint32 lockUntil = 0;         // 锁定到期时间（毫秒）
        // [2026-10-18] "有目标但走不过去"守卫：同一波移动 8 秒**位置没动过** ⇒ 放弃该波。
        //   用位置而不是"到目标距离"：绕路时距离可能先变大，但那不是卡住。
        uint16 moveOrd = 0;           // 正在朝哪一波移动（0 = 无）
        float moveLastX = 0.0f;       // 上次记录的位置
        float moveLastY = 0.0f;
        float moveLastZ = 0.0f;
        uint32 moveProgressAt = 0;    // 上次"位置明显变化"的时间
        uint32 linkWaitAt = 0;        // 升降台等待起点（0 = 没在等）
        uint32 linkUsedAt = 0;        // 上次用跳点/电梯的时间（节流）
        uint32 jumpUntil = 0;         // 跳跃推进的"保护期"（期间不要用 MoveTo 覆盖跳跃样条）
        uint32 lastSeenAt = 0;
    };

    inline std::mutex& StateMutex() { static std::mutex m; return m; }
    inline std::map<Player*, BotState>& States() { static std::map<Player*, BotState> s; return s; }

    inline BotState& StateOf(Player* bot)
    {
        std::scoped_lock lock(StateMutex());
        uint32 now = WorldTimer::getMSTime();
        auto& s = States();
        if (s.size() > 1024)      // 僵尸清理：10 分钟没动过的 bot 记录丢掉
        {
            for (auto it = s.begin(); it != s.end();)
                it = (it->second.lastSeenAt && WorldTimer::getMSTimeDiff(it->second.lastSeenAt, now) > 600000)
                ? s.erase(it) : std::next(it);
        }
        BotState& st = s[bot];
        st.lastSeenAt = now;
        return st;
    }

    // ---- 数据加载（线程安全，每图只加载一次）--------------------------------------
    inline std::mutex& RouteMutex() { static std::mutex m; return m; }
    inline std::map<uint32, MapRoute>& RouteCache() { static std::map<uint32, MapRoute> c; return c; }

    inline bool HasVerifiedColumn()
    {
        static std::mutex m;
        static bool loaded = false;
        static bool has = false;
        std::scoped_lock lock(m);
        if (!loaded)
        {
            std::unique_ptr<QueryResult> res = WorldDatabase.PQuery(
                "SHOW COLUMNS FROM `ai_playerbot_dungeon_route` LIKE 'verified'");
            has = (res != nullptr && res->GetRowCount() > 0);
            loaded = true;
        }
        return has;
    }

    inline void LoadMapRoute(MapRoute& route, uint32 mapId)
    {
        route.mapId = mapId;
        route.loaded = true;
        route.checkedAt = WorldTimer::getMSTime();

        if (!HasVerifiedColumn())
        {
            route.verified = false;
            return;
        }

        std::unique_ptr<QueryResult> ver = WorldDatabase.PQuery(
            "SELECT `verified` FROM `ai_playerbot_dungeon_route` WHERE `map` = '%u' LIMIT 1", mapId);
        route.verified = ver && ver->Fetch()[0].GetUInt32() != 0;
        if (!route.verified)
            return;

        std::unique_ptr<QueryResult> rows = WorldDatabase.PQuery(
            "SELECT r.guid, r.ord, r.group_id, c.id FROM `ai_playerbot_dungeon_route` r "
            "JOIN `creature` c ON c.guid = r.guid WHERE r.map = '%u' ORDER BY r.ord, r.guid", mapId);
        if (rows)
        {
            do
            {
                Field* f = rows->Fetch();
                uint32 guid = f[0].GetUInt32();
                uint16 ord = uint16(f[1].GetUInt32());
                uint32 gid = f[2].GetUInt32();
                uint32 entry = f[3].GetUInt32();
                if (!guid || !ord)
                    continue;

                Wave& w = route.waves[ord];      // map 保证按 ord 构造，缺的 ord 后面会补默认值
                if (!w.ord)
                {
                    w.ord = ord;
                    w.gid = gid;
                }
                Member m; m.guid = guid; m.entry = entry;
                w.members.push_back(m);
                route.guidOrd[guid] = ord;
                if (ord > route.maxOrd)
                    route.maxOrd = ord;
            } while (rows->NextRow());
        }

        std::unique_ptr<QueryResult> anchors = WorldDatabase.PQuery(
            "SELECT `ord`, `pull_x`, `pull_y`, `pull_z` FROM `ai_playerbot_wave_anchor` "
            "WHERE `map` = '%u'", mapId);
        if (anchors)
        {
            do
            {
                Field* f = anchors->Fetch();
                uint16 ord = uint16(f[0].GetUInt32());
                auto it = route.waves.find(ord);
                if (it == route.waves.end())
                    continue;
                it->second.pull.has = true;
                it->second.pull.x = f[1].GetFloat();
                it->second.pull.y = f[2].GetFloat();
                it->second.pull.z = f[3].GetFloat();
            } while (anchors->NextRow());
        }

        // [2026-10-18 站长口径] "让 bot 知道每一波怪的位置"：用刷点坐标算**波中心**。
        //   mmap 联通的简单图里，坦克直接朝波中心走即可（不依赖 wave_anchor 是否可达）；
        //   走到之后网格自然加载，再按"30 码视线内即停"开拉。
        std::unique_ptr<QueryResult> centers = WorldDatabase.PQuery(
            "SELECT r.ord, AVG(c.position_x), AVG(c.position_y), AVG(c.position_z) "
            "FROM `ai_playerbot_dungeon_route` r JOIN `creature` c ON c.guid = r.guid AND c.map = r.map "
            "WHERE r.map = '%u' AND r.ord > 0 GROUP BY r.ord", mapId);
        if (centers)
        {
            do
            {
                Field* f = centers->Fetch();
                uint16 ord = uint16(f[0].GetUInt32());
                auto it = route.waves.find(ord);
                if (it == route.waves.end())
                    continue;
                it->second.cx = f[1].GetFloat();
                it->second.cy = f[2].GetFloat();
                it->second.cz = f[3].GetFloat();
                it->second.hasCenter = true;
            } while (centers->NextRow());
        }

        std::unique_ptr<QueryResult> bosses = WorldDatabase.PQuery(
            "SELECT b.entry, b.x, b.y, b.z, c.guid FROM `ai_playerbot_boss_spawn` b "
            "JOIN `creature` c ON c.id = b.entry AND c.map = b.map WHERE b.map = '%u'", mapId);
        if (bosses)
        {
            do
            {
                Field* f = bosses->Fetch();
                BossSpawn b;
                b.entry = f[0].GetUInt32();
                b.x = f[1].GetFloat();
                b.y = f[2].GetFloat();
                b.z = f[3].GetFloat();
                b.guid = f[4].GetUInt32();
                route.bosses.push_back(b);
            } while (bosses->NextRow());
        }

        // [2026-10-19] 跳点 / 升降台连接点（表不存在时静默为空）
        {
            std::unique_ptr<QueryResult> links = WorldDatabase.PQuery(
                "SELECT src_x, src_y, src_z, dst_x, dst_y, dst_z, kind FROM `ai_playerbot_route_link` WHERE map = '%u'",
                mapId);
            if (links)
            {
                do
                {
                    Field* f = links->Fetch();
                    RouteLink l;
                    l.sx = f[0].GetFloat();
                    l.sy = f[1].GetFloat();
                    l.sz = f[2].GetFloat();
                    l.dx = f[3].GetFloat();
                    l.dy = f[4].GetFloat();
                    l.dz = f[5].GetFloat();
                    l.kind = f[6].GetUInt8();
                    route.links.push_back(l);
                } while (links->NextRow());
            }
        }

        // [2026-10-19] boss 战后开关（门禁面板/桥控制台）：挂在 boss_entry 上
        {
            std::unique_ptr<QueryResult> switches = WorldDatabase.PQuery(
                "SELECT boss_entry, go_guid, go_entry, x, y, z, kind, note "
                "FROM `ai_playerbot_boss_switch` WHERE map = '%u'", mapId);
            if (switches)
            {
                do
                {
                    Field* f = switches->Fetch();
                    BossSwitch s;
                    s.bossEntry = f[0].GetUInt32();
                    s.goGuid = f[1].GetUInt32();
                    s.goEntry = f[2].GetUInt32();
                    s.x = f[3].GetFloat();
                    s.y = f[4].GetFloat();
                    s.z = f[5].GetFloat();
                    s.kind = f[6].GetUInt8();
                    s.note = f[7].GetCppString();
                    route.switches.push_back(s);
                } while (switches->NextRow());
            }
        }

        // [全图怪物清单] 统计波里的数据缺口（运行期动态刷怪 9,000,xxx 不在 ObjectMgr 里）
        DungeonRoster const& roster = RosterOf(mapId);
        uint32 totalMembers = 0, missingMembers = 0, critterMembers = 0, npcMembers = 0;
        uint32 trivialWaves = 0;
        for (auto& kv : route.waves)
        {
            uint32 known = 0, hostile = 0;
            for (auto& m : kv.second.members)
            {
                ++totalMembers;
                auto rit = roster.byGuid.find(m.guid);
                if (rit == roster.byGuid.end())
                {
                    ++missingMembers;
                    continue;
                }
                ++known;
                if (rit->second.info && rit->second.info->CreatureType == CREATURE_TYPE_CRITTER)
                    ++critterMembers;
                else if (!rit->second.hostile)
                    ++npcMembers;
                else
                    ++hostile;
            }
            // 整波都在清单里且一个敌对都没有 ⇒ 纯 NPC/小动物波，开局就跳过（不用走过去等 5 秒）
            if (known > 0 && hostile == 0)
            {
                kv.second.allNonHostile = true;
                ++trivialWaves;
            }
        }

        sLog.outString("[DUNGEONROUTE] map %u 已加载推进数据：%u 波 / %u 只 / boss 刷点 %u 个",
            mapId, uint32(route.waves.size()), uint32(route.guidOrd.size()), uint32(route.bosses.size()));
        sLog.outString("[DUNGEONROUTE] map %u 波成员 %u：数据缺口(动态刷怪) %u / 小动物 %u / 非敌对NPC %u / 纯NPC波 %u",
            mapId, totalMembers, missingMembers, critterMembers, npcMembers, trivialWaves);
    }

    inline MapRoute const* GetMapRoute(uint32 mapId)
    {
        std::scoped_lock lock(RouteMutex());
        auto& cache = RouteCache();
        auto it = cache.find(mapId);
        if (it != cache.end() && it->second.loaded)
        {
            if (it->second.verified)
                return &it->second;

            // [2026-10-18] 未放行的图**不永久缓存**：每 ROUTE_RECHECK_MS 重查一次，
            //   这样站长 UPDATE verified=1 后几秒内就生效，不需要重启进程。
            if (WorldTimer::getMSTimeDiff(it->second.checkedAt, WorldTimer::getMSTime()) < ROUTE_RECHECK_MS)
                return nullptr;
            it->second = MapRoute();
        }

        MapRoute& route = cache[mapId];
        LoadMapRoute(route, mapId);
        return route.loaded ? &route : nullptr;
    }

    // ---- 存活 / 当前波 ------------------------------------------------------------
    inline Creature* MemberCreature(Map* map, Member const& m)
    {
        if (!map || !m.guid)
            return nullptr;
        Creature* c = map->GetCreature(m.guid);
        return (c && c->IsAlive() && c->IsInWorld()) ? c : nullptr;
    }

    // [2026-10-18] 这一只"能不能打"：友好 NPC（CanAttack=false）与小动物不参与路线推进
    //   （route 表里混进了任务 NPC / 小动物，会让坦克把拉怪位当目标站桩）。
    inline bool SessionCanFight(Player* bot, Creature* c)
    {
        if (!bot || !c || !c->IsAlive() || !c->IsInWorld())
            return false;
        if (CreatureInfo const* info = c->GetCreatureInfo())
        {
            if (info->CreatureType == CREATURE_TYPE_CRITTER)
                return false;
        }
        return bot->CanAttack(c);
    }

    // [2026-10-18 站长口径] "打完最近的一波之后就不会继续推进"的根因：
    //   实例里**远处的网格还没加载**时 `map->GetCreature(guid)` 返回 nullptr，
    //   旧逻辑把这波当成"已清" ⇒ CurrentOrd 直接跳到更远的波（同样没加载）⇒ FindTarget 永远失败 ⇒
    //   坦克原地不动。这里把"没加载"与"已清"分开：
    //     - 见过活怪（seenAlive）之后才消失 ⇒ 已清
    //     - 从没见过、但有刷点数据 ⇒ 只是没加载 ⇒ 仍是当前波（坦克先走过去，网格就加载了）
    //   这需要每个 bot 记一点状态，放在 BotState 里（见下）。
    inline bool WaveHasAlive(Map* map, Wave const& w)
    {
        for (auto& m : w.members)
            if (MemberCreature(map, m))
                return true;
        return false;
    }

    // 当前波 = **还有活怪/没加载怪的最小 ord**（跳过 skip / 已清；0 = 没有）
    inline uint16 CurrentOrdRaw(Player* bot, MapRoute const& route, BotState& st)
    {
        Map* map = bot->GetMap();
        for (auto& kv : route.waves)
        {
            if (st.skip.count(kv.first) || st.cleared.count(kv.first))
                continue;

            bool anyAlive = false;
            bool anyUnloaded = false;
            for (auto& m : kv.second.members)
            {
                Creature* c = map->GetCreature(m.guid);
                if (c)
                {
                    if (c->IsAlive())
                        anyAlive = true;
                }
                else if (sObjectMgr.GetCreatureData(m.guid))
                    anyUnloaded = true;
            }

            if (anyAlive)
            {
                st.seenAlive.insert(kv.first);
                return kv.first;
            }

            // 见过活怪、现在活着的一个都没有 ⇒ 本波已清
            if (st.seenAlive.count(kv.first))
            {
                st.cleared.insert(kv.first);
                st.seenAlive.erase(kv.first);
                continue;
            }

            // 从没见过、只是没加载（远处） ⇒ 仍算当前波：先走过去把网格带起来
            if (anyUnloaded)
                return kv.first;
        }
        return 0;
    }

    inline uint16 CurrentOrd(Player* bot, MapRoute const& route)
    {
        BotState& st = StateOf(bot);
        uint32 now = WorldTimer::getMSTime();
        if (st.calcedAt && WorldTimer::getMSTimeDiff(st.calcedAt, now) < ORD_CACHE_MS)
            return st.ord;

        uint16 ord = CurrentOrdRaw(bot, route, st);
        if (ord != st.ord && st.calcedAt)
            st.ordSince = now;               // 换了新的一波：重置卡住计时
        else if (!st.calcedAt)
            st.ordSince = now;
        st.ord = ord;
        st.calcedAt = now;
        return ord;
    }

    // 该波里有怪正在战斗中吗（用于卡住保护：正在打就不算卡住）
    inline bool WaveInFight(Player* bot, MapRoute const& route, uint16 ord)
    {
        Map* map = bot->GetMap();
        auto it = route.waves.find(ord);
        if (it == route.waves.end())
            return false;
        for (auto& m : it->second.members)
        {
            Creature* c = MemberCreature(map, m);
            if (c && c->IsInCombat())
                return true;
        }
        return false;
    }

    // 同一波卡了多久（毫秒）；<=0 表示还没卡
    inline uint32 WaveStuckMs(Player* bot)
    {
        BotState& st = StateOf(bot);
        if (!st.ordSince)
            return 0;
        return WorldTimer::getMSTimeDiff(st.ordSince, WorldTimer::getMSTime());
    }

    // 前置声明：MarkSkipped 里要清掉本波的"路线拉怪"提示（实现见下面"拉怪记录"一节；
    //   记录本体已移到核心 pullrec::Update()）
    inline void PullEnd(Player* bot, const char* result);

    // [2026-10-19 543 实测] PULL 记录报 cleared 时，路线状态要同步推进
    inline void MarkCleared(Player* bot, uint16 ord, const char* why)
    {
        if (!bot || !ord)
            return;
        BotState& st = StateOf(bot);
        if (st.cleared.insert(ord).second)
        {
            sLog.outString("[DUNGEONROUTE] %s 波已清 ord=%u（%s）",
                bot->GetName(), uint32(ord), why ? why : "");
        }
        st.seenAlive.erase(ord);
        if (st.lockOrd == ord)
        {
            st.lockOrd = 0;
            st.lockUntil = 0;
        }
        st.calcedAt = 0;
    }

    inline void MarkSkipped(Player* bot, uint16 ord, const char* why)
    {
        BotState& st = StateOf(bot);
        if (st.skip.insert(ord).second)
        {
            sLog.outString("[DUNGEONROUTE] %s 跳过 ord=%u（%s）",
                bot->GetName(), uint32(ord), why ? why : "");
        }

        // 这一波被放弃了：如果刚才正是它在自动开怪，清掉记录器上的 ord 提示
        PullEnd(bot, "skipped");
        st.seenAlive.erase(ord);
        st.cleared.erase(ord);
        if (st.lockOrd == ord)
            st.lockOrd = 0;   // 锁定的波被放弃 ⇒ 解锁，换下一波
        st.calcedAt = 0;      // 强制立刻重算当前波
    }

    // boss 刷点放弃（远处一直没加载 / 已经死透但对不上）：避免坦克在空点原地等
    inline void MarkBossSkipped(Player* bot, uint32 dbGuid)
    {
        if (!bot || !dbGuid)
            return;
        BotState& st = StateOf(bot);
        if (st.bossCleared.insert(dbGuid).second)
            sLog.outString("[DUNGEONROUTE] %s 跳过 boss 刷点 %u（未加载/不可达）",
                bot->GetName(), dbGuid);
        st.calcedAt = 0;
    }

    // [2026-10-19 站长口径] 找"该去点的 boss 开关"：对应 boss 已死且见过、开关还没点过。
    //   由 DungeonRouteMoveAction 负责走位 + GameObject::Use()；不是通用开锁逻辑。
    inline bool FindPendingBossSwitch(Player* bot, MapRoute const& route, BossSwitch& out)
    {
        if (!bot || route.switches.empty() || !bot->IsInWorld() || bot->IsInCombat())
            return false;

        Map* map = bot->GetMap();
        if (!map)
            return false;

        BotState& st = StateOf(bot);
        for (auto const& sw : route.switches)
        {
            if (st.switchUsed.count(sw.goGuid))
                continue;

            bool anyAlive = false;
            bool seen = false;
            for (auto const& b : route.bosses)
            {
                if (b.entry != sw.bossEntry)
                    continue;
                Creature* c = map->GetCreature(b.guid);
                if (c && c->IsAlive())
                {
                    anyAlive = true;
                    break;
                }
                if (st.bossCleared.count(b.guid) || st.bossSeen.count(b.guid))
                    seen = true;
            }
            if (anyAlive || !seen)
                continue;

            out = sw;
            return true;
        }
        return false;
    }

    // ---------------------------------------------------------------------------
    // [2026-10-19] 路线连接点：跳点(kind=0) / 升降台·传送点(kind=1)
    //   到 src 附近（<=10码）且 dst 更靠近当前目标时：
    //     kind=0 ⇒ 直接 MoveJump 到 dst（小缺口/矮台，navmesh 没有的跳点）；
    //     kind=1 ⇒ 原地等 3 秒（等升降台/读条），然后 TeleportTo 到 dst（脚本化的"坐电梯"）。
    //   数据表：ai_playerbot_route_link(map, src_x,src_y,src_z, dst_x,dst_y,dst_z, kind)。
    //   只在 DungeonRoute 启用时生效；不能用时返回 false，正常寻路继续。
    // ---------------------------------------------------------------------------
    inline bool TryRouteLink(Player* bot, float targetX, float targetY, float targetZ)
    {
        if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported())
            return false;

        Map* map = bot->GetMap();
        if (!map || (!map->IsDungeon() && !map->IsRaid()))
            return false;

        MapRoute const* route = GetMapRoute(map->GetId());
        if (!route || route->links.empty())
            return false;

        BotState& st = StateOf(bot);
        uint32 const now = WorldTimer::getMSTime();
        float const targetDist = bot->GetDistance(targetX, targetY, targetZ);

        bool nearAny = false;
        for (auto& l : route->links)
        {
            if (bot->GetDistance(l.sx, l.sy, l.sz) > 10.0f)
                continue;

            float const ddx = targetX - l.dx, ddy = targetY - l.dy, ddz = targetZ - l.dz;
            if (std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz) > targetDist)
                continue;               // 落点比现在离目标更远 ⇒ 不用

            if (l.kind == 0)            // 跳点
            {
                if (!sPlayerbotAIConfig.routeJumpAssist)
                    continue;
                if (st.linkUsedAt && WorldTimer::getMSTimeDiff(st.linkUsedAt, now) < 5000)
                    continue;

                nearAny = true;
                st.linkUsedAt = now;
                st.linkWaitAt = 0;
                bot->GetMotionMaster()->MoveJump(l.dx, l.dy, l.dz, 9.0f, 5.0f);
                sLog.outString("[DUNGEONROUTE] %s 跳点 (%.1f,%.1f,%.1f) -> (%.1f,%.1f,%.1f)",
                    bot->GetName(), l.sx, l.sy, l.sz, l.dx, l.dy, l.dz);
                return true;
            }

            // 升降台 / 传送点
            if (!sPlayerbotAIConfig.routeLiftAssist)
                continue;

            nearAny = true;
            if (!st.linkWaitAt)
            {
                st.linkWaitAt = now ? now : 1;
                sLog.outString("[DUNGEONROUTE] %s 等升降台/传送点 (%.1f,%.1f,%.1f)",
                    bot->GetName(), l.sx, l.sy, l.sz);
                return true;
            }
            if (WorldTimer::getMSTimeDiff(st.linkWaitAt, now) >= 3000)
            {
                st.linkWaitAt = 0;
                st.linkUsedAt = now;
                bot->TeleportTo(map->GetId(), l.dx, l.dy, l.dz, bot->GetOrientation());
                sLog.outString("[DUNGEONROUTE] %s 升降台/传送点 -> (%.1f,%.1f,%.1f)",
                    bot->GetName(), l.dx, l.dy, l.dz);
                return true;
            }
            return true;                // 等待中：本 tick 不要做别的移动
        }

        if (!nearAny && st.linkWaitAt)
            st.linkWaitAt = 0;          // 离开连接点范围 ⇒ 取消等待
        return false;
    }

    // ---- 拉怪记录（现在由**核心**统一记录所有玩家；这里只补"这一拉是第几波"）----------
    //
    // [2026-10-18 站长口径] 站长："拉怪日志是记录所有玩家在副本的日志，因为我们要采集真玩家数据，
    //   所有图都放行。"
    //   ⇒ 记录逻辑已移到核心 `pullrec::Update()`（src/game/Combat/InstancePullRecorder.cpp）：
    //      **所有副本图**（不再要求 verified=1）、**所有队伍/单人（含真人，不再只认自动开怪）**
    //      的"进战斗 → 脱战"都会落 `ai_playerbot_pull_log`（world 库；列见 dev/192 + dev/194）。
    //
    // 模块这边只做一件事：自动开怪成功起手时，告诉记录器"这一次拉的是第几波（ord）"，
    //   让 bot 路线拉怪的行里带上 ord，方便和 `ai_playerbot_dungeon_route.ord` 对齐。
    //   ord=0 表示 boss 阶段；真人队伍永远不会走到这里（只影响 source='bot' 的行）。
    inline void PullBegin(Player* bot, MapRoute const& route, Target const& target)
    {
        if (!bot || !bot->GetGroup())
            return;

        const uint16 ord = target.boss ? 0 : (target.ord ? target.ord : CurrentOrd(bot, route));
        pullrec::SetRouteOrd(bot->GetGroup(), ord);

        sLog.outString("[DUNGEONROUTE] %s 自动开怪起手 -> 记录器 ord=%u（%s）",
            bot->GetName(), uint32(ord), target.boss ? "boss" : "wave");
    }

    inline void PullEnd(Player* bot, const char* result)
    {
        if (!bot || !bot->GetGroup())
            return;

        pullrec::ClearRouteOrd(bot->GetGroup());

        // [2026-10-18 站长口径] 一拉结束就把"观测到的读条优先级"落库（dev/193_优先级观测表.sql）：
        //   每轮副本结束都能复盘本场见到了哪些治疗/控制读条怪；另有 120s 定时兜底，
        //   非副本场景也会持久化。表不存在时静默降级（观测仍在内存生效）。
        ai::PossibleAttackTargetsValue::FlushPriorityObs();

        if (result)
            sLog.outString("[DUNGEONROUTE] %s 自动开怪拉怪结束（%s）", bot->GetName(), result);
    }

    // ---- 目标选择 -----------------------------------------------------------------
    // 拉怪位：本波有锚点就用锚点；没有（boss 阶段）就走"离目标 22 码、朝我们这侧"的点
    inline void MakeApproachPoint(Player* bot, float tx, float ty, float tz, Target& t)
    {
        float dx = bot->GetPositionX() - tx;
        float dy = bot->GetPositionY() - ty;
        float len = sqrtf(dx * dx + dy * dy);
        if (len < 1e-3f)
        {
            t.px = tx; t.py = ty; t.pz = tz;
        }
        else
        {
            float k = BOSS_PULL_DIST / len;
            t.px = tx + dx * k;
            t.py = ty + dy * k;
            t.pz = tz;
        }
        t.hasPullPoint = true;
    }

    // ---- 找目标（2026-10-18 站长口径 · 模式A：30 码见敌即停，不踩精确锚点）------------
    //   ① 只按"最近"挑目标（不硬套 ord 顺序；ord 只用来记日志/对齐 route 表）
    //   ② 目标波里有一只 ≤30 码且**视线内**的敌对怪 ⇒ 就地停（拉怪位 = 坦克当前位置）并拉它
    //   ③ 否则朝最近的活着目标走过去；目标怪还没加载 ⇒ 用**静态刷点坐标**当路点走过去
    //      （走过去网格就会加载，不再依赖 route 表里生成的锚点是否可达）
    //   ④ 波全清/放弃完 ⇒ boss 阶段，同样支持"没加载先走过去"
    inline bool IsVisible(Player* bot, Creature* c)
    {
        if (!bot || !c)
            return false;
        Map* map = bot->GetMap();
        if (!map)
            return false;
        return map->IsInLineOfSight(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ() + 1.5f,
                                    c->GetPositionX(), c->GetPositionY(), c->GetPositionZ() + 1.0f,
                                    false);
    }

    inline bool MemberSpawnPos(Member const& m, float& x, float& y, float& z)
    {
        CreatureData const* d = sObjectMgr.GetCreatureData(m.guid);
        if (!d)
            return false;
        x = d->posX;
        y = d->posY;
        z = d->posZ;
        return true;
    }

    // 维护"见过活怪 / 已清"：区分"已清"与"远处未加载"（见 CurrentOrdRaw 注释）
    inline void RefreshWaveKnowledge(Map* map, MapRoute const& route, BotState& st)
    {
        for (auto& kv : route.waves)
        {
            if (st.skip.count(kv.first) || st.cleared.count(kv.first))
                continue;

            if (WaveHasAlive(map, kv.second))
                st.seenAlive.insert(kv.first);
            else if (st.seenAlive.count(kv.first))
            {
                st.cleared.insert(kv.first);
                st.seenAlive.erase(kv.first);
                if (st.lockOrd == kv.first)
                    st.lockOrd = 0;             // 锁定的波清完了 ⇒ 解锁，换下一波
            }
        }
    }

    // 找"该打的下一个目标"：先当前波，波全清则 boss 阶段。
    // [2026-10-18 站长口径] 波里先打谁**按威胁等级表**（ai_playerbot_priority_target，越大越优先）：
    //   同级再按距离近。以前这里只按"最近的一只"选 ⇒ 表里点名的怪（加血/召唤/控制）会被
    //   站在旁边的杂兵抢走，表现就是"怪物不一定按照这个表来决定先打哪个"。
    inline bool FindTarget(Player* bot, MapRoute const& route, Target& out)
    {
        Map* map = bot->GetMap();
        uint16 ord = CurrentOrd(bot, route);
        if (ord)
        {
            auto it = route.waves.find(ord);
            if (it != route.waves.end())
            {
                // [2026-10-18 站长口径] 骷髅标记是**人工指挥（结果/动作）**，不是可学习特征：
                //   先按标记直接选中（override）；没有标记才走加权打分。
                //   标记绝不进 score、也不进训练数据集（否则就是标签泄漏，学成"复述人标了谁"）。
                Member bestM;
                for (auto& m : it->second.members)
                {
                    Creature* c = MemberCreature(map, m);
                    if (!c || !SessionCanFight(bot, c))
                        continue;
                    if (c && PossibleAttackTargetsValue::IsSkullMarked(c, bot))
                    {
                        bestM = m;
                        break;
                    }
                }

                bool const skullOverride = (bestM.guid != 0);
                float bestScore = -1e18f;
                float bestFeatureDistance = 0.0f;
                int bestFeatureTier = 0;

                if (!skullOverride)
                {
                    for (auto& m : it->second.members)
                    {
                        Creature* c = MemberCreature(map, m);
                        if (!c)
                            continue;
                        if (!SessionCanFight(bot, c))
                            continue;

                        // 威胁等级：表里的 priority（5 最高）> 启发式（高威胁技能/交战…）
                        //   注意：ThreatTier 里为了兼容"人工标了就先打"仍会看骷髅；数据集里要标 override
                        const int tier = PossibleAttackTargetsValue::ThreatTier(c, bot);
                        const float d = bot->GetDistance(c);

                        // [决策参数化 2026-10-18] 线性加权打分（权重可离线调）
                        float score = dw::Get("target.threat_tier") * float(tier)
                                    + dw::Get("target.distance") * d;
                        if (c->IsNonMeleeSpellCasted(true))
                            score += dw::Get("target.casting");

                        if (score > bestScore)
                        {
                            bestScore = score;
                            bestFeatureDistance = d;
                            bestFeatureTier = tier;
                            bestM = m;
                        }
                    }
                }

                if (bestM.guid)
                {
                    if (dw::Get("log.decision") >= 0.5f)
                        sLog.outString("[DECISION] bot=%s kind=route_target entry=%u override=%d score=%.1f tier=%d dist=%.1f",
                            bot->GetName(), bestM.entry, skullOverride ? 1 : 0,
                            bestScore, bestFeatureTier, bestFeatureDistance);

                    out.guid = bestM.guid;
                    out.entry = bestM.entry;
                    out.boss = false;
                    if (it->second.pull.has)
                    {
                        out.px = it->second.pull.x;
                        out.py = it->second.pull.y;
                        out.pz = it->second.pull.z;
                        out.hasPullPoint = true;
                    }
                    else
                    {
                        Creature* c = MemberCreature(map, bestM);
                        if (c)
                            MakeApproachPoint(bot, c->GetPositionX(), c->GetPositionY(), c->GetPositionZ(), out);
                    }
                    return true;
                }

                // [2026-10-18 站长口径] 本波"还没加载"（怪在远处、网格没起来）：
                //   没有可拉的 Creature，但**必须先把坦克带到拉怪位**，走过去网格才会加载。
                //   guid=0 表示"只是走位，还没有可拉目标"（PullAction 会跳过）。
                if (it->second.pull.has)
                {
                    bool anyUnloaded = false;
                    for (auto& m : it->second.members)
                        if (!map->GetCreature(m.guid) && sObjectMgr.GetCreatureData(m.guid))
                            anyUnloaded = true;

                    if (anyUnloaded)
                    {
                        out = Target();
                        out.guid = 0;
                        out.boss = false;
                        out.px = it->second.pull.x;
                        out.py = it->second.pull.y;
                        out.pz = it->second.pull.z;
                        out.hasPullPoint = true;
                        return true;
                    }
                }
            }
        }

        // boss 阶段：拉还活着的静态 boss（就近）；远处没加载的先走过去
        BotState& st = StateOf(bot);
        float best = 1e18f;
        BossSpawn bestB;
        for (auto& b : route.bosses)
        {
            if (st.bossCleared.count(b.guid))
                continue;

            Creature* c = map->GetCreature(b.guid);
            if (c && !c->IsAlive())
            {
                st.bossCleared.insert(b.guid);      // 尸体 = 已死
                continue;
            }
            if (!c && st.bossSeen.count(b.guid))
            {
                st.bossCleared.insert(b.guid);      // 见过活的、现在没了 = 已死/已消失
                continue;
            }
            if (c)
                st.bossSeen.insert(b.guid);

            float d = bot->GetDistance(c ? c->GetPositionX() : b.x,
                                       c ? c->GetPositionY() : b.y,
                                       c ? c->GetPositionZ() : b.z);
            if (d < best)
            {
                best = d;
                bestB = b;
                if (c)
                {
                    bestB.x = c->GetPositionX();
                    bestB.y = c->GetPositionY();
                    bestB.z = c->GetPositionZ();
                }
            }
        }
        if (bestB.guid)
        {
            out.guid = bestB.guid;                   // 可能是"还没加载"：PullAction 会等/跳过
            out.entry = bestB.entry;
            out.boss = true;
            MakeApproachPoint(bot, bestB.x, bestB.y, bestB.z, out);
            return true;
        }

        return false;
    }

    // ---- [模式A 2026-10-18] 最近目标 + 30 码视线内即停 ---------------------------------
    // 取代"必须踩 route 表精确锚点"的老逻辑：只要 30 码内有可攻击且视线内的怪就停下开拉；
    // 否则朝最近目标/刷点走过去。这样即使 route 表的 ord/锚点不准，队伍也能一路推进。
    inline bool FindTargetNearest(Player* bot, MapRoute const& route, Target& out,
                                  bool onlyCurrentWave = false)
    {
        Map* map = bot->GetMap();
        if (!map || !bot)
            return false;

        BotState& st = StateOf(bot);
        RefreshWaveKnowledge(map, route, st);

        // [2026-10-19 543 实测] 拉怪/移动只瞄 CurrentOrd（当前波）；否则会按“全图最近”
        //   跨波乱拉，和攻击目标过滤（唯一当前波）打架，出现“15s 无目标 → 跳波”。
        uint16 const curOrd = onlyCurrentWave ? CurrentOrd(bot, route) : 0;

        const float STOP_DIST = 30.0f;       // 见敌即停
        const float APPROACH_DIST = 22.0f;   // 走到离怪多近（沿用旧的 22 码）

        Member nearestM;                     // 最近的"活着的、可攻击的"怪
        float nearestAliveDist = 1e18f;
        bool nearestAliveVisible = false;
        uint16 nearestAliveOrd = 0;

        // [2026-10-18 站长口径] "多波同时可见时不要来回横跳，只保证清完一波"：
        //   选中一波后锁定 LOCK_MS，期间只打这一波（除非它被清/被放弃）。
        uint32 const nowMs = WorldTimer::getMSTime();
        bool const lockUsable = st.lockOrd
            && !st.skip.count(st.lockOrd) && !st.cleared.count(st.lockOrd)
            && nowMs < st.lockUntil;

        auto scanWaves = [&](bool lockedOnly)
        {
            nearestM = Member();
            nearestAliveDist = 1e18f;
            nearestAliveVisible = false;
            nearestAliveOrd = 0;

            for (auto& kv : route.waves)
            {
                uint16 const waveOrd = kv.first;
                if (kv.second.allNonHostile)              // 纯 NPC/小动物波：清单里就没有敌对怪
                    continue;
                if (st.skip.count(waveOrd) || st.cleared.count(waveOrd))
                    continue;
                if (lockedOnly && waveOrd != st.lockOrd)
                    continue;
                if (onlyCurrentWave && curOrd && waveOrd != curOrd)
                    continue;

                for (auto& m : kv.second.members)
                {
                    Creature* c = MemberCreature(map, m);
                    if (!c)
                        continue;
                    if (!SessionCanFight(bot, c))
                        continue;                       // 友好 NPC / 小动物：不参与推进

                    // [2026-10-19 简单图口径] 先跳过**水里**的波：Slave Pens 水池的怪 mmap 走不过去，
                    //   反复"拉 1 只 → 跳波"会浪费大量时间；boss 走 bosses 列表，不受这里影响。
                    if (c->IsInWater())
                        continue;

                    // [全图清单] 清单里明确非敌对（小动物/任务 NPC）的跳过；
                    //   运行期动态刷怪不在清单里 ⇒ 保留（不能误杀真正要打的波）。
                    bool rosterHostile = true;
                    if (RosterLookup(map->GetId(), m.guid, rosterHostile) && !rosterHostile)
                        continue;

                    st.seenAlive.insert(waveOrd);

                    float const d = bot->GetDistance(c);
                    if (d < nearestAliveDist)
                    {
                        nearestAliveDist = d;
                        nearestM = m;
                        nearestAliveOrd = waveOrd;
                        nearestAliveVisible = (d <= STOP_DIST) && IsVisible(bot, c);
                    }
                }
            }
        };

        if (lockUsable)
            scanWaves(true);        // 先只看锁定的波
        else if (st.lockOrd)
            st.lockOrd = 0;         // 锁定失效（已清/已放弃/超时）

        if (!nearestM.guid)
            scanWaves(false);       // 锁定的波没活了 ⇒ 放开全图选下一波

        // 锁定"当前推进波"：保证多波同时可见时先把这一波清掉
        if (nearestM.guid && nearestAliveOrd && st.lockOrd != nearestAliveOrd)
        {
            st.lockOrd = nearestAliveOrd;
            st.lockUntil = nowMs + LOCK_MS;
        }

        // ① 有活目标
        if (nearestM.guid)
        {
            Creature* c = map->GetCreature(nearestM.guid);
            out = Target();
            out.guid = nearestM.guid;
            out.entry = nearestM.entry;
            out.ord = nearestAliveOrd;
            out.boss = false;

            if (nearestAliveVisible)
            {
                // 30 码内 + 视线内 ⇒ 就地停，位置就是拉怪位
                out.px = bot->GetPositionX();
                out.py = bot->GetPositionY();
                out.pz = bot->GetPositionZ();
                out.hasPullPoint = true;
                return true;
            }

            // 还没到 30 码/没视线 ⇒ 走过去（22 码外一点，避免贴脸）
            if (c)
                MakeApproachPoint(bot, c->GetPositionX(), c->GetPositionY(), c->GetPositionZ(), out);
            else
                return false;
            return true;
        }

        // ② 没有活目标：朝最近的"波中心"走（mmap 联通的简单图直接走得过去）；
        //    优先用波中心而不是离线锚点/单只刷点——锚点可能不可达，波中心是整波怪的几何中心。
        uint16 goalOrd = 0;
        float goalDist = 1e18f;
        float goalX = 0, goalY = 0, goalZ = 0;
        for (auto& kv : route.waves)
        {
            uint16 const waveOrd = kv.first;
            if (kv.second.allNonHostile)              // 纯 NPC/小动物波：不用走过去
                continue;
            if (st.skip.count(waveOrd) || st.cleared.count(waveOrd))
                continue;
            if (onlyCurrentWave && curOrd && waveOrd != curOrd)
                continue;
            if (!kv.second.hasCenter)
                continue;

            float const d = bot->GetDistance(kv.second.cx, kv.second.cy, kv.second.cz);
            if (d < goalDist)
            {
                goalDist = d;
                goalOrd = waveOrd;
                goalX = kv.second.cx;
                goalY = kv.second.cy;
                goalZ = kv.second.cz;
            }
        }

        if (goalOrd)
        {
            out = Target();
            out.guid = 0;
            out.boss = false;
            out.ord = goalOrd;
            out.px = goalX;
            out.py = goalY;
            out.pz = goalZ;
            out.hasPullPoint = true;
            return true;
        }

        // [2026-10-19] 只瞄当前波时，绝不提前进 boss 阶段
        if (onlyCurrentWave && curOrd)
            return false;

        // ③ boss 阶段：最近的 boss（活着/没加载都先过去）
        float best = 1e18f;
        BossSpawn bestB;
        bool bestBAlive = false;
        for (auto& b : route.bosses)
        {
            if (st.bossCleared.count(b.guid))
                continue;

            Creature* c = map->GetCreature(b.guid);
            if (c && !c->IsAlive())
            {
                st.bossCleared.insert(b.guid);
                continue;
            }
            if (!c && st.bossSeen.count(b.guid))
            {
                st.bossCleared.insert(b.guid);
                continue;
            }
            if (c)
                st.bossSeen.insert(b.guid);

            float const d = bot->GetDistance(c ? c->GetPositionX() : b.x,
                                             c ? c->GetPositionY() : b.y,
                                             c ? c->GetPositionZ() : b.z);
            if (d < best)
            {
                best = d;
                bestB = b;
                bestBAlive = (c != nullptr);
                if (c)
                {
                    bestB.x = c->GetPositionX();
                    bestB.y = c->GetPositionY();
                    bestB.z = c->GetPositionZ();
                }
            }
        }
        if (bestB.guid)
        {
            out = Target();
            out.guid = bestBAlive ? bestB.guid : 0;
            out.entry = bestB.entry;
            out.boss = true;
            out.ord = 0;

            if (bestBAlive && best <= STOP_DIST && IsVisible(bot, map->GetCreature(bestB.guid)))
            {
                out.px = bot->GetPositionX();
                out.py = bot->GetPositionY();
                out.pz = bot->GetPositionZ();
                out.hasPullPoint = true;
            }
            else
            {
                MakeApproachPoint(bot, bestB.x, bestB.y, bestB.z, out);
            }
            out.hasPullPoint = true;
            return true;
        }

        return false;
    }

    // ---- "能不能打它"（PossibleAttackTargetsValue 过滤用）--------------------------
    inline bool IsAttackingUs(Player* bot, Unit* unit)
    {
        if (!unit)
            return false;
        if (Unit* v = unit->GetVictim())
        {
            if (v == bot)
                return true;

            Unit* vMaster = v->GetMaster();
            Player* vPlayer = vMaster && vMaster->GetTypeId() == TYPEID_PLAYER
                ? (Player*)vMaster
                : (v->GetTypeId() == TYPEID_PLAYER ? (Player*)v : nullptr);
            if (vPlayer)
            {
                Group* g = bot->GetGroup();
                if (g && g->IsMember(vPlayer->GetObjectGuid()))
                    return true;
            }
        }
        if (unit->getThreatManager().getThreat(bot) > 0.0f)
            return true;

        // [2026-10-19 543 实测] 引到队友/队友宠物的怪也算“在打我们”：
        //   云端标记的路线跳过了一部分怪，坦克拉路线怪时会把它们引到；
        //   只要它们已经在打队伍里任何一人，就必须允许还手，否则会“战斗中无合法目标”。
        if (Group* g = bot->GetGroup())
        {
            for (GroupReference* ref = g->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->getSource();
                if (member && member->IsInWorld() && unit->getThreatManager().getThreat(member) > 0.0f)
                    return true;
            }
        }
        return false;
    }

    inline bool AllowAttack(Player* bot, Unit* unit)
    {
        if (!RouteEnabled())
            return true;
        if (!bot || !unit || unit->IsPlayer())
            return true;
        if (unit->GetTypeId() != TYPEID_UNIT)
            return true;

        Map* map = bot->GetMap();
        if (!map || (!map->IsDungeon() && !map->IsRaid()))
            return true;

        MapRoute const* route = GetMapRoute(map->GetId());
        if (!route || !route->verified || route->waves.empty())
            return true;

        // ① 已经在打我们（或队友）⇒ 一律放行：被打必须还手（= "类型B 巡逻遇上就清"）
        if (IsAttackingUs(bot, unit))
            return true;

        // ② 波全清完（boss 阶段）：不再限制（boss 可能不在 route 表里）
        uint16 cur = CurrentOrd(bot, *route);
        if (cur == 0)
            return true;

        // ③ 只打"还没清的波"的成员：已清/已放弃的波不再回头打；
        //    [模式A] 不再要求等于当前 ord（就近推进时可能同时接触两波）。
        auto it = route->guidOrd.find(unit->GetGUIDLow());
        if (it == route->guidOrd.end())
            return false;

        BotState& st = StateOf(bot);
        uint16 const waveOrd = it->second;
        if (st.cleared.count(waveOrd) || st.skip.count(waveOrd))
            return false;
        return true;
    }


    // ---- 驱动资格（"谁是驱动坦克"）------------------------------------------------
    // ① 坦克 bot；② 队伍里没有"真人坦克"；③ 多个 bot 坦克时 guid 最小的那个（避免抢拉）
    inline bool ShouldDrive(PlayerbotAI* ai)
    {
        if (!RouteEnabled())
            return false;

        Player* bot = ai ? ai->GetBot() : nullptr;
        if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported() || !bot->IsAlive())
            return false;

        // [自动进本 2026-10-18] 不在本里 ⇒ 发起"带队伍进本"，本 tick 不推进路线
        if (!dungeonrun::MaybeEnterDungeon(bot, RouteRuntimeSwitch()))
            return false;
        if (bot->IsInCombat())
            return false;
        if (!PlayerbotAI::IsTank(bot))
            return false;

        Map* map = bot->GetMap();
        if (!map || (!map->IsDungeon() && !map->IsRaid()))
            return false;

        MapRoute const* route = GetMapRoute(map->GetId());
        if (!route || route->waves.empty())
            return false;
        // [自动进本] 自动进本模式下允许未 verified 的图
        if (!route->verified && !dungeonrun::AllowUnverified(RouteRuntimeSwitch()))
            return false;

        if (Group* group = bot->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* m = ref->getSource();
                if (!m || m == bot || !m->IsInWorld())
                    continue;
                if (PlayerbotAI::IsTank(m))
                {
                    if (!m->GetPlayerbotAI())                     // 真人坦克：让真人拉
                        return false;
                    if (m->GetGUIDLow() < bot->GetGUIDLow())      // 别的 bot 坦克更小：让它拉
                        return false;
                }
            }
        }

        return true;
    }

    // 队伍/主人就位（防止坦克一个人冲出去）
    inline bool ReadyToEngage(PlayerbotAI* ai, float dist)
    {
        Player* bot = ai->GetBot();
        Player* master = ai->GetMaster();
        if (master && !master->GetPlayerbotAI())
        {
            if (!master->IsAlive() || !master->IsInWorld())
                return false;
            if (master->GetMapId() != bot->GetMapId())
                return false;
            if (bot->GetDistance(master) > dist)
                return false;
        }
        return true;
    }
}
