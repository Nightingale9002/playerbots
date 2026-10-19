#pragma once
#include "playerbot/strategy/Trigger.h"

namespace ai
{
    // [副本推进 2026-10-18 站长口径] 运行期按波推进的两个触发器（都只对 verified=1 的图 +
    //   驱动坦克生效，判据见 DungeonRoute.h）：
    //     dungeon route move : 驱动坦克不在本波拉怪位上 ⇒ 走过去
    //     dungeon route pull : 已经站在拉怪位 ⇒ 对当前波/boss 起手拉
    class DungeonRouteMoveTrigger : public Trigger
    {
    public:
        DungeonRouteMoveTrigger(PlayerbotAI* ai) : Trigger(ai, "dungeon route move", 1) {}
        bool IsActive() override;
    };

    class DungeonRoutePullTrigger : public Trigger
    {
    public:
        DungeonRoutePullTrigger(PlayerbotAI* ai) : Trigger(ai, "dungeon route pull", 1) {}
        bool IsActive() override;
    };
}
