#pragma once
#include "MovementActions.h"

namespace ai
{
    // [副本推进 2026-10-18 站长口径] 运行期按波推进（只对 verified=1 的图生效）：
    //   驱动坦克先走到本波的"拉怪位"（见 DungeonRoute.h），到位后由 DungeonRoutePullAction 起手。
    class DungeonRouteMoveAction : public MovementAction
    {
    public:
        DungeonRouteMoveAction(PlayerbotAI* ai, std::string name = "dungeon route move") : MovementAction(ai, name) {}
        bool Execute(Event& event) override;
        bool isPossible() override;

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "dungeon route move"; }
        virtual std::string GetHelpDescription()
        {
            return "Moves the driving tank to the pull position of the current wave in a verified dungeon route.";
        }
        virtual std::vector<std::string> GetUsedActions() { return {}; }
        virtual std::vector<std::string> GetUsedValues() { return {}; }
#endif
    };

    //   站在拉怪位后，直接复用现有 pull 链路（"pull my target"：负责 posMap/OnCombatStarted）
    //   对"当前波最近的怪 / boss 阶段最近的 boss"起手。
    class DungeonRoutePullAction : public Action
    {
    public:
        DungeonRoutePullAction(PlayerbotAI* ai, std::string name = "dungeon route pull") : Action(ai, name) {}
        bool Execute(Event& event) override;
        bool isPossible() override;

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "dungeon route pull"; }
        virtual std::string GetHelpDescription()
        {
            return "Pulls the current wave (or boss) target of a verified dungeon route from the wave pull position.";
        }
        virtual std::vector<std::string> GetUsedActions() { return {"pull my target"}; }
        virtual std::vector<std::string> GetUsedValues() { return {}; }
#endif
    };
}
