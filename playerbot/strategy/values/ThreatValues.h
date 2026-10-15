#pragma once
#include "playerbot/strategy/Value.h"

namespace ai
{
    class MyThreatValue : public LogCalculatedValue<float>, public Qualified
    {
    public:
        MyThreatValue(PlayerbotAI* ai, std::string name = "my threat") : LogCalculatedValue(ai, name), Qualified() { minChangeInterval = 1; lastTarget = ObjectGuid(); }
        virtual bool EqualToLast(float value) override { return value == lastValue; }
        virtual float Calculate() override;

    public:
        ObjectGuid lastTarget;
    };

    class TankThreatValue : public FloatCalculatedValue, public Qualified
    {
    public:
        TankThreatValue(PlayerbotAI* ai, std::string name = "tank threat") : FloatCalculatedValue(ai, name), Qualified() {}
        virtual float Calculate() override;
    };

    class ThreatValue : public Uint8CalculatedValue, public Qualified
	{
	public:
        ThreatValue(PlayerbotAI* ai, std::string name = "threat") : Uint8CalculatedValue(ai, name), Qualified() {}
    	virtual uint8 Calculate() override;
        static float GetThreat(Player* player, Unit* target);
        static float GetTankThreat(PlayerbotAI* ai, Unit* target);
        // [threat 2026-10-18 站长口径] 队里对这个目标的**最高仇恨**（不限坦克角色，含真人）。
        //   返回 -1 = 没有队伍 / 没有其他人；用于"仇恨基础值"判断（比例判断在没有坦克时失效）。
        static float GetTopGroupThreat(PlayerbotAI* ai, Unit* target);

    protected:
    	uint8 Calculate(Unit* target);
    };
}
