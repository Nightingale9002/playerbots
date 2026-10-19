#pragma once
#include "playerbot/strategy/Value.h"

namespace ai
{
    class AoePositionValue : public CalculatedValue<WorldLocation>
	{
	public:
        AoePositionValue(PlayerbotAI* ai) : CalculatedValue<WorldLocation>(ai, "aoe position") {}

	public:
        virtual WorldLocation Calculate();
    };

    class AoeCountValue : public CalculatedValue<uint8>
	{
	public:
        AoeCountValue(PlayerbotAI* ai) : CalculatedValue<uint8>(ai, "aoe count") {}

	public:
        static std::list<ObjectGuid> FindMaxDensity(Player* bot, float range = 100.0f);
        virtual uint8 Calculate() override;
    };

    class HasAreaDebuffValue : public BoolCalculatedValue, public Qualified
    {
    public:
        HasAreaDebuffValue(PlayerbotAI* ai) : BoolCalculatedValue(ai), Qualified() {}

        Unit* GetTarget()
        {
            AiObjectContext* ctx = AiObject::context;

            return ai->GetUnit(ctx->GetValue<ObjectGuid>(qualifier)->Get());
        }
        virtual bool Calculate() override;
    };

    // [rules 2026-10-17] `hostile aoe incoming` —— **敌方地面 AOE 正在落在自己/队友脚下**。
    //
    // 与 `HasAreaDebuffValue`（`avoid aoe` 用的那个）的区别：
    //   HasAreaDebuffValue 判的是"**这个 AOE 是否已经影响到我的当前目标**"（IsAffecting(target)）；
    //   本值判的是"**我方某人此刻正站在敌方 AOE 的落点半径内**" —— 也就是"脚下有地板"，
    //   这正是攻略里"躲开脚下的地板/AOE 区"要的东西，而原来只有副本专用 hazard 列表能表达。
    //
    // 判据（每条都便宜、且只扫一次"最近动态对象"）：对 `nearest dynamic objects no los` 里每个动态对象：
    //   ① 它不是我方放的（`go->GetCaster()` 敌对）—— 治疗/增益型地面法术不会被误判；
    //   ② 法术不是正面效果（`IsPositiveEffect`），排除正气的奉献/治疗之泉那类；
    //   ③ 半径内有人（自己或队友）；返回**最近的那个人**，方便调用方决定往哪边挪。
    //
    // 限定词（逗号分隔，可省略）：
    //   `dist::<码数>`   把"最近动态对象"的搜索半径收紧（默认用 farDistance）
    //   `include self::0` 不把自己算进去（只看队友）
    class HostileAoeValue : public BoolCalculatedValue, public Qualified
    {
    public:
        HostileAoeValue(PlayerbotAI* ai) : BoolCalculatedValue(ai), Qualified() {}

        virtual bool Calculate() override;

        // 命中时返回被覆盖的那个人（自己或队友），未命中返回 nullptr。
        // 给"往反方向挪"的动作复用（动作侧不必再扫一遍）。
        static Unit* FindCoveredMember(PlayerbotAI* ai, bool includeSelf = true);

        // 命中时返回该敌方 AOE 的中心（用于决定往哪边躲），未命中返回 false。
        static bool GetHostileAoeCenter(PlayerbotAI* ai, bool includeSelf, WorldLocation& outCenter, Unit*& outVictim);

        // [dodge 2026-10-17 方案A] 把所有"敌方地面 AOE"登记成 `stored hazards`，
        //   让既有的 `move away from hazard`（`DungeonActions.h → MoveAwayFromHazard`）真的躲开它们。
        //
        // 为什么需要：`HazardsValue::Calculate()` **只读** `stored hazards`、自己不做任何检测；
        //   而 `stored hazards` 原先只有 `DungeonTriggers.cpp → CloseToHazardTrigger` 会写，
        //   那是**按预置 BOSS 危险物 GUID** 的（只有熔火之心那类在用）⇒ 通用副本里 hazards 恒为空，
        //   "躲技能"实际不存在。这里把动态对象（地板 AOE）接进同一条链：
        //   敌方动态对象 → stored hazards → hazards → MoveAwayFromHazard → 走位躲开。
        //
        // 与 `GetHostileAoeCenter()` 的分工：那个只返回**第一个**覆盖队友的、用于布尔判定；
        //   本方法**枚举所有**敌方 AOE 圆圈（不去重覆盖判定），覆盖多个同时存在的地板技。
        // 返回登记了几处。
        static uint32 RegisterHostileAoeHazards(PlayerbotAI* ai, bool includeSelf = true);
    };
}
