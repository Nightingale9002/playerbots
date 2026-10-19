#pragma once
#include "playerbot/strategy/Strategy.h"

namespace ai
{
    class DungeonStrategy : public Strategy
    {
    public:
        DungeonStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        std::string getName() override { return "dungeon"; }

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "dungeon"; } //Must equal iternal name
        virtual std::string GetHelpDescription()
        {
            return "This strategy will enable and disable various dungeon and raid specific strategies as the bot enters and leaves.";
        }
        virtual std::vector<std::string> GetRelatedStrategies() { return {"onyxias lair", "molten core" }; }
#endif

    private:
        void InitCombatTriggers(std::list<TriggerNode*>& triggers) override;
        void InitNonCombatTriggers(std::list<TriggerNode*>& triggers) override;

        // [dodge 2026-10-17 方案A] 通用「躲技能」反应。
        //   放在 reaction 引擎：`AiFactory` 已给 reactionEngine 挂上 "dungeon"，且 reaction 引擎
        //   本身就是"每 tick 一个紧急动作"的语义（`avoid aoe` 也挂在这里）⇒ 躲地板技属于它。
        void InitReactionTriggers(std::list<TriggerNode*>& triggers) override;
    };
}