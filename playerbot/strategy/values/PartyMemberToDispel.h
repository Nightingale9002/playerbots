#pragma once
#include "playerbot/strategy/Value.h"
#include "PartyMemberValue.h"

namespace ai
{
    class PartyMemberToDispel : public PartyMemberValue, public Qualified
	{
	public:
        PartyMemberToDispel(PlayerbotAI* ai, std::string name = "party member to dispel") :
          PartyMemberValue(ai, name), Qualified() {}

    protected:
        virtual ObjectGuid Calculate() override;
	};

    // [BOSS 战策略 2026-10-19] `team member to cure` —— 队里**第一个**中了可驱散负面光环的成员。
    //   与 PartyMemberToDispel 的区别：
    //     · 限定词是**可读的类型表**（magic / curse / disease / poison，逗号分隔，或数值 1/2/3/4），
    //       省略 = 四种都算；PartyMemberToDispel 只吃单个数值 dispelType。
    //     · 只在自己队伍 + 主号里找（FindPartyMember(predicate, ignoreOutOfGroup = true)），
    //       不会被"组外友好玩家"顶掉。
    //   空/非法限定词：空 ⇒ 全部类型；写了但一个都不认识 ⇒ 不选目标（不崩、不误解）。
    class TeamMemberToCure : public PartyMemberValue, public Qualified
    {
    public:
        TeamMemberToCure(PlayerbotAI* ai, std::string name = "team member to cure") :
          PartyMemberValue(ai, name), Qualified() {}

    protected:
        virtual ObjectGuid Calculate() override;
    };
}
