
#include "playerbot/playerbot.h"
#include "PossibleAttackTargetsValue.h"
#include "PossibleTargetsValue.h"

#include "playerbot/ServerFacade.h"
#include "playerbot/WorldPosition.h"
#include "playerbot/DungeonRoute.h"
#include "Grids/GridNotifiers.h"
#include "Grids/GridNotifiersImpl.h"
#include "Grids/CellImpl.h"
#include "AttackersValue.h"
#include "RtiTargetValue.h"
#include "EnemyPlayerValue.h"
#include "ThreatValues.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace ai;
using namespace MaNGOS;

// ===========================================================================
// [aggro 2026-10-17 站长口径] **攻略点名的重点小怪**表（数据优先于一切启发式）。
//
// 表：`tbcmangos.ai_playerbot_priority_target`（现 dev/182_机器人相关_整合.sql）
//   entry / priority / source / reason
//   [2026-10-19 口径·定稿] 数字越大越优先，含义分档（站长原文）：
//     priority 5 = 任何情况必须**立刻转火击杀**（压过一切；吃控制也不降级）
//     priority 3 = 有读条类控制技能、瞬发治疗、不可驱散伤害类 dot、本身高伤害或者有高伤害技能
//                  ⇒ **优先控制，如果控制不够则优先击杀**
//     priority 2 = 有伤害类 dot，**没有对应驱散**的情况下优先击杀（队里有对应驱散 ⇒ 不优先）
//     priority 1 = 有读条类治疗，只需要**打断**；打断好就不用优先击杀（击杀优先级 0，交给打断逻辑）
//     priority 4 = **优先控制，最后击杀**
//     通用规则：**吃控制的怪，控制能替我们处理 ⇒ 优先级降到最后，不能打醒控制**
//
// 为什么要有它（站长原话）：**"并不是每个有技能的怪都是高威胁"** —— 所以高威胁名单不能靠
//   引擎扫技能表推出来（那样会把 487 个"有治疗/召唤"的怪全算进去），必须用**人工校订过的攻略名单**。
//   引擎自己的"观察它施放过高威胁技能"只作为**名字不在表里**时的回落。
//
// 口径映射（表里的 priority 越大越优先，与 ThreatTier 同级语义）：
//   表 priority 5 -> ThreatTier 5（不降级，必须立刻杀）
//   表 priority 3 -> ThreatTier 4/3（吃控制降 1 级；控制够就先控，控制不够则优先击杀）
//   表 priority 2 -> ThreatTier 0/3（队里有驱散 ⇒ 0；否则 3，吃控制再降）
//   表 priority 1 -> ThreatTier 0（只交给打断逻辑）
//   表 priority 4 -> ThreatTier 1（击杀放到最后；优先控制由 CcTargetValue 负责）
//
// 刷新：每 kPriorityRefreshSeconds 秒重读一次 ⇒ 改数据不用重启（与 BossStrategyHook 同范式）。
//   多地图线程会同时调到这里 ⇒ 用 mutex；读路径走 thread_local 缓存，避免每 tick 抢锁。
// 表不存在 / 查询失败：**静默回落**到启发式（本表是可选增强，缺了不能影响出怪）。
// ===========================================================================
namespace
{
    const uint32 kPriorityRefreshSeconds = 300;

    std::mutex priorityMutex;
    std::map<uint32, uint8> priorityByEntry;   // entry -> 表里的 priority（1 最高）
    time_t priorityLoadedAt = 0;
    bool priorityLoadTried = false;

    // [2026-10-20 站长口径] `.bot prio` 要"实时生效"：每次手动改优先级就 ++ 这个 epoch，
    //   PriorityTargetRank() 的 thread_local 读缓存里带着 epoch，对不上就当场重算
    //   （否则改完要等 kPriorityRefreshSeconds 才生效）。
    std::atomic<uint32> priorityEpoch{0};

    // ===========================================================================
    // 观测层：**引擎真的看见它读过的条**（治疗 / 控制）→ 计数 → 生效 → 落库。
    //
    // 站长口径（2026-10-18）：
    //   ① "对于怪物所有读条的治疗都标记为优先级[最高]，对于读条的控制技能，包括击晕、魅惑、恐惧
    //      等等标记为[优先击杀]，不包括缠绕"；
    //   ② "ai_playerbot_priority_obs 表 + 观测持久化，每个变化加日志"。
    //
    // 按**现行**等级口径（数字越大越优先，5 = 任何情况必须立刻转火杀）换算：
    //   读条治疗 → priority 5（不打掉它，其它目标都打不死）
    //   读条控制 → priority 3（优先击杀；缠绕/定身、减速、缴械**不算**控制）
    //
    // 为什么不猜静态技能表：本站怪物的技能在 `creature_template.SpellList` 模板里，
    //   静态展开要处理模板继承、还把"会不会"当成"此刻在不在读"，容易把几百只有治疗技能的怪
    //   全算成最高优先。这里与 HasHighThreatAbility 同思路：**它真的读过**才算证据。
    //
    // 优先级来源（高 → 低）：人工表 `ai_playerbot_priority_target` > 观测表 > 启发式。
    //   观测只补"人工表里没有这只怪"的 entry。
    // 生效门槛：同一 entry 同一类观察到 >= 2 次（一次可能是被打断的残条 / 偶然）。
    // 持久化：count 落 `tbccharacters.ai_playerbot_priority_obs`（见 dev/193），
    //   `ON DUPLICATE KEY UPDATE count = GREATEST(count, VALUES(count))` ⇒ 只增 + 重复落库幂等；
    //   进程重启后从库里读回来，不用重新攒。表不存在 ⇒ 只记内存（可选增强，缺了不能影响出怪）。
    // ===========================================================================
    const uint32 kPriorityObsMinCount = 2;          // 观察几次才算数
    const uint32 kPriorityObsTtlSeconds = 600;      // 内存里"还没生效"的条目多久没再看到就清
    const uint32 kPriorityObsFlushSeconds = 120;    // 定时落库间隔（拉怪结束时也会落一次）
    const size_t kPriorityObsMaxEntries = 4096;     // 内存条目上限（防无限增长）
    const uint8 kPriorityObsHealTier = 5;           // 读条治疗 ⇒ 表 priority 5
    const uint8 kPriorityObsControlTier = 3;        // 读条控制 ⇒ 表 priority 3

    enum PriorityObsKind : uint8
    {
        PRIO_OBS_HEAL = 1,          // 读条治疗
        PRIO_OBS_CONTROL = 2,       // 读条控制（击晕/魅惑/恐惧/沉默/变形/放逐…）
    };

    struct PriorityObsRecord
    {
        uint32 count;
        time_t lastSeen;
        uint32 lastMap;
        uint32 lastSpell;
        time_t effectiveAt;         // 首次达到生效门槛的时刻（0 = 还没生效）

        PriorityObsRecord() : count(0), lastSeen(0), lastMap(0), lastSpell(0), effectiveAt(0) {}
    };

    std::mutex obsMutex;
    std::map<std::pair<uint32, uint8>, PriorityObsRecord> obsByKey;  // (entry, kind) -> 观测
    std::set<std::pair<uint32, uint8>> obsDirty;                     // 待落库的 key
    bool obsLoaded = false;
    time_t obsLastFlush = 0;
    bool obsTableChecked = false;
    bool obsTableExists = false;
    time_t obsTableCheckedAt = 0;

    const char* PriorityObsKindName(uint8 kind)
    {
        return kind == PRIO_OBS_CONTROL ? "control" : "heal";
    }

    uint8 PriorityObsKindTier(uint8 kind)
    {
        return kind == PRIO_OBS_CONTROL ? kPriorityObsControlTier : kPriorityObsHealTier;
    }

    std::string PriorityObsCreatureName(uint32 entry)
    {
        CreatureInfo const* info = ObjectMgr::GetCreatureTemplate(entry);
        if (!info || !info->Name || !info->Name[0])
            return "?";
        return info->Name;
    }

    // 表存在性（可选增强：缺表不能影响出怪/推图）
    bool PriorityObsTableExists()
    {
        std::scoped_lock lock(obsMutex);
        const time_t now = time(nullptr);
        if (obsTableChecked && (now - obsTableCheckedAt) < (time_t)kPriorityRefreshSeconds)
            return obsTableExists;

        obsTableChecked = true;
        obsTableCheckedAt = now;

        std::unique_ptr<QueryResult> res = CharacterDatabase.Query(
            "SELECT 1 FROM information_schema.tables WHERE table_schema = DATABASE() "
            "AND table_name = 'ai_playerbot_priority_obs'");
        obsTableExists = (res != nullptr);
        if (!obsTableExists)
        {
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                sLog.outError("[PRIO-DB] ai_playerbot_priority_obs 不存在 —— 观测只记内存、不落库"
                              "（应用 dev/193_优先级观测表.sql 可启用持久化）");
            }
        }
        return obsTableExists;
    }

    // 从 chars 库把历史观测读回来（只在首次使用时读一次；之后内存是权威，落库用 GREATEST 合并）
    void LoadPriorityObs()
    {
        if (!PriorityObsTableExists())
            return;

        std::unique_ptr<QueryResult> results = CharacterDatabase.Query(
            "SELECT `entry`,`kind`,`count`,`last_seen`,`last_map`,`last_spell` FROM `ai_playerbot_priority_obs`");
        if (!results)
            return;

        std::scoped_lock lock(obsMutex);
        do
        {
            Field* fields = results->Fetch();
            const uint32 entry = fields[0].GetUInt32();
            if (!entry)
                continue;

            // ENUM 列：只有 'heal' / 'control' 两种。驱动一般返回字符串，这里再按数值兜一层
            //   （2 = control），保证换驱动/换连接串时不会把控制观测错记成治疗。
            const std::string kindStr = fields[1].GetString() ? fields[1].GetString() : "";
            const bool isControl = (kindStr == "control" || kindStr == "CONTROL" ||
                                    fields[1].GetUInt32() == uint32(PRIO_OBS_CONTROL));
            const uint8 kind = isControl ? uint8(PRIO_OBS_CONTROL) : uint8(PRIO_OBS_HEAL);

            PriorityObsRecord& rec = obsByKey[std::make_pair(entry, kind)];
            rec.count = std::max(rec.count, fields[2].GetUInt32());
            rec.lastSeen = std::max(rec.lastSeen, time_t(fields[3].GetUInt32()));
            rec.lastMap = fields[4].GetUInt32();
            rec.lastSpell = fields[5].GetUInt32();
            if (!rec.effectiveAt && rec.count >= kPriorityObsMinCount)
                rec.effectiveAt = rec.lastSeen;
        } while (results->NextRow());

        sLog.outString("[PRIO-DB] 载入历史观测 %u 条（entry×kind）", (uint32)obsByKey.size());
    }

    void EnsurePriorityObsLoaded()
    {
        {
            std::scoped_lock lock(obsMutex);
            if (obsLoaded)
                return;
            obsLoaded = true;       // 先置标记：并发调用只真正加载一次
        }
        LoadPriorityObs();
    }

    // 控制类机制（**不含缠绕/定身**，站长明确排除；减速/缴械也不算"控制"）
    bool IsPriorityObsControlMechanic(uint32 mechanic)
    {
        switch (mechanic)
        {
            case MECHANIC_STUN:
            case MECHANIC_FEAR:
            case MECHANIC_CHARM:
            case MECHANIC_SILENCE:
            case MECHANIC_SLEEP:
            case MECHANIC_POLYMORPH:
            case MECHANIC_BANISH:
            case MECHANIC_HORROR:
            case MECHANIC_DISORIENTED:
            case MECHANIC_SHACKLE:
            case MECHANIC_FREEZE:
            case MECHANIC_SAPPED:
            case MECHANIC_KNOCKOUT:
            case MECHANIC_TURN:
                return true;
            // MECHANIC_ROOT（缠绕/定身）、MECHANIC_SNARE（减速）、MECHANIC_DISARM（缴械）不算
            default:
                return false;
        }
    }

    // 一条法术属于哪一类观测：0 = 既不是治疗也不是控制（不记）
    uint8 ClassifyPriorityObsSpell(uint32 spellId)
    {
        SpellEntry const* spellInfo = sSpellTemplate.LookupEntry<SpellEntry>(spellId);
        if (!spellInfo)
            return 0;

        bool control = IsPriorityObsControlMechanic(spellInfo->Mechanic);
        bool heal = false;

        for (int i = 0; i < MAX_EFFECT_INDEX; ++i)
        {
            if (IsPriorityObsControlMechanic(spellInfo->EffectMechanic[i]))
                control = true;

            switch (spellInfo->Effect[i])
            {
                case SPELL_EFFECT_HEAL:
                case SPELL_EFFECT_HEAL_MAX_HEALTH:
                case SPELL_EFFECT_HEAL_MECHANICAL:
                case SPELL_EFFECT_HEAL_PCT:
                case SPELL_EFFECT_HEALTH_LEECH:
                    heal = true;
                    break;

                case SPELL_EFFECT_APPLY_AURA:
                case SPELL_EFFECT_APPLY_AREA_AURA_PARTY:
                case SPELL_EFFECT_APPLY_AREA_AURA_FRIEND:
                    switch (spellInfo->EffectApplyAuraName[i])
                    {
                        case SPELL_AURA_PERIODIC_HEAL:
                        case SPELL_AURA_MOD_HEALING_PCT:
                        case SPELL_AURA_MOD_HEALING_DONE:
                            heal = true;
                            break;
                        case SPELL_AURA_MOD_STUN:
                        case SPELL_AURA_MOD_FEAR:
                        case SPELL_AURA_MOD_SILENCE:
                        case SPELL_AURA_MOD_CONFUSE:
                        case SPELL_AURA_MOD_CHARM:
                            control = true;
                            break;
                        default:
                            break;
                    }
                    break;

                default:
                    break;
            }
        }

        if (heal)              // 治疗优先于控制：治疗会让其它目标都打不死
            return PRIO_OBS_HEAL;
        if (control)
            return PRIO_OBS_CONTROL;
        return 0;
    }

    void PrunePriorityObsLocked(time_t now)
    {
        for (std::map<std::pair<uint32, uint8>, PriorityObsRecord>::iterator it = obsByKey.begin();
             it != obsByKey.end();)
        {
            // 只丢"还没生效 + 很久没看到 + 没有待落库数据"的条目；已生效的是证据，永久保留
            if (it->second.count < kPriorityObsMinCount &&
                now > it->second.lastSeen + (time_t)kPriorityObsTtlSeconds &&
                obsDirty.find(it->first) == obsDirty.end())
                it = obsByKey.erase(it);
            else
                ++it;
        }
    }

    // 观测表里该 entry 的**有效优先级原值**（0 = 证据不足）；ThreatTier / PriorityRankOf 用
    uint8 ObservedPriorityRankFor(Unit* target)
    {
        if (!target)
            return 0;

        Creature* creature = dynamic_cast<Creature*>(target);
        if (!creature || !creature->GetCreatureInfo())
            return 0;

        const uint32 entry = creature->GetEntry();

        // 读路径缓存：结论最多 5s 重算一次（观测积累很慢，不必每 tick 抢锁）
        struct CacheEntry { uint8 rank; time_t expire; };
        thread_local std::unordered_map<uint32, CacheEntry> cache;
        const time_t now = time(nullptr);

        auto hit = cache.find(entry);
        if (hit != cache.end() && hit->second.expire > now)
            return hit->second.rank;

        EnsurePriorityObsLoaded();

        uint8 rank = 0;
        {
            std::scoped_lock lock(obsMutex);

            auto heal = obsByKey.find(std::make_pair(entry, uint8(PRIO_OBS_HEAL)));
            if (heal != obsByKey.end() && heal->second.count >= kPriorityObsMinCount)
                rank = kPriorityObsHealTier;
            else
            {
                auto control = obsByKey.find(std::make_pair(entry, uint8(PRIO_OBS_CONTROL)));
                if (control != obsByKey.end() && control->second.count >= kPriorityObsMinCount)
                    rank = kPriorityObsControlTier;
            }
        }

        if (cache.size() > 4096)
            cache.clear();

        cache[entry] = CacheEntry{rank, now + 5};
        return rank;
    }

    void LoadPriorityTargets()
    {
        // 调用方已判过"该刷新了"；这里只保证串行执行
        std::scoped_lock lock(priorityMutex);

        priorityLoadedAt = time(nullptr);
        priorityLoadTried = true;

        // 表是可选增强：先探测存在性，避免每次刷出 SQL 报错刷日志
        auto exists = WorldDatabase.Query(
            "SELECT 1 FROM information_schema.tables WHERE table_schema = DATABASE() "
            "AND table_name = 'ai_playerbot_priority_target'");
        if (!exists)
        {
            if (!priorityByEntry.empty())
                priorityByEntry.clear();

            static bool warned = false;
            if (!warned)
            {
                warned = true;
                sLog.outError("[priority] ai_playerbot_priority_target 不存在 —— 回落到启发式判定"
                              "（应用 dev/179_重点小怪优先级表.sql 可启用攻略名单）");
            }
            return;
        }

        auto results = WorldDatabase.Query(
            "SELECT entry, priority FROM ai_playerbot_priority_target WHERE priority > 0");
        if (!results)
            return;

        std::map<uint32, uint8> loaded;
        do
        {
            Field* fields = results->Fetch();
            uint32 entry = fields[0].GetUInt32();
            uint8 priority = fields[1].GetUInt8();
            if (entry && priority)
                loaded[entry] = priority;
        } while (results->NextRow());

        // [站长口径 2026-10-18] "应该给每个变化加日志"：人工表每次增 / 改 / 删都单独记一行
        //   （首次加载会把整张表当"0 -> 新值"记一遍，之后只有真变了才记）。
        for (std::map<uint32, uint8>::const_iterator i = loaded.begin(); i != loaded.end(); ++i)
        {
            uint8 old = 0;
            auto prev = priorityByEntry.find(i->first);
            if (prev != priorityByEntry.end())
                old = prev->second;

            if (old != i->second)
                sLog.outString("[PRIO-CHANGE] entry=%u name=%s old=%u -> new=%u source=table",
                    i->first, PriorityObsCreatureName(i->first).c_str(), uint32(old), uint32(i->second));
        }

        for (std::map<uint32, uint8>::const_iterator i = priorityByEntry.begin(); i != priorityByEntry.end(); ++i)
        {
            if (loaded.find(i->first) == loaded.end())
                sLog.outString("[PRIO-CHANGE] entry=%u name=%s old=%u -> new=0 source=table(removed)",
                    i->first, PriorityObsCreatureName(i->first).c_str(), uint32(i->second));
        }

        priorityByEntry.swap(loaded);
        sLog.outError("[priority] loaded %u entries", (uint32)priorityByEntry.size());
    }

    // 0 = 表里没有这只怪（调用方继续走启发式）
    uint8 PriorityTargetRank(Unit* target)
    {
        if (!target)
            return 0;

        Creature* creature = dynamic_cast<Creature*>(target);
        if (!creature || !creature->GetCreatureInfo())
            return 0;

        const uint32 entry = creature->GetEntry();

        // 读路径缓存：怪 entry 的判定结果不随 tick 变化（thread_local ⇒ 不加锁）。
        // [2026-10-18] 加**过期时间**：以前缓存永不过期 ⇒ 改完表要重启进程才生效；
        //   现在每个 entry 的缓存最多活 kPriorityRefreshSeconds 秒，到期重查 ⇒ 改表 5 分钟内生效。
        // [2026-10-20] 加 **epoch**：`.bot prio` 手动改档要求"实时生效" ⇒ 手动改档会推进
        //   priorityEpoch，缓存里的 epoch 对不上就当场重算（不必等 5 分钟）。
        struct CacheEntry { uint8 rank; time_t expire; uint32 epoch; };
        thread_local std::unordered_map<uint32, CacheEntry> cache;
        const time_t now = time(nullptr);
        const uint32 epoch = priorityEpoch.load(std::memory_order_relaxed);

        auto hit = cache.find(entry);
        if (hit != cache.end() && hit->second.expire > now && hit->second.epoch == epoch)
            return hit->second.rank;

        if (!priorityLoadTried || (now - priorityLoadedAt) >= kPriorityRefreshSeconds)
            LoadPriorityTargets();

        uint8 rank = 0;
        {
            std::scoped_lock lock(priorityMutex);
            auto found = priorityByEntry.find(entry);
            if (found != priorityByEntry.end())
                rank = found->second;
        }

        if (cache.size() > 8192)
            cache.clear();

        cache[entry] = CacheEntry{ rank, now + kPriorityRefreshSeconds, epoch };
        return rank;
    }
}

// [2026-10-20 站长口径] ".bot prio <数字1-5> 指定当前目标的优先级，并且实时生效"
//   ① 先落库（幂等 upsert；表不存在 ⇒ 只在内存生效，不影响出怪）；
//   ② 再改内存表 + 推进 epoch ⇒ 各线程的读缓存全部失效，全服 bot 下一 tick 就按新档位行动。
//   0 = 清除（回落启发式 / 观测表）。
bool PossibleAttackTargetsValue::SetPriorityRank(uint32 entry, uint8 priority, char const* source)
{
    if (!entry || priority > 5)
        return false;

    char const* src = (source && *source) ? source : "gm";

    // 表是可选增强：探一次存在性（与 LoadPriorityTargets 同口径），缺表就只改内存
    static bool checkedTable = false;
    static bool tableExists = false;
    if (!checkedTable)
    {
        checkedTable = true;
        std::unique_ptr<QueryResult> exists = WorldDatabase.Query(
            "SELECT 1 FROM information_schema.tables WHERE table_schema = DATABASE() "
            "AND table_name = 'ai_playerbot_priority_target'");
        tableExists = exists != nullptr;
    }

    bool persisted = false;
    if (tableExists)
    {
        if (priority == 0)
            persisted = WorldDatabase.PExecute("DELETE FROM `ai_playerbot_priority_target` WHERE `entry` = %u", entry);
        else
            persisted = WorldDatabase.PExecute(
                "INSERT INTO `ai_playerbot_priority_target` (`entry`,`priority`,`source`,`reason`) "
                "VALUES (%u, %u, '%s', 'GM .bot prio 手动指定') "
                "ON DUPLICATE KEY UPDATE `priority` = VALUES(`priority`), `source` = VALUES(`source`)",
                entry, uint32(priority), src);
    }

    uint8 oldPriority = 0;
    {
        std::scoped_lock lock(priorityMutex);

        auto found = priorityByEntry.find(entry);
        if (found != priorityByEntry.end())
            oldPriority = found->second;

        if (priority)
            priorityByEntry[entry] = priority;
        else
            priorityByEntry.erase(entry);

        // 别让下一次自动重载把这次手动改动当"旧数据"覆盖掉：
        //   已落库 ⇒ 重载读到的就是新值；没落库 ⇒ 推迟重载，内存值继续生效。
        priorityLoadedAt = time(nullptr);
        priorityLoadTried = true;
    }

    priorityEpoch.fetch_add(1, std::memory_order_relaxed);

    sLog.outString("[PRIO-CHANGE] entry=%u name=%s old=%u -> new=%u source=%s%s",
        entry, PriorityObsCreatureName(entry).c_str(), uint32(oldPriority), uint32(priority), src,
        persisted ? "" : " (memory only)");

    return true;
}

std::list<ObjectGuid> PossibleAttackTargetsValue::Calculate()
{
    std::list<ObjectGuid> result;
    if (ai->AllowActivity(ALL_ACTIVITY))
    {
        if (bot->IsInWorld() && !bot->IsBeingTeleported())
        {
            // Check if we only need one possible attack target
            bool getOne = false;
            if (!qualifier.empty())
            {
                getOne = stoi(qualifier);
            }

            if (getOne)
            {
                // Try to get one possible attack target
                result = AI_VALUE2(std::list<ObjectGuid>, "attackers", 1);
                RemoveNonThreating(result, getOne);
            }

            // If the one possible attack target failed, retry with multiple attackers
            if (result.empty())
            {
                result = AI_VALUE(std::list<ObjectGuid>, "attackers");
                RemoveNonThreating(result, getOne);

                // [aggro 2026-10-17 站长口径] 排序放在**迭代结束之后**，用快照排序。
                //   为什么不能在 RemoveNonThreating 里边选边算：那两个判据里 IsHealerCaster() 依赖
                //   "此刻在读什么条"，在迭代过程中调用会改变它自己的判定条件（读同一个值有副作用）；
                //   而且 AttackersValue 内部用的是 `std::set<Unit*>`（**按指针地址排序**），
                //   候选顺序本来就跟威胁无关 —— 所以"同优先级"其实是随机顺序。
                RankCandidates(result);
            }

            // [副本推进 2026-10-18 站长口径] 按波推进：verified 副本里只打"当前波" + "已经在打我们的"
            //   （判据见 playerbot/DungeonRoute.h；未验证的图 / 关掉开关时 AllowAttack 恒为 true，行为不变）
            if (!result.empty())
            {
                result.remove_if([this](const ObjectGuid& guid)
                {
                    return !dungeonroute::AllowAttack(bot, ai->GetUnit(guid));
                });
            }
        }
    }

	return result;
}

// [aggro 2026-10-17 站长口径] 候选排序：骷髅标记 > 高威胁技能怪 > 正在交战 > 可打断施法者 > 治疗施法中.
//   同级再按 近战怪 > 距离近 > 仇恨高。残血**不作为主判据**（站长指出的"一直残血优先"问题），
//   只在最后一级、且同级条件全部相同时才用。
void PossibleAttackTargetsValue::RankCandidates(std::list<ObjectGuid>& targets)
{
    if (targets.size() < 2)
        return;

    std::vector<Unit*> units;
    units.reserve(targets.size());

    for (const ObjectGuid& guid : targets)
    {
        if (Unit* unit = ai->GetUnit(guid))
            units.push_back(unit);
    }

    // 先一次性算好判据（避免比较器里重复做贵的判定）
    struct Ranked
    {
        Unit* unit;
        int tier;
        bool melee;
        float distance;
    };

    std::vector<Ranked> ranked;
    ranked.reserve(units.size());

    for (Unit* unit : units)
    {
        Ranked entry;
        entry.unit = unit;
        entry.tier = PossibleAttackTargetsValue::ThreatTier(unit, bot);
        entry.melee = PossibleAttackTargetsValue::IsMeleeAttacker(unit);
        entry.distance = bot->GetDistance(unit, true, DIST_CALC_COMBAT_REACH);
        ranked.push_back(entry);
    }

    std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b)
    {
        if (a.tier != b.tier)
            return a.tier > b.tier;

        if (a.melee != b.melee)
            return a.melee;

        return a.distance < b.distance;
    });

    targets.clear();
    for (const Ranked& entry : ranked)
        targets.push_back(entry.unit->GetObjectGuid());
}

namespace
{
    // [pull 2026-10-18 站长口径] 坦克开怪目标的路网校验缓存。
    //   navmesh 寻路不便宜，而 GrindTargetValue / AttackAnythingAction / InvalidTargetValue / TankTargetValue
    //   都会问"这只怪走不走得到"，所以：
    //     * 同一个 (bot, target) 的结果缓存 N 秒（可达 30s / 不可达 10s）；
    //     * 每线程每秒最多做 kPullPathChecksPerSecond 次寻路，超了直接返回"可达"（真正拦住"另一层"的是
    //       便宜判据 |Δz|，路网校验只是补刀）；
    //     * 用 thread_local，避免给地图线程加锁。
    const float kPullPathReachTolerance = 5.0f;
    const uint32 kPullPathCacheReachableSeconds = 30;
    const uint32 kPullPathCacheUnreachableSeconds = 10;
    const uint32 kPullPathChecksPerSecond = 40;
    const size_t kPullPathCacheMaxEntries = 4096;

    // [aggro 2026-10-17 站长口径] "这只怪是高威胁怪"的记忆时长：一个怪施放过高威胁技能后，
    //   在这么久之内一直按高威胁对待（避免"读条才高威胁、读完就变回普通怪"的目标抖动）。
    const uint32 kHighThreatMemorySeconds = 120;

    struct PullPathCacheEntry
    {
        time_t expire;
        bool reachable;
    };

    thread_local std::map<std::pair<uint64, uint64>, PullPathCacheEntry> pullPathCache;
    thread_local time_t pullPathCacheSecond = 0;
    thread_local uint32 pullPathChecksThisSecond = 0;

    bool HasNavMeshPathTo(Unit* target, Player* bot)
    {
        const uint64 botGuid = bot->GetObjectGuid().GetRawValue();
        const uint64 targetGuid = target->GetObjectGuid().GetRawValue();
        const time_t now = time(nullptr);

        auto cached = pullPathCache.find(std::make_pair(botGuid, targetGuid));
        if (cached != pullPathCache.end() && cached->second.expire > now)
            return cached->second.reachable;

        if (pullPathCacheSecond != now)
        {
            pullPathCacheSecond = now;
            pullPathChecksThisSecond = 0;
        }

        if (pullPathChecksThisSecond >= kPullPathChecksPerSecond)
            return true;

        ++pullPathChecksThisSecond;

        // 单次寻路：getPathStepFrom 只做一次 pathfinder->calculate（canPathTo 最多做 40 次尝试，这里不用）。
        const WorldPosition targetPos(target);
        const std::vector<WorldPosition> path = targetPos.getPathStepFrom(WorldPosition(bot), bot);

        bool reachable = false;
        if (!path.empty())
        {
            const WorldPosition& end = path.back();
            reachable = end.sqDistance2d(targetPos) <= kPullPathReachTolerance * kPullPathReachTolerance &&
                        fabs(end.getZ() - targetPos.getZ()) <= kPullPathReachTolerance;
        }

        if (pullPathCache.size() > kPullPathCacheMaxEntries)
            pullPathCache.clear();

        PullPathCacheEntry entry;
        entry.reachable = reachable;
        entry.expire = now + (reachable ? kPullPathCacheReachableSeconds : kPullPathCacheUnreachableSeconds);
        pullPathCache[std::make_pair(botGuid, targetGuid)] = entry;

        return reachable;
    }
}

// [pull 2026-10-18 站长口径] 坦克开怪目标"同一层 + 可达"判据：
//   多楼层副本（卡拉赞 / 黑石深渊 / 厄运之槌）里"看得见但其实是另一层"的怪以前会被选中，
//   坦克就直奔过去（路径断了也照跑），路上打它的怪也不管。
bool PossibleAttackTargetsValue::CanReachTarget(Unit* target, Player* bot, bool requireLos)
{
    if (!target || !bot || !bot->IsInWorld() || !target->IsInWorld())
        return false;

    // 不同地图 / 不同副本实例一定不可达
    if (bot->GetMapId() != target->GetMapId() || bot->GetMap() != target->GetMap())
        return false;

    // 主判据（便宜）：层高差超过"同层"阈值 ⇒ 一定是别的层，直接不当开怪目标（阈值 0 = 关）
    const float zDelta = fabs(bot->GetPositionZ() - target->GetPositionZ());
    if (sPlayerbotAIConfig.pullSameLevelZDistance > 0.0f && zDelta > sPlayerbotAIConfig.pullSameLevelZDistance)
        return false;

    // 次判据（便宜）：选新开怪目标时要求有 LOS（远处被墙挡着的怪不选；requireLos=false 时不看，避免
    //   战斗中因为暂时被柱子挡住就丢掉已经拉住的目标）
    const bool inLos = bot->IsWithinLOSInMap(target, true);
    if (requireLos && !inLos)
        return false;

    // 三判据（贵，带缓存 + 限流）：同层但离得远（或没 LOS 但要走过去）⇒ 确认真的走得通
    if (inLos && sServerFacade.GetDistance2d(bot, target) <= sPlayerbotAIConfig.pullPathCheckDistance)
        return true;

    return HasNavMeshPathTo(target, bot);
}

// [aggro 2026-10-17 站长口径] 共享的"交战"判定。放在这里（而不是各自策略里）是为了让
//   坦克链（TankTargetValue → FindTargetForTankStrategy）与 DPS 链（DpsTargetValue →
//   FindLeastHpTargetStrategy）用**同一套**口径，不会再出现"两边排序规则不一样"。
//
// 为什么需要它：AttackersValue 内部用的是 `std::set<Unit*>`（按**指针地址**排序！），
//   而 PossibleAttackTargetsValue::RemoveNonThreating(getOne) 直接取第一个命中项
//   ⇒ 候选顺序实际上是随机的。表现就是站长说的"对每一个攻击其他人的怪是同优先级的"。
bool PossibleAttackTargetsValue::IsEngagedAttacker(Unit* target, Player* bot)
{
    if (!target || !bot || !target->IsInWorld())
        return false;

    if (target->IsInCombat())
        return true;

    Unit* victim = target->GetVictim();
    if (!victim)
        return false;

    if (victim == bot)
        return true;

    if (Player* victimPlayer = dynamic_cast<Player*>(victim))
        return (victimPlayer == bot) || bot->IsInGroup(victimPlayer);

    // 非玩家受害者：它正在打的是友方单位（宠物/守护者/图腾/友军 NPC）⇒ 也算"在打我们这边"
    return !sServerFacade.IsHostileTo(bot, victim);
}

// [aggro 2026-10-17 站长口径] 门禁只对"副本/团本 + 非坦克 + 队里有活着的坦克"生效。
//   野外、纯机器人小队（没坦克）、坦克自己（自己就是坦克）一律不限制 —— 与
//   AttackAction::IsWaitingForTankThreat() 和 WaitForAttackStrategy::ShouldWait() 的口径一致。
bool PossibleAttackTargetsValue::IsInstanceTankGateActive() const
{
    if (!bot || !bot->IsInWorld() || !bot->GetMap())
        return false;

    if (!bot->GetMap()->IsDungeon() && !bot->GetMap()->IsRaid())
        return false;

    if (ai->IsTank(bot))
        return false;

    Group* group = bot->GetGroup();
    if (!group)
        return false;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->getSource();
        if (member && member != bot && sServerFacade.IsAlive(member) && ai->IsTank(member))
            return true;
    }

    return false;
}

bool PossibleAttackTargetsValue::IsAllowedForNonTank(Unit* target) const
{
    if (!target)
        return false;

    // 坦克已经上过仇恨 ⇒ 可以打（这是"集火坦克的目标"）
    if (ThreatValue::GetTankThreat(ai, target) > 0)
        return true;

    // 它正在打队伍成员（含自己）⇒ 可以还手。这不是"开新怪"。
    if (IsEngagedAttacker(target, bot))
        return true;

    sLog.outError("[threat] %s: candidate dropped, tank has no threat on %s yet",
        bot->GetName(), target->GetName());
    return false;
}

// [aggro 2026-10-17 站长口径] 高威胁技能的统一判据。站长口径原话：
//   "实际上更应该强调的是高威胁技能，比如加血，击晕，召唤等，其中有些是可以打断的，
//    我们可以用'有高威胁技能'作为高威胁怪物的判断规则。"
//   ⇒ 判据是"**这个怪会不会**这类技能"，但要能在运行时观测到 —— 见 HasHighThreatAbility()。
bool PossibleAttackTargetsValue::IsHighThreatSpell(uint32 spellId)
{
    if (!spellId)
        return false;

    SpellEntry const* spellInfo = sSpellTemplate.LookupEntry<SpellEntry>(spellId);
    if (!spellInfo)
        return false;

    for (int i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        switch (spellInfo->Effect[i])
        {
            // 治疗：不打掉它，其它目标都打不死
            case SPELL_EFFECT_HEAL:
            case SPELL_EFFECT_HEAL_MAX_HEALTH:
            case SPELL_EFFECT_HEAL_MECHANICAL:
            case SPELL_EFFECT_HEAL_PCT:
            case SPELL_EFFECT_HEALTH_LEECH:
            // 召唤：多打少
            case SPELL_EFFECT_SUMMON:
            case SPELL_EFFECT_SUMMON_PET:
            case SPELL_EFFECT_SUMMON_DEAD_PET:
            // 击晕 / 定身 / 恐惧 / 沉默 / 高额持续伤害：看光环类型
            case SPELL_EFFECT_APPLY_AURA:
            case SPELL_EFFECT_APPLY_AREA_AURA_PARTY:
            case SPELL_EFFECT_APPLY_AREA_AURA_FRIEND:
                break;
            default:
                continue;
        }

        if (spellInfo->Effect[i] == SPELL_EFFECT_APPLY_AURA ||
            spellInfo->Effect[i] == SPELL_EFFECT_APPLY_AREA_AURA_PARTY ||
            spellInfo->Effect[i] == SPELL_EFFECT_APPLY_AREA_AURA_FRIEND)
        {
            switch (spellInfo->EffectApplyAuraName[i])
            {
                case SPELL_AURA_PERIODIC_HEAL:
                case SPELL_AURA_MOD_HEALING_PCT:
                case SPELL_AURA_MOD_HEALING_DONE:
                case SPELL_AURA_MOD_STUN:
                case SPELL_AURA_MOD_ROOT:
                case SPELL_AURA_MOD_FEAR:
                case SPELL_AURA_MOD_SILENCE:
                case SPELL_AURA_PERIODIC_DAMAGE:
                case SPELL_AURA_PERIODIC_DAMAGE_PERCENT:
                case SPELL_AURA_MOD_DAMAGE_DONE:
                case SPELL_AURA_MOD_DAMAGE_PERCENT_DONE:
                case SPELL_AURA_MOD_INCREASE_HEALTH_PERCENT:
                    return true;
                default:
                    continue;
            }
        }

        return true;
    }

    return false;
}

// [aggro 2026-10-17 站长口径] "这个怪会不会高威胁技能" —— **粘性观测**。
//   怎么做：怪一旦施放过高威胁技能（治疗/击晕/召唤/高额伤害……），就把它记进缓存
//   （creature guid → 到期时刻）。这样判据从"此刻在读什么条"（tick 级抖动，会导致目标来回跳）
//   变成"这只怪是危险怪"（属性，稳定）。
//   为什么不做静态查表：本库的 `CreatureInfo` 用 `SpellList` 模板 id（不是 4 个 spell 字段），
//   静态展开成本高且要处理模板继承；而"它真的用过"是最可靠的证据 —— 玩家也是这么认出加血怪/召唤怪的。
//   缓存：thread_local（地图线程各自一份，不加锁）；命中续期、上限清理；非怪立即返回 false。
bool PossibleAttackTargetsValue::HasHighThreatAbility(Unit* target)
{
    if (!target || !target->IsInWorld())
        return false;

    Creature* creature = dynamic_cast<Creature*>(target);
    if (!creature)
        return false;

    const uint32 guid = creature->GetGUIDLow();
    const time_t now = time(nullptr);

    thread_local std::map<uint32, time_t> highThreatUntil;

    auto found = highThreatUntil.find(guid);
    if (found != highThreatUntil.end() && found->second > now)
        return true;

    // 观测：正在施放非近战法术且该法术属于高威胁类别
    if (target->IsNonMeleeSpellCasted(true))
    {
        if (Spell* spell = target->GetCurrentSpell(CURRENT_GENERIC_SPELL))
        {
            // [2026-10-18 站长口径] 顺手把"这只怪读过治疗/控制条"记进观测层（dev/193）：
            //   威胁等级表里没有的 entry 靠这份证据自适应；落库 + 每次生效变化都有 [PRIO-*] 日志。
            if (spell->m_spellInfo)
                ObservePriorityCast(target, spell->m_spellInfo->Id);

            if (spell->m_spellInfo && IsHighThreatSpell(spell->m_spellInfo->Id))
            {
                if (highThreatUntil.size() > 4096)
                    highThreatUntil.clear();

                highThreatUntil[guid] = now + kHighThreatMemorySeconds;
                return true;
            }
        }
    }

    return false;
}

// 正在施放治疗法术 ⇒ 最紧急（此刻就在回血）。
bool PossibleAttackTargetsValue::IsHealerCaster(Unit* target)
{
    if (!target || !target->IsInWorld())
        return false;

    if (!target->IsNonMeleeSpellCasted(true))
        return false;

    Spell* spell = target->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    if (!spell || !spell->m_spellInfo)
        return false;

    SpellEntry const* spellInfo = spell->m_spellInfo;
    for (int i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        switch (spellInfo->Effect[i])
        {
            case SPELL_EFFECT_HEAL:
            case SPELL_EFFECT_HEAL_MAX_HEALTH:
            case SPELL_EFFECT_HEAL_MECHANICAL:
            case SPELL_EFFECT_HEAL_PCT:
            case SPELL_EFFECT_HEALTH_LEECH:
                return true;
            case SPELL_EFFECT_APPLY_AURA:
            case SPELL_EFFECT_APPLY_AREA_AURA_PARTY:
            case SPELL_EFFECT_APPLY_AREA_AURA_FRIEND:
                if (spellInfo->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_HEAL ||
                    spellInfo->EffectApplyAuraName[i] == SPELL_AURA_MOD_HEALING_PCT ||
                    spellInfo->EffectApplyAuraName[i] == SPELL_AURA_MOD_HEALING_DONE)
                    return true;
                break;
            default:
                break;
        }
    }

    return false;
}

// [aggro 2026-10-17 站长口径] "可以通过打断减少输出的法系怪"：正在施放**可打断**的非近战法术。
//   打断就是减伤，所以它比普通近战怪更该被优先处理（坦克该把它的仇恨拉稳、DPS 该集火/打断）。
bool PossibleAttackTargetsValue::IsInterruptibleCaster(Unit* target)
{
    if (!target || !target->IsInWorld())
        return false;

    if (!target->IsNonMeleeSpellCasted(true))
        return false;

    return target->IsInterruptible();
}

// [aggro 2026-10-17 站长口径] 优先打带**骷髅标记**的：全队约定的第一集火目标。
//   用玩家/机器人真实摆在目标头上的团队标记（group->GetTargetIcon(RTI_SKULL_INDEX)），
//   而不是"某个 bot 的 rti 字符串设置"—— 标记是**对象上的事实**，全队共享、与谁设的无关。
bool PossibleAttackTargetsValue::IsSkullMarked(Unit* target, Player* bot)
{
    if (!target || !bot)
        return false;

    Group* group = bot->GetGroup();
    if (!group)
        return false;

    // 索引取自 RtiTargetValue::GetRtiIndex("skull")==7，与 HasIgnoreCCRti() 用的是同一个标记。
    return group->GetTargetIcon(RtiTargetValue::GetRtiIndex("skull")) == target->GetObjectGuid();
}

// [aggro 2026-10-17 站长口径] 威胁分级（定义见头文件注释）。
//   第 0 步是**数据优先**：攻略点名的重点小怪（dev/179）直接压过所有启发式判定。
//   站长口径："并不是每个有技能的怪都是高威胁" ⇒ 名单只认人工校订的攻略表；
//   引擎自己的"观察它施放过高威胁技能"（HasHighThreatAbility）只在**表里没有这只怪**时回落。
// [2026-10-18 站长口径] 暴露"表里的优先级原值"，给 CC 选目标用（4 = 需要优先控制）
uint8 PossibleAttackTargetsValue::PriorityRankOf(Unit* target)
{
    // 优先级来源：人工表(ai_playerbot_priority_target) > 观测表(ai_playerbot_priority_obs)
    //   观测只补"人工表里没有这只怪"的 entry。
    if (uint8 rank = PriorityTargetRank(target))
        return rank;

    return ObservedPriorityRankFor(target);
}

// [2026-10-18 站长口径] 观测表里该 entry 的有效优先级原值（0 = 证据不足 / 不在观测表里）
uint8 PossibleAttackTargetsValue::ObservedPriorityRank(Unit* target)
{
    return ObservedPriorityRankFor(target);
}

// [2026-10-18 站长口径] 观测一次施法："对于怪物所有读条的治疗都标记为优先级[最高]，
//   对于读条的控制技能（击晕/魅惑/恐惧等，**不含缠绕**）标记为[优先击杀]"。
//   只记"怪在战斗中真的读过这个条"：同一 guid 同一法术 1 秒内只算一次（清 target 值每 ~1s 扫一遍）。
void PossibleAttackTargetsValue::ObservePriorityCast(Unit* caster, uint32 spellId)
{
    if (!caster || !spellId)
        return;

    Creature* creature = dynamic_cast<Creature*>(caster);
    if (!creature || !creature->IsInWorld() || !creature->GetCreatureInfo())
        return;

    // 非战斗施法多半是城镇 NPC / 剧情 / 自己刷 buff，不进威胁等级
    if (!creature->IsInCombat())
        return;

    const uint32 entry = creature->GetEntry();
    const uint32 guid = creature->GetGUIDLow();
    const time_t now = time(nullptr);

    // 同一 guid + 同一法术 + 6 秒内 = 同一次施法（重复扫描不重复计数；长读条/引导条也只记一次）。
    //   为什么不是 1 秒：EnemyCastingTargetValue 每 ~1s 扫一遍，1 秒窗口会让**一次 3 秒的读条**
    //   被记成 2 次，直接骗过"同一 entry 至少观察到 2 次"的门槛。
    const uint32 kPriorityObsSameCastSeconds = 6;
    thread_local std::map<uint32, std::pair<uint32, time_t>> dedup;
    std::pair<uint32, time_t>& seen = dedup[guid];
    if (seen.first == spellId && now < seen.second)
        return;
    seen.first = spellId;
    seen.second = now + (time_t)kPriorityObsSameCastSeconds;
    if (dedup.size() > 4096)
        dedup.clear();

    const uint8 kind = ClassifyPriorityObsSpell(spellId);
    if (!kind)
        return;

    EnsurePriorityObsLoaded();

    bool becameEffective = false;
    {
        std::scoped_lock lock(obsMutex);

        PriorityObsRecord& rec = obsByKey[std::make_pair(entry, kind)];
        ++rec.count;
        rec.lastSeen = now;
        rec.lastMap = creature->GetMapId();
        rec.lastSpell = spellId;
        obsDirty.insert(std::make_pair(entry, kind));

        if (!rec.effectiveAt && rec.count >= kPriorityObsMinCount)
        {
            rec.effectiveAt = now;
            becameEffective = true;
        }

        if (obsByKey.size() > kPriorityObsMaxEntries)
            PrunePriorityObsLocked(now);
    }

    if (becameEffective)
    {
        const uint8 tier = PriorityObsKindTier(kind);
        sLog.outString("[PRIO-OBS] entry=%u name=%s kind=%s spell=%u count=%u 达到生效门槛 -> priority %u",
            entry, PriorityObsCreatureName(entry).c_str(), PriorityObsKindName(kind), spellId,
            kPriorityObsMinCount, uint32(tier));
        sLog.outString("[PRIO-CHANGE] entry=%u name=%s old=0 -> new=%u source=obs(kind=%s)",
            entry, PriorityObsCreatureName(entry).c_str(), uint32(tier), PriorityObsKindName(kind));
    }

    // 定时兜底落库（非副本场景也要持久化；副本里另有 PullEnd 触发）
    if (now - obsLastFlush >= (time_t)kPriorityObsFlushSeconds)
        FlushPriorityObs();
}

// [2026-10-18 站长口径] 观测落库：批量 upsert，count 只增（GREATEST）⇒ 重复落库幂等。
//   表不存在 ⇒ 把脏数据放回去、静默降级（观测仍在内存生效）。
void PossibleAttackTargetsValue::FlushPriorityObs()
{
    struct Row
    {
        uint32 entry;
        uint8 kind;
        uint32 count;
        uint32 lastSeen;
        uint32 lastMap;
        uint32 lastSpell;
    };
    std::vector<Row> rows;

    {
        std::scoped_lock lock(obsMutex);
        obsLastFlush = time(nullptr);
        if (obsDirty.empty())
            return;

        for (std::set<std::pair<uint32, uint8>>::const_iterator i = obsDirty.begin(); i != obsDirty.end(); ++i)
        {
            auto found = obsByKey.find(*i);
            if (found == obsByKey.end())
                continue;

            Row row;
            row.entry = i->first;
            row.kind = i->second;
            row.count = found->second.count;
            row.lastSeen = uint32(found->second.lastSeen);
            row.lastMap = found->second.lastMap;
            row.lastSpell = found->second.lastSpell;
            rows.push_back(row);
        }
        obsDirty.clear();
    }

    if (rows.empty())
        return;

    if (!PriorityObsTableExists())
    {
        std::scoped_lock lock(obsMutex);
        for (size_t i = 0; i < rows.size(); ++i)
            obsDirty.insert(std::make_pair(rows[i].entry, rows[i].kind));
        return;
    }

    const size_t chunkSize = 100;   // 一条 INSERT 100 行：秒级、可重复执行
    for (size_t offset = 0; offset < rows.size(); offset += chunkSize)
    {
        std::string sql =
            "INSERT INTO `ai_playerbot_priority_obs` "
            "(`entry`,`kind`,`count`,`last_seen`,`last_map`,`last_spell`) VALUES ";

        const size_t end = std::min(offset + chunkSize, rows.size());
        for (size_t i = offset; i < end; ++i)
        {
            char buf[256];
            snprintf(buf, sizeof(buf), "%s('%u','%s','%u','%u','%u','%u')",
                (i == offset ? "" : ","),
                rows[i].entry, PriorityObsKindName(rows[i].kind), rows[i].count,
                rows[i].lastSeen, rows[i].lastMap, rows[i].lastSpell);
            sql += buf;
        }

        sql += " ON DUPLICATE KEY UPDATE "
               "`count`=GREATEST(`count`,VALUES(`count`)), "
               "`last_seen`=VALUES(`last_seen`), `last_map`=VALUES(`last_map`), "
               "`last_spell`=VALUES(`last_spell`)";

        CharacterDatabase.Execute(sql.c_str());
    }

    uint32 obsRows = 0;
    if (std::unique_ptr<QueryResult> res = CharacterDatabase.Query(
            "SELECT COUNT(*) FROM `ai_playerbot_priority_obs`"))
        obsRows = res->Fetch()[0].GetUInt32();

    uint32 targetRows = 0;
    {
        std::scoped_lock lock(priorityMutex);
        targetRows = (uint32)priorityByEntry.size();
    }

    sLog.outString("[PRIO-DB] obs 落库 %u 行（entry×kind）；表行数 obs=%u target=%u",
        (uint32)rows.size(), obsRows, targetRows);
}
// [2026-10-18 站长口径] 星星标记 = 控制标记（与骷髅=击杀标记同理）
bool PossibleAttackTargetsValue::IsCcMarked(Unit* target, Player* bot)
{
    if (!target || !bot)
        return false;

    Group* group = bot->GetGroup();
    if (!group)
        return false;

    // 队伍标记 index：0=star(星星) 1=circle 2=diamond 3=triangle 4=moon 5=square 6=cross 7=skull
    return group->GetTargetIcon(0) == target->GetObjectGuid();
}
int PossibleAttackTargetsValue::ThreatTier(Unit* target, Player* bot)
{
    if (!target)
        return 0;

    // ===========================================================================
    // [2026-10-18 站长口径] 威胁等级表（`ai_playerbot_priority_target`）的**正式语义**
    //   —— 站长原话整理：
    //     5 = 任何情况必须立刻转火击杀（压过一切，且**不因吃控制而降级**）
    //     3 = 优先击杀
    //     2 = **没有对应驱散**的情况下优先击杀（队里有对应驱散 ⇒ 不优先）
    //     1 = 只需要**打断**；打断好就不用优先击杀（⇒ 击杀优先级 0，交给打断逻辑）
    //     4 = 暂未使用（保留）
    //   另有两条通用规则：
    //     ① "如果怪物吃控制，那么控制会降低优先级" ⇒ 3/2 各降 1 级（5 不降，必须杀）
    //     ② 表里没有的怪仍走启发式（骷髅标记/高威胁技能/正在交战/可打断/正在治疗）
    // ===========================================================================

    // 这只怪能不能被控（吃控制 = 控制能替我们处理掉它）
    //   判据：机械免疫表里 stun/root/fear/polymorph/sleep/sap **没有全免疫**
    //   （全免疫 = 首领级/免控怪 ⇒ 不算"吃控制"）
    static auto IsCcAble = [](Unit* unit) -> bool
    {
        Creature* creature = dynamic_cast<Creature*>(unit);
        if (!creature || !creature->GetCreatureInfo())
            return false;

        uint32 const mask = MECHANIC_STUN | MECHANIC_ROOT | MECHANIC_FEAR |
                            MECHANIC_POLYMORPH | MECHANIC_SLEEP | MECHANIC_SAPPED;
        return (creature->GetCreatureInfo()->MechanicImmuneMask & mask) != mask;
    };

    // 队伍里有没有"对应驱散"——近似判据：队里有能驱散的角色职业
    //   （精确到"这个 debuff 是魔法/诅咒/疾病/毒"需要解析怪的实际技能，先按职业近似）
    static auto GroupHasDispel = [](Player* player) -> bool
    {
        if (!player || !player->GetGroup())
            return false;

        Group* group = player->GetGroup();
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->getSource();
            if (!member || !member->IsInWorld() || !member->IsAlive())
                continue;

            switch (member->getClass())
            {
                case CLASS_PALADIN:     // 清洁术 / 净化
                case CLASS_PRIEST:      // 驱散魔法 / 祛病 / 驱除疾病
                case CLASS_SHAMAN:      // 净化 / 驱毒 / 祛病
                case CLASS_DRUID:       // 驱毒术 / 解除诅咒
                case CLASS_MAGE:        // 解除诅咒
                    return true;
                default:
                    break;
            }
        }
        return false;
    };

    // [2026-10-20 站长口径] "骷髅头标记也要随时生效"：骷髅是**人工即时指令**，
    //   必须压过表里的优先级（以前这段在表判定之后 ⇒ 表里已有档位的怪被标成骷髅也不生效）。
    if (IsSkullMarked(target, bot))
        return 5;

    // 优先级来源（高 → 低）：人工表 ai_playerbot_priority_target > 观测表 ai_playerbot_priority_obs
    //   > 启发式；观测只补"人工表里没有这只怪"的 entry。
    uint8 priority = PriorityTargetRank(target);
    if (!priority)
        priority = ObservedPriorityRankFor(target);

    if (priority)
    {
        switch (priority)
        {
            case 5:
                // 任何情况必须立刻转火击杀：最高档，不降级
                return 5;

            case 3:
            {
                // 优先击杀（吃控制 ⇒ 降 1 级，交给控制）
                int tier = 4;
                if (IsCcAble(target))
                    --tier;
                return tier < 0 ? 0 : tier;
            }

            case 2:
            {
                // 没有对应驱散才优先击杀；有驱散 ⇒ 基本不优先
                bool const hasDispel = GroupHasDispel(bot);
                int tier = hasDispel ? 0 : 3;
                if (!hasDispel && IsCcAble(target))
                    --tier;
                return tier < 0 ? 0 : tier;
            }

            case 1:
                // 只需要打断：不参与击杀优先级（打断逻辑仍会对可打断施法起效）
                return 0;

            case 4:
                // [2026-10-19 口径] 优先控制、**最后击杀**：击杀档位压到最低（1），
                // 由 CcTargetValue（needsControl = prio==4）优先把它选成控制目标。
                return 1;

            default:
                return 4;
        }
    }

    if (HasHighThreatAbility(target))
        return 4;

    if (IsEngagedAttacker(target, bot))
        return 3;

    if (IsInterruptibleCaster(target))
        return 2;

    if (IsHealerCaster(target))
        return 1;

    return 0;
}

// [pull 2026-10-18 站长口径] 坦克拿怪排序：先近战怪、再距离近的（见 TankTargetValue.cpp 的排序）。
bool PossibleAttackTargetsValue::IsMeleeAttacker(Unit* target)
{
    if (!target || !target->IsInWorld())
        return false;

    // 正在吟唱/引导非近战法术 ⇒ 远程 / 法系怪
    if (target->IsNonMeleeSpellCasted(true))
        return false;

    // 正在贴身打它的目标 ⇒ 近战怪（"贴身威胁"，坦克该先拉开）
    Unit* victim = target->GetVictim();
    if (victim && victim->IsInWorld() && victim->GetMapId() == target->GetMapId())
    {
        const float contactReach = target->GetCombinedCombatReach(victim, true) + ATTACK_DISTANCE;
        if (target->GetDistance(victim, true, DIST_CALC_COMBAT_REACH) <= contactReach)
            return true;
    }

    // 没在贴身打：用 creature_template 的静态数据判"只有远程伤害/远程攻击间隔"的纯远程怪
    Creature* creature = dynamic_cast<Creature*>(target);
    if (creature && creature->GetCreatureInfo())
    {
        const CreatureInfo* info = creature->GetCreatureInfo();
        const bool hasRanged = info->RangedBaseAttackTime > 0 && (info->MinRangedDmg > 0.0f || info->MaxRangedDmg > 0.0f);
        const bool hasMelee = info->MeleeBaseAttackTime > 0 && (info->MinMeleeDmg > 0.0f || info->MaxMeleeDmg > 0.0f);

        if (hasRanged && !hasMelee)
            return false;
    }

    // 判不出来时按近战算（坦克优先拉开最坏情况）
    return true;
}

void PossibleAttackTargetsValue::RemoveNonThreating(std::list<ObjectGuid>& targets, bool getOne)
{
    std::list<ObjectGuid> breakableCC;
    std::list<ObjectGuid> unBreakableCC;

    // [aggro 2026-10-17 站长口径] 副本/团本门禁的**候选层**收口：
    //   非坦克 + 队里有活着的坦克 ⇒ 从候选里摘掉"坦克从没碰过、也没在打队友"的怪。
    //   为什么放在这里而不是只在 AttackAction 出手时拦：AttackersValue 内部用 `std::set<Unit*>`
    //   （按指针地址排序），候选顺序本来是随机的；只在出手那一步拦，会出现"选中了不该打的怪 ⇒
    //   被拦下 ⇒ 这一 tick 不出手"的空转，观感就是发呆/反应慢。候选层就摘掉更干净。
    //   野外（非副本）不做任何限制。
    const bool instanceTankGate = IsInstanceTankGateActive();

    for(std::list<ObjectGuid>::iterator tIter = targets.begin(); tIter != targets.end();)
    {
        Unit* target = ai->GetUnit(*tIter);
        if (!IsValid(target, bot, sPlayerbotAIConfig.sightDistance, true, false))
        {
            std::list<ObjectGuid>::iterator tIter2 = tIter;
            ++tIter;
            targets.erase(tIter2);
        }
        else if (instanceTankGate && !IsAllowedForNonTank(target))
        {
            std::list<ObjectGuid>::iterator tIter2 = tIter;
            ++tIter;
            targets.erase(tIter2);
        }
        else if (!HasIgnoreCCRti(target, bot) && HasBreakableCC(target, bot))
        {
            breakableCC.push_back(*tIter);
            std::list<ObjectGuid>::iterator tIter2 = tIter;
            ++tIter;
            targets.erase(tIter2);
        }
        else if (!HasIgnoreCCRti(target, bot) && HasUnBreakableCC(target, bot))
        {
            unBreakableCC.push_back(*tIter);
            std::list<ObjectGuid>::iterator tIter2 = tIter;
            ++tIter;
            targets.erase(tIter2);
        }
        else
        {
            if (getOne)
            {
                // If the target is valid return it straight away
                std::list<ObjectGuid> result = { *tIter };
                targets = result;
                break;
            }
            else
            {
                ++tIter;
            }
        }
    }

    // [2026-10-18 站长口径] 被控制的目标一律**不当攻击候选**：
    //   "吃控制的怪，如果被控制了 就是最低优先级 最后击杀，不应该打醒控制"。
    //   ⇒ 这里不再把 breakableCC / unBreakableCC 加回候选（以前会加回 ⇒ bot 会去打醒羊/闷棍/恐惧）。
    //   控制一结束（下一 tick 它不再带 CC）自然恢复成正常候选；这等价于"最后才轮到它"。
    (void)breakableCC;
    (void)unBreakableCC;
}

bool PossibleAttackTargetsValue::HasIgnoreCCRti(Unit* target, Player* player)
{
    Group* group = player->GetGroup();
    return group && (group->GetTargetIcon(7) == target->GetObjectGuid());
}

bool PossibleAttackTargetsValue::HasBreakableCC(Unit* target, Player* player)
{
    if (target->IsPolymorphed())
    {
        return true;
    }

    if (sServerFacade.IsFrozen(target))
    {
        return true;
    }

    PlayerbotAI* ai = player->GetPlayerbotAI();
    if (ai)
    {
        if (ai->HasAura("sap", target))
        {
            return true;
        }

        if (ai->HasAura("gouge", target))
        {
            return true;
        }

        if (ai->HasAura("shackle undead", target))
        {
            return true;
        }
    }

    return false;
}

bool PossibleAttackTargetsValue::HasUnBreakableCC(Unit* target, Player* player)
{
    if (target->IsStunned())
    {
        return true;
    }

    if (sServerFacade.IsFeared(target))
    {
        return true;
    }

    if (sServerFacade.IsInRoots(target))
    {
        return true;
    }

    return false;
}

bool PossibleAttackTargetsValue::IsCcTarget(Unit* attacker, Player* player)
{
    PlayerbotAI* ai = player->GetPlayerbotAI();
    if (ai)
    {
        Group* group = ai->GetBot()->GetGroup();
        if (group)
        {
            Group::MemberSlotList const& groupSlot = group->GetMemberSlots();
            for (Group::member_citerator itr = groupSlot.begin(); itr != groupSlot.end(); itr++)
            {
                Player *player = sObjectMgr.GetPlayer(itr->guid);
                if (!player || !sServerFacade.IsAlive(player) || !ai->IsSafe(player))
                    continue;

                if (player->GetPlayerbotAI())
                {
                    if (PAI_VALUE(ObjectGuid,"rti cc target") == attacker->GetObjectGuid())
                        return true;

                    std::string rti = PAI_VALUE(std::string,"rti cc");
                    int index = RtiTargetValue::GetRtiIndex(rti);
                    if (index != -1)
                    {
                        uint64 guid = group->GetTargetIcon(index);
                        if (guid && attacker->GetObjectGuid() == ObjectGuid(guid))
                            return true;
                    }
                }
            }
        }
    }

    return false;
}

bool PossibleAttackTargetsValue::IsImmuneToDamage(Unit* target, Player* player)
{
    // Charmed
    if (sServerFacade.IsCharmed(target) && target->IsInTeam(player, true))
    {
        return true;
    }

    // Immune to damage
    // Before we check auras, check school derived immunity for creatures
    if (target->IsCreature())
    {
        if(((Creature*)target)->GetCreatureInfo()->SchoolImmuneMask == SPELL_SCHOOL_MASK_ALL)
            return true;
    }


    PlayerbotAI* ai = player->GetPlayerbotAI();
    if (!ai)
        return false;

    for (const Aura* aura : ai->GetAuras(target))
    {
        const SpellEntry* spellInfo = aura->GetSpellProto();
        if (!spellInfo)
            continue;

        if (spellInfo->Mechanic == MECHANIC_BANISH)
            return true;

        if (!aura->IsPositive())
            continue;

        if (spellInfo->Mechanic == MECHANIC_INVULNERABILITY ||
            spellInfo->Mechanic == MECHANIC_IMMUNE_SHIELD)
        {
            return true;
        }
    }

    return false;
}

std::string PossibleAttackTargetsValue::Format()
{
    std::ostringstream out;

    for (auto& target : value)
    {
        if (target != value.front())
            out << ",";

        WorldObject* wo = ai->GetWorldObject(target);

        if (wo)
            out << wo->GetName();
        else
            out << target;
    }

    return out.str();
}

bool PossibleAttackTargetsValue::IsTapped(Unit* target, Player* player)
{
    if (player)
    {
        PlayerbotAI* ai = player->GetPlayerbotAI();

        if (ai && ai->HasAura("tame beast", target))
            return false;

        const Creature* creature = dynamic_cast<Creature*>(target);
        if (creature)
        {
            Unit* victim = creature->GetVictim();
            Player* master = ai ? ai->GetMaster() : nullptr;
            PlayerbotAI* ai = player->GetPlayerbotAI();

             if (!victim) //Target is not attacking anything.
                return true;

            if (master && victim == master) //Target is attacking master.
                return true;

            if (player->IsInGroup(victim)) //Target is attacking groupmember.
                return true;

            if (!creature->HasLootRecipient()) //Target is untapped.
                return true;

            if (creature->IsTappedBy(player)) //Target is tapped by player.
                return true;

            if (master && target->getThreatManager().getThreat(master)) //Master as threat
                return true;

            if (ai && ai->HasStrategy("attack tagged", BotState::BOT_STATE_NON_COMBAT)) //Can attack tagged.
                if(ai->HasActivePlayerMaster() || !player->GetGroup() || player->GetGroup()->IsLeader(player->GetObjectGuid())) //Playing with a player or not in a group or master of group.
                    return true;
        }
    }

    return false;
}

bool PossibleAttackTargetsValue::IsValid(Unit* target, Player* player, float range, bool ignoreCC, bool checkAttackerValid)
{
    // Check for the valid attackers value
    if (checkAttackerValid && !AttackersValue::IsValid(target, player))
    {
        return false;
    }

    if (!IsPossibleTarget(target, player, range, ignoreCC))
        return false;

    if (sServerFacade.GetThreatManager(target).getCurrentVictim())
        return true;

    if (target->GetGuidValue(UNIT_FIELD_TARGET))
        return true;

    if (target->GetObjectGuid().IsPlayer())
        return true;

    if (player->GetPlayerbotAI() && (target->GetObjectGuid() == PAI_VALUE(ObjectGuid, "attack target")))
        return true;

    if(!HasIgnoreCCRti(target, player) && (HasBreakableCC(target, player) || HasUnBreakableCC(target, player)))
        return true;

    if (player->GetPlayerbotAI() && !player->GetPlayerbotAI()->HasActivePlayerMaster()&& PAI_VALUE(ObjectGuid, "rti target") == target->GetObjectGuid())
        return true;

    return false;
}

bool PossibleAttackTargetsValue::IsPossibleTarget(Unit* target, Player* player, float range, bool ignoreCC)
{
    if(target)
    {
        // [critter 2026-10-18 站长口径] "t 的战斗行为应该跳过小动物等"：
        //   critter（小动物 / 无害 NPC）在候选层就摘掉 ⇒ 坦克不会去拉它、DPS 也不会顺手 A 到它。
        //   配置 AiPlayerbot.SkipCritters（默认 1；0 = 老行为）。
        // [2026-10-18 站长口径] "bot 在副本中会优先击杀被控制的队友。这是错的。"
        //   魅惑/精神控制会让队友的阵营变敌对 ⇒ 只看 IsHostileTo 就会把他当怪打。
        //   这里硬过滤：**同队玩家永远不是普通攻击目标**（任何阵营状态）。
        //   被魅惑的队友改由 CcTargetValue / 硬控逻辑处理（控住，不打伤害）。
        if (target->IsPlayer())
        {
            Player* targetPlayer = static_cast<Player*>(target);
            if (targetPlayer != player && player->IsInGroup(targetPlayer))
                return false;
        }

        if (sPlayerbotAIConfig.skipCritters)
        {
            Creature* critter = dynamic_cast<Creature*>(target);
            if (critter && critter->GetCreatureInfo() && critter->IsCritter())
                return false;
        }

        // If the target is in an attackable distance
        if(!player->IsWithinDistInMap(target, range))
        {
            return false;
        }

        // If the target is immune to any damage
        if (IsImmuneToDamage(target, player))
        {
            return false;
        }

        // If the target is CC'ed or is a CC target (don't place a dot before we recast cc...)
        if(!ignoreCC && !HasIgnoreCCRti(target, player) && (HasBreakableCC(target, player) || HasUnBreakableCC(target, player)) || IsCcTarget(target, player))
        {
            return false;
        }

        // If the target is a NPC
        Player* enemyPlayer = dynamic_cast<Player*>(target);
        if (!enemyPlayer)
        {
            // If the target is not tapped (the player doesn't have the loot rights (gray name))
            if (!IsTapped(target, player))
            {
                return false;
            }
        }

        return true;
    }

    return false;
}

bool PossibleAddsValue::Calculate()
{
    PlayerbotAI *ai = bot->GetPlayerbotAI();
    std::list<ObjectGuid> possible = ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("possible targets no los")->Get();
    std::list<ObjectGuid> attackers = ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("possible attack targets")->Get();

    for (std::list<ObjectGuid>::iterator i = possible.begin(); i != possible.end(); ++i)
    {
        ObjectGuid guid = *i;
        if (find(attackers.begin(), attackers.end(), guid) != attackers.end()) continue;

        Unit* add = ai->GetUnit(guid);
        if (add && !add->GetGuidValue(UNIT_FIELD_TARGET) && !sServerFacade.GetThreatManager(add).getCurrentVictim() && sServerFacade.IsHostileTo(add, bot))
        {
            for (std::list<ObjectGuid>::iterator j = attackers.begin(); j != attackers.end(); ++j)
            {
                Unit* attacker = ai->GetUnit(*j);
                if (!attacker) continue;

                float dist = sServerFacade.GetDistance2d(attacker, add);
                if (sServerFacade.IsDistanceLessOrEqualThan(dist, sPlayerbotAIConfig.aoeRadius * 1.5f)) continue;
                if (sServerFacade.IsDistanceLessOrEqualThan(dist, sPlayerbotAIConfig.aggroDistance)) return true;
            }
        }
    }
    return false;
}