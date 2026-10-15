
#include "playerbot/playerbot.h"
#include "AoeValues.h"

#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/ServerFacade.h"
#include "HazardsValue.h"
using namespace ai;

// [dodge 2026-10-17 方案A] 一轮"敌方地面 AOE"圆环。
namespace
{
    struct HostileAoeCircle
    {
        WorldLocation center;
        float radius;
    };

    // 收集"我方某人脚下"的全部敌方地面 AOE 圆环。
    //   判据与 HostileAoeValue::GetHostileAoeCenter() 保持一致（同一套排除规则），
    //   区别只是：这里**枚举所有**圆圈，且由调用方决定要不要"必须有人站在里面"。
    std::list<HostileAoeCircle> CollectHostileAoeCircles(PlayerbotAI* ai, bool includeSelf, bool requireCoveredMember)
    {
        std::list<HostileAoeCircle> result;

        if (!ai)
            return result;

        Player* bot = ai->GetBot();
        if (!bot || !bot->IsInWorld())
            return result;

        AiObjectContext* ctx = ai->GetAiObjectContext();
        if (!ctx)
            return result;

        std::list<ObjectGuid> dynObjects = *ctx->GetValue<std::list<ObjectGuid> >("nearest dynamic objects no los");
        if (dynObjects.empty())
            return result;

        std::list<Unit*> members;
        if (includeSelf)
            members.push_back(bot);

        if (Group* group = bot->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->getSource();
                if (!member || member == bot || !member->IsInWorld() || !sServerFacade.IsAlive(member))
                    continue;

                if (member->GetMapId() != bot->GetMapId())
                    continue;

                members.push_back(member);
            }
        }

        if (members.empty())
            return result;

        for (ObjectGuid const& guid : dynObjects)
        {
            DynamicObject* go = bot->GetMap()->GetDynamicObject(guid);
            if (!go)
                continue;

            SpellEntry const* spellProto = sSpellTemplate.LookupEntry<SpellEntry>(go->GetSpellId());
            if (!spellProto)
                continue;

            // 火焰花环特例：与 HasAreaDebuffValue 保持一致（"the raid blows up" 那种不该乱跑）
            if (spellProto->Id == 29946)
                continue;

            // 正面效果（奉献、治疗之泉、我方放的地板）不算敌方 AOE
            if (IsPositiveEffect(spellProto, go->GetEffIndex()))
                continue;

            // 必须是我方之外的人放的
            Unit* caster = go->GetCaster();
            if (caster && !sServerFacade.IsHostileTo(bot, caster))
                continue;

            float radius = go->GetRadius();
            if (radius <= 0.0f)
                radius = sPlayerbotAIConfig.aoeRadius;

            if (requireCoveredMember)
            {
                bool covered = false;
                for (Unit* member : members)
                {
                    if (go->GetDistance(member) <= radius)
                    {
                        covered = true;
                        break;
                    }
                }

                if (!covered)
                    continue;
            }

            HostileAoeCircle circle;
            circle.center = WorldLocation(go->GetMapId(), go->GetPositionX(), go->GetPositionY(), go->GetPositionZ(), 0.0f);
            circle.radius = radius;
            result.push_back(circle);
        }

        return result;
    }
}

std::list<ObjectGuid> AoeCountValue::FindMaxDensity(Player* bot, float range)
{
    int maxCount = 0;
    ObjectGuid maxGroup;
    std::map<ObjectGuid, std::list<ObjectGuid> > groups;
    if (bot)
    {
        std::list<ObjectGuid> units = *bot->GetPlayerbotAI()->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("possible attack targets");
        
        for (std::list<ObjectGuid>::iterator i = units.begin(); i != units.end(); ++i)
        {
            Unit* unit = bot->GetPlayerbotAI()->GetUnit(*i);
            if (unit)
            {
                float distanceToPlayer = sServerFacade.GetDistance2d(unit, bot);
                if (sServerFacade.IsDistanceLessOrEqualThan(distanceToPlayer, range))
                {
                    for (std::list<ObjectGuid>::iterator j = units.begin(); j != units.end(); ++j)
                    {
                        Unit* other = bot->GetPlayerbotAI()->GetUnit(*j);
                        if (other)
                        {
                            float d = sServerFacade.GetDistance2d(unit, other);
                            if (sServerFacade.IsDistanceLessOrEqualThan(d, sPlayerbotAIConfig.aoeRadius * 2.0f))
                            {
                                groups[*i].push_back(*j);
                            }
                        }
                    }

                    if (maxCount < groups[*i].size())
                    {
                        maxCount = groups[*i].size();
                        maxGroup = *i;
                    }
                }
            }
        }
    }

    if (!maxCount)
    {
        return std::list<ObjectGuid>();
    }

    return groups[maxGroup];
}

WorldLocation AoePositionValue::Calculate()
{
    std::list<ObjectGuid> group = AoeCountValue::FindMaxDensity(bot);
    if (group.empty())
        return WorldLocation();

    // Note: don't know where these values come from or even used.
    float x1, y1, x2, y2;
    for (std::list<ObjectGuid>::iterator i = group.begin(); i != group.end(); ++i)
    {
        Unit* unit = bot->GetPlayerbotAI()->GetUnit(*i);
        if (!unit)
            continue;

        if (i == group.begin() || x1 > unit->GetPositionX())
            x1 = unit->GetPositionX();
        if (i == group.begin() || x2 < unit->GetPositionX())
            x2 = unit->GetPositionX();
        if (i == group.begin() || y1 > unit->GetPositionY())
            y1 = unit->GetPositionY();
        if (i == group.begin() || y2 < unit->GetPositionY())
            y2 = unit->GetPositionY();
    }
    float x = (x1 + x2) / 2;
    float y = (y1 + y2) / 2;
    float z = bot->GetPositionZ() + CONTACT_DISTANCE;;
    bot->UpdateAllowedPositionZ(x, y, z);
    return WorldLocation(bot->GetMapId(), x, y, z, 0);
}

uint8 AoeCountValue::Calculate()
{
    return FindMaxDensity(bot).size();
}

bool HasAreaDebuffValue::Calculate()
{
    if (!GetTarget())
        return false;

    Unit* checkTarget = GetTarget();
    if (!checkTarget)
        return false;

    std::list<ObjectGuid> nearestDynObjects = *context->GetValue<std::list<ObjectGuid> >("nearest dynamic objects no los");
    if (nearestDynObjects.empty())
        return false;

    for (std::list<ObjectGuid>::iterator i = nearestDynObjects.begin(); i != nearestDynObjects.end(); ++i)
    {
        DynamicObject* go = checkTarget->GetMap()->GetDynamicObject(*i);
        if (!go)
            continue;

        SpellEntry const* spellProto = sSpellTemplate.LookupEntry<SpellEntry>(go->GetSpellId());
        if (!spellProto)
            continue;
        
        // I will not move when flame wreath is cast or the raid blows up
        if (spellProto->Id == 29946)
            continue;

        if (IsPositiveEffect(spellProto, go->GetEffIndex()))
            continue;

        if (go->IsAffecting(checkTarget))
            return true;
    }

    return false;
}

// [rules 2026-10-17] `hostile aoe incoming` 的判据（说明见 AoeValues.h）。
//   "敌方地面 AOE 正在落在自己/队友脚下" —— 与 HasAreaDebuffValue 分开实现，不改它的语义。
bool HostileAoeValue::GetHostileAoeCenter(PlayerbotAI* ai, bool includeSelf, WorldLocation& outCenter, Unit*& outVictim)
{
    if (!ai)
        return false;

    Player* bot = ai->GetBot();
    if (!bot || !bot->IsInWorld())
        return false;

    AiObjectContext* ctx = ai->GetAiObjectContext();
    if (!ctx)
        return false;

    std::list<HostileAoeCircle> circles = CollectHostileAoeCircles(ai, includeSelf, true);
    if (circles.empty())
        return false;

    // 找一个正在半径内的人（取最近的那个，方便决定往哪边躲）
    for (HostileAoeCircle const& circle : circles)
    {
        Unit* victim = nullptr;
        float best = 0.0f;

        std::list<Unit*> members;
        if (includeSelf)
            members.push_back(bot);

        if (Group* group = bot->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->getSource();
                if (!member || member == bot || !member->IsInWorld() || !sServerFacade.IsAlive(member))
                    continue;
                if (member->GetMapId() != bot->GetMapId())
                    continue;
                members.push_back(member);
            }
        }

        // 注意：WorldLocation 是被继承的裸坐标结构（没有 distance()，也没有 x/y/z），
        //   所以用 WorldPosition 包一层再算距离。
        WorldPosition centerPos(circle.center);

        for (Unit* member : members)
        {
            float dist = centerPos.distance(member);
            if (dist > circle.radius)
                continue;

            if (!victim || dist < best)
            {
                victim = member;
                best = dist;
            }
        }

        if (victim)
        {
            outVictim = victim;
            outCenter = circle.center;
            return true;
        }
    }

    return false;
}

// [dodge 2026-10-17 方案A] 把敌方地面 AOE 登记进 `stored hazards`，喂给既有的 `move away from hazard`。
uint32 HostileAoeValue::RegisterHostileAoeHazards(PlayerbotAI* ai, bool includeSelf)
{
    if (!ai)
        return 0;

    // 只登记"真的罩住了我方某人"的那些：脚下的地板才需要躲，
    // 远处的圆圈登记了只会让走位到处乱绕（MoveAwayFromHazard 对所有 hazards 都躲）。
    std::list<HostileAoeCircle> circles = CollectHostileAoeCircles(ai, includeSelf, true);

    uint32 registered = 0;
    for (HostileAoeCircle const& circle : circles)
    {
        // 过期时间给足：动态对象本身消失时 Hazard::IsValid() 会失败并被自动清掉，
        //   Hazard 构造里的 expirationTime 参数是**相对秒数**（内部会 + time(0)）。
        Hazard hazard(circle.center, 15, circle.radius);

        // 这里是 static 成员函数，SET_AI_VALUE 宏依赖类内的 `context` 成员 ⇒ 必须显式取 context。
        ::AiSetValue<Hazard>(ai->GetAiObjectContext(), "add hazard", hazard);
        ++registered;
    }

    return registered;
}

bool HostileAoeValue::Calculate()
{
    WorldLocation center;
    Unit* victim = nullptr;
    return GetHostileAoeCenter(ai, true, center, victim);
}

Unit* HostileAoeValue::FindCoveredMember(PlayerbotAI* ai, bool includeSelf)
{
    WorldLocation center;
    Unit* victim = nullptr;
    if (!GetHostileAoeCenter(ai, includeSelf, center, victim))
        return nullptr;

    return victim;
}

    // [dodge 2026-10-18 站长口径] 致命地板判定：任一"正落在我方脚下"的敌方地面 AOE，
    //   其单跳最大伤害 >= AiPlayerbot.LethalAoeMinDamage，或该法术带"直接杀死"效果 ⇒ 致命。
    bool HostileAoeValue::IsLethalIncoming(PlayerbotAI* ai)
    {
        if (!ai)
            return false;

        Player* bot = ai->GetBot();
        if (!bot || !bot->IsInWorld() || !bot->GetMap())
            return false;

        float const lethalDamage = (float)sPlayerbotAIConfig.lethalAoeMinDamage;

        std::list<HostileAoeCircle> circles = CollectHostileAoeCircles(ai, true, true);
        if (circles.empty())
            return false;

        std::list<ObjectGuid> nearestDynObjects = ai->GetAiObjectContext()->
            GetValue<std::list<ObjectGuid> >("nearest dynamic objects no los")->Get();

        for (std::list<ObjectGuid>::iterator i = nearestDynObjects.begin(); i != nearestDynObjects.end(); ++i)
        {
            DynamicObject* go = bot->GetMap()->GetDynamicObject(*i);
            if (!go || go->GetOwnerGuid().IsPlayer())
                continue;

            SpellEntry const* spellProto = sSpellTemplate.LookupEntry<SpellEntry>(go->GetSpellId());
            if (!spellProto)
                continue;

            if (IsPositiveEffect(spellProto, go->GetEffIndex()))
                continue;

            // 直接杀死类效果：无条件致命
            for (uint32 eff = 0; eff < MAX_EFFECT_INDEX; ++eff)
            {
                if (spellProto->Effect[eff] == SPELL_EFFECT_INSTAKILL)
                    return true;
            }

            float damage = 0.0f;
            for (uint32 eff = 0; eff < MAX_EFFECT_INDEX; ++eff)
            {
                if (spellProto->Effect[eff] != SPELL_EFFECT_SCHOOL_DAMAGE &&
                    spellProto->Effect[eff] != SPELL_EFFECT_WEAPON_PERCENT_DAMAGE)
                    continue;

                damage = std::max(damage, (float)spellProto->EffectBasePoints[eff] + 1.0f);
            }

            if (lethalDamage > 0.0f && damage >= lethalDamage)
                return true;
        }

        return false;
}
