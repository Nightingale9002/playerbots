
#include "playerbot/playerbot.h"
#include "playerbot/DungeonRoute.h"
#include "DungeonRouteTriggers.h"

using namespace ai;

// [2026-10-18 站长口径] "打完最近的一波之后就不会继续推进"：
//   如果当前波**没有任何可打目标**（友好 NPC / 小动物 / 怪全在远处没加载），
//   旧逻辑是 FindTarget 直接 false ⇒ 两个触发器都不激活 ⇒ 坦克原地站着。
//   这里加"自动跳过"：等 WAVE_NOT_LOADED_MS 就把这一波标记为放弃，ord 自然推进到下一波。
static bool AutoSkipIfNoTarget(Player* bot, dungeonroute::MapRoute const& route, bool hasTarget)
{
    if (hasTarget)
        return false;

    uint16 const ord = dungeonroute::CurrentOrd(bot, route);
    // [2026-10-22 站长口径] 有怪在打我们（含未登记进路线表的怪）就不许因为"没目标"跳过这一波
    if (ord && dungeonroute::WaveStuckMs(bot) > dungeonroute::WAVE_NOT_LOADED_MS &&
        !dungeonroute::WaveInFight(bot, route, ord) &&
        !dungeonroute::UnknownFighterInCombat(bot, route))
        dungeonroute::MarkSkipped(bot, ord, "wave no valid target");
    return true;
}

bool DungeonRouteMoveTrigger::IsActive()
{
    if (!dungeonroute::ShouldDrive(ai))
    {
        return false;
    }

    Map* map = bot->GetMap();
    dungeonroute::MapRoute const* route = dungeonroute::GetMapRoute(map->GetId());
    if (!route || !route->verified)
    {
        return false;
    }

    dungeonroute::Target target;
    bool const hasTarget = dungeonroute::FindTargetNearest(bot, *route, target) && target.hasPullPoint;
    if (AutoSkipIfNoTarget(bot, *route, hasTarget))
    {
        return false;
    }
    if (!hasTarget)
        return false;

    {
        // [2026-10-25 修] PULLSAFE 的安全点也算"到位"：否则坦克从安全点被推回原拉怪位，
        //   到原拉怪位 PullAction 又把它送回安全点 ⇒ 两个动作互顶不开怪（557 实测）。
        float ex = target.px, ey = target.py, ez = target.pz;
        bool const safePt = dungeonroute::SafePullPoint(bot, target, ex, ey, ez);
        float const dStand = bot->GetDistance(ex, ey, ez);
        float const dPoint = bot->GetDistance(target.px, target.py, target.pz);
        bool const atStand = safePt && dStand <= dungeonroute::AT_PULL_POINT_DIST;
        bool const needMove = !atStand && dPoint > dungeonroute::AT_PULL_POINT_DIST;
        return needMove;
    }

    return bot->GetDistance(target.px, target.py, target.pz) > dungeonroute::AT_PULL_POINT_DIST;
}

bool DungeonRoutePullTrigger::IsActive()
{
    if (!dungeonroute::ShouldDrive(ai))
    {
        return false;
    }

    Map* map = bot->GetMap();
    dungeonroute::MapRoute const* route = dungeonroute::GetMapRoute(map->GetId());
    if (!route || !route->verified)
    {
        return false;
    }

    dungeonroute::Target target;
    bool const hasTarget = dungeonroute::FindTargetNearest(bot, *route, target) && target.hasPullPoint;
    if (AutoSkipIfNoTarget(bot, *route, hasTarget))
    {
        return false;
    }
    if (!hasTarget)
        return false;

    {
        // [2026-10-25 修] 站在安全拉怪点（PULLSAFE 挪位点）也要能触发开怪 —— 见 SafePullPoint 注释。
        float ex = target.px, ey = target.py, ez = target.pz;
        bool const safePt = dungeonroute::SafePullPoint(bot, target, ex, ey, ez);
        float const dStand = bot->GetDistance(ex, ey, ez);
        float const dPoint = bot->GetDistance(target.px, target.py, target.pz);
        bool const atStand = safePt && dStand <= dungeonroute::AT_PULL_POINT_DIST;
        return atStand || dPoint <= dungeonroute::AT_PULL_POINT_DIST;
    }

    return bot->GetDistance(target.px, target.py, target.pz) <= dungeonroute::AT_PULL_POINT_DIST;
}


// [2026-10-24 站长口径] 战斗中有怪选了队友、自己没有目标 ⇒ 触发"补拉漏怪"
bool DungeonLooseMobTrigger::IsActive()
{
    // [2026-10-24 站长口径] 补拉只能是**坦克**的事：DPS/治疗去嘲讽抢怪会让坦克 TPS 更低、OT 反复
    if (!bot || !PlayerbotAI::IsTank(bot))
        return false;
    if (!bot->IsInCombat() || bot->GetVictim())
        return false;
    Group* grp = bot->GetGroup();
    if (!grp)
        return false;
    for (GroupReference* ref = grp->GetFirstMember(); ref; ref = ref->next())
    {
        Player* m = ref->getSource();
        if (!m || !m->IsInWorld())
            continue;
        for (Unit* a : m->getAttackers())
        {
            if (!a || !a->IsAlive() || a->GetTypeId() != TYPEID_UNIT)
                continue;
            if (a->GetMapId() != bot->GetMapId())
                continue;
            if (bot->GetDistance(a) <= 40.0f)
                return true;
        }
    }
    return false;
}


// [2026-10-24 站长口径] 坦克 OT：按**仇恨表**判断（法术临时锁目标不算 OT）
bool DungeonTankOtTrigger::IsActive()
{
    // [2026-10-24 站长口径] 判据就一条：**有怪在打队友**（怪的目标是队友、不是坦克）= OT
    if (!bot || !PlayerbotAI::IsTank(bot) || !bot->IsInCombat())
        return false;
    Group* grp = bot->GetGroup();
    if (!grp)
        return false;

    std::set<ObjectGuid> seen;
    for (GroupReference* ref = grp->GetFirstMember(); ref; ref = ref->next())
    {
        Player* m = ref->getSource();
        if (!m || !m->IsInWorld() || m == bot)
            continue;
        for (Unit* a : m->getAttackers())
        {
            if (!a || !a->IsAlive() || a->GetTypeId() != TYPEID_UNIT || a->GetMapId() != bot->GetMapId())
                continue;
            if (!seen.insert(a->GetObjectGuid()).second)
                continue;
            if (bot->GetDistance(a) > 30.0f)
                continue;
            if (a->GetVictim() == bot)          // 已经拉回坦克
                continue;
            return true;                         // 有怪在打队友 ⇒ OT
        }
    }
    return false;
}




// [2026-10-24 站长口径] 群拉判据：战斗中的怪里，**没有被控制**的 >= 3 只
bool DungeonTankAoeTrigger::IsActive()
{
    if (!bot || !PlayerbotAI::IsTank(bot) || !bot->IsInCombat())
        return false;
    Group* grp = bot->GetGroup();
    if (!grp)
        return false;

    auto isControlled = [](Unit* u) -> bool
    {
        if (!u)
            return false;
        if (u->IsPolymorphed())
            return true;
        if (u->hasUnitState(UNIT_STAT_CAN_NOT_REACT_OR_LOST_CONTROL | UNIT_STAT_ROOT))
            return true;
        if (u->hasUnitState(UNIT_STAT_FLEEING | UNIT_STAT_CONFUSED | UNIT_STAT_STUNNED))
            return true;
        return false;
    };

    std::set<ObjectGuid> seen;
    uint32 free = 0;
    for (GroupReference* ref = grp->GetFirstMember(); ref; ref = ref->next())
    {
        Player* m = ref->getSource();
        if (!m || !m->IsInWorld())
            continue;
        for (Unit* a : m->getAttackers())
        {
            if (!a || !a->IsAlive() || a->GetTypeId() != TYPEID_UNIT || a->GetMapId() != bot->GetMapId())
                continue;
            if (!seen.insert(a->GetObjectGuid()).second)
                continue;
            if (isControlled(a))
                continue;
            if (++free >= 3)
                return true;
        }
    }
    return false;
}


// [2026-10-24 站长口径] 站位调整：战斗中当前位置已经踩到"未进战斗怪"的仇恨圈（30 码内且 LOS 通过）
bool DungeonTankRepositionTrigger::IsActive()
{
    // [2026-10-25 站长口径] 不节流：**该是就是** —— 当前位置不安全就该挪。
    //   脱战：aggro+5（巡逻怪再 +5）；战斗中：aggro+2（马上要引到才算）。
    if (!bot || !PlayerbotAI::IsTank(bot) || !bot->GetMap())
        return false;

    dungeonroute::BotState& rpst = dungeonroute::StateOf(bot);

    // [2026-10-25 站长口径] **拉回**：刚起手、还没退回到拉怪位 ⇒ 无条件退回去（此时不看站得安不安全）。
    if (rpst.pullBackPending && bot->IsInCombat() && rpst.standOverrideUntil &&
        WorldTimer::getMSTime() < rpst.standOverrideUntil &&
        bot->GetDistance(rpst.standOverrideX, rpst.standOverrideY, rpst.standOverrideZ) > 5.0f)
        return true;

    // [2026-10-25 修复] 用**状态**判断，不用时间节流：
    //   上一次决策的挪位点还没走到 ⇒ 这次挪位正在执行中，不许重新决策。
    //   原来每 tick 都从当前位置重挑"最近安全点"，坦克就在 1~5 码里抖、永远到不了任何点，
    //   条件因此永远为真 —— 这才是 REPOS 每 tick 触发、而且挪了等于没挪的原因。
    if (rpst.standOverrideUntil && WorldTimer::getMSTime() < rpst.standOverrideUntil &&
        bot->GetDistance(rpst.standOverrideX, rpst.standOverrideY, rpst.standOverrideZ) >
            dungeonroute::AT_PULL_POINT_DIST)
        return false;

    float const margin = bot->IsInCombat() ? 2.0f : 5.0f;
    Creature* bad = dungeonroute::UnsafeStandOffender(bot, bot->GetPositionX(), bot->GetPositionY(),
        bot->GetPositionZ(), margin);

    // 从"不安全位置"拉起来的 -> 战斗中也要 pullback 到安全点
    bool const pending = (rpst.pullBackPending != 0);
    if (!bad && !(pending && bot->IsInCombat()))
        return false;
    if (!bad && pending)
        bad = dungeonroute::UnsafeStandOffender(bot, bot->GetPositionX(), bot->GetPositionY(),
            bot->GetPositionZ(), 5.0f);
    if (!bad)
        return false;

    // [2026-10-25] 这里不再打日志：条件每天几百上千次为真，刷屏且掩盖真正的日志。
    //   真正的"退了多少码、避哪只、目标距离"由 DungeonTankRepositionAction 在**真的退位**时打印。
    return true;
}


// [2026-10-25 站长口径] **坦克是队伍核心**：任意队友正被敌对怪攻击 ⇒ 立刻救场。
//   不看坦克自己有没有目标、正在打谁、在走哪条路线 —— 优先级最高（60），压过 OT/站位/推进。
bool DungeonTankEngageTrigger::IsActive()
{
    if (!bot || !PlayerbotAI::IsTank(bot) || !bot->GetMap())
        return false;

    Group* grp = bot->GetGroup();
    if (!grp)
        return false;

    Creature* victimOfTeammate = nullptr;
    uint32 worstDeficit = 0;
    for (GroupReference* ref = grp->GetFirstMember(); ref; ref = ref->next())
    {
        Player* m = ref->getSource();
        if (!m || m == bot || !m->IsInWorld())
            continue;
        for (Unit* a : m->getAttackers())
        {
            if (!a || !a->IsAlive() || a->GetTypeId() != TYPEID_UNIT || a->GetMapId() != bot->GetMapId())
                continue;
            Creature* c = static_cast<Creature*>(a);
            if (c->IsCritter() || !bot->CanAttack(c))
                continue;
            // 已经在打它了就不算"需要救场"（避免每 tick 抢断自己的攻击）
            if (bot->GetVictim() && bot->GetVictim()->GetObjectGuid() == c->GetObjectGuid())
                continue;
            // 只处理"坦克没拉住"的：坦克在这只怪上的仇恨要显著低于被打的那个人
            float selfThreat = c->getThreatManager().getThreat(bot);
            float mateThreat = c->getThreatManager().getThreat(m);
            uint32 const deficit = uint32(mateThreat > selfThreat ? (mateThreat - selfThreat) : 0.0f);
            if (!victimOfTeammate || deficit > worstDeficit)
            {
                worstDeficit = deficit;
                victimOfTeammate = c;
            }
        }
    }
    return victimOfTeammate != nullptr;
}


// [2026-10-25 站长口径] **怪必须聚到一起才能开始打**：DPS/法系停手的两个条件见头文件。
bool DungeonDpsHoldFireTrigger::IsActive()
{
    if (!bot || !bot->GetMap() || !bot->IsInCombat())
        return false;
    if (PlayerbotAI::IsTank(bot) || PlayerbotAI::IsHeal(bot))   // 坦克/治疗永不被门禁卡住
        return false;
    if (!bot->GetGroup())
        return false;

    // [2026-10-25 站长口径] **被我方控住的怪谁都不许打**（恐惧/羊/忏悔一碰伤害就破）。
    if (Unit* cv = bot->GetVictim())
    {
        std::lock_guard<std::mutex> lock(dungeonroute::CcMutex());
        auto it = dungeonroute::CcAssign().find(cv->GetObjectGuid().GetRawValue());
        if (it != dungeonroute::CcAssign().end() && it->second > WorldTimer::getMSTime())
            return true;
    }

    // ② 还没聚齐（有任何怪离坦克 >5 码）⇒ 停手。
    //   [2026-10-25 站长口径] **只检查开怪后 5 秒**：boss 召唤小怪 / 有 add 时"聚齐"永远不成立，
    //   不加超时就会死锁（实测 557 站在 Pandemonius 面前 15 秒零伤害、无限循环）。
    {
        dungeonroute::GatherState const g = dungeonroute::ComputeGather(dungeonroute::GroupTank(bot));
        dungeonroute::BotState& gst = dungeonroute::StateOf(bot);
        uint32 const gnow = WorldTimer::getMSTime();
        if (g.pending > 0)
        {
            if (!gst.gatherSince)
                gst.gatherSince = gnow ? gnow : 1;
            if (WorldTimer::getMSTimeDiff(gst.gatherSince, gnow ? gnow : 1) < 5000)
                return true;                 // 开怪 5 秒内：等聚齐
            // 超过 5 秒：放行（宁可多挨几下，也不能站着不打）
        }
        else
            gst.gatherSince = 0;
    }

    // ① 坦克对该目标的仇恨不够（含 tankThreat == 0）⇒ 停手
    Unit* tgt = bot->GetVictim();
    if (!tgt)
        return false;
    float const tankThreat = dungeonroute::ThreatOfTankOn(bot, tgt);
    float const selfThreat = tgt->getThreatManager().getThreat(bot);
    uint32 absLead = sPlayerbotAIConfig.waitForAttackThreatLead;
    if (tgt->GetTypeId() == TYPEID_UNIT)
    {
        Creature* tc = static_cast<Creature*>(tgt);
        if (dungeonroute::IsRouteBoss(tgt->GetMapId(), tc->GetGUIDLow()))
            absLead = sPlayerbotAIConfig.waitForAttackThreatLeadBoss;
        if (dungeonroute::IsImmuneToTaunt(tc))
            absLead = sPlayerbotAIConfig.waitForAttackThreatLeadImmune;
    }
    if (absLead != 0 && tankThreat < float(absLead))
        return true;
    uint32 const pct = sPlayerbotAIConfig.waitForAttackThreatPercent
        ? sPlayerbotAIConfig.waitForAttackThreatPercent : 90;
    if (selfThreat > 1.0f && tankThreat * 100.0f < selfThreat * float(pct))
        return true;
    return false;
}


// [2026-10-25 站长口径] **控制模式**：有 CC 能力的职业去控一只高威胁怪。
//   规则：① 坦克不控（坦克负责把怪拉离被控目标）；② 每人一只、不重复；
//        ③ priority 5（例如 Legionnaire）不控 —— 它是优先击杀目标。
bool DungeonCcCastTrigger::IsActive()
{
    if (!bot || !bot->GetMap() || !bot->IsInCombat())
        return false;
    if (PlayerbotAI::IsTank(bot) || PlayerbotAI::IsHeal(bot))
        return false;
    if (!bot->GetGroup())
        return false;

    std::vector<uint32> const spells = dungeonroute::CcSpellsFor(bot);
    bool hasCc = false;
    for (uint32 id : spells)
        if (bot->HasSpell(id))
        {
            hasCc = true;
            break;
        }
    if (!hasCc)
        return false;

    // 本队战斗中的敌对活怪，排除 priority5
    uint32 candidates = 0;
    for (GroupReference* ref = bot->GetGroup()->GetFirstMember(); ref; ref = ref->next())
    {
        Player* m = ref->getSource();
        if (!m || !m->IsInWorld())
            continue;
        for (Unit* a : m->getAttackers())
        {
            if (!a || !a->IsAlive() || a->GetTypeId() != TYPEID_UNIT || a->GetMapId() != bot->GetMapId())
                continue;
            Creature* c = static_cast<Creature*>(a);
            if (c->IsCritter() || !bot->CanAttack(c))
                continue;
            if (PossibleAttackTargetsValue::ThreatTier(c, bot) >= 5)   // priority 5：优先击杀，不控
                continue;
            if (bot->GetDistance(c) > 30.0f)
                continue;
            ++candidates;
        }
    }
    // 至少两只才值得控（单只就集火杀了）
    return candidates >= 2;
}
