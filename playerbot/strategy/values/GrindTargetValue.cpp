
#include "playerbot/playerbot.h"
#include "GrindTargetValue.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/RandomPlayerbotMgr.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/DungeonRoute.h"
#include "AttackersValue.h"
#include "PossibleAttackTargetsValue.h"
#include "playerbot/strategy/actions/ChooseTargetActions.h"
#include "playerbot/strategy/values/FreeMoveValues.h"
#include "Tools/Formulas.h"

using namespace ai;

ObjectGuid GrindTargetValue::Calculate()
{
    uint32 memberCount = 1;
    Group* group = bot->GetGroup();
    if (group)
        memberCount = group->GetMembersCount();

    Unit* target = NULL;
    uint32 assistCount = 0;
    while (!target && assistCount < memberCount)
    {
        target = FindTargetForGrinding(assistCount++);
    }

    return target ? target->GetObjectGuid() : ObjectGuid();
}

Unit* GrindTargetValue::FindTargetForGrinding(int assistCount)
{
    uint32 memberCount = 1;
    Group* group = bot->GetGroup();
    Player* master = GetMaster();

    if (master && (master == bot || master->GetMapId() != bot->GetMapId() || master->IsBeingTeleported() || !master->GetPlayerbotAI()))
        master = nullptr;

    // [pull 2026-10-18 站长口径] 坦克开怪目标要过"同一层 + 可达"判据（见 PossibleAttackTargetsValue::CanReachTarget()）。
    const bool checkSameLevel = ai->IsTank(bot) && sPlayerbotAIConfig.pullSameLevelZDistance > 0.0f;

    std::list<ObjectGuid> attackers = context->GetValue<std::list<ObjectGuid>>("possible attack targets")->Get();
    for (std::list<ObjectGuid>::iterator i = attackers.begin(); i != attackers.end(); i++)
    {
        Unit* unit = ai->GetUnit(*i);
        if (!unit || !sServerFacade.IsAlive(unit))
            continue;

        // [副本推进 2026-10-18 站长口径] verified 副本按波推进：只把"当前波 + 已经打我们的"当开怪目标
        //   （见 playerbot/DungeonRoute.h；未验证的图 / 关掉开关时恒为 true，行为不变）
        if (!dungeonroute::AllowAttack(bot, unit))
            continue;

        if (!bot->InBattleGround() && !CanFreeMoveValue::CanFreeTarget(ai, GuidPosition(unit)))
        {
            if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                ai->TellPlayer(GetMaster(), chat->formatWorldobject(unit) + "(hostile) ignored (out of free range).");
            continue;
        }

        // [pull 2026-10-18 站长口径] 坦克的开怪目标必须"同一层且走得到"：多楼层副本（卡拉赞 / 黑石深渊 /
        //   厄运之槌）里"看得见但其实是另一层"的怪以前会被选中 ⇒ 坦克直奔过去（路径断了也不停），
        //   路上打它的怪也不还手。判据见 PossibleAttackTargetsValue::CanReachTarget()。
        if (checkSameLevel && !PossibleAttackTargetsValue::CanReachTarget(unit, bot))
        {
            if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                ai->TellPlayer(GetMaster(), BOT_TEXT2("%object ignored (not on the same floor or unreachable).", (std::map<std::string, std::string>{ {"%object", chat->formatWorldobject(unit)} })));
            continue;
        }

        if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
            ai->TellPlayer(GetMaster(), chat->formatWorldobject(unit) +"(hostile) selected.");
       
        return unit;
    }

    std::list<ObjectGuid> targets = *context->GetValue<std::list<ObjectGuid> >("possible targets");

    if (targets.empty())
        return NULL;

    float distance = 0;
    Unit* result = NULL;

    bool travelTargetWorking = AI_VALUE(bool, "travel target working");
    bool travelTargetTraveling = AI_VALUE(bool, "travel target traveling");
    TravelTarget* travelTarget = AI_VALUE(TravelTarget*, "travel target");
    bool isGrindTravelDest = travelTarget && typeid(travelTarget->GetDestination()) == typeid(GrindTravelDestination);

    struct MemberInfo {
        Player* player;
        float x, y;
    };
    std::vector<MemberInfo> groupMembers;
    if (group)
    {
        Group::MemberSlotList const& groupSlot = group->GetMemberSlots();
        groupMembers.reserve(groupSlot.size());
        for (Group::member_citerator itr = groupSlot.begin(); itr != groupSlot.end(); itr++)
        {
            Player* member = sObjectMgr.GetPlayer(itr->guid);
            if (member && sServerFacade.IsAlive(member))
                groupMembers.push_back({ member, member->GetPositionX(), member->GetPositionY() });
        }
    }

    std::unordered_map<uint32, bool> needForQuestCache;

    for (std::list<ObjectGuid>::iterator tIter = targets.begin(); tIter != targets.end(); tIter++)
    {
        Unit* unit = ai->GetUnit(*tIter);
        if (!unit)
            continue;

        // [副本推进 2026-10-18 站长口径] 同第一个循环：verified 副本里不把"还没轮到的波"当开怪目标
        if (!dungeonroute::AllowAttack(bot, unit))
            continue;

#ifdef MANGOSBOT_TWO 
        if (bot->GetMapId() == 609)
        {
            switch (unit->GetEntry())
            {
            case 28605: //Havenshire Stallion
            case 28606: //Havenshire Mare
            case 28607: //Havenshire Cotl
            case 28891: //Scarlet Miner
            case 28819: //Scarlet Miner
            case 28769: //Shadowy tormentor
            case 28768: //Dark Rider of Archeus
                continue;
            case 29080: //Scarlet Champion
            case 29029: //Scarlet Inquisitor
            case 29000: //Scarlet Commander Rodrick
            case 28940: //Scarlet Crusader
            case 28939: //Scarlet Preacher
            case 28936: //Scarlet Commander
            case 28898: //Scarlet Captain
            case 28896: //Scarlet Infantryman
            case 28895: //Scarlet Medic
            case 28892: //Scarlet Peasant
            case 28610: //Scarlet Marksman
                if (AI_VALUE2(bool, "need quest objective", "12680,0"))
                    continue;
                if (AI_VALUE2(bool, "need quest objective", "12701"))
                    continue;
                if (AI_VALUE2(bool, "need quest objective", "12727"))
                    continue;
                if (AI_VALUE2(bool, "need quest objective", "12754"))
                    continue;                
                break;
            case 29102: //HearthglenCrusader
            case 29103: //Tirisfal Crusader
            case 29104: //Scarlet Ballista
                if (AI_VALUE2(bool, "need quest objective", "12779"))
                {
                    if (AI_VALUE2(bool, "trigger active", "in vehicle") && bot->IsWithinDistInMap(unit, sPlayerbotAIConfig.sightDistance))
                        return unit;
                    continue;
                }
                break;
            case 28834: //Scarlet Fleet Defender
            case 28850: //Scarlet Land Canon
            case 28856: //Scarlet Fleet Guardian
                if (AI_VALUE2(bool, "need quest objective", "12701,0"))
                {
                    if (AI_VALUE2(bool, "trigger active", "in vehicle::Scarlet Cannon") && !urand(0, 5))
                        return unit;
                    continue;
                }
            }
        }
#endif

        if (abs(bot->GetPositionZ() - unit->GetPositionZ()) > sPlayerbotAIConfig.spellDistance)
        {
            if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                ai->TellPlayer(GetMaster(), BOT_TEXT2("%object ignored (to far above/below).", (std::map<std::string, std::string>{ {"%object", chat->formatWorldobject(unit)} })));
            continue;
        }

        // [pull 2026-10-18 站长口径] 坦克开怪目标的"同一层 + 可达"判据（上面那条只挡了 Δz > spellDistance 的极端情况，
        //   多楼层副本里差一层往往不到 26 码，"看得见但走不到"仍然会被选中）。见 PossibleAttackTargetsValue::CanReachTarget()。
        if (checkSameLevel && !PossibleAttackTargetsValue::CanReachTarget(unit, bot))
        {
            if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                ai->TellPlayer(GetMaster(), BOT_TEXT2("%object ignored (not on the same floor or unreachable).", (std::map<std::string, std::string>{ {"%object", chat->formatWorldobject(unit)} })));
            continue;
        }

        if (!bot->InBattleGround() && !CanFreeMoveValue::CanFreeTarget(ai, GuidPosition(unit))) //Do not grind mobs far away from master.
        {
            if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                ai->TellPlayer(GetMaster(), BOT_TEXT2("%object ignored (out of free range).", (std::map<std::string, std::string>{ {"%object", chat->formatWorldobject(unit)} })));
            continue;
        }

        if (!bot->InBattleGround() && master &&
            (ai->HasStrategy("follow", BotState::BOT_STATE_NON_COMBAT) ||
             ai->HasStrategy("wander", BotState::BOT_STATE_NON_COMBAT)) &&
            sServerFacade.GetDistance2d(master, unit) > sPlayerbotAIConfig.proximityDistance)
        {
            if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                ai->TellPlayer(GetMaster(), BOT_TEXT2("%object ignored (far from master).", (std::map<std::string, std::string>{ {"%object", chat->formatWorldobject(unit)} })));
            continue;
        }

        if (!bot->InBattleGround() && (int)unit->GetLevel() - (int)bot->GetLevel() > 4 && !unit->GetObjectGuid().IsPlayer())
        {
            if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                ai->TellPlayer(GetMaster(), BOT_TEXT2("%object ignored (%levels levels above bot).", (std::map<std::string, std::string>{ {"%levels", std::to_string((int)unit->GetLevel() - (int)bot->GetLevel())}, {"%object", chat->formatWorldobject(unit)} })));
            continue;
        }

        Creature* creature = dynamic_cast<Creature*>(unit);
        if (creature && creature->GetCreatureInfo() && creature->GetCreatureInfo()->Rank > CREATURE_ELITE_NORMAL && !AI_VALUE(bool, "can fight elite") &&
            !AI_VALUE2(bool, "trigger active", "in vehicle"))
        {
            if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                ai->TellPlayer(GetMaster(), BOT_TEXT2("%object ignored (can not fight elites currently).", (std::map<std::string, std::string>{ {"%object", chat->formatWorldobject(unit)} })));
            continue;
        }

        if (!AttackersValue::IsValid(unit, bot, nullptr, false, false))
        {
            if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                ai->TellPlayer(GetMaster(), BOT_TEXT2("%object ignored (is pet or evading/unkillable).", (std::map<std::string, std::string>{ {"%object", chat->formatWorldobject(unit)} })));
            continue;
        }

        if (!PossibleAttackTargetsValue::IsPossibleTarget(unit, bot, sPlayerbotAIConfig.sightDistance, false))
        {
            if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                ai->TellPlayer(GetMaster(), BOT_TEXT2("%object ignored (tapped, cced or out of range).", (std::map<std::string, std::string>{ {"%object", chat->formatWorldobject(unit)} })));
            continue;
        }

        if (creature && creature->IsCritter() && urand(0, 10))
        {
            if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                ai->TellPlayer(GetMaster(), BOT_TEXT2("%object ignored (ignore critters).", (std::map<std::string, std::string>{ {"%object", chat->formatWorldobject(unit)} })));
            continue;
        }

        float newdistance = sServerFacade.GetDistance2d(bot, unit);

        uint32 entry = unit->GetEntry();
        bool needForQuest = false;
        if (entry)
        {
            auto cacheIt = needForQuestCache.find(entry);
            if (cacheIt != needForQuestCache.end())
            {
                needForQuest = cacheIt->second;
            }
            else
            {
                needForQuest = AI_VALUE2(bool, "need for quest", std::to_string(entry));
                needForQuestCache[entry] = needForQuest;
            }
        }

        if (entry && !needForQuest)
        {
            if (urand(0, 100) < 99 && travelTargetWorking && !isGrindTravelDest)
            {
                if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                    ai->TellPlayer(GetMaster(), BOT_TEXT2("%object ignored (not needed for active quest).", (std::map<std::string, std::string>{ {"%object", chat->formatWorldobject(unit)} })));

                continue;
            }
            else if (creature && !MaNGOS::XP::Gain(bot, creature) && urand(0, 50))
            {
                if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                    if ((AI_VALUE(bool, "travel target traveling") && typeid(AI_VALUE(TravelTarget*, "travel target")->GetDestination()) != typeid(GrindTravelDestination)))
                        ai->TellPlayer(GetMaster(), BOT_TEXT2("%object ignored (not xp and not needed for quest).", (std::map<std::string, std::string>{ {"%object", chat->formatWorldobject(unit)} })));

                continue;
            }
            else if (urand(0, 100) < 75)
            {
                if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                    if (AI_VALUE(bool, "travel target traveling") && typeid(AI_VALUE(TravelTarget*, "travel target")->GetDestination()) != typeid(GrindTravelDestination))
                        ai->TellPlayer(GetMaster(), BOT_TEXT2("%object increased distance (not needed for quest).", (std::map<std::string, std::string>{ {"%object", chat->formatWorldobject(unit)} })));

                newdistance += 20;
            }
        }

        if (!bot->InBattleGround() && GetTargetingPlayerCount(unit) > assistCount)
        {
            if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                ai->TellPlayer(GetMaster(), BOT_TEXT2("%object increased distance (%count bots already targeting).", (std::map<std::string, std::string>{ {"%count", std::to_string(GetTargetingPlayerCount(unit))}, {"%object", chat->formatWorldobject(unit)} })));

            newdistance =+ GetTargetingPlayerCount(unit) * 5;
        }

        if (group)
        {
            Group::MemberSlotList const& groupSlot = group->GetMemberSlots();
            for (Group::member_citerator itr = groupSlot.begin(); itr != groupSlot.end(); itr++)
            {
                Player* member = sObjectMgr.GetPlayer(itr->guid);
                if (!member || !sServerFacade.IsAlive(member))
                    continue;

                newdistance = sServerFacade.GetDistance2d(member, unit);
                if (!result || newdistance < distance)
                {
                    distance = newdistance;
                    result = unit;
                }
            }
        }
        else
        {
            if (!result || (newdistance < distance && urand(0, abs(distance - newdistance)) > sPlayerbotAIConfig.sightDistance * 0.1))
            {
                distance = newdistance;
                result = unit;
            }
        }
    }

    if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
    {
        if(result)
        {
            ai->TellPlayer(GetMaster(), BOT_TEXT2("%object selected.", (std::map<std::string, std::string>{ {"%object", chat->formatWorldobject(result)} })));
        }
        else
        {
            ai->TellPlayer(GetMaster(), BOT_TEXT("No grind target found."));
        }
    }

    return result;
}

int GrindTargetValue::GetTargetingPlayerCount( Unit* unit )
{
    Group* group = bot->GetGroup();
    if (!group)
        return 0;

    int count = 0;
    Group::MemberSlotList const& groupSlot = group->GetMemberSlots();
    for (Group::member_citerator itr = groupSlot.begin(); itr != groupSlot.end(); itr++)
    {
        Player *member = sObjectMgr.GetPlayer(itr->guid);
        if( !member || !sServerFacade.IsAlive(member) || member == bot)
            continue;

        PlayerbotAI* ai = member->GetPlayerbotAI();
        if ((ai && ai->GetAiObjectContext()->GetValue<ObjectGuid>("current target")->Get() == unit->GetObjectGuid()) ||
            (!ai && member->GetSelectionGuid() == unit->GetObjectGuid()))
            count++;
    }

    return count;
}