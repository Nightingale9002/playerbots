
#include "playerbot/playerbot.h"
#include "GenericTriggers.h"
#include "CureTriggers.h"
#include "playerbot/strategy/actions/WorldBuffAction.h"

using namespace ai;

bool NeedCureTrigger::IsActive() 
{
	Unit* target = GetTarget();
	return target && ai->HasAuraToDispel(target, dispelType);
}

Value<ObjectGuid>* PartyMemberNeedCureTrigger::GetTargetValue()
{
	return context->GetValue<ObjectGuid>("party member to dispel", dispelType);
}

// [BOSS 战策略触发器 2026-10-19] `team member needs cure`，见 CureTriggers.h。
Value<ObjectGuid>* TeamMemberNeedsCureTrigger::GetTargetValue()
{
	// 注意这里用 GetValue(name, param) 而不是 GetValue(getQualifier())：
	//   · 限定词为空时得到的是 "team member to cure::"，**名字非空 ⇒ 能正常解析**，
	//     不会走 AiObjectContext::GetValue("") 那条返回 nullptr 再 ->Get() 的崩溃路径；
	//   · 与动作 CureTeamMemberAction::GetTargetQualifier() 拼出的名字一致 ⇒ 两者共用
	//     同一个值对象（同一个队友）。
	return context->GetValue<ObjectGuid>("team member to cure", getQualifier());
}

bool TeamMemberNeedsCureTrigger::IsActive()
{
	Value<ObjectGuid>* value = GetTargetValue();
	if (!value)          // 上下文里没有这个名字 ⇒ 安全返回 false，不崩
		return false;

	return !value->Get().IsEmpty();
}

bool NeedWorldBuffTrigger::IsActive()
{
    return !WorldBuffAction::NeedWorldBuffs(bot).empty();   
}
