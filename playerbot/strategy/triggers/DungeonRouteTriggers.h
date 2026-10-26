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

    // [2026-10-24 站长口径] "漏怪"触发：战斗中有怪选择了队友，而自己没有目标（坦克丢怪 / DPS 空转）。
    // [2026-10-24 站长口径] 坦克 OT 触发：有怪的"仇恨第一"不是坦克（按仇恨表，不看谁在挨打）
    class DungeonTankOtTrigger : public Trigger
    {
    public:
        DungeonTankOtTrigger(PlayerbotAI* ai) : Trigger(ai, "dungeon tank ot", 1) {}
        bool IsActive() override;
    };

    // [2026-10-24 站长口径] 群拉触发：坦克在战斗中、没有 OT、且"战斗中未被控制的怪" >= 3
    // [2026-10-24 站长口径] 站位调整：战斗中坦克当前位置会踩到"未进战斗怪"的仇恨圈
    class DungeonTankRepositionTrigger : public Trigger
    {
    public:
        DungeonTankRepositionTrigger(PlayerbotAI* ai) : Trigger(ai, "dungeon tank reposition", 1) {}
        bool IsActive() override;
    };

    class DungeonTankAoeTrigger : public Trigger
    {
    public:
        DungeonTankAoeTrigger(PlayerbotAI* ai) : Trigger(ai, "dungeon tank aoe", 1) {}
        bool IsActive() override;
    };

    class DungeonLooseMobTrigger : public Trigger
    {
    public:
        DungeonLooseMobTrigger(PlayerbotAI* ai) : Trigger(ai, "dungeon loose mob", 1) {}
        bool IsActive() override;
    };

    class DungeonRoutePullTrigger : public Trigger
    {
    public:
        DungeonRoutePullTrigger(PlayerbotAI* ai) : Trigger(ai, "dungeon route pull", 1) {}
        bool IsActive() override;
    };

    // [2026-10-25 站长口径] **坦克是队伍核心**：任意队友正被敌对怪攻击 ⇒ 坦克立刻去救场，
    //   不管它当前有没有目标、正在打谁、正在走哪条路线 —— 全部让位（优先级最高）。
    class DungeonTankEngageTrigger : public Trigger
    {
    public:
        DungeonTankEngageTrigger(PlayerbotAI* ai) : Trigger(ai, "dungeon tank engage", 60) {}
        bool IsActive() override;
    };

    // [2026-10-25 站长口径] **怪必须聚到一起才能开始打**：DPS/法系在任一条件下停手 ——
    //   ① 坦克对该目标仇恨 < lead（含 tankThreat == 0）；
    //   ② 本次战斗里还有敌对活怪离坦克 > 5 码（没聚齐）。
    //   治疗不受影响。
    class DungeonDpsHoldFireTrigger : public Trigger
    {
    public:
        DungeonDpsHoldFireTrigger(PlayerbotAI* ai) : Trigger(ai, "dungeon dps hold fire", 55) {}
        bool IsActive() override;
    };

    // [2026-10-25 站长口径] **控制模式**：有 CC 的职业去控 1 只高威胁怪（每人一只、不重复），
    //   坦克不参与控制（坦克负责把怪拉离被控目标）。
    class DungeonCcCastTrigger : public Trigger
    {
    public:
        DungeonCcCastTrigger(PlayerbotAI* ai) : Trigger(ai, "dungeon cc cast", 50) {}
        bool IsActive() override;
    };
}
