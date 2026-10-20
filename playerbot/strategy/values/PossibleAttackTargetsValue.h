#pragma once
#include "playerbot/strategy/Value.h"
#include "TargetValue.h"
#include "NearestUnitsValue.h"

namespace ai
{
    // List of hostile targets that are in combat with the bot (or bot group) and can be attacked (is reachable, not cc'ed, etc...)
    class PossibleAttackTargetsValue : public ObjectGuidListCalculatedValue, public Qualified
	{
	public:
        PossibleAttackTargetsValue(PlayerbotAI* ai) : ObjectGuidListCalculatedValue(ai, "possible attack targets", 2), Qualified() {}
        std::list<ObjectGuid> Calculate() override;
        static bool IsValid(Unit* target, Player* player, float range = sPlayerbotAIConfig.sightDistance, bool ignoreCC = false, bool checkAttackerValid = true);
        static bool IsPossibleTarget(Unit* target, Player* player, float range, bool ignoreCC);
        static bool HasBreakableCC(Unit* target, Player* player);
        static bool HasUnBreakableCC(Unit* target, Player* player);

        // [pull 2026-10-18 站长口径] 坦克开怪目标的"同一层 + 可达"判据（多楼层副本防选到另一层的怪）：
        //   便宜判据：同图/同副本实例 + |Δz| ≤ AiPlayerbot.PullSameLevelZDistance（+ requireLos 时的 LOS）；
        //   贵判据：2D 距离超过 AiPlayerbot.PullPathCheckDistance（或没 LOS 但要走过去）时，
        //           做一次 navmesh 单次寻路（结果缓存 + 每线程每秒限流，见 .cpp）。
        //   requireLos = false 用于"丢掉已经拉住的目标"的场合（战斗中暂时被墙挡不该丢目标）。
        static bool CanReachTarget(Unit* target, Player* bot, bool requireLos = true);
        // [pull 2026-10-18 站长口径] 坦克拿怪排序用：这只怪算不算"近战怪"（优先拉）。
        //   判据：正在吟唱非近战法术 ⇒ 远程/法系；否则看它有没有贴身打它的目标；否则回落到
        //   creature_template 的"只有远程伤害/远程攻击间隔"静态数据。无法判定时按近战算。
        static bool IsMeleeAttacker(Unit* target);
        // [aggro 2026-10-17 站长口径] 共享的"交战"判定：这只怪是否已经在打人（自己/队友/盟友）。
        //   坦克与 DPS 两条选目标链都用它当第一判据，避免两处口径漂移。
        static bool IsEngagedAttacker(Unit* target, Player* bot);
        // [aggro 2026-10-17 站长口径] "威胁更高的怪"判据（站长："实际上更应该强调的是高威胁技能，
        //   比如加血、击晕、召唤等，其中有些是可以打断的，我们可以用'有高威胁技能'作为高威胁怪物的
        //   判断规则"）：
        //     HasHighThreatAbility()   这个怪**会不会**高威胁技能 —— 查 creature_template.spells[4]
        //                              静态技能表（治疗/击晕/定身/恐惧/召唤/高额伤害），当前施法兜底。
        //                              是**怪的属性**，不是瞬时状态 ⇒ 不会"一会儿高一会儿低"。
        //     IsHealerCaster()         此刻正在施放治疗 ⇒ 更紧急的一层
        //     IsInterruptibleCaster()  此刻正在施放**可打断**的非近战法术 ⇒ 打断就是减输出
        static bool IsHighThreatSpell(uint32 spellId);
        static bool HasHighThreatAbility(Unit* target);
        static bool IsHealerCaster(Unit* target);
        static bool IsInterruptibleCaster(Unit* target);
        // [aggro 2026-10-17] 带**骷髅标记**（RTI 0 = skull）的目标：全队约定的第一集火目标，最高优先。
        static bool IsSkullMarked(Unit* target, Player* bot);
        // [2026-10-18 站长口径] "把星星标记作为控制标记，和骷髅头一致"
        //   队伍标记 index 0 = 星星（星形）= **控制标记**；与骷髅（index 7 = 击杀标记）同一套读法：
        //   标记是对象上的事实（group->GetTargetIcon），谁设的都算。
        static bool IsCcMarked(Unit* target, Player* bot);
        // [aggro 2026-10-17 站长口径] **威胁分级**（数字越大越优先），DPS 与坦克共用同一套：
        //     5 = 骷髅标记（人工指定优先于一切自动判断）
        //     4 = 有高威胁技能（HasHighThreatAbility：治疗/击晕/召唤/高额伤害 —— **怪的属性**）
        //     3 = 正在交战（在打自己/队友）
        //     2 = 正在施放可打断的法术（打断即减输出）
        //     1 = 正在施放治疗法术（此刻就在回血）
        //     0 = 其余
        //   放在这里而不是各自的策略类里：避免 TankTargetValue.cpp ↔ LeastHpTargetValue.h 循环包含。
        static int ThreatTier(Unit* target, Player* bot);
        // [2026-10-18 站长口径] 表 `ai_playerbot_priority_target` 里的优先级原值（0 = 不在表里）。
        //   语义：5=任何情况立刻转火杀、4=**需要优先控制**、3=优先击杀、2=无对应驱散才优先、1=只打断。
        static uint8 PriorityRankOf(Unit* target);
        // [2026-10-20 站长口径] `.bot prio <1-5>`（0 = 清除）：把一只怪（按 entry）的优先级
        //   **立刻**写进内存并落库（world 库 `ai_playerbot_priority_target`）。
        //   内存直改 + 推进 epoch 让各线程的读缓存失效 ⇒ 全服 bot 下一 tick 就按新的优先级
        //   选目标/控场，不用重启、也不用等 5 分钟自动重载。
        //   表不存在 / 写库失败 ⇒ 只在内存生效（可选增强，不能影响出怪），返回值仍是 true。
        static bool SetPriorityRank(uint32 entry, uint8 priority, char const* source = "gm");

        // -------------------------------------------------------------------
        // [2026-10-18 站长口径] **读条观测**层（dev/193_优先级观测表.sql）
        //   站长原话："对于怪物所有读条的治疗都标记为优先级[最高]，对于读条的控制技能，
        //   包括击晕、魅惑、恐惧等等标记为[优先击杀]，不包括缠绕"；
        //   以及 "ai_playerbot_priority_obs 表 + 观测持久化，每个变化加日志"。
        //
        //   按现行等级口径（数字越大越优先）换算：读条治疗 → 5；读条控制 → 3。
        //   来源是"引擎确实看见它读过这个条"（同 HasHighThreatAbility 的思路），不是静态猜。
        //   生效门槛：同一 entry 同类观察 >= 2 次；观测计数落 chars 库，跨重启保留。
        //   优先级来源：人工表 > 观测表 > 启发式（观测只补人工表里没有的 entry）。
        // -------------------------------------------------------------------
        // 观测一次施法（同一 guid 同一法术 1s 内不重复计；只认敌对怪 / 战斗中）。
        static void ObservePriorityCast(Unit* caster, uint32 spellId);
        // 观测表里该 entry 的**有效优先级原值**（0 = 还没攒够证据 / 不在观测表里）。
        static uint8 ObservedPriorityRank(Unit* target);
        // 把内存里积累的观测批量落库（拉怪结束调；表不存在 ⇒ 静默降级为只记内存）。
        static void FlushPriorityObs();
        virtual std::string Format() override;

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "possible attack targets"; } //Must equal iternal name
        virtual std::string GetHelpTypeName() { return "combat"; }
        virtual std::string GetHelpDescription()
        {
            return "This value contains all the units the bot should attack.";
        }
        virtual std::vector<std::string> GetUsedValues() { return { "attackers", "attack target"}; }
#endif 

	private:
		void RemoveNonThreating(std::list<ObjectGuid>& targets, bool getOne);
        // [aggro 2026-10-17 站长口径] 候选排序（骷髅 > 高威胁技能怪 > 交战 > 可打断 > 治疗中 > 其余）。
        void RankCandidates(std::list<ObjectGuid>& targets);

        // [aggro 2026-10-17 站长口径] 副本/团本门禁的候选层判据（只在副本里生效）：
        //   IsInstanceTankGateActive()：在副本/团本 + 自己不是坦克 + 队里有活着的坦克
        //   IsAllowedForNonTank()    ：坦克已有仇恨，或这只怪正在打队伍成员（队友被咬要能还手）
        bool IsInstanceTankGateActive() const;
        bool IsAllowedForNonTank(Unit* target) const;

        static bool IsImmuneToDamage(Unit* target, Player* player);
        static bool HasIgnoreCCRti(Unit* target, Player* player);
        static bool IsTapped(Unit* target, Player* player);
        static bool IsCcTarget(Unit* attacker, Player* player);
    };

    class PossibleAddsValue : public BoolCalculatedValue
    {
    public:
        PossibleAddsValue(PlayerbotAI* const ai, std::string name = "possible adds") : BoolCalculatedValue(ai, name) {}
        virtual bool Calculate() override;

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "possible adds"; } //Must equal iternal name
        virtual std::string GetHelpTypeName() { return "combat"; }
        virtual std::string GetHelpDescription()
        {
            return "This value returns true if any of the targets the bot wants to attack could agro adds.";
        }
        virtual std::vector<std::string> GetUsedValues() { return { "possible targets no los", "possible attack targets" }; }
#endif 
    };
}
