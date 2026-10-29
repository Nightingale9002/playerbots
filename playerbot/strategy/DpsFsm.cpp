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
        bool castThisTick = false;             // 本 tick 状态机是否真的放出了技能
        time_t lastEngageLog = 0;              // "咬不上去"日志节流
        time_t lastBehindTry = 0;              // 上一次尝试绕背的时间（别每 tick 都动）
        time_t lastCloseTry = 0;               // 上一次"真够不着就贴上去"的时间
        time_t lastConsLog = 0;                // "自身中心 AoE 放不出"日志节流
        time_t lastPotionTry = 0;              // 上一次尝试喝药水的时间
        time_t lastSummary = 0;
        uint32 castCount = 0;
        uint32 tickCount = 0;
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

    double Tune(char const* spec, char const* key, double def)
    {
        std::lock_guard<std::mutex> lock(s_tuneMutex);
        auto it = s_tune.vals.find(std::string(spec) + ":" + key);
        if (it != s_tune.vals.end())
            return it->second;
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

    int   AoEPack(BotMem* m)  { return int(TP(m, "aoe_pack", 3.0)); }
    float AoERange(BotMem* m) { return float(TP(m, "aoe_range", 11.0)); }
    float BurstHp(BotMem* m)  { return float(TP(m, "burst_hp", 20.0)); }

    // 名字 → id：先问引擎（它已经处理了中英文/DBC 名称匹配），再顶到"已学会的最高 rank"。

    // 省蓝等级 > 1 时引擎会给最低 rank，所以这里一定要自己往上爬 spell_chain。

    uint32 SpellId(PlayerbotAI* ai, Player* bot, BotMem* m, char const* name, bool warn = true)
    {
        auto it = m->ids.find(name);
        if (it != m->ids.end())
            return it->second;

        uint32 id = 0;
        if (auto* value = ai->GetAiObjectContext()->GetValue<uint32>("spell id", name))
            id = value->Get();

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

        s.casting = bot->IsNonMeleeSpellCasted(false);
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
        SpellCastResult res = SPELL_FAILED_DONT_REPORT;
        if (!ai->CanCastSpell(id, tgt, 0, false, nullptr, false, false, false, &res))
        {
            // [2026-10-29 实测] 一律退 2 秒会把怒气/能量职业的 DPS 砍半：
            //   "蓝/怒/能量不够"是**等一下就够**，只退 0.4 秒；射程/视线/其它条件才退 2 秒。
            bool const waitingResource = (res == SPELL_FAILED_NO_POWER
                || res == SPELL_FAILED_OUT_OF_RANGE || res == SPELL_FAILED_LINE_OF_SIGHT
                || res == SPELL_FAILED_NOT_READY);
            m->failUntil[id] = now + (waitingResource ? 1 : 2);
            return false;
        }
        if (!ai->CastSpell(id, tgt))
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

    bool TryCastAt(PlayerbotAI* ai, BotMem* m, Snapshot& s, char const* name, float x, float y, float z)
    {
        uint32 id = SpellId(ai, s.bot, m, name);
        if (!Ready(s.bot, id))
            return false;
        if (!ai->CastSpell(id, x, y, z))
            return false;
        ++m->castCount;
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
                // 唤醒（8 分钟一次）优先，其次魔杖

                if (!CastFirst(ai, m, s, { "evocation" }))
                    CastFirst(ai, m, s, { "shoot" });
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

                    int const blastStacks = AuraStacks(ai, s.bot, m, "arcane blast", s.bot);
                    if (blastStacks >= 3 && CastFirst(ai, m, s, { "arcane missiles" })) break;
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
                if (fury && CastFirst(ai, m, s, { "death wish" })) break;

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
                if (!fury && s.rage >= slamCost && CastFirst(ai, m, s, { "slam" })) break;
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
                if (mutilate && CastFirst(ai, m, s, { "cold blood" })) break;
                if (!mutilate && CastFirst(ai, m, s, { "adrenaline rush" })) break;
                if (CastFirst(ai, m, s, { "blade flurry" })) break;
                break;
            case State::Burn:
            default:
            {
                st = State::Burn;
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
                if (TryTable(ai, m, s, {
                        { "slice and dice", s.bot, nullptr, 2500, nullptr, sndCp, 0 } }))
                    break;
                if (s.combo >= finCp && s.energy >= evisEnergy
                    && CastFirst(ai, m, s, { finisher })) break;
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
                if (CastFirst(ai, m, s, { "seed of corruption" })) break;
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
                    if (AuraStacks(ai, s.bot, m, "immolate", s.target) == 0 && CastFirst(ai, m, s, { "immolate" })) break;
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
                    if (CastFirst(ai, m, s, { "bloodlust" })) break;
                    if (CastFirst(ai, m, s, { "heroism" })) break;
                    if (CastFirst(ai, m, s, { "shamanistic rage" })) break;
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
                    if (CastFirst(ai, m, s, { "bloodlust" })) break;
                    if (CastFirst(ai, m, s, { "heroism" })) break;
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
                    if (CastFirst(ai, m, s, { "tiger's fury" })) break;
                    if (s.combo >= 5 && CastFirst(ai, m, s, { "rip" })) break;
                    if (CastFirst(ai, m, s, { "mangle (cat)" })) break;
                    CastFirst(ai, m, s, { "shred" });
                    break;
                case State::Burn:
                default:
                {
                    st = State::Burn;
                    if (UseTrinket(ai, m, s)) break;
                    if (CastFirst(ai, m, s, { "tiger's fury" })) break;
                    // 裂伤(�? 的流血易伤先挂，再割裂/扫击
                    if (CastFirst(ai, m, s, { "mangle (cat)" })) break;
                    if (AuraStacks(ai, s.bot, m, "rake", s.target) == 0 && CastFirst(ai, m, s, { "rake" })) break;
                    if (s.combo >= 5 && CastFirst(ai, m, s, { "rip" })) break;
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

        bool Tick(PlayerbotAI* ai)
        {
            if (!ai || !ai->GetBot())
                return false;

            Player* bot = ai->GetBot();
            if (!bot->IsInWorld() || !bot->IsAlive())
                return false;
            {
                std::lock_guard<std::mutex> lock(s_disabledMutex);
                if (s_disabled.count(bot->GetGUIDLow()))
                    return false;
            }
            BotMem* m = Mem(bot->GetGUIDLow());
            m->castThisTick = false;
            ReloadTune();

            uint32 const hbNow = WorldTimer::getMSTime();
            if (!m->hbLast || WorldTimer::getMSTimeDiff(m->hbLast, hbNow ? hbNow : 1) >= 10000)
            {
                m->hbLast = hbNow ? hbNow : 1;
                sLog.outError("[DPSFSM-HB] %s spec=%s state=%s incombat=%d target=%s hp=%.0f res=%.0f ticks=%u casts=%u",
                    bot->GetName(), SpecName(m->spec), StateName(m->state), bot->IsInCombat() ? 1 : 0,
                    bot->GetVictim() ? bot->GetVictim()->GetName() : "-", bot->GetHealthPercent(), bot->GetPowerPercent(),
                    m->tickCount, m->castCount);
            }

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
                return false;
            }

            // 被控�?被打断到不能自己行动时，交回引擎（解控、逃跑、驱散都更重要）
            if (bot->hasUnitState(UNIT_STAT_STUNNED | UNIT_STAT_FLEEING | UNIT_STAT_CONFUSED | UNIT_STAT_ROOT))
                return false;

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
            if (AttackAction::WaitingForTankThreat(ai, bot, bot->GetVictim()))
                return false;

            ++m->tickCount;

            switch (m->spec)
            {
                case Spec::Fire:
                case Spec::Frost:
                case Spec::Arcane:
                    return TickMage(ai, m);

                case Spec::Arms:
                case Spec::Fury:
                case Spec::Protection:
                    return TickWarrior(ai, m);

                case Spec::Combat:
                case Spec::Assassination:
                case Spec::Subtlety:
                    return TickRogue(ai, m);

                case Spec::BeastMastery:
                case Spec::Marksmanship:
                case Spec::Survival:
                    return TickHunter(ai, m);

                case Spec::Affliction:
                case Spec::Demonology:
                case Spec::Destruction:
                    return TickWarlock(ai, m);

                case Spec::Enhancement:
                case Spec::Elemental:
                case Spec::Restoration:
                    return TickShaman(ai, m);

                case Spec::Feral:
                case Spec::Balance:
                    return TickDruid(ai, m);

                case Spec::Retribution:
                case Spec::Holy:
                    return TickPaladin(ai, m);

                case Spec::Shadow:
                case Spec::Discipline:
                    return TickPriest(ai, m);

                default:
                    break;
            }

            if (m->flags.insert("unimplemented logged").second)
                sLog.outError("[DPSFSM] %s 职业/专精 %s 状态机还没写，先回退原引擎",
                    bot->GetName(), SpecName(m->spec));
            return false;
        }
    }
}
