
#include "playerbot/playerbot.h"
#include "PossibleAttackTargetsValue.h"
#include "PossibleTargetsValue.h"

#include "playerbot/ServerFacade.h"
#include "playerbot/WorldPosition.h"
#include "Grids/GridNotifiers.h"
#include "Grids/GridNotifiersImpl.h"
#include "Grids/CellImpl.h"
#include "AttackersValue.h"
#include "RtiTargetValue.h"
#include "EnemyPlayerValue.h"
#include "ThreatValues.h"

#include <cmath>
#include <ctime>
#include <map>
#include <mutex>
#include <unordered_map>
#include <utility>

using namespace ai;
using namespace MaNGOS;

namespace ai { namespace prioritytarget { void Reload(); } }

// ===========================================================================
// [aggro 2026-10-17 站长口径] **攻略点名的重点小怪**表（数据优先于一切启发式）。
//
// 表：`tbccharacters.ai_playerbot_priority_target`（见 dev/179_重点小怪优先级表.sql）
//   entry / priority / source / reason
//   priority 1 = 攻略把它的技能标了 [打断] 或 [重要]（例：血警卫修理工 19633 的"强效治疗术 务必打断"）
//   priority 2 = 攻略「重点小怪」章节点名
//
// 为什么要有它（站长原话）：**"并不是每个有技能的怪都是高威胁"** —— 所以高威胁名单不能靠
//   引擎扫技能表推出来（那样会把 487 个"有治疗/召唤"的怪全算进去），必须用**人工校订过的攻略名单**。
//   引擎自己的"观察它施放过高威胁技能"只作为**名字不在表里**时的回落。
//
// 口径映射（表里的 priority 越大越优先，与 ThreatTier 同级语义）：
//   表 priority 1 -> ThreatTier 5（与骷髅标记同级：人工明确要处理）
//   表 priority 2 -> ThreatTier 4（与"有高威胁技能"同级）
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

    void LoadPriorityTargets()
    {
        // 调用方已判过"该刷新了"；这里只保证串行执行
        std::scoped_lock lock(priorityMutex);

        priorityLoadedAt = time(nullptr);
        priorityLoadTried = true;

        // 表是可选增强：先探测存在性，避免每次刷出 SQL 报错刷日志
        auto exists = CharacterDatabase.Query(
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

        auto results = CharacterDatabase.Query(
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

        priorityByEntry.swap(loaded);
        sLog.outError("[priority] 攻略重点小怪表已加载: %u 条", (uint32)priorityByEntry.size());
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

        // 读路径缓存：怪 entry 的判定结果不随 tick 变化（thread_local ⇒ 不加锁）
        thread_local std::unordered_map<uint32, uint8> cache;
        auto hit = cache.find(entry);
        if (hit != cache.end())
            return hit->second;

        if (!priorityLoadTried || (time(nullptr) - priorityLoadedAt) >= kPriorityRefreshSeconds)
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

        cache[entry] = rank;
        return rank;
    }
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

        // [tank-pull 2026-10-18 站长口径] **必须用完整路径**：`getPathStepFrom()` 只返回"下一步"的那个点，
        //   拿它跟目标位置比距离（终点容差 5 码）在 20 码以外的目标上**永远判不到**⇒ 坦克把所有远处目标都当成
        //   "走不到"⇒ 表现就是"T 不会自己开怪"。改成 bot -> target 的完整路径，再看终点是否落在目标附近。
        const WorldPosition targetPos(target);
        const std::vector<WorldPosition> path = WorldPosition(bot).getPathTo(targetPos, bot);

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
int PossibleAttackTargetsValue::ThreatTier(Unit* target, Player* bot)
{
    if (!target)
        return 0;

    switch (PriorityTargetRank(target))
    {
        case 1:  return 5;   // 攻略标了 [打断]/[重要]：与骷髅同级，人工明确要处理
        case 2:  return 4;   // 攻略「重点小怪」点名：与"有高威胁技能"同级
        case 3:  return 4;   // 预留：以后若要细分第三档，仍高于普通交战
        default: break;      // 表里没有 -> 走启发式
    }

    if (IsSkullMarked(target, bot))
        return 5;

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

    if (targets.empty())
    {
        if (!unBreakableCC.empty())
        {
            targets = unBreakableCC;
        }
        else if(!breakableCC.empty())
        {
            targets = breakableCC;
        }
    }
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

// [prio 2026-10-18 站长口径] `.bot prio` 写完表后立刻生效（见 PossibleAttackTargetsValue.h）。
//   这里能直接用上面匿名 namespace 里的 priorityMutex / priorityLoadTried / priorityLoadedAt（同一 TU）。
namespace ai
{
    namespace prioritytarget
    {
        void Reload()
        {
            std::scoped_lock lock(priorityMutex);
            priorityLoadTried = false;
            priorityLoadedAt = 0;
        }
    }
}