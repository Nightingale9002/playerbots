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
            Prep,       // [2026-10-31] 开怪前准备（buff/护甲/姿态/资源），原 Form+Buff 并入
            Flee,       // [2026-10-31] 逃跑/风筝：血太低或扛不住，先拉开
            Approach,   // 有目标但不在射程，交给移动策略
            Burn,       // 单体常规输出（平稳输出期）
            Instant,    // 移动中/需要瞬发
            Burst,      // 爆发窗口（仇恨稳定 + CD 好即开，早用早 CD）
            Aoe,        // 群伤
            Weave,      // 等自动射击窗口（猎人：读条会顶掉自动射击）
            Maintain,   // [2026-10-31] 猎人平射计时器窗口：按平射剩余时间决定这一手放什么/空着
                        //   [2026-10-31 站长口径] 奥法也走这个状态，口径同一份：**预判排程 + 空档填充**
                        //   （先算窗口放得下什么：寒冰箭 → 瞬发 → 卡秒奥冲 → 确实放不进才 Hold），
                        //   绝不"等 debuff 掉到 500ms"。规则表见 DpsFsm.cpp → TickArcaneMaintain()。
            Recover,    // 资源/血量不足，回蓝回血
            Execute     // [2026-10-31 站长口径] 战士斩杀窗口：目标血 <= execute_hp（默认 20%）
                        //   单列一个状态，是为了让"低血段只打斩杀"这件事在状态表上看得见，
                        //   而不是藏在 Maintain 里被猛击/致死抢走怒气（legacy 实测低血木桩仍只出致死）。
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

        // [2026-10-31 站长口径] 一 tick 的三种结果（不再用 bool：bool 逼着"什么都不放"和"我不管"共用一个返回值）：
        //   Acted   —— 本 tick 我放了技能（= 原来的 true，引擎让位，语义完全不变）；
        //   Hold    —— 本 tick 我**有意空着**：不放技能，引擎也不许动这一手
        //              （例：猎人自动射击锁定窗口期间；交回引擎它立刻插自己的技能，窗口就废了）；
        //   Decline —— 我不处理，交回引擎（走位/追击/开怪/喝水/魔杖都归它）。
        enum class TickResult
        {
            Acted,
            Hold,
            Decline
        };

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
            // [2026-10-31 站长口径] 仇恨三档的输入：自己 / 坦克对**同一个 target** 的威胁值。
            //   坦克 = 组队成员里第一个 PlayerbotAI::IsTank() 的人；没有队伍或没有坦克
            //   ⇒ tankAlive=false、tankThreat=0 ⇒ EvalThreat 恒 Open（假人场景绝不掉 DPS）。
            float myThreat = 0.0f;
            float tankThreat = 0.0f;
            bool tankAlive = false;
            // [2026-10-31 站长口径] 猎人平射/读条时间轴：**只在 spec 是猎人时计算**
            //   （其它职业保持默认值，不增加任何开销）。
            //   autoRemainMs —— 距下一发自动射击还剩多少毫秒。
            //      ⚠️ 本核心的 `Unit::getAttackTimer(RANGED_ATTACK)` **不随时间递减**
            //      （Unit::Update 只递减 BASE/OFF），所以该字段由状态机侧"自动射击计数 + 观测周期"反推，
            //      不是直接读那个死计时器（详见 DpsFsm.cpp 的 BotMem::autoShotAt 注释）。
            //   steadyCastMs —— 当前急速下稳固射击的**实际**读条（SpellMgr::GetSpellCastTime × 远程攻速倍率）
            //   multiReady / arcaneReady / killCommandReady —— 本 tick 是否可用（含 GCD/冷却）
            int autoRemainMs = 0;
            int steadyCastMs = 1500;
            bool multiReady = false;
            bool arcaneReady = false;
            bool killCommandReady = false;
        };

        Spec DetectSpec(PlayerbotAI* ai);

        // 该 bot 半径 range 内看到的敌对数量（假人场景用来证明"1 目标就是 1 个目标"）
        int PackCount(Player* bot, float range);

        // 假人场景专用：声明"这一轮只有这些目标是自己的" ⇒ pack 统计只数它们，
        //   避免多组同时跑时把邻居的假人算进 AoE 判断（1 目标场景被误判成群架）。
        void SetScenarioTargets(uint32 guid, std::vector<ObjectGuid> const& targets, uint32 runId = 0);

        // 只清自己那一轮（同一只 bot 被复用时，上一轮的收尾不能把新一轮的目标集清掉）
        void ClearScenarioTargets(uint32 guid, uint32 runId = 0);

        // [2026-11-01 站长口径] 新一轮假人场景开始 ⇒ 把上一轮残留的 FSM 记忆清干净。
        //   根因：`s_mem` 是 per-guid 常驻的（SimScenario 只重置 HP/蓝），状态机自己的记忆没人管 ——
        //   上一轮可能停在 planStep=5（寒冰箭相位）或刚打完爆发的 lastBurst/burstUntil，
        //   下一轮开局就带着它，行为与"干净开局"完全不同 ⇒ 实测 `arcane:mode=5` 三轮
        //   dps 819.8 / 581.3 / 544.7（波动接近 50%，远超以往）。
        //   只清"跨轮会改变决策"的字段；`failUntil`（技能退避）与宠物相关字段**保留**
        //   （那是核心/宠物侧的短期状态，清掉只会让退避失效、多刷无用的施法失败）。
        //   由 SetScenarioTargets() 在 **runId 变化时**调用一次（同一 runId 的重复登记不重置）。
        void ResetScenarioState(uint32 guid);

        // 假人场景：没有 victim 时自己咬上去（只对"声明过的场景目标"生效，副本里不改变行为）
        bool EngageScenarioTarget(PlayerbotAI* ai);

        // 假人场景里状态机已经全权处理技能：让引擎这一步不要再插自己的施法（避免抢 GCD）。
        //   副本里返回 false（引擎照常跑），保证走位/打断/救人不受影响。
        bool SkipEngine(uint32 guid);


        // 这一 tick 状态机是否占了**施法输出**（放了技能，或有意的 TickResult::Hold）。
        //   只有在这种情况下才让引擎让位，否则引擎要负责追击/走位。
        //   [2026-10-31 站长口径] 从"真的放了技能"扩成"放了技能 **或** 有意空着"：
        //   Hold 的全部意义就是"这一 tick 什么都别放，引擎也别插"（否则引擎立刻放自己的技能）。
        bool DidCastThisTick(uint32 guid);

        // [2026-10-29] 调优并发：给**单只 bot** 覆盖某个参数，这样同一轮里可以用不同的参数值做 A/B。
        void SetParamOverride(uint32 guid, char const* key, double value);
        void ClearParamOverrides(uint32 guid);

        // [2026-10-29] 单只 bot 开关（A/B 对照：同一轮里一部分 bot 走状态机、一部分走原引擎）
        void SetBotEnabled(uint32 guid, bool on);

        // [2026-10-29] 假人测试用：强制按 DPS 打（忽略"坦/奶不归状态机"的角色门）
        void SetBotForceDps(uint32 guid, bool on);

        // 返回 true 表示本 tick 已由状态机接管（无论是否成功施法）。
        // [2026-10-30] 该 bot 当前专精的技能里是否有"必须站背后"的（只有这些才值得让移动层去摆位；
        //   给所有近战都摆位会打断挥击：实测平砍 78 → 54，dps 824 → 518）。
        bool NeedsBehind(Player* bot);

        // [2026-10-30 诊断] 引擎这一 tick 是否真的跑了（用来定位"policy 模式白字平砍变少"）：
        //   如果 policy 模式让引擎"少跑了"，白字/开怪这些由引擎负责的部分就会掉。
        void NoteEngineRun(uint32 guid);

        // [2026-11-01 站长口径] mode=5（显式状态图）当前覆盖的专精：
        //   法师 冰/奥/火 · 猎人 兽王/射击/生存 · 战士 武器/狂暴 · 盗贼 战斗/刺杀/敏锐
        //   · 术士 毁灭/**痛苦/恶魔** · 圣骑士 惩戒 · 萨满 元素/增强 · **牧师 暗影**
        //   （术士三系共用 `DpsFsm.cpp → TickWarlockFsmCore()`：毁灭走 `TickWarlockFsm()`，
        //     痛苦/恶魔走 `TickWarlockDotsFsm()`，只换单体表与群架档；
        //     痛苦 DoT 续期用 `TargetAuraRemain() < refresh_dot`，多目标铺前 `multidot_max` 个腐蚀）。
        //   萨满·恢复（Restoration）**不做 DPS** ⇒ 不进图，继续走 legacy TickShaman（治疗以后单独做）。
        //   萨满状态表（细节见 DpsFsm.cpp 的"萨满"一节）：
        //     元素：Idle / Prep(闪电盾缺失) / Approach(>range_cast) / Recover(蓝<recover_power)
        //           / Aoe / Burst(元素掌握就绪或目标<burst_hp) / Maintain(蓝够闪电链优先、
        //           蓝<cl_mana_min 只打闪电箭；震击只按 ls_stacks 层数震)
        //     增强：Idle / Approach(够不着) / Recover(空蓝→萨满之怒) / Aoe(火元素新星一次→熔岩) / Maintain
        //   ⚠️ 图腾**不进状态表**，是每 tick 的最高优先级动作（空槽/快到期 > 输出技能）：
        //     施放图腾后 `GetTotemGuid(slot)` **仍为空**（召唤走 SPELL_EFFECT_SUMMON，不登记槽位，
        //     见 KNOWN_ISSUES §10.53）⇒ 状态机自己记账（BotMem 的 totemCastAt/totemEntry/totemPos），
        //     判据见 DpsFsm.cpp → ShTotemNeed()。**不要**改回"查槽位判空"，那会把 GCD 全吃在图腾上。
        //   牧师状态表（细节见 DpsFsm.cpp 的"牧师·暗影"一节）：
        //     Idle / Prep(暗影形态缺失) / Approach(>range_caster) / Recover(蓝<recover_power：药水→魔杖)
        //       / Aoe(多目标铺 DoT) / Burst(目标<burst_hp：饰品→灭→轮) / Maintain
        //   ⚠️ 暗影是**唯一走 GCD 时间轮**的专精，不是优先级表：一拍 = 一个 GCD，
        //     维护 swp/vt/mb 三个时间点，并做**两拍前视**避让（两个 DoT 不许同拍到期，
        //     也不许卡在心爆的拍上）。戒律/神圣继续 Decline（不是 DPS 循环问题）。
        //   见 DpsFsm.cpp → TickShadowFsm() / ShadowPlanBeat() / TickShadowBeat()。
        bool Tick(PlayerbotAI* ai);
    }
}
