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
// [2026-10-25 站长口径] "一波"由刷怪系统自己分组（SpawnGroup / CreatureGroup），
//   不靠人工 guid 表、也不靠坐标聚类 ⇒ 这里要拿 Creature::GetCreatureGroup()->GetGroupId()。
#include "Maps/SpawnGroup.h"
#include <algorithm>
#include <mutex>       // [2026-10-25] 固化路线生成的跨线程互斥
#include <unordered_map>   // [DRIVEFAIL 2026-11-02] ShouldDrive 失败原因的节流表（guidLow -> 上次打印时刻）

namespace dungeonroute
{
    // 同一波多久没有任何"成员死亡"的推进就放弃（毫秒）
    const uint32 WAVE_GIVE_UP_MS = 90000;
    // [2026-10-18] "这一波没有可打目标"（友好 NPC / 小动物 / 全在远处没加载）时，
    //   等这么久就跳过——否则坦克会站在拉怪位一直不动。
    const uint32 WAVE_NOT_LOADED_MS = 15000;   // [2026-10-22 寻路修] 5s→15s：从"站到拉怪位"起算，给网格加载/怪走位留时间
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
                else if (info->UnitFlags & (UNIT_FLAG_UNTARGETABLE | UNIT_FLAG_UNINTERACTIBLE |
                                            UNIT_FLAG_NOT_ATTACKABLE_1))
                {
                    // [2026-10-27 站长口径] **不可选中/不可交互的单位不作为波成员**：
                    //   546 的 Large AOI Underbat(18409) = UNINTERACTIBLE|IMMUNE_TO_PLAYER，
                    //   之前被当成敌对怪并进波次 ⇒ 波永远清不掉。
                    //   ⚠ 不能用 IMMUNE_TO_PLAYER 单独判：555 的 Cabal Ritualist 等"阶段免疫"
                    //   会被误剔（实测 67 只），也不能按 faction 判（Wastewalker Slave 是脚本入战的）。
                    ++r->npcs;
                }
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
        sLog.outError("[DUNGEONROUTE] 全图怪物清单 map=%u：刷点 %u（敌对 %u / 小动物 %u / NPC %u / 无模板 %u）",
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
    // [2026-10-25 修复] 判"某个点安不安全"时，只看这一点周围多远之内的怪。
    //   原来这个 30 写死在 SafePointVisitor 里，而 UnsafeStandOffender 又按 **bot 的位置**搜 30 码，
    //   于是判 20 码外的候选点时 30~50 码那一段成了盲区 ⇒ 探出来的"安全点"其实踩在怪脸上。
    const float THREAT_SCREEN_RADIUS = 30.0f;
    // [2026-10-25] "遇到就清"的观察半径：一个刷怪网格格的尺度。
    //   比这更远的怪通常还没加载（走过去自然会刷出来）；墙后的本来就不该算"路上遇到"。
    const float LIVE_GROUP_SCAN_DIST = 60.0f;
    // [2026-10-25 站长口径] 现场普查半径：一次决策能"看见"多大一片已加载的怪。
    //   副本很小，这个半径基本覆盖当前房间到下一个房间；再远的等走过去自然加载。
    const float LIVE_CENSUS_DIST = 120.0f;
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
        // [2026-10-22 站长口径] "按同类怪数量动态调整权重"（表 ai_playerbot_mob_count_weight）：
        //   附近该 entry 的活怪 < engageCount ⇒ 无视它继续推进；>= engageCount ⇒ 正常清，
        //   且数量越多权重越高（等效距离 d/(1+weightPerCount*count)）。
        struct CountWeightRule { float radius = 25.0f; uint32 engageCount = 8; float weightPerCount = 0.15f; };
        std::map<uint32, CountWeightRule> countWeights;   // entry -> 规则
        // [2026-10-23 站长口径] "无敌 + 击飞"类怪不拉，按躲地板处理（表 ai_playerbot_avoid_mob）。
        //   这里只放数据；运行时由 route 动作把它们灌进 bot 的 "avoid creature list"，
        //   交给引擎自带的 avoid specific creatures 策略去 move away。
        std::set<uint32> avoidEntries;
        // [2026-10-28] 每个"要躲的怪"的自定义危险半径（码，0=沿用仇恨圈）：
        //   例 546 的 Underbog Mushroom(17990) 是 8 码 Spore Cloud（SpellRadius.dbc idx14），
        //   比默认仇恨圈小得多；用仇恨圈躲会过度走位、还躲不掉真正的 8 码爆炸。
        std::map<uint32, float> avoidRadius;
        // [2026-10-24 站长指令] 会消失/打不到的怪（例：552 Protean Horror）：不当目标，消失就换下一个
        std::set<uint32> skipEntries;

        // [2026-10-23 站长指令] boss 前置条件（表 ai_playerbot_boss_require）：
        //   例：555 Hellmaw 被放逐，直接选 boss 拉不了，必须先清掉周围 Cabal Ritualist。
        struct BossRequire
        {
            uint32 reqEntry = 0;
            float radius = 40.0f;
        };
        std::vector<std::pair<uint32, BossRequire>> bossRequires;   // (boss_entry, rule)

        // [2026-10-23 站长指令] 副本进度链（key-node 粒度）：
        //   kind = kill_entry（清指定 entry）/ boss（打指定 boss）/ room_clear（anchor 半径内清空）
        struct Stage
        {
            uint16 ord = 0;
            std::string kind;
            uint32 targetEntry = 0;
            float ax = 0.0f, ay = 0.0f;
            float radius = 0.0f;
            uint32 countLimit = 0;   // [2026-10-23] gauntlet：附近活怪超过这个数就先清，否则继续往前
            float keepOut = 0.0f;    // [2026-10-23] 该阶段别靠近 anchor 这么近（269 清野兽时不要触发麦迪文）
            // [2026-10-26 站长口径] kind='wait' 用：原地干等这么多毫秒再推进。
            //   用途：Arcatraz 开头清完 4 个 warder 后，等会自己消失的 Protean 走干净再往前走
            //   （以前用 skip_entry 按 entry 跳过，会把同 entry 的正常怪也跳过）。
            uint32 waitMs = 0;
            // [2026-10-26 站长指令] kind='link' 用：走到 (ax,ay) 附近的 route_link，然后跳到 (lx,ly,lz)。
            //   link 数据仍只存在 `ai_playerbot_route_link`（单一来源），加载时按 src 最近匹配填进来。
            float lx = 0.0f, ly = 0.0f, lz = 0.0f;
            uint8 lkind = 0;
        };
        std::vector<Stage> stages;   // 按 ord 升序
        // [2026-10-23] interact_go 用：GO entry -> 该图内的 guid 列表
        std::map<uint32, std::vector<uint32>> goGuids;
        // [2026-10-23] talk_npc 用：NPC entry -> 该图内的 creature guid 列表
        std::map<uint32, std::vector<uint32>> npcGuids;

        // [2026-10-23 站长指令] "先拉这一只、别引太多"：进战斗前优先把指定 entry 单独拉过来
        struct PullFirst
        {
            uint32 entry = 0;
            float maxDist = 30.0f;    // 超过这个距离不主动去拉
            uint32 maxAdds = 2;       // 目标周围（15 码）其它活怪超过这个数 ⇒ 先不拉，等/退
        };
        std::vector<PullFirst> pullFirst;
        // [2026-10-23 站长口径] "adds 用连锁范围和连锁组判断，不要硬性距离"：
        //   连锁范围 = creature_template.CallForHelp；连锁组 = spawn_group_spawn / creature_linking
        std::map<uint32, uint32> spawnGroupOf;   // guid -> spawn group Id
        std::map<uint32, uint32> linkMasterOf;   // guid -> master_guid (linked)

        // [2026-10-23 站长指令] 危险 AoE（燃烧新星这类）：怪在念指定法术时，立刻后退到半径外
        struct AvoidSpell
        {
            uint32 spellId = 0;
            float radius = 25.0f;
        };
        std::vector<AvoidSpell> avoidSpells;
        // [2026-10-24 站长指令] 552：怪物之间没打完之前不要开怪；也别一次拉很多
        struct PullWait
        {
            float radius = 40.0f;
            uint32 maxAssist = 1;
            bool waitEnemyFight = true;
        };
        std::vector<PullWait> pullWaits;
        // [2026-10-24 站长指令] 552 弓箭手：要**拉回很远**再打，否则它会一直往回跑、容易 add
        // [2026-10-24 站长口径] 仇恨规则：默认 rank=1 / pct=100（队员仇恨不得超过第一仇恨的 100%）；
        //   特殊 boss 需要"不超过第二/第三仇恨"时，往 ai_playerbot_threat_rule 里加行即可，不用改代码。
        struct ThreatRule
        {
            uint32 entry = 0;
            uint8 rank = 1;
            float pct = 100.0f;
        };
        std::vector<ThreatRule> threatRules;

        struct PullBack
        {
            uint32 entry = 0;
            float dist = 40.0f;   // 把战斗位置拉到离它这么远
        };
        std::vector<PullBack> pullBacks;
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
        // [2026-10-22 站长口径] boss 可达性：PathFinder(mmaps) 判定 + 位置卡住判定
        uint32 bossPathGuid = 0;
        uint32 bossPathCheckAt = 0;
        bool   bossPathOk = true;
        float  bossPosX = 0.0f;
        float  bossPosY = 0.0f;
        uint32 bossPosAt = 0;
        // [2026-10-22 站长口径] "30 码内可拉但会引到巡逻怪" ⇒ 原地等它走开
        //   patrolWaitGuid = 挡路的巡逻怪 guid；每秒重判一次，最多等 patrolWaitMaxMs
        uint32 patrolWaitGuid = 0;
        uint32 patrolWaitAt = 0;
        uint32 patrolWaitSince = 0;
        uint32 patrolWaitLogAt = 0;
        // [2026-10-22] 等满 30s 仍不安全的巡逻怪：短时间内不再挡路
        //   （否则会"等 30s → 放弃这一只 → 下一 tick 又选中它 → 再等 30s"死循环，555 实测卡满 900 秒）
        uint32 patrolIgnoreGuid = 0;
        uint32 patrolIgnoreUntil = 0;
        uint32 patrolWaitTargetGuid = 0;   // 因它而等的"想拉的那只怪"   // [2026-10-22] 已为"不可达 boss"试过的连接点下标（避免死循环）
        // [2026-10-25 站长口径] 开怪准入：等"代价最小的那一刻"再拉。
        //   存的是**等到什么时候**（不是节流）：到点前不再重算，避免每 tick 把等待时刻往后推。
        uint32 pullWaitUntil = 0;
        // [GHOST-FIGHT 2026-10-27] 幽灵战斗（顶着战斗标记但没有任何交战）的确认起点；0 = 不在该状态
        uint32 ghostCombatSince = 0;
        // [2026-10-28 站长口径] "全员一起行动"：非战斗时有人阵亡/掉队就停住**一直等**（不设超时）；
        //   regroupSince=开始等的时刻（0=不需要等）。
        uint32 regroupSince = 0;
        uint32 regroupLogAt = 0;
        uint32 regroupSummonAt = 0;   // [2026-10-28] summon 重试节流（30s）
        // [2026-10-28 站长口径] 掉队者详情（供 SimScenario 打 [REGROUP]，说明为什么追不上）
        ObjectGuid regroupStraggler;
        float regroupDist = 0.0f;
        bool regroupStragglerDead = false;
        // [2026-10-27 整合] [THREAT] hold fire 日志节流（原来每 tick 一条）
        uint32 holdFireLogAt = 0;
        // [2026-10-27 站长口径] 施法怪/够不着目标的挪位节流（冲上去 / 卡视野 / 后拉）
        uint32 casterPosAt = 0;
        uint32 pullCostAdds = 0;      // 上一次算出的代价（额外怪数），只用于日志/观察
        uint32 pullCostPatrol = 0;    // 其中巡逻怪数
        // [2026-10-25 站长口径] **聚怪门只检查开怪后 5 秒**：
        //   boss 会召唤小怪 / 有 add 时，"等怪全聚到 5 码"永远不成立 ⇒ DPS 永久停手，
        //   所以门只在开怪瞬间生效，超时 5 秒一律放行。
        uint32 gatherSince = 0;
        // [2026-10-25 性能] 现场普查的缓存：普查是**只读**的，500ms 内结果等价。
        //   目标还活着、没被拉黑、没在战斗 ⇒ 直接复用，不重扫网格（这不是行为节流，是读缓存）。
        uint32 liveCensusAt = 0;
        uint32 liveCensusGuid = 0;
        uint32 deadEndLogAt = 0;      // "拉怪位不可达 ⇒ 改朝怪本体走" 的日志节流
        // [2026-10-25 站长口径] "跑通一个阶段就把这段路存下来"：
        //   recStage = 正在记录的 stage；recPath = 这段路实际走过的路点；
        //   stage 推进（= 这一段跑通了）就把 recPath 存库（verified=1），以后懒加载也能直接沿它走。
        uint16 recStage = 0xFFFF;
        std::vector<Vector3> recPath;
        uint32 stagePathSeq = 0;      // 回放：已经走过这一段的第几个点（前向，不回退）
        std::set<uint16> seenAlive;   // 见过活怪的波（用来区分"已清"与"远处未加载"）
        std::set<uint16> cleared;     // 确认已清的波
        std::set<uint32> bossSeen;    // 见过活的 boss 刷点
        std::set<uint32> bossCleared; // 确认已死的 boss 刷点（只认尸体，跳过不算）
        std::set<uint32> bossSkipped; // 放弃的 boss 刷点（未加载/不可达）——绝不算已死
        std::set<uint32> switchUsed;  // 已点过的 boss 开关（goGuid）
        uint32 switchWaitAt = 0;      // 开关没加载时的等待起点
        uint16 lockOrd = 0;           // 当前锁定的推进波（0 = 未锁定）
        uint32 lockUntil = 0;         // 锁定到期时间（毫秒）
        // [2026-10-22 寻路修] "等网格加载"的计时起点：必须等**站到本波拉怪位之后**才开始计时。
        //   原来用 WaveStuckMs（从进入本波开始算）⇒ 走路本身就要几秒，一到点就误判"未加载"整波跳过。
        uint16 atPullOrd = 0;         // 正在哪个波/拉怪位上等（0 = boss 或未定）
        uint32 atPullSince = 0;       // 第一次站上该拉怪位的时刻
        // [2026-10-23 站长口径] 拉怪失败兜底：
        //   unattackable = 不可攻击/永远无敌的目标（召唤物、无敌机关）⇒ 拉黑，继续推进（"应该逃跑"）；
        //   approachGuid/At = "要贴近才能打"的目标 ⇒ 主动走近再试（2 秒节流）。
        std::set<uint32> unattackable;
        uint32 approachGuid = 0;
        uint32 approachAt = 0;
        // [2026-10-23] 不完整路径收束：路径检查节流（1.5s）+ 上次检查的目标
        uint32 pathCheckAt = 0;
        uint32 pathTargetGuid = 0;
        uint32 pullDbgAt = 0;      // [2026-10-23] PULLWHY 诊断节流
        uint32 bossRequireLogAt = 0;   // [2026-10-23] boss 前置未满足的日志节流
        uint16 stageOrd = 0;           // [2026-10-23] 进度链当前阶段（0=未初始化，>=9000=已完成）
        uint32 stageWaitSince = 0;     // [2026-10-26] kind='wait' 阶段的起始时间
        uint32 stageLogAt = 0;         // [2026-10-23] 进度链日志节流
        std::set<uint16> stageSeen;    // [2026-10-23] 该阶段的目标"见过活的"（防止网格没加载就误判已清）
        uint32 waveClearCount = 0;     // [2026-10-23] waves_clear：已清空次数
        uint32 waveClearSince = 0;     // [2026-10-23] waves_clear：当前"空场"从何时开始（3 秒确认）
        uint32 beaconAt = 0;           // [2026-10-23] event_guard：上次放时空道标的时间（60s 节流）
        uint32 moveTgtLogAt = 0;       // [2026-10-24] MOVETGT 诊断节流
        uint32 waveLogAt = 0;          // [2026-10-24] WAVE 诊断节流
        uint32 pullBackPending = 0;        // [2026-10-24 站长口径] 拉完/挪位后的新站位：生效期间不再回原拉怪位
        float standOverrideX = 0, standOverrideY = 0, standOverrideZ = 0;
        uint32 standOverrideUntil = 0;
        // [2026-10-25 修] PULLSAFE 挪出来的"安全拉怪点"就是**这一次拉怪的站位点**（不只是拉完之后的站位）。
        //   原因：挪到安全点后，PullAction 的 stand_override_hold 直接 return false 挡着不开怪，
        //   而 MoveAction/PullTrigger 还按**原拉怪位**算距离 ⇒ 把坦克推回原拉怪位，
        //   到了原拉怪位 PullAction 又把人送回安全点 ⇒ 两个动作互顶，一次开怪都不发生
        //   （r92 557 Pandemonius：坐标在拉怪位 0 码 ↔ 安全点 8 码之间来回，单组钉死 8 分钟）。
        bool safePullPoint = false;
        // [2026-10-25 只读诊断] 越界（站在导航网格外）日志节流：定位"谁在什么时刻被丢出有效区域"
        // [2026-10-26 站长口径] 波卡死判据的"正在走近"豁免：记离拉怪位的距离变化
        float  waveLastDist = -1.0f;    // 上一次采样时离当前拉怪位的距离
        uint32 waveLastDistAt = 0;
        uint32 waveApproachAt = 0;      // 最后一次"距离在缩短"的时间（20 秒内有效）

        uint32 oobLogAt = 0;
        // 连续几次(每 10s 一次)判"在网格外"才算越界：避开"刚传送到入口、网格还没加载"那几秒的误报
        uint8 oobStrikes = 0;
        // 记第一次判越界时的坐标：只有"基本没动"才继续累加 strikes（击飞兜底只救真卡住的人）
        float oobX = 0.0f, oobY = 0.0f, oobZ = 0.0f;
        // [2026-10-25 站长口径] 站位调整"找不到安全点"的日志节流（此时**不移动**，等巡逻怪走过来）
        uint32 reposFailAt = 0;
        // [2026-10-26 修] "等代价最小的开怪时刻"的起始时间：总等待上限 10s，防止巡逻外推每 2~8s 又给一个
        //   "更便宜的时刻" ⇒ 无限推迟、整组空转 18 分钟（r100：`waiting_best_window` + 1100s 未进战斗）。
        uint32 pullCostWaitSince = 0;
    // [2026-10-24] pulled from unsafe spot -> pullback after combat starts
        uint32 assistGuid = 0;          // [2026-10-24] 当前正在补拉的怪（拉到坦克身上就清空）
        uint32 threatHolds = 0;        // [2026-10-24] 起手门"压着不出手"次数
        uint32 threatLogAt = 0;        // [2026-10-24] 起手门日志节流
        uint32 directMoveGuid = 0;     // [2026-10-24] 最近一次直接 MovePoint 的目标
        uint32 directMoveAt = 0;       // [2026-10-24] 直接 MovePoint 节流（20s）
        float moveCheckX = 0, moveCheckY = 0;   // [2026-10-24] "原地不动"检测：上次移动过的位置
        uint32 moveCheckAt = 0;                 // 采样时间
        uint32 lastMovedAt = 0;                 // 上次确认"真的移动了"的时间
        // [2026-10-23] 目标级拉黑：同一只怪连续拉怪失败 N 次（打不了/不可交互/感知不到）⇒ 跳过，
        //   避免整队死磕一只怪（实测 558 的 Raging Soul 被重试 175 次，整张图 0 进度）。
        std::map<uint32, uint8> pullFailCount;
        // [2026-10-24 站长指令] 会消失的怪（Protean Horror 这类）：**按具体 guid 临时跳过**，
        //   不能按 entry 一刀切（同一个 entry 也有正常可打的）
        std::map<uint32, uint32> guidSkipUntil;
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

    // ---- 诊断日志的"最小键"（dtelemetry::Event 的 key 参数）------------------------
    // [2026-11-01 站长口径] 节流是**按 (tag,key)** 的，所以 key 必须能区分"谁在哪"，
    //   否则一个 bot 的噪声会把整张图别的 bot 的诊断压掉。用最小、最稳定的键：
    //     · "谁的决策"（PULLWHY/PULLSAFE/PULLCOST）⇒ bot guid low；
    //     · "在哪一波"（MOVETGT/PULLDIAG）        ⇒ (map<<20)|ord（ord 是 16 位、map 只有几千，不会溢出）。
    //   key=0 表示"全局键"，同一 tag 下所有 key=0 的调用共用一个窗口。
    //   这两个函数是**纯函数**，不读也不写任何状态；只影响日志口径，不影响行为。
    inline uint32 TeleKeyBot(Player const* bot)
    {
        return bot ? uint32(bot->GetGUIDLow()) : 0;
    }

    // ord=0（boss 阶段 / 还没定波）时退化成 bot 维度：否则所有 boss 阶段会共用一个 key
    inline uint32 TeleKeyWave(uint32 mapId, uint16 ord, Player const* bot)
    {
        if (!ord)
            return TeleKeyBot(bot);
        return (mapId << 20) | uint32(ord);
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
        // [2026-10-24 站长口径] mmap 确实不可达的怪：**从一开始就跳过**（不登记进波次/guidOrd）。
        //   数据来源：离线 navmesh flood（ai_playerbot_unreachable，按 guid）。
        std::set<uint32> unreachable;
        {
            std::unique_ptr<QueryResult> ur = WorldDatabase.PQuery(
                "SELECT `guid` FROM `ai_playerbot_unreachable` WHERE `map` = '%u'", mapId);
            if (ur)
            {
                do { unreachable.insert(ur->Fetch()[0].GetUInt32()); } while (ur->NextRow());
            }
        }
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
                if (unreachable.count(guid))
                    continue;   // [2026-10-24] 不可达的怪不参与推进

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

        // [2026-10-26 站长口径] `ai_playerbot_wave_anchor` 表**已废弃并删除**（只做统计用，运行时不再读）：
        //   拉怪位一律由 `MakeApproachPoint()` 实时计算；下表留空 ⇒ `Wave::pull.has=false`，
        //   所有用到 pull 点的地方都会走实时计算分支。
        //   （历史：这里原本 SELECT ord,pull_x,pull_y,pull_z 覆盖实时点。）

        // [2026-10-22 站长口径] 按同类怪数量动态调整权重（540：Creeping Oozeling / Shattered Hand Zealot）
        std::unique_ptr<QueryResult> cwrules = WorldDatabase.PQuery(
            "SELECT `entry`, `radius`, `engage_count`, `weight_per_count` FROM `ai_playerbot_mob_count_weight` "
            "WHERE `map` = '%u'", mapId);
        if (cwrules)
        {
            do
            {
                Field* f = cwrules->Fetch();
                MapRoute::CountWeightRule r;
                r.radius = f[1].GetFloat();
                r.engageCount = f[2].GetUInt32();
                r.weightPerCount = f[3].GetFloat();
                route.countWeights[f[0].GetUInt32()] = r;
            } while (cwrules->NextRow());
        }


        // [2026-10-23] 无敌+击飞类怪：加载"要躲"的 entry 列表（[2026-10-28] 带自定义危险半径）
        std::unique_ptr<QueryResult> avoidRows = WorldDatabase.PQuery(
            "SELECT `entry`, `radius` FROM `ai_playerbot_avoid_mob` WHERE `map` = '%u'", mapId);
        if (avoidRows)
        {
            do
            {
                Field* f = avoidRows->Fetch();
                uint32 const entry = f[0].GetUInt32();
                float const radius = f[1].GetFloat();
                route.avoidEntries.insert(entry);
                if (radius > 0.0f)
                    route.avoidRadius[entry] = radius;
            } while (avoidRows->NextRow());
        }

        // [2026-10-26 站长指令] **禁用 skip_entry**：
        //   会消失的临时召唤怪（例：Arcatraz 的 Protean Nightmare/Horror/Spawn）与要正常清的怪
        //   共用同一个 entry，按 entry 跳过 ⇒ 该打的怪也被跳过（实测 552 一整段都不打）。
        //   表保留（ai_playerbot_skip_entry），这里不再加载 ⇒ skipEntries 恒为空集，行为回到"全都按正常波次处理"。
        //   等"会消失的怪"处理逻辑改成按**运行时状态**（目标消失/自动 despawn）判定后再启用。
        /*
        {
            std::unique_ptr<QueryResult> skRows = WorldDatabase.PQuery(
                "SELECT `entry` FROM `ai_playerbot_skip_entry` WHERE `map` = '%u'", mapId);
            if (skRows)
            {
                do { route.skipEntries.insert(skRows->Fetch()[0].GetUInt32()); } while (skRows->NextRow());
            }
        }
        */

        // [2026-10-23 站长指令] boss 前置条件：某些 boss 必须先清掉指定怪才能拉
        std::unique_ptr<QueryResult> reqRows = WorldDatabase.PQuery(
            "SELECT `boss_entry`, `req_entry`, `radius` FROM `ai_playerbot_boss_require` WHERE `map` = '%u'", mapId);
        if (reqRows)
        {
            do
            {
                Field* f = reqRows->Fetch();
                MapRoute::BossRequire r;
                r.reqEntry = f[1].GetUInt32();
                r.radius = f[2].GetFloat();
                route.bossRequires.emplace_back(f[0].GetUInt32(), r);
            } while (reqRows->NextRow());
        }

        // [2026-10-23 站长指令] 副本进度链
        std::unique_ptr<QueryResult> stageRows = WorldDatabase.PQuery(
            "SELECT `stage_ord`, `kind`, `target_entry`, `anchor_x`, `anchor_y`, `radius`, `count_limit`, `keep_out`, `wait_ms` "
            "FROM `ai_playerbot_dungeon_stage` WHERE `map` = '%u' ORDER BY `stage_ord`", mapId);
        if (stageRows)
        {
            do
            {
                Field* f = stageRows->Fetch();
                MapRoute::Stage s;
                s.ord = f[0].GetUInt16();
                s.kind = f[1].GetString();
                s.targetEntry = f[2].GetUInt32();
                s.ax = f[3].GetFloat();
                s.ay = f[4].GetFloat();
                s.radius = f[5].GetFloat();
                s.countLimit = f[6].GetUInt32();
                s.keepOut = f[7].GetFloat();
                s.waitMs = f[8].GetUInt32();
                route.stages.push_back(s);
            } while (stageRows->NextRow());
        }

        // [2026-10-19] 跳点/升降台连接点：**必须在 stage 匹配之前加载**——
        //   下面 kind='link' 的阶段要按 src 最近匹配 route.links；之前这一段放在后面，
        //   匹配时 links 还是空的 ⇒ 所有 link 阶段都被 "15 码没有 route_link" 跳过（547 实测）。
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

        // [2026-10-26 站长指令] kind='link' 的阶段：按 src(anchor_x/anchor_y) 最近的 route_link 填 dst。
        //   link 数据只存 ai_playerbot_route_link，避免两份坐标漂移。
        for (auto& s : route.stages)
        {
            if (s.kind != "link")
                continue;
            float bestD = 1e18f;
            RouteLink const* best = nullptr;
            for (auto const& l : route.links)
            {
                float const dx = l.sx - s.ax, dy = l.sy - s.ay;
                float const d = std::sqrt(dx * dx + dy * dy);
                if (d < bestD)
                {
                    bestD = d;
                    best = &l;
                }
            }
            if (best && bestD <= 15.0f)
            {
                s.lx = best->dx; s.ly = best->dy; s.lz = best->dz;
                s.lkind = best->kind;
            }
            else
            {
                sLog.outError("[STAGE] map %u stage %u kind=link 但 (%.1f,%.1f) 附近 15 码没有 route_link ⇒ 这条阶段会被跳过",
                    mapId, uint32(s.ord), s.ax, s.ay);
            }
        }

        // [2026-10-23] interact_go：把阶段里用到的 GO 在该图的 guid 都载入
        std::string goList;
        for (auto const& s : route.stages)
        {
            if (s.kind != "interact_go" || !s.targetEntry)
                continue;
            if (!goList.empty()) goList += ",";
            goList += std::to_string(s.targetEntry);
        }
        if (!goList.empty())
        {
            std::unique_ptr<QueryResult> goRows = WorldDatabase.PQuery(
                "SELECT `id`, `guid` FROM `gameobject` WHERE `map` = '%u' AND `id` IN (%s)",
                mapId, goList.c_str());
            if (goRows)
            {
                do
                {
                    Field* f = goRows->Fetch();
                    route.goGuids[f[0].GetUInt32()].push_back(f[1].GetUInt32());
                } while (goRows->NextRow());
            }
        }

        // [2026-10-23] talk_npc：把阶段里要对话的 NPC guid 载入
        std::string npcList;
        for (auto const& s : route.stages)
        {
            if (s.kind != "talk_npc" || !s.targetEntry)
                continue;
            if (!npcList.empty()) npcList += ",";
            npcList += std::to_string(s.targetEntry);
        }
        if (!npcList.empty())
        {
            std::unique_ptr<QueryResult> npcRows = WorldDatabase.PQuery(
                "SELECT `id`, `guid` FROM `creature` WHERE `map` = '%u' AND `id` IN (%s)",
                mapId, npcList.c_str());
            if (npcRows)
            {
                do
                {
                    Field* f = npcRows->Fetch();
                    route.npcGuids[f[0].GetUInt32()].push_back(f[1].GetUInt32());
                } while (npcRows->NextRow());
            }
        }

        // [2026-10-23 站长指令] "先拉这一只、别引太多"
        std::unique_ptr<QueryResult> pfRows = WorldDatabase.PQuery(
            "SELECT `entry`, `max_dist`, `max_adds` FROM `ai_playerbot_pull_first` WHERE `map` = '%u'", mapId);
        if (pfRows)
        {
            do
            {
                Field* f = pfRows->Fetch();
                MapRoute::PullFirst p;
                p.entry = f[0].GetUInt32();
                p.maxDist = f[1].GetFloat();
                p.maxAdds = f[2].GetUInt32();
                route.pullFirst.push_back(p);
            } while (pfRows->NextRow());
        }

        // [2026-10-23] 连锁组数据：spawn_group + creature_linking（只载本图的怪）
        if (!route.pullFirst.empty())
        {
            std::unique_ptr<QueryResult> sgRows = WorldDatabase.PQuery(
                "SELECT s.`Guid`, s.`Id` FROM `spawn_group_spawn` s "
                "JOIN `creature` c ON c.guid = s.Guid WHERE c.map = '%u'", mapId);
            if (sgRows)
            {
                do
                {
                    Field* f = sgRows->Fetch();
                    route.spawnGroupOf[f[0].GetUInt32()] = f[1].GetUInt32();
                } while (sgRows->NextRow());
            }

            std::unique_ptr<QueryResult> lkRows = WorldDatabase.PQuery(
                "SELECT l.`guid`, l.`master_guid` FROM `creature_linking` l "
                "JOIN `creature` c ON c.guid = l.guid WHERE c.map = '%u'", mapId);
            if (lkRows)
            {
                do
                {
                    Field* f = lkRows->Fetch();
                    route.linkMasterOf[f[0].GetUInt32()] = f[1].GetUInt32();
                } while (lkRows->NextRow());
            }
        }

        // [2026-10-23 站长指令] 危险法术列表（念到就跑）
        {
            std::unique_ptr<QueryResult> asRows = WorldDatabase.PQuery(
                "SELECT `spell_id`, `radius` FROM `ai_playerbot_avoid_spell` WHERE `map` = '%u'", mapId);
            if (asRows)
            {
                do
                {
                    Field* f = asRows->Fetch();
                    MapRoute::AvoidSpell s;
                    s.spellId = f[0].GetUInt32();
                    s.radius = f[1].GetFloat();
                    route.avoidSpells.push_back(s);
                } while (asRows->NextRow());
            }
        }

        // [2026-10-24 站长指令] 等怪之间打完再开怪 / 不要一次拉很多
        {
            std::unique_ptr<QueryResult> pwRows = WorldDatabase.PQuery(
                "SELECT `radius`,`max_assist`,`wait_enemy_fight` FROM `ai_playerbot_pull_wait` WHERE `map` = '%u'", mapId);
            if (pwRows)
            {
                do
                {
                    Field* f = pwRows->Fetch();
                    MapRoute::PullWait w;
                    w.radius = f[0].GetFloat();
                    w.maxAssist = f[1].GetUInt32();
                    w.waitEnemyFight = f[2].GetUInt32() != 0;
                    route.pullWaits.push_back(w);
                } while (pwRows->NextRow());
            }
        }

        // [2026-10-24] 弓箭手类：拉回很远再打
        {
            std::unique_ptr<QueryResult> pbRows = WorldDatabase.PQuery(
                "SELECT `entry`, `dist` FROM `ai_playerbot_pullback` WHERE `map` = '%u'", mapId);
            if (pbRows)
            {
                do
                {
                    Field* f = pbRows->Fetch();
                    MapRoute::PullBack b;
                    b.entry = f[0].GetUInt32();
                    b.dist = f[1].GetFloat();
                    route.pullBacks.push_back(b);
                } while (pbRows->NextRow());
            }
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

        // [2026-10-19] 跳点 / 升降台连接点：已提前到 stage 匹配之前加载（见上），这里不再重复查询。

        // [2026-10-19] boss 战后开关（门禁面板 / 桥控制台）：挂在 boss_entry 上
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
            // [2026-10-27 站长口径] **非敌对 NPC / 小动物不是"波成员"**：
            //   例 546 的 Underbat（faction 1712，不可选中/无敌）被并进波里 ⇒ 波永远清不掉、
            //   拉怪还把它们算进去。加载时直接剔除（数据缺口/动态刷怪保留）。
            std::vector<Member> keep;
            keep.reserve(kv.second.members.size());
            for (auto& m : kv.second.members)
            {
                ++totalMembers;
                auto rit = roster.byGuid.find(m.guid);
                if (rit == roster.byGuid.end())
                {
                    ++missingMembers;
                    keep.push_back(m);
                    continue;
                }
                ++known;
                bool const critter = rit->second.info &&
                    rit->second.info->CreatureType == CREATURE_TYPE_CRITTER;
                if (critter)
                    ++critterMembers;
                else if (!rit->second.hostile)
                    ++npcMembers;
                else
                    ++hostile;
                if (critter || !rit->second.hostile)
                    continue;                       // 不可攻击：不阻塞波次、不参与拉怪
                keep.push_back(m);
            }
            kv.second.members.swap(keep);
            // 整波都在清单里且一个敌对都没有 ⇒ 纯 NPC/小动物波，开局就跳过（不用走过去等 5 秒）
            if (known > 0 && hostile == 0)
            {
                kv.second.allNonHostile = true;
                ++trivialWaves;
            }
        }

        sLog.outError("[DUNGEONROUTE] map %u 已加载推进数据：%u 波 / %u 只 / boss 刷点 %u 个",
            mapId, uint32(route.waves.size()), uint32(route.guidOrd.size()), uint32(route.bosses.size()));
        sLog.outError("[DUNGEONROUTE] map %u 波成员 %u：数据缺口(动态刷怪) %u / 小动物 %u / 非敌对NPC %u / 纯NPC波 %u",
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

    // [2026-10-24 站长口径] "不能攻击的目标不该算一波怪" —— 假人（Arcane Warder Target）、
    //   友好 NPC、小动物、飞行/无敌脚本单位都不该阻塞波次推进。
    //   WaveHasAlive 版本没有 bot 指针，这里用"没有可攻击目标"的通用判据（CanAttack 需要 bot）。
    inline bool MemberFightable(Map* map, Member const& m)
    {
        Creature* c = MemberCreature(map, m);
        if (!c)
            return false;
        if (CreatureInfo const* info = c->GetCreatureInfo())
        {
            if (info->CreatureType == CREATURE_TYPE_CRITTER)
                return false;
            if (info->NpcFlags != 0)                 // 商人/任务/对话类 NPC
                return false;
        }
        return true;
    }

    inline bool WaveHasFightable(Map* botMap, Wave const& w)
    {
        for (auto& m : w.members)
            if (MemberFightable(botMap, m))
                return true;
        return false;
    }

    // [2026-10-24 站长口径] 有 bot 指针时用**核心的 CanAttack** 判据（覆盖假人/无敌/脚本单位），
    //   这样"不能攻击的目标"一律不算一波怪。
    // [2026-10-24 站长口径] 判断"这一只是不是本图 boss（路线表 boss_spawn 里的）"
    // [2026-10-25] 威胁圈半径 = "离它多近会被它 aggro"的纯几何量：
    //   max(它对目标的攻击距离, 全局 aggroDistance)；巡逻怪会移动 ⇒ 多算 5 码。
    //   抽出来是为了让"判一个点安不安全"和"要退到多远"用**同一个数**，不再各写一份。
    inline float ThreatRingRadius(Player* bot, Creature* c)
    {
        if (!bot || !c)
            return 0.0f;
        // [2026-10-28 站长口径] 表里给了自定义半径就用它（例：蘑菇 Spore Cloud 8 码），
        //   不再用默认仇恨圈（20+ 码）去躲 8 码的地板技。
        MapRoute const* rt = GetMapRoute(c->GetMapId());
        if (rt)
        {
            auto it = rt->avoidRadius.find(c->GetEntry());
            if (it != rt->avoidRadius.end() && it->second > 0.0f)
                return it->second;
        }
        float aggro = std::max(c->GetAttackDistance(bot), sPlayerbotAIConfig.aggroDistance);
        if (c->GetMotionMaster())
        {
            uint8 const mt = c->GetMotionMaster()->GetCurrentMovementGeneratorType();
            if (mt == WAYPOINT_MOTION_TYPE || mt == RANDOM_MOTION_TYPE)
                aggro += 5.0f;
        }
        return aggro;
    }

    // [2026-10-24 站长口径] "不踩仇恨圈"：候选点 30 码内、LOS 通过、**还没进战斗**的敌对怪，
    //   若候选点落在 max(GetAttackDistance, AggroDistance)+margin 之内 ⇒ 不安全（会引到不该引的怪）。
    struct SafePointVisitor
    {
        Player* bot;
        float px, py, pz;
        float margin;
        // [2026-10-25 崩溃修] 只记 guid，不跨"遍历结束"持有 Creature*：
        //   实测 core 崩在 `DungeonTankRepositionTrigger::IsActive` → `Object::GetUInt32Value`，
        //   就是遍历结束后继续用 visitor 里那只怪（期间它可能被刷怪系统移除/重刷）。
        //   一律用 guid 回查 Map，拿到的才保证是活指针。
        ObjectGuid offenderGuid;

        // [2026-10-26 修] 本次**要拉的这一波**要排除：拉怪位本来就该落在目标波仇恨圈边上，
        //   不排除的话它自己永远被判"不安全"（实测 offender 就是待拉的怪/boss）。
        std::set<uint32> const* excludeLow;

        SafePointVisitor(Player* b, float x, float y, float z, float m, std::set<uint32> const* ex = nullptr)
            : bot(b), px(x), py(y), pz(z), margin(m), excludeLow(ex) {}

        bool operator()(Creature* c)
        {
            if (offenderGuid || !c || !c->IsAlive() || !c->IsInWorld())
                return true;
            if (c->IsInCombat())
                return true;
            if (!bot->CanAttack(c))
                return true;
            // [2026-10-24 站长口径] 小动物/等级差太大不会主动 aggro ⇒ 不算威胁（否则坦克为蟑螂乱挪位）
            if (c->IsCritter())
                return true;
            if (c->GetLevel() + 10 <= bot->GetLevel())
                return true;
            if (excludeLow && excludeLow->count(c->GetGUIDLow()))
                return true;                       // 待拉的这一波：它本来就在圈里
            if (!c->IsWithinDist3d(px, py, pz, THREAT_SCREEN_RADIUS))
                return true;
            if (!c->IsWithinLOS(px, py, pz))
                return true;
            if (c->IsWithinDist3d(px, py, pz, ThreatRingRadius(bot, c) + margin))
                offenderGuid = c->GetObjectGuid();
            return true;
        }

        // Cell::VisitAllObjects 需要 GridRefManager 的 Visit 重载（照 GridNotifiers 的写法）
        template<class T>
        void Visit(GridRefManager<T>&) {}
        void Visit(CreatureMapType& m)
        {
            for (CreatureMapType::iterator itr = m.begin(); itr != m.end(); ++itr)
            {
                Creature* c = itr->getSource();
                if (c)
                    (*this)(c);
            }
        }
    };

    inline Creature* UnsafeStandOffender(Player* bot, float x, float y, float z, float margin = 5.0f,
                                         std::set<uint32> const* excludeLow = nullptr)
    {
        if (!bot || !bot->GetMap())
            return nullptr;
        SafePointVisitor visitor(bot, x, y, z, margin, excludeLow);
        // [2026-10-25 修复] 原来固定按 **bot 的位置**搜 30 码，但被评估的点最远可以离 bot 30 多码，
        //   要判这个点安不安全得看它自己周围 30 码 —— 那最远到 bot 外 60 码。
        //   照 bot 的位置搜 ⇒ 30~60 码那一段是盲区，探出的"安全点"可能正踩在没被搜到的怪身上，
        //   挪过去立刻又被判不安全（REPOS 每 tick 触发的元凶之一）。
        //   以"被评估点到 bot 的距离 + 筛查半径"为搜索半径，才算覆盖被评估点完整一圈。
        Cell::VisitAllObjects(bot, visitor, bot->GetDistance(x, y, z) + THREAT_SCREEN_RADIUS);
        return visitor.offenderGuid ? bot->GetMap()->GetCreature(visitor.offenderGuid) : nullptr;
    }

    // [2026-10-25 站长口径] **安全分**（取代"是/否"）：
    //   权重：普通怪 0.2 / 精英 1 / boss 3；被**我们要打的那一波**覆盖不算危险（exclude）。
    //   返回分值：< 0.2 安全（可停留/开拉）；0.2~0.8 只能路过；>= 0.8 危险。
    const float SAFE_SCORE_OK = 0.5f;        // < 0.5 安全（可停留/开拉）
    const float SAFE_SCORE_TRANSIT = 2.5f;   // 0.5~2.5 只能路过；>= 2.5 危险
    const float SAFETY_W_TRASH = 0.2f;
    const float SAFETY_W_ELITE = 1.0f;
    const float SAFETY_W_BOSS = 3.0f;

    // 前置声明（定义在本文件后面，安全分里要用它判 boss 权重）
    inline bool IsRouteBoss(uint32 mapId, uint32 guidLow);

    struct SafetyScoreVisitor
    {
        Player* bot;
        float px, py, pz, margin;
        std::set<ObjectGuid> const* exclude;
        float score;        // 静止怪的分（从严）
        float patrolScore;  // **确实在巡逻**的怪的分（可以放宽：它会走开）

        SafetyScoreVisitor(Player* b, float x, float y, float z, float m, std::set<ObjectGuid> const* ex)
            : bot(b), px(x), py(y), pz(z), margin(m), exclude(ex), score(0.0f), patrolScore(0.0f) {}

        // TypeContainerVisitor 协议：其它类型直接忽略，只处理 Creature
        template<class T> void Visit(GridRefManager<T>&) {}
        void Visit(Creature& c) { operator()(&c); }

        bool operator()(Creature* c)
        {
            if (!c || !c->IsAlive() || !c->IsInWorld())
                return true;
            if (c->IsInCombat())                       // 已经在打的，归战斗逻辑管
                return true;
            if (exclude && exclude->count(c->GetObjectGuid()))   // 正是我们要拉的那一波
                return true;
            if (!bot->CanAttack(c) || c->IsCritter())
                return true;
            if (c->GetLevel() + 10 <= bot->GetLevel())
                return true;

            float const R = ThreatRingRadius(bot, c) + margin;
            if (R <= 0.1f)
                return true;
            float const d = std::sqrt((c->GetPositionX() - px) * (c->GetPositionX() - px) +
                                      (c->GetPositionY() - py) * (c->GetPositionY() - py));
            if (d >= R)
                return true;
            if (!c->IsWithinLOS(px, py, pz))
                return true;

            float w = SAFETY_W_TRASH;
            CreatureInfo const* ci = c->GetCreatureInfo();
            if (ci && ci->Rank >= CREATURE_ELITE_NORMAL)
                w = SAFETY_W_ELITE;
            if (IsRouteBoss(bot->GetMapId(), c->GetGUIDLow()))
                w = SAFETY_W_BOSS;
            float const add = w * (R - d) / R;
            // [2026-10-25 站长口径] **只有确实在巡逻的怪**才允许放宽（它会走开）；
            //   站桩的怪一律从严，能不引就不引。
            uint8 const dmt = c->GetDefaultMovementType();
            bool const patrolling = (dmt == WAYPOINT_MOTION_TYPE || dmt == RANDOM_MOTION_TYPE);
            if (patrolling)
                patrolScore += add;
            else
                score += add;
            return true;
        }
    };

    inline float SafetyScore(Player* bot, float x, float y, float z,
                             std::set<ObjectGuid> const* exclude = nullptr, float margin = 3.0f,
                             float* outPatrol = nullptr)
    {
        if (!bot || !bot->GetMap())
            return 0.0f;
        SafetyScoreVisitor v(bot, x, y, z, margin, exclude);
        Cell::VisitAllObjects(bot, v, bot->GetDistance(x, y, z) + THREAT_SCREEN_RADIUS);
        if (outPatrol)
            *outPatrol = v.patrolScore;
        return v.score;
    }

    // [2026-10-25 站长口径] **坦克的"集火目标"**：坦克对它堆高仇恨，全队打它，杀一个减一个。
    //   选法：priority 5（Legionnaire）优先 > 血量最少的精英 > 最近的。
    inline Creature* FindTankFocusTarget(Player* bot)
    {
        if (!bot || !bot->GetMap())
            return nullptr;
        Group* grp = bot->GetGroup();
        if (!grp)
            return nullptr;

        Creature* best = nullptr;
        float bestScore = -1.0f;
        std::set<ObjectGuid> seen;
        for (GroupReference* ref = grp->GetFirstMember(); ref; ref = ref->next())
        {
            Player* m = ref->getSource();
            if (!m || !m->IsInWorld())
                continue;
            for (Unit* a : m->getAttackers())
            {
                if (!a || !a->IsAlive() || a->GetTypeId() != TYPEID_UNIT || a->GetMapId() != bot->GetMapId())
                    continue;
                if (!seen.insert(a->GetObjectGuid()).second)
                    continue;
                Creature* c = static_cast<Creature*>(a);
                if (c->IsCritter() || !bot->CanAttack(c))
                    continue;
                bool const prio5 = (PossibleAttackTargetsValue::ThreatTier(c, bot) >= 5);
                float const hpPct = c->GetMaxHealth() ? float(c->GetHealth()) * 100.0f / float(c->GetMaxHealth()) : 100.0f;
                // 分数：priority5 压倒一切（+1000），其次血量越低越优先（0~100）
                float const score = (prio5 ? 1000.0f : 0.0f) + (100.0f - hpPct);
                if (score > bestScore)
                {
                    bestScore = score;
                    best = c;
                }
            }
        }
        return best;
    }

    // [2026-10-25 站长口径] 其它怪的"基础仇恨"：**治疗仇恨接近时就补**（不等超过）。
    //   阈值与门禁同一个百分比（默认 90%）。返回"需要坦克补仇恨"的那只怪。
    inline Creature* FindHealerThreatPressure(Player* bot)
    {
        if (!bot || !bot->GetMap())
            return nullptr;
        Group* grp = bot->GetGroup();
        if (!grp)
            return nullptr;
        Player* healer = nullptr;
        for (GroupReference* ref = grp->GetFirstMember(); ref; ref = ref->next())
        {
            Player* m = ref->getSource();
            if (m && m != bot && m->IsInWorld() && m->IsAlive() && PlayerbotAI::IsHeal(m))
            {
                healer = m;
                break;
            }
        }
        if (!healer)
            return nullptr;

        uint32 const pct = sPlayerbotAIConfig.waitForAttackThreatPercent
            ? sPlayerbotAIConfig.waitForAttackThreatPercent : 90;

        Creature* worst = nullptr;
        float worstRatio = 0.0f;
        std::set<ObjectGuid> seen;
        for (GroupReference* ref = grp->GetFirstMember(); ref; ref = ref->next())
        {
            Player* m = ref->getSource();
            if (!m || !m->IsInWorld())
                continue;
            for (Unit* a : m->getAttackers())
            {
                if (!a || !a->IsAlive() || a->GetTypeId() != TYPEID_UNIT || a->GetMapId() != bot->GetMapId())
                    continue;
                if (!seen.insert(a->GetObjectGuid()).second)
                    continue;
                Creature* c = static_cast<Creature*>(a);
                if (c->IsCritter() || !bot->CanAttack(c))
                    continue;
                float const tankT = c->getThreatManager().getThreat(bot);
                float const healT = c->getThreatManager().getThreat(healer);
                // 治疗仇恨已经到坦克的 pct% ⇒ 该补了
                if (healT * 100.0f >= tankT * float(pct))
                {
                    float const ratio = healT / (tankT > 0.01f ? tankT : 0.01f);
                    if (ratio > worstRatio)
                    {
                        worstRatio = ratio;
                        worst = c;
                    }
                }
            }
        }
        return worst;
    }

    // 前置声明（定义在本文件后面）
    inline Player* GroupTank(Player* bot);

    // [2026-10-25 站长口径] **DPS 按"仇恨余量"选目标**：
    //   对战斗中的每只怪算 room = 坦克仇恨×100/pct − 自己仇恨，挑 room 最大的那只打。
    //   一只都没有（全被自己顶满）⇒ 返回 nullptr，由停手门停手。
    inline Creature* FindThreatHeadroomTarget(Player* bot, float* outRoom = nullptr, float* outBestTankT = nullptr)
    {
        if (!bot || !bot->GetMap())
            return nullptr;
        Group* grp = bot->GetGroup();
        if (!grp)
            return nullptr;
        Player* tank = GroupTank(bot);
        if (!tank)
            return nullptr;

        uint32 const pct = sPlayerbotAIConfig.waitForAttackThreatPercent
            ? sPlayerbotAIConfig.waitForAttackThreatPercent : 90;

        Creature* best = nullptr;
        float bestRoom = 0.0f;
        float bestTankT = 0.0f;
        std::set<ObjectGuid> seen;
        for (GroupReference* ref = grp->GetFirstMember(); ref; ref = ref->next())
        {
            Player* m = ref->getSource();
            if (!m || !m->IsInWorld())
                continue;
            for (Unit* a : m->getAttackers())
            {
                if (!a || !a->IsAlive() || a->GetTypeId() != TYPEID_UNIT || a->GetMapId() != bot->GetMapId())
                    continue;
                if (!seen.insert(a->GetObjectGuid()).second)
                    continue;
                Creature* c = static_cast<Creature*>(a);
                if (c->IsCritter() || !bot->CanAttack(c))
                    continue;
                float const tankT = c->getThreatManager().getThreat(tank);
                float const myT = c->getThreatManager().getThreat(bot);
                float const room = tankT * 100.0f / float(pct) - myT;
                if (room > bestRoom)
                {
                    bestRoom = room;
                    bestTankT = tankT;
                    best = c;
                }
            }
        }
        if (outRoom)
            *outRoom = bestRoom;
        if (outBestTankT)
            *outBestTankT = bestTankT;
        return best;
    }

    // [2026-10-25 站长口径] 这个点能不能**停留/开拉**：
    //   静止怪的分必须 < 0.5（从严，能不引就不引）；
    //   巡逻怪的分只要 < 2.5（它确实会走开）。
    inline bool IsSafeStandPoint(Player* bot, float x, float y, float z,
                                 std::set<ObjectGuid> const* exclude = nullptr, float* outScore = nullptr)
    {
        float patrol = 0.0f;
        float const stat = SafetyScore(bot, x, y, z, exclude, 3.0f, &patrol);
        if (outScore)
            *outScore = stat + patrol;
        return stat < SAFE_SCORE_OK && patrol < SAFE_SCORE_TRANSIT;
    }

    // [2026-10-25 站长口径] "一波"不该由我聚类、也不是人工编号 —— 刷怪系统本来就分了组：
    //   同一个 CreatureGroup（日志里的 "... Group 001"）就是一波。
    //   没有组的落单怪按 entry 归，避免同一批同种怪被拆成一堆独立目标。
    //   为什么不再用 guid 表：副本刷点每次进本都随机（SpawnGroup::Update 里 shuffle / chance /
    //   m_chosenEntries），"这个 guid 是哪只怪"本身就不稳定。
    inline uint32 PullGroupKey(Creature* c)
    {
        if (!c)
            return 0;
        if (CreatureGroup* g = c->GetCreatureGroup())
            return g->GetGroupId();
        return 0x80000000u | (c->GetEntry() & 0x7FFFFFFFu);
    }

    // [2026-10-25 崩溃修] 同样只收 guid：遍历结束后一律用 Map 回查，不跨遍历持有 Creature*。
    struct LiveHostileVisitor
    {
        Player* bot;
        std::vector<ObjectGuid> found;

        explicit LiveHostileVisitor(Player* b) : bot(b) {}

        bool operator()(Creature* c)
        {
            if (!c || !c->IsAlive() || !c->IsInWorld())
                return true;
            if (c->IsInCombat())                       // 已经打起来的归战斗逻辑管
                return true;
            if (!SessionCanFight(bot, c))
                return true;
            if (c->IsCritter())
                return true;
            if (c->GetLevel() + 10 <= bot->GetLevel())  // 不会主动 aggro 的低级怪不算
                return true;
            if (!bot->GetMap()->IsInLineOfSight(bot->GetPositionX(), bot->GetPositionY(),
                    bot->GetPositionZ() + 1.5f, c->GetPositionX(), c->GetPositionY(),
                    c->GetPositionZ() + 1.0f, false))
                return true;                            // 墙后的不算"路上遇到"
            found.push_back(c->GetObjectGuid());
            return true;
        }

        template<class T>
        void Visit(GridRefManager<T>&) {}
        void Visit(CreatureMapType& m)
        {
            for (CreatureMapType::iterator itr = m.begin(); itr != m.end(); ++itr)
            {
                Creature* c = itr->getSource();
                if (c)
                    (*this)(c);
            }
        }
    };

    inline Creature* NearestLiveHostile(Player* bot, float radius = LIVE_GROUP_SCAN_DIST)
    {
        if (!bot || !bot->GetMap())
            return nullptr;
        LiveHostileVisitor visitor(bot);
        Cell::VisitAllObjects(bot, visitor, radius);
        Creature* best = nullptr;
        float bestD = 1e18f;
        for (ObjectGuid const& g : visitor.found)
        {
            Creature* c = bot->GetMap()->GetCreature(g);
            if (!c || !c->IsAlive())
                continue;
            float const d = bot->GetDistance(c);
            if (d < bestD)
            {
                bestD = d;
                best = c;
            }
        }
        return best;
    }

    // 前置声明：实现在本文件后面（§接近点 / §目标选择）
    inline void MakeApproachPoint(Player* bot, float tx, float ty, float tz, Target& t);
    inline bool FindBossTarget(Player* bot, MapRoute const& route, BotState& st, Target& out);
    inline bool NextPathNode(Player* bot, uint32 mapId, float& x, float& y, float& z);
    inline bool StageTarget(Player* bot, Map* map, MapRoute const& route, BotState& st, Target& out);

    // [2026-10-25 站长口径] "只打挡路的"：点到线段的距离（2D），用来判断一只怪在不在走廊上。
    inline float DistToSegment2D(float px, float py, float ax, float ay, float bx, float by)
    {
        float const vx = bx - ax;
        float const vy = by - ay;
        float const len2 = vx * vx + vy * vy;
        if (len2 < 1e-6f)
        {
            float const dx = px - ax, dy = py - ay;
            return std::sqrt(dx * dx + dy * dy);
        }
        float t = ((px - ax) * vx + (py - ay) * vy) / len2;
        if (t < 0.0f)
            t = 0.0f;
        if (t > 1.0f)
            t = 1.0f;
        float const cx = ax + vx * t;
        float const cy = ay + vy * t;
        float const dx = px - cx, dy = py - cy;
        return std::sqrt(dx * dx + dy * dy);
    }

    // [2026-10-25 站长口径] **控制分配**：同一只怪只给一个人控（20 秒有效），避免重复控；
    //   坦克不参与控制 —— 坦克负责把怪**拉离**被控目标。
    inline std::map<uint64, uint32>& CcAssign()
    {
        static std::map<uint64, uint32> m;
        return m;
    }
    inline std::mutex& CcMutex()
    {
        static std::mutex m;
        return m;
    }

    // 各职业可用的"硬控"法术（按等级从高到低，用 bot->HasSpell 挑第一个会的）
    inline std::vector<uint32> CcSpellsFor(Player* bot)
    {
        std::vector<uint32> out;
        if (!bot)
            return out;
        switch (bot->getClass())
        {
            case CLASS_MAGE:     { uint32 const s[] = {12826, 118, 28271, 28272}; out.assign(s, s + 4); break; }
            case CLASS_ROGUE:    { uint32 const s[] = {11297, 6770, 8643, 408}; out.assign(s, s + 4); break; }
            case CLASS_HUNTER:   { uint32 const s[] = {14311, 14310, 1499, 13809}; out.assign(s, s + 4); break; }
            case CLASS_DRUID:    { uint32 const s[] = {26989, 9853, 5196}; out.assign(s, s + 3); break; }
            case CLASS_WARLOCK:  { uint32 const s[] = {6215, 6213, 5782}; out.assign(s, s + 3); break; }
            case CLASS_PALADIN:  { uint32 const s[] = {20066, 10308, 5589}; out.assign(s, s + 3); break; }
            case CLASS_SHAMAN:   { uint32 const s[] = {2484}; out.assign(s, s + 1); break; }
            default: break;
        }
        return out;
    }

    // [2026-10-25 站长口径] **聚怪**：怪必须聚到一起才能开始打。
    //   汇聚半径 5 码；本次战斗里只要有敌对活怪离坦克 >5 码就算"还没聚齐"。
    //   另外统计"没朝坦克走"的数量（它的 victim 不是坦克）—— 这种必须由坦克去卡视野。
    const float GATHER_RADIUS = 5.0f;

    struct GatherState
    {
        uint32 pending = 0;
        uint32 notApproaching = 0;
        Creature* farthest = nullptr;
        float maxDist = 0.0f;
    };

    // [2026-10-25 站长口径] 队伍里的坦克（所有"以坦克为核心"的判定都用它）
    inline Player* GroupTank(Player* bot)
    {
        if (!bot)
            return nullptr;
        Group* grp = bot->GetGroup();
        if (!grp)
            return nullptr;
        for (GroupReference* ref = grp->GetFirstMember(); ref; ref = ref->next())
        {
            Player* m = ref->getSource();
            if (m && m->IsInWorld() && m->IsAlive() && PlayerbotAI::IsTank(m))
                return m;
        }
        return nullptr;
    }

    // [2026-10-25] 坦克在某个目标上的仇恨（坦克不在仇恨表上时返回 0）
    inline float ThreatOfTankOn(Player* bot, Unit* target)
    {
        Player* tank = GroupTank(bot);
        if (!tank || !target)
            return 0.0f;
        return target->getThreatManager().getThreat(tank);
    }

    inline GatherState ComputeGather(Player* bot)
    {
        GatherState g;
        if (!bot)
            return g;
        Group* grp = bot->GetGroup();
        if (!grp)
            return g;

        std::set<ObjectGuid> seen;
        for (GroupReference* ref = grp->GetFirstMember(); ref; ref = ref->next())
        {
            Player* m = ref->getSource();
            if (!m || !m->IsInWorld())
                continue;
            for (Unit* a : m->getAttackers())
            {
                if (!a || !a->IsAlive() || a->GetTypeId() != TYPEID_UNIT || a->GetMapId() != bot->GetMapId())
                    continue;
                if (!seen.insert(a->GetObjectGuid()).second)
                    continue;
                Creature* c = static_cast<Creature*>(a);
                if (c->IsCritter())
                    continue;
                float const d = bot->GetDistance(c);
                if (d > GATHER_RADIUS)
                {
                    ++g.pending;
                    if (d > g.maxDist)
                    {
                        g.maxDist = d;
                        g.farthest = c;
                    }
                }
                if (c->GetVictim() != bot)
                    ++g.notApproaching;
            }
        }
        return g;
    }

    // [2026-10-25 站长口径] **现场在前、表退到辅助**：
    //   打谁 / 往哪走，只看"这个副本此刻真的刷出来了哪些怪"（已加载 + 敌对 + 能打）。
    //   波次表、ord、锚点**只作为记录/标签**，不再决定行为 —— 因为副本刷点每次进本都随机，
    //   表里那个 guid 这一趟可能根本没刷出来（[WAVE] alive=0 就是这么来的）。
    //   分组用刷怪系统自己的 CreatureGroup（PullGroupKey），不做坐标聚类。
    inline bool FindLiveGroupTarget(Player* bot, Map* map, MapRoute const& route, BotState& st, Target& out)
    {
        if (!bot || !map)
            return false;

        // [2026-10-25 站长口径] **只打挡路的**：走廊 = 我 → 下一个路点（固化路线优先，没有就用 boss 方向 70 码）。
        //   一只怪只有当"它的仇恨圈会盖住走廊"时才打（走过去一定会被它拉进战斗），
        //   侧房/岔路里的怪一律跳过 —— 否则坦克会在门口一路清小怪，永远到不了 boss。
        float const corAx = bot->GetPositionX();
        float const corAy = bot->GetPositionY();
        float corBx = 0.0f, corBy = 0.0f;
        bool haveCorridor = false;
        {
            float npx = 0, npy = 0, npz = 0;
            if (NextPathNode(bot, map->GetId(), npx, npy, npz))
            {
                corBx = npx;
                corBy = npy;
                haveCorridor = true;
            }
            else
            {
                Target bossTgt;
                if (FindBossTarget(bot, route, st, bossTgt))
                {
                    float const dx = bossTgt.px - corAx;
                    float const dy = bossTgt.py - corAy;
                    float const len = std::sqrt(dx * dx + dy * dy);
                    if (len > 1.0f)
                    {
                        float const stp = std::min(70.0f, len);
                        corBx = corAx + dx / len * stp;
                        corBy = corAy + dy / len * stp;
                        haveCorridor = true;
                    }
                }
            }
        }
        // 在走廊上吗？判据用"它的威胁圈会不会覆盖走廊"，不是拍一个固定宽度。
        auto blocksCorridor = [&](Creature* c) -> bool
        {
            if (!haveCorridor)
                return true;                     // 没有走廊信息时退化为"见到就打"
            float const d = DistToSegment2D(c->GetPositionX(), c->GetPositionY(),
                corAx, corAy, corBx, corBy);
            return d <= ThreatRingRadius(bot, c) + 5.0f;
        };

        LiveHostileVisitor visitor(bot);
        Cell::VisitAllObjects(bot, visitor, LIVE_CENSUS_DIST);

        Creature* best = nullptr;
        float bestD = 1e18f;

        // [2026-10-25 性能] 普查是**只读**的：500ms 内、目标还活着没被打黑 ⇒ 直接复用上次结果，
        //   不重扫网格。（这不是行为节流 —— 决策仍然每 tick 做，只是数据不用重算。）
        uint32 const nowMs = WorldTimer::getMSTime();
        if (st.liveCensusGuid && st.liveCensusAt &&
            WorldTimer::getMSTimeDiff(st.liveCensusAt, nowMs ? nowMs : 1) < 500)
        {
            Creature* cached = map->GetCreature(st.liveCensusGuid);
            if (cached && cached->IsAlive() && cached->IsInWorld() && !cached->IsInCombat() &&
                !route.avoidEntries.count(cached->GetEntry()) &&
                !st.unattackable.count(cached->GetGUIDLow()) && blocksCorridor(cached))
            {
                best = cached;
                bestD = bot->GetDistance(cached);
            }
        }

        if (!best)
        {
            LiveHostileVisitor visitor(bot);
            Cell::VisitAllObjects(bot, visitor, LIVE_CENSUS_DIST);
            for (ObjectGuid const& g : visitor.found)
            {
                Creature* c = map->GetCreature(g);
                if (!c || !c->IsAlive() || !c->IsInWorld())
                    continue;
                if (route.avoidEntries.count(c->GetEntry()))   // 躲地板/无敌击飞类：不主动拉
                    continue;
                if (st.unattackable.count(c->GetGUIDLow()))    // 已经被判定打不了
                    continue;
                if (!blocksCorridor(c))                        // 侧房里的怪：不打，直接过
                    continue;
                float const d = bot->GetDistance(c);
                if (d < bestD)
                {
                    bestD = d;
                    best = c;
                }
            }
            st.liveCensusAt = nowMs ? nowMs : 1;
            st.liveCensusGuid = best ? best->GetGUIDLow() : 0;
        }
        if (!best)
            return false;

        out = Target();
        out.guid = best->GetGUIDLow();
        out.entry = best->GetEntry();
        out.ord = 0;          // 只用于记录（拉怪日志）；不再代表"第几波"
        out.boss = false;
        out.dynamic = true;
        // 已经贴到"开怪距离"以内 ⇒ 就地拉；否则算一个接近点（含拉怪位安全判定）
        if (bestD <= sPlayerbotAIConfig.aggroDistance)
        {
            out.px = bot->GetPositionX();
            out.py = bot->GetPositionY();
            out.pz = bot->GetPositionZ();
            out.hasPullPoint = true;
        }
        else
        {
            // [2026-10-25 修] 拉怪位改"**沿寻路上**离怪 aggro 距离的那个点"：
            //   原来 MakeApproachPoint 是"从怪直线退 N 码"，那个点经常落在墙里/柱子后 ——
            //   不在导航面上 ⇒ 后面的 PathFinder 到它必然失败 ⇒ 坦克走到路径末端就 return false 干等。
            //   用寻路点本身就保证在导航面上，方向也一定朝向目标。
            bool got = false;
            PathFinder pf(bot);
            if (pf.calculate(best->GetPositionX(), best->GetPositionY(), best->GetPositionZ()))
            {
                Movement::PointsArray const& path = pf.getPath();
                Vector3 const* keep = nullptr;              // 最后一个"仍在 aggro 距离之外"的路径点
                for (auto const& p : path)
                {
                    float const ddx = p.x - best->GetPositionX();
                    float const ddy = p.y - best->GetPositionY();
                    float const d = std::sqrt(ddx * ddx + ddy * ddy);
                    if (d <= sPlayerbotAIConfig.aggroDistance)
                        break;
                    keep = &p;
                }
                if (!keep && !path.empty())
                    keep = &path.front();
                if (keep)
                {
                    out.px = keep->x;
                    out.py = keep->y;
                    out.pz = keep->z;
                    out.hasPullPoint = true;
                    got = true;
                }
            }
            if (!got)
            {
                MakeApproachPoint(bot, best->GetPositionX(), best->GetPositionY(), best->GetPositionZ(), out);
                if (!out.hasPullPoint)      // 兜底：朝怪本体走
                {
                    out.px = best->GetPositionX();
                    out.py = best->GetPositionY();
                    out.pz = best->GetPositionZ();
                    out.hasPullPoint = true;
                }
            }
        }
        return true;
    }

    // [2026-10-25 站长口径] 现场没有活怪时"朝 boss 走"：**必须沿寻路**，不能直线。
    //   做法：对 boss 的接近点做一次 mmaps 寻路，取路径上**走一段就停**的那个点（默认 70 码）当路标；
    //   走到哪儿网格就加载到哪儿，一旦刷出新的一批怪，现场普查立刻接管（见组打组）。
    //   这样不会出现"盯一个 465 码外的点走一整局"。
    const float BOSS_WAYPOINT_STEP = 70.0f;
    // [2026-10-25 站长口径] stage 路标步长：沿现算路径走多远的点当"这一步的目标"。
    const float STAGE_WAYPOINT_STEP = 25.0f;

    // [2026-10-25 站长口径] 路线固化：把"入口 → 各 boss"的 mmap 折线**一次性算出来**存库，
    //   运行期只沿这些点走，不再实时对几百码外寻路（目标那片 tile 没加载，实时算必失败）。
    //   ⚠ 这不是"边走边学"：点是用全图载格 + PathFinder 算出来的确定值，不是某次走歪的记录。
    struct PathNode
    {
        uint32 bossGuid = 0;
        uint32 seq = 0;
        float x = 0, y = 0, z = 0;
    };

    const float MAP_PATH_SAMPLE_DIST = 18.0f;   // 折线采样间隔（码）

    inline std::map<uint32, std::vector<PathNode>>& MapPathCache()
    {
        static std::map<uint32, std::vector<PathNode>> cache;
        return cache;
    }

    // [2026-10-25 竞态修] 同一张图可能有多个实例跑在不同地图线程上：
    //   生成是 DELETE+INSERT，如果别人在生成中途读库，会缓存到"半成品"（实测 553 读到 4 个点）。
    //   这里用 "正在生成" 标记：生成期间其他线程读到就**不缓存**，等下一轮再读完整数据。
    inline std::mutex& MapPathMutex()
    {
        static std::mutex m;
        return m;
    }

    inline std::set<uint32>& MapPathGenerating()
    {
        static std::set<uint32> s;
        return s;
    }

    inline void LoadMapPath(uint32 mapId)
    {
        std::lock_guard<std::mutex> lock(MapPathMutex());
        std::map<uint32, std::vector<PathNode>>& cache = MapPathCache();
        if (cache.count(mapId))
            return;
        if (MapPathGenerating().count(mapId))
            return;                    // 别人正在生成：这次先不缓存，交给实时寻路

        std::vector<PathNode>& nodes = cache[mapId];
        std::unique_ptr<QueryResult> res = WorldDatabase.PQuery(
            "SELECT `boss_guid`,`seq`,`x`,`y`,`z` FROM `ai_playerbot_map_path` WHERE `map` = '%u' ORDER BY `seq`",
            mapId);
        if (!res)
            return;
        do
        {
            Field* f = res->Fetch();
            PathNode n;
            n.bossGuid = f[0].GetUInt32();
            n.seq = f[1].GetUInt32();
            n.x = f[2].GetFloat();
            n.y = f[3].GetFloat();
            n.z = f[4].GetFloat();
            nodes.push_back(n);
        } while (res->NextRow());

        sLog.outError("[PATHGEN] map=%u 已加载固化路线 %u 个点", mapId, uint32(nodes.size()));
    }

    // 一次性生成：全图按直线强制载格（mmap tile 也随之加载）→ 逐 boss 寻路 → 每 18 码采一点写库。
    inline bool GenerateMapPath(Player* bot, MapRoute const& route)
    {
        if (!bot || !bot->GetMap() || route.bosses.empty())
            return false;
        Map* map = bot->GetMap();
        uint32 const mapId = map->GetId();

        // 抢"生成权"：同一张图同一时刻只允许一个线程生成
        {
            std::lock_guard<std::mutex> lock(MapPathMutex());
            if (MapPathGenerating().count(mapId))
                return false;
            auto cit = MapPathCache().find(mapId);
            if (cit != MapPathCache().end() && !cit->second.empty())
                return false;                 // 已经有完整路线
            MapPathGenerating().insert(mapId);
        }
        // 生成失败也要把标记摘掉
        struct GenGuard
        {
            uint32 mapId;
            ~GenGuard()
            {
                std::lock_guard<std::mutex> lock(MapPathMutex());
                MapPathGenerating().erase(mapId);
            }
        } guard{mapId};

        std::vector<BossSpawn const*> ordered;
        ordered.reserve(route.bosses.size());
        for (auto const& b : route.bosses)
            if (b.guid)
                ordered.push_back(&b);
        std::sort(ordered.begin(), ordered.end(), [](BossSpawn const* a, BossSpawn const* b)
        {
            bool const aHas = a->ord != 0;
            bool const bHas = b->ord != 0;
            if (aHas != bHas)
                return aHas;
            if (aHas && a->ord != b->ord)
                return a->ord < b->ord;
            return a->guid < b->guid;
        });
        if (ordered.empty())
            return false;

        WorldDatabase.PExecute("DELETE FROM `ai_playerbot_map_path` WHERE `map` = '%u'", mapId);

        float sx = bot->GetPositionX();
        float sy = bot->GetPositionY();
        float sz = bot->GetPositionZ();
        uint32 seq = 0;
        uint32 legs = 0;
        uint32 written = 0;

        for (BossSpawn const* bp : ordered)
        {
            float tx = bp->x, ty = bp->y, tz = bp->z;
            if (Creature* c = map->GetCreature(bp->guid))
            {
                tx = c->GetPositionX();
                ty = c->GetPositionY();
                tz = c->GetPositionZ();
            }

            // 先把直线经过的格全部载进来（这样 mmap tile 也齐了，PathFinder 才可能给出完整路径）
            for (int i = 0; i <= 40; ++i)
            {
                float const t = float(i) / 40.0f;
                map->ForceLoadGrid(sx + (tx - sx) * t, sy + (ty - sy) * t);
            }

            PathFinder pf(bot);
            if (!pf.calculate(tx, ty, tz))
            {
                sLog.outError("[PATHGEN] map=%u boss=%u 寻路失败，跳过", mapId, bp->guid);
                continue;
            }
            Movement::PointsArray const& path = pf.getPath();
            if (path.empty())
            {
                sLog.outError("[PATHGEN] map=%u boss=%u 空路径，跳过", mapId, bp->guid);
                continue;
            }

            float px = sx, py = sy;
            float acc = 0.0f;
            for (auto const& p : path)
            {
                float const dx = p.x - px;
                float const dy = p.y - py;
                acc += std::sqrt(dx * dx + dy * dy);
                px = p.x;
                py = p.y;
                if (acc < MAP_PATH_SAMPLE_DIST)
                    continue;
                acc = 0.0f;
                WorldDatabase.PExecute(
                    "INSERT INTO `ai_playerbot_map_path` (`map`,`boss_guid`,`seq`,`x`,`y`,`z`) "
                    "VALUES ('%u','%u','%u','%f','%f','%f')",
                    mapId, bp->guid, ++seq, p.x, p.y, p.z);
                ++written;
            }

            sx = tx;
            sy = ty;
            sz = tz;
            ++legs;
        }

        {
            std::lock_guard<std::mutex> lock(MapPathMutex());
            MapPathCache().erase(mapId);      // 生成完让下一次读取重新从库里拿完整数据
        }
        sLog.outError("[PATHGEN] map=%u 生成完成：%u 段 / %u 个点", mapId, legs, written);
        return written > 0;
    }

    // [2026-10-25] 走不通的路点：记下来跳过（否则坦克每 tick 盯一个到不了的点，原地不动）。
    inline std::set<uint64>& SkippedPathNodes()
    {
        static std::set<uint64> s;
        return s;
    }

    inline void SkipPathNodeAt(uint32 mapId, float x, float y, float z)
    {
        std::map<uint32, std::vector<PathNode>>& cache = MapPathCache();
        auto it = cache.find(mapId);
        if (it == cache.end())
            return;
        for (PathNode const& n : it->second)
        {
            float const dx = n.x - x, dy = n.y - y, dz = n.z - z;
            if (dx * dx + dy * dy + dz * dz <= 4.0f)     // 2 码内算同一个点
            {
                uint64 const key = (uint64(mapId) << 32) | uint64(n.seq);
                if (SkippedPathNodes().insert(key).second)
                    sLog.outError("[PATHNODE] map=%u seq=%u 走不到 ⇒ 跳过这个路点", mapId, n.seq);
                return;
            }
        }
    }

    // ---- [2026-10-25 站长口径] 按 stage 存/取路线：**跑通一段才存**，以后懒加载也能沿它走 ----
    inline uint32 StageKey(uint32 mapId, uint16 stageOrd)
    {
        return (mapId << 16) | uint32(stageOrd);
    }

    inline std::map<uint32, std::vector<PathNode>>& StagePathCache()
    {
        static std::map<uint32, std::vector<PathNode>> c;
        return c;
    }

    inline std::vector<PathNode> const* GetStagePath(uint32 mapId, uint16 stageOrd)
    {
        uint32 const key = StageKey(mapId, stageOrd);
        std::map<uint32, std::vector<PathNode>>& cache = StagePathCache();
        auto it = cache.find(key);
        if (it == cache.end())
        {
            std::vector<PathNode>& nodes = cache[key];
            std::unique_ptr<QueryResult> res = WorldDatabase.PQuery(
                "SELECT `seq`,`x`,`y`,`z` FROM `ai_playerbot_map_path` "
                "WHERE `map` = '%u' AND `stage_ord` = '%u' AND `verified` = 1 ORDER BY `seq`",
                mapId, uint32(stageOrd));
            if (res)
            {
                do
                {
                    Field* f = res->Fetch();
                    PathNode n;
                    n.seq = f[0].GetUInt32();
                    n.x = f[1].GetFloat();
                    n.y = f[2].GetFloat();
                    n.z = f[3].GetFloat();
                    nodes.push_back(n);
                } while (res->NextRow());
            }
            if (!nodes.empty())
                sLog.outError("[STAGEPATH] map=%u stage=%u 命中已保存路线 %u 个点", mapId,
                    uint32(stageOrd), uint32(nodes.size()));
        }
        auto it2 = cache.find(key);
        if (it2 == cache.end() || it2->second.empty())
            return nullptr;
        return &it2->second;
    }

    inline void SaveStagePath(uint32 mapId, uint16 stageOrd, std::vector<Vector3> const& pts)
    {
        if (pts.size() < 3)
            return;
        WorldDatabase.PExecute(
            "DELETE FROM `ai_playerbot_map_path` WHERE `map` = '%u' AND `stage_ord` = '%u'",
            mapId, uint32(stageOrd));
        for (size_t i = 0; i < pts.size(); ++i)
        {
            WorldDatabase.PExecute(
                "INSERT INTO `ai_playerbot_map_path` (`map`,`boss_guid`,`seq`,`x`,`y`,`z`,`stage_ord`,`verified`) "
                "VALUES ('%u','0','%u','%f','%f','%f','%u','1')",
                mapId, uint32(i + 1), pts[i].x, pts[i].y, pts[i].z, uint32(stageOrd));
        }
        StagePathCache().erase(StageKey(mapId, stageOrd));
        sLog.outError("[STAGEPATH] map=%u stage=%u 跑通 ⇒ 保存 %u 个点", mapId, uint32(stageOrd),
            uint32(pts.size()));
    }

    // 取"沿固化路线、在我前方最近的一个点"。没有路线返回 false。
    inline bool NextPathNode(Player* bot, uint32 mapId, float& x, float& y, float& z)
    {
        std::map<uint32, std::vector<PathNode>>& cache = MapPathCache();
        if (!cache.count(mapId))
            LoadMapPath(mapId);
        auto it = cache.find(mapId);
        if (it == cache.end() || it->second.empty())
            return false;

        bool found = false;
        float best = 1e18f;
        for (PathNode const& n : it->second)
        {
            uint64 const key = (uint64(mapId) << 32) | uint64(n.seq);
            if (SkippedPathNodes().count(key))       // 已知走不到的路点：跳过
                continue;
            float const d = bot->GetDistance(n.x, n.y, n.z);
            if (d < 6.0f)              // 已经踩在这个点上了 ⇒ 看下一个
                continue;
            if (d < best)
            {
                best = d;
                x = n.x;
                y = n.y;
                z = n.z;
                found = true;
            }
        }
        if (!found)
            return false;
        // 只取 45 码内的下一个点，太远说明路线在别处（或还没走到）⇒ 交给实时逻辑
        return best <= 45.0f;
    }

    inline bool FindBossWaypoint(Player* bot, MapRoute const& route, BotState& st, Target& out,
        float step = BOSS_WAYPOINT_STEP)
    {
        if (!bot || !bot->GetMap())
            return false;

        // [2026-10-25 站长口径] **按 stage + mmap 现算路径，在路径上取点**：
        //   不再用"库里存的路点 + 按直线距离挑最近"—— 副本绕圈/上下层时那样会挑到另一条腿上的点，
        //   那个点隔着墙/地板，PathFinder 到不了，坦克就钉在原地（dist 一直不变得就是这个）。
        //   现在每次都：① 取当前 stage 的目标（没有 stage 就用最近的 boss）；
        //              ② 对目标做一次 mmap 寻路；③ 在这条**新鲜路径**上取"沿路 ~25 码"的点。
        //   路径点天然在导航面上，且天然是前向的 ⇒ 不需要"坏点兜底"。
        float nx = 0, ny = 0, nz = 0;
        uint32 const stageMapId = bot->GetMap()->GetId();
        uint16 const curStage = st.stageOrd;

        // [2026-10-25 站长口径] stage 推进 = 上一段**跑通了** ⇒ 把它存下来（verified=1）。
        if (st.recStage != 0xFFFF && st.recStage != curStage && st.recPath.size() >= 3)
            SaveStagePath(stageMapId, st.recStage, st.recPath);
        if (st.recStage != curStage)
        {
            st.recStage = curStage;
            st.recPath.clear();
            st.stagePathSeq = 0;
        }

        // ① 这一段已经存过（跑通过）⇒ 沿它前向走一个点，不再现算
        if (std::vector<PathNode> const* sp = GetStagePath(stageMapId, curStage))
        {
            for (PathNode const& n : *sp)
            {
                if (n.seq <= st.stagePathSeq)
                    continue;
                if (bot->GetDistance(n.x, n.y, n.z) <= 6.0f)
                {
                    st.stagePathSeq = n.seq;      // 走到了 ⇒ 前进
                    continue;
                }
                out = Target();
                out.guid = 0;
                out.entry = 0;
                out.ord = 0;
                out.boss = false;
                out.dynamic = true;
                out.px = n.x;
                out.py = n.y;
                out.pz = n.z;
                out.hasPullPoint = true;
                return true;
            }
        }
        float objX = 0.0f, objY = 0.0f, objZ = 0.0f;
        bool haveObj = false;
        {
            Target obj;
            if (!route.stages.empty() && StageTarget(bot, bot->GetMap(), route, st, obj))
                haveObj = true;
            else if (FindBossTarget(bot, route, st, obj))
                haveObj = true;
            if (haveObj)
            {
                objX = obj.px;
                objY = obj.py;
                objZ = obj.pz;
                // [2026-10-25 站长口径] 目标若是一只**已加载的怪**，就寻路到它本体（它一定站在导航面上），
                //   不要用"从它直退 22 码"的接近点 —— 那个点可能落在墙里/柱子里，寻路必失败。
                if (obj.guid)
                {
                    if (Creature* oc = bot->GetMap()->GetCreature(obj.guid))
                    {
                        objX = oc->GetPositionX();
                        objY = oc->GetPositionY();
                        objZ = oc->GetPositionZ();
                    }
                }
                PathFinder pfObj(bot);
                if (pfObj.calculate(objX, objY, objZ))
                {
                    Movement::PointsArray const& pth = pfObj.getPath();
                    float pxp = bot->GetPositionX();
                    float pyp = bot->GetPositionY();
                    float acc = 0.0f;
                    Vector3 const* pick = nullptr;
                    for (auto const& p : pth)
                    {
                        float const ddx = p.x - pxp;
                        float const ddy = p.y - pyp;
                        acc += std::sqrt(ddx * ddx + ddy * ddy);
                        pxp = p.x;
                        pyp = p.y;
                        pick = &p;
                        if (acc >= STAGE_WAYPOINT_STEP)
                            break;
                    }
                    if (pick && bot->GetDistance(pick->x, pick->y, pick->z) > AT_PULL_POINT_DIST)
                    {
                        nx = pick->x;
                        ny = pick->y;
                        nz = pick->z;
                        // [2026-10-25 站长口径] 边走边记这一段的路点；等这一 stage 跑通了再存库。
                        float const lx = st.recPath.empty() ? bot->GetPositionX() : st.recPath.back().x;
                        float const ly = st.recPath.empty() ? bot->GetPositionY() : st.recPath.back().y;
                        float const rx = pick->x - lx;
                        float const ry = pick->y - ly;
                        if (std::sqrt(rx * rx + ry * ry) >= MAP_PATH_SAMPLE_DIST)
                            st.recPath.push_back(Vector3(pick->x, pick->y, pick->z));
                    }
                }
            }
        }
        if (nx != 0.0f || ny != 0.0f)
        {
            out = Target();
            out.guid = 0;
            out.entry = 0;
            out.ord = 0;
            out.boss = false;
            out.dynamic = true;
            out.px = nx;
            out.py = ny;
            out.pz = nz;
            out.hasPullPoint = true;
            return true;
        }

        // [2026-10-25 站长口径] 旧的"两级瞄准"兜底**已删除**：
        //   全图（LoadAllGridsOnMaps）已经加载，不该再出现"tile 没加载导致寻路失败"；
        //   真失败就如实报坐标，不再退化成"直扑几百码外的坐标"。
        sLog.outError("[WAYPT] %s 寻路失败：map=%u stage=%u obj=(%.0f,%.0f,%.0f) hasObj=%d",
            bot->GetName(), stageMapId, uint32(curStage), objX, objY, objZ, haveObj ? 1 : 0);
        return false;
    }
    // [2026-10-25 站长口径] 巡逻怪的位置**不能只看此刻**：
    //   "刚判安全、它一转身就走进来"是最常见的翻车。按当前朝向 + 移速外推 dt 秒后的位置。
    // [2026-10-26 站长口径] 巡逻怪要**按路径**推演位置，不按当前朝向直线外推：
    //   直线外推在拐角处必然推错，导致"更便宜的开怪时刻"永远不落地（r100 实测整组等 1100s）。
    //   实现在 DungeonRouteActions.cpp（那里才能包含 WaypointMovementGenerator/WaypointManager）。
    void PredictMobPosPath(Creature* c, float dtSec, float& x, float& y);

    inline void PredictMobPos(Creature* c, float dtSec, float& x, float& y)
    {
        PredictMobPosPath(c, dtSec, x, y);
    }

    struct PullCostSample
    {
        uint32 adds = 0;         // 会被一起带进战斗的"非本组"怪数量
        uint32 patrolAdds = 0;   // 其中正在移动的（巡逻/游走）
        uint32 atMs = 0;         // 这个代价对应"多少毫秒之后"
    };

    // [2026-10-25 站长口径] 联动放行：某只怪**几乎总是**和某一波一起进战斗 ⇒ 它本来就是这一波的一部分，
    //   不该再当 ADD 算（例：巡逻怪总在某组身边转）。
    //   判据用**观察统计**，不是人填的表：每次在本组评估一次开怪，就记一次"它当时在不在威胁圈里"。
    const float LINK_ALLOW_RATIO = 0.5f;    // 观察到"同时在圈内"的比例 ≥ 这个 ⇒ 视为联动
    const uint32 LINK_ALLOW_MIN_SEEN = 20;  // 至少观察这么多次才认（避免偶然两次就放行）

    struct LinkStats
    {
        uint32 seen = 0;
        uint32 inside = 0;
    };

    // key = (本组 groupKey << 32) | 那只怪的 groupKey
    inline std::map<uint64, LinkStats>& GroupLinkStats()
    {
        static std::map<uint64, LinkStats> stats;   // 只在进程内存里积累：同一趟跑测内就会生效
        return stats;
    }

    // [2026-10-25] 拉动代价专用扫描：以**开打位置**（目标位置）为圆心，不做 LOS 过滤
    //   （同一个房间里的怪即使此刻被柱子挡视线，开打后也会被引到；宁可多算，不要漏算）。
    struct PullAddVisitor
    {
        Player* bot;
        std::vector<ObjectGuid> found;

        explicit PullAddVisitor(Player* b) : bot(b) {}

        bool operator()(Creature* c)
        {
            if (!c || !c->IsAlive() || !c->IsInWorld())
                return true;
            if (c->IsInCombat())
                return true;
            if (!SessionCanFight(bot, c))
                return true;
            if (c->IsCritter())
                return true;
            if (c->GetLevel() + 10 <= bot->GetLevel())
                return true;
            found.push_back(c->GetObjectGuid());
            return true;
        }

        template<class T>
        void Visit(GridRefManager<T>&) {}
        void Visit(CreatureMapType& m)
        {
            for (CreatureMapType::iterator itr = m.begin(); itr != m.end(); ++itr)
            {
                Creature* c = itr->getSource();
                if (c)
                    (*this)(c);
            }
        }
    };

    // [2026-10-25 站长口径] 拉动代价：dtSec 之后在**目标所在位置**开打，会有多少**不属于目标组**的怪
    //   被一起带进战斗。判据 = "开打位置落进它的威胁圈"，而不是"坦克此刻站位" ——
    //   站位通常离目标 25 码以上，开打后整队压上去，旁边那只就会被引到。
    //   本组 = CreatureGroup（见 PullGroupKey），同组的一律不算 ADD。
    inline PullCostSample PullCostAt(Player* bot, Creature* target, float dtSec, float margin)
    {
        PullCostSample r;
        r.atMs = uint32(dtSec * 1000.0f);
        if (!bot || !bot->GetMap() || !target)
            return r;
        uint32 const groupKey = PullGroupKey(target);

        // 目标自己也会动（巡逻怪）：它 dt 秒后的位置才是"那时候在哪儿开打"。
        float tx, ty;
        PredictMobPos(target, dtSec, tx, ty);
        // 目标"喊人"的半径：核心的求援判据（和 pullWaits 用的是同一个字段）。
        float const assistR = target->GetCreatureInfo() && target->GetCreatureInfo()->CallForHelp > 0
            ? float(target->GetCreatureInfo()->CallForHelp) : 5.0f;

        PullAddVisitor visitor(bot);
        Cell::VisitAllObjects(tx, ty, bot->GetMap(), visitor, LIVE_GROUP_SCAN_DIST);

        for (ObjectGuid const& g : visitor.found)
        {
            Creature* c = bot->GetMap()->GetCreature(g);   // 回查，避免跨遍历持有失效指针
            if (!c || !c->IsAlive() || c == target)
                continue;
            if (PullGroupKey(c) == groupKey)      // 同一波，本来就该一起打
                continue;

            float cx, cy;
            PredictMobPos(c, dtSec, cx, cy);
            float const dfx = tx - cx;
            float const dfy = ty - cy;
            float const dFight = std::sqrt(dfx * dfx + dfy * dfy);
            float const reach = ThreatRingRadius(bot, c) + margin;

            bool const inAssist = (dFight <= assistR);
            bool const inRing = (dFight <= reach);
            bool const isAdd = inRing || inAssist;

            // 联动统计：seen = 它在判断范围内的观察次数；inside = 其中真的会被带进来的次数。
            //   只在 dtSec == 0 这一次评估里记，否则一次决策会被算 5 遍。
            if (dtSec <= 0.0f && dFight <= reach + LIVE_GROUP_SCAN_DIST)
            {
                LinkStats& ls = GroupLinkStats()[(uint64(groupKey) << 32) | uint64(PullGroupKey(c))];
                if (ls.seen < 1000000)
                    ++ls.seen;
                if (isAdd && ls.inside < 1000000)
                    ++ls.inside;
                if (ls.seen >= LINK_ALLOW_MIN_SEEN &&
                    float(ls.inside) >= LINK_ALLOW_RATIO * float(ls.seen))
                    continue;                     // 几乎总在一起 ⇒ 放行，不算 ADD
            }

            if (!isAdd)
                continue;

            ++r.adds;
            if (c->GetMotionMaster() && !c->IsStopped())
                ++r.patrolAdds;
        }
        return r;
    }

    // [2026-10-25 站长口径] 开怪准入 = 等"代价最小的那一刻"：
    //   ① 现在就没有额外怪 ⇒ 立刻拉；
    //   ② 现在会带进来、但往后几秒有更便宜的时刻 ⇒ 等那一刻再拉；
    //   ③ 等也没用（最小代价就是这样）⇒ 现在就拉 —— 站长口径：不可避免的那只就接受
    //      （例：540 门口必须先开的军团士兵，它离入口最近那一刻仍会带一只静态怪）。
    //   返回 0 = 现在就拉；>0 = 等这么多毫秒。
    inline uint32 PullAdmissionDelayMs(Player* bot, Creature* target, PullCostSample* outNow = nullptr)
    {
        if (!bot || !target)
            return 0;
        // 脱战拉怪，余量取 5 码 —— 与 UnsafeStandOffender 的脱战口径一致（不另发明距离）。
        float const margin = 5.0f;
        PullCostSample const now = PullCostAt(bot, target, 0.0f, margin);
        if (outNow)
            *outNow = now;
        if (now.adds == 0)
            return 0;

        PullCostSample best = now;
        static float const horizon[] = {2.0f, 4.0f, 6.0f, 8.0f};
        for (size_t i = 0; i < sizeof(horizon) / sizeof(horizon[0]); ++i)
        {
            PullCostSample const s = PullCostAt(bot, target, horizon[i], margin);
            if (s.adds < best.adds)
                best = s;
        }
        if (best.adds < now.adds)
        {
            // [2026-10-26 修] outNow 必须始终是**当前**这一刻的代价（以前被 best 覆盖，日志才会印出"代价=0 还在等"的假象）。
            //   等窗口的次数/总时长限制在调用方（DungeonRouteActions 的 pullCostWaitSince）。
            return best.atMs ? best.atMs : 2000;
        }
        return 0;
    }

    inline bool IsRouteBoss(uint32 mapId, uint32 guid)
    {
        MapRoute const* r = GetMapRoute(mapId);
        if (!r)
            return false;
        for (auto const& b : r->bosses)
            if (b.guid == guid)
                return true;
        return false;
    }

    // [2026-10-24 站长口径] 不吃嘲讽（对 Taunt 355 免疫）⇒ 仇恨门槛要高（1500）
    inline bool IsImmuneToTaunt(Creature* c)
    {
        if (!c)
            return false;
        SpellEntry const* taunt = sSpellTemplate.LookupEntry<SpellEntry>(355);
        if (!taunt)
            return false;
        return c->IsImmuneToSpellEffect(taunt, EFFECT_INDEX_0, false);
    }

    inline bool WaveHasFightable(Player* bot, Wave const& w)
    {
        Map* map = bot ? bot->GetMap() : nullptr;
        for (auto& m : w.members)
            if (SessionCanFight(bot, MemberCreature(map, m)))
                return true;
        return false;
    }

    // [2026-10-22 站长口径] 540 策略的"攒够才清"门槛：表里的 engage_count × 全局倍率（可在线调，300s 热加载）。
    inline uint32 EngageCount(MapRoute::CountWeightRule const& r)
    {
        float const scale = dw::Get("mob.count.engage_scale");   // 缺省值在 DecisionWeights.h 里（1.0）
        float const v = float(r.engageCount) * (scale > 0.0f ? scale : 1.0f);
        return uint32(std::max(1.0f, v));
    }

    // [2026-10-22 站长口径] 数一下某坐标附近"同 entry 的活怪"（用于 540 的按数量动态权重）。
    //   只用路线成员 + 巡逻表：540 的 Creeping Oozeling / Shattered Hand Zealot 都在路线里，
    //   这样不用做格子扫描，开销可控。
    inline uint32 CountNearbyEntry(Map* map, MapRoute const& route, uint32 entry,
                                   float x, float y, float z, float radius)
    {
        if (!map)
            return 0;
        uint32 n = 0;
        float const r2 = radius * radius;
        auto visit = [&](uint32 guid)
        {
            Creature* c = map->GetCreature(guid);
            if (!c || !c->IsAlive() || !c->IsInWorld() || c->GetEntry() != entry)
                return;
            float const dx = c->GetPositionX() - x;
            float const dy = c->GetPositionY() - y;
            float const dz = c->GetPositionZ() - z;
            if (dx * dx + dy * dy + dz * dz <= r2)
                ++n;
        };
        for (auto const& kv : route.waves)
            for (auto const& m : kv.second.members)
                visit(m.guid);
        for (uint32 pg : route.patrolGuids)
            visit(pg);
        return n;
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
        sLog.outError("[DUNGEONROUTE] 重新尝试之前暂跳过的波 ord=%u（到点复活）", uint32(ord));
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
                        sLog.outError("[DUNGEONROUTE] %s 有未登记怪在战斗 ⇒ 保持波次 ord=%u（不推进）",
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
            sLog.outError("[DUNGEONROUTE] %s 波已清 ord=%u（%s）",
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
            sLog.outError("[DUNGEONROUTE] %s 跳过 ord=%u（%s）",
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
            sLog.outError("[DUNGEONROUTE] %s 跳过 boss 刷点 %u（未加载/不可达，不算已死）",
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
                sLog.outError("[DUNGEONROUTE] %s 跳点 (%.1f,%.1f,%.1f) -> (%.1f,%.1f,%.1f)",
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
                    sLog.outError("[DUNGEONROUTE] %s 跳跃链接不可行：vXY=%.1f > max=%.1f (dist=%.1f drop=%.1f)",
                        bot->GetName(), speedXY, maxXY, dist2d, drop);
                    continue;
                }

                nearAny = true;
                st.linkUsedAt = now;
                st.linkWaitAt = 0;
                bot->GetMotionMaster()->MoveJump(l.dx, l.dy, l.dz, speedXY, v0);
                sLog.outError("[DUNGEONROUTE] %s 客户端式跳跃 (%.1f,%.1f,%.1f) -> (%.1f,%.1f,%.1f) vXY=%.1f vZ=%.1f t=%.2f",
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
                sLog.outError("[DUNGEONROUTE] %s 等升降台/传送点 (%.1f,%.1f,%.1f)",
                    bot->GetName(), l.sx, l.sy, l.sz);
                return true;
            }
            if (WorldTimer::getMSTimeDiff(st.linkWaitAt, now) >= 3000)
            {
                st.linkWaitAt = 0;
                st.linkUsedAt = now;
                bot->TeleportTo(map->GetId(), l.dx, l.dy, l.dz, bot->GetOrientation());
                sLog.outError("[DUNGEONROUTE] %s 升降台/传送点 -> (%.1f,%.1f,%.1f)",
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

        // [2026-10-25 站长口径] **拉回**：起手后坦克要带怪退回拉怪位再打（怪会跟过来），
        //   而不是走进房间、把隔壁那几组一起引进来（552 Arcatraz 实测：拉 2 只 → 6 个攻击者）。
        //   由 DungeonTankRepositionAction 在战斗中每 tick 执行退回，退到就清标记。
        {
            BotState& pb = StateOf(bot);
            if (target.hasPullPoint)
            {
                pb.pullBackPending = 1;
                pb.standOverrideX = target.px;
                pb.standOverrideY = target.py;
                pb.standOverrideZ = target.pz;
                pb.standOverrideUntil = WorldTimer::getMSTime() + 20000;
                pb.safePullPoint = false;      // 这是"拉回点"，不是 PULLSAFE 的安全拉怪点
            }
        }

        const uint16 ord = target.boss ? 0 : (target.ord ? target.ord : CurrentOrd(bot, route));
        pullrec::SetRouteOrd(bot->GetGroup(), ord);

        sLog.outError("[DUNGEONROUTE] %s 自动开怪起手 -> 记录器 ord=%u（%s）",
            bot->GetName(), uint32(ord), target.boss ? "boss" : "wave");
        // [2026-10-24 站长口径] 坦克拉怪效率也要按职业统计：每次真的起手记一条（含职业）
        sLog.outError("[PULLED] bot=%s class=%u map=%u ord=%u boss=%d",
            bot->GetName(), uint32(bot->getClass()), route.mapId, uint32(ord), target.boss ? 1 : 0);

        // [2026-10-23 站长口径] "开怪姿势"要先能量化：起手这一刻，目标周围 10/15/20 码内有多少敌对活怪。
        //   只加观测、不改行为 —— 用来回答"到底是拉 1 只还是拉了一整窝"。
        if (Creature* t = bot->GetMap() ? bot->GetMap()->GetCreature(target.guid) : nullptr)
        {
            uint32 c10 = 0, c15 = 0, c20 = 0;
            auto countAt = [&](float radius, uint32& out)
            {
                for (auto const& kv : route.waves)
                    for (auto const& m : kv.second.members)
                    {
                        Creature* c = MemberCreature(bot->GetMap(), m);
                        if (!c || !c->IsAlive() || !c->IsInWorld() || !SessionCanFight(bot, c))
                            continue;
                        if (t->IsWithinDist(c, radius, false))
                            ++out;
                    }
            };
            countAt(10.0f, c10);
            countAt(15.0f, c15);
            countAt(20.0f, c20);
            sLog.outError("[PULLSIZE] %s ord=%u target=%u near10=%u near15=%u near20=%u",
                bot->GetName(), uint32(ord), target.guid, c10, c15, c20);
        }
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
            sLog.outError("[DUNGEONROUTE] %s 自动开怪拉怪结束（%s）", bot->GetName(), result);
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
    inline void RefreshWaveKnowledge(Player* bot, Map* map, MapRoute const& route, BotState& st)
    {
        // [2026-10-21 假清修复] 灭团/出本后 map 上所有怪 GetCreature 都为空，
        //   不能据此把"见过活怪"的波判定为已清（543 出现过 66 秒 25/25 的假通关）。
        if (!map || map->GetId() != route.mapId)
            return;

        for (auto& kv : route.waves)
        {
            if (st.skip.count(kv.first) || st.cleared.count(kv.first))
                continue;

            if (WaveHasFightable(bot, kv.second))
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
            if (wit != route.waves.end() && WaveHasFightable(bot, wit->second))
            {
                uint16 const ord = *it;
                it = st.cleared.erase(it);
                st.seenAlive.insert(ord);
                if (st.lockOrd == ord)
                    st.lockOrd = 0;
                sLog.outError("[DUNGEONROUTE] 波 %u 的怪重新刷出 ⇒ 重新打开（不能算已清）", (uint32)ord);
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
                // [2026-10-22 验证] 逐 boss 击杀留痕（boss_report.py 靠这行统计）
                sLog.outError("[BOSSKILL] map=%u inst=%u entry=%u guid=%u", map->GetId(),
                    map->GetInstanceId(), b.entry, b.guid);
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
                sLog.outError("[BOSSKILL] map=%u inst=%u entry=%u guid=%u", map->GetId(),
                    map->GetInstanceId(), b.entry, b.guid);
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



    // [2026-10-22 站长口径] 与 BossModePullSafe 同样的判定，但**返回挡路的怪 guid**：
    //   用于区分"能不能等" —— 挡路的是巡逻怪 ⇒ 等它走开；是别的波/未登记怪 ⇒ 等也没用。
    inline uint32 BossModePullBlocker(Player* bot, Map* map, MapRoute const& route, Creature* target)
    {
        if (!bot || !map || !target)
            return 0;

        // [2026-10-22 站长口径] 宽松模式（cheat 验证寻路专用）：pull.safe.strict=0 时
        //   除了"巡逻怪会引到"以外都放行 —— 先让坦克把 boss/跳点/路口跑通，再回头收紧危险度。
        bool const strict = dw::Get("pull.safe.strict") > 0.0f;

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
            // [2026-10-22 站长口径] 540 特殊策略：数量不够的同类怪（Oozeling/Zealot）不算"挡路"，
            //   否则队伍会为了躲它们站着不动；等它们攒够数量才当正常目标处理。
            auto cw = route.countWeights.find(c->GetEntry());
            if (cw != route.countWeights.end() &&
                CountNearbyEntry(map, route, c->GetEntry(),
                    c->GetPositionX(), c->GetPositionY(), c->GetPositionZ(), cw->second.radius) < EngageCount(cw->second))
                return false;
            if (!target->IsWithinDist(c, 10.0f, false))
                return false;
            uint16 const ord = ordOf(guid);
            if (ord != targetOrd || ord == 0)
            {
                if (!strict && !route.patrolGuids.count(guid))
                    return false;              // 宽松模式：非巡逻怪不算 ADD 风险
                blocker = guid;
                return true;
            }
            return false;
        };

        for (auto& kv : route.waves)
            for (auto& m : kv.second.members)
                if (risky(m.guid))
                    return blocker;
        BotState& pst = StateOf(bot);
        uint32 const nowMs = WorldTimer::getMSTime();
        for (uint32 pg : route.patrolGuids)
        {
            if (pst.patrolIgnoreGuid == pg && pst.patrolIgnoreUntil > nowMs)
                continue;                       // 这只巡逻怪刚被"等超时"过：先放行，避免死循环
            if (risky(pg))
                return blocker;
        }
        return 0;
    }

    inline bool BossModePullSafe(Player* bot, Map* map, MapRoute const& route, Creature* target)
    {
        return BossModePullBlocker(bot, map, route, target) == 0;
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
        sLog.outError("[DUNGEONROUTE] %s 没有可达 boss ⇒ 走连接点#%u (%.1f,%.1f,%.1f)->(%.1f,%.1f,%.1f)，跳完重查 boss",
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
                sLog.outError("[DUNGEONROUTE] %s 重新检查 boss 可达性 entry=%u（挂起到期）",
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

            // [2026-10-23 站长指令] boss 前置条件：前置怪还活着（在 boss 周围 radius 内）⇒ **不选这个 boss，
            //   也不许跳到后面的 boss**，而是把"最近的活前置怪"当作目标去清掉。
            //   （第一版只 continue，结果 555 直接去选了尾王 Murmur、跨半张图，ritualist 永远不清）
            if (!route.bossRequires.empty())
            {
                Creature* nearestReq = nullptr;
                float nearestReqDist = 1e18f;
                uint32 blockedByEntry = 0;
                float blockedRadius = 0.0f;
                for (auto const& br : route.bossRequires)
                {
                    if (br.first != bp->entry)
                        continue;
                    auto consider = [&](uint32 g)
                    {
                        Creature* rc = map->GetCreature(g);
                        if (!rc || !rc->IsAlive() || !rc->IsInWorld())
                            return;
                        if (rc->GetEntry() != br.second.reqEntry)
                            return;
                        if (rc->GetDistance(tx, ty, tz) > br.second.radius)
                            return;
                        float const d = bot->GetDistance(rc);
                        if (d < nearestReqDist)
                        {
                            nearestReqDist = d;
                            nearestReq = rc;
                            blockedByEntry = br.second.reqEntry;
                            blockedRadius = br.second.radius;
                        }
                    };
                    for (auto const& kv : route.guidOrd)
                        consider(kv.first);
                    for (uint32 pg : route.patrolGuids)
                        consider(pg);
                }
                if (nearestReq)
                {
                    if (!st.bossRequireLogAt || WorldTimer::getMSTimeDiff(st.bossRequireLogAt, now ? now : 1) >= 10000)
                    {
                        st.bossRequireLogAt = now ? now : 1;
                        sLog.outError("[DUNGEONROUTE] %s boss entry=%u 前置未满足（entry=%u 还有活着的，半径 %.0f）⇒ 先去清它 dist=%.1f",
                            bot->GetName(), bp->entry, blockedByEntry, blockedRadius, nearestReqDist);
                    }
                    out = Target();
                    out.guid = nearestReq->GetGUIDLow();
                    out.entry = nearestReq->GetEntry();
                    out.ord = 0;
                    out.boss = false;
                    out.px = nearestReq->GetPositionX();
                    out.py = nearestReq->GetPositionY();
                    out.pz = nearestReq->GetPositionZ();
                    out.hasPullPoint = true;
                    return true;
                }
            }

            // [2026-10-22 站长口径] 可达性用**真实寻路**（mmaps PathFinder），不用"距离没缩短"这种代理：
            //   绕墙、打架、等巡逻都会让距离暂时变大，但那不代表不可达。
            uint32 const recheckMs = uint32(dw::Get("boss.path_recheck_sec") * 1000.0f);
            bool const needPathCheck = (st.bossPathGuid != guid) || !st.bossPathCheckAt ||
                WorldTimer::getMSTimeDiff(st.bossPathCheckAt, now) >= recheckMs;
            if (needPathCheck)
            {
                st.bossPathCheckAt = now ? now : 1;
                st.bossPathGuid = guid;

                PathFinder pf(bot);
                bool ok = pf.calculate(tx, ty, tz);
                if (ok)
                {
                    auto& pts = pf.getPath();
                    if (pts.empty())
                        ok = false;
                    else
                    {
                        Vector3 const& end = pts.back();
                        float const ex = end.x - tx, ey = end.y - ty, ez = end.z - tz;
                        if (std::sqrt(ex * ex + ey * ey + ez * ez) > dw::Get("boss.path_tolerance_yd"))
                            ok = false;
                    }
                }

                if (ok != st.bossPathOk)
                {
                    st.bossPathOk = ok;
                    if (!ok)
                    {
                        // [2026-10-22 诊断] 打出 PathFinder 的真实结论，区分 "navmesh 没加载"(type=BLANK)
                        //   与 "确实找不到路"(type=NOPATH)，以及路径点数/终点距离。
                        PathFinder dbg(bot);
                        bool const dbgOk = dbg.calculate(tx, ty, tz);
                        float dbgEndDist = -1.0f;
                        auto& dbgPts = dbg.getPath();
                        if (!dbgPts.empty())
                        {
                            Vector3 const& e2 = dbgPts.back();
                            float const dx2 = e2.x - tx, dy2 = e2.y - ty, dz2 = e2.z - tz;
                            dbgEndDist = std::sqrt(dx2 * dx2 + dy2 * dy2 + dz2 * dz2);
                        }
                        sLog.outString("[DUNGEONROUTE] %s boss entry=%u 寻路不可达（mmaps dist=%.0f ok=%d type=%d pts=%u endDist=%.1f）⇒ 先试连接点",
                            bot->GetName(), bp->entry, dist, dbgOk ? 1 : 0, int(dbg.getPathType()),
                            uint32(dbgPts.size()), dbgEndDist);
                    }
                    else
                        sLog.outString("[DUNGEONROUTE] %s boss entry=%u 寻路可达（dist=%.0f）",
                            bot->GetName(), bp->entry, dist);
                }
            }

            if (st.bossGuid != guid)
            {
                st.bossGuid = guid;
                st.bossSince = now ? now : 1;
                st.bossBestDist = dist;
                st.bossPosX = bot->GetPositionX();
                st.bossPosY = bot->GetPositionY();
                st.bossPosAt = now ? now : 1;
            }

            // 只有"路径可达、但 boss 目标在 stuck_sec 内位置几乎没动"才算卡住
            bool stuck = false;
            if (st.bossPathOk && st.bossPosAt &&
                WorldTimer::getMSTimeDiff(st.bossPosAt, now) >= uint32(dw::Get("boss.stuck_sec") * 1000.0f))
            {
                float const mx = bot->GetPositionX() - st.bossPosX;
                float const my = bot->GetPositionY() - st.bossPosY;
                stuck = (mx * mx + my * my) < 4.0f;
                st.bossPosX = bot->GetPositionX();
                st.bossPosY = bot->GetPositionY();
                st.bossPosAt = now ? now : 1;
            }

            // [2026-10-22 站长口径] 不再用"PathFinder 说不可达"来挂起 boss（路线断点/门口/部分路径都会误判，
            //   实测 183/218 次误判，而 boss 明明可达）。只有**位置卡住**才走连接点/挂起。
            if (stuck)
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
                    sLog.outError("[DUNGEONROUTE] %s boss entry=%u 走不过去（寻路不可达/卡住）⇒ 先执行连接点#%u "
                        "(%.1f,%.1f,%.1f)->(%.1f,%.1f,%.1f)，跳完再查可达",
                        bot->GetName(), bp->entry, uint32(li), l.sx, l.sy, l.sz, l.dx, l.dy, l.dz);
                    return true;
                }

                st.bossDeferUntil[guid] = (now ? now : 1) + 120000;
                st.bossGuid = 0;
                st.bossSince = 0;
                sLog.outError("[DUNGEONROUTE] %s boss entry=%u 寻路不可达/卡住（dist=%.0f，连接点都试过）⇒ 暂时挂起 120 秒，改走波次",
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
                    // [寻路修C 2026-10-22] 只有锚点真的在 boss 附近（≤40 码）才拿它当拉怪位；
                    //   否则就是 boss↔波次锚点映射错位（实测 547 Mennu 的锚点偏 367 码，队伍会朝地图另一头走）。
                    //   这种情况按 boss 自身实际落点（tx,ty,tz：已加载用实时坐标，否则用刷点）过去。
                    float const ax = wit->second.pull.x;
                    float const ay = wit->second.pull.y;
                    float const az = wit->second.pull.z;
                    float const adx = ax - tx, ady = ay - ty, adz = az - tz;
                    if (adx * adx + ady * ady + adz * adz <= 40.0f * 40.0f)
                    {
                        out.px = ax;
                        out.py = ay;
                        out.pz = az;
                        return true;
                    }
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
    // [2026-10-23 站长指令] 进度链：按 stage 顺序推进，条件满足才进下一阶段。
    //   返回 true ⇒ 本 tick 的目标由 stage 决定（不再走"见怪就拉"的波次逻辑，避免清无关小怪）。
    inline bool StageTarget(Player* bot, Map* map, MapRoute const& route, BotState& st, Target& out)
    {
        if (route.stages.empty() || st.stageOrd >= 9000)
            return false;

        uint32 const now = WorldTimer::getMSTime();
        auto logStage = [&](const char* msg, uint16 from, uint16 to)
        {
            if (!st.stageLogAt || WorldTimer::getMSTimeDiff(st.stageLogAt, now ? now : 1) >= 5000)
            {
                st.stageLogAt = now ? now : 1;
                sLog.outError("[STAGE] %s map=%u %s stage %u -> %u",
                    bot->GetName(), map->GetId(), msg, uint32(from), uint32(to));
            }
        };

        auto findMember = [&](uint32 entry, float ax, float ay, float radius) -> Creature*
        {
            Creature* best = nullptr;
            float bestD = 1e18f;
            for (auto const& kv : route.guidOrd)
            {
                Creature* c = map->GetCreature(kv.first);
                if (!c || !c->IsAlive() || !c->IsInWorld())
                    continue;
                if (entry && c->GetEntry() != entry)
                    continue;
                if (route.skipEntries.count(c->GetEntry()))
                    continue;   // [2026-10-24] 会消失的怪不当阶段目标
                if (!entry && !SessionCanFight(bot, c))
                    continue;
                if (radius > 0.0f)
                {
                    float const dx = c->GetPositionX() - ax;
                    float const dy = c->GetPositionY() - ay;
                    if (std::sqrt(dx * dx + dy * dy) > radius)
                        continue;
                }
                float const d = bot->GetDistance(c);
                if (d < bestD) { bestD = d; best = c; }
            }
            return best;
        };

        for (int guard = 0; guard < 8; ++guard)
        {
            if (st.stageOrd == 0)
                st.stageOrd = route.stages.front().ord;

            MapRoute::Stage const* cur = nullptr;
            for (auto const& s : route.stages)
                if (s.ord == st.stageOrd) { cur = &s; break; }
            if (!cur) { st.stageOrd = 9000; return false; }

            auto advance = [&](const char* why)
            {
                uint16 next = 9000;
                for (auto const& s : route.stages)
                    if (s.ord > st.stageOrd) { next = s.ord; break; }
                logStage(why, st.stageOrd, next);
                st.stageOrd = next;
                st.stageWaitSince = 0;
            };

            // [2026-10-26 站长指令] kind='link'：走到连接点 src，然后跳/落到 dst，跳完推进下一阶段。
            //   以前 `Target::linkHop` 只写不读（全模块没有消费点）⇒ 这些 link 从来没被执行过，
            //   队伍只会"按残路径直冲 boss、到末端不可达就跳过"。现在把 link 当成和 boss 同级的阶段。
            if (cur->kind == "link")
            {
                // [2026-10-26 修] 加载阶段 links 还没读进来（loader 顺序问题）⇒ 改成**每次按需匹配**：
                //   在 route.links 里找 src 离 (ax,ay) 最近且 ≤15 码的那条。links 只有几条，开销可忽略。
                RouteLink const* lk = nullptr;
                {
                    float bestD = 15.0f;
                    for (auto const& l : route.links)
                    {
                        float const dx = l.sx - cur->ax, dy = l.sy - cur->ay;
                        float const d = std::sqrt(dx * dx + dy * dy);
                        if (d < bestD)
                        {
                            bestD = d;
                            lk = &l;
                        }
                    }
                }
                if (!lk)
                {
                    sLog.outError("[STAGE] %s map=%u stage %u kind=link 但 (%.1f,%.1f) 15 码内没有 route_link ⇒ 跳过该阶段",
                        bot->GetName(), map->GetId(), uint32(cur->ord), cur->ax, cur->ay);
                    advance("link 未匹配到数据");
                    continue;
                }
                // [2026-10-27 站长口径] 走向跳点时用 **link src 的 z**，不用 bot 当前 z：
                //   之前 out.pz = bot->GetPositionZ()，bot 一旦在平台下层（水里），就会以水面高度
                //   朝 anchor 的 x/y 走，永远到不了 5 码起跳圈、也上不去（547 实测）。
                float const dSrc = bot->GetDistance(cur->ax, cur->ay, lk->sz);
                if (dSrc > 5.0f)
                {
                    out = Target();
                    out.guid = 0;
                    out.entry = 0;
                    out.ord = 0;
                    out.boss = false;
                    out.px = cur->ax;
                    out.py = cur->ay;
                    out.pz = lk->sz;
                    out.hasPullPoint = true;
                    return true;                          // 先走到连接点起点
                }
                // 到位：跳过去（kind 0/2 都是"跳/落"；MoveJump 不依赖寻路，能跨断崖/落水）
                bot->GetMotionMaster()->MoveJump(lk->dx, lk->dy, lk->dz, 12.0f, 1.0f);
                sLog.outError("[STAGE] %s map=%u 用连接点 stage %u：从 (%.1f,%.1f) 跳到 (%.1f,%.1f,%.1f)（kind=%u）",
                    bot->GetName(), map->GetId(), uint32(cur->ord),
                    cur->ax, cur->ay, lk->dx, lk->dy, lk->dz, uint32(lk->kind));
                st.stageWaitSince = 0;
                advance("连接点已跳");
                continue;
            }

            // [2026-10-26 站长口径] kind='wait'：原地干等 waitMs 再推进（等会自己消失的临时召唤怪走干净）
            if (cur->kind == "wait")
            {
                uint32 const nowW = now ? now : 1;
                if (!st.stageWaitSince)
                    st.stageWaitSince = nowW;
                if (WorldTimer::getMSTimeDiff(st.stageWaitSince, nowW) >= cur->waitMs)
                {
                    advance("干等结束");
                    continue;
                }
                out = Target();
                out.guid = 0;
                out.entry = 0;
                out.ord = 0;
                out.boss = false;
                out.px = cur->ax;
                out.py = cur->ay;
                out.pz = bot->GetPositionZ();
                out.hasPullPoint = true;
                return true;
            }

            if (cur->kind == "kill_entry" || cur->kind == "room_clear")
            {
                Creature* c = findMember(cur->targetEntry, cur->ax, cur->ay, cur->radius);
                // [2026-10-23] keep_out：太靠近 anchor 的怪先不打（269 清野兽时不能触发麦迪文事件）
                if (c && cur->keepOut > 0.0f)
                {
                    float const dx = c->GetPositionX() - cur->ax;
                    float const dy = c->GetPositionY() - cur->ay;
                    if (std::sqrt(dx * dx + dy * dy) < cur->keepOut)
                        c = nullptr;
                }
                if (!c)
                {
                    // [2026-10-23 修] 网格没加载时 creature 指针为空 ≠ 已清空。
                    //   只有"这个阶段确实见过活的、现在一只都没有"才算清完；否则先去该阶段区域。
                    if (!st.stageSeen.count(cur->ord))
                    {
                        float tx = 0, ty = 0, tz = 0;
                        bool has = false;
                        if (cur->radius > 0.0f)
                        {
                            tx = cur->ax; ty = cur->ay; tz = bot->GetPositionZ(); has = true;
                        }
                        else
                        {
                            for (auto const& kv : route.waves)
                            {
                                bool hit = false;
                                for (auto const& m : kv.second.members)
                                    if (m.entry == cur->targetEntry) { hit = true; break; }
                                if (hit && kv.second.hasCenter)
                                {
                                    tx = kv.second.cx; ty = kv.second.cy; tz = kv.second.cz; has = true;
                                    break;
                                }
                            }
                        }
                        if (has)
                        {
                            if (!st.stageLogAt || WorldTimer::getMSTimeDiff(st.stageLogAt, now ? now : 1) >= 5000)
                            {
                                st.stageLogAt = now ? now : 1;
                                sLog.outError("[STAGE] %s map=%u 阶段 %u 目标未加载 ⇒ 先去它所在区域 (%.0f,%.0f)",
                                    bot->GetName(), map->GetId(), uint32(cur->ord), tx, ty);
                            }
                            out = Target();
                            out.guid = 0;
                            out.entry = cur->targetEntry;
                            out.ord = 0;
                            out.boss = false;
                            out.px = tx; out.py = ty; out.pz = tz;
                            out.hasPullPoint = true;
                            return true;
                        }
                        return false;
                    }
                    advance("条件满足（已清空）");
                    continue;
                }

                st.stageSeen.insert(cur->ord);
                out = Target();
                out.guid = c->GetGUIDLow();
                out.entry = c->GetEntry();
                out.ord = 0;
                out.boss = false;
                out.px = c->GetPositionX();
                out.py = c->GetPositionY();
                out.pz = c->GetPositionZ();
                out.hasPullPoint = true;
                return true;
            }

            if (cur->kind == "boss")
            {
                BossSpawn const* bp = nullptr;
                for (auto const& b : route.bosses)
                    if (b.entry == cur->targetEntry) { bp = &b; break; }
                if (!bp) { advance("阶段无此 boss，跳过"); continue; }

                Creature* bc = map->GetCreature(bp->guid);
                if (bc && !bc->IsAlive()) { advance("boss 已死"); continue; }

                out = Target();
                out.guid = bp->guid;
                out.entry = bp->entry;
                out.ord = 0;
                out.boss = true;
                out.px = bc ? bc->GetPositionX() : bp->x;
                out.py = bc ? bc->GetPositionY() : bp->y;
                out.pz = bc ? bc->GetPositionZ() : bp->z;
                out.hasPullPoint = true;
                return true;
            }

            if (cur->kind == "waves_clear")
            {
                // [2026-10-23 站长指令] 例如 542 The Maker：拉开关后要清 4 波小怪（波次由事件刷出来）。
                //   以 anchor 为圆心、radius 内是否还有活怪判断"一波清空"；空场持续 3 秒记 1 波。
                float const r = cur->radius > 0.0f ? cur->radius : 45.0f;
                Creature* nearest = nullptr;
                float nd = 1e18f;
                uint32 alive = 0;
                auto scan = [&](uint32 g)
                {
                    Creature* c = map->GetCreature(g);
                    if (!c || !c->IsAlive() || !c->IsInWorld())
                        return;
                    if (!SessionCanFight(bot, c))
                        return;
                    float const dx = c->GetPositionX() - cur->ax;
                    float const dy = c->GetPositionY() - cur->ay;
                    if (std::sqrt(dx * dx + dy * dy) > r)
                        return;
                    ++alive;
                    float const d = bot->GetDistance(c);
                    if (d < nd) { nd = d; nearest = c; }
                };
                for (auto const& kv : route.guidOrd)
                    scan(kv.first);
                for (uint32 pg : route.patrolGuids)
                    scan(pg);

                if (alive > 0)
                {
                    st.waveClearSince = 0;
                    out = Target();
                    out.guid = nearest->GetGUIDLow();
                    out.entry = nearest->GetEntry();
                    out.ord = 0;
                    out.boss = false;
                    out.px = nearest->GetPositionX();
                    out.py = nearest->GetPositionY();
                    out.pz = nearest->GetPositionZ();
                    out.hasPullPoint = true;
                    return true;
                }

                if (!st.waveClearSince)
                {
                    st.waveClearSince = now ? now : 1;
                    // 空场：原地等着下一波（给个目标点，避免移动逻辑乱跑）
                    out = Target();
                    out.guid = 0;
                    out.entry = 0;
                    out.ord = 0;
                    out.boss = false;
                    out.px = cur->ax;
                    out.py = cur->ay;
                    out.pz = bot->GetPositionZ();
                    out.hasPullPoint = true;
                    return true;
                }
                if (WorldTimer::getMSTimeDiff(st.waveClearSince, now ? now : 1) >= 3000)
                {
                    ++st.waveClearCount;
                    st.waveClearSince = 0;
                    sLog.outError("[STAGE] %s map=%u waves_clear 第 %u/%u 波清空",
                        bot->GetName(), map->GetId(), st.waveClearCount, cur->countLimit);
                    if (cur->countLimit && st.waveClearCount >= cur->countLimit)
                    {
                        st.waveClearCount = 0;
                        advance("清波数达标");
                        continue;
                    }
                }
                out = Target();
                out.guid = 0;
                out.entry = 0;
                out.ord = 0;
                out.boss = false;
                out.px = cur->ax;
                out.py = cur->ay;
                out.pz = bot->GetPositionZ();
                out.hasPullPoint = true;
                return true;
            }

            if (cur->kind == "interact_go")
            {
                // [2026-10-23 站长指令] 开关/阀门：走到 GO 旁边再 Use（545 蒸汽地窟的两个 Access Panel）
                GameObject* useGo = nullptr;
                float bestD = 1e18f;
                auto it = route.goGuids.find(cur->targetEntry);
                if (it != route.goGuids.end())
                {
                    for (uint32 gguid : it->second)
                    {
                        GameObject* g = map->GetGameObject(ObjectGuid(HIGHGUID_GAMEOBJECT, cur->targetEntry, gguid));
                        if (!g)
                            continue;
                        float const d = bot->GetDistance(g);
                        if (d < bestD) { bestD = d; useGo = g; }
                    }
                }
                if (!useGo) { advance("GO 不存在/已用，跳过"); continue; }

                if (bestD > 5.0f)
                {
                    out = Target();
                    out.guid = 0;
                    out.entry = cur->targetEntry;
                    out.ord = 0;
                    out.boss = false;
                    out.px = useGo->GetPositionX();
                    out.py = useGo->GetPositionY();
                    out.pz = useGo->GetPositionZ();
                    out.hasPullPoint = true;
                    return true;
                }

                if (!st.stageLogAt || WorldTimer::getMSTimeDiff(st.stageLogAt, now ? now : 1) >= 5000)
                {
                    st.stageLogAt = now ? now : 1;
                    sLog.outError("[STAGE] %s map=%u 使用开关 entry=%u dist=%.1f",
                        bot->GetName(), map->GetId(), cur->targetEntry, bestD);
                }
                useGo->Use(bot);
                // [2026-10-24 修] 开关是一次性的：用掉就推进阶段（以前不推进 ⇒ 542/545 卡在"使用开关"反复刷）
                advance("已使用开关");
                continue;
            }

            if (cur->kind == "talk_npc")
            {
                // [2026-10-23 站长指令] 和 NPC 对话（进度链的一个 stage）：走到 5 码内 → gossip hello → 记完成
                Creature* npc = nullptr;
                float bestD = 1e18f;
                auto it = route.npcGuids.find(cur->targetEntry);
                if (it != route.npcGuids.end())
                {
                    for (uint32 nguid : it->second)
                    {
                        Creature* c = map->GetCreature(nguid);
                        if (!c || !c->IsAlive() || !c->IsInWorld())
                            continue;
                        float const d = bot->GetDistance(c);
                        if (d < bestD) { bestD = d; npc = c; }
                    }
                }
                if (!npc)
                {
                    // NPC 还没进网格：朝它的刷点走（用 DB 坐标不可得，这里退化为等待并记录）
                    advance("NPC 未加载/不存在，跳过");
                    continue;
                }
                if (bestD > 5.0f)
                {
                    out = Target();
                    out.guid = npc->GetGUIDLow();
                    out.entry = npc->GetEntry();
                    out.ord = 0;
                    out.boss = false;
                    out.px = npc->GetPositionX();
                    out.py = npc->GetPositionY();
                    out.pz = npc->GetPositionZ();
                    out.hasPullPoint = true;
                    return true;
                }

                if (!st.stageLogAt || WorldTimer::getMSTimeDiff(st.stageLogAt, now ? now : 1) >= 5000)
                {
                    st.stageLogAt = now ? now : 1;
                    sLog.outError("[STAGE] %s map=%u 对话 NPC entry=%u guid=%u dist=%.1f",
                        bot->GetName(), map->GetId(), npc->GetEntry(), npc->GetGUIDLow(), bestD);
                }
                {
                    WorldPacket gp;
                    gp << npc->GetObjectGuid();
                    bot->GetSession()->HandleGossipHelloOpcode(gp);
                }
                // [2026-10-23 站长指令] 需要"选一个选项才给东西"的 NPC（如 Sa'at 给时空道标）：
                //   count_limit 当"选项序号"用；先 hello 再 select
                if (cur->countLimit)
                {
                    WorldPacket sp;
                    uint32 const menuId = bot->GetPlayerMenu() ? bot->GetPlayerMenu()->GetGossipMenu().GetMenuId() : 0;
                    std::string code;
                    sp << npc->GetObjectGuid() << menuId << uint32(cur->countLimit) << code;
                    bot->GetSession()->HandleGossipSelectOptionOpcode(sp);
                    sLog.outError("[STAGE] %s 对话选项 NPC entry=%u menu=%u option=%u",
                        bot->GetName(), npc->GetEntry(), menuId, cur->countLimit);
                }
                advance("对话完成");
                continue;
            }

            if (cur->kind == "gauntlet")
            {
                // [2026-10-23 站长指令] 540 天堂之路：一边无限刷怪一边往前走，把 Porung 引进战斗；
                //   引来的怪太多（>= count_limit）就先清，否则会灭团。
                uint32 crowd = 0;
                Creature* nearestHostile = nullptr;
                float nearestHostileDist = 1e18f;
                float const crowdR = cur->radius > 0.0f ? cur->radius : 30.0f;
                auto countOne = [&](uint32 g)
                {
                    Creature* c = map->GetCreature(g);
                    if (!c || !c->IsAlive() || !c->IsInWorld())
                        return;
                    if (!SessionCanFight(bot, c))
                        return;
                    if (bot->GetDistance(c) > crowdR)
                        return;
                    ++crowd;
                    float const d = bot->GetDistance(c);
                    if (d < nearestHostileDist) { nearestHostileDist = d; nearestHostile = c; }
                };
                for (auto const& kv : route.guidOrd)
                    countOne(kv.first);
                for (uint32 pg : route.patrolGuids)
                    countOne(pg);

                if (cur->countLimit && crowd >= cur->countLimit && nearestHostile)
                {
                    out = Target();
                    out.guid = nearestHostile->GetGUIDLow();
                    out.entry = nearestHostile->GetEntry();
                    out.ord = 0;
                    out.boss = false;
                    out.px = nearestHostile->GetPositionX();
                    out.py = nearestHostile->GetPositionY();
                    out.pz = nearestHostile->GetPositionZ();
                    out.hasPullPoint = true;
                    return true;
                }

                // 人不多 ⇒ 继续往前，把目标 boss 引进来（目标不是 boss 时直接朝 anchor 走）
                BossSpawn const* bp = nullptr;
                for (auto const& b : route.bosses)
                    if (b.entry == cur->targetEntry) { bp = &b; break; }
                if (!bp)
                {
                    // 例：540 普通难度用的是 Shattered Hand Blood Guard(17461)，不在 boss_spawn 里
                    if (cur->ax == 0.0f && cur->ay == 0.0f) { advance("gauntlet 无目标点"); continue; }
                    float const dToAnchor = bot->GetDistance(cur->ax, cur->ay, bot->GetPositionZ());
                    if (dToAnchor <= 8.0f) { advance("gauntlet 已到目标点"); continue; }
                    out = Target();
                    out.guid = 0;
                    out.entry = cur->targetEntry;
                    out.ord = 0;
                    out.boss = false;
                    out.px = cur->ax;
                    out.py = cur->ay;
                    out.pz = bot->GetPositionZ();
                    out.hasPullPoint = true;
                    return true;
                }
                Creature* bc = map->GetCreature(bp->guid);
                if (bc && !bc->IsAlive()) { advance("gauntlet 目标 boss 已死"); continue; }

                out = Target();
                out.guid = bp->guid;
                out.entry = bp->entry;
                out.ord = 0;
                out.boss = true;
                out.px = bc ? bc->GetPositionX() : bp->x;
                out.py = bc ? bc->GetPositionY() : bp->y;
                out.pz = bc ? bc->GetPositionZ() : bp->z;
                out.hasPullPoint = true;
                return true;
            }

            if (cur->kind == "walk_to")
            {
                // [2026-10-23 站长指令] 只走到指定点（例如走到麦迪文旁边触发事件），到了就推进
                if (cur->ax == 0.0f && cur->ay == 0.0f) { advance("walk_to 无坐标"); continue; }
                if (bot->GetDistance(cur->ax, cur->ay, bot->GetPositionZ()) <= 8.0f)
                {
                    advance("已到达目标点");
                    continue;
                }
                out = Target();
                out.guid = 0;
                out.entry = 0;
                out.ord = 0;
                out.boss = false;
                out.px = cur->ax;
                out.py = cur->ay;
                out.pz = bot->GetPositionZ();
                out.hasPullPoint = true;
                return true;
            }

            if (cur->kind == "event_guard")
            {
                // [2026-10-23 站长指令] 黑色沼泽：以麦迪文/时空门为中心只打半径内的怪；
                //   优先拦"正在追麦迪文"的怪；有怪打麦迪文就放时空道标（物品 24289 → 法术 31479）。
                float const r = cur->radius > 0.0f ? cur->radius : 60.0f;
                Creature* nearest = nullptr;
                float nd = 1e18f;
                Creature* chasing = nullptr;
                float cd = 1e18f;
                bool medivhUnderAttack = false;
                auto scan = [&](uint32 g)
                {
                    Creature* c = map->GetCreature(g);
                    if (!c || !c->IsAlive() || !c->IsInWorld())
                        return;
                    if (!SessionCanFight(bot, c))
                        return;
                    float const dx = c->GetPositionX() - cur->ax;
                    float const dy = c->GetPositionY() - cur->ay;
                    if (std::sqrt(dx * dx + dy * dy) > r)
                        return;
                    bool const isChasing = (c->GetVictim() && c->GetVictim()->GetEntry() == 15608);
                    if (isChasing)
                    {
                        medivhUnderAttack = true;
                        float const d0 = bot->GetDistance(c);
                        if (d0 < cd) { cd = d0; chasing = c; }
                    }
                    float const d = bot->GetDistance(c);
                    if (d < nd) { nd = d; nearest = c; }
                };
                for (auto const& kv : route.guidOrd)
                    scan(kv.first);
                for (uint32 pg : route.patrolGuids)
                    scan(pg);

                if (medivhUnderAttack && !st.beaconAt)
                {
                    st.beaconAt = now ? now : 1;
                    sLog.outError("[GUARD] %s 有怪在打麦迪文 ⇒ 放时空道标(24289/31479)", bot->GetName());
                    bot->CastSpell(cur->ax, cur->ay, bot->GetPositionZ(), 31479, uint32(0));
                }
                if (st.beaconAt && WorldTimer::getMSTimeDiff(st.beaconAt, now ? now : 1) >= 60000)
                    st.beaconAt = 0;   // 一分钟后再评估

                Creature* pick = chasing ? chasing : nearest;
                if (pick)
                {
                    out = Target();
                    out.guid = pick->GetGUIDLow();
                    out.entry = pick->GetEntry();
                    out.ord = 0;
                    out.boss = false;
                    out.px = pick->GetPositionX();
                    out.py = pick->GetPositionY();
                    out.pz = pick->GetPositionZ();
                    out.hasPullPoint = true;
                    return true;
                }
                // 没有怪：原地守着（不推进，事件会继续刷）
                out = Target();
                out.guid = 0;
                out.entry = 0;
                out.ord = 0;
                out.boss = false;
                out.px = cur->ax;
                out.py = cur->ay;
                out.pz = bot->GetPositionZ();
                out.hasPullPoint = true;
                return true;
            }

            advance("未知阶段类型，跳过");
        }
        return false;
    }

    // [2026-10-23 站长指令] "先拉这一只、别引太多"：
    //   如果有指定 entry 的活怪在 max_dist 内、LOS 可见 ⇒ 用它作为**唯一开怪目标**（忽略波次）；
    //   它周围 15 码内其它活怪超过 max_adds ⇒ hold=true（先别拉，等/退，避免一次引一堆）。
    inline bool PullFirstTarget(Player* bot, Map* map, MapRoute const& route, Target& out, bool& hold)
    {
        hold = false;
        if (route.pullFirst.empty() || !map || !bot)
            return false;
        if (bot->IsInCombat())
            return false;

        for (auto const& rule : route.pullFirst)
        {
            Creature* best = nullptr;
            float bestD = 1e18f;
            auto consider = [&](uint32 g)
            {
                Creature* c = map->GetCreature(g);
                if (!c || !c->IsAlive() || !c->IsInWorld())
                    return;
                if (c->GetEntry() != rule.entry)
                    return;
                float const d = bot->GetDistance(c);
                if (d > rule.maxDist)
                    return;
                if (!bot->IsWithinLOS(c->GetPositionX(), c->GetPositionY(), c->GetPositionZ()))
                    return;
                if (d < bestD) { bestD = d; best = c; }
            };
            for (auto const& kv : route.guidOrd)
                consider(kv.first);
            for (uint32 pg : route.patrolGuids)
                consider(pg);
            if (!best)
                continue;

            // [2026-10-23 站长口径] 用"连锁范围 + 连锁组"判断会一起进战斗的怪，不用硬性距离：
            //   连锁范围 = 目标的 CallForHelp（>0 时）否则 CreatureFamilyAssistanceRadius（默认 5）；
            //   连锁组   = 核心自己的判定 AnyAssistCreatureInRangeCheck（和怪实际求援的规则一致）。
            float chainRange = sWorld.getConfig(CONFIG_FLOAT_CREATURE_FAMILY_ASSISTANCE_RADIUS);
            CreatureInfo const* ci = best->GetCreatureInfo();
            if (ci && ci->CallForHelp > 0)
                chainRange = float(ci->CallForHelp);

            uint32 adds = 0;
            {
                CreatureList receivers;
                MaNGOS::AnyAssistCreatureInRangeCheck u_check(best, bot, chainRange);
                MaNGOS::CreatureListSearcher<MaNGOS::AnyAssistCreatureInRangeCheck> searcher(receivers, u_check);
                Cell::VisitAllObjects(best, searcher, chainRange);
                adds = uint32(receivers.size());
            }

            if (adds > rule.maxAdds)
            {
                hold = true;
                return true;      // 先别拉（等巡逻走开/退一点）
            }

            out = Target();
            out.guid = best->GetGUIDLow();
            out.entry = best->GetEntry();
            out.ord = 0;
            out.boss = false;
            // 就地起手（远距离拉），不再走到怪堆里
            out.px = bot->GetPositionX();
            out.py = bot->GetPositionY();
            out.pz = bot->GetPositionZ();
            out.hasPullPoint = true;
            return true;
        }
        return false;
    }

    inline bool FindTargetNearest(Player* bot, MapRoute const& route, Target& out,
                                  bool onlyCurrentWave = false)
    {
        Map* map = bot->GetMap();
        if (!map || !bot)
            return false;

        BotState& st = StateOf(bot);
        RefreshWaveKnowledge(bot, map, route, st);
        RefreshBossKnowledge(map, route, st);

        // [2026-10-23 站长指令] 有进度链的图：先按 stage 推进（只打该打的），阶段全部完成才回落到通用逻辑
        if (!route.stages.empty() && StageTarget(bot, map, route, st, out))
            return true;

        // [2026-10-22 站长口径] BOSS 优先模式的两条规矩：
        //   ① 路上遇到**30 码内、可拉**的敌对怪/敌对该波 ⇒ 先打它（不绕过、不硬冲 boss）；
        //   ② 附近没有可拉的怪 ⇒ 才按 boss 顺序去可达 boss；boss 不可达/挂起 ⇒ 落回波次/特殊寻路。
        bool const bossMode = BossFirstMode();

        // [2026-10-25 站长口径] **现场在前、表退到辅助**：
        //   ① 先看这个副本此刻刷出来的怪（CreatureGroup 分组，最近的一组先打）；
        //   ② 现场没活怪 ⇒ 用最近的 boss 刷点当"路标"继续往里走（走过去网格就会加载出下一批）；
        //   ③ 再不行才回落到旧的波次/连接点逻辑（那些只当兜底走位，不再决定打谁）。
        if (bossMode)
        {
            bool inFight = bot->IsInCombat();
            if (!inFight && bot->GetGroup())
            {
                for (GroupReference* ref = bot->GetGroup()->GetFirstMember(); ref; ref = ref->next())
                {
                    Player* gm = ref->getSource();
                    if (gm && gm->IsAlive() && gm->IsInCombat())
                    {
                        inFight = true;
                        break;
                    }
                }
            }
            if (!inFight && FindLiveGroupTarget(bot, map, route, st, out))
                return true;
            if (!inFight && FindBossWaypoint(bot, route, st, out))
                return true;
        }

        // [2026-10-19 543 实测] 拉怪/移动只瞄 CurrentOrd（当前波）；否则会按“全图最近”
        //   跨波乱拉，和攻击目标过滤（唯一当前波）打架，出现“15s 无目标 → 跳波”。
        uint16 const curOrd = onlyCurrentWave ? CurrentOrd(bot, route) : 0;

        // [2026-10-24 诊断] 当前波到底还有几只活着、中心在哪（553/557 卡"0 波"时看这一行）
        if (onlyCurrentWave)
        {
            uint32 const nowW = WorldTimer::getMSTime();
            if (!st.waveLogAt || WorldTimer::getMSTimeDiff(st.waveLogAt, nowW ? nowW : 1) >= 10000)
            {
                st.waveLogAt = nowW ? nowW : 1;
                uint32 aliveN = 0;
                float cx = 0, cy = 0;
                std::string detail;   // [2026-10-24] 活口明细：guid(entry)@(x,y) dist_to_center
                auto wit = route.waves.find(curOrd);
                bool hasC = false;
                if (wit != route.waves.end())
                {
                    for (auto const& m : wit->second.members)
                    {
                        Creature* mc = MemberFightable(map, m) ? MemberCreature(map, m) : nullptr;
                        if (!mc)
                            continue;
                        ++aliveN;
                        if (aliveN <= 4)
                        {
                            float const dcx = mc->GetPositionX() - wit->second.cx;
                            float const dcy = mc->GetPositionY() - wit->second.cy;
                            float const dc = std::sqrt(dcx * dcx + dcy * dcy);
                            detail += " " + std::to_string(m.guid) + "(" + std::to_string(m.entry) + ")@" +
                                std::to_string(int32(mc->GetPositionX())) + "," +
                                std::to_string(int32(mc->GetPositionY())) + " d=" + std::to_string(uint32(dc));
                        }
                    }
                    cx = wit->second.cx; cy = wit->second.cy; hasC = wit->second.hasCenter;
                }
                sLog.outError("[WAVE] %s map=%u curOrd=%u alive=%u center=(%.0f,%.0f) hasCenter=%d lockOrd=%u pos=(%.0f,%.0f)%s",
                    bot->GetName(), map->GetId(), uint32(curOrd), aliveN, cx, cy, hasC ? 1 : 0,
                    uint32(st.lockOrd), bot->GetPositionX(), bot->GetPositionY(), detail.c_str());
            }
        }

        const float STOP_DIST = 30.0f;       // 见敌即停（保留）
        const float PULL_ENGAGE_DIST = 30.0f; // 站长口径：停步点保留 30 码，拉不到由拉怪策略自己找位置/走近
        const float APPROACH_DIST = 22.0f;   // 走到离怪多近（沿用旧的 22 码）


        Member nearestM;                     // 最近的"活着的、可攻击的"怪
        float nearestAliveDist = 1e18f;
        bool nearestAliveVisible = false;
        uint16 nearestAliveOrd = 0;
        // [寻路修 2026-10-22] 不限距离的"本波最近活成员"：只用来决定**往哪走**。
        //   巡逻怪会走开、静态锚点会落在空地（546 实测站在锚点上 tg=0 不动）⇒ 没有可拉目标时朝它走过去。
        Member nearestAnyM;
        float nearestAnyDist = 1e18f;
        uint16 nearestAnyOrd = 0;

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
            nearestAnyM = Member();
            nearestAnyDist = 1e18f;
            nearestAnyOrd = 0;

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
                    // [2026-10-23] 已被判定"不可攻击/永远无敌"的目标不再选（否则整队站着发呆）
                    if (st.unattackable.count(m.guid))
                        continue;
                    // [2026-10-24] 这只刚"消失/打不到"过 ⇒ 60 秒内先换下一个（按 guid，不按 entry）
                    {
                        auto sk = st.guidSkipUntil.find(m.guid);
                        if (sk != st.guidSkipUntil.end())
                        {
                            if (sk->second > WorldTimer::getMSTime())
                                continue;
                            st.guidSkipUntil.erase(sk);
                        }
                    }
                    // [2026-10-23 站长口径] 无敌+击飞类怪不拉（按躲地板处理，交给 avoid specific creatures）
                    if (route.avoidEntries.count(c->GetEntry()))
                        continue;
                    // [2026-10-24 站长指令] 会消失/打不到的怪：直接跳过
                    if (route.skipEntries.count(c->GetEntry()))
                        continue;
                    if (!SessionCanFight(bot, c))
                        continue;                       // 友好 NPC / 小动物：不参与推进

                    // [2026-10-22 站长口径] 取消"跳过水里怪"：现在 mmap/游泳已经能推进水中的怪，
                    //   再过滤掉它们会让整波永远清不掉（546/547 实测卡 90 秒后整波跳过）。

                    // [全图清单] 清单里明确非敌对（小动物/任务 NPC）的跳过；
                    //   运行期动态刷怪不在清单里 ⇒ 保留（不能误杀真正要打的波）。
                    bool rosterHostile = true;
                    if (RosterLookup(map->GetId(), m.guid, rosterHostile) && !rosterHostile)
                        continue;

                    float d = bot->GetDistance(c);

                    // [2026-10-22 站长口径] 540 特殊策略：按"附近同类怪数量"动态调整权重。
                    //   数量 < engage_count ⇒ 无视它继续推进（不站下来打）；
                    //   数量 >= engage_count ⇒ 正常打，且数量越多权重越高（等效距离缩短）。
                    auto cwit = route.countWeights.find(c->GetEntry());
                    if (cwit != route.countWeights.end())
                    {
                        uint32 const near = CountNearbyEntry(map, route, c->GetEntry(),
                            c->GetPositionX(), c->GetPositionY(), c->GetPositionZ(), cwit->second.radius);
                        if (near < EngageCount(cwit->second))
                            continue;
                        d = d / (1.0f + cwit->second.weightPerCount * float(near));
                    }

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
                                st.patrolWaitTargetGuid = c->GetGUIDLow();
                                st.patrolWaitAt = 0;
                                st.patrolWaitSince = 0;
                                st.patrolWaitLogAt = 0;
                            }
                            continue;
                        }
                    }

                    st.seenAlive.insert(waveOrd);

                    // [寻路修 2026-10-22] 记录"本波最近的活成员（不限距离）"，只用于走位。
                    //   放在这里是因为上面已经滤掉水里/非敌对/会 ADD 的怪。
                    if (d < nearestAnyDist)
                    {
                        nearestAnyDist = d;
                        nearestAnyM = m;
                        nearestAnyOrd = waveOrd;
                    }

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
                        sLog.outError("[DUNGEONROUTE] %s 30码内有可拉怪但会引到巡逻怪 %s ⇒ 原地等它走开（%us/%us）",
                            bot->GetName(), patrol->GetName(),
                            uint32(WorldTimer::getMSTimeDiff(st.patrolWaitSince, waitNow) / 1000),
                            uint32(maxWaitMs / 1000));
                    }
                }
                else
                {
                    if (!stillThere)
                        sLog.outError("[DUNGEONROUTE] %s 巡逻怪已走开 ⇒ 继续拉怪", bot->GetName());
                    else if (dw::Get("pull.safe.strict") <= 0.0f)
                    {
                        // 宽松模式（cheat 验证寻路）：等满仍挡路就**直接拉**，并把这只巡逻怪忽略 20 秒，
                        //   否则会陷入"等 30s→放弃→再选回同一只→再等"死循环（555 实测 900 秒零推进）。
                        sLog.outError("[DUNGEONROUTE] %s 等巡逻怪 %us 仍不安全 ⇒ 宽松模式：直接拉（该巡逻忽略 20s）",
                            bot->GetName(), uint32(maxWaitMs / 1000));
                        st.patrolIgnoreGuid = st.patrolWaitGuid;
                        st.patrolIgnoreUntil = waitNow + 20000;
                    }
                    else
                    {
                        sLog.outError("[DUNGEONROUTE] %s 等巡逻怪 %us 仍不安全 ⇒ 跳过这只所在波，继续推进",
                            bot->GetName(), uint32(maxWaitMs / 1000));
                        st.patrolIgnoreGuid = st.patrolWaitGuid;
                        st.patrolIgnoreUntil = waitNow + 20000;
                        if (st.patrolWaitTargetGuid)
                        {
                            auto oit = route.guidOrd.find(st.patrolWaitTargetGuid);
                            if (oit != route.guidOrd.end() && oit->second)
                                MarkSkipped(bot, oit->second, "patrol wait 30s");
                        }
                    }
                    st.patrolWaitGuid = 0;
                    st.patrolWaitAt = 0;
                    st.patrolWaitSince = 0;
                    st.patrolWaitLogAt = 0;
                    st.patrolWaitTargetGuid = 0;
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
            if (!pc || !pc->IsAlive() || !SessionCanFight(bot, pc))    // [2026-10-22] 水里怪不再排除
                continue;
            // [2026-10-23 站长口径] 无敌+击飞类怪（avoid_mob）即使登记成巡逻，也不去拉
            if (route.avoidEntries.count(pc->GetEntry()))
                continue;
            // [2026-10-24 站长指令] 会消失/打不到的怪：直接跳过
            if (route.skipEntries.count(pc->GetEntry()))
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
            // [2026-10-24 修] 当前波还没清完 ⇒ **不许切 boss**。
            //   实测：553 curOrd=1 却朝 300 码外的 boss 走、557/552 同样，结果当前波永远清不掉。
            uint16 const curForBoss = CurrentOrd(bot, route);
            bool waveLeft = false;
            if (curForBoss)
            {
                auto wit = route.waves.find(curForBoss);
                if (wit != route.waves.end() && WaveHasFightable(bot, wit->second))
                    waveLeft = true;
            }
            // [2026-10-24 站长口径] "进入战斗就应该先解决进入战斗的，战斗后再继续朝 boss 走"：
            //   队伍里任何人在战斗中 ⇒ 不切 boss 目标（否则坦克会把队伍拖着往 boss 走，打断当前战斗）。
            bool groupInCombat = bot->IsInCombat();
            if (!groupInCombat && bot->GetGroup())
            {
                for (GroupReference* ref = bot->GetGroup()->GetFirstMember(); ref; ref = ref->next())
                {
                    Player* gm = ref->getSource();
                    if (gm && gm->IsAlive() && gm->IsInCombat())
                    {
                        groupInCombat = true;
                        break;
                    }
                }
            }

            if (!waveLeft && !groupInCombat)
            {
                if (FindBossTarget(bot, route, st, out))
                    return true;
                // [2026-10-22 站长口径] 所有 boss 都不可达 ⇒ 按 link 跳到下一个连通区间，跳完再重查 boss
                if (FindLinkExplorationTarget(bot, route, st, out))
                    return true;
            }
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

        // [寻路修A 2026-10-22] 没有可拉目标 ⇒ 先朝"本波最近活成员的实时坐标"走。
        //   原因：巡逻波（546 第一波这种）没有固定锚点，静态锚点/波心会落在空地，
        //   实测队伍站在锚点上 tg=0 一动不动。走到 30 码内会重新命中上面的"有活目标"分支。
        if (!nearestM.guid && nearestAnyM.guid)
        {
            Creature* c = map->GetCreature(nearestAnyM.guid);
            if (c && c->IsAlive() && c->IsInWorld())
            {
                out = Target();
                out.guid = nearestAnyM.guid;
                out.entry = nearestAnyM.entry;
                out.ord = nearestAnyOrd;
                out.boss = false;
                MakeApproachPoint(bot, c->GetPositionX(), c->GetPositionY(), c->GetPositionZ(), out);
                out.hasPullPoint = true;
                return true;
            }
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

            // [2026-10-23 站长口径] "沿路线往前走、周围有怪就拉"：
            //   如果我们已经站在本波拉怪位上却**没有可拉的怪**（nearestM/nearestAny 都空），
            //   说明这波现在打不到（未刷新/不可达）⇒ 不要原地守着，直接推进到下一波。
            if (d <= AT_PULL_POINT_DIST && st.lockOrd && waveOrd == st.lockOrd)
                continue;

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
            // 推进到下一波时同步锁定 ord，保证"沿路线往前走"的顺序
            st.lockOrd = goalOrd;
            st.lockUntil = nowMs + LOCK_MS;

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
                sLog.outError("[BOSSKILL] map=%u inst=%u entry=%u guid=%u", map->GetId(),
                    map->GetInstanceId(), b.entry, b.guid);
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


    // [2026-10-25 修] PULLSAFE 挪出来的"安全拉怪点"= **本次拉怪的站位点**。
    //   生效期间（未过期、未进战斗、离原拉怪位 ≤60 码）返回它，调用方拿它当拉怪位判断"到没到位"；
    //   旧逻辑只把它当"拉完之后别回旧位"，结果拉怪动作在安全点上被 stand_override_hold 挡住、
    //   移动动作又按原拉怪位把坦克推回去 —— 两个动作互顶，永远不开怪。
    inline bool SafePullPoint(Player* bot, Target const& target, float& x, float& y, float& z)
    {
        if (!bot)
            return false;
        BotState& st = StateOf(bot);
        if (!st.safePullPoint)
            return false;

        uint32 const now = WorldTimer::getMSTime();
        if (!st.standOverrideUntil || (now && now >= st.standOverrideUntil))
        {
            st.safePullPoint = false;
            return false;
        }
        if (bot->IsInCombat())                 // 战斗中由挪位/站位逻辑接管，安全点不再当拉怪位
            return false;
        if (!target.hasPullPoint || bot->GetDistance(target.px, target.py, target.pz) > 60.0f)
            return false;                      // 拉怪位换波了 / 队伍被兜底传回入口后留下的旧点

        x = st.standOverrideX;
        y = st.standOverrideY;
        z = st.standOverrideZ;
        return true;
    }

    // ---- 驱动资格（"谁是驱动坦克"）------------------------------------------------
    // ① 坦克 bot；② 队伍里没有"真人坦克"；③ 多个 bot 坦克时 guid 最小的那个（避免抢拉）
    // [GHOST-FIGHT 2026-10-27] "队伍真的在挨打吗"：任何成员/宠物身上有 attacker（= 有怪正在打我们）。
    //   幽灵战斗的现场是 **atkM=0**（谁都没在打我们），这时 `IsInCombat` 只是战斗标记没散，
    //   不该继续挡住路线推进（[GHOSTCOMBAT] r128/r130 实测：整组零攻击者、零推进）。
    inline bool GroupUnderAttack(Player* bot)
    {
        if (!bot)
            return false;

        auto attacked = [](Player* p) -> bool
        {
            if (!p || !p->IsAlive() || !p->IsInWorld())
                return false;
            if (!p->getAttackers().empty())
                return true;
            if (Unit* pet = p->GetPet())
                if (!pet->getAttackers().empty())
                    return true;
            return false;
        };

        if (attacked(bot))
            return true;
        if (Group* g = bot->GetGroup())
        {
            for (GroupReference* ref = g->GetFirstMember(); ref; ref = ref->next())
                if (attacked(ref->getSource()))
                    return true;
        }
        return false;
    }

    // [2026-10-28 站长口径] **全员一起行动**：非战斗状态下，只要队里还有人没到位就停住等。
    //   "没到位" = 有人死亡（等复活跑回）或存活队员离坦克 > 35 码。
    //   这里只填"掉队者详情"（guid/距离/死活），日志与"等太久就 summon"由 SimScenario 处理。
    inline bool GroupNeedsRegroup(Player* bot, BotState& st)
    {
        if (!bot)
            return false;
        Group* grp = bot->GetGroup();
        if (!grp)
            return false;
        // 优先报"死亡"，其次报"最远"的那个
        ObjectGuid farGuid;
        float farDist = 0.0f;
        for (GroupReference* ref = grp->GetFirstMember(); ref; ref = ref->next())
        {
            Player* m = ref->getSource();
            if (!m || m == bot || m->GetMapId() != bot->GetMapId())
                continue;
            if (!m->IsAlive())
            {
                st.regroupStraggler = m->GetObjectGuid();
                st.regroupStragglerDead = true;
                st.regroupDist = 0.0f;
                return true;
            }
            float const d = bot->GetDistance(m);
            if (d > 35.0f && d > farDist)
            {
                farDist = d;
                farGuid = m->GetObjectGuid();
            }
        }
        if (!farGuid.IsEmpty())
        {
            st.regroupStraggler = farGuid;
            st.regroupStragglerDead = false;
            st.regroupDist = farDist;
            return true;
        }
        return false;
    }

    // [DRIVEFAIL 2026-11-02] 可观测性补丁：ShouldDrive() 每处 return false 都打一条带"门名"的日志。
    //   原来外部只能看到 driver=(-)，分不清被哪一道门挡住。这里**只加观测**，不改任何判断。
    //   节流：同一只 bot（guidLow）每 10 秒最多一条 —— ShouldDrive 每 tick 都被调，不节流会刷屏。
    //   bot 可能为 nullptr（函数开头 ai 为空的分支），此时按 key=0 记录、名字打 "?"。
    inline void LogDriveFail(Player* bot, char const* reason)
    {
        uint32 const nowMsRaw = WorldTimer::getMSTime();
        uint32 const nowMs = nowMsRaw ? nowMsRaw : 1;   // 0 当 1，避免和"从未打印过"的哨兵撞车
        uint32 const key = bot ? bot->GetGUIDLow() : 0;

        static std::mutex s_lock;
        static std::unordered_map<uint32, uint32> s_lastMs;
        {
            std::lock_guard<std::mutex> guard(s_lock);
            std::unordered_map<uint32, uint32>::const_iterator it = s_lastMs.find(key);
            if (it != s_lastMs.end() && WorldTimer::getMSTimeDiff(it->second, nowMs) < 10000)
                return;
            s_lastMs[key] = nowMs;
        }

        char const* name = (bot && bot->GetName()) ? bot->GetName() : "?";
        sLog.outError("[DRIVEFAIL] %s reason=%s", name, reason);
    }

    inline bool ShouldDrive(PlayerbotAI* ai)
    {
        if (!RouteEnabled())
        {
            LogDriveFail(nullptr, "route-disabled");
            return false;
        }

        Player* bot = ai ? ai->GetBot() : nullptr;
        // [DRIVEFAIL] 原为一条 `!bot || !IsInWorld() || IsBeingTeleported() || !IsAlive()` 短路链，
        //   这里按**同样的顺序、同样的短路语义**逐步展开，只为让日志能指出是哪一道门（行为不变）。
        if (!bot)
        {
            LogDriveFail(nullptr, "no-bot");
            return false;
        }
        if (!bot->IsInWorld())
        {
            LogDriveFail(bot, "not-in-world");
            return false;
        }
        if (bot->IsBeingTeleported())
        {
            LogDriveFail(bot, "teleporting");
            return false;
        }
        if (!bot->IsAlive())
        {
            LogDriveFail(bot, "dead");
            return false;
        }

        // [自动进本 2026-10-18] 不在本里 ⇒ 发起"带队伍进本"，本 tick 不推进路线
        if (!dungeonrun::MaybeEnterDungeon(bot, RouteRuntimeSwitch()))
        {
            LogDriveFail(bot, "not-entered");
            return false;
        }
        if (bot->IsInCombat())
        {
            // [GHOST-FIGHT 2026-10-27] 只剩战斗标记（没有 victim、队伍/宠物也没被任何怪打）时，
            //   给 10 秒确认窗口；确认是幽灵战斗就**不再用 IsInCombat 挡推进**。
            //   不主动清玩家战斗标记（那只是派生结果）：核心侧 CombatManager 的 30 秒无 victim
            //   兜底会把怪脱战、引用自然消失，这里只是别让整队陪着干等。
            BotState& st = StateOf(bot);
            uint32 const nowMs = WorldTimer::getMSTime();
            bool const ghost = !bot->GetVictim() && !GroupUnderAttack(bot);
            if (!ghost)
            {
                st.ghostCombatSince = 0;
                LogDriveFail(bot, "combat");
                return false;
            }
            if (!st.ghostCombatSince)
                st.ghostCombatSince = nowMs ? nowMs : 1;
            if (WorldTimer::getMSTimeDiff(st.ghostCombatSince, nowMs ? nowMs : 1) < 10000)
            {
                LogDriveFail(bot, "ghost-wait");
                return false;
            }
        }
        else
            StateOf(bot).ghostCombatSince = 0;
        if (!PlayerbotAI::IsTank(bot))
        {
            LogDriveFail(bot, "not-tank");
            return false;
        }

        // [2026-10-28 站长口径] **全员一起行动**：脱战且有人阵亡/掉队 ⇒ 停住等。
        //   这里只置状态、不推进；"等太久就把全队拉到坦克身边（summon）"由 SimScenario 负责。
        {
            BotState& rst = StateOf(bot);
            uint32 const nowMs = WorldTimer::getMSTime();
            if (bot->IsInCombat() || !GroupNeedsRegroup(bot, rst))
                rst.regroupSince = 0;
            else
            {
                if (!rst.regroupSince)
                    rst.regroupSince = nowMs ? nowMs : 1;
                LogDriveFail(bot, "regroup-wait");
                return false;
            }
        }

        Map* map = bot->GetMap();
        if (!map || (!map->IsDungeon() && !map->IsRaid()))
        {
            LogDriveFail(bot, "not-dungeon");
            return false;
        }

        MapRoute const* route = GetMapRoute(map->GetId());
        if (!route)
        {
            LogDriveFail(bot, "no-route");
            return false;
        }
        if (route->waves.empty())
        {
            LogDriveFail(bot, "route-empty");
            return false;
        }
        // [自动进本] 自动进本模式下允许未 verified 的图
        if (!route->verified && !dungeonrun::AllowUnverified(RouteRuntimeSwitch()))
        {
            LogDriveFail(bot, "unverified");
            return false;
        }

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
                    {
                        LogDriveFail(bot, "human-tank");
                        return false;
                    }
                    if (m->GetGUIDLow() < bot->GetGUIDLow())      // 别的 bot 坦克更小：让它拉
                    {
                        LogDriveFail(bot, "other-tank");
                        return false;
                    }
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
