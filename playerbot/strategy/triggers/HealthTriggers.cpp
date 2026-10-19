
#include "playerbot/playerbot.h"
#include "HealthTriggers.h"

using namespace ai;

float HealthInRangeTrigger::GetValue()
{
    return AI_VALUE2(uint8, "health", GetTargetName());
}

bool HealthInRangeTrigger::IsActive()
{
    bool healthCheck = false;
    if (ai->HasStrategy("preheal", BotState::BOT_STATE_COMBAT))
    {
        Unit* target = GetTarget();
        if (target)
        {
            uint32 incDmg = AI_VALUE2(uint32, "incoming damage", GetTargetName());
            if (incDmg)
            {
                float healthPercent = GetValue();
                float incDmgPercent = float(float(incDmg) / float(target->GetMaxHealth())) * 100;
                float healthPredict = healthPercent;
                if (incDmgPercent >= healthPercent)
                    healthPredict = 0.f;
                else
                    healthPredict = healthPercent - incDmgPercent;

                healthCheck = healthPredict < maxValue && healthPredict >= minValue;
                if (healthCheck && ai->HasStrategy("debug", BotState::BOT_STATE_NON_COMBAT))
                {
                    std::string msg = GetTargetName() + " hp: " + std::to_string(healthPercent) + ", predicted: " + std::to_string(healthPredict);
                    ai->TellPlayerNoFacing(GetMaster(), msg);
                }
            }
            else
                healthCheck = ValueInRangeTrigger::IsActive();
        }
    }
    else
        healthCheck = ValueInRangeTrigger::IsActive();

    return healthCheck
        && !AI_VALUE2(bool, "dead", GetTargetName())
        && (!isTankRequired || (GetTarget()->IsPlayer() && ai->IsTank((Player*)GetTarget(), false)));
}

bool PartyMemberDeadTrigger::IsActive()
{
	return GetTarget();
}

// [BOSS 战策略触发器 2026-10-19] `target health below::<n>[::<m>]`：
//   复用"目标血量百分比"取值（AI_VALUE2(uint8,"health","current target")），档位来自限定词，
//   再交给 TargetLowHealthTrigger::IsActive() 复用它的 preheal 预测与"目标已死 ⇒ false"判定。
bool TargetHealthBelowTrigger::IsActive()
{
    std::vector<std::string> tokens = Qualified::getMultiQualifiers(getQualifier(), "::");

    // 空限定词 / 非法限定词防崩：缺第一个数字就直接 false（绝不 GetValue("")->Get()，
    // 也不退化成"任何掉血都触发"）。
    if (tokens.empty() || !Qualified::isValidNumberString(tokens[0]))
        return false;

    float maxValue = (float)atoi(tokens[0].c_str());
    float minValue = 0.0f;
    if (tokens.size() >= 2 && Qualified::isValidNumberString(tokens[1]))
        minValue = (float)atoi(tokens[1].c_str());

    if (maxValue <= 0.0f || maxValue > 100.0f || minValue < 0.0f || minValue >= maxValue)
        return false;

    this->maxValue = maxValue;
    this->minValue = minValue;

    float health = GetValue();
    if (health >= maxValue || health < minValue)
        return false;

    return TargetLowHealthTrigger::IsActive();
}

bool DeadTrigger::IsActive()
{
    return AI_VALUE2(bool, "dead", GetTargetName());
}

bool AoeHealTrigger::IsActive()
{
    return AI_VALUE2(uint8, "aoe heal", type) >= count;
}

bool HealTargetFullHealthTrigger::IsActive()
{
    // [precast 2026-10-19 站长口径] "预读打断应该对所有治疗技能都起效"。
    //   原来只看 CURRENT_GENERIC_SPELL（普通读条），于是：
    //     · 引导类治疗（CURRENT_CHANNELED_SPELL，如宁静）永远判不到；
    //     · `IsHealSpell` 的效果白名单也漏了机械治疗/团队区域光环等类型。
    //   现在两个施法槽都看，并且用统一的 `IsHealSpell`（已补全效果类型）。
    static CurrentSpellTypes const kCastSlots[] = { CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL };

    for (CurrentSpellTypes slot : kCastSlots)
    {
        Spell* currentSpell = bot->GetCurrentSpell(slot);
        if (!currentSpell || currentSpell->getState() != SPELL_STATE_CASTING || currentSpell->GetCastedTime() == 0U)
            continue;

        // Interrupt pre casted heals if target is not injured.
        if (!PlayerbotAI::IsHealSpell(currentSpell->m_spellInfo))
            continue;

        std::string status = "fullhp";
        Unit* pTarget = currentSpell->m_targets.getUnitTarget();
        if (!pTarget)
            continue;

        bool hpFull = pTarget->GetHealth() == pTarget->GetMaxHealth();
        if (!hpFull && (pTarget->GetHealthPercent() > 90.f))
        {
            uint32 healValue = currentSpell->GetDamage();
            uint32 needHeal = pTarget->GetMaxHealth() - pTarget->GetHealth();
            if (healValue > needHeal && float((needHeal * 100.0f) / healValue) < 50.0f)
            {
                status = "almost fullhp";
                hpFull = true;
            }
        }

        if (!hpFull)
            continue;

        uint32 manaCost = currentSpell->GetPowerCost();
        if (ai->HasStrategy("debug", BotState::BOT_STATE_NON_COMBAT))
        {
            std::string msg = "target " + status + ", can save " + std::to_string(manaCost) + " mana, cast left : " + std::to_string(currentSpell->GetCastedTime()) + "ms";
            ai->TellPlayerNoFacing(GetMaster(), msg);
        }

        return true;
    }

    return false;
}
