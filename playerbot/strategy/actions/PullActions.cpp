
#include "playerbot/playerbot.h"
#include "playerbot/strategy/generic/PullStrategy.h"
#include "playerbot/strategy/values/AttackersValue.h"
#include "playerbot/strategy/values/PossibleAttackTargetsValue.h"
#include "PullActions.h"
#include "playerbot/BossPositions.h"
#include "playerbot/strategy/values/PositionValue.h"
#include "playerbot/DungeonPatrol.h"
#include "playerbot/DungeonRoute.h"
#include "playerbot/ServerFacade.h"
#include "Grids/GridNotifiers.h"
#include "Grids/GridNotifiersImpl.h"
#include "Grids/CellImpl.h"

using namespace ai;

namespace ai
{
    // [corner pull 2026-10-18 站长口径] 远程判定射程阈值：>近战即算远程。
    static const float PULL_RANGED_SPELL_MIN_RANGE = 10.0f;

    // [patrol 2026-10-18 站长口径] 巡逻怪"孤立"判定半径（同营地尺度）与战斗点安全余量。
    static const float PULL_PATROL_ISOLATION_RADIUS = 20.0f;
    static const float PULL_PATROL_SAFE_MARGIN = 5.0f;


    // [pullback 2026-10-18 站长口径] 孤立判定：**只看距离**（周围没别的怪 + 不靠巡逻路线），不看这一波几只。
    bool PullPackIsIsolated(PlayerbotAI* ai, Creature* target)
    {
        if (!ai || !target)
            return false;

        Player* bot = ai->GetBot();
        if (!bot)
            return false;

        float const radius = sPlayerbotAIConfig.pullIsolationRadius;
        if (radius <= 0.0f)
            return false;

        // ① 巡逻路线：卡视野/拉怪点不能贴着巡逻路线（复用既有判定）
        if (dungeonpatrol::RouteNear(ai, WorldPosition(target), radius))
            return false;

        // ② 这一波以外的怪：用服务器的联动半径组出"这一波"，再扫附近敌人排除它们
        std::vector<Creature*> pack;
        GetPullPackCreatures(target, bot, pack);

        std::list<Unit*> nearby;
        MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck check(bot, radius);
        MaNGOS::UnitListSearcher<MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck> searcher(nearby, check);
        Cell::VisitAllObjects(bot, searcher, radius);

        for (Unit* unit : nearby)
        {
            if (!unit || !unit->IsAlive() || !unit->IsCreature())
                continue;

            Creature* creature = (Creature*)unit;
            if (std::find(pack.begin(), pack.end(), creature) != pack.end())
                continue;

            // 只算"会主动打我们"的怪：小动物/中立不参与
            if (!creature->CanAttackOnSight(bot))
                continue;

            if (bot->GetDistance(creature) > radius)
                continue;

            return false;
        }

        return true;
    }
    // [pullback 2026-10-18 站长口径] "这一波"多少只（含目标本身）。
    uint32 PullPackCreatureCount(Creature* target, Player* bot)
    {
        if (!target || !bot)
            return 0;

        std::vector<Creature*> pack;
        GetPullPackCreatures(target, bot, pack);
        return (uint32)pack.size();
    }
    // [corner pull 2026-10-18 站长口径] 单个怪是否"远程"（弓箭手类 / 法师系）。
    //   注意：只查 ChanceRangedAttack/ranged 标志会漏判弓箭手（如血环弓箭手 17270：ChanceRangedAttack=0、
    //   法术无 ranged 标志，但 Aimed Shot/Multi Shot 射程 ~35y），所以还要看法术射程。
    bool CreatureHasRangedThreat(Creature* creature)
    {
        if (!creature)
            return false;

        // EventAI / 脚本显式设过远程模式的
        if (creature->AI() && creature->AI()->IsRangedUnit())
            return true;

        CreatureSpellList const& spellList = creature->GetSpellList();
        if (spellList.Disabled)
            return false;

        // 施法型：进远程模式的概率 > 0
        if (spellList.ChanceRangedAttack > 0)
            return true;

        for (auto const& entry : spellList.Spells)
        {
            auto const& spell = entry.second;
            if (spell.DisabledForAI)
                continue;

            if (spell.Flags & SPELL_LIST_FLAG_RANGED_ACTION)
                return true;

            SpellEntry const* spellInfo = sSpellTemplate.LookupEntry<SpellEntry>(spell.SpellId);
            if (!spellInfo)
                continue;

            SpellRangeEntry const* spellRange = sServerFacade.LookupSpellRangeEntry(spellInfo->rangeIndex);
            if (spellRange && spellRange->maxRange > PULL_RANGED_SPELL_MIN_RANGE)
                return true;
        }

        return false;
    }

    void GetPullPackCreatures(Creature* target, Player* bot, std::vector<Creature*>& out)
    {        out.clear();
        if (!target || !bot)
            return;

        // call-for-help 半径：与 Creature::MarkCallAssistanceOnPull 同款取值（此处只读复刻，不触发其副作用）
        float assistRadius = sWorld.getConfig(CONFIG_FLOAT_CREATURE_FAMILY_ASSISTANCE_RADIUS);
        if (target->GetCreatureInfo() && target->GetCreatureInfo()->CallForHelp > 0)
            assistRadius = float(target->GetCreatureInfo()->CallForHelp);

        // 扫描半径取"营地"尺度（核心侧 CreatureFamilyFleeAssistanceRadius，默认 30y）与协助半径的较大者，
        // 只为能把"联动组"成员扫进来；accept 条件仍是严格的协助/同组，不做 proximity，
        // 免得把"根本不会跟着过来"的邻居远程也算进这一波。
        const float campRadius = sWorld.getConfig(CONFIG_FLOAT_CREATURE_FAMILY_FLEE_ASSISTANCE_RADIUS);
        const float scanRadius = std::max(assistRadius, campRadius);

        CreatureGroup* group = target->GetCreatureGroup();

        std::list<Unit*> nearby;
        MaNGOS::AnyUnitInObjectRangeCheck u_check(target, scanRadius);
        MaNGOS::UnitListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(nearby, u_check);
        Cell::VisitAllObjects(target, searcher, scanRadius);

        for (Unit* unit : nearby)
        {
            if (!unit || unit == target || !unit->IsCreature())
                continue;

            Creature* creature = (Creature*)unit;
            if (!creature->IsAlive() || creature->GetMaster())
                continue;

            const bool inAssistRadius = target->GetDistance(creature) <= assistRadius && creature->CanAssistInCombatAgainst(target, bot);
            const bool inSameGroup = group && creature->GetCreatureGroup() == group;

            if (inAssistRadius || inSameGroup)
                out.push_back(creature);
        }
    }

    bool PullPackHasRanged(Creature* target, Player* bot)
    {
        if (!target)
            return false;

        if (CreatureHasRangedThreat(target))
            return true;

        std::vector<Creature*> pack;
        GetPullPackCreatures(target, bot, pack);
        for (Creature* creature : pack)
        {
            if (CreatureHasRangedThreat(creature))
                return true;
        }

        return false;
    }

    // [patrol 2026-10-18 站长口径 → 2026-10-18 v2] 见 PullActions.h。
    //
    // v2 口径：**类型B 巡逻怪不躲，战斗中看情况清掉**。
    //   旧实现是"这一拉的联动组会捎带巡逻怪 ⇒ 这个目标不拉"，结果是坦克在巡逻路线密集的
    //   位置**一个怪都不拉**（被自己的保护规则锁死），而巡逻怪本来就会走进来。
    //   现在只保留一条：**主动开巡逻怪**（把它当目标拉）仍要求它孤立 —— 它会自己走开，
    //   主动开容易白跑一趟；被顺带拖进来的一律就地清掉，由战斗流程处理。
    //   另外 `AdvanceWaveAction::NeedsAdvance()` 加了"战斗中不推进"，保证眼前这只清完再走。
    bool PullTargetIsSafe(PlayerbotAI* ai, Creature* target)
    {
        if (!ai || !target)
            return true;

        Player* bot = ai->GetBot();
        if (!bot)
            return true;

        // 目标本身是巡逻怪：只有孤立才**主动**拉（它自己会走，不主动开）
        if (dungeonpatrol::IsPatrol(ai, target))
        {
            if (!dungeonpatrol::IsIsolated(ai, target, PULL_PATROL_ISOLATION_RADIUS))
            {
                sLog.outDetail("[Playerbot][patrol] %s 拒绝主动拉巡逻怪 %s (entry %u): 不孤立",
                    bot->GetName(), target->GetName(), target->GetEntry());
                return false;
            }
            return true;
        }

        // 目标静态怪：联动组里捎带巡逻怪 —— **允许**，顺手清掉，只记日志
        std::vector<Creature*> pack;
        GetPullPackCreatures(target, bot, pack);
        uint32 swept = 0;
        for (Creature* creature : pack)
        {
            if (creature && creature != target && dungeonpatrol::IsPatrol(ai, creature))
                ++swept;
        }
        if (swept)
        {
            sLog.outDetail("[Playerbot][patrol] %s 拉 %s (entry %u)：顺手清掉 %u 只巡逻怪",
                bot->GetName(), target->GetName(), target->GetEntry(), swept);
        }

        return true;
    }
}

bool PullRequestAction::Execute(Event& event)
{
    PullStrategy* strategy = PullStrategy::Get(ai);
    if (!strategy)
    {
        return false;
    }

    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();

    Unit* target = GetTarget(event);
    if (!target)
    {
        ai->TellPlayerNoFacing(requester,BOT_TEXT("You have no target"));
        return false;
    }

    const float maxPullDistance = sPlayerbotAIConfig.reactDistance * 3;
    const float distanceToPullTarget = target->GetDistance(ai->GetBot());
    if (distanceToPullTarget > maxPullDistance)
    {
        ai->TellPlayerNoFacing(requester,BOT_TEXT("The target is too far away"));
        return false;
    }

    if (!AttackersValue::IsValid(target, bot, nullptr, false))
    {
        ai->TellPlayerNoFacing(requester,BOT_TEXT("The target can't be pulled"));
        return false;
    }

    // [pull 2026-10-18 站长口径] 目标不在同一层 / 走不到 ⇒ 不接这个拉怪请求（否则坦克会一路直线冲过去）。
    if (!PossibleAttackTargetsValue::CanReachTarget(target, bot))
    {
        sLog.outDetail("[Playerbot][pull] %s 拒绝拉怪目标 %s (entry %u): 不同层或不可达",
            bot->GetName(), target->GetName(), target->GetEntry());
        ai->TellPlayerNoFacing(requester, BOT_TEXT("The target can't be pulled"));
        return false;
    }

    // [patrol 2026-10-18 站长口径] 巡逻怪须孤立；拉静态怪不得捎带巡逻怪（见 PullTargetIsSafe）。
    if (target->IsCreature() && !PullTargetIsSafe(ai, (Creature*)target))
    {
        ai->TellPlayerNoFacing(requester, BOT_TEXT("The target can't be pulled"));
        return false;
    }

    if (!strategy->CanDoPullAction(target))
    {
        std::ostringstream out; out << "Can't perform pull action '" << strategy->GetPullActionName() << "'";
        ai->TellPlayerNoFacing(requester, out.str());
        return false;
    }

    //Set position to return to after pulling.
    PositionMap& posMap = AI_VALUE(PositionMap&, "position");
    PositionEntry pullPosition = posMap["pull"];

    pullPosition.Set(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetMapId());
    posMap["pull"] = pullPosition;

    strategy->RequestPull(target);

    // Force change combat state to have a faster reaction time
    ai->OnCombatStarted();

    return true;
}

Unit* PullMyTargetAction::GetTarget(Event& event)
{
    Unit* target = nullptr;

    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    if (event.getSource() == "attack anything")
    {
        ObjectGuid guid = event.getObject();
        target = ai->GetCreature(guid);
    }
    else if (requester)
    {
        target = ai->GetUnit(requester->GetSelectionGuid());
    }

    // [pullback 2026-10-18 站长口径] **不许拿小动物/非战斗宠物当拉怪目标**。
    //   实测：坦克"选目标"把路边的兔子/小动物当成最近的可攻击目标，拉怪路线就跑去打小动物，
    //   真正的怪群反倒没拉到。这里直接拒掉（critter / non-combat pet）。
    if (target && target->IsCreature())
    {
        Creature* creature = (Creature*)target;
        uint32 const type = creature->GetCreatureType();
        if (type == CREATURE_TYPE_CRITTER || type == CREATURE_TYPE_NON_COMBAT_PET)
            return nullptr;

        // [pullback 2026-10-18 站长口径] conf 黑名单：AiPlayerbot.NoPullTargets
        //   （破碎大厅：只拉蠕动的软泥怪，不一只只去拉它分裂出来的小软泥怪）
        if (!sPlayerbotAIConfig.noPullTargetEntries.empty() &&
            sPlayerbotAIConfig.noPullTargetEntries.count(creature->GetEntry()))
            return nullptr;
    }

    return target;
}

Unit* PullRTITargetAction::GetTarget(Event& event)
{
    return ai->GetUnit(AI_VALUE(ObjectGuid, "rti target"));
}

bool PullStartAction::Execute(Event& event)
{
    bool result = false;
    PullStrategy* strategy = PullStrategy::Get(ai);
    if (strategy)
    {
        Unit* target = strategy->GetTarget();
        if (target)
        {
            if (strategy->GetPreActionName().empty())
                result = true;
            else
            {
                result = ai->DoSpecificAction(strategy->GetPreActionName(), event, true);
                if(result)
                    SetDuration(3000);
            }

            // Set the pet on passive mode during the pull
            Pet* pet = bot->GetPet();
            if (pet)
            {
                UnitAI* creatureAI = ((Creature*)pet)->AI();
                if (creatureAI)
                {
                    strategy->SetPetReactState(creatureAI->GetReactState());
                    creatureAI->SetReactState(REACT_PASSIVE);
                }
            }

            strategy->OnPullStarted();
        }
    }

    return result;
}


PullAction::PullAction(PlayerbotAI* ai, std::string name) : CastSpellAction(ai, name)
{
    InitPullAction();
}

bool PullAction::Execute(Event& event)
{
    InitPullAction();

    PullStrategy* strategy = PullStrategy::Get(ai);
    if (strategy)
    {
        Unit* target = strategy->GetTarget();
        if (target)
        {
            // Check if we are on pull range
            const float distanceToTarget = target->GetDistance(bot);
            if (distanceToTarget <= strategy->GetRange())
            {
                if (sServerFacade.isMoving(bot))
                {
                    // Force stop
                    ai->StopMoving();
                    strategy->RequestPull(target, false);
                    return false;
                }

                std::string actionName = strategy->GetPullActionName();

                // Execute the pull action
                SET_AI_VALUE(ObjectGuid, "current target", GetTarget()->GetObjectGuid());
                if (ai->DoSpecificAction(actionName, event, true))
                {
                    strategy->RequestPull(target); //extend pull timer to walk back.
                    return true;
                }
                else
                    return false;
            }
            else
            {
                // Retry the reach pull action
                strategy->RequestPull(target, false);
            }
        }
    }

    return false;
}

bool PullAction::isPossible()
{
    InitPullAction();

    PullStrategy* strategy = PullStrategy::Get(ai);
    if (strategy)
    {
        std::string spellName = strategy->GetSpellName();
        Unit* target = strategy->GetTarget();
        if (!spellName.empty() && target)
        {
            if (!ai->CanCastSpell(spellName, target, true, nullptr, true))
            {
                return false;
            }
        }
    }

    return true;
}

void PullAction::InitPullAction()
{
    // Get the pull action spell name from the strategy
    PullStrategy* strategy = PullStrategy::Get(ai);
    if (strategy)
    {
        std::string spellName = strategy->GetSpellName();
        if (!spellName.empty())
        {
            SetSpellName(spellName);

            float spellRange;
            if (ai->GetSpellRange(spellName, &spellRange))
            {
                range = spellRange;
            }
        }
    }
}

bool PullEndAction::Execute(Event& event)
{
    PullStrategy* strategy = PullStrategy::Get(ai);
    if (strategy)
    {
        // Restore the pet react state
        Pet* pet = bot->GetPet();
        if (pet)
        {
            UnitAI* creatureAI = ((Creature*)pet)->AI();
            if (creatureAI)
            {
                creatureAI->SetReactState(strategy->GetPetReactState());
                Unit* target = ai->GetUnit(AI_VALUE(ObjectGuid, "current target"));
                if (creatureAI->GetReactState() != REACT_PASSIVE && target)
                    creatureAI->AttackStart(target);
            }
        }

        // Remove the saved pull position
        AiObjectContext* context = ai->GetAiObjectContext();
        PositionMap& posMap = AI_VALUE(PositionMap&, "position");
        PositionEntry stayPosition = posMap["pull"];
        if (stayPosition.isSet())
        {
            posMap.erase("pull");
        }

        strategy->OnPullEnded();
        return true;
    }

    return false;
}

bool CornerPullPositionAction::Execute(Event& event)
{
    PullStrategy* strategy = PullStrategy::Get(ai);
    if (!strategy || !strategy->HasPullStarted())
        return false;

    Unit* target = strategy->GetTarget();
    if (!target || !target->IsCreature())
        return false;

    PositionMap& posMap = AI_VALUE(PositionMap&, "position");
    PositionEntry pullPosition = posMap["pull"];
    if (!pullPosition.isSet())
        return false;

    // 这一波怪（含目标本身）都纳入视线评估
    std::vector<Creature*> pack;
    pack.push_back((Creature*)target);
    std::vector<Creature*> others;
    GetPullPackCreatures((Creature*)target, bot, others);
    pack.insert(pack.end(), others.begin(), others.end());

    const WorldPosition pullPos = pullPosition.Get();
    const WorldPosition corner = FindCornerPoint(ai, pullPos, pack);
    if (!corner)
        return false;

    // [corner pull 2026-10-18 站长口径] **到了卡视野点就钉在这里，直到怪走进来**。
    //   以前到这里就返回 false ⇒ `return to pull position`（优先级只低 1）立刻接手，
    //   把坦克又拽回拉怪原点，等于"卡视野完全没效果"。现在到点就 StopMoving 并返回 true
    //   （本节点优先级高于 return-to-pull），真正的结束靠下面两道闸：
    //     ① `AiPlayerbot.PullCornerHoldSeconds`（默认 5 秒，0 = 不钉）—— 站长"pullback 太过了"的调节旋钮；
    //     ② `PullEndTrigger`（怪已进近战 / `AiPlayerbot.PullMaxTime` 超时）。
    if (WorldPosition(bot).fDist(corner) <= 3.0f)
    {
        const uint8 holdSeconds = sPlayerbotAIConfig.pullCornerHoldSeconds;
        const time_t pullStart = strategy->GetPullStartTime();
        if (holdSeconds == 0 || (pullStart > 0 && (time(0) - pullStart) >= (time_t)holdSeconds))
            return false;

        if (sServerFacade.isMoving(bot))
            ai->StopMoving();

        return true;
    }

    return MoveTo(corner.getMapId(), corner.getX(), corner.getY(), corner.getZ(), false, false, false, true);
}

WorldPosition CornerPullPositionAction::FindCornerPoint(PlayerbotAI* ai, const WorldPosition& basePos, const std::vector<Creature*>& pack)
{
    Player* bot = ai->GetBot();
    const WorldPosition botPosition(bot);
    const std::vector<CombatHostileUnit> hostiles = GetNearbyCombatHostiles(ai);

    const float radiansIncrement = (10.0f / 180.0f) * M_PI_F;
    static const float distances[] = { 3.0f, 5.0f, 7.0f, 10.0f, 13.0f };

    WorldPosition best;
    int bestBlocked = 0; // 至少对一个怪断视线才算"卡视野点"
    float bestMove = 0.0f;

    for (float distance : distances)
    {
        // [pullback 2026-10-18 站长口径] 卡视野点最远距离走 conf（默认 8 码；原来一路搜到 13 码，退太远）
        if (sPlayerbotAIConfig.pullCornerMaxDistance > 0.0f && distance > sPlayerbotAIConfig.pullCornerMaxDistance)
            continue;

        for (float angle = 0.0f; angle < 2 * M_PI_F; angle += radiansIncrement)
        {
            WorldPosition point = basePos + WorldPosition(0, distance * cos(angle), distance * sin(angle), 1.0f);
            point.setZ(point.getHeight());

            if (!botPosition.canPathTo(point, bot))
                continue;

            // [patrol 2026-10-18 站长口径] 卡视野点必须离巡逻路线足够远（Detection + 余量），
            //   否则巡逻怪会自己走进战斗（这一波本来只想拉目标那几只）。
            if (dungeonpatrol::RouteNear(ai, point, PULL_PATROL_SAFE_MARGIN))
                continue;

            if (sPlayerbotAIConfig.combatMoveSafetyEnabled &&
                !IsCombatMovePointSafe(ai, hostiles, point, sPlayerbotAIConfig.combatMoveSafetyMargin, nullptr))
                continue;

            int blocked = 0;
            for (Creature* creature : pack)
            {
                if (!creature || !creature->IsAlive())
                {
                    ++blocked;
                    continue;
                }

                const WorldPosition creaturePos(creature->GetMapId(), creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ() + creature->GetCollisionHeight());
                if (!point.IsInLineOfSight(creaturePos))
                    ++blocked;
            }

            if (blocked <= 0)
                continue;

            const float moveDist = botPosition.fDist(point);
            if (blocked > bestBlocked || (blocked == bestBlocked && (!best || moveDist < bestMove)))
            {
                best = point;
                bestBlocked = blocked;
                bestMove = moveDist;
            }
        }
    }

    return best;
}

// [corner pull 2026-10-18 站长口径] 非坦克卡视野：跟到坦克的卡视野点后面，把远程怪逼进近战。
//   判定在 `PullTriggers.cpp → EnemyRangedOutOfMeleeTrigger::IsActive()`（当前目标是远程怪且离我 > 近战距离）。
bool UseCoverAction::Execute(Event& event)
{
    Unit* target = ai->GetUnit(AI_VALUE(ObjectGuid, "current target"));
    if (!target || !target->IsCreature() || !target->IsAlive() || !target->IsInCombat())
        return false;

    Creature* creature = (Creature*)target;
    if (!CreatureHasRangedThreat(creature))
        return false;

    if (sServerFacade.GetDistance2d(bot, creature) <= ATTACK_DISTANCE + 1.0f)
        return false;

    // 锚点 = 队里坦克站的位置（"和 t 一起卡视野"）；没有坦克就退回自己脚下
    WorldPosition anchor(bot);
    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->getSource();
            if (member && member != bot && sServerFacade.IsAlive(member) && ai->IsTank(member))
            {
                anchor = WorldPosition(member);
                break;
            }
        }
    }

    std::vector<Creature*> pack;
    pack.push_back(creature);

    const WorldPosition corner = CornerPullPositionAction::FindCornerPoint(ai, anchor, pack);
    if (!corner)
        return false;

    // 已经在卡视野点 3 码内就别再挪（避免抖动）
    if (WorldPosition(bot).fDist(corner) <= 3.0f)
        return false;

    return MoveTo(corner.getMapId(), corner.getX(), corner.getY(), corner.getZ(), false, false, false, true);
}

// [pull 2026-10-18 站长口径] 找"正在打队友、而我没有仇恨"的怪（取最近的一只）。
Unit* GrabLooseMobAction::FindLooseMob()
{
    Player* bot = ai->GetBot();
    if (!bot || !bot->GetGroup())
        return nullptr;

    Group* group = bot->GetGroup();
    Unit* best = nullptr;
    float bestDistance = 0.0f;

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

        if (sServerFacade.GetThreatManager(unit).getThreat(bot) > 0.0f)
            continue;

        const float distance = bot->GetDistance(unit);
        if (!best || distance < bestDistance)
        {
            best = unit;
            bestDistance = distance;
        }
    }

    return best;
}

// [pull 2026-10-18 站长口径] 补拉：走过去 + 嘲讽，直到"怪的目标是我 + 我对它有仇恨"。
bool GrabLooseMobAction::Execute(Event& event)
{
    Unit* loose = FindLooseMob();
    if (!loose)
        return false;

    SET_AI_VALUE(ObjectGuid, "current target", loose->GetObjectGuid());

    // 还在外面 ⇒ 走过去（站到近战距离）
    if (sServerFacade.GetDistance2d(bot, loose) > ATTACK_DISTANCE)
    {
        return MoveTo(loose->GetMapId(), loose->GetPositionX(), loose->GetPositionY(), loose->GetPositionZ(), false, false, false, true);
    }

    // 到近战了：先嘲讽（各职业的叫法不同，按顺序试），嘲讽不到就直接上手
    bool hasAggro = loose->GetVictim() && loose->GetVictim()->GetObjectGuid() == bot->GetObjectGuid();
    if (!hasAggro)
    {
        if (ai->DoSpecificAction("taunt", event, true) ||
            ai->DoSpecificAction("hand of reckoning", event, true) ||
            ai->DoSpecificAction("righteous defense", event, true) ||
            ai->DoSpecificAction("growl", event, true) ||
            ai->DoSpecificAction("dark command", event, true))
        {
            return true;
        }
    }

    // 嘲讽都没出手成功（冷却/没学）⇒ 直接打它，把仇恨抢回来
    if (!bot->GetVictim() || bot->GetVictim()->GetObjectGuid() != loose->GetObjectGuid())
        bot->Attack(loose, true);

    return true;
}

    // [wave 2026-10-18 站长口径] 当前该往哪一波推进：优先当前目标的 ord，其次可攻击目标里最小的 ord。
    uint16 AdvanceWaveAction::CurrentWaveOrder(PlayerbotAI* ai)
    {
        if (!ai)
            return 0;

        Unit* current = ai->GetUnit(ai->GetAiObjectContext()->GetValue<ObjectGuid>("current target")->Get());
        uint16 ord = dungeonroute::GetOrder(ai, current);
        if (ord)
            return ord;

        std::list<ObjectGuid> targets = ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid> >("possible attack targets")->Get();
        uint16 best = 0;
        for (std::list<ObjectGuid>::iterator i = targets.begin(); i != targets.end(); ++i)
        {
            Unit* unit = ai->GetUnit(*i);
            if (!unit || !unit->IsAlive())
                continue;

            uint16 const unitOrd = dungeonroute::GetOrder(ai, unit);
            if (unitOrd && (!best || unitOrd < best))
                best = unitOrd;
        }

        return best;
    }

    bool AdvanceWaveAction::NeedsAdvance(PlayerbotAI* ai)
    {
        if (!ai)
            return false;

        Player* bot = ai->GetBot();
        if (!bot || !bot->IsInWorld() || !bot->GetMap() || !bot->GetMap()->IsDungeon())
            return false;

        if (!PlayerbotAI::IsTank(bot))
            return false;

        // [patrol v2 2026-10-18 站长口径] 战斗中**不推进**：
        //   类型B 巡逻怪是"遇上就清"的，它可能在打当前波时走进来。
        //   原来只看"下一波离我多远"，于是正在跟巡逻怪打的时候也算"离下一波够远"
        //   ⇒ 队伍会一边打一边往下一波走，把这只巡逻怪拖着走或者干脆丢掉半血。
        if (bot->IsInCombat())
            return false;

        // 队伍血量不够 ⇒ 停（Hold）
        if (sPlayerbotAIConfig.waveHoldHealthPercent > 0 &&
            ai->GetAiObjectContext()->GetValue<uint8>("health", "self target")->Get() < sPlayerbotAIConfig.waveHoldHealthPercent)
            return false;

        uint16 const ord = CurrentWaveOrder(ai);
        if (!ord)
            return false;

        float const distance = dungeonroute::GetWaveDistance(ai, ord);
        if (distance < 0.0f)
            return false;

        return distance > sPlayerbotAIConfig.waveAdvanceSafeDistance;
    }

    bool AdvanceWaveAction::Execute(Event& event)
    {
        if (!NeedsAdvance(ai))
            return false;

        uint16 const ord = CurrentWaveOrder(ai);
        WorldPosition center;
        if (!dungeonroute::GetWaveCenter(ai, ord, center))
            return false;

        // 目标点 = 波次质心往外(朝我这边) D_safe 码
        float const safe = sPlayerbotAIConfig.waveAdvanceSafeDistance;
        WorldPosition botPos(bot);
        float dx = botPos.getX() - center.getX();
        float dy = botPos.getY() - center.getY();
        float const len = sqrt(dx * dx + dy * dy);
        if (len < 0.1f)
            return false;

        dx /= len;
        dy /= len;

        float const destX = center.getX() + dx * safe;
        float const destY = center.getY() + dy * safe;
        float destZ = center.getZ();

        WorldPosition dest(center.getMapId(), destX, destY, destZ, 0.0f);
        dest.setZ(dest.getHeight());

        if (botPos.fDist(dest) <= 2.0f)
            return false;

        return MoveTo(dest.getMapId(), dest.getX(), dest.getY(), dest.getZ(), false, false, false, true);
    }

    // [boss-pos 2026-10-18 站长口径] BOSS 战按角色站位（见 PullActions.h / BossPositions.h）：
    //   每个 BOSS 站哪儿在自己的表里（游戏内 `.bot bosspos` 录入），这里只负责"站过去 + 别出圈"。
    bool StandBossPositionAction::FindSpot(PlayerbotAI* ai, uint32& entry, bossposition::Spot& spot)
    {
        if (!ai)
            return false;

        Player* bot = ai->GetBot();
        if (!bot || !bot->IsInWorld())
            return false;

        entry = bossposition::FightEntry(ai, bot);
        if (!entry)
            return false;

        return bossposition::Get(entry, bot->GetMapId(), bossposition::RoleOf(ai, bot), spot);
    }

    bool StandBossPositionAction::NeedsStand(PlayerbotAI* ai)
    {
        uint32 entry = 0;
        bossposition::Spot spot;
        if (!FindSpot(ai, entry, spot))
            return false;

        return ai->GetBot()->GetDistance(spot.x, spot.y, spot.z) > spot.radius;
    }

    bool StandBossPositionAction::Execute(Event& event)
    {
        uint32 entry = 0;
        bossposition::Spot spot;
        if (!FindSpot(ai, entry, spot))
            return false;

        // 已经在这个 BOSS 的战场半径内 ⇒ 不动（半径内自由接怪，不来回抖）
        if (bot->GetDistance(spot.x, spot.y, spot.z) <= spot.radius)
            return false;

        uint32 mapId = spot.mapId ? spot.mapId : bot->GetMapId();
        return MoveTo(mapId, spot.x, spot.y, spot.z, false, false, false, true);
    }
