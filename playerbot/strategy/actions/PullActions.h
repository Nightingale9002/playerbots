#pragma once
#include "GenericSpellActions.h"
#include "GenericActions.h"
#include "MovementActions.h"
#include "playerbot/WorldPosition.h"

namespace bossposition { struct Spot; }

namespace ai
{
    // [corner pull 2026-10-18 站长口径] 卡视野拉怪用：收集"这一波"怪（call-for-help 协助半径 ∪ 联动组），
    // 并判断其中是否有远程（弓箭手类 / 法师系）。定义见 PullActions.cpp。
    void GetPullPackCreatures(Creature* target, Player* bot, std::vector<Creature*>& out);
    bool PullPackHasRanged(Creature* target, Player* bot);

    // [patrol 2026-10-18 站长口径 / v2] 这个目标能不能拉：
    //   · 目标本身是巡逻怪 ⇒ 只有"孤立"（周围没有别的可攻击怪）时才**主动**拉；
    //   · 目标是静态怪 ⇒ 联动组里捎带巡逻怪**不再是拒绝理由**（v2：顺手清掉，只记日志）。
    //   非副本/团本、或该副本没有巡逻数据 ⇒ 一律返回 true（行为同改动前）。定义见 PullActions.cpp。
    bool PullTargetIsSafe(PlayerbotAI* ai, Creature* target);

    class PullRequestAction : public ChatCommandAction
    {
    public:
        PullRequestAction(PlayerbotAI* ai, std::string name) : ChatCommandAction(ai, name) {}

    protected:
        virtual bool Execute(Event& event) override;
        virtual Unit* GetTarget(Event& event) = 0;
    };

    class PullMyTargetAction : public PullRequestAction
    {
    public:
        PullMyTargetAction(PlayerbotAI* ai) : PullRequestAction(ai, "pull my target") {}
    
    private:
        Unit* GetTarget(Event& event) override;
    };

    class PullRTITargetAction : public PullRequestAction
    {
    public:
        PullRTITargetAction(PlayerbotAI* ai) : PullRequestAction(ai, "pull rti target") {}

    private:
        Unit* GetTarget(Event& event) override;
    };

    class PullStartAction : public Action
    {
    public:
        PullStartAction(PlayerbotAI* ai, std::string name = "pull start") : Action(ai, name) {}
        bool Execute(Event& event) override;
    };

    class PullAction : public CastSpellAction
    {
    public:
        PullAction(PlayerbotAI* ai, std::string name = "pull action");
        bool Execute(Event& event) override;
        bool isPossible() override;
    private:
        void InitPullAction();
        std::string GetTargetName() override { return "pull target"; }
        std::string GetReachActionName() override { return "reach pull"; }
    };

    class PullEndAction : public Action
    {
    public:
        PullEndAction(PlayerbotAI* ai, std::string name = "pull end") : Action(ai, name) {}
        bool Execute(Event& event) override;
    };

    // [corner pull 2026-10-18 站长口径] 单体远程判定（弓箭手/法师系），供"全员卡视野"复用。
    bool CreatureHasRangedThreat(Creature* creature);

    // [pullback 2026-10-18 站长口径] "这一波"有多少只（call-for-help 半径 ∪ 联动组，含目标本身）。
    uint32 PullPackCreatureCount(Creature* target, Player* bot);

    // [pullback 2026-10-18 站长口径] **是否孤立——只看距离，不看数量**：
    //   周围 `AiPlayerbot.PullIsolationRadius` 内没有"这一波"以外的怪，且不在巡逻路线附近 ⇒ 孤立 ⇒ 不 pullback。
    bool PullPackIsIsolated(PlayerbotAI* ai, Creature* target);

    // [corner pull 2026-10-18 站长口径] 卡视野拉怪：拉完退回到"能对怪堆断视线"的墙后点（在拉怪原点附近找），
    // 逼远程怪走进近战范围聚堆。触发点见 PullBackStrategy::InitCombatTriggers（"pull pack has ranged"）。
    class CornerPullPositionAction : public MovementAction
    {
    public:
        CornerPullPositionAction(PlayerbotAI* ai) : MovementAction(ai, "corner pull position") {}
        bool Execute(Event& event) override;

        // [corner pull 2026-10-18 站长口径] 供"全员卡视野"复用：在 basePos 周围找一个
        // ① 能走到 ② 离巡逻路线够远 ③ 战斗点安全 ④ 至少对 pack 里一只断视线 的点，取"离自己最近"的。
        static WorldPosition FindCornerPoint(PlayerbotAI* ai, const WorldPosition& basePos, const std::vector<Creature*>& pack);
    };

    // [corner pull 2026-10-18 站长口径] **非坦克也要卡视野**：远处有怪在打我们时，跟到坦克的卡视野点后面，
    // 把远程怪逼进近战（否则远程怪原地放箭，队伍一直挨打）。
    class UseCoverAction : public MovementAction
    {
    public:
        UseCoverAction(PlayerbotAI* ai) : MovementAction(ai, "use cover") {}
        bool Execute(Event& event) override;
    };

    // [pull 2026-10-18 站长口径] 坦克补拉：走过去 + 嘲讽（职业没有嘲讽就用各自的开怪技能），
    // 直到"怪的目标是我、且我对它有仇恨"为止。触发点见 TankAssistStrategy（"loose mob on party"）。
    class GrabLooseMobAction : public MovementAction
    {
    public:
        GrabLooseMobAction(PlayerbotAI* ai) : MovementAction(ai, "grab loose mob") {}
        bool Execute(Event& event) override;

    private:
        Unit* FindLooseMob();
    };

    // [wave 2026-10-18 站长口径] **波次推进**（简化版状态机里的 Advance）：
    //   当前波次（`ai_playerbot_dungeon_route.ord`）离我 > D_safe ⇒ 走到"波次质心外 D_safe"处停下接怪；
    //   已经 <= D_safe ⇒ 返回 false，交给正常战斗循环（= Hold）。
    //   躲地板（Retreat）由 `AoeDodgePriorityMultiplier` 用更高优先级压过它，卡视野（LosWait）走 corner pull。
    class AdvanceWaveAction : public MovementAction
    {
    public:
        AdvanceWaveAction(PlayerbotAI* ai) : MovementAction(ai, "advance wave") {}
        bool Execute(Event& event) override;

        // 供触发器做便宜判定（同一套判据）
        static bool NeedsAdvance(PlayerbotAI* ai);
        static uint16 CurrentWaveOrder(PlayerbotAI* ai);
    };

    // [boss-pos 2026-10-18 站长口径] **BOSS 战按角色站位**：每个 BOSS 单独一套数据，
    //   存 `ai_playerbot_boss_position(entry, map, bot_role, x, y, z, radius)`，
    //   游戏内 `.bot bosspos ...` 录入（见 PlayerbotHolder::HandleBosspos），写库立即生效。
    //   命中登记过的 BOSS 后：tank/healer/melee/ranged 各站自己的点；`radius` 同时是
    //   "不许走出 BOSS 战范围"的护栏。没登记的 BOSS ⇒ 完全不干预（保持默认行为）。
    class StandBossPositionAction : public MovementAction
    {
    public:
        StandBossPositionAction(PlayerbotAI* ai) : MovementAction(ai, "boss position") {}
        bool Execute(Event& event) override;

        // 供触发器做便宜判定（同一套判据）
        static bool NeedsStand(PlayerbotAI* ai);

    private:
        static bool FindSpot(PlayerbotAI* ai, uint32& entry, bossposition::Spot& spot);
    };
}