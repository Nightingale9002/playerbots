#pragma once
#include "playerbot/strategy/Strategy.h"

namespace ai
{
    // [副本推进 2026-10-18 站长口径] 运行期按波推进（查 route 表 + 拉怪锚点，见 DungeonRoute.h）。
    //   只对 ai_playerbot_dungeon_route.verified = 1 的副本生效；未验证的图完全回落原逻辑。
    //   开关：AiPlayerbot.DungeonRoute（默认 1）。
    class DungeonRouteStrategy : public Strategy
    {
    public:
        DungeonRouteStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        std::string getName() override { return "dungeon route"; }

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "dungeon route"; }
        virtual std::string GetHelpDescription()
        {
            return "This strategy drives the tank through a verified dungeon route wave by wave.";
        }
        virtual std::vector<std::string> GetRelatedStrategies() { return {"dungeon"}; }
#endif

    private:
        void InitNonCombatTriggers(std::list<TriggerNode*>& triggers) override;
    };
}
