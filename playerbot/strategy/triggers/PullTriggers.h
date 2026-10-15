#pragma once
#include "playerbot/strategy/Trigger.h"

namespace ai
{
	class PullStartTrigger : public Trigger
	{
	public:
		PullStartTrigger(PlayerbotAI* ai, std::string name = "pull start") : Trigger(ai, name) {}
		bool IsActive() override;
	};

    class PullEndTrigger : public Trigger
    {
    public:
        PullEndTrigger(PlayerbotAI* ai, std::string name = "pull end") : Trigger(ai, name) {}
        bool IsActive() override;
    };

    // [corner pull 2026-10-18 站长口径] 坦克拉怪时"这一波"里含远程怪（仅副本/团本）⇒ 改走卡视野拉怪。
    class PullPackHasRangedTrigger : public Trigger
    {
    public:
        PullPackHasRangedTrigger(PlayerbotAI* ai) : Trigger(ai, "pull pack has ranged", 2) {}
        bool IsActive() override;
    };

    // [corner pull 2026-10-18 站长口径] **非坦克也要卡视野**：当前目标是"远程怪"且离我 > 近战距离
    //   （它在远处放箭/放法术、我们够不到）⇒ 跟到坦克的卡视野点后面，逼它走进近战。
    //   坦克本身不走这条（坦克走 PullBackStrategy 的卡视野拉怪流程）。
    class EnemyRangedOutOfMeleeTrigger : public Trigger
    {
    public:
        EnemyRangedOutOfMeleeTrigger(PlayerbotAI* ai) : Trigger(ai, "enemy ranged out of melee", 2) {}
        virtual std::string GetTargetName() override { return "current target"; }
        bool IsActive() override;
    };

    // [pull 2026-10-18 站长口径] 坦克补拉：有怪正在打**队友**（不是我），而我对它还没有仇恨 ⇒ 该去接手。
    //   站长口径："嘲讽并且过去拉，直到怪物的目标是 T，而且仇恨稳定"。
    class LooseMobOnPartyTrigger : public Trigger
    {
    public:
        LooseMobOnPartyTrigger(PlayerbotAI* ai) : Trigger(ai, "loose mob on party", 2) {}
        bool IsActive() override;
    };

    // [killorder 2026-10-18 站长口径] 优先击杀名单（`AiPlayerbot.PriorityKillTargets`）里的怪在场，
    //   而当前目标不是它 ⇒ 切过去打（破碎大厅：碎手军团士兵 16700 / 20589）。
    class PriorityKillTargetAvailableTrigger : public Trigger
    {
    public:
        PriorityKillTargetAvailableTrigger(PlayerbotAI* ai) : Trigger(ai, "priority kill target available", 1) {}
        bool IsActive() override;
    };

    // [wave 2026-10-18 站长口径] 波次推进（Advanced）：当前波还离得远、队伍血量够 ⇒ 坦克往前走。
    class DungeonWaveAdvanceTrigger : public Trigger
    {
    public:
        DungeonWaveAdvanceTrigger(PlayerbotAI* ai) : Trigger(ai, "dungeon wave advance", 2) {}
        bool IsActive() override;
    };

    // [boss-pos 2026-10-18 站长口径] 正在打的 BOSS 登记了角色站位、而我跑出了它给的战场半径 ⇒ 回去
    //   （数据来源 `ai_playerbot_boss_position`，游戏内 `.bot bosspos` 录入）。
    class BossPositionTrigger : public Trigger
    {
    public:
        BossPositionTrigger(PlayerbotAI* ai) : Trigger(ai, "boss position", 2) {}
        bool IsActive() override;
    };
}