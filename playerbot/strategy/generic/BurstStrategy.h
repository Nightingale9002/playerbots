#pragma once
#include "playerbot/strategy/Strategy.h"

namespace ai
{
    // [2026-10-18 站长口径] "爆发拆成独立策略，每个 DPS 职业都挂上；爆发重叠输出会更高"
    //   独立于基础循环，只在"战斗中 + 目标有效"时按优先级开爆发：
    //     - 法师：冰冷血脉 / 燃烧 / 奥术强化 / 气定神闲
    //     - 盗贼：冲动 / 剑刃乱舞 / 冷血
    //     - 战士：死亡之愿 / 鲁莽
    //     - 猎人：急速射击
    //     - 萨满：嗜血 / 英勇（团队爆发）
    //     - 骑士：复仇之怒
    //     - 牧师：暗影魔 / 内省
    //     - 术士：暗影灼烧
    //   触发器与动作都复用各职业已有的实现（只在这里统一挂载），不重复造轮子。
    class BurstStrategy : public Strategy
    {
    public:
        BurstStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        std::string getName() override { return "burst"; }

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "burst"; }
        virtual std::string GetHelpDescription()
        {
            return "Uses class burst cooldowns (Icy Veins, Adrenaline Rush, Bloodlust, ...) on cooldown in combat.";
        }
        virtual std::vector<std::string> GetRelatedStrategies() { return { "dps assist" }; }
#endif

    private:
        void InitCombatTriggers(std::list<TriggerNode*>& triggers) override;
    };
}
