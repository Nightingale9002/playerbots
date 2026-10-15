#pragma once
#include "playerbot/strategy/Value.h"
#include "TargetValue.h"

namespace ai
{
    class SpellIdValue : public CalculatedValue<uint32>, public Qualified
	{
	public:
        SpellIdValue(PlayerbotAI* ai);
        virtual uint32 Calculate() override;
    };

    class VehicleSpellIdValue : public CalculatedValue<uint32>, public Qualified
    {
    public:
        VehicleSpellIdValue(PlayerbotAI* ai);
        virtual uint32 Calculate() override;
    };

    // [disc 2026-10-17] 治疗法术动态 rank：按“目标缺血量 + 预计承伤”选能满足的最小 rank（省蓝、平滑）。
    //   限定词 = 法术名（如 "healing wave"）。算不出目标/rank（或只有一个 rank）时返回 0，
    //   调用方回退到原二值 "spell id" 逻辑。
    class HealRankValue : public CalculatedValue<uint32>, public Qualified
    {
    public:
        HealRankValue(PlayerbotAI* ai);
        virtual uint32 Calculate() override;
    };
}
