#pragma once

#include "playerbot/playerbot.h"

namespace ai
{
    namespace dpsfsm
    {
        // [2026-10-29 站长口径] DPS 技能释放也用状态机（不是优先级循环表）：
        //   状态 = "现在在做什么"，动作 = 本 GCD 要放的技能/等待，转移 = 资源/目标/距离/触发。
        // [2026-10-29 追加口径] 血量、资源、目标血量、技能释放状态都要进状态机：
        //   一次 tick 先把局面拍成 Snapshot，转移只看 Snapshot + 每个 bot 的记忆（冷却/叠层/饰品）。
        enum class State
        {
            Idle,       // 没目标
            Approach,   // 有目标但不在射程，交给移动策略
            Burn,       // 单体常规输出
            Instant,    // 移动中/需要瞬发
            Burst,      // 爆发窗口（大招/饰品/斩杀）
            Aoe,        // 群伤
            Weave,      // 等自动射击窗口（猎人：读条会顶掉自动射击）
            Recover     // 资源不足，回蓝/等待
        };

        enum class Spec
        {
            Unknown = 0,
            // 法系
            Fire, Frost, Arcane,
            Affliction, Demonology, Destruction,
            Shadow, Discipline, Holy,
            Balance, Restoration, Elemental,
            // 物理
            Arms, Fury, Protection,
            Combat, Assassination, Subtlety,
            BeastMastery, Marksmanship, Survival,
            Enhancement, Feral,
            Retribution
        };

        char const* StateName(State s);
        char const* SpecName(Spec s);

        // 一次 tick 的局面快照。
        //   selfHp / power   —— 自己血量百分比、主资源百分比（法力按 %；怒气/能量按占满值 %）
        //   rage/energy      —— 原始数值（判断"够不够放这一手"用）
        //   combo            —— 连击点
        //   targetHp         —— 目标血量百分比
        //   casting          —— 正在读条/引导（此时一律不插技能）
        //   gcd              —— 公共冷却还没转好（Ready() 里逐技能再判一次）
        //   hitLowSelfHp     —— 自己血量低（留给保命/换防御状态）
        struct Snapshot
        {
            Player* bot = nullptr;
            Unit* target = nullptr;
            float selfHp = 100.0f;
            float power = 100.0f;
            int32 rage = 0;
            int32 energy = 0;
            uint8 combo = 0;
            float targetHp = 100.0f;
            float dist = 0.0f;
            bool moving = false;
            bool casting = false;
            bool gcd = false;
            bool silenced = false;
            int pack10 = 0;     // 10 码内敌对（近身群伤判断）
            int pack30 = 0;     // 30 码内敌对（"这一坨是不是群架"）
        };

        Spec DetectSpec(PlayerbotAI* ai);

        // 该 bot 半径 range 内看到的敌对数量（假人场景用来证明"1 目标就是 1 个目标"）
        int PackCount(Player* bot, float range);

        // 假人场景专用：声明"这一轮只有这些目标是自己的" ⇒ pack 统计只数它们，
        //   避免多组同时跑时把邻居的假人算进 AoE 判断（1 目标场景被误判成群架）。
        void SetScenarioTargets(uint32 guid, std::vector<ObjectGuid> const& targets);

        // 假人场景：没有 victim 时自己咬上去（只对"声明过的场景目标"生效，副本里不改变行为）
        bool EngageScenarioTarget(PlayerbotAI* ai);

        // 假人场景里状态机已经全权处理技能：让引擎这一步不要再插自己的施法（避免抢 GCD）。
        //   副本里返回 false（引擎照常跑），保证走位/打断/救人不受影响。
        bool SkipEngine(uint32 guid);

        // [2026-10-29] 单只 bot 开关（A/B 对照：同一轮里一部分 bot 走状态机、一部分走原引擎）
        void SetBotEnabled(uint32 guid, bool on);

        // [2026-10-29] 假人测试用：强制按 DPS 打（忽略"坦/奶不归状态机"的角色门）
        void SetBotForceDps(uint32 guid, bool on);

        // 返回 true 表示本 tick 已由状态机接管（无论是否成功施法）。
        bool Tick(PlayerbotAI* ai);
    }
}
