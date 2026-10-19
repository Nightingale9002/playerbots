
#include "playerbot/playerbot.h"
#include "PartyMemberToDispel.h"

#include "playerbot/ServerFacade.h"
using namespace ai;

class PartyMemberToDispelPredicate : public FindPlayerPredicate, public PlayerbotAIAware
{
public:
    PartyMemberToDispelPredicate(PlayerbotAI* ai, uint32 dispelType) :
        PlayerbotAIAware(ai), FindPlayerPredicate(), dispelType(dispelType) {}

public:
    virtual bool Check(Unit* unit) override
    {
        Pet* pet = dynamic_cast<Pet*>(unit);
        if (pet && (pet->getPetType() == MINI_PET || pet->getPetType() == SUMMON_PET))
            return false;

        return sServerFacade.IsAlive(unit) && sServerFacade.GetDistance2d(ai->GetBot(), unit) <= ai->GetRange("spell") && ai->HasAuraToDispel(unit, dispelType);
    }

private:
    uint32 dispelType;
};

ObjectGuid PartyMemberToDispel::Calculate()
{
    uint32 dispelType = atoi(qualifier.c_str());

    PartyMemberToDispelPredicate predicate(ai, dispelType);
    Unit* target = FindPartyMember(predicate);
    return target ? target->GetObjectGuid() : ObjectGuid();
}

// ===========================================================================
// [BOSS 战策略 2026-10-19] `team member to cure`（见 PartyMemberToDispel.h）
// ===========================================================================

namespace
{
    // 可读类型 token → DispelType。返回 false = 不认识这个 token。
    static bool CureTypeFromToken(const std::string& token, uint32& outDispelType)
    {
        if (token == "magic")
            outDispelType = DISPEL_MAGIC;
        else if (token == "curse")
            outDispelType = DISPEL_CURSE;
        else if (token == "disease")
            outDispelType = DISPEL_DISEASE;
        else if (token == "poison")
            outDispelType = DISPEL_POISON;
        else if (Qualified::isValidNumberString(token))
        {
            int32 value = atoi(token.c_str());
            if (value <= DISPEL_NONE || value > DISPEL_ENRAGE)
                return false;

            outDispelType = (uint32)value;
        }
        else
            return false;

        return true;
    }

    // 返回位掩码（1 << DispelType）；限定词为空时返回 0 = "四种常规驱散全都算"。
    // parsed=false 表示"限定词非空但一个 token 都不认识" ⇒ 调用方直接不选目标。
    static uint32 ParseCureDispelMask(const std::string& qualifier, bool& parsed)
    {
        std::vector<uint32> types;
        parsed = qualifier.empty();

        std::vector<std::string> tokens = Qualified::getMultiQualifiers(qualifier, ",");
        for (std::vector<std::string>::const_iterator i = tokens.begin(); i != tokens.end(); ++i)
        {
            if (i->empty())
                continue;

            uint32 dispelType = 0;
            if (!CureTypeFromToken(*i, dispelType))
                continue;

            parsed = true;
            types.push_back(dispelType);
        }

        uint32 mask = 0;
        for (std::vector<uint32>::const_iterator i = types.begin(); i != types.end(); ++i)
            mask |= (1 << *i);

        return mask;
    }

    static bool HasDispellableAuraOfMask(PlayerbotAI* ai, Unit* unit, uint32 dispelMask)
    {
        static const uint32 allTypes[4] = { DISPEL_MAGIC, DISPEL_CURSE, DISPEL_DISEASE, DISPEL_POISON };

        for (int i = 0; i < 4; ++i)
        {
            uint32 dispelType = allTypes[i];
            if (dispelMask && !(dispelMask & (1 << dispelType)))
                continue;

            if (ai->HasAuraToDispel(unit, dispelType))
                return true;
        }

        return false;
    }
}

class TeamMemberToCurePredicate : public FindPlayerPredicate, public PlayerbotAIAware
{
public:
    TeamMemberToCurePredicate(PlayerbotAI* ai, uint32 dispelMask) :
        PlayerbotAIAware(ai), FindPlayerPredicate(), dispelMask(dispelMask) {}

public:
    virtual bool Check(Unit* unit) override
    {
        if (!unit || unit == ai->GetBot())
            return false;

        Pet* pet = dynamic_cast<Pet*>(unit);
        if (pet && (pet->getPetType() == MINI_PET || pet->getPetType() == SUMMON_PET))
            return false;

        if (!sServerFacade.IsAlive(unit))
            return false;

        // 只在自己"够得着"的施法距离内找，避免选到站太远的队友（与 party member to dispel 口径一致）
        if (sServerFacade.GetDistance2d(ai->GetBot(), unit) > ai->GetRange("spell"))
            return false;

        return HasDispellableAuraOfMask(ai, unit, dispelMask);
    }

private:
    uint32 dispelMask;
};

ObjectGuid TeamMemberToCure::Calculate()
{
    bool parsed = false;
    uint32 dispelMask = ParseCureDispelMask(qualifier, parsed);
    if (!parsed)
        return ObjectGuid();

    TeamMemberToCurePredicate predicate(ai, dispelMask);
    Unit* target = FindPartyMember(predicate, true /* ignoreOutOfGroup */);
    return target ? target->GetObjectGuid() : ObjectGuid();
}

