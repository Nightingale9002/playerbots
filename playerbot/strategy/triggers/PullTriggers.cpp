
#include "playerbot/playerbot.h"
#include "playerbot/strategy/Action.h"
#include "playerbot/strategy/generic/PullStrategy.h"
#include "PullTriggers.h"
#include "playerbot/strategy/values/PositionValue.h"
#include "playerbot/strategy/actions/PullActions.h"
#include "playerbot/strategy/actions/ChooseTargetActions.h"

using namespace ai;

bool PullStartTrigger::IsActive()
{
    const PullStrategy* strategy = PullStrategy::Get(ai);
    return strategy && strategy->IsPullPendingToStart();
}

bool PullEndTrigger::IsActive()
{
    const PullStrategy* strategy = PullStrategy::Get(ai);
    if (strategy)
    {
        // [pullback 2026-10-18 站长口径] **拉怪迟迟没真正开始 ⇒ 快速放弃**。
        //   站长实测："我标了骷髅头的怪没进战斗就会卡"——一旦 `RequestPull()` 被调用，
        //   `PullMultiplier` 会把所有非拉怪动作压成 0；可如果那只怪根本拉不到（不可选/够不着/被挡），
        //   bot 会傻站到 `PullMaxTime`（15 秒）才恢复。这里加一道短超时（`AiPlayerbot.PullNeverStartedAbortSeconds`）。
        //   [pull 2026-10-18 修正] **只在"已经走到拉怪射程内"时才允许放弃**：原来不看距离就放弃，
        //   坦克还在往怪那边走（正常拉怪流程）就被判"没开始"⇒ 目标被清掉 ⇒ 表现成"坦克完全不主动拉怪"。
        if (strategy->IsPullPendingToStart() && strategy->HasTarget())
        {
            Unit* pullTarget = strategy->GetTarget();
            float const pullRange = strategy->GetRange() + 2.0f;
            bool const inRange = pullTarget && ai->GetBot()->GetDistance(pullTarget) <= pullRange;

            const time_t requestTime = strategy->GetPullStartTime();
            if (inRange && requestTime > 0 &&
                (time(0) - requestTime) >= (time_t)sPlayerbotAIConfig.pullNeverStartedAbortSeconds)
            {
                sLog.outDetail("[PULL] %s: abort pull that never started (target=%s, in range)",
                    ai->GetBot()->GetName(), pullTarget->GetName());
                return true;
            }
        }
    }

    if (strategy && strategy->HasPullStarted())
    {
        Unit* target = strategy->GetTarget();
        if (target)
        {
            // Check if the pull is taking too long
            const time_t secondsSincePullStarted = time(0) - strategy->GetPullStartTime();
            if (secondsSincePullStarted >= strategy->GetMaxPullTime())
            {
                return true;
            }
            else
            {
                float distanceToPullTarget = target->GetDistance(ai->GetBot());
                // sometimes creatures can reach slightly more than normal attack distance
                float creatureMeleeRange = ATTACK_DISTANCE + BASE_MELEERANGE_OFFSET + 1;

                if (distanceToPullTarget <= creatureMeleeRange || (target->IsNonMeleeSpellCasted(true) && target->IsInCombat()) || (secondsSincePullStarted >= 10 && target->GetTarget() != bot) || (ai->IsRanged(bot) && distanceToPullTarget <= ai->GetRange("spell")))
                {
                    if (ai->HasStrategy("pull back", BotState::BOT_STATE_COMBAT))
                    {
                        PositionMap& posMap = AI_VALUE(PositionMap&, "position");
                        PositionEntry pullPosition = posMap["pull"];
                        if (pullPosition.isSet())
                        {
                            distanceToPullTarget = bot->GetDistance(pullPosition.x, pullPosition.y, pullPosition.z);
                            return distanceToPullTarget <= ai->GetRange("follow");
                        }
                    }

                    // Check if the pulled target has approached the bot
                    return true;
                }
            }
        }
    }

    return false;
}

bool PullPackHasRangedTrigger::IsActive()
{
    Player* bot = ai->GetBot();
    if (!bot || !bot->GetMap() || (!bot->GetMap()->IsDungeon() && !bot->GetMap()->IsRaid()))
        return false;

    if (!PlayerbotAI::IsTank(bot))
        return false;

    PullStrategy* strategy = PullStrategy::Get(ai);
    if (!strategy || !strategy->HasPullStarted())
        return false;

    PositionEntry pullPosition = AI_VALUE(PositionMap&, "position")["pull"];
    if (!pullPosition.isSet())
        return false;

    Unit* target = strategy->GetTarget();
    if (!target || !target->IsCreature())
        return false;

    // [corner pull 2026-10-18 站长口径] **必须真的拉到了才卡视野**。
    //   站长实测："还没把怪拉进战斗就开始卡了"。原因是这里只判 `HasPullStarted()`（= 拉怪动作刚发起），
    //   于是拉怪技能还没落地/怪还没动，坦克就往墙角跑了，怪根本不跟。
    //   现在的判据：目标**已进战斗**、且**正追着我**（victim == 我）⇒ 已经拉到了，这时才退到卡视野点。
    if (!target->IsInCombat())
        return false;

    Unit* victim = target->GetVictim();
    if (!victim || victim->GetObjectGuid() != bot->GetObjectGuid())
        return false;

    // [pullback 2026-10-18 站长口径] **孤立的单只怪不 pullback**：周围没别的怪就原地打。
    if (PullPackIsIsolated(ai, (Creature*)target))
        return false;

    return PullPackHasRanged((Creature*)target, bot);
}

// [pull 2026-10-18 站长口径] 坦克补拉：找"正在打队友、而我没有仇恨"的怪。
bool LooseMobOnPartyTrigger::IsActive()
{
    Player* bot = ai->GetBot();
    if (!bot || !bot->IsInCombat())
        return false;

    if (!PlayerbotAI::IsTank(bot))
        return false;

    Group* group = bot->GetGroup();
    if (!group)
        return false;

    std::list<ObjectGuid> targets = AI_VALUE(std::list<ObjectGuid>, "possible attack targets");
    for (std::list<ObjectGuid>::iterator i = targets.begin(); i != targets.end(); ++i)
    {
        Unit* unit = ai->GetUnit(*i);
        if (!unit || !unit->IsAlive() || unit->IsPlayer() || !unit->IsInCombat())
            continue;

        Unit* victim = unit->GetVictim();
        if (!victim || !victim->IsPlayer())
            continue;

        Player* victimPlayer = (Player*)victim;
        if (victimPlayer == bot || victimPlayer->GetGroup() != group)
            continue;

        // 我对它没有仇恨 ⇒ 这只怪不归我管，要去接手
        if (sServerFacade.GetThreatManager(unit).getThreat(bot) <= 0.0f)
            return true;
    }

    return false;
}
bool EnemyRangedOutOfMeleeTrigger::IsActive(){
    Player* bot = ai->GetBot();
    if (!bot || !bot->IsInCombat())
        return false;

    // 坦克走自己的卡视野拉怪流程，这里只管其他人
    if (PlayerbotAI::IsTank(bot))
        return false;

    Unit* target = GetTarget();
    if (!target || !target->IsCreature() || !target->IsAlive() || !target->IsInCombat())
        return false;

    if (!CreatureHasRangedThreat((Creature*)target))
        return false;

    return sServerFacade.GetDistance2d(bot, target) > ATTACK_DISTANCE + 1.0f;
}

// [killorder 2026-10-18 站长口径] 名单里的优先击杀目标在场且当前目标不是它 ⇒ 切过去。
bool PriorityKillTargetAvailableTrigger::IsActive()
{
    Unit* priority = SwitchToPriorityKillTargetAction::FindPriorityKillTarget(ai);
    if (!priority)
        return false;

    Unit* current = ai->GetUnit(AI_VALUE(ObjectGuid, "current target"));
    return !current || current->GetObjectGuid() != priority->GetObjectGuid();
}

// [wave 2026-10-18 站长口径] 波次推进触发器：判据与动作共用 AdvanceWaveAction::NeedsAdvance()。
bool DungeonWaveAdvanceTrigger::IsActive()
{
    return AdvanceWaveAction::NeedsAdvance(ai);
}

// [boss-pos 2026-10-18 站长口径] 判据与动作共用 StandBossPositionAction::NeedsStand()。
bool BossPositionTrigger::IsActive()
{
    return StandBossPositionAction::NeedsStand(ai);
}