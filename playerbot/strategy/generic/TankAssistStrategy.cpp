
#include "playerbot/playerbot.h"
#include "TankAssistStrategy.h"

using namespace ai;

void TankAssistStrategy::InitCombatTriggers(std::list<TriggerNode*> &triggers)
{
    triggers.push_back(new TriggerNode(
        "tank assist",
        NextAction::array(0, new NextAction("tank assist", 60.0f), NULL)));

    // [pull 2026-10-18 站长口径] 补拉：有怪在打队友、我却没仇恨 ⇒ 立刻去接手
    //   （走过去 + 嘲讽，直到怪的目标是我；判定见 PullTriggers.cpp → LooseMobOnPartyTrigger）。
    triggers.push_back(new TriggerNode(
        "loose mob on party",
        NextAction::array(0, new NextAction("grab loose mob", ACTION_EMERGENCY), NULL)));
}
