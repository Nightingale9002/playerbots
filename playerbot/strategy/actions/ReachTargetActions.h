#pragma once

#include "playerbot/strategy/Action.h"
#include "MovementActions.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/strategy/generic/PullStrategy.h"
#include "playerbot/strategy/NamedObjectContext.h"
#include "playerbot/strategy/values/MoveStyleValue.h"
#include "GenericSpellActions.h"
#include "playerbot/PlayerbotFactory.h"

namespace ai
{
    class ReachTargetAction : public MovementAction, public Qualified
    {
    public:
        ReachTargetAction(PlayerbotAI* ai, std::string name, float range = 0.0f) : MovementAction(ai, name), Qualified(), range(range), spellName("") {}

        virtual void Qualify(const std::string& qualifier) override
        {
            Qualified::Qualify(qualifier);

            // Get the distance from the qualified spell given
            if (!qualifier.empty())
            {
                spellName = Qualified::getMultiQualifierStr(qualifier, 0, "::");

                float maxSpellRange;
                if (ai->GetSpellRange(spellName, &maxSpellRange))
                {
                    range = maxSpellRange;
                }
            }
        }

        // [2026-10-30 站长口径] **引擎的接近距离：远程 bot 打到射程就停，不贴脸**。
        //   原来 reach 动作直接按调用方给的 range 计算 offset：`reach melee` 的 range=0（且不查距离），
        //   于是任何走到它面前的施法动作（猎人的陷阱 / 近战技 / 其它 reach melee 前置）都会把
        //   带 "ranged" 策略的 bot 一路拽到 2~3 码，自动射击从此进死区（实测 18.9 → 2.2）。
        //   这里给"敌对目标 + 有 ranged 策略"加下限 = 射程（AiPlayerbot.ShootDistance，兜底 25），
        //   近战 bot（没有 ranged 策略）仍按调用方给的 range 走（0 = 贴脸）。
        virtual float GetApproachRange(Unit* target) const
        {
            if (target && sServerFacade.IsHostileTo(bot, target) &&
                ai->HasStrategy("ranged", BotState::BOT_STATE_COMBAT))
            {
                float shootDistance = ai->GetRange("shoot");
                if (shootDistance <= 0.0f)
                    shootDistance = 25.0f;   // 配置缺失时的兜底射程

                if (range < shootDistance)
                    return shootDistance;
            }

            return range;
        }

        virtual bool Execute(Event& event) override
		{
            Unit* target = GetTarget();
            if (target)
            {
                UpdateMovementState();

                // Ignore movement if too far
                const float distanceToTarget = bot->GetDistance(target, false, DIST_CALC_COMBAT_REACH);
                const float approachRange = GetApproachRange(target);
                float chaseDist = approachRange;
                const bool inLos = bot->IsWithinLOSInMap(target, true);
                const bool isFriend = sServerFacade.IsFriendlyTo(bot, target);

                if (approachRange > 0.0f)
                {
                    chaseDist = inLos ? approachRange : (isFriend ? std::min(distanceToTarget * 0.9f, approachRange) : approachRange);
                    chaseDist = (chaseDist - sPlayerbotAIConfig.contactDistance);
                }

                if (MoveStyleValue::WaitForEnemy(ai) && !ai->GetUnit(AI_VALUE(ObjectGuid, "rti cc target")) && target->m_movementInfo.HasMovementFlag(movementFlagsMask) &&
                        sServerFacade.IsInFront(target, bot, sPlayerbotAIConfig.sightDistance, CAST_ANGLE_IN_FRONT) &&
                        sServerFacade.IsDistanceGreaterThan(distanceToTarget, sPlayerbotAIConfig.tooCloseDistance))
                {
                    return true;
                }                 

                if (ai->HasStrategy("debug move", BotState::BOT_STATE_NON_COMBAT))
                {
                    std::ostringstream out;
                    out << "Moving to reach " << ChatHelper::formatWorldobject(target);
                    ai->TellPlayerNoFacing(GetMaster(), out);
                }
                
                if (inLos && isFriend && (range <= ai->GetRange("follow")))
                {
                    return MoveNear(target, chaseDist);
                }
                else
                {
                    return ChaseTo(target, chaseDist, bot->GetAngle(target));
                }
            }

            return false;
        }

        virtual bool isUseful() override
		{
            // Do not move if stay strategy is set
            if (!ai->HasStrategy("stay", ai->GetState()))
            {
                Unit* target = GetTarget();
                if (target)
                {
                    // Do not move while casting
                    if (!bot->IsNonMeleeSpellCasted(true, false, true))
                    {
                        // Check if the spell for which the reach action is used for can be casted
                        if (!spellName.empty() && !ai->CanCastSpell(spellName, target, true, nullptr, true, true, true))
                        {
                            return false;
                        }

                        // Force move if not in los
                        if (bot->IsWithinLOSInMap(target, true))
                        {
                            // Check if the bot is already on the range required
                            // [2026-10-30] 同一套"接近距离"：远程 bot 在射程内就不要再走近战位
                            return bot->GetDistance(target, true, DIST_CALC_COMBAT_REACH) > GetApproachRange(target);
                        }

                        return true;
                    }
                }
            }

            return false;
        }

        virtual std::string GetTargetName() override { return "current target"; }
        std::string GetSpellName() const { return spellName; }

        virtual Unit* GetTarget() override
        {
            // Get the target from the qualifiers
            if (!qualifier.empty())
            {
                std::string targetQualifier;
                const std::string targetName = Qualified::getMultiQualifierStr(qualifier, 1, "::");
                if (targetName != "current target")
                {
                    targetQualifier = Qualified::getMultiQualifierStr(qualifier, 2, "::");
                }

                return targetQualifier.empty() ? ai->GetUnit(AI_VALUE(ObjectGuid, targetName)) : ai->GetUnit(AI_VALUE2(ObjectGuid, targetName, targetQualifier));
            }
            else
            {
                return ai->GetUnit(AI_VALUE(ObjectGuid, GetTargetName()));
            }
        }

    protected:
        float range;
        std::string spellName;
    };

    class CastReachTargetSpellAction : public CastSpellAction
    {
    public:
        CastReachTargetSpellAction(PlayerbotAI* ai, std::string spell, float distance) : CastSpellAction(ai, spell), distance(distance) {}

		virtual bool isUseful() override
		{
            // Do not move if stay strategy is set
            if (ai->HasStrategy("stay", ai->GetState()))
                return false;

			return sServerFacade.IsDistanceGreaterThan(AI_VALUE2(float, "distance", "current target"), (distance + sPlayerbotAIConfig.contactDistance));
		}

    protected:
        float distance;
    };

    class ReachMeleeAction : public ReachTargetAction
	{
    public:
        ReachMeleeAction(PlayerbotAI* ai) : ReachTargetAction(ai, "reach melee") {}
    };

    class ReachSpellAction : public ReachTargetAction
	{
    public:
        ReachSpellAction(PlayerbotAI* ai) : ReachTargetAction(ai, "reach spell", ai->GetRange("spell")) {}
    };

    class ReachPullAction : public ReachTargetAction
    {
    public:
        ReachPullAction(PlayerbotAI* ai) : ReachTargetAction(ai, "reach pull")
        {
            PullStrategy* strategy = PullStrategy::Get(ai);
            if (strategy)
            {
                range = strategy->GetRange();
            }
        }

        void Qualify(const std::string& qualifier) override
        {
            ReachTargetAction::Qualify(qualifier);

            // Reduce the range slightly for a more accurate pull (moving targets can get out of reach)
            const float threshold = 5.0f;
            range = range > threshold ? range - threshold : range;
        }

        std::string GetTargetName() override { return "pull target"; }
    };

    class ReachPartyMemberToHealAction : public ReachTargetAction
    {
    public:
        ReachPartyMemberToHealAction(PlayerbotAI* ai) : ReachTargetAction(ai, "reach party member to heal", ai->GetRange("heal")) {}
        virtual std::string GetTargetName() override { return "party member to heal"; }
    };

    class ReachPartyMemberForTotemAction : public ReachTargetAction
    {
    public:
        ReachPartyMemberForTotemAction(PlayerbotAI* ai)
            : ReachTargetAction(ai, "reach party member for totem", 20.0f) {}

        void Qualify(const std::string& qualifier) override
        {
            ReachTargetAction::Qualify(qualifier);
            range = 20.0f;
        }

        Unit* GetTarget() override
        {
            std::string totemSpell = spellName;
            if (totemSpell.empty() && !qualifier.empty())
                totemSpell = Qualified::getMultiQualifierStr(qualifier, 0, "::");

            Group* group = bot->GetGroup();
            if (!group || totemSpell.empty())
                return ai->GetUnit(AI_VALUE(ObjectGuid, "master target"));

            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->getSource();
                if (!member || !sServerFacade.IsAlive(member))
                    continue;

                if (member == bot)
                    continue;

                if (!member->IsInWorld() || member->GetMapId() != bot->GetMapId())
                    continue;

                if (!bot->IsWithinDistInMap(member, 100.0f, false))
                    continue;

                if (!bot->IsWithinDistInMap(member, range, false) || !AI_VALUE2(bool, "has totem", name))
                    return member; 
            }

            return nullptr;
        }
    };
}
