#include "playerbot/strategy/Action.h"
#include "ChooseTargetActions.h"
#include "MotionGenerators/MovementGenerator.h"
#include "AI/BaseAI/CreatureAI.h"
#include "playerbot/TravelMgr.h"
#include "playerbot/strategy/generic/PullStrategy.h"
#include "playerbot/strategy/values/FreeMoveValues.h"
#include "playerbot/strategy/values/PossibleAttackTargetsValue.h"

bool DpsAssistAction::isUseful()
{
    // if carry flag, do not start fight
    if (bot->HasAura(23333) || bot->HasAura(23335) || bot->HasAura(34976))
        return false;

    // [threat 2026-10-17 站长口径] 副本/团本里"非坦克只打坦克已有仇恨的目标"：
    //   连"选"都不选（不走去打、不放技能）。判定与"出手"拦截同一处：
    //   AttackAction::IsWaitingForTankThreat()（内部自带 IsDungeon()/IsRaid() 门禁 ⇒ 野外不受影响）。
    if (IsWaitingForTankThreat(GetTarget()))
        return false;

    return true;
}

bool AttackAnythingAction::isUseful() 
{
    if (!ai->AllowActivity(GRIND_ACTIVITY)) //Bot not allowed to be active
        return false;

    if (!AI_VALUE(bool, "can move around"))
        return false;

    // [pull 2026-10-16 站长口径] 队伍里**有坦克**（或真人在队里）时，非坦克不许自己开怪：
    //   坦克/真人负责拉怪，其他人只打已经进入战斗的目标（由 tank assist / wait for attack 那条路处理）。
    //   "纯机器人队伍也按同样规则" ⇒ 判据是"队里有没有坦克/真人"，不是"有没有真人 master"。
    //   单人/没有坦克的纯机器人小队保持原行为（会自己找怪打）。
    if (!ai->IsTank(bot))
    {
        bool groupHasTankOrPlayer = false;
        if (Group* group = bot->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->getSource();
                if (!member || member == bot)
                    continue;

                if (member->isRealPlayer() || ai->IsTank(member))
                {
                    groupHasTankOrPlayer = true;
                    break;
                }
            }
        }

        if (groupHasTankOrPlayer)
            return false;
    }

    Unit* target = GetTarget();

    if (!target || !ai->IsSafe(target))
        return false;

    // [pull 2026-10-18 站长口径] 坦克的开怪目标必须"同一层且走得到"：多楼层副本（卡拉赞 / 黑石深渊 /
    //   厄运之槌）里"看得见但其实是另一层"的怪会被选中 ⇒ 坦克直奔过去（路径断了也照跑），路上打它的怪
    //   也不还手。判据见 PossibleAttackTargetsValue::CanReachTarget()（|Δz| + LOS，远目标才做路网校验）。
    if (ai->IsTank(bot) && !PossibleAttackTargetsValue::CanReachTarget(target, bot))
        return false;

    if (ai->ContainsStrategy(STRATEGY_TYPE_HEAL) && !ai->HasStrategy("offdps", BotState::BOT_STATE_COMBAT))
        return false;

    // [pull 2026-10-16 站长口径] **副本/团本里不许"迎面看到怪就先动手"**（原第 34 行那条
    // "Attack before being attacked" 在副本里也会触发 ⇒ bot 走进副本就自己开怪）。
    // 判据用"**队伍是否已经在战斗**"而不是"目标是否在战斗"：只要坦克/谁已经开怪，所有敌对目标
    // （包括 BOSS 召唤出来的小怪）都能打；战斗没开始前谁都不许先动手。
    // [pull 2026-10-17 站长口径] **坦克豁免**：坦克就是负责开怪的，副本里必须能自己起手
    // （起手目标会走 AttackAnythingAction::Execute() 里的 "pull my target" 拉怪路径）；
    // 这一豁免只跳过后面的"队伍已进战斗"门禁，上面"非坦克 + 队里有坦克/真人 ⇒ 不许自己开怪"
    // 那条对非坦克完全不变。
    if (!ai->IsTank(bot) && bot->GetMap() && (bot->GetMap()->IsDungeon() || bot->GetMap()->IsRaid()))
    {
        bool groupInCombat = bot->IsInCombat();
        if (!groupInCombat)
        {
            if (Group* group = bot->GetGroup())
            {
                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                {
                    Player* member = ref->getSource();
                    if (member && member->IsInCombat())
                    {
                        groupInCombat = true;
                        break;
                    }
                }
            }
        }

        if (!groupInCombat)
            return false;
    }

    if(!target->IsPlayer() && bot->isInFront(target,target->GetAttackDistance(bot)*1.5f, M_PI_F*0.5f) && target->CanAttackOnSight(bot) && target->GetLevel() < bot->GetLevel() + 3.0) //Attack before being attacked.
        return true;

    if (AI_VALUE(bool, "travel target traveling") && CanFreeMoveValue::CanFreeMoveTo(ai, *AI_VALUE(TravelTarget*,"travel target")->GetPosition())) //Bot is traveling
        return false;

    return true;
}

bool ai::AttackAnythingAction::isPossible()
{
    return AttackAction::isPossible() && GetTarget();
}

bool ai::AttackAnythingAction::Execute(Event& event)
{
    Unit* grindTarget = GetTarget();

    // [pull 2026-10-18 站长口径] 目标已经不可达（不在同一层 / 没有导航路径）⇒ 放弃，绝不退化成"直线跑过去"：
    //   清 attack target / current target，让选择逻辑重新挑（坦克会走 tank assist，见 TankTargetValue.cpp）。
    if (ai->IsTank(bot) && grindTarget && !PossibleAttackTargetsValue::CanReachTarget(grindTarget, bot))
    {
        sLog.outDetail("[Playerbot][pull] %s 放弃不可达的开怪目标 %s (entry %u): 不同层或没有导航路径",
            bot->GetName(), grindTarget->GetName(), grindTarget->GetEntry());

        SET_AI_VALUE(ObjectGuid, "attack target", ObjectGuid());
        SET_AI_VALUE(ObjectGuid, "current target", ObjectGuid());
        bot->SetSelectionGuid(ObjectGuid());
        bot->AttackStop();
        ai->StopMoving();
        return false;
    }

    bool result = AttackAction::Execute(event);
    if (result)
    {
        Unit* grindTarget = GetTarget();
        if (grindTarget)
        {
            std::string grindName = grindTarget->GetName();
            if (!grindName.empty())
            {
                sPlayerbotAIConfig.logEvent(ai, "AttackAnythingAction", grindName, std::to_string(grindTarget->GetEntry()));

                if (ai->HasStrategy("pull", BotState::BOT_STATE_COMBAT))
                {
                    if (PullStrategy* strategy = PullStrategy::Get(ai))
                    {
                        if (strategy->CanDoPullAction(grindTarget) && (ai->GetBot()->getClass() == CLASS_DRUID || ai->GetBot()->getClass() == CLASS_PALADIN || AI_VALUE2(uint32, "item count", "ammo")))
                        {
                            Event pullEvent("attack anything", grindTarget->GetObjectGuid());
                            bool doAction = ai->DoSpecificAction("pull my target", pullEvent, true);

                            if (doAction)
                            {
                                return true;
                            }
                        }
                    }
                }

                context->GetValue<ObjectGuid>("attack target")->Set(grindTarget->GetObjectGuid());
                ai->StopMoving();
            }
        }
    }

    return result;
}

bool AttackEnemyPlayerAction::isUseful()
{
    return !sPlayerbotAIConfig.IsInPvpProhibitedZone(sServerFacade.GetAreaId(bot));
}

bool AttackEnemyFlagCarrierAction::isUseful()
{
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    Unit* target = ai->GetUnit(context->GetValue<ObjectGuid>("enemy flag carrier")->Get());
    return target && sServerFacade.IsDistanceLessOrEqualThan(sServerFacade.GetDistance2d(bot, target), 75.0f) && (bot->HasAura(23333) || bot->HasAura(23335) || bot->HasAura(34976));
}

bool SelectNewTargetAction::Execute(Event& event)
{
    // [2026-10-18 输出修复] 读条中**不要因为重选目标而打断施法**：
    //   旧行为下面会 `ai->InterruptSpell()`，而 "invalid target" 触发器频率很高
    //   （目标暂时超距离 / selection guid 不同步 / 目标切波都会触发）⇒ 法系大量读条被自己打断。
    //   目标真死了也不影响：读条结束服务端会判失败，下一 tick 再重选即可。
    if (bot->IsNonMeleeSpellCasted(true))
        return true;

    Unit* target = ai->GetUnit(AI_VALUE(ObjectGuid, "current target"));
    if (target && sServerFacade.UnitIsDead(target))
    {
        // Save the dead target for later looting
        ObjectGuid guid = target->GetObjectGuid();
        if (guid)
        {
            AI_VALUE(LootObjectStack*, "available loot")->Add(guid);
        }
    }

    // Clear the target variables
    ObjectGuid attackTarget = AI_VALUE(ObjectGuid, "attack target");
    std::list<ObjectGuid> possible = AI_VALUE(std::list<ObjectGuid>, "possible targets no los");
    if (attackTarget && find(possible.begin(), possible.end(), attackTarget) == possible.end())
    {
        SET_AI_VALUE(ObjectGuid, "attack target", ObjectGuid());
    }

    // Save the old target and clear the current target
    if(target)
    {
        SET_AI_VALUE(ObjectGuid, "old target", target->GetObjectGuid());
        SET_AI_VALUE(ObjectGuid, "current target", ObjectGuid());
    }
    
    // Stop attacking
    bot->SetSelectionGuid(ObjectGuid());
    ai->InterruptSpell();
    bot->AttackStop();
    // Stop pet attacking
    Pet* pet = bot->GetPet();
    if (pet)
    {
        UnitAI* creatureAI = ((Creature*)pet)->AI();
        if (creatureAI)
        {
            // Send pet action packet
            const ObjectGuid& petGuid = pet->GetObjectGuid();
            const ObjectGuid& targetGuid = ObjectGuid();
            const uint8 flag = ACT_COMMAND;
            const uint32 spellId = COMMAND_FOLLOW;
            const uint32 command = (flag << 24) | spellId;

            WorldPacket data(CMSG_PET_ACTION);
            data << petGuid;
            data << command;
            data << targetGuid;
            bot->GetSession()->HandlePetAction(data);
        }
    }

    bool moreAttackers = false;
    // Check if there is any enemy targets available to attack
    if (AI_VALUE(bool, "has attackers"))
    {
        if (ai->HasStrategy("pvp", BotState::BOT_STATE_COMBAT) ||
            ai->HasStrategy("duel", BotState::BOT_STATE_COMBAT))
        {
            // Check if there is an enemy player nearby
            if (AI_VALUE(bool, "has enemy player targets"))
            {
                moreAttackers = true;
                return ai->DoSpecificAction("attack enemy player", event, true);
            }
        }

        // Let the dps/tank assist pick a target to attack
        if (ai->HasStrategy("dps assist", BotState::BOT_STATE_NON_COMBAT))
        {
            moreAttackers = true;
            return ai->DoSpecificAction("dps assist", event, true);
        }
        else if (ai->HasStrategy("tank assist", BotState::BOT_STATE_NON_COMBAT))
        {
            moreAttackers = true;
            return ai->DoSpecificAction("tank assist", event, true);
        }
    }

    return false;
}