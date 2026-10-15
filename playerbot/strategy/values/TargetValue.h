#pragma once
#include "playerbot/strategy//AiObject.h"
#include "playerbot/strategy/Value.h"
#include "playerbot/TravelMgr.h"

namespace ai
{
    class FindTargetStrategy
    {
    public:
        FindTargetStrategy(PlayerbotAI* ai)
        {
            result = NULL;
            this->ai = ai;
        }

    public:
        Unit* GetResult() { return result; }

    public:
        virtual void CheckAttacker(Unit* attacker, ThreatManager* threatManager) = 0;
        void GetPlayerCount(Unit* creature, int* tankCount, int* dpsCount);

    protected:
        Unit* result;
        PlayerbotAI* ai;

    protected:
        std::map<Unit*, int> tankCountCache;
        std::map<Unit*, int> dpsCountCache;
    };

    class FindNonCcTargetStrategy : public FindTargetStrategy
    {
    public:
        FindNonCcTargetStrategy(PlayerbotAI* ai) : FindTargetStrategy(ai) {}

    protected:
        virtual bool IsCcTarget(Unit* attacker);

    };

    class TargetValue : public UnitCalculatedValue
	{
	public:
        TargetValue(PlayerbotAI* ai, std::string name = "target", int checkInterval = 1) : UnitCalculatedValue(ai, name, checkInterval) {}
#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "target"; } //Must equal iternal name
        virtual std::string GetHelpTypeName() { return "target"; }
        virtual std::string GetHelpDescription() { return "This value contains a [h:object|unit] which is the best target based on the current target strategy."; }
        virtual std::vector<std::string> GetUsedValues() { return { "possible attack targets" }; }
#endif 
    protected:
        Unit* FindTarget(FindTargetStrategy* strategy);
    };

    class RpgTargetValue : public ManualSetValue<GuidPosition>
    {
    public:
        RpgTargetValue(PlayerbotAI* ai, std::string name = "rpg target") : ManualSetValue<GuidPosition>(ai, GuidPosition(), name) {}
#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "rpg target"; } //Must equal iternal name
        virtual std::string GetHelpTypeName() { return "rpg"; }
        virtual std::string GetHelpDescription() { return "This value contains the [h:object|objectGuid] of a [h:object|unit] or [h:object|gameObject] to move to and rpg with.\nThis value is manually set."; }
        virtual std::vector<std::string> GetUsedValues() { return {}; }
#endif 

        virtual std::string Format() override;
    };

    class TravelTargetValue : public ManualSetValue<TravelTarget*>
    {
    public:
        TravelTargetValue(PlayerbotAI* ai, std::string name = "travel target") : ManualSetValue<TravelTarget*>(ai, new TravelTarget(ai), name) {}
        virtual ~TravelTargetValue() override { delete value; }
    };	

    class LeaderTravelTargetValue : public CalculatedValue<TravelTarget*>
    {
    public:
        LeaderTravelTargetValue(PlayerbotAI* ai, std::string name = "leader travel target") : CalculatedValue<TravelTarget*>(ai, name) {};

        virtual TravelTarget* Calculate() override;
    };

    class IgnoreRpgTargetValue : public ManualSetValue<std::set<ObjectGuid>& >
    {
    public:
        IgnoreRpgTargetValue(PlayerbotAI* ai) : ManualSetValue<std::set<ObjectGuid>& >(ai, data, "ignore rpg targets") {}

    private:
        std::set<ObjectGuid> data;
    };

    class TalkTargetValue : public ManualSetValue<ObjectGuid>
    {
    public:
        TalkTargetValue(PlayerbotAI* ai, std::string name = "talk target") : ManualSetValue<ObjectGuid>(ai, ObjectGuid(), name) {}
    };

    class AttackTargetValue : public ManualSetValue<ObjectGuid>
    {
    public:
        AttackTargetValue(PlayerbotAI* ai, std::string name = "attack target") : ManualSetValue<ObjectGuid>(ai, ObjectGuid(), name) {}
    };

    class PullTargetValue : public UnitManualSetValue
    {
    public:
        PullTargetValue(PlayerbotAI* ai, std::string name = "pull target") : UnitManualSetValue(ai, ObjectGuid(), name) {}
        void Set(ObjectGuid unitGuid) override;
        ObjectGuid Get() override;

    private:
        ObjectGuid guid;
    };

    class FollowTargetValue : public UnitCalculatedValue
    {
    public:
        FollowTargetValue(PlayerbotAI* ai, std::string name = "follow target") : UnitCalculatedValue(ai, name) {}
        ObjectGuid Calculate() override;
    };

    class ManualFollowTargetValue : public GuidPositionManualSetValue
    {
    public:
        ManualFollowTargetValue(PlayerbotAI* ai, std::string name = "manual follow target") : GuidPositionManualSetValue(ai, GuidPosition(), name) {}
    };

    class ClosestAttackerTargetingMeTargetValue : public UnitCalculatedValue
    {
    public:
        ClosestAttackerTargetingMeTargetValue(PlayerbotAI* ai, std::string name = "closest attacker targeting me") : UnitCalculatedValue(ai, name) {}
        ObjectGuid Calculate() override;
    };

    // ===========================================================================
    // [BOSS 战策略 2026-10-19] `enemy casting target` / `enemy casting interruptible target`
    //   附近**敌对且正在施放非近战法术**的单位，优先 current target，其次取最近的。
    //   为什么需要：原来只有卡拉赞等副本专用触发器，打断全靠各职业策略自己判断
    //   ⇒ 规则行写不出"指定某只怪在读条就打断/控场"。
    //   限定词（逗号分隔，可省略）：
    //     dist::<码数>       扫描半径（默认 30 码；只接受正数）
    //     spellid::<id>      只认这一个法术 id
    //   性能（每条都是刻意做的）：
    //     · 先看 current target —— 命中就直接返回，**不做范围扫描**（最热路径）；
    //     · 范围扫描用 Cell::VisitAllObjects(range) —— 只走与半径相交的格子，不是全图扫描；
    //     · 缓存 = ObjectGuidCalculatedValue 的 checkInterval=2：引擎侧
    //       CalculatedValue::Get 的 `now - lastCheckTime >= checkInterval/2` ⇒ 约每 1 秒最多重算一次。
    //       （注意 checkInterval=1 等于**不缓存**：第三个判据 `>= checkInterval/2 == 0` 恒真。）
    //     · 空限定词 / 半径非法 ⇒ 用默认值或直接返回空 guid，绝不去 GetValue("")。
    class EnemyCastingTargetValue : public ObjectGuidCalculatedValue, public Qualified
    {
    public:
        EnemyCastingTargetValue(PlayerbotAI* ai, std::string name = "enemy casting target", bool interruptibleOnly = false) :
            ObjectGuidCalculatedValue(ai, name, 2), Qualified(), interruptibleOnly(interruptibleOnly) {}

        virtual ObjectGuid Calculate() override;

    private:
        bool interruptibleOnly;
    };

    class FriendlyManualTargetsValue : public ManualSetValue<std::list<ObjectGuid>>
    {
    public:
        FriendlyManualTargetsValue(PlayerbotAI* ai) : ManualSetValue<std::list<ObjectGuid>>(ai, {}) {}
        std::list<ObjectGuid> Get() override;
        std::list<ObjectGuid> LazyGet() override;
    };

    class FocusHealTargetValue : public FriendlyManualTargetsValue
    {
    public:
        FocusHealTargetValue(PlayerbotAI* ai) : FriendlyManualTargetsValue(ai) {}
    };

    class BoostTargetsValue : public FriendlyManualTargetsValue
    {
    public:
        BoostTargetsValue(PlayerbotAI* ai) : FriendlyManualTargetsValue(ai) {}
    };

    class ReviveTargetsValue : public FriendlyManualTargetsValue
    {
    public:
        ReviveTargetsValue(PlayerbotAI* ai) : FriendlyManualTargetsValue(ai) {}
    };

    class BuffTargetsValue : public FriendlyManualTargetsValue
    {
    public:
        BuffTargetsValue(PlayerbotAI* ai) : FriendlyManualTargetsValue(ai) {}
    };

    class ClosestEntryValue : public CalculatedValue<GuidPosition>,
                                    public Qualified
    {
    public:
        ClosestEntryValue(PlayerbotAI* ai, std::string name = "closest entry") : CalculatedValue<GuidPosition>(ai, name), Qualified() {}
        GuidPosition Calculate() override;
    };
}