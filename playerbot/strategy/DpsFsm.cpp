#include "playerbot/playerbot.h"
#include "playerbot/strategy/DpsFsm.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/strategy/actions/AttackAction.h"
#include "Grids/CellImpl.h"
#include "Maps/Map.h"

#include <cmath>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>

namespace ai
{
    namespace dpsfsm
    {
        char const* StateName(State s)
        {
            switch (s)
            {
                case State::Idle:     return "Idle";
                case State::Approach: return "Approach";
                case State::Burn:     return "Burn";
                case State::Instant:  return "Instant";
                case State::Burst:    return "Burst";
                case State::Aoe:      return "Aoe";
                case State::Weave:    return "Weave";
                case State::Recover:  return "Recover";
            }
            return "?";
        }

        char const* SpecName(Spec s)
        {
            switch (s)
            {
                case Spec::Unknown:       return "unknown";
                case Spec::Fire:          return "fire";
                case Spec::Frost:         return "frost";
                case Spec::Arcane:        return "arcane";
                case Spec::Affliction:    return "affliction";
                case Spec::Demonology:    return "demonology";
                case Spec::Destruction:   return "destruction";
                case Spec::Shadow:        return "shadow";
                case Spec::Discipline:    return "discipline";
                case Spec::Holy:          return "holy";
                case Spec::Balance:       return "balance";
                case Spec::Restoration:   return "restoration";
                case Spec::Elemental:     return "elemental";
                case Spec::Arms:          return "arms";
                case Spec::Fury:          return "fury";
                case Spec::Protection:    return "protection";
                case Spec::Combat:        return "combat";
                case Spec::Assassination: return "assassination";
                case Spec::Subtlety:      return "subtlety";
                case Spec::BeastMastery:  return "beast mastery";
                case Spec::Marksmanship:  return "marksmanship";
                case Spec::Survival:      return "survival";
                case Spec::Enhancement:   return "enhancement";
                case Spec::Feral:         return "feral";
                case Spec::Retribution:   return "retribution";
            }
            return "?";
        }
    }
}

namespace
{
    using ai::dpsfsm::Snapshot;
    using ai::dpsfsm::Spec;
    using ai::dpsfsm::State;

// [2026-10-29] 每个 bot 一份状态机记忆（按 guid）。不同副本线程可能并发，统一加锁。

    struct BotMem;

    Spec DetectSpecByTalentsImpl(PlayerbotAI* ai, BotMem* m);

    struct BotMem
    {
        uint32 guid = 0;
        State state = State::Idle;
        Spec spec = Spec::Unknown;
        time_t specCheckedAt = 0;
        std::map<std::string, uint32> ids;      // 技能名 �?已学会的最�?rank id
        std::set<std::string> unresolved;       // 只报一次的"没有这个技�?
        std::set<std::string> flags;            // 一次性判断结果（如是否有强化灼烧）

        time_t lastTrinketTry = 0;
        time_t sealAt = 0;       // 上一次成功挂圣印的时间（圣印 30 秒，不用每 tick 重挂）
        std::map<uint32, time_t> failUntil;   // 技能 id → 这个时间点之前别再试（施法被拒的退避）
        std::map<uint32, time_t> lastOk;       // 技能 id → 上次成功时间
        uint32 hbLast = 0;                     // 这只 bot 上次打心跳的时间
        bool castThisTick = false;
        bool policyLogged = false;
        bool behindWarned = false;
        bool coverageLogged = false;    // 覆盖率断言只打一次
        bool modeLogged = false;        // [2026-10-31 S1] 决策模式日志只打一次
        int  mode = 0;                  // [2026-10-31 S1] 本 tick 解析出的 DecisionMode（存 int：
                                        //   枚举声明在 BotMem 之后；TickPolicy 直接读它，不再二次读 conf）
        time_t lastBurst = 0;           // [2026-10-31 S2] 上次进入爆发的时间（"早用早 CD"的门）
        uint32 engRuns = 0;
        uint32 dupDebuff = 0;       // [2026-10-30] 同类 debuff 已被更强/相等效果占住时仍施放的次数（互斥分析用）         // 引擎真正跑过的 tick 数（诊断白字缺口用）
        uint32 movTicks = 0;        // 在移动的 tick 数（移动会打断挥击）
        uint32 skipTicks = 0;       // 因为状态机放了技能而让位给状态机的 tick 数      // "需要背后但没站好"只记一次      // [2026-10-30] "策略里点名但没学会"的日志只打一次             // 本 tick 状态机是否真的放出了技能
        time_t lastEngageLog = 0;              // "咬不上去"日志节流
        time_t lastBehindTry = 0;              // 上一次尝试绕背的时间（别每 tick 都动）
        time_t lastCloseTry = 0;               // 上一次"真够不着就贴上去"的时间
        time_t lastConsLog = 0;                // "自身中心 AoE 放不出"日志节流
        time_t lastPotionTry = 0;              // 上一次尝试喝药水的时间
        time_t lastSummary = 0;
        uint32 castCount = 0;
        std::map<std::string, time_t> prereqLast;   // [2026-10-31] 前置动作按名字记上次施放时间
                                                    //   （图腾必须 60 秒内不重铺；靠 spell id 不可靠）
        uint32 stuckSpellId = 0;      // [2026-10-31] 读条看门狗：当前卡住的法术
        time_t stuckSince = 0;        //                       开始卡住的时间

        uint32 tickCount = 0;
        // [2026-10-31 站长口径] 组动作（macro）的执行状态：价值引擎选中"整组"之后，
        //   这里只负责把选中的那组一步步走完。选择仍然只有价值引擎一处。
        int planId = 0;          // 0=无；1=奥法 3+3（3 奥冲 + 寒冰箭填窗 + 卡秒那发）
        int planStep = 0;
    };

    std::mutex s_disabledMutex;
    std::set<uint32> s_disabled;      // A/B 对照：这些 guid 这一轮不走状态机
    std::set<uint32> s_forceDps;      // 假人测试：强制按 DPS 打（忽略坦/奶角色门）
    std::map<uint32, std::set<ObjectGuid>> s_scenarioTargets;   // guid → 这一轮"自己的"假人
    std::map<uint32, std::map<std::string, double>> s_overrides; // guid → 本轮参数覆盖（调优并发用）
    std::map<uint32, uint32> s_scenarioRun;                      // guid → 登记它的 runId

    std::mutex s_memMutex;
    std::unordered_map<uint32, std::unique_ptr<BotMem>> s_mem;

    BotMem* Mem(uint32 guid)
    {
        std::lock_guard<std::mutex> lock(s_memMutex);
        std::unique_ptr<BotMem>& slot = s_mem[guid];
        if (!slot)
        {
            slot.reset(new BotMem());
            slot->guid = guid;
        }
        return slot.get();
    }

    // ---- [2026-10-29 站长口径] 离线调优：参数热加载，脚本改文件即可，不用重编译/重启 ----
    //   文件格式�? 注释）：<scope>:<key>=<value>；scope = 专精名（fire/fury/...）或 * 表示全部
    struct TuneStore
    {
        time_t mtime = 0;
        time_t checkedAt = 0;
        std::map<std::string, double> vals;
    };
    std::mutex s_tuneMutex;
    TuneStore s_tune;

    std::string Trim(std::string s)
    {
        size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return "";
        size_t e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    }

    void ReloadTune()
    {
        time_t const now = time(nullptr);
        if (now - s_tune.checkedAt < 5)
            return;
        s_tune.checkedAt = now;

        std::string const path = sPlayerbotAIConfig.dpsFsmTuneFile.empty()
            ? std::string("dpsfsm_tune.conf") : sPlayerbotAIConfig.dpsFsmTuneFile;

        struct stat st;
        if (stat(path.c_str(), &st) != 0 || st.st_mtime == s_tune.mtime)
            return;
        s_tune.mtime = st.st_mtime;

        std::ifstream in(path.c_str());
        if (!in)
            return;

        std::map<std::string, double> vals;
        std::string line;
        while (std::getline(in, line))
        {
            size_t const hash = line.find('#');
            if (hash != std::string::npos) line = line.substr(0, hash);
            size_t const eq = line.find('=');
            size_t const colon = line.find(':');
            if (eq == std::string::npos || colon == std::string::npos || colon > eq)
                continue;
            std::string const scope = Trim(line.substr(0, colon));
            std::string const key = Trim(line.substr(colon + 1, eq - colon - 1));
            if (scope.empty() || key.empty())
                continue;
            vals[scope + ":" + key] = atof(line.substr(eq + 1).c_str());
        }

        std::lock_guard<std::mutex> lock(s_tuneMutex);
        s_tune.vals.swap(vals);
        sLog.outError("[DPSFSM] 调优参数已加载 %u 项（%s）", (uint32)s_tune.vals.size(), path.c_str());
    }

    double TuneForBot(BotMem* m, char const* key, double def);

    // [2026-10-31 根因] conf 里的参数前缀用的是**短名**（destro/bm/marks/enhance/ret），
    //   而 `SpecName()` 返回的是**长名**（destruction/beast mastery/marksmanship/enhancement/retribution）
    //   ⇒ 这 5 个专精的 conf 参数（包括 `policy=1`）**从来没有被读到过**，一直跑旧的硬编码轮转。
    //   实测证据：毁灭术 HB 里 `pol=0`、casts 里全是 TickWarlock 的献祭/腐蚀/燃烧/痛苦诅咒。
    //   这里做一次别名映射，两个名字都能命中（新配置写短名、老配置写长名都不会踩空）。
    static char const* TuneAlias(char const* spec)
    {
        if (!spec)
            return spec;
        if (!strcmp(spec, "destruction"))   return "destro";
        if (!strcmp(spec, "beast mastery")) return "bm";
        if (!strcmp(spec, "marksmanship"))  return "marks";
        if (!strcmp(spec, "enhancement"))   return "enhance";
        if (!strcmp(spec, "retribution"))   return "ret";
        return spec;
    }

    double Tune(char const* spec, char const* key, double def)
    {
        std::lock_guard<std::mutex> lock(s_tuneMutex);
        auto it = s_tune.vals.find(std::string(spec) + ":" + key);
        if (it != s_tune.vals.end())
            return it->second;
        char const* alias = TuneAlias(spec);
        if (alias && strcmp(alias, spec) != 0)
        {
            it = s_tune.vals.find(std::string(alias) + ":" + key);
            if (it != s_tune.vals.end())
                return it->second;
        }
        it = s_tune.vals.find(std::string("*:") + key);
        if (it != s_tune.vals.end())
            return it->second;
        return def;
    }

    double TuneForBot(BotMem* m, char const* key, double def)
    {
        // 先看"这只 bot 自己的覆盖"（同一轮里不同 bot 可以跑不同参数值）
        {
            std::lock_guard<std::mutex> lock(s_disabledMutex);
            auto it = s_overrides.find(m->guid);
            if (it != s_overrides.end())
            {
                auto kv = it->second.find(key);
                if (kv != it->second.end())
                    return kv->second;
            }
        }
        return Tune(ai::dpsfsm::SpecName(m->spec), key, def);
    }

    double TP(BotMem* m, char const* key, double def)
    {
        return TuneForBot(m, key, def);
    }

    // ==========================================================================
    // [2026-10-31 S1] 决策模式：**一个键取代四个布尔**
    //   旧口径 value / policy / pselect / pauto 是历史叠加出来的：
    //     - pselect 只在 policy=1 时被读；pauto 又只在 pselect=1 时被读
    //       ⇒ 4 个布尔理论 16 态，实际有意义的只有 4 种（另有 value 引擎 1 种）。
    //     - 典型哑弹：pauto=1 但 pselect=0 完全不生效；policy 没开时 pselect/pauto 全废。
    //   新口径只有一个键 `mode`（数值，便于 Optuna 当 categorical 采样）：
    //     0 legacy            手写 Tick<Class>（现默认）
    //     1 table             策略表，顺序"第一条能放就放"
    //     2 table_scored      策略表 + 表内两遍打分
    //     3 table_scored_book 2 + 法术书表外候选（w_offtable 打折后参与同一把尺子）
    //     4 value             价值引擎（TickValue）
    //   旧四键仍会被读一次做**自动迁移**，并打一条 WARN 告诉该写 `mode=N`；
    //   tuner 的 `k=v` 里也常直接带旧键，所以兼容层保留到调参脚本改完再删。
    // 详见 dev/DESIGN_dps_fsm.md §1。
    // ==========================================================================
    // 名字避开 ai::Engine（经典策略引擎），别让两个 Engine 在同一命名空间打架。
    enum class DecisionMode { Legacy = 0, Table = 1, TableScored = 2, TableScoredBook = 3, Value = 4, Fsm = 5 };

    char const* DecisionModeName(DecisionMode m)
    {
        switch (m)
        {
            case DecisionMode::Table:           return "table";
            case DecisionMode::TableScored:     return "table_scored";
            case DecisionMode::TableScoredBook: return "table_scored_book";
            case DecisionMode::Value:           return "value";
            case DecisionMode::Fsm:             return "fsm";
            default:                            return "legacy";
        }
    }

    bool ModeIsTable(DecisionMode m)
    {
        return m == DecisionMode::Table || m == DecisionMode::TableScored || m == DecisionMode::TableScoredBook;
    }

    bool ModeIsScored(DecisionMode m)
    {
        return m == DecisionMode::TableScored || m == DecisionMode::TableScoredBook;
    }

    bool ModeIsBook(DecisionMode m)
    {
        return m == DecisionMode::TableScoredBook;
    }

    // 解析顺序：新键 `mode` 优先；没有它才从旧四键推导（并警告一次，告诉你该写哪个 mode）。
    DecisionMode ResolveMode(BotMem* m, bool& derivedFromLegacy)
    {
        derivedFromLegacy = false;

        double const raw = TP(m, "mode", -1.0);     // -1 = 没配
        if (raw >= 0.0)
        {
            int const v = int(raw + 0.5);
            if (v >= 0 && v <= 5)
                return DecisionMode(v);
            sLog.outError("[DPSFSM-MODE] mode=%d 越界（合法 0..5），按 legacy 处理", v);
            return DecisionMode::Legacy;
        }

        // ---- 旧口径兼容（derive 一次，行为与旧代码逐一等价）----
        bool const value   = TP(m, "value",   0.0) >= 1.0;
        bool const policy  = TP(m, "policy",  0.0) >= 1.0;
        bool const pselect = TP(m, "pselect", 0.0) >= 1.0;
        bool const pauto   = TP(m, "pauto",   0.0) >= 1.0;

        if (!value && !policy && !pselect && !pauto)
            return DecisionMode::Legacy;        // 什么都没配 = 原默认，不必警告

        derivedFromLegacy = true;
        if (value)
            return DecisionMode::Value;         // 旧代码里 value 先跑、失败后还能落 policy；见警告

        // 旧代码的 nested 读法：pauto 只在 pselect=1 时才被读
        if (policy)
        {
            if (!pselect)
                return DecisionMode::Table;     // pselect=0 ⇒ pauto 配了也不生效
            return pauto ? DecisionMode::TableScoredBook : DecisionMode::TableScored;
        }
        return DecisionMode::Legacy;            // 只有 pselect/pauto、没有 policy ⇒ 原来完全无效
    }

    int   AoEPack(BotMem* m)  { return int(TP(m, "aoe_pack", 3.0)); }
    float AoERange(BotMem* m) { return float(TP(m, "aoe_range", 11.0)); }
    float BurstHp(BotMem* m)  { return float(TP(m, "burst_hp", 20.0)); }

    // 名字 → id：先问引擎（它已经处理了中英文/DBC 名称匹配），再顶到"已学会的最高 rank"。

    // 省蓝等级 > 1 时引擎会给最低 rank，所以这里一定要自己往上爬 spell_chain。

    // [2026-10-30 修] 兜底：直接按名字扫 bot 自己的法术表，取**最高 rank**。
    //   为什么需要它：引擎的 "spell id" 值（SpellIdValue）会过滤 `IsPassiveSpell`，而姿态(战斗/狂暴姿态)、
    //   图腾这类"形态/召唤"法术正是被过滤掉的一类 ⇒ 值返回 0 ⇒ 状态机整条技能表静默失效
    //   （实测：狂暴战整场 0 次旋风斩，日志只有一句"没有可用技能 'berserker stance'"）。
    uint32 ScanSpellbookByName(Player* bot, char const* name)
    {
        uint32 best = 0;
        for (PlayerSpellMap::const_iterator itr = bot->GetSpellMap().begin(); itr != bot->GetSpellMap().end(); ++itr)
        {
            uint32 const id = itr->first;
            if (itr->second.state == PLAYERSPELL_REMOVED || itr->second.disabled)
                continue;
            SpellEntry const* entry = sServerFacade.LookupSpellInfo(id);
            if (!entry || !entry->SpellName[0])
                continue;
            if (strcasecmp(entry->SpellName[0], name) != 0)
                continue;
            if (!best || sSpellMgr.IsSpellHigherRankOfSpell(id, best))
                best = id;
        }
        return best;
    }

    uint32 SpellId(PlayerbotAI* ai, Player* bot, BotMem* m, char const* name, bool warn = true)
    {
        auto it = m->ids.find(name);
        if (it != m->ids.end())
            return it->second;

        uint32 id = 0;
        if (auto* value = ai->GetAiObjectContext()->GetValue<uint32>("spell id", name))
            id = value->Get();

        if (!id)
            id = ScanSpellbookByName(bot, name);

        if (id)
        {
            for (uint32 next = sSpellMgr.GetNextSpellInChain(id); next; next = sSpellMgr.GetNextSpellInChain(next))
                if (bot->HasSpell(next))
                    id = next;
            if (!bot->HasSpell(id))
                id = 0;
        }

        m->ids[name] = id;
        if (!id && warn && m->unresolved.insert(name).second)
            sLog.outError("[DPSFSM] %s(%s) 没有可用技�?'%s'", bot->GetName(), ai::dpsfsm::SpecName(m->spec), name);
        return id;
    }

    // [2026-10-29 站长口径] **有些技能不吃 GCD**（下一个平砍附带 / 无 GCD 的爆发技），
    //   不能因为"公共冷却没转完"就把它们跳过。命中这些名字时绕过 GCD 检查。
    bool NoGcd(char const* name)
    {
        static char const* kNOCD[] = {
            "heroic strike", "cleave", "maul", "swipe", "thunder clap",       // 下一次平砍附带
            "auto shot",                                                       // 自动射击
            "cold blood", "presence of mind", "arcane power", "icy veins",     // 法师/盗贼爆发
            "tiger's fury", "berserk", "kill command", "shamanistic rage",     // 德/猎/萨爆发
            "bloodlust", "heroism", "use trinket",
        };
        for (char const* k : kNOCD)
            if (strcmp(name, k) == 0)
                return true;
        return false;
    }

    // 自身 buff 的**剩余时间**（毫秒）：圣印 / 切割 / 节能施法这类都靠它做状态
    int AuraRemain(PlayerbotAI* ai, BotMem* m, Snapshot& s, char const* name)
    {
        uint32 id = SpellId(ai, s.bot, m, name, false);
        if (!id)
            return 0;
        SpellAuraHolder* holder = s.bot->GetSpellAuraHolder(id);
        if (!holder)
            return 0;
        int const dur = holder->GetAuraDuration();
        return dur < 0 ? 999999 : dur;    // 永久 buff
    }

    bool Ready(Player* bot, uint32 id)
    {
        // IsSpellReady 同时覆盖：技能自身冷�?/ 分类冷却 / 公共冷却 / 沉默封锁
        return id && bot->HasSpell(id) && bot->IsSpellReady(id);
    }

    bool GcdActive(Player* bot, uint32 id)
    {
        SpellEntry const* entry = id ? sServerFacade.LookupSpellInfo(id) : nullptr;
        return entry && bot->HasGCD(entry);
    }

    // 某目标身上这个法术（任意 rank）的叠层总数
    int AuraStacks(PlayerbotAI* ai, Player* bot, BotMem* m, char const* name, Unit* u)
    {
        if (!u)
            return 0;
        uint32 id = SpellId(ai, bot, m, name, false);
        if (!id)
            return 0;
        int total = 0;
        uint32 first = sSpellMgr.GetFirstSpellInChain(id);
        for (uint32 cur = first ? first : id; cur; cur = sSpellMgr.GetNextSpellInChain(cur))
        {
            if (u->HasAura(cur))
                total += int(u->GetAuraCount(cur));
        }
        return total;
    }

    int CountHostilesNear(PlayerbotAI* ai, Player* bot, float range)
    {
        // [2026-10-29 崩溃修] 不在世界里的 bot 调 GetMap() 会触发核心断言 m_currMap 直接关服
        if (!bot || !bot->IsInWorld())
            return 0;
        // 假人场景声明过目标集时，只数这些目标（否则会把邻居测点的假人算进 AoE 判断）
        std::set<ObjectGuid> scenario;
        {
            std::lock_guard<std::mutex> lock(s_disabledMutex);
            auto it = s_scenarioTargets.find(bot->GetGUIDLow());
            if (it != s_scenarioTargets.end())
                scenario = it->second;
        }
        if (!bot || !bot->GetMap() || !ai)
            return 0;
        auto* value = ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("possible targets");
        if (!value)
            return 0;
        int n = 0;
        for (ObjectGuid const& guid : value->Get())
        {
            Unit* u = bot->GetMap()->GetUnit(guid);
            if (!u || !u->IsAlive() || u->GetTypeId() != TYPEID_UNIT)
                continue;
            if (bot->GetDistance(u) > range)
                continue;
            if (!sServerFacade.IsHostileTo(bot, u))
                continue;
            if (!scenario.empty() && !scenario.count(u->GetObjectGuid()))
                continue;
            ++n;
        }
        return n;
    }

    void BuildSnapshot(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        Player* bot = ai->GetBot();
        s.bot = bot;
        s.target = (bot->GetVictim() && bot->GetVictim()->IsAlive()) ? bot->GetVictim() : nullptr;
        s.selfHp = bot->GetHealthPercent();

        // 主资源：法力职业看蓝 %；怒战看怒气、盗�?猫德看能量（都换算成占满值的 %，同时保留原始值）
        uint32 const maxMana = bot->GetMaxPower(POWER_MANA);
        if (maxMana)
            s.power = 100.0f * float(bot->GetPower(POWER_MANA)) / float(maxMana);
        else
            s.power = 100.0f;

        s.rage = int32(bot->GetPower(POWER_RAGE));
        s.energy = int32(bot->GetPower(POWER_ENERGY));
        s.combo = bot->GetComboPoints();

        if (!maxMana)
        {
            // 物理职业用怒气/能量�?资源"
            uint32 const maxRage = bot->GetMaxPower(POWER_RAGE);
            if (maxRage)
                s.power = 100.0f * float(s.rage) / float(maxRage);
            else
                s.power = float(s.energy);
        }

        // [2026-10-30 修] IsNonMeleeSpellCasted 的第三参 skipAutorepeat 默认 false ⇒ 自动射击
        //   （CURRENT_AUTOREPEAT_SPELL）也算"正在读条"。猎人自动射击一直挂着 ⇒ s.casting 恒 true
        //   ⇒ TickHunter 在 `if (s.casting) return true` 就返回：稳固射击/猎人印记/毒蛇钉刺整场 0 次
        //   （实测 marks 40 秒 casts=19、top 只有 75:18 = 自动射击的重复登记）。自动射击不是"读条"，跳过它。
        s.casting = bot->IsNonMeleeSpellCasted(false, false, true);
        s.moving = bot->IsMoving();
        s.silenced = bot->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SILENCED);

        if (s.target)
        {
            s.targetHp = s.target->GetHealthPercent();
            s.dist = bot->GetDistance(s.target);
        }

        s.pack10 = CountHostilesNear(ai, bot, 10.0f);
        s.pack30 = CountHostilesNear(ai, bot, 30.0f);
    }

    void LogTransition(PlayerbotAI* ai, BotMem* m, Snapshot& s, State next)
    {
        if (m->state == next)
            return;
        Player* bot = s.bot;
        sLog.outError("[DPSFSM] %s %-13s %s -> %-8s hp=%3.0f%% res=%3.0f%% tgt=%3.0f%% pack=%d/%d dist=%4.1f cast=%d gcd=%d",
            bot->GetName(), ai::dpsfsm::SpecName(m->spec), ai::dpsfsm::StateName(m->state), ai::dpsfsm::StateName(next),
            s.selfHp, s.power, s.targetHp, s.pack10, s.pack30, s.dist, s.casting ? 1 : 0, s.gcd ? 1 : 0);
        m->state = next;
        if (next == State::Idle)
            m->castCount = 0;
    }

    // [2026-10-29 诊断结论] 原来只判 IsSpellReady 就放，失败也照样每 tick 重试（怒气不够、目标闪避条件、
    //   移动中读条被静默取消…），统计里就出现 "40 秒 1204 次施法"。现在：①先跑一遍完整的 CanCastSpell
    //   （Spell::CheckCast 的怒气/射程/朝向/条件都算上）；②失败按 globalCoolDown 退避，别死循环重试。
    bool CastChecked(PlayerbotAI* ai, BotMem* m, Snapshot& s, char const* name, Unit* target, Unit* selfTarget = nullptr)
    {
        uint32 id = SpellId(ai, s.bot, m, name);
        if (NoGcd(name))
        {
            if (!id || !s.bot->HasSpell(id))      // 不吃 GCD，但仍必须会这个技能
                return false;
        }
        else if (!Ready(s.bot, id))
            return false;

        time_t const now = time(nullptr);
        auto it = m->failUntil.find(id);
        if (it != m->failUntil.end() && now < it->second)
            return false;

        Unit* tgt = selfTarget ? selfTarget : (target ? target : s.target);

        // [2026-10-31 根因] `PlayerbotAI::CastSpellBody` 里有一段：
        //     if (!IsInFront(bot, faceTo, ...)) { SetFacingTo(bot, faceTo);
        //         if (!HasRealPlayerMaster()) failWithDelay = true; }
        //     if (failWithDelay) { ... return false; }
        //   ⇒ bot 没有真人主人时，**背对目标的第一次调用只转向、不施法、返回 false**。
        //   而我们把这次 false 当成"施法失败"，写 2 秒退避；几个候选一起被写退避 ⇒ 整轮空转
        //   （实测 1/3 轮次 victim 正常、不 casting、不移动，却 0 施法 0 伤害）。
        //   修法：在调 CastSpell 之前自己先转向，让 IsInFront 成立，一次调用就真施法。
        if (tgt && tgt != s.bot)
            s.bot->SetFacingToObject(tgt);

        SpellCastResult res = SPELL_FAILED_DONT_REPORT;
        if (!ai->CanCastSpell(id, tgt, 0, false, nullptr, false, false, false, &res))
        {
            // [2026-10-30 修] 自身增益/自身姿态这类"该打自己"的法术，走 ai->CanCastSpell(对敌目标) 会被判死。
            //   实测两类结果码：
            //     ① TARGET_ENEMY(108)：燃烧 11129 / 冰冷血脉 12472 / 奥强 12042 / 闪现 12051 /
            //        剑刃乱舞 13877 / 冲动 13750 / 冷血 14177 / 元素掌握 16166 / 英雄 32182 / 唤水元素 31687…
            //        （CanCastSpell 见"正法术 + 敌对目标"直接拒 ⇒ 这些技能整场 0 次）；
            //     ② TARGET_FRIENDLY(110)：切割 6774 这类"效果正面但 IsPositiveSpell=false"的法术，
            //        对着自己放又被判"负法术 + 友方目标" ⇒ 盗贼切割整场 0 次。
            //   本质都是"这一手应该以自己为目标"，改用核心 Unit::CastSpell(self) 放（和奉献 CastSelfCentered 同一条路）。
            bool const selfFix = (res == SPELL_FAILED_TARGET_FRIENDLY)
                || (res == SPELL_FAILED_TARGET_ENEMY && tgt != s.bot);
            if (selfFix)
            {
                SpellCastResult const res2 = s.bot->CastSpell(s.bot, id, uint32(TRIGGERED_NONE));
                if (res2 == SPELL_CAST_OK)
                {
                    ++m->castCount;
                    m->lastOk[id] = now;
                    m->castThisTick = true;
                    PlayerbotAI::RecordSpellCast(s.bot->GetObjectGuid(), id);
                    return true;
                }
                res = res2;
            }
            // [2026-10-29 实测] 一律退 2 秒会把怒气/能量职业的 DPS 砍半：
            //   "蓝/怒/能量不够"是**等一下就够**，只退 0.4 秒；射程/视线/其它条件才退 2 秒。
            bool const waitingResource = (res == SPELL_FAILED_NO_POWER
                || res == SPELL_FAILED_OUT_OF_RANGE || res == SPELL_FAILED_LINE_OF_SIGHT
                || res == SPELL_FAILED_NOT_READY);
            m->failUntil[id] = now + (waitingResource ? 1 : 2);
            return false;
        }
        // [2026-10-31 根因修复] `PlayerbotAI::CastSpellBody()` 对 bot 恒有：
        //     if (!IsInFront(...)) { SetFacingTo(...); if (!HasRealPlayerMaster()) failWithDelay = true; }
        //     if (failWithDelay) return false;      // ← 只转向、不施法
        //   而需要 FacingCasterFlags 的技能（寒冰箭/火球/暗影箭/奥术冲击…）都会撞上这个分支，
        //   于是"自身法术能放、对敌技能全灭"（实测约 1/3 轮次 0 伤害、casts 只有冰冷血脉+水元素）。
        //   ⇒ 对敌施法直接走核心 Unit::CastSpell，绕开这段 bot 专用逻辑。
        if (tgt && tgt != s.bot)
            s.bot->SetFacingToObject(tgt);
        if (tgt && tgt != s.bot)
        {
            SpellCastResult const r2 = s.bot->CastSpell(tgt, id, uint32(TRIGGERED_NONE));
            if (r2 != SPELL_CAST_OK)
            {
                m->failUntil[id] = now;      // 不退避，下一 tick 立刻重试
                return false;
            }
            // [2026-10-31] 核心施法要自己记账，否则 botCasts / top= 统计不到
            //   （之前出现 "dmg=20k 但 casts=0"，正是漏记）
            PlayerbotAI::RecordSpellCast(s.bot->GetObjectGuid(), id);
        }
        else if (!ai->CastSpell(id, tgt))
            return false;
        ++m->castCount;
        m->lastOk[id] = now;
        m->castThisTick = true;
        return true;
    }

    bool TryCast(PlayerbotAI* ai, BotMem* m, Snapshot& s, char const* name, Unit* target)
    {
        return CastChecked(ai, m, s, name, target);
    }

    // 自身增益（圣印/守护/暗影形态/战斗怒吼…）必须打在自己身上：打敌人会被判为负面法术而失败
    bool TryCastSelf(PlayerbotAI* ai, BotMem* m, Snapshot& s, char const* name)
    {
        return CastChecked(ai, m, s, name, nullptr, s.bot);
    }

    // [2026-10-31] **以自己宠物为目标**的施法（恶魔牺牲 18788 就是这样：目标必须是自己的恶魔，
    //   对着自己放会 BAD_TARGETS）。以前 Prereq 一律走 TryCastSelf ⇒ 牺牲暗的 18788 一次都没放出来。
    bool TryCastPet(PlayerbotAI* ai, BotMem* m, Snapshot& s, char const* name)
    {
        Pet* pet = s.bot->GetPet();
        if (!pet || !pet->IsAlive())
            return false;
        return CastChecked(ai, m, s, name, pet, nullptr);
    }

    // [2026-10-30 修] 地面目标技（暴风雪/烈焰风暴/飓风/箭雨/火雨）原来**不做任何校验**就直接
    //   交给 `PlayerbotAI::CastSpell(id, x, y, z)`：那一层只看"能不能排队"，里面 `Spell::SpellStart`
    //   的 CheckCast 失败时是**静默**的（不返回错误码）⇒ 状态机以为放出去了，整条优先级表
    //   就卡死在这一个技能上，后面的兜底技能永远轮不到。
    //   实测（5 个假人一堆）：平衡德进 Aoe 后 40 秒 **0 伤害**（全是"飓风"的幽灵施法：
    //   FSM 记账 46 次、真实施法 1 次），把 balance:aoe_pack 调高绕开 Aoe 分支立刻回到 1040 dps；
    //   兽王猎同理（Aoe 里箭雨幽灵化 ⇒ 0~216 dps，绕开 Aoe 反而 471）。
    //   现在先跑一次完整的 CanCastSpell（射程/视线/资源/站姿/地形都算上，还带回结果码），
    //   失败就按资源类/其它类退避，并且**返回 false 让表里的下一个技能兜底**。
    bool TryCastAt(PlayerbotAI* ai, BotMem* m, Snapshot& s, char const* name, float x, float y, float z)
    {
        uint32 id = SpellId(ai, s.bot, m, name);
        if (!Ready(s.bot, id))
            return false;

        time_t const now = time(nullptr);
        auto it = m->failUntil.find(id);
        if (it != m->failUntil.end() && now < it->second)
            return false;

        SpellCastResult res = SPELL_FAILED_DONT_REPORT;
        if (!ai->CanCastSpell(id, x, y, z, 0, true, nullptr, false, false, false, &res))
        {
            bool const waitingResource = (res == SPELL_FAILED_NO_POWER
                || res == SPELL_FAILED_OUT_OF_RANGE || res == SPELL_FAILED_LINE_OF_SIGHT
                || res == SPELL_FAILED_NOT_READY);
            m->failUntil[id] = now + (waitingResource ? 1 : 2);
            return false;
        }
        if (!ai->CastSpell(id, x, y, z))
            return false;
        ++m->castCount;
        m->lastOk[id] = now;
        m->castThisTick = true;
        PlayerbotAI::RecordSpellCast(s.bot->GetObjectGuid(), id);
        return true;
    }

    // =====================================================================================
    // [2026-10-29 站长口径] **放什么技能、buff 什么时候续，都由"状态 → 优先级表"决定**，
    //   不再用 if/else 把每个技能的优先级硬编码在处理器里。
    //   一条 Entry 描述一件事：
    //     name           技能名（走 SpellId 自动升级 rank）
    //     target         施法目标（nullptr=当前目标；s.bot=自身；其它=指定单位）
    //     requireAura    必须先有这个 aura（例如节能施法才丢火球）
    //     minAuraRemain  续 buff 用：自身该 aura 剩余 < 这个毫秒数就续（<=0 表示"没有就补"）
    //     auraOn         检查哪个 aura 的剩余（默认用 name）
    //     minCombo       至少几个连击点
    //     minEnergy      至少多少能量
    //   表内按优先级从上往下试，第一条满足条件的就执行。
    // =====================================================================================
    struct Entry
    {
        char const* name = nullptr;
        Unit* target = nullptr;
        char const* requireAura = nullptr;
        int minAuraRemain = 0;
        char const* auraOn = nullptr;
        int minCombo = 0;
        int minEnergy = 0;
    };

    bool TryEntry(PlayerbotAI* ai, BotMem* m, Snapshot& s, Entry const& e)
    {
        if (!e.name)
            return false;
        if (e.requireAura && AuraStacks(ai, s.bot, m, e.requireAura, s.bot) == 0)
            return false;
        if (e.minCombo > 0 && s.combo < e.minCombo)
            return false;
        if (e.minEnergy > 0 && s.energy < e.minEnergy)
            return false;
        if (e.minAuraRemain > 0)
        {
            int const remain = AuraRemain(ai, m, s, e.auraOn ? e.auraOn : e.name);
            if (remain >= e.minAuraRemain)     // 还够久，不用续
                return false;
        }
        Unit* tgt = e.target ? e.target : s.target;
        return CastChecked(ai, m, s, e.name, tgt);
    }

    bool TryTable(PlayerbotAI* ai, BotMem* m, Snapshot& s, std::initializer_list<Entry> const& table)
    {
        for (Entry const& e : table)
            if (TryEntry(ai, m, s, e))
                return true;
        return false;
    }

    bool CastFirst(PlayerbotAI* ai, BotMem* m, Snapshot& s, std::initializer_list<char const*> names, Unit* target = nullptr)
    {
        for (char const* n : names)
            if (TryCast(ai, m, s, n, target))
                return true;
        return false;
    }

    // 饰品：复用现成的 "use trinket" 动作（UseTrinketAction 自己会挑已装备、可用的使用型饰品）
    bool UseTrinket(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        time_t const now = time(nullptr);
        if (m->lastTrinketTry && now - m->lastTrinketTry < 30)
            return false;
        m->lastTrinketTry = now;
        if (!ai->DoSpecificAction("use trinket"))
            return false;
        sLog.outError("[DPSFSM] %s %s 开饰品（hp=%.0f%% res=%.0f%% tgt=%.0f%%）",
            s.bot->GetName(), ai::dpsfsm::SpecName(m->spec), s.selfHp, s.power, s.targetHp);
        return true;
    }

    // 法力药水（实现见下方 UseManaPotion）：法师的 Recover 也要用，先声明
    bool UseManaPotion(BotMem* m, Snapshot& s);

    // ============================ 法师 ============================
    bool TickMage(PlayerbotAI* ai, BotMem* m)
    {
        Snapshot s;
        BuildSnapshot(ai, m, s);
        if (!s.target)
        {
            LogTransition(ai, m, s, State::Idle);
            return true;
        }
        if (s.casting)
            return true;   // 读条/引导中，任何技能都不插

        bool const fire = (m->spec == Spec::Fire);
        bool const frost = (m->spec == Spec::Frost);

        char const* filler = fire ? "fireball" : (frost ? "frostbolt" : "arcane blast");
        float const castRange = 30.0f;
        bool const inRange = s.dist <= castRange;

        s.gcd = GcdActive(s.bot, SpellId(ai, s.bot, m, filler));

        // 节能施法：优先级表——有 clearcasting 状态时，最贵的读条法术排到最前
        if (!s.casting && TryTable(ai, m, s, {
                { "fireball",   nullptr, "clearcasting" },
                { "frostbolt",  nullptr, "clearcasting" },
                { "arcane blast", nullptr, "clearcasting" } }))
            return true;

        State st = m->state;
        if (st == State::Idle)
            st = State::Burn;

        // ---- 转移：资�?�?群架 �?斩杀/爆发 �?射程 ----
        float const lowMana = fire ? 10.0f : (frost ? 8.0f : 6.0f);
        if (s.power < lowMana)
            st = State::Recover;
        else if (st == State::Recover && s.power > lowMana + 20.0f)
            st = State::Burn;

        if (st != State::Recover && s.pack30 >= AoEPack(m))
            st = State::Aoe;
        else if (st == State::Aoe)
            st = State::Burn;

        bool const burstWindow = (s.targetHp <= BurstHp(m));
        if (st == State::Burn && burstWindow && s.dist <= castRange)
            st = State::Burst;
        else if (st == State::Burst && !burstWindow)
            st = State::Burn;

        if (st != State::Recover && st != State::Aoe && !inRange)
            st = State::Approach;
        else if (st == State::Approach && inRange)
            st = State::Burn;

        // ---- 动作：本 GCD 一�?----
        switch (st)
        {
            case State::Recover:
                // 唤醒（8 分钟一次）优先，其次法力药水，最后才是魔杖

                if (!TryCastSelf(ai, m, s, "evocation"))
                {
                    if (UseManaPotion(m, s)) break;
                    CastFirst(ai, m, s, { "shoot" });
                }
                break;

            case State::Aoe:
                if (s.dist <= 10.0f && CastFirst(ai, m, s, { "blast wave" })) break;
                if (s.dist <= 10.0f && TryCast(ai, m, s, "arcane explosion", nullptr)) break;
                if (fire)
                {
                    if (TryCastAt(ai, m, s, "flamestrike", s.target->GetPositionX(), s.target->GetPositionY(), s.target->GetPositionZ())) break;
                }
                else if (frost)
                {
                    if (TryCastAt(ai, m, s, "blizzard", s.target->GetPositionX(), s.target->GetPositionY(), s.target->GetPositionZ())) break;
                }
                CastFirst(ai, m, s, { filler });
                break;

            case State::Burst:
                if (UseTrinket(ai, m, s)) break;
                if (fire)
                {
                    // 燃烧 �?奥强? 无；火法：饰品位已用，燃�?�?气定炎爆
                    if (CastFirst(ai, m, s, { "combustion" })) break;
                    if (CastFirst(ai, m, s, { "presence of mind" })) break;
                    if (CastFirst(ai, m, s, { "pyroblast" })) break;
                    if (CastFirst(ai, m, s, { "fire blast" })) break;
                }
                else if (frost)
                {
                    if (CastFirst(ai, m, s, { "icy veins" })) break;
                    if (CastFirst(ai, m, s, { "cold snap" })) break;
                    if (CastFirst(ai, m, s, { "ice lance" })) break;
                }
                else
                {
                    if (CastFirst(ai, m, s, { "arcane power" })) break;
                    if (CastFirst(ai, m, s, { "arcane barrage" })) break;
                }
                CastFirst(ai, m, s, { filler });
                break;

            case State::Approach:
                if (s.moving && CastFirst(ai, m, s, { fire ? "fire blast" : (frost ? "ice lance" : "arcane barrage") }))
                    break;
                break;

            case State::Instant:
            case State::Burn:
            default:
            {
                st = State::Burn;
                if (s.moving)
                {
                    // 移动中只能用瞬发：火�?冰枪/弹幕
                    if (CastFirst(ai, m, s, { fire ? "fire blast" : (frost ? "ice lance" : "arcane barrage") }))
                        break;
                }

                if (fire)
                {
                    // [2026-10-29] 大招/饰品原来只在"斩杀 Burst"里放 ⇒ 打木桩（满血）永远不用。
                    //   燃烧/冰冷血脉/奥强这类"CD 好了就该放"，放进常规循环。
                    if (UseTrinket(ai, m, s)) break;
                    if (CastFirst(ai, m, s, { "combustion" })) break;
                    // 强化灼烧：目标身上叠层 <5 就补灼烧；没这天赋（从没见过该 debuff）就跳过。

                    // 叠层 debuff 与灼烧本体不是同一个 id（强化灼烧挂的是"火焰易伤"），两个名字都查。

                    int const scorchStacks = std::max(
                        AuraStacks(ai, s.bot, m, "scorch", s.target),
                        AuraStacks(ai, s.bot, m, "fire vulnerability", s.target));
                    if (scorchStacks > 0)
                        m->flags.insert("scorch seen");
                    int const scorchFull = int(TP(m, "scorch_stacks", 5.0));
                    if (scorchStacks < scorchFull && m->flags.count("scorch seen")
                        && TryCast(ai, m, s, "scorch", nullptr))
                        break;
                    // 火焰冲击 8 秒 CD、瞬发：不占读条，CD 好了就该打（原来只在移动/斩杀才放）
                    if (CastFirst(ai, m, s, { "fire blast" })) break;
                }
                else if (frost)
                {
                    if (UseTrinket(ai, m, s)) break;
                    if (CastFirst(ai, m, s, { "icy veins" })) break;
                    // 水元素：没宠物又学会就叫一只（一次性开销，别�?tick 试）
                    if (!s.bot->GetPet() && CastFirst(ai, m, s, { "summon water elemental" })) break;
                }
                else
                {
                    if (UseTrinket(ai, m, s)) break;
                    if (CastFirst(ai, m, s, { "arcane power" })) break;
                    // 奥：奥术冲击叠到 3 层就放奥术飞弹泄掉
                    // [2026-10-30 修] 叠层 debuff 是**独立 id 36032**，不在 30451 的 spell_chain 里；
                    //   原来按 "arcane blast" 查 aura 永远是 0 ⇒ 奥术飞弹整场 0 次。
                    int const blastStacks = int(s.bot->GetAuraCount(36032));
                    // 蓝够就继续奥冲（叠层既加伤害也加耗蓝）；蓝紧了再用飞弹把叠层泄掉、稳住续航
                    if (blastStacks >= 3 && s.power < 45.0f && CastFirst(ai, m, s, { "arcane missiles" })) break;
                    // [2026-10-31 修正] TBC 2.4.3 **没有奥术弹幕**（Arcane Barrage 是 WLK 的）。
                    //   原来这里挂着它：SpellId 取不到 → 每 tick 白试一次，日志更会误导读技能表的人。
                }

                CastFirst(ai, m, s, { filler });
                break;
            }
        }

        LogTransition(ai, m, s, st);
        return true;
    }

    // 天赋技能判定：这些名字只有点了对应天赋才学得会 ⇒ 用它定专精
    struct TalentSpecName { char const* name; Spec spec; };

    Spec DetectSpecByTalentsImpl(PlayerbotAI* ai, BotMem* m)
    {
        Player* bot = ai ? ai->GetBot() : nullptr;
        if (!bot)
            return Spec::Unknown;

        static TalentSpecName const mage[] = {
            { "combustion", Spec::Fire }, { "cold snap", Spec::Frost }, { "presence of mind", Spec::Arcane } };
        static TalentSpecName const warrior[] = {
            { "bloodthirst", Spec::Fury }, { "mortal strike", Spec::Arms }, { "shield slam", Spec::Protection } };
        static TalentSpecName const rogue[] = {
            { "mutilate", Spec::Assassination }, { "blade flurry", Spec::Combat }, { "preparation", Spec::Subtlety } };
        static TalentSpecName const hunter[] = {
            { "bestial wrath", Spec::BeastMastery }, { "aimed shot", Spec::Marksmanship }, { "wyvern sting", Spec::Survival } };
        static TalentSpecName const warlock[] = {
            { "unstable affliction", Spec::Affliction }, { "soul link", Spec::Demonology }, { "shadowfury", Spec::Destruction } };
        static TalentSpecName const shaman[] = {
            { "stormstrike", Spec::Enhancement }, { "elemental mastery", Spec::Elemental }, { "earth shield", Spec::Restoration } };
        static TalentSpecName const druid[] = {
            { "mangle (cat)", Spec::Feral }, { "force of nature", Spec::Balance }, { "tree of life", Spec::Restoration } };
        static TalentSpecName const paladin[] = {
            { "crusader strike", Spec::Retribution }, { "holy shield", Spec::Protection }, { "holy shock", Spec::Holy } };
        static TalentSpecName const priest[] = {
            { "shadowform", Spec::Shadow }, { "pain suppression", Spec::Discipline }, { "circle of healing", Spec::Holy } };

        TalentSpecName const* table = nullptr;
        switch (bot->getClass())
        {
            case CLASS_MAGE:    table = mage;    break;
            case CLASS_WARRIOR: table = warrior; break;
            case CLASS_ROGUE:   table = rogue;   break;
            case CLASS_HUNTER:  table = hunter;  break;
            case CLASS_WARLOCK: table = warlock; break;
            case CLASS_SHAMAN:  table = shaman;  break;
            case CLASS_DRUID:   table = druid;   break;
            case CLASS_PALADIN: table = paladin; break;
            case CLASS_PRIEST:  table = priest;  break;
            default: return Spec::Unknown;
        }

        for (int i = 0; i < 3; ++i)
            if (SpellId(ai, bot, m, table[i].name, false))
                return table[i].spec;

        return Spec::Unknown;
    }

    // [2026-10-29 站长口径] 法力职业的**法力药水**：在背包里找"使用后回蓝"的消耗品，
    //   蓝低于阈值就喝（核心的 CastItemUseSpell 会正常消耗物品并应用效果）。
    //   注意：不用 ai->DoSpecificAction("mana potion")，那个动作的 isUseful 依赖 AI 自己的战斗状态，
    //   假人场景里会被判成"不 useful"，实测一次都没喝到。
    bool UseManaPotion(BotMem* m, Snapshot& s)
    {
        if (s.power > float(TP(m, "potion_mana", 35.0)))
            return false;

        time_t const now = time(nullptr);
        if (m->lastPotionTry && now - m->lastPotionTry < 3)
            return false;
        m->lastPotionTry = now;

        for (int slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        {
            Item* item = s.bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;
            ItemPrototype const* proto = item->GetProto();
            if (!proto)
                continue;

            for (int e = 0; e < MAX_ITEM_PROTO_SPELLS; ++e)
            {
                uint32 const spellId = proto->Spells[e].SpellId;
                if (!spellId || proto->Spells[e].SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
                    continue;
                SpellEntry const* entry = sServerFacade.LookupSpellInfo(spellId);
                if (!entry)
                    continue;

                bool energize = false;
                for (int i = 0; i < 3; ++i)
                    if (entry->Effect[i] == SPELL_EFFECT_ENERGIZE)
                        energize = true;
                if (!energize)
                    continue;
                if (!s.bot->IsSpellReady(spellId))
                    continue;

                SpellCastTargets targets;
                targets.setUnitTarget(s.bot);
                s.bot->CastItemUseSpell(item, targets, 0, 0);
                sLog.outError("[DPSFSM] %s 喝法力药水（item=%u, res=%.0f%%）",
                    s.bot->GetName(), proto->ItemId, s.power);
                return true;
            }
        }
        return false;
    }

    // [2026-10-29 站长口径] 近战输出要**找背**：毁伤/背刺/撕碎都要求站在目标背后，
    //   其它近战站背后也不会被招架/躲闪。这里不自己写寻路——算出目标背后 0.8×攻击距离的点，
    //   用引擎的 MovePoint 走过去；已经在背后就不动。
    bool GetBehind(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        if (!s.target || s.dist > 15.0f)
            return false;
        if (s.bot->IsFacingTargetsBack(s.target))
            return false;

        time_t const now = time(nullptr);
        if (m->lastBehindTry && now - m->lastBehindTry <= 1)
            return false;
        m->lastBehindTry = now;

        float const dist = s.bot->GetCombinedCombatReach(s.target, true) * 0.8f;
        float const ang = s.target->GetOrientation();
        float x = s.target->GetPositionX() - cos(ang) * dist;
        float y = s.target->GetPositionY() - sin(ang) * dist;
        float z = s.target->GetPositionZ();
        s.bot->UpdateGroundPositionZ(x, y, z);
        s.bot->GetMotionMaster()->MovePoint(0, x, y, z);
        return true;
    }

    // [2026-10-30 修] **近战要真的站进 MeleeReach 里**，不能只看"表面距离够近"。
    //   背景（实测 40 秒 melee=0 / 十字军打击 0 次）：
    //     · Snapshot.dist 是 bot->GetDistance(target)，默认 DIST_CALC_BOUNDING_RADIUS
    //       ⇒ 是"减去双方体积半径之后的表面距离"，比核心的判定小 0.5~1 码；
    //     · 核心 Unit::CanReachWithMeleeAttack 用**中心距离**比 GetCombinedCombatReach(...,true)
    //       （其下限就是 ATTACK_DISTANCE=5.0，而且是严格 `<`）。
    //   于是 bot 站在表面距离 4.5（中心 5.00）时：状态机以为"在近战范围"（<= 6.5），不出 Approach；
    //   核心却认为够不着 ⇒ 一次平砍都不挥、Crusader Strike 一直被 CheckCast 判 OUT_OF_RANGE。
    //   引擎的 "reach melee" 也救不了：ReachTargetAction 的 range=0 ⇒ ChaseTo(target, 0) 里
    //   endPoint = botPoint + dir*min(0, dist) = 自己脚下，路径长度为 0 ⇒ 原地不动。
    //   所以这里由状态机自己贴上去：走到"够得着"的内侧点（0.8 倍 MeleeReach）。
    bool CloseToMelee(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        if (!s.target || !s.bot->IsInWorld())
            return false;

        time_t const now = time(nullptr);
        if (m->lastCloseTry && now - m->lastCloseTry <= 0)   // 1 秒一次，别每 tick 都改路径
            return false;
        m->lastCloseTry = now ? now : 1;

        float const reach = s.bot->GetCombinedCombatReach(s.target, true);
        float const want = (reach > 1.0f) ? reach * 0.8f : 2.0f;   // 站到"够得着"的内侧
        float const dx = s.bot->GetPositionX() - s.target->GetPositionX();
        float const dy = s.bot->GetPositionY() - s.target->GetPositionY();
        float const len = sqrtf(dx * dx + dy * dy);
        if (len < 0.05f)
            return false;

        float x = s.target->GetPositionX() + dx / len * want;
        float y = s.target->GetPositionY() + dy / len * want;
        float z = s.target->GetPositionZ();
        s.bot->UpdateGroundPositionZ(x, y, z);
        s.bot->GetMotionMaster()->MovePoint(0, x, y, z);
        return true;
    }

    // [2026-10-29 实测] 英勇打击/顺劈/重殴这类"下一次挥击附带"的技能没有冷却，
    //   每个 AI tick 都能重新施放 —— 实测 arms 40 秒刷了 1116 次、fury 934 次，
    //   既白烧怒气也把施法统计搞脏。它们排进 CURRENT_MELEE_SPELL 就说明已经挂上了，不用再放。
    // [2026-10-29 站长口径] 资源账本：怒气/能量/法力的真实消耗一律从 SpellEntry->manaCost 读，
    //   不写死数字（不同等级/不同 rank 的消耗不同）。这样"放了这一手还能不能放下关键技能"才判得准。
    int SpellCost(PlayerbotAI* ai, BotMem* m, char const* name)
    {
        uint32 const id = SpellId(ai, ai->GetBot(), m, name, false);
        SpellEntry const* entry = id ? sServerFacade.LookupSpellInfo(id) : nullptr;
        return entry ? int(entry->manaCost) : 0;
    }

    bool TrySwing(PlayerbotAI* ai, BotMem* m, Snapshot& s, char const* name)
    {
        if (s.bot->GetCurrentSpell(CURRENT_MELEE_SPELL))
            return false;
        return TryCast(ai, m, s, name, nullptr);
    }

    // [2026-10-30] **自身中心的"敌方"AoE（奉献）**：两条现成入口都进不去——
    //   ① `TryCast`/`CanCastSpell(自己)`：IsPositiveSpell(奉献)=false（带伤害），于是
    //      PlayerbotAI::CanCastSpell 里"负法术 + 友方目标"⇒ SPELL_FAILED_TARGET_FRIENDLY(110)；
    //   ② `TryCastAt(x,y,z)`：PlayerbotAI::CastSpell(x,y,z) 要求 Targets 里有 DEST/SOURCE_LOCATION，
    //      而奉献是 TARGET_UNIT_CASTER（Targets=0）⇒ 直接 return false。
    //   这里照引擎 SPELL_ACTION 的做法走核心 `Unit::CastSpell`（单位目标是敌人，法术隐式目标是施法者），
    //   自己守 GCD / 冷却，成功才算一次施法。
    bool CastSelfCentered(PlayerbotAI* ai, BotMem* m, Snapshot& s, char const* name)
    {
        uint32 const id = SpellId(ai, s.bot, m, name);
        if (!Ready(s.bot, id))
            return false;
        SpellEntry const* entry = sServerFacade.LookupSpellInfo(id);
        if (entry && s.bot->HasGCD(entry))
            return false;    // 别顶掉正在转的 GCD

        // 目标是**自己**：奉献的 RangeIndex=1 是"自身 0 码"，拿敌人当单位目标会被核心判
        //   SPELL_FAILED_OUT_OF_RANGE(0x5C=92)（实测）。核心不像 bot 的 CanCastSpell 那样做正负法术
        //   启发式，所以"对自己放一个带伤害的自身 AoE"是合法的（客户端点奉献就是这个效果）。
        Unit* const tgt = s.bot;
        SpellCastResult const res = s.bot->CastSpell(tgt, id, uint32(TRIGGERED_NONE));
        if (res != SPELL_CAST_OK)
        {
            time_t const now = time(nullptr);
            if (!m->lastConsLog || now - m->lastConsLog >= 10)
            {
                m->lastConsLog = now;
                sLog.outError("[DPSFSM] %s %s 放不出：id=%u result=%d（0=OK，其它见 SpellCastResult）",
                    s.bot->GetName(), name, id, int(res));
            }
            return false;
        }
        ++m->castCount;
        m->castThisTick = true;
        m->lastOk[id] = time(nullptr);
        PlayerbotAI::RecordSpellCast(s.bot->GetObjectGuid(), id);
        return true;
    }

    // ============================ 战士 ============================
    bool TickWarrior(PlayerbotAI* ai, BotMem* m)
    {
        Snapshot s;
        BuildSnapshot(ai, m, s);
        if (!s.target) { LogTransition(ai, m, s, State::Idle); return false; }   // 交回引擎重选目标
        if (s.casting) return true;
        if (m->spec == Spec::Protection) return false;   // 防御坦的技能后面单独做

        GetBehind(ai, m, s);   // 绕背：只走位，不占这个 GCD   // 找背
        bool const fury = (m->spec == Spec::Fury);
        bool const inRange = s.dist <= 6.5f;   // 近战够得着就行（5.0 太紧，卡在 5.2 会永远不进循环）
        bool const executeWindow = s.targetHp <= BurstHp(m);

        State st = (m->state == State::Idle) ? State::Burn : m->state;
        if (s.pack10 >= AoEPack(m) && inRange)
            st = State::Aoe;
        else if (st == State::Aoe)
            st = State::Burn;
        if (st == State::Burn && executeWindow)
            st = State::Burst;
        else if (st == State::Burst && !executeWindow)
            st = State::Burn;
        if (st != State::Aoe && !inRange)
            st = State::Approach;
        else if (st == State::Approach && inRange)
            st = State::Burn;

        switch (st)
        {
            case State::Approach:
                break;
            case State::Aoe:
                if (CastFirst(ai, m, s, { "sweeping strikes" })) break;
                if (CastFirst(ai, m, s, { "whirlwind" })) break;
                if (s.rage >= 60 && TrySwing(ai, m, s, "cleave")) break;
                break;
            case State::Burst:
                if (UseTrinket(ai, m, s)) break;
                if (fury && CastFirst(ai, m, s, { "death wish" })) break;
                if (CastFirst(ai, m, s, { "execute" })) break;
                CastFirst(ai, m, s, { fury ? "bloodthirst" : "mortal strike" });
                break;
            case State::Burn:
            default:
            {
                st = State::Burn;
                if (UseTrinket(ai, m, s)) break;
                // [2026-10-30] 低血段（<BurstHp）先打斩杀：核心对斩杀有 TargetAuraState=2 的门，
                //   只有目标 <20% 血才成立；不放在最前会被猛击/致死把怒气吃光（实测低血木桩仍只出致死）。
                if (executeWindow)
                {
                    uint32 const exId = SpellId(ai, s.bot, m, "execute", false);
                    bool const has = exId && s.bot->HasSpell(exId);
                    SpellCastResult res = SPELL_FAILED_DONT_REPORT;
                    bool const can = exId != 0 && ai->CanCastSpell(exId, s.target, 0, false, nullptr, false, false, false, &res);
                    static time_t lastExLog = 0;
                    if (lastExLog + 3 <= time(nullptr))
                    {
                        lastExLog = time(nullptr);
                        sLog.outError("[EXECUTE-DBG] %s exId=%u has=%d can=%d res=%d hp=%.1f burst=%.0f rage=%.0f",
                            s.bot->GetName(), exId, has ? 1 : 0, can ? 1 : 0, int(res), s.targetHp, BurstHp(m), s.rage);
                    }
                    if (CastFirst(ai, m, s, { "execute" })) break;
                }
                if (fury && CastFirst(ai, m, s, { "death wish" })) break;
                // [2026-10-30 修] 狂暴战必须在**狂暴姿态**：旋风斩(1680) Stances=0x40000 只要狂暴姿态，
                //   实测 bot 默认战斗姿态(17) ⇒ whirlwind 每次都被判 ONLY_SHAPESHIFT(89)、整场 0 次。
                //   姿态切换只占一个 GCD，开场切一次就够（切完一直保持）。
                if (fury && s.bot->GetShapeshiftForm() != FORM_BERSERKERSTANCE)
                {
                    if (TryCastSelf(ai, m, s, "berserker stance")) break;
                }

                // 怒气账本：先看"这一手放完，下一个 CD 到点的关键技能（嗜血/致死）还够不够怒"
                char const* core = fury ? "bloodthirst" : "mortal strike";
                int const coreCost = SpellCost(ai, m, core);
                int const wwCost = SpellCost(ai, m, "whirlwind");
                int const hsCost = SpellCost(ai, m, "heroic strike");
                int const slamCost = SpellCost(ai, m, "slam");
                int const hsFloor = int(TP(m, "hs_rage", 75.0));

                if (s.rage >= coreCost && CastFirst(ai, m, s, { core })) break;
                if (s.rage >= wwCost && CastFirst(ai, m, s, { "whirlwind" })) break;
                if (CastFirst(ai, m, s, { "overpower" })) break;          // 压制只花 5 怒，随时可打
                // [2026-10-30 修] 猛击(15 怒) 会吃掉致死(30 怒) 的怒气：实测 arms 40 秒只出 12 次猛击、
                //   致死 **0 次**（怒气在 150~299 之间被猛击一路吃光，永远攒不到 300）。
                //   所以猛击必须给致死留够一发：rage >= 致死 + 猛击。
                if (!fury && s.rage >= slamCost + coreCost && CastFirst(ai, m, s, { "slam" })) break;
                // 英勇/顺劈是"下次挥击附带"，会顶掉普攻（换怒）⇒ 必须给下一个 core 留够怒
                if (s.rage >= coreCost + hsCost && s.rage >= hsFloor
                    && TrySwing(ai, m, s, "heroic strike")) break;
                break;   // 怒不够就等普攻，别硬放（硬放只会让核心技能永远没怒）
            }
        }

        LogTransition(ai, m, s, st);
        return true;
    }

    // ============================ 盗贼 ============================
    bool TickRogue(PlayerbotAI* ai, BotMem* m)
    {
        Snapshot s;
        BuildSnapshot(ai, m, s);
        if (!s.target) { LogTransition(ai, m, s, State::Idle); return false; }   // 交回引擎重选目标
        if (s.casting) return true;

        bool const inRange = s.dist <= 6.5f;   // 近战够得着就行（5.0 太紧，卡在 5.2 会永远不进循环）
        bool const mutilate = (m->spec == Spec::Assassination);
        GetBehind(ai, m, s);   // 绕背：只走位，不占这个 GCD   // 毁伤/背刺要背身
        bool const executeWindow = s.targetHp <= BurstHp(m);
        // [2026-10-30 修] 盗贼原来**根本没有 Aoe 状态**（`aoe_pack` / `combat:aoe_pack` 对盗贼是死配置），
        //   群战里跟单体一模一样。TBC 盗贼没有真正的群体技（刀扇 Fan of Knives 是 WLK 才有），
        //   群战能做的只有：剑刃乱舞（额外打一个目标 + 20% 攻速）和冲动，其余照常攒星泄星。
        //   所以这里补一个 Aoe 状态，把这两张 CD 技顶到最前面，后面的循环完全复用单体表。
        bool const aoe = (s.pack10 >= AoEPack(m));

        State st = (m->state == State::Idle) ? State::Burn : m->state;
        if (!inRange)
            st = State::Approach;
        else if (st == State::Approach)
            st = State::Burn;
        if (st != State::Approach && aoe)
            st = State::Aoe;
        else if (st == State::Aoe)
            st = State::Burn;
        if (st == State::Burn && executeWindow)
            st = State::Burst;
        else if (st == State::Burst && !executeWindow)
            st = State::Burn;

        switch (st)
        {
            case State::Approach:
                break;
            case State::Aoe:
                // 群战：先把两张 CD 技挂上（剑刃乱舞 = 额外打一个目标 + 20% 攻速；冲动 = 回能量），
                //   之后完全沿用下面的单体循环——TBC 盗贼的"群伤"就是这两张 CD 技，没有别的。
                if (UseTrinket(ai, m, s)) break;
                if (!mutilate && CastFirst(ai, m, s, { "adrenaline rush" })) break;
                if (CastFirst(ai, m, s, { "blade flurry" })) break;
                [[fallthrough]];
            case State::Burst:
                if (st == State::Burst)
                {
                    if (UseTrinket(ai, m, s)) break;
                    if (mutilate && CastFirst(ai, m, s, { "cold blood" })) break;
                    if (!mutilate && CastFirst(ai, m, s, { "adrenaline rush" })) break;
                    if (CastFirst(ai, m, s, { "blade flurry" })) break;
                    break;
                }
                [[fallthrough]];
            case State::Burn:
            default:
            {
                if (st != State::Aoe) st = State::Burn;
                // 切割常驻：有星就先补，再攒星；终结技走毒�?剔骨
                if (UseTrinket(ai, m, s)) break;
                if (!mutilate && CastFirst(ai, m, s, { "adrenaline rush" })) break;
                if (CastFirst(ai, m, s, { "blade flurry" })) break;
                if (mutilate && CastFirst(ai, m, s, { "cold blood" })) break;

                // 能量账本：终结技会清空连击点，放完还要留够一发攒星技的能量，否则出现"空星干等"
                char const* builder = mutilate ? "mutilate" : "sinister strike";
                char const* finisher = mutilate ? "envenom" : "eviscerate";
                int const builderCost = SpellCost(ai, m, builder);
                int const finisherCost = SpellCost(ai, m, finisher);
                int const sndCp = int(TP(m, "snd_cp", 4.0));      // 切割要几星
                int const finCp = int(TP(m, "fin_cp", 5.0));      // 终结技要几星
                int const evisEnergy = int(TP(m, "fin_energy", 45.0));  // 放终结技前至少留这么多能量

                // 切割：优先级表里一条"剩余 < 2.5s 且连击点够就续"的自身 buff 条目
                // [2026-10-30 修] 目标必须填**当前敌人**，不能填自己：核心 Spell::CheckCast 对连击点法术
                //   要求 `unitTarget == GetComboTargetGuid()`，对自己放会判 NO_COMBO_POINTS(73)
                //   （实测切割 40 秒 0 次，全是 res=73）。切割的隐式目标本来就是施法者，单位目标给敌人没关系。
                if (TryTable(ai, m, s, {
                        { "slice and dice", nullptr, nullptr, 2500, nullptr, sndCp, 0 } }))
                    break;
                if (s.combo >= finCp && s.energy >= evisEnergy)
                {
                    // [2026-10-30 修] 刺杀：毒伤(Envenom)要求目标身上有中毒状态（TargetAuraState=16，实测 res=106
                    //   TARGET_AURASTATE），木桩/没上毒的怪身上永远没有 ⇒ 星级满了也放不出终结技，
                    //   只能反复攒星。改成"毒伤放不出就剔骨兜底"。
                    if (mutilate && CastFirst(ai, m, s, { "envenom", "eviscerate" })) break;
                    if (!mutilate && CastFirst(ai, m, s, { "eviscerate" })) break;
                }
                // 星已经满、能量还不够放终结技时**别再攒星**（连击点上限 5，多打的全浪费）
                if (s.combo >= finCp)
                    break;
                // 毁灭/毁伤要求匕首 + 背后；随机 bot 经常不满足 ⇒ 一定留个邪恶攻击兜底，
                //   否则整场 0 施法（实测 assassination 40 秒 casts=0、只有 2 次平砍）。
                if (CastFirst(ai, m, s, mutilate ? std::initializer_list<char const*>{ "mutilate", "sinister strike" }
                                                 : std::initializer_list<char const*>{ "sinister strike" })) break;
                break;
            }
        }

        LogTransition(ai, m, s, st);
        return true;
    }

    // ============================ 猎人 ============================
    bool TickHunter(PlayerbotAI* ai, BotMem* m)
    {
        Snapshot s;
        BuildSnapshot(ai, m, s);
        if (!s.target) { LogTransition(ai, m, s, State::Idle); return false; }   // 交回引擎重选目标
        if (s.casting) return true;

        bool const mm = (m->spec == Spec::Marksmanship);

        // 8 码内是猎人死角：移动交给引擎（它现在会按射程停住，见 GetRange("attack")），
        //   状态机这里只判断"能不能打"。
        if (s.dist < 8.0f)
            return false;
        bool const inRange = s.dist <= 35.0f;
        bool const executeWindow = s.targetHp <= BurstHp(m);

        // [2026-10-29 站长口径] 猎人的"自动射击时间"本身就是一个状态：
        //   getAttackTimer(RANGED_ATTACK) = 下一发自动射击还剩多少毫秒；稳固射击 1.5s 读条。
        //   只有在"剩余时间 > 读条 + 余量"时才能读，否则会把这一发自动射击顶掉（净亏伤害）；
        //   窗口不够就进 Weave 等一发（Weave 是本状态机的正式状态，不是"卡住"）。
        int constexpr STEADY_CAST_MS = 1500;
        int constexpr AIMED_CAST_MS = 3000;
        uint32 const autoRemainMs = s.bot->getAttackTimer(RANGED_ATTACK);
        // 自动射击没起手时计时器是 0，只按"窗口够不够"判会永远进 Weave（实测 marks 整场 casts=0）。
        // [2026-10-29 实测] 自动射击不是普通法术：必须走引擎的 "auto shot" 动作
        //   （CastAutoShotAction 会登记 CURRENT_AUTOREPEAT_SPELL）。而且**不能只看计时器**：
        //   没有自动射击在跑时 getAttackTimer(RANGED_ATTACK) 也可能是非 0，于是永远不尝试。
        //   这里改成"只要当前没有自动射击在跑，就每 tick 试一次"。
        if (!s.bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL) && !s.moving)
        {
            // [2026-10-29 站长口径] 自动射击走"双通道"：
            //   ① 引擎动作 CastAutoShotAction（会登记 CURRENT_AUTOREPEAT_SPELL）
            //   ② 动作被拒时直接 CastSpell 一发，再检查登记是否成功
            //   两路结果都记日志，方便判断到底是"动作不可用"还是"武器/弹药问题"。
            bool okAction = ai->DoSpecificAction("auto shot", Event(), true);
            bool okCast = false;
            if (!okAction)
                okCast = TryCast(ai, m, s, "auto shot", s.target);
            bool const registered = s.bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL) != nullptr;

            uint32 const nowSec = uint32(time(nullptr));
            if (m->lastEngageLog + 5 <= nowSec)
            {
                m->lastEngageLog = nowSec;
                sLog.outError("[DPSFSM-AUTO] %s 自动射击: action=%d cast=%d registered=%d dist=%.1f",
                    s.bot->GetName(), okAction ? 1 : 0, okCast ? 1 : 0, registered ? 1 : 0, s.dist);
            }
            if (okAction || okCast)
                return true;
        }
        uint32 const weaveMargin = uint32(TP(m, "weave_margin", 150.0));
        // [2026-10-29 站长口径] 猎人的时间轴要单独算：
        //   读条技（稳固 1.5s / 瞄准 3s）只在"距下一发自动射击还够"时读；
        //   瞬发技（多重/奥术/毒蛇/印记）也不能贴着自动射击放，否则这一发自动射击会被推迟。
        bool const canSteady = !s.moving && autoRemainMs > uint32(STEADY_CAST_MS + int(weaveMargin));
        bool const canAimed = !s.moving && autoRemainMs > uint32(AIMED_CAST_MS + int(weaveMargin));
        bool const canInstant = !s.moving && (autoRemainMs == 0 || autoRemainMs > 700);

        State st = (m->state == State::Idle) ? State::Burn : m->state;
        if (s.pack30 >= AoEPack(m))
            st = State::Aoe;
        else if (st == State::Aoe)
            st = State::Burn;
        if (st == State::Burn && executeWindow)
            st = State::Burst;
        else if (st == State::Burst && !executeWindow)
            st = State::Burn;
        if (s.power < 10.0f)
            st = State::Recover;
        else if (st == State::Recover && s.power > 30.0f)
            st = State::Burn;
        if (st != State::Recover && st != State::Aoe && !inRange)
            st = State::Approach;
        else if (st == State::Approach && inRange)
            st = canSteady ? State::Burn : State::Weave;
        if (st == State::Weave && (canSteady || !inRange))
            st = State::Burn;

        switch (st)
        {
            case State::Approach:
                break;
            case State::Weave:
                // [2026-10-29 实测] 时间轴窗口不好时不能整场空转：
                //   有的 bot（无弹药/动作被拒）根本拉不起自动射击，会卡在 Weave 到结束（casts=0）。
                //   兜底：放一发能吃 GCD 的瞬发，至少不浪费整段时间。
                if (CastFirst(ai, m, s, { "arcane shot", "multi-shot" })) break;
                break;
            case State::Recover:
                // 蝰蛇守护回蓝（有就用），否则平射
                if (AuraStacks(ai, s.bot, m, "aspect of the viper", s.bot) == 0
                    && TryCastSelf(ai, m, s, "aspect of the viper")) break;
                CastFirst(ai, m, s, { "shoot" });
                break;
            case State::Aoe:
                if (canInstant && TryCastAt(ai, m, s, "volley", s.target->GetPositionX(), s.target->GetPositionY(), s.target->GetPositionZ())) break;
                if (canInstant && CastFirst(ai, m, s, { "multi-shot" })) break;
                if (canSteady && CastFirst(ai, m, s, { "steady shot" })) break;
                st = State::Weave;
                break;
            case State::Burst:
                if (UseTrinket(ai, m, s)) break;
                if (CastFirst(ai, m, s, { "bestial wrath" })) break;
                if (CastFirst(ai, m, s, { "kill command" })) break;
                if (mm && canAimed && CastFirst(ai, m, s, { "aimed shot" })) break;
                if (canSteady && CastFirst(ai, m, s, { "steady shot" })) break;
                st = State::Weave;
                break;
            case State::Burn:
            default:
            {
                st = State::Burn;
                if (UseTrinket(ai, m, s)) break;
                if (CastFirst(ai, m, s, { "bestial wrath" })) break;
                if (CastFirst(ai, m, s, { "kill command" })) break;
                if (canInstant && AuraStacks(ai, s.bot, m, "hunter's mark", s.target) == 0
                    && CastFirst(ai, m, s, { "hunter's mark" })) break;
                if (canInstant && AuraStacks(ai, s.bot, m, "serpent sting", s.target) == 0
                    && CastFirst(ai, m, s, { "serpent sting" })) break;
                if (mm && canAimed && CastFirst(ai, m, s, { "aimed shot" })) break;
                if (canSteady && CastFirst(ai, m, s, { "steady shot" })) break;
                if (canInstant && CastFirst(ai, m, s, { "multi-shot" })) break;
                if (canInstant && CastFirst(ai, m, s, { "arcane shot" })) break;
                st = State::Weave;   // 自动射击窗口不够：等，别读条顶掉
                break;
            }
        }

        // [2026-10-29 站长口径] 自动射击的"核心链路"：
        //   核心 `Player::CastSpell()` 在**每次施法前**都会 `InterruptSpell(CURRENT_AUTOREPEAT_SPELL)`，
        //   所以每放一发稳固/奥术/多重，自动射击就会被顶掉；只在"没登记时"补挂的话，
        //   中间那 0.5 秒起手（m_AutoRepeatFirstCast）会把自动射击饿死（实测 40 秒只出 1 发）。
        //   正确做法：**每发技能之后立刻重新挂自动射击**（Auto Shot 不吃 GCD，已在 NoGcd 白名单）。
        if (!s.bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL) && !s.moving && s.dist >= 8.0f)
        {
            if (!TryCast(ai, m, s, "auto shot", s.target))
                ai->DoSpecificAction("auto shot", Event(), true);

        }

        LogTransition(ai, m, s, st);
        return true;
    }

    // ============================ 术士 ============================
    bool TickWarlock(PlayerbotAI* ai, BotMem* m)
    {
        Snapshot s;
        BuildSnapshot(ai, m, s);
        if (!s.target) { LogTransition(ai, m, s, State::Idle); return false; }   // 交回引擎重选目标
        if (s.casting) return true;

        bool const affliction = (m->spec == Spec::Affliction);
        bool const destruction = (m->spec == Spec::Destruction);
        bool const inRange = s.dist <= 30.0f;
        bool const executeWindow = s.targetHp <= BurstHp(m);

        State st = (m->state == State::Idle) ? State::Burn : m->state;
        if (s.power < 12.0f && s.selfHp > 55.0f)
            st = State::Recover;
        else if (st == State::Recover && s.power > 40.0f)
            st = State::Burn;

        if (st != State::Recover && s.pack30 >= AoEPack(m))
            st = State::Aoe;
        else if (st == State::Aoe)
            st = State::Burn;
        if (st == State::Burn && executeWindow)
            st = State::Burst;
        else if (st == State::Burst && !executeWindow)
            st = State::Burn;
        if (st != State::Recover && st != State::Aoe && !inRange)
            st = State::Approach;
        else if (st == State::Approach && inRange)
            st = State::Burn;

        switch (st)
        {
            case State::Approach:
                break;
            case State::Recover:
                // 生命分流把血换成蓝（血够厚才换）

                CastFirst(ai, m, s, { "life tap" });
                break;
            case State::Aoe:
                // [2026-10-30 修] 种子(Seed of Corruption) 对**同一个目标**重复种会把上一颗顶掉：
                //   上一颗永远等不到"到期/受创引爆"，等于每 2 秒白读一次条。
                //   实测群战 40 秒种了 16~18 颗，总伤害只有 4~17 千（对比参考专精的 4~7 万）。
                //   现在：每个目标只种一颗；群体主输出改用地狱火雨(Rain of Fire，地面通道技)，
                //   火雨也不满足条件（站位/蓝不够）时才回到暗影箭。
                if (AuraStacks(ai, s.bot, m, "seed of corruption", s.target) == 0
                    && CastFirst(ai, m, s, { "seed of corruption" })) break;
                if (TryCastAt(ai, m, s, "rain of fire", s.target->GetPositionX(), s.target->GetPositionY(), s.target->GetPositionZ())) break;
                if (CastFirst(ai, m, s, { "shadow bolt" })) break;
                break;
            case State::Burst:
                if (UseTrinket(ai, m, s)) break;
                if (CastFirst(ai, m, s, { "shadowburn" })) break;
                if (CastFirst(ai, m, s, { "shadow bolt" })) break;
                break;
            case State::Burn:
            default:
            {
                st = State::Burn;
                if (UseTrinket(ai, m, s)) break;
                if (AuraStacks(ai, s.bot, m, "curse of elements", s.target) == 0
                    && AuraStacks(ai, s.bot, m, "curse of shadows", s.target) == 0
                    && AuraStacks(ai, s.bot, m, "curse of agony", s.target) == 0)
                {
                    if (affliction || destruction)
                    {
                        if (CastFirst(ai, m, s, { "curse of elements" })) break;
                    }
                    if (CastFirst(ai, m, s, { "curse of agony" })) break;
                }
                if (AuraStacks(ai, s.bot, m, "corruption", s.target) == 0 && CastFirst(ai, m, s, { "corruption" })) break;
                if (affliction)
                {
                    if (AuraStacks(ai, s.bot, m, "siphon life", s.target) == 0 && CastFirst(ai, m, s, { "siphon life" })) break;
                    if (AuraStacks(ai, s.bot, m, "unstable affliction", s.target) == 0 && CastFirst(ai, m, s, { "unstable affliction" })) break;
                }
                if (destruction)
                {
                    bool const immo = AuraStacks(ai, s.bot, m, "immolate", s.target) > 0;
                    if (!immo && CastFirst(ai, m, s, { "immolate" })) break;
                    // [2026-10-30 修] 燃烧(Conflagrate) 要目标身上有献祭，原来的表里根本没有这个技能
                    if (immo && CastFirst(ai, m, s, { "conflagrate" })) break;
                }
                if (affliction && CastFirst(ai, m, s, { "drain life" })) break;
                CastFirst(ai, m, s, { "shadow bolt" });
                break;
            }
        }

        LogTransition(ai, m, s, st);
        return true;
    }

    // ============================ 萨满 ============================
    // [2026-10-30 修] 增强萨的输出有一大块在图腾上（大地之力/优雅/灼热/法力之泉），
    //   而原来的优先级表里**一个图腾都没有** ⇒ 实测 40 秒只有风暴打击 + 震击。
    //   这里按"这个插槽空着就补一个"来做（图腾被拆/到期/没人插都会走到这里），
    //   没学的图腾（低等级/别的元素图腾）直接跳过，不占 GCD。
    bool TotemUpkeep(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        struct TotemSlotName { TotemSlot slot; char const* name; };
        static TotemSlotName const kTotems[] = {
            { TOTEM_SLOT_EARTH, "strength of earth totem" },
            { TOTEM_SLOT_AIR,   "grace of air totem" },
            { TOTEM_SLOT_FIRE,  "searing totem" },
            { TOTEM_SLOT_WATER, "mana spring totem" },
        };
        for (TotemSlotName const& t : kTotems)
        {
            if (!s.bot->GetTotemGuid(t.slot).IsEmpty())
                continue;
            if (!SpellId(ai, s.bot, m, t.name, false))
                continue;
            if (TryCastSelf(ai, m, s, t.name))
                return true;
        }
        return false;
    }

    bool TickShaman(PlayerbotAI* ai, BotMem* m)
    {
        Snapshot s;
        BuildSnapshot(ai, m, s);
        if (!s.target) { LogTransition(ai, m, s, State::Idle); return false; }   // 交回引擎重选目标
        if (s.casting) return true;

        bool const enhance = (m->spec == Spec::Enhancement);
        if (enhance) GetBehind(ai, m, s);   // 增强萨找背
        bool const inRange = enhance ? (s.dist <= 5.0f) : (s.dist <= 25.0f);
        bool const executeWindow = s.targetHp <= BurstHp(m);

        State st = (m->state == State::Idle) ? State::Burn : m->state;
        if (s.power < 10.0f)
            st = State::Recover;
        else if (st == State::Recover && s.power > 30.0f)
            st = State::Burn;
        if (st != State::Recover && s.pack10 >= AoEPack(m) && enhance)
            st = State::Aoe;
        else if (st == State::Aoe && !(s.pack10 >= AoEPack(m) && enhance))
            st = State::Burn;
        if (st == State::Burn && executeWindow)
            st = State::Burst;
        else if (st == State::Burst && !executeWindow)
            st = State::Burn;
        if (st != State::Recover && st != State::Aoe && !inRange)
            st = State::Approach;
        else if (st == State::Approach && inRange)
            st = State::Burn;

        switch (st)
        {
            case State::Approach:
                break;
            case State::Recover:
                if (UseManaPotion(m, s)) break;
                CastFirst(ai, m, s, { "shoot" });
                break;
            case State::Aoe:
                if (CastFirst(ai, m, s, { "chain lightning" })) break;
                if (CastFirst(ai, m, s, { "magma totem" })) break;
                TryCastSelf(ai, m, s, "lightning shield");
                break;
            case State::Burst:
                if (UseTrinket(ai, m, s)) break;
                if (enhance)
                {
                    if (TotemUpkeep(ai, m, s)) break;
                    if (TryCastSelf(ai, m, s, "bloodlust")) break;
                    if (TryCastSelf(ai, m, s, "heroism")) break;
                    if (TryCastSelf(ai, m, s, "shamanistic rage")) break;
                    if (CastFirst(ai, m, s, { "stormstrike" })) break;
                }
                else
                {
                    if (CastFirst(ai, m, s, { "elemental mastery" })) break;
                    if (CastFirst(ai, m, s, { "chain lightning" })) break;
                }
                CastFirst(ai, m, s, { enhance ? "earth shock" : "lightning bolt" });
                break;
            case State::Burn:
            default:
            {
                st = State::Burn;
                if (UseTrinket(ai, m, s)) break;
                if (enhance)
                {
                    if (TotemUpkeep(ai, m, s)) break;
                    // [2026-10-30] 萨满之怒/嗜血/英雄都是自身增益：原来走 CastFirst（对敌目标）
                    //   被判 TARGET_ENEMY(108) ⇒ 整场 0 次。改 TryCastSelf。
                    if (TryCastSelf(ai, m, s, "bloodlust")) break;
                    if (TryCastSelf(ai, m, s, "heroism")) break;
                    if (TryCastSelf(ai, m, s, "shamanistic rage")) break;
                    if (CastFirst(ai, m, s, { "stormstrike" })) break;
                    // 震击共用冷却：烈焰震击常驻，地球震击补位
                    if (AuraStacks(ai, s.bot, m, "flame shock", s.target) == 0 && CastFirst(ai, m, s, { "flame shock" })) break;
                    if (CastFirst(ai, m, s, { "earth shock" })) break;
                    break;   // 其余等普攻/风怒
                }
                if (CastFirst(ai, m, s, { "elemental mastery" })) break;
                if (AuraStacks(ai, s.bot, m, "flame shock", s.target) == 0 && CastFirst(ai, m, s, { "flame shock" })) break;
                if (CastFirst(ai, m, s, { "chain lightning" })) break;
                CastFirst(ai, m, s, { "lightning bolt" });
                break;
            }
        }

        LogTransition(ai, m, s, st);
        return true;
    }

    // ============================ 德鲁�?============================
    bool TickDruid(PlayerbotAI* ai, BotMem* m)
    {
        Snapshot s;
        BuildSnapshot(ai, m, s);
        if (!s.target) { LogTransition(ai, m, s, State::Idle); return false; }   // 交回引擎重选目标
        if (s.casting) return true;

        bool const feral = (m->spec == Spec::Feral);
        if (feral)
        {
            // 豹形态才按猫的循环。原引擎在假人场景不会自己变形（实测整场在外形平砍），
            // 所以这里先用一次变形动作，变形不成就交回引擎。
            uint8 const form = s.bot->GetShapeshiftForm();
            if (form != FORM_CAT)
            {
                // 已经挂着变形光环却仍认为"不在猫形态"时别再刷（实测 40 秒刷了 140 次 Cat Form）
                if (AuraStacks(ai, s.bot, m, "cat form", s.bot) > 0)
                    return false;
                if (TryCastSelf(ai, m, s, "cat form"))
                    return true;
                return false;
            }

            GetBehind(ai, m, s);   // 撕碎要背身（只走位，不占 GCD）
            bool const inRange = s.dist <= 6.5f;   // 近战够得着就行（5.0 太紧，卡在 5.2 会永远不进循环）
            bool const executeWindow = s.targetHp <= BurstHp(m);
            State st = (m->state == State::Idle) ? State::Burn : m->state;
            if (!inRange) st = State::Approach;
            else if (st == State::Approach) st = State::Burn;
            if (st == State::Burn && executeWindow) st = State::Burst;
            else if (st == State::Burst && !executeWindow) st = State::Burn;

            switch (st)
            {
                case State::Approach:
                    break;
                case State::Burst:
                    if (UseTrinket(ai, m, s)) break;
                    if (s.energy >= 80 && CastFirst(ai, m, s, { "tiger's fury" })) break;
                    if (s.combo >= 5 && AuraStacks(ai, s.bot, m, "rip", s.target) == 0
                        && CastFirst(ai, m, s, { "rip" })) break;
                    if (s.combo >= 5 && CastFirst(ai, m, s, { "ferocious bite" })) break;
                    if (s.combo >= 5) break;
                    if (CastFirst(ai, m, s, { "mangle (cat)" })) break;
                    CastFirst(ai, m, s, { "shred" });
                    break;
                case State::Burn:
                default:
                {
                    st = State::Burn;
                    if (UseTrinket(ai, m, s)) break;
                    // [2026-10-30 修] 凶猛之怒(Tiger's Fury) 没有冷却、只花 30 能量，原来排在第一位，
                    //   自增益"打自己"修好以后它把能量全吃光（实测 40 秒 15 次 TF、裂伤只剩 2 次、dps 359）。
                    //   改成"能量快满才放"（等效于把快溢出的能量换成攻强），不跟裂伤/终结技抢能量。
                    if (s.energy >= 80 && CastFirst(ai, m, s, { "tiger's fury" })) break;
                    // [2026-10-30 修] 终结技（割裂/凶猛撕咬）原来排在"攒星技"后面：
                    //   mangle(cat) 没有冷却、能量一够就先被它吃掉，星满了也在继续打裂伤
                    //   ⇒ 实测 40 秒 rip 只有 1 次、凶猛撕咬（Ferocious Bite）**表里根本没有**。
                    //   现在：5 星先泄终结技；割裂没挂先挂割裂，挂上了就改用凶猛撕咬。
                    bool const ripUp = AuraStacks(ai, s.bot, m, "rip", s.target) > 0;
                    if (s.combo >= 5 && !ripUp && CastFirst(ai, m, s, { "rip" })) break;
                    if (s.combo >= 5 && ripUp && CastFirst(ai, m, s, { "ferocious bite" })) break;
                    if (s.combo >= 5) break;      // 星满了在等能量放终结技，别再攒星
                    // 裂伤(�? 的流血易伤先挂，再补扫击/撕碎
                    if (CastFirst(ai, m, s, { "mangle (cat)" })) break;
                    if (AuraStacks(ai, s.bot, m, "rake", s.target) == 0 && CastFirst(ai, m, s, { "rake" })) break;
                    CastFirst(ai, m, s, { "shred" });
                    break;
                }
            }

            LogTransition(ai, m, s, st);
            return true;
        }

        // 平衡
        bool const inRange = s.dist <= 30.0f;
        State st = (m->state == State::Idle) ? State::Burn : m->state;
        if (s.power < 10.0f)
            st = State::Recover;
        else if (st == State::Recover && s.power > 30.0f)
            st = State::Burn;
        if (st != State::Recover && s.pack30 >= AoEPack(m))
            st = State::Aoe;
        else if (st == State::Aoe)
            st = State::Burn;
        if (st == State::Burn && s.targetHp <= BurstHp(m))
            st = State::Burst;
        else if (st == State::Burst && s.targetHp > 20.0f)
            st = State::Burn;
        if (st != State::Recover && st != State::Aoe && !inRange)
            st = State::Approach;
        else if (st == State::Approach && inRange)
            st = State::Burn;

        switch (st)
        {
            case State::Approach:
                break;
            case State::Recover:
                if (UseManaPotion(m, s)) break;
                CastFirst(ai, m, s, { "shoot" });
                break;
            case State::Aoe:
                if (TryCastAt(ai, m, s, "hurricane", s.target->GetPositionX(), s.target->GetPositionY(), s.target->GetPositionZ())) break;
                CastFirst(ai, m, s, { "starfire" });
                break;
            case State::Burst:
                if (UseTrinket(ai, m, s)) break;
                if (CastFirst(ai, m, s, { "force of nature" })) break;
                CastFirst(ai, m, s, { "starfire" });
                break;
            case State::Burn:
            default:
                st = State::Burn;
                if (UseTrinket(ai, m, s)) break;
                if (CastFirst(ai, m, s, { "force of nature" })) break;
                if (AuraStacks(ai, s.bot, m, "moonfire", s.target) == 0 && CastFirst(ai, m, s, { "moonfire" })) break;
                if (AuraStacks(ai, s.bot, m, "insect swarm", s.target) == 0 && CastFirst(ai, m, s, { "insect swarm" })) break;
                if (s.moving && CastFirst(ai, m, s, { "moonfire" })) break;
                CastFirst(ai, m, s, { "starfire" });
                break;
        }

        LogTransition(ai, m, s, st);
        return true;
    }

    // ============================ 圣骑�?============================
    bool TickPaladin(PlayerbotAI* ai, BotMem* m)
    {
        Snapshot s;
        BuildSnapshot(ai, m, s);
        if (!s.target) { LogTransition(ai, m, s, State::Idle); return false; }   // 交回引擎重选目标
        if (s.casting) return true;
        if (m->spec != Spec::Retribution) return false;

        GetBehind(ai, m, s);   // 绕背：只走位，不占这个 GCD   // 找背（不被招架/躲闪）
        // [2026-10-30 修] 近战范围要用**核心的判定**，不能用表面距离：
        //   s.dist <= 6.5 会把"中心距离 5.0 码、其实够不着"的情形也判成在范围内，
        //   结果整场不挥刀、Crusader Strike 一直被拒（实测 melee=0 / 35395:0）。
        bool const inRange = s.bot->CanReachWithMeleeAttack(s.target);
        if (!inRange)
            CloseToMelee(ai, m, s);   // 真够不着就自己贴上去（引擎 reach melee 的 range=0 是零长度路径）
        bool const executeWindow = s.targetHp <= BurstHp(m);
        // [2026-10-29 实测] 很多随机圣骑士没点"命令圣印"（它是天赋），直接按名字取会取不到 ⇒
        //   一路空判、40 秒 0 伤害。这里给一串候选，取第一个"这个 bot 真会"的。
        char const* seals[3] = {
            (s.bot->GetTeam() == HORDE) ? "seal of blood" : "seal of command",
            (s.bot->GetTeam() == HORDE) ? "seal of command" : "seal of blood",
            "seal of righteousness"     // 所有圣骑士都学得到，兜底
        };
        char const* seal = "seal of righteousness";
        for (char const* cand : seals)
        {
            if (SpellId(ai, s.bot, m, cand, false))
            {
                seal = cand;
                break;
            }
        }
        // 圣印：**只续当前这一个**（剩余 < 2s 才续）。
        //   ⚠️ 别把候选圣印做成同一张表：表是"第一条能放就放"，主圣印一次施法失败就会串到下一个，
        //   几个圣印互相顶掉、还白吃 GCD（实测 40 秒：Command 12 次 + Righteousness 10 次 + Blood 1 次，
        //   而十字军打击 0 次、审判 1 次）。
        if (TryTable(ai, m, s, { { seal, s.bot, nullptr, 2000 } }))
            return true;

        State st = (m->state == State::Idle) ? State::Burn : m->state;
        if (!inRange)
            st = State::Approach;
        else if (st == State::Approach)
            st = State::Burn;
        if (st == State::Burn && executeWindow)
            st = State::Burst;
        else if (st == State::Burst && !executeWindow)
            st = State::Burn;

        switch (st)
        {
            case State::Approach:
                break;
            case State::Burst:
                if (UseTrinket(ai, m, s)) break;
                // [2026-10-30 修] 复仇之怒/神圣之怒都是**自身增益**（spell_template.Targets = 0，
                //   没有 DEST/SOURCE_LOCATION 标志），CastFirst 不传 target ⇒ 拿敌人当目标 ⇒
                //   PlayerbotAI::CastSpell 里 CanCastSpell 判成"对敌放增益"失败，整场 0 次。
                if (TryCastSelf(ai, m, s, "avenging wrath")) break;
                if (CastFirst(ai, m, s, { "hammer of wrath" })) break;
                if (CastFirst(ai, m, s, { "judgement" })) break;
                CastFirst(ai, m, s, { "crusader strike" });
                break;
            case State::Burn:
            default:
            {
                st = State::Burn;
                if (UseTrinket(ai, m, s)) break;
                if (TryCastSelf(ai, m, s, "avenging wrath")) break;
                // [2026-10-29 实测] 审判的伤害全靠圣印；圣印没挂上就判 = 白扔 CD
                //   （假人场景实测 40 秒 210 次审判、只有 105 伤害）。这里改成"没圣印只挂圣印"。
                // [2026-10-29 诊断结论] 圣印的光环 id 就是施法 id，而且**必须打在自己身上**
                //   （原来的 CastFirst 没传 target ⇒ 对敌人放 ⇒ 判成负面法术失败 ⇒ 每 tick 重试，
                //     再叠上"失败就 break"，整个 Burn 分支等于空转）。
                uint32 const sealId = SpellId(ai, s.bot, m, seal, false);
                if (sealId && !s.bot->HasAura(sealId))
                {
                    if (TryCastSelf(ai, m, s, seal))
                    {
                        m->sealAt = time(nullptr);
                        break;
                    }
                    // 挂不上（没学/没蓝）也不 break：接着打审判/十字军，别整段空转
                }
                if (CastFirst(ai, m, s, { "judgement" })) break;
                if (CastFirst(ai, m, s, { "crusader strike" })) break;
                // [2026-10-30 修] 奉献（27173）：自身中心的 AoE，两条常规入口都会失败（见 CastSelfCentered）。
                //   它是惩戒唯一的填充技（审判 10s / 十字军 6s 都有空档），不吃这一手 40 秒就白丢 5~6 次。
                //   蓝门限放到 15%：惩戒的审判/十字军在本库里 ManaCost=0，蓝只花在圣印(65)和奉献(660)上，
                //   40% 会白等 GCD；留 15% 足够续一次圣印就行。
                if (s.power > 15.0f && CastSelfCentered(ai, m, s, "consecration")) break;
                break;
            }
        }

        LogTransition(ai, m, s, st);
        return true;
    }

    // ============================ 牧师 ============================
    bool TickPriest(PlayerbotAI* ai, BotMem* m)
    {
        Snapshot s;
        BuildSnapshot(ai, m, s);
        if (!s.target) { LogTransition(ai, m, s, State::Idle); return false; }   // 交回引擎重选目标
        if (s.casting) return true;
        if (m->spec != Spec::Shadow) return false;

        bool const inRange = s.dist <= 30.0f;
        State st = (m->state == State::Idle) ? State::Burn : m->state;
        if (s.power < 10.0f)
            st = State::Recover;
        else if (st == State::Recover && s.power > 30.0f)
            st = State::Burn;
        if (st == State::Burn && s.targetHp <= BurstHp(m))
            st = State::Burst;
        else if (st == State::Burst && s.targetHp > 20.0f)
            st = State::Burn;
        if (st != State::Recover && !inRange)
            st = State::Approach;
        else if (st == State::Approach && inRange)
            st = State::Burn;

        switch (st)
        {
            case State::Approach:
                break;
            case State::Recover:
                CastFirst(ai, m, s, { "shoot" });
                break;
            case State::Burst:
                if (UseTrinket(ai, m, s)) break;
                if (CastFirst(ai, m, s, { "shadow word: death" })) break;
                CastFirst(ai, m, s, { "mind blast" });
                break;
            case State::Burn:
            default:
                st = State::Burn;
                if (UseTrinket(ai, m, s)) break;
                if (AuraStacks(ai, s.bot, m, "shadowform", s.bot) == 0 && TryCastSelf(ai, m, s, "shadowform")) break;
                if (AuraStacks(ai, s.bot, m, "shadow word: pain", s.target) == 0 && CastFirst(ai, m, s, { "shadow word: pain" })) break;
                if (AuraStacks(ai, s.bot, m, "vampiric touch", s.target) == 0 && CastFirst(ai, m, s, { "vampiric touch" })) break;
                if (CastFirst(ai, m, s, { "mind blast" })) break;
                if (CastFirst(ai, m, s, { "shadow word: death" })) break;
                CastFirst(ai, m, s, { "mind flay" });
                break;
        }

        LogTransition(ai, m, s, st);
        return true;
    }
}

namespace ai
{
    namespace dpsfsm
    {
        void SetParamOverride(uint32 guid, char const* key, double value)
        {
            std::lock_guard<std::mutex> lock(s_disabledMutex);
            s_overrides[guid][key] = value;
        }

        void ClearParamOverrides(uint32 guid)
        {
            std::lock_guard<std::mutex> lock(s_disabledMutex);
            s_overrides.erase(guid);
        }

        bool DidCastThisTick(uint32 guid)
        {
            std::lock_guard<std::mutex> lock(s_memMutex);
            auto it = s_mem.find(guid);
            return it != s_mem.end() && it->second && it->second->castThisTick;
        }

        bool SkipEngine(uint32 guid)
        {
            std::lock_guard<std::mutex> lock(s_disabledMutex);
            return s_scenarioTargets.find(guid) != s_scenarioTargets.end();
        }

        // 这个专精是不是近战（决定 bot->Attack 的 meleeAttack 参数：远程职业传 true 会走去平砍）
        bool IsMeleeSpec(Spec spec)
        {
            switch (spec)
            {
                case Spec::Arms:
                case Spec::Fury:
                case Spec::Protection:
                case Spec::Combat:
                case Spec::Assassination:
                case Spec::Subtlety:
                case Spec::Feral:
                case Spec::Enhancement:
                case Spec::Retribution:
                    return true;
                default:
                    return false;
            }
        }

    // [2026-10-31 诊断] 记录"状态机为什么没干活"。定义在 EngageScenarioTarget 之前，
    //   这样各处早退点都能直接调用（上次放到文件末尾导致编译失败）。
    static void NoteBail(Player* bot, char const* why)
    {
        static std::map<uint32, std::pair<std::string, time_t>> s_lastBail;
        if (!bot)
            return;
        time_t const now = time(nullptr);
        uint32 const g = bot->GetGUIDLow();
        auto& e = s_lastBail[g];
        if (e.first == why && e.second && now - e.second < 5)
            return;
        e.first = why;
        e.second = now;
        sLog.outError("[FSM-BAIL] bot=%s why=%s", bot->GetName(), why);
    }

        bool EngageScenarioTarget(PlayerbotAI* ai)
        {
            Player* bot = ai ? ai->GetBot() : nullptr;
            if (!bot || !bot->IsInWorld() || !bot->IsAlive() || bot->GetVictim())
                return false;
            // 骑乘状态 Unit::Attack 直接 false（猎人整场 0 输出的根因）
            if (bot->IsMounted())
                bot->Unmount();

            std::set<ObjectGuid> scenario;
            {
                std::lock_guard<std::mutex> lock(s_disabledMutex);
                auto it = s_scenarioTargets.find(bot->GetGUIDLow());
                if (it == s_scenarioTargets.end())
                    return false;      // 没声明场景 ⇒ 副本里照旧，不自动开怪
                scenario = it->second;
            }

            Map* map = bot->GetMap();
            if (!map)
                return false;
            for (ObjectGuid const& g : scenario)
            {
                Unit* u = map->GetUnit(g);
                if (!u || !u->IsAlive())
                    continue;
                if (bot->GetDistance(u) > 45.0f)
                    continue;
                if (!sServerFacade.IsHostileTo(bot, u))
                    continue;
                // 召唤物常被标成 evade，而 Unit::Attack 对 evade 目标直接 false ⇒ 先清掉再咬
                if (u->GetCombatManager().IsInEvadeMode())
                {
                    u->GetCombatManager().SetEvadeState(EVADE_NONE);
                    u->GetCombatManager().StopEvade();
                }
                bool const melee = IsMeleeSpec((Mem(bot->GetGUIDLow()))->spec);
                bool ok = bot->Attack(u, melee);   // 远程职业必须传 false，否则会走进近战位平砍
                bool const firstTry = ok;
                if (!ok || !bot->GetVictim())
                {
                    // Unit::Attack() 在本核心里有个不对称：bot 还挂在目标的 m_attackers 里、
                    //   但自己的 m_attacking 已经空了 ⇒ _addAttacker 返回 false，Attack 直接失败，
                    //   而且之后每次都失败（实测 shadow 整场 attack=0 / dist=2.0 / mounted=0）。
                    //   先 CombatStop 把这个状态清干净再咬一次（此时本来就不在战斗，停了没副作用）。
                    bot->CombatStopWithPets(true);
                    ok = bot->Attack(u, melee);
                }
                if (!ok || !bot->GetVictim())
                {
                    // 诊断：Attack 失败本身是核心里的 m_attackers 不对称（见 SimScenario 的兜底），
                    //   这里至少让日志能看出来"是 Attack 返回 false，而不是找不到目标"。
                    BotMem* mem = Mem(bot->GetGUIDLow());
                    time_t const now = time(nullptr);
                    if (now - mem->lastEngageLog >= 10)
                    {
                        mem->lastEngageLog = now;
                        sLog.outError("[DPSFSM-ENGAGE] %s 咬不上去: attack=%d(retry=%d) victim=%s mounted=%d "
                            "dist=%.1f alive=%d inworld=%d evade=%d dead=%d flying=%d",
                            bot->GetName(), firstTry ? 1 : 0, ok ? 1 : 0,
                            bot->GetVictim() ? bot->GetVictim()->GetName() : "-",
                            bot->IsMounted() ? 1 : 0, bot->GetDistance(u),
                            u->IsAlive() ? 1 : 0, u->IsInWorld() ? 1 : 0,
                            u->GetCombatManager().IsInEvadeMode() ? 1 : 0,
                            bot->IsAlive() ? 0 : 1, bot->IsFlying() ? 1 : 0);
                    }
                    continue;
                }
                return true;
            }
            return false;
        }

        void SetScenarioTargets(uint32 guid, std::vector<ObjectGuid> const& targets, uint32 runId)
        {
            std::lock_guard<std::mutex> lock(s_disabledMutex);
            if (targets.empty())
            {
                s_scenarioTargets.erase(guid);
                s_scenarioRun.erase(guid);
            }
            else
            {
                s_scenarioTargets[guid] = std::set<ObjectGuid>(targets.begin(), targets.end());
                s_scenarioRun[guid] = runId;
            }
        }

        void ClearScenarioTargets(uint32 guid, uint32 runId)
        {
            std::lock_guard<std::mutex> lock(s_disabledMutex);
            auto it = s_scenarioRun.find(guid);
            if (it == s_scenarioRun.end())
                return;
            if (runId && it->second != runId)
                return;      // 这条是新一轮登记的，别动
            s_scenarioTargets.erase(guid);
            s_scenarioRun.erase(guid);
        }

        int PackCount(Player* bot, float range)
        {
            if (!bot || !bot->GetPlayerbotAI())
                return 0;
            return CountHostilesNear(bot->GetPlayerbotAI(), bot, range);
        }

        void SetBotForceDps(uint32 guid, bool on)
        {
            std::lock_guard<std::mutex> lock(s_disabledMutex);
            if (on)
                s_forceDps.insert(guid);
            else
                s_forceDps.erase(guid);
        }

        void SetBotEnabled(uint32 guid, bool on)
        {
            std::lock_guard<std::mutex> lock(s_disabledMutex);
            if (on)
                s_disabled.erase(guid);
            else
                s_disabled.insert(guid);
        }

        // [2026-10-29 实测] 只按策略名判专精会错：随机 bot 身上同时挂着好几套策略
        //   （druid 判成 restoration、fury 战士其实没学嗜血），于是状态机拿错技能表。
        //   改成**先按天赋技能判**（学会了才有的技能名），判不出来再用策略名兜底。
        Spec DetectSpecByTalents(PlayerbotAI* ai, BotMem* m);

        Spec DetectSpecByTalents(PlayerbotAI* ai, BotMem* m)
        {
            return DetectSpecByTalentsImpl(ai, m);
        }

        Spec DetectSpec(PlayerbotAI* ai)
        {
            Player* bot = ai ? ai->GetBot() : nullptr;
            if (!bot)
                return Spec::Unknown;

            struct Entry { char const* strategy; Spec spec; };
            static Entry const mage[] = { { "fire", Spec::Fire }, { "arcane", Spec::Arcane }, { "frost", Spec::Frost } };
            static Entry const warrior[] = { { "fury", Spec::Fury }, { "arms", Spec::Arms }, { "protection", Spec::Protection } };
            static Entry const rogue[] = { { "assassination", Spec::Assassination }, { "subtlety", Spec::Subtlety }, { "combat", Spec::Combat } };
            static Entry const hunter[] = { { "beast mastery", Spec::BeastMastery }, { "marksmanship", Spec::Marksmanship }, { "survival", Spec::Survival } };
            static Entry const warlock[] = { { "affliction", Spec::Affliction }, { "demonology", Spec::Demonology }, { "destruction", Spec::Destruction } };
            static Entry const shaman[] = { { "enhancement", Spec::Enhancement }, { "elemental", Spec::Elemental }, { "restoration", Spec::Restoration } };
            static Entry const druid[] = { { "feral", Spec::Feral }, { "balance", Spec::Balance }, { "restoration", Spec::Restoration } };
            static Entry const paladin[] = { { "retribution", Spec::Retribution }, { "protection", Spec::Protection }, { "holy", Spec::Holy } };
            static Entry const priest[] = { { "shadow", Spec::Shadow }, { "discipline", Spec::Discipline }, { "holy", Spec::Holy } };

            Entry const* table = nullptr;
            size_t count = 0;
            switch (bot->getClass())
            {
                case CLASS_MAGE:    table = mage;    count = 3; break;
                case CLASS_WARRIOR: table = warrior; count = 3; break;
                case CLASS_ROGUE:   table = rogue;   count = 3; break;
                case CLASS_HUNTER:  table = hunter;  count = 3; break;
                case CLASS_WARLOCK: table = warlock; count = 3; break;
                case CLASS_SHAMAN:  table = shaman;  count = 3; break;
                case CLASS_DRUID:   table = druid;   count = 3; break;
                case CLASS_PALADIN: table = paladin; count = 3; break;
                case CLASS_PRIEST:  table = priest;  count = 3; break;
                default: return Spec::Unknown;
            }

            // [2026-10-29 实测] 优先级：①只开了一个专精策略 ⇒ 就是它（假人场景会强制成唯一一个，
            //   否则 "holy 天赋的圣骑士" 会被天赋判定带偏，整场不放技能）；
            //   ②天赋标志技能；③开着的第一个专精策略；④职业默认单体循环。
            int enabled = 0;
            Spec onlyEnabled = Spec::Unknown;
            for (size_t i = 0; i < count; ++i)
            {
                if (ai->HasStrategy(table[i].strategy, BotState::BOT_STATE_COMBAT)
                    || ai->HasStrategy(table[i].strategy, BotState::BOT_STATE_NON_COMBAT))
                {
                    ++enabled;
                    onlyEnabled = table[i].spec;
                }
            }
            if (enabled == 1)
                return onlyEnabled;

            Spec const byTalent = DetectSpecByTalents(ai, Mem(bot->GetGUIDLow()));
            if (byTalent != Spec::Unknown)
                return byTalent;

            if (enabled > 1)
                return onlyEnabled;
            // [2026-10-29 实测] 随机 bot 天赋随机，很多人点不出"标志技能"（术士没痛苦无常、
            //   牧师没暗影形态）。这时别什么都不做：按职业给一套只用基础技能的通用单体循环。
            switch (bot->getClass())
            {
                case CLASS_MAGE:    return Spec::Fire;
                case CLASS_WARRIOR: return Spec::Arms;
                case CLASS_ROGUE:   return Spec::Combat;
                case CLASS_HUNTER:  return Spec::BeastMastery;
                case CLASS_WARLOCK: return Spec::Affliction;
                case CLASS_SHAMAN:  return Spec::Enhancement;
                case CLASS_DRUID:   return Spec::Feral;
                case CLASS_PALADIN: return Spec::Retribution;
                case CLASS_PRIEST:  return Spec::Shadow;
                default: break;
            }
            return Spec::Unknown;
        }

        // [2026-10-30 站长口径] 职业=技能空间、专精=策略；实现见 DpsPolicy.inc（纯文本包含，便于与旧处理器 A/B）
    // [2026-10-30] 顺序很重要：DpsValue 必须在 DpsPolicy 之前包含（表内打分调用 dpsvalue::ValueOfId）
#include "playerbot/strategy/DpsValue.inc"
#include "playerbot/strategy/DpsPolicy.inc"




    // [2026-10-30 站长口径] **所有物理近战输出都要找背**（找背是站位问题，跟具体技能无关）；
    //   但"找背"不等于一直调位置：站到背后之后就尽量不动（走动会打断挥击，实测平砍 78→54）。
    bool NeedsBehind(Player* bot)
    {
        if (!bot)
            return false;
        switch (bot->getClass())
        {
            case CLASS_WARRIOR:
            case CLASS_ROGUE:
            case CLASS_PALADIN:
            case CLASS_SHAMAN:
            case CLASS_DRUID:
                return true;
            default:
                return false;
        }
    }

    // [2026-10-30 诊断] 引擎这一 tick 真的跑了 ⇒ 计数（对比 policy=0/1 时引擎被让位的比例）
    void NoteEngineRun(uint32 guid)
    {
        std::lock_guard<std::mutex> lock(s_memMutex);
        auto it = s_mem.find(guid);
        if (it != s_mem.end())
            ++it->second->engRuns;
    }

    // ==========================================================================
    // [2026-10-31 S2] 冰法状态图（试点，mode=5 才用；其它专精仍走原路径）
    //   图见 dev/DESIGN_dps_fsm_graphs.md §2.1
    //   规矩：① 状态只决定"用哪张动作表"，不直接施法；
    //         ② 转移条件只读 Snapshot + TP 参数，不读世界、不打日志；
    //         ③ 没放出去就 return false，把走位/追击交回引擎（共同作用，不是抢活）。
    // ==========================================================================
    State EvalFrost(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        if (!s.target)                                     return State::Idle;

        // [2026-10-31 站长口径] 先保命：血太低先撤（风筝），不做"站桩换血"
        if (s.selfHp < float(TP(m, "flee_hp", 30.0)))
            return State::Flee;

        if (s.power < float(TP(m, "recover_enter", 8.0)))  return State::Recover;
        if (s.pack30 >= AoEPack(m))                        return State::Aoe;

        // [2026-10-31 站长口径] 爆发 = 仇恨稳定后**立刻开**、早用早 CD（不再等目标血量低）。
        //   单 bot 用时间间隔当"CD 门"，`burst_interval` 可调（越小越激进）。
        //   假人场景没有仇恨（SkipEngine 为真），视为仇恨稳定。
        bool const threatOk = dpsfsm::SkipEngine(s.bot->GetGUIDLow())
            || !AttackAction::WaitingForTankThreat(ai, s.bot, s.target);
        if (threatOk && time(nullptr) - m->lastBurst >= time_t(TP(m, "burst_interval", 120.0)))
            return State::Burst;

        if (s.dist > float(TP(m, "range_caster", 30.0)))   return State::Approach;
        return State::Burn;
    }

    bool TickFrostFsm(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        State const st = EvalFrost(ai, m, s);
        m->state = st;

        switch (st)
        {
            case State::Recover:
                if (TryCastSelf(ai, m, s, "evocation"))
                    return true;
                return false;               // 剩下的（喝水/魔杖）交回引擎

            case State::Flee:
                if (CastSelfCentered(ai, m, s, "frost nova"))
                    return true;
                if (TryCastSelf(ai, m, s, "ice barrier"))
                    return true;
                if (TryCastSelf(ai, m, s, "blink"))
                    return true;
                return CastFirst(ai, m, s, { "ice lance" });

            case State::Approach:
                return false;               // 走位归引擎

            case State::Aoe:
                if (TryCastAt(ai, m, s, "blizzard", s.target->GetPositionX(),
                        s.target->GetPositionY(), s.target->GetPositionZ()))
                    return true;
                if (CastFirst(ai, m, s, { "ice lance" }))
                    return true;
                return CastFirst(ai, m, s, { "frostbolt" });

            case State::Burst:
                m->lastBurst = time(nullptr);      // 早用早 CD：进来就打点，别每 tick 重判
                if (!s.bot->GetPet() && CastFirst(ai, m, s, { "summon water elemental" }))
                    return true;
                if (CastFirst(ai, m, s, { "icy veins" }))
                    return true;
                if (CastFirst(ai, m, s, { "cold snap" }))
                    return true;
                return CastFirst(ai, m, s, { "frostbolt" });

            case State::Idle:
            default:
                break;
        }
        return CastFirst(ai, m, s, { "frostbolt" });
    }

    bool Tick(PlayerbotAI* ai)
        {
            if (!ai || !ai->GetBot())
                return false;

            Player* bot = ai->GetBot();
            if (!bot->IsInWorld() || !bot->IsAlive())
            {
                NoteBail(bot, !bot->IsInWorld() ? "not-in-world" : "dead");
                return false;
            }
            {
                std::lock_guard<std::mutex> lock(s_disabledMutex);
                if (s_disabled.count(bot->GetGUIDLow()))
                {
                    NoteBail(bot, "disabled");
                    return false;
                }
            }
            BotMem* m = Mem(bot->GetGUIDLow());
            m->castThisTick = false;
            ReloadTune();

            uint32 const hbNow = WorldTimer::getMSTime();
            bool const hbDue = (!m->hbLast || WorldTimer::getMSTimeDiff(m->hbLast, hbNow ? hbNow : 1) >= 10000);
            if (hbDue)
                m->hbLast = hbNow ? hbNow : 1;

            // [2026-10-29 诊断结论] 原来只认 IsInCombat()：本核心里"有目标但还没造成/受到伤害"不算战斗，
            //   于是远程 bot 永远不挥刀 -> 不进战斗 -> 状态机拒绝运行 -> 死锁（实测整场 incombat=0）。
            //   只要有可攻击目标就进来，由状态机自己放出第一发伤害技能来起战斗。
            // 假人场景：没有 victim 就自己咬上去（否则远程 bot 永远不造成伤害 ⇒ 不进战斗 ⇒ 死锁）
            if (!bot->GetVictim())
                EngageScenarioTarget(ai);

            if (!bot->IsInCombat() && !bot->GetVictim())
            {
                if (m->state != State::Idle)
                    m->state = State::Idle;
                NoteBail(bot, "no-victim");
                return false;
            }

            // 被控�?被打断到不能自己行动时，交回引擎（解控、逃跑、驱散都更重要）
            if (bot->hasUnitState(UNIT_STAT_STUNNED | UNIT_STAT_FLEEING | UNIT_STAT_CONFUSED | UNIT_STAT_ROOT))
            {
                NoteBail(bot, "stunned/rooted");
                return false;
            }

            // [2026-10-29] 坦/奶不归 DPS 状态机管（副本里奶和坦另有职责，这里只做输出）
            {
                bool forced = false;
                {
                    std::lock_guard<std::mutex> lock(s_disabledMutex);
                    forced = s_forceDps.count(bot->GetGUIDLow()) != 0;
                }
                if (!forced && (PlayerbotAI::IsHeal(bot) || ai->IsTank(bot)))
                    return false;
            }

            // 专精每 5 秒重判一次：假人场景/命令行动态换专精时，缓存的旧专精会让技能表全错
            {
                time_t const nowT = time(nullptr);
                if (m->spec == Spec::Unknown || !m->specCheckedAt || nowT - m->specCheckedAt >= 5)
                {
                    m->specCheckedAt = nowT;
                    m->spec = DetectSpec(ai);
                }
            }

            // [2026-10-29 站长口径] 之前做的仇恨门不能因为状态机被绕过：
            //   门禁与 AttackAction 完全同一份实现（WaitingForTankThreat），不复制判断。
            // [2026-10-31 修] 假人场景**必须绕过**这道门：假人没有仇恨，`WaitingForTankThreat`
            //   会一直为真 ⇒ 状态机 return false、引擎的 AttackAction 也在等 ⇒ **互相推诿的死锁**。
            //   实测表现：bot 有目标、canattack=1、站 17.4 码，但 40 秒 0 施法 0 伤害（约 1/3 轮次）。
            if (!dpsfsm::SkipEngine(bot->GetGUIDLow())
                && AttackAction::WaitingForTankThreat(ai, bot, bot->GetVictim()))
                return false;

            ++m->tickCount;
            if (bot->IsMoving())
                ++m->movTicks;

            // [2026-10-30] 覆盖率断言：一次运行里每只 bot 打一条"表外候选"日志（漏配立刻可见）
            if (dpspolicy::PPolicy const* covPol = dpspolicy::PolicyFor(bot->getClass(), m->spec))
                dpspolicy::LogCoverage(ai, m, bot, covPol);

            // [2026-10-31 S1] 决策模式在这里**唯一一次**解析（新键 `mode` 优先；旧四键只用于
            //   自动迁移并给一条 WARN）。TickPolicy 读 m->mode，不再自己去看 pselect/pauto。
            bool derivedFromLegacy = false;
            DecisionMode const dmode = ResolveMode(m, derivedFromLegacy);
            m->mode = int(dmode);
            if (!m->modeLogged)
            {
                m->modeLogged = true;
                sLog.outError("[DPSFSM-MODE] %s spec=%s mode=%d(%s)%s",
                    bot->GetName(), SpecName(m->spec), int(dmode), DecisionModeName(dmode),
                    derivedFromLegacy
                        ? " WARN:旧键 value/policy/pselect/pauto 已废弃，请改成 mode=N（见 dev/DESIGN_dps_fsm.md）"
                        : "");
            }

            if (hbDue)
            {
                // [2026-10-30] 心跳，10 秒一条；pol/eng/mov/melee 用于定位"白字平砍变少"。
                //   [2026-10-31 S1] pol 改为"这一轮是否走策略表"（mode=1/2/3）——原先是读 policy 布尔，
                //   现在只是 DecisionMode 的一个投影；字段名保持不变，避免打断既有解析脚本。
                sLog.outError("[DPSFSM-HB] %s spec=%s state=%s pol=%d incombat=%d target=%s hp=%.0f res=%.0f auras=%u dup=%u ticks=%u casts=%u eng=%u mov=%u melee=%u",
                    bot->GetName(), SpecName(m->spec), StateName(m->state),
                    ModeIsTable(dmode) ? 1 : 0, bot->IsInCombat() ? 1 : 0,
                    bot->GetVictim() ? bot->GetVictim()->GetName() : "-", bot->GetHealthPercent(), bot->GetPowerPercent(),
                    bot->GetVictim() ? uint32(bot->GetVictim()->GetSpellAuraHolderMap().size()) : 0u,
                    m->dupDebuff, m->tickCount, m->castCount, m->engRuns, m->movTicks, bot->botMeleeHits);

                // [2026-10-31 S1] 唯一一处"语义会变"的旧组合：value=1 且 policy=1。
                //   旧代码是 value 先跑、失败后落 policy（两段串联）；新口径只保留 value 引擎。
                //   仓库里没有任何配置这样配过（conf 与 arms 都不带 value=），但要留痕，别让人以为是静默改行为。
                if (derivedFromLegacy && TP(m, "value", 0.0) >= 1.0 && TP(m, "policy", 0.0) >= 1.0)
                    sLog.outError("[DPSFSM-MODE] %s WARN:value=1 与 policy=1 同时设置在新口径里不再串联，已按 mode=4(value) 处理",
                        bot->GetName());
            }

            // [2026-10-30] 价值驱动：mode=4 时由价值引擎选技能；
            //   它没有伤害技能可用（或都放不出）时返回 false，落回下面做 buff/圣印/形态维护。
            if (dmode == DecisionMode::Value)
            {
                if (TickValue(ai, m))
                    return true;
                NoteBail(bot, "value-engine-declined");
            }

            // [2026-10-30] 职业技能空间 + 专精策略（mode=1/2/3 开启；默认 legacy 走原分专精处理器）
            if (ModeIsTable(dmode))
                return TickPolicy(ai, m);

            // [2026-10-31 S2] mode=5：显式状态图。目前只有冰法有图（试点），
            //   其它专精还没有图 ⇒ 继续往下走手写处理器，行为与 legacy 相同。
            if (dmode == DecisionMode::Fsm && m->spec == Spec::Frost)
            {
                Snapshot s;
                BuildSnapshot(ai, m, s);
                if (TickFrostFsm(ai, m, s))
                    return true;
                return false;   // 没出手 ⇒ 交回引擎（走位/追击/开怪）
            }

            bool handled = false;
            switch (m->spec)
            {
                case Spec::Fire:
                case Spec::Frost:
                case Spec::Arcane:
                    handled = TickMage(ai, m);
                    break;

                case Spec::Arms:
                case Spec::Fury:
                case Spec::Protection:
                    handled = TickWarrior(ai, m);
                    break;

                case Spec::Combat:
                case Spec::Assassination:
                case Spec::Subtlety:
                    handled = TickRogue(ai, m);
                    break;

                case Spec::BeastMastery:
                case Spec::Marksmanship:
                case Spec::Survival:
                    handled = TickHunter(ai, m);
                    break;

                case Spec::Affliction:
                case Spec::Demonology:
                case Spec::Destruction:
                    handled = TickWarlock(ai, m);
                    break;

                case Spec::Enhancement:
                case Spec::Elemental:
                case Spec::Restoration:
                    handled = TickShaman(ai, m);
                    break;

                case Spec::Feral:
                case Spec::Balance:
                    handled = TickDruid(ai, m);
                    break;

                case Spec::Retribution:
                case Spec::Holy:
                    handled = TickPaladin(ai, m);
                    break;

                case Spec::Shadow:
                case Spec::Discipline:
                    handled = TickPriest(ai, m);
                    break;

                default:
                    break;
            }

            if (handled)
                return true;

            if (m->flags.insert("unimplemented logged").second)
                sLog.outError("[DPSFSM] %s 职业/专精 %s 状态机还没写，先回退原引擎",
                    bot->GetName(), SpecName(m->spec));
            return false;
        }
    }
}
