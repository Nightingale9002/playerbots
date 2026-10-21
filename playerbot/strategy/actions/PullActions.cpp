
#include "playerbot/playerbot.h"
#include "playerbot/strategy/generic/PullStrategy.h"
#include "playerbot/strategy/values/AttackersValue.h"
#include "playerbot/strategy/values/PossibleAttackTargetsValue.h"
#include "PullActions.h"
#include "playerbot/strategy/values/PositionValue.h"

using namespace ai;

bool PullRequestAction::Execute(Event& event)
{
    PullStrategy* strategy = PullStrategy::Get(ai);
    if (!strategy)
    {
        return false;
    }

    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();

    Unit* target = GetTarget(event);
    if (!target)
    {
        ai->TellPlayerNoFacing(requester,BOT_TEXT("You have no target"));
        return false;
    }

    const float maxPullDistance = sPlayerbotAIConfig.reactDistance * 3;
    const float distanceToPullTarget = target->GetDistance(ai->GetBot());
    if (distanceToPullTarget > maxPullDistance)
    {
        ai->TellPlayerNoFacing(requester,BOT_TEXT("The target is too far away"));
        return false;
    }

    if (!AttackersValue::IsValid(target, bot, nullptr, false))
    {
        ai->TellPlayerNoFacing(requester,BOT_TEXT("The target can't be pulled"));
        return false;
    }

    // [2026-10-20 站长口径] 拉怪策略自己找"能拉到的位置"：LOS/距离检查不过时不再原地拒绝，
    //   坦克在周围环形采样找有视线的点，走过去下一 tick 再拉。
    Map* map = bot->GetMap();
    float const bx = bot->GetPositionX(), by = bot->GetPositionY(), bz = bot->GetPositionZ();
    float const tx = target->GetPositionX(), ty = target->GetPositionY(), tz = target->GetPositionZ();
    auto findPullSpot = [&](float radius) -> bool
    {
        for (int i = 0; i < 12; ++i)
        {
            float const ang = float(i) * M_PI_F / 6.0f;
            float const px = bx + radius * cosf(ang);
            float const py = by + radius * sinf(ang);
            float const pz = map->GetHeight(px, py, bz + 2.0f);
            if (pz < -1000.0f)
                continue;
            if (!map->IsInLineOfSight(px, py, pz + 1.5f, tx, ty, tz + 1.0f, false))
                continue;
            bot->GetMotionMaster()->MovePoint(0, px, py, pz);
            return true;
        }
        return false;
    };

    if (!PossibleAttackTargetsValue::CanReachTarget(target, bot) || !strategy->CanDoPullAction(target))
    {
        for (float r : {3.0f, 5.0f, 8.0f, 12.0f})
            if (findPullSpot(r))
                return true;

        // [2026-10-20 站长口径] 四个半径都找不到能拉的位置：用 mmap 寻路继续往怪身上走，
        //   走到进近战范围为止；返回 true 表示本 tick 已处理（在移动），下一 tick 重新判定。
        bot->GetMotionMaster()->MovePoint(0, tx, ty, tz);
        return true;
    }

    //Set position to return to after pulling.
    PositionMap& posMap = AI_VALUE(PositionMap&, "position");
    PositionEntry pullPosition = posMap["pull"];

    pullPosition.Set(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetMapId());
    posMap["pull"] = pullPosition;

    strategy->RequestPull(target);

    // Force change combat state to have a faster reaction time
    ai->OnCombatStarted();

    return true;
}

Unit* PullMyTargetAction::GetTarget(Event& event)
{
    Unit* target = nullptr;

    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    if (event.getSource() == "attack anything")
    {
        ObjectGuid guid = event.getObject();
        target = ai->GetCreature(guid);
    }
    else if (requester)
    {
        target = ai->GetUnit(requester->GetSelectionGuid());
    }

    return target;
}

Unit* PullRTITargetAction::GetTarget(Event& event)
{
    return ai->GetUnit(AI_VALUE(ObjectGuid, "rti target"));
}

bool PullStartAction::Execute(Event& event)
{
    bool result = false;
    PullStrategy* strategy = PullStrategy::Get(ai);
    if (strategy)
    {
        Unit* target = strategy->GetTarget();
        if (target)
        {
            if (strategy->GetPreActionName().empty())
                result = true;
            else
            {
                result = ai->DoSpecificAction(strategy->GetPreActionName(), event, true);
                if(result)
                    SetDuration(3000);
            }

            // Set the pet on passive mode during the pull
            Pet* pet = bot->GetPet();
            if (pet)
            {
                UnitAI* creatureAI = ((Creature*)pet)->AI();
                if (creatureAI)
                {
                    strategy->SetPetReactState(creatureAI->GetReactState());
                    creatureAI->SetReactState(REACT_PASSIVE);
                }
            }

            strategy->OnPullStarted();
        }
    }

    return result;
}


PullAction::PullAction(PlayerbotAI* ai, std::string name) : CastSpellAction(ai, name)
{
    InitPullAction();
}

bool PullAction::Execute(Event& event)
{
    InitPullAction();

    PullStrategy* strategy = PullStrategy::Get(ai);
    if (strategy)
    {
        Unit* target = strategy->GetTarget();
        if (target)
        {
            // Check if we are on pull range
            const float distanceToTarget = target->GetDistance(bot);
            if (distanceToTarget <= strategy->GetRange())
            {
                if (sServerFacade.isMoving(bot))
                {
                    // Force stop
                    ai->StopMoving();
                    strategy->RequestPull(target, false);
                    return false;
                }

                std::string actionName = strategy->GetPullActionName();

                // Execute the pull action
                SET_AI_VALUE(ObjectGuid, "current target", GetTarget()->GetObjectGuid());
                if (ai->DoSpecificAction(actionName, event, true))
                {
                    strategy->RequestPull(target); //extend pull timer to walk back.
                    return true;
                }
                else
                    return false;
            }
            else
            {
                // Retry the reach pull action
                strategy->RequestPull(target, false);
            }
        }
    }

    return false;
}

bool PullAction::isPossible()
{
    InitPullAction();

    PullStrategy* strategy = PullStrategy::Get(ai);
    if (strategy)
    {
        std::string spellName = strategy->GetSpellName();
        Unit* target = strategy->GetTarget();
        if (!spellName.empty() && target)
        {
            if (!ai->CanCastSpell(spellName, target, true, nullptr, true))
            {
                return false;
            }
        }
    }

    return true;
}

void PullAction::InitPullAction()
{
    // Get the pull action spell name from the strategy
    PullStrategy* strategy = PullStrategy::Get(ai);
    if (strategy)
    {
        std::string spellName = strategy->GetSpellName();
        if (!spellName.empty())
        {
            SetSpellName(spellName);

            float spellRange;
            if (ai->GetSpellRange(spellName, &spellRange))
            {
                range = spellRange;
            }
        }
    }
}

bool PullEndAction::Execute(Event& event)
{
    PullStrategy* strategy = PullStrategy::Get(ai);
    if (strategy)
    {
        // Restore the pet react state
        Pet* pet = bot->GetPet();
        if (pet)
        {
            UnitAI* creatureAI = ((Creature*)pet)->AI();
            if (creatureAI)
            {
                creatureAI->SetReactState(strategy->GetPetReactState());
                Unit* target = ai->GetUnit(AI_VALUE(ObjectGuid, "current target"));
                if (creatureAI->GetReactState() != REACT_PASSIVE && target)
                    creatureAI->AttackStart(target);
            }
        }

        // Remove the saved pull position
        AiObjectContext* context = ai->GetAiObjectContext();
        PositionMap& posMap = AI_VALUE(PositionMap&, "position");
        PositionEntry stayPosition = posMap["pull"];
        if (stayPosition.isSet())
        {
            posMap.erase("pull");
        }

        strategy->OnPullEnded();
        return true;
    }

    return false;
}