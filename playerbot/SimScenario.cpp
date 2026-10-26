#include "playerbot/playerbot.h"
#include "playerbot/SimScenario.h"
#include "playerbot/AiFactory.h"
#include "playerbot/PlayerbotFactory.h"
#include "playerbot/DungeonRoute.h"
#include "playerbot/DpsExempt.h"
#include "playerbot/CoverSpot.h"   // 卡视野点库（离线预筛 + 运行期取点）
#include "MotionGenerators/PathFinder.h"
#include "playerbot/DungeonRun.h"
#include "Combat/CombatLogRecorder.h"
#include "MotionGenerators/MoveMap.h"   // [2026-10-25] 击飞/掉出网格兜底：用导航网格判"人在不在图上"

#include <set>
#include <fstream>
#include <random>

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
                                    p->TeleportTo(run.mapId, run.ex, run.ey, run.ez, run.eo);
                                    rescued = true;
                                }
                                if (rescued)
                                    sLog.outError("[SIM] run=%u g=%u 60 秒内死亡 %u 次 ⇒ 整组撤回副本入口（解除复活死循环）",
                                        run.runId, run.index, delta);
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
                                float rx = run.ex, ry = run.ey, rz = run.ez, ro = run.eo;
                                uint32 rmap = run.mapId;
                                Player* alive = nullptr;
                                for (uint32 ag : run.guids)
                                {
                                    Player* ap = FindPlayerInWorld(ag);
                                    if (ap && ap != p && ap->IsAlive() && ap->GetMapId() == run.mapId)
                                    {
                                        alive = ap;
                                        break;
                                    }
                                }
                                if (alive)
                                {
                                    float ax = alive->GetPositionX() + float(urand(0, 3)) - 1.5f;
                                    float ay = alive->GetPositionY() + float(urand(0, 3)) - 1.5f;
                                    float az = alive->GetPositionZ() + 0.5f;
                                    if (OobOnNavMesh(alive->GetMap(), ax, ay, az))
                                    {
                                        rx = ax;
                                        ry = ay;
                                        rz = az;
                                        ro = alive->GetOrientation();
                                        rmap = alive->GetMapId();
                                    }
                                }
                                p->TeleportTo(rmap, rx, ry, rz, ro);
                                if (!classList.empty()) classList += ",";
                                classList += std::to_string(uint32(p->getClass()));
                                ++revived;
                            }

                            if (revived)
                                sLog.outError("[SIM] run=%u g=%u %s %u 人 → 回入口安全复活（deaths=%u）classes=%s",
                                    run.runId, run.index, alive == 0 ? "灭团" : "阵亡", revived, run.deaths,
                                    classList.c_str());
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
                                // continue：死即复活，拉回入口继续推（不终止）
                                uint32 const revived = ReviveParty(run, true);
                                sLog.outError("[SIM] run=%u g=%u attempt=%u WIPE→continue 复活 %u 人（deaths=%u）",
                                    run.runId, run.index, run.attempt, revived, run.deaths);
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
