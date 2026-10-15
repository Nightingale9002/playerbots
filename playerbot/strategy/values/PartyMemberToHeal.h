#pragma once
#include "playerbot/strategy/Value.h"
#include "PartyMemberValue.h"

namespace ai
{
    class PartyMemberToHeal : public PartyMemberValue
	{
	public:
        PartyMemberToHeal(PlayerbotAI* ai, std::string name = "party member to heal") :
          PartyMemberValue(ai, name) {}
    
    protected:
        virtual ObjectGuid Calculate() override;
        bool CanHealPet(Pet* pet);
        virtual bool Check(Unit* player);

    private:
        std::vector<Player*> GetPartyMembers();
	};

    class PartyMemberToProtect : public PartyMemberValue
    {
    public:
        PartyMemberToProtect(PlayerbotAI* ai, std::string name = "party member to protect") :
            PartyMemberValue(ai, name) {}

    protected:
        virtual ObjectGuid Calculate() override;
    };

    // [disc 2026-10-17] Pain Suppression 专用（预判式减伤）。
    //   与 PartyMemberToProtect 的“正在挨打才保护”语义隔离：PS 的价值在于伤害落地前挂 40% 减伤，
    //   故这里用“当前血量 - 预计承伤”推算 8 秒窗口内的最低血量来选择目标。
    class PartyMemberToPainSuppress : public PartyMemberValue
    {
    public:
        PartyMemberToPainSuppress(PlayerbotAI* ai, std::string name = "party member to pain suppress") :
            PartyMemberValue(ai, name) {}

    protected:
        virtual ObjectGuid Calculate() override;
    };

    class PartyMemberToRemoveRoots : public PartyMemberValue
    {
    public:
        PartyMemberToRemoveRoots(PlayerbotAI* ai, std::string name = "party member to remove roots") :
            PartyMemberValue(ai, name) {}

    protected:
        virtual ObjectGuid Calculate() override;
    };
}
