#pragma once
#include "playerbot/strategy/Strategy.h"

namespace ai
{
    // TO DO: Remove this class when no more dependencies
    class CombatStrategy : public Strategy
    {
    public:
        CombatStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        virtual int GetType() override { return STRATEGY_TYPE_COMBAT; }

    protected:
        virtual void InitCombatTriggers(std::list<TriggerNode*>& triggers) override;
    };

    class AvoidAoeStrategy : public Strategy
    {
    public:
        AvoidAoeStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        std::string getName() override { return "avoid aoe"; }

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "avoid aoe"; } //Must equal iternal name
        virtual std::string GetHelpDescription() 
        {
            return "This strategy will make bots move away when they are in aoe.";
        }
        virtual std::vector<std::string> GetRelatedStrategies() { return { }; }
#endif

    private:
        void InitCombatMultipliers(std::list<Multiplier*>& multipliers) override;
        void InitReactionMultipliers(std::list<Multiplier*>& multipliers) override;
        void InitCombatTriggers(std::list<TriggerNode*>& triggers) override;
        void InitReactionTriggers(std::list<TriggerNode*>& triggers) override;
    };

    // [2026-10-22 站长口径] 战斗策略的三个"参数化乘数"（全部走 ai_playerbot_decision_weight，300s 热加载）：
    //   ① aoe cc guard     ：附近有被控目标 ⇒ 压掉 AoE 动作（破控来自 AoE 溅射）
    //   ② reselect penalty ：等坦克仇恨期间压低 "select new target"（避免 assist/重选每 tick 抖动）
    //   ③ pull encounter   ：boss 优先模式下路上有"安全可拉"的怪 ⇒ 抬高 pull 权重（不新增触发器）
    class AoeCcGuardStrategy : public Strategy
    {
    public:
        AoeCcGuardStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        std::string getName() override { return "aoe cc guard"; }
    private:
        void InitCombatMultipliers(std::list<Multiplier*>& multipliers) override;
    };

    class AoeCcGuardMultiplier : public Multiplier
    {
    public:
        AoeCcGuardMultiplier(PlayerbotAI* ai) : Multiplier(ai, "aoe cc guard") {}
        float GetValue(Action* action) override;
    };

    class ReselectPenaltyStrategy : public Strategy
    {
    public:
        ReselectPenaltyStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        std::string getName() override { return "reselect penalty"; }
    private:
        void InitCombatMultipliers(std::list<Multiplier*>& multipliers) override;
    };

    class ReselectPenaltyMultiplier : public Multiplier
    {
    public:
        ReselectPenaltyMultiplier(PlayerbotAI* ai) : Multiplier(ai, "reselect penalty") {}
        float GetValue(Action* action) override;
    };

    class PullEncounterBoostStrategy : public Strategy
    {
    public:
        PullEncounterBoostStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        std::string getName() override { return "pull encounter boost"; }
    private:
        void InitCombatMultipliers(std::list<Multiplier*>& multipliers) override;
    };

    class PullEncounterBoostMultiplier : public Multiplier
    {
    public:
        PullEncounterBoostMultiplier(PlayerbotAI* ai) : Multiplier(ai, "pull encounter boost") {}
        float GetValue(Action* action) override;
    };

    class AvoidSpecificCreaturesStrategy : public Strategy
    {
    public:
        AvoidSpecificCreaturesStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        std::string getName() override { return "avoid specific creatures"; }

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "avoid specific creatures"; } //Must equal iternal name
        virtual std::string GetHelpDescription() 
        {
            return "This strategy will make bots move away from specific creatures.";
        }
        virtual std::vector<std::string> GetRelatedStrategies() { return { }; }
#endif

    private:
        void InitCombatMultipliers(std::list<Multiplier*>& multipliers) override;
        void InitReactionMultipliers(std::list<Multiplier*>& multipliers) override;
        void InitCombatTriggers(std::list<TriggerNode*>& triggers) override;
        void InitReactionTriggers(std::list<TriggerNode*>& triggers) override;
    };

    class AvoidAoeStrategyMultiplier : public Multiplier
    {
    public:
        AvoidAoeStrategyMultiplier(PlayerbotAI* ai) : Multiplier(ai, "run away on area debuff") {}
        float GetValue(Action* action) override;
        
#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "run away on area debuff"; } //Must equal iternal name
        virtual std::string GetHelpDescription() {
            return "This stops bots from casting certain (cast-time) spells when affected by an area debuf.";
        }
        virtual std::vector<std::string> GetRelatedStrategies() { return {}; }
#endif
    };

    class AvoidSpecificCreaturesStrategyMultiplier : public Multiplier
    {
    public:
        AvoidSpecificCreaturesStrategyMultiplier(PlayerbotAI* ai) : Multiplier(ai, "avoid specific creatures") {}
        float GetValue(Action* action) { return 1.0f; }
        
#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "avoid specific creatures" } //Must equal iternal name
        virtual std::string GetHelpDescription() {
            return "This makes bots run away from specific creatures.";
        }
        virtual std::vector<std::string> GetRelatedStrategies() { return {}; }
#endif
    };

    class WaitForAttackStrategy : public Strategy
    {
    public:
        WaitForAttackStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        static WaitForAttackStrategy* Get(PlayerbotAI* ai);
        std::string getName() override { return "wait for attack"; }

        static bool ShouldWait(PlayerbotAI* ai);
        static uint8 GetWaitTime(PlayerbotAI* ai);
        // [threat 2026-10-16 站长口径] 等坦克时"保持的安全距离"改为独立配置（默认 15 码），
        // 不再借用 fleeDistance（那是逃跑距离，改动会连带影响逃跑行为）。
        static float GetSafeDistance() { return sPlayerbotAIConfig.waitForAttackSafeDistance; }
        static float GetSafeDistanceThreshold() { return 2.5f; }
        ReactStates GetPetReactState() const { return petReactState; }
        void SetPetReactState(ReactStates reactState) { petReactState = reactState; }

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "wait for attack"; } //Must equal iternal name
        virtual std::string GetHelpDescription() {
            return "This strategy will make bots wait a specified time before attacking.";
        }
        virtual std::vector<std::string> GetRelatedStrategies() { return { }; }
#endif

    private:
        void InitCombatTriggers(std::list<TriggerNode*>& triggers) override;
        void InitCombatMultipliers(std::list<Multiplier*>& multipliers) override;

    private:
        ReactStates petReactState;
    };

    class WaitForAttackMultiplier : public Multiplier
    {
    public:
        WaitForAttackMultiplier(PlayerbotAI* ai) : Multiplier(ai, "wait for for attack") {}
        float GetValue(Action* action) override;
    };

    class HealInterruptStrategy : public Strategy
    {
    public:
        HealInterruptStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        std::string getName() override { return "heal interrupt"; }

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "heal interrupt"; } //Must equal iternal name
        virtual std::string GetHelpDescription()
        {
            return "This strategy will make the bot interrupt the heal it currently casts if target is at full health";
        }
#endif

    private:
        void InitCombatTriggers(std::list<TriggerNode*>& triggers) override;
        void InitReactionTriggers(std::list<TriggerNode*>& triggers) override;
    };

    class PreHealStrategy : public Strategy
    {
    public:
        PreHealStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        std::string getName() override { return "preheal"; }

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "preheal"; } //Must equal iternal name
        virtual std::string GetHelpDescription()
        {
            return "This strategy will make the bot calculate melee damage of attacker when deciding how to heal target";
        }
#endif
    };

    // [pull-gate 2026-10-19 站长口径] "坦克乱开怪"：
    //   还没进战斗时，附近敌对数量达到 AiPlayerbot.PullHostileLimit 就不主动开怪（默认 3 只 / 15 码）。
    //   只影响"开怪"这一刻：一旦已经进战斗就不再生效，避免战斗中反复被压住。
    //   与 wait for attack 的区别：那条只看"仇恨/坦克有没有 pull back"，这条看"这一堆怪能不能碰"。
    class PullGateStrategy : public Strategy
    {
    public:
        PullGateStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        std::string getName() override { return "pull gate"; }

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "pull gate"; }
        virtual std::string GetHelpDescription()
        {
            return "This strategy stops the bot from starting a fight when too many hostiles are nearby (AI cannot fight a whole pack).";
        }
#endif

    private:
        void InitCombatTriggers(std::list<TriggerNode*>& triggers) override;
    };

    // [defend-self 2026-10-19 站长口径] "一直挨打也不知道跑"。
    //   与 kite 的区别（站长指出复用 kite 是错的）：
    //     · kite 的判据是 `has aggro`，单刷时几乎恒真，挂上去会让 bot 见到怪就跑 —— 那是**风筝**战术；
    //     · 这条只管"我不该扛却被打到残血"：非坦克 + 正在被近战打 + 血量 < AiPlayerbot.SelfDefendHealth，
    //       才允许一次脱离近战（动作复用现有的 `flee`，受 AiPlayerbot.FleeingEnabled 约束）。
    class DefendSelfStrategy : public Strategy
    {
    public:
        DefendSelfStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        std::string getName() override { return "defend self"; }

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "defend self"; }
        virtual std::string GetHelpDescription()
        {
            return "This strategy makes a non-tank bot step out of melee when it is being hit and low health.";
        }
#endif

    private:
        void InitCombatTriggers(std::list<TriggerNode*>& triggers) override;
    };

    // [tank 2026-10-19 站长口径] 坦克救场：把"正在打队友、坦克没仇恨"的怪接过来。
    //   站长原话："缺的是「选中它并嘲讽」，如果嘲讽 cd 也可以做其他高仇恨技能"
    //   ⇒ 动作 `pick up loose mob` 内部：换成该目标 → 试遍本职业嘲讽技；嘲讽都在 CD 就返回 false，
    //     让普通仇恨循环去打这个新目标。
    class TankPickUpStrategy : public Strategy
    {
    public:
        TankPickUpStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        std::string getName() override { return "tank pick up"; }

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "tank pick up"; }
        virtual std::string GetHelpDescription()
        {
            return "This strategy makes the tank grab mobs that are hitting party members but have no tank threat.";
        }
#endif

    private:
        void InitCombatTriggers(std::list<TriggerNode*>& triggers) override;
    };
}

