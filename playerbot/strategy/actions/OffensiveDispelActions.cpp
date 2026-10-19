#include "playerbot/playerbot.h"
#include "OffensiveDispelActions.h"
#include "Spells/SpellAuras.h"
#include "playerbot/DecisionWeights.h"
#include "playerbot/strategy/values/PossibleAttackTargetsValue.h"

#include "playerbot/strategy/values/AttackersValue.h"

using namespace ai;

namespace
{
    // 职业 → 进攻驱散法术名（与各职业 ActionContext 里注册的名字一致）
    char const* DispelSpellForClass(uint8 cls)
    {
        switch (cls)
        {
            case CLASS_PRIEST: return "dispel magic";
            case CLASS_MAGE:   return "spellsteal";
            case CLASS_SHAMAN: return "purge";
            case CLASS_HUNTER: return "tranquilizing shot";
            default:           return nullptr;
        }
    }

    // 单位身上是否有"可被魔法驱散的增益"
    bool HasMagicBuff(Unit* unit)
    {
        if (!unit || !unit->IsInWorld() || !unit->IsAlive())
            return false;

        for (auto const& entry : unit->GetSpellAuraHolderMap())
        {
            SpellAuraHolder* holder = entry.second;
            if (!holder)
                continue;

            SpellEntry const* spell = holder->GetSpellProto();
            if (!spell || !(spell->Dispel & DISPEL_MAGIC))
                continue;

            if (holder->IsPositive())
                return true;
        }
        return false;
    }
}

Unit* ai::FindDispelEnemy(PlayerbotAI* ai, std::string& spellName, bool requireCastable)
{
    spellName.clear();
    if (!ai)
        return nullptr;

    Player* bot = ai->GetBot();
    if (!bot)
        return nullptr;

    char const* name = DispelSpellForClass(bot->getClass());
    if (!name)
        return nullptr;

    spellName = name;

    // [2026-10-18] 只挑"正在打我们/我们在打"的敌人（possible attack targets 已过滤可达/可攻击/控制等）
    Value<std::list<ObjectGuid>>* value = ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("possible attack targets");
    if (!value)
        return nullptr;

    std::list<ObjectGuid> targets = value->Get();

    // [决策参数化 2026-10-18] 进攻驱散目标打分：增益数量 + 读条 + 威胁等级 - 距离
    Unit* best = nullptr;
    float bestScore = -1e18f;
    for (std::list<ObjectGuid>::const_iterator i = targets.begin(); i != targets.end(); ++i)
    {
        Unit* unit = ai->GetUnit(*i);
        if (!unit || !HasMagicBuff(unit))
            continue;

        if (requireCastable && !ai->CanCastSpell(spellName, unit, EFFECT_MASK_ALL))
            continue;

        int buffCount = 0;
        for (auto const& entry : unit->GetSpellAuraHolderMap())
        {
            SpellAuraHolder* holder = entry.second;
            if (!holder) continue;
            SpellEntry const* spell = holder->GetSpellProto();
            if (spell && (spell->Dispel & DISPEL_MAGIC) && holder->IsPositive())
                ++buffCount;
        }

        float score = dw::Get("dispel.buff_count") * float(buffCount)
                    + dw::Get("dispel.priority_rank") * float(PossibleAttackTargetsValue::PriorityRankOf(unit))
                    + dw::Get("dispel.distance") * bot->GetDistance(unit);
        if (unit->IsNonMeleeSpellCasted(true))
            score += dw::Get("dispel.casting");

        if (score > bestScore)
        {
            bestScore = score;
            best = unit;
        }
    }

    return best;
}

bool DispelEnemyAction::isPossible()
{
    std::string spellName;
    return ai::FindDispelEnemy(ai, spellName) != nullptr;
}

bool DispelEnemyAction::Execute(Event& event)
{
    std::string spellName;
    Unit* target = ai::FindDispelEnemy(ai, spellName, true);
    if (!target)
        return false;

    return ai->CastSpell(spellName, target);
}
