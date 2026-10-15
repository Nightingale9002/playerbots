
#include "playerbot/playerbot.h"
#include "HunterTriggers.h"
#include "HunterActions.h"
#include "playerbot/strategy/values/PossibleAttackTargetsValue.h"
#include "playerbot/strategy/values/AoeValues.h"
#include "Grids/GridNotifiers.h"
#include "Grids/GridNotifiersImpl.h"
#include "Grids/CellImpl.h"

using namespace ai;

// [bm hunter 2026-10-17 站长口径] 技能的实际施放距离（取不到时用 fallback）。
float ai::HunterSpellRange(PlayerbotAI* ai, std::string spellName, float fallback)
{
    if (ai)
    {
        float maxRange = 0.0f, minRange = 0.0f;
        if (ai->GetSpellRange(spellName, &maxRange, &minRange) && maxRange > 0.0f)
            return maxRange;
    }

    return fallback;
}

// [bm hunter 2026-10-17 站长口径] AOE 的破控判据：只看"多目标选择"挑出来的那一簇。
bool ai::HunterHasBreakableCCInAoeCluster(Player* bot, float range)
{
    if (!bot || !bot->GetPlayerbotAI())
        return false;

    if (range <= 0.0f)
        range = 35.0f;

    std::list<ObjectGuid> cluster = AoeCountValue::FindMaxDensity(bot, range);
    for (std::list<ObjectGuid>::iterator i = cluster.begin(); i != cluster.end(); ++i)
    {
        Unit* unit = bot->GetPlayerbotAI()->GetUnit(*i);
        if (unit && unit->IsAlive() && sServerFacade.IsHostileTo(unit, bot) &&
            PossibleAttackTargetsValue::HasBreakableCC(unit, bot))
            return true;
    }

    return false;
}

// [bm hunter 2026-10-17 站长口径] 平射禁飞区（见 HunterTriggers.h 的说明）。
bool ai::HunterRangedSwingTooSoon(Player* bot)
{
    if (!bot)
        return false;

    uint32 timer = bot->getAttackTimer(RANGED_ATTACK);
    return timer > 0 && timer <= 500;
}

// [hunter-weave 2026-10-18 站长口径] 平射穿插（见 HunterTriggers.h 的说明）。
bool ai::HunterShouldLetAutoShotLand(Player* bot)
{
    if (!bot)
        return false;

    uint32 timer = bot->getAttackTimer(RANGED_ATTACK);
    if (!timer)                 // 没有平射在跑 ⇒ 没有可保的，不拦
        return false;

    uint32 speed = bot->GetAttackTime(RANGED_ATTACK);
    if (!speed)                 // 取不到武器速度时的兜底（TBC 弓/枪典型 2.9s）
        speed = 2900;

    // "刚打出平射"的窗口：计时刚被重置到接近满值。窗口外 = 还在等这一发 ⇒ 让给它
    const uint32 window = 400;
    return timer + window < speed;
}

// [bm hunter 2026-10-17 站长口径] 猎人印记由队伍/团队里"第一个猎人"负责保持，其他人不抢。
bool HuntersMarkTrigger::IsActive()
{
    if (!DebuffTrigger::IsActive())
        return false;

    Group* group = bot->GetGroup();
    if (!group)
        return true;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->getSource();
        if (!member)
            continue;

        if (member->getClass() == CLASS_HUNTER)
            return member->GetObjectGuid() == bot->GetObjectGuid();
    }

    return true;
}


bool HunterNoStingsActiveTrigger::IsActive()
{
	Unit* target = ai->GetUnit(AI_VALUE(ObjectGuid, "current target"));
    return target && AI_VALUE2(uint8, "health", "current target") > 40 &&
        !ai->HasAura("serpent sting", target) &&
        !ai->HasAura("scorpid sting", target) &&
        !ai->HasAura("viper sting", target);
}

bool HuntersPetDeadTrigger::IsActive()
{
    return AI_VALUE(bool, "pet dead") && !AI_VALUE2(bool, "mounted", "self target");
}

bool HuntersPetLowHealthTrigger::IsActive()
{
    Unit* pet = ai->GetUnit(AI_VALUE(ObjectGuid, "pet target"));
    return pet && AI_VALUE2(uint8, "health", "pet target") < 40 &&
        !AI_VALUE2(bool, "dead", "pet target") && !AI_VALUE2(bool, "mounted", "self target");
}

bool HunterPetNotHappy::IsActive()
{
    return !AI_VALUE(bool, "pet happy") && !AI_VALUE2(bool, "mounted", "self target");
}

bool ViperStingOnAttackerTrigger::IsActive()
{
    Unit* target = GetTarget();
    if (target)
    {
        const bool noStings = !ai->HasAura("serpent sting", target) &&
                              !ai->HasAura("scorpid sting", target) &&
                              !ai->HasAura("viper sting", target);
        if (noStings)
        {
            if (target->GetPower(POWER_MANA) >= 10)
            {
                return DebuffOnAttackerTrigger::IsActive();
            }
        }
    }

    return false;
}

bool SerpentStingOnAttackerTrigger::IsActive()
{
    Unit* target = GetTarget();
    if (target)
    {
        const bool noStings = !ai->HasAura("serpent sting", target) &&
                              !ai->HasAura("scorpid sting", target) &&
                              !ai->HasAura("viper sting", target);
        if (noStings)
        {
            if (target->GetPower(POWER_MANA) < 10 || ai->HasStrategy("sting serpent", BotState::BOT_STATE_COMBAT))
            {
                return DebuffOnAttackerTrigger::IsActive();
            }
        }
    }

    return false;
}
