#pragma once
#include "playerbot/strategy/Value.h"

// [感知层 2026-10-19] 把核心里已经存在的地形/位置数据包成 AI 可用的值。
//
// 背景（站长口径）："感知层现在更大的问题是不能识别地形和怪物位置等信息"。
//   · 躲地板的优先级早就够了（ACTION_EMERGENCY+5 = 95，见 DungeonStrategy.cpp），
//     真正缺的是"能看见"——bot 只能看见 DynamicObject 型地板，看不见墙、水面、坡、
//     也数不清身边有几只怪 ⇒ 拉怪、追人、施法、选落点全都靠感觉。
// 本文件只做"感知"（只读世界状态），不含任何决策；改优先级另有其事。
//
// 约定：
//   · 每个值都是**只读**、**不做世界写操作**（不移动、不施法、不召唤）。
//   · 贵的值必须靠 CalculatedValue 的 checkInterval 缓存（见 Value.h 顶部的说明：
//     `checkInterval=1` 等于不缓存，`=2` 约 1 秒一次，`=4` 约 2 秒一次）。

namespace ai
{
    // ---------------------------------------------------------------------------------------
    // 周围 R 码内的敌对存活单位数量。
    //   限定词：半径（码），默认 10；非法值 / 空限定词 ⇒ 默认值。
    //   为什么需要：拉怪前判断会不会群拉；躲地板选落点时避开怪堆。
    //   判据用 AnyUnfriendlyUnitInObjectRangeCheck（核心既有）：存活 + 可攻击 + 距离 + LOS，
    //   一次 Cell::VisitAllObjects 只走与半径相交的格子，不做全图扫描。
    // ---------------------------------------------------------------------------------------
    class NearbyHostilesValue : public Uint8CalculatedValue, public Qualified
    {
    public:
        NearbyHostilesValue(PlayerbotAI* ai) : Uint8CalculatedValue(ai, "nearby hostiles", 2), Qualified() {}

        virtual uint8 Calculate() override;
    };

    // ---------------------------------------------------------------------------------------
    // 我和当前目标之间被墙/地形挡住（= 没有视线）。
    //   数据源：vmap 射线（WorldObject::IsWithinLOSInMap），一次查询，便宜。
    //   为什么需要：隔墙不许施法/不许开怪 —— 现在 bot 只要距离够就会硬读条。
    //   没有目标 ⇒ false（"没有墙"），调用方自己判断要不要用。
    // ---------------------------------------------------------------------------------------
    class WallBetweenValue : public BoolCalculatedValue
    {
    public:
        WallBetweenValue(PlayerbotAI* ai) : BoolCalculatedValue(ai, "wall between me and target", 2) {}

        virtual bool Calculate() override;
    };

    // ---------------------------------------------------------------------------------------
    // 目标"走得到"（mmap 寻路成功）。
    //   数据源：PathFinder::calculate + getPathType；PATHFIND_NOPATH ⇒ 不可达。
    //   代价：一次寻路是**毫秒级**，而且本仓库的寻路会按需载格（PathFinder.cpp 的
    //     EnsureGridLoadedForPath）⇒ 绝不能每 tick 算。三重限制：
    //       1. checkInterval=4 ⇒ 约 2 秒一次；
    //       2. 只对 60 码内的目标算（追/不追只看近处，远处由 travel 逻辑负责）；
    //       3. 同图同实例才算（跨图寻路没有意义）。
    //   没有目标 / 超距 / 跨图 ⇒ true（"当作可达"，宁可不影响原有行为）。
    // ---------------------------------------------------------------------------------------
    class TargetReachableValue : public BoolCalculatedValue
    {
    public:
        TargetReachableValue(PlayerbotAI* ai) : BoolCalculatedValue(ai, "target reachable", 4) {}

        virtual bool Calculate() override;
    };

    // ---------------------------------------------------------------------------------------
    // 脚下是不是水。
    //   数据源：WorldObject::IsInWater（核心既有）。
    //   为什么需要：水里不能施法/不能站桩，被恐惧/击退掉水里要立刻上岸。
    // ---------------------------------------------------------------------------------------
    class StandingInWaterValue : public BoolCalculatedValue
    {
    public:
        StandingInWaterValue(PlayerbotAI* ai) : BoolCalculatedValue(ai, "standing in water", 2) {}

        virtual bool Calculate() override;
    };

    // ---------------------------------------------------------------------------------------
    // 我 → 当前目标的高差（目标 z − 我的 z，单位码）。
    //   正数 = 目标在上方（要爬/要跳），负数 = 目标在下方（可能跳崖）。
    //   为什么需要：追人时"看着近其实隔一层楼"，跳崖前要能看出落差。
    //   没有目标 ⇒ 0.0f。
    // ---------------------------------------------------------------------------------------
    class SlopeToTargetValue : public FloatCalculatedValue
    {
    public:
        SlopeToTargetValue(PlayerbotAI* ai) : FloatCalculatedValue(ai, "slope to target", 2) {}

        virtual float Calculate() override;
    };
}
