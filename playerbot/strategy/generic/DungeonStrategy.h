#pragma once
#include "playerbot/strategy/Strategy.h"

namespace ai
{
    // [dodge 2026-10-18 站长口径] **任何情况优先躲地板**。
    //   现场反馈"在躲地板和其他策略之间来回冲突"：原来只有 reaction 引擎带 `avoid aoe` 的乘数、
    //   combat 引擎没有任何压制 ⇒ 走位动作刚起步就被 `reach`/`attack`/读条顶回去，形成来回抖。
    //   这个乘数挂在 combat 引擎（`dungeon` 策略所有职业都有）：脚下有敌方 AOE / 已经中招时，
    //   除"躲地板/转向/换目标"以外全部压成 0，只让躲地板跑。
    class AoeDodgePriorityMultiplier : public Multiplier
    {
    public:
        AoeDodgePriorityMultiplier(PlayerbotAI* ai) : Multiplier(ai, "aoe dodge priority") {}
        virtual float GetValue(Action* action);
    };

    class DungeonStrategy : public Strategy
    {
    public:
        DungeonStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        std::string getName() override { return "dungeon"; }

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() override { return "dungeon"; } //Must equal iternal name
        virtual std::string GetHelpDescription()
        {
            return "This strategy will enable and disable various dungeon and raid specific strategies as the bot enters and leaves.";
        }
        virtual std::vector<std::string> GetRelatedStrategies() { return {"onyxias lair", "molten core" }; }
#endif

    private:
        void InitCombatTriggers(std::list<TriggerNode*>& triggers) override;
        void InitCombatMultipliers(std::list<Multiplier*>& multipliers) override;
        void InitNonCombatTriggers(std::list<TriggerNode*>& triggers) override;

        // [dodge 2026-10-17 方案A] 通用「躲技能」反应。
        //   放在 reaction 引擎：`AiFactory` 已给 reactionEngine 挂上 "dungeon"，且 reaction 引擎
        //   本身就是"每 tick 一个紧急动作"的语义（`avoid aoe` 也挂在这里）⇒ 躲地板技属于它。
        void InitReactionTriggers(std::list<TriggerNode*>& triggers) override;
    };
}