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
            Execute,    // [2026-10-31 站长口径] 战士斩杀窗口：目标血 <= execute_hp（默认 20%）
                        //   单列一个状态，是为了让"低血段只打斩杀"这件事在状态表上看得见，
                        //   而不是藏在 Maintain 里被猛击/致死抢走怒气（legacy 实测低血木桩仍只出致死）。
            // [2026-11-01 站长口径] 治疗状态（HealCore，四治疗共用；DPS 专精永远不会进这四个）：
            //   Dispel    —— 分配的驱散，优先级最高（有可驱散的负面效果就先驱散再谈加血）
            //   Emergency —— 有人 < emergency_hp(35%)：先交大 CD（守护之魂/痛苦压制），再盾/快速治疗
            //   Tank      —— 只盯**分配的主坦**：renew 常驻 → 低级强效维持 → 预判尖刺用高级强效
            //   Raid      —— 其余队友：血量最低者优先 / 小队集中掉血用群体治疗
            //   另有 Recover（蓝 < recover_mana：只放最省的）与 Prep（自身 buff）沿用既有状态。
            Dispel, Emergency, Tank, Raid,
            // [2026-11-01 站长口径] 坦克（第 1 层：仇恨/生存/嘲讽/加怪；见 DESIGN §14）：
            //   Survival —— **按"预期受到伤害"触发**（受伤速率 × 下一段窗口），不是当前血量阈值；
            //               战士 盾墙/破釜；熊 狂暴回复/生存本能。⚠ 熊坦不默认开树皮（会变人形）。
            //   Pickup   —— 附近的新怪不在我身上（加怪接管）：嘲讽 / 远程拉（投掷、精灵之火）。
            //   Taunt    —— 已经有我仇恨、但 victim **确实在打别人**（丢仇恨）：只这时才嘲讽；
            //               对已经在自己身上的目标嘲讽是白交 CD（绝不预防性嘲讽）。
            //   仇恨循环本身复用 Maintain（"其余"那一档）。
            Survival, Pickup, Taunt
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
        //              [2026-11-01 站长口径] **Hold 在 Tick() 派发层有统一兜底**：只有 GCD 真被锁
        //              （读条中 / GCD 未转好）或专精用 `HoldWait()` 显式登记了"有意等"时才成立，
        //              其余一律降级成 Decline —— 不让"一拍什么都放不出"把引擎整个锁死
        //              （实测暗牧/平衡出现过整轮 45 秒 0 施法 0 伤害）。新专精默认不用管这条。
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
            // [2026-11-02 修·副本拉整间房] 只数**已经进战**的敌对（`IsInCombat()` / 有 victim）。
            //   副本实测：AoE 判据用 pack10/pack30 会把"站着没动"的怪也算进来
            //   ⇒ 只有 1 只进战、旁边 11 只站桩时就放 AoE / 群拉，把整间房拉过来
            //   （实测 adds=12 / onme=3 / hp 16%）。pack10/pack30 保持原义（诊断用），
            //   AoE 判据改读这两个字段（参数 `aoe_require_engaged`，默认 1；设 0 回旧行为 A/B）。
            int engaged10 = 0;  // 10 码内**已进战**敌对
            int engaged30 = 0;  // 30 码内**已进战**敌对
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

            // [2026-11-01 站长口径] **B. NearbyEnemy 列表**（多目标 DoT / AoE 选牌的输入）。
            //   以前多目标只能靠 `pack10/pack30` 的**个数**，缺"每只怪的血量/仇恨/DoT 剩余"这三样：
            //     · 血量（绝对值优先）：选"能跳满 DoT"的目标要看还剩多少血，百分比在 1,000,000 血的
            //       假人身上完全分不出 60% 和 59% 的差别；
            //     · 仇恨（双方）：怪身上的威胁表里"我 / 坦克"各多少 —— 仇恨档位不是 Open 的目标
            //       不给新 DoT（给一只马上要 OT 的怪铺 DoT = 把仇恨再顶高一截）；
            //     · DoT 剩余：已经满 DoT 的目标不该再花一个 GCD（重复铺会顶掉上一颗，白费）。
            //   ⚠️ **只在需要多目标的专精里填**（暗牧/痛苦术/鸟德/元素萨/火法），其它专精保持空表
            //     ⇒ 不增加任何每 tick 开销（`BuildNearbyEnemies()` 第一行就 return）。
            //   ⚠️ 过滤口径复用现成的 `possible targets`（敌对 + 存活 + 射程内 + 假人场景只数本轮
            //     声明的目标），不再新造一套扫描（见 DpsFsm.cpp → CountHostilesNear()）。
            enum { kNearbyDotMax = 3 };     // 每个专精最多记 3 个"关键 DoT"
            struct NearbyEnemy
            {
                ObjectGuid guid;            // 这只怪
                uint32 hp = 0;              // **绝对血量**（选"能跳满 DoT"的目标用它）
                float hpPct = 0.0f;         // 血量百分比（只给日志/人看）
                float dist = 0.0f;          // 距离
                float myThreat = 0.0f;      // 从**怪的**威胁表读：`target->getThreatManager().getThreat(bot)`
                float tankThreat = 0.0f;    // 同上，读队伍主坦（没有坦克 = 0）
                bool  tankAlive = false;    // 队伍里有没有坦克（没有 ⇒ 仇恨档位恒 Open）
                int   dots = 0;             // 关键 DoT 已挂几个
                int   dotRemain[kNearbyDotMax] = { 0, 0, 0 };   // 每个关键 DoT 的剩余毫秒（0=没挂）
                bool  dotFull = false;      // 关键 DoT 全满（多目标铺场要跳过）
                bool  threatOpen = true;    // 这只怪的仇恨档位是不是 Open（不是 ⇒ 不给新 DoT）
            };
            // 上限 `nearby_max`(默认 8)，按**绝对血量降序**（血最多的死得最慢 ⇒ DoT 跳得最多）。
            std::vector<NearbyEnemy> nearby;
        };

        Spec DetectSpec(PlayerbotAI* ai);

        // 该 bot 半径 range 内看到的敌对数量（假人场景用来证明"1 目标就是 1 个目标"）
        int PackCount(Player* bot, float range);

        // 假人场景专用：声明"这一轮只有这些目标是自己的" ⇒ pack 统计只数它们，
        //   避免多组同时跑时把邻居的假人算进 AoE 判断（1 目标场景被误判成群架）。
        void SetScenarioTargets(uint32 guid, std::vector<ObjectGuid> const& targets, uint32 runId = 0);

        // 只清自己那一轮（同一只 bot 被复用时，上一轮的收尾不能把新一轮的目标集清掉）
        void ClearScenarioTargets(uint32 guid, uint32 runId = 0);

        // [2026-11-01 站长口径] **准备窗口（prep_seconds，假人场景专用）**：图腾属于准备阶段。
        //   为什么需要：SimScenario 现在一 spawn 就接战 ⇒ 图腾只能在**战斗里**补，开打前 4 个 GCD
        //   全花在插图腾上。这里给一个"开局先脱战"的窗口：
        //     · 窗口内状态机**不接战**（不跑 EngageScenarioTarget、不跑战斗图），只跑 Prep
        //       （萨满 = `ShTickTotemsPrep()` 把四槽插满）；
        //     · 窗口到点后由 SimScenario 重置基线并开始正式计时（准备这几秒**不计入** dps 窗口）。
        //   `untilMs` 用 `WorldTimer::getMSTime()` 口径的绝对毫秒。0 / 未登记 = 没有窗口
        //   ⇒ 副本与现有 DPS 测法一个字节不变（`prep_seconds` 默认 0）。
        void SetScenarioPrep(uint32 guid, uint32 untilMs);
        void ClearScenarioPrep(uint32 guid);
        bool InScenarioPrep(uint32 guid, uint32& untilMs);

        // [2026-11-02 站长口径] **全职业 准备(Prep) / 维护(Upkeep)**（共用同一张表）：
        //   `DpsFsm.cpp → TickPrepCore()`（`TickPrep()` = 脱战层，`TickUpkeep()` = 战斗层）。
        //   一张按"职业 + 专精"的表，缺什么补什么，一次一个 GCD：
        //     法师 = 护甲(冰/熔岩/法师，按专精取能放的) → 奥术智慧；
        //     德鲁伊 = 枭兽/猫/巨熊形态 → 野性印记；
        //     牧师 = 暗影形态(暗牧) → 心灵之火 → 真言术韧；
        //     萨满 = 四槽图腾(复用 `ShTickTotemsPrep`) → 闪电盾(元素)/水盾 → 风怒武器(增强)；
        //     圣骑 = 圣印(按专精) → 光环(已有任意光环不换) → 祝福；
        //     战士 = 姿态(狂暴/战斗/防御) → 盾牌格挡(防护) → 战斗怒吼；
        //     盗贼 = 主手毒药/磨刀石 → 潜行(非战斗系，仅脱战)；
        //     猎人 = 宠物(复用 `SummonPetIfNeeded`/`HunterPetGuard`) → 守护 → 猎人印记；
        //     术士 = 恶魔护甲 → 召唤恶魔 → 灵魂链接。
        //   两层（站长口径：形态会掉、盾会没、宠物会死、圣印会消，**战斗中必须补**）：
        //     · Prep（脱战 / 场景准备窗口）：整张表逐项补齐；
        //     · Upkeep（战斗中）：同一张表，只做"当前缺失/即将到期"的项，
        //       优先级 `救急 > 爆发 > Upkeep(形态/盾/图腾/宠物/圣印) > 常规循环`
        //       （由 `TickInner` 在专精战斗图之前调用；上一 tick 处于救急/爆发档时不插）。
        //   纪律：① 一次只做一个动作；② 失败就跳过并退避 5 秒（**绝不 Decline 死循环**，
        //   踩过的坑：卡在 Prep 整场 0 输出）；③ 每个 item 都有结果日志
        //   `[DPSFSM-PREP] <bot> spec=<x> item=<name> ok|skip reason=<...> incombat=<0|1>`
        //   （同一 item 的失败 10 秒最多一条）；④ 战斗中施法权始终在状态机，Upkeep 不交回引擎。
        //   开关：`prep_enable`（默认 1；0 = 关掉本入口）。

        // [2026-11-01 站长口径] **治疗场景的目标分配**（假人实测用；副本里走 Group 成员）。
        //   为什么单独一张表：治疗者的"病人"不是敌对目标，`SetScenarioTargets()` 那套是给
        //   "我要打谁"用的（pack 统计 / 自动咬上去）；治疗这边要的是"我要治谁 + 谁是主坦"。
        //   healTargets 为空 ⇒ HealCore 退回扫 `bot->GetGroup()` 的队员（副本里的正常路径）。
        //   mainTankGuid = 0 ⇒ 主坦由 `PlayerbotAI::IsTank(member)` 现场判。
        //   runId 语义与 SetScenarioTargets 一致（变化 = 新一轮，清掉上一轮的速率窗口/统计）。
        void SetScenarioHealTargets(uint32 healerGuid, std::vector<ObjectGuid> const& healTargets,
                                    uint32 mainTankGuid, uint32 runId = 0);
        void ClearScenarioHealTargets(uint32 healerGuid, uint32 runId = 0);

        // [2026-11-01 站长口径] **坦克场景标记**（假人实测 + 副本）。
        //   为什么需要：德鲁伊的 `Spec` 里野性只有一个值（猫/熊共用 `Feral`），
        //   "这一轮按坦克图跑"和"按猫德图跑"只能靠**当前形态**区分（`GetShapeshiftForm()==FORM_BEAR`）；
        //   而熊坦开打前往往还在人形（要先变熊）⇒ 单看形态会在第一 tick 判成猫德。
        //   这个标记只影响派发（走 `TickTankFsm()`），不改变任何技能表/阈值。
        void SetScenarioTank(uint32 guid, bool on);
        bool IsScenarioTank(uint32 guid);
        void ClearScenarioTank(uint32 guid);

        // [2026-11-01 站长口径] 坦克验收计数（嘲讽 / 加怪接管 / 保命次数）。
        //   只写进日志与报告，不参与任何决策；由假人场景每 10 秒取一次。
        //   [2026-11-01 站长口径·防护骑] 嘲讽要能分清**来源**（验收要用 `taunt=rd|reckoning`）：
        //     tauntRd       —— `righteous defense` 真的放出去（含两种打法，细分见下两行）；
        //     tauntRdDirect —— 其中"按站长口径**直接对敌人**放"成功的次数（核心支持重定向后自然增长）；
        //     tauntRdHelp   —— 其中"对正在被打的友方目标放"成功的次数（TBC 核心的真实语义）；
        //     tauntReck     —— `hand of reckoning` 放出去的次数（TBC 库里没有这个 id ⇒ 正常恒 0）。
        //   战/熊不吃这四个字段（`tauntRd*`/`tauntReck` 恒 0，`taunts` 照旧）。
        struct TankStats
        {
            uint32 taunts = 0;      // 真的放出去的嘲讽次数（对"在打别人"的目标）
            uint32 pickups = 0;     // 加怪接管次数（嘲讽或远程拉成功）
            uint32 survivals = 0;   // 保命技（盾墙/破釜/狂暴回复/生存本能/圣盾术）放出去次数
            uint32 tauntRd = 0;
            uint32 tauntRdDirect = 0;
            uint32 tauntRdHelp = 0;
            uint32 tauntReck = 0;
        };
        void GetTankStats(uint32 guid, TankStats& out);

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

        // [2026-11-01 站长口径·A] **场景目标守卫**：假人场景里"只能打自己这堆木桩"。
        //   病根（实测 `[DUMMY] id=22 spec=arcane`）：引擎在状态机 Decline 的那些 tick 里自己挑目标，
        //   把 victim 换成了**同场的另一只 bot**（`victim=Nenrus victimGuid=51`），之后 90 秒
        //   dps 恒 0（`dps=326.8`），而自己那堆木桩一直活着。
        //   这里每个 tick 校正一次：victim 不在本轮 `s_scenarioTargets` 里 ⇒ `CombatStop` 后重新咬自己的。
        //   非场景 bot 直接返回 false（副本里一个字节都不变）。
        bool EnforceScenarioVictim(PlayerbotAI* ai);

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
        //   · **德鲁伊 野性/平衡**（恢复德在 mode=5 里 Decline：治疗以后单独做）
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
        //   德鲁伊状态表（细节见 DpsFsm.cpp 的"德鲁伊"一节）：
        //     野性（猫）：Idle / Prep(猫形态) / Recover(蓝低且不能变身) / Approach(近战够不着)
        //       / Aoe(横扫) / Maintain(能量轮)。**这不是优先级表，是能量 tick 相位的排程机**：
        //       核心状态是"距下一跳回能还有多久"（2 秒一跳、20 点），按"一跳够不够"决定
        //       等一跳还是直接变身（Power Shift，有激怒时 +40 能量）。相位靠观测能量跳变得到
        //       ——核心的 `m_regenTimer` 是 protected，模块侧读不到。
        //     平衡（鸟）：Idle / Prep(枭兽形态) / Approach(>range_caster) / Recover(蓝<recover_power)
        //       / Aoe(hurricane) / Burst(目标<burst_hp：饰品→force of nature) / Maintain
        //       （moonfire → insect swarm 按 refresh_dot 续，starfire 填充；移动/蓝紧改 wrath）。
        //   见 DpsFsm.cpp → TickDruidFsm() / TickFeralFsm() / TickBalanceFsm()。
        //
        // [2026-11-01 站长口径] **治疗侧（HealCore）**：`mode=5` 接**四治疗**——
        //   牧师·神圣 / 圣骑士·神圣 / 萨满·恢复 / 德鲁伊·恢复（**戒律牧继续 Decline**）。
        //   骨架共用：Idle → Prep(自身 buff/形态/水盾) → Dispel(最高优先) → Emergency → Tank → Raid → Recover。
        //   三套"辅助功能"配置表在 DpsFsm.cpp → HealCfgFor()，只差技能名与阈值：
        //     神圣牧 = 反应式工具箱（强效降级 + 盾 + 祷言 + 治疗之环）；
        //     神圣骑 = 光之优雅为轴的预读单刷（低级圣光术续 grace、6 级闪现填充、震击救急、
        //              **几乎没有群疗** ⇒ Raid 只做有限支援）；
        //     恢复萨 = 治疗链为核心（受伤人数 ≥2 就 chain heal）、大地之盾给主坦、水盾常驻自己、
        //              治疗之道用低等级治疗波叠 3 层、图腾复用 DPS 侧的 ShTotemNeed 子系统。
        //   **恢复德是另一种模型**（`hotModel=true`，见 DpsFsm.cpp → TickHealHot()）：
        //     IDLE 判据从"掉血才加 + 预读读条"换成 **"HoT 剩余时间 + 受伤速率"**；
        //     坦克三层生命绽放是绝对轴心（层数/剩余不足就补，不看血量），
        //     优先级 = 三层花不断档 > 回春 > 愈合的 HoT，救急只有 swiftmend 与
        //     自然迅捷 + 治疗之触（临时切出树形态），另加宁静/激活。
        //   派发在"必须进战斗 / 必须有 victim / 坦奶让位"三道门**之前**：治疗者没有敌人也要工作。
        //   见 DpsFsm.cpp → TickHealFsm() / TickHealHot() / HealCfgFor()。
        //
        // [2026-11-01 站长口径] **坦克侧（第 1 层：仇恨/生存/嘲讽/加怪）**：`mode=5` 接
        //   **防护战（战士 · `Spec::Protection`）**、**熊德（德鲁伊 · `Spec::Feral` 且熊形态/坦克场景）**
        //   与 **[2026-11-01 站长口径] 防护骑（圣骑士 · `Spec::Protection`）**。
        //   状态：Idle → Prep(防御姿态/熊形态/正义之怒+光环) → Survival(按预期伤害) → Pickup(加怪)
        //         → Taunt(丢仇恨) → Maintain。
        //   防骑与战/熊**共用同一张图，只换动作表**（`DpsFsm.cpp → TankKind::PaladinProt`）：
        //     Prep     : righteous fury（缺则补）→ holy shield → devotion aura（光环二选一按已有光环判断）
        //     Taunt    : righteous defense（站长口径"直接对敌人放"；放不出退到对 victim 放）
        //                → hand of reckoning（若学会）
        //     Pickup   : avenger's shield（弹射由法术自身处理）→ judgement → 嘲讽
        //     Maintain : 多目标 consecration → holy shield（掉了补）→ avenger's shield → judgement
        //                → seal of righteousness + hammer of the righteous（若学会）
        //     Survival : divine protection → holy shield；`tank_surv_emerg` 第二档 lay on hands
        //   ⚠ 惩戒骑（`Spec::Retribution`）走 `TickRetFsm()`，与这里**互斥**（派发处按职业+专精这一对判）。
        //   ⚠ 走位/拉怪路线/波次/boss 换嘲**不归 FSM**（DESIGN §14.3：只消费，不重造）。
        // [2026-11-01 站长口径] **状态机独占施法权（mode=5）**：`Tick()` 的返回值 = "这一 tick
        //   谁占着施法闸门"，不再等于"我放了技能"（后者看 DidCastThisTick）。规则：
        //     · mode=5 且**战斗中**（IsInCombat 或 有 victim）：无论本 tick 出手与否都占闸
        //       ⇒ 引擎不得施法（但照常走位/追击/平砍/上马/拾取/交易，没有用 Hold）；
        //     · 脱战：放行（补 buff / 喝水 / 上马 / 治疗 / 复活 / 拾取 交给引擎）；
        //     · mode=0~4：返回 `handled && SkipEngine()`，与 dd09509d 之前**完全一致**。
        //   参数（tune 文件）：`fsm_own_cast_combat`（默认 1，0 = 回到旧行为）、
        //     `fsm_own_cast_ooc`（默认 0；⚠ 置 1 目前不会真的挡住脱战施法 —— 闸门在
        //     PlayerbotAI::UpdateAIInternal 里只于 inCombat||victim 时合上，需要那边配合）。
        //   统计日志：`[FSM-CAST] bot=%s fsm=%u engine=%u blocked=%u inCombat=%d`（每 bot 30 秒）。
        // [2026-11-02 站长口径] **战斗内 AI 更新可以更密**：状态机每 tick 的开销已经压到 ~50-100µs，
    //   而 bot AI 的更新间隔默认是 1 秒（`RandomBotUpdateInterval`）—— 打副本时反应太慢。
    //   这里只回答"最近有没有 bot 在战斗"，由管理器据此把更新间隔切到更短的值。
    bool RecentCombatBot();
    bool Tick(PlayerbotAI* ai);
    }
}
