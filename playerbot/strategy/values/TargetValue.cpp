
#include "playerbot/playerbot.h"
#include "TargetValue.h"

#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/ServerFacade.h"
#include "RtiTargetValue.h"
#include "PossibleAttackTargetsValue.h"
#include "Entities/Unit.h"
#include "LastMovementValue.h"
#include "playerbot/strategy/values/Formations.h"

#include "Grids/GridNotifiers.h"
#include "Grids/GridNotifiersImpl.h"
#include "Grids/CellImpl.h"

using namespace ai;

Unit* TargetValue::FindTarget(FindTargetStrategy* strategy)
{
    std::list<ObjectGuid> attackers = ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("attackers")->Get();
    for (std::list<ObjectGuid>::iterator i = attackers.begin(); i != attackers.end(); ++i)
    {
        Unit* unit = ai->GetUnit(*i);
        if (!unit)
            continue;

        ThreatManager &threatManager = sServerFacade.GetThreatManager(unit);
        strategy->CheckAttacker(unit, &threatManager);
    }

    return strategy->GetResult();
}

bool FindNonCcTargetStrategy::IsCcTarget(Unit* attacker)
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

    return false;
}

void FindTargetStrategy::GetPlayerCount(Unit* creature, int* tankCount, int* dpsCount)
{
    Player* bot = ai->GetBot();
    if (tankCountCache.find(creature) != tankCountCache.end())
    {
        *tankCount = tankCountCache[creature];
        *dpsCount = dpsCountCache[creature];
        return;
    }

    *tankCount = 0;
    *dpsCount = 0;

    Unit::AttackerSet attackers(creature->getAttackers());
    for (std::set<Unit*>::const_iterator i = attackers.begin(); i != attackers.end(); i++)
    {
        Unit* attacker = *i;
        if (!attacker || !sServerFacade.IsAlive(attacker) || attacker == bot)
            continue;

        Player* player = dynamic_cast<Player*>(attacker);
        if (!player)
            continue;

        if (ai->IsTank(player))
            (*tankCount)++;
        else
            (*dpsCount)++;
    }

    tankCountCache[creature] = *tankCount;
    dpsCountCache[creature] = *dpsCount;
}

TravelTarget* LeaderTravelTargetValue::Calculate()
{
    TravelTarget* target = AI_VALUE(TravelTarget*, "travel target");

    Player* player = ai->GetGroupMaster();
    if (!player || player == bot || !player->GetPlayerbotAI() || !ai->IsSafe(player))
        return target;

    if (bot->GetGroup() && !ai->IsGroupLeader())
        if (!(ai->HasStrategy("follow", BotState::BOT_STATE_NON_COMBAT) ||
            ai->HasStrategy("wander", BotState::BOT_STATE_NON_COMBAT)) &&
            !ai->HasStrategy("stay", BotState::BOT_STATE_NON_COMBAT) &&
            !ai->HasStrategy("guard", BotState::BOT_STATE_NON_COMBAT))
            return target;

     TravelTarget* leaderTarget = PAI_VALUE(TravelTarget*, "travel target");

     if (!leaderTarget)
         return target;

     return leaderTarget;
}

WorldPosition LastLongMoveValue::Calculate()
{
    LastMovement& lastMove = *context->GetValue<LastMovement&>("last movement");

    if (lastMove.lastPath.empty())
        return WorldPosition();

    return lastMove.lastPath.getBack();
}

WorldPosition HomeBindValue::Calculate()
{
    float x, y, z;
    uint32 mapId;
    bot->GetHomebindLocation(x, y, z, mapId);
    return WorldPosition(mapId, x, y, z, 0.0);
}

std::string RpgTargetValue::Format()
{
    return chat->formatGuidPosition(value, bot);
}

std::string HomeBindValue::Format()
{
    WorldPosition pos = this->Calculate();
    return chat->formatWorldPosition(pos);
}

void PullTargetValue::Set(ObjectGuid unitGuid)
{
    guid = unitGuid;
}

ObjectGuid PullTargetValue::Get()
{   
    return guid;
}

ObjectGuid FollowTargetValue::Calculate()
{
    Unit* followTarget = AI_VALUE(GuidPosition, "manual follow target").GetUnit(bot->GetInstanceId());
    if (followTarget == nullptr)
    {
        Formation* formation = AI_VALUE(Formation*, "formation");
        if (formation && !formation->GetTargetName().empty())
        {
            followTarget = ai->GetUnit(AI_VALUE(ObjectGuid, formation->GetTargetName()));
        }
        else
        {
            followTarget = ai->GetUnit(AI_VALUE(ObjectGuid, "master target"));
        }
    }

    return followTarget ? followTarget->GetObjectGuid() : ObjectGuid();
}

ObjectGuid ClosestAttackerTargetingMeTargetValue::Calculate()
{
    Unit* result = nullptr;
    float closest = 9999.0f;

    const std::list<ObjectGuid>& attackers = AI_VALUE(std::list<ObjectGuid>, "attackers targeting me");
    for (const ObjectGuid& attackerGuid : attackers)
    {
        Unit* attacker = ai->GetUnit(attackerGuid);
        if (attacker)
        {
            const float distance = bot->GetDistance(attacker, true, DIST_CALC_COMBAT_REACH);
            if (distance < closest)
            {
                closest = distance;
                result = attacker;
            }
        }
    }

    return result ? result->GetObjectGuid() : ObjectGuid();
}

std::list<ObjectGuid> FriendlyManualTargetsValue::Get()
{
    value.remove_if([&](const ObjectGuid& playerGuid)
    {
        Unit* player = ai->GetUnit(playerGuid);
        if (ai->IsSafe(player))
        {
            if (bot->IsInGroup(player))
            {
                return false;
            }
        }

        return true;
    });

    return value;
}

std::list<ObjectGuid> FriendlyManualTargetsValue::LazyGet()
{
    return Get();
}

GuidPosition ClosestEntryValue::Calculate()
{
    // Implement the logic to calculate the closest entry target
    WorldPosition botPos(bot);

    if (!Qualified::isValidNumberString(qualifier))
        return GuidPosition();

    int32 entry = stoi(qualifier);

    if (entry > 0)
    {
        std::vector<CreatureDataPair const*> creatures = botPos.getCreaturesNear(0.0f, entry);

        float minDistance = std::numeric_limits<float>::max();
        GuidPosition closestTarget;

        for (auto& creature : creatures)
        {
            GuidPosition target(creature);
            float distance = botPos.sqDistance2d(target);
            if (distance < minDistance)
            {
                minDistance = distance;
                closestTarget = target;
            }
        }

        return closestTarget;
    }
    else
    {
        std::vector<GameObjectDataPair const*> gameObjects = botPos.getGameObjectsNear(0.0f, -entry);
        float minDistance = std::numeric_limits<float>::max();
        GuidPosition closestTarget;
        for (auto& gameObject : gameObjects)
        {
            GuidPosition target(gameObject);
            float distance = botPos.sqDistance2d(target);
            if (distance < minDistance)
            {
                minDistance = distance;
                closestTarget = target;
            }
        }
        return closestTarget;
    }

    return GuidPosition();
}

// ===========================================================================
// [BOSS 战策略值 2026-10-19] `enemy casting target` / `enemy casting interruptible target`
//   （见 TargetValue.h 顶部同一段注释）
// ===========================================================================

namespace
{
    // 一个候选是否算"在读条的敌人"：敌对 + 活着 + 正在施放**非近战**法术（+ 可选可打断 + 可选法术过滤）
    static bool IsEnemyCastingCandidate(PlayerbotAI* ai, Player* bot, Unit* unit, bool interruptibleOnly, uint32 spellFilter)
    {
        if (!unit || unit == bot || !unit->IsInWorld() || !unit->IsAlive())
            return false;

        if (unit->GetMapId() != bot->GetMapId())
            return false;

        if (ai->IsSafe(unit) || !sServerFacade.IsHostileTo(unit, bot))
            return false;

        // IsNonMeleeSpellCasted = 正在施放/引导非近战法术（近战平砍/自动射击不算）
        if (!unit->IsNonMeleeSpellCasted(true))
            return false;

        if (interruptibleOnly && !unit->IsInterruptible())
            return false;

        if (spellFilter)
        {
            Spell* spell = unit->GetCurrentSpell(CURRENT_GENERIC_SPELL);
            if (!spell)
                spell = unit->GetCurrentSpell(CURRENT_CHANNELED_SPELL);

            if (!spell || !spell->m_spellInfo || spell->m_spellInfo->Id != spellFilter)
                return false;
        }

        return true;
    }

    // [2026-10-18 站长口径] 观测"怪在读治疗/控制条"（dev/193）：
    //   打断扫描覆盖所有视野内的敌对施法者，比只靠 ThreatTier 被调到的时机更全，
    //   观测计数才攒得起来（ ThreatTier 里也会观测，两处去重由 ObservePriorityCast 保证）。
    static void ObservePriorityCastOf(Unit* unit)
    {
        if (!unit || !unit->IsNonMeleeSpellCasted(true))
            return;

        if (Spell* spell = unit->GetCurrentSpell(CURRENT_GENERIC_SPELL))
            if (spell->m_spellInfo)
                PossibleAttackTargetsValue::ObservePriorityCast(unit, spell->m_spellInfo->Id);

        if (Spell* spell = unit->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            if (spell->m_spellInfo)
                PossibleAttackTargetsValue::ObservePriorityCast(unit, spell->m_spellInfo->Id);
    }
}

ObjectGuid EnemyCastingTargetValue::Calculate()
{
    Player* bot = ai->GetBot();
    if (!bot)
        return ObjectGuid();

    float range = 30.0f;
    uint32 spellFilter = 0;

    // 限定词解析：空限定词 ⇒ 全部走默认；非法 token 直接忽略（不崩、不误解）
    if (!getQualifier().empty())
    {
        std::vector<std::string> tokens = Qualified::getMultiQualifiers(getQualifier(), ",");
        for (std::vector<std::string>::const_iterator i = tokens.begin(); i != tokens.end(); ++i)
        {
            if (i->empty())
                continue;

            // 裸数字 = 半径（写起来最短：`enemy casting target::25`）
            if (Qualified::isValidNumberString(*i))
            {
                range = (float)atof(i->c_str());
                continue;
            }

            std::vector<std::string> parts = Qualified::getMultiQualifiers(*i, "::");
            if (parts.size() != 2 || !Qualified::isValidNumberString(parts[1]))
                continue;

            if (parts[0] == "dist" || parts[0] == "distance")
                range = (float)atof(parts[1].c_str());
            else if (parts[0] == "spellid")
                spellFilter = (uint32)atoi(parts[1].c_str());
        }
    }

    if (range <= 0.0f || range > sPlayerbotAIConfig.sightDistance)
        return ObjectGuid();

    // 1) current target 优先（最热路径；命中就不进范围扫描）
    Unit* currentTarget = nullptr;
    if (Value<ObjectGuid>* currentTargetValue = ai->GetAiObjectContext()->GetValue<ObjectGuid>("current target"))
        currentTarget = ai->GetUnit(currentTargetValue->Get());

    if (IsEnemyCastingCandidate(ai, bot, currentTarget, interruptibleOnly, spellFilter) &&
        sServerFacade.GetDistance2d(bot, currentTarget) <= range)
    {
        ObservePriorityCastOf(currentTarget);
        return currentTarget->GetObjectGuid();
    }

    // 2) 范围扫描：AnyUnfriendlyUnitInObjectRangeCheck 自带"敌对 + 射程内 + 有 LOS + 忽略图腾"，
    //    Cell::VisitAllObjects 只访问与 range 相交的格子（不是全图扫描）。
    std::list<Unit*> units;
    MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck check(bot, range);
    MaNGOS::UnitListSearcher<MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck> searcher(units, check);
    // 注意 `Cell` 是**全局**命名空间里的 struct（Grids/Cell.h），不在 MaNGOS 里
    Cell::VisitAllObjects((const WorldObject*)bot, searcher, range);

    Unit* best = nullptr;
    float bestDistance = 0.0f;
    for (std::list<Unit*>::iterator i = units.begin(); i != units.end(); ++i)
    {
        Unit* unit = *i;
        if (!IsEnemyCastingCandidate(ai, bot, unit, interruptibleOnly, spellFilter))
            continue;

        ObservePriorityCastOf(unit);

        float distance = sServerFacade.GetDistance2d(bot, unit);
        if (distance > range)
            continue;

        if (!best || distance < bestDistance)
        {
            best = unit;
            bestDistance = distance;
        }
    }

    return best ? best->GetObjectGuid() : ObjectGuid();
}
