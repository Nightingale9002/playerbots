#include "playerbot/playerbot.h"
#include "RelationValues.h"
#include "ThreatValues.h"

#include "playerbot/ServerFacade.h"

#include <algorithm>
#include <map>
#include <list>

using namespace ai;

namespace
{
    // "一发大概会涨多少仇恨"的估计参数。先做成常量，等对应策略落地时再搬进 aiplayerbot.conf。
    float const kThreatReserveRatio = 1.15f;    // 余量系数
    uint32 const kThreatReserveWindow = 10;     // 统计窗口（秒）
    size_t const kThreatProbeLimit = 32;        // 单目标最多保留的采样点

    // 关系类值统一的目标取法（不能直接用 AI_VALUE 宏：Value 类里没有 `context` 成员）。
    //   qualifier 通常是值名（"current target"），空则默认 current target。
    Unit* ResolveQualifiedTarget(PlayerbotAI* ai, std::string const& qualifier)
    {
        if (!ai || !ai->GetBot() || !ai->GetBot()->IsInWorld())
            return nullptr;

        std::string const targetValue = qualifier.empty() ? std::string("current target") : qualifier;
        Unit* target = ai->GetUnit(::AiValueOr<ObjectGuid>(ai->GetAiObjectContext(), targetValue));

        // 和 ThreatValues 保持一致：给的是队友 ⇒ 换成它在打的目标
        if (target && target->IsFriend(ai->GetBot()))
            target = target->GetTarget();

        return target;
    }
}

// ---------------------------------------------------------------------------------------------
float PredictedThreatValue::Calculate()
{
    Unit* target = ResolveQualifiedTarget(ai, getQualifier());
    if (!target || target->GetObjectGuid().IsPlayer())
        return 0.0f;

    float const myThreat = ThreatValue::GetThreat(bot, target);
    time_t const now = time(0);

    std::list<Probe>& probes = history[target->GetObjectGuid()];
    probes.push_back(Probe{ now, myThreat });
    while (!probes.empty() && now - probes.front().time > (time_t)kThreatReserveWindow)
        probes.pop_front();
    while (probes.size() > kThreatProbeLimit)
        probes.pop_front();

    // 窗口内单次最大正增量 = "下一发大概能涨多少"的保守估计
    float reserve = 0.0f;
    float previous = 0.0f;
    bool first = true;
    for (Probe const& probe : probes)
    {
        if (!first && probe.threat > previous)
            reserve = std::max(reserve, probe.threat - previous);

        previous = probe.threat;
        first = false;
    }

    return myThreat + reserve * kThreatReserveRatio;
}

// ---------------------------------------------------------------------------------------------
bool WillOveraggroValue::Calculate()
{
    Unit* target = ResolveQualifiedTarget(ai, getQualifier());
    if (!target || target->GetObjectGuid().IsPlayer())
        return false;

    // 坦克自己不需要压
    if (ai->IsTank(bot))
        return false;

    float const tankThreat = ThreatValue::GetTankThreat(ai, target);
    if (tankThreat <= 0.0f)
        return false;   // 没有坦克 / 坦克还没上仇恨 ⇒ 交给 `threat` 的百分比规则，这里不重复拦

    AiObjectContext* context = ai->GetAiObjectContext();
    if (!context)
        return false;

    std::string const qualifier = getQualifier().empty() ? std::string("current target") : getQualifier();
    float const predicted = ::AiValueOr<float>(context, std::string("predicted threat") + "::" + qualifier);

    return predicted >= tankThreat;
}

// ---------------------------------------------------------------------------------------------
std::list<ObjectGuid> LooseMobsValue::Calculate()
{
    std::list<ObjectGuid> result;

    if (!bot || !bot->IsInWorld())
        return result;

    Group* group = bot->GetGroup();
    if (!group)
        return result;

    // [2026-10-21 站长口径] "有怪在打队友 ⇒ 坦克去拉回来" 必须是一条**策略**，
    //   不能只依赖 possible attack targets（路线过滤会把非当前波的怪剔掉）。
    //   这里直接从队伍成员的 attackers 里收集：正在打某个队友、且坦克没有仇恨的敌对单位。
    struct LooseRank
    {
        uint8 healer;           // 0 = 正在打治疗（最该先接），1 = 其它
        uint8 healthPercent;    // 受害者血量百分比（越低越急）
        ObjectGuid guid;
    };

    std::vector<LooseRank> ranked;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->getSource();
        if (!member || !member->IsInWorld() || !member->IsAlive())
            continue;

        for (Unit* attacker : member->getAttackers())
        {
            if (!attacker || !attacker->IsInWorld() || !attacker->IsAlive())
                continue;
            if (attacker->GetTypeId() != TYPEID_UNIT)
                continue;

            // 打坦克自己的不算"漏"（那是坦克的责任），坦克已有仇恨的也不算漏
            if (attacker->GetVictim() == bot)
                continue;
            if (ThreatValue::GetTankThreat(ai, attacker) > 0.0f)
                continue;
            if (!bot->IsWithinDistInMap(attacker, 60.0f))
                continue;

            bool duplicate = false;
            for (auto const& r : ranked)
            {
                if (r.guid == attacker->GetObjectGuid())
                {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate)
                continue;

            bool const hitsHealer = PlayerbotAI::IsHeal(member);
            ranked.push_back({ hitsHealer ? 0 : 1, (uint8)member->GetHealthPercent(), attacker->GetObjectGuid() });
        }
    }

    std::sort(ranked.begin(), ranked.end(), [](LooseRank const& a, LooseRank const& b)
    {
        if (a.healer != b.healer)
            return a.healer < b.healer;     // 0 = 打治疗，排前面

        return a.healthPercent < b.healthPercent;
    });

    for (auto const& entry : ranked)
        result.push_back(entry.guid);

    return result;
}

// ---------------------------------------------------------------------------------------------
bool TargetAttackingHealerValue::Calculate()
{
    if (!bot || !bot->IsInWorld())
        return false;

    AiObjectContext* context = ai->GetAiObjectContext();
    if (!context)
        return false;

    Unit* target = ai->GetUnit(::AiValueOr<ObjectGuid>(context, "current target"));
    if (!target)
        return false;

    Unit* victim = target->GetVictim();
    if (!victim || victim == bot)
        return false;

    Player* victimPlayer = dynamic_cast<Player*>(victim);
    if (!victimPlayer || !sServerFacade.IsAlive(victimPlayer))
        return false;

    return PlayerbotAI::IsHeal(victimPlayer);
}
