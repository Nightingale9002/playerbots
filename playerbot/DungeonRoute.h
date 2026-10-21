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
#include <algorithm>

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

    // [2026-10-22 站长口径] BOSS 优先模式（运行时开关，仿真 `.rndbot sim start ... boss=1` 打开）：
    //   顺序：先按 boss 顺序去可达的 boss；不可达（45 秒没有靠近趋势）⇒ 暂时挂起该 boss，
    //         退回该图原有的波次/特殊寻路（跳点、升降台、门禁开关都在这条路里），
    //         波次有推进后立刻重新检查 boss 可达性；
    //   路上一旦被敌对怪拉住 ⇒ 正常 pull（`AllowAttack` 在战斗就放行 + 坦克接漏怪），不会绕过去。
    inline bool& BossFirstSwitch()
    {
        static bool on = false;
        return on;
    }

    inline bool BossFirstMode() { return BossFirstSwitch(); }

    struct Anchor { bool has = false; float x = 0, y = 0, z = 0; };
    struct Member { uint32 guid = 0; uint32 entry = 0; };
    struct Wave { uint16 ord = 0; uint32 gid = 0; std::vector<Member> members; Anchor pull;
                  float cx = 0, cy = 0, cz = 0; bool hasCenter = false;   // 波中心（刷点均值）
                  bool allNonHostile = false; };   // 全图清单判定：整波没有敌对怪（纯 NPC/小动物）
    // [2026-10-22 站长口径] "boss 优先"模式：ord 用来排 boss 顺序（取自路线表里该 boss 刷点的最小 ord；
    //   路线里没有这个 boss 时为 0 ⇒ 排在最后，按离入口距离决定）。
    struct BossSpawn { uint32 guid = 0; uint32 entry = 0; float x = 0, y = 0, z = 0; uint16 ord = 0; };

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
        // [2026-10-19 站长口径] 动态巡逻：不绝对按波次表，运行到附近再动态决策
        std::set<uint32> patrolGuids;             // 本图巡逻怪 guid（ai_playerbot_patrol）
        uint16 maxOrd = 0;
    };

    struct Target
    {
        uint32 guid = 0;
        uint32 entry = 0;
        uint16 ord = 0;              // 目标所属波次（0 = boss 阶段）
        bool boss = false;
        bool dynamic = false;        // 动态巡逻目标（不按波次表，遇到就清）
        // [2026-10-22 站长口径] "boss 不可达 ⇒ 先走我们补的跳跃点，到了跳过去再重查"：
        //   linkHop = 目标点是连接点 src（px/py/pz），到附近后用 (lx,ly,lz) 作为方向执行跳跃/电梯。
        bool linkHop = false;
        float lx = 0, ly = 0, lz = 0;
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
        // [2026-10-22 站长口径] 跳过只是"暂时延后"，不是永久放弃：
        //   skipUntil[ord] = 到这个时间点重新打开该波再试；skipRetry = 已重试次数（超上限才永久 skip）。
        std::map<uint16, uint32> skipUntil;
        std::map<uint16, uint8> skipRetry;
        // [2026-10-22] boss 优先模式：当前正在去的 boss + 卡住判定 / 某 boss 挂起到何时（到点重新检查可达）
        uint32 bossGuid = 0;
        uint32 bossSince = 0;
        float bossBestDist = 1e9f;
        std::map<uint32, uint32> bossDeferUntil;
        std::set<uint32> bossLinksTried;
        // [2026-10-22 站长口径] "30 码内可拉但会引到巡逻怪" ⇒ 原地等它走开
        //   patrolWaitGuid = 挡路的巡逻怪 guid；每秒重判一次，最多等 patrolWaitMaxMs
        uint32 patrolWaitGuid = 0;
        uint32 patrolWaitAt = 0;
        uint32 patrolWaitSince = 0;
        uint32 patrolWaitLogAt = 0;   // [2026-10-22] 已为"不可达 boss"试过的连接点下标（避免死循环）
        std::set<uint16> seenAlive;   // 见过活怪的波（用来区分"已清"与"远处未加载"）
        std::set<uint16> cleared;     // 确认已清的波
        std::set<uint32> bossSeen;    // 见过活的 boss 刷点
        std::set<uint32> bossCleared; // 确认已死的 boss 刷点（只认尸体，跳过不算）
        std::set<uint32> bossSkipped; // 放弃的 boss 刷点（未加载/不可达）——绝不算已死
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
        uint32 unknownHoldSince = 0;  // [2026-10-21] "未登记怪在战斗"的保持起点（0 = 没保持）
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

        // [2026-10-22] BOSS 优先模式要"按顺序"：ord 取该 boss 刷点在路线表里的最小 ord
        //   （路线里没登记 ⇒ 0，排最后）；同时按 ord 排序，省得运行期再排。
        std::unique_ptr<QueryResult> bosses = WorldDatabase.PQuery(
            "SELECT b.entry, b.x, b.y, b.z, c.guid, "
            "(SELECT MIN(r.ord) FROM `ai_playerbot_dungeon_route` r WHERE r.map = b.map AND r.guid = c.guid) "
            "FROM `ai_playerbot_boss_spawn` b "
            "JOIN `creature` c ON c.id = b.entry AND c.map = b.map WHERE b.map = '%u' "
            "ORDER BY (ord IS NULL), ord, b.entry", mapId);
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
                b.ord = uint16(f[5].GetUInt32());
                route.bosses.push_back(b);
            } while (bosses->NextRow());
        }

        // [2026-10-19] 动态巡逻：只取 guid 集合；运行期遇到（可见/近）才动态决定拉不拉。
        {
            std::unique_ptr<QueryResult> patrols = WorldDatabase.PQuery(
                "SELECT DISTINCT `guid` FROM `ai_playerbot_patrol` WHERE `map` = '%u'", mapId);
            if (patrols)
            {
                do
                {
                    route.patrolGuids.insert(patrols->Fetch()[0].GetUInt32());
                } while (patrols->NextRow());
            }
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

    // [2026-10-21 站长口径·兜底] 有没有"没登记进路线表"的怪正在打队伍。
    //   为什么需要：路线表只登记了副本的一部分怪（546 33/228、547 47/238…），
    //   没登记的怪被引到后只能靠 AllowAttack 的"在战斗就放行"还手，但 **CurrentOrd 不认识它们**，
    //   结果队伍拉着治疗打 Unbat 的同时，坦克已经走去开下一波（正好违反"只要一个怪在和我们战斗就不应该跳过"）。
    inline bool UnknownFighterInCombat(Player* bot, MapRoute const& route, float maxDist = 60.0f)
    {
        Group* g = bot->GetGroup();
        if (!g)
            return false;

        Map* map = bot->GetMap();
        for (GroupReference* ref = g->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->getSource();
            if (!member || !member->IsInWorld() || member->GetMap() != map)
                continue;

            for (Unit* att : member->getAttackers())
            {
                if (!att || !att->IsAlive() || att->GetTypeId() != TYPEID_UNIT)
                    continue;
                Creature* c = static_cast<Creature*>(att);
                if (c->IsPet() || c->IsTotem())
                    continue;
                if (route.guidOrd.count(c->GetGUIDLow()))
                    continue;                       // 登记过的怪由波次逻辑管，不在这里兜
                if (member->GetDistance(c) > maxDist)
                    continue;
                return true;
            }
        }
        return false;
    }

    // 离 bot 最近的"还有活怪"的波 ord（0 = 没有）
    inline uint16 NearestOrdByPosition(Player* bot, MapRoute const& route)
    {
        Map* map = bot->GetMap();
        float best = 1e18f;
        uint16 bestOrd = 0;
        for (auto& kv : route.waves)
        {
            for (auto& m : kv.second.members)
            {
                Creature* c = map->GetCreature(m.guid);
                if (!c || !c->IsAlive())
                    continue;
                float const d = bot->GetDistance(c);
                if (d < best)
                {
                    best = d;
                    bestOrd = kv.first;
                }
            }
        }
        return bestOrd;
    }

    // [2026-10-22 站长口径] "跳过"= 暂时延后，不是永久放弃：
    //   · 有 skipUntil 且还没到点 ⇒ 仍算跳过；
    //   · 到点了 ⇒ 自动重新打开这一波（清掉 skip），再给一次机会；
    //   · 永久跳过（重试次数超上限）没有 skipUntil，保持跳过。
    inline bool SkipStillActive(BotState& st, uint16 ord)
    {
        if (!st.skip.count(ord))
            return false;

        auto it = st.skipUntil.find(ord);
        if (it == st.skipUntil.end() || !it->second)
            return true;                       // 永久放弃

        uint32 const now = WorldTimer::getMSTime();
        if (now < it->second)
            return true;                       // 还没到重试时间

        st.skip.erase(ord);
        st.skipUntil.erase(it);
        st.calcedAt = 0;
        sLog.outString("[DUNGEONROUTE] 重新尝试之前暂跳过的波 ord=%u（到点复活）", uint32(ord));
        return false;
    }

    // 当前波 = **还有活怪/没加载怪的最小 ord**（跳过 skip / 已清；0 = 没有）
    inline uint16 CurrentOrdRaw(Player* bot, MapRoute const& route, BotState& st)
    {
        Map* map = bot->GetMap();

        // [2026-10-21 站长口径·兜底] 有未登记的怪正在打队伍 ⇒ 不许推进到下一波，先把它清掉。
        //   45 秒上限：万一那只怪不可达/逃脱卡住（546 的 Underbat 就追不到），
        //   不能让整支队伍无限停在原地。
        if (UnknownFighterInCombat(bot, route))
        {
            uint32 const now = WorldTimer::getMSTime();
            bool const freshHold = (st.unknownHoldSince == 0);
            if (freshHold)
                st.unknownHoldSince = now ? now : 1;
            else if (WorldTimer::getMSTimeDiff(st.unknownHoldSince, now) < 45000)
            {
                uint16 const hold = st.ord ? st.ord : NearestOrdByPosition(bot, route);
                if (hold)
                {
                    if (freshHold)
                        sLog.outString("[DUNGEONROUTE] %s 有未登记怪在战斗 ⇒ 保持波次 ord=%u（不推进）",
                            bot->GetName(), uint32(hold));
                    return hold;
                }
            }
        }
        else
            st.unknownHoldSince = 0;

        for (auto& kv : route.waves)
        {
            if (SkipStillActive(st, kv.first) || st.cleared.count(kv.first))
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
        // [2026-10-22] 波次有推进 ⇒ 立刻重查一遍刚才"不可达"的 boss（站长口径：再重新检查可达）
        if (!st.bossDeferUntil.empty())
        {
            st.bossDeferUntil.clear();
            st.bossLinksTried.clear();
            st.bossGuid = 0;
            st.bossSince = 0;
        }
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
    // [2026-10-19 站长口径] 跳过 ≠ 已死：以前这里插 bossCleared，导致"放弃 boss 也算通关"，
    //   仿真里 17/19 波就判 cleared 就是这么来的。现在单独记 bossSkipped，通关只看 bossCleared。
    inline void MarkBossSkipped(Player* bot, uint32 dbGuid)
    {
        if (!bot || !dbGuid)
            return;
        BotState& st = StateOf(bot);
        if (st.bossSkipped.insert(dbGuid).second)
            sLog.outString("[DUNGEONROUTE] %s 跳过 boss 刷点 %u（未加载/不可达，不算已死）",
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
                if (st.bossSkipped.count(b.guid))
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

            // [2026-10-21 站长口径] kind=2 客户端式跳跃/坠落：上层 -> 下层（mmap 可能不连通，
            //   因为服务器有门/落差；客户端是自己算落点，这里服务端按同一套抛物线算 vXY/vZ 后 MoveJump）。
            if (l.kind == 2)
            {
                if (!sPlayerbotAIConfig.routeJumpAssist)
                    continue;
                if (st.linkUsedAt && WorldTimer::getMSTimeDiff(st.linkUsedAt, now) < 5000)
                    continue;

                float const ddx2 = l.dx - bot->GetPositionX();
                float const ddy2 = l.dy - bot->GetPositionY();
                float const dist2d = sqrtf(ddx2 * ddx2 + ddy2 * ddy2);
                float const drop = bot->GetPositionZ() - l.dz;   // >0 = 往下跳/掉
                float const v0 = sPlayerbotAIConfig.jumpVSpeed;
                float const g = 19.2911f;
                float const disc = v0 * v0 + 2.0f * g * drop;    // 上跳时 drop<0
                if (disc <= 0.0f)
                    continue;
                float const t = (v0 + sqrtf(disc)) / g;
                if (t <= 0.0f || t > 15.0f)
                    continue;
                float const speedXY = dist2d / t;
                float const maxXY = bot->GetSpeed(MOVE_RUN) * sPlayerbotAIConfig.jumpHSpeed * 1.05f + 0.5f;
                if (speedXY > maxXY)
                {
                    sLog.outString("[DUNGEONROUTE] %s 跳跃链接不可行：vXY=%.1f > max=%.1f (dist=%.1f drop=%.1f)",
                        bot->GetName(), speedXY, maxXY, dist2d, drop);
                    continue;
                }

                nearAny = true;
                st.linkUsedAt = now;
                st.linkWaitAt = 0;
                bot->GetMotionMaster()->MoveJump(l.dx, l.dy, l.dz, speedXY, v0);
                sLog.outString("[DUNGEONROUTE] %s 客户端式跳跃 (%.1f,%.1f,%.1f) -> (%.1f,%.1f,%.1f) vXY=%.1f vZ=%.1f t=%.2f",
                    bot->GetName(), l.sx, l.sy, l.sz, l.dx, l.dy, l.dz, speedXY, v0, t);
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
        // [2026-10-21 假清修复] 灭团/出本后 map 上所有怪 GetCreature 都为空，
        //   不能据此把"见过活怪"的波判定为已清（543 出现过 66 秒 25/25 的假通关）。
        if (!map || map->GetId() != route.mapId)
            return;

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
        // [2026-10-21 站长口径] 已清波必须"确实没有刷新"才算清：
        //   召唤/刷新怪（如碎手军团士兵的援军）重新出现时，把该波重新打开，
        //   避免"清一次就结束"导致后面漏掉刷新出来的怪。
        for (auto it = st.cleared.begin(); it != st.cleared.end(); )
        {
            auto wit = route.waves.find(*it);
            if (wit != route.waves.end() && WaveHasAlive(map, wit->second))
            {
                uint16 const ord = *it;
                it = st.cleared.erase(it);
                st.seenAlive.insert(ord);
                if (st.lockOrd == ord)
                    st.lockOrd = 0;
                sLog.outString("[DUNGEONROUTE] 波 %u 的怪重新刷出 ⇒ 重新打开（不能算已清）", (uint32)ord);
            }
            else
                ++it;
        }
    }

    // 维护 boss 知识：只把**看见尸体**的 boss 记成已死；网格卸载（GetCreature 为空）不算死。
    inline void RefreshBossKnowledge(Map* map, MapRoute const& route, BotState& st)
    {
        if (!map)
            return;

        for (auto const& b : route.bosses)
        {
            if (st.bossCleared.count(b.guid))
                continue;

            Creature* c = map->GetCreature(b.guid);
            if (!c)
                continue;                       // 未加载：既不算死，也不算见过

            if (!c->IsAlive())
            {
                st.bossCleared.insert(b.guid);
                st.bossSkipped.erase(b.guid);
            }
            else
                st.bossSeen.insert(b.guid);
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
            if (st.bossCleared.count(b.guid) || st.bossSkipped.count(b.guid))
                continue;

            Creature* c = map->GetCreature(b.guid);
            if (c && !c->IsAlive())
            {
                st.bossCleared.insert(b.guid);      // 尸体 = 已死
                continue;
            }
            // [boss-fix 2026-10-19] 不能再把“见过活的、现在没加载”当成已死：
            //   网格卸载时 GetCreature 也会返回 null，之前 Omor/Vazruden 就是这样被误判成已清。
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

    // [2026-10-22 站长口径] boss 优先模式：路上遇到 30 码内的怪，拉之前先判断"拉它会不会引到别的怪"。
    //   判据：目标 10 码内还有**别的波次**的敌对活怪、或未登记进路线的敌对活怪 ⇒ 视为会连带 ADD，先不拉它。
    //   （同一波/同一刷怪组的成员不算"别的怪"——那本来就是要一起打的遭遇）
    inline bool BossModePullSafe(Player* bot, Map* map, MapRoute const& route, Creature* target)
    {
        if (!bot || !map || !target)
            return false;

        uint16 const targetOrd = [&]()
        {
            auto it = route.guidOrd.find(target->GetGUIDLow());
            return it == route.guidOrd.end() ? uint16(0) : it->second;
        }();

        auto risky = [&](uint32 guid) -> bool
        {
            if (guid == target->GetGUIDLow())
                return false;
            Creature* c = map->GetCreature(guid);
            if (!c || !c->IsAlive() || !c->IsInWorld())
                return false;
            if (!SessionCanFight(bot, c))
                return false;
            if (!target->IsWithinDist(c, 10.0f, false))
                return false;

            auto it = route.guidOrd.find(guid);
            uint16 const ord = it == route.guidOrd.end() ? uint16(0) : it->second;
            // 同波 ⇒ 不算 ADD；不同波 / 未登记 ⇒ 有连带风险
            return ord != targetOrd || ord == 0;
        };

        for (auto& kv : route.waves)
        {
            for (auto& m : kv.second.members)
            {
                if (risky(m.guid))
                    return false;
            }
        }
        for (uint32 pg : route.patrolGuids)
        {
            if (risky(pg))
                return false;
        }
        return true;
    }

    // [2026-10-22 站长口径] 与 BossModePullSafe 同样的判定，但**返回挡路的怪 guid**：
    //   用于区分"能不能等" —— 挡路的是巡逻怪 ⇒ 等它走开；是别的波/未登记怪 ⇒ 等也没用。
    inline uint32 BossModePullBlocker(Player* bot, Map* map, MapRoute const& route, Creature* target)
    {
        if (!bot || !map || !target)
            return 0;

        auto ordOf = [&](uint32 guid) -> uint16
        {
            auto it = route.guidOrd.find(guid);
            return it == route.guidOrd.end() ? uint16(0) : it->second;
        };
        uint16 const targetOrd = ordOf(target->GetGUIDLow());
        uint32 blocker = 0;

        auto risky = [&](uint32 guid) -> bool
        {
            if (guid == target->GetGUIDLow())
                return false;
            Creature* c = map->GetCreature(guid);
            if (!c || !c->IsAlive() || !c->IsInWorld())
                return false;
            if (!SessionCanFight(bot, c))
                return false;
            if (!target->IsWithinDist(c, 10.0f, false))
                return false;
            uint16 const ord = ordOf(guid);
            if (ord != targetOrd || ord == 0)
            {
                blocker = guid;
                return true;
            }
            return false;
        };

        for (auto& kv : route.waves)
            for (auto& m : kv.second.members)
                if (risky(m.guid))
                    return blocker;
        for (uint32 pg : route.patrolGuids)
            if (risky(pg))
                return blocker;
        return 0;
    }

    // [2026-10-22 站长口径] boss 优先：所有 boss 都不可达（挂起）时，按 link 跳到下一个连通区间再重查。
    //   选中条件：该 link 的落点比起点更靠近某个还活着的 boss；已试过的 link 不重试（波次推进/挂起到期会清表）。
    inline bool FindLinkExplorationTarget(Player* bot, MapRoute const& route, BotState& st, Target& out)
    {
        if (!bot || route.links.empty())
            return false;

        Map* map = bot->GetMap();
        if (!map)
            return false;

        // 取一个"还没打到"的 boss 作为方向参考
        BossSpawn const* toward = nullptr;
        float bestBossDist = 1e18f;
        for (auto const& b : route.bosses)
        {
            if (st.bossCleared.count(b.guid))
                continue;
            float const d = bot->GetDistance(b.x, b.y, b.z);
            if (d < bestBossDist)
            {
                bestBossDist = d;
                toward = &b;
            }
        }
        if (!toward)
            return false;

        size_t bestIdx = size_t(-1);
        float bestScore = 1e18f;
        for (size_t li = 0; li < route.links.size(); ++li)
        {
            if (st.bossLinksTried.count(uint32(li)))
                continue;
            RouteLink const& l = route.links[li];
            if (bot->GetDistance(l.dx, l.dy, l.dz) <= 6.0f)
            {
                st.bossLinksTried.insert(uint32(li));
                continue;
            }
            float const srcToBoss = std::sqrt((l.sx - toward->x) * (l.sx - toward->x)
                + (l.sy - toward->y) * (l.sy - toward->y) + (l.sz - toward->z) * (l.sz - toward->z));
            float const dstToBoss = std::sqrt((l.dx - toward->x) * (l.dx - toward->x)
                + (l.dy - toward->y) * (l.dy - toward->y) + (l.dz - toward->z) * (l.dz - toward->z));
            if (dstToBoss >= srcToBoss - 1.0f)
                continue;                        // 这个 link 不靠近 boss
            float const score = bot->GetDistance(l.sx, l.sy, l.sz);
            if (score < bestScore)
            {
                bestScore = score;
                bestIdx = li;
            }
        }
        if (bestIdx == size_t(-1))
            return false;

        RouteLink const& l = route.links[bestIdx];
        st.bossLinksTried.insert(uint32(bestIdx));

        out = Target();
        out.guid = 0;
        out.boss = (toward->guid != 0);
        out.ord = toward->ord;
        out.linkHop = true;
        out.lx = l.dx; out.ly = l.dy; out.lz = l.dz;
        out.px = l.sx; out.py = l.sy; out.pz = l.sz;
        out.hasPullPoint = true;
        sLog.outString("[DUNGEONROUTE] %s 没有可达 boss ⇒ 走连接点#%u (%.1f,%.1f,%.1f)->(%.1f,%.1f,%.1f)，跳完重查 boss",
            bot->GetName(), uint32(bestIdx), l.sx, l.sy, l.sz, l.dx, l.dy, l.dz);
        return true;
    }

    // [2026-10-22 站长口径] BOSS 优先：按 ord 顺序找第一个"还活着/还没到"的 boss 当目标。
    //   可达性判定：45 秒内距离没有缩短 ≥3 码 ⇒ 认为不可达（门没开/在另一层/没有路），
    //   挂起 120 秒退回"波次/特殊寻路"，到点自动重新检查；波次有推进时也会立刻重查（MarkCleared 里清表）。
    inline bool FindBossTarget(Player* bot, MapRoute const& route, BotState& st, Target& out)
    {
        if (route.bosses.empty())
            return false;

        Map* map = bot->GetMap();
        if (!map)
            return false;

        uint32 const now = WorldTimer::getMSTime();

        // 顺序：ord 升序；路线里没登记的 boss（ord=0）排最后
        std::vector<BossSpawn const*> ordered;
        ordered.reserve(route.bosses.size());
        for (auto const& b : route.bosses)
            ordered.push_back(&b);
        std::sort(ordered.begin(), ordered.end(), [](BossSpawn const* a, BossSpawn const* b)
        {
            bool const aHas = a->ord != 0;
            bool const bHas = b->ord != 0;
            if (aHas != bHas)
                return aHas;                     // 有 ord 的优先
            if (aHas && a->ord != b->ord)
                return a->ord < b->ord;
            return a->guid < b->guid;
        });

        for (BossSpawn const* bp : ordered)
        {
            uint32 const guid = bp->guid;
            if (!guid || st.bossCleared.count(guid))
                continue;

            auto dit = st.bossDeferUntil.find(guid);
            if (dit != st.bossDeferUntil.end())
            {
                if (dit->second > now)
                    continue;                    // 还在挂起期：这次先走波次
                st.bossDeferUntil.erase(dit);    // 到点：重新检查可达
                st.bossLinksTried.clear();       // 连接点也重试
                sLog.outString("[DUNGEONROUTE] %s 重新检查 boss 可达性 entry=%u（挂起到期）",
                    bot->GetName(), bp->entry);
            }

            Creature* c = map->GetCreature(guid);
            bool const loaded = (c != nullptr);
            if (loaded && !c->IsAlive())
                continue;                        // 尸体由 RefreshBossKnowledge 记账

            float const tx = loaded ? c->GetPositionX() : bp->x;
            float const ty = loaded ? c->GetPositionY() : bp->y;
            float const tz = loaded ? c->GetPositionZ() : bp->z;
            float const dist = bot->GetDistance(tx, ty, tz);

            // 卡住判定：45 秒没有靠近趋势
            if (st.bossGuid != guid)
            {
                st.bossGuid = guid;
                st.bossSince = now ? now : 1;
                st.bossBestDist = dist;
            }
            else if (dist < st.bossBestDist - 3.0f)
            {
                st.bossBestDist = dist;
                st.bossSince = now ? now : 1;
            }
            else if (st.bossSince && now > st.bossSince &&
                     WorldTimer::getMSTimeDiff(st.bossSince, now) > 45000)
            {
                // [2026-10-22 站长口径] 走不过去先别放弃：如果图里有连接点（跳跃点/升降台/传送）
                //   能让我们更靠近这个 boss，就先走过去把它执行了，跳完再重新检查可达性；
                //   全部连接点都试过仍不行，才挂起该 boss、改走波次/特殊寻路。
                for (size_t li = 0; li < route.links.size(); ++li)
                {
                    if (st.bossLinksTried.count(uint32(li)))
                        continue;

                    RouteLink const& l = route.links[li];
                    float const sdx = l.sx - tx, sdy = l.sy - ty, sdz = l.sz - tz;
                    float const ddx = l.dx - tx, ddy = l.dy - ty, ddz = l.dz - tz;
                    float const srcToBoss = std::sqrt(sdx * sdx + sdy * sdy + sdz * sdz);
                    float const dstToBoss = std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
                    if (dstToBoss >= srcToBoss - 1.0f)
                        continue;                                  // 这个连接点不靠近 boss
                    if (bot->GetDistance(l.dx, l.dy, l.dz) <= 6.0f)
                    {
                        st.bossLinksTried.insert(uint32(li));      // 已经站在落点
                        continue;
                    }

                    st.bossLinksTried.insert(uint32(li));
                    st.bossSince = now ? now : 1;
                    st.bossBestDist = dist;

                    out = Target();
                    out.guid = 0;
                    out.entry = bp->entry;
                    out.ord = bp->ord;
                    out.boss = true;
                    out.linkHop = true;
                    out.lx = l.dx; out.ly = l.dy; out.lz = l.dz;
                    out.px = l.sx; out.py = l.sy; out.pz = l.sz;
                    out.hasPullPoint = true;
                    sLog.outString("[DUNGEONROUTE] %s boss entry=%u 45 秒走不过去 ⇒ 先执行连接点#%u "
                        "(%.1f,%.1f,%.1f)->(%.1f,%.1f,%.1f)，跳完再查可达",
                        bot->GetName(), bp->entry, uint32(li), l.sx, l.sy, l.sz, l.dx, l.dy, l.dz);
                    return true;
                }

                st.bossDeferUntil[guid] = (now ? now : 1) + 120000;
                st.bossGuid = 0;
                st.bossSince = 0;
                sLog.outString("[DUNGEONROUTE] %s boss entry=%u 45 秒没有靠近（dist=%.0f，连接点都试过）⇒ 暂时挂起，改走波次/特殊寻路",
                    bot->GetName(), bp->entry, dist);
                continue;
            }

            out = Target();
            out.guid = guid;
            out.entry = bp->entry;
            out.ord = bp->ord;
            out.boss = true;
            out.hasPullPoint = true;

            if (bp->ord)
            {
                auto wit = route.waves.find(bp->ord);
                if (wit != route.waves.end() && wit->second.pull.has)
                {
                    out.px = wit->second.pull.x;
                    out.py = wit->second.pull.y;
                    out.pz = wit->second.pull.z;
                    return true;
                }
            }

            if (loaded && dist <= 30.0f)
            {
                out.px = bot->GetPositionX();
                out.py = bot->GetPositionY();
                out.pz = bot->GetPositionZ();
                return true;
            }

            MakeApproachPoint(bot, tx, ty, tz, out);
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
        RefreshBossKnowledge(map, route, st);

        // [2026-10-22 站长口径] BOSS 优先模式的两条规矩：
        //   ① 路上遇到**30 码内、可拉**的敌对怪/敌对该波 ⇒ 先打它（不绕过、不硬冲 boss）；
        //   ② 附近没有可拉的怪 ⇒ 才按 boss 顺序去可达 boss；boss 不可达/挂起 ⇒ 落回波次/特殊寻路。
        bool const bossMode = BossFirstMode();

        // [2026-10-19 543 实测] 拉怪/移动只瞄 CurrentOrd（当前波）；否则会按“全图最近”
        //   跨波乱拉，和攻击目标过滤（唯一当前波）打架，出现“15s 无目标 → 跳波”。
        uint16 const curOrd = onlyCurrentWave ? CurrentOrd(bot, route) : 0;

        const float STOP_DIST = 30.0f;       // 见敌即停（保留）
        const float PULL_ENGAGE_DIST = 30.0f; // 站长口径：停步点保留 30 码，拉不到由拉怪策略自己找位置/走近
        const float APPROACH_DIST = 22.0f;   // 走到离怪多近（沿用旧的 22 码）

        Member nearestM;                     // 最近的"活着的、可攻击的"怪
        float nearestAliveDist = 1e18f;
        bool nearestAliveVisible = false;
        uint16 nearestAliveOrd = 0;

        // [2026-10-18 站长口径] "多波同时可见时不要来回横跳，只保证清完一波"：
        //   选中一波后锁定 LOCK_MS，期间只打这一波（除非它被清/被放弃）。
        //   boss 优先模式不锁波次（目标就是"路过的都先打掉"，锁死会导致只盯一波）。
        uint32 const nowMs = WorldTimer::getMSTime();
        bool const lockUsable = !bossMode && st.lockOrd
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

                    float const d = bot->GetDistance(c);

                    // [2026-10-22 站长口径] boss 优先模式：只把"30 码内、视线内、且**拉它不会引到别的怪**"
                    //   的怪当成"路上遇到要先打的一波"；会 ADD 的、更远的都不主动拉（先打 boss）。
                    if (bossMode)
                    {
                        if (d > STOP_DIST || !IsVisible(bot, c))
                            continue;
                        uint32 const blocker = BossModePullBlocker(bot, map, route, c);
                        if (blocker)
                        {
                            // 挡路的是巡逻怪 ⇒ 记下来，进入"原地等它走开"（每秒判一次，最多 30 秒）
                            if (!st.patrolWaitGuid && route.patrolGuids.count(blocker))
                            {
                                st.patrolWaitGuid = blocker;
                                st.patrolWaitAt = 0;
                                st.patrolWaitSince = 0;
                                st.patrolWaitLogAt = 0;
                            }
                            continue;
                        }
                    }

                    st.seenAlive.insert(waveOrd);

                    if (d < nearestAliveDist)
                    {
                        nearestAliveDist = d;
                        nearestM = m;
                        nearestAliveOrd = waveOrd;
                        nearestAliveVisible = (d <= PULL_ENGAGE_DIST) && IsVisible(bot, c);
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

        // [2026-10-22 站长口径] 30 码内有可拉怪、但会引到巡逻怪 ⇒ 原地等巡逻走开：
        //   每秒重判一次，最多等 30 秒；巡逻走开就正常拉，等满还挡路就放弃这一只继续推进。
        if (bossMode && st.patrolWaitGuid)
        {
            uint32 const waitNow = WorldTimer::getMSTime();
            if (!st.patrolWaitSince)
                st.patrolWaitSince = waitNow ? waitNow : 1;

            bool const timeToCheck = (!st.patrolWaitAt) || WorldTimer::getMSTimeDiff(st.patrolWaitAt, waitNow) >= 1000;
            if (timeToCheck)
            {
                st.patrolWaitAt = waitNow ? waitNow : 1;
                Creature* patrol = map->GetCreature(st.patrolWaitGuid);
                float const waitRange = dw::Get("pull.encounter.range");
                bool const stillThere = patrol && patrol->IsAlive() && patrol->IsInWorld() &&
                    bot->GetDistance(patrol) <= waitRange;
                uint32 const maxWaitMs = uint32(dw::Get("pull.patrol.wait_max_sec") * 1000.0f);

                if (stillThere && WorldTimer::getMSTimeDiff(st.patrolWaitSince, waitNow) < maxWaitMs)
                {
                    if (!st.patrolWaitLogAt || WorldTimer::getMSTimeDiff(st.patrolWaitLogAt, waitNow) >= 5000)
                    {
                        st.patrolWaitLogAt = waitNow ? waitNow : 1;
                        sLog.outString("[DUNGEONROUTE] %s 30码内有可拉怪但会引到巡逻怪 %s ⇒ 原地等它走开（%us/%us）",
                            bot->GetName(), patrol->GetName(),
                            uint32(WorldTimer::getMSTimeDiff(st.patrolWaitSince, waitNow) / 1000),
                            uint32(maxWaitMs / 1000));
                    }
                }
                else
                {
                    if (!stillThere)
                        sLog.outString("[DUNGEONROUTE] %s 巡逻怪已走开 ⇒ 继续拉怪", bot->GetName());
                    else
                        sLog.outString("[DUNGEONROUTE] %s 等巡逻怪 %us 仍不安全 ⇒ 放弃这一只，继续推进",
                            bot->GetName(), uint32(maxWaitMs / 1000));
                    st.patrolWaitGuid = 0;
                    st.patrolWaitAt = 0;
                    st.patrolWaitSince = 0;
                    st.patrolWaitLogAt = 0;
                }
            }

            if (st.patrolWaitGuid)
            {
                // 原地待命：目标点 = 自己脚下（MoveAction/PullAction 都不会再往前走）
                out = Target();
                out.hasPullPoint = true;
                out.px = bot->GetPositionX();
                out.py = bot->GetPositionY();
                out.pz = bot->GetPositionZ();
                return true;
            }
        }

        // 锁定"当前推进波"：保证多波同时可见时先把这一波清掉
        if (nearestM.guid && nearestAliveOrd && st.lockOrd != nearestAliveOrd)
        {
            st.lockOrd = nearestAliveOrd;
            st.lockUntil = nowMs + LOCK_MS;
        }

        // [2026-10-19 站长口径] 动态巡逻波次：不绝对按波次表 —— 附近（40 码内可见）有巡逻怪时，
        //   若它比当前波目标更近（或当前波没目标）⇒ 先动态清它；清完再回波次推进。
        Member patrolM;
        float patrolDist = 1e18f;
        for (uint32 pg : route.patrolGuids)
        {
            Creature* pc = map->GetCreature(pg);
            if (!pc || !pc->IsAlive() || pc->IsInWater() || !SessionCanFight(bot, pc))
                continue;
            float const pd = bot->GetDistance(pc);
            if (pd > 40.0f || !IsVisible(bot, pc))
                continue;
            if (pd < patrolDist)
            {
                patrolDist = pd;
                patrolM.guid = pg;
                patrolM.entry = pc->GetEntry();
            }
        }
        if (patrolM.guid && (!nearestM.guid || patrolDist <= nearestAliveDist))
        {
            Creature* pc = map->GetCreature(patrolM.guid);
            out = Target();
            out.guid = patrolM.guid;
            out.entry = patrolM.entry;
            out.ord = 0;
            out.boss = false;
            out.dynamic = true;
            if (pc && patrolDist <= STOP_DIST)
            {
                out.px = bot->GetPositionX();
                out.py = bot->GetPositionY();
                out.pz = bot->GetPositionZ();
                out.hasPullPoint = true;
                return true;
            }
            if (pc)
                MakeApproachPoint(bot, pc->GetPositionX(), pc->GetPositionY(), pc->GetPositionZ(), out);
            return pc != nullptr;
        }

        // [2026-10-22 站长口径] 30 码内没有可拉的怪（也没巡逻怪）⇒ boss 优先模式下去找 boss。
        //   找不到可达 boss（都打完/都挂起）⇒ 继续往下走原来的波次/特殊寻路。
        if (bossMode && !nearestM.guid)
        {
            if (FindBossTarget(bot, route, st, out))
                return true;
            // [2026-10-22 站长口径] 所有 boss 都不可达 ⇒ 按 link 跳到下一个连通区间，跳完再重查 boss
            if (FindLinkExplorationTarget(bot, route, st, out))
                return true;
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

            // [2026-10-19 站长口径] 锚点真正接进推进：本波有锚点时，**先站到锚点再拉**。
            //   以前 `FindTarget()` 读锚点但没人调用，锚点表等于死数据；这里改由运行时生效：
            //     - 非战斗：离锚点 >3.5 码 ⇒ 拉怪位设为锚点，MoveAction 走过去；
            //       到锚点后拉怪位=当前位置，PullAction 开最近的一只。
            //     - 战斗中仍按“30 码视线内就地停”，避免坦边打边被拉回锚点。
            if (!bot->IsInCombat())
            {
                auto wit = route.waves.find(nearestAliveOrd);
                if (wit != route.waves.end() && wit->second.pull.has)
                {
                    float const ax = wit->second.pull.x;
                    float const ay = wit->second.pull.y;
                    float const az = wit->second.pull.z;
                    float const ad = bot->GetDistance(ax, ay, az);

                    out.hasPullPoint = true;
                    if (ad > AT_PULL_POINT_DIST)
                    {
                        out.px = ax;
                        out.py = ay;
                        out.pz = az;
                    }
                    else
                    {
                        out.px = bot->GetPositionX();
                        out.py = bot->GetPositionY();
                        out.pz = bot->GetPositionZ();
                    }
                    return true;
                }
            }

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

        // ② 没有活目标：朝最近的"锚点/波中心"走（mmap 联通的简单图直接走得过去）。
        //     [2026-10-19 站长口径] 有锚点优先走锚点：波还没加载时也要先站到拉怪位，
        //     而不是走到怪堆几何中心（那会直接踩进警戒圈）；没有锚点才退回波中心。
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
            if (!kv.second.hasCenter && !kv.second.pull.has)
                continue;

            float tx, ty, tz;
            if (kv.second.pull.has)
            {
                tx = kv.second.pull.x;
                ty = kv.second.pull.y;
                tz = kv.second.pull.z;
            }
            else
            {
                tx = kv.second.cx;
                ty = kv.second.cy;
                tz = kv.second.cz;
            }

            float const d = bot->GetDistance(tx, ty, tz);
            if (d < goalDist)
            {
                goalDist = d;
                goalOrd = waveOrd;
                goalX = tx;
                goalY = ty;
                goalZ = tz;
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
            if (st.bossCleared.count(b.guid) || st.bossSkipped.count(b.guid))
                continue;

            Creature* c = map->GetCreature(b.guid);
            if (c && !c->IsAlive())
            {
                st.bossCleared.insert(b.guid);
                continue;
            }
            // [boss-fix 2026-10-19] 不能再把“见过活的、现在没加载”当成已死：
            //   网格卸载时 GetCreature 也会返回 null，之前 Omor/Vazruden 就是这样被误判成已清。
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
        {
            // [2026-10-19 站长口径] 动态巡逻不属于固定波次：遇到就可以打，不按波次表卡死。
            if (route->patrolGuids.count(unit->GetGUIDLow()))
                return true;

            // [2026-10-21 站长口径] 已经在和队伍战斗 = 反击从进战斗开始算，不等它先打到我们身上。
            //   553 出现过：4 只非 route 怪把队伍拉进战斗，路线过滤却不让打 → 15s 无伤害 → 跳正常波。
            if (unit->IsInCombat() && bot->IsInCombat())
                return true;
            return false;
        }

        BotState& st = StateOf(bot);
        uint16 const waveOrd = it->second;
        // [2026-10-21] 已跳过的波：如果它的成员还在战斗/在打我们（常见于 evade 卡住的怪），
        //   必须允许反击；否则目标过滤全拒绝 ⇒ 坦克"在战斗但没目标" ⇒ sim 15s 后连环跳下一波。
        if (st.skip.count(waveOrd))
            return IsAttackingUs(bot, unit) || (unit->IsInCombat() && bot->IsInCombat());
        if (st.cleared.count(waveOrd))
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
