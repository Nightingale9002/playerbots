
#include "playerbot/playerbot.h"
#include "GenericTriggers.h"
#include "playerbot/PartyPositionCoordinator.h"
#include "playerbot/strategy/actions/OffensiveDispelActions.h"
#include "playerbot/LootObjectStack.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/strategy/values/PositionValue.h"
#include "playerbot/strategy/values/AoeValues.h"

#include <regex>

using namespace ai;

bool NoManaTrigger::IsActive()
{
    return AI_VALUE2(bool, "has mana", "self target") && AI_VALUE2(uint8, "mana", "self target") < 5;
}

bool LowManaTrigger::IsActive()
{
    return AI_VALUE2(bool, "has mana", "self target") && AI_VALUE2(uint8, "mana", "self target") < sPlayerbotAIConfig.lowMana;
}

bool MediumManaTrigger::IsActive()
{
    return AI_VALUE2(bool, "has mana", "self target") && AI_VALUE2(uint8, "mana", "self target") < sPlayerbotAIConfig.mediumMana;
}

bool HighManaTrigger::IsActive()
{
    return AI_VALUE2(bool, "has mana", "self target") && AI_VALUE2(uint8, "mana", "self target") < 65;
}

bool AlmostFullManaTrigger::IsActive()
{
    return AI_VALUE2(bool, "has mana", "self target") && AI_VALUE2(uint8, "mana", "self target") > 85;
}

bool RageAvailable::IsActive()
{
    return AI_VALUE2(uint8, "rage", "self target") >= amount;
}

bool EnergyAvailable::IsActive()
{
	return AI_VALUE2(uint8, "energy", "self target") >= amount;
}

bool ComboPointsAvailableTrigger::IsActive()
{
    return AI_VALUE2(uint8, "combo", "current target") >= amount;
}

bool EnemyHasDispellableBuffTrigger::IsActive()
{
    std::string spellName;
    return ai::FindDispelEnemy(ai, spellName) != nullptr;
}

bool PartyPositionTrigger::IsActive()
{
    // 决策者：每 500ms 算一次阵型（其它 bot 只读槽位）
    partypos::ComputeIfLeader(bot);

    partypos::Slot slot;
    return partypos::SlotFor(bot, slot);
}

bool LoseAggroTrigger::IsActive()
{
    if(!AI_VALUE2(bool, "has aggro", "current target"))
    {
        // Check if the aggro has been taken by another tank
        if(ai->IsTank(bot))
        {
            Unit* target = ai->GetUnit(AI_VALUE(ObjectGuid, "current target"));
            if(target && !target->IsPlayer())
            {
                Unit* targetsTarget = target->GetVictim();
                if(targetsTarget && targetsTarget->IsPlayer())
                {
                    Player* targetsPlayerTarget = (Player*)targetsTarget;
                    return !ai->IsTank(targetsPlayerTarget);
                }
            }
        }

        return true;
    }

    return false;
}

bool HasAggroTrigger::IsActive()
{
    return AI_VALUE2(bool, "has aggro", "current target");
}

bool WillOveraggroTrigger::IsActive()
{
    return AI_VALUE2(bool, "will overaggro", "current target");
}

bool TooManyHostilesTrigger::IsActive()
{
    // [pull-gate 2026-10-19] 只在"还没开打"时限制：已经进战斗就不再生效
    //   （否则战斗中会被反复压住，等于让 bot 停手）。
    if (!sPlayerbotAIConfig.pullHostileLimit || ai->GetBot()->IsInCombat())
        return false;

    uint8 const nearby = AI_VALUE2(uint8, "nearby hostiles", (int32)sPlayerbotAIConfig.pullHostileRadius);
    return nearby >= sPlayerbotAIConfig.pullHostileLimit;
}

bool BeingMeleedLowHealthTrigger::IsActive()
{
    // [defend-self 2026-10-19 站长口径] 被近战贴脸 + 血量低于阈值 ⇒ 该走位了。
    //   坦克不算（它就该扛着）；0 = 关闭。
    if (!sPlayerbotAIConfig.selfDefendHealth)
        return false;

    if (ai->IsTank(ai->GetBot()))
        return false;

    if (AI_VALUE2(uint8, "health", "self target") >= sPlayerbotAIConfig.selfDefendHealth)
        return false;

    return AI_VALUE(uint8, "my attacker count") > 0;
}

bool HasLooseMobsTrigger::IsActive()
{
    // [tank 2026-10-19] 只有坦克需要接漏怪；`loose mobs` 值本身已经排除了"坦克已拉住"的怪。
    if (!ai->IsTank(ai->GetBot()))
        return false;

    return !AI_VALUE(std::list<ObjectGuid>, "loose mobs").empty();
}

bool TargetAttackingHealerTrigger::IsActive()
{
    // [tank 2026-10-19 站长口径] 这是**坦克**策略的输入：治疗正在被目标追 ⇒ 立刻接。
    if (!ai->IsTank(ai->GetBot()))
        return false;

    return AI_VALUE(bool, "target attacking healer");
}

bool PanicTrigger::IsActive()
{
    return !ai->IsInPvp() &&
           AI_VALUE2(uint8, "health", "self target") < sPlayerbotAIConfig.criticalHealth &&
		   (!AI_VALUE2(bool, "has mana", "self target") || 
            AI_VALUE2(uint8, "mana", "self target") < sPlayerbotAIConfig.lowMana);
}

bool OutNumberedTrigger::IsActive()
{
    // Don't trigger if the bot is a dungeon or raid
    if (!bot->IsInWorld() || bot->IsBeingTeleported() || bot->GetMap()->IsDungeon() || bot->GetMap()->IsRaid())
        return false;

    // Don't trigger if the bot is in a raid group
    if (bot->GetGroup() && bot->GetGroup()->IsRaidGroup())
        return false;

    // Don't trigger if the bot is in a group with a real player
    if (bot->GetGroup() && ai->HasRealPlayerMaster())
        return false;

    int32 botLevel = bot->GetLevel();
    float healthMod = bot->GetHealthPercent() / 100.0f;
    uint32 friendPower = 100 + 100 * healthMod, foePower = 0;
    for (auto &attacker : ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("possible attack targets")->Get())
    {
        Creature* creature = ai->GetCreature(attacker);
        if (!creature)
            continue;

        if (!creature->CanAttackOnSight(bot))
            continue;

        int32 dLevel = creature->GetLevel() - botLevel;

        healthMod = creature->GetHealthPercent() / 100.0f;

        if(dLevel > -10)
            foePower += std::max(100 + 10 * dLevel, dLevel * 200) * healthMod;
    }

    if (!foePower)
        return false;

    for (auto & helper : ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid> >("nearest friendly players")->Get())
    {
        Unit* player = ai->GetUnit(helper);

        if (!player || player == bot)
            continue;

        int32 dLevel = player->GetLevel() - botLevel;

        healthMod = player->GetHealthPercent() / 100.0f;

        if (dLevel > -10 && sServerFacade.GetDistance2d(bot, player) < 10.0f)
            friendPower += std::max(200 + 20 * dLevel, dLevel * 200)* healthMod;
    }

    return friendPower < foePower;
}

bool BuffTrigger::IsActive()
{
    Unit* target = GetTarget();
	return target && !ai->HasAura(spell, target, false, checkIsOwner) && target->IsAlive();
}

bool MyBuffTrigger::IsActive()
{
    Unit* target = GetTarget();
    return target && !ai->HasMyAura(spell, target);
}

Value<ObjectGuid>* BuffOnPartyTrigger::GetTargetValue()
{
    const std::string qualifier = spell + "-" + (ignoreTanks ? "1" : "0");
	return context->GetValue<ObjectGuid>("friendly unit without aura", qualifier);
}

Value<ObjectGuid>* GreaterBuffOnPartyTrigger::GetTargetValue()
{
    const std::string spells = !lowerSpell.empty() ? spell + "," + lowerSpell : spell;
    const std::string qualifier = spells + "-" + (ignoreTanks ? "1" : "0");
    return context->GetValue<ObjectGuid>("party member without aura", qualifier);
}

Value<ObjectGuid>* MyBuffOnPartyTrigger::GetTargetValue()
{
    const std::string qualifier = spell + "-" + (ignoreTanks ? "1" : "0");
    return context->GetValue<ObjectGuid>("party member without my aura", qualifier);
}

ai::Value<ObjectGuid>* BuffOnTankTrigger::GetTargetValue()
{
    return context->GetValue<ObjectGuid>("party tank without aura", spell);
}

Value<ObjectGuid>* DebuffOnAttackerTrigger::GetTargetValue()
{
	return context->GetValue<ObjectGuid>("attacker without aura", spell);
}

bool NoAttackersTrigger::IsActive()
{
    return !ai->GetUnit(AI_VALUE(ObjectGuid, "current target")) && AI_VALUE(uint8, "my attacker count") > 0;
}

bool InvalidTargetTrigger::IsActive()
{
    return AI_VALUE2(bool, "invalid target", "current target");
}

bool NoTargetTrigger::IsActive()
{
	return !ai->GetUnit(AI_VALUE(ObjectGuid, "current target")) || AI_VALUE2(bool, "invalid target", "current target");
}

bool MyAttackerCountTrigger::IsActive()
{
    return AI_VALUE2(bool, "combat", "self target") && AI_VALUE(uint8, "my attacker count") >= amount;
}

bool HighThreatTrigger::IsActive()
{
    uint8 relativeThreat = AI_VALUE2(uint8, "threat", "current target");

    if (relativeThreat >= 80)
    {
        //Watch delta.
        uint32 lastTime = MEM_AI_VALUE(float, "my threat::current target")->GetLastTime();

        if (lastTime < time(0))
        {
            float deltaThreat = LOG_AI_VALUE(float, "my threat::current target")->GetDelta(5.0f);
            float currentThreat = AI_VALUE2(float, "my threat", "current target");

            if (deltaThreat > 0)
            {
                float tankThreat = AI_VALUE2(float, "tank threat", "current target");

                float newThreat = currentThreat + deltaThreat * 5.0f; //No agro in 5 seconds.

                if (newThreat < tankThreat)
                    return false;
            }
        }

        return true;
    }

    return false;
}

bool MediumThreatTrigger::IsActive()
{
    if (AI_VALUE2(uint8, "threat", "current target") >= 60)
        return true;

    return false;
}

bool SomeThreatTrigger::IsActive()
{
    if (AI_VALUE2(uint8, "threat", "current target") >= 25)
        return true;

    return false;
}

bool NoThreatTrigger::IsActive()
{
    if (SomeThreatTrigger::IsActive())
        return false;

    return true;
}

bool AoeTrigger::IsActive()
{
    std::list<ObjectGuid> aoeEnemies = AoeCountValue::FindMaxDensity(bot, range);
    return aoeEnemies.size() >= amount;
}

bool DebuffTrigger::IsActive()
{
    Unit* target = GetTarget();
    if(target && target->IsAlive())
    {
        if (!ai->HasAura(spell, target, false, checkIsOwner))
        {
            return !HasMaxDebuffs();
        }
    }

    return false;
}

bool DebuffTrigger::HasMaxDebuffs()
{
#ifdef MANGOSBOT_TWO
    return false;
#else
    Unit* target = GetTarget();
    if(target)
    {
#ifdef MANGOSBOT_ONE
        constexpr uint32 debuffLimit = 40;
#else
        constexpr uint32 debuffLimit = 16;
#endif

        return ai->GetAuras(target, false, false).size() >= debuffLimit;
    }
#endif

    return false;
}

bool SpellTrigger::IsActive()
{
	return GetTarget();
}

bool SpellCanBeCastedTrigger::IsActive()
{
	Unit* target = GetTarget();
	return target && ai->CanCastSpell(spell, target, true);
}

bool SpellNoCooldownTrigger::IsActive()
{
    uint32 spellId = AI_VALUE2(uint32, "spell id", name);
    if (!spellId)
        return false;

    return bot->IsSpellReady(spellId);
}

bool RandomTrigger::IsActive()
{
    if (time(0) - lastCheck < sPlayerbotAIConfig.repeatDelay / 1000)
        return false;

    lastCheck = time(0);
    int k = (int)(probability / sPlayerbotAIConfig.randomChangeMultiplier);
    if (k < 1) k = 1;
    return (rand() % k) == 0;
}

bool AndTrigger::IsActive()
{
    std::vector<std::string> tnames = getMultiQualifiers(getQualifier(), ",");

    for (auto tname : tnames)
    {
        Trigger* trigger1 = ai->GetAiObjectContext()->GetTrigger(tname);
        if (!trigger1 || !trigger1->IsActive())
            return false;
    }

    return true;
}

std::string AndTrigger::getName()
{
    std::string name;
    std::vector<std::string> tnames = getMultiQualifiers(getQualifier(), ",");

    for (auto tname : tnames)
    {
        if (!name.empty())
            name += " and ";
        name += tname;
    }

    return name;
}

bool OrTrigger::IsActive()
{
    std::vector<std::string> tnames = getMultiQualifiers(getQualifier(), ",");

    for (auto tname : tnames)
    {
        Trigger* trigger1 = ai->GetAiObjectContext()->GetTrigger(tname);
        if (trigger1 && trigger1->IsActive())
            return true;
    }

    return false;
}

std::string OrTrigger::getName()
{
    std::string name;
    std::vector<std::string> tnames = getMultiQualifiers(getQualifier(), ",");

    for (auto tname : tnames)
    {
        if (!name.empty())
            name += " or ";
        name += tname;
    }

    return name;
}

bool TwoTriggers::IsActive()
{
    if (name1.empty() || name2.empty())
        return false;

    Trigger* trigger1 = ai->GetAiObjectContext()->GetTrigger(name1);
    Trigger* trigger2 = ai->GetAiObjectContext()->GetTrigger(name2);

    if (!trigger1 || !trigger2)
        return false;

    return trigger1->IsActive() && trigger2->IsActive();
}

std::string TwoTriggers::getName()
{
    std::string name;
    name = name1 + " and " + name2;
    return name;
}

bool BoostTrigger::IsActive()
{
    if (ai->IsStateActive(BotState::BOT_STATE_COMBAT) && BuffTrigger::IsActive())
    {
        if (!ai->HasRealPlayerMaster())
        {
            return AI_VALUE(uint8, "balance") <= balance;
        }
        else
        {
            uint32 spellId = AI_VALUE2(uint32, "spell id", spell);
            SpellEntry const* spellInfo = sServerFacade.LookupSpellInfo(spellId);
            // in instances, save long cd boosts for bosses
            if ((bot->GetMap()->IsRaid() || bot->GetMap()->IsDungeon()) && spellInfo && spellInfo->RecoveryTime >= 5 * MINUTE * IN_MILLISECONDS)
            {
                std::list<ObjectGuid> v = context->GetValue<std::list<ObjectGuid>>("possible attack targets")->Get();
                for (std::list<ObjectGuid>::iterator i = v.begin(); i!=v.end(); i++)
                {
                    Unit* unit = ai->GetUnit(*i);
                    if (!unit || !sServerFacade.IsAlive(unit) || unit->IsPlayer())
                        continue;

                    if (sObjectMgr.IsEncounter(unit->GetEntry(), unit->GetMapId()))
                        return true;
                }
            }
            else
                return true;
        }
    }

    return false;
}

bool ItemCountTrigger::IsActive()
{
	return AI_VALUE2(uint32, "item count", item) < uint32(count);
}

bool InterruptSpellTrigger::IsActive()
{
	if (!SpellTrigger::IsActive())
		return false;

	// [2026-10-18 站长口径] "所有威胁等级都需要 1s 内打断高危技能"：
	//   本触发器 checkInterval=1（≈每个 AI tick，200~500ms）⇒ 反应天然 <1s；
	//   但下面原来只扫"可攻击目标"，而副本按波推进 / 威胁等级表过滤后，正在读条的怪可能不在候选里
	//   （还没轮到它、或它不在当前波）⇒ 会漏打断。这里用 `enemy casting interruptible target`
	//   （附近敌对、可打断、正在读条的兜底值，刷新 interval=1）补一条：
	//   **与威胁等级 / 波次无关，谁在高危读条就先打断谁**。
	if (Unit* caster = ai->GetUnit(AI_VALUE(ObjectGuid, "enemy casting interruptible target")))
	{
		if (ai->IsInterruptableSpellCasting(caster, getName()) &&
			sServerFacade.GetDistance2d(ai->GetBot(), caster) <= ai->GetRange("spell"))
		{
			context->GetValue<ObjectGuid>("current target")->Set(caster->GetObjectGuid());
			return true;
		}
	}

	Unit* currentTarget = GetTarget();
	if (currentTarget && ai->IsInterruptableSpellCasting(currentTarget, getName()))
		return true;

	// [interrupt 2026-10-16 站长口径] 原来只检查"当前目标"能否打断 ⇒ 治疗职业（当前目标常是队友）
	// 或主目标是不会施法的近战怪时，永远不打断。这里补一条：从"可攻击目标"里找一个正在施放
	// **可打断法术**的敌人，把它切成当前目标再打断（各职业的打断动作都打"当前目标"）。
	// 注意：不限制"只有有益法术才打断" —— 敌人的伤害吟唱同样要打断。
	if (!context->GetValue<uint32>("spell id", getName())->Get())
		return false;

	Unit* bot = ai->GetBot();
	std::list<ObjectGuid> const attackers = AI_VALUE(std::list<ObjectGuid>, "possible attack targets");
	for (ObjectGuid const& guid : attackers)
	{
		Unit* unit = ai->GetUnit(guid);
		if (!unit || unit == GetTarget())
			continue;

		if (sServerFacade.GetDistance2d(bot, unit) > ai->GetRange("spell"))
			continue;

		if (!ai->IsInterruptableSpellCasting(unit, getName()))
			continue;

		context->GetValue<ObjectGuid>("current target")->Set(guid);
		return true;
	}

	return false;
}

bool DeflectSpellTrigger::IsActive()
{
    Unit* target = GetTarget();
    if (!target)
        return false;

    if (!target->IsNonMeleeSpellCasted(true))
        return false;

    if (!target->HasTarget(bot->GetObjectGuid()))
        return false;

    uint32 spellid = context->GetValue<uint32>("spell id", spell)->Get();
    if (!spellid)
        return false;

    SpellEntry const *deflectSpell = sServerFacade.LookupSpellInfo(spellid);
    if (!deflectSpell)
        return false;

    // warrior deflects all
    if (spell == "spell reflection")
        return true;

    // human priest feedback
    if (spell == "feedback")
        return true;

    SpellSchoolMask deflectSchool = SpellSchoolMask(deflectSpell->EffectMiscValue[0]);
    SpellSchoolMask attackSchool = SPELL_SCHOOL_MASK_NONE;

    Spell* spell = target->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    if (spell)
    {
        SpellEntry const* tarSpellInfo = spell->m_spellInfo;
        if (tarSpellInfo)
        {
            attackSchool = GetSpellSchoolMask(tarSpellInfo);
            if (deflectSchool == attackSchool)
                return true;
        }
    }
    return false;
}

bool HasAuraTrigger::IsActive()
{
   if (!name.empty())
	{
      return ai->HasAura(name, GetTarget(), false, false, -1, false, 0, auraTypeId);
   }

   std::string str = getQualifier();
   std::regex pattern(R"(spellid::(\d+)::([^:]*)::(\d+))");
   std::smatch match;

   if (std::regex_search(str, match, pattern) && match.size() == 4)
   {
      uint32 spell_id = atoi(match[1].str().c_str());

      if (Aura* aura = ai->GetAura(spell_id, GetTarget()))
      {
         uint32 count = atoi(match[3].str().c_str());
         uint32 stack_size = aura->GetStackAmount();
         std::string comp_symb = match[2].str();

         if (comp_symb == "equal")
         {
            return stack_size == count;
         }
         else if (comp_symb == "greater or equal")
         {
            return stack_size >= count;
         }
         else if (comp_symb == "lesser or equal")
         {
            return stack_size <= count;
         }
         else if (comp_symb == "greater")
         {
            return stack_size > count;
         }
         else if (comp_symb == "lesser")
         {
            return stack_size < count;
         }
         else
         {
            return false;
         }
      }
   };

   pattern = R"(spellid::(\d+))";

   if (std::regex_search(str, match, pattern) && match.size() == 2)
   {

      uint32 spell_id = atoi(match[1].str().c_str());
      return ai->HasAura(spell_id, GetTarget());
   }

   return false;
}

std::string HasAuraTrigger::getName()
{
   if (!name.empty())
   {
      return name;
   }

   std::ostringstream ss;
   ss << "has aura with " << getQualifier();
   return ss.str();
}


bool HasNoAuraTrigger::IsActive()
{
    return !ai->HasAura(getName(), GetTarget());
}

// ===========================================================================
// [BOSS 战策略触发器 2026-10-19] 见 GenericTriggers.h 顶部同一段注释。
// ===========================================================================

bool TargetHasAuraTrigger::IsActive()
{
    // 空限定词防崩：绝不 context->GetValue("")->Get()。同时值对象本身也要判空。
    const std::string qualifier = getQualifier();
    if (qualifier.empty())
        return false;

    Value<ObjectGuid>* targetValue = context->GetValue<ObjectGuid>("current target");
    if (!targetValue)
        return false;

    Unit* target = ai->GetUnit(targetValue->Get());
    if (!target || !target->IsInWorld())
        return false;

    // `name::<法术名>` 分支（按名字查，方便规则作者；spellid:: 分支仍走 HasAuraTrigger 原解析）
    static const std::string namePrefix = "name::";
    if (qualifier.size() > namePrefix.size() && qualifier.compare(0, namePrefix.size(), namePrefix) == 0)
    {
        std::string spellName = qualifier.substr(namePrefix.size());
        if (spellName.empty())
            return false;

        return ai->HasAura(spellName, target);
    }

    return HasAuraTrigger::IsActive();
}

std::string TargetHasAuraTrigger::getName()
{
    std::ostringstream ss;
    ss << "target has aura with " << getQualifier();
    return ss.str();
}

bool EnemyCastingTrigger::IsActive()
{
    // [boss-rules 2026-10-19] 允许 `enemy casting::spellid::40425` 这种限定词：
    //   只对指定法术/距离的读条生效；没有限定词时保持原行为。
    Value<ObjectGuid>* value = getQualifier().empty()
        ? context->GetValue<ObjectGuid>(targetValue)
        : context->GetValue<ObjectGuid>(targetValue, getQualifier());
    if (!value)
        return false;

    return !value->Get().IsEmpty();
}

bool TeamMembersCloseTrigger::FindCrowdedMember(PlayerbotAI* ai, const std::string& qualifier, int defaultCount, float defaultDistance, Unit*& outMember)
{
    outMember = nullptr;

    int count = defaultCount;
    float distance = defaultDistance;

    if (!qualifier.empty())
    {
        std::vector<std::string> tokens = Qualified::getMultiQualifiers(qualifier, ",");
        for (std::vector<std::string>::const_iterator i = tokens.begin(); i != tokens.end(); ++i)
        {
            std::string token = *i;
            if (token.empty())
                continue;

            // 裸数字 = 码数（写起来最短：`team member too close::5`）
            if (Qualified::isValidNumberString(token))
            {
                distance = (float)atof(token.c_str());
                continue;
            }

            std::vector<std::string> parts = Qualified::getMultiQualifiers(token, "::");
            if (parts.size() != 2 || !Qualified::isValidNumberString(parts[1]))
                continue;

            if (parts[0] == "dist" || parts[0] == "distance")
                distance = (float)atof(parts[1].c_str());
            else if (parts[0] == "count")
                count = atoi(parts[1].c_str());
        }
    }

    if (distance <= 0.0f || count < 1)
        return false;

    Player* bot = ai->GetBot();
    if (!bot)
        return false;

    int closeCount = 0;
    Unit* closest = nullptr;
    float closestDistance = 0.0f;

    // 只遍历**自己队伍**（最多 40 人），不做任何全图扫描；每个成员一次 2D 距离。
    Group* group = bot->GetGroup();
    if (group)
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->getSource();
            if (!member || member == bot || member->GetMapId() != bot->GetMapId() || !member->IsAlive())
                continue;

            float dist = sServerFacade.GetDistance2d(bot, member);
            if (dist >= distance)
                continue;

            closeCount++;
            if (!closest || dist < closestDistance)
            {
                closest = member;
                closestDistance = dist;
            }
        }
    }

    // 没组队时主号也算队友（单机带 bot 的常见形态）
    Player* master = ai->GetMaster();
    if (master && master != bot && master->GetMapId() == bot->GetMapId() && master->IsAlive() &&
        (!group || !master->IsInGroup(bot)))
    {
        float dist = sServerFacade.GetDistance2d(bot, master);
        if (dist < distance)
        {
            closeCount++;
            if (!closest || dist < closestDistance)
            {
                closest = master;
                closestDistance = dist;
            }
        }
    }

    if (closeCount < count)
        return false;

    outMember = closest;
    return true;
}

bool TeamMembersCloseTrigger::IsActive()
{
    Unit* member = nullptr;
    return FindCrowdedMember(ai, getQualifier(), defaultCount, defaultDistance, member);
}

bool TankAssistTrigger::IsActive()
{
    // [tank-rescue 2026-10-19 站长口径] 坦克自己没挨打、但队友（尤其治疗）被追时也要去救：
    //   原判据只看坦克自己的 attackers ⇒ 治疗被追、坦克没被摸时永远不触发。
    bool anyThreatened = AI_VALUE(bool, "has attackers");
    if (!anyThreatened)
    {
        if (Group* group = ai->GetBot()->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref && !anyThreatened; ref = ref->next())
            {
                Player* member = ref->getSource();
                if (!member || !member->IsInWorld() || !member->IsAlive())
                    continue;
                for (Unit* attacker : member->getAttackers())
                {
                    if (attacker && attacker->IsAlive())
                    {
                        anyThreatened = true;
                        break;
                    }
                }
            }
        }
    }
    if (!anyThreatened)
        return false;

    Unit* currentTarget = ai->GetUnit(AI_VALUE(ObjectGuid, "current target"));
    if (!currentTarget)
        return true;

    // do not switch if enemy target
    Unit* enemy = ai->GetUnit(AI_VALUE(ObjectGuid, "enemy player target"));
    if (enemy)
    {
        return currentTarget != enemy;
    }

    // [pull 2026-10-18 站长口径] 坦克被怪打、而当前目标不是"正在打我那些怪"之一时，允许立刻切目标：
    //   典型就是"正跑去开一个走不到/在另一层的目标，路上被别的怪缠住"——不切的话坦克会一路跑，
    //   路上打它的怪永远不还手。切过去由 TankTargetValue 负责挑（先交战、再近战、再距离近）。
    //   注意：currentTarget 自己就在"打我那些怪"里时不算（正常拉住当前目标，不折腾）；
    //   另外只有 tank assist 真的会挑到别的目标时才切，避免每 tick 空转。
    {
        const std::list<ObjectGuid>& attackers = AI_VALUE(std::list<ObjectGuid>, "attackers targeting me");
        if (!attackers.empty() && std::find(attackers.begin(), attackers.end(), currentTarget->GetObjectGuid()) == attackers.end())
        {
            Unit* switchTarget = ai->GetUnit(AI_VALUE(ObjectGuid, "tank target"));
            if (switchTarget && switchTarget != currentTarget)
                return true;
        }
    }

    Unit* tankTarget = ai->GetUnit(AI_VALUE(ObjectGuid, "tank target"));
    if (!tankTarget || currentTarget == tankTarget)
        return false;
#ifdef CMANGOS
    return tankTarget->GetVictim() != ai->GetUnit(AI_VALUE(ObjectGuid, "self target"));
#endif
#ifdef MANGOS
    return tankTarget->getVictim() != ai->GetUnit(AI_VALUE(ObjectGuid, "self target"));
#endif
}

bool DpsAssistTrigger::IsActive()
{
    if (!AI_VALUE(bool, "has attackers"))
        return false;

    Unit* currentTarget = ai->GetUnit(AI_VALUE(ObjectGuid, "current target"));
    if (!currentTarget)
        return false;

    // If owner is waiting this will trigger attack again to call for pet
    WaitForAttackStrategy* strategy = WaitForAttackStrategy::Get(ai);
    bool isWaitingForAttack = false;
    if (strategy)
        isWaitingForAttack = strategy->ShouldWait(ai); 
        
    Pet* pet = bot->GetPet();
    if (pet)
    {
        UnitAI* creatureAI = ((Creature*)pet)->AI();
        if (creatureAI)
        {
            if (isWaitingForAttack)
                return false;
        }
    }

    return true;
}

bool IsBehindTargetTrigger::IsActive()
{
    Unit* target = ai->GetUnit(AI_VALUE(ObjectGuid, "current target"));
    return target && AI_VALUE2(bool, "behind", "current target");
}

bool IsNotBehindTargetTrigger::IsActive()
{
    Unit* target = ai->GetUnit(AI_VALUE(ObjectGuid, "current target"));
    return target && !AI_VALUE2(bool, "behind", "current target");
}

bool IsNotFacingTargetTrigger::IsActive()
{
    return !AI_VALUE2(bool, "facing", "current target");
}

// [rules 2026-10-17] `hostile aoe incoming`：敌方地面 AOE 正落在自己/队友脚下。
//   判据全在值里（AoeValues.cpp → HostileAoeValue::GetHostileAoeCenter），这里只查一次。
// [dodge 2026-10-17 方案A] 触发器一旦被策略引用就会每 tick 求值 ⇒ 顺手把敌方地面 AOE
//   **登记进 `stored hazards`**，喂给既有的 `move away from hazard`
//   （HazardsValue 只读 stored hazards、自己不检测；原先只有副本预置 BOSS 危险物会写它）。
//   这样"躲技能"= 检测(本触发器) → hazards → MoveAwayFromHazard 走位，全用既有零件。
bool HostileAoeIncomingTrigger::IsActive()
{
    const bool incoming = AI_VALUE(bool, "hostile aoe incoming");

    if (incoming)
        HostileAoeValue::RegisterHostileAoeHazards(ai, true);

    return incoming;
}

// [rules 2026-10-17] `target facing group`：当前目标正面朝着队伍质心。
bool TargetFacingGroupTrigger::IsActive()
{
    // 解析限定词（空限定词走默认值，绝不把空串交给 GetValue）
    float angleDeg = 60.0f;
    float memberRadius = 40.0f;

    std::vector<std::string> tokens = Qualified::getMultiQualifiers(getQualifier(), ",");
    for (const std::string& token : tokens)
    {
        if (token.empty())
            continue;

        size_t pos = token.find("::");
        std::string key = (pos == std::string::npos) ? token : token.substr(0, pos);
        std::string value = (pos == std::string::npos) ? std::string() : token.substr(pos + 2);

        if (key == "dist" && !value.empty())
            memberRadius = (float)atof(value.c_str());
        else if (key == "angle" && !value.empty())
            angleDeg = (float)atof(value.c_str());
    }

    if (angleDeg <= 0.0f || angleDeg > 180.0f)
        angleDeg = 60.0f;
    if (memberRadius <= 0.0f)
        memberRadius = 40.0f;

    return IsTargetFacingGroup(ai, angleDeg, memberRadius);
}

bool TargetFacingGroupTrigger::IsTargetFacingGroup(PlayerbotAI* ai, float angleDeg, float memberRadius)
{
    if (!ai)
        return false;

    Player* bot = ai->GetBot();
    if (!bot || !bot->IsInWorld() || !bot->GetGroup())
        return false;

    // 这是 static 成员函数，不能用 AI_VALUE 宏（宏体会引用成员 `context`）；显式走值接口。
    // 用 ::AiValueOr 而不是 "GetValue(...)->Get()"：后者对未注册的名字会拿到 nullptr 再解引用
    // （文档 §6 记过的 InsideTargetValue 坑，AoeValues.cpp 的 HasAreaDebuffValue 就是那种写法）。
    AiObjectContext* ctx = ai->GetAiObjectContext();
    if (!ctx)
        return false;

    Unit* target = ai->GetUnit(::AiValueOr<ObjectGuid>(ctx, "current target"));
    if (!target || !target->IsInWorld() || !sServerFacade.IsAlive(target))
        return false;

    if (target->GetMapId() != bot->GetMapId())
        return false;

    // 队伍质心（只算同地图、活着、且在 memberRadius 内的成员）
    float sumX = 0.0f, sumY = 0.0f;
    uint32 count = 0;

    for (GroupReference* ref = bot->GetGroup()->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->getSource();
        if (!member || !member->IsInWorld() || !sServerFacade.IsAlive(member))
            continue;

        if (member->GetMapId() != target->GetMapId())
            continue;

        if (target->GetDistance(member) > memberRadius)
            continue;

        sumX += member->GetPositionX();
        sumY += member->GetPositionY();
        ++count;
    }

    // 人太少说明不了"面向人群"（单人也可能是坦克自己）
    if (count < 2)
        return false;

    float centerX = sumX / count;
    float centerY = sumY / count;

    // 目标 → 质心 的方向角，与目标朝向比较
    float dx = centerX - target->GetPositionX();
    float dy = centerY - target->GetPositionY();
    if (fabs(dx) < 0.01f && fabs(dy) < 0.01f)
        return false;

    float toGroup = atan2(dy, dx);
    float diff = fabs(toGroup - target->GetOrientation());

    // 归一化到 [0, PI]
    while (diff > M_PI)
        diff = fabs(diff - 2 * M_PI);

    return diff <= (angleDeg * M_PI / 180.0f);
}

bool HasCcTargetTrigger::IsActive()
{
    uint32 spellid = AI_VALUE2(uint32, "spell id", getName());
    if (spellid && sServerFacade.IsSpellReady(bot, spellid))
    {
        return ai->GetUnit(AI_VALUE(ObjectGuid,"rti cc target"))  || (ai->GetUnit(AI_VALUE2(ObjectGuid, "cc target", getName())) && !ai->GetUnit(AI_VALUE2(ObjectGuid, "current cc target", getName())));
    }

    return false;
}

bool NoMovementTrigger::IsActive()
{
	return !AI_VALUE2(bool, "moving", "self target");
}

bool NoPossibleTargetsTrigger::IsActive()
{
    std::list<ObjectGuid> targets = AI_VALUE(std::list<ObjectGuid>, "possible targets");
    return !targets.size();
}

bool PossibleAddsTrigger::IsActive()
{
    return AI_VALUE(bool, "possible adds") && !AI_VALUE(ObjectGuid, "attack target");
}

bool NotDpsTargetActiveTrigger::IsActive()
{
    Unit* target = ai->GetUnit(AI_VALUE(ObjectGuid, "current target"));
    if (target)
    {
        if (target->IsPlayer())
        {
            return false;
        }

        if(sServerFacade.IsAlive(target))
        {
            // do not switch if enemy target
            Unit* enemy = ai->GetUnit(AI_VALUE(ObjectGuid, "enemy player target"));
            if (enemy)
            {
                return target != enemy;
            }

            Unit* dps = ai->GetUnit(AI_VALUE(ObjectGuid, "dps target"));
            if (dps)
            {
                return target != dps;
            }
        }
    }

    return false;
}

bool NotDpsAoeTargetActiveTrigger::IsActive()
{
    Unit* target = ai->GetUnit(AI_VALUE(ObjectGuid, "current target"));
    if (target)
    {
        if (target->IsPlayer())
        {
            return false;
        }

        if (sServerFacade.IsAlive(target))
        {
            // do not switch if enemy target
            Unit* enemy = ai->GetUnit(AI_VALUE(ObjectGuid, "enemy player target"));
            if (enemy)
            {
                return target != enemy;
            }

            Unit* dps = ai->GetUnit(AI_VALUE(ObjectGuid, "dps aoe target"));
            if (dps)
            {
                return target != dps;
            }
        }
    }

    return false;
}

bool IsSwimmingTrigger::IsActive()
{
    return AI_VALUE2(bool, "swimming", "self target");
}

bool HasNearestAddsTrigger::IsActive()
{
    std::list<ObjectGuid> targets = AI_VALUE(std::list<ObjectGuid>, "nearest adds");
    return targets.size();
}

bool HasItemForSpellTrigger::IsActive()
{
    std::string spell = getName();
    uint32 spellId = AI_VALUE2(uint32, "spell id", spell);
    return spellId && AI_VALUE2(Item*, "item for spell", spellId);
}


bool TargetChangedTrigger::IsActive()
{
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    Unit* oldTarget = ai->GetUnit(context->GetValue<ObjectGuid>("old target")->Get());
    Unit* target = ai->GetUnit(context->GetValue<ObjectGuid>("current target")->Get());
    return target && oldTarget != target;
}

Value<ObjectGuid>* InterruptEnemyHealerTrigger::GetTargetValue()
{
    return context->GetValue<ObjectGuid>("enemy healer target", spell);
}

Value<ObjectGuid>* SnareTargetTrigger::GetTargetValue()
{
    return context->GetValue<ObjectGuid>("snare target", spell);
}

bool StayTimeTrigger::IsActive()
{
    time_t stayTime = AI_VALUE(time_t, "stay time");
    time_t now = time(0);
    return delay && stayTime && now > stayTime + 2 * delay / 1000;
}

bool IsMountedTrigger::IsActive()
{
    return AI_VALUE2(bool, "mounted", "self target");
}

bool CorpseNearTrigger::IsActive()
{
    return bot->GetCorpse() && bot->GetCorpse()->IsWithinDistInMap(bot, CORPSE_RECLAIM_RADIUS, true);
}

bool IsFallingTrigger::IsActive()
{
#ifndef MANGOSBOT_TWO
    return bot->HasMovementFlag(MOVEFLAG_JUMPING);
#else
    return bot->HasMovementFlag(MOVEFLAG_FALLING);
#endif
}

bool IsFallingFarTrigger::IsActive()
{
    return bot->HasMovementFlag(MOVEFLAG_FALLINGFAR);
}

bool HasAreaDebuffTrigger::IsActive()
{
    return AI_VALUE2(bool, "has area debuff", "self target");
}

bool ReturnToStayPositionTrigger::IsActive()
{
    PositionEntry stayPosition = AI_VALUE(PositionMap&, "position")["stay"];
    if (stayPosition.isSet())
    {
        const float distance = bot->GetDistance(stayPosition.x, stayPosition.y, stayPosition.z);
        return distance > ai->GetRange("follow");
    }

    return false;
}

bool ReturnToPullPositionTrigger::IsActive()
{
    PositionEntry pullPosition = AI_VALUE(PositionMap&, "position")["pull"];
    if (pullPosition.isSet())
    {
        const float distance = bot->GetDistance(pullPosition.x, pullPosition.y, pullPosition.z);
        return distance > ai->GetRange("follow");
    }

    return false;
}

bool NoBuffAndComboPointsAvailableTrigger::IsActive()
{
    if (BuffTrigger::IsActive())
    {
        return AI_VALUE2(uint8, "combo", GetComboPointsTargetName()) >= comboPoints;
    }

    return false;
}

bool InPvpTrigger::IsActive()
{
    return ai->IsInPvp();
}

bool InPveTrigger::IsActive()
{
    return ai->IsInPve();
}

bool InRaidFightTrigger::IsActive()
{
    return ai->IsInRaid();
}

bool GreaterBuffOnPartyTrigger::IsActive()
{
    Unit* target = GetTarget();
    return target && bot->IsInGroup(target);
}

bool TargetOfAttacker::IsActive()
{
    return !AI_VALUE(std::list<ObjectGuid>, "attackers targeting me").empty();
}

bool TargetOfAttackerInRange::IsActive()
{
    const Unit* closestAttacker = ai->GetUnit(AI_VALUE(ObjectGuid, "closest attacker targeting me"));
    return closestAttacker && bot->GetDistance(closestAttacker, true, DIST_CALC_COMBAT_REACH) <= (distance - sPlayerbotAIConfig.contactDistance);
}

bool TargetOfCastedAuraTypeTrigger::IsActive()
{
    const std::list<ObjectGuid>& attackers = AI_VALUE(std::list<ObjectGuid>, "attackers");
    for (const ObjectGuid& attackerGuid : attackers)
    {
        // Check against the given creature id
        Unit* attacker = ai->GetUnit(attackerGuid);
        if (attacker)
        {
            const Spell* auraTypeSpell = nullptr;
            const Spell* genericSpell = attacker->GetCurrentSpell(CurrentSpellTypes::CURRENT_GENERIC_SPELL);
            if (genericSpell)
            {
                const SpellEntry* spellInfo = genericSpell->m_spellInfo;
                if (spellInfo)
                {
                    for (int32 i = SpellEffectIndex::EFFECT_INDEX_0; i <= SpellEffectIndex::EFFECT_INDEX_2; i++)
                    {
                        if ((spellInfo->Effect[i] == SPELL_EFFECT_APPLY_AURA) && (spellInfo->EffectApplyAuraName[i] == auraType))
                        {
                            auraTypeSpell = genericSpell;
                            break;
                        }
                    }
                }
            }

            if (!auraTypeSpell)
            {
                const Spell* channeledSpell = attacker->GetCurrentSpell(CurrentSpellTypes::CURRENT_CHANNELED_SPELL);
                if (channeledSpell)
                {
                    const SpellEntry* spellInfo = channeledSpell->m_spellInfo;
                    if (spellInfo)
                    {
                        for (int32 i = SpellEffectIndex::EFFECT_INDEX_0; i <= SpellEffectIndex::EFFECT_INDEX_2; i++)
                        {
                            if ((spellInfo->Effect[i] == SPELL_EFFECT_APPLY_AURA) && (spellInfo->EffectApplyAuraName[i] == auraType))
                            {
                                auraTypeSpell = channeledSpell;
                                break;
                            }
                        }
                    }
                }
            }

            if (auraTypeSpell)
            {
                Unit* spellTarget = auraTypeSpell->m_targets.getUnitTarget();
                if (spellTarget == bot)
                {
                    return true;
                }
            }
        }
    }

    return false;
}

bool BuffOnTargetTrigger::IsActive()
{
    const Unit* target = GetTarget();
    return target && target->HasAura(buffID);
}

bool DispelOnTargetTrigger::IsActive()
{
    Unit* target = GetTarget();
    if (target)
    {
        const uint32 dispelMask = GetDispellMask(dispelType);
        const std::vector<Aura*> auras = ai->GetAuras(target);
        for (const Aura* aura : auras)
        {
            const SpellEntry* spellInfo = aura->GetSpellProto();
            if (spellInfo && ((1 << spellInfo->Dispel) & dispelMask))
            {
                return true;
            }
        }
    }

    return false;
}

bool RtscJumpTrigger::IsActive()
{
    return AI_VALUE2(WorldPosition, "RTSC saved location", "jump point") && AI_VALUE2(WorldPosition, "RTSC saved location", "jump");
}

bool SpellTargetTrigger::IsActive()
{
    if (IsSpellReady())
    {
        // Check for assigned targets
        const std::list<ObjectGuid>& possibleTargets = AI_VALUE(std::list<ObjectGuid>, targetsValue);
        if (!possibleTargets.empty())
        {
            for (const ObjectGuid& possibleTargetGuid : possibleTargets)
            {
                if (IsTargetValid(ai->GetUnit(possibleTargetGuid)))
                {
                    return true;
                }
            }
        }
        else
        {
            // Check for the default target
            if (IsTargetValid(GetTarget()))
            {
                return true;
            }
        }
    }

    return false;
}

bool SpellTargetTrigger::IsTargetValid(Unit* target)
{
    return target &&
           ai->IsSafe(target) &&
           (bot == target || sServerFacade.GetDistance2d(bot, target) < sPlayerbotAIConfig.sightDistance) &&
           (bot->IsInGroup(target)) &&
           (!aliveCheck || !target->IsDead()) &&
           (!auraCheck || !ai->HasAura(spell, target));
}

bool SpellTargetTrigger::IsSpellReady()
{
    uint32 spellId = AI_VALUE2(uint32, "spell id", spell);
    return spellId && bot->IsSpellReady(spellId);
}

bool ItemTargetTrigger::IsTargetValid(Unit* target)
{
    if (SpellTargetTrigger::IsTargetValid(target))
    {
        if (itemAuraCheck)
        {
            const uint32 itemId = GetItemId();
            const ItemPrototype* proto = sObjectMgr.GetItemPrototype(itemId);
            if (proto)
            {
                for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
                {
#ifdef MANGOSBOT_ZERO
                    if (proto->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE || proto->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_NO_DELAY_USE)
#else
                    if (proto->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
#endif
                    {
                        if (proto->Spells[i].SpellId > 0 && ai->HasAura(proto->Spells[i].SpellId, target))
                        {
                            return false;
                        }
                    }
                }

                return true;
            }
        }
        else
        {
            return true;
        }
    }

    return false;
}

bool ItemTargetTrigger::IsSpellReady()
{
    const uint32 itemId = GetItemId();
    if (!ai->HasCheat(BotCheatMask::item) && !bot->HasItemCount(itemId, 1))
        return false;

    const ItemPrototype* proto = sObjectMgr.GetItemPrototype(itemId);
    if (proto)
    {
        for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
        {
#ifdef MANGOSBOT_ZERO
            if (proto->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE || proto->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_NO_DELAY_USE)
#else
            if (proto->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
#endif
            {
                if (proto->Spells[i].SpellId > 0)
                {
                    if (!sServerFacade.IsSpellReady(bot, proto->Spells[i].SpellId) ||
                        !sServerFacade.IsSpellReady(bot, proto->Spells[i].SpellId, itemId))
                    {
                        return false;
                    }
                }
            }
        }

        return true;
    }

    return false;
}

bool AtWarTrigger::IsActive()
{
    ReputationMgr& mgr = bot->GetReputationMgr();

#ifndef MANGOSBOT_ONE
    for (uint32 id = 0; id < sFactionStore.GetNumRows(); ++id)
#else
    for (uint32 id = 0; id < sFactionStore.GetMaxEntry(); ++id)
#endif
    {
#ifndef MANGOSBOT_ONE
        const FactionEntry* factionEntry = sFactionStore.LookupEntry(id);
#else
        const FactionEntry* factionEntry = sFactionStore.LookupEntry<FactionEntry>(id);
#endif

        if (!factionEntry)
            continue;

        FactionState const* repState = mgr.GetState(factionEntry);

        if (!repState)
            continue;

        if (!(repState->Flags & FACTION_FLAG_VISIBLE))
            continue;

        if (repState->Flags & FACTION_FLAG_HIDDEN)
            continue;

        if (repState->Flags & FACTION_FLAG_INVISIBLE_FORCED)
            continue;

        if (repState->Flags & FACTION_FLAG_PEACE_FORCED)
            continue;

        if (!(repState->Flags & FACTION_FLAG_AT_WAR))
            continue;

        if (mgr.GetRank(factionEntry) < REP_HOSTILE)
            continue;

        return true;
    }

    return false;
}