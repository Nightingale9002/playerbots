#pragma once

#include "playerbot/ServerFacade.h"
#include "playerbot/strategy/Action.h"
#include "MovementActions.h"
#include "playerbot/strategy/values/LastMovementValue.h"
#include "ReviveFromCorpseAction.h"
#include "playerbot/TravelMgr.h"

namespace ai
{
    // [corpse-run 2026-10-17 站长口径] 队伍里现在还有没有"活着的、能复活的职业"。
    //   能复活的职业 = 牧师 / 骑士 / 萨满（站长点名这三类）。
    //   判据用职业 + 存活 + 与 bot 同地图：不同地图的复活者救不到副本里的尸体，
    //   不能让他"看着有人会复活"就一直躺尸。
    //   用途：副本里 bot 死后，若队里没有活着的复活职业 ⇒ 不要傻等，直接释放灵魂跑尸。
    inline bool GroupHasLivingRezzer(Player* bot)
    {
        Group* group = bot->GetGroup();
        if (!group)
            return false;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->getSource();
            if (!member || member == bot)
                continue;

            if (member->getClass() != CLASS_PRIEST && member->getClass() != CLASS_PALADIN &&
                member->getClass() != CLASS_SHAMAN)
                continue;

            if (sServerFacade.UnitIsDead(member) || !sServerFacade.IsAlive(member))
                continue;

            if (member->GetMapId() == bot->GetMapId())
                return true;
        }

        return false;
    }

    class ReleaseSpiritAction : public ChatCommandAction
    {
    public:
        ReleaseSpiritAction(PlayerbotAI* ai, std::string name = "release") : ChatCommandAction(ai, name) {}

    public:
        virtual bool Execute(Event& event) override
        {
            Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
            if (sServerFacade.IsAlive(bot))
                return false;

            if (bot->GetCorpse() && bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
            {
                ai->TellPlayerNoFacing(requester,BOT_TEXT("I am already a spirit"), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
                return false;
            }

            WorldPacket& p = event.getPacket();
            if (!p.empty() && p.GetOpcode() == CMSG_REPOP_REQUEST)
                ai->TellPlayerNoFacing(requester,BOT_TEXT("Releasing..."), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
            else
                ai->TellPlayerNoFacing(requester,BOT_TEXT("Meet me at the graveyard"), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);

            sLog.outDetail("Bot #%d %s:%d <%s> released", bot->GetGUIDLow(), bot->GetTeam() == ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName());

            WorldPacket packet(CMSG_REPOP_REQUEST);
            packet << uint8(0);
            bot->GetSession()->HandleRepopRequestOpcode(packet);

            // add waiting for ress aura
            if (bot->InBattleGround() && !ai->HasAura(2584, bot))
            {
                // cast Waiting for Resurrect
                bot->CastSpell(bot, 2584, TRIGGERED_OLD_TRIGGERED);
            }

            return true;
        }
    };

    class AutoReleaseSpiritAction : public ReleaseSpiritAction 
    {
    public:
        AutoReleaseSpiritAction(PlayerbotAI* ai, std::string name = "auto release") : ReleaseSpiritAction(ai, name) {}

        virtual bool Execute(Event& event) override
        {
            sLog.outDetail("Bot #%d %s:%d <%s> auto released", bot->GetGUIDLow(), bot->GetTeam() == ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName());

            WorldPacket packet(CMSG_REPOP_REQUEST);
            packet << uint8(0);
            bot->GetSession()->HandleRepopRequestOpcode(packet);

            // add waiting for ress aura
            if (bot->InBattleGround() && !ai->HasAura(2584, bot))
            {
                // cast Waiting for Resurrect
                bot->CastSpell(bot, 2584, TRIGGERED_OLD_TRIGGERED);
            }

            sPlayerbotAIConfig.logEvent(ai, "AutoReleaseSpiritAction");

            return true;
        }

        virtual bool isUseful() override
        {
            if (!sServerFacade.UnitIsDead(bot))
                return false;

            // [2026-10-21 仿真] 副本里不自动释放灵魂：
            //   放了就会走"跑尸/找灵魂医者"→ RepopAction 把 bot 移出队伍并传回主城
            //   （实测 543 一支队伍 13.8 秒被解散），同时 → 530 的传送被出本守卫拦下后每 tick 重试。
            //   仿真自己会在单人阵亡 8 秒后就地复活（SimScenario `soloReviveAt`）。
            if (sPlayerbotAIConfig.simMode && bot->FindMap() && bot->FindMap()->IsDungeon() && bot->GetGroup())
                return false;

            if (bot->GetCorpse())
                return false;

#ifndef MANGOSBOT_ZERO
            if (bot->InArena())
                return false;
#endif

            if (bot->InBattleGround())
                return !bot->GetCorpse() || !bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST);

            if (bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
                return false;

            if (!bot->GetGroup())
                return true;

            if (!ai->GetGroupMaster())
                return true;

            if (ai->GetGroupMaster() == bot)
                return true;

            if (!ai->HasActivePlayerMaster())
                return true;

            if (ai->HasActivePlayerMaster() && ai->GetGroupMaster()->GetMapId() == bot->GetMapId() && (bot->GetMap()->IsRaid() || bot->GetMap()->IsDungeon()))
            {
                // [corpse-run 2026-10-17 站长口径] 上游原版在副本里**一律不释放灵魂**（原地等真人复活）。
                //   [2026-10-17 二次确认] 本站也一律不释放：**副本里永不自动释放尸体** ——
                //   躺尸等真人复活 / 重置副本，不要再让 bot 自己跑尸（跑回去半路人就没了，反而拖住整队）。
                //   GroupHasLivingRezzer() 保留备用，但不再参与本判定；野外与战场逻辑不受影响。
                return false;
            }

            if(sServerFacade.UnitIsDead(ai->GetGroupMaster()))
                return true;

            if (sServerFacade.IsDistanceGreaterThan(AI_VALUE2(float, "distance", "master target"), sPlayerbotAIConfig.sightDistance))
                return true;

            return false;
        }
    };

    class RepopAction : public SpiritHealerAction 
    {
    public:
        RepopAction(PlayerbotAI* ai, std::string name = "repop") : SpiritHealerAction(ai, name) {}

    public:
        virtual bool Execute(Event& event) override
        {
            // [2026-10-21 仿真] 副本里的仿真队员永远不做"找灵魂医者复活"：
            //   这条路径会 RemoveMember 退出队伍 + 传回主城，等于把仿真队伍拆了。复活交给 SimScenario。
            if (sPlayerbotAIConfig.simMode && bot->FindMap() && bot->FindMap()->IsDungeon() && bot->GetGroup())
                return false;

            Player* requester = event.getOwner() ? event.getOwner() : GetMaster();

            SET_AI_VALUE(uint32, "death count", 0);

            if (bot->IsDead())
            {
                bot->ResurrectPlayer(1.0f, false);
                bot->SpawnCorpseBones();
                bot->SaveToDB();
            }

            
            if (!ai->HasRealPlayerMaster())
            {
                if (Group* group = bot->GetGroup())
                {
                    sLog.outDetail("Repop: Removing bot #%d %s:%d <%s> from group", bot->GetGUIDLow(), bot->GetTeam() == ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName());
                    group->RemoveMember(bot->GetObjectGuid(), 0);
                }
            }

            RESET_AI_VALUE(ObjectGuid, "old target");
            RESET_AI_VALUE(ObjectGuid, "current target");
            RESET_AI_VALUE(ObjectGuid, "pull target");
            RESET_AI_VALUE(bool, "combat::self target");
            RESET_AI_VALUE(WorldPosition, "current position");

            bot->SetSelectionGuid(ObjectGuid());
            ai->TellPlayer(requester, BOT_TEXT("hello"), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);

            TravelTarget* travelTarget = AI_VALUE(TravelTarget*, "travel target");
            sTravelMgr.SetNullTravelTarget(travelTarget);
            travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
            travelTarget->SetExpireIn(1000);

            PlayerInfo const* defaultPlayerInfo = sObjectMgr.GetPlayerInfo(bot->getRace(), bot->getClass());
            if (defaultPlayerInfo)
            {
                sLog.outDetail("Repop: Teleporting bot #%d %s:%d <%s> to spawn", bot->GetGUIDLow(), bot->GetTeam() == ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName());
                //teleport bot to spawn
                bot->TeleportTo(defaultPlayerInfo->mapId, defaultPlayerInfo->positionX, defaultPlayerInfo->positionY, defaultPlayerInfo->positionZ, defaultPlayerInfo->orientation);
                if (bot->isRealPlayer())
                    bot->SendHeartBeat();
            }
            else
            {
                sLog.outDetail("Repop: Teleporting bot #%d %s:%d <%s> to homebind", bot->GetGUIDLow(), bot->GetTeam() == ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName());
                //teleport bot to homebind
                bot->TeleportToHomebind();
                bot->SendHeartBeat();
            }

            sPlayerbotAIConfig.logEvent(ai, "RepopAction");

            return true;
        }

        virtual bool isUseful() override
        {
            if (bot->InBattleGround())
                return false;

            if (ai->HasActivePlayerMaster())
                return false;

            return true;
        }
    };

    class SelfResurrectAction : public ChatCommandAction
    {
    public:
        SelfResurrectAction(PlayerbotAI* ai, std::string name = "self resurrect") : ChatCommandAction(ai, name) {}

    public:
        bool Execute(Event& event) override
        {
            WorldPacket packet(CMSG_SELF_RES);
            bot->GetSession()->HandleSelfResOpcode(packet);
            return true;
        }

        bool isPossible() override
        {
            return ai->IsStateActive(BotState::BOT_STATE_DEAD) && bot->GetUInt32Value(PLAYER_SELF_RES_SPELL);
        }
    };
}
