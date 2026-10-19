
#include "playerbot/playerbot.h"
#include "playerbot/DungeonRoute.h"
#include "DungeonRouteStrategy.h"

using namespace ai;

void DungeonRouteStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{
    // 驱动坦克专用：站在本波拉怪位上 ⇒ 起手拉（优先于普通移动/开怪）
    triggers.push_back(new TriggerNode(
        "dungeon route pull",
        NextAction::array(0, new NextAction("dungeon route pull", 85.0f), NULL)));

    // 驱动坦克专用：朝本波拉怪位走（普通 "attack anything" 是 5.0，这里要压住它）
    triggers.push_back(new TriggerNode(
        "dungeon route move",
        NextAction::array(0, new NextAction("dungeon route move", 75.0f), NULL)));
}
