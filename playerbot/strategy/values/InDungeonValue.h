#pragma once
#include "playerbot/strategy/Value.h"

namespace ai
{
    // [corpse-run 2026-10-18 站长口径] 我在副本/团本里吗（含英雄本）。
    //   给"只在副本里改行为"的规则当开关用，例如 `DeadStrategy` 用它关掉副本里的自动释放灵魂。
    class InDungeonValue : public BoolCalculatedValue
    {
    public:
        InDungeonValue(PlayerbotAI* ai, std::string name = "in dungeon") : BoolCalculatedValue(ai, name, 2) {}

        virtual bool Calculate() override
        {
            Map* map = bot->GetMap();
            return map && (map->IsDungeon() || map->IsRaid());
        }

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "in dungeon"; }
        virtual std::string GetHelpTypeName() { return "movement"; }
        virtual std::string GetHelpDescription() { return "True when the bot is inside a dungeon or raid map."; }
        virtual std::vector<std::string> GetUsedValues() { return {}; }
#endif
    };
}
