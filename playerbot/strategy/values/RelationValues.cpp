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

    AiObjectContext* context = ai->GetAiObjectContext();
    if (!context)
        return result;

    std::list<ObjectGuid> const candidates = ::AiValueOr<std::list<ObjectGuid>>(context, "possible attack targets");

    // 先收集候选（记下"受害者是不是治疗"和"受害者血量"，用来排序）
    struct LooseRank
    {
        uint8 healer;           // 0 = 正在打治疗（最该先接），1 = 其它
        uint8 healthPercent;    // 受害者血量百分比（越低越急）
        ObjectGuid guid;
    };

    std::vector<LooseRank> ranked;
    for (ObjectGuid const& guid : candidates)
    {
        Unit* unit = ai->GetUnit(guid);
        if (!unit || !unit->IsInWorld() || !unit->IsAlive())
            continue;

        Unit* victim = unit->GetVictim();
        if (!victim || victim == bot)
            continue;   // 打我的不算"漏"（那是我的责任）

        Player* victimPlayer = dynamic_cast<Player*>(victim);
        if (!victimPlayer || victimPlayer->GetGroup() != group)
            continue;

        if (!sServerFacade.IsAlive(victimPlayer))
            continue;

        // 坦克已经有仇恨的目标不算漏
        if (ThreatValue::GetTankThreat(ai, unit) > 0.0f)
            continue;

        // [tank 2026-10-19 站长口径] "target attacking healer 应该是 t 策略"：
        //   正在打治疗的漏怪排在最前面（其次是"受害者血量最低"）。
        bool const hitsHealer = PlayerbotAI::IsHeal(victimPlayer);
        ranked.push_back({ hitsHealer ? 0 : 1, victimPlayer->GetHealthPercent(), guid });
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
