
#include "playerbot/playerbot.h"
#include "CcTargetValue.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/strategy/Action.h"
#include "playerbot/strategy/values/RtiTargetValue.h"
#include "playerbot/strategy/values/PossibleAttackTargetsValue.h"
#include "playerbot/DecisionWeights.h"

using namespace ai;

class FindTargetForCcStrategy : public FindTargetStrategy
{
public:
    FindTargetForCcStrategy(PlayerbotAI* ai, std::string spell) : FindTargetStrategy(ai)
    {
        this->spell = spell;
        maxDistance = sPlayerbotAIConfig.sightDistance;
    }

public:
    virtual void CheckAttacker(Unit* creature, ThreatManager* threatManager)
    {
        Player* bot = ai->GetBot();

        AiObjectContext* context = ai->GetAiObjectContext();

        if (ai->GetUnit(AI_VALUE(ObjectGuid,"rti cc target")) && ai->GetUnit(AI_VALUE(ObjectGuid,"rti cc target"))->GetObjectGuid() == creature->GetObjectGuid())
        {
            result = creature;
            return;
        }

        // [2026-10-18 站长口径] "把星星标记作为控制标记，和骷髅头一致"：
        //   星星（队伍标记 index 0）是**人工指定的控制目标**，优先级等同于"rti cc target"。
        if (PossibleAttackTargetsValue::IsCcMarked(creature, bot))
        {
            result = creature;
            return;
        }

        if (AI_VALUE(ObjectGuid,"current target") == creature->GetObjectGuid())
            return;

        if (AI_VALUE(ObjectGuid,"rti target") == creature->GetObjectGuid())
            return;

        uint8 health = creature->GetHealthPercent();
        if (health < sPlayerbotAIConfig.mediumHealth)
            return;

        // [2026-10-18 站长口径] 不再需要"离坦克最近"的距离基准（见下面按输出排序）
        if (!bot->GetGroup())
            return;

        if (AI_VALUE(uint8,"aoe count") > 2)
        {
            WorldLocation aoe = AI_VALUE(WorldLocation,"aoe position");
            if (sServerFacade.IsDistanceLessOrEqualThan(sServerFacade.GetDistance2d(creature, aoe.coord_x, aoe.coord_y), sPlayerbotAIConfig.aoeRadius))
                return;
        }

        if (creature->HasAuraType(SPELL_AURA_PERIODIC_DAMAGE) && !(spell == "fear" || spell == "banish"))
            return;

        if (!ai->CanCastSpell(spell, creature, true, nullptr, false, true))
            return;

        // If we have rti cc none but have cc strategy, then we'll cc something we're able to
        std::string rti = AI_VALUE(std::string, "rti cc");
        int index = RtiTargetValue::GetRtiIndex(rti);
        if (index == -1 && ai->HasStrategy("cc", BotState::BOT_STATE_COMBAT))
        {
            // [2026-10-18 站长口径] "控制不一定要给到高优先级的怪物，而是应该给到
            //   正常情况输出最高的怪物" ⇒ 按**普通输出能力**排序（近战/远程 DPS 取大者），
            //   而不是"离坦克最近"或威胁等级表：
            //     · 高优先级怪通常是"要打断/要驱散"的，控它反而浪费一次控制；
            //     · 一个不需要打断、但平砍/射击很疼的怪，控住它才是最大的减伤。
            float dps = 0.0f;
            Creature* ccCreature = dynamic_cast<Creature*>(creature);
            CreatureInfo const* info = ccCreature ? ccCreature->GetCreatureInfo() : nullptr;
            if (info)
            {
                float meleeDps = 0.0f;
                float rangedDps = 0.0f;
                if (info->MeleeBaseAttackTime > 0)
                    meleeDps = (info->MinMeleeDmg + info->MaxMeleeDmg) * 500.0f / float(info->MeleeBaseAttackTime);
                if (info->RangedBaseAttackTime > 0)
                    rangedDps = (info->MinRangedDmg + info->MaxRangedDmg) * 500.0f / float(info->RangedBaseAttackTime);
                dps = std::max(meleeDps, rangedDps);
            }

            // 没有近战/远程数据（纯法系）：用等级做保守估计，避免"读条最该被控的法系"永远排不上
            if (dps <= 0.01f)
                dps = float(creature->GetLevel()) * 1.5f;

            // [2026-10-18 站长口径] **威胁等级 4 = 需要优先控制**：
            //   只要场上有表里优先级 4 的怪且我们能控，就先控它（输出高低只作同级 tie-break），
            //   之后再按"输出最高"补其它控制。
            uint8 const prio = PossibleAttackTargetsValue::PriorityRankOf(creature);
            bool const needsControl = (prio == 4);

            // [决策参数化 2026-10-18] 控制目标打分（权重表可离线调）：
            //   优先级4（需要优先控制）加成 + 普通输出 + 威胁等级 + 距离惩罚
            Player* bot = ai->GetBot();
            float score = dw::Get("cc.needs_control") * (needsControl ? 1.0f : 0.0f)
                        + dw::Get("cc.dps") * dps
                        + dw::Get("cc.priority_rank") * float(prio)
                        + dw::Get("cc.distance") * (bot ? bot->GetDistance(creature) : 0.0f);

            if (!result || score > bestScore)
            {
                result = creature;
                bestDps = dps;
                bestNeedsControl = needsControl;
                bestScore = score;
            }
        }
    }

private:
    std::string spell;
    float maxDistance;
    float bestDps = 0.0f;
    float bestScore = -1e18f;
    bool bestNeedsControl = false;
};

ObjectGuid CcTargetValue::Calculate()
{
    std::list<ObjectGuid> possible = AI_VALUE(std::list<ObjectGuid>,"possible targets no los");

    for (std::list<ObjectGuid>::iterator i = possible.begin(); i != possible.end(); ++i)
    {
        ObjectGuid guid = *i;
        Unit* add = ai->GetUnit(guid);
        if (!add)
            continue;

        // [2026-10-18 站长口径] "bot 在副本中会优先击杀被控制的队友。这是错的。"
        //   被魅惑/精神控制的队友是敌对状态（IsSafe=false），但他**不是怪**：
        //   正确做法是把他当**控制目标**（控住等魅惑结束），而不是当伤害目标。
        Player* bot = ai->GetBot();
        bool const charmedTeammate = add->IsPlayer() && bot &&
            bot->IsInGroup(static_cast<Player*>(add)) && add->HasCharmer();

        if (!ai->IsSafe(add) && !charmedTeammate)
            continue;

        if (ai->HasMyAura(qualifier, add))
            return ObjectGuid();

        if (qualifier == "polymorph")
        {
            if (ai->HasMyAura("polymorph: pig", add))
                return ObjectGuid();
            if (ai->HasMyAura("polymorph: turtle", add))
                return ObjectGuid();
        }
    }

    FindTargetForCcStrategy strategy(ai, qualifier);
    Unit* target = FindTarget(&strategy);
    return target ? target->GetObjectGuid() : ObjectGuid();
}
