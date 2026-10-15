
#include "playerbot/playerbot.h"
#include "ShamanTriggers.h"
#include "ShamanActions.h"

using namespace ai;

std::list<std::string> ShamanWeaponTrigger::spells;

bool ShamanWeaponTrigger::IsActive()
{
    if (spells.empty())
    {
        spells.push_back("frostbrand weapon");
        spells.push_back("rockbiter weapon");
        spells.push_back("flametongue weapon");
        spells.push_back("earthliving weapon");
        spells.push_back("windfury weapon");
    }

    for (std::list<std::string>::iterator i = spells.begin(); i != spells.end(); ++i)
    {
        uint32 spellId = AI_VALUE2(uint32, "spell id", spell);
        if (!spellId)
            continue;

        if (AI_VALUE2(Item*, "item for spell", spellId))
            return true;
    }

    return false;
}

bool ShockTrigger::IsActive()
{
    return SpellTrigger::IsActive() && !ai->HasAnyAuraOf(GetTarget(), "frost shock", "earth shock", "flame shock", NULL) && !HasMaxDebuffs();
}

bool FlameShockTrigger::IsActive()
{
    return SpellTrigger::IsActive() && !ai->HasAnyAuraOf(GetTarget(), "flame shock", NULL) && !HasMaxDebuffs();
}

// [totem 2026-10-17] 队伍图腾权重：见 ShamanTriggers.h 顶部说明。
namespace
{
    bool IsManualAirTotem(PlayerbotAI* ai)
    {
        return ai->HasStrategy("totem air grace", BotState::BOT_STATE_COMBAT)
            || ai->HasStrategy("totem air grounding", BotState::BOT_STATE_COMBAT)
            || ai->HasStrategy("totem air resistance", BotState::BOT_STATE_COMBAT)
            || ai->HasStrategy("totem air tranquil", BotState::BOT_STATE_COMBAT)
            || ai->HasStrategy("totem air windfury", BotState::BOT_STATE_COMBAT)
            || ai->HasStrategy("totem air windwall", BotState::BOT_STATE_COMBAT)
            || ai->HasStrategy("totem air wrath", BotState::BOT_STATE_COMBAT);
    }

    void AddMemberWeight(PlayerbotAI* ai, Player* member, PartyTotemWeight& w)
    {
        if (!member)
            return;

        if (PlayerbotAI::IsHeal(member))
        {
            w.wrath += 2;
            return;
        }

        if (member->getClass() == CLASS_HUNTER)
        {
            w.grace += 2;
            return;
        }

        if (ai->IsMelee(member, false))
        {
            w.windfury += 2;
            w.strength += 2;
            if (member->getClass() == CLASS_ROGUE || member->getClass() == CLASS_DRUID || member->getClass() == CLASS_SHAMAN)
                w.grace += 1;
            return;
        }

        w.wrath += 2;
    }
}

namespace ai
{
    PartyTotemWeight GetPartyTotemWeight(PlayerbotAI* ai, Player* bot)
    {
        PartyTotemWeight w;

        AddMemberWeight(ai, bot, w);

        Group* group = bot->GetGroup();
        if (!group)
            return w;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->getSource();
            if (!member || member == bot)
                continue;
            if (!sServerFacade.IsAlive(member))
                continue;
            if (!member->IsInWorld() || member->GetMapId() != bot->GetMapId())
                continue;

            AddMemberWeight(ai, member, w);
        }

        return w;
    }
}

bool AirTotemWeightTrigger::IsActive()
{
    if (IsManualAirTotem(ai))
        return false;

    PartyTotemWeight w = GetPartyTotemWeight(ai, bot);

    int top = 0;
    if (w.windfury >= w.grace && w.windfury >= w.wrath)
        top = 0;
    else if (w.grace >= w.windfury && w.grace >= w.wrath)
        top = 1;
    else
        top = 2;

    if (which != top)
        return false;

    std::string totem = which == 0 ? "windfury totem" : (which == 1 ? "grace of air totem" : "wrath of air totem");
    return !AI_VALUE2(bool, "has totem", totem);
}

bool EarthTotemWeightTrigger::IsActive()
{
    PartyTotemWeight w = GetPartyTotemWeight(ai, bot);

    bool wantStrength = w.strength > 0;
    if (strength != wantStrength)
        return false;

    return !AI_VALUE2(bool, "has totem", strength ? "strength of earth totem" : "stoneskin totem");
}
