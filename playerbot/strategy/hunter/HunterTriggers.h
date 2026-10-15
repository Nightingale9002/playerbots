#pragma once

#include "playerbot/strategy/triggers/GenericTriggers.h"

namespace ai
{
    // [bm hunter 2026-10-17 站长口径] AOE 前先确认**技能真能打到的那一簇**里没有"受伤害会脱离控制"
    // 的怪（可破控），否则多射/乱射/爆炸陷阱会把被控制的怪打醒。判据复用
    // PossibleAttackTargetsValue::HasBreakableCC，范围用"多目标选择"的同一份判断
    // （AoeCountValue::FindMaxDensity），即挑出来的那一簇。
    //
    // 早期版本是"40 码内有没有被控的怪"——但这三个技能的实际覆盖都远小于 40 码
    // （多射=前方锥形 3 目标、乱射=落点 8 码圈、爆炸陷阱=落点一圈），
    // 40 码扫描会把隔着半张地图的一只羊也算进来，导致技能整段被禁用。
    bool HunterHasBreakableCCInAoeCluster(Player* bot, float range);

    // 取法术的实际施放距离（取不到时用 fallback）。用来把上面的破控判据限制在技能真能覆盖的范围。
    float HunterSpellRange(PlayerbotAI* ai, std::string spellName, float fallback);

    // [bm hunter 2026-10-17 站长口径] 平射禁飞区：离下一发平射不到 0.5 秒时**不开始读条**
    // （稳固/瞄准这类读条会把这一发平射顶到后面，等于白丢一发）。只看"正要打出"的窗口
    // （timer > 0），所以不会在"没有平射在跑"的时候把读条锁死。
    bool HunterRangedSwingTooSoon(Player* bot);

// [hunter-weave 2026-10-18 站长口径] **平射穿插**：只在"刚打出一发平射"的窗口里放瞬发技能。
//   为什么：TBC 里任何远程特殊技能都会把平射的挥砍计时重置成"武器速度"⇒ 机器人每个 GCD 都放瞬发时，
//   平射计时永远重置、**一发平射也打不出去**（站长："不会在技能中间穿插平射"）。
//   判据：`getAttackTimer(RANGED_ATTACK)` 刚被重置（接近 `GetAttackTime()`）⇒ 放技能不吃亏；
//   还在等平射（剩余时间少）⇒ 先让这一发打出去。没有平射在跑（timer=0）⇒ 不拦。
bool HunterShouldLetAutoShotLand(Player* bot);

    HAS_AURA_TRIGGER_TIME(FeignDeathTrigger, "feign death", 2);

    BEGIN_TRIGGER(HunterNoStingsActiveTrigger, Trigger)
    END_TRIGGER()

    class AspectOfTheHawkTrigger : public BuffTrigger
    {
    public:
        AspectOfTheHawkTrigger(PlayerbotAI* ai) : BuffTrigger(ai, "aspect of the hawk") {}
    };

    class AspectOfTheWildTrigger : public BuffTrigger
    {
    public:
        AspectOfTheWildTrigger(PlayerbotAI* ai) : BuffTrigger(ai, "aspect of the wild") {}
    };

    class AspectOfTheViperTrigger : public BuffTrigger
    {
    public:
        AspectOfTheViperTrigger(PlayerbotAI* ai) : BuffTrigger(ai, "aspect of the viper") {}
    };

    class AspectOfThePackTrigger : public BuffTrigger
    {
    public:
        AspectOfThePackTrigger(PlayerbotAI* ai) : BuffTrigger(ai, "aspect of the pack") {}
    };

    class AspectOfTheMonkeyTrigger : public BuffTrigger
    {
    public:
        AspectOfTheMonkeyTrigger(PlayerbotAI* ai) : BuffTrigger(ai, "aspect of the monkey") {}
    };

    class AspectOfTheBeastTrigger : public BuffTrigger
    {
    public:
        AspectOfTheBeastTrigger(PlayerbotAI* ai) : BuffTrigger(ai, "aspect of the beast") {}
    };

    class AspectOfTheCheetahTrigger : public BuffTrigger
    {
    public:
        AspectOfTheCheetahTrigger(PlayerbotAI* ai) : BuffTrigger(ai, "aspect of the cheetah") {}
    };

    class AspectOfTheDragonhawkTrigger : public BuffTrigger
    {
    public:
        AspectOfTheDragonhawkTrigger(PlayerbotAI* ai) : BuffTrigger(ai, "aspect of the dragonhawk") {}
    
        bool IsActive() override
        {
            return BuffTrigger::IsActive() && !ai->HasAura("aspect of the hawk", bot);
        }
    };

    BEGIN_TRIGGER(HuntersPetDeadTrigger, Trigger)
    END_TRIGGER()

    BEGIN_TRIGGER(HuntersPetLowHealthTrigger, Trigger)
    END_TRIGGER()

    class BlackArrowTrigger : public DebuffTrigger
    {
    public:
        BlackArrowTrigger(PlayerbotAI* ai) : DebuffTrigger(ai, "black arrow") {}
    };

    SNARE_TRIGGER(BlackArrowSnareTrigger, "black arrow");

    class HuntersMarkTrigger : public DebuffTrigger
    {
    public:
        HuntersMarkTrigger(PlayerbotAI* ai) : DebuffTrigger(ai, "hunter's mark") {}
        virtual bool IsActive() override;
    };

    class FreezingTrapTrigger : public HasCcTargetTrigger
    {
    public:
        FreezingTrapTrigger(PlayerbotAI* ai) : HasCcTargetTrigger(ai, "freezing trap") {}

#ifdef MANGOSBOT_ZERO
        bool IsActive() override
        {
            // Check if feign death not on cooldown
            if (sServerFacade.IsSpellReady(bot, 5384))
            {
                return HasCcTargetTrigger::IsActive();
            }

            return false;
        }
#endif
    };

    class FrostTrapTrigger : public MeleeLightAoeTrigger
    {
    public:
        FrostTrapTrigger(PlayerbotAI* ai, std::string spell = "frost trap") : MeleeLightAoeTrigger(ai)
        {
            spellId = AI_VALUE2(uint32, "spell id", spell);
        }

        bool IsActive() override
        {
#ifdef MANGOSBOT_ZERO
            // Check if feign death not on cooldown
            if (!sServerFacade.IsSpellReady(bot, 5384))
            {
                return false;
            }
#endif

            return sServerFacade.IsSpellReady(bot, spellId) && MeleeLightAoeTrigger::IsActive();
        }

    private:
        uint32 spellId;
    };

    // [bm hunter 2026-10-17 站长口径] 爆炸陷阱只在≥7 目标时放（原来 3），
    // 且"多目标选择"那一簇里没有破控怪（范围 = 陷阱的实际施放距离，不再是 40 码）。
    class ExplosiveTrapTrigger : public AoeTrigger
    {
    public:
        ExplosiveTrapTrigger(PlayerbotAI* ai, std::string spell = "explosive trap") : AoeTrigger(ai, 7)
        {
            spellId = AI_VALUE2(uint32, "spell id", spell);
        }

        bool IsActive() override
        {
#ifdef MANGOSBOT_ZERO
            // Check if feign death not on cooldown
            if (!sServerFacade.IsSpellReady(bot, 5384))
            {
                return false;
            }
#endif

            return sServerFacade.IsSpellReady(bot, spellId) && AoeTrigger::IsActive() &&
                !HunterHasBreakableCCInAoeCluster(bot, HunterSpellRange(ai, "explosive trap", 30.0f));
        }

    private:
        uint32 spellId;
    };

    // [bm hunter 2026-10-17 站长口径] 乱射只在≥10 目标、且爆炸陷阱与多射都在冷却时放；
    // 破控判据同多射（只看"多目标选择"那一簇，范围 = 乱射的实际施放距离）。
    class HunterVolleyTrigger : public AoeTrigger
    {
    public:
        HunterVolleyTrigger(PlayerbotAI* ai) : AoeTrigger(ai, 10)
        {
            multishotId = AI_VALUE2(uint32, "spell id", "multi-shot");
            explosiveTrapId = AI_VALUE2(uint32, "spell id", "explosive trap");
        }

        bool IsActive() override
        {
            if (!AoeTrigger::IsActive())
                return false;

            if (HunterHasBreakableCCInAoeCluster(bot, HunterSpellRange(ai, "volley", 30.0f)))
                return false;

            return !sServerFacade.IsSpellReady(bot, multishotId) && !sServerFacade.IsSpellReady(bot, explosiveTrapId);
        }

    private:
        uint32 multishotId;
        uint32 explosiveTrapId;
    };

    // [bm hunter 2026-10-17 站长口径] 守护阈值 20/35：<20 切蝰蛇，>35 切回鹰（中间不横跳）。
    class AspectOfTheViperLowManaTrigger : public Trigger
    {
    public:
        AspectOfTheViperLowManaTrigger(PlayerbotAI* ai) : Trigger(ai, "aspect of the viper low mana") {}

        bool IsActive() override
        {
            return bot->HasSpell(34074) && AI_VALUE2(uint8, "mana", "self target") < 20 &&
                !ai->HasAura("aspect of the viper", bot);
        }
    };

    class AspectOfTheHawkManaOkTrigger : public Trigger
    {
    public:
        AspectOfTheHawkManaOkTrigger(PlayerbotAI* ai) : Trigger(ai, "aspect of the hawk mana ok") {}

        bool IsActive() override
        {
            return AI_VALUE2(uint8, "mana", "self target") >= 35 && !ai->HasAura("aspect of the hawk", bot);
        }
    };

    class RapidFireTrigger : public BuffTrigger
    {
    public:
        RapidFireTrigger(PlayerbotAI* ai) : BuffTrigger(ai, "rapid fire") {}
    };

    class TrueshotAuraTrigger : public BuffTrigger
    {
    public:
        TrueshotAuraTrigger(PlayerbotAI* ai) : BuffTrigger(ai, "trueshot aura") {}
    };

    class SerpentStingOnAttackerTrigger : public DebuffOnAttackerTrigger
    {
    public:
        SerpentStingOnAttackerTrigger(PlayerbotAI* ai) : DebuffOnAttackerTrigger(ai, "serpent sting") {}
        virtual bool IsActive() override;
    };

    class ViperStingOnAttackerTrigger : public DebuffOnAttackerTrigger
    {
    public:
        ViperStingOnAttackerTrigger(PlayerbotAI* ai) : DebuffOnAttackerTrigger(ai, "viper sting") {}
        virtual bool IsActive() override;
    };

    BEGIN_TRIGGER(HunterPetNotHappy, Trigger)
    END_TRIGGER()

    class ConsussiveShotSnareTrigger : public SnareTargetTrigger
    {
    public:
        ConsussiveShotSnareTrigger(PlayerbotAI* ai) : SnareTargetTrigger(ai, "concussive shot") {}
    };

    SNARE_TRIGGER(ScatterShotSnareTrigger, "scatter shot");

    class ScareBeastTrigger : public HasCcTargetTrigger
    {
    public:
        ScareBeastTrigger(PlayerbotAI* ai) : HasCcTargetTrigger(ai, "scare beast") {}
    };

    class HunterLowAmmoTrigger : public AmmoCountTrigger
    {
    public:
        HunterLowAmmoTrigger(PlayerbotAI* ai) : AmmoCountTrigger(ai, "ammo", 1, 30) {}
        virtual bool IsActive() override { return bot->GetGroup() && (AI_VALUE2(uint32, "item count", "ammo") < 100) && (AI_VALUE2(uint32, "item count", "ammo") > 0); }
    };

    class HunterNoAmmoTrigger : public AmmoCountTrigger
{
public:
    HunterNoAmmoTrigger(PlayerbotAI* ai)
        : AmmoCountTrigger(ai, "ammo", 1, 10), lastCheckTime(0)
    {
    }

    virtual bool IsActive() override
    {
        uint32 now = time(nullptr);

        // Only check every 3 seconds to avoid spamming
        if (now - lastCheckTime < 3)
            return false;

        lastCheckTime = now;

        uint32 ammoId = bot->GetUInt32Value(PLAYER_AMMO_ID);

        // No ammo equipped at all
        if (ammoId == 0)
            return AI_VALUE2(uint32, "item count", "ammo") > 0;

        // Check if we still have THIS ammo in inventory
        uint32 count = AI_VALUE2(uint32, "item count", std::to_string(ammoId));

        // If equipped ammo is gone, but we have other ammo → trigger
        return count == 0 && AI_VALUE2(uint32, "item count", "ammo") > 0;
    }

private:
    time_t lastCheckTime;
};
    class HunterHasAmmoTrigger : public AmmoCountTrigger
    {
    public:
        HunterHasAmmoTrigger(PlayerbotAI* ai) : AmmoCountTrigger(ai, "ammo", 1, 10) {}
        virtual bool IsActive() override { return !AmmoCountTrigger::IsActive(); }
    };

    class SwitchToRangedTrigger : public Trigger
    {
    public:
        SwitchToRangedTrigger(PlayerbotAI* ai) : Trigger(ai, "switch to ranged", 1) {}

        bool IsActive() override
        {
#ifdef MANGOSBOT_ZERO
            bool hasAmmo = ai->HasCheat(BotCheatMask::item) || AI_VALUE2(uint32, "item count", "ammo");
#else
            bool hasAmmo = ai->HasCheat(BotCheatMask::item) || bot->HasAura(46699) || AI_VALUE2(uint32, "item count", "ammo");
#endif
            if (!hasAmmo)
                return false;

            Unit* target = ai->GetUnit(AI_VALUE(ObjectGuid, "current target"));
            float distance = AI_VALUE2(float, "distance", "current target");
            return target && ai->HasStrategy("close", BotState::BOT_STATE_COMBAT) &&
                (target->GetVictim() != bot ||
                target->IsImmobilizedState() ||
                (target->GetSpeed(MOVE_RUN) <= (bot->GetSpeed(MOVE_RUN) / 2) && !((!bot->GetPet() || bot->GetPet()->IsDead()) && target->IsCreature() && target->GetHealthPercent() < 50.f && target->GetHealth() < bot->GetHealth())) ||
                distance > 8.0f);
        }
    };

    class SwitchToMeleeTrigger : public Trigger
    {
    public:
        SwitchToMeleeTrigger(PlayerbotAI* ai) : Trigger(ai, "switch to melee", 1) {}

        bool IsActive() override
        {
#ifdef MANGOSBOT_ZERO
            bool hasAmmo = ai->HasCheat(BotCheatMask::item) || AI_VALUE2(uint32, "item count", "ammo");
#else
            bool hasAmmo = ai->HasCheat(BotCheatMask::item) || bot->HasAura(46699) || AI_VALUE2(uint32, "item count", "ammo");
#endif
            if (!hasAmmo)
                return true;

            Unit* target = ai->GetUnit(AI_VALUE(ObjectGuid, "current target"));
            return target && ((target->GetSpeed(MOVE_RUN) > (bot->GetSpeed(MOVE_RUN) / 2)) || ((!bot->GetPet() || bot->GetPet()->IsDead()) && target->GetHealthPercent() < 50.f && target->IsCreature() && target->GetHealth() < bot->GetHealth())) &&
                !target->IsImmobilizedState() &&
                ai->HasStrategy("ranged", BotState::BOT_STATE_COMBAT) &&
                target->GetVictim() == bot &&
                sServerFacade.IsDistanceLessOrEqualThan(AI_VALUE2(float, "distance", "current target"), 8.0f);
        }
    };

    CAN_CAST_TRIGGER(ChimeraShotCanCastTrigger, "chimera shot");
    CAN_CAST_TRIGGER(ExplosiveShotCanCastTrigger, "explosive shot");
    CAN_CAST_TRIGGER(MultishotCanCastTrigger, "multi-shot");
    CAN_CAST_TRIGGER(SteadyShotCanCastTrigger, "steady shot");
    CAN_CAST_TRIGGER(ArcaneShotTrigger, "arcane shot");
#ifdef MANGOSBOT_TWO
    BOOST_TRIGGER(KillCommandBoostTrigger, "kill command");
#else
    CAN_CAST_TRIGGER(KillCommandBoostTrigger, "kill command");
#endif
    SNARE_TRIGGER(IntimidationSnareTrigger, "intimidation");
    CAN_CAST_TRIGGER(CounterattackCanCastTrigger, "counterattack");
    SNARE_TRIGGER(WybernStingSnareTrigger, "wyvern sting");
    CAN_CAST_TRIGGER(MongooseBiteCastTrigger, "mongoose bite");
    BOOST_TRIGGER(BestialWrathBoostTrigger, "bestial wrath");

    // [bm hunter 2026-10-17 站长口径] 多射卡CD（单体也打），但"多目标选择"的那一簇里有破控怪时不打。
    class HunterMultishotCanCastTrigger : public MultishotCanCastTrigger
    {
    public:
        HunterMultishotCanCastTrigger(PlayerbotAI* ai) : MultishotCanCastTrigger(ai) {}

        bool IsActive() override
        {
            if (!MultishotCanCastTrigger::IsActive())
                return false;

            float maxRange = 35.0f, minRange = 0.0f;
            if (!ai->GetSpellRange("multi-shot", &maxRange, &minRange) || maxRange <= 0.0f)
                maxRange = 35.0f;

            return !HunterHasBreakableCCInAoeCluster(bot, maxRange);
        }
    };

    // [bm hunter 2026-10-17 站长口径] 读条技能（稳固）要让开平射：离下一发平射 <0.5 秒时不开始读条。
    class HunterSteadyShotCanCastTrigger : public SteadyShotCanCastTrigger
    {
    public:
        HunterSteadyShotCanCastTrigger(PlayerbotAI* ai) : SteadyShotCanCastTrigger(ai) {}

        bool IsActive() override
        {
            return SteadyShotCanCastTrigger::IsActive() && !HunterRangedSwingTooSoon(bot);
        }
    };

    INTERRUPT_TRIGGER(SilencingShotInterruptTrigger, "silencing shot");
    INTERRUPT_HEALER_TRIGGER(SilencingShotInterruptHealerTrigger, "silencing shot");

    class ViperStingTrigger : public DebuffTrigger
    {
    public:
        ViperStingTrigger(PlayerbotAI* ai) : DebuffTrigger(ai, "viper sting") {}

        virtual bool IsActive() override
        {
            return DebuffTrigger::IsActive() && AI_VALUE2(uint8, "mana", "current target") >= 10;
        }
    };

    class AimedShotTrigger : public Trigger
    {
    public:
        AimedShotTrigger(PlayerbotAI* ai) : Trigger(ai, "aimed shot", 2) {}
        virtual std::string GetTargetName() override { return "current target"; }

        virtual bool IsActive() override
        {
            if (!bot->HasSpell(19434) || !bot->IsSpellReady(19434))
                return false;

            // [bm hunter 2026-10-17 站长口径] 瞄准是长读条，离下一发平射 <0.5 秒时不开始读。
            if (HunterRangedSwingTooSoon(bot))
                return false;

            Unit* target = GetTarget();
            if (!target)
                return false;

            float distanceTo = AI_VALUE2(float, "distance", GetTargetName());
            if (target->GetSelectionGuid() != bot->GetObjectGuid() && sServerFacade.IsDistanceGreaterOrEqualThan(distanceTo, 8.0f))
                return true;

            // victim
            if (target->GetSelectionGuid() == bot->GetObjectGuid())
            {
                if (sServerFacade.IsDistanceGreaterOrEqualThan(distanceTo, 15.0f))
                    return true;
            }
            return false;
        }
    };

    class HunterNoPet : public Trigger 
    {
    public:
        HunterNoPet(PlayerbotAI* ai) : Trigger(ai, "no pet", 1) {}
        virtual bool IsActive() override
        {
            if (AI_VALUE2(bool, "mounted", "self target"))
            return false;

            if (bot->GetPetGuid())
            return false;

            if (ai->CanCastSpell("call pet", bot, 0))
            return false;

            return ai->CanCastSpell("tame beast", bot, 0);
        }
    };

    class StealthedNearbyTrigger : public Trigger 
    {
    public:
        StealthedNearbyTrigger(PlayerbotAI* ai) : Trigger(ai, "stealthed nearby", 5) {}
        virtual bool IsActive() override
        {
            if (!bot->HasSpell(1543))
                return false;

            Unit* target = ai->GetUnit(AI_VALUE(ObjectGuid, "nearest stealthed unit"));
            return target;
        }
    };
}

