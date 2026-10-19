#pragma once
#include "playerbot/strategy/Trigger.h"

namespace ai
{
	class SpellTrigger;

    class NeedCureTrigger : public SpellTrigger 
    {
    public:
        NeedCureTrigger(PlayerbotAI* ai, std::string spell, uint32 dispelType, int checkInterval = 3) : SpellTrigger(ai, spell, checkInterval), dispelType(dispelType) {}
        virtual std::string GetTargetName() override { return "self target"; }
        virtual bool IsActive() override;

    protected:
        uint32 dispelType;
    };

    class TargetAuraDispelTrigger : public NeedCureTrigger 
    {
    public:
        TargetAuraDispelTrigger(PlayerbotAI* ai, std::string spell, uint32 dispelType, int checkInterval = 3) : NeedCureTrigger(ai, spell, dispelType, checkInterval) {}
		virtual std::string GetTargetName() override { return "current target"; }
    };

    class PartyMemberNeedCureTrigger : public NeedCureTrigger 
    {
    public:
        PartyMemberNeedCureTrigger(PlayerbotAI* ai, std::string spell, uint32 dispelType) : NeedCureTrigger(ai, spell, dispelType) {}
		virtual Value<ObjectGuid>* GetTargetValue() override;
    };

    // [BOSS 战策略触发器 2026-10-19] `team member needs cure` —— 队里有人中了**可驱散**的负面光环。
    //   为什么需要：`cure poison` / `cleanse ...` 这些只在**各职业的** AiObjectContext 里注册，
    //   共享 TriggerContext 里没有通用名字 ⇒ BOSS 规则行（自定义规则）写不出"给被点名的队友驱散"。
    //   限定词 = 逗号分隔的驱散类型（可省略 = 全部四种）：
    //     magic / curse / disease / poison，或直接写数值 1/2/3/4
    //   目标走值 `team member to cure`（values/PartyMemberToDispel.h），与动作 `cure team member`
    //   （actions/GenericSpellActions.h）共用同一个目标值 —— 两边写**同样的类型 token** 时，
    //   动作拿到的一定就是触发器看到的那个队友。
    class TeamMemberNeedsCureTrigger : public Trigger, public Qualified
    {
    public:
        TeamMemberNeedsCureTrigger(PlayerbotAI* ai, std::string name = "team member needs cure", int checkInterval = 3) :
            Trigger(ai, name, checkInterval), Qualified() {}
        virtual Value<ObjectGuid>* GetTargetValue() override;
        virtual bool IsActive() override;
    };

    class NeedWorldBuffTrigger : public Trigger 
    {
    public:
        NeedWorldBuffTrigger(PlayerbotAI* ai) : Trigger(ai, "need world buff", 5) {}
        virtual bool IsActive() override;
    };
}