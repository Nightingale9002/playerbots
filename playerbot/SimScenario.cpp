#include "playerbot/playerbot.h"
#include "playerbot/SimScenario.h"
#include "playerbot/AiFactory.h"
#include "playerbot/PlayerbotFactory.h"
#include "playerbot/DungeonRoute.h"
#include "playerbot/DungeonRun.h"
#include "Combat/CombatLogRecorder.h"

#include <set>
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
            uint32 leaderTryAt = 0;
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
            uint32 noVictimSince = 0;    // 坦克"在战斗但没目标"的起始时间（0=有目标/不在战斗）
            uint32 deaths = 0;           // 本 attempt 死亡次数累计（复活后不清零）
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
        std::vector<Candidate> LoadCandidates(uint32 minLevel, std::vector<uint32> const& excluded)
        {
            std::vector<Candidate> out;

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

            // 队长（坦克候选）：优先坦克职业
            Candidate tank;
            bool foundTank = false;
            for (auto const& c : free)
            {
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
            takeFirst(&IsTankClass);
            takeFirst(&IsTankClass);
            takeFirst(&IsHealClass);
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
                    sLog.outString("[SIM] run=%u g=%u 入队失败 member=%s", run.runId, run.index, member->GetName());
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

                bool const isTank = PlayerbotAI::IsTank(p);
                bool const isHeal = PlayerbotAI::IsHeal(p);
                poolDesc += " " + std::string(p->GetName()) + "(" + ClassName(p->getClass())
                          + (isTank ? ",tank" : (isHeal ? ",heal" : ",dps")) + ")";

                if (!tank && isTank)
                    tank = p;
                if (!heal && isHeal && !isTank)
                    heal = p;
            }

            sLog.outString("[SIM] run=%u g=%u 候选 %u 人：%s", run.runId, run.index,
                (uint32)run.guids.size(), poolDesc.c_str());

            if (!tank || !heal)
            {
                error = std::string("没有真正的") + (tank ? "" : "坦克")
                      + (!tank && !heal ? "/" : "") + (heal ? "" : "治疗")
                      + "（候选：" + poolDesc + "）";
                return false;
            }

            // 队伍 = 1 真坦 + 1 真奶 + 其余**真 DPS**（同阵营已由 PickParty 保证）。
            //   [2026-10-18] 不能让第二个真坦进队：DungeonRoute 的驱动坦克是"队里 guid 最小的坦"，
            //   多坦会让实际驱动坦克 ≠ run.tankGuid（监控/治疗对象全错），所以 DPS 位优先挑非坦非奶。
            std::vector<uint32> party;
            party.push_back(tank->GetGUIDLow());
            if (heal->GetGUIDLow() != tank->GetGUIDLow())
                party.push_back(heal->GetGUIDLow());

            // ① DPS 位：既不是真坦也不是真奶
            for (uint32 guid : run.guids)
            {
                if (party.size() >= partySize)
                    break;
                if (std::find(party.begin(), party.end(), guid) != party.end())
                    continue;

                Player* p = FindPlayerInWorld(guid);
                if (!p)
                    continue;
                if (PlayerbotAI::IsTank(p) || PlayerbotAI::IsHeal(p))
                    continue;

                party.push_back(guid);
            }

            // ② 不够再放开（极端情况：候选里全是坦/奶）
            for (uint32 guid : run.guids)
            {
                if (party.size() >= partySize)
                    break;
                if (std::find(party.begin(), party.end(), guid) != party.end())
                    continue;
                if (FindPlayerInWorld(guid))
                    party.push_back(guid);
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

            sLog.outString("[SIM] run=%u g=%u 用真坦/真奶组队：tank=%s heal=%s 其余=%u 人；候选未用下线 %u",
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
                sLog.outString("[SIM] run=%u g=%u WARN 坦克 %s 不再带 tank 策略（专精/策略被重置？）",
                    run.runId, run.index, tank->GetName());

            Player* heal = run.healGuid ? FindPlayerInWorld(run.healGuid) : nullptr;
            if (heal && !PlayerbotAI::IsHeal(heal))
                sLog.outString("[SIM] run=%u g=%u WARN 治疗 %s 不再带 heal 策略（专精/策略被重置？）",
                    run.runId, run.index, heal->GetName());
        }

        // `[SIM] diag`：为什么拉怪/跑位没动？把驱动判据和 route 数据一次性打出来（只在 RUN 阶段）。
        void LogDiagnostics(GroupRun& run)
        {
            dungeonroute::MapRoute const* route = dungeonroute::GetMapRoute(run.mapId);
            sLog.outString("[SIM] run=%u g=%u diag route loaded=%d verified=%d waves=%u guidOrd=%u bosses=%u routeSwitch=%d routeEnabled=%d",
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
                    sLog.outString("[SIM] run=%u g=%u diag bot=%u offline/no-ai", run.runId, run.index, guid);
                    continue;
                }

                sLog.outString("[SIM] run=%u g=%u diag bot=%s cls=%u tank=%d heal=%d tankStrat=%d dungeonRoute=%d drive=%d leader=%d allowAct=%d map=%u instance=%u alive=%d combat=%d master=%s",
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
                sLog.outString("[SIM] run=%u g=%u probe tank=%u ai=%d route=%d", run.runId, run.index,
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

            sLog.outString("[SIM] run=%u g=%u probe tank=%s pos=(%.1f,%.1f,%.1f) map=%u ord=%u find=%d tg=%u entry=%u boss=%d hasPull=%d dist=%.1f pull=(%.1f,%.1f,%.1f) act=%s pullStrat=%d",
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
                sLog.outString("[SIM] run=%u g=%u probe wave ord=%u alive=%u/%u anchor=%d",
                    run.runId, run.index, (uint32)kv.first, alive, (uint32)kv.second.members.size(),
                    kv.second.pull.has ? 1 : 0);
            }

            uint32 bossAlive = 0;
            for (auto const& b : route->bosses)
                if (dungeonroute::MemberCreature(map, dungeonroute::Member{ b.guid, b.entry }))
                    ++bossAlive;
            sLog.outString("[SIM] run=%u g=%u probe bosses alive=%u/%u", run.runId, run.index,
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
            sLog.outString("[SIM] prepare start count=%u", (uint32)guids.size());
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

                CharacterDatabase.PExecute(
                    "INSERT INTO ai_playerbot_random_bots (owner,bot,time,validIn,event,value) "
                    "VALUES (0,'%u',UNIX_TIMESTAMP(),0,'prepared',1)", guid);

                sLog.outString("[SIM] prepare ok guid=%u %s lv=%u cls=%u", guid, bot->GetName(),
                    bot->GetLevel(), (uint32)bot->getClass());

                sRandomPlayerbotMgr.LogoutPlayerBot(guid);
                ++s_prepare.done;
            }

            if (s_prepare.done >= s_prepare.guids.size() || now - s_prepare.startedAt > 900000)
            {
                sLog.outString("[SIM] prepare finish %u/%u", (uint32)s_prepare.done, (uint32)s_prepare.guids.size());
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
            sLog.outString("[SIM] run=%u g=%u %s map=%u inst=%u total=%.1fs indungeon=%.1fs bots=%s%s%s",
                run.runId, run.index, ok ? "done" : "FAILED", run.mapId, run.instanceId,
                report.totalMs / 1000.0f, report.inDungeonMs / 1000.0f, names.c_str(),
                error.empty() ? "" : " error=", error.c_str());

            // 整个 run 的 attempt 汇总（灭团即重置模式下这是最重要的产出）
            if (run.attemptsDone || !run.attemptSummary.empty())
            {
                sLog.outString("[SIM] run=%u g=%u attempts=%u best=%u/%u seed=%u policy=%s %s",
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
                sLog.outString("[SIM] 路线开关恢复为 %d", s_savedRouteSwitch);
            }

            if (s_activityPrioritiesOverridden)
            {
                sPlayerbotAIConfig.disableActivityPriorities = s_savedDisableActivityPriorities;
                s_activityPrioritiesOverridden = false;
                sLog.outString("[SIM] 机器人活跃度优先级恢复为 %d", s_savedDisableActivityPriorities ? 1 : 0);
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

            sLog.outString("[SIM] run=%u g=%u %s seed=%u policy=%s",
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
                            sLog.outString("[SIM] run=%u g=%u 账号缺失，剔除候选 guid=%u",
                                run.runId, run.index, run.guids[i]);
                            run.guids.erase(run.guids.begin() + i);
                            run.nextLoginTryAt.erase(run.nextLoginTryAt.begin() + i);
                            return true;
                        }

                        sRandomPlayerbotMgr.AddPlayerBot(run.guids[i], accountId);
                        nextTry = now + 15000;                 // 15s 内不重复发起
                    }

                    if (online == run.guids.size())
                    {
                        run.phase = PHASE_FORM;
                        run.phaseSince = now;
                        sLog.outString("[SIM] run=%u g=%u online %u/%u in %.1fs", run.runId, run.index,
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
                        sLog.outString("[SIM] run=%u g=%u 角色不足（%s）⇒ 换人重选第 %u 次",
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

                    sLog.outString("[SIM] run=%u g=%u group id=%u lead=%s members=%u tank=%s heal=%s",
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
                    if (!run.leaderTeleported)
                    {
                        if (leader->GetMapId() == run.mapId && !leader->IsBeingTeleported())
                        {
                            run.leaderTeleported = true;
                            run.phaseSince = now;
                            sLog.outString("[SIM] run=%u g=%u leader %s 已进本 map=%u inst=%u",
                                run.runId, run.index, leader->GetName(), run.mapId, leader->GetInstanceId());
                            return true;
                        }

                        if (now - run.phaseSince > 60000)
                        {
                            CloseRun(run, false, "队长进本超时");
                            return false;
                        }

                        if (leader->IsBeingTeleported())
                            return true;

                        if (now - run.leaderTryAt < 3000)
                            return true;

                        run.leaderTryAt = now ? now : 1;
                        leader->TeleportTo(run.mapId, run.ex, run.ey, run.ez, run.eo);
                        sLog.outString("[SIM] run=%u g=%u teleport leader %s -> map=%u (%.1f,%.1f,%.1f)",
                            run.runId, run.index, leader->GetName(), run.mapId, run.ex, run.ey, run.ez);
                        return true;
                    }

                    // ② 其余成员再进本（此时队伍已经绑定 instance）
                    //   [2026-10-19 修] 必须**逐个**传、等落地再传下一个：同一 tick 批量传会各自创建
                    //   一个 instance（实测 5 人分到 inst 11/12/13，导致只有 3 人一起打、清波统计为 0）。
                    if (!run.membersTeleported)
                    {
                        bool pending = false;
                        for (size_t i = 1; i < run.guids.size(); ++i)
                        {
                            Player* p = FindPlayerInWorld(run.guids[i]);
                            if (!p)
                                continue;
                            if (p->GetMapId() == run.mapId && !p->IsBeingTeleported())
                                continue;                       // 已落地

                            pending = true;
                            if (p->IsBeingTeleported())
                                return true;                    // 等它落地

                            p->TeleportTo(run.mapId, run.ex, run.ey, run.ez, run.eo);
                            sLog.outString("[SIM] run=%u g=%u teleport member %s（逐个传送）",
                                run.runId, run.index, p->GetName());
                            return true;
                        }

                        if (pending)
                            return true;

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
                                    sLog.outString("[SIM] run=%u g=%u 修正 instance：%s 在 inst=%u（队长 inst=%u）⇒ 送回主城",
                                        run.runId, run.index, p->GetName(), p->GetInstanceId(), leadInst);
                                }
                                else
                                {
                                    p->TeleportTo(run.mapId, leader->GetPositionX(), leader->GetPositionY(),
                                                  leader->GetPositionZ(), leader->GetOrientation());
                                    sLog.outString("[SIM] run=%u g=%u 修正 instance：%s 传回队长身边（inst=%u）",
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
                        sLog.outString("[SIM] run=%u g=%u IN DUNGEON map=%u inst=%u members=%u attempt=%u/%u t=%.1fs",
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
                    if (route && tank)
                    {
                        Map* tmap = tank->GetMap();
                        for (auto& kv : route->waves)
                        {
                            if (kv.second.allNonHostile)
                                continue;
                            ++realWaves;

                            bool anyAlive = false;
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
                            if (anyAlive)
                                run.seenWaves.insert(kv.first);
                            else if (run.seenWaves.count(kv.first))
                                ++cleared;
                        }
                        run.routeWaves = realWaves;

                        // [2026-10-19] 卡死判定不能只看"清整波"：水里的 1 只可打、其余不可达的波永远清不掉，
                        //   但每杀一只就是推进 ⇒ 用小怪存活数下降作为推进信号。
                        if (run.lastAliveMobs && aliveMobs < run.lastAliveMobs)
                            run.lastProgressAt = now;
                        run.lastAliveMobs = aliveMobs;
                    }

                    uint32 const dead = online - alive;
                    if (dead > run.lastDead)
                        run.deaths += dead - run.lastDead;
                    run.lastDead = dead;

                    // 只有"清掉一波"才算进展：单纯在战斗里磨不算（否则永远判不出卡死）。
                    if (cleared != run.lastCleared)
                    {
                        sLog.outString("[SIM] run=%u g=%u progress cleared=%u→%u/%u skipped=%u",
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
                    if (inCombat && tank && route && !run.combatSkipDone)
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
                            }
                            else if (run.lastVictimAt && now - run.lastVictimAt >= 15000)
                            {
                                dungeonroute::Target t;
                                uint16 const ord = (dungeonroute::FindTargetNearest(tank, *route, t) && t.ord)
                                    ? t.ord : dungeonroute::CurrentOrd(tank, *route);
                                if (ord && tank->GetPlayerbotAI())
                                {
                                    dungeonroute::MarkSkipped(tank, ord, "no damage 15s");
                                    run.combatSkipDone = true;
                                    ForceDisengage(run);
                                    sLog.outString("[SIM] run=%u g=%u attempt=%u 目标 %s 15s 无伤害 ⇒ 跳过波 ord=%u，强制脱战继续下一波",
                                        run.runId, run.index, run.attempt, victim->GetName(), (uint32)ord);
                                }
                                run.lastVictimAt = now;
                                run.lastVictim = ObjectGuid();
                            }
                        }
                        else
                        {
                            // [2026-10-18] "在战斗但坦克没有目标"：怪已 aggro 却打不到/在 evade
                            //   （例如水里/另一层的 Bogstrok）⇒ 15 秒就跳过这波。
                            //   但先给坦克 5 秒"自救"：主动 attack anything 重新锁定（很多是目标丢了没重选）。
                            if (!run.noVictimSince)
                                run.noVictimSince = now;
                            else if (now - run.noVictimSince >= 5000 && now - run.noVictimSince < 15000
                                     && tank->GetPlayerbotAI())
                            {
                                tank->GetPlayerbotAI()->DoSpecificAction("attack anything", Event(), true);
                            }
                            else if (now - run.noVictimSince >= 15000 && !run.combatSkipDone && tank->GetPlayerbotAI())
                            {
                                // [2026-10-19 543 实测] 先看是不是“引到了路线外/跳过批次的怪”：
                                //   这类怪不在 route->guidOrd 里 ⇒ 目标过滤会把它剔掉 ⇒ 表现为"战斗中无目标"。
                                //   修法：只要坦克身上还有活着的攻击者，先强迫 attack anything 清掉，不跳波。
                                bool hasLiveAttacker = false;
                                for (Unit* attacker : tank->getAttackers())
                                {
                                    if (attacker && attacker->IsAlive())
                                    {
                                        hasLiveAttacker = true;
                                        break;
                                    }
                                }
                                if (hasLiveAttacker)
                                {
                                    tank->GetPlayerbotAI()->DoSpecificAction("attack anything", Event(), true);
                                    run.noVictimSince = now;
                                    sLog.outString("[SIM] run=%u g=%u attempt=%u 坦克战斗中无目标、但有攻击者 ⇒ 先清引到的怪，不跳波",
                                        run.runId, run.index, run.attempt);
                                    run.lastVictim = ObjectGuid();
                                    run.lastVictimAt = 0;
                                }
                                else
                                {
                                    dungeonroute::Target t;
                                    uint16 const ord = (dungeonroute::FindTargetNearest(tank, *route, t) && t.ord)
                                        ? t.ord : dungeonroute::CurrentOrd(tank, *route);
                                    if (ord)
                                    {
                                        dungeonroute::MarkSkipped(tank, ord, "no victim 15s");
                                        run.combatSkipDone = true;
                                        ForceDisengage(run);
                                        sLog.outString("[SIM] run=%u g=%u attempt=%u 坦克战斗中 15s 无目标 ⇒ 跳过波 ord=%u，强制脱战继续下一波",
                                            run.runId, run.index, run.attempt, (uint32)ord);
                                    }
                                    run.noVictimSince = now;
                                }
                            }
                            run.lastVictim = ObjectGuid();
                            run.lastVictimAt = 0;
                        }
                    }
                    else if (!inCombat)
                    {
                        run.combatSkipDone = false;
                        run.noVictimSince = 0;
                        run.lastVictim = ObjectGuid();
                        run.lastVictimAt = 0;
                    }

                    // ① 通关：所有波都"已清/被跳过" + boss 要么死了要么确认见过
                    bool clearedAll = false;
                    if (route && st && tank && !route->waves.empty())
                    {
                        bool allWavesDone = true;
                        for (auto const& kv : route->waves)
                        {
                            if (!st->cleared.count(kv.first) && !st->skip.count(kv.first))
                            {
                                allWavesDone = false;
                                break;
                            }
                        }

                        uint32 bossesAlive = 0;
                        uint32 bossesResolved = 0;
                        for (auto const& b : route->bosses)
                        {
                            if (dungeonroute::MemberCreature(tank->GetMap(),
                                dungeonroute::Member{ b.guid, b.entry }))
                                ++bossesAlive;
                            else if (st->bossCleared.count(b.guid) || st->bossSeen.count(b.guid))
                                ++bossesResolved;
                        }

                        clearedAll = allWavesDone && bossesAlive == 0
                                  && bossesResolved >= (uint32)route->bosses.size();
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
                                sLog.outString("[SIM] run=%u g=%u attempt=%u WIPE→continue 复活 %u 人（deaths=%u）",
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
                                sLog.outString("[SIM] run=%u g=%u WIPE → reset：下一轮 attempt=%u/%u seed=%u",
                                    run.runId, run.index, run.attempt, run.maxAttempts, run.baseSeed);
                                return true;
                            }
                        }
                    }
                    else
                        run.wipeSince = 0;

                    // ③ 卡死 / attempt 超时
                    bool const stuck = (now - run.lastProgressAt > run.stuckMs)
                        || (run.attemptStartedAt && now - run.attemptStartedAt > ATTEMPT_TIMEOUT_MS);
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
                        sLog.outString("[SIM] run=%u g=%u STUCK → reset：下一轮 attempt=%u/%u",
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

                            char buf[256];
                            snprintf(buf, sizeof(buf), "%s@(%.0f,%.0f) act=%s tgt=%s:%d%% dist=%.0f",
                                driver->GetName(), driver->GetPositionX(), driver->GetPositionY(),
                                driverAI ? driverAI->GetLastExecutedActionName(driverAI->GetState()).c_str() : "-",
                                victim ? victim->GetName() : "-",
                                (victim && victim->GetMaxHealth())
                                    ? (int)(100.0f * float(victim->GetHealth()) / float(victim->GetMaxHealth())) : 0,
                                dist);
                            driverInfo = buf;
                        }

                        sLog.outString("[SIM] run=%u g=%u status t=%.0fs attempt=%u/%u seed=%u alive=%u/%u deaths=%u combat=%u cleared=%u/%u skipped=%u seen=%u mobs=%u tank=%s inst=%u driver=(%s)",
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
                    run.nextRepairAt = 0;
                    run.instanceId = 0;
                    run.lastCleared = 0;
                    run.diagLeft = 2;
                    run.phase = PHASE_LOGIN;
                    run.phaseSince = now;

                    std::string names;
                    FormatRunGuids(run, names);
                    sLog.outString("[SIM] run=%u g=%u reset done → attempt=%u/%u seed=%u names=%s",
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
                messages.push_back("用法: .bot sim start <mapId> [groups=1] [partySize=5] [policy=reset|continue] [attempts=3] [seed=N] | stop [runId|all] | status | prepare <n>");
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
            if (count > 100) count = 100;
            StartPrepare(count, messages);
            return messages;
        }

        if (args[0] != "start")
        {
            messages.push_back("用法: .bot sim start <mapId> [groups=1] [partySize=5] [policy=reset|continue] [attempts=3] [seed=N] | stop [runId|all] | status | prepare <n>");
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
        }
        // attempts=0 ⇒ 一直跑（灭团/卡死就重置再开，直到手动 stop）
        if (maxAttempts > 20) maxAttempts = 20;
        if (!baseSeed) baseSeed = uint32(time(nullptr));

        if (!mapId)
        {
            messages.push_back("用法: .bot sim start <mapId> [groups=1] [partySize=5] [policy=reset|continue] [attempts=3] [seed=N]");
            return messages;
        }
        if (groups < 1) groups = 1;
        if (groups > 5) groups = 5;
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
        std::set<uint32> used;
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
            sLog.outString("[SIM] start run=%u g=%u map=%u party=%u candidates=%u seed=%u attempt=1/%u policy=%s names=%s by=%s",
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
