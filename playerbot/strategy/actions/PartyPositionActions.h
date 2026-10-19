#pragma once
#include "MovementActions.h"

// [跑位 2026-10-18 站长口径] 执行"队伍统一决策层"下发的占位槽。
//   - 决策层（PartyPositionCoordinator）每队只有一个 bot 在算，结果按 group 共享；
//   - 本动作只读自己那一格，走到位后停住（不自己判断该站哪）；
//   - 优先级由挂它的触发器/策略给（低于紧急躲圈、高于默认追击）。
namespace ai
{
    class MoveToPartySlotAction : public MovementAction
    {
    public:
        MoveToPartySlotAction(PlayerbotAI* ai) : MovementAction(ai, "move to party slot") {}

        bool Execute(Event& event) override;
        bool isPossible() override;

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "move to party slot"; }
        virtual std::string GetHelpDescription()
        {
            return "Move to the position slot assigned by the party positioning coordinator.";
        }
        virtual std::vector<std::string> GetUsedActions() { return {}; }
        virtual std::vector<std::string> GetUsedValues() { return {}; }
#endif
    };
}
