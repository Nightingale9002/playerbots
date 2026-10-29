#include "playerbot/playerbot.h"
#include "playerbot/SimScenario.h"
#include "playerbot/AiFactory.h"
#include "playerbot/PlayerbotFactory.h"
#include "playerbot/Talentspec.h"   // premade 天赋（按专精固定一套）
#include "playerbot/DungeonRoute.h"
#include "playerbot/DpsExempt.h"
#include "playerbot/strategy/DpsFsm.h"
#include "playerbot/strategy/actions/EquipAction.h"   // [2026-10-29] 假人场景 A/B：单只 bot 开关 DPS 状态机
#include "playerbot/CoverSpot.h"   // 卡视野点库（离线预筛 + 运行期取点）
#include "MotionGenerators/PathFinder.h"
#include "playerbot/DungeonRun.h"
#include "Combat/CombatLogRecorder.h"
#include "MotionGenerators/MoveMap.h"   // [2026-10-25] 击飞/掉出网格兜底：用导航网格判"人在不在图上"

#include <set>
#include <fstream>
#include <random>
#include <algorithm>
#include <functional>
#include <cmath>

using namespace ai;

// ---------------------------------------------------------------------------
// [仿真 2026-10-18 站长口径] 本地可控场景注入（多组并发；云端 SimMode=0 不生效）
//   设计/验收见 dev/SIM_SCENARIO_设计.md；日志前缀 `[SIM]`。
// ---------------------------------------------------------------------------

namespace simscenario
{
    namespace
    {
        // [2026-10-25 站长口径] 击飞/掉出地形兜底用的判据：人还在不在导航网格上（2 码容差）。
        //   558 是桥面被击飞、545 是水域，这两种情况下导航面本来就对不上 —— 只用来"发现异常"，不当结论。
        bool OobOnNavMesh(Map* map, float x, float y, float z)
        {
            if (!map)
                return false;
            dtNavMeshQuery const* q =
                MMAP::MMapFactory::createOrGetMMapManager()->GetNavMeshQuery(map->GetId(), map->GetInstanceId());
            if (!q)
                return true;   // 没网格数据就不下结论（当作正常，避免误救）
            dtQueryFilter filter;
            dtPolyRef ref = 0;
            // mmap 顶点按 (y, z, x) 存（同 `PathFinder::getArea`），传 {x,y,z} 必然找不到多边形 ⇒ 全员误判越界
            float const pt[3] = {y, z, x};
            float closest[3] = {0.0f, 0.0f, 0.0f};
            float const extents[3] = {3.0f, 5.0f, 3.0f};
            q->findNearestPoly(pt, extents, &filter, &ref, closest);
            if (!ref)
                return true;
            float const gx = closest[2], gy = closest[0], gz = closest[1];
            float const dx = gx - x, dy = gy - y, dz = gz - z;
            return (dx * dx + dy * dy + dz * dz) <= 4.0f;
        }

        // ---- 状态机阶段 ------------------------------------------------------
        enum Phase : uint8
        {
            PHASE_LOGIN = 0,      // 把 bot 拉上线
            PHASE_FORM = 1,       // 组队 + 挑真坦真奶 + 打开路线
            PHASE_TELEPORT = 2,   // 先队长进本，再其它成员进本
            PHASE_MOVEIN = 3,     // 等全员落在副本图里
            PHASE_RUN = 4,        // 交给 DungeonRoute 推进（只做状态日志/死亡复活/灭团判定）
            PHASE_RESET = 5,      // 灭团后重置（复活 + 回主城 + 下线 → 下一 attempt）
        };

        // 灭团判定：全队同时死亡持续这么久才认定（避免"死一个/假死"误判）
        const uint32 WIPE_CONFIRM_MS = 3000;
        // 卡死判定：这么久没有任何"波次推进"就认为卡住（单波磨 8 分钟已经不正常）
        const uint32 STUCK_MS = 480000;
        // 单个 attempt 的硬上限
        const uint32 ATTEMPT_TIMEOUT_MS = 2700000;

        struct Candidate
        {
            uint32 guid = 0;
            uint8 cls = 0;
            uint8 level = 0;
            uint8 race = 0;
            uint8 role = 0;   // [2026-10-21] 来自 ai_playerbot_bot_profile：1=tank 2=heal 3=dps（0=未知）
        };

        struct GroupRun
        {
            uint32 runId = 0;
            uint32 index = 0;
            uint32 mapId = 0;
            uint32 partySize = 5;        // 目标队伍人数（坦+奶+其余）
            uint32 minLevel = 1;         // 本图等级门槛（reset 时重新选人用）
            float ex = 0, ey = 0, ez = 0, eo = 0;
            std::vector<uint32> guids;   // LOGIN 阶段 = 候选；FORM 阶段收缩成最终队伍
            std::vector<uint32> nextLoginTryAt;
            Phase phase = PHASE_LOGIN;
            uint32 startedAt = 0;
            uint32 phaseSince = 0;
            uint32 enteredDungeonAt = 0;
            uint32 lastStatusAt = 0;
            // [2026-10-23 站长口径] "保证进入战斗"：整组连续 N 秒没在战斗 ⇒ 给坦克排一次 attack anything
            uint32 idleSince = 0;
            uint32 forceEngageAt = 0;
            uint32 staleLogAt = 0;        // [2026-10-26 诊断] [STALEFIGHT] 节流
            // [2026-10-26 诊断] "锁着目标不出手"取证：每个成员上次动作名+时间、每只怪上次血量+时间
            std::map<uint32, std::pair<std::string, uint32>> lastActOf;
            std::map<uint32, std::pair<uint32, uint32>> lastHpOf;
            uint32 tankAssistLogAt = 0;   // [2026-10-23] 坦克补目标日志节流（动作每 tick 都补）
            uint32 tankAssistAt = 0;      // [2026-10-23] 坦克补目标动作节流（500ms）
            uint32 leaderTryAt = 0;
            uint32 memberIdx = 1;        // 逐个进本：下一个要传的成员下标
            uint32 memberTryAt = 0;      // 该成员最近一次传送时间（0 = 还没传）
            uint32 tankGuid = 0;         // 真实专精挑出来的坦克（不再强挂策略）
            uint32 healGuid = 0;
            uint32 nextRoleFixAt = 0;    // 职能校验的节流
            uint32 diagLeft = 3;         // 进本后还要打几次诊断（没打起来时用来看卡在哪）
            uint32 roleRetries = 0;      // 组不齐真坦/真奶时"换人重选"的次数（不终止 run）
            uint32 nextRepairAt = 0;     // MOVEIN 里"修正 instance"的节流

            // ---- [2026-10-18 仿真实验] seed / attempt / policy ----
            uint32 baseSeed = 0;         // 启动参数 seed（缺省=时间戳）
            uint32 attempt = 1;          // 第几次尝试（1-based）
            uint32 maxAttempts = 3;      // reset 模式最多几次
            uint32 stuckMs = 480000;     // 卡死阈值（可用 stuck=<秒> 覆盖，测试重置用）
            uint32 maxRunMs = 1500000;   // 单轮硬性时间上限（可用 maxtime=<秒> 覆盖，默认 25 分钟）
            uint8 policy = 0;            // 0 = reset（灭团→重置重跑） / 1 = continue（死即复活续推）
            uint16 bestCleared = 0;      // 全部 attempt 里清掉的最大波次
            uint16 attemptBest = 0;      // 本 attempt 清掉的最大波次
            uint16 lastCleared = 0;      // 上一 tick 的已清波数（推进/卡死判定用）
            // [2026-10-18 站长口径] "有目标但走不过去 / 脱战残留"守卫：15 秒零伤害就跳过该波
            ObjectGuid lastVictim;       // 坦克当前目标
            uint32 lastVictimHp = 0;     // 上次看到的该目标血量
            uint32 lastVictimAt = 0;     // 上次看到"目标血量变化"的时间
            // [2026-10-18] 零伤害守卫的"战斗段"状态：一次战斗最多跳一波（避免把整图波次连锁跳光）
            uint32 combatSince = 0;      // 本次战斗开始时间（0 = 不在战斗）
            uint16 combatOrd = 0;        // 本次战斗开始时对应的波
            bool combatSkipDone = false; // 本次战斗已经跳过一波（战斗结束后复位）
            uint32 combatLastDmgMs = 0;  // 本次战斗里最近一次看到的伤害时间戳
            bool combatDamageSeen = false; // 本次战斗是否已经真打起来（有伤害）
            uint32 combatCloseInSince = 0; // [2026-10-21] 本次目标开始"追过去打"的时间（0=未追）
            uint32 chaseBestAt = 0;        // 追击中最近一次"距离明显缩短"的时间（判断是否真的走不过去）
            float  chaseBestDist = 1e9f;   // 追击中见过的最小距离
            uint32 chaseLogAt = 0;         // 追击日志节流
            uint32 meleeSince = 0;         // 已进入近战距离但仍无伤害的起点
            uint32 combatAssistAt = 0;     // [2026-10-21] 坦克"去帮队友打"的节流
            bool outReported = false;      // [2026-10-21] 本次 run 是否已记录过"有人离开副本"
            uint32 outFixAt = 0;           // [2026-10-21] 离本拉回节流
            uint32 oobFixAt = 0;           // [2026-10-25] 击飞/掉出网格兜底节流
            uint32 pickUpAt = 0;           // [2026-10-21] 强制"接漏怪"节流
            uint32 noVictimSince = 0;    // 坦克"在战斗但没目标"的起始时间（0=有目标/不在战斗）
            uint32 soloReviveAt = 0;     // [2026-10-21] 单人阵亡就地复活的节流（0 = 没有待复活的）
            uint32 deaths = 0;           // 本 attempt 死亡次数累计（复活后不清零）
            uint32 lastRescueDeaths = 0; // [2026-10-24] 上次"死亡循环救援"时的死亡数
            uint32 lastRescueAt = 0;     // [2026-10-24] 上次救援时间
            uint32 manaPotionAt = 0;      // [2026-10-24] 蓝药水节流
            uint32 assistFreeAt = 0;      // [2026-10-24] 漏怪补位节流
            uint32 assistFreeLogAt = 0;
            uint32 tankHoldAt = 0;
            double tankCombatSec = 0.0;   // [2026-10-24] 坦克处于战斗状态的时间（秒）
            double lastTotalThreat = 0.0; // [2026-10-24] 上次采样的坦克总仇恨
            uint32 tpsLogAt = 0;       // [2026-10-24] TANKHOLD 统计节流
            std::map<uint32, float> threatPrev;   // [2026-10-24] 每只怪上次记录的坦克仇恨
            double threatAccum = 0.0;             // [2026-10-24] 坦克累计仇恨增量（用于 TPS）
            uint32 tpsAt = 0;                     // [2026-10-24] TPS 窗口起点
            uint32 stuckSince = 0;                // [2026-10-24] 本次"打不完的架"起始时间
            // [2026-10-28 站长口径] "**从上一个 stage 复活**"：记录进入每个 stage 时的坦克位置，
            //   复活时回到**当前 stage 的起点**（也就是上一个 stage 的终点），而不是怪堆里的波位。
            struct StagePos { float x = 0.0f, y = 0.0f, z = 0.0f; };
            uint16 lastStageOrd = 0;
            std::map<uint16, StagePos> stageStart;
uint32 stuckFightLogAt = 0;           // [2026-10-24] STUCKFIGHT 日志节流
uint32 stuckFightCleared = 0;
            uint32 lostCheckAt = 0;      // [2026-10-24] 离队检查节流
            std::map<uint32, uint64> botDmgSeen;  // [2026-10-24] 每 bot 进入战斗时的伤害基线
            std::map<uint32, uint64> botHealSeen;
            std::map<uint32, bool> botWasInCombat;
            std::map<uint8, double> classCombatSec;  // [2026-10-24] 各职业处于战斗的秒数（1 秒采样）
            std::map<uint8, uint64> classDmgAcc;     // 窗口内伤害增量累计
            std::map<uint8, uint64> classHealAcc;    // 窗口内治疗增量累计
            std::map<uint8, uint64> classDmgSeen;    // 上次采样值
            std::map<uint8, uint64> classHealSeen;
            uint32 dpsTickAt = 0;
            uint32 dpsLogAt = 0;
            std::map<uint8, uint64> dpsLast;   // [2026-10-24] 各职业累计伤害（用于算 DPS）
            std::map<uint8, uint64> healLast;  // [2026-10-24] 各职业累计治疗（用于算 HPS）
            std::map<uint32, uint32> combatMs; // [2026-10-24] 每个 bot 的"战斗中时间"（毫秒）
            std::map<uint8, uint32> combatMsLast;  // [2026-10-24] 上窗口各职业战斗毫秒
            uint32 combatAccAt = 0;            // [2026-10-24] 战斗时间累计节流
            uint64 lastTotalDmg = 0;           // [2026-10-24] 本组累计总伤（dps 块用）
            uint32 dpsAt = 0;                  // [2026-10-24] DPS 统计节流
            // [2026-10-29 战斗取证] 每 10 秒窗口：总伤害/宠物伤害/平砍/施法/技能分布
            std::map<uint32, uint64> combatSnapAllDmg;   // guid -> botDamageDealt + botPetDamageDealt
            std::map<uint32, uint64> combatSnapPetDmg;
            std::map<uint32, uint64> combatSnapMelee;
            std::map<uint32, uint64> combatSnapCast;
            std::map<uint32, std::map<uint32, uint64>> combatSnapSpell;
            uint64 winDmg = 0, winPet = 0, winMelee = 0, winCasts = 0;
            std::map<uint32, uint64> winSpell;
            uint32 lastDead = 0;         // 上一 tick 死亡人数（算增量）
            uint32 wipeSince = 0;        // 全员死亡开始时间（0 = 未灭团）
            uint32 lastProgressAt = 0;   // 最近一次推进（换波/开打）时间
            uint32 nextResetTryAt = 0;   // PHASE_RESET 里传送节流
            uint32 attemptsDone = 0;
            uint32 attemptStartedAt = 0; // 本 attempt 进本时刻（算时长）
            uint16 routeWaves = 0;       // 本图总波数（route.maxOrd）
            std::set<uint16> seenWaves;  // 本 attempt 见过活怪的波（统计已清用）
            uint32 lastAliveMobs = 0;    // 上一 tick 本图活着的小怪数（减少即算推进）
            std::string attemptSummary;  // 每次 attempt 结果（落日志/状态）
            bool groupCreated = false;
            bool leaderTeleported = false;
            bool membersTeleported = false;
            Group* group = nullptr;
            uint32 instanceId = 0;
            std::string error;
        };

        struct RunReport
        {
            uint32 runId = 0;
            uint32 index = 0;
            uint32 mapId = 0;
            bool ok = false;
            bool reachedDungeon = false;
            uint32 totalMs = 0;
            uint32 inDungeonMs = 0;
            std::string error;
            std::vector<uint32> guids;
        };

        std::vector<GroupRun> s_runs;
        std::vector<RunReport> s_reports;
        uint32 s_runSeq = 0;

        // -------------------------------------------------------------------
        // [2026-10-29 站长口径] 训练假人 DPS 场景：每次打固定时长，1 目标 / 5 目标各一轮。
        //   为什么要单独做：副本跑一轮 20 分钟以上，杂项太多，DPS 状态机改一点就要等很久；
        //   假人场景 2 分钟就能拿到"这个专精到底打多少"，也是离线调优的测量单元。
        //   数据来源：Player 上已有的 botDamageDealt / botPetDamageDealt / botMeleeHits /
        //   botCasts / botSpellCounts 计数器取**增量**（假人 1,000,000 血，2 分钟打不死）。
        // -------------------------------------------------------------------
        struct DummyRun
        {
            uint32 id = 0;
            std::string spec;
            uint32 cls = 0;
            std::string enable;     // 要打开的专精策略
            std::string disable1;   // 要关掉的竞争策略
            std::string disable2;
            uint32 targets = 1;
            uint32 durationMs = 120000;
            uint32 startMs = 0;
            uint32 guid = 0;
            uint32 mapId = 1;
            float x = 0.0f, y = 0.0f, z = 0.0f;
            float standDist = 3.0f;   // bot 相对假人的站位距离
            bool meleeSpec = false;   // 近战专精（决定 Attack(meleeAttack) 参数）
            uint32 pileOwner = 0;     // !=0：这一轮不自己召假人，共用这个 id 的假人堆
            float ringAngle = 0.0f;   // 共享堆时自己站的角度（一圈人围着打）
            bool centerPile = false;  // true = 假人就摆在 (x,y)（共享堆的堆主）
            bool sharedPile = false;  // true = 这一轮的木桩是全队共用的，收尾时谁都不许删
            // [2026-10-30] 一堆一个专精后，**堆主也要站到自己那堆外面**（远程 18 码），
            //   所以"堆心"和"bot 站位"必须分开存：(x,y,z) 一律是 bot 自己的站位，假人摆 pileX/Y/Z。
            float pileX = 0.0f, pileY = 0.0f, pileZ = 0.0f;
            bool pileSet = false;     // 堆心是否已确定（单组场景在第一次 Setup 时按"bot 东侧"算出）
            std::map<std::string, double> overrides;   // 本 bot 的参数覆盖（调优并发用）
            std::string armLabel;                      // 例如 "burst_hp=10" 或 "baseline"
            std::vector<std::pair<uint8, uint32>> bisPending;   // 静态 BIS：还没穿上的 (slot,item)
            bool gearReady = true;                              // 装备流水是否已完成
            std::vector<ObjectGuid> dummies;
            bool reported = false;
            uint32 lastPulseMs = 0;
            uint32 lastAttackMs = 0;
            bool waitingLogin = false;
            bool setupDone = false;
            uint32 pinnedGuid = 0;      // 调优时要"同一只 bot 复测"，不能每轮换人
            uint8 tab = 0;              // 目标天赋树
            std::string premade;        // 目标 premade 天赋名（配置里那一套）
            bool respec = true;         // 开跑前按目标专精重天赋 + 换最好装备
            bool respecced = false;
            bool useFsm = true;         // false = 这一轮关掉 DPS 状态机（A/B 对照基线）
            // 基线（用来算增量）
            uint64 dmg0 = 0, pet0 = 0, melee0 = 0, casts0 = 0;
            std::map<uint32, uint64> spells0;   // 施法分布也要取增量，否则 top 里是历史累计
        };

        std::vector<DummyRun> s_dummies;
        uint32 s_dummySeq = 0;

        // GM 岛：平整、无怪、无地形坑，5 个假人围一圈也不会互相卡
        float const DUMMY_SPOT_X = 16222.3f;
        float const DUMMY_SPOT_Y = 16266.8f;
        float const DUMMY_SPOT_Z = 12.6f;
        // 一堆多目标时假人围成的半径（m）：第 0 个假人永远在堆心正东 PILE_DUMMY_R 处
        float const PILE_DUMMY_R = 2.0f;

        struct SpecKey
        {
            char const* key;
            uint32 cls;
            char const* enable;
            char const* disable1;
            char const* disable2;
            uint8 tab;          // 该职业的天赋树序号（talentTab.tabpage：0/1/2）
            char const* premade; // 配置里 AiPlayerbot.PremadeSpecName.<cls>.<n> 的**精确名字**
        };

        SpecKey const s_specKeys[] =
        {
            // mage: 0=奥 1=火 2=冰 | warrior: 0=武器 1=狂暴 2=防护 | rogue: 0=刺杀 1=战斗 2=敏锐
            { "fire",         CLASS_MAGE,    "fire",          "frost",         "arcane",       1, "pve dps fire" },
            { "frost",        CLASS_MAGE,    "frost",         "fire",          "arcane",       2, "pve dps frost" },
            { "arcane",       CLASS_MAGE,    "arcane",        "fire",          "frost",        0, "pve dps arcane" },
            { "arms",         CLASS_WARRIOR, "arms",          "fury",          "protection",   0, "pve arms" },
            { "fury",         CLASS_WARRIOR, "fury",          "arms",          "protection",   1, "pve fury" },
            { "combat",       CLASS_ROGUE,   "combat",        "assassination", "subtlety",     1, "pve dps combat" },
            { "assassination",CLASS_ROGUE,   "assassination", "combat",        "subtlety",     0, "pve assassination" },
            // hunter: 0=兽王 1=射击 2=生存 | warlock: 0=痛苦 1=恶魔 2=毁灭
            { "bm",           CLASS_HUNTER,  "beast mastery", "marksmanship",  "survival",     0, "pve dps bm" },
            { "marks",        CLASS_HUNTER,  "marksmanship",  "beast mastery", "survival",     1, "pve dps mm" },
            { "affliction",   CLASS_WARLOCK, "affliction",    "demonology",    "destruction",  0, "pve dps affli" },
            { "destro",       CLASS_WARLOCK, "destruction",   "affliction",    "demonology",   2, "pve dps destro" },
            // shaman: 0=元素 1=增强 2=恢复 | druid: 0=平衡 1=野性 2=恢复
            { "enhance",      CLASS_SHAMAN,  "enhancement",   "elemental",     "restoration",  1, "pve dps enh" },
            { "elemental",    CLASS_SHAMAN,  "elemental",     "enhancement",   "restoration",  0, "pve dps elem" },
            { "feral",        CLASS_DRUID,   "feral",         "balance",       "restoration",  1, "pve dps feral cat" },
            { "balance",      CLASS_DRUID,   "balance",       "feral",         "restoration",  0, "pve dps balance" },
            // paladin: 0=神圣 1=防护 2=惩戒 | priest: 0=戒律 1=神圣 2=暗影
            { "ret",          CLASS_PALADIN, "retribution",   "protection",    "holy",         2, "pve dps ret" },
            { "shadow",       CLASS_PRIEST,  "shadow",        "discipline",    "holy",         2, "pve dps shadow" },
        };

        SpecKey const* FindSpecKey(std::string const& key)
        {
            for (auto const& k : s_specKeys)
                if (key == k.key)
                    return &k;
            return nullptr;
        }

        // 优先挑已经在线、等级 60+ 的同职业 bot（不用等登录）；没有就返回 0
        Player* FindOnlineBotOfClass(uint32 cls, std::set<uint32> const& used)
        {
            PlayerBotMap& bots = sRandomPlayerbotMgr.GetAllBots();
            for (auto const& kv : bots)
            {
                Player* p = kv.second;
                if (!p || !p->IsInWorld() || !p->IsAlive())
                    continue;
                if (p->getClass() != cls)
                    continue;
                if (p->GetLevel() < 60)
                    continue;
                if (used.count(p->GetGUIDLow()))
                    continue;
                if (!p->GetPlayerbotAI())
                    continue;
                return p;
            }
            return nullptr;
        }


        // 仿真改成强制开路线；stop 时恢复原值（-1 = 跟随配置）
        int s_savedRouteSwitch = -1;
        bool s_routeOverridden = false;
        // [2026-10-18] 机器人"活跃度优先级"：实例里的 bot 在非战斗时会命中 IN_ACTIVE_MAP 之类
        //   的低优先级 ⇒ `DoNextAction` 走 minimal 分支 ⇒ **相关性 <100 的触发器全跳过**
        //   （dungeon route move=75 / pull=85 正好在门槛下）⇒ 站在拉怪位不动、不拉怪。
        //   仿真必须把优先级关掉（等价 AiPlayerbot.DisableActivityPriorities=1）。
        bool s_savedDisableActivityPriorities = false;
        bool s_activityPrioritiesOverridden = false;

        // ---- 小工具 ----------------------------------------------------------
        uint32 Now() { return WorldTimer::getMSTime(); }

        ObjectGuid GuidOf(uint32 low)
        {
            return ObjectGuid(HIGHGUID_PLAYER, low);
        }

        Player* FindPlayerAny(uint32 low)
        {
            return sObjectMgr.GetPlayer(GuidOf(low), false);
        }

        Player* FindPlayerInWorld(uint32 low)
        {
            Player* p = FindPlayerAny(low);
            return (p && p->IsInWorld()) ? p : nullptr;
        }

        bool IsAliveInWorld(Player* p)
        {
            return p && p->IsInWorld() && p->IsAlive();
        }

        std::string ClassName(uint8 cls)
        {
            switch (cls)
            {
                case CLASS_WARRIOR:      return "warrior";
                case CLASS_PALADIN:      return "paladin";
                case CLASS_HUNTER:       return "hunter";
                case CLASS_ROGUE:        return "rogue";
                case CLASS_PRIEST:       return "priest";
                case CLASS_SHAMAN:       return "shaman";
                case CLASS_MAGE:         return "mage";
                case CLASS_WARLOCK:      return "warlock";
                case CLASS_DRUID:        return "druid";
#ifdef MANGOSBOT_TWO
                case CLASS_DEATH_KNIGHT: return "deathknight";
#endif
                default:                 return "?";
            }
        }

        bool IsTankClass(uint8 cls)
        {
            return cls == CLASS_WARRIOR || cls == CLASS_PALADIN || cls == CLASS_DRUID
#ifdef MANGOSBOT_TWO
                || cls == CLASS_DEATH_KNIGHT
#endif
                ;
        }

        bool IsHealClass(uint8 cls)
        {
            return cls == CLASS_PRIEST || cls == CLASS_PALADIN || cls == CLASS_DRUID || cls == CLASS_SHAMAN;
        }

        // ---- 候选池 ----------------------------------------------------------
        // 口径：
        //   ① 必须是随机 bot 角色（ai_playerbot_random_bots，owner=0）——它们有随机器/装备；
        //   ② 轮换开着时还要排除轮换池里的（event='add'）：那些被 ProcessBot 管着，
        //      会随机传送/随机换策略，仿真期间会捣乱；轮换关着（RandomBotAutologin=0）时
        //      这些 bot 完全没人管 ⇒ 可以放心用（本地多组并发就靠这批补人）。
        //   ③ 排除已在队伍里的；④ 排除正在被别的 run 用的。
        // ---- bot 固定职责（DB 持久化） ---------------------------------------
        // [2026-10-21 站长口径] bot 的天赋/专精/职责是固定的，必须落库复用，
        //   不能在每次登录后"重新生成策略"再判断。表：character DB `ai_playerbot_bot_profile`。
        std::map<uint32, uint8>& ProfileRoles()
        {
            static std::map<uint32, uint8> roles;
            return roles;
        }

        bool& ProfileRolesLoaded()
        {
            static bool loaded = false;
            return loaded;
        }

        void LoadProfileRoles()
        {
            if (ProfileRolesLoaded())
                return;
            ProfileRolesLoaded() = true;
            std::unique_ptr<QueryResult> res(CharacterDatabase.Query(
                "SELECT guid, role FROM ai_playerbot_bot_profile"));
            if (!res)
                return;
            do
            {
                Field* f = res->Fetch();
                ProfileRoles()[f[0].GetUInt32()] = f[1].GetUInt8();
            } while (res->NextRow());
        }

        uint8 CachedRole(uint32 guid)
        {
            LoadProfileRoles();
            auto it = ProfileRoles().find(guid);
            return it == ProfileRoles().end() ? 0 : it->second;
        }

        // 从 character_spell（天赋=已学天赋法术）+ DBC 天赋表算出固定 spec_tab。
        uint8 SpecTabFromSpells(uint8 cls, uint8 level, std::set<uint32> const& spells)
        {
            std::map<uint32, int32> tabs;
            for (uint32 i = 0; i < 3; ++i)
                tabs[i] = 0;

            uint32 const classMask = 1u << (cls - 1);
            for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
            {
                TalentEntry const* talentInfo = sTalentStore.LookupEntry(i);
                if (!talentInfo)
                    continue;
                TalentTabEntry const* tabInfo = sTalentTabStore.LookupEntry(talentInfo->TalentTab);
                if (!tabInfo || (classMask & tabInfo->ClassMask) == 0)
                    continue;
                for (int rank = MAX_TALENT_RANK - 1; rank >= 0; --rank)
                {
                    uint32 spellid = talentInfo->RankID[rank];
                    if (spellid && spells.count(spellid))
                    {
                        tabs[tabInfo->tabpage] += rank + 1;
                        break;
                    }
                }
            }

            int32 const total = tabs[0] + tabs[1] + tabs[2];
            if (level >= 10 && total > 0)
            {
                uint8 best = 0;
                for (uint8 t = 1; t < 3; ++t)
                    if (tabs[t] > tabs[best])
                        best = t;
                return best;
            }

            switch (cls)
            {
                case CLASS_MAGE:    return 1;
                case CLASS_PALADIN: return 2;
                case CLASS_PRIEST:  return 1;
                case CLASS_WARRIOR: return 2;
                default:            return 0;
            }
        }

        // 一次性构建/刷新 bot 固定职责表（数据操作；之后仿真直接用 DB 里的角色）。
        std::string BuildBotProfiles()
        {
            std::unique_ptr<QueryResult> bots(CharacterDatabase.Query(
                "SELECT DISTINCT r.bot, c.class, c.level FROM ai_playerbot_random_bots r "
                "JOIN characters c ON c.guid = r.bot WHERE r.owner = 0"));
            if (!bots)
                return "profile: 没有随机 bot 数据";

            std::map<uint32, std::pair<uint8, uint8>> botInfo;   // guid -> (class, level)
            do
            {
                Field* f = bots->Fetch();
                botInfo[f[0].GetUInt32()] = std::make_pair(f[1].GetUInt8(), f[2].GetUInt8());
            } while (bots->NextRow());

            std::map<uint32, std::set<uint32>> botSpells;
            std::vector<uint32> guids;
            for (auto const& kv : botInfo)
                guids.push_back(kv.first);

            for (size_t base = 0; base < guids.size(); base += 400)
            {
                std::string in;
                for (size_t i = base; i < guids.size() && i < base + 400; ++i)
                {
                    if (!in.empty())
                        in += ",";
                    in += std::to_string(guids[i]);
                }
                std::unique_ptr<QueryResult> res(CharacterDatabase.PQuery(
                    "SELECT guid, spell FROM character_spell WHERE guid IN (%s)", in.c_str()));
                if (!res)
                    continue;
                do
                {
                    Field* f = res->Fetch();
                    botSpells[f[0].GetUInt32()].insert(f[1].GetUInt32());
                } while (res->NextRow());
            }

            uint32 written = 0, tanks = 0, heals = 0;
            std::string values;
            auto flush = [&]()
            {
                if (values.empty())
                    return;
                CharacterDatabase.PExecute(
                    "INSERT INTO ai_playerbot_bot_profile(guid, cls, spec_tab, role) VALUES %s "
                    "ON DUPLICATE KEY UPDATE cls=VALUES(cls), spec_tab=VALUES(spec_tab), role=VALUES(role)",
                    values.c_str());
                values.clear();
            };

            for (auto const& kv : botInfo)
            {
                uint32 const guid = kv.first;
                uint8 const cls = kv.second.first;
                uint8 const level = kv.second.second;
                auto spellIt = botSpells.find(guid);
                std::set<uint32> empty;
                std::set<uint32> const& spells = (spellIt == botSpells.end()) ? empty : spellIt->second;

                uint8 const tab = SpecTabFromSpells(cls, level, spells);
                BotRoles const roles = AiFactory::GetPlayerRoles(cls, tab);
                uint8 role = 3;
                if (roles & BOT_ROLE_TANK) role = 1;
                else if (roles & BOT_ROLE_HEALER) role = 2;
                if (role == 1) ++tanks;
                if (role == 2) ++heals;

                if (!values.empty())
                    values += ",";
                values += "(" + std::to_string(guid) + "," + std::to_string(cls) + "," +
                    std::to_string(tab) + "," + std::to_string(role) + ")";
                ++written;
                if (values.size() > 30000)
                    flush();
            }
            flush();

            // 刷新内存缓存
            ProfileRoles().clear();
            ProfileRolesLoaded() = false;
            LoadProfileRoles();

            return "profile: 写入/刷新 " + std::to_string(written) + " 个 bot（tank=" +
                   std::to_string(tanks) + " heal=" + std::to_string(heals) + " dps=" +
                   std::to_string(written - tanks - heals) + "）";
        }

        std::vector<Candidate> LoadCandidates(uint32 minLevel, std::vector<uint32> const& excluded)
        {
            std::vector<Candidate> out;
            LoadProfileRoles();   // [2026-10-21] 候选带固定职责（DB）

            std::string sql =
                "SELECT c.guid, c.class, c.level, c.race FROM characters c "
                "WHERE c.level >= '%u' "
                "AND EXISTS (SELECT 1 FROM ai_playerbot_random_bots r WHERE r.bot = c.guid AND r.owner = 0) ";

            if (sPlayerbotAIConfig.randomBotAutologin)
                sql += "AND NOT EXISTS (SELECT 1 FROM ai_playerbot_random_bots a WHERE a.bot = c.guid AND a.event = 'add') ";

            sql += "AND NOT EXISTS (SELECT 1 FROM group_member m WHERE m.memberGuid = c.guid) "
                   "ORDER BY c.class ASC, c.guid ASC";

            std::unique_ptr<QueryResult> res(CharacterDatabase.PQuery(sql.c_str(), minLevel));

            if (!res)
                return out;

            do
            {
                Field* fields = res->Fetch();
                Candidate c;
                c.guid = fields[0].GetUInt32();
                c.cls = fields[1].GetUInt8();
                c.level = fields[2].GetUInt8();
                c.race = fields[3].GetUInt8();
                c.role = CachedRole(c.guid);

                if (!c.guid || !c.level)
                    continue;
                if (std::find(excluded.begin(), excluded.end(), c.guid) != excluded.end())
                    continue;
                if (FindPlayerAny(c.guid))       // 已经在线（别的用途），不抢
                    continue;
                // [2026-10-18] 账号缺失的角色不能选（否则登录阶段报"拿不到账号"整组失败）
                if (!sObjectMgr.GetPlayerAccountIdByGUID(GuidOf(c.guid)))
                    continue;

                out.push_back(c);
            } while (res->NextRow());

            return out;
        }

        // 一组 1 真坦 + 1 真奶 + 其余 DPS；同阵营（TeamForRace）。
        //   [2026-10-18 实验种子] seed != 0 时用 mt19937 洗牌：**同 seed 同名单**，不同 attempt 阵容不同。
        bool PickParty(std::vector<Candidate> const& pool, size_t size, std::set<uint32>& used,
                       std::vector<uint32>& out, std::string& error, uint32 seed = 0)
        {
            if (size < 1)
                size = 1;

            std::vector<Candidate> free;
            for (auto const& c : pool)
                if (!used.count(c.guid))
                    free.push_back(c);

            if (free.empty())
            {
                error = "候选池没有可用 bot";
                return false;
            }

            if (seed)
            {
                std::mt19937 rng(seed);
                std::shuffle(free.begin(), free.end(), rng);
            }

            // 队长（坦克候选）：优先 DB 固定职责=tank，其次坦克职业
            Candidate tank;
            bool foundTank = false;
            for (auto const& c : free)
            {
                if (c.role == 1)
                {
                    tank = c;
                    foundTank = true;
                    break;
                }
            }
            for (auto const& c : free)
            {
                if (foundTank)
                    break;
                if (IsTankClass(c.cls))
                {
                    tank = c;
                    foundTank = true;
                    break;
                }
            }
            if (!foundTank)
                tank = free.front();                       // 没有坦克职业：先记下，登录后按真专精校验（过不了就失败）

            Team team = Player::TeamForRace(tank.race);

            auto sameTeam = [&](Candidate const& c) { return Player::TeamForRace(c.race) == team; };

            std::vector<Candidate> picked;
            picked.push_back(tank);
            used.insert(tank.guid);

            auto takeFirst = [&](bool (*pred)(uint8)) -> bool
            {
                if (picked.size() >= size)
                    return true;
                for (auto const& c : free)
                {
                    if (used.count(c.guid) || !sameTeam(c))
                        continue;
                    if (pred && !pred(c.cls))
                        continue;
                    picked.push_back(c);
                    used.insert(c.guid);
                    return true;
                }
                return false;
            };

            // [2026-10-18] 真坦/真奶只能从"候选池"里挑 ⇒ 多抓几种坦职业/奶职业，
            //   否则 8 个候选里可能一个防护坦都没有（站长口径：不用 DPS 顶替坦克）。
            // [2026-10-21] 固定职责来自 DB（ai_playerbot_bot_profile），不再靠登录后的临时策略判定。
            auto takeFirstRole = [&](uint8 role) -> bool
            {
                if (picked.size() >= size)
                    return true;
                for (auto const& c : free)
                {
                    if (used.count(c.guid) || !sameTeam(c) || c.role != role)
                        continue;
                    picked.push_back(c);
                    used.insert(c.guid);
                    return true;
                }
                return false;
            };
            // [2026-10-21 修正] 只各取 1 个坦/1 个奶，其余**优先 DPS**，
            //   否则候选池里坦多时会组出"2 个真坦 + 没有 DPS"的队伍（与驱动坦克口径冲突）。
            takeFirstRole(1);
            takeFirstRole(2);
            while (picked.size() < size)
            {
                if (!takeFirstRole(3))
                    break;
            }
            // 职责表缺失/不够时再退回职业启发式
            takeFirst(&IsTankClass);
            takeFirst(&IsHealClass);
            while (picked.size() < size)
            {
                if (!takeFirst(nullptr))
                    break;
            }

            if (picked.size() < size)
            {
                error = "同阵营候选不足（需要 " + std::to_string(size) + "，只有 " + std::to_string(picked.size()) + "）";
                // 回滚已被占用的
                for (auto const& c : picked)
                    used.erase(c.guid);
                return false;
            }

            for (auto const& c : picked)
                out.push_back(c.guid);

            return true;
        }

        // ---- 组队 / 职能 -----------------------------------------------------
        bool CreateBotParty(GroupRun& run, std::string& error)
        {
            Player* leader = FindPlayerInWorld(run.guids[0]);
            if (!leader)
            {
                error = "队长不在线";
                return false;
            }

            Group* group = new Group();
            if (!group->AddLeaderInvite(leader))
            {
                delete group;
                error = "AddLeaderInvite 失败";
                return false;
            }

            group->RemoveInvite(leader);

            if (!group->Create(group->GetLeaderGuid(), group->GetLeaderName()))
            {
                delete group;
                error = "Group::Create 失败";
                return false;
            }

            sObjectMgr.AddGroup(group);

            for (size_t i = 1; i < run.guids.size(); ++i)
            {
                Player* member = FindPlayerInWorld(run.guids[i]);
                if (!member)
                {
                    error = "成员不在线 guid=" + std::to_string(run.guids[i]);
                    break;
                }

                if (!group->AddMember(member->GetObjectGuid(), member->GetName()))
                    sLog.outError("[SIM] run=%u g=%u 入队失败 member=%s", run.runId, run.index, member->GetName());
            }

            run.group = group;
            run.groupCreated = true;

            // 队伍成员策略刷新（tank/heal 等按天赋重新挂上）
            for (uint32 guid : run.guids)
            {
                Player* p = FindPlayerInWorld(guid);
                PlayerbotAI* botAI = p ? p->GetPlayerbotAI() : nullptr;
                if (botAI)
                    botAI->ResetStrategies();
            }

            return true;
        }

        // [2026-10-18 站长口径] "只用正确的坦克和治疗"：**不强行改策略**。
        //   登录一批候选后按真实专精（PlayerbotAI::IsTank / IsHeal，由天赋+默认策略得出）挑选；
        //   选不到就明确报错（列出每个候选的真实职能），绝不拿 DPS 顶替。
        bool SelectRealRoles(GroupRun& run, size_t partySize, std::string& error)
        {
            Player* tank = nullptr;
            Player* heal = nullptr;
            std::string poolDesc;

            for (uint32 guid : run.guids)          // 此时 guids = 候选（已全部在线）
            {
                Player* p = FindPlayerInWorld(guid);
                if (!p)
                {
                    poolDesc += " " + std::to_string(guid) + ":offline";
                    continue;
                }

                // [2026-10-21] 固定职责：优先用 DB（ai_playerbot_bot_profile）；缺失才退回运行时判定。
                uint8 const dbRole = CachedRole(guid);
                bool isTank = (dbRole == 1);
                bool isHeal = (dbRole == 2);
                if (dbRole == 0)
                {
                    isTank = PlayerbotAI::IsTank(p);
                    isHeal = PlayerbotAI::IsHeal(p);
                }
                poolDesc += " " + std::string(p->GetName()) + "(" + ClassName(p->getClass())
                          + (isTank ? ",tank" : (isHeal ? ",heal" : ",dps")) + ")";

                if (!tank && isTank)
                    tank = p;
                if (!heal && isHeal && !isTank)
                    heal = p;
            }

            sLog.outError("[SIM] run=%u g=%u 候选 %u 人：%s", run.runId, run.index,
                (uint32)run.guids.size(), poolDesc.c_str());

            if (!tank || !heal)
            {
                error = std::string("没有真正的") + (tank ? "" : "坦克")
                      + (!tank && !heal ? "/" : "") + (heal ? "" : "治疗")
                      + "（候选：" + poolDesc + "）";
                return false;
            }

            // 队伍 = 1 真坦 + 1 真奶 + (partySize-2) 真 DPS；**绝不多带第二个坦/奶**（1+1+3）。
            std::vector<uint32> party;
            party.push_back(tank->GetGUIDLow());
            if (heal->GetGUIDLow() != tank->GetGUIDLow())
                party.push_back(heal->GetGUIDLow());

            // ① DPS 位：优先 DB role=3；没有职责数据时，只接受"非坦非奶"的在线成员
            for (int pass = 0; pass < 2 && party.size() < partySize; ++pass)
            {
                for (uint32 guid : run.guids)
                {
                    if (party.size() >= partySize)
                        break;
                    if (std::find(party.begin(), party.end(), guid) != party.end())
                        continue;

                    Player* p = FindPlayerInWorld(guid);
                    if (!p)
                        continue;

                    uint8 const r = CachedRole(guid);
                    bool const isT = (r == 1) || (r == 0 && PlayerbotAI::IsTank(p));
                    bool const isH = (r == 2) || (r == 0 && PlayerbotAI::IsHeal(p));
                    if (isT || isH)
                        continue;
                    if (pass == 0 && r != 3)
                        continue;

                    party.push_back(guid);
                }
            }

            // ② 不够就明确失败（换人重选），不用第二个坦/奶凑数
            if (party.size() < partySize)
            {
                error = "没有足够的真 DPS（需要 " + std::to_string(partySize - 2) +
                        "，只有 " + std::to_string(party.size() - 2) + "）（候选：" + poolDesc + "）";
                return false;
            }

            // 候选里没用上的：直接下线（不组队、不占世界）
            uint32 dropped = 0;
            for (uint32 guid : run.guids)
            {
                if (std::find(party.begin(), party.end(), guid) != party.end())
                    continue;
                if (FindPlayerAny(guid))
                {
                    sRandomPlayerbotMgr.LogoutPlayerBot(guid);
                    ++dropped;
                }
            }

            run.guids = party;
            run.nextLoginTryAt.assign(party.size(), 0);
            run.tankGuid = tank->GetGUIDLow();
            run.healGuid = heal->GetGUIDLow();

            sLog.outError("[SIM] run=%u g=%u 用真坦/真奶组队：tank=%s heal=%s 其余=%u 人；候选未用下线 %u",
                run.runId, run.index, tank->GetName(), heal->GetName(),
                (uint32)(party.size() > 2 ? party.size() - 2 : 0), dropped);
            return true;
        }

        // 组队后校验：真坦/真奶的默认策略必须还在（组队会让 AI 以队长为 master 并 ResetStrategies，
        //   但真专精的默认策略会被重新挂上 ⇒ 这里只报警、不再强改策略）。
        void CheckRoles(GroupRun& run, uint32 now)
        {
            if (now < run.nextRoleFixAt)
                return;
            run.nextRoleFixAt = now ? (now + 5000) : 1;

            Player* tank = run.tankGuid ? FindPlayerInWorld(run.tankGuid) : nullptr;
            if (tank && !PlayerbotAI::IsTank(tank))
                sLog.outError("[SIM] run=%u g=%u WARN 坦克 %s 不再带 tank 策略（专精/策略被重置？）",
                    run.runId, run.index, tank->GetName());

            Player* heal = run.healGuid ? FindPlayerInWorld(run.healGuid) : nullptr;
            if (heal && !PlayerbotAI::IsHeal(heal))
                sLog.outError("[SIM] run=%u g=%u WARN 治疗 %s 不再带 heal 策略（专精/策略被重置？）",
                    run.runId, run.index, heal->GetName());
        }

        // `[SIM] diag`：为什么拉怪/跑位没动？把驱动判据和 route 数据一次性打出来（只在 RUN 阶段）。
        void LogDiagnostics(GroupRun& run)
        {
            dungeonroute::MapRoute const* route = dungeonroute::GetMapRoute(run.mapId);
            sLog.outError("[SIM] run=%u g=%u diag route loaded=%d verified=%d waves=%u guidOrd=%u bosses=%u routeSwitch=%d routeEnabled=%d",
                run.runId, run.index,
                route ? (route->loaded ? 1 : 0) : 0,
                route ? (route->verified ? 1 : 0) : 0,
                route ? (uint32)route->waves.size() : 0,
                route ? (uint32)route->guidOrd.size() : 0,
                route ? (uint32)route->bosses.size() : 0,
                dungeonroute::RouteRuntimeSwitch(),
                dungeonroute::RouteEnabled() ? 1 : 0);

            for (uint32 guid : run.guids)
            {
                Player* p = FindPlayerInWorld(guid);
                PlayerbotAI* botAI = p ? p->GetPlayerbotAI() : nullptr;
                if (!botAI)
                {
                    sLog.outError("[SIM] run=%u g=%u diag bot=%u offline/no-ai", run.runId, run.index, guid);
                    continue;
                }

                sLog.outError("[SIM] run=%u g=%u diag bot=%s cls=%u tank=%d heal=%d tankStrat=%d dungeonRoute=%d drive=%d leader=%d allowAct=%d map=%u instance=%u alive=%d combat=%d master=%s",
                    run.runId, run.index, p->GetName(), (uint32)p->getClass(),
                    PlayerbotAI::IsTank(p) ? 1 : 0,
                    PlayerbotAI::IsHeal(p) ? 1 : 0,
                    botAI->HasStrategy("tank assist", BotState::BOT_STATE_COMBAT) ? 1 : 0,
                    botAI->HasStrategy("dungeon route", BotState::BOT_STATE_NON_COMBAT) ? 1 : 0,
                    dungeonroute::ShouldDrive(botAI) ? 1 : 0,
                    (p->GetGroup() && p->GetGroup()->IsLeader(p->GetObjectGuid())) ? 1 : 0,
                    botAI->AllowActivity() ? 1 : 0,
                    p->GetMapId(), p->GetInstanceId(),
                    p->IsAlive() ? 1 : 0, p->IsInCombat() ? 1 : 0,
                    botAI->GetMaster() ? botAI->GetMaster()->GetName() : "-");
            }

            // 驱动坦克卡在哪：当前波 / FindTarget / 拉怪位距离 / 实际坐标 / 最近执行的动作
            //   [2026-10-18] "实际驱动坦克"= ShouldDrive 为真的那只（DungeonRoute 取 guid 最小的坦），
            //   不一定等于 run.tankGuid；多坦时监控错对象会看起来"坦克站着不动"。
            Player* tank = nullptr;
            for (uint32 guid : run.guids)
            {
                Player* p = FindPlayerInWorld(guid);
                PlayerbotAI* botAI = p ? p->GetPlayerbotAI() : nullptr;
                if (botAI && dungeonroute::ShouldDrive(botAI))
                {
                    tank = p;
                    break;
                }
            }
            if (!tank)
            {
                tank = FindPlayerInWorld(run.tankGuid);
                for (uint32 guid : run.guids)      // 退路：取 guid 最小的真坦
                {
                    Player* p = FindPlayerInWorld(guid);
                    if (p && PlayerbotAI::IsTank(p) && (!tank || p->GetGUIDLow() < tank->GetGUIDLow()))
                        tank = p;
                }
            }

            PlayerbotAI* tankAI = tank ? tank->GetPlayerbotAI() : nullptr;
            if (!tank || !tankAI || !route)
            {
                sLog.outError("[SIM] run=%u g=%u probe tank=%u ai=%d route=%d", run.runId, run.index,
                    run.tankGuid, tankAI ? 1 : 0, route ? 1 : 0);
                return;
            }

            Map* map = tank->GetMap();
            uint16 ord = dungeonroute::CurrentOrd(tank, *route);
            dungeonroute::Target target;
            bool const found = dungeonroute::FindTargetNearest(tank, *route, target);

            float dist = -1.0f;
            if (found && target.hasPullPoint)
                dist = tank->GetDistance(target.px, target.py, target.pz);

            sLog.outError("[SIM] run=%u g=%u probe tank=%s pos=(%.1f,%.1f,%.1f) map=%u ord=%u find=%d tg=%u entry=%u boss=%d hasPull=%d dist=%.1f pull=(%.1f,%.1f,%.1f) act=%s pullStrat=%d",
                run.runId, run.index, tank->GetName(),
                tank->GetPositionX(), tank->GetPositionY(), tank->GetPositionZ(), tank->GetMapId(),
                (uint32)ord, found ? 1 : 0, target.guid, target.entry, target.boss ? 1 : 0,
                target.hasPullPoint ? 1 : 0, dist, target.px, target.py, target.pz,
                tankAI->GetLastExecutedActionName(tankAI->GetState()).c_str(),
                tankAI->HasStrategy("pull", BotState::BOT_STATE_COMBAT) ? 1 : 0);

            // 前 3 波的存活数：route 表里的刷点 guid 能不能在运行期 map 里找到
            uint32 checked = 0;
            for (auto const& kv : route->waves)
            {
                if (checked >= 3)
                    break;
                ++checked;
                uint32 alive = 0;
                for (auto const& m : kv.second.members)
                    if (dungeonroute::MemberCreature(map, m))
                        ++alive;
                sLog.outError("[SIM] run=%u g=%u probe wave ord=%u alive=%u/%u anchor=%d",
                    run.runId, run.index, (uint32)kv.first, alive, (uint32)kv.second.members.size(),
                    kv.second.pull.has ? 1 : 0);
            }

            uint32 bossAlive = 0;
            for (auto const& b : route->bosses)
                if (dungeonroute::MemberCreature(map, dungeonroute::Member{ b.guid, b.entry }))
                    ++bossAlive;
            sLog.outError("[SIM] run=%u g=%u probe bosses alive=%u/%u", run.runId, run.index,
                bossAlive, (uint32)route->bosses.size());
        }

        // ---- [2026-10-18] bot 制备：把 RNDBOT 账号上的 1 级空壳升到 70 级 + 天赋/装备，并登记为随机 bot ----
        struct PrepareState
        {
            bool active = false;
            uint32 startedAt = 0;
            uint32 nextTryAt = 0;
            std::vector<uint32> guids;
            size_t done = 0;
        };
        PrepareState s_prepare;

        void StartPrepare(uint32 count, std::list<std::string>& messages)
        {
            if (s_prepare.active)
            {
                messages.push_back("制备已在进行中");
                return;
            }

            std::string accounts;
            {
                std::unique_ptr<QueryResult> acc(LoginDatabase.PQuery(
                    "SELECT id FROM account WHERE username LIKE 'RNDBOT%%' ORDER BY id"));
                if (acc)
                {
                    do
                    {
                        if (!accounts.empty())
                            accounts += ",";
                        accounts += std::to_string(acc->Fetch()[0].GetUInt32());
                    } while (acc->NextRow());
                }
            }
            if (accounts.empty())
            {
                messages.push_back("没有 RNDBOT 账号，无法制备");
                return;
            }

            std::vector<uint32> guids;
            std::unique_ptr<QueryResult> res(CharacterDatabase.PQuery(
                "SELECT c.guid FROM characters c WHERE c.account IN (%s) AND c.level < 70 "
                "AND NOT EXISTS (SELECT 1 FROM ai_playerbot_random_bots r WHERE r.bot = c.guid AND r.owner = 0) "
                "ORDER BY c.guid LIMIT %u", accounts.c_str(), count));
            if (res)
            {
                do { guids.push_back(res->Fetch()[0].GetUInt32()); } while (res->NextRow());
            }
            if (guids.empty())
            {
                messages.push_back("没有可制备的 1 级空壳角色了");
                return;
            }

            s_prepare = PrepareState();
            s_prepare.active = true;
            s_prepare.startedAt = Now();
            s_prepare.guids = guids;
            sLog.outError("[SIM] prepare start count=%u", (uint32)guids.size());
            messages.push_back("开始制备 " + std::to_string(guids.size()) + " 个 bot（登录→Randomize(70)→登记→下线）");
        }

        void TickPrepare()
        {
            if (!s_prepare.active)
                return;

            uint32 const now = Now();
            bool requestLogin = (now >= s_prepare.nextTryAt);
            if (requestLogin)
                s_prepare.nextTryAt = now ? now + 15000 : 1;

            for (uint32 guid : s_prepare.guids)
            {
                Player* bot = FindPlayerInWorld(guid);
                if (!bot)
                {
                    if (requestLogin)
                        sRandomPlayerbotMgr.AddPlayerBot(guid, sObjectMgr.GetPlayerAccountIdByGUID(GuidOf(guid)));
                    continue;
                }

                if (!bot->GetPlayerbotAI())
                    continue;

                PlayerbotFactory factory(bot, 70);
                factory.Randomize(false, false);
                bot->SaveToDB();   // [站长 2026-10-20] prepare 必须落库，否则 70 级只在内存

                CharacterDatabase.PExecute(
                    "INSERT INTO ai_playerbot_random_bots (owner,bot,time,validIn,event,value) "
                    "VALUES (0,'%u',UNIX_TIMESTAMP(),0,'prepared',1)", guid);

                sLog.outError("[SIM] prepare ok guid=%u %s lv=%u cls=%u", guid, bot->GetName(),
                    bot->GetLevel(), (uint32)bot->getClass());

                sRandomPlayerbotMgr.LogoutPlayerBot(guid);
                ++s_prepare.done;
            }

            if (s_prepare.done >= s_prepare.guids.size() || now - s_prepare.startedAt > 900000)
            {
                sLog.outError("[SIM] prepare finish %u/%u", (uint32)s_prepare.done, (uint32)s_prepare.guids.size());
                s_prepare.active = false;
            }
        }

        // ---- 收尾 ------------------------------------------------------------
        void FormatRunGuids(GroupRun const& run, std::string& out)
        {
            out.clear();
            for (size_t i = 0; i < run.guids.size(); ++i)
            {
                Player* p = FindPlayerAny(run.guids[i]);
                if (i)
                    out += ",";
                out += p ? p->GetName() : std::to_string(run.guids[i]);
            }
        }

        void CloseRun(GroupRun& run, bool ok, std::string const& error)
        {
            // ① 解散队伍（Disband 只清空成员，还需要 RemoveGroup + delete）
            if (run.group)
            {
                Group* group = run.group;
                run.group = nullptr;
                group->Disband(true);
                sObjectMgr.RemoveGroup(group);
                delete group;
            }

            // ② 下线本组 bot
            uint32 offline = 0;
            for (uint32 guid : run.guids)
            {
                Player* p = FindPlayerAny(guid);
                if (!p)
                    continue;
                if (p->GetGroup())
                    p->RemoveFromGroup();
                sRandomPlayerbotMgr.LogoutPlayerBot(guid);
                ++offline;
            }

            RunReport report;
            report.runId = run.runId;
            report.index = run.index;
            report.mapId = run.mapId;
            report.ok = ok;
            report.error = error;
            report.guids = run.guids;
            report.totalMs = Now() - run.startedAt;
            report.reachedDungeon = (run.enteredDungeonAt != 0);
            report.inDungeonMs = run.enteredDungeonAt ? (Now() - run.enteredDungeonAt) : 0;
            s_reports.push_back(report);
            if (s_reports.size() > 32)
                s_reports.erase(s_reports.begin());

            std::string names;
            FormatRunGuids(run, names);
            sLog.outError("[SIM] run=%u g=%u %s map=%u inst=%u total=%.1fs indungeon=%.1fs bots=%s%s%s",
                run.runId, run.index, ok ? "done" : "FAILED", run.mapId, run.instanceId,
                report.totalMs / 1000.0f, report.inDungeonMs / 1000.0f, names.c_str(),
                error.empty() ? "" : " error=", error.c_str());

            // 整个 run 的 attempt 汇总（灭团即重置模式下这是最重要的产出）
            if (run.attemptsDone || !run.attemptSummary.empty())
            {
                sLog.outError("[SIM] run=%u g=%u attempts=%u best=%u/%u seed=%u policy=%s %s",
                    run.runId, run.index, run.attemptsDone,
                    (uint32)run.bestCleared, (uint32)run.routeWaves, run.baseSeed,
                    run.policy ? "continue" : "reset",
                    run.attemptSummary.empty() ? "(no attempt recorded)" : run.attemptSummary.c_str());
            }
        }

        void RestoreRouteSwitchIfIdle()
        {
            if (!s_runs.empty())
                return;

            if (s_routeOverridden)
            {
                dungeonroute::RouteRuntimeSwitch() = s_savedRouteSwitch;
                s_routeOverridden = false;
                sLog.outError("[SIM] 路线开关恢复为 %d", s_savedRouteSwitch);
            }

            if (s_activityPrioritiesOverridden)
            {
                sPlayerbotAIConfig.disableActivityPriorities = s_savedDisableActivityPriorities;
                s_activityPrioritiesOverridden = false;
                sLog.outError("[SIM] 机器人活跃度优先级恢复为 %d", s_savedDisableActivityPriorities ? 1 : 0);
            }
        }

        // ---- [2026-10-18 实验] attempt 记录 / 复活 / 波次统计 --------------------
        void RecordAttempt(GroupRun& run, char const* result)
        {
            ++run.attemptsDone;
            uint32 const dur = run.attemptStartedAt ? (Now() - run.attemptStartedAt) : 0;

            std::string line = "attempt " + std::to_string(run.attempt) + " result=" + result
                + " waves=" + std::to_string((uint32)run.attemptBest)
                + (run.routeWaves ? "/" + std::to_string((uint32)run.routeWaves) : "")
                + " deaths=" + std::to_string(run.deaths)
                + " dur=" + std::to_string(dur / 1000) + "s";

            if (!run.attemptSummary.empty())
                run.attemptSummary += " | ";
            run.attemptSummary += line;

            sLog.outError("[SIM] run=%u g=%u %s seed=%u policy=%s",
                run.runId, run.index, line.c_str(), run.baseSeed, run.policy ? "continue" : "reset");

            run.attemptBest = 0;
            run.deaths = 0;
            run.lastDead = 0;
            run.attemptStartedAt = 0;
        }

        // 复活全队（可选：拉回副本入口），返回实际复活人数
        uint32 ReviveParty(GroupRun& run, bool teleportBack)
        {
            uint32 revived = 0;
            for (uint32 guid : run.guids)
            {
                Player* p = FindPlayerInWorld(guid);
                if (!p)
                    continue;

                if (!p->IsAlive())
                {
                    p->ResurrectPlayer(1.0f);
                    p->SpawnCorpseBones();
                    ++revived;
                }

                p->SetHealthPercent(100.0f);
                if (p->GetMaxPower(POWER_MANA))
                    p->SetPower(POWER_MANA, p->GetMaxPower(POWER_MANA));
                p->CombatStopWithPets(true);

                if (PlayerbotAI* botAI = p->GetPlayerbotAI())
                {
                    botAI->ResetStrategies();
                    if (teleportBack && p->GetMapId() == run.mapId && !p->IsBeingTeleported())
                        p->TeleportTo(run.mapId, run.ex, run.ey, run.ez, run.eo);
                }
            }
            return revived;
        }

        // [2026-10-28 站长口径] **复活点 = 当前 stage 的起点**（即上一个 stage 结束的位置）。
        //   stage 在 `run.stageStart[]` 里按"进入时坦克位置"记录；不要用"最后一波拉怪位"，
        //   也不要再加"25 码内有怪就换地方"这种兜底——要的就是固定回 stage 起点。
        bool PreviousStageRevivePoint(GroupRun& run, float& x, float& y, float& z)
        {
            if (!run.lastStageOrd)
                return false;
            auto it = run.stageStart.find(run.lastStageOrd);
            if (it == run.stageStart.end())
                return false;
            x = it->second.x;
            y = it->second.y;
            z = it->second.z;
            return true;
        }

        uint32 TeleportParty(GroupRun& run, float x, float y, float z)
        {
            uint32 n = 0;
            for (uint32 guid : run.guids)
            {
                Player* p = FindPlayerInWorld(guid);
                if (p && p->GetMapId() == run.mapId && !p->IsBeingTeleported())
                {
                    p->TeleportTo(run.mapId, x, y, z, 0.0f);
                    ++n;
                }
            }
            return n;
        }

        // [2026-10-19] 放弃一波后**强制脱战**：否则全队卡在"打不到/打不动"的怪身上，
        //   ShouldDrive 要求非战斗 ⇒ 坦克永远无法走去下一波（实测一次水里的 Greater Bogstrok 卡 200 秒）。
        void ForceDisengage(GroupRun& run)
        {
            for (uint32 guid : run.guids)
            {
                Player* p = FindPlayerInWorld(guid);
                if (!p)
                    continue;
                p->CombatStopWithPets(true);
                p->getHostileRefManager().deleteReferences();
                p->SetSelectionGuid(ObjectGuid());
                if (p->IsNonMeleeSpellCasted(false))
                    p->InterruptNonMeleeSpells(false);
            }
        }

        // ---- 每 tick 的阶段推进（返回 false = 本组结束）------------------------
        bool TickRun(GroupRun& run, uint32 now)
        {
            switch (run.phase)
            {
                case PHASE_LOGIN:
                {
                    uint32 online = 0;
                    for (size_t i = 0; i < run.guids.size(); ++i)
                    {
                        if (FindPlayerInWorld(run.guids[i]))
                        {
                            ++online;
                            continue;
                        }

                        uint32& nextTry = run.nextLoginTryAt[i];
                        if (now < nextTry)
                            continue;

                        uint32 accountId = sObjectMgr.GetPlayerAccountIdByGUID(GuidOf(run.guids[i]));
                        if (!accountId)
                        {
                            // [2026-10-18] 单个角色账号缺失不终止整组：从候选里剔除并继续等（组不齐会走角色重选）
                            sLog.outError("[SIM] run=%u g=%u 账号缺失，剔除候选 guid=%u",
                                run.runId, run.index, run.guids[i]);
                            run.guids.erase(run.guids.begin() + i);
                            run.nextLoginTryAt.erase(run.nextLoginTryAt.begin() + i);
                            return true;
                        }

                        sRandomPlayerbotMgr.AddPlayerBot(run.guids[i], accountId);
                        nextTry = now + 3000;                  // [2026-10-22] 15s→3s：登录重试太慢是准备阶段的主要耗时
                    }

                    if (online == run.guids.size())
                    {
                        run.phase = PHASE_FORM;
                        run.phaseSince = now;
                        sLog.outError("[SIM] run=%u g=%u online %u/%u in %.1fs", run.runId, run.index,
                            online, (uint32)run.guids.size(), (now - run.startedAt) / 1000.0f);
                    }
                    else if (now - run.phaseSince > 90000)
                    {
                        std::string names;
                        FormatRunGuids(run, names);
                        run.error = "登录超时 " + std::to_string(online) + "/" + std::to_string(run.guids.size()) + " bots=" + names;
                        CloseRun(run, false, run.error);
                        return false;
                    }
                    return true;
                }

                case PHASE_FORM:
                {
                    // 等 AI 起来（OnBotLogin 里会 SetActionDuration(2~4s)）
                    if (now - run.phaseSince < 3000)
                        return true;

                    std::string error;

                    // [2026-10-18 站长口径] 只用**真专精**的坦克/治疗，不用 DPS 顶替
                    if (!SelectRealRoles(run, run.partySize, error))
                    {
                        // 候选里没有真坦/真奶（种子/轮换随机导致）⇒ 不终止整个 run：
                        //   走一次重置换人重选（pickSeed 里叠加 roleRetries，保证换到不同的人）。
                        ++run.roleRetries;
                        sLog.outError("[SIM] run=%u g=%u 角色不足（%s）⇒ 换人重选第 %u 次",
                            run.runId, run.index, error.c_str(), run.roleRetries);
                        if (run.roleRetries > 20)
                        {
                            CloseRun(run, false, error);
                            return false;
                        }
                        run.phase = PHASE_RESET;
                        run.phaseSince = now;
                        run.nextResetTryAt = now;
                        return true;
                    }

                    if (!CreateBotParty(run, error))
                    {
                        CloseRun(run, false, error);
                        return false;
                    }

                    sLog.outError("[SIM] run=%u g=%u group id=%u lead=%s members=%u tank=%s heal=%s",
                        run.runId, run.index, run.group ? run.group->GetId() : 0,
                        run.group ? run.group->GetLeaderName() : "?",
                        run.group ? run.group->GetMembersCount() : 0,
                        FindPlayerInWorld(run.tankGuid) ? FindPlayerInWorld(run.tankGuid)->GetName() : "?",
                        FindPlayerInWorld(run.healGuid) ? FindPlayerInWorld(run.healGuid)->GetName() : "?");

                    // attempt 计数从"进本"开始算
                    run.attemptBest = 0;
                    run.roleRetries = 0;
                    run.seenWaves.clear();
                    run.lastAliveMobs = 0;
                    run.deaths = 0;
                    run.combatSince = 0;
                    run.combatOrd = 0;
                    run.combatSkipDone = false;
                    run.noVictimSince = 0;
                    run.lastDead = 0;
                    run.lastCleared = 0;
                    run.wipeSince = 0;
                    run.lastProgressAt = now;

                    run.phase = PHASE_TELEPORT;
                    run.phaseSince = now;
                    return true;
                }

                case PHASE_TELEPORT:
                {
                    CheckRoles(run, now);

                    Player* leader = FindPlayerInWorld(run.guids[0]);
                    if (!leader)
                    {
                        CloseRun(run, false, "队长在传送阶段掉线");
                        return false;
                    }

                    // ① 先队长进本：由他创建 instance 并把队伍绑定进去
                    //   [2026-10-20 站长口径] 不能只看 GetMapId：角色可能是死在本里后被复活在
                    //   墓地/入口旧坐标（实例里 0,0 附近），map 相同但离入口 2000 码。
                    //   这里第一次一定先传送到 areatrigger 入口，落地后才算"已进本"。
                    if (!run.leaderTeleported)
                    {
                        if (now - run.phaseSince > 60000)
                        {
                            CloseRun(run, false, "队长进本超时");
                            return false;
                        }

                        if (leader->IsBeingTeleported())
                            return true;

                        if (run.leaderTryAt && leader->GetMapId() == run.mapId)
                        {
                            run.leaderTeleported = true;
                            run.phaseSince = now;
                            sLog.outError("[SIM] run=%u g=%u leader %s 已进本 map=%u inst=%u",
                                run.runId, run.index, leader->GetName(), run.mapId, leader->GetInstanceId());
                            return true;
                        }

                        if (now - run.leaderTryAt < 3000)
                            return true;

                        run.leaderTryAt = now ? now : 1;
                        leader->TeleportTo(run.mapId, run.ex, run.ey, run.ez, run.eo);
                        sLog.outError("[SIM] run=%u g=%u teleport leader %s -> map=%u (%.1f,%.1f,%.1f)",
                            run.runId, run.index, leader->GetName(), run.mapId, run.ex, run.ey, run.ez);
                        return true;
                    }

                    // ② 其余成员再进本（此时队伍已经绑定 instance）
                    //   [2026-10-19 修] 必须**逐个**传、等落地再传下一个：同一 tick 批量传会各自创建
                    //   一个 instance（实测 5 人分到 inst 11/12/13，导致只有 3 人一起打、清波统计为 0）。
                    //   [2026-10-20 站长口径] 每个成员第一次也一定先传入口：不能因为"map 已经对"就跳过，
                    //   否则会保留角色登录时的旧坐标（实例里 0,0 附近，离入口 2000 码）。
                    if (!run.membersTeleported)
                    {
                        for (; run.memberIdx < run.guids.size(); ++run.memberIdx)
                        {
                            Player* p = FindPlayerInWorld(run.guids[run.memberIdx]);
                            if (!p)
                                continue;

                            if (run.memberTryAt)
                            {
                                if (p->IsBeingTeleported())
                                    return true;                    // 等它落地
                                if (p->GetMapId() == run.mapId)
                                {
                                    run.memberTryAt = 0;            // 已落地 → 传下一个
                                    continue;
                                }
                                if (now - run.memberTryAt < 3000)
                                    return true;
                            }

                            run.memberTryAt = now ? now : 1;
                            p->TeleportTo(run.mapId, run.ex, run.ey, run.ez, run.eo);
                            sLog.outError("[SIM] run=%u g=%u teleport member %s（逐个传送）",
                                run.runId, run.index, p->GetName());
                            return true;
                        }

                        run.membersTeleported = true;
                        run.phase = PHASE_MOVEIN;

                        run.phaseSince = now;
                        return true;
                    }
                    return true;
                }

                case PHASE_MOVEIN:
                {
                    CheckRoles(run, now);

                    Player* leader = FindPlayerInWorld(run.guids[0]);
                    uint32 const leadInst = (leader && leader->GetMapId() == run.mapId && !leader->IsBeingTeleported())
                        ? leader->GetInstanceId() : 0;

                    // [2026-10-19 修] 任何成员不在"队长的 instance"里：先送出本图、再传回队长身边
                    if (leadInst)
                    {
                        for (uint32 guid : run.guids)
                        {
                            Player* p = FindPlayerInWorld(guid);
                            if (!p || p == leader || p->IsBeingTeleported())
                                continue;
                            if (p->GetMapId() == run.mapId && p->GetInstanceId() == leadInst)
                                continue;

                            if (now >= run.nextRepairAt)
                            {
                                run.nextRepairAt = now + 2000;
                                if (p->GetMapId() == run.mapId)
                                {
                                    p->TeleportToHomebind();
                                    sLog.outError("[SIM] run=%u g=%u 修正 instance：%s 在 inst=%u（队长 inst=%u）⇒ 送回主城",
                                        run.runId, run.index, p->GetName(), p->GetInstanceId(), leadInst);
                                }
                                else
                                {
                                    p->TeleportTo(run.mapId, leader->GetPositionX(), leader->GetPositionY(),
                                                  leader->GetPositionZ(), leader->GetOrientation());
                                    sLog.outError("[SIM] run=%u g=%u 修正 instance：%s 传回队长身边（inst=%u）",
                                        run.runId, run.index, p->GetName(), leadInst);
                                }
                            }
                        }
                    }

                    uint32 inside = 0;
                    for (uint32 guid : run.guids)
                    {
                        Player* p = FindPlayerInWorld(guid);
                        if (p && !p->IsBeingTeleported() && p->GetMapId() == run.mapId
                            && leadInst && p->GetInstanceId() == leadInst)
                            ++inside;
                    }

                    if (inside == run.guids.size())
                    {
                        Player* leader = FindPlayerInWorld(run.guids[0]);
                        run.instanceId = leader ? leader->GetInstanceId() : 0;
                        run.enteredDungeonAt = now;
                        run.lastStatusAt = now;
                        run.phase = PHASE_RUN;
                        run.attemptStartedAt = now;
                        run.lastProgressAt = now;
                        run.attemptBest = 0;
                        run.deaths = 0;
                        run.lastDead = 0;
                        run.lastCleared = 0;
                        run.wipeSince = 0;
                        run.diagLeft = 8;
                        sLog.outError("[SIM] run=%u g=%u IN DUNGEON map=%u inst=%u members=%u attempt=%u/%u t=%.1fs",
                            run.runId, run.index, run.mapId, run.instanceId, inside,
                            run.attempt, run.maxAttempts, (now - run.startedAt) / 1000.0f);
                        // 进本后第一次诊断：驱动判据/route 数据（拉不动怪时看这一行就够）
                        LogDiagnostics(run);
                        return true;
                    }

                    if (now - run.phaseSince > 60000)
                    {
                        run.error = "只有 " + std::to_string(inside) + "/" + std::to_string(run.guids.size()) + " 落在副本图";
                        CloseRun(run, false, run.error);
                        return false;
                    }
                    return true;
                }

                case PHASE_RUN:
                {
                    CheckRoles(run, now);

                    // [2026-10-23 站长指令] 已删除"越界救回"：那是补丁式保护，
                    //   第一版误判 9 万次、把整轮吃掉；正确做法是让寻路/推进自己收束。

                    // [2026-10-23 站长口径] 保证进入战斗：整组连续 10 秒没有成员在战斗 ⇒
                    //   给坦克排一次 attack anything（AI 自己找最近可打目标），每 10 秒最多一次。
                    //   解决"坦克无目标/check values 站桩"，不是限制开怪。
                    {
                        bool anyFight = false;
                        for (uint32 g : run.guids)
                        {
                            Player* p = FindPlayerInWorld(g);
                            if (p && p->IsInCombat())
                            {
                                anyFight = true;
                                break;
                            }
                        }
                        uint32 const nowMs = now ? now : 1;
                        if (anyFight)
                            run.idleSince = 0;
                        else
                        {
                            if (!run.idleSince)
                                run.idleSince = nowMs;
                            if (WorldTimer::getMSTimeDiff(run.idleSince, nowMs) >= 10000 &&
                                (!run.forceEngageAt || WorldTimer::getMSTimeDiff(run.forceEngageAt, nowMs) >= 10000))
                            {
                                Player* tank = run.guids.empty() ? nullptr : FindPlayerInWorld(run.guids[0]);
                                if (tank && tank->GetPlayerbotAI())
                                {
                                    uint32 const idleSec = uint32(WorldTimer::getMSTimeDiff(run.idleSince, nowMs) / 1000);
                                    // [2026-10-25 站长口径] **超过 240 秒完全没进战斗 ⇒ 这一组已经僵死**：
                                    //   只发 "attack anything" 是救不回来的（实测 60 组全部卡在 1070+ 秒）。
                                    //   这里做一次**真正的重置**：把路线状态清掉、全队复活并送回副本入口，
                                    //   让它们从头再走一遍（attempts=0 下相当于"软重开"）。
                                    if (idleSec >= 240)
                                    {
                                        // [2026-10-25 只读诊断] 兜底重置时把现场记下来：
                                        //   位置/地图/实例/离入口多远 —— 用来抓"被丢到副本有效区域外"的那一组。
                                        if (Player* t = FindPlayerInWorld(run.guids[0]))
                                        {
                                            sLog.outError("[OOB] run=%u g=%u reset: tank=%s pos=(%.1f,%.1f,%.1f) "
                                                "map=%u inst=%u dist_entry=%.0f",
                                                run.runId, run.index, t->GetName(),
                                                t->GetPositionX(), t->GetPositionY(), t->GetPositionZ(),
                                                t->GetMapId(), t->GetInstanceId(),
                                                t->GetDistance(run.ex, run.ey, run.ez));
                                        }
                                        for (uint32 guid : run.guids)
                                        {
                                            Player* p = FindPlayerInWorld(guid);
                                            if (!p)
                                                continue;
                                            dungeonroute::BotState& bs = dungeonroute::StateOf(p);
                                            bs.stageOrd = 0;
                                            bs.recStage = 0xFFFF;
                                            bs.recPath.clear();
                                            bs.pullWaitUntil = 0;
                                            bs.gatherSince = 0;
                                            bs.standOverrideUntil = 0;
                                            bs.pullBackPending = 0;
                                            bs.safePullPoint = false;
                                            bs.liveCensusAt = 0;
                                            bs.deadEndLogAt = 0;
                                            if (!p->IsAlive())
                                                p->ResurrectPlayer(1.0f, false);
                                        }
                                        ReviveParty(run, false);
                                        sLog.outError("[SIM] run=%u g=%u 连续 %us 未进战斗 ⇒ **重置推进状态**（清路线 + 复活 + 回入口）",
                                            run.runId, run.index, idleSec);
                                        run.idleSince = nowMs;
                                        run.forceEngageAt = nowMs;
                                    }
                                    tank->GetPlayerbotAI()->QueueSimAction("attack anything");
                                    run.forceEngageAt = nowMs;
                                    sLog.outError("[SIM] run=%u g=%u 连续 %us 未进战斗 ⇒ 强制 attack anything",
                                        run.runId, run.index, idleSec);
                                }
                            }
                        }
                    }

                    // [2026-10-26 诊断 站长批准] **[STALEFIGHT]**：整组在战斗、但 60 秒没有任何波次推进
                    //   ⇒ 把缠着队伍的那几只怪打出来（entry / hp% / 离坦克多远 / 坦克够不够得着 / 是否坦克目标）。
                    //   用途：定位"打不死也死不了"的僵持（实测 555 有一组 combat=4 连续 20 分钟 cleared 不进）。
                    if (!run.staleLogAt || WorldTimer::getMSTimeDiff(run.staleLogAt, now ? now : 1) >= 30000)
                    {
                        run.staleLogAt = now ? now : 1;
                        uint32 const noProg = run.lastProgressAt
                            ? WorldTimer::getMSTimeDiff(run.lastProgressAt, now ? now : 1) : 0;
                        bool anyCombat = false;
                        for (uint32 g : run.guids)
                        {
                            Player* p = FindPlayerInWorld(g);
                            if (p && p->IsInCombat())
                            {
                                anyCombat = true;
                                break;
                            }
                        }
                        if (anyCombat && noProg >= 60000)
                        {
                            Player* tank = nullptr;
                            for (uint32 g : run.guids)
                            {
                                Player* p = FindPlayerInWorld(g);
                                if (p && PlayerbotAI::IsTank(p) && p->IsAlive())
                                {
                                    tank = p;
                                    break;
                                }
                            }
                            std::set<ObjectGuid> seen;
                            std::string detail;
                            // [2026-10-26 诊断] 这只怪"血量没变"的秒数（0 = 刚变过/第一次见）
                            auto hpuSec = [&](Unit* u) -> uint32
                            {
                                if (!u)
                                    return 0;
                                uint32 const nowL = now ? now : 1;
                                uint32 const key = u->GetGUIDLow();
                                uint32 const hp = uint32(u->GetHealthPercent());
                                auto it = run.lastHpOf.find(key);
                                if (it == run.lastHpOf.end() || it->second.first != hp)
                                {
                                    run.lastHpOf[key] = std::make_pair(hp, nowL);
                                    return 0;
                                }
                                return WorldTimer::getMSTimeDiff(it->second.second, nowL) / 1000;
                            };
                            uint32 idx = 0;
                            for (uint32 g : run.guids)
                            {
                                Player* p = FindPlayerInWorld(g);
                                if (!p)
                                    continue;
                                for (Unit* a : p->getAttackers())
                                {
                                    if (!a || a->GetTypeId() != TYPEID_UNIT || !seen.insert(a->GetObjectGuid()).second)
                                        continue;
                                    if (++idx > 5)
                                        break;
                                    char buf[220];
                                    snprintf(buf, sizeof(buf),
                                        " %u(evt=%d,tmr=%u,hp=%u%%,hpu=%us,d=%.0f,vic=%s,reach=%d,victimTank=%d)",
                                        a->GetEntry(),
                                        a->GetCombatManager().IsInEvadeMode() ? 1 : 0,
                                        a->GetCombatManager().GetEvadeTimer(),
                                        uint32(a->GetHealthPercent()), hpuSec(a),
                                        tank ? tank->GetDistance(a) : -1.0f,
                                        a->GetVictim() ? a->GetVictim()->GetName() : "-",
                                        (tank && a->CanReachWithMeleeAttack(tank)) ? 1 : 0,
                                        (tank && a->GetVictim() == tank) ? 1 : 0);
                                    detail += buf;
                                }
                            }
                            if (detail.empty())
                                detail = " (攻击者列表为空：只剩战斗标记)";
                            sLog.outError("[STALEFIGHT] run=%u g=%u t=%us 无推进=%us 坦克=%s 目标=%s%s",
                                run.runId, run.index, uint32((now - run.startedAt) / 1000),
                                noProg / 1000,
                                tank ? tank->GetName() : "-",
                                (tank && tank->GetVictim()) ? tank->GetVictim()->GetName() : "-",
                                detail.c_str());

                            // [2026-10-27 诊断] **打不动的架**：无推进 ≥120s 时，把每个成员的
                            //   最后动作 / AI 当前目标 / 可攻击候选数打出来，回答"怪在 5 码内、血却 8 分钟不动"。
                            //   纯仿真诊断（不进生产路径）。
                            if (noProg >= 120000)
                            {
                                std::string why;
                                for (uint32 g : run.guids)
                                {
                                    Player* p2 = FindPlayerInWorld(g);
                                    if (!p2)
                                        continue;
                                    PlayerbotAI* pai = p2->GetPlayerbotAI();
                                    std::string actNow = "-";
                                    uint32 poss = 0;
                                    std::string aiTgt = "-";
                                    if (pai)
                                    {
                                        actNow = pai->GetLastExecutedActionName(BotState::BOT_STATE_COMBAT);
                                        if (actNow.empty())
                                            actNow = "-";
                                        AiObjectContext* ctx = pai->GetAiObjectContext();
                                        if (ctx)
                                        {
                                            if (auto* v = ctx->GetValue<std::list<ObjectGuid>>("possible attack targets"))
                                                poss = uint32(v->Get().size());
                                            ObjectGuid tg = ctx->GetValue<ObjectGuid>("current target")->Get();
                                            if (Unit* tu = pai->GetUnit(tg))
                                                aiTgt = tu->GetName();
                                        }
                                    }
                                    char b4[260];
                                    snprintf(b4, sizeof(b4), " %s(act=%s,aiTgt=%s,poss=%u,vic=%s)",
                                        p2->GetName(), actNow.c_str(), aiTgt.c_str(), poss,
                                        p2->GetVictim() ? p2->GetVictim()->GetName() : "-");
                                    why += b4;
                                }
                                sLog.outError("[STUCKWHY] run=%u g=%u 无推进=%us%s",
                                    run.runId, run.index, noProg / 1000, why.c_str());
                            }

                            // [2026-10-26 诊断 站长批准] **[GHOSTCOMBAT]**：整组在战斗、但攻击者列表为空。
                            //   这种状态会让 tank 一直 IsInCombat ⇒ ShouldDrive 永久 false ⇒
                            //   路线驱动关闭、不推进、也不挨打（"零死亡零推进"的真凶之一）。
                            //   这里把每个人的战斗状态/目标/宠物都打出来，用来区分：
                            //     ① 怪已经死了/消失了，战斗标记没清；② 宠物还在战斗；③ 目标是个不可攻击的假目标。
                            if (idx == 0)
                            {
                                std::string who;
                                for (uint32 g : run.guids)
                                {
                                    Player* p2 = FindPlayerInWorld(g);
                                    if (!p2)
                                        continue;
                                    // [2026-10-26 诊断] act=<最后动作>/<持续秒数>：回答"锁着目标为什么不动"
                                    std::string actNow = "-";
                                    if (PlayerbotAI* pai = p2->GetPlayerbotAI())
                                    {
                                        actNow = pai->GetLastExecutedActionName(BotState::BOT_STATE_COMBAT);
                                        if (actNow.empty())
                                            actNow = "-";
                                    }
                                    uint32 actSec = 0;
                                    {
                                        auto itA = run.lastActOf.find(g);
                                        if (itA == run.lastActOf.end() || itA->second.first != actNow)
                                            run.lastActOf[g] = std::make_pair(actNow, now ? now : 1);
                                        else
                                            actSec = WorldTimer::getMSTimeDiff(itA->second.second, now ? now : 1) / 1000;
                                    }
                                    char b2[340];
                                    Unit* pet = p2->GetPet();
                                    // [2026-10-26] href=我这边的敌对引用数（脱战就看它是否归零）
                                    //              atkM=正在打我的人/怪数量
                                    snprintf(b2, sizeof(b2),
                                        " %s(cb=%d,href=%u,atkM=%u,tgt=%s,act=%s/%us,pet=%s%s)",
                                        p2->GetName(), p2->IsInCombat() ? 1 : 0,
                                        uint32(p2->getHostileRefManager().getSize()),
                                        uint32(p2->getAttackers().size()),
                                        p2->GetVictim() ? p2->GetVictim()->GetName() : "-",
                                        actNow.c_str(), actSec,
                                        pet ? pet->GetName() : "-",
                                        (pet && pet->IsInCombat()) ? "(cb)" : "");
                                    who += b2;
                                }
                                // [2026-10-26 补] 把"我们锁定的目标怪"的状态也打出来：
                                //   evade=0 正常 / 1 副本内不可达(闪避全部伤害且回血) / 2 跑回家
                                //   vic=它的目标 / cb=它在不在战斗 / hp=血量百分比
                                std::string tgtInfo;
                                std::set<ObjectGuid> seenT;
                                uint32 ti = 0;
                                for (uint32 g : run.guids)
                                {
                                    Player* p2 = FindPlayerInWorld(g);
                                    Unit* v = p2 ? p2->GetVictim() : nullptr;
                                    if (!v || v->GetTypeId() != TYPEID_UNIT || !seenT.insert(v->GetObjectGuid()).second)
                                        continue;
                                    if (++ti > 3)
                                        break;
                                    Creature* c = static_cast<Creature*>(v);
                                    char b3[240];
                                    // [2026-10-26 修正] evt=IsInEvadeMode()（含 10 秒计时器，真正让伤害变 EVADE 的东西）
                                    //   evs=GetEvadeState()（状态机枚举，只有跑回家 EVADE_HOME 才会非 0）
                                    // [一次性取证] AI / 状态机 / 威胁表全量
                                    UnitAI* cai = c->AI();
                                    std::string refs;
                                    {
                                        uint32 rn = 0;
                                        for (HostileReference* hr = c->getThreatManager().getThreatList().empty()
                                                ? nullptr : c->getHostileRefManager().getFirst(); hr && rn < 4; hr = hr->next(), ++rn)
                                        {
                                            char rb[160];
                                            snprintf(rb, sizeof(rb), " r%u[%s thr=%.0f st=%u taunt=%u on=%d acc=%d]",
                                                rn,
                                                hr->getSource() && hr->getSource()->getOwner() ? "src" : "?",
                                                hr->getThreat(), uint32(hr->GetHostileState()),
                                                uint32(hr->GetTauntState()), hr->isOnline() ? 1 : 0,
                                                hr->isAccessable() ? 1 : 0);
                                            refs += rb;
                                        }
                                    }
                                    snprintf(b3, sizeof(b3),
                                        " %s(%u)(evt=%d,tmr=%u,evs=%u,cbt=%u,leash=%d,"
                                        "cane=%d,cast=%d,react=%d,script=%d,mot=%u,"
                                        "thr=%u,curvic=%s,guidvic=%s,ai=%s,rst=%d,ord=%d,st=%x,fl=%x,cc=%d,"
                                        "hp=%u%%,hpu=%us,vic=%s,cb=%d,atk=%u,d=%.0f)%s",
                                        c->GetName(), c->GetEntry(),
                                        c->GetCombatManager().IsInEvadeMode() ? 1 : 0,
                                        c->GetCombatManager().GetEvadeTimer(),
                                        uint32(c->GetCombatManager().GetEvadeState()),
                                        c->GetCombatManager().GetCombatTimer(),
                                        c->GetCombatManager().IsLeashingDisabled() ? 1 : 0,
                                        (cai && cai->CanExecuteCombatAction()) ? 1 : 0,
                                        c->IsNonMeleeSpellCasted(false) ? 1 : 0,
                                        (cai && c->CanReactInCombat()) ? 1 : 0,
                                        (cai && cai->GetCombatScriptStatus()) ? 1 : 0,
                                        uint32(c->GetMotionMaster()->GetCurrentMovementGeneratorType()),
                                        uint32(c->getThreatManager().getThreatList().size()),
                                        c->getThreatManager().getCurrentVictim()
                                            ? c->getThreatManager().getCurrentVictim()->getTarget()->GetName() : "-",
                                        c->GetCombatManager().IsInEvadeMode() ? "ev?" :
                                            (c->GetGuidValue(UNIT_FIELD_TARGET) ? "set" : "empty"),
                                        cai ? typeid(*cai).name() : "noai",
                                        (cai && cai->IsTargetingRestricted()) ? 1 : 0,
                                        cai ? int32(cai->GetAIOrder()) : -1,
                                        c->hasUnitState(UNIT_STAT_CAN_NOT_REACT|UNIT_STAT_STUNNED|UNIT_STAT_ROOT|UNIT_STAT_PROPELLED|UNIT_STAT_RETREATING)?1:0,
                                        c->GetUInt32Value(UNIT_FIELD_FLAGS),
                                        c->IsCrowdControlled() ? 1 : 0,
                                        uint32(c->GetHealthPercent()), hpuSec(c),
                                        c->GetVictim() ? c->GetVictim()->GetName() : "-",
                                        c->IsInCombat() ? 1 : 0,
                                        uint32(c->getAttackers().size()),
                                        p2 ? p2->GetDistance(c) : -1.0f,
                                        refs.c_str());
                                    tgtInfo += b3;
                                }
                                if (tgtInfo.empty())
                                    tgtInfo = " (没有锁定任何怪)";

                                sLog.outError("[GHOSTCOMBAT] run=%u g=%u t=%us 无推进=%us 全体成员：%s | 目标怪：%s",
                                    run.runId, run.index, uint32((now - run.startedAt) / 1000),
                                    noProg / 1000, who.c_str(), tgtInfo.c_str());
                            }
                        }
                    }

                    // [2026-10-25 站长口径] **击飞/掉出网格兜底**：任何成员连续 30s 判不在导航网格上（且不在水里）
                    //   ⇒ 先保推进，把他传回队长身边（队长不在本图就回入口）。
                    //   击飞本身是"没躲好技能"，那是技能层的事；这里只保证不因为一个人掉出去整组卡死。
                    if (!run.oobFixAt || WorldTimer::getMSTimeDiff(run.oobFixAt, now ? now : 1) >= 10000)
                    {
                        run.oobFixAt = now ? now : 1;
                        Player* lead = FindPlayerInWorld(run.guids[0]);
                        for (uint32 guid : run.guids)
                        {
                            Player* p = FindPlayerInWorld(guid);
                            if (!p || p->GetMapId() != run.mapId || p->IsBeingTeleported())
                                continue;
                            Map* pm = p->GetMap();
                            if (!pm)
                                continue;
                            dungeonroute::BotState& bs = dungeonroute::StateOf(p);
                            if (OobOnNavMesh(pm, p->GetPositionX(), p->GetPositionY(), p->GetPositionZ()))
                            {
                                bs.oobStrikes = 0;
                                continue;
                            }
                            // [2026-10-25 站长口径] **不用 z 判水域**：游泳状态看移动标志 MOVEFLAG_SWIMMING。
                            //   游泳中不救（人可能只是在水里赶路），其余照常按"静止 + 离队长太远"判。
                            if (p->m_movementInfo.HasMovementFlag(MOVEFLAG_SWIMMING))
                            {
                                bs.oobStrikes = 0;
                                continue;
                            }
                            // 只救"真卡住的"：与上一次采样比基本没动（≤5 码）才累加；在正常赶路的不算
                            if (bs.oobStrikes == 0)
                            {
                                bs.oobX = p->GetPositionX();
                                bs.oobY = p->GetPositionY();
                                bs.oobZ = p->GetPositionZ();
                            }
                            else if (p->GetDistance(bs.oobX, bs.oobY, bs.oobZ) > 5.0f)
                            {
                                bs.oobX = p->GetPositionX();
                                bs.oobY = p->GetPositionY();
                                bs.oobZ = p->GetPositionZ();
                                bs.oobStrikes = 1;
                                continue;
                            }
                            if (++bs.oobStrikes < 3)
                                continue;
                            // 还得"离队长很远"（被击飞/掉出去的人通常已经脱离队伍），避免把正常站位的人拽回入口
                            float const dLeader = lead ? p->GetDistance(lead) : 999.0f;
                            if (dLeader <= 40.0f)
                            {
                                bs.oobStrikes = 0;
                                continue;
                            }

                            float tx = run.ex, ty = run.ey, tz = run.ez, to = p->GetOrientation();
                            bool const toLeader = lead && lead != p && lead->GetMapId() == run.mapId &&
                                lead->GetInstanceId() == run.instanceId &&
                                OobOnNavMesh(pm, lead->GetPositionX(), lead->GetPositionY(), lead->GetPositionZ());
                            if (toLeader)
                            {
                                tx = lead->GetPositionX();
                                ty = lead->GetPositionY();
                                tz = lead->GetPositionZ();
                                to = lead->GetOrientation();
                            }
                            sLog.outError("[OOB-RESCUE] run=%u g=%u bot=%s from=(%.1f,%.1f,%.1f) to=%s(%.1f,%.1f,%.1f) "
                                "dist=%.0f strikes=%u",
                                run.runId, run.index, p->GetName(),
                                p->GetPositionX(), p->GetPositionY(), p->GetPositionZ(),
                                toLeader ? "leader" : "entrance", tx, ty, tz,
                                p->GetDistance(tx, ty, tz), uint32(bs.oobStrikes));
                            p->GetMotionMaster()->Clear();
                            p->StopMoving();
                            p->TeleportTo(run.mapId, tx, ty, tz, to);
                            bs.oobStrikes = 0;
                        }
                    }

                    uint32 online = 0, alive = 0, inCombat = 0;
                    for (uint32 guid : run.guids)
                    {
                        Player* p = FindPlayerInWorld(guid);
                        if (!p)
                            continue;
                        ++online;
                        if (p->IsAlive())
                            ++alive;
                        if (p->IsInCombat())
                            ++inCombat;
                    }

                    // [2026-10-21] 记录"谁离开了副本图"（546 出现过 cleared=14/14 但 inst=0：
                    //   波次清完人却已经不在本里 ⇒ 需要知道是谁/哪个动作把队伍带出去的）。
                    // [2026-10-21] 记录并**拉回**：随机传送/轮换可能把单个队员拽出副本，
                    //   仿真期间直接把人传送回副本（队长位置，队长不在则入口）。
                    if (!run.outFixAt || now - run.outFixAt > 5000)
                    {
                        Player* leader = FindPlayerInWorld(run.guids[0]);
                        for (uint32 guid : run.guids)
                        {
                            Player* p = FindPlayerInWorld(guid);
                            if (!p || p->GetMapId() == run.mapId)
                                continue;

                            float tx = run.ex, ty = run.ey, tz = run.ez;
                            float to = run.eo;
                            if (leader && leader->GetMapId() == run.mapId)
                            {
                                tx = leader->GetPositionX();
                                ty = leader->GetPositionY();
                                tz = leader->GetPositionZ();
                                to = leader->GetOrientation();
                            }
                            sLog.outError("[SIM] run=%u g=%u 离本检测+拉回: %s map=%u → %u pos=(%.1f,%.1f,%.1f)",
                                run.runId, run.index, p->GetName(), p->GetMapId(), run.mapId,
                                p->GetPositionX(), p->GetPositionY(), p->GetPositionZ());
                            p->TeleportTo(run.mapId, tx, ty, tz, to);
                            run.outFixAt = now;
                        }
                    }

                    if (!online)
                    {
                        CloseRun(run, false, "全部掉线");
                        return false;
                    }

                    Player* leader = FindPlayerInWorld(run.guids[0]);
                    if (leader && !leader->GetGroup())
                    {
                        CloseRun(run, true, "队伍被解散");
                        return false;
                    }

                    // ---- 波次/死亡/推进 统计 [模式A：按"已清波数"算，不再依赖 ord 当前波] ----
                    Player* driver = nullptr;
                    for (uint32 guid : run.guids)
                    {
                        Player* p = FindPlayerInWorld(guid);
                        PlayerbotAI* botAI = p ? p->GetPlayerbotAI() : nullptr;
                        if (botAI && dungeonroute::ShouldDrive(botAI))
                        {
                            driver = p;
                            break;
                        }
                    }
                    Player* tank = driver ? driver : FindPlayerInWorld(run.tankGuid);
                    dungeonroute::MapRoute const* route =
                        (tank && tank->GetMap()) ? dungeonroute::GetMapRoute(run.mapId) : nullptr;

                    // 注意：统计状态要用**稳定的坦克**（run.tankGuid），不能用"当前驱动者"——
                    //   战斗里 ShouldDrive 为 false，驱动者为空，会把已清计数误报成 0。
                    dungeonroute::BotState* st = (tank && route) ? &dungeonroute::StateOf(tank) : nullptr;
                    uint16 const skipped = st ? (uint16)st->skip.size() : 0;

                    // [2026-10-28 站长口径] 记录"进入每个 stage 时"的坦克位置：
                    //   复活时用**当前 stage 的起点**（= 上一个 stage 结束的位置），而不是最后一波的位置。
                    if (st && tank)
                    {
                        uint16 const so = st->stageOrd;
                        if (so != run.lastStageOrd)
                        {
                            GroupRun::StagePos mark;
                            mark.x = tank->GetPositionX();
                            mark.y = tank->GetPositionY();
                            mark.z = tank->GetPositionZ();
                            run.stageStart[so] = mark;
                            run.lastStageOrd = so;
                        }
                    }

                    // [2026-10-28 站长口径] [REGROUP] 详情 + 等太久就 summon：
                    //   记录掉队者是谁、死活、距离、最近动作（看清是卡地形还是被怪缠住）；
                    //   等超过 60 秒还不到位 ⇒ 把全队（存活成员）直接拉到坦克身边，等价 summon。
                    if (st && tank && st->regroupSince)
                    {
                        uint32 const nowMs = WorldTimer::getMSTime();
                        uint32 const waited = WorldTimer::getMSTimeDiff(st->regroupSince, nowMs ? nowMs : 1);
                        Player* straggler = st->regroupStraggler ? FindPlayerInWorld(st->regroupStraggler) : nullptr;
                        std::string lastAct = "-";
                        uint32 actAgo = 0;
                        if (straggler)
                        {
                            auto it = run.lastActOf.find(straggler->GetGUIDLow());
                            if (it != run.lastActOf.end())
                            {
                                lastAct = it->second.first;
                                actAgo = nowMs - it->second.second;
                            }
                        }
                        if (!st->regroupLogAt || nowMs - st->regroupLogAt > 5000)
                        {
                            st->regroupLogAt = nowMs;
                            sLog.outError("[REGROUP] run=%u g=%u 坦克=%s 等 %s（%s 距离=%.0f 战斗=%d 最近动作=%s %us）已等 %us",
                                run.runId, run.index, tank->GetName(),
                                straggler ? straggler->GetName() : "?",
                                st->regroupStragglerDead ? "阵亡" : "掉队",
                                st->regroupDist,
                                (straggler && straggler->IsInCombat()) ? 1 : 0,
                                lastAct.c_str(), actAgo / 1000, waited / 1000);
                        }
                        if (waited >= 60000 && (!st->regroupSummonAt || nowMs - st->regroupSummonAt >= 30000))
                        {
                            // [2026-10-28 站长口径] 优先走**队伍 summon**（SendSummonRequest 正常请求路径）；
                            //   目标在战斗/死亡时请求会被拒 ⇒ 用 summon 路径里的线程安全传送兜底
                            //   （PlayerbotAI::RunOnOwningThread，等价"坦克把队员拉过来"），不再自己拍脑袋写循环。
                            st->regroupSummonAt = nowMs ? nowMs : 1;
                            PlayerbotAI* tankAI = tank->GetPlayerbotAI();
                            uint32 tp = 0, direct = 0;
                            for (uint32 g : run.guids)
                            {
                                Player* m = FindPlayerInWorld(g);
                                if (!m || m == tank || !m->IsAlive() || m->GetMapId() != run.mapId || m->IsBeingTeleported())
                                    continue;
                                if (PlayerbotAI::SendSummonRequest(tank, m))
                                {
                                    ++tp;
                                    continue;
                                }
                                if (m->IsInCombat() && tankAI)
                                {
                                    uint32 const mapId = run.mapId;
                                    float const x = tank->GetPositionX();
                                    float const y = tank->GetPositionY();
                                    float const z = tank->GetPositionZ();
                                    float const o = tank->GetOrientation();
                                    tankAI->RunOnOwningThread(m, [mapId, x, y, z, o](Player* p)
                                    {
                                        p->CombatStopWithPets(true);
                                        p->TeleportTo(mapId, x, y, z, o);
                                    });
                                    ++direct;
                                }
                            }
                            if (tp || direct)
                                sLog.outError("[REGROUP] run=%u g=%u 等超 60s ⇒ summon %u 人 / 战斗兜底拉 %u 人",
                                    run.runId, run.index, tp, direct);
                            if (tp || direct)
                            {
                                st->regroupSince = 0;
                                st->regroupLogAt = 0;
                            }
                        }
                    }
                    else if (st)
                    {
                        st->regroupLogAt = 0;
                        st->regroupSummonAt = 0;
                    }

                    // [2026-10-18] 已清波数**直接按本图遍历**统计（不再依赖坦克 BotState 的 seenAlive）：
                    //   波成员见过活怪（进入过 loaded 状态）→ 记 seenWaves；之后没活怪 → 记已清。
                    //   纯 NPC/小动物波不算分母。
                    uint16 cleared = 0;
                    uint16 realWaves = 0;
                    uint32 aliveMobs = 0;
                    if (route)
                    {
                        // [2026-10-22] 分母不能依赖 tank 是否存在/已加载：boss 优先模式下 tank 可能还在准备，
                        //   以前这里直接跳过 ⇒ status 出现 cleared=0/0（555 等）。先用任意队员的 map，实在没有就只数波数。
                        Map* tmap = tank ? tank->GetMap() : nullptr;
                        if (!tmap)
                        {
                            for (uint32 g : run.guids)
                            {
                                Player* p = FindPlayerInWorld(g);
                                if (p && p->FindMap())
                                {
                                    tmap = p->FindMap();
                                    break;
                                }
                            }
                        }

                        for (auto& kv : route->waves)
                        {
                            if (kv.second.allNonHostile)
                                continue;
                            ++realWaves;

                            bool anyAlive = false;
                            if (tmap)
                            {
                                for (auto& m : kv.second.members)
                                {
                                    // 用"活着"判据而不是"存在"：尸体/未加载都不算活怪，
                                    //   否则 dungeon 里尸体要等很久才消失，cleared 会滞后好几波。
                                    Creature* c = dungeonroute::MemberCreature(tmap, m);
                                    if (c && c->IsAlive())
                                    {
                                        anyAlive = true;
                                        ++aliveMobs;
                                    }
                                }
                            }
                            if (anyAlive)
                                run.seenWaves.insert(kv.first);
                            else if (tmap && run.seenWaves.count(kv.first))
                                ++cleared;
                        }
                        run.routeWaves = realWaves;
                        // 只认尸体：每 tick 刷新 boss 死亡状态，网格卸载不算死
                        if (tmap)
                            dungeonroute::RefreshBossKnowledge(tmap, *route, *st);

                        // [2026-10-19] 卡死判定不能只看"清整波"：水里的 1 只可打、其余不可达的波永远清不掉，
                        //   但每杀一只就是推进 ⇒ 用小怪存活数下降作为推进信号。
                        if (run.lastAliveMobs && aliveMobs < run.lastAliveMobs)
                            run.lastProgressAt = now;
                        run.lastAliveMobs = aliveMobs;
                    }

                    uint32 const dead = online - alive;
                    if (dead > run.lastDead)
                        {
                        uint32 const diedNow = dead > run.lastDead ? dead - run.lastDead : 0;
                        run.deaths += diedNow;
                        run.lastDead = dead;
                        // [2026-10-24 诊断] 死人时记录"谁在打我们"：附近敌对数量 + 进攻者 entry
                        if (diedNow && run.guids.size())
                        {
                            Player* tk = FindPlayerInWorld(run.guids[0]);
                            if (tk)
                            {
                                std::string entries;
                                uint32 n = 0;
                                for (Unit* a : tk->getAttackers())
                                {
                                    if (!a || !a->IsAlive())
                                        continue;
                                    if (n < 8)
                                    {
                                        if (!entries.empty()) entries += ",";
                                        entries += std::to_string(a->GetEntry());
                                    }
                                    ++n;
                                }
                                sLog.outError("[DEATHCTX] run=%u g=%u died=%u pos=(%.0f,%.0f) attackers=%u entries=%s",
                                    run.runId, run.index, diedNow, tk->GetPositionX(), tk->GetPositionY(),
                                    n, entries.c_str());
                            }
                        }
                    }

                    // [2026-10-24 站长口径] DPS/HPS 要按**战斗时间**算（不是墙钟）：先累计每个 bot 在战斗中的毫秒数
                    {
                        uint32 const nowC = WorldTimer::getMSTime();
                        if (run.combatAccAt && nowC > run.combatAccAt)
                        {
                            uint32 const d = nowC - run.combatAccAt;
                            for (uint32 guid : run.guids)
                            {
                                Player* p = FindPlayerInWorld(guid);
                                if (p && p->IsInCombat())
                                    run.combatMs[guid] += d;
                            }
                        }
                        run.combatAccAt = nowC ? nowC : 1;
                    }

                    // [2026-10-24 站长口径] DPS/HPS：**从每次进入战斗开始**计算，只统计战斗窗口。
                    //   进入战斗那一刻取基线（不计入历史累计），之后每 1 秒累计增量与 1 秒战斗时间；
                    //   脱战时把基线重置为本轮结束值。每 10 秒输出一次"Σ增量 / Σ战斗秒"。
                    {
                        uint32 const nowD = WorldTimer::getMSTime();
                        if (!run.dpsTickAt || WorldTimer::getMSTimeDiff(run.dpsTickAt, nowD ? nowD : 1) >= 1000)
                        {
                            run.dpsTickAt = nowD ? nowD : 1;
                            for (uint32 guid : run.guids)
                            {
                                Player* p = FindPlayerInWorld(guid);
                                if (!p)
                                    continue;
                                uint8 const cls = uint8(p->getClass());
                                uint64 const dmg = p->botDamageDealt;
                                uint64 const heal = p->botHealingDone;
                                bool const inC = p->IsInCombat();

                                auto itC = run.botWasInCombat.find(guid);
                                bool const wasInC = (itC != run.botWasInCombat.end()) && itC->second;

                                if (inC && !wasInC)
                                {
                                    // 进入战斗：取基线，不累计历史
                                    run.botDmgSeen[guid] = dmg;
                                    run.botHealSeen[guid] = heal;
                                }
                                if (inC)
                                {
                                    uint64 const prevD = run.botDmgSeen.count(guid) ? run.botDmgSeen[guid] : dmg;
                                    uint64 const prevH = run.botHealSeen.count(guid) ? run.botHealSeen[guid] : heal;
                                    if (dmg > prevD)
                                        run.classDmgAcc[cls] += dmg - prevD;
                                    if (heal > prevH)
                                        run.classHealAcc[cls] += heal - prevH;
                                    run.botDmgSeen[guid] = dmg;
                                    run.botHealSeen[guid] = heal;
                                    run.classCombatSec[cls] += 1.0;
                                }
                                else
                                {
                                    // 脱战：基线对齐当前值，避免下次进战斗把离线期间的变化算进来
                                    run.botDmgSeen[guid] = dmg;
                                    run.botHealSeen[guid] = heal;
                                }
                                run.botWasInCombat[guid] = inC;
                                // [2026-10-29 战斗取证] 总量/宠物/平砍/施法/技能：快照每 tick 更新，
                                //   只在"在战斗"时把增量计入本窗口。
                                {
                                    uint64 const allDmg = dmg + p->botPetDamageDealt;
                                    auto& sAll = run.combatSnapAllDmg[guid];
                                    auto& sPet = run.combatSnapPetDmg[guid];
                                    auto& sMel = run.combatSnapMelee[guid];
                                    auto& sCst = run.combatSnapCast[guid];
                                    auto& sSpell = run.combatSnapSpell[guid];
                                    uint64 const dAll = allDmg > sAll ? allDmg - sAll : 0;
                                    uint64 const dPet = p->botPetDamageDealt > sPet ? p->botPetDamageDealt - sPet : 0;
                                    uint64 const dMel = p->botMeleeHits > sMel ? p->botMeleeHits - sMel : 0;
                                    uint64 const dCst = p->botCasts > sCst ? p->botCasts - sCst : 0;
                                    if (inC)
                                    {
                                        run.winDmg += dAll;
                                        run.winPet += dPet;
                                        run.winMelee += dMel;
                                        run.winCasts += dCst;
                                        for (auto const& sc : p->botSpellCounts)
                                        {
                                            uint64 const prev = sSpell.count(sc.first) ? sSpell[sc.first] : 0;
                                            if (sc.second > prev)
                                                run.winSpell[sc.first] += sc.second - prev;
                                        }
                                    }
                                    sAll = allDmg;
                                    sPet = p->botPetDamageDealt;
                                    sMel = p->botMeleeHits;
                                    sCst = p->botCasts;
                                    sSpell = p->botSpellCounts;
                                }
                            }

                            if (!run.dpsLogAt)
                                run.dpsLogAt = nowD ? nowD : 1;
                            if (WorldTimer::getMSTimeDiff(run.dpsLogAt, nowD ? nowD : 1) >= 10000)
                            {
                                std::string sDps, sHps;
                                for (auto const& kv : run.classCombatSec)
                                {
                                    uint8 const cls = kv.first;
                                    double const cs = kv.second;
                                    if (cs < 0.5)
                                        continue;
                                    uint64 const d = run.classDmgAcc.count(cls) ? run.classDmgAcc[cls] : 0;
                                    uint64 const h = run.classHealAcc.count(cls) ? run.classHealAcc[cls] : 0;
                                    if (!sDps.empty()) sDps += " ";
                                    sDps += std::to_string(uint32(cls)) + ":" + std::to_string(uint32(double(d) / cs));
                                    if (!sHps.empty()) sHps += " ";
                                    sHps += std::to_string(uint32(cls)) + ":" + std::to_string(uint32(double(h) / cs));
                                }
                                if (!sDps.empty())
                                    sLog.outError("[DPS] run=%u g=%u %s", run.runId, run.index, sDps.c_str());
                                if (!sHps.empty())
                                    sLog.outError("[HEAL] run=%u g=%u %s", run.runId, run.index, sHps.c_str());
                                // [2026-10-29 战斗取证] 原始量：总伤害/宠物/平砍/施法/Top技能
                                {
                                    double csAll = 0.0;
                                    for (auto const& kv : run.classCombatSec)
                                        csAll += kv.second;
                                    std::string top;
                                    std::vector<std::pair<uint64, uint32>> topv;
                                    for (auto const& kv : run.winSpell)
                                        topv.push_back(std::make_pair(kv.second, kv.first));
                                    std::sort(topv.begin(), topv.end(), std::greater<std::pair<uint64, uint32>>());
                                    for (size_t i = 0; i < topv.size() && i < 5; ++i)
                                    {
                                        if (!top.empty()) top += ",";
                                        top += std::to_string(topv[i].second) + ":" + std::to_string(topv[i].first);
                                    }
                                    sLog.outError("[COMBAT] run=%u g=%u dmg=%llu pet=%llu melee=%llu casts=%llu combat_s=%.0f top=%s",
                                        run.runId, run.index,
                                        (unsigned long long)run.winDmg, (unsigned long long)run.winPet,
                                        (unsigned long long)run.winMelee, (unsigned long long)run.winCasts,
                                        csAll, top.c_str());
                                }
                                run.winDmg = run.winPet = run.winMelee = run.winCasts = 0;
                                run.winSpell.clear();
                                run.classCombatSec.clear();
                                run.classDmgAcc.clear();
                                run.classHealAcc.clear();
                                run.dpsLogAt = nowD ? nowD : 1;
                            }
                        }
                    }

                    // [2026-10-24 站长口径] 坦克 TPS：
                    //   窗口 1 秒；只要坦克处于**战斗状态**就计时间（哪怕这一秒没出手）。
                    //   TPS = Σ(坦克总仇恨的正增量) / Σ(坦克在战斗的秒数)；每 10 秒打一条。
                    {
                        uint32 const nowT = WorldTimer::getMSTime();
                        if (!run.tankHoldAt || WorldTimer::getMSTimeDiff(run.tankHoldAt, nowT ? nowT : 1) >= 1000)
                        {
                            run.tankHoldAt = nowT ? nowT : 1;

                            Player* tk = nullptr;
                            for (uint32 guid : run.guids)
                            {
                                Player* p = FindPlayerInWorld(guid);
                                if (p && PlayerbotAI::IsTank(p) && p->IsAlive())
                                {
                                    tk = p;
                                    break;
                                }
                            }
                            if (tk)
                            {
                                if (tk->IsInCombat())
                                    run.tankCombatSec += 1.0;

                                std::set<ObjectGuid> foes;
                                for (uint32 guid : run.guids)
                                {
                                    Player* p = FindPlayerInWorld(guid);
                                    if (!p || !p->IsInWorld())
                                        continue;
                                    for (Unit* a : p->getAttackers())
                                        if (a && a->IsAlive() && a->GetTypeId() == TYPEID_UNIT)
                                            foes.insert(a->GetObjectGuid());
                                }

                                double totalThreat = 0.0;
                                for (auto const& g : foes)
                                {
                                    Unit* u = tk->GetMap() ? tk->GetMap()->GetUnit(g) : nullptr;
                                    if (!u)
                                        continue;
                                    totalThreat += u->getThreatManager().getThreat(tk);
                                }
                                double const dth = totalThreat - run.lastTotalThreat;
                                if (dth > 0.0)
                                    run.threatAccum += dth;
                                run.lastTotalThreat = totalThreat;

                                if (!run.tpsLogAt)
                                    run.tpsLogAt = nowT ? nowT : 1;
                                if (WorldTimer::getMSTimeDiff(run.tpsLogAt, nowT ? nowT : 1) >= 10000 &&
                                    run.tankCombatSec > 0.5)
                                {
                                    double const tps = run.threatAccum / run.tankCombatSec;
                                    // [2026-10-26 补] 再带三个现场值，免得"TPS=0"又变成黑盒：
                                    //   tot=坦克当前在这批怪上的总仇恨 / vic=是否有目标 / d=与目标的距离
                                    float const vicD = tk->GetVictim() ? tk->GetDistance(tk->GetVictim()) : -1.0f;
                                    sLog.outError("[TANKTPS] run=%u g=%u tank=%s class=%u tps=%.0f gain=%.0f combat_s=%.0f tot=%.0f vic=%d d=%.1f",
                                        run.runId, run.index, tk->GetName(), uint32(tk->getClass()), tps,
                                        run.threatAccum, run.tankCombatSec, totalThreat,
                                        tk->GetVictim() ? 1 : 0, vicD);
                                    run.threatAccum = 0.0;
                                    run.tankCombatSec = 0.0;
                                    run.tpsLogAt = nowT ? nowT : 1;
                                }
                            }
                        }
                    }

                    // [2026-10-24] 离队过远救援：某个成员离"队伍中心"超过 500 码 ⇒ 拉回队伍中心。
                    if (!run.lostCheckAt || now - run.lostCheckAt >= 5000)
                    {
                        run.lostCheckAt = now;
                        std::vector<Player*> inside;
                        float sx = 0, sy = 0, sz = 0;
                        uint32 n = 0;
                        for (uint32 guid : run.guids)
                        {
                            Player* p = FindPlayerInWorld(guid);
                            if (!p || !p->IsInWorld() || p->GetMapId() != run.mapId)
                                continue;
                            inside.push_back(p);
                            sx += p->GetPositionX(); sy += p->GetPositionY(); sz += p->GetPositionZ();
                            ++n;
                        }
                        if (n >= 2)
                        {
                            float const mx = sx / n, my = sy / n, mz = sz / n;
                            for (Player* p : inside)
                            {
                                if (!p || p->IsBeingTeleported())
                                    continue;
                                if (p->GetDistance(mx, my, mz) <= 500.0f)
                                    continue;
                                sLog.outError("[SIM] run=%u g=%u 成员 %s 离队过远（%.0f 码）⇒ 拉回队伍中心",
                                    run.runId, run.index, p->GetName(), p->GetDistance(mx, my, mz));
                                p->CombatStopWithPets(true);
                                p->TeleportTo(run.mapId, mx, my, mz, p->GetOrientation());
                            }
                        }
                    }

                    // [2026-10-24] 死亡循环救援：一组在 60 秒内又死了 >=10 次 ⇒ 整组传回副本入口，
                    //   否则会出现"就地复活→立刻再死"的死循环（R21 543：deaths 848 / 151 次灭团）。
                    {
                        uint32 const nowMs2 = now ? now : 1;
                        if (run.lastRescueDeaths == 0 || nowMs2 - run.lastRescueAt > 60000)
                        {
                            if (run.lastRescueDeaths == 0)
                                run.lastRescueAt = nowMs2;
                            if (run.deaths - run.lastRescueDeaths >= 10 && nowMs2 - run.lastRescueAt <= 60000)
                            {
                                uint32 const delta = run.deaths - run.lastRescueDeaths;
                                run.lastRescueDeaths = run.deaths;
                                run.lastRescueAt = nowMs2;
                                bool rescued = false;
                                // [2026-10-28 站长口径] 死亡循环救援优先撤到"上一波已清"的位置，
                                //   回入口太远、原地复活会继续 add；没有已清波才回入口。
                                float rx = run.ex, ry = run.ey, rz = run.ez;
                                bool const safe = PreviousStageRevivePoint(run, rx, ry, rz);
                                for (uint32 guid : run.guids)
                                {
                                    Player* p = FindPlayerInWorld(guid);
                                    if (!p || p->IsBeingTeleported())
                                        continue;
                                    p->CombatStopWithPets(true);
                                    if (!p->IsAlive())
                                    {
                                        p->ResurrectPlayer(1.0f);
                                        p->SpawnCorpseBones();
                                        p->SetHealthPercent(100.0f);
                                    }
                                    p->TeleportTo(run.mapId, rx, ry, rz, run.eo);
                                    rescued = true;
                                }
                                if (rescued)
                                    sLog.outError("[SIM] run=%u g=%u 60 秒内死亡 %u 次 ⇒ 整组撤回%s (%.0f,%.0f)（解除复活死循环）",
                                        run.runId, run.index, delta, safe ? "上一个stage起点" : "副本入口", rx, ry);
                            }
                        }
                    }

                    // [2026-10-24 站长指令] 不再"就地复活"：阵亡/灭团一律**回副本入口（安全位置）复活**。
                    //   就地复活会出现"复活在怪堆里→立刻再死"的死循环（552/543 实测）。
                    if (dead > 0)
                    {
                        if (!run.soloReviveAt)
                            run.soloReviveAt = now;
                        else if (now - run.soloReviveAt >= 8000)
                        {
                            uint32 revived = 0;
                            std::string classList;   // [2026-10-24] 死亡职业统计（职业战斗策略排查）
                            for (uint32 guid : run.guids)
                            {
                                Player* p = FindPlayerInWorld(guid);
                                if (!p || p->IsAlive())
                                    continue;

                                p->ResurrectPlayer(1.0f);
                                p->SpawnCorpseBones();
                                p->SetHealthPercent(100.0f);
                                if (p->GetMaxPower(POWER_MANA))
                                    p->SetPower(POWER_MANA, p->GetMaxPower(POWER_MANA));
                                p->CombatStopWithPets(true);

                                if (PlayerbotAI* pAI = p->GetPlayerbotAI())
                                    pAI->ResetStrategies();

                                // [2026-10-26 修] 复活点：整队活着就回**队伍身边**（存活队友脚下 2 码内），
                                //   否则回副本入口。入口复活后 bot 要独自穿过整条路线走回来，
                                //   实测同一只 bot 45 分钟死 30 次 —— 死亡里一大半是"单人跑尸被沿途怪杀"。
                                // [2026-10-28 站长口径] 单人阵亡也回**当前 stage 起点**（上个 stage 结束的位置）；
                                //   还没有记录到任何 stage（刚进本）就回**副本门口**，不退回队友身边。
                                float rx = run.ex, ry = run.ey, rz = run.ez, ro = run.eo;
                                bool const atStage = PreviousStageRevivePoint(run, rx, ry, rz);
                                (void)atStage;
                                p->TeleportTo(run.mapId, rx, ry, rz, ro);
                                if (!classList.empty()) classList += ",";
                                classList += std::to_string(uint32(p->getClass()));
                                ++revived;
                            }

                            if (revived)
                                sLog.outError("[SIM] run=%u g=%u %s %u 人 → 复活（%s）deaths=%u classes=%s",
                                    run.runId, run.index, revived >= run.partySize ? "灭团" : "阵亡",
                                    revived, "stage起点/门口", run.deaths, classList.c_str());
                            run.soloReviveAt = 0;
                        }
                    }
                    else
                    {
                        run.soloReviveAt = 0;
                    }

                    // 只有"清掉一波"才算进展：单纯在战斗里磨不算（否则永远判不出卡死）。
                    if (cleared != run.lastCleared)
                    {
                        sLog.outError("[SIM] run=%u g=%u progress cleared=%u→%u/%u skipped=%u",
                            run.runId, run.index, (uint32)run.lastCleared, (uint32)cleared,
                            (uint32)run.routeWaves, (uint32)skipped);
                        run.lastProgressAt = now;
                    }
                    run.lastCleared = cleared;
                    if (cleared > run.attemptBest)
                        run.attemptBest = cleared;
                    if (cleared > run.bestCleared)
                        run.bestCleared = cleared;

                    // [2026-10-18 站长口径] "有目标但走不过去 / 脱战残留"守卫（保守版）：
                    //   只认**坦克当前目标**：它 15 秒血量没变化（打不到 / 不可达 / 脱战残留）⇒ 跳过该目标所在的波；
                    //   一次战斗段最多跳一波（脱战后复位）。
                    //   不再用"整队零伤害"做判据 —— 接近/拉怪途中会误杀虫群（实测连锁跳过 17 波）。
                    // [2026-10-21 站长口径] 只要**队伍里有人在战斗**就进入这个守卫：
                    //   坦克有目标 → 追/打；坦克没目标 → 去帮队友打（不跳波、不开下一波）。
                    // [2026-10-24 站长口径] 坦克漏怪（怪选了队友）必须补 —— 不受"跳波标记"影响。
                    //   实测 540 g=2：坦克 victim=- 却被 5 只怪围着打，就是因为 combatSkipDone 把整块逻辑跳过了。
                    if (tank && inCombat && route && !run.combatSkipDone)
                    {
                        Unit* victim = tank->GetVictim();
                        if (victim && victim->GetTypeId() == TYPEID_UNIT && victim->GetMapId() == run.mapId)
                        {
                            uint32 const hp = victim->GetHealth();
                            if (victim->GetObjectGuid() != run.lastVictim || hp != run.lastVictimHp)
                            {
                                run.lastVictim = victim->GetObjectGuid();
                                run.lastVictimHp = hp;
                                run.lastVictimAt = now;
                                run.combatCloseInSince = 0;
                                run.chaseBestAt = 0;
                                run.chaseBestDist = 1e9f;
                                run.chaseLogAt = 0;
                                run.meleeSince = 0;
                            }
                            else if (run.lastVictimAt && now - run.lastVictimAt >= 15000)
                            {
                                // [2026-10-21 站长口径]
                                //   ① 坦克开怪后**原地等怪走过来**，不冲上去；
                                //   ② 只要还有怪在和队伍战斗，就**不跳波**（继续等/打）；
                                //   只有"没有任何怪在和队伍战斗"时才允许放弃这个打不到的波。
                                bool anyFighting = false;
                                for (uint32 g : run.guids)
                                {
                                    Player* p = FindPlayerInWorld(g);
                                    if (!p)
                                        continue;
                                    for (Unit* a : p->getAttackers())
                                    {
                                        if (a && a->IsAlive() && a->IsInCombat())
                                        {
                                            anyFighting = true;
                                            break;
                                        }
                                    }
                                    if (!anyFighting && p->GetVictim() && p->GetVictim()->IsAlive())
                                        anyFighting = true;
                                    if (anyFighting)
                                        break;
                                }

                                if (anyFighting)
                                {
                                    if (!run.chaseLogAt || now - run.chaseLogAt > 5000)
                                    {
                                        run.chaseLogAt = now;
                                        sLog.outError("[SIM] run=%u g=%u attempt=%u 15s 无伤害（目标 %s，距离 %.1f）⇒ 原地等待怪过来、不跳波",
                                            run.runId, run.index, run.attempt, victim->GetName(), tank->GetDistance(victim));
                                    }
                                    run.lastVictimAt = now;
                                    run.lastVictim = ObjectGuid();
                                    return true;
                                }

                                dungeonroute::Target t;
                                uint16 const ord = (dungeonroute::FindTargetNearest(tank, *route, t) && t.ord)
                                    ? t.ord : dungeonroute::CurrentOrd(tank, *route);
                                if (ord && tank->GetPlayerbotAI())
                                {
                                    Creature* vc = (victim->GetTypeId() == TYPEID_UNIT) ? (Creature*)victim : nullptr;
                                    bool const evading = vc && vc->GetMotionMaster()
                                        && vc->GetMotionMaster()->GetCurrentMovementGeneratorType() == HOME_MOTION_TYPE;
                                    sLog.outError("[SIM] run=%u g=%u attempt=%u 无伤害诊断: victim=%s entry=%u hp=%u inCombat=%d evade=%d dist=%.1f",
                                        run.runId, run.index, run.attempt, victim->GetName(), victim->GetEntry(),
                                        (uint32)victim->GetHealth(), victim->IsInCombat() ? 1 : 0,
                                        evading ? 1 : 0, tank->GetDistance(victim));
                                    dungeonroute::MarkSkipped(tank, ord, "no damage 15s no combat");
                                    run.combatSkipDone = true;
                                    ForceDisengage(run);
                                    sLog.outError("[SIM] run=%u g=%u attempt=%u 目标 %s 15s 无伤害且已无战斗 ⇒ 跳过波 ord=%u",
                                        run.runId, run.index, run.attempt, victim->GetName(), (uint32)ord);
                                }
                                run.lastVictimAt = now;
                                run.lastVictim = ObjectGuid();
                            }
                        }
                        else
                        {
                            // [2026-10-21 站长口径] 队伍一部分在战斗时，其余人要去帮，**不要自己开下一波**；
                            //   坦克没目标时：① 优先打队友正在打的目标；② 否则 attack anything 重新锁定。
                            //   [2026-10-23 修] 以前**动作**也被 2 秒节流 ⇒ 坦克丢目标后要等 2 秒才补，
                            //   战斗里等于每两秒空一次手。现在：**每 tick 都补动作**，只把日志节流到 5 秒。
                            if (!run.noVictimSince)
                                run.noVictimSince = now;

                            // [2026-10-24 站长口径·纠正] "坦克帮队友打"= 有**怪选择了队友**（怪的 victim 是队友）
                            //   也就是坦克漏怪；不是去帮队友打"队友自己选的目标"。
                            // [2026-10-24 站长口径·纠正] "坦克帮队友打" = **有怪选择了队友**（怪的 victim 是队友）
                            //   ⇒ 坦克漏怪，去把它拉回来。不是去帮队友打"队友自己挑的目标"。
                            Unit* assist = nullptr;
                            {
                                std::vector<Creature*> loose;
                                for (uint32 g : run.guids)
                                {
                                    Player* p = FindPlayerInWorld(g);
                                    if (!p || p == tank || !p->IsInWorld())
                                        continue;
                                    for (Unit* a : p->getAttackers())
                                    {
                                        if (!a || !a->IsAlive() || a->GetMapId() != run.mapId)
                                            continue;
                                        if (a->GetTypeId() != TYPEID_UNIT)
                                            continue;
                                        if (std::find(loose.begin(), loose.end(), (Creature*)a) == loose.end())
                                            loose.push_back((Creature*)a);
                                    }
                                }
                                for (Creature* vc : loose)
                                {
                                    uint32 const vguid = vc->GetGUIDLow();
                                    bool const isPatrol = route->patrolGuids.count(vguid) > 0;
                                    bool isBoss = false;
                                    for (auto const& b : route->bosses)
                                        if (b.guid == vguid) { isBoss = true; break; }
                                    bool const isRouteMob = route->guidOrd.count(vguid) > 0;

                                    uint8 mtype = 0;
                                    if (vc->GetMotionMaster())
                                        mtype = vc->GetMotionMaster()->GetCurrentMovementGeneratorType();
                                    bool const wandering = (mtype == WAYPOINT_MOTION_TYPE || mtype == RANDOM_MOTION_TYPE);

                                    if (isPatrol || (!isRouteMob && !isBoss && wandering))
                                        continue;                     // 巡逻/游荡的野怪：不主动去追
                                    if (tank->GetDistance(vc) > dw::Get("sim.assist.max_dist"))
                                        continue;                     // 太远（多半已跑偏）
                                    if (!assist || tank->GetDistance(vc) < tank->GetDistance(assist))
                                        assist = vc;
                                }

                                // [2026-10-24] 兜底：坦克自己被怪打却没有目标 ⇒ 必须还手（否则"坦克站着挨打"）
                                if (!assist)
                                {
                                    for (Unit* a : tank->getAttackers())
                                    {
                                        if (!a || !a->IsAlive() || a->GetTypeId() != TYPEID_UNIT)
                                            continue;
                                        if (!assist || tank->GetDistance(a) < tank->GetDistance(assist))
                                            assist = a;
                                    }
                                }
                            }

                            // 动作：坦克没目标时补，但**不是每 tick**（每 tick 调 Attack 会和 AI 自己的选敌/走位打架），
                            //   500ms 一次足够：丢目标后最多半秒空手，又不会把 AttackStart 刷爆。
                            if (assist)
                            {
                                uint32 const nowMs2 = now ? now : 1;
                                if (!run.tankAssistAt || WorldTimer::getMSTimeDiff(run.tankAssistAt, nowMs2) >= 500)
                                {
                                    run.tankAssistAt = nowMs2;
                                    tank->Attack(assist, true);
                                }
                                // [2026-10-24] 直接 Attack 有时会被 AI 自己的动作覆盖 ⇒ 同时排队让它自己重新选敌
                                if (tank->GetVictim() != assist && tank->GetPlayerbotAI() &&
                                    (!run.combatAssistAt || now - run.combatAssistAt > 2000))
                                {
                                    run.combatAssistAt = now;
                                    tank->GetPlayerbotAI()->QueueSimAction("attack anything");
                                }
                                if (!run.tankAssistLogAt || now - run.tankAssistLogAt >= 5000)
                                {
                                    run.tankAssistLogAt = now;
                                    sLog.outError("[SIM] run=%u g=%u attempt=%u 坦克无目标 ⇒ 去帮队友打 %s(entry=%u)",
                                        run.runId, run.index, run.attempt, assist->GetName(), assist->GetEntry());
                                }
                            }
                            else if (tank->GetPlayerbotAI() &&
                                     (!run.combatAssistAt || now - run.combatAssistAt > 2000))
                            {
                                // [2026-10-22 崩溃修] World 线程不能直接调 AI：排队到坦克自己的 map 线程
                                run.combatAssistAt = now;
                                tank->GetPlayerbotAI()->QueueSimAction("attack anything");
                            }

                            run.lastVictim = ObjectGuid();
                            run.lastVictimAt = 0;
                        }
                    }
                    else if (!inCombat)
                    {
                        run.combatSkipDone = false;
                        run.combatCloseInSince = 0;
                        run.chaseBestAt = 0;
                        run.chaseBestDist = 1e9f;
                        run.chaseLogAt = 0;
                        run.meleeSince = 0;
                        run.combatAssistAt = 0;
                        run.noVictimSince = 0;
                        run.lastVictim = ObjectGuid();
                        run.lastVictimAt = 0;
                    }

                    // ① 通关：只认**所有 boss 确认死亡**，不要求所有波清完
                    bool clearedAll = false;
                    if (route && st && tank && !route->bosses.empty())
                    {
                        uint32 bossesCleared = 0;
                        for (auto const& b : route->bosses)
                            if (st->bossCleared.count(b.guid))
                                ++bossesCleared;
                        clearedAll = (bossesCleared == (uint32)route->bosses.size());
                    }
                    if (clearedAll)
                    {
                        RecordAttempt(run, "cleared");
                        CloseRun(run, true, "cleared");
                        return false;
                    }

                    // ② 灭团：全员死亡持续 WIPE_CONFIRM_MS
                    if (alive == 0)
                    {
                        if (!run.wipeSince)
                            run.wipeSince = now;
                        else if (now - run.wipeSince >= WIPE_CONFIRM_MS)
                        {
                            if (run.policy == 1)
                            {
                                // continue：死即复活。优先撤回"上一波已清"的拉怪位（不在怪堆里复活），
                                //   没有已清波才回副本入口；原地复活会被 add 立刻再打死（546 g=1 死循环）。
                                uint32 const revived = ReviveParty(run, false);
                                float rx = run.ex, ry = run.ey, rz = run.ez;
                                bool const safe = PreviousStageRevivePoint(run, rx, ry, rz);
                                uint32 const tp = TeleportParty(run, rx, ry, rz);
                                sLog.outError("[SIM] run=%u g=%u attempt=%u WIPE→continue 复活 %u 人（deaths=%u，撤回%s (%.0f,%.0f) %u 人）",
                                    run.runId, run.index, run.attempt, revived, run.deaths,
                                    safe ? "上一个stage起点" : "副本入口", rx, ry, tp);
                                run.wipeSince = 0;
                                run.lastDead = 0;
                                run.lastProgressAt = now;
                            }
                            else
                            {
                                // reset：记一次 attempt，重置后重跑
                                RecordAttempt(run, "wipe");
                                if (run.maxAttempts && run.attempt >= run.maxAttempts)
                                {
                                    CloseRun(run, true, "attempts done");
                                    return false;
                                }
                                ++run.attempt;
                                run.wipeSince = 0;
                                run.phase = PHASE_RESET;
                                run.phaseSince = now;
                                run.nextResetTryAt = now;
                                sLog.outError("[SIM] run=%u g=%u WIPE → reset：下一轮 attempt=%u/%u seed=%u",
                                    run.runId, run.index, run.attempt, run.maxAttempts, run.baseSeed);
                                return true;
                            }
                        }
                    }
                    else
                        run.wipeSince = 0;

                    // ③ 超时：本轮进本后跑满 maxRunMs（默认 25 分钟，可用 maxtime=<秒> 覆盖）就收尾。
                    //    policy=continue 是"死即复活"，没有这一步的话「灭团→复活→再灭」会无限循环，
                    //    整轮测试永远停不下来（2026-10-21 站长口径：必须有一个时间限制）。
                    if (run.attemptStartedAt && now - run.attemptStartedAt > run.maxRunMs)
                    {
                        RecordAttempt(run, "timeout");
                        CloseRun(run, true, "超时（跑满 " + std::to_string(run.maxRunMs / 1000) + " 秒，best="
                            + std::to_string(run.bestCleared) + "/" + std::to_string(run.routeWaves) + "）");
                        return false;
                    }

                    // ④ 卡死（**只看"多久没有波次推进"**）
                    //   ⚠️ 这里原来还有一条 `now - attemptStartedAt > ATTEMPT_TIMEOUT_MS`（写死 45 分钟），
                    //   把 `maxtime=` 完全盖掉 ⇒ 所有长窗口实验其实都在 45 分钟被强制 error=stuck 收尾。
                    //   时间上限统一由上面的 ③（run.maxRunMs）负责，这里不再重复判时间。
                    bool const stuck = (now - run.lastProgressAt > run.stuckMs);
                    if (stuck)
                    {
                        RecordAttempt(run, "stuck");
                        if (run.policy == 1 || (run.maxAttempts && run.attempt >= run.maxAttempts))
                        {
                            CloseRun(run, true, "stuck");
                            return false;
                        }
                        ++run.attempt;
                        run.wipeSince = 0;
                        run.phase = PHASE_RESET;
                        run.phaseSince = now;
                        run.nextResetTryAt = now;
                        sLog.outError("[SIM] run=%u g=%u STUCK → reset：下一轮 attempt=%u/%u",
                            run.runId, run.index, run.attempt, run.maxAttempts);
                        return true;
                    }

                    if (now - run.lastStatusAt > 20000)
                    {
                        run.lastStatusAt = now;

                        // 驱动坦克现场：位置/动作/目标血量/距下个目标（卡住时看这一行就够；driver 上面已算）
                        PlayerbotAI* driverAI = driver ? driver->GetPlayerbotAI() : nullptr;
                        std::string driverInfo = "-";
                        if (driver)
                        {
                            Unit* victim = driver->GetVictim();
                            dungeonroute::Target t;
                            float dist = -1.0f;
                            if (route && dungeonroute::FindTargetNearest(driver, *route, t) && t.hasPullPoint)
                                dist = driver->GetDistance(t.px, t.py, t.pz);

                            // [2026-10-23 站长口径] 状态行里带"为什么不走"的原因码（不额外写日志）：
                            //   drive=是否具备驱动资格；ok=FindTargetNearest 是否给出目标；
                            //   curOrd=当前锁定波；defer=挂起的 boss 数；pw=是否在等巡逻；skip=已跳过波数
                            dungeonroute::BotState& dsi = dungeonroute::StateOf(driver);
                            bool const driveOk = driverAI ? dungeonroute::ShouldDrive(driverAI) : false;
                            dungeonroute::Target dt2;
                            bool const tgtOk = route && dungeonroute::FindTargetNearest(driver, *route, dt2);
                            uint32 const curOrd = route ? dungeonroute::CurrentOrd(driver, *route) : 0;

                            char buf[420];
                            snprintf(buf, sizeof(buf),
                                "%s@(%.0f,%.0f) act=%s tgt=%s:%d%% dist=%.0f drive=%d ok=%d ord=%u defer=%u pw=%u skip=%u clr=%u",
                                driver->GetName(), driver->GetPositionX(), driver->GetPositionY(),
                                driverAI ? driverAI->GetLastExecutedActionName(driverAI->GetState()).c_str() : "-",
                                victim ? victim->GetName() : "-",
                                (victim && victim->GetMaxHealth())
                                    ? (int)(100.0f * float(victim->GetHealth()) / float(victim->GetMaxHealth())) : 0,
                                dist, driveOk ? 1 : 0, tgtOk ? 1 : 0, (uint32)curOrd,
                                (uint32)dsi.bossDeferUntil.size(), (uint32)(dsi.patrolWaitGuid ? 1 : 0),
                                (uint32)dsi.skip.size(), (uint32)dsi.cleared.size());
                            driverInfo = buf;
                        }

                        sLog.outError("[SIM] run=%u g=%u status t=%.0fs attempt=%u/%u seed=%u alive=%u/%u deaths=%u combat=%u cleared=%u/%u skipped=%u seen=%u mobs=%u tank=%s inst=%u driver=(%s)",
                            run.runId, run.index, (now - run.startedAt) / 1000.0f,
                            run.attempt, run.maxAttempts, run.baseSeed,
                            alive, online, run.deaths, inCombat,
                            (uint32)cleared, (uint32)run.routeWaves, (uint32)skipped,
                            (uint32)run.seenWaves.size(), (uint32)aliveMobs,
                            tank ? tank->GetName() : "-",
                            leader ? leader->GetInstanceId() : 0, driverInfo.c_str());

                        // 拉不动怪时看 diag：驱动坦克/route/只拉一次（进本时已打过一次）
                        if (inCombat == 0 && run.diagLeft)
                        {
                            --run.diagLeft;
                            LogDiagnostics(run);
                        }
                    }
                    return true;
                }

                case PHASE_RESET:
                {
                    // ① 解散队伍（原来那队的 instance 也随之作废）
                    if (run.group)
                    {
                        Group* group = run.group;
                        run.group = nullptr;
                        group->Disband(true);
                        sObjectMgr.RemoveGroup(group);
                        delete group;
                    }

                    // ② 复活 + 传送回主城（离开副本图；同图近传不会换 instance，所以必须先出去）
                    bool allOut = true;
                    bool const requestTp = (now >= run.nextResetTryAt);
                    for (uint32 guid : run.guids)
                    {
                        Player* p = FindPlayerInWorld(guid);
                        if (!p)
                        {
                            // [2026-10-18] 没登录的 bot（账号缺失/已登出）视为"不在本里"，不要卡住重置
                            continue;
                        }

                        if (!p->IsAlive())
                        {
                            p->ResurrectPlayer(1.0f);
                            p->SpawnCorpseBones();
                        }

                        if (p->GetMapId() == run.mapId || p->IsBeingTeleported())
                        {
                            allOut = false;
                            if (requestTp && !p->IsBeingTeleported())
                                p->TeleportToHomebind();
                        }
                    }
                    if (requestTp)
                        run.nextResetTryAt = now ? (now + 3000) : 1;

                    if (now - run.phaseSince > 60000)
                    {
                        CloseRun(run, false, "重置超时（还在副本里）");
                        return false;
                    }

                    if (!allOut || now - run.phaseSince < 2000)
                        return true;

                    // ③ 下线旧 attempt 的 bot，重新选阵容（attempt 变 ⇒ 种子变）
                    for (uint32 guid : run.guids)
                    {
                        if (FindPlayerAny(guid))
                            sRandomPlayerbotMgr.LogoutPlayerBot(guid);
                    }

                    std::vector<uint32> excluded;
                    for (auto const& other : s_runs)
                    {
                        if (&other == &run)
                            continue;
                        for (uint32 g : other.guids)
                            excluded.push_back(g);
                    }

                    std::vector<Candidate> pool = LoadCandidates(run.minLevel, excluded);
                    std::vector<uint32> party;
                    std::string error;
                    std::set<uint32> used;
                    uint32 const pickSeed = run.baseSeed + run.index * 7919u
                        + (run.attempt + run.roleRetries) * 104729u;
                    if (pool.empty() || !PickParty(pool, run.partySize + 3, used, party, error, pickSeed))
                    {
                        CloseRun(run, true, "attempts done（无法组下一轮: " + error + "）");
                        return false;
                    }

                    run.guids = party;
                    run.nextLoginTryAt.assign(party.size(), 0);
                    run.leaderTeleported = false;
                    run.membersTeleported = false;
                    run.memberIdx = 1;
                    run.memberTryAt = 0;
                    run.leaderTryAt = 0;
                    run.nextRepairAt = 0;
                    run.instanceId = 0;
                    run.lastCleared = 0;
                    run.diagLeft = 2;
                    run.phase = PHASE_LOGIN;
                    run.phaseSince = now;

                    std::string names;
                    FormatRunGuids(run, names);
                    sLog.outError("[SIM] run=%u g=%u reset done → attempt=%u/%u seed=%u names=%s",
                        run.runId, run.index, run.attempt, run.maxAttempts, pickSeed, names.c_str());
                    return true;
                }
            }

            return true;
        }

        void CloseAll(std::string const& reason, std::list<std::string>* messages)
        {
            for (auto& run : s_runs)
            {
                CloseRun(run, false, reason);
                if (messages)
                    messages->push_back("run=" + std::to_string(run.runId) + " g=" + std::to_string(run.index) + " 已停止");
            }
            s_runs.clear();
            RestoreRouteSwitchIfIdle();
        }

        std::string PhaseName(Phase p)
        {
            switch (p)
            {
                case PHASE_LOGIN:    return "login";
                case PHASE_FORM:     return "form";
                case PHASE_TELEPORT: return "teleport";
                case PHASE_MOVEIN:   return "movein";
                case PHASE_RUN:      return "run";
                case PHASE_RESET:    return "reset";
            }
            return "?";
        }

        // ---- 训练假人场景 ----------------------------------------------------
        // 把 bot 摆到测试点并强开专精策略（登录后调用一次）
        void ApplyDummySetup(DummyRun& d, Player* bot)
        {
            d.setupDone = true;

            // [2026-10-29 站长口径] "毕业装 bot 也只有 500 dps" 的根因之一：随机 bot 的天赋树是随机的、
            //   装备也是旧的（StaticBots=1 不会自己换）。这里按目标专精**重天赋 + 换最好装备**，
            //   否则测的既不是毕业装、也不是这个专精的循环上限。
            static std::set<std::string> s_prepared;   // "<guid>:<premade>" 已经准备过的组合
            std::string const prepKey = std::to_string(d.guid) + ":" + d.premade;
            if (d.respec && !d.respecced && !s_prepared.count(prepKey))
            {
                d.respecced = true;
                s_prepared.insert(prepKey);
                if (bot->GetLevel() >= 70)
                {
                    // [站长口径 2026-10-29] 天赋是**按专精固定一套**（AiPlayerbot.PremadeSpecName/Link），
                    //   不能用 PlayerbotFactory::Randomize 的随机填点 —— 那会把现成的 build 换成随机点，
                    //   反而把 DPS 上限砸下来。这里按名字找 premade 那一套，直接 ApplyTalents。
                    ClassSpecs& specs = sPlayerbotAIConfig.classSpecs[d.cls];
                    bool applied = false;
                    for (auto& path : specs.talentPath)
                    {
                        if (path.name != d.premade || path.talentSpec.empty())
                            continue;
                        TalentSpec& ts = path.talentSpec.back();     // 最高等级那一档
                        std::ostringstream out;
                        bot->resetTalents(true);
                        ts.ApplyTalents(bot, &out);
                        applied = true;
                        sLog.outError("[DUMMY] id=%u spec=%s premade=[%s] 天赋已套用", d.id, d.spec.c_str(), d.premade.c_str());
                        break;
                    }
                    if (!applied)
                        sLog.outError("[DUMMY] id=%u spec=%s 没找到 premade 天赋 [%s]，保留原天赋",
                            d.id, d.spec.c_str(), d.premade.c_str());

                    // 装备：`EquipGearBest` 的品质是按等级段 urand 出来的 ⇒ 会摇到很差的组合
                    //   （实测火法 sp 只有 669、野性德 ap 3360 但没智力）。这里给一个**属性下限**，
                    //   不达标就重摇几次（最多 5 次），把"毕业装"这个前提做扎实。
                    bool const caster = (d.cls == CLASS_MAGE || d.cls == CLASS_WARLOCK
                        || d.cls == CLASS_PRIEST
                        || (d.cls == CLASS_DRUID && d.spec == "balance")
                        || (d.cls == CLASS_SHAMAN && d.spec == "elemental")
                        || (d.cls == CLASS_PALADIN && d.spec == "holy"));
                    // [2026-10-29 站长口径] 装备要"从数据库里挑 **GS 最高、且满足命中要求** 的那套"。
                    //   做法：`EquipGearBest()` 本来就是从 item_template 按专精挑装（唯一随机的是品质），
                    //   所以把它当**候选生成器**：反复重摇，只在"命中达到该专精上限"的候选里保留 GS 最高的那套；
                    //   并记住该专精的历史最好档位，让后续同专精 bot 一起收敛到同一档。
                    static std::map<std::string, double> s_bestGearScore;
                    std::string const gearKey = std::to_string(d.cls) + ":" + d.premade;

                    // 命中上限（TBC 对 boss，rating）：法术 202、近战/远程特殊攻击 142
                    uint32 const hitRating = caster
                        ? bot->GetUInt32Value(PLAYER_FIELD_COMBAT_RATING_1 + CR_HIT_SPELL)
                        : bot->GetUInt32Value(PLAYER_FIELD_COMBAT_RATING_1 + CR_HIT_MELEE);
                    uint32 const hitTarget = caster ? 202 : 142;

                    // [2026-10-29 站长口径] **静态 BIS 表**：优先按 `ai_playerbot_bis_gear`
                    //   （从 item_template 静态筛出来的每专精 BIS：命中优先、其次物品等级）给 bot 穿同一套。
                    //   表不存在或该专精没数据时，回退到 EquipGearBest() 的重摇逻辑。
                    uint32 bisApplied = 0;
                    // [2026-10-29 实测] 17 个 bot 在同一 tick 里"清空+穿 16 件"会出现穿不全
                    //   （实测 sp 2600 → 1216）。站长口径：**每 tick 穿一件**，做成流水；
                    //   这里只清空槽位并建好待穿清单，实际穿戴放到 TickDummies 的脉冲里逐件做。
                    bool const useBis = true;
                    if (useBis)
                    {
                        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
                            if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                                bot->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);

                        if (auto bres = WorldDatabase.PQuery(
                                "SELECT slot, item_id FROM ai_playerbot_bis_gear WHERE cls=%u AND spec='%s' ORDER BY slot",
                                (uint32)d.cls, d.spec.c_str()))
                        {
                            do
                            {
                                Field* f = bres->Fetch();
                                d.bisPending.push_back(std::make_pair(f[0].GetUInt8(), f[1].GetUInt32()));
                            } while (bres->NextRow());
                        }
                        d.gearReady = d.bisPending.empty();
                        sLog.outError("[DUMMY] id=%u spec=%s BIS 流水：待穿 %u 件",
                            d.id, d.spec.c_str(), (uint32)d.bisPending.size());
                    }

                    if (bisApplied)
                    {
                        // [2026-10-29 实测] 换完武器必须**练满武器技能**：随机 bot 的武器技能是跟着
                        //   原来那套装备练的，一旦换成没练过的类型（匕首/拳套/长柄…）技能就是 0，
                        //   结果 ic=1 但白字几乎全 miss（melee=0），dps 掉到几十。
                        static uint16 const weaponSkills[] = {
                            SKILL_AXES, SKILL_2H_AXES, SKILL_BOWS, SKILL_CROSSBOWS, SKILL_DAGGERS,
                            SKILL_FIST_WEAPONS, SKILL_GUNS, SKILL_MACES, SKILL_2H_MACES, SKILL_POLEARMS,
                            SKILL_STAVES, SKILL_SWORDS, SKILL_2H_SWORDS, SKILL_THROWN, SKILL_WANDS };
                        for (uint16 sk : weaponSkills)
                            bot->SetSkill(sk, bot->GetLevel() * 5, bot->GetLevel() * 5);   // SetSkill(id, value, max)：别把 value 写成 1（=技能 1 级，全 miss）

                        // [2026-10-29 实测] 换装后**必须重算武器伤害**：否则 m_weaponDamage 还是 0，
                        //   技能与白字都打 0（实测 dmg=0、melee=0，而 wtimer/inreach/delay 全正常）。
                        bot->UpdateDamagePhysical(BASE_ATTACK);
                        bot->UpdateDamagePhysical(OFF_ATTACK);
                        bot->UpdateAllStats();

                        int const sp = int(bot->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_FIRE));
                        int const ap = int(bot->GetTotalAttackPowerValue(BASE_ATTACK));
                        sLog.outError("[DUMMY] id=%u spec=%s 按静态 BIS 表换装 %u 件：sp=%d ap=%d GS=%u",
                            d.id, d.spec.c_str(), bisApplied, sp, ap,
                            bot->GetPlayerbotAI() ? bot->GetPlayerbotAI()->GetEquipGearScore(bot, false, false) : 0);
                    }

                    PlayerbotFactory factory(bot, 70);
                    double bestScore = s_bestGearScore.count(gearKey) ? s_bestGearScore[gearKey] : 0.0;
                    double score = 0.0;
                    uint32 bestHit = 0;
                    uint32 hit = 0;
                    bool hitOk = false;
                    double bestEffCache = 0.0;
                    for (int attempt = 0; attempt < (bisApplied ? 0 : 12); ++attempt)
                    {
                        factory.EquipGearBest();
                        score = double(bot->GetPlayerbotAI()
                            ? bot->GetPlayerbotAI()->GetEquipGearScore(bot, false, false) : 0);
                        hit = caster
                            ? bot->GetUInt32Value(PLAYER_FIELD_COMBAT_RATING_1 + CR_HIT_SPELL)
                            : bot->GetUInt32Value(PLAYER_FIELD_COMBAT_RATING_1 + CR_HIT_MELEE);
                        hitOk = (hit >= hitTarget);

                        // 选择口径（站长口径）：**先满足命中，再比 GS**
                        //   用 eff = GS + 2000*min(命中, 上限)：命中每一档（远大于 GS 量级的 100 左右）
                        //   都优先于 GS，命中打满之后才按 GS 排序。
                        double const eff = score + 2000.0 * double(hit < hitTarget ? hit : hitTarget);
                        double const bestEff = bestScore + 2000.0 * double(bestHit < hitTarget ? bestHit : hitTarget);
                        if (eff > bestEff)
                        {
                            bestScore = score;
                            bestHit = hit;
                            bestEffCache = eff;
                            hitOk = true;
                            break;                  // 命中与 GS 都更好：收工
                        }
                        if (eff >= bestEff)
                            break;
                    }
                    s_bestGearScore[gearKey] = bestScore;
                    sLog.outError("[DUMMY] id=%u spec=%s 装备 GS=%.0f 命中=%u/%u%s（该专精最好 GS=%.0f）",
                        d.id, d.spec.c_str(), score, hit, hitTarget, hitOk ? "" : " 未达标", bestScore);
                    // [2026-10-29 站长口径] 猎人要有**合适的弹药**：没有弹药/弹药类型不对时
                    //   自动射击与稳固射击全部失败 ⇒ 整场站着不动（实测 40 秒只有 1 次施法）。
                    if (d.cls == CLASS_HUNTER)
                    {
                        Item* ranged = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
                        uint8 const sub = (ranged && ranged->GetProto()) ? ranged->GetProto()->SubClass : 0;
                        uint32 const ammoId = (sub == ITEM_SUBCLASS_WEAPON_GUN) ? 31735 /*Timeless Shell*/
                                                                              : 31737 /*Timeless Arrow*/;
                        bot->StoreNewItemInBestSlots(ammoId, 1000);
                        bot->SetUInt32Value(PLAYER_AMMO_ID, ammoId);
                        sLog.outError("[DUMMY] id=%u spec=%s 已发弹药 id=%u（远程武器子类=%u，持有=%u）",
                            d.id, d.spec.c_str(), ammoId, (uint32)sub, bot->GetItemCount(ammoId));
                    }

                    if (caster)
                    {
                        bot->StoreNewItemInBestSlots(22832, 20);   // 超级法力药水 x20
                        sLog.outError("[DUMMY] id=%u spec=%s 已发法力药水 x20", d.id, d.spec.c_str());
                    }
                    sLog.outError("[DUMMY] id=%u spec=%s 换装完成：sp=%d ap=%d（GS %.0f）",
                        d.id, d.spec.c_str(),
                        int(bot->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_FIRE)),
                        int(bot->GetTotalAttackPowerValue(BASE_ATTACK)), score);
                }
            }
            // 近战要贴脸、远程要离 18 码（猎人有 8 码死角），否则状态机整场卡在 Approach
            {
                bool const melee = (d.cls == CLASS_WARRIOR || d.cls == CLASS_ROGUE
                    || (d.cls == CLASS_PALADIN && d.spec != "holy")
                    || (d.cls == CLASS_DRUID && d.spec == "feral")
                    || (d.cls == CLASS_SHAMAN && d.spec == "enhance"));
                d.standDist = melee ? 3.0f : 18.0f;
                d.meleeSpec = melee;
            }
            bot->CombatStopWithPets(true);
            // [2026-10-29 实测] 骑乘状态下 Unit::Attack 直接返回 false（"player cannot attack in mount state"），
            //   随机 bot 常年骑在坐骑上 ⇒ 假人测试会变成"站着挨打、victim 永远是空"。
            if (bot->IsMounted())
                bot->Unmount();
            // [2026-10-30] (x,y,z) 一律是 bot 自己的站位（堆主也一样），假人的堆心在 pileX/Y/Z。
            //   顺便按地图实际高度校正一次：GM 岛是座小山，17 个堆的 z 各不相同；
            //   .map 与 vmap 差个一两码（房子/台阶）也能兜住，差太多就保留布局表里的值。
            {
                float z = d.z;
                if (Map* map = bot->GetMap())
                {
                    float const gz = map->GetHeight(d.x, d.y, z, false);
                    if (std::fabs(gz - z) <= 6.0f)
                        z = gz;
                }
                bot->TeleportTo(d.mapId, d.x, d.y, z, 0.0f);
                d.z = z;
            }
            sLog.outError("[DUMMY] id=%u 站位 %.1f 码（近战/远程区分）", d.id, d.standDist);

            if (PlayerbotAI* ai = bot->GetPlayerbotAI())
            {
                ai->ChangeStrategy("+" + d.enable, BotState::BOT_STATE_COMBAT);
                ai->ChangeStrategy("+" + d.enable, BotState::BOT_STATE_NON_COMBAT);
                ai->ChangeStrategy("-" + d.disable1, BotState::BOT_STATE_COMBAT);
                ai->ChangeStrategy("-" + d.disable1, BotState::BOT_STATE_NON_COMBAT);
                ai->ChangeStrategy("-" + d.disable2, BotState::BOT_STATE_COMBAT);
                ai->ChangeStrategy("-" + d.disable2, BotState::BOT_STATE_NON_COMBAT);
            }
            // 骑乘状态下 Unit::Attack 直接返回 false（实测猎人 mounted=1 ⇒ 整场 attack=0）
            if (PlayerbotAI* ai2 = bot->GetPlayerbotAI())
            {
                ai2->ChangeStrategy("-mount", BotState::BOT_STATE_NON_COMBAT);
                ai2->ChangeStrategy("-mount", BotState::BOT_STATE_COMBAT);
                // 猎人：必须开 "ranged"，引擎才知道按远程打（否则会走进近战位，自动射击永远起不来）
                if (d.cls == CLASS_HUNTER)
                {
                    ai2->ChangeStrategy("+ranged", BotState::BOT_STATE_COMBAT);
                    ai2->ChangeStrategy("+ranged", BotState::BOT_STATE_NON_COMBAT);
                }
            }
            if (bot->IsMounted())
                bot->Unmount();

            for (auto const& kv : d.overrides)
                dpsfsm::SetParamOverride(d.guid, kv.first.c_str(), kv.second);
            dpsfsm::SetBotEnabled(d.guid, d.useFsm);
            dpsfsm::SetBotForceDps(d.guid, true);   // 假人场景：坦/奶也按 DPS 打，测的是输出能力
            if (!d.armLabel.empty())
                sLog.outError("[DUMMY] id=%u spec=%s arm=%s", d.id, d.spec.c_str(), d.armLabel.c_str());
            sLog.outError("[DUMMY] id=%u spec=%s bot=%s 已就位（策略 +%s -%s -%s，fsm=%d）",
                d.id, d.spec.c_str(), bot->GetName(), d.enable.c_str(), d.disable1.c_str(), d.disable2.c_str(),
                d.useFsm ? 1 : 0);
        }

        bool StartDummy(std::list<std::string>& messages, std::string const& key, uint32 targets, uint32 seconds,
                        uint32 pinnedGuid = 0, bool useFsm = true, bool respec = true)
        {
            SpecKey const* spec = FindSpecKey(key);
            if (!spec)
            {
                messages.push_back("dummy: 不认识的专精 '" + key + "'"
                    "（fire/frost/arcane/arms/fury/combat/assassination/bm/marks/affliction/destro/enhance/elemental/feral/balance/ret/shadow）");
                return false;
            }

            if (targets < 1) targets = 1;
            if (targets > 5) targets = 5;
            if (seconds < 20) seconds = 20;
            if (seconds > 600) seconds = 600;

            std::set<uint32> used;
            for (auto const& d : s_dummies)
                used.insert(d.guid);
            for (auto const& r : s_runs)
                for (uint32 g : r.guids)
                    used.insert(g);

            Player* bot = nullptr;
            if (pinnedGuid)
            {
                Player* p = FindPlayerAny(pinnedGuid);
                if (p && p->IsInWorld() && p->IsAlive() && p->getClass() == spec->cls && p->GetPlayerbotAI())
                    bot = p;
            }
            if (!bot)
                bot = FindOnlineBotOfClass(spec->cls, used);
            if (!bot)
            {
                // 随机 bot 自动登录是关的 ⇒ 现登一个同职业候选（登录后 TickDummies 再摆姿势）
                std::vector<Candidate> pool = LoadCandidates(60, {});
                Candidate const* pick = nullptr;
                for (auto const& c : pool)
                {
                    if (c.cls != spec->cls || used.count(c.guid))
                        continue;
                    pick = &c;
                    break;
                }
                if (!pick)
                {
                    messages.push_back("dummy: 候选池里也没有 class=" + std::to_string(spec->cls)
                        + " 的角色（level>=60）——先 .bot sim prepare 一批");
                    return false;
                }

                DummyRun d0;
                d0.id = ++s_dummySeq;
                d0.spec = key;
                d0.cls = spec->cls;
                d0.enable = spec->enable;
                d0.disable1 = spec->disable1;
                d0.disable2 = spec->disable2;
                d0.tab = spec->tab;
                d0.premade = spec->premade;
                d0.respec = respec;
                d0.targets = targets;
                d0.durationMs = seconds * 1000;
                d0.startMs = Now();
                d0.guid = pinnedGuid ? pinnedGuid : pick->guid;
                d0.pinnedGuid = pinnedGuid;
                d0.mapId = 1;
                d0.x = DUMMY_SPOT_X + 14.0f * float(int(d0.id % 3) - 1);
                d0.y = DUMMY_SPOT_Y + 14.0f * float(int((d0.id / 3) % 2));
                d0.z = DUMMY_SPOT_Z;
                d0.waitingLogin = true;
                d0.useFsm = useFsm;

                uint32 const accountId = sObjectMgr.GetPlayerAccountIdByGUID(GuidOf(d0.guid));
                if (!accountId)
                {
                    messages.push_back("dummy: 候选机器人没有账号，登录不了");
                    return false;
                }
                sRandomPlayerbotMgr.AddPlayerBot(d0.guid, accountId);
                sLog.outError("[DUMMY] start id=%u spec=%s guid=%u 登录中…", d0.id, d0.spec.c_str(), d0.guid);
                messages.push_back("假人场景已排队 id=" + std::to_string(d0.id) + " spec=" + d0.spec
                    + "（等 bot 登录，约 10 秒）targets=" + std::to_string(targets));
                s_dummies.push_back(d0);
                return true;
            }

            DummyRun d;
            d.id = ++s_dummySeq;
            d.spec = key;
            d.cls = spec->cls;
            d.enable = spec->enable;
            d.disable1 = spec->disable1;
            d.disable2 = spec->disable2;
            d.tab = spec->tab;
            d.premade = spec->premade;
            d.respec = respec;
            d.targets = targets;
            d.durationMs = seconds * 1000;
            d.startMs = Now();
            d.guid = bot->GetGUIDLow();
            d.pinnedGuid = pinnedGuid;
            d.useFsm = useFsm;
            d.mapId = 1;
            // 每个场景错开 40 码（3x2 网格），这样才能同时跑很多组互不干扰
            // GM 岛不大：3 列 × 2 行（列距 14、行距 14），假人再往东 standDist ⇒ 全部留在岛上
            d.x = DUMMY_SPOT_X + 14.0f * float(int(d.id % 3) - 1);
            d.y = DUMMY_SPOT_Y + 14.0f * float(int((d.id / 3) % 2));
            d.z = DUMMY_SPOT_Z;

            bot->CombatStopWithPets(true);
            ApplyDummySetup(d, bot);

            d.dmg0 = bot->botDamageDealt;
            d.pet0 = bot->botPetDamageDealt;
            d.melee0 = bot->botMeleeHits;
            d.casts0 = bot->botCasts;

            sLog.outError("[DUMMY] start id=%u spec=%s bot=%s class=%u targets=%u seconds=%u at=(%.1f,%.1f,%.1f) map=%u",
                d.id, d.spec.c_str(), bot->GetName(), spec->cls, targets, seconds, d.x, d.y, d.z, d.mapId);
            messages.push_back("假人场景已开 id=" + std::to_string(d.id) + " spec=" + d.spec + " bot=" + bot->GetName()
                + " targets=" + std::to_string(targets) + " seconds=" + std::to_string(seconds));
            s_dummies.push_back(d);
            return true;
        }

        // [2026-10-29 站长口径] 所有专精在**同一轮**里一起打同一堆木桩：
        //   堆主（第一个专精）负责召唤，其余人加入同一堆；每人仍只数"自己声明的目标"，
        //   所以 1 目标/5 目标的口径不变，但一轮就能把 17 个专精全测完。
        // [2026-10-29 站长口径] 一轮里同时跑多组不同参数（A/B 并发）：
        //   每个 "spec:key=value" 是一臂，各用**不同 bot**、共用同一堆木桩（玩家之间没有碰撞体积，站一起没事）。
        //   "spec:@" 表示该专精的基线（不覆盖任何参数）。
        void StartArms(std::list<std::string>& messages, std::vector<std::string> const& arms,
                       uint32 targets, uint32 seconds)
        {
            if (targets < 1) targets = 1;
            if (targets > 5) targets = 5;
            if (seconds < 20) seconds = 20;

            struct Arm
            {
                SpecKey const* spec = nullptr;
                std::string key;
                double value = 0.0;
                bool baseline = false;
                std::string label;
            };
            std::vector<Arm> parsed;
            for (auto const& a : arms)
            {
                size_t const colon = a.find(':');
                if (colon == std::string::npos)
                    continue;
                std::string const specKey = a.substr(0, colon);
                std::string const rest = a.substr(colon + 1);
                SpecKey const* spec = FindSpecKey(specKey);
                if (!spec)
                {
                    messages.push_back("arms: 不认识的专精 " + specKey);
                    continue;
                }
                Arm arm;
                arm.spec = spec;
                if (rest == "@")
                {
                    arm.baseline = true;
                    arm.label = "baseline";
                }
                else
                {
                    size_t const eq = rest.find('=');
                    if (eq == std::string::npos)
                        continue;
                    arm.key = rest.substr(0, eq);
                    arm.value = atof(rest.substr(eq + 1).c_str());
                    arm.label = arm.key + "=" + rest.substr(eq + 1);
                }
                parsed.push_back(arm);
            }
            if (parsed.empty())
            {
                messages.push_back("arms: 没有可跑的臂（格式 spec:key=value 或 spec:@）");
                return;
            }

            std::set<uint32> used;
            for (auto const& d : s_dummies)
                used.insert(d.guid);
            for (auto const& r : s_runs)
                for (uint32 g : r.guids)
                    used.insert(g);

            uint32 ownerId = 0;
            uint32 started = 0;
            std::string skipped;
            for (auto const& arm : parsed)
            {
                Player* bot = FindOnlineBotOfClass(arm.spec->cls, used);
                uint32 loginGuid = 0;
                if (!bot)
                {
                    std::vector<Candidate> pool = LoadCandidates(60, {});
                    for (auto const& c : pool)
                    {
                        if (c.cls != arm.spec->cls || used.count(c.guid))
                            continue;
                        loginGuid = c.guid;
                        break;
                    }
                    if (!loginGuid)
                    {
                        skipped += std::string(skipped.empty() ? "" : ",") + arm.label;
                        continue;
                    }
                }

                bool const melee = (arm.spec->cls == CLASS_WARRIOR || arm.spec->cls == CLASS_ROGUE
                    || (arm.spec->cls == CLASS_PALADIN && std::string(arm.spec->key) != "holy")
                    || (arm.spec->cls == CLASS_DRUID && std::string(arm.spec->key) == "feral")
                    || (arm.spec->cls == CLASS_SHAMAN && std::string(arm.spec->key) == "enhance"));

                DummyRun d;
                d.id = ++s_dummySeq;
                d.spec = arm.spec->key;
                d.cls = arm.spec->cls;
                d.enable = arm.spec->enable;
                d.disable1 = arm.spec->disable1;
                d.disable2 = arm.spec->disable2;
                d.tab = arm.spec->tab;
                d.premade = arm.spec->premade;
                d.respec = true;
                d.targets = targets;
                d.durationMs = seconds * 1000;
                d.startMs = Now();
                d.guid = bot ? bot->GetGUIDLow() : loginGuid;
                d.waitingLogin = (bot == nullptr);
                d.mapId = 1;
                d.standDist = melee ? 3.0f : 18.0f;
                d.meleeSpec = melee;
                d.z = DUMMY_SPOT_Z;
                d.sharedPile = true;
                d.armLabel = arm.label;
                if (!arm.baseline)
                    d.overrides[arm.key] = arm.value;

                // 所有臂共用同一堆木桩：第一个臂当堆主（召 1 个假人），其余加入
                d.ringAngle = 6.2831853f * float(started) / float(parsed.size());
                if (!ownerId)
                {
                    ownerId = d.id;
                    d.centerPile = true;
                    d.pileX = DUMMY_SPOT_X;
                    d.pileY = DUMMY_SPOT_Y;
                    d.pileZ = DUMMY_SPOT_Z;
                    d.pileSet = true;
                    // 堆主也站到自己那一侧（远程 18 / 近战 3），量的是**离 0 号假人**的距离
                    //   （假人在堆心正东 PILE_DUMMY_R 处；站到堆心西侧 3 码的话离假人是 5 码，近战够不着）
                    d.x = DUMMY_SPOT_X + PILE_DUMMY_R - d.standDist;
                    d.y = DUMMY_SPOT_Y;
                    d.z = DUMMY_SPOT_Z;
                }
                else
                {
                    d.pileOwner = ownerId;
                    d.x = DUMMY_SPOT_X + PILE_DUMMY_R + cos(d.ringAngle) * d.standDist;
                    d.y = DUMMY_SPOT_Y + sin(d.ringAngle) * d.standDist;
                }

                if (d.waitingLogin)
                {
                    uint32 const accountId = sObjectMgr.GetPlayerAccountIdByGUID(GuidOf(d.guid));
                    if (!accountId)
                    {
                        skipped += std::string(skipped.empty() ? "" : ",") + arm.label;
                        continue;
                    }
                    sRandomPlayerbotMgr.AddPlayerBot(d.guid, accountId);
                }

                used.insert(d.guid);
                s_dummies.push_back(d);
                ++started;
            }

            sLog.outError("[DUMMY] arms 开跑：%u 臂、木桩 %u 个、%u 秒、堆主 id=%u、跳过=[%s]",
                started, targets, seconds, ownerId, skipped.c_str());
            messages.push_back("参数并发开跑：" + std::to_string(started) + " 臂 × "
                + std::to_string(targets) + " 木桩 × " + std::to_string(seconds) + " 秒");
        }

        void StartDummyAll(std::list<std::string>& messages, uint32 targets, uint32 seconds)
        {
            if (targets < 1) targets = 1;
            if (targets > 5) targets = 5;
            if (seconds < 20) seconds = 20;

            std::set<uint32> used;
            for (auto const& d : s_dummies)
                used.insert(d.guid);
            for (auto const& r : s_runs)
                for (uint32 g : r.guids)
                    used.insert(g);

            uint32 const n = uint32(sizeof(s_specKeys) / sizeof(s_specKeys[0]));
            struct PileSpot
            {
                float x, y, z;          // 堆心：假人围在这个点 PILE_DUMMY_R 码一圈
                float standAngle;       // bot 站位方向（弧度）：从 0 号假人（堆心正东 PILE_DUMMY_R）量起
                float standZ;           // bot 站位处的实测地面高度（近战取 3 码点 / 远程取 18 码点）
            };
            // [2026-10-30 站长口径] 一个专精一堆：17 个专精 = 17 堆独立木桩，互不干扰。
            //   ⚠️ GM 岛（map 1, DUMMY_SPOT）是座小山：z≈12.6 的平地只有沿海一条窄带，
            //   **30 码见方的 17 堆场地放不下**（实测 5×4 / 4×5 网格最多只能到 19~20 码）。
            //   所以沿岛的下层台阶排一条 17 点蛇形环带，每堆 z 取地形实测值（运行期再用
            //   Map::GetHeight 校正一次），两两堆心最小间距 **34.1 码**（≥30 ⇒ AoE 不会跨堆）；
            //   已避开岛上的房子（chairs 那片 WMO）和 Spirit Healer。
            //   本表与上面的 s_specKeys 一一对应；坐标/高度来自 ad.exe 的 .map 采样（2026-10-30）。
            static PileSpot const s_specPiles[] =
            {
                { 16346.0f, 16204.0f,  8.1f, 4.5815f,  6.1f },  // fire
                { 16348.0f, 16238.0f, 13.3f, 1.7890f, 11.6f },  // frost
                { 16320.0f, 16262.0f, 21.9f, 3.6652f, 23.6f },  // arcane
                { 16286.0f, 16270.0f, 18.6f, 4.6688f, 19.9f },  // arms
                { 16262.0f, 16296.0f, 14.2f, 3.5779f, 13.9f },  // fury
                { 16274.0f, 16328.0f, 13.5f, 0.0000f, 12.8f },  // combat
                { 16286.0f, 16360.0f, 27.8f, 1.2217f, 28.3f },  // assassination
                { 16242.0f, 16342.0f, 14.9f, 0.9163f, 16.8f },  // bm
                { 16210.0f, 16322.0f, 17.8f, 3.0543f, 17.6f },  // marks
                { 16186.0f, 16294.0f, 13.8f, 1.8326f, 12.5f },  // affliction
                { 16222.0f, 16272.0f, 12.9f, 3.3161f, 12.9f },  // destro
                { 16210.0f, 16238.0f,  8.2f, 4.3633f,  7.5f },  // enhance
                { 16254.0f, 16254.0f, 21.1f, 3.8834f, 22.7f },  // elemental
                { 16272.0f, 16216.0f, 13.3f, 3.7961f, 14.2f },  // feral
                { 16310.0f, 16228.0f, 26.6f, 2.4871f, 25.0f },  // balance
                { 16302.0f, 16306.0f,  8.2f, 5.6287f,  8.3f },  // ret
                { 16188.0f, 16354.0f, 18.5f, 5.6723f, 20.3f },  // shadow
            };
            if (uint32(sizeof(s_specPiles) / sizeof(s_specPiles[0])) != n)
            {
                messages.push_back("dummyall: 木桩布局表与专精表长度不一致（改专精表时别忘了它）");
                return;
            }

            uint32 started = 0;
            std::string skipped;

            for (uint32 i = 0; i < n; ++i)
            {
                SpecKey const& spec = s_specKeys[i];
                PileSpot const& spot = s_specPiles[i];
                Player* bot = FindOnlineBotOfClass(spec.cls, used);
                uint32 loginGuid = 0;
                if (!bot)
                {
                    // 没有在线同职业 bot ⇒ 从候选池里挑一个拉上线（TickDummies 会等它登录）
                    std::vector<Candidate> pool = LoadCandidates(60, {});
                    for (auto const& c : pool)
                    {
                        if (c.cls != spec.cls || used.count(c.guid))
                            continue;
                        loginGuid = c.guid;
                        break;
                    }
                    if (!loginGuid)
                    {
                        skipped += std::string(skipped.empty() ? "" : ",") + spec.key;
                        continue;
                    }
                }

                bool const melee = (spec.cls == CLASS_WARRIOR || spec.cls == CLASS_ROGUE
                    || (spec.cls == CLASS_PALADIN && std::string(spec.key) != "holy")
                    || (spec.cls == CLASS_DRUID && std::string(spec.key) == "feral")
                    || (spec.cls == CLASS_SHAMAN && std::string(spec.key) == "enhance"));

                DummyRun d;
                d.id = ++s_dummySeq;
                d.spec = spec.key;
                d.cls = spec.cls;
                d.enable = spec.enable;
                d.disable1 = spec.disable1;
                d.disable2 = spec.disable2;
                d.tab = spec.tab;
                d.premade = spec.premade;
                d.respec = true;
                d.targets = targets;
                d.durationMs = seconds * 1000;
                d.startMs = Now();
                d.guid = bot ? bot->GetGUIDLow() : loginGuid;
                d.waitingLogin = (bot == nullptr);
                d.mapId = 1;
                d.standDist = melee ? 3.0f : 18.0f;
                d.meleeSpec = melee;
                // 一堆一个专精：自己就是这堆的堆主，假人摆在 pileX/pileY（不共用别人的堆）。
                d.pileX = spot.x;
                d.pileY = spot.y;
                d.pileZ = spot.z;
                d.pileSet = true;
                d.ringAngle = spot.standAngle;
                // 站位量的是离 **0 号假人** 的距离（假人在堆心正东 PILE_DUMMY_R 处），
                //   否则近战站在堆心西侧 3 码、离假人其实是 5 码，正好卡在 ATTACK_DISTANCE 边上。
                d.x = spot.x + PILE_DUMMY_R + cos(spot.standAngle) * d.standDist;
                d.y = spot.y + sin(spot.standAngle) * d.standDist;
                d.z = spot.standZ;

                // 一堆一个专精 ⇒ 这堆木桩就是本 run **私有**的：收尾时由 ReportDummy 撤掉自己这一批。
                //   （旧规则"共享堆谁都不许删"仍保留给 `.rndbot sim arms`：多个臂共用一堆时 sharedPile=true，
                //    堆主先收尾也不会删掉别人还在打的木桩。）
                //   不这么清的话，下一轮开始前上一轮的 17 堆还在原地，AoE 会同时打到旧木桩 ⇒ dps 虚高。
                d.sharedPile = false;
                d.centerPile = true;

                if (d.waitingLogin)
                {
                    uint32 const accountId = sObjectMgr.GetPlayerAccountIdByGUID(GuidOf(d.guid));
                    if (!accountId)
                    {
                        skipped += std::string(skipped.empty() ? "" : ",") + spec.key;
                        continue;
                    }
                    sRandomPlayerbotMgr.AddPlayerBot(d.guid, accountId);
                }

                used.insert(d.guid);
                s_dummies.push_back(d);
                ++started;
            }

            sLog.outError("[DUMMY] dummyall 开跑：%u 个专精 = %u 堆独立木桩（每堆 %u 个）、%u 秒、"
                "最小堆心间距 34.1 码（≥30）、跳过=[%s]",
                started, started, targets, seconds, skipped.c_str());
            messages.push_back("整队假人开跑：" + std::to_string(started) + " 个专精，各自独立木桩 × "
                + std::to_string(targets) + " 个 × " + std::to_string(seconds) + " 秒"
                + (skipped.empty() ? "" : "；跳过（无在线 bot）：" + skipped));
        }

        bool SpawnDummies(DummyRun& d, Player* bot)
        {
            if (bot->GetMapId() != d.mapId)
                return false;
            Map* map = bot->GetMap();
            if (!map)
                return false;

            float const r = PILE_DUMMY_R;
            // [2026-10-30] 假人摆在自己那一堆的堆心（pileX/pileY），不是 bot 站位 (x,y)。
            float cx = d.pileX;
            float cy = d.pileY;
            float cz = d.pileZ;
            {
                float const gz = map->GetHeight(cx, cy, cz, false);
                if (std::fabs(gz - cz) <= 6.0f)
                    cz = gz;
                d.pileZ = cz;
            }
            for (uint32 i = 0; i < d.targets; ++i)
            {
                float const ang = (2.0f * 3.14159265358979f * float(i)) / float(d.targets);
                float const px = cx + cos(ang) * r;
                float const py = cy + sin(ang) * r;
                // 190014 = 普通训练假人（100 万血、无技能、不还手）；faction 14 = 敌对，否则 bot 打不了
                Creature* c = bot->SummonCreature(190014, px, py, cz, ang,
                    TEMPSPAWN_TIMED_OR_DEAD_DESPAWN, d.durationMs + 120000, false, false, 0, 14);
                if (!c)
                {
                    sLog.outError("[DUMMY] id=%u 召唤假人失败（entry=190014 模板不存在？）", d.id);
                    return false;
                }
                c->setFaction(14);
                // [2026-10-29 实测] SummonCreature 出来的假人没有 home 点，CombatManager 会标成 evade；
                //   而 Unit::Attack() 对 evade 目标直接返回 false（"victim in evade mode"）⇒ 远程 bot 永远开不了打。
                c->GetCombatManager().SetEvadeState(EVADE_NONE);
                c->GetCombatManager().StopEvade();
                // [2026-10-29 站长口径] 假人**不要转身**：它一转，近战就得绕背，绕背又打断输出。
                //   站桩 + 固定朝向（朝东），近战就能稳定站在背后打。
                // 假人本来就不该转身：模板侧由 dev/232_测试假人_不转身.sql 设成 STUNNED|PACIFIED，
                // 这里给召唤出来的副本补同一组标志（不等 DB 重载）。
                c->GetMotionMaster()->MoveIdle();
                // DB 里 232_测试假人_不转身.sql 已经把模板设成 STUNNED|PACIFIED；
                // 召唤出来的副本也补一遍（不依赖 DB 重载）：STUNNED 禁用转身，PACIFIED 让它不主动出手。
                c->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_STUNNED | UNIT_FLAG_PACIFIED);
                c->Relocate(px, py, c->GetPositionZ());
                if (c->GetPositionZ() < cz - 5.0f || c->GetPositionZ() > cz + 20.0f)
                    sLog.outError("[DUMMY] id=%u 假人落点异常 z=%.1f（堆心 z=%.1f）⇒ 可能掉进水里/地形下",
                        d.id, c->GetPositionZ(), cz);
                // 模板血量可能没跟 SQL 对齐（实测只有几千）⇒ 强制拉到 100 万，2 分钟打不死
                // 共享堆时一堆人围打：100 万撑不住（17 人 × 500 dps × 60s ≈ 51 万，留余量）
                c->SetMaxHealth(5000000);
                c->SetHealth(c->GetMaxHealth());
                d.dummies.push_back(c->GetObjectGuid());
            }
            // 召唤完立刻让假人咬 bot 一口：核心的"进入战斗"是双向的，先把假人→bot 这条建立起来，
            //   免得状态机的第一次 Attack 因为 combat manager 还没初始化而被拒。
            if (Creature* first = map->GetCreature(d.dummies[0]))
            {
                if (first->GetCombatManager().IsInEvadeMode())
                {
                    first->GetCombatManager().SetEvadeState(EVADE_NONE);
                    first->GetCombatManager().StopEvade();
                }
                // 同上：假人主动咬 bot 会让它自己走到 bot 脸上（远程场景致命），只给近战用
                    /* 假人只当木桩：不主动咬 bot（否则它会自己走到 bot 脸上，远程职业进 8 码死角） */
            }
            dpsfsm::SetScenarioTargets(d.guid, d.dummies, d.id);   // pack 只数自己的假人（带 runId 防串）
            return !d.dummies.empty();
        }

        void ReportDummy(DummyRun& d, Player* bot, char const* result)
        {
            if (d.reported)
                return;
            d.reported = true;

            uint64 dmg = 0, pet = 0, melee = 0, casts = 0;
            std::string top;
            if (bot)
            {
                dmg = bot->botDamageDealt > d.dmg0 ? bot->botDamageDealt - d.dmg0 : 0;
                pet = bot->botPetDamageDealt > d.pet0 ? bot->botPetDamageDealt - d.pet0 : 0;
                melee = bot->botMeleeHits > d.melee0 ? bot->botMeleeHits - d.melee0 : 0;
                casts = bot->botCasts > d.casts0 ? bot->botCasts - d.casts0 : 0;

                std::vector<std::pair<uint32, uint64>> list;
                for (auto const& kv : bot->botSpellCounts)
                {
                    uint64 const before = d.spells0.count(kv.first) ? d.spells0[kv.first] : 0;
                    if (kv.second > before)
                        list.push_back(std::make_pair(kv.first, kv.second - before));
                }
                std::sort(list.begin(), list.end(),
                    [](std::pair<uint32, uint64> const& a, std::pair<uint32, uint64> const& b) { return a.second > b.second; });
                for (size_t i = 0; i < list.size() && i < 6; ++i)
                {
                    if (!top.empty()) top += ",";
                    top += std::to_string(list[i].first) + ":" + std::to_string(list[i].second);
                }
            }

            std::string extra = "";
            if (!d.armLabel.empty())
                extra += "arm=" + d.armLabel + " ";   // 参数并发调优靠这个字段归组
            if (bot)
            {
                // 属性：用来判断这一轮到底是不是"毕业装"（法伤 / 攻强）
                extra += "sp=" + std::to_string(int(bot->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_FIRE)));
                extra += " ap=" + std::to_string(int(bot->GetTotalAttackPowerValue(BASE_ATTACK))) + " ";
                // 武器子类（证明刺杀贼是不是双匕首：16=dagger）
                {
                    Item* mh = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
                    Item* oh = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
                    // 远程职业是否真的带着 "ranged" 策略（引擎的接近距离靠它判断）
                    extra += bot->GetPlayerbotAI()
                        && bot->GetPlayerbotAI()->HasStrategy("ranged", BotState::BOT_STATE_COMBAT)
                        ? "ranged=1 " : "ranged=0 ";
                    // 近战诊断：主手是否真有武器、攻击计时是否正常、是否在近战范围内、武器技能
                    if (d.meleeSpec)
                    {
                        Item* mhi = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
                        Unit* v = bot->GetVictim();
                        extra += "mh=" + std::to_string(mhi ? int(mhi->GetEntry()) : -1);
                        extra += " wtimer=" + std::to_string(bot->getAttackTimer(BASE_ATTACK));
                        extra += " inreach=" + std::to_string(v ? (bot->CanReachWithMeleeAttack(v) ? 1 : 0) : -1);
                        extra += " delay=" + std::to_string(mhi && mhi->GetProto() ? int(mhi->GetProto()->Delay) : -1);
                        extra += " ";
                    }
                    extra += "wep=" + std::to_string(mh && mh->GetProto() ? int(mh->GetProto()->SubClass) : -1)
                           + "/" + std::to_string(oh && oh->GetProto() ? int(oh->GetProto()->SubClass) : -1) + " ";
                }
                // pack = 该场景里 bot 半径 10/30 码看到的敌对数量（1 目标场景应当只有自己的假人）
                int const pack10 = bot->IsInWorld() ? dpsfsm::PackCount(bot, 10.0f) : 0;
                int const pack30 = bot->IsInWorld() ? dpsfsm::PackCount(bot, 30.0f) : 0;
                extra += "pack=" + std::to_string(pack10) + "/" + std::to_string(pack30);
                extra += " fsm=";
                extra += d.useFsm ? "1" : "0";
                extra += " incombat=";
                extra += bot->IsInCombat() ? "1" : "0";
                extra += " victim=";
                extra += (bot->GetVictim() ? bot->GetVictim()->GetName() : "-");
            }
            double const sec = d.durationMs / 1000.0;
            double const total = double(dmg + pet);
            sLog.outError("[DUMMY] id=%u spec=%s bot=%s guid=%u targets=%u dur=%.1fs result=%s %s dmg=%llu pet=%llu total=%llu dps=%.1f casts=%llu melee=%llu top=%s",
                d.id, d.spec.c_str(), bot ? bot->GetName() : "?", d.guid, d.targets, sec, result, extra.c_str(),
                (unsigned long long)dmg, (unsigned long long)pet, (unsigned long long)total,
                sec > 0.0 ? total / sec : 0.0, (unsigned long long)casts, (unsigned long long)melee,
                top.empty() ? "-" : top.c_str());

            dpsfsm::ClearScenarioTargets(d.guid, d.id);
            dpsfsm::ClearParamOverrides(d.guid);
            // 收尾：撤假人 + 停手
            if (bot && bot->IsInWorld())
            {
                // [2026-10-29 实测] 共享堆不能在这里删：堆主先收尾会把别人的木桩一起删掉
                //   （其余 15 个专精全部 aborted(dummy-gone)）。共用的靠召唤时的定时消失。
                if (!d.sharedPile)
                {
                    if (Map* map = bot->GetMap())
                        for (ObjectGuid const& g : d.dummies)
                            if (Creature* c = map->GetCreature(g))
                                c->ForcedDespawn();
                }
                bot->CombatStopWithPets(true);
            }
        }

        void TickDummies(uint32 now)
        {
            for (auto it = s_dummies.begin(); it != s_dummies.end();)
            {
                DummyRun& d = *it;

                Player* bot = FindPlayerAny(d.guid);
                if (!bot || !bot->GetPlayerbotAI())
                {
                    if (d.waitingLogin && now - d.startMs < 90000)
                    {
                        ++it;
                        continue;
                    }
                    ReportDummy(d, nullptr, "aborted(offline)");
                    it = s_dummies.erase(it);
                    continue;
                }

                // [2026-10-29 崩溃修] 刚登录的 bot 可能还没进世界（m_currMap 为空），
                //   这时做 Unmount/TeleportTo/GetMap 会触发核心断言 "GetMap(): m_currMap" 直接关服。
                if (!bot->IsInWorld() || !bot->IsAlive())
                {
                    ++it;
                    continue;
                }

                if (!d.setupDone)
                    ApplyDummySetup(d, bot);

                // [2026-10-30] 单组场景（`.rndbot sim dummy`）没在启动时给堆心：按老规矩摆在 bot 东侧
                //   standDist 码处（ApplyDummySetup 刚算好近战/远程的 standDist）。dummyall/arms 已自带堆心。
                if (!d.pileSet && !d.pileOwner)
                {
                    d.pileX = d.x + d.standDist;
                    d.pileY = d.y;
                    d.pileZ = d.z;
                    d.pileSet = true;
                }

                // BIS 流水：每 tick 只穿一件（站长口径），穿完刷新武器技能/伤害再开始计时
                if (!d.gearReady && bot->IsInWorld())
                {
                    if (!d.bisPending.empty())
                    {
                        auto const item = d.bisPending.front();
                        d.bisPending.erase(d.bisPending.begin());
                        if (ItemPrototype const* proto = sObjectMgr.GetItemPrototype(item.second))
                        {
                            bool const twoHand = (proto->InventoryType == INVTYPE_2HWEAPON
                                || proto->InventoryType == INVTYPE_RANGED || proto->InventoryType == INVTYPE_RANGEDRIGHT
                                || proto->InventoryType == INVTYPE_THROWN);
                            if (!(item.first == EQUIPMENT_SLOT_OFFHAND && twoHand))
                                if (Item* it2 = bot->StoreNewItemInInventorySlot(item.second, 1))
                                    ai::EquipAction::EquipItem(bot->GetPlayerbotAI(), bot, it2, true);
                        }
                        ++it;
                        continue;                 // 这一 tick 只做这一件
                    }

                    d.gearReady = true;
                    static uint16 const weaponSkills[] = {
                        SKILL_AXES, SKILL_2H_AXES, SKILL_BOWS, SKILL_CROSSBOWS, SKILL_DAGGERS,
                        SKILL_FIST_WEAPONS, SKILL_GUNS, SKILL_MACES, SKILL_2H_MACES, SKILL_POLEARMS,
                        SKILL_STAVES, SKILL_SWORDS, SKILL_2H_SWORDS, SKILL_THROWN, SKILL_WANDS };
                    for (uint16 sk : weaponSkills)
                        bot->SetSkill(sk, bot->GetLevel() * 5, bot->GetLevel() * 5);
                    bot->UpdateDamagePhysical(BASE_ATTACK);
                    bot->UpdateDamagePhysical(OFF_ATTACK);
                    bot->UpdateAllStats();
                    d.dmg0 = bot->botDamageDealt;
                    d.pet0 = bot->botPetDamageDealt;
                    d.melee0 = bot->botMeleeHits;
                    d.casts0 = bot->botCasts;
                    d.spells0 = bot->botSpellCounts;
                    d.startMs = now;           // 装备齐了才开始计时
                    sLog.outError("[DUMMY] id=%u spec=%s BIS 流水完成：sp=%d ap=%d",
                        d.id, d.spec.c_str(),
                        int(bot->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_FIRE)),
                        int(bot->GetTotalAttackPowerValue(BASE_ATTACK)));
                }

                // [2026-10-29 站长口径] 一堆木桩、所有专精同一轮一起打：
                //   pileOwner 的 run 自己不召假人，等堆主召好后"加入"同一堆（各自仍然只数自己声明的目标）。
                if (d.pileOwner)
                {
                    DummyRun* owner = nullptr;
                    for (auto& other : s_dummies)
                        if (other.id == d.pileOwner && !other.dummies.empty())
                        {
                            owner = &other;
                            break;
                        }
                    if (!owner)
                    {
                        if (now - d.startMs > 90000)
                        {
                            ReportDummy(d, bot, "aborted(no-pile)");
                            it = s_dummies.erase(it);
                            continue;
                        }
                        ++it;
                        continue;
                    }

                    d.dummies = owner->dummies;
                    // 站位坐标在 StartDummyAll 里就算好了（按自己那堆的堆心 + 角度），这里直接用 d.x/d.y。
                    //   ⚠️ 曾经这里重新按 DUMMY_SPOT_X 算，导致外围两堆的近战被传到中间那堆，永远够不着自己的木桩。
                    float const cx = d.x;
                    float const cy = d.y;
                    if (bot->IsMounted())
                        bot->Unmount();
                    bot->TeleportTo(d.mapId, cx, cy, d.z, 0.0f);
                    d.x = cx;
                    d.y = cy;
                    dpsfsm::SetScenarioTargets(d.guid, d.dummies, d.id);
                    d.dmg0 = bot->botDamageDealt;
                    d.pet0 = bot->botPetDamageDealt;
                    d.melee0 = bot->botMeleeHits;
                    d.casts0 = bot->botCasts;
                    d.spells0 = bot->botSpellCounts;
                    d.startMs = now;
                    d.pileOwner = 0;      // 已加入
                    sLog.outError("[DUMMY] id=%u spec=%s 加入共享木桩堆（%u 个），站位角度 %.2f",
                        d.id, d.spec.c_str(), (uint32)d.dummies.size(), d.ringAngle);
                }

                // 传送落地后才召唤假人
                if (d.dummies.empty())
                {
                    if (bot->GetMapId() != d.mapId)
                    {
                        if (now - d.startMs > 60000)
                        {
                            ReportDummy(d, bot, "aborted(no-teleport)");
                            it = s_dummies.erase(it);
                            continue;
                        }
                        ++it;
                        continue;
                    }
                    if (!SpawnDummies(d, bot))
                    {
                        ReportDummy(d, bot, "aborted(no-dummy)");
                        it = s_dummies.erase(it);
                        continue;
                    }
                    // 基线改成"假人到手之后"，避免把传送前的动作算进来
                    d.dmg0 = bot->botDamageDealt;
                    d.pet0 = bot->botPetDamageDealt;
                    d.melee0 = bot->botMeleeHits;
                    d.casts0 = bot->botCasts;
                    d.spells0 = bot->botSpellCounts;
                    d.startMs = now;
                    sLog.outError("[DUMMY] id=%u spec=%s 假人已就位 %u 个，开始计时", d.id, d.spec.c_str(), (uint32)d.dummies.size());
                }

                // 每 2 秒把人拽回来 + 重新咬住假人（假人不还手，bot 自己容易脱战）
                if (now - d.lastAttackMs > 1000)
                {
                    d.lastAttackMs = now;
                    Map* map = bot->IsInWorld() ? bot->GetMap() : nullptr;
                    if (!map)
                    {
                        ++it;
                        continue;
                    }
                    Creature* first = map ? map->GetCreature(d.dummies[0]) : nullptr;
                    if (!first || !first->IsAlive())
                    {
                        ReportDummy(d, bot, "aborted(dummy-gone)");
                        it = s_dummies.erase(it);
                        continue;
                    }
                    if (!bot->IsWithinDist3d(d.x, d.y, d.z, 28.0f) || bot->GetMapId() != d.mapId)
                        bot->TeleportTo(d.mapId, d.x, d.y, d.z, 0.0f);
                    if (now - d.lastPulseMs > 10000)
                    {
                        d.lastPulseMs = now;
                        sLog.outError("[DUMMY-PULSE] id=%u bot=%s incombat=%d victim=%s dummy=%s alive=%d dist=%.1f "
                            "moving=%d gen=%u mounted=%d hostile=%d hostileBack=%d evade=%d faction=%u dummypos=(%.1f,%.1f)",
                            d.id, bot->GetName(), bot->IsInCombat() ? 1 : 0,
                            bot->GetVictim() ? bot->GetVictim()->GetName() : "-",
                            first ? first->GetName() : "-", first ? (first->IsAlive() ? 1 : 0) : -1,
                            first ? bot->GetDistance(first) : -1.0f,
                            bot->IsMoving() ? 1 : 0, uint32(bot->GetMotionMaster()->GetCurrentMovementGeneratorType()),
                            bot->IsMounted() ? 1 : 0,
                            first ? (sServerFacade.IsHostileTo(bot, first) ? 1 : 0) : -1,
                            first ? (sServerFacade.IsHostileTo(first, bot) ? 1 : 0) : -1,
                            first ? (first->GetCombatManager().IsInEvadeMode() ? 1 : 0) : -1,
                            first ? first->GetFaction() : 0,
                            first ? first->GetPositionX() : 0.0f, first ? first->GetPositionY() : 0.0f);
                    }
                    // [2026-10-30 gdb 抓栈] MoveChase 是**场景驱动自己**发的（TickDummies），
                    //   一开打就把远程 bot 拽进近战（18.9 → 2.2 码，自动射击进死区）。
                    //   ⇒ 场景驱动层把远程 bot 钉回站位（不动引擎、不动状态机）。
                    if (!d.meleeSpec && bot->IsInWorld() && !bot->IsWithinDist3d(d.x, d.y, d.z, 2.0f))
                        bot->GetMotionMaster()->MovePoint(0, d.x, d.y, d.z);

                    if (!bot->GetVictim())
                    {
                        // 先清 evade：召唤物没有 home 点会被标成回城，Unit::Attack 直接拒绝
                        if (first->GetCombatManager().IsInEvadeMode())
                        {
                            first->GetCombatManager().SetEvadeState(EVADE_NONE);
                            first->GetCombatManager().StopEvade();
                        }
                        // [2026-10-29] 直接用 bot->Attack() 会被 bot 自己的 AI 立刻清掉
                        //   （实测：整场 incombat=1 / victim=- / casts=0，只有魔杖自动射击在打）。
                        //   走引擎自己的 "attack anything" 动作才是和副本里同一条路径。
                        Event pullEvent("attack anything", first->GetObjectGuid());
                        bool const acted = bot->GetPlayerbotAI()->DoSpecificAction("attack anything", pullEvent, true);
                        bool const direct = bot->GetVictim() ? false : bot->Attack(first, d.meleeSpec);
                        // 动作路径偶尔不落地（实测 elemental/affliction 整场 incombat=0），
                        //   补一次直接 Attack —— 现在先把坐骑下了，Unit::Attack 不会再被 mount 检查挡掉
                        if (!bot->GetVictim() && !bot->IsMounted() && first->IsAlive())
                            bot->Attack(first, d.meleeSpec);
                        // Unit::Attack 有个坑：如果 bot 还挂在目标的 m_attackers 里（但自己的 m_attacking 已清空），
                        //   _addAttacker 返回 false ⇒ Attack 直接失败，而且之后每次都失败（实测整场 victim 拿不到）。
                        //   先 CombatStop 把这个状态清干净，再咬一次。
                        if (!bot->GetVictim() && !bot->IsMounted() && first->IsAlive())
                        {
                            if (first->GetCombatManager().IsInEvadeMode())
                            {
                                first->GetCombatManager().SetEvadeState(EVADE_NONE);
                                first->GetCombatManager().StopEvade();
                            }
                            bot->CombatStopWithPets(true);
                            bot->Attack(first, d.meleeSpec);
                        }
                        if (!bot->GetVictim() && bot->IsMounted())
                        {
                            bot->Unmount();
                            sLog.outError("[DUMMY-PULSE] id=%u bot 又骑上坐骑了，强制下马", d.id);
                        }
                        // [2026-10-30 gdb 取证] 这句会让**假人**去追 bot（UnitAI::AttackStart →
                        //   MoveChase(bot, 0)），假人贴到 2~3 码后猎人落进 8 码死角、自动射击停摆。
                        //   只在近战场景保留这条"进入战斗"的兜底；远程场景不许假人动。
                            /* 假人只当木桩：不主动咬 bot（否则它会自己走到 bot 脸上，远程职业进 8 码死角） */
                        sLog.outError("[DUMMY-PULSE] id=%u 出手: action=%d direct=%d victim=%s attacker=%d",
                            d.id, acted ? 1 : 0, direct ? 1 : 0,
                            bot->GetVictim() ? bot->GetVictim()->GetName() : "-",
                            bot->getAttackerForHelper() ? 1 : 0);
                    }
                }

                if (now - d.startMs >= d.durationMs)
                {
                    ReportDummy(d, bot, "ok");
                    it = s_dummies.erase(it);
                    continue;
                }
                ++it;
            }
        }

    } // namespace

    // -----------------------------------------------------------------------
    std::list<std::string> HandleCommand(Player* master, const std::string& param, AccountTypes security)
    {
        std::list<std::string> messages;
        char const* who = master ? master->GetName() : "console";

        if (security < SEC_GAMEMASTER)
        {
            messages.push_back("只有 GM 能用仿真命令。");
            return messages;
        }

        if (!sPlayerbotAIConfig.simMode)
        {
            messages.push_back("仿真模式未开启：需要 AiPlayerbot.SimMode = 1（云端必须是 0）。");
            return messages;
        }

        std::vector<std::string> args;
        {
            std::string cur;
            for (char c : param)
            {
                if (c == ' ' || c == '\t')
                {
                    if (!cur.empty()) { args.push_back(cur); cur.clear(); }
                }
                else
                    cur += c;
            }
            if (!cur.empty())
                args.push_back(cur);
        }

        // [2026-10-24 站长口径] 不要靠加日志查问题：`.rndbot sim dump` 直接**读当前内存状态**打一行一份快照
        //   （每成员：目标/受害/距离/仇恨、坦克目标是否可达、队伍正在被几只怪打）
        if (!args.empty() && args[0] == "dump")
        {
            // [2026-10-24 站长规矩] dump 要**一次给全**：组状态 + 每个成员（位置/血蓝/战斗/目标/仇恨/可达/AI目标/被谁打）
            //   + 场上所有正在打队伍的怪（entry/hp/目标/仇恨/距离）。写 /tmp/simdump.txt（覆盖），不刷日志。
            std::vector<std::string> out;
            for (auto const& run : s_runs)
            {
                Map* dmap = sMapMgr.FindMap(run.mapId, run.instanceId);
                Player* tank = nullptr;
                for (uint32 guid : run.guids)
                {
                    Player* p = FindPlayerInWorld(guid);
                    if (p && PlayerbotAI::IsTank(p)) { tank = p; break; }
                }

                uint32 cleared = 0, skipped = 0;
                uint16 curOrd = 0, lockOrd = 0;
                if (tank)
                {
                    dungeonroute::MapRoute const* r = dungeonroute::GetMapRoute(run.mapId);
                    if (r)
                    {
                        curOrd = dungeonroute::CurrentOrd(tank, *r);
                        lockOrd = dungeonroute::StateOf(tank).lockOrd;
                    }
                    // cleared/skipped 用仿真自己的统计（阶段推进时另算）
                    cleared = run.lastCleared;
                    skipped = 0;
                }
                out.push_back("G run=" + std::to_string(run.runId) + " g=" + std::to_string(run.index)
                    + " map=" + std::to_string(run.mapId) + " inst=" + std::to_string(run.instanceId)
                    + " phase=" + PhaseName(run.phase) + " t=" + std::to_string((Now() - run.startedAt) / 1000)
                    + "s cleared=" + std::to_string(cleared) + " skipped=" + std::to_string(skipped)
                    + " deaths=" + std::to_string(run.deaths)
                    + " curOrd=" + std::to_string(uint32(curOrd)) + " lockOrd=" + std::to_string(uint32(lockOrd))
                    + " map=" + (dmap ? "loaded" : "null"));

                // 场上正在打队伍的所有怪
                std::set<ObjectGuid> engaged;
                for (uint32 guid : run.guids)
                {
                    Player* p = FindPlayerInWorld(guid);
                    if (!p || !p->IsInWorld()) continue;
                    for (Unit* a : p->getAttackers())
                        if (a && a->IsAlive()) engaged.insert(a->GetObjectGuid());
                }
                for (auto const& eg : engaged)
                {
                    Unit* u = dmap ? dmap->GetUnit(eg) : nullptr;
                    if (!u) continue;
                    Unit* v = u->GetVictim();
                    float thTank = tank ? u->getThreatManager().getThreat(tank) : 0.0f;
                    out.push_back("E run=" + std::to_string(run.runId) + " g=" + std::to_string(run.index)
                        + " " + std::to_string(u->GetEntry()) + "(" + u->GetName() + ")"
                        + " hp=" + std::to_string(uint32(u->GetHealthPercent())) + "%"
                        + " pos=(" + std::to_string(int32(u->GetPositionX())) + "," + std::to_string(int32(u->GetPositionY())) + ")"
                        + " victim=" + (v ? v->GetName() : "-")
                        + " th_tank=" + std::to_string(uint32(thTank))
                        + " dist_tank=" + std::to_string(tank ? uint32(tank->GetDistance(u)) : 0));
                }

                for (uint32 guid : run.guids)
                {
                    Player* p = FindPlayerInWorld(guid);
                    if (!p) { out.push_back("M run=" + std::to_string(run.runId) + " guid=" + std::to_string(guid) + " MISSING"); continue; }
                    Unit* v = p->GetVictim();
                    std::string vs = "-";
                    if (v)
                    {
                        float const selfTh = v->getThreatManager().getThreat(p);
                        float const tankTh = tank ? v->getThreatManager().getThreat(tank) : 0.0f;
                        bool okP = false; float endP = -1.0f;
                        if (tank)
                        {
                            PathFinder pf(tank);
                            okP = pf.calculate(v->GetPositionX(), v->GetPositionY(), v->GetPositionZ());
                            if (okP && !pf.getPath().empty())
                            {
                                auto& e = pf.getPath().back();
                                float dx = e.x - v->GetPositionX(), dy = e.y - v->GetPositionY(), dz = e.z - v->GetPositionZ();
                                endP = std::sqrt(dx * dx + dy * dy + dz * dz);
                            }
                        }
                        vs = std::to_string(v->GetEntry()) + ":" + v->GetName()
                            + " hp=" + std::to_string(uint32(v->GetHealthPercent())) + "%"
                            + " d=" + std::to_string(uint32(p->GetDistance(v)))
                            + " th=" + std::to_string(uint32(selfTh)) + "/" + std::to_string(uint32(tankTh))
                            + (okP ? (endP <= 3.0f ? " R" : (" end" + std::to_string(uint32(endP)))) : " NR");
                    }
                    std::string atk;
                    for (Unit* a : p->getAttackers())
                        if (a) atk += " " + std::to_string(a->GetEntry()) + "@" + std::to_string(uint32(p->GetDistance(a)));
                    ObjectGuid aiTgt; uint32 aiTgtEntry = 0;
                    if (PlayerbotAI* pai = p->GetPlayerbotAI())
                    {
                        aiTgt = pai->GetAiObjectContext()->GetValue<ObjectGuid>("current target")->Get();
                        if (Unit* au = pai->GetUnit(aiTgt)) aiTgtEntry = au->GetEntry();
                    }
                    std::string role = PlayerbotAI::IsTank(p) ? "T" : (PlayerbotAI::IsHeal(p) ? "H" : "D");
                    out.push_back(std::string("M run=") + std::to_string(run.runId) + " g=" + std::to_string(run.index)
                        + " " + p->GetName() + " cls=" + std::to_string(uint32(p->getClass())) + " role=" + role
                        + " alive=" + std::to_string(p->IsAlive() ? 1 : 0)
                        + " combat=" + std::to_string(p->IsInCombat() ? 1 : 0)
                        + " hp=" + std::to_string(uint32(p->GetHealthPercent())) + "%"
                        + " mp=" + (p->GetMaxPower(POWER_MANA) ? std::to_string(uint32(p->GetPowerPercent(POWER_MANA))) + "%" : "-")
                        + " pos=(" + std::to_string(int32(p->GetPositionX())) + "," + std::to_string(int32(p->GetPositionY())) + ")"
                        + " aitgt=" + std::to_string(aiTgtEntry)
                        // [2026-10-26] 补现场：当前移动生成器 + 拉回状态（"钉在原地"到底是谁钉的）
                        + " move=" + std::to_string(uint32(p->GetMotionMaster()
                            ? uint8(p->GetMotionMaster()->GetCurrentMovementGeneratorType()) : 0))
                        + " pb=" + std::to_string(uint32(dungeonroute::StateOf(p).pullBackPending))
                        + " so=" + std::to_string(dungeonroute::StateOf(p).standOverrideUntil ? 1 : 0)
                        + " sod=" + std::to_string(int32(dungeonroute::StateOf(p).standOverrideUntil
                            ? p->GetDistance(dungeonroute::StateOf(p).standOverrideX,
                                             dungeonroute::StateOf(p).standOverrideY,
                                             dungeonroute::StateOf(p).standOverrideZ) : -1.0f))
                        + " casts=" + [&]{ std::string cs; for (uint32 sid : PlayerbotAI::LastSpellCasts(p->GetObjectGuid())) { if (!cs.empty()) cs += ","; cs += std::to_string(sid); } return cs.empty() ? std::string("-") : cs; }()
                        + " victim=" + vs
                        + " attackers=" + (atk.empty() ? "-" : atk.c_str()));
                }
            }
            if (out.empty()) out.push_back("dump: no runs");
            {
                std::ofstream df("/tmp/simdump.txt", std::ios::trunc);
                if (df.is_open())
                    for (auto const& m : out) df << m << "\n";
            }
            return messages;
        }

        if (args.empty() || args[0] == "status")
        {
            if (s_runs.empty())
                messages.push_back("仿真：没有正在跑的组。");
            for (auto const& run : s_runs)
            {
                std::string names;
                FormatRunGuids(run, names);
                messages.push_back("run=" + std::to_string(run.runId) + " g=" + std::to_string(run.index)
                    + " map=" + std::to_string(run.mapId) + " phase=" + PhaseName(run.phase)
                    + " attempt=" + std::to_string(run.attempt) + "/" + std::to_string(run.maxAttempts)
                    + " seed=" + std::to_string(run.baseSeed)
                    + " policy=" + (run.policy ? "continue" : "reset")
                    + " best=" + std::to_string(run.bestCleared) + "/" + std::to_string(run.routeWaves)
                    + " deaths=" + std::to_string(run.deaths)
                    + " inst=" + std::to_string(run.instanceId) + " bots=" + names);
                if (!run.attemptSummary.empty())
                    messages.push_back("  attempts: " + run.attemptSummary);
            }
            for (auto const& r : s_reports)
                messages.push_back("history run=" + std::to_string(r.runId) + " g=" + std::to_string(r.index)
                    + " map=" + std::to_string(r.mapId) + (r.ok ? " ok" : " FAILED")
                    + " inDungeon=" + std::to_string(r.inDungeonMs / 1000) + "s"
                    + (r.error.empty() ? "" : (" error=" + r.error)));

            if (args.empty())
                messages.push_back("用法: .bot sim start <mapId> [groups=1] [partySize=5] [policy=reset|continue] [attempts=3] [seed=N] [stuck=<秒>] [maxtime=<秒>] [boss=0|1] | stop [runId|all] | status | prepare <n>");
            return messages;
        }

        // [2026-10-26 站长口径] **DPS 仇恨门豁免**的游戏内管理命令（写库 + 原地刷新内存）：
        //   .rndbot sim dpsx add            → 把**你当前选中的怪**加进当前地图的豁免表
        //   .rndbot sim dpsx del            → 把当前选中的怪移出
        //   .rndbot sim dpsx add <entry>    → 手动指定 entry（选中不是怪时用）
        //   .rndbot sim dpsx list [map]     → 列出（map 省略 = 你当前地图）
        //   命中表里的 (map, entry)：DPS 不必等坦克仇恨即可开打（Protean Spawn 这类必须尽快杀的怪）。
        if (!args.empty() && args[0] == "dpsx")
        {
            if (args.size() < 2)
            {
                messages.push_back("用法：.rndbot sim dpsx add|del [entry]（省略 entry = 当前选中的怪）| list [map]");
                return messages;
            }

            if (args[1] == "list")
            {
                uint32 mapId = (args.size() >= 3) ? uint32(atoi(args[2].c_str()))
                                                  : (master ? master->GetMapId() : 0);
                uint32 cnt = 0;
                if (auto qr = mapId
                        ? WorldDatabase.PQuery("SELECT `map`,`entry`,`note` FROM ai_playerbot_dps_exempt "
                                               "WHERE `map`=%u ORDER BY `entry`", mapId)
                        : WorldDatabase.PQuery("SELECT `map`,`entry`,`note` FROM ai_playerbot_dps_exempt "
                                               "ORDER BY `map`,`entry`"))
                {
                    do
                    {
                        Field* f = qr->Fetch();
                        messages.push_back("dpsx map=" + std::to_string(f[0].GetUInt32())
                            + " entry=" + std::to_string(f[1].GetUInt32())
                            + " " + f[2].GetCppString());
                        ++cnt;
                    } while (qr->NextRow());
                }
                if (!cnt)
                    messages.push_back("dpsx: 空" + (mapId ? ("（map=" + std::to_string(mapId) + "）") : ""));
                return messages;
            }

            // map：默认 = 发起命令的人所在地图
            uint32 const mapId = master ? master->GetMapId() : 0;

            // entry：显式给了就用；否则取"当前选中的目标"
            uint32 entry = 0;
            if (args.size() >= 3)
                entry = uint32(atoi(args[2].c_str()));
            if (!entry && master)
            {
                if (Unit* sel = master->GetMap()->GetUnit(master->GetSelectionGuid()))
                    if (sel->GetTypeId() == TYPEID_UNIT)
                        entry = sel->GetEntry();
            }
            if (!mapId || !entry)
            {
                messages.push_back("dpsx: 取不到目标——先用鼠标选中一只怪，或写成 .rndbot sim dpsx add <entry>");
                return messages;
            }

            if (args[1] == "add")
            {
                WorldDatabase.DirectPExecute("REPLACE INTO ai_playerbot_dps_exempt (`map`,`entry`,`note`) "
                    "VALUES (%u,%u,'%s 由 GM 命令添加')", mapId, entry, master ? master->GetName() : "GM");
                dpsexempt::Reload();
                messages.push_back("dpsx: 已加入 map=" + std::to_string(mapId) + " entry=" + std::to_string(entry)
                    + "（DPS 不必等坦克仇恨即可开打）");
            }
            else if (args[1] == "del")
            {
                WorldDatabase.DirectPExecute("DELETE FROM ai_playerbot_dps_exempt WHERE `map`=%u AND `entry`=%u",
                    mapId, entry);
                dpsexempt::Reload();
                messages.push_back("dpsx: 已移除 map=" + std::to_string(mapId) + " entry=" + std::to_string(entry));
            }
            else
                messages.push_back("用法：.rndbot sim dpsx add|del [entry]（省略 entry = 当前选中的怪）| list [map]");
            return messages;
        }

        // [2026-10-26 站长口径] 卡视野点库：离线把"贴墙 + 本图怪看不见"的点批量算好写库，
        //   战斗时只做"取最近点 + 现场复核"，不再现场盲扫。
        if (args[0] == "spots")
        {
            if (args.size() < 2)
            {
                messages.push_back("用法：.rndbot sim spots <mapId|all>");
                return messages;
            }
            std::vector<uint32> maps;
            if (args[1] == "all")
            {
                if (auto qr = WorldDatabase.Query(
                        "SELECT DISTINCT `map` FROM ai_playerbot_dungeon_route WHERE `verified`=1 ORDER BY `map`"))
                {
                    do
                        maps.push_back(qr->Fetch()[0].GetUInt32());
                    while (qr->NextRow());
                }
            }
            else
                maps.push_back(uint32(atoi(args[1].c_str())));

            for (uint32 m : maps)
            {
                uint32 scanned = 0, kept = 0;
                std::string msg;
                coverspot::ScanMap(m, scanned, kept, msg);
                messages.push_back("spots map=" + std::to_string(m) + " " + msg);
                sLog.outError("[COVERSPOT] map %u %s", m, msg.c_str());
            }
            return messages;
        }

        if (args[0] == "profile")
        {
            messages.push_back(BuildBotProfiles());
            return messages;
        }

        if (args[0] == "stop")
        {
            if (s_runs.empty())
            {
                messages.push_back("仿真：没有正在跑的组。");
                return messages;
            }

            if (args.size() > 1 && args[1] != "all")
            {
                uint32 runId = uint32(atoi(args[1].c_str()));
                for (auto it = s_runs.begin(); it != s_runs.end();)
                {
                    if (it->runId != runId)
                    {
                        ++it;
                        continue;
                    }
                    CloseRun(*it, false, "命令停止");
                    messages.push_back("run=" + std::to_string(runId) + " g=" + std::to_string(it->index) + " 已停止");
                    it = s_runs.erase(it);
                }
                RestoreRouteSwitchIfIdle();
                return messages;
            }

            CloseAll("命令停止", &messages);
            return messages;
        }

        if (args[0] == "prepare")
        {
            uint32 count = args.size() > 1 ? uint32(atoi(args[1].c_str())) : 20;
            if (count < 1) count = 1;
            if (count > 1000) count = 1000;   // [站长 2026-10-20] 每批 1000 个空壳
            StartPrepare(count, messages);
            return messages;
        }

        if (args[0] == "dummy")
        {
            // .bot sim dummy <spec> [targets=1|5] [seconds=120]
            if (args.size() < 2)
            {
                messages.push_back("用法：.bot sim dummy <spec> [targets=1] [seconds=120]；"
                    "spec = fire/frost/arcane/arms/fury/combat/assassination/bm/marks/affliction/destro/enhance/elemental/feral/balance/ret/shadow");
                return messages;
            }
            uint32 targets = args.size() > 2 ? uint32(atoi(args[2].c_str())) : 1;
            uint32 seconds = args.size() > 3 ? uint32(atoi(args[3].c_str())) : 120;
            uint32 pinBot = args.size() > 4 ? uint32(atoi(args[4].c_str())) : 0;
            bool useFsm = !(args.size() > 5 && atoi(args[5].c_str()) == 0);
            bool respec = !(args.size() > 6 && atoi(args[6].c_str()) == 0);   // 第 7 个参数传 0 = 不重天赋
            StartDummy(messages, args[1], targets, seconds, pinBot, useFsm, respec);
            return messages;
        }

        if (args[0] == "arms")
        {
            if (args.size() < 4)
            {
                messages.push_back("用法：.rndbot sim arms <targets> <seconds> <spec:key=value|spec:@> [...]");
                return messages;
            }
            uint32 targets = uint32(atoi(args[1].c_str()));
            uint32 seconds = uint32(atoi(args[2].c_str()));
            std::vector<std::string> arms(args.begin() + 3, args.end());
            StartArms(messages, arms, targets, seconds);
            return messages;
        }

        if (args[0] == "dummyall")
        {
            uint32 targets = args.size() > 1 ? uint32(atoi(args[1].c_str())) : 1;
            uint32 seconds = args.size() > 2 ? uint32(atoi(args[2].c_str())) : 60;
            StartDummyAll(messages, targets, seconds);
            return messages;
        }

        if (args[0] == "dummy-stop")
        {
            for (auto& d : s_dummies)
            {
                Player* bot = FindPlayerAny(d.guid);
                ReportDummy(d, bot, "stopped");
            }
            s_dummies.clear();
            messages.push_back("假人场景已全部停止");
            return messages;
        }

        if (args[0] != "start")
        {
            messages.push_back("用法: .bot sim start <mapId> [groups=1] [partySize=5] [policy=reset|continue] [attempts=3] [seed=N] [stuck=<秒>] [maxtime=<秒>] [boss=0|1] | stop [runId|all] | status | prepare <n>");
            return messages;
        }

        uint32 mapId = args.size() > 1 ? uint32(atoi(args[1].c_str())) : 0;
        uint32 groups = args.size() > 2 ? uint32(atoi(args[2].c_str())) : 1;
        uint32 partySize = args.size() > 3 ? uint32(atoi(args[3].c_str())) : 5;

        // [2026-10-18 实验参数] policy：灭团后怎么处理；attempts：reset 模式最多几次；seed：阵容/复现种子
        uint8 policy = 0;                 // 0 = reset（灭团→重置重跑） / 1 = continue（死即复活续推）
        uint32 maxAttempts = 3;
        uint32 baseSeed = uint32(time(nullptr));
        uint32 stuckMs = STUCK_MS;        // 卡死阈值（测试时可 stuck=<秒> 覆盖）
        uint32 maxRunMs = ATTEMPT_TIMEOUT_MS; // 单轮硬性时间上限（maxtime=<秒>，默认 45 分钟）
        bool bossFirst = false;           // [2026-10-22] boss 优先模式（boss=1 打开）
        for (size_t i = 4; i < args.size(); ++i)
        {
            std::string const& a = args[i];
            if (a.rfind("policy=", 0) == 0)
            {
                std::string v = a.substr(7);
                policy = (v == "continue" || v == "1") ? 1 : 0;
            }
            else if (a.rfind("attempts=", 0) == 0)
                maxAttempts = uint32(atoi(a.substr(9).c_str()));
            else if (a.rfind("seed=", 0) == 0)
                baseSeed = uint32(strtoul(a.substr(5).c_str(), nullptr, 10));
            else if (a.rfind("stuck=", 0) == 0)
                stuckMs = uint32(atoi(a.substr(6).c_str())) * 1000;
            else if (a.rfind("maxtime=", 0) == 0)
                maxRunMs = uint32(atoi(a.substr(8).c_str())) * 1000;
            else if (a.rfind("boss=", 0) == 0)
                bossFirst = (atoi(a.substr(5).c_str()) != 0);
        }
        // [2026-10-22] boss 优先默认关：只有显式 boss=1 才开（同一二进制做 A/B）
        dungeonroute::BossFirstSwitch() = bossFirst;
        // attempts=0 ⇒ 一直跑（灭团/卡死就重置再开，直到手动 stop）
        if (maxAttempts > 20) maxAttempts = 20;
        if (!baseSeed) baseSeed = uint32(time(nullptr));

        if (!mapId)
        {
            messages.push_back("用法: .bot sim start <mapId> [groups=1] [partySize=5] [policy=reset|continue] [attempts=3] [seed=N] [stuck=<秒>] [maxtime=<秒>] [boss=0|1]");
            return messages;
        }
        if (groups < 1) groups = 1;
        if (groups > 8) groups = 8;   // [2026-10-24] 上限 5 → 8（站长要求 7 组）
        if (partySize < 1) partySize = 1;
        if (partySize > 5) partySize = 5;

        dungeonrun::EnsureEntrances();
        auto it = dungeonrun::Entrances().find(mapId);
        if (it == dungeonrun::Entrances().end() || !it->second.has)
        {
            messages.push_back("没有 map " + std::to_string(mapId) + " 的副本入口数据（areatrigger_teleport）。");
            return messages;
        }

        dungeonrun::Entrance const entrance = it->second;

        // 候选池里排除已在跑的组合
        std::vector<uint32> excluded;
        for (auto const& run : s_runs)
            for (uint32 g : run.guids)
                excluded.push_back(g);

        uint32 minLevel = entrance.requiredLevel ? entrance.requiredLevel : 1;
        // [2026-10-25 修复] 同一批 bot 不能被分给多张图：
        //   `used` 原来只在**这一次 sim start** 内生效，多个地图各自 start 时会挑到同一批账号，
        //   结果后登录的组把前一个踢下线/自己超时（实测 546/547/552/553/555/556 同一批名字全部登录超时）。
        //   这里把"已经在跑的 run 里的 guid"从候选池排除，并预置进 used。
        std::set<uint32> used;
        {
            std::vector<uint32> busy;
            for (auto const& r0 : s_runs)
                for (uint32 g0 : r0.guids)
                {
                    used.insert(g0);
                    busy.push_back(g0);
                }
            if (!busy.empty())
            {
                std::vector<uint32> excludedPlus = excluded;
                excludedPlus.insert(excludedPlus.end(), busy.begin(), busy.end());
                excluded.swap(excludedPlus);
            }
        }

        std::vector<Candidate> pool = LoadCandidates(minLevel, excluded);
        if (pool.empty())
        {
            messages.push_back("候选池为空：没有满足条件的随机 bot 角色（level>=" + std::to_string(minLevel) + "）。");
            return messages;
        }

        if (!s_routeOverridden)
        {
            s_savedRouteSwitch = dungeonroute::RouteRuntimeSwitch();
            dungeonroute::RouteRuntimeSwitch() = 1;      // 允许未 verified 的图 + 自动进本联动
            s_routeOverridden = true;
        }

        if (!s_activityPrioritiesOverridden)
        {
            s_savedDisableActivityPriorities = sPlayerbotAIConfig.disableActivityPriorities;
            sPlayerbotAIConfig.disableActivityPriorities = true;
            s_activityPrioritiesOverridden = true;
        }

        uint32 runId = ++s_runSeq;
        uint32 started = 0;

        for (uint32 g = 0; g < groups; ++g)
        {
            GroupRun run;
            run.runId = runId;
            run.index = g;
            run.mapId = mapId;
            run.partySize = partySize;
            run.minLevel = minLevel;
            run.baseSeed = baseSeed;
            run.attempt = 1;
            run.maxAttempts = maxAttempts;
            run.stuckMs = stuckMs;
            run.maxRunMs = maxRunMs;
            run.policy = policy;
            run.ex = entrance.x;
            run.ey = entrance.y;
            run.ez = entrance.z;
            run.eo = entrance.o;
            run.startedAt = Now();
            run.phaseSince = run.startedAt;

            std::string error;
            std::vector<uint32> party;

            // [2026-10-18 站长口径] "只用正确的坦克和治疗"：先多登几个候选（+3），
            //   登录后按真实专精挑坦/奶，选不到就明确失败（见 SelectRealRoles）。
            uint32 const poolSize = partySize + 3;
            uint32 const pickSeed = baseSeed + g * 7919u + run.attempt * 104729u;
            if (!PickParty(pool, poolSize, used, party, error, pickSeed))
            {
                messages.push_back("组 " + std::to_string(g) + " 建不起来：" + error);
                continue;
            }

            run.guids = party;
            run.nextLoginTryAt.assign(party.size(), 0);
            s_runs.push_back(run);
            ++started;

            std::string names;
            FormatRunGuids(s_runs.back(), names);
            sLog.outError("[SIM] start run=%u g=%u map=%u party=%u candidates=%u seed=%u attempt=1/%u policy=%s names=%s by=%s",
                runId, g, mapId, partySize, (uint32)party.size(), baseSeed, maxAttempts,
                policy ? "continue" : "reset", names.c_str(), who);
        }

        if (!started)
        {
            if (s_runs.empty())
                RestoreRouteSwitchIfIdle();
            messages.push_back("没有组起来（候选池不够？）。");
            return messages;
        }

        messages.push_back("仿真已启动 run=" + std::to_string(runId) + " groups=" + std::to_string(started)
            + " party=" + std::to_string(partySize) + " map=" + std::to_string(mapId));
        messages.push_back("看日志: grep '\\[SIM\\]' 或 .bot sim status");
        return messages;
    }

    // -----------------------------------------------------------------------
    void Update()
    {
        TickPrepare();
        TickDummies(Now());

        if (s_runs.empty())
            return;

        uint32 now = Now();
        for (auto it = s_runs.begin(); it != s_runs.end();)
        {
            if (!TickRun(*it, now))
                it = s_runs.erase(it);
            else
                ++it;
        }

        RestoreRouteSwitchIfIdle();
    }

    void Shutdown()
    {
        if (s_runs.empty())
            return;
        CloseAll("进程退出", nullptr);
    }

    bool Running()
    {
        return !s_runs.empty();
    }
}
