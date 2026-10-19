#pragma once
#include "playerbot/strategy/Value.h"

// [关系/统计谓词 2026-10-19] 感知层的第二类：不回答"现在是什么"，而回答
//   "谁在打谁 / 排第几 / 按现在的趋势接下来会怎样"。
//
// 为什么要这一层（站长口径："问题的关键是怎么把复杂的战斗场景落到触发+动作上"）：
//   `Strategy.h` 的优先级表只做"谓词 → 动作"，没有流程概念。所以复杂场景必须被**降维成
//   一行判据**：谁在打谁、会不会 OT、有几只漏怪 —— 而不是给每个场景写一个 if。
//
// 设计约束（都是刻意的）：
//   · 只读：不改世界状态、不施法、不移动；
//   · 带时间窗：需要"趋势"的用 `CalculatedValue` 的 checkInterval 缓存（=2 约 1 秒）；
//   · 没有坦克 / 没有队伍 ⇒ 一律返回"不拦"，不能让纯机器人小队/野外无法开打。

namespace ai
{
    // -------------------------------------------------------------------------------------
    // 预期仇恨：自己当前仇恨 + "再来一发大概会涨多少"。
    //   "一发增量"用自己仇恨历史的**窗口内单次最大正增量**估计（不需要 DBC 伤害公式，
    //   也不依赖具体职业/技能；缺点是第一发之前没有样本，此时增量为 0 = 不拦）。
    //   限定词：与 `my threat` 同一套（通常是 "current target"）。
    // -------------------------------------------------------------------------------------
    class PredictedThreatValue : public FloatCalculatedValue, public Qualified
    {
    public:
        PredictedThreatValue(PlayerbotAI* ai) : FloatCalculatedValue(ai, "predicted threat", 2), Qualified() {}

        virtual float Calculate() override;

    private:
        struct Probe
        {
            time_t time;
            float threat;
        };

        std::map<ObjectGuid, std::list<Probe>> history;
    };

    // -------------------------------------------------------------------------------------
    // 打完这一发会不会超过坦克仇恨。
    //   false 的两种情况（都是刻意的）：
    //     ① 没有坦克 / 坦克仇恨 <= 0 ⇒ 交给 `threat` 的百分比规则去压，这里不重复拦；
    //     ② 还在攒样本 ⇒ 不拦。
    // -------------------------------------------------------------------------------------
    class WillOveraggroValue : public BoolCalculatedValue, public Qualified
    {
    public:
        WillOveraggroValue(PlayerbotAI* ai) : BoolCalculatedValue(ai, "will overaggro", 2), Qualified() {}

        virtual bool Calculate() override;
    };

    // -------------------------------------------------------------------------------------
    // 漏怪：正在打队友、但坦克**还没有仇恨**的敌对（坦克救场的输入）。
    //   排序：按"受害者血量百分比升序" —— 快死的队友先救。
    //   只认 `possible attack targets`（已经过距离/视线/可攻击性筛选），不另做全图扫描。
    // -------------------------------------------------------------------------------------
    class LooseMobsValue : public ObjectGuidListCalculatedValue
    {
    public:
        LooseMobsValue(PlayerbotAI* ai) : ObjectGuidListCalculatedValue(ai, "loose mobs", 2) {}

        virtual std::list<ObjectGuid> Calculate() override;
    };

    // -------------------------------------------------------------------------------------
    // 当前目标是不是在打队伍里的治疗。
    //   用途：DPS 集火优先级（打治疗 = 优先集火），以及治疗自保的输入。
    // -------------------------------------------------------------------------------------
    class TargetAttackingHealerValue : public BoolCalculatedValue
    {
    public:
        TargetAttackingHealerValue(PlayerbotAI* ai) : BoolCalculatedValue(ai, "target attacking healer", 2) {}

        virtual bool Calculate() override;
    };
}
