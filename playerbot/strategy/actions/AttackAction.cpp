
#include "playerbot/playerbot.h"
#include "AttackAction.h"
#include "MotionGenerators/MovementGenerator.h"
#include "AI/BaseAI/CreatureAI.h"
#include "playerbot/LootObjectStack.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/strategy/generic/CombatStrategy.h"
#include "playerbot/strategy/values/ThreatValues.h"

using namespace ai;

bool AttackAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();

    Unit* target = GetTarget();
    if (target && target->IsInWorld() && target->GetMapId() == bot->GetMapId())
    {
        return Attack(requester, target);
    }

    return false;
}

bool AttackMyTargetAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    if(requester)
    {
        const ObjectGuid guid = requester->GetSelectionGuid();
        if (guid)
        {
            if (Attack(requester, ai->GetUnit(guid)))
            {
                SET_AI_VALUE(ObjectGuid, "attack target", guid);
                return true;
            }
        }
        else if (verbose)
        {
            ai->TellError(requester, "You have no target");
        }
    }

    return false;
}

bool AttackRTITargetAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    Unit* rtiTarget = ai->GetUnit(AI_VALUE(ObjectGuid, "rti target"));

    if (rtiTarget && rtiTarget->IsInWorld() && rtiTarget->GetMapId() == bot->GetMapId())
    {
        if (Attack(requester, rtiTarget))
        {
            SET_AI_VALUE(ObjectGuid, "attack target", rtiTarget->GetObjectGuid());
            return true;
        }
    }
    else
    {
        ai->TellError(requester, "I dont see my rti attack target");
    }

    return false;
}

bool AttackMyTargetAction::isUseful()
{
    return !ai->ContainsStrategy(STRATEGY_TYPE_HEAL) || ai->HasStrategy("offdps", BotState::BOT_STATE_COMBAT);
}

bool AttackRTITargetAction::isUseful()
{
    return !ai->ContainsStrategy(STRATEGY_TYPE_HEAL) || ai->HasStrategy("offdps", BotState::BOT_STATE_COMBAT);
}

bool AttackAction::Attack(Player* requester, Unit* target)
{
    MotionMaster &mm = *bot->GetMotionMaster();
	if (mm.GetCurrentMovementGeneratorType() == TAXI_MOTION_TYPE || (bot->IsFlying() && WorldPosition(bot).currentHeight() > 10.0f))
    {
        if (verbose)
        {
            ai->TellPlayerNoFacing(requester,BOT_TEXT("I cannot attack in flight"));
        }

        return false;
    }

    if (IsTargetValid(requester, target))
    {
        if (bot->IsMounted() && (sServerFacade.GetDistance2d(bot, target) < 40.0f || bot->IsFlying()))
        {
            ai->Unmount();
            
            if (bot->IsFlying())
            {
                return true;
            }
        }

        ObjectGuid guid = target->GetObjectGuid();
        bot->SetSelectionGuid(target->GetObjectGuid());

        Unit* oldTarget = ai->GetUnit(AI_VALUE(ObjectGuid, "current target"));
        if(oldTarget)
        {
            SET_AI_VALUE(ObjectGuid, "old target", oldTarget->GetObjectGuid());
        }

        SET_AI_VALUE(ObjectGuid, "current target", target->GetObjectGuid());
        AI_VALUE(LootObjectStack*, "available loot")->Add(guid);

        WaitForAttackStrategy* strategy = WaitForAttackStrategy::Get(ai);
        bool isWaitingForAttack = false;
        Pet* pet = bot->GetPet();

        // [threat 2026-10-17 站长口径] 副本/团本专用门禁（野外恒为 false）：
        //   非坦克 + 队里有活着的坦克 ⇒ 只打"坦克已经有仇恨的目标"或"正在攻击队友的目标"。
        //   放在这里是为了盖住**所有** AttackAction 派生动作（melee / attack anything / dps assist /
        //   attack least hp target / attack rti target ...）—— WaitForAttackMultiplier 的豁免名单
        //   会放过它们（见 CombatStrategy.cpp → WaitForAttackMultiplier::GetValue()）。
        //   这道门禁不依赖 AiPlayerbot.WaitForAttackTime / WaitForAttackThreatPercent 配置。
        if (IsWaitingForTankThreat(target))
            isWaitingForAttack = true;

        // [感知层 2026-10-19] "看着近其实过不去"不再硬追：mmap 明确判死路（PATHFIND_NOPATH）⇒ 不接这次攻击。
        //   值本身有三重限制（同图同实例 / 60 码内 / 约 2 秒缓存），超出范围一律返回可达，
        //   所以这条门禁只在"近处的目标确实没有可行路径"时生效，不会影响远处的正常赶路。
        //   放在这里（而不是 isUseful）是因为 current target 刚在几行前被设成本目标，值才对准它。
        if (!AI_VALUE(bool, "target reachable"))
        {
            sLog.outDetail("[Playerbot][perception] %s 放弃攻击 %s (entry %u): mmap 无可行路径",
                bot->GetName(), target->GetName(), target->GetEntry());
            if (verbose)
                ai->TellPlayerNoFacing(requester, BOT_TEXT("The target can't be reached"));

            return false;
        }

        if (strategy)
        {
            isWaitingForAttack = strategy->ShouldWait(ai) || isWaitingForAttack;
            if (pet)
            {
                UnitAI* creatureAI = ((Creature*)pet)->AI();
                if (creatureAI)
                {
                    // Don't send the pet to attack if the bot is waiting for attack
                    if (!isWaitingForAttack && (!ai->HasStrategy("stay", BotState::BOT_STATE_COMBAT) || AI_VALUE2(float, "distance", "current target") < ai->GetRange("spell")))
                    {
                        // Reset the pet state if no master
                        if (creatureAI->GetReactState() == REACT_PASSIVE && !ai->GetMaster())
                            creatureAI->SetReactState(REACT_DEFENSIVE);
                        else 
                            PetAttack(requester, target);
                    }
                    else
                    {
                        if (!isWaitingForAttack)
                            PetAttack(requester, target);
                        else
                        {
                            strategy->SetPetReactState(creatureAI->GetReactState() != REACT_PASSIVE ? creatureAI->GetReactState() : REACT_PASSIVE);
                            creatureAI->SetReactState(REACT_PASSIVE);

                            // Send pet action packet
                            const ObjectGuid& petGuid = pet->GetObjectGuid();
                            const uint8 flag = ACT_REACTION;
                            const uint32 spellId = REACT_PASSIVE;
                            const uint32 data = (flag << 24) | spellId;

                            WorldPacket packet(CMSG_PET_ACTION);
                            packet << petGuid;
                            packet << data;
                            packet << uint64(0);
                            bot->GetSession()->HandlePetAction(packet);
                            bot->PetSpellInitialize();
                        }
                    }
                }
            }
        }
        else
        {
            // [threat 2026-10-17] 没有"等坦克"策略时也不能让宠物去开"坦克还没仇恨"的怪
            if (!isWaitingForAttack)
                PetAttack(requester, target);
        }

        if (ai->CanMove() && !sServerFacade.IsInFront(bot, target, sPlayerbotAIConfig.sightDistance, CAST_ANGLE_IN_FRONT))
        {
            sServerFacade.SetFacingTo(bot, target);
        }

        bool result = true;

        // Don't attack target if it is waiting for attack or in stealth
        if (!ai->HasStrategy("stealthed", BotState::BOT_STATE_COMBAT) && !isWaitingForAttack)
        {
            // Don't attack a target that has a high damage shield in melee
            if (!ai->IsRanged(bot) || (sServerFacade.GetDistance2d(bot, target) < 5.0f))
            {
                std::set<Aura*> alreadyDone;
                Unit::AuraList const& vDamageShields = target->GetAurasByType(SPELL_AURA_DAMAGE_SHIELD);
                for (Unit::AuraList::const_iterator i = vDamageShields.begin(); i != vDamageShields.end();)
                {
                    if (alreadyDone.find(*i) == alreadyDone.end())
                    {
                        alreadyDone.insert(*i);
                        uint32 damage = (*i)->GetModifier()->m_amount;

                        // If the damage shield does at least 10% of our max hp on each hit we do, we shouldn't attack
                        if (damage >= bot->GetMaxHealth() * 0.10f)
                        {
                            bot->AttackStop();
                            return false;
                        }

                        i = vDamageShields.begin();
                    }
                    else
                        ++i;
                }
            }

            ai->PlayAttackEmote(1);
            result = bot->Attack(target, !ai->IsRanged(bot) || (sServerFacade.GetDistance2d(bot, target) < 5.0f));
        }

        if (result)
        {
            // Force change combat state to have a faster reaction time
            ai->OnCombatStarted();
        }

        return result;
    }

    return false;
}

// [threat 2026-10-17 站长口径] 副本/团本专用门禁："非坦克只打坦克已经有仇恨的目标"。
//   规则（顺序即短路顺序）：
//     1) 不是副本/团本（bot->GetMap()->IsDungeon() || IsRaid()）⇒ 不限制（野外照旧自由开打）；
//     2) 自己就是坦克 ⇒ 不限制（坦克要能自己起手、自己接怪）；
//     3) 队里没有**活着的**坦克 ⇒ 不限制（纯机器人小队、坦克阵亡/不在队里时不能集体发呆）；
//     4) 目标上已经有某个坦克的仇恨（ThreatValue::GetTankThreat() > 0）⇒ 允许打；
//     5) 目标正在攻击队伍成员（含自己）⇒ 允许打（队友被咬了要能还手，这不是"开新怪"）；
//     6) 其余情况 ⇒ 返回 true = 现在先不出手（选目标/朝向可以做，"出手"这一步被拦下）。
//   注意：门禁**不依赖** AiPlayerbot.WaitForAttackTime / WaitForAttackThreatPercent
//   （那两个配置是全局的"等坦克"，关掉 0 时这道副本门禁依然生效）。
bool AttackAction::IsWaitingForTankThreat(Unit* target)
{
    if (!target)
        return false;

    if (!bot->GetMap() || (!bot->GetMap()->IsDungeon() && !bot->GetMap()->IsRaid()))
        return false;

    if (ai->IsTank(bot))
        return false;

    Group* group = bot->GetGroup();
    if (!group)
        return false;

    Player* tank = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->getSource();
        if (member && member != bot && sServerFacade.IsAlive(member) && ai->IsTank(member))
        {
            tank = member;
            break;
        }
    }

    if (!tank)
        return false;

    if (ThreatValue::GetTankThreat(ai, target) > 0)
        return false;

    // 目标当前的受害者在队伍里（含自己）⇒ 它已经在打我们，允许还手
    Unit* victim = target->GetVictim();
    if (victim == bot)
        return false;

    if (Player* victimPlayer = dynamic_cast<Player*>(victim))
    {
        if (victimPlayer->GetGroup() == group)
            return false;
    }

    sLog.outDetail("[threat] %s: hold fire on %s (instance gate: tank %s has no threat on it yet)",
        bot->GetName(), target->GetName(), tank->GetName());

    return true;
}

bool AttackAction::PetAttack(Player* requester, Unit* target)
{
    // If we're done waiting to attack and there's mobs to cc, we can't use defensive/aggressive
    // because non passive pets will ignore our cc
    // Therefore, we'll keep passive so we can only attack the current target specifically
    // In other words, pet only attacks what owner can attack
    Pet* pet = bot->GetPet();
    Unit* ccTarget = ai->GetUnit(AI_VALUE(ObjectGuid, "rti cc target"));
    if (pet && (!ccTarget || (ccTarget && target->GetObjectGuid() != ccTarget->GetObjectGuid())))
    {
        constexpr uint32 PET_IMP = 416;
        constexpr uint32 PHASE_SHIFT = 4511;
        if (!(bot->getClass() == CLASS_WARLOCK &&
            pet->AI() && pet->AI()->HasReactState(REACT_PASSIVE) &&
            pet->GetEntry() == PET_IMP && pet->HasAura(PHASE_SHIFT)))
        {
            // Send pet action packet
            const ObjectGuid& petGuid = pet->GetObjectGuid();
            const ObjectGuid& targetGuid = target->GetObjectGuid();
            const uint8 flag = ACT_COMMAND;
            const uint32 spellId = COMMAND_ATTACK;
            const uint32 command = (flag << 24) | spellId;

            WorldPacket data(CMSG_PET_ACTION);
            data << petGuid;
            data << command;
            data << targetGuid;
            bot->GetSession()->HandlePetAction(data);
        }

        return true;
    }

    return false;
    
}

bool AttackAction::IsTargetValid(Player* requester, Unit* target)
{
    if (!target)
    {
        if (verbose) 
        {
            ai->TellPlayerNoFacing(requester,BOT_TEXT("I have no target"));
        }

        return false;
    }
    else if (sServerFacade.IsFriendlyTo(bot, target))
    {
        if (verbose)
        {
            std::ostringstream msg;
            msg << target->GetName();
            msg << " is friendly to me";
            ai->TellPlayerNoFacing(requester, msg.str());
        }

        return false;
    }
    else if (sServerFacade.UnitIsDead(target))
    {
        if (verbose)
        {
            std::ostringstream msg;
            msg << target->GetName();
            msg << " is dead";
            ai->TellPlayerNoFacing(requester, msg.str());
        }

        return false;
    }
    else if (sServerFacade.GetDistance2d(bot, target) > sPlayerbotAIConfig.sightDistance)
    {
        if (verbose)
        {
            std::ostringstream msg;
            msg << target->GetName();
            msg << " is too far away";
            ai->TellPlayerNoFacing(requester, msg.str());
        }

        return false;
    }

    return true;
}

bool AttackDuelOpponentAction::isUseful()
{
    return ai->GetUnit(AI_VALUE(ObjectGuid, "duel target"));
}

bool AttackDuelOpponentAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    return Attack(requester, ai->GetUnit(AI_VALUE(ObjectGuid, "duel target")));
}
