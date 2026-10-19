#pragma once

#include "playerbot/strategy/Value.h"
#include "playerbot/strategy/Trigger.h"

namespace ai
{
    // ---------------------------------------------------------------------------
    // [BOSS 战策略 2026-10-16] 数据化 BOSS 规则要用的「角色值」与「触发器取值」。
    //
    // 为什么要有它：一个规则行只能写**一个触发器**；`val`（GenericTriggers.h → ValueTrigger）
    // 把限定词当**值名**求值，而 `and`/`or`/`not`（ValueContext.h）是作用在**值**上的运算符，
    // 于是可以在**一条规则内**写复合条件：
    //     val::and::{trigger::has aura::spellid::36383::greater or equal::4,is melee,is dps} > flee!68
    //
    // 与"集名后缀"（boss_<entry>_dps / _ranged / _dps_melee，由 BossStrategyHook.h → RoleAllows()
    // 在挂载时过滤）**两套并存**，分工口径：
    //   · 整个集都只给某角色        → 用后缀（数据自解释，不用编引擎）
    //   · 集里只有某几条限角色、
    //     或角色要和别的条件复合    → 用本文件的 `val::and::{…,is dps}`（一条规则内表达）
    //
    // 角色判据与引擎其它地方一致：PlayerbotAI::IsTank() / IsHeal() / IsMelee() / IsRanged()；
    // `is dps` = 既非坦克也非治疗（引擎没有现成的 IsDps()）。
    // ---------------------------------------------------------------------------

    class IsTankValue : public BoolCalculatedValue
    {
    public:
        IsTankValue(PlayerbotAI* ai) : BoolCalculatedValue(ai, "is tank") {}
        virtual bool Calculate() override { return PlayerbotAI::IsTank(ai->GetBot()); }
    };

    class IsHealerValue : public BoolCalculatedValue
    {
    public:
        IsHealerValue(PlayerbotAI* ai) : BoolCalculatedValue(ai, "is healer") {}
        virtual bool Calculate() override { return PlayerbotAI::IsHeal(ai->GetBot()); }
    };

    class IsDpsValue : public BoolCalculatedValue
    {
    public:
        IsDpsValue(PlayerbotAI* ai) : BoolCalculatedValue(ai, "is dps") {}
        virtual bool Calculate() override
        {
            Player* bot = ai->GetBot();
            return !PlayerbotAI::IsTank(bot) && !PlayerbotAI::IsHeal(bot);
        }
    };

    class IsMeleeValue : public BoolCalculatedValue
    {
    public:
        IsMeleeValue(PlayerbotAI* ai) : BoolCalculatedValue(ai, "is melee") {}
        virtual bool Calculate() override { return ai->IsMelee(ai->GetBot()); }
    };

    class IsRangedValue : public BoolCalculatedValue
    {
    public:
        IsRangedValue(PlayerbotAI* ai) : BoolCalculatedValue(ai, "is ranged") {}
        virtual bool Calculate() override { return ai->IsRanged(ai->GetBot()); }
    };

    // Bridge: use ANY trigger as a boolean value: `val::and::{trigger::has nearest adds,is dps}`
    //   NOTE: AiObjectContext::GetTrigger() returns an object OWNED BY the trigger context (it caches
    //   it in its own map). Do NOT delete it here and do NOT keep the pointer.
    //   [crash-fix 2026-10-16 - parallel session] The old code cached `trigger` and had
    //   `~TriggerAsValue() { delete trigger; }`. That object is owned by AiObjectContext::triggerContexts,
    //   so deleting it left a dangling pointer inside that map => double free / use-after-free on the next
    //   lookup, and heap corruption right where the AI value system lives (online crash:
    //   CalculatedValue<bool>::Get -> BoolAndValue::Calculate -> indirect call into a std::string buffer).
    //   Resolving the trigger through the context (by name) on every Calculate() is safe: the context
    //   caches it, so at most one trigger object exists, and this value never owns anything.
    class TriggerAsValue : public BoolCalculatedValue, public Qualified
    {
    public:
        TriggerAsValue(PlayerbotAI* ai) : BoolCalculatedValue(ai, "trigger"), Qualified() {}

        virtual bool Calculate() override
        {
            // TODO(boss-session): the same fix belongs in the TriggerAsValue helper of
            // playerbot/BossStrategyHook.* if it caches a Trigger*.
            if (getQualifier().empty() || !ai || !ai->GetAiObjectContext())
                return false;

            Trigger* trigger = ai->GetAiObjectContext()->GetTrigger(getQualifier());
            return trigger ? trigger->IsActive() : false;
        }
    };
}
