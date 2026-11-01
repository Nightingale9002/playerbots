#include "playerbot/playerbot.h"
#include "playerbot/strategy/DpsFsm.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/strategy/actions/AttackAction.h"
#include "Grids/CellImpl.h"
#include "Maps/Map.h"
#include "Entities/Pet.h"           // [2026-10-31 站长口径] 猎人宠物生命周期守卫要现场建宠物（Pet::Create）
#include "Entities/Totem.h"         // [2026-11-01 站长口径] 萨满图腾：GetTotem()/GetTotemDuration() 要完整类型
#include "Server/SQLStorages.h"     // sCreatureStorage：按 entry 取 CreatureInfo（hunter_pet_entry）
#include "Server/DBCStores.h"       // [2026-11-01 站长口径] sSpellRangeStore：填充候选的射程过滤
// [2026-10-31 根因] 宠物 AI 的选择由 `FactorySelector::selectAI` 按 UNIT_FLAG_PLAYER_CONTROLLED 决定：
//   带这个 flag 的 HUNTER_PET 才是 PetAI（每 tick 复检追击 + 自动施法），否则退化成 GuardianAI
//   （只在"主人被打"时 AttackStart，之后不管追击）。这里只用 dynamic_cast 打一行诊断日志。
#include "AI/BaseAI/PetAI.h"
#include "AI/BaseAI/GuardianAI.h"
#include "Movement/MoveSpline.h"    // 诊断用：pet->movespline->Finalized()（Unit.h 里只有前向声明）

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>

namespace ai
{
    namespace dpsfsm
    {
        char const* StateName(State s)
        {
            switch (s)
            {
                case State::Idle:     return "Idle";
                case State::Approach: return "Approach";
                case State::Burn:     return "Burn";
                case State::Instant:  return "Instant";
                case State::Burst:    return "Burst";
                case State::Aoe:      return "Aoe";
                case State::Weave:    return "Weave";
                case State::Recover:  return "Recover";
                case State::Maintain: return "Maintain";
                case State::Execute:  return "Execute";
                // [2026-11-01 站长口径] 这两个状态早已在枚举里活着，但名字一直落在 "?" 上
                //   （术士 Prep 的日志就成了 `state=?`，验收脚本没法按状态切）；只补名字，不动行为。
                case State::Prep:     return "Prep";
                case State::Flee:     return "Flee";
                // [2026-11-01 站长口径] 治疗四状态（HealCore）
                case State::Dispel:    return "Dispel";
                case State::Emergency: return "Emergency";
                case State::Tank:      return "Tank";
                case State::Raid:      return "Raid";
            }
            return "?";
        }

        char const* SpecName(Spec s)
        {
            switch (s)
            {
                case Spec::Unknown:       return "unknown";
                case Spec::Fire:          return "fire";
                case Spec::Frost:         return "frost";
                case Spec::Arcane:        return "arcane";
                case Spec::Affliction:    return "affliction";
                case Spec::Demonology:    return "demonology";
                case Spec::Destruction:   return "destruction";
                case Spec::Shadow:        return "shadow";
                case Spec::Discipline:    return "discipline";
                case Spec::Holy:          return "holy";
                case Spec::Balance:       return "balance";
                case Spec::Restoration:   return "restoration";
                case Spec::Elemental:     return "elemental";
                case Spec::Arms:          return "arms";
                case Spec::Fury:          return "fury";
                case Spec::Protection:    return "protection";
                case Spec::Combat:        return "combat";
                case Spec::Assassination: return "assassination";
                case Spec::Subtlety:      return "subtlety";
                case Spec::BeastMastery:  return "beast mastery";
                case Spec::Marksmanship:  return "marksmanship";
                case Spec::Survival:      return "survival";
                case Spec::Enhancement:   return "enhancement";
                case Spec::Feral:         return "feral";
                case Spec::Retribution:   return "retribution";
            }
            return "?";
        }
    }
}

namespace
{
    using ai::dpsfsm::Snapshot;
    using ai::dpsfsm::Spec;
    using ai::dpsfsm::State;

// [2026-10-29] 每个 bot 一份状态机记忆（按 guid）。不同副本线程可能并发，统一加锁。

    struct BotMem;

    Spec DetectSpecByTalentsImpl(PlayerbotAI* ai, BotMem* m);

    // ==================================================================================
    // [2026-11-01 站长口径] **HealCore** 的两块记忆（四治疗共用）
    // ----------------------------------------------------------------------------------
    // HealWatch —— 每个"病人"一份**受伤速率**窗口：预读的唯一输入。
    //   口径：`heal_rate_window_ms`(3000) 内的掉落速率（HP 点/秒）。只累计**血量下降**的增量，
    //   不累计回复（自己和别的治疗抬起来的血不算"受伤"）⇒ 自己治疗不会污染速率。
    //   窗口没走满 / 一直没掉血 ⇒ rate=0 ⇒ 退化回"掉血才加"（站长口径的兜底）。
    // HealPending —— 一次已经放出去、还没落地的治疗：落地后再读一次血量，结算
    //   `eff = min(理论治疗量, maxHp - 落地前hp)`。**这是估算值**（见 HealFlushPending 注释）。
    // ==================================================================================
    struct HealWatch
    {
        uint32 prevHp = 0;      // 上一次采样到的血量（HP 点）
        uint32 prevAt = 0;      // 上一次采样的毫秒时刻
        uint32 dmgAcc = 0;      // 本窗口累计掉血（HP 点）
        uint32 winStart = 0;    // 本窗口起点（毫秒）
        float  rate = 0.0f;     // 上一次结算出的受伤速率（HP 点/秒）
    };

    struct HealPending
    {
        bool   active = false;
        ObjectGuid tgt;
        uint32 spellId = 0;
        char   name[40] = {};
        int32  theory = 0;      // 理论治疗量（EffectBasePoints × 系数 + 治疗强度）
        uint32 startAt = 0;     // 开始施法的毫秒时刻
        uint32 castEndAt = 0;   // 预计落地时刻（startAt + 实际读条）
        uint32 preHp = 0;       // **落地前**最后一次采样到的血量（估算有效量的基准）
        float  hp0Pct = 0.0f;   // 施法开始时的血量百分比
    };

    struct BotMem
    {
        uint32 guid = 0;
        State state = State::Idle;
        Spec spec = Spec::Unknown;
        time_t specCheckedAt = 0;
        std::map<std::string, uint32> ids;      // 技能名 �?已学会的最�?rank id
        std::set<std::string> unresolved;       // 只报一次的"没有这个技�?
        std::set<std::string> flags;            // 一次性判断结果（如是否有强化灼烧）

        time_t lastTrinketTry = 0;
        time_t sealAt = 0;       // 上一次成功挂圣印的时间（圣印 30 秒，不用每 tick 重挂）
        std::map<uint32, time_t> failUntil;   // 技能 id → 这个时间点之前别再试（施法被拒的退避）
        std::map<uint32, time_t> lastOk;       // 技能 id → 上次成功时间
        uint32 hbLast = 0;                     // 这只 bot 上次打心跳的时间
        bool castThisTick = false;
        // [2026-10-31 站长口径] 本 tick 状态机**有意空着**（TickResult::Hold）：引擎这一手也不许动。
        //   生命周期 = 一个 tick：Tick 开头清零，返回后留到引擎读到为止（引擎的让位判断在 Tick 返回之后）。
        bool holdThisTick = false;
        // [2026-10-31 站长口径] [DPSFSM-THREAT] 诊断的节流（WorldTimer 毫秒，10 秒一条）
        uint32 lastThreatLog = 0;
        // [2026-10-31 站长口径] [DPSFSM-MELEE] 诊断的节流（战士/盗贼共用一个字段：一只 bot 只会是其中一种）
        uint32 lastMeleeLog = 0;
        bool policyLogged = false;
        bool behindWarned = false;
        bool coverageLogged = false;    // 覆盖率断言只打一次
        bool aoeLogDone = false;        // [2026-10-31 站长口径] 冰法 Aoe 的 [DPSFSM-AOE] 诊断只打一次
        std::set<std::string> rankWarned;   // [2026-10-31 站长口径] "没有这条 rank 链/rank 越界"只报一次
        bool modeLogged = false;        // [2026-10-31 S1] 决策模式日志只打一次
        int  mode = 0;                  // [2026-10-31 S1] 本 tick 解析出的 DecisionMode（存 int：
                                        //   枚举声明在 BotMem 之后；TickPolicy 直接读它，不再二次读 conf）
        time_t lastBurst = 0;           // [2026-10-31 S2] 上次进入爆发的时间（"早用早 CD"的门）
        int  burstRounds = 0;           // [2026-10-31 站长口径] 冰法进入 Burst 的轮数：
                                        //   急冷(cold snap) 只在第 2 轮及以后放（第一轮先叠 buff/召水元素）
        time_t burstUntil = 0;          // [2026-10-31 站长口径] 本轮爆发的结束时刻：窗口内保持 Burst，
                                        //   让"冰冷血脉 → 饰品 → 水元素 → 急冷"能分几个 tick 依次放完
        // [2026-10-31 站长口径] 宠物（水元素/地狱火/猎人宠物/暗影魔…）是 17 个专精共用的输出单元：
        //   召唤失败只打一条诊断（别每 tick 刷屏），换了 bot 对象（重登）才允许再打一条。
        bool petLogDone = false;        // 宠物召唤失败的诊断只打一次
        Player* petLogBot = nullptr;    // 上次打诊断时的 bot 对象（重登后指针会变 ⇒ 重置一次）
        // [2026-10-31 站长口径] 猎人宠物生命周期守卫（mend/revive/call/create）的节流时间戳，
        //   单位毫秒（WorldTimer）。以前"宠物在不在"根本不归状态机管：召唤/复活全靠引擎，
        //   而假人场景引擎让位 ⇒ 猎人常常整场没有宠物（`[DUMMY] ... pet=0`）。
        uint32 petMendAt = 0;           // 上次尝试 mend pet
        uint32 petReviveAt = 0;         // 上次尝试 revive pet
        uint32 petCallAt = 0;           // 上次尝试 call pet（没有可召唤的宠物时别每 tick 重试）
        bool petCreateSkipLogged = false;   // skip-create(non-scenario) 只打一次
        // [2026-10-31] 猎人时间轴诊断只打一次：把 core 算出来的稳固读条/急速倍率留痕，
        //   方便判断"这一轮用的是 1500 回落值还是真实急速值"。
        bool steadyCastLogged = false;
        // [2026-10-31] 猎人计时器窗口诊断（`hunter_diag=1` 才开，2 秒一条）：取证"为什么没读稳固"
        uint32 hunterDiagAt = 0;
        // [2026-10-31 实测结论] 本核心 **不递减** `m_attackTimer[RANGED_ATTACK]`
        //   （Unit::Update 只递减 BASE_ATTACK/OFF_ATTACK）⇒ getAttackTimer(RANGED_ATTACK)
        //   不会随时间下降，不能当"下一发平射还剩多久"。真实节奏由两个核心事实决定：
        //     ① 自动射击的冷却 = Player::AddCooldown 里 `RecoveryTime(0) + UNIT_FIELD_RANGEDATTACKTIME(2000)`；
        //     ② 每次施法/移动打断自动射击后重挂，核心 `_UpdateAutoRepeatSpell` 先走 0.5s 起手。
        //   所以这里用 `botSpellCounts[75]`（核心每发自动射击都会自增）反推"上一发时刻/观测周期"。
        uint32 autoShotCount = 0;
        uint32 autoShotAt = 0;
        uint32 autoShotInterval = 2000;
        bool autoRepeatWasPresent = false;
        uint32 engRuns = 0;
        uint32 dupDebuff = 0;       // [2026-10-30] 同类 debuff 已被更强/相等效果占住时仍施放的次数（互斥分析用）         // 引擎真正跑过的 tick 数（诊断白字缺口用）
        uint32 movTicks = 0;        // 在移动的 tick 数（移动会打断挥击）
        uint32 skipTicks = 0;       // 因为状态机放了技能而让位给状态机的 tick 数      // "需要背后但没站好"只记一次      // [2026-10-30] "策略里点名但没学会"的日志只打一次             // 本 tick 状态机是否真的放出了技能
        time_t lastEngageLog = 0;              // "咬不上去"日志节流
        time_t lastBehindTry = 0;              // 上一次尝试绕背的时间（别每 tick 都动）
        time_t lastCloseTry = 0;               // 上一次"真够不着就贴上去"的时间
        time_t lastConsLog = 0;                // "自身中心 AoE 放不出"日志节流
        time_t lastPotionTry = 0;              // 上一次尝试喝药水的时间
        time_t lastSummary = 0;
        uint32 castCount = 0;
        std::map<std::string, time_t> prereqLast;   // [2026-10-31] 前置动作按名字记上次施放时间
                                                    //   （图腾必须 60 秒内不重铺；靠 spell id 不可靠）
        uint32 stuckSpellId = 0;      // [2026-10-31] 读条看门狗：当前卡住的法术
        time_t stuckSince = 0;        //                       开始卡住的时间

        uint32 tickCount = 0;
        // [2026-10-31 站长口径] 组动作（macro）的执行状态：价值引擎选中"整组"之后，
        //   这里只负责把选中的那组一步步走完。选择仍然只有价值引擎一处。
        int planId = 0;          // 0=无；1=奥法 3+3（legacy，DpsPolicy.inc）；2=奥法 3+3（mode=5，本文件）
        int planStep = 0;
        // [2026-10-31 站长口径] mode=5 的奥法 3+3：这一轮计划**打的是谁**（换目标/目标死 ⇒ 整组重来）。
        ObjectGuid planTarget;
        uint32 lastArcLog = 0;   // [DPSFSM-ARC] 诊断节流（WorldTimer 毫秒，10 秒一条）
        // [2026-10-31 站长口径] [DPSFSM-ARC] 的本轮窗口计数（打一条清一次）：
        //   arcHolds = 本窗口"有意空着"的 tick 数，arcCasts = 本窗口成功施法数。
        //   空转 GCD 是奥法 dps 的命门（60 秒 27 次 vs 39 次），没有这两个数就只能靠猜。
        uint32 arcHolds = 0;
        uint32 arcCasts = 0;
        // [2026-10-31 站长口径] `arc_diag=1` 时的分支取证（默认关）：本 tick 走了规则表里的哪一条。
        //   'S'=叠层 'F'=寒冰箭 'I'=瞬发 'B'=卡秒奥冲 'M'=蓝不足交回 'H'=Hold 'G'=读条中 '?'=其它状态
        //   [2026-11-01 站长口径] 追加 'C' = 其它读条填充（灼烧 / 奥术飞弹，由填充候选表选出）
        char arcBranch = '?';
        uint32 arcDiagAt = 0;    // 取证日志节流（1 秒一条）
        uint32 lastGemTry = 0;   // 吃法力宝石的节流（没有宝石/还在 CD 时别每 tick 走一遍引擎）
        // [2026-10-31 站长口径] 有蓝职业（奥法/冰法）的耗蓝账本：每次**施法成功**累加 SpellEntry->manaCost。
        //   只写统计、不参与任何决策（改不了施法选择 ⇒ mode=1/2/3/4 的行为不变，只是多一条 [DPSFSM-MANA]）。
        uint64 manaSpent = 0;    // 开测以来累计法力消耗（技能面板消耗，不含 5 秒规则回蓝/药水回复）
        std::map<uint32, uint64> manaCounts; // 上次结算时 bot->botSpellCounts 的快照（id → 成功次数）
        uint32 lastManaLog = 0;  // [DPSFSM-MANA] 日志节流（WorldTimer 毫秒，10 秒一条）
        uint64 lastManaSpent = 0;// 上次打日志时的累计值（便于看出这一段涨了多少，也避免"没变化还刷屏"）
        // [2026-11-01 站长口径] [DPSFSM-FILL] 填充筛选日志的节流（WorldTimer 毫秒，10 秒一条）：
        //   要回答的是"这一手填充为什么是它"——窗口 W / 实际挑中 / 通过过滤的候选数。
        uint32 lastFillLog = 0;
        // [2026-11-01 站长口径] `fill_dbg=1` 时候选读条日志的节流（1 秒一条，默认关）：
        //   验收要看的是"寒冰箭 cast≈2500"（旧口径 3000，丢了 SPELLMOD_CASTING_TIME）。
        uint32 lastFillDbg = 0;
        // [2026-11-01 站长口径] 术士·毁灭（mode=5）专用：
        //   lastWarlockLog —— [DPSFSM-WARLOCK] 的 10 秒节流（WorldTimer 毫秒）。
        //   lastCastId     —— 上一次**成功**施放的法术 id（日志里的 last=；CastChecked 成功路径统一记，
        //                      对所有职业只是多写一个字段，不改变任何决策）。
        //   engagedAt      —— 本次交战开始的毫秒时刻：灵魂碎裂的 shatter_min_engage_ms 用它判断战斗时长
        //                      （没有目标就清零 ⇒ 下一场重新计，碎裂每场只能交一次）。
        //   shatterUsed    —— 本场战斗已经交过灵魂碎裂（长 CD：开局就交等于把牌打空）。
        //   dot*           —— "目标预期存活"的采样（同一目标的血量斜率，见 WarlockDotLifetimeSec）。
        uint32 lastWarlockLog = 0;
        uint32 lastCastId = 0;
        uint32 engagedAt = 0;
        bool shatterUsed = false;
        ObjectGuid dotTarget;
        float dotPrevHp = 100.0f;
        uint32 dotPrevAt = 0;
        int dotLastLifeSec = -1;
        float wlManaGap = 0.0f;   // 本 tick 的原始法力缺口（max - cur），[DPSFSM-WARLOCK] 与 life tap 共用
        // [2026-11-01 站长口径] 术士·痛苦/恶魔（mode=5，与毁灭共用 TickWarlockFsmCore）专用：
        //   wlDots    —— 当前目标身上这套 DoT/诅咒已挂几个（[DPSFSM-WARLOCK] 的 dots=，只统计不决策）。
        //   wlMulti   —— 本 tick 参与多目标铺场的敌人数（multidot_max 截断后；单目标恒 1，日志 multi=）。
        //   demoPetAt —— 宠物**第一次被看见在场**的毫秒时刻：恶魔牺牲的"不是刚召唤的"门用它，
        //                宠物不在场就归零（重召后从头计时，不会把召唤的 GCD 白扔）。
        int wlDots = 0;
        int wlMulti = 1;
        uint32 demoPetAt = 0;
        // [2026-11-01 实测修正] 诅咒位的重放保护：放出去但读不到目标 debuff 时累计次数，
        //   连续 2 次仍读不到就退避 30 秒（实测一轮 18 次诅咒空转、dps 205、dots 恒 0）。
        int wlCurseTries = 0;
        uint32 wlCurseSkipUntil = 0;
        // [2026-11-01 站长口径] 牧师·暗影（mode=5，**GCD 时间轮**）专用：
        //   spBeat / spBeatAt —— 拍钟：`spBeat` 是"现在是第几拍"，`spBeatAt` 是**下一拍开始**的毫秒时刻。
        //                        一拍 = 一个 GCD(1.5s)；引导技（精神鞭笞 3s）额外再吃一拍。
        //   spMbReadyAt       —— 心爆"记账"的可再用时刻：核心没有"还剩多少毫秒"的公开接口
        //                        （`IsSpellReady` 只给 bool），轮上要算"距心爆还有几拍"就只能自己记；
        //                        CD 长度取自模板 `CategoryRecoveryTime` 经 `SPELLMOD_COOLDOWN`
        //                        （强化心爆天赋）修正，与核心 `Player::AddCooldown` 同一条判据。
        //                        **能不能放仍由 `Ready()` 决定**，记账只用于排拍。
        //   spWheelBeat       —— 上一次打 `[DPSFSM-WHEEL]` 的拍号（同一次撞车只报一行）。
        //   spDots / spBeatAct —— `[DPSFSM-PRIEST]` 的 dots=（当前目标挂了几个 DoT）与 beats=（本拍动作）。
        uint32 lastPriestLog = 0;
        int    spBeat = 0;
        uint32 spBeatAt = 0;
        uint32 spMbReadyAt = 0;
        int    spWheelBeat = -1;
        int    spDots = 0;
        char const* spBeatAct = "-";
        //   spDiagAt       —— [DPSFSM-PRIEST-DIAG] 的 10 秒节流（`sp_diag=1` 才打）。
        // [2026-11-01 站长口径] **`ids` 缓存体检提到通用层**（见 SpellId() 上面的 IdsHealthTick）：
        //   原先是暗牧路径专用的 `spIdsCheckedAt`，只救得了暗牧；同一个病（解析失败把 0 永久
        //   缓存 ⇒ 某个技能整场不放）存在于全部 20 个 DPS 专精，所以在通用层做。字段：
        //     idsCheckedAt —— 轮转体检的 5 秒节流（WorldTimer 毫秒；与 idsNegAt 分开计）；
        //     idsCursor    —— 下一次要检查的条目下标（**一次只查一条**，绝不全表扫描）；
        //     idsNegAt     —— 负缓存（解析成 0）**立刻重解析**的 5 秒节流；
        //     lastIdsLog   —— `[DPSFSM-IDS] <bot> reparse <name>` 的 5 秒节流（验收要看，但别刷屏）；
        //     holdIntent   —— 本 tick 专精**显式登记**的"有意空着"原因（`HoldWait()`）；
        //                     Tick() 的 Hold 通用兜底读它：没登记且 GCD 没锁 ⇒ 降级 Decline。
        uint32 idsCheckedAt = 0;
        uint32 idsCursor = 0;
        uint32 idsNegAt = 0;
        uint32 lastIdsLog = 0;
        char const* holdIntent = nullptr;
        uint32 spDiagAt = 0;
        // [2026-11-01 站长口径] 法师·火法（mode=5）专用（详见 DpsFsm.cpp 的“法师·火法”一节）：
        //   fireFbSinceScorch —— 自上次补灼烧以来**放出的火球数**：维护节奏的**主**判据
        //                        （叠满后每 scorch_refresh_fireballs(8) 发火球补一次 ≈ 24 秒）；
        //   fireScorchTries   —— 已放过几次灼烧（配合"从没见过 debuff"判定没有强化灼烧天赋）；
        //   fireScorchSeen    —— 真的见过层数 > 0（天赋在，debuff 会挂上）⇒ 静态事实缓存，不跨轮清；
        //   fireScorchPending / Before —— 刚补过一发灼烧、还没核对（被抵抗的判据）；
        //   fireScorchForce   —— 抵抗后的"**立刻**再补一次"（下一手直接补，不再等 8 个火球）；
        //   fireScorchResent  —— 本轮维护已经重补过一次（重补只做一次，不退化成链）；
        //   fireFlameToggle   —— 烈焰风暴 7 级 / 6 级交替计数（两级地面火池互不覆盖）；
        //   fireBurstDone     —— 三套爆发表里已经开过的轮次（位掩码：1 起手 / 2 尾轮 / 4 斩杀 / 8 急冷）；
        //   fireBurstRound / Want* —— 当前这一轮要放哪些牌（Burst 状态里按它出手）。
        uint32 lastFireLog = 0;
        int    fireFbSinceScorch = 0;
        int    fireScorchTries = 0;
        bool   fireScorchSeen = false;
        bool   fireScorchPending = false;
        int    fireScorchBefore = 0;
        bool   fireScorchForce = false;
        bool   fireScorchResent = false;
        int    fireFlameToggle = 0;
        int    fireBurstDone = 0;
        int    fireBurstRound = 0;
        bool   fireBurstWantIcy = false;
        bool   fireBurstWantComb = false;
        bool   fireBurstWantTrinket = false;
        // [2026-11-01 站长口径] 圣骑士·惩戒（mode=5）专用：
        //   retPrevSwingMs —— 上一次读到的 BASE 平砍计时（毫秒）。核心 `Unit::Update` 每 tick 递减
        //                     `m_attackTimer[BASE_ATTACK]`，平砍成功后会 `resetAttackTimer()` 跳回攻速
        //                     ⇒ "读数突然变大（> 旧读数 + 800ms）"就是刚过了一刀；用它记平砍数，
        //                     不维护任何影子计时器（RANGED 那个死值更不能用）。
        //   retSwings      —— 距上次圣印舞已经过去的平砍数（twist_every_swings 的输入）。
        //   retTwistCount  —— 本场累计舞次数（[DPSFSM-RET] 的 twists=，验收"真的切了"）。
        //   retEngagedAt   —— 本场交战开始的毫秒时刻（fight_length_est / burst_delay 用它判断"开打多久"）。
        //   retBurstUntil  —— 本轮爆发的结束时刻：窗口内保持 Burst，让"复仇之怒 → 饰品 → 药水"跨几个 tick 放完。
        //   retNeedSeal   —— 审判刚放完、下一轮圣印必须立刻补（审判会吃掉当前圣印）。
        //   retRestoreSeal—— 刚舞过（当前挂的是鲜血/正义），要回到命令圣印；只在**非窗口**时回切，
        //                     否则回切本身也会在 400ms 内触发一次双印，破坏"2 刀 1 舞"的节奏。
        //   retTarget      —— 本场的目标；换目标 = 换一场战斗（计数/计时全清）。
        uint32 lastRetLog = 0;
        uint32 retSwings = 0;
        uint32 retTwistCount = 0;
        int    retPrevSwingMs = -1;
        uint32 retEngagedAt = 0;
        uint32 retBurstUntil = 0;
        bool   retNeedSeal = false;
        bool   retRestoreSeal = false;
        ObjectGuid retTarget;
        // [2026-11-01 站长口径] 萨满·图腾子系统（元素/增强共用）—— **自己记账，不看槽位**。
        //   核心事实（KNOWN_ISSUES §10.53 实证）：施放图腾后 `GetTotemGuid(slot)` 仍是空的
        //   （召唤走 SPELL_EFFECT_SUMMON，没登记成玩家的图腾槽位）⇒ 一旦按槽位判空，
        //   每 tick 都认为"这槽没图腾"，GCD 全喂给图腾（实测 25 秒 20 次法力之泉、元素萨 dps 0~250）。
        //   ⇒ 每个槽登记三样：**什么时候插的**（totemCastAt）、**插的是哪个法术**（totemEntry，
        //     用来识别"换图腾组"）、**插在哪里**（totemPos，队伍移动 > totem_range 就重插）。
        //   totemExpireAt = totemCastAt + 该法术的 Duration（拿不到按 120 秒兜底），
        //   剩余 < totem_refresh_ms(3000) 就补 —— 四条判据见 DpsFsm.cpp → ShTotemNeed()。
        uint32 totemCastAt[MAX_TOTEM_SLOT] = {};
        uint32 totemEntry[MAX_TOTEM_SLOT] = {};
        uint32 totemExpireAt[MAX_TOTEM_SLOT] = {};
        float  totemPos[MAX_TOTEM_SLOT][3] = {};
        // 图腾舞（增强，可选）：空气槽 风怒 → 立刻优雅 → twist_period_ms(10s) 后换回风怒。
        //   phase 0=该挂风怒 / 1=风怒已挂、等切优雅 / 2=优雅期、等切回。
        int    twistPhase = 0;
        uint32 twistNextAt = 0;
        // 萨满诊断/验收字段（只用于日志，不参与决策）：
        uint32 lastShLog = 0;          // [DPSFSM-SH] 10 秒节流
        uint32 lastShDiag = 0;         // [DPSFSM-SH-DIAG] 10 秒节流（sh_diag=1 才打）
        bool   shClOk = false;         // 这一手是否走"闪电链优先"（蓝 ≥ cl_mana_min）
        int    shLightningShield = 0;  // 闪电盾层数（AuraStacks/charge 取大，见 ShLightningShieldStacks）
        uint32 lsCastAt = 0;           // 上一次补闪电盾的时刻（防"引擎水盾 ↔ 我方闪电盾"互顶时紧着补）
        uint32 shEarthShocks = 0;      // 本场地震击次数（层数达标才震）
        uint32 shTotemCasts = 0;       // 本场补图腾次数（验收"图腾真的在补"）
        // 本场（= 同一个目标）的一次性开手：Aoe 起手只放一次火元素新星；开门准备只做一次
        bool   shAoeNova = false;
        ObjectGuid shTarget;
        uint32 shEngagedAt = 0;
        // 风怒武器 3 秒 ICD 的**观测值**（核心不暴露 proc 冷却，只能看 proc 法术的冷却边沿；
        //   -1 = 还没观测到 / 没附风怒武器）。只做日志，不做微延迟对齐。
        int    wfIcdMs = -1;
        int    wfLastProcAt = 0;
        uint32 wfProcs = 0;
        bool   wfReadyPrev = true;
        // [2026-11-01 站长口径] 德鲁伊 · 野性（猫，mode=5）—— **能量跳相位的排程机**专用记忆。
        //   状态的核心不是"现在有多少能量"，而是"距离下一跳回能还有多久"（能量 2 秒一跳、20 点）。
        //   核心**没有** `m_regenTimer` 的公开接口（Unit.h 里是 protected）⇒ 相位只能观测：
        //   本 tick 的能量比上一 tick 多、且增量是"一跳的量"（5~50）⇒ 记一次回能跳。
        //     drTickAt     —— 最近一次观测到回能跳的毫秒时刻；
        //     drTickPeriod —— 观测到的跳间隔（默认 2000ms；假人服 Rate.Energy 不是 1 时以观测为准）；
        //     drEnergyPrev —— 上一 tick 的能量读数（跳的检测用）；
        //     drShifts     —— 本场成功变身次数（[DPSFSM-DRUID] 的 powershifts=，验收"真的变了"）；
        //     drFuror      —— 有没有"激怒"（Furor）天赋：**没有就不变身**（变身会把能量清零、却不给 40）；
        //                     判据与核心同一条：被动 dummy aura 的 SpellIconID == 238；
        //     drShiftAt    —— 上一次变身的毫秒时刻（形态 1 秒冷却/日志用）。
        uint32 drTickAt = 0;
        int    drTickPeriod = 2000;
        int    drEnergyPrev = -1;
        uint32 drShifts = 0;
        uint32 drLastLog = 0;
        bool   drFuror = false;
        bool   drFurorChecked = false;
        uint32 drShiftAt = 0;
        ObjectGuid drTarget;
        uint32 drEngagedAt = 0;
        int    drFurorLoggedPeriod = 0;   // 首次确定 furor/period 的诊断只打一次
        uint32 drDiagAt = 0;              // `dr_diag=1` 时"等一跳/变身"判据的取证节流（2 秒一条）
        // [2026-11-01 实测] `powershift_min_mana_pct`（默认 35）的判据结果：
        //   1 = 变完还剩 >= 该比例（允许变身）/ 0 = 会被蓝量门挡住（这次改成等一跳）。
        //   只用于 `[DPSFSM-DRUID] ... shiftGate=` 的 A/B 取证，不参与别的决策。
        int    drShiftGate = 0;
        // [2026-11-01 站长口径] **治疗（HealCore，四治疗共用）** —— 只装"病人状态 + 统计"，
        //   配置（技能名/阈值）在 HealCfgFor() 那张表里，不在这里。
        //   healWatch   —— guid → 受伤速率窗口（预读的唯一输入，见 HealWatch 注释）；
        //   healPending —— 已经放出去、还没落地的**一次**治疗（治疗都是单目标，不需要队列）；
        //   healCasts/healEff/healTheory/healOver —— 本场累计（验收数字，只写不参与决策）；
        //   hps*Win     —— 最近 10 秒窗口的累计（[DPSFSM-HPS] 用，打一条清一次）；
        //   lastHealLog / lastHpsLog —— 两条日志各自的节流（WorldTimer 毫秒）；
        //   healTargetGuid —— 本 tick 选中的病人（只用于日志/诊断，不跨 tick 做决策）。
        std::map<uint32, HealWatch> healWatch;
        HealPending healPending;
        uint32 healCasts = 0;
        uint32 healEff = 0;
        uint32 healTheory = 0;
        uint32 healOver = 0;
        uint32 hpsWinStart = 0;
        uint32 hpsWinEff = 0;
        uint32 hpsWinTheory = 0;
        uint32 hpsWinOver = 0;
        uint32 hpsWinCasts = 0;
        uint32 lastHealLog = 0;
        uint32 lastHpsLog = 0;
        uint32 healTargetGuid = 0;
    };

    std::mutex s_disabledMutex;
    std::set<uint32> s_disabled;      // A/B 对照：这些 guid 这一轮不走状态机
    std::set<uint32> s_forceDps;      // 假人测试：强制按 DPS 打（忽略坦/奶角色门）
    std::map<uint32, std::set<ObjectGuid>> s_scenarioTargets;   // guid → 这一轮"自己的"假人
    std::map<uint32, std::map<std::string, double>> s_overrides; // guid → 本轮参数覆盖（调优并发用）
    std::map<uint32, uint32> s_scenarioRun;                      // guid → 登记它的 runId
    // [2026-11-01 站长口径] guid → 已经做过"轮间重置"的 runId。单独一张表而不是复用 s_scenarioRun：
    //   s_scenarioRun 在 ClearScenarioTargets / 空目标集时会被抹掉，抹掉后再登记同一个 runId 会
    //   被误判成"新一轮"而二次重置（多打一条 [DPSFSM-RESET]）。runId 单调递增，这张表只增不减。
    std::map<uint32, uint32> s_resetRun;
    // [2026-11-01 站长口径] 治疗场景：治疗者 guid → 病人集合 / 主坦 guid / 登记它的 runId。
    //   与 s_scenarioTargets 分开，是因为语义不同（"要治谁" ≠ "要打谁"），而且治疗者
    //   往往**没有**敌对目标 —— 复用同一张表会让 EngageScenarioTarget 把治疗者拽去咬人。
    std::map<uint32, std::set<ObjectGuid>> s_healTargets;
    std::map<uint32, uint32> s_healTank;
    std::map<uint32, uint32> s_healRun;
    std::map<uint32, uint32> s_petLogAt;                         // [2026-10-31] guid → 上次打 [DPSFSM-PET] 周期日志的毫秒时刻
    std::map<uint32, uint32> s_petStuckAt;                       // [2026-10-31] guid → "在追但贴不上且没在动"的起始毫秒时刻

    std::mutex s_memMutex;
    std::unordered_map<uint32, std::unique_ptr<BotMem>> s_mem;

    BotMem* Mem(uint32 guid)
    {
        std::lock_guard<std::mutex> lock(s_memMutex);
        std::unique_ptr<BotMem>& slot = s_mem[guid];
        if (!slot)
        {
            slot.reset(new BotMem());
            slot->guid = guid;
        }
        return slot.get();
    }

    // ---- [2026-10-29 站长口径] 离线调优：参数热加载，脚本改文件即可，不用重编译/重启 ----
    //   文件格式�? 注释）：<scope>:<key>=<value>；scope = 专精名（fire/fury/...）或 * 表示全部
    struct TuneStore
    {
        time_t mtime = 0;
        time_t checkedAt = 0;
        std::map<std::string, double> vals;
    };
    std::mutex s_tuneMutex;
    TuneStore s_tune;

    std::string Trim(std::string s)
    {
        size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return "";
        size_t e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    }

    void ReloadTune()
    {
        time_t const now = time(nullptr);
        if (now - s_tune.checkedAt < 5)
            return;
        s_tune.checkedAt = now;

        std::string const path = sPlayerbotAIConfig.dpsFsmTuneFile.empty()
            ? std::string("dpsfsm_tune.conf") : sPlayerbotAIConfig.dpsFsmTuneFile;

        struct stat st;
        if (stat(path.c_str(), &st) != 0 || st.st_mtime == s_tune.mtime)
            return;
        s_tune.mtime = st.st_mtime;

        std::ifstream in(path.c_str());
        if (!in)
            return;

        std::map<std::string, double> vals;
        std::string line;
        while (std::getline(in, line))
        {
            size_t const hash = line.find('#');
            if (hash != std::string::npos) line = line.substr(0, hash);
            size_t const eq = line.find('=');
            size_t const colon = line.find(':');
            if (eq == std::string::npos || colon == std::string::npos || colon > eq)
                continue;
            std::string const scope = Trim(line.substr(0, colon));
            std::string const key = Trim(line.substr(colon + 1, eq - colon - 1));
            if (scope.empty() || key.empty())
                continue;
            vals[scope + ":" + key] = atof(line.substr(eq + 1).c_str());
        }

        std::lock_guard<std::mutex> lock(s_tuneMutex);
        s_tune.vals.swap(vals);
        sLog.outError("[DPSFSM] 调优参数已加载 %u 项（%s）", (uint32)s_tune.vals.size(), path.c_str());
    }

    double TuneForBot(BotMem* m, char const* key, double def);

    // [2026-10-31 根因] conf 里的参数前缀用的是**短名**（destro/bm/marks/enhance/ret），
    //   而 `SpecName()` 返回的是**长名**（destruction/beast mastery/marksmanship/enhancement/retribution）
    //   ⇒ 这 5 个专精的 conf 参数（包括 `policy=1`）**从来没有被读到过**，一直跑旧的硬编码轮转。
    //   实测证据：毁灭术 HB 里 `pol=0`、casts 里全是 TickWarlock 的献祭/腐蚀/燃烧/痛苦诅咒。
    //   这里做一次别名映射，两个名字都能命中（新配置写短名、老配置写长名都不会踩空）。
    static char const* TuneAlias(char const* spec)
    {
        if (!spec)
            return spec;
        if (!strcmp(spec, "destruction"))   return "destro";
        if (!strcmp(spec, "beast mastery")) return "bm";
        if (!strcmp(spec, "marksmanship"))  return "marks";
        if (!strcmp(spec, "enhancement"))   return "enhance";
        if (!strcmp(spec, "retribution"))   return "ret";
        return spec;
    }

    double Tune(char const* spec, char const* key, double def)
    {
        std::lock_guard<std::mutex> lock(s_tuneMutex);
        auto it = s_tune.vals.find(std::string(spec) + ":" + key);
        if (it != s_tune.vals.end())
            return it->second;
        char const* alias = TuneAlias(spec);
        if (alias && strcmp(alias, spec) != 0)
        {
            it = s_tune.vals.find(std::string(alias) + ":" + key);
            if (it != s_tune.vals.end())
                return it->second;
        }
        it = s_tune.vals.find(std::string("*:") + key);
        if (it != s_tune.vals.end())
            return it->second;
        return def;
    }

    double TuneForBot(BotMem* m, char const* key, double def)
    {
        // 先看"这只 bot 自己的覆盖"（同一轮里不同 bot 可以跑不同参数值）
        {
            std::lock_guard<std::mutex> lock(s_disabledMutex);
            auto it = s_overrides.find(m->guid);
            if (it != s_overrides.end())
            {
                auto kv = it->second.find(key);
                if (kv != it->second.end())
                    return kv->second;
            }
        }
        return Tune(ai::dpsfsm::SpecName(m->spec), key, def);
    }

    double TP(BotMem* m, char const* key, double def)
    {
        return TuneForBot(m, key, def);
    }

    // ==========================================================================
    // [2026-10-31 S1] 决策模式：**一个键取代四个布尔**
    //   旧口径 value / policy / pselect / pauto 是历史叠加出来的：
    //     - pselect 只在 policy=1 时被读；pauto 又只在 pselect=1 时被读
    //       ⇒ 4 个布尔理论 16 态，实际有意义的只有 4 种（另有 value 引擎 1 种）。
    //     - 典型哑弹：pauto=1 但 pselect=0 完全不生效；policy 没开时 pselect/pauto 全废。
    //   新口径只有一个键 `mode`（数值，便于 Optuna 当 categorical 采样）：
    //     0 legacy            手写 Tick<Class>（现默认）
    //     1 table             策略表，顺序"第一条能放就放"
    //     2 table_scored      策略表 + 表内两遍打分
    //     3 table_scored_book 2 + 法术书表外候选（w_offtable 打折后参与同一把尺子）
    //     4 value             价值引擎（TickValue）
    //   旧四键仍会被读一次做**自动迁移**，并打一条 WARN 告诉该写 `mode=N`；
    //   tuner 的 `k=v` 里也常直接带旧键，所以兼容层保留到调参脚本改完再删。
    // 详见 dev/DESIGN_dps_fsm.md §1。
    // ==========================================================================
    // 名字避开 ai::Engine（经典策略引擎），别让两个 Engine 在同一命名空间打架。
    enum class DecisionMode { Legacy = 0, Table = 1, TableScored = 2, TableScoredBook = 3, Value = 4, Fsm = 5 };

    char const* DecisionModeName(DecisionMode m)
    {
        switch (m)
        {
            case DecisionMode::Table:           return "table";
            case DecisionMode::TableScored:     return "table_scored";
            case DecisionMode::TableScoredBook: return "table_scored_book";
            case DecisionMode::Value:           return "value";
            case DecisionMode::Fsm:             return "fsm";
            default:                            return "legacy";
        }
    }

    bool ModeIsTable(DecisionMode m)
    {
        return m == DecisionMode::Table || m == DecisionMode::TableScored || m == DecisionMode::TableScoredBook;
    }

    bool ModeIsScored(DecisionMode m)
    {
        return m == DecisionMode::TableScored || m == DecisionMode::TableScoredBook;
    }

    bool ModeIsBook(DecisionMode m)
    {
        return m == DecisionMode::TableScoredBook;
    }

    // 解析顺序：新键 `mode` 优先；没有它才从旧四键推导（并警告一次，告诉你该写哪个 mode）。
    DecisionMode ResolveMode(BotMem* m, bool& derivedFromLegacy)
    {
        derivedFromLegacy = false;

        double const raw = TP(m, "mode", -1.0);     // -1 = 没配
        if (raw >= 0.0)
        {
            int const v = int(raw + 0.5);
            if (v >= 0 && v <= 5)
                return DecisionMode(v);
            sLog.outError("[DPSFSM-MODE] mode=%d 越界（合法 0..5），按 legacy 处理", v);
            return DecisionMode::Legacy;
        }

        // ---- 旧口径兼容（derive 一次，行为与旧代码逐一等价）----
        bool const value   = TP(m, "value",   0.0) >= 1.0;
        bool const policy  = TP(m, "policy",  0.0) >= 1.0;
        bool const pselect = TP(m, "pselect", 0.0) >= 1.0;
        bool const pauto   = TP(m, "pauto",   0.0) >= 1.0;

        if (!value && !policy && !pselect && !pauto)
            return DecisionMode::Legacy;        // 什么都没配 = 原默认，不必警告

        derivedFromLegacy = true;
        if (value)
            return DecisionMode::Value;         // 旧代码里 value 先跑、失败后还能落 policy；见警告

        // 旧代码的 nested 读法：pauto 只在 pselect=1 时才被读
        if (policy)
        {
            if (!pselect)
                return DecisionMode::Table;     // pselect=0 ⇒ pauto 配了也不生效
            return pauto ? DecisionMode::TableScoredBook : DecisionMode::TableScored;
        }
        return DecisionMode::Legacy;            // 只有 pselect/pauto、没有 policy ⇒ 原来完全无效
    }

    int   AoEPack(BotMem* m)  { return int(TP(m, "aoe_pack", 3.0)); }
    float AoERange(BotMem* m) { return float(TP(m, "aoe_range", 11.0)); }
    float BurstHp(BotMem* m)  { return float(TP(m, "burst_hp", 20.0)); }

    // 名字 → id：先问引擎（它已经处理了中英文/DBC 名称匹配），再顶到"已学会的最高 rank"。

    // 省蓝等级 > 1 时引擎会给最低 rank，所以这里一定要自己往上爬 spell_chain。

    // [2026-10-30 修] 兜底：直接按名字扫 bot 自己的法术表，取**最高 rank**。
    //   为什么需要它：引擎的 "spell id" 值（SpellIdValue）会过滤 `IsPassiveSpell`，而姿态(战斗/狂暴姿态)、
    //   图腾这类"形态/召唤"法术正是被过滤掉的一类 ⇒ 值返回 0 ⇒ 状态机整条技能表静默失效
    //   （实测：狂暴战整场 0 次旋风斩，日志只有一句"没有可用技能 'berserker stance'"）。
    uint32 ScanSpellbookByName(Player* bot, char const* name)
    {
        uint32 best = 0;
        for (PlayerSpellMap::const_iterator itr = bot->GetSpellMap().begin(); itr != bot->GetSpellMap().end(); ++itr)
        {
            uint32 const id = itr->first;
            if (itr->second.state == PLAYERSPELL_REMOVED || itr->second.disabled)
                continue;
            SpellEntry const* entry = sServerFacade.LookupSpellInfo(id);
            if (!entry || !entry->SpellName[0])
                continue;
            if (strcasecmp(entry->SpellName[0], name) != 0)
                continue;
            if (!best || sSpellMgr.IsSpellHigherRankOfSpell(id, best))
                best = id;
        }
        return best;
    }

    // ==================================================================================
    // [2026-11-01 站长口径] **法术缓存体检（通用层）** —— 三个"整轮静默报废"的病之一。
    //
    //   现状：`BotMem::ids` 是"名字 → id"的**永久**缓存，`SpellId()` 解析失败时缓存的是 **0**
    //   （负缓存）。假人场景每轮 respec（`arms`/`dummy` 的 respec=1），若某次解析正好落在
    //   "天赋正在重排、法术书是空的"那一瞬，0 就被永久记下来 ⇒ 之后整轮（甚至整场）都认为
    //   "没学会这个法术" ⇒ 该技能永远不放。暗牧那轮已经加过 5 秒一次的体检，但**只在暗牧路径**。
    //   现在提到这里，对所有专精生效（`SpellId()` 是唯一入口）。
    //
    //   两条闸，保证开销可控（绝不每 tick 全表扫描）：
    //     ① **轮转体检** `IdsHealthTick()`：5 秒节流，每次**只查一条**（`idsCursor` 轮转），
    //        判据 = "缓存值 0" 或 "该 id 已不在法术书"（respec 换了 rank / 丢了法术）⇒ 删掉重解析；
    //     ② **负缓存即时修** `SpellId()`：命中缓存值是 0 时，5 秒节流后立刻删掉重解析一次
    //        （轮转要等好几窗口才轮到，整轮 45 秒里很可能永远等不到）。
    //   两条路径都打 `[DPSFSM-IDS] <bot> reparse <name>`（5 秒节流）—— 验收就是看它在跑。
    // ==================================================================================
    void IdsNoteReparse(BotMem* m, Player* bot, char const* name, uint32 safeNow)
    {
        if (!m || !bot || !name)
            return;
        if (m->lastIdsLog && WorldTimer::getMSTimeDiff(m->lastIdsLog, safeNow) < 5000)
            return;
        m->lastIdsLog = safeNow;
        sLog.outError("[DPSFSM-IDS] %s reparse %s", bot->GetName(), name);
    }

    // 轮转体检：5 秒一次、一次只查一条（成本可忽略）。被删掉的条目下一 tick 由 SpellId() 重解析。
    void IdsHealthTick(Player* bot, BotMem* m)
    {
        if (!m || !bot || !bot->IsInWorld() || m->ids.empty())
            return;
        uint32 const nowMs = WorldTimer::getMSTime();
        uint32 const safeNow = nowMs ? nowMs : 1;
        if (m->idsCheckedAt && WorldTimer::getMSTimeDiff(m->idsCheckedAt, safeNow) < 5000)
            return;
        m->idsCheckedAt = safeNow;

        uint32 idx = m->idsCursor;
        if (idx >= uint32(m->ids.size()))
            idx = 0;
        std::map<std::string, uint32>::iterator it = m->ids.begin();
        for (uint32 i = 0; i < idx && it != m->ids.end(); ++i)
            ++it;
        if (it == m->ids.end())
            return;
        // 游标前进一格（下个窗口查下一条）。条目被删时游标归零：表变了，从头来更稳。
        m->idsCursor = idx + 1;

        if (it->second && bot->HasSpell(it->second))
            return;                     // 正缓存且仍在法术书：这条是健康的

        std::string const name = it->first;
        m->ids.erase(it);
        m->idsCursor = 0;
        IdsNoteReparse(m, bot, name.c_str(), safeNow);
    }

    uint32 SpellId(PlayerbotAI* ai, Player* bot, BotMem* m, char const* name, bool warn = true)
    {
        // [2026-11-01 站长口径] 体检放在**唯一入口**：任何专精第一次问某个名字都会顺带体检一条。
        IdsHealthTick(bot, m);

        auto it = m->ids.find(name);
        if (it != m->ids.end())
        {
            if (it->second)
                return it->second;

            // 负缓存（解析成 0）：5 秒节流后**立刻**丢掉重解析一次（轮转体检太慢，等不到）。
            //   节流未到点就先按 0 返回（与改造前的行为一致，不会每 tick 去扫法术书）。
            uint32 const nowMs = WorldTimer::getMSTime();
            uint32 const safeNow = nowMs ? nowMs : 1;
            if (m->idsNegAt && WorldTimer::getMSTimeDiff(m->idsNegAt, safeNow) < 5000)
                return 0;
            m->idsNegAt = safeNow;
            m->ids.erase(it);
            IdsNoteReparse(m, bot, name, safeNow);
        }

        uint32 id = 0;
        if (auto* value = ai->GetAiObjectContext()->GetValue<uint32>("spell id", name))
            id = value->Get();

        if (!id)
            id = ScanSpellbookByName(bot, name);

        if (id)
        {
            for (uint32 next = sSpellMgr.GetNextSpellInChain(id); next; next = sSpellMgr.GetNextSpellInChain(next))
                if (bot->HasSpell(next))
                    id = next;
            if (!bot->HasSpell(id))
                id = 0;
        }

        m->ids[name] = id;
        if (!id && warn && m->unresolved.insert(name).second)
            sLog.outError("[DPSFSM] %s(%s) 没有可用技�?'%s'", bot->GetName(), ai::dpsfsm::SpecName(m->spec), name);
        return id;
    }

    // [2026-10-31 站长口径] **按指定 rank 取法术 id**：先拿到"已学会的最高 rank"（复用 SpellId），
    //   再顺着 cmangos 的法术链往回退到第 rank 级（rank 从 1 开始数）。
    //   为什么要它：省蓝/控场场景要的常常不是满级技能（1 级暴风雪减速效果一样、蓝耗低得多），
    //   而 SpellId 一律顶到最高 rank ⇒ 以前只能放满级的。
    //   兜底：该法术没有链（GetSpellChainNode 为空）或 rank 越界时，返回最高 rank 的 id，并只打一条日志
    //         （别每 tick 刷屏）。
    uint32 SpellIdAtRank(PlayerbotAI* ai, BotMem* m, char const* name, uint32 rank, bool warn = true)
    {
        Player* bot = ai->GetBot();
        uint32 const top = SpellId(ai, bot, m, name, warn);
        if (!top)
            return 0;

        if (rank < 1)
            rank = 1;

        // 链上没有这个法术（GetSpellChainNode 为空 ⇒ GetSpellRank 返回 0）：不是多 rank 技能，别退。
        SpellChainNode const* node = sSpellMgr.GetSpellChainNode(top);
        uint32 const topRank = node ? uint32(node->rank) : 0;
        if (!topRank)
        {
            if (rank > 1)
            {
                std::string const key = std::string(name) + "#nochain";
                if (m->rankWarned.insert(key).second)
                    sLog.outError("[DPSFSM-RANK] %s 技能 '%s' 没有 rank 链（id=%u），只能用最高 rank，忽略 rank=%u",
                        bot->GetName(), name, top, rank);
            }
            return top;
        }
        if (rank > topRank)
        {
            std::string const key = std::string(name) + "#oob";
            if (m->rankWarned.insert(key).second)
                sLog.outError("[DPSFSM-RANK] %s 技能 '%s' 只有 %u 级（最高 id=%u），rank=%u 越界，按最高 rank 放",
                    bot->GetName(), name, topRank, top, rank);
            return top;
        }

        // 往回退：GetPrevSpellInChain 一路走到第 rank 级（rank=1 走到链首）
        uint32 cur = top;
        uint32 curRank = topRank;
        while (curRank > rank)
        {
            uint32 const prev = sSpellMgr.GetPrevSpellInChain(cur);
            if (!prev)
                break;
            cur = prev;
            curRank = sSpellMgr.GetSpellRank(cur);
            if (!curRank)
                break;
        }
        return cur;
    }

    // [2026-10-29 站长口径] **有些技能不吃 GCD**（下一个平砍附带 / 无 GCD 的爆发技），
    //   不能因为"公共冷却没转完"就把它们跳过。命中这些名字时绕过 GCD 检查。
    bool NoGcd(char const* name)
    {
        static char const* kNOCD[] = {
            "heroic strike", "cleave", "maul", "swipe", "thunder clap",       // 下一次平砍附带
            "auto shot",                                                       // 自动射击
            "cold blood", "presence of mind", "arcane power", "icy veins",     // 法师/盗贼爆发
            "tiger's fury", "berserk", "kill command", "shamanistic rage",     // 德/猎/萨爆发
            "bloodlust", "heroism", "use trinket",
        };
        for (char const* k : kNOCD)
            if (strcmp(name, k) == 0)
                return true;
        return false;
    }

    // 自身 buff 的**剩余时间**（毫秒）：圣印 / 切割 / 节能施法这类都靠它做状态
    int AuraRemain(PlayerbotAI* ai, BotMem* m, Snapshot& s, char const* name)
    {
        uint32 id = SpellId(ai, s.bot, m, name, false);
        if (!id)
            return 0;
        SpellAuraHolder* holder = s.bot->GetSpellAuraHolder(id);
        if (!holder)
            return 0;
        int const dur = holder->GetAuraDuration();
        return dur < 0 ? 999999 : dur;    // 永久 buff
    }

    bool Ready(Player* bot, uint32 id)
    {
        // IsSpellReady 同时覆盖：技能自身冷�?/ 分类冷却 / 公共冷却 / 沉默封锁
        return id && bot->HasSpell(id) && bot->IsSpellReady(id);
    }

    bool GcdActive(Player* bot, uint32 id)
    {
        SpellEntry const* entry = id ? sServerFacade.LookupSpellInfo(id) : nullptr;
        return entry && bot->HasGCD(entry);
    }

    // [2026-11-01 站长口径] **这一 tick 的 GCD 是不是真的锁着**（Hold 通用兜底的判据）。
    //   两来源：
    //     ① `s.casting`：正在读条/引导（此时谁也插不了技能）；
    //     ② `WorldObject::HasGCD(nullptr)` = "GCD 分类表里还有条目没到期" ——
    //        核心 `WorldObject::UpdateCooldowns()` 每 tick 会把过期的 GCD 删掉，所以
    //        非空 ⇔ 确实还有一类 GCD 在转（`HasGCD(entry)` 是按 StartRecoveryCategory 查的同一张表）。
    //   为什么不用 `s.gcd`：那是 Snapshot 里的字段，只有奥法那条路径按"填充技"算过，
    //   别的专精恒 false ⇒ 拿它当兜底判据会让所有 Hold 都被降级，等于把猎人平射窗口直接废掉。
    bool GcdLocked(Snapshot& s)
    {
        if (s.casting)
            return true;
        return s.bot && s.bot->HasGCD(nullptr);
    }

    // [2026-11-01 站长口径] 专精**显式登记**"这一手有意空着"的唯一入口（真正放不出的场合别用）。
    //   只有"等资源 / 等相位 / 等平射窗口 / 等走位"这类**明知这一手放不出、但引擎插一脚会打乱
    //   节奏**的场景才登记；技能表解析失败、条件不满足、真没技能可放，一律别登记 ⇒
    //   Tick() 的兜底会把 Hold 降级成 Decline，把走位/喝水/魔杖还给引擎。
    //   病根：一拍什么都放不出就 Hold ⇒ 引擎整 tick 被挡（实测暗牧/平衡整轮 45 秒 0 施法 0 伤害）。
    ai::dpsfsm::TickResult HoldWait(BotMem* m, char const* why)
    {
        if (m)
            m->holdIntent = why;
        return ai::dpsfsm::TickResult::Hold;
    }

    // 某目标身上这个法术（任意 rank）的叠层总数
    int AuraStacks(PlayerbotAI* ai, Player* bot, BotMem* m, char const* name, Unit* u)
    {
        if (!u)
            return 0;
        uint32 id = SpellId(ai, bot, m, name, false);
        if (!id)
            return 0;
        int total = 0;
        uint32 first = sSpellMgr.GetFirstSpellInChain(id);
        for (uint32 cur = first ? first : id; cur; cur = sSpellMgr.GetNextSpellInChain(cur))
        {
            if (u->HasAura(cur))
                total += int(u->GetAuraCount(cur));
        }
        return total;
    }

    // [2026-11-01 站长口径] **目标身上**某个 DoT/诅咒的剩余时间（毫秒），任意 rank。
    //   与 AuraRemain() 的区别：那个只看施法者**自己**身上的 buff；DoT 续期判据要的是**目标**的
    //   剩余时间 —— "剩余 < refresh_dot 就补"，而不是"没挂才补"。旧口径（AuraStacks()==0 才补）
    //   会让 DoT 断档：到期的那一刻才想补，读条时间 + 反应延迟里目标身上是空的。
    //   0 = 没挂（或没学会该法术）；999999 = 永久 debuff。
    //   按法术链遍历的理由与 AuraStacks() 相同：bot 身上的 rank 与目标身上挂着的 rank 未必一致。
    int TargetAuraRemain(PlayerbotAI* ai, Player* bot, BotMem* m, char const* name, Unit* u)
    {
        if (!u)
            return 0;
        uint32 id = SpellId(ai, bot, m, name, false);
        if (!id)
            return 0;
        int best = 0;
        uint32 first = sSpellMgr.GetFirstSpellInChain(id);
        for (uint32 cur = first ? first : id; cur; cur = sSpellMgr.GetNextSpellInChain(cur))
        {
            SpellAuraHolder* holder = u->GetSpellAuraHolder(cur);
            if (!holder)
                continue;
            int const dur = holder->GetAuraDuration();
            int const remain = dur < 0 ? 999999 : dur;   // 永久 debuff
            if (remain > best)
                best = remain;
        }
        return best;
    }

    // [2026-11-01 站长口径] `out`（可选）= 同一份过滤口径下的**敌人列表**（痛苦术多目标铺腐蚀要用）。
    //   传 nullptr 时行为与以前逐字节一致（只数个数），所以 pack10/pack30 的调用点一个都没动。
    int CountHostilesNear(PlayerbotAI* ai, Player* bot, float range, std::vector<Unit*>* out = nullptr)
    {
        // [2026-10-29 崩溃修] 不在世界里的 bot 调 GetMap() 会触发核心断言 m_currMap 直接关服
        if (!bot || !bot->IsInWorld())
            return 0;
        // 假人场景声明过目标集时，只数这些目标（否则会把邻居测点的假人算进 AoE 判断）
        std::set<ObjectGuid> scenario;
        {
            std::lock_guard<std::mutex> lock(s_disabledMutex);
            auto it = s_scenarioTargets.find(bot->GetGUIDLow());
            if (it != s_scenarioTargets.end())
                scenario = it->second;
        }
        if (!bot || !bot->GetMap() || !ai)
            return 0;
        auto* value = ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("possible targets");
        if (!value)
            return 0;
        int n = 0;
        for (ObjectGuid const& guid : value->Get())
        {
            Unit* u = bot->GetMap()->GetUnit(guid);
            if (!u || !u->IsAlive() || u->GetTypeId() != TYPEID_UNIT)
                continue;
            if (bot->GetDistance(u) > range)
                continue;
            if (!sServerFacade.IsHostileTo(bot, u))
                continue;
            if (!scenario.empty() && !scenario.count(u->GetObjectGuid()))
                continue;
            if (out)
                out->push_back(u);
            ++n;
        }
        return n;
    }

    // ==========================================================================
    // [2026-10-31 站长口径] 猎人时间轴（平射计时器模型）的**取数**：
    //   平射隐藏施法 0.5s（受急速）｜平射独立冷却 (武器速度−0.5)s（受急速）
    //   ｜GCD 1.5s（不受急速）｜稳固读条 1.5s 基础、**受急速**。
    //   不写急速挡位表：每 tick 现算 steady_window = 实际稳固读条 + autoshot_land。
    // ==========================================================================
    bool IsHunterSpec(Spec sp)
    {
        return sp == Spec::BeastMastery || sp == Spec::Marksmanship || sp == Spec::Survival;
    }

    // 远程攻速倍率（<1 = 有急速/箭袋加速）。
    //   核心在 `Spell::prepare → GetSpellCastTime(spell != nullptr)` 里对
    //   `USES_RANGED_SLOT` 技能（稳固/瞄准都是）用 `m_modAttackSpeedPct[RANGED_ATTACK]` 缩放，
    //   而那个成员是 protected，模块侧拿不到 ⇒ 用同一份数据的等价关系反推：
    //     · ApplyAttackTimePercentMod 对 `UNIT_FIELD_BASEATTACKTIME` 与 m_modAttackSpeedPct **同步**缩放；
    //     · Unit::GetAttackTime() = 字段 / m_modAttackSpeedPct ⇒ 比值恒为未急速的武器速度；
    //     · 所以 倍率 = 字段 / GetAttackTime()。
    float HunterRangedSpeedMod(Player* bot)
    {
        if (!bot || !bot->IsInWorld())
            return 1.0f;
        uint32 const base = bot->GetAttackTime(RANGED_ATTACK);
        float const field = bot->GetFloatValue(
            static_cast<uint16>(UNIT_FIELD_BASEATTACKTIME) + static_cast<uint16>(RANGED_ATTACK));
        if (!base || field <= 0.0f)
            return 1.0f;
        float const mod = field / float(base);
        if (mod <= 0.05f || mod > 1.5f)     // 异常值 ⇒ 当没有急速，别把窗口算翻
            return 1.0f;
        return mod;
    }

    // 当前急速下某个远程读条技的**实际**读条毫秒。取不到返回 0（调用方回落到 TP 默认值）。
    //   注意 `GetSpellCastTime(entry, bot)`（无 Spell 对象）本身**不加急速**，但会把核心对猎人远程
    //   技能的 +500ms 工具条口径算进去（稳固的 CastingTimeIndex 是 1000 ⇒ 这里返回 1500）；
    //   急速那一层由上面的远程攻速倍率补上，与 Spell 对象路径一致（稳固是 IS_ABILITY + RANGED_SLOT）。
    int HunterRangedCastMs(PlayerbotAI* ai, BotMem* m, char const* name)
    {
        if (!ai || !ai->GetBot())
            return 0;
        uint32 const id = SpellId(ai, ai->GetBot(), m, name, false);
        if (!id)
            return 0;
        SpellEntry const* entry = sServerFacade.LookupSpellInfo(id);
        if (!entry)
            return 0;
        uint32 const base = GetSpellCastTime(entry, ai->GetBot());
        if (!base)
            return 0;
        int const ms = int(float(base) * HunterRangedSpeedMod(ai->GetBot()) + 0.5f);
        if (ms < 300 || ms > 6000)
            return 0;
        return ms;
    }

    void BuildSnapshot(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        Player* bot = ai->GetBot();
        s.bot = bot;
        s.target = (bot->GetVictim() && bot->GetVictim()->IsAlive()) ? bot->GetVictim() : nullptr;
        s.selfHp = bot->GetHealthPercent();

        // 主资源：法力职业看蓝 %；怒战看怒气、盗�?猫德看能量（都换算成占满值的 %，同时保留原始值）
        uint32 const maxMana = bot->GetMaxPower(POWER_MANA);
        if (maxMana)
            s.power = 100.0f * float(bot->GetPower(POWER_MANA)) / float(maxMana);
        else
            s.power = 100.0f;

        s.rage = int32(bot->GetPower(POWER_RAGE));
        s.energy = int32(bot->GetPower(POWER_ENERGY));
        s.combo = bot->GetComboPoints();

        if (!maxMana)
        {
            // 物理职业用怒气/能量�?资源"
            uint32 const maxRage = bot->GetMaxPower(POWER_RAGE);
            if (maxRage)
                s.power = 100.0f * float(s.rage) / float(maxRage);
            else
                s.power = float(s.energy);
        }

        // [2026-10-30 修] IsNonMeleeSpellCasted 的第三参 skipAutorepeat 默认 false ⇒ 自动射击
        //   （CURRENT_AUTOREPEAT_SPELL）也算"正在读条"。猎人自动射击一直挂着 ⇒ s.casting 恒 true
        //   ⇒ TickHunter 在 `if (s.casting) return true` 就返回：稳固射击/猎人印记/毒蛇钉刺整场 0 次
        //   （实测 marks 40 秒 casts=19、top 只有 75:18 = 自动射击的重复登记）。自动射击不是"读条"，跳过它。
        s.casting = bot->IsNonMeleeSpellCasted(false, false, true);
        s.moving = bot->IsMoving();
        s.silenced = bot->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SILENCED);

        if (s.target)
        {
            s.targetHp = s.target->GetHealthPercent();
            s.dist = bot->GetDistance(s.target);
        }

        s.pack10 = CountHostilesNear(ai, bot, 10.0f);
        s.pack30 = CountHostilesNear(ai, bot, 30.0f);

        // [2026-10-31 站长口径] 仇恨三档的输入：自己 / 坦克对**同一个目标**的威胁值。
        //   坦克认定直接复用 PlayerbotAI::IsTank（与 AttackAction 同一份口径），不自己按职业/天赋猜；
        //   没有队伍、队伍里没坦克、坦克和我不打同一个目标 ⇒ tankAlive=false / tankThreat=0
        //   ⇒ EvalThreat 恒 Open（假人场景绝不能因为"没仇恨"把 DPS 卡住）。
        s.myThreat = 0.0f;
        s.tankThreat = 0.0f;
        s.tankAlive = false;
        if (s.target)
        {
            s.myThreat = bot->getThreatManager().getThreat(s.target);
            if (Group* group = bot->GetGroup())
            {
                Player* tank = nullptr;
                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                {
                    Player* member = ref->getSource();
                    if (member && member != bot && sServerFacade.IsAlive(member) && PlayerbotAI::IsTank(member))
                    {
                        tank = member;
                        break;
                    }
                }
                if (tank)
                {
                    s.tankAlive = true;
                    s.tankThreat = tank->getThreatManager().getThreat(s.target);
                }
            }
        }

        // [2026-10-31] 猎人时间轴字段：只在猎人 spec 计算（别的职业不碰这几个成员，
        //   保持默认值 0/1500/false，避免每 tick 多做几次法术/光环查询）。
        if (IsHunterSpec(m->spec))
        {
            // 到下一发自动射击的剩余毫秒。
            //   ⚠️ `getAttackTimer(RANGED_ATTACK)` 在本核心是**死值**（见 BotMem 里的取证注释），
            //   只保留原始值进诊断；真正可用的剩余时间由"自动射击计数 + 观测周期"反推。
            uint32 const rawTimer = bot->getAttackTimer(RANGED_ATTACK);
            bool const repeatPresent = bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL) != nullptr;
            uint32 const nowMs = WorldTimer::getMSTime();
            uint32 const safeNow = nowMs ? nowMs : 1;

            // 核心每发自动射击都会在 `_UpdateAutoRepeatSpell` 里 ++botSpellCounts[75]
            uint32 const shotCount = uint32(bot->botSpellCounts[75]);
            if (shotCount != m->autoShotCount)
            {
                // 观测周期：只有 ≥1000ms 的间隔才算"平射冷却"（核心 2s，打断后约 2.5s）；
                //   重挂后第一发的 ~500ms 起手不算周期，观测完把间隔恢复成核心默认 2000。
                if (m->autoShotCount && m->autoShotAt)
                {
                    uint32 const delta = WorldTimer::getMSTimeDiff(m->autoShotAt, safeNow);
                    if (delta >= 1000 && delta <= 6000)
                        m->autoShotInterval = delta;
                    else if (delta < 1000)
                        m->autoShotInterval = 2000;
                }
                m->autoShotCount = shotCount;
                m->autoShotAt = safeNow;
            }
            else if (repeatPresent && !m->autoRepeatWasPresent)
            {
                // 刚重挂上：核心先走 0.5s 起手（m_AutoRepeatFirstCast），第一发约 500ms 后
                m->autoShotAt = safeNow;
                m->autoShotInterval = 500;
            }
            m->autoRepeatWasPresent = repeatPresent;

            if (!repeatPresent || !m->autoShotAt)
                s.autoRemainMs = 0;
            else
            {
                uint32 const interval = m->autoShotInterval ? m->autoShotInterval : 2000;
                int const elapsed = int(WorldTimer::getMSTimeDiff(m->autoShotAt, safeNow));
                int remain = int(interval) - elapsed;
                s.autoRemainMs = remain > 0 ? (remain < int(interval) ? remain : int(interval)) : 0;
            }

            int const steady = HunterRangedCastMs(ai, m, "steady shot");
            s.steadyCastMs = steady ? steady : int(TP(m, "steady_cast_ms", 1500.0));

            s.multiReady = Ready(bot, SpellId(ai, bot, m, "multi-shot", false));
            s.arcaneReady = Ready(bot, SpellId(ai, bot, m, "arcane shot", false));
            s.killCommandReady = Ready(bot, SpellId(ai, bot, m, "kill command", false));

            if (!m->steadyCastLogged)
            {
                m->steadyCastLogged = true;
                sLog.outError("[DPSFSM-HUNTER] %s spec=%s autoRemain=%dms(observed interval=%ums rawTimer=%ums) "
                    "steadyCast=%dms(%s) rangedMod=%.3f weapon=%ums hasted=%.0f multi=%d arcane=%d kc=%d dist=%.1f",
                    bot->GetName(), ai::dpsfsm::SpecName(m->spec), s.autoRemainMs, m->autoShotInterval,
                    rawTimer, s.steadyCastMs, steady ? "core" : "fallback", HunterRangedSpeedMod(bot),
                    bot->GetAttackTime(RANGED_ATTACK),
                    double(bot->GetFloatValue(static_cast<uint16>(UNIT_FIELD_BASEATTACKTIME)
                        + static_cast<uint16>(RANGED_ATTACK))),
                    s.multiReady ? 1 : 0, s.arcaneReady ? 1 : 0, s.killCommandReady ? 1 : 0, s.dist);
            }
        }
    }

    void LogTransition(PlayerbotAI* ai, BotMem* m, Snapshot& s, State next)
    {
        if (m->state == next)
            return;
        Player* bot = s.bot;
        sLog.outError("[DPSFSM] %s %-13s %s -> %-8s hp=%3.0f%% res=%3.0f%% tgt=%3.0f%% pack=%d/%d dist=%4.1f cast=%d gcd=%d",
            bot->GetName(), ai::dpsfsm::SpecName(m->spec), ai::dpsfsm::StateName(m->state), ai::dpsfsm::StateName(next),
            s.selfHp, s.power, s.targetHp, s.pack10, s.pack30, s.dist, s.casting ? 1 : 0, s.gcd ? 1 : 0);
        m->state = next;
        if (next == State::Idle)
            m->castCount = 0;
    }

    // ==========================================================================
    // [2026-10-31 站长口径] 有蓝职业（奥法/冰法）的耗蓝账本（DPM 用）
    //   `spent` 只根据**真的施法成功**的记录累加，单价一律取 `SpellEntry->manaCost`
    //   （拿不到 entry 就跳过，绝不按蓝量百分比瞎估）。这里只记账，不做任何条件判断，
    //   所以 mode=1/2/3/4 的施法路径行为不变（只多一行统计）。
    //
    //   结算点 = 核心的 `Player::botSpellCounts`（id → 成功施法次数），**按次数差结算**，
    //   而不是在每个施法函数里各写一遍 `manaSpent += ...`：
    //     · mode=5 的 CastChecked / TryCastAt / CastIdAt / CastSelfCentered 每次成功都会
    //       `PlayerbotAI::RecordSpellCast` ⇒ 计数 +1；
    //     · mode=1/2/3 的评分路径走 `dpsvalue::CastId`（DpsValue.inc，不在本次允许改动的文件里），
    //       它同样只调 `RecordSpellCast` ⇒ 只有按"成功次数差"结算才能一次覆盖两条路径，
    //       也不会因为两处都 + 而重复计数。
    //   粒度 = 相邻两次 Sync（Tick 每 tick 调一次），但只包含真实成功的施法，不含"施法被拒"。
    // ==========================================================================
    void SyncManaSpent(BotMem* m, Player* bot)
    {
        if (!m || !bot)
            return;
        if (m->spec != Spec::Arcane && m->spec != Spec::Frost)
            return;
        for (std::map<uint32, uint64>::const_iterator it = bot->botSpellCounts.begin();
            it != bot->botSpellCounts.end(); ++it)
        {
            uint32 const id = it->first;
            uint64 const total = it->second;
            uint64& seen = m->manaCounts[id];
            if (total <= seen)
            {
                if (total < seen)       // 计数被重置（重登/重置统计）⇒ 重新对齐，不补差额
                    seen = total;
                continue;
            }
            uint64 const fresh = total - seen;
            seen = total;
            SpellEntry const* entry = sServerFacade.LookupSpellInfo(id);
            if (!entry)                 // 拿不到法术模板 ⇒ 跳过，不瞎估
                continue;
            m->manaSpent += uint64(entry->manaCost) * fresh;
        }
    }

    // [DPSFSM-MANA] <bot> spent=<累计法力消耗> pct=<当前蓝量%>：10 秒一条节流（不是每 tick 打）。
    //   由 Tick() 在本 tick 末尾统一调用（mode=5 走完 FSM / mode=1/2/3 走完策略表之后），
    //   两种模式的统计口径一致，可以直接横向比 DPM。
    void ManaLogTick(BotMem* m, Player* bot)
    {
        if (!m || !bot)
            return;
        if (m->spec != Spec::Arcane && m->spec != Spec::Frost)
            return;
        SyncManaSpent(m, bot);          // 先结算本 tick 之前新增的成功施法
        uint32 const nowMs = WorldTimer::getMSTime();
        uint32 const safeNow = nowMs ? nowMs : 1;
        if (m->lastManaLog && WorldTimer::getMSTimeDiff(m->lastManaLog, safeNow) < 10000)
            return;
        m->lastManaLog = safeNow;
        m->lastManaSpent = m->manaSpent;
        sLog.outError("[DPSFSM-MANA] %s spent=%llu pct=%.0f",
            bot->GetName(), (unsigned long long)m->manaSpent, bot->GetPowerPercent());
    }

    // [2026-10-29 诊断结论] 原来只判 IsSpellReady 就放，失败也照样每 tick 重试（怒气不够、目标闪避条件、
    //   移动中读条被静默取消…），统计里就出现 "40 秒 1204 次施法"。现在：①先跑一遍完整的 CanCastSpell
    //   （Spell::CheckCast 的怒气/射程/朝向/条件都算上）；②失败按 globalCoolDown 退避，别死循环重试。
    bool CastChecked(PlayerbotAI* ai, BotMem* m, Snapshot& s, char const* name, Unit* target, Unit* selfTarget = nullptr)
    {
        uint32 id = SpellId(ai, s.bot, m, name);
        if (NoGcd(name))
        {
            if (!id || !s.bot->HasSpell(id))      // 不吃 GCD，但仍必须会这个技能
                return false;
        }
        else if (!Ready(s.bot, id))
            return false;

        time_t const now = time(nullptr);
        auto it = m->failUntil.find(id);
        if (it != m->failUntil.end() && now < it->second)
            return false;

        Unit* tgt = selfTarget ? selfTarget : (target ? target : s.target);

        // [2026-10-31 根因] `PlayerbotAI::CastSpellBody` 里有一段：
        //     if (!IsInFront(bot, faceTo, ...)) { SetFacingTo(bot, faceTo);
        //         if (!HasRealPlayerMaster()) failWithDelay = true; }
        //     if (failWithDelay) { ... return false; }
        //   ⇒ bot 没有真人主人时，**背对目标的第一次调用只转向、不施法、返回 false**。
        //   而我们把这次 false 当成"施法失败"，写 2 秒退避；几个候选一起被写退避 ⇒ 整轮空转
        //   （实测 1/3 轮次 victim 正常、不 casting、不移动，却 0 施法 0 伤害）。
        //   修法：在调 CastSpell 之前自己先转向，让 IsInFront 成立，一次调用就真施法。
        if (tgt && tgt != s.bot)
            s.bot->SetFacingToObject(tgt);

        SpellCastResult res = SPELL_FAILED_DONT_REPORT;
        if (!ai->CanCastSpell(id, tgt, 0, false, nullptr, false, false, false, &res))
        {
            // [2026-10-30 修] 自身增益/自身姿态这类"该打自己"的法术，走 ai->CanCastSpell(对敌目标) 会被判死。
            //   实测两类结果码：
            //     ① TARGET_ENEMY(108)：燃烧 11129 / 冰冷血脉 12472 / 奥强 12042 / 闪现 12051 /
            //        剑刃乱舞 13877 / 冲动 13750 / 冷血 14177 / 元素掌握 16166 / 英雄 32182 / 唤水元素 31687…
            //        （CanCastSpell 见"正法术 + 敌对目标"直接拒 ⇒ 这些技能整场 0 次）；
            //     ② TARGET_FRIENDLY(110)：切割 6774 这类"效果正面但 IsPositiveSpell=false"的法术，
            //        对着自己放又被判"负法术 + 友方目标" ⇒ 盗贼切割整场 0 次。
            //   本质都是"这一手应该以自己为目标"，改用核心 Unit::CastSpell(self) 放（和奉献 CastSelfCentered 同一条路）。
            bool const selfFix = (res == SPELL_FAILED_TARGET_FRIENDLY)
                || (res == SPELL_FAILED_TARGET_ENEMY && tgt != s.bot);
            if (selfFix)
            {
                SpellCastResult const res2 = s.bot->CastSpell(s.bot, id, uint32(TRIGGERED_NONE));
                if (res2 == SPELL_CAST_OK)
                {
                    ++m->castCount;
                    m->lastOk[id] = now;
                    m->lastCastId = id;      // [2026-11-01] [DPSFSM-WARLOCK] 的 last=
                    m->castThisTick = true;
                    PlayerbotAI::RecordSpellCast(s.bot->GetObjectGuid(), id);
                    return true;
                }
                res = res2;
            }
            // [2026-10-29 实测] 一律退 2 秒会把怒气/能量职业的 DPS 砍半：
            //   "蓝/怒/能量不够"是**等一下就够**，只退 0.4 秒；射程/视线/其它条件才退 2 秒。
            bool const waitingResource = (res == SPELL_FAILED_NO_POWER
                || res == SPELL_FAILED_OUT_OF_RANGE || res == SPELL_FAILED_LINE_OF_SIGHT
                || res == SPELL_FAILED_NOT_READY);
            m->failUntil[id] = now + (waitingResource ? 1 : 2);
            return false;
        }
        // [2026-10-31 根因修复] `PlayerbotAI::CastSpellBody()` 对 bot 恒有：
        //     if (!IsInFront(...)) { SetFacingTo(...); if (!HasRealPlayerMaster()) failWithDelay = true; }
        //     if (failWithDelay) return false;      // ← 只转向、不施法
        //   而需要 FacingCasterFlags 的技能（寒冰箭/火球/暗影箭/奥术冲击…）都会撞上这个分支，
        //   于是"自身法术能放、对敌技能全灭"（实测约 1/3 轮次 0 伤害、casts 只有冰冷血脉+水元素）。
        //   ⇒ 对敌施法直接走核心 Unit::CastSpell，绕开这段 bot 专用逻辑。
        if (tgt && tgt != s.bot)
            s.bot->SetFacingToObject(tgt);
        if (tgt && tgt != s.bot)
        {
            SpellCastResult const r2 = s.bot->CastSpell(tgt, id, uint32(TRIGGERED_NONE));
            if (r2 != SPELL_CAST_OK)
            {
                m->failUntil[id] = now;      // 不退避，下一 tick 立刻重试
                return false;
            }
            // [2026-10-31] 核心施法要自己记账，否则 botCasts / top= 统计不到
            //   （之前出现 "dmg=20k 但 casts=0"，正是漏记）
            PlayerbotAI::RecordSpellCast(s.bot->GetObjectGuid(), id);
        }
        else if (!ai->CastSpell(id, tgt))
            return false;
        ++m->castCount;
        m->lastOk[id] = now;
        m->lastCastId = id;              // [2026-11-01] [DPSFSM-WARLOCK] 的 last=
        m->castThisTick = true;
        return true;
    }

    bool TryCast(PlayerbotAI* ai, BotMem* m, Snapshot& s, char const* name, Unit* target)
    {
        return CastChecked(ai, m, s, name, target);
    }

    // 自身增益（圣印/守护/暗影形态/战斗怒吼…）必须打在自己身上：打敌人会被判为负面法术而失败
    bool TryCastSelf(PlayerbotAI* ai, BotMem* m, Snapshot& s, char const* name)
    {
        return CastChecked(ai, m, s, name, nullptr, s.bot);
    }

    // [2026-10-31] **以自己宠物为目标**的施法（恶魔牺牲 18788 就是这样：目标必须是自己的恶魔，
    //   对着自己放会 BAD_TARGETS）。以前 Prereq 一律走 TryCastSelf ⇒ 牺牲暗的 18788 一次都没放出来。
    bool TryCastPet(PlayerbotAI* ai, BotMem* m, Snapshot& s, char const* name)
    {
        Pet* pet = s.bot->GetPet();
        if (!pet || !pet->IsAlive())
            return false;
        return CastChecked(ai, m, s, name, pet, nullptr);
    }

    // ==========================================================================
    // [2026-10-31 站长口径] **宠物控制**：17 个专精共用的一份实现（水元素/地狱火/猎人宠物/
    //   暗影魔/树人…）。以前每处各自 `GetPet()` 判空、判活着，召唤失败又静默 ⇒
    //   实测冰法 `pet=0` 而日志里看不出是"施法被拒"还是"没学会这个技能"。
    //   统一成三个入口：PetPresent（在不在场）/ CommandPetAttack（该不该重新下令）/
    //   SummonPetIfNeeded（没有就召，失败留一条可区分的诊断）。
    // ==========================================================================

    // 宠物在场 = 有宠物 + 活着 + 和主人在同一张图（换图/副本传送后宠物会在别的地图，
    //   这种"影子宠物"绝不能再给它下令，否则指令跨图）。
    bool PetPresent(Snapshot& s)
    {
        if (!s.bot || !s.bot->IsInWorld())
            return false;
        Pet* pet = s.bot->GetPet();
        if (!pet || !pet->IsAlive() || !pet->IsInWorld())
            return false;
        // [2026-10-29 崩溃修] 不在世界里的对象调 GetMap() 会触发核心断言
        //   （CountHostilesNear 那条注释同一个坑），所以先过 IsInWorld 再比地图。
        return pet->GetMap() == s.bot->GetMap();
    }

    // [2026-10-31 站长口径] [DPSFSM-PET] 的 10 秒节流（CommandPetAttack 与猎人宠物生命周期守卫共用）：
    //   守卫在状态机**之前**跑，所以猎人的周期日志由守卫统一打（带 action 字段）；
    //   冰法的水元素没有守卫，仍然由 CommandPetAttack 自己打。
    //   共用同一个 s_petLogAt 时间戳 ⇒ 每只 bot 每 10 秒只有一行，不会两边各打一条。
    bool PetLogDue(uint32 guid)
    {
        std::lock_guard<std::mutex> lock(s_disabledMutex);
        uint32 const nowMs = WorldTimer::getMSTime();
        uint32 const safeNow = nowMs ? nowMs : 1;
        uint32& last = s_petLogAt[guid];
        if (!last || WorldTimer::getMSTimeDiff(last, safeNow) >= 10000)
        {
            last = safeNow;
            return true;
        }
        return false;
    }

    // [2026-10-31 修] 宠物命令状态 / AI 类型的可读名（只给 [DPSFSM-PET] 日志用）。
    //   排查结论：程序化建宠时 `UNIT_FLAG_PLAYER_CONTROLLED` 是在 `AIM_Initialize()` **之后**才设的，
    //   而 `FactorySelector::selectAI()` 用 `creature->IsPlayerControlled() && pet->isControlled()`
    //   才选 PetAI；否则走 `ai_registry.GetRegistryItem("GuardianAI")`
    //   （见 CreatureAISelector.cpp → FactorySelector::selectAI）。
    //   GuardianAI 只在"主人被谁打了"时 AttackStart（GuardianAI.cpp → GuardianAI::UpdateAI），
    //   没有任何"我没贴到目标就再追"的复检 ⇒ 宠物拿到 victim 之后停在原地不动（实测 dist 恒定 ~17）。
    char const* PetCommandName(Pet* pet)
    {
        CharmInfo* charmInfo = pet ? pet->GetCharmInfo() : nullptr;
        if (!charmInfo)
            return "-";
        switch (charmInfo->GetCommandState())
        {
            case COMMAND_STAY:   return "stay";
            case COMMAND_FOLLOW: return "follow";
            case COMMAND_ATTACK: return "attack";
            default:             return "?";
        }
    }

    char const* PetAiName(Pet* pet)
    {
        if (!pet || !pet->AI())
            return "-";
        if (dynamic_cast<PetAI*>(pet->AI()))
            return "PetAI";
        if (dynamic_cast<GuardianAI*>(pet->AI()))
            return "GuardianAI";
        return "other";
    }

    bool PetIsChasing(Pet* pet)
    {
        return pet && pet->GetMotionMaster() &&
            pet->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE;
    }

    // [2026-10-31 修] **地形卡宠兜底**：`MovePoint(..., generatePath=false)` = 直线点移动（不寻路）。
    //   触发条件只有一种：宠物已经在咬这个目标，但**追击 spline 已经走完（Finalized）却仍够不着**
    //   且**连续 1.5 秒没动** —— 实测态是"dist 恒定 11.7、splineDone=1、moving=0"。
    //   原因（core 侧）：`ChaseMovementGenerator::HandleFinalizedMovement()` 把 spline 结束当成
    //   "到达目标"；随后 `m_reachable=false` 让 `HandleTargetedMovement()` 直接 return
    //   （TargetedMovementGenerator.cpp），不再重新寻路 ⇒ 宠物卡在半路再也不动。
    //   1.5 秒防抖是为了不误伤正常追击：两次寻路之间 spline 可能刚好结束一瞬，
    //   那种情况不该插直线（会抄近路穿地形）。
    void StraightApproachPet(Pet* pet, Unit* target, float meleeReach)
    {
        if (!pet || !target || !pet->GetMotionMaster())
            return;

        float dx = pet->GetPositionX() - target->GetPositionX();
        float dy = pet->GetPositionY() - target->GetPositionY();
        float len = std::sqrt(dx * dx + dy * dy);
        if (len < 0.01f)
        {
            dx = 0.0f; dy = 1.0f; len = 1.0f;
        }
        // 落点放在目标身边（近战距离内侧），别站进目标模型中心
        float const stop = std::max(0.5f, std::min(meleeReach * 0.5f, 2.0f));
        pet->GetMotionMaster()->MovePoint(0,
            target->GetPositionX() + dx / len * stop,
            target->GetPositionY() + dy / len * stop,
            target->GetPositionZ(), FORCED_MOVEMENT_NONE, false);
    }

    // 卡宠计时：movingOrPathing=true（还在走 / 追击 spline 没结束）就清零；
    //   否则连续 1.5s 才返回 true（= 真的卡住了，可以走直线兜底）。
    bool PetStuckLongEnough(uint32 guid, bool movingOrPathing)
    {
        std::lock_guard<std::mutex> lock(s_disabledMutex);
        uint32 const nowMs = WorldTimer::getMSTime();
        uint32 const safeNow = nowMs ? nowMs : 1;
        uint32& since = s_petStuckAt[guid];
        if (movingOrPathing)
        {
            since = 0;
            return false;
        }
        if (!since)
        {
            since = safeNow;
            return false;
        }
        return WorldTimer::getMSTimeDiff(since, safeNow) >= 1500;
    }

    void ResetPetStuck(uint32 guid)
    {
        std::lock_guard<std::mutex> lock(s_disabledMutex);
        s_petStuckAt[guid] = 0;
    }

    // 每 tick 的"宠物事务"：宠物没在咬当前目标（换目标 / 掉队 / 刚召出来）就重新下令。
    //   返回 true = 这一 tick 宠物事务已处理（已在打同一目标，或刚下完令）；
    //   没有宠物 / 没目标 / 宠物被禁止行动时返回 false（调用方照常放技能）。
    //   顺带每 10 秒打一条 [DPSFSM-PET]，用 WorldTimer 节流（和 [DPSFSM-HB] 的 hbLast 同一套写法，
    //   不要每 tick time(nullptr)）。
    //   [2026-10-31 站长口径] `allowLog=false` 由**猎人宠物守卫**传进来：守卫每 tick 都先跑，
    //   它自己要在同一 tick 结尾打一条带 action 的 [DPSFSM-PET]；如果这里也打，就成了
    //   "旧的先吃掉 10 秒窗口、守卫那行永远打不出来"（实测第一次跑就是这样）。
    bool CommandPetAttack(Snapshot& s, bool allowLog = true)
    {
        if (!s.bot || !s.bot->IsInWorld())
            return false;

        Pet* pet = s.bot->GetPet();
        bool const present = PetPresent(s);

        if (allowLog && PetLogDue(s.bot->GetGUIDLow()))
        {
            Unit* petVictim = present ? pet->GetVictim() : nullptr;
            float petDist = -1.0f;
            bool chasing = false;
            if (present && s.target && s.target->IsInWorld() && s.target->GetMap() == pet->GetMap())
            {
                petDist = pet->GetDistance(s.target);
                chasing = PetIsChasing(pet);
            }
            sLog.outError("[DPSFSM-PET] %s pet=%s ai=%s victim=%s cmd=%s chasing=%d dist=%.1f tgt=%s",
                s.bot->GetName(), present ? "有" : "无", PetAiName(pet),
                petVictim ? petVictim->GetName() : "-", PetCommandName(pet), chasing ? 1 : 0, petDist,
                s.target ? s.target->GetName() : "-");
        }

        if (!present || !s.target)
            return false;
        if (!s.target->IsInWorld() || !s.target->IsAlive() || s.target->GetMap() != pet->GetMap())
            return false;

        // 本核心没有 `Unit::GetMeleeReach()`（grep 全仓库无此符号）；近战距离用
        //   `GetCombinedCombatReach(target, true)`（= 双方体积，和 CanReachWithMeleeAttack 同一口径）。
        //   法系宠物（水元素/小鬼）由 PetAI 的 m_meleeEnabled=false 标记，它们要保持距离，
        //   绝不能补 MoveChase（默认 dist=0 会把它拽进近战）⇒ 只有近战宠物才走补追那段。
        float const meleeReach = pet->GetCombinedCombatReach(s.target, true);
        bool const tooFar = pet->GetDistance(s.target) > meleeReach + 1.0f;
        bool const meleePet = pet->AI() ? pet->AI()->IsMeleeEnabled() : true;

        // 已经在咬这个目标：正常情况下无需下令，宠物事务算处理完。
        //   [2026-10-31 修] **但**"GetVictim()==target 却停在 17 码外"正是实测的故障态：
        //   追击一旦被打断（跟随/施法/选 AI 错误）就再也没人重发 ⇒ 整场 25 dps 平砍都没有。
        //   所以近战宠物在这个状态下也要补一次追击（每 tick 判一次，追上后就不再调用）。
        if (pet->GetVictim() == s.target)
        {
            if (tooFar && meleePet)
            {
                if (pet->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE)
                    return true;    // 下面那条"直线接近"正在走，别打断
                // 追击 spline 已经走完却仍够不着 = ChaseMovementGenerator 判过 i_targetReached /
                //   m_reachable=false，不会再重新寻路（实测宠物停在 11.7 码外不动，splineDone=1）。
                //   连续 1.5s 确认卡住后，改用**不寻路的直线点移动**（generatePath=false）送到目标身边。
                bool const splineDone = !pet->movespline || pet->movespline->Finalized();
                bool const stuck = PetStuckLongEnough(s.bot->GetGUIDLow(), pet->IsMoving() || !splineDone);
                if (stuck)
                    StraightApproachPet(pet, s.target, meleeReach);
                else if (pet->GetMotionMaster()->GetCurrentMovementGeneratorType() != CHASE_MOTION_TYPE)
                    pet->GetMotionMaster()->MoveChase(s.target);
            }
            else
                ResetPetStuck(s.bot->GetGUIDLow());   // 已经贴上目标 ⇒ 清掉卡宠计时
            return true;
        }

        ResetPetStuck(s.bot->GetGUIDLow());

        // 宠物被禁止行动（支配/惊吓/临时禁手）时下不了令，交回调用方，下一 tick 再试
        if (pet->HasActionsDisabled())
            return false;

        // [2026-10-31 站长口径] **走引擎同一条命令路径**：发 `CMSG_PET_ACTION`
        //   （`ACT_COMMAND | COMMAND_ATTACK`），由核心 `HandlePetAction` 去设命令状态、
        //   停掉旧追击、重新 AttackStart —— 这就是真玩家点"攻击"的流程，也是引擎自己用的那条
        //   （`AttackAction::PetAttack()` 里就是这么发；它是成员函数、依赖它自己的 bot/ai，
        //   所以这里照抄那 6 行，不硬调）。
        //   只有拿不到 WorldSession 时才回退到下面的手工路径。
        if (s.bot->GetSession())
        {
            const uint8 flag = ACT_COMMAND;
            const uint32 command = (uint32(flag) << 24) | COMMAND_ATTACK;
            WorldPacket petData(CMSG_PET_ACTION);
            petData << pet->GetObjectGuid();
            petData << command;
            petData << s.target->GetObjectGuid();
            s.bot->GetSession()->HandlePetAction(petData);
            ResetPetStuck(s.bot->GetGUIDLow());
            return true;
        }

        // ---- 回退：无会话时手工下同样的令（保留旧行为）----
        // [2026-10-31 修] 真玩家的宠物收到"攻击"命令时，`PetHandler.cpp → HandlePetActionHelper()`
        //   的 COMMAND_ATTACK 分支做的是：SetPetLastAttackCommandTime → AttackStop →
        //   MotionMaster()->Clear() → AI()->AttackStart(target)，并且**命令状态是 COMMAND_ATTACK**。
        //   我们以前只调 AttackStart，CharmInfo 默认 COMMAND_FOLLOW（Unit.cpp → CharmInfo::CharmInfo
        //   的 m_CommandState(COMMAND_FOLLOW)），PetAI 在不咬目标时按"跟随"处理，也不会重发追击。
        if (CharmInfo* charmInfo = pet->GetCharmInfo())
            charmInfo->SetCommandState(COMMAND_ATTACK);

        // 换目标 / 掉队：走 PetAI::AttackStart（它按宠物类型自己选近战还是远程，并开始追击）。
        //   裸 Unit::Attack 也可以，但会把"远程宠物该不该冲上去"这种判断留给调用方。
        if (pet->AI())
            pet->AI()->AttackStart(s.target);
        else
            pet->Attack(s.target, false);

        // AttackStart → UnitAI::HandleMovementOnAttackStart 只在"真的从没目标变成有目标"时发一次追击。
        //   刚下令这一 tick 不动直线兜底：先让它按寻路走（下一 tick GetVictim() 已经是这个目标，
        //   真被地形卡住会走上面那段 1.5s 防抖 + 直线接近）。
        ResetPetStuck(s.bot->GetGUIDLow());

        return true;
    }

    // 没有宠物就召一只（水元素/地狱火/暗影魔…）。返回 true = 本 tick 真的放了召唤（占掉 GCD）。
    //   失败时**只打一条**可区分原因的诊断（前缀 [DPSFSM-PET]）：
    //     not-learned  —— SpellId 取不到/没学会（天赋没点、技能名对不上）
    //     cast-failed  —— 学会了但施法被拒（带上引擎给的结果码，便于分射程/资源/姿态）
    //     no-pet       —— 施法成功但宠物没出来（core 侧把它实现成 guardian/临时召唤时要留意）
    //   每只 bot 每次重登只打一次（BotMem::petLogDone；bot 对象换了就重置）。
    bool SummonPetIfNeeded(PlayerbotAI* ai, BotMem* m, Snapshot& s, char const* spellName)
    {
        if (m->petLogBot != s.bot)
        {
            m->petLogBot = s.bot;
            m->petLogDone = false;
        }

        if (PetPresent(s))
            return false;                    // 宠物已经在场，不用召

        uint32 const id = SpellId(ai, s.bot, m, spellName, false);
        bool const learned = (id != 0);
        // 召唤技一律对自己放（对敌目标会被 CanCastSpell 判 TARGET_ENEMY）
        bool const cast = learned && TryCastSelf(ai, m, s, spellName);

        if (PetPresent(s))
            return cast;                     // 召出来了（或这一 tick 刚放出来）

        if (m->petLogDone)
            return cast;
        m->petLogDone = true;

        if (!learned)
        {
            sLog.outError("[DPSFSM-PET] %s 召唤失败 '%s' reason=not-learned（没学会这个技能：天赋没点或技能名对不上）",
                s.bot->GetName(), spellName);
            return cast;
        }
        if (!cast)
        {
            // 再问一次引擎要结果码：TryCastSelf 只回 true/false，日志里要能看出"被拒"的具体原因
            SpellCastResult res = SPELL_FAILED_DONT_REPORT;
            bool const canNow = ai->CanCastSpell(id, s.bot, 0, false, nullptr, false, false, false, &res);
            sLog.outError("[DPSFSM-PET] %s 召唤失败 '%s'(id=%u) reason=cast-failed res=%d ready=%d cancast=%d",
                s.bot->GetName(), spellName, id, int(res),
                s.bot->IsSpellReady(id) ? 1 : 0, canNow ? 1 : 0);
            return cast;
        }

        sLog.outError("[DPSFSM-PET] %s 召唤成功 '%s'(id=%u) 但宠物仍未出场 reason=no-pet（core 侧可能按 guardian/临时召唤实现）",
            s.bot->GetName(), spellName, id);
        return cast;
    }

    // ==========================================================================
    // [2026-10-31 站长口径] **猎人宠物生命周期守卫**（mend / revive / call / create）
    //   起因（站长指出这是猎人最关键的问题）：假人场景 `[DUMMY] ... pet=0`（宠物伤害 0）。
    //   兽王的杀戮命令（Kill Command 34026）**必须有宠物在场且在打**，兽王一半循环靠它；
    //   宠物不在 ⇒ 兽王 dps 直接掉一半。
    //
    //   根因：宠物从来没人管"在不在"—— summon/revive 全靠引擎的策略（call pet / revive pet /
    //   mend pet 触发器），而假人场景引擎让位（`SkipEngine` 为真）、bot 又被 TeleportTo 到 GM 岛，
    //   `character_pet.current` 槽的宠物根本不会自己跟过来 ⇒ 整场 pet=0。
    //   （排查证据见 dev/KNOWN_ISSUES 第 34 章：PlayerbotFactory::InitPet 只在 Randomize 里跑，
    //     假人场景走的是 ApplyDummySetup，不调 InitPet，也不用引擎的 hunter pet 策略。）
    //
    //   顺序（每 tick 最多做一件；真的放了技能就返回 true，别和状态机抢同一个 GCD）：
    //     ① 在场（活着 + 同图）且 hp < pet_mend_hp（默认 60%）→ mend pet
    //     ② 在场但**死了**（尸体还在世界里）→ revive pet；复活成功后下一 tick 走 ③ 补召
    //     ③ 完全没有宠物 → call pet（把 current 槽的宠物拉回主人身边）
    //        call 之后**仍然没有**（= 没有可召唤的宠物）→ 假人场景 + hunter_pet_autocreate 才现场建一只
    //     ④ 有宠物 → 每 tick CommandPetAttack（换目标/掉队就重新下令）
    //
    //   阈值全部走 TP()：pet_mend_hp / hunter_pet_autocreate / hunter_pet_entry。
    //   **生产安全**：自动创建只在假人场景（SkipEngine）生效——真人 bot 建宠会往 character_pet
    //   写行、污染数据库，非假人场景直接跳过并打 skip-create(non-scenario)。
    // ==========================================================================

    // 假人场景专用：宠物"天生没有"时现场建一只。
    //   调用顺序照抄 `PlayerbotFactory::InitPet()`（那是本核心唯一被验证过的建宠路径），
    //   区别只在 entry 由 `hunter_pet_entry` 指定（默认掠食者 Rip-Blade Ravager 22123）。
    //   ⚠️ 本核心**没有** `Player::CreatePet` 这个 API（grep 全仓库无此符号），只有 `Pet::Create`，
    //      签名 `bool Pet::Create(uint32 guidlow, CreatureCreatePos&, CreatureInfo const*, uint32 pet_number)`；
    //      TBC 构建里 `MANGOSBOT_TWO` 未定义 ⇒ `CreatureCreatePos` 用 5 参构造（无 phase 形参）。
    Pet* CreateScenarioHunterPet(Player* bot, uint32 entry)
    {
        if (!bot || !bot->IsInWorld() || bot->getClass() != CLASS_HUNTER)
            return nullptr;
        Map* const map = bot->GetMap();
        if (!map)
            return nullptr;

        CreatureInfo const* co = sCreatureStorage.LookupEntry<CreatureInfo>(entry);
        if (!co)
        {
            sLog.outError("[DPSFSM-PET] %s 创建宠物失败 entry=%u reason=no-template（creature_template 里没有这一条）",
                bot->GetName(), entry);
            return nullptr;
        }

        CreatureCreatePos pos(map, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
            bot->GetOrientation());
        uint32 const guid = map->GenerateLocalLowGuid(HIGHGUID_PET);
        uint32 const petNumber = sObjectMgr.GeneratePetNumber();

        Pet* pet = new Pet(HUNTER_PET);
        if (!pet->Create(guid, pos, co, petNumber))
        {
            delete pet;
            sLog.outError("[DPSFSM-PET] %s 创建宠物失败 entry=%u reason=create-failed", bot->GetName(), entry);
            return nullptr;
        }

        pet->SetOwnerGuid(bot->GetObjectGuid());
        pet->SetGuidValue(UNIT_FIELD_CREATEDBY, bot->GetObjectGuid());
        pet->setFaction(bot->GetFaction());
        pet->SetLevel(bot->GetLevel());
        pet->InitStatsForLevel(bot->GetLevel());
#ifndef MANGOSBOT_TWO
        pet->SetLoyaltyLevel(BEST_FRIEND);
#endif
        pet->SetPower(POWER_HAPPINESS, HAPPINESS_LEVEL_SIZE * 2);
        pet->GetCharmInfo()->SetPetNumber(pet->GetObjectGuid().GetEntry(), true);
        // [2026-10-31 根因修] `UNIT_FLAG_PLAYER_CONTROLLED` 必须在 `AIM_Initialize()` **之前**设：
        //   `CreatureAISelector.cpp → FactorySelector::selectAI()` 正是用
        //   `creature->IsPlayerControlled() && pet->isControlled()` 选 PetAI，否则落到 GuardianAI。
        //   核心自己的两条建宠路径（Pet.cpp → Pet::LoadPetFromDB / CreateBaseAtCreature）也都是
        //   先设这个 flag、后 AIM_Initialize；我们以前写在 Add/AIM_Initialize 之后 ⇒ 拿到 GuardianAI
        //   （GuardianAI.cpp → GuardianAI::UpdateAI 只在"主人被谁打了"时 AttackStart，没有追击复检）。
        pet->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PLAYER_CONTROLLED);
        pet->GetMap()->Add((Creature*)pet);
        pet->AIM_Initialize();
        // 反应状态保持 REACT_DEFENSIVE（不主动开视野里的怪/不算乱拉怪）；
        // "咬哪一只"由每 tick 的 CommandPetAttack 用 COMMAND_ATTACK 显式指定。
        pet->AI()->SetReactState(REACT_DEFENSIVE);
        // InitPetCreateSpells 会按 DBC 的 CreatureSpellData（22123 → 11568）学家族技能：
        //   角刺 Gore（还有低吼/撕咬），这也是"掠食者"作为目标宠物的理由。
        pet->InitPetCreateSpells();

        // [2026-10-31 站长口径] **学了 ≠ 会放**：InitPetCreateSpells 内部把动作条清零（ACT_DISABLED），
        //   而 PetAI 的自动施法循环读的是动作条 ⇒ 技能永远不会自动释放，宠物只剩平砍。
        //   这里把"可自动施法的非被动家族技能"逐个打开（与 PlayerbotFactory::InitPet 同一份修法）。
        if (PetCreateSpellEntry const* createSpells = sObjectMgr.GetPetCreateSpellEntry(pet->GetEntry()))
        {
            for (uint8 ci = 0; ci < 4; ++ci)
            {
                if (!createSpells->spellid[ci])
                    break;
                SpellEntry const* learnSpell = sSpellTemplate.LookupEntry<SpellEntry>(createSpells->spellid[ci]);
                if (!learnSpell)
                    continue;
                uint32 const petSpellId = (learnSpell->Effect[0] == SPELL_EFFECT_LEARN_SPELL
                        || learnSpell->Effect[0] == SPELL_EFFECT_LEARN_PET_SPELL)
                    ? learnSpell->EffectTriggerSpell[0] : learnSpell->Id;
                if (petSpellId && pet->HasSpell(petSpellId) && !IsPassiveSpell(petSpellId))
                {
                    pet->ToggleAutocast(petSpellId, true);
                    sLog.outError("[DPSFSM-PET] %s 开启宠物自动施法 spell=%u entry=%u",
                        bot->GetName(), petSpellId, pet->GetEntry());
                }
            }
        }
        pet->LearnPetPassives();
        pet->CastPetAuras(true);
        pet->CastOwnerTalentAuras();
        pet->UpdateAllStats();
        pet->SetHealth(pet->GetMaxHealth());
        // （UNIT_FLAG_PLAYER_CONTROLLED 已在 AIM_Initialize 之前设好，见上面"根因修"注释）
        bot->SetPet(pet);
        bot->SetPetGuid(pet->GetObjectGuid());
#ifdef MANGOSBOT_TWO
        pet->SetUInt32Value(UNIT_CREATED_BY_SPELL, 13481);   // Tame Beast（WotLK 分支才需要）
#endif
        pet->SavePetToDB(PET_SAVE_AS_CURRENT, bot);
        bot->PetSpellInitialize();

        sLog.outError("[DPSFSM-PET] %s 创建宠物 entry=%u ai=%s react=%d",
            bot->GetName(), entry, PetAiName(pet), int(pet->AI() ? pet->AI()->GetReactState() : -1));
        return pet;
    }

    // 返回 true = 本 tick 宠物守卫**放了技能**（占了 GCD，调用方应当立刻返回 Acted）。
    bool HunterPetGuard(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        if (!ai || !m || !s.bot || !s.bot->IsInWorld() || s.bot->getClass() != CLASS_HUNTER)
            return false;

        Pet* const raw = s.bot->GetPet();
        bool const present = PetPresent(s);
        // "死了但尸体还在世界里"：核心的 Pet::Die 会保留尸体到 IsCorpseExpired，期间 GetPet() 仍返回它
        bool const dead = raw && !raw->IsAlive() && raw->IsInWorld();

        uint32 const nowMs = WorldTimer::getMSTime();
        uint32 const safeNow = nowMs ? nowMs : 1;
        char const* action = "none";
        bool acted = false;

        if (present)
        {
            // ① 血量低于阈值就喂血（Mend Pet 是引导技：正在读条/引导时不要插，否则白断一次输出）
            float const mendHp = float(TP(m, "pet_mend_hp", 60.0));
            if (raw->GetHealthPercent() < mendHp && !s.casting
                && (!m->petMendAt || WorldTimer::getMSTimeDiff(m->petMendAt, safeNow) >= 5000))
            {
                m->petMendAt = safeNow;
                if (TryCastPet(ai, m, s, "mend pet"))
                {
                    action = "mend";
                    acted = true;
                }
            }
            // ④ 有宠物：每 tick 确保它在咬当前目标（内部已判"同一目标"就直接返回，不会重复下令）
            CommandPetAttack(s, false);
        }
        else if (dead)
        {
            // ② 宠物尸体还在：复活（Revive Pet 982 = SPELL_EFFECT_SUMMON_DEAD_PET，
            //   effect 内部自己 `GetPet()` 找尸体，所以对**自己**施法即可）
            if (!m->petReviveAt || WorldTimer::getMSTimeDiff(m->petReviveAt, safeNow) >= 5000)
            {
                m->petReviveAt = safeNow;
                if (TryCastSelf(ai, m, s, "revive pet"))
                {
                    action = "revive";
                    acted = true;
                }
            }
        }
        else
        {
            // ③ 完全没有宠物：先 Call Pet（883 = SPELL_EFFECT_SUMMON_PET；核心按
            //   `Pet::LoadPetFromDB(owner, ..., current=false)` 拉 character_pet 的 current 槽。
            //   注意：**没有 current 行时施法本身仍会"成功"**（EffectSummonPet 里 delete 掉新 pet），
            //   所以成败只能看"放完之后宠物到底有没有出来"，不能看施法返回值。）
            bool callTried = false;
            if (!m->petCallAt || WorldTimer::getMSTimeDiff(m->petCallAt, safeNow) >= 10000)
            {
                m->petCallAt = safeNow;
                callTried = true;
                TryCastSelf(ai, m, s, "call pet");
                if (PetPresent(s))
                {
                    action = "call";
                    CommandPetAttack(s, false);
                }
            }

            if (!PetPresent(s) && callTried)
            {
                if (TP(m, "hunter_pet_autocreate", 1.0) < 1.0)
                {
                    // 关掉自动创建：什么都不做（日志行里 action=none 已经能看出"没有可召唤的宠物"）
                }
                else if (!ai::dpsfsm::SkipEngine(s.bot->GetGUIDLow()))
                {
                    // 非假人场景：绝不建（会往 character_pet 写行、污染数据库）
                    if (!m->petCreateSkipLogged)
                    {
                        m->petCreateSkipLogged = true;
                        sLog.outError("[DPSFSM-PET] %s skip-create(non-scenario)", s.bot->GetName());
                    }
                }
                else
                {
                    uint32 const entry = uint32(TP(m, "hunter_pet_entry", 22123.0) + 0.5);
                    if (CreateScenarioHunterPet(s.bot, entry))
                    {
                        action = "create";
                        acted = true;
                        CommandPetAttack(s, false);    // 刚建好就立刻下令咬当前目标
                    }
                }
            }
        }

        // 诊断：每 10 秒一行，字段 = pet=<有/无> alive=<0/1> hp=<%> action=<mend/revive/call/create/none>
        //   （外加原来的 victim/dist/tgt，保持既有日志解析脚本可用）
        //   [2026-10-31 修] 追加 ai= / cmd= / chasing=：宠物"没贴上"时要能一眼看出
        //   到底是 AI 选错（GuardianAI）、命令状态还是 follow、还是追击没发出去。
        if (PetLogDue(s.bot->GetGUIDLow()))
        {
            Pet* const pet = s.bot->GetPet();
            bool const nowPresent = PetPresent(s);
            Unit* petVictim = (nowPresent && pet) ? pet->GetVictim() : nullptr;
            float petDist = -1.0f;
            bool chasing = false;
            if (nowPresent && pet && s.target && s.target->IsInWorld() && s.target->GetMap() == pet->GetMap())
            {
                petDist = pet->GetDistance(s.target);
                chasing = PetIsChasing(pet);
            }
            sLog.outError("[DPSFSM-PET] %s pet=%s ai=%s alive=%d hp=%.0f action=%s victim=%s cmd=%s chasing=%d dist=%.1f tgt=%s",
                s.bot->GetName(), nowPresent ? "有" : "无", nowPresent ? PetAiName(pet) : "-",
                (pet && pet->IsAlive()) ? 1 : 0,
                pet ? pet->GetHealthPercent() : -1.0f,
                action, petVictim ? petVictim->GetName() : "-", PetCommandName(pet), chasing ? 1 : 0, petDist,
                s.target ? s.target->GetName() : "-");

            // [2026-10-31 诊断] 只在"够不着"时打：宠物在追却贴不上，要能看出卡在哪一层
            //   （spline 是否已经走完、是否被施法/定身打断、是否还有 CHASE_MOVE）。
            if (nowPresent && pet && s.target
                && pet->GetDistance(s.target) > pet->GetCombinedCombatReach(s.target, true) + 1.0f)
            {
                sLog.outError("[DPSFSM-PETDBG] %s pos=(%.1f,%.1f,%.1f) tpos=(%.1f,%.1f,%.1f) moving=%d splineDone=%d "
                    "casting=%d immobilized=%d rooted=%d st=%d%d%d%d speedRate=%.2f",
                    s.bot->GetName(),
                    pet->GetPositionX(), pet->GetPositionY(), pet->GetPositionZ(),
                    s.target->GetPositionX(), s.target->GetPositionY(), s.target->GetPositionZ(),
                    pet->IsMoving() ? 1 : 0,
                    (pet->movespline && pet->movespline->Finalized()) ? 1 : 0,
                    pet->IsNonMeleeSpellCasted(false, false, true, true) ? 1 : 0,
                    pet->IsImmobilizedState() ? 1 : 0,
                    pet->IsRooted() ? 1 : 0,
                    pet->hasUnitState(UNIT_STAT_NOT_MOVE) ? 1 : 0,
                    pet->hasUnitState(UNIT_STAT_CHASE) ? 1 : 0,
                    pet->hasUnitState(UNIT_STAT_CHASE_MOVE) ? 1 : 0,
                    pet->hasUnitState(UNIT_STAT_MELEE_ATTACKING) ? 1 : 0,
                    pet->GetSpeedRate(MOVE_RUN));
            }
        }
        return acted;
    }

    // [2026-10-30 修] 地面目标技（暴风雪/烈焰风暴/飓风/箭雨/火雨）原来**不做任何校验**就直接
    //   交给 `PlayerbotAI::CastSpell(id, x, y, z)`：那一层只看"能不能排队"，里面 `Spell::SpellStart`
    //   的 CheckCast 失败时是**静默**的（不返回错误码）⇒ 状态机以为放出去了，整条优先级表
    //   就卡死在这一个技能上，后面的兜底技能永远轮不到。
    //   实测（5 个假人一堆）：平衡德进 Aoe 后 40 秒 **0 伤害**（全是"飓风"的幽灵施法：
    //   FSM 记账 46 次、真实施法 1 次），把 balance:aoe_pack 调高绕开 Aoe 分支立刻回到 1040 dps；
    //   兽王猎同理（Aoe 里箭雨幽灵化 ⇒ 0~216 dps，绕开 Aoe 反而 471）。
    //   现在先跑一次完整的 CanCastSpell（射程/视线/资源/站姿/地形都算上，还带回结果码），
    //   失败就按资源类/其它类退避，并且**返回 false 让表里的下一个技能兜底**。
    bool TryCastAt(PlayerbotAI* ai, BotMem* m, Snapshot& s, char const* name, float x, float y, float z)
    {
        uint32 id = SpellId(ai, s.bot, m, name);
        if (!Ready(s.bot, id))
            return false;

        time_t const now = time(nullptr);
        auto it = m->failUntil.find(id);
        if (it != m->failUntil.end() && now < it->second)
            return false;

        SpellCastResult res = SPELL_FAILED_DONT_REPORT;
        if (!ai->CanCastSpell(id, x, y, z, 0, true, nullptr, false, false, false, &res))
        {
            bool const waitingResource = (res == SPELL_FAILED_NO_POWER
                || res == SPELL_FAILED_OUT_OF_RANGE || res == SPELL_FAILED_LINE_OF_SIGHT
                || res == SPELL_FAILED_NOT_READY);
            m->failUntil[id] = now + (waitingResource ? 1 : 2);
            return false;
        }
        if (!ai->CastSpell(id, x, y, z))
            return false;
        ++m->castCount;
        m->lastOk[id] = now;
        m->castThisTick = true;
        PlayerbotAI::RecordSpellCast(s.bot->GetObjectGuid(), id);
        return true;
    }

    // [2026-10-31 站长口径] **按指定 id 对地面/位置施法**：TryCastAt 只吃名字（一律顶到最高 rank），
    //   而冰法 Aoe 要按 `aoe_slow_rank` 放**低 rank 暴风雪**（1 级一样减速、蓝耗低得多）。
    //   实现照抄 TryCastAt：Ready → failUntil 退避 → CanCastSpell 带结果码 → 核心位置施法 → 记账，
    //   失败同样按资源类/其它类退避，并返回 false 让调用方回落到下一个技能。
    bool CastIdAt(PlayerbotAI* ai, BotMem* m, Snapshot& s, uint32 spellId, float x, float y, float z)
    {
        if (!Ready(s.bot, spellId))
            return false;

        time_t const now = time(nullptr);
        auto it = m->failUntil.find(spellId);
        if (it != m->failUntil.end() && now < it->second)
            return false;

        SpellCastResult res = SPELL_FAILED_DONT_REPORT;
        if (!ai->CanCastSpell(spellId, x, y, z, 0, true, nullptr, false, false, false, &res))
        {
            bool const waitingResource = (res == SPELL_FAILED_NO_POWER
                || res == SPELL_FAILED_OUT_OF_RANGE || res == SPELL_FAILED_LINE_OF_SIGHT
                || res == SPELL_FAILED_NOT_READY);
            m->failUntil[spellId] = now + (waitingResource ? 1 : 2);
            return false;
        }
        if (!ai->CastSpell(spellId, x, y, z))
            return false;
        ++m->castCount;
        m->lastOk[spellId] = now;
        m->castThisTick = true;
        PlayerbotAI::RecordSpellCast(s.bot->GetObjectGuid(), spellId);
        return true;
    }

    // =====================================================================================
    // [2026-10-29 站长口径] **放什么技能、buff 什么时候续，都由"状态 → 优先级表"决定**，
    //   不再用 if/else 把每个技能的优先级硬编码在处理器里。
    //   一条 Entry 描述一件事：
    //     name           技能名（走 SpellId 自动升级 rank）
    //     target         施法目标（nullptr=当前目标；s.bot=自身；其它=指定单位）
    //     requireAura    必须先有这个 aura（例如节能施法才丢火球）
    //     minAuraRemain  续 buff 用：自身该 aura 剩余 < 这个毫秒数就续（<=0 表示"没有就补"）
    //     auraOn         检查哪个 aura 的剩余（默认用 name）
    //     minCombo       至少几个连击点
    //     minEnergy      至少多少能量
    //   表内按优先级从上往下试，第一条满足条件的就执行。
    // =====================================================================================
    struct Entry
    {
        char const* name = nullptr;
        Unit* target = nullptr;
        char const* requireAura = nullptr;
        int minAuraRemain = 0;
        char const* auraOn = nullptr;
        int minCombo = 0;
        int minEnergy = 0;
    };

    bool TryEntry(PlayerbotAI* ai, BotMem* m, Snapshot& s, Entry const& e)
    {
        if (!e.name)
            return false;
        if (e.requireAura && AuraStacks(ai, s.bot, m, e.requireAura, s.bot) == 0)
            return false;
        if (e.minCombo > 0 && s.combo < e.minCombo)
            return false;
        if (e.minEnergy > 0 && s.energy < e.minEnergy)
            return false;
        if (e.minAuraRemain > 0)
        {
            int const remain = AuraRemain(ai, m, s, e.auraOn ? e.auraOn : e.name);
            if (remain >= e.minAuraRemain)     // 还够久，不用续
                return false;
        }
        Unit* tgt = e.target ? e.target : s.target;
        return CastChecked(ai, m, s, e.name, tgt);
    }

    bool TryTable(PlayerbotAI* ai, BotMem* m, Snapshot& s, std::initializer_list<Entry> const& table)
    {
        for (Entry const& e : table)
            if (TryEntry(ai, m, s, e))
                return true;
        return false;
    }

    bool CastFirst(PlayerbotAI* ai, BotMem* m, Snapshot& s, std::initializer_list<char const*> names, Unit* target = nullptr)
    {
        for (char const* n : names)
            if (TryCast(ai, m, s, n, target))
                return true;
        return false;
    }

    // 饰品：复用现成的 "use trinket" 动作（UseTrinketAction 自己会挑已装备、可用的使用型饰品）
    bool UseTrinket(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        time_t const now = time(nullptr);
        if (m->lastTrinketTry && now - m->lastTrinketTry < 30)
            return false;
        m->lastTrinketTry = now;
        if (!ai->DoSpecificAction("use trinket"))
            return false;
        sLog.outError("[DPSFSM] %s %s 开饰品（hp=%.0f%% res=%.0f%% tgt=%.0f%%）",
            s.bot->GetName(), ai::dpsfsm::SpecName(m->spec), s.selfHp, s.power, s.targetHp);
        return true;
    }

    // 法力药水（实现见下方 UseManaPotion）：法师的 Recover 也要用，先声明
    bool UseManaPotion(BotMem* m, Snapshot& s);

    // ============================ 法师 ============================
    bool TickMage(PlayerbotAI* ai, BotMem* m)
    {
        Snapshot s;
        BuildSnapshot(ai, m, s);
        if (!s.target)
        {
            LogTransition(ai, m, s, State::Idle);
            return true;
        }
        if (s.casting)
            return true;   // 读条/引导中，任何技能都不插

        bool const fire = (m->spec == Spec::Fire);
        bool const frost = (m->spec == Spec::Frost);

        char const* filler = fire ? "fireball" : (frost ? "frostbolt" : "arcane blast");
        float const castRange = 30.0f;
        bool const inRange = s.dist <= castRange;

        s.gcd = GcdActive(s.bot, SpellId(ai, s.bot, m, filler));

        // 节能施法：优先级表——有 clearcasting 状态时，最贵的读条法术排到最前
        if (!s.casting && TryTable(ai, m, s, {
                { "fireball",   nullptr, "clearcasting" },
                { "frostbolt",  nullptr, "clearcasting" },
                { "arcane blast", nullptr, "clearcasting" } }))
            return true;

        State st = m->state;
        if (st == State::Idle)
            st = State::Burn;

        // ---- 转移：资�?�?群架 �?斩杀/爆发 �?射程 ----
        float const lowMana = fire ? 10.0f : (frost ? 8.0f : 6.0f);
        if (s.power < lowMana)
            st = State::Recover;
        else if (st == State::Recover && s.power > lowMana + 20.0f)
            st = State::Burn;

        if (st != State::Recover && s.pack30 >= AoEPack(m))
            st = State::Aoe;
        else if (st == State::Aoe)
            st = State::Burn;

        bool const burstWindow = (s.targetHp <= BurstHp(m));
        if (st == State::Burn && burstWindow && s.dist <= castRange)
            st = State::Burst;
        else if (st == State::Burst && !burstWindow)
            st = State::Burn;

        if (st != State::Recover && st != State::Aoe && !inRange)
            st = State::Approach;
        else if (st == State::Approach && inRange)
            st = State::Burn;

        // ---- 动作：本 GCD 一�?----
        switch (st)
        {
            case State::Recover:
                // 唤醒（8 分钟一次）优先，其次法力药水，最后才是魔杖

                if (!TryCastSelf(ai, m, s, "evocation"))
                {
                    if (UseManaPotion(m, s)) break;
                    CastFirst(ai, m, s, { "shoot" });
                }
                break;

            case State::Aoe:
                if (s.dist <= 10.0f && CastFirst(ai, m, s, { "blast wave" })) break;
                if (s.dist <= 10.0f && TryCast(ai, m, s, "arcane explosion", nullptr)) break;
                if (fire)
                {
                    if (TryCastAt(ai, m, s, "flamestrike", s.target->GetPositionX(), s.target->GetPositionY(), s.target->GetPositionZ())) break;
                }
                else if (frost)
                {
                    if (TryCastAt(ai, m, s, "blizzard", s.target->GetPositionX(), s.target->GetPositionY(), s.target->GetPositionZ())) break;
                }
                CastFirst(ai, m, s, { filler });
                break;

            case State::Burst:
                if (UseTrinket(ai, m, s)) break;
                if (fire)
                {
                    // 燃烧 �?奥强? 无；火法：饰品位已用，燃�?�?气定炎爆
                    if (CastFirst(ai, m, s, { "combustion" })) break;
                    if (CastFirst(ai, m, s, { "presence of mind" })) break;
                    if (CastFirst(ai, m, s, { "pyroblast" })) break;
                    if (CastFirst(ai, m, s, { "fire blast" })) break;
                }
                else if (frost)
                {
                    if (CastFirst(ai, m, s, { "icy veins" })) break;
                    if (CastFirst(ai, m, s, { "cold snap" })) break;
                    if (CastFirst(ai, m, s, { "ice lance" })) break;
                }
                else
                {
                    if (CastFirst(ai, m, s, { "arcane power" })) break;
                    if (CastFirst(ai, m, s, { "arcane barrage" })) break;
                }
                CastFirst(ai, m, s, { filler });
                break;

            case State::Approach:
                if (s.moving && CastFirst(ai, m, s, { fire ? "fire blast" : (frost ? "ice lance" : "arcane barrage") }))
                    break;
                break;

            case State::Instant:
            case State::Burn:
            default:
            {
                st = State::Burn;
                if (s.moving)
                {
                    // 移动中只能用瞬发：火�?冰枪/弹幕
                    if (CastFirst(ai, m, s, { fire ? "fire blast" : (frost ? "ice lance" : "arcane barrage") }))
                        break;
                }

                if (fire)
                {
                    // [2026-10-29] 大招/饰品原来只在"斩杀 Burst"里放 ⇒ 打木桩（满血）永远不用。
                    //   燃烧/冰冷血脉/奥强这类"CD 好了就该放"，放进常规循环。
                    if (UseTrinket(ai, m, s)) break;
                    if (CastFirst(ai, m, s, { "combustion" })) break;
                    // 强化灼烧：目标身上叠层 <5 就补灼烧；没这天赋（从没见过该 debuff）就跳过。

                    // 叠层 debuff 与灼烧本体不是同一个 id（强化灼烧挂的是"火焰易伤"），两个名字都查。

                    int const scorchStacks = std::max(
                        AuraStacks(ai, s.bot, m, "scorch", s.target),
                        AuraStacks(ai, s.bot, m, "fire vulnerability", s.target));
                    if (scorchStacks > 0)
                        m->flags.insert("scorch seen");
                    int const scorchFull = int(TP(m, "scorch_stacks", 5.0));
                    if (scorchStacks < scorchFull && m->flags.count("scorch seen")
                        && TryCast(ai, m, s, "scorch", nullptr))
                        break;
                    // 火焰冲击 8 秒 CD、瞬发：不占读条，CD 好了就该打（原来只在移动/斩杀才放）
                    if (CastFirst(ai, m, s, { "fire blast" })) break;
                }
                else if (frost)
                {
                    if (UseTrinket(ai, m, s)) break;
                    if (CastFirst(ai, m, s, { "icy veins" })) break;
                    // 水元素：没宠物又学会就叫一只（一次性开销，别�?tick 试）
                    if (!s.bot->GetPet() && CastFirst(ai, m, s, { "summon water elemental" })) break;
                }
                else
                {
                    if (UseTrinket(ai, m, s)) break;
                    if (CastFirst(ai, m, s, { "arcane power" })) break;
                    // 奥：奥术冲击叠到 3 层就放奥术飞弹泄掉
                    // [2026-10-30 修] 叠层 debuff 是**独立 id 36032**，不在 30451 的 spell_chain 里；
                    //   原来按 "arcane blast" 查 aura 永远是 0 ⇒ 奥术飞弹整场 0 次。
                    int const blastStacks = int(s.bot->GetAuraCount(36032));
                    // 蓝够就继续奥冲（叠层既加伤害也加耗蓝）；蓝紧了再用飞弹把叠层泄掉、稳住续航
                    if (blastStacks >= 3 && s.power < 45.0f && CastFirst(ai, m, s, { "arcane missiles" })) break;
                    // [2026-10-31 修正] TBC 2.4.3 **没有奥术弹幕**（Arcane Barrage 是 WLK 的）。
                    //   原来这里挂着它：SpellId 取不到 → 每 tick 白试一次，日志更会误导读技能表的人。
                }

                CastFirst(ai, m, s, { filler });
                break;
            }
        }

        LogTransition(ai, m, s, st);
        return true;
    }

    // 天赋技能判定：这些名字只有点了对应天赋才学得会 ⇒ 用它定专精
    struct TalentSpecName { char const* name; Spec spec; };

    Spec DetectSpecByTalentsImpl(PlayerbotAI* ai, BotMem* m)
    {
        Player* bot = ai ? ai->GetBot() : nullptr;
        if (!bot)
            return Spec::Unknown;

        static TalentSpecName const mage[] = {
            { "combustion", Spec::Fire }, { "cold snap", Spec::Frost }, { "presence of mind", Spec::Arcane } };
        static TalentSpecName const warrior[] = {
            { "bloodthirst", Spec::Fury }, { "mortal strike", Spec::Arms }, { "shield slam", Spec::Protection } };
        static TalentSpecName const rogue[] = {
            { "mutilate", Spec::Assassination }, { "blade flurry", Spec::Combat }, { "preparation", Spec::Subtlety } };
        static TalentSpecName const hunter[] = {
            { "bestial wrath", Spec::BeastMastery }, { "aimed shot", Spec::Marksmanship }, { "wyvern sting", Spec::Survival } };
        static TalentSpecName const warlock[] = {
            { "unstable affliction", Spec::Affliction }, { "soul link", Spec::Demonology }, { "shadowfury", Spec::Destruction } };
        static TalentSpecName const shaman[] = {
            { "stormstrike", Spec::Enhancement }, { "elemental mastery", Spec::Elemental }, { "earth shield", Spec::Restoration } };
        static TalentSpecName const druid[] = {
            { "mangle (cat)", Spec::Feral }, { "force of nature", Spec::Balance }, { "tree of life", Spec::Restoration } };
        static TalentSpecName const paladin[] = {
            { "crusader strike", Spec::Retribution }, { "holy shield", Spec::Protection }, { "holy shock", Spec::Holy } };
        static TalentSpecName const priest[] = {
            { "shadowform", Spec::Shadow }, { "pain suppression", Spec::Discipline }, { "circle of healing", Spec::Holy } };

        TalentSpecName const* table = nullptr;
        switch (bot->getClass())
        {
            case CLASS_MAGE:    table = mage;    break;
            case CLASS_WARRIOR: table = warrior; break;
            case CLASS_ROGUE:   table = rogue;   break;
            case CLASS_HUNTER:  table = hunter;  break;
            case CLASS_WARLOCK: table = warlock; break;
            case CLASS_SHAMAN:  table = shaman;  break;
            case CLASS_DRUID:   table = druid;   break;
            case CLASS_PALADIN: table = paladin; break;
            case CLASS_PRIEST:  table = priest;  break;
            default: return Spec::Unknown;
        }

        for (int i = 0; i < 3; ++i)
            if (SpellId(ai, bot, m, table[i].name, false))
                return table[i].spec;

        return Spec::Unknown;
    }

    // [2026-10-29 站长口径] 法力职业的**法力药水**：在背包里找"使用后回蓝"的消耗品，
    //   蓝低于阈值就喝（核心的 CastItemUseSpell 会正常消耗物品并应用效果）。
    //   注意：不用 ai->DoSpecificAction("mana potion")，那个动作的 isUseful 依赖 AI 自己的战斗状态，
    //   假人场景里会被判成"不 useful"，实测一次都没喝到。
    bool UseManaPotion(BotMem* m, Snapshot& s)
    {
        // [2026-10-31 站长口径] 改成按**蓝缺口**判断：缺得超过 `potion_mana_deficit`（默认 3000）就喝。
        //   比按百分比准——不同职业/装备的蓝上限差很多，35% 对 8000 蓝的号是缺 5200、对 14000 蓝的号是缺 9100。
        //   `potion_mana` 保留为兜底（没有蓝上限信息时按百分比）。
        uint32 const maxMana = s.bot->GetMaxPower(POWER_MANA);
        int32 const deficit = int32(maxMana) - int32(s.bot->GetPower(POWER_MANA));
        bool const byDeficit = maxMana > 0 && deficit > int32(TP(m, "potion_mana_deficit", 3000.0));
        bool const byPct = maxMana == 0 && s.power <= float(TP(m, "potion_mana", 35.0));
        if (!byDeficit && !byPct)
            return false;

        time_t const now = time(nullptr);
        if (m->lastPotionTry && now - m->lastPotionTry < 3)
            return false;
        m->lastPotionTry = now;

        for (int slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        {
            Item* item = s.bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;
            ItemPrototype const* proto = item->GetProto();
            if (!proto)
                continue;

            for (int e = 0; e < MAX_ITEM_PROTO_SPELLS; ++e)
            {
                uint32 const spellId = proto->Spells[e].SpellId;
                if (!spellId || proto->Spells[e].SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
                    continue;
                SpellEntry const* entry = sServerFacade.LookupSpellInfo(spellId);
                if (!entry)
                    continue;

                bool energize = false;
                for (int i = 0; i < 3; ++i)
                    if (entry->Effect[i] == SPELL_EFFECT_ENERGIZE)
                        energize = true;
                if (!energize)
                    continue;
                if (!s.bot->IsSpellReady(spellId))
                    continue;

                SpellCastTargets targets;
                targets.setUnitTarget(s.bot);
                s.bot->CastItemUseSpell(item, targets, 0, 0);
                sLog.outError("[DPSFSM] %s 喝法力药水（item=%u, res=%.0f%%）",
                    s.bot->GetName(), proto->ItemId, s.power);
                return true;
            }
        }
        return false;
    }

    // [2026-10-31 站长口径] **没有蓝的职业（战士/盗贼）用加速药水当爆发消耗品**：
    //   只在 Burst 状态里喝；有蓝的职业一律走 UseManaPotion（这里直接返回 false）。
    //   识别方式：按物品名匹配（本服 DB 是英文，`Haste Potion`），也可用参数 `haste_potion_item`
    //   直接指定物品 id（换了自定义药水时用）。
    // [2026-11-01 站长口径] 惩戒骑的爆发药水：骑士**有蓝**但加速药水一样有用（爆发窗口里换的是伤害，
    //   不是续航），所以给 UseHastePotion 加一个"允许有蓝职业"的开关，默认关闭 ——
    //   战士/盗贼的调用点不传这个参数，行为一个字节不变。
    bool UseHastePotion(BotMem* m, Snapshot& s, bool allowManaClass = false)
    {
        if (!allowManaClass && s.bot->GetMaxPower(POWER_MANA) > 0)
            return false;

        time_t const now = time(nullptr);
        if (m->lastPotionTry && now - m->lastPotionTry < 3)
            return false;
        m->lastPotionTry = now;

        uint32 const wantId = uint32(TP(m, "haste_potion_item", 0.0));
        for (int slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        {
            Item* item = s.bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;
            ItemPrototype const* proto = item->GetProto();
            if (!proto)
                continue;

            bool const match = wantId ? (proto->ItemId == wantId)
                                      : (std::string(proto->Name1).find("Haste Potion") != std::string::npos);
            if (!match)
                continue;

            for (int e = 0; e < MAX_ITEM_PROTO_SPELLS; ++e)
            {
                uint32 const spellId = proto->Spells[e].SpellId;
                if (!spellId || proto->Spells[e].SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
                    continue;
                if (!s.bot->IsSpellReady(spellId))
                    continue;

                SpellCastTargets targets;
                targets.setUnitTarget(s.bot);
                s.bot->CastItemUseSpell(item, targets, 0, 0);
                sLog.outError("[DPSFSM] %s 喝加速药水（item=%u）", s.bot->GetName(), proto->ItemId);
                return true;
            }
        }
        return false;
    }

    // [2026-10-29 站长口径] 近战输出要**找背**：毁伤/背刺/撕碎都要求站在目标背后，
    //   其它近战站背后也不会被招架/躲闪。这里不自己写寻路——算出目标背后 0.8×攻击距离的点，
    //   用引擎的 MovePoint 走过去；已经在背后就不动。
    bool GetBehind(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        if (!s.target || s.dist > 15.0f)
            return false;
        if (s.bot->IsFacingTargetsBack(s.target))
            return false;

        time_t const now = time(nullptr);
        if (m->lastBehindTry && now - m->lastBehindTry <= 1)
            return false;
        m->lastBehindTry = now;

        float const dist = s.bot->GetCombinedCombatReach(s.target, true) * 0.8f;
        float const ang = s.target->GetOrientation();
        float x = s.target->GetPositionX() - cos(ang) * dist;
        float y = s.target->GetPositionY() - sin(ang) * dist;
        float z = s.target->GetPositionZ();
        s.bot->UpdateGroundPositionZ(x, y, z);
        s.bot->GetMotionMaster()->MovePoint(0, x, y, z);
        return true;
    }

    // [2026-10-30 修] **近战要真的站进 MeleeReach 里**，不能只看"表面距离够近"。
    //   背景（实测 40 秒 melee=0 / 十字军打击 0 次）：
    //     · Snapshot.dist 是 bot->GetDistance(target)，默认 DIST_CALC_BOUNDING_RADIUS
    //       ⇒ 是"减去双方体积半径之后的表面距离"，比核心的判定小 0.5~1 码；
    //     · 核心 Unit::CanReachWithMeleeAttack 用**中心距离**比 GetCombinedCombatReach(...,true)
    //       （其下限就是 ATTACK_DISTANCE=5.0，而且是严格 `<`）。
    //   于是 bot 站在表面距离 4.5（中心 5.00）时：状态机以为"在近战范围"（<= 6.5），不出 Approach；
    //   核心却认为够不着 ⇒ 一次平砍都不挥、Crusader Strike 一直被 CheckCast 判 OUT_OF_RANGE。
    //   引擎的 "reach melee" 也救不了：ReachTargetAction 的 range=0 ⇒ ChaseTo(target, 0) 里
    //   endPoint = botPoint + dir*min(0, dist) = 自己脚下，路径长度为 0 ⇒ 原地不动。
    //   所以这里由状态机自己贴上去：走到"够得着"的内侧点（0.8 倍 MeleeReach）。
    bool CloseToMelee(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        if (!s.target || !s.bot->IsInWorld())
            return false;

        time_t const now = time(nullptr);
        if (m->lastCloseTry && now - m->lastCloseTry <= 0)   // 1 秒一次，别每 tick 都改路径
            return false;
        m->lastCloseTry = now ? now : 1;

        float const reach = s.bot->GetCombinedCombatReach(s.target, true);
        float const want = (reach > 1.0f) ? reach * 0.8f : 2.0f;   // 站到"够得着"的内侧
        float const dx = s.bot->GetPositionX() - s.target->GetPositionX();
        float const dy = s.bot->GetPositionY() - s.target->GetPositionY();
        float const len = sqrtf(dx * dx + dy * dy);
        if (len < 0.05f)
            return false;

        float x = s.target->GetPositionX() + dx / len * want;
        float y = s.target->GetPositionY() + dy / len * want;
        float z = s.target->GetPositionZ();
        s.bot->UpdateGroundPositionZ(x, y, z);
        s.bot->GetMotionMaster()->MovePoint(0, x, y, z);
        return true;
    }

    // [2026-10-29 实测] 英勇打击/顺劈/重殴这类"下一次挥击附带"的技能没有冷却，
    //   每个 AI tick 都能重新施放 —— 实测 arms 40 秒刷了 1116 次、fury 934 次，
    //   既白烧怒气也把施法统计搞脏。它们排进 CURRENT_MELEE_SPELL 就说明已经挂上了，不用再放。
    // [2026-10-29 站长口径] 资源账本：怒气/能量/法力的真实消耗一律从 SpellEntry->manaCost 读，
    //   不写死数字（不同等级/不同 rank 的消耗不同）。这样"放了这一手还能不能放下关键技能"才判得准。
    int SpellCost(PlayerbotAI* ai, BotMem* m, char const* name)
    {
        uint32 const id = SpellId(ai, ai->GetBot(), m, name, false);
        SpellEntry const* entry = id ? sServerFacade.LookupSpellInfo(id) : nullptr;
        return entry ? int(entry->manaCost) : 0;
    }

    bool TrySwing(PlayerbotAI* ai, BotMem* m, Snapshot& s, char const* name)
    {
        if (s.bot->GetCurrentSpell(CURRENT_MELEE_SPELL))
            return false;
        return TryCast(ai, m, s, name, nullptr);
    }

    // [2026-10-31 站长口径] 这一手蓝够不够放：按 `SpellEntry->manaCost`（与 [DPSFSM-MANA] 的账本同一口径）
    //   跟**当前法力绝对值**比，而不是拿百分比拍脑袋（不同 rank 的消耗差一倍以上）。
    //   cost<=0（拿不到模板 / 无消耗技能）⇒ 不拦。
    bool AffordSpell(PlayerbotAI* ai, BotMem* m, Snapshot& s, char const* name)
    {
        int const cost = SpellCost(ai, m, name);
        if (cost <= 0)
            return true;
        return int(s.bot->GetPower(POWER_MANA)) >= cost;
    }

    // [2026-10-30] **自身中心的"敌方"AoE（奉献）**：两条现成入口都进不去——
    //   ① `TryCast`/`CanCastSpell(自己)`：IsPositiveSpell(奉献)=false（带伤害），于是
    //      PlayerbotAI::CanCastSpell 里"负法术 + 友方目标"⇒ SPELL_FAILED_TARGET_FRIENDLY(110)；
    //   ② `TryCastAt(x,y,z)`：PlayerbotAI::CastSpell(x,y,z) 要求 Targets 里有 DEST/SOURCE_LOCATION，
    //      而奉献是 TARGET_UNIT_CASTER（Targets=0）⇒ 直接 return false。
    //   这里照引擎 SPELL_ACTION 的做法走核心 `Unit::CastSpell`（单位目标是敌人，法术隐式目标是施法者），
    //   自己守 GCD / 冷却，成功才算一次施法。
    bool CastSelfCentered(PlayerbotAI* ai, BotMem* m, Snapshot& s, char const* name)
    {
        uint32 const id = SpellId(ai, s.bot, m, name);
        if (!Ready(s.bot, id))
            return false;
        SpellEntry const* entry = sServerFacade.LookupSpellInfo(id);
        if (entry && s.bot->HasGCD(entry))
            return false;    // 别顶掉正在转的 GCD

        // 目标是**自己**：奉献的 RangeIndex=1 是"自身 0 码"，拿敌人当单位目标会被核心判
        //   SPELL_FAILED_OUT_OF_RANGE(0x5C=92)（实测）。核心不像 bot 的 CanCastSpell 那样做正负法术
        //   启发式，所以"对自己放一个带伤害的自身 AoE"是合法的（客户端点奉献就是这个效果）。
        Unit* const tgt = s.bot;
        SpellCastResult const res = s.bot->CastSpell(tgt, id, uint32(TRIGGERED_NONE));
        if (res != SPELL_CAST_OK)
        {
            time_t const now = time(nullptr);
            if (!m->lastConsLog || now - m->lastConsLog >= 10)
            {
                m->lastConsLog = now;
                sLog.outError("[DPSFSM] %s %s 放不出：id=%u result=%d（0=OK，其它见 SpellCastResult）",
                    s.bot->GetName(), name, id, int(res));
            }
            return false;
        }
        ++m->castCount;
        m->castThisTick = true;
        m->lastOk[id] = time(nullptr);
        PlayerbotAI::RecordSpellCast(s.bot->GetObjectGuid(), id);
        return true;
    }

    // ============================ 战士 ============================
    bool TickWarrior(PlayerbotAI* ai, BotMem* m)
    {
        Snapshot s;
        BuildSnapshot(ai, m, s);
        if (!s.target) { LogTransition(ai, m, s, State::Idle); return false; }   // 交回引擎重选目标
        if (s.casting) return true;
        if (m->spec == Spec::Protection) return false;   // 防御坦的技能后面单独做

        GetBehind(ai, m, s);   // 绕背：只走位，不占这个 GCD   // 找背
        bool const fury = (m->spec == Spec::Fury);
        bool const inRange = s.dist <= 6.5f;   // 近战够得着就行（5.0 太紧，卡在 5.2 会永远不进循环）
        bool const executeWindow = s.targetHp <= BurstHp(m);

        State st = (m->state == State::Idle) ? State::Burn : m->state;
        if (s.pack10 >= AoEPack(m) && inRange)
            st = State::Aoe;
        else if (st == State::Aoe)
            st = State::Burn;
        if (st == State::Burn && executeWindow)
            st = State::Burst;
        else if (st == State::Burst && !executeWindow)
            st = State::Burn;
        if (st != State::Aoe && !inRange)
            st = State::Approach;
        else if (st == State::Approach && inRange)
            st = State::Burn;

        switch (st)
        {
            case State::Approach:
                break;
            case State::Aoe:
                if (CastFirst(ai, m, s, { "sweeping strikes" })) break;
                if (CastFirst(ai, m, s, { "whirlwind" })) break;
                if (s.rage >= 60 && TrySwing(ai, m, s, "cleave")) break;
                break;
            case State::Burst:
                if (UseTrinket(ai, m, s)) break;
                if (fury && CastFirst(ai, m, s, { "death wish" })) break;
                if (CastFirst(ai, m, s, { "execute" })) break;
                CastFirst(ai, m, s, { fury ? "bloodthirst" : "mortal strike" });
                break;
            case State::Burn:
            default:
            {
                st = State::Burn;
                if (UseTrinket(ai, m, s)) break;
                // [2026-10-30] 低血段（<BurstHp）先打斩杀：核心对斩杀有 TargetAuraState=2 的门，
                //   只有目标 <20% 血才成立；不放在最前会被猛击/致死把怒气吃光（实测低血木桩仍只出致死）。
                if (executeWindow)
                {
                    uint32 const exId = SpellId(ai, s.bot, m, "execute", false);
                    bool const has = exId && s.bot->HasSpell(exId);
                    SpellCastResult res = SPELL_FAILED_DONT_REPORT;
                    bool const can = exId != 0 && ai->CanCastSpell(exId, s.target, 0, false, nullptr, false, false, false, &res);
                    static time_t lastExLog = 0;
                    if (lastExLog + 3 <= time(nullptr))
                    {
                        lastExLog = time(nullptr);
                        sLog.outError("[EXECUTE-DBG] %s exId=%u has=%d can=%d res=%d hp=%.1f burst=%.0f rage=%.0f",
                            s.bot->GetName(), exId, has ? 1 : 0, can ? 1 : 0, int(res), s.targetHp, BurstHp(m), s.rage);
                    }
                    if (CastFirst(ai, m, s, { "execute" })) break;
                }
                if (fury && CastFirst(ai, m, s, { "death wish" })) break;
                // [2026-10-30 修] 狂暴战必须在**狂暴姿态**：旋风斩(1680) Stances=0x40000 只要狂暴姿态，
                //   实测 bot 默认战斗姿态(17) ⇒ whirlwind 每次都被判 ONLY_SHAPESHIFT(89)、整场 0 次。
                //   姿态切换只占一个 GCD，开场切一次就够（切完一直保持）。
                if (fury && s.bot->GetShapeshiftForm() != FORM_BERSERKERSTANCE)
                {
                    if (TryCastSelf(ai, m, s, "berserker stance")) break;
                }

                // 怒气账本：先看"这一手放完，下一个 CD 到点的关键技能（嗜血/致死）还够不够怒"
                char const* core = fury ? "bloodthirst" : "mortal strike";
                int const coreCost = SpellCost(ai, m, core);
                int const wwCost = SpellCost(ai, m, "whirlwind");
                int const hsCost = SpellCost(ai, m, "heroic strike");
                int const slamCost = SpellCost(ai, m, "slam");
                int const hsFloor = int(TP(m, "hs_rage", 75.0));

                if (s.rage >= coreCost && CastFirst(ai, m, s, { core })) break;
                if (s.rage >= wwCost && CastFirst(ai, m, s, { "whirlwind" })) break;
                if (CastFirst(ai, m, s, { "overpower" })) break;          // 压制只花 5 怒，随时可打
                // [2026-10-30 修] 猛击(15 怒) 会吃掉致死(30 怒) 的怒气：实测 arms 40 秒只出 12 次猛击、
                //   致死 **0 次**（怒气在 150~299 之间被猛击一路吃光，永远攒不到 300）。
                //   所以猛击必须给致死留够一发：rage >= 致死 + 猛击。
                if (!fury && s.rage >= slamCost + coreCost && CastFirst(ai, m, s, { "slam" })) break;
                // 英勇/顺劈是"下次挥击附带"，会顶掉普攻（换怒）⇒ 必须给下一个 core 留够怒
                if (s.rage >= coreCost + hsCost && s.rage >= hsFloor
                    && TrySwing(ai, m, s, "heroic strike")) break;
                break;   // 怒不够就等普攻，别硬放（硬放只会让核心技能永远没怒）
            }
        }

        LogTransition(ai, m, s, st);
        return true;
    }

    // ============================ 盗贼 ============================
    bool TickRogue(PlayerbotAI* ai, BotMem* m)
    {
        Snapshot s;
        BuildSnapshot(ai, m, s);
        if (!s.target) { LogTransition(ai, m, s, State::Idle); return false; }   // 交回引擎重选目标
        if (s.casting) return true;

        bool const inRange = s.dist <= 6.5f;   // 近战够得着就行（5.0 太紧，卡在 5.2 会永远不进循环）
        bool const mutilate = (m->spec == Spec::Assassination);
        GetBehind(ai, m, s);   // 绕背：只走位，不占这个 GCD   // 毁伤/背刺要背身
        bool const executeWindow = s.targetHp <= BurstHp(m);
        // [2026-10-30 修] 盗贼原来**根本没有 Aoe 状态**（`aoe_pack` / `combat:aoe_pack` 对盗贼是死配置），
        //   群战里跟单体一模一样。TBC 盗贼没有真正的群体技（刀扇 Fan of Knives 是 WLK 才有），
        //   群战能做的只有：剑刃乱舞（额外打一个目标 + 20% 攻速）和冲动，其余照常攒星泄星。
        //   所以这里补一个 Aoe 状态，把这两张 CD 技顶到最前面，后面的循环完全复用单体表。
        bool const aoe = (s.pack10 >= AoEPack(m));

        State st = (m->state == State::Idle) ? State::Burn : m->state;
        if (!inRange)
            st = State::Approach;
        else if (st == State::Approach)
            st = State::Burn;
        if (st != State::Approach && aoe)
            st = State::Aoe;
        else if (st == State::Aoe)
            st = State::Burn;
        if (st == State::Burn && executeWindow)
            st = State::Burst;
        else if (st == State::Burst && !executeWindow)
            st = State::Burn;

        switch (st)
        {
            case State::Approach:
                break;
            case State::Aoe:
                // 群战：先把两张 CD 技挂上（剑刃乱舞 = 额外打一个目标 + 20% 攻速；冲动 = 回能量），
                //   之后完全沿用下面的单体循环——TBC 盗贼的"群伤"就是这两张 CD 技，没有别的。
                if (UseTrinket(ai, m, s)) break;
                if (!mutilate && CastFirst(ai, m, s, { "adrenaline rush" })) break;
                if (CastFirst(ai, m, s, { "blade flurry" })) break;
                [[fallthrough]];
            case State::Burst:
                if (st == State::Burst)
                {
                    if (UseTrinket(ai, m, s)) break;
                    if (mutilate && CastFirst(ai, m, s, { "cold blood" })) break;
                    if (!mutilate && CastFirst(ai, m, s, { "adrenaline rush" })) break;
                    if (CastFirst(ai, m, s, { "blade flurry" })) break;
                    break;
                }
                [[fallthrough]];
            case State::Burn:
            default:
            {
                if (st != State::Aoe) st = State::Burn;
                // 切割常驻：有星就先补，再攒星；终结技走毒�?剔骨
                if (UseTrinket(ai, m, s)) break;
                if (!mutilate && CastFirst(ai, m, s, { "adrenaline rush" })) break;
                if (CastFirst(ai, m, s, { "blade flurry" })) break;
                if (mutilate && CastFirst(ai, m, s, { "cold blood" })) break;

                // 能量账本：终结技会清空连击点，放完还要留够一发攒星技的能量，否则出现"空星干等"
                char const* builder = mutilate ? "mutilate" : "sinister strike";
                char const* finisher = mutilate ? "envenom" : "eviscerate";
                int const builderCost = SpellCost(ai, m, builder);
                int const finisherCost = SpellCost(ai, m, finisher);
                int const sndCp = int(TP(m, "snd_cp", 4.0));      // 切割要几星
                int const finCp = int(TP(m, "fin_cp", 5.0));      // 终结技要几星
                int const evisEnergy = int(TP(m, "fin_energy", 45.0));  // 放终结技前至少留这么多能量

                // 切割：优先级表里一条"剩余 < 2.5s 且连击点够就续"的自身 buff 条目
                // [2026-10-30 修] 目标必须填**当前敌人**，不能填自己：核心 Spell::CheckCast 对连击点法术
                //   要求 `unitTarget == GetComboTargetGuid()`，对自己放会判 NO_COMBO_POINTS(73)
                //   （实测切割 40 秒 0 次，全是 res=73）。切割的隐式目标本来就是施法者，单位目标给敌人没关系。
                if (TryTable(ai, m, s, {
                        { "slice and dice", nullptr, nullptr, 2500, nullptr, sndCp, 0 } }))
                    break;
                if (s.combo >= finCp && s.energy >= evisEnergy)
                {
                    // [2026-10-30 修] 刺杀：毒伤(Envenom)要求目标身上有中毒状态（TargetAuraState=16，实测 res=106
                    //   TARGET_AURASTATE），木桩/没上毒的怪身上永远没有 ⇒ 星级满了也放不出终结技，
                    //   只能反复攒星。改成"毒伤放不出就剔骨兜底"。
                    if (mutilate && CastFirst(ai, m, s, { "envenom", "eviscerate" })) break;
                    if (!mutilate && CastFirst(ai, m, s, { "eviscerate" })) break;
                }
                // 星已经满、能量还不够放终结技时**别再攒星**（连击点上限 5，多打的全浪费）
                if (s.combo >= finCp)
                    break;
                // 毁灭/毁伤要求匕首 + 背后；随机 bot 经常不满足 ⇒ 一定留个邪恶攻击兜底，
                //   否则整场 0 施法（实测 assassination 40 秒 casts=0、只有 2 次平砍）。
                if (CastFirst(ai, m, s, mutilate ? std::initializer_list<char const*>{ "mutilate", "sinister strike" }
                                                 : std::initializer_list<char const*>{ "sinister strike" })) break;
                break;
            }
        }

        LogTransition(ai, m, s, st);
        return true;
    }

    // ============================ 猎人 ============================
    bool TickHunter(PlayerbotAI* ai, BotMem* m)
    {
        Snapshot s;
        BuildSnapshot(ai, m, s);
        if (!s.target) { LogTransition(ai, m, s, State::Idle); return false; }   // 交回引擎重选目标
        if (s.casting) return true;

        bool const mm = (m->spec == Spec::Marksmanship);

        // 8 码内是猎人死角：移动交给引擎（它现在会按射程停住，见 GetRange("attack")），
        //   状态机这里只判断"能不能打"。
        if (s.dist < 8.0f)
            return false;
        bool const inRange = s.dist <= 35.0f;
        bool const executeWindow = s.targetHp <= BurstHp(m);

        // [2026-10-29 站长口径] 猎人的"自动射击时间"本身就是一个状态：
        //   getAttackTimer(RANGED_ATTACK) = 下一发自动射击还剩多少毫秒；稳固射击 1.5s 读条。
        //   只有在"剩余时间 > 读条 + 余量"时才能读，否则会把这一发自动射击顶掉（净亏伤害）；
        //   窗口不够就进 Weave 等一发（Weave 是本状态机的正式状态，不是"卡住"）。
        int constexpr STEADY_CAST_MS = 1500;
        int constexpr AIMED_CAST_MS = 3000;
        uint32 const autoRemainMs = s.bot->getAttackTimer(RANGED_ATTACK);
        // 自动射击没起手时计时器是 0，只按"窗口够不够"判会永远进 Weave（实测 marks 整场 casts=0）。
        // [2026-10-29 实测] 自动射击不是普通法术：必须走引擎的 "auto shot" 动作
        //   （CastAutoShotAction 会登记 CURRENT_AUTOREPEAT_SPELL）。而且**不能只看计时器**：
        //   没有自动射击在跑时 getAttackTimer(RANGED_ATTACK) 也可能是非 0，于是永远不尝试。
        //   这里改成"只要当前没有自动射击在跑，就每 tick 试一次"。
        if (!s.bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL) && !s.moving)
        {
            // [2026-10-29 站长口径] 自动射击走"双通道"：
            //   ① 引擎动作 CastAutoShotAction（会登记 CURRENT_AUTOREPEAT_SPELL）
            //   ② 动作被拒时直接 CastSpell 一发，再检查登记是否成功
            //   两路结果都记日志，方便判断到底是"动作不可用"还是"武器/弹药问题"。
            bool okAction = ai->DoSpecificAction("auto shot", Event(), true);
            bool okCast = false;
            if (!okAction)
                okCast = TryCast(ai, m, s, "auto shot", s.target);
            bool const registered = s.bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL) != nullptr;

            uint32 const nowSec = uint32(time(nullptr));
            if (m->lastEngageLog + 5 <= nowSec)
            {
                m->lastEngageLog = nowSec;
                sLog.outError("[DPSFSM-AUTO] %s 自动射击: action=%d cast=%d registered=%d dist=%.1f",
                    s.bot->GetName(), okAction ? 1 : 0, okCast ? 1 : 0, registered ? 1 : 0, s.dist);
            }
            if (okAction || okCast)
                return true;
        }
        uint32 const weaveMargin = uint32(TP(m, "weave_margin", 150.0));
        // [2026-10-29 站长口径] 猎人的时间轴要单独算：
        //   读条技（稳固 1.5s / 瞄准 3s）只在"距下一发自动射击还够"时读；
        //   瞬发技（多重/奥术/毒蛇/印记）也不能贴着自动射击放，否则这一发自动射击会被推迟。
        bool const canSteady = !s.moving && autoRemainMs > uint32(STEADY_CAST_MS + int(weaveMargin));
        bool const canAimed = !s.moving && autoRemainMs > uint32(AIMED_CAST_MS + int(weaveMargin));
        bool const canInstant = !s.moving && (autoRemainMs == 0 || autoRemainMs > 700);

        State st = (m->state == State::Idle) ? State::Burn : m->state;
        if (s.pack30 >= AoEPack(m))
            st = State::Aoe;
        else if (st == State::Aoe)
            st = State::Burn;
        if (st == State::Burn && executeWindow)
            st = State::Burst;
        else if (st == State::Burst && !executeWindow)
            st = State::Burn;
        if (s.power < 10.0f)
            st = State::Recover;
        else if (st == State::Recover && s.power > 30.0f)
            st = State::Burn;
        if (st != State::Recover && st != State::Aoe && !inRange)
            st = State::Approach;
        else if (st == State::Approach && inRange)
            st = canSteady ? State::Burn : State::Weave;
        if (st == State::Weave && (canSteady || !inRange))
            st = State::Burn;

        switch (st)
        {
            case State::Approach:
                break;
            case State::Weave:
                // [2026-10-29 实测] 时间轴窗口不好时不能整场空转：
                //   有的 bot（无弹药/动作被拒）根本拉不起自动射击，会卡在 Weave 到结束（casts=0）。
                //   兜底：放一发能吃 GCD 的瞬发，至少不浪费整段时间。
                if (CastFirst(ai, m, s, { "arcane shot", "multi-shot" })) break;
                break;
            case State::Recover:
                // 蝰蛇守护回蓝（有就用），否则平射
                if (AuraStacks(ai, s.bot, m, "aspect of the viper", s.bot) == 0
                    && TryCastSelf(ai, m, s, "aspect of the viper")) break;
                CastFirst(ai, m, s, { "shoot" });
                break;
            case State::Aoe:
                if (canInstant && TryCastAt(ai, m, s, "volley", s.target->GetPositionX(), s.target->GetPositionY(), s.target->GetPositionZ())) break;
                if (canInstant && CastFirst(ai, m, s, { "multi-shot" })) break;
                if (canSteady && CastFirst(ai, m, s, { "steady shot" })) break;
                st = State::Weave;
                break;
            case State::Burst:
                if (UseTrinket(ai, m, s)) break;
                if (CastFirst(ai, m, s, { "bestial wrath" })) break;
                if (CastFirst(ai, m, s, { "kill command" })) break;
                if (mm && canAimed && CastFirst(ai, m, s, { "aimed shot" })) break;
                if (canSteady && CastFirst(ai, m, s, { "steady shot" })) break;
                st = State::Weave;
                break;
            case State::Burn:
            default:
            {
                st = State::Burn;
                if (UseTrinket(ai, m, s)) break;
                if (CastFirst(ai, m, s, { "bestial wrath" })) break;
                if (CastFirst(ai, m, s, { "kill command" })) break;
                if (canInstant && AuraStacks(ai, s.bot, m, "hunter's mark", s.target) == 0
                    && CastFirst(ai, m, s, { "hunter's mark" })) break;
                if (canInstant && AuraStacks(ai, s.bot, m, "serpent sting", s.target) == 0
                    && CastFirst(ai, m, s, { "serpent sting" })) break;
                if (mm && canAimed && CastFirst(ai, m, s, { "aimed shot" })) break;
                if (canSteady && CastFirst(ai, m, s, { "steady shot" })) break;
                if (canInstant && CastFirst(ai, m, s, { "multi-shot" })) break;
                if (canInstant && CastFirst(ai, m, s, { "arcane shot" })) break;
                st = State::Weave;   // 自动射击窗口不够：等，别读条顶掉
                break;
            }
        }

        // [2026-10-29 站长口径] 自动射击的"核心链路"：
        //   核心 `Player::CastSpell()` 在**每次施法前**都会 `InterruptSpell(CURRENT_AUTOREPEAT_SPELL)`，
        //   所以每放一发稳固/奥术/多重，自动射击就会被顶掉；只在"没登记时"补挂的话，
        //   中间那 0.5 秒起手（m_AutoRepeatFirstCast）会把自动射击饿死（实测 40 秒只出 1 发）。
        //   正确做法：**每发技能之后立刻重新挂自动射击**（Auto Shot 不吃 GCD，已在 NoGcd 白名单）。
        if (!s.bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL) && !s.moving && s.dist >= 8.0f)
        {
            if (!TryCast(ai, m, s, "auto shot", s.target))
                ai->DoSpecificAction("auto shot", Event(), true);

        }

        LogTransition(ai, m, s, st);
        return true;
    }

    // ============================ 术士 ============================
    bool TickWarlock(PlayerbotAI* ai, BotMem* m)
    {
        Snapshot s;
        BuildSnapshot(ai, m, s);
        if (!s.target) { LogTransition(ai, m, s, State::Idle); return false; }   // 交回引擎重选目标
        if (s.casting) return true;

        bool const affliction = (m->spec == Spec::Affliction);
        bool const destruction = (m->spec == Spec::Destruction);
        bool const inRange = s.dist <= 30.0f;
        bool const executeWindow = s.targetHp <= BurstHp(m);

        State st = (m->state == State::Idle) ? State::Burn : m->state;
        if (s.power < 12.0f && s.selfHp > 55.0f)
            st = State::Recover;
        else if (st == State::Recover && s.power > 40.0f)
            st = State::Burn;

        if (st != State::Recover && s.pack30 >= AoEPack(m))
            st = State::Aoe;
        else if (st == State::Aoe)
            st = State::Burn;
        if (st == State::Burn && executeWindow)
            st = State::Burst;
        else if (st == State::Burst && !executeWindow)
            st = State::Burn;
        if (st != State::Recover && st != State::Aoe && !inRange)
            st = State::Approach;
        else if (st == State::Approach && inRange)
            st = State::Burn;

        switch (st)
        {
            case State::Approach:
                break;
            case State::Recover:
                // 生命分流把血换成蓝（血够厚才换）

                CastFirst(ai, m, s, { "life tap" });
                break;
            case State::Aoe:
                // [2026-10-30 修] 种子(Seed of Corruption) 对**同一个目标**重复种会把上一颗顶掉：
                //   上一颗永远等不到"到期/受创引爆"，等于每 2 秒白读一次条。
                //   实测群战 40 秒种了 16~18 颗，总伤害只有 4~17 千（对比参考专精的 4~7 万）。
                //   现在：每个目标只种一颗；群体主输出改用地狱火雨(Rain of Fire，地面通道技)，
                //   火雨也不满足条件（站位/蓝不够）时才回到暗影箭。
                if (AuraStacks(ai, s.bot, m, "seed of corruption", s.target) == 0
                    && CastFirst(ai, m, s, { "seed of corruption" })) break;
                if (TryCastAt(ai, m, s, "rain of fire", s.target->GetPositionX(), s.target->GetPositionY(), s.target->GetPositionZ())) break;
                if (CastFirst(ai, m, s, { "shadow bolt" })) break;
                break;
            case State::Burst:
                if (UseTrinket(ai, m, s)) break;
                if (CastFirst(ai, m, s, { "shadowburn" })) break;
                if (CastFirst(ai, m, s, { "shadow bolt" })) break;
                break;
            case State::Burn:
            default:
            {
                st = State::Burn;
                if (UseTrinket(ai, m, s)) break;
                if (AuraStacks(ai, s.bot, m, "curse of elements", s.target) == 0
                    && AuraStacks(ai, s.bot, m, "curse of shadows", s.target) == 0
                    && AuraStacks(ai, s.bot, m, "curse of agony", s.target) == 0)
                {
                    if (affliction || destruction)
                    {
                        if (CastFirst(ai, m, s, { "curse of elements" })) break;
                    }
                    if (CastFirst(ai, m, s, { "curse of agony" })) break;
                }
                if (AuraStacks(ai, s.bot, m, "corruption", s.target) == 0 && CastFirst(ai, m, s, { "corruption" })) break;
                if (affliction)
                {
                    if (AuraStacks(ai, s.bot, m, "siphon life", s.target) == 0 && CastFirst(ai, m, s, { "siphon life" })) break;
                    if (AuraStacks(ai, s.bot, m, "unstable affliction", s.target) == 0 && CastFirst(ai, m, s, { "unstable affliction" })) break;
                }
                if (destruction)
                {
                    bool const immo = AuraStacks(ai, s.bot, m, "immolate", s.target) > 0;
                    if (!immo && CastFirst(ai, m, s, { "immolate" })) break;
                    // [2026-10-30 修] 燃烧(Conflagrate) 要目标身上有献祭，原来的表里根本没有这个技能
                    if (immo && CastFirst(ai, m, s, { "conflagrate" })) break;
                }
                if (affliction && CastFirst(ai, m, s, { "drain life" })) break;
                CastFirst(ai, m, s, { "shadow bolt" });
                break;
            }
        }

        LogTransition(ai, m, s, st);
        return true;
    }

    // ============================ 萨满 ============================
    // [2026-10-30 修] 增强萨的输出有一大块在图腾上（大地之力/优雅/灼热/法力之泉），
    //   而原来的优先级表里**一个图腾都没有** ⇒ 实测 40 秒只有风暴打击 + 震击。
    //   这里按"这个插槽空着就补一个"来做（图腾被拆/到期/没人插都会走到这里），
    //   没学的图腾（低等级/别的元素图腾）直接跳过，不占 GCD。
    bool TotemUpkeep(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        struct TotemSlotName { TotemSlot slot; char const* name; };
        static TotemSlotName const kTotems[] = {
            { TOTEM_SLOT_EARTH, "strength of earth totem" },
            { TOTEM_SLOT_AIR,   "grace of air totem" },
            { TOTEM_SLOT_FIRE,  "searing totem" },
            { TOTEM_SLOT_WATER, "mana spring totem" },
        };
        for (TotemSlotName const& t : kTotems)
        {
            if (!s.bot->GetTotemGuid(t.slot).IsEmpty())
                continue;
            if (!SpellId(ai, s.bot, m, t.name, false))
                continue;
            if (TryCastSelf(ai, m, s, t.name))
                return true;
        }
        return false;
    }

    bool TickShaman(PlayerbotAI* ai, BotMem* m)
    {
        Snapshot s;
        BuildSnapshot(ai, m, s);
        if (!s.target) { LogTransition(ai, m, s, State::Idle); return false; }   // 交回引擎重选目标
        if (s.casting) return true;

        bool const enhance = (m->spec == Spec::Enhancement);
        if (enhance) GetBehind(ai, m, s);   // 增强萨找背
        bool const inRange = enhance ? (s.dist <= 5.0f) : (s.dist <= 25.0f);
        bool const executeWindow = s.targetHp <= BurstHp(m);

        State st = (m->state == State::Idle) ? State::Burn : m->state;
        if (s.power < 10.0f)
            st = State::Recover;
        else if (st == State::Recover && s.power > 30.0f)
            st = State::Burn;
        if (st != State::Recover && s.pack10 >= AoEPack(m) && enhance)
            st = State::Aoe;
        else if (st == State::Aoe && !(s.pack10 >= AoEPack(m) && enhance))
            st = State::Burn;
        if (st == State::Burn && executeWindow)
            st = State::Burst;
        else if (st == State::Burst && !executeWindow)
            st = State::Burn;
        if (st != State::Recover && st != State::Aoe && !inRange)
            st = State::Approach;
        else if (st == State::Approach && inRange)
            st = State::Burn;

        switch (st)
        {
            case State::Approach:
                break;
            case State::Recover:
                if (UseManaPotion(m, s)) break;
                CastFirst(ai, m, s, { "shoot" });
                break;
            case State::Aoe:
                if (CastFirst(ai, m, s, { "chain lightning" })) break;
                if (CastFirst(ai, m, s, { "magma totem" })) break;
                TryCastSelf(ai, m, s, "lightning shield");
                break;
            case State::Burst:
                if (UseTrinket(ai, m, s)) break;
                if (enhance)
                {
                    if (TotemUpkeep(ai, m, s)) break;
                    if (TryCastSelf(ai, m, s, "bloodlust")) break;
                    if (TryCastSelf(ai, m, s, "heroism")) break;
                    if (TryCastSelf(ai, m, s, "shamanistic rage")) break;
                    if (CastFirst(ai, m, s, { "stormstrike" })) break;
                }
                else
                {
                    if (CastFirst(ai, m, s, { "elemental mastery" })) break;
                    if (CastFirst(ai, m, s, { "chain lightning" })) break;
                }
                CastFirst(ai, m, s, { enhance ? "earth shock" : "lightning bolt" });
                break;
            case State::Burn:
            default:
            {
                st = State::Burn;
                if (UseTrinket(ai, m, s)) break;
                if (enhance)
                {
                    if (TotemUpkeep(ai, m, s)) break;
                    // [2026-10-30] 萨满之怒/嗜血/英雄都是自身增益：原来走 CastFirst（对敌目标）
                    //   被判 TARGET_ENEMY(108) ⇒ 整场 0 次。改 TryCastSelf。
                    if (TryCastSelf(ai, m, s, "bloodlust")) break;
                    if (TryCastSelf(ai, m, s, "heroism")) break;
                    if (TryCastSelf(ai, m, s, "shamanistic rage")) break;
                    if (CastFirst(ai, m, s, { "stormstrike" })) break;
                    // 震击共用冷却：烈焰震击常驻，地球震击补位
                    if (AuraStacks(ai, s.bot, m, "flame shock", s.target) == 0 && CastFirst(ai, m, s, { "flame shock" })) break;
                    if (CastFirst(ai, m, s, { "earth shock" })) break;
                    break;   // 其余等普攻/风怒
                }
                if (CastFirst(ai, m, s, { "elemental mastery" })) break;
                if (AuraStacks(ai, s.bot, m, "flame shock", s.target) == 0 && CastFirst(ai, m, s, { "flame shock" })) break;
                if (CastFirst(ai, m, s, { "chain lightning" })) break;
                CastFirst(ai, m, s, { "lightning bolt" });
                break;
            }
        }

        LogTransition(ai, m, s, st);
        return true;
    }

    // ============================ 德鲁�?============================
    bool TickDruid(PlayerbotAI* ai, BotMem* m)
    {
        Snapshot s;
        BuildSnapshot(ai, m, s);
        if (!s.target) { LogTransition(ai, m, s, State::Idle); return false; }   // 交回引擎重选目标
        if (s.casting) return true;

        bool const feral = (m->spec == Spec::Feral);
        if (feral)
        {
            // 豹形态才按猫的循环。原引擎在假人场景不会自己变形（实测整场在外形平砍），
            // 所以这里先用一次变形动作，变形不成就交回引擎。
            uint8 const form = s.bot->GetShapeshiftForm();
            if (form != FORM_CAT)
            {
                // 已经挂着变形光环却仍认为"不在猫形态"时别再刷（实测 40 秒刷了 140 次 Cat Form）
                if (AuraStacks(ai, s.bot, m, "cat form", s.bot) > 0)
                    return false;
                if (TryCastSelf(ai, m, s, "cat form"))
                    return true;
                return false;
            }

            GetBehind(ai, m, s);   // 撕碎要背身（只走位，不占 GCD）
            bool const inRange = s.dist <= 6.5f;   // 近战够得着就行（5.0 太紧，卡在 5.2 会永远不进循环）
            bool const executeWindow = s.targetHp <= BurstHp(m);
            State st = (m->state == State::Idle) ? State::Burn : m->state;
            if (!inRange) st = State::Approach;
            else if (st == State::Approach) st = State::Burn;
            if (st == State::Burn && executeWindow) st = State::Burst;
            else if (st == State::Burst && !executeWindow) st = State::Burn;

            switch (st)
            {
                case State::Approach:
                    break;
                case State::Burst:
                    if (UseTrinket(ai, m, s)) break;
                    if (s.energy >= 80 && CastFirst(ai, m, s, { "tiger's fury" })) break;
                    if (s.combo >= 5 && AuraStacks(ai, s.bot, m, "rip", s.target) == 0
                        && CastFirst(ai, m, s, { "rip" })) break;
                    if (s.combo >= 5 && CastFirst(ai, m, s, { "ferocious bite" })) break;
                    if (s.combo >= 5) break;
                    if (CastFirst(ai, m, s, { "mangle (cat)" })) break;
                    CastFirst(ai, m, s, { "shred" });
                    break;
                case State::Burn:
                default:
                {
                    st = State::Burn;
                    if (UseTrinket(ai, m, s)) break;
                    // [2026-10-30 修] 凶猛之怒(Tiger's Fury) 没有冷却、只花 30 能量，原来排在第一位，
                    //   自增益"打自己"修好以后它把能量全吃光（实测 40 秒 15 次 TF、裂伤只剩 2 次、dps 359）。
                    //   改成"能量快满才放"（等效于把快溢出的能量换成攻强），不跟裂伤/终结技抢能量。
                    if (s.energy >= 80 && CastFirst(ai, m, s, { "tiger's fury" })) break;
                    // [2026-10-30 修] 终结技（割裂/凶猛撕咬）原来排在"攒星技"后面：
                    //   mangle(cat) 没有冷却、能量一够就先被它吃掉，星满了也在继续打裂伤
                    //   ⇒ 实测 40 秒 rip 只有 1 次、凶猛撕咬（Ferocious Bite）**表里根本没有**。
                    //   现在：5 星先泄终结技；割裂没挂先挂割裂，挂上了就改用凶猛撕咬。
                    bool const ripUp = AuraStacks(ai, s.bot, m, "rip", s.target) > 0;
                    if (s.combo >= 5 && !ripUp && CastFirst(ai, m, s, { "rip" })) break;
                    if (s.combo >= 5 && ripUp && CastFirst(ai, m, s, { "ferocious bite" })) break;
                    if (s.combo >= 5) break;      // 星满了在等能量放终结技，别再攒星
                    // 裂伤(�? 的流血易伤先挂，再补扫击/撕碎
                    if (CastFirst(ai, m, s, { "mangle (cat)" })) break;
                    if (AuraStacks(ai, s.bot, m, "rake", s.target) == 0 && CastFirst(ai, m, s, { "rake" })) break;
                    CastFirst(ai, m, s, { "shred" });
                    break;
                }
            }

            LogTransition(ai, m, s, st);
            return true;
        }

        // 平衡
        bool const inRange = s.dist <= 30.0f;
        State st = (m->state == State::Idle) ? State::Burn : m->state;
        if (s.power < 10.0f)
            st = State::Recover;
        else if (st == State::Recover && s.power > 30.0f)
            st = State::Burn;
        if (st != State::Recover && s.pack30 >= AoEPack(m))
            st = State::Aoe;
        else if (st == State::Aoe)
            st = State::Burn;
        if (st == State::Burn && s.targetHp <= BurstHp(m))
            st = State::Burst;
        else if (st == State::Burst && s.targetHp > 20.0f)
            st = State::Burn;
        if (st != State::Recover && st != State::Aoe && !inRange)
            st = State::Approach;
        else if (st == State::Approach && inRange)
            st = State::Burn;

        switch (st)
        {
            case State::Approach:
                break;
            case State::Recover:
                if (UseManaPotion(m, s)) break;
                CastFirst(ai, m, s, { "shoot" });
                break;
            case State::Aoe:
                if (TryCastAt(ai, m, s, "hurricane", s.target->GetPositionX(), s.target->GetPositionY(), s.target->GetPositionZ())) break;
                CastFirst(ai, m, s, { "starfire" });
                break;
            case State::Burst:
                if (UseTrinket(ai, m, s)) break;
                if (CastFirst(ai, m, s, { "force of nature" })) break;
                CastFirst(ai, m, s, { "starfire" });
                break;
            case State::Burn:
            default:
                st = State::Burn;
                if (UseTrinket(ai, m, s)) break;
                if (CastFirst(ai, m, s, { "force of nature" })) break;
                if (AuraStacks(ai, s.bot, m, "moonfire", s.target) == 0 && CastFirst(ai, m, s, { "moonfire" })) break;
                if (AuraStacks(ai, s.bot, m, "insect swarm", s.target) == 0 && CastFirst(ai, m, s, { "insect swarm" })) break;
                if (s.moving && CastFirst(ai, m, s, { "moonfire" })) break;
                CastFirst(ai, m, s, { "starfire" });
                break;
        }

        LogTransition(ai, m, s, st);
        return true;
    }

    // ============================ 圣骑�?============================
    bool TickPaladin(PlayerbotAI* ai, BotMem* m)
    {
        Snapshot s;
        BuildSnapshot(ai, m, s);
        if (!s.target) { LogTransition(ai, m, s, State::Idle); return false; }   // 交回引擎重选目标
        if (s.casting) return true;
        if (m->spec != Spec::Retribution) return false;

        GetBehind(ai, m, s);   // 绕背：只走位，不占这个 GCD   // 找背（不被招架/躲闪）
        // [2026-10-30 修] 近战范围要用**核心的判定**，不能用表面距离：
        //   s.dist <= 6.5 会把"中心距离 5.0 码、其实够不着"的情形也判成在范围内，
        //   结果整场不挥刀、Crusader Strike 一直被拒（实测 melee=0 / 35395:0）。
        bool const inRange = s.bot->CanReachWithMeleeAttack(s.target);
        if (!inRange)
            CloseToMelee(ai, m, s);   // 真够不着就自己贴上去（引擎 reach melee 的 range=0 是零长度路径）
        bool const executeWindow = s.targetHp <= BurstHp(m);
        // [2026-10-29 实测] 很多随机圣骑士没点"命令圣印"（它是天赋），直接按名字取会取不到 ⇒
        //   一路空判、40 秒 0 伤害。这里给一串候选，取第一个"这个 bot 真会"的。
        char const* seals[3] = {
            (s.bot->GetTeam() == HORDE) ? "seal of blood" : "seal of command",
            (s.bot->GetTeam() == HORDE) ? "seal of command" : "seal of blood",
            "seal of righteousness"     // 所有圣骑士都学得到，兜底
        };
        char const* seal = "seal of righteousness";
        for (char const* cand : seals)
        {
            if (SpellId(ai, s.bot, m, cand, false))
            {
                seal = cand;
                break;
            }
        }
        // 圣印：**只续当前这一个**（剩余 < 2s 才续）。
        //   ⚠️ 别把候选圣印做成同一张表：表是"第一条能放就放"，主圣印一次施法失败就会串到下一个，
        //   几个圣印互相顶掉、还白吃 GCD（实测 40 秒：Command 12 次 + Righteousness 10 次 + Blood 1 次，
        //   而十字军打击 0 次、审判 1 次）。
        if (TryTable(ai, m, s, { { seal, s.bot, nullptr, 2000 } }))
            return true;

        State st = (m->state == State::Idle) ? State::Burn : m->state;
        if (!inRange)
            st = State::Approach;
        else if (st == State::Approach)
            st = State::Burn;
        if (st == State::Burn && executeWindow)
            st = State::Burst;
        else if (st == State::Burst && !executeWindow)
            st = State::Burn;

        switch (st)
        {
            case State::Approach:
                break;
            case State::Burst:
                if (UseTrinket(ai, m, s)) break;
                // [2026-10-30 修] 复仇之怒/神圣之怒都是**自身增益**（spell_template.Targets = 0，
                //   没有 DEST/SOURCE_LOCATION 标志），CastFirst 不传 target ⇒ 拿敌人当目标 ⇒
                //   PlayerbotAI::CastSpell 里 CanCastSpell 判成"对敌放增益"失败，整场 0 次。
                if (TryCastSelf(ai, m, s, "avenging wrath")) break;
                if (CastFirst(ai, m, s, { "hammer of wrath" })) break;
                if (CastFirst(ai, m, s, { "judgement" })) break;
                CastFirst(ai, m, s, { "crusader strike" });
                break;
            case State::Burn:
            default:
            {
                st = State::Burn;
                if (UseTrinket(ai, m, s)) break;
                if (TryCastSelf(ai, m, s, "avenging wrath")) break;
                // [2026-10-29 实测] 审判的伤害全靠圣印；圣印没挂上就判 = 白扔 CD
                //   （假人场景实测 40 秒 210 次审判、只有 105 伤害）。这里改成"没圣印只挂圣印"。
                // [2026-10-29 诊断结论] 圣印的光环 id 就是施法 id，而且**必须打在自己身上**
                //   （原来的 CastFirst 没传 target ⇒ 对敌人放 ⇒ 判成负面法术失败 ⇒ 每 tick 重试，
                //     再叠上"失败就 break"，整个 Burn 分支等于空转）。
                uint32 const sealId = SpellId(ai, s.bot, m, seal, false);
                if (sealId && !s.bot->HasAura(sealId))
                {
                    if (TryCastSelf(ai, m, s, seal))
                    {
                        m->sealAt = time(nullptr);
                        break;
                    }
                    // 挂不上（没学/没蓝）也不 break：接着打审判/十字军，别整段空转
                }
                if (CastFirst(ai, m, s, { "judgement" })) break;
                if (CastFirst(ai, m, s, { "crusader strike" })) break;
                // [2026-10-30 修] 奉献（27173）：自身中心的 AoE，两条常规入口都会失败（见 CastSelfCentered）。
                //   它是惩戒唯一的填充技（审判 10s / 十字军 6s 都有空档），不吃这一手 40 秒就白丢 5~6 次。
                //   蓝门限放到 15%：惩戒的审判/十字军在本库里 ManaCost=0，蓝只花在圣印(65)和奉献(660)上，
                //   40% 会白等 GCD；留 15% 足够续一次圣印就行。
                if (s.power > 15.0f && CastSelfCentered(ai, m, s, "consecration")) break;
                break;
            }
        }

        LogTransition(ai, m, s, st);
        return true;
    }

    // ============================ 牧师 ============================
    bool TickPriest(PlayerbotAI* ai, BotMem* m)
    {
        Snapshot s;
        BuildSnapshot(ai, m, s);
        if (!s.target) { LogTransition(ai, m, s, State::Idle); return false; }   // 交回引擎重选目标
        if (s.casting) return true;
        if (m->spec != Spec::Shadow) return false;

        bool const inRange = s.dist <= 30.0f;
        State st = (m->state == State::Idle) ? State::Burn : m->state;
        if (s.power < 10.0f)
            st = State::Recover;
        else if (st == State::Recover && s.power > 30.0f)
            st = State::Burn;
        if (st == State::Burn && s.targetHp <= BurstHp(m))
            st = State::Burst;
        else if (st == State::Burst && s.targetHp > 20.0f)
            st = State::Burn;
        if (st != State::Recover && !inRange)
            st = State::Approach;
        else if (st == State::Approach && inRange)
            st = State::Burn;

        switch (st)
        {
            case State::Approach:
                break;
            case State::Recover:
                CastFirst(ai, m, s, { "shoot" });
                break;
            case State::Burst:
                if (UseTrinket(ai, m, s)) break;
                if (CastFirst(ai, m, s, { "shadow word: death" })) break;
                CastFirst(ai, m, s, { "mind blast" });
                break;
            case State::Burn:
            default:
                st = State::Burn;
                if (UseTrinket(ai, m, s)) break;
                if (AuraStacks(ai, s.bot, m, "shadowform", s.bot) == 0 && TryCastSelf(ai, m, s, "shadowform")) break;
                if (AuraStacks(ai, s.bot, m, "shadow word: pain", s.target) == 0 && CastFirst(ai, m, s, { "shadow word: pain" })) break;
                if (AuraStacks(ai, s.bot, m, "vampiric touch", s.target) == 0 && CastFirst(ai, m, s, { "vampiric touch" })) break;
                if (CastFirst(ai, m, s, { "mind blast" })) break;
                if (CastFirst(ai, m, s, { "shadow word: death" })) break;
                CastFirst(ai, m, s, { "mind flay" });
                break;
        }

        LogTransition(ai, m, s, st);
        return true;
    }
}

namespace ai
{
    namespace dpsfsm
    {
        void SetParamOverride(uint32 guid, char const* key, double value)
        {
            std::lock_guard<std::mutex> lock(s_disabledMutex);
            s_overrides[guid][key] = value;
        }

        void ClearParamOverrides(uint32 guid)
        {
            std::lock_guard<std::mutex> lock(s_disabledMutex);
            s_overrides.erase(guid);
        }

        // [2026-10-31 站长口径] 让位判据 = "放了技能 **或** 有意 Hold"。
        //   消费方（PlayerbotAI 的引擎让位）只问"这一 tick 的施法输出归不归状态机"，
        //   Hold 的语义正是"归状态机、但这一手我什么都不放"，所以两者在这里合并。
        bool DidCastThisTick(uint32 guid)
        {
            std::lock_guard<std::mutex> lock(s_memMutex);
            auto it = s_mem.find(guid);
            return it != s_mem.end() && it->second && (it->second->castThisTick || it->second->holdThisTick);
        }

        bool SkipEngine(uint32 guid)
        {
            std::lock_guard<std::mutex> lock(s_disabledMutex);
            // [2026-11-01 站长口径] 治疗场景也算"场景接管"：治疗者没有假人可打，
            //   `s_scenarioTargets` 里不会有它 —— 但引擎同样必须让位（否则它自己也在加血，
            //   HPS 就分不清是谁放的了）。
            return s_scenarioTargets.find(guid) != s_scenarioTargets.end()
                || s_healTargets.find(guid) != s_healTargets.end();
        }

        // 这个专精是不是近战（决定 bot->Attack 的 meleeAttack 参数：远程职业传 true 会走去平砍）
        bool IsMeleeSpec(Spec spec)
        {
            switch (spec)
            {
                case Spec::Arms:
                case Spec::Fury:
                case Spec::Protection:
                case Spec::Combat:
                case Spec::Assassination:
                case Spec::Subtlety:
                case Spec::Feral:
                case Spec::Enhancement:
                case Spec::Retribution:
                    return true;
                default:
                    return false;
            }
        }

    // [2026-10-31 诊断] 记录"状态机为什么没干活"。定义在 EngageScenarioTarget 之前，
    //   这样各处早退点都能直接调用（上次放到文件末尾导致编译失败）。
    static void NoteBail(Player* bot, char const* why)
    {
        static std::map<uint32, std::pair<std::string, time_t>> s_lastBail;
        if (!bot)
            return;
        time_t const now = time(nullptr);
        uint32 const g = bot->GetGUIDLow();
        auto& e = s_lastBail[g];
        if (e.first == why && e.second && now - e.second < 5)
            return;
        e.first = why;
        e.second = now;
        sLog.outError("[FSM-BAIL] bot=%s why=%s", bot->GetName(), why);
    }

        bool EngageScenarioTarget(PlayerbotAI* ai)
        {
            Player* bot = ai ? ai->GetBot() : nullptr;
            if (!bot || !bot->IsInWorld() || !bot->IsAlive() || bot->GetVictim())
                return false;
            // 骑乘状态 Unit::Attack 直接 false（猎人整场 0 输出的根因）
            if (bot->IsMounted())
                bot->Unmount();

            std::set<ObjectGuid> scenario;
            {
                std::lock_guard<std::mutex> lock(s_disabledMutex);
                auto it = s_scenarioTargets.find(bot->GetGUIDLow());
                if (it == s_scenarioTargets.end())
                    return false;      // 没声明场景 ⇒ 副本里照旧，不自动开怪
                scenario = it->second;
            }

            Map* map = bot->GetMap();
            if (!map)
                return false;
            for (ObjectGuid const& g : scenario)
            {
                Unit* u = map->GetUnit(g);
                if (!u || !u->IsAlive())
                    continue;
                if (bot->GetDistance(u) > 45.0f)
                    continue;
                if (!sServerFacade.IsHostileTo(bot, u))
                    continue;
                // 召唤物常被标成 evade，而 Unit::Attack 对 evade 目标直接 false ⇒ 先清掉再咬
                if (u->GetCombatManager().IsInEvadeMode())
                {
                    u->GetCombatManager().SetEvadeState(EVADE_NONE);
                    u->GetCombatManager().StopEvade();
                }
                bool const melee = IsMeleeSpec((Mem(bot->GetGUIDLow()))->spec);
                bool ok = bot->Attack(u, melee);   // 远程职业必须传 false，否则会走进近战位平砍
                bool const firstTry = ok;
                if (!ok || !bot->GetVictim())
                {
                    // Unit::Attack() 在本核心里有个不对称：bot 还挂在目标的 m_attackers 里、
                    //   但自己的 m_attacking 已经空了 ⇒ _addAttacker 返回 false，Attack 直接失败，
                    //   而且之后每次都失败（实测 shadow 整场 attack=0 / dist=2.0 / mounted=0）。
                    //   先 CombatStop 把这个状态清干净再咬一次（此时本来就不在战斗，停了没副作用）。
                    bot->CombatStopWithPets(true);
                    ok = bot->Attack(u, melee);
                }
                if (!ok || !bot->GetVictim())
                {
                    // 诊断：Attack 失败本身是核心里的 m_attackers 不对称（见 SimScenario 的兜底），
                    //   这里至少让日志能看出来"是 Attack 返回 false，而不是找不到目标"。
                    BotMem* mem = Mem(bot->GetGUIDLow());
                    time_t const now = time(nullptr);
                    if (now - mem->lastEngageLog >= 10)
                    {
                        mem->lastEngageLog = now;
                        sLog.outError("[DPSFSM-ENGAGE] %s 咬不上去: attack=%d(retry=%d) victim=%s mounted=%d "
                            "dist=%.1f alive=%d inworld=%d evade=%d dead=%d flying=%d",
                            bot->GetName(), firstTry ? 1 : 0, ok ? 1 : 0,
                            bot->GetVictim() ? bot->GetVictim()->GetName() : "-",
                            bot->IsMounted() ? 1 : 0, bot->GetDistance(u),
                            u->IsAlive() ? 1 : 0, u->IsInWorld() ? 1 : 0,
                            u->GetCombatManager().IsInEvadeMode() ? 1 : 0,
                            bot->IsAlive() ? 0 : 1, bot->IsFlying() ? 1 : 0);
                    }
                    continue;
                }
                return true;
            }
            return false;
        }

        // [2026-11-01 站长口径] 轮间重置：见 DpsFsm.h → ResetScenarioState() 的说明。
        //   `BotMem` 是 per-guid 常驻的（`s_mem`），所以 planId/planStep/planTarget、
        //   lastBurst/burstRounds/burstUntil、manaSpent/manaCounts/lastManaSpent、
        //   lastArcLog/lastThreatLog、state/holdThisTick 全部会跨轮残留 —— 上一轮停在哪个相位，
        //   下一轮就从哪个相位开局，行为完全不同（实测 arcane:mode=5 dps 波动接近 50%）。
        //   **保留**：failUntil（技能退避，核心侧拒绝的短期记忆）、宠物字段（pet*）、
        //   ids/unresolved/flags（技能表缓存）、lastOk/prereqLast（buff 节奏），
        //   这些要么是"学会没学会"的静态缓存、要么是跨轮不该被清掉的短期状态。
        void ResetScenarioState(uint32 guid)
        {
            // runId 只用于日志：SetScenarioTargets 先把新 runId 写进 s_resetRun 再调这里
            uint32 runId = 0;
            {
                std::lock_guard<std::mutex> lock(s_disabledMutex);
                auto it = s_resetRun.find(guid);
                if (it != s_resetRun.end())
                    runId = it->second;
            }

            BotMem* m = Mem(guid);

            m->planId = 0;
            m->planStep = 0;
            m->planTarget = ObjectGuid();
            m->burstRounds = 0;
            m->burstUntil = 0;
            m->lastBurst = 0;
            m->manaSpent = 0;
            m->manaCounts.clear();
            m->lastManaSpent = 0;
            m->lastArcLog = 0;
            m->lastFillLog = 0;
            m->lastThreatLog = 0;
            m->state = State::Idle;
            m->holdThisTick = false;
            // 诊断窗口计数一并归零：这样每一轮的 [DPSFSM-ARC] 只报**本轮**的 holds/casts
            //   （只影响日志，不影响任何决策；上一轮的窗口计数留着反而会把验收数字算串）。
            m->arcHolds = 0;
            m->arcCasts = 0;
            m->arcBranch = '?';
            // [2026-11-01 站长口径] 术士·毁灭：交战计时与碎裂"每场一次"必须跨轮清零，
            //   否则上一轮的 engagedAt 会让 shatter_min_engage_ms 在开局就立刻满足（等于开局就交碎裂）。
            m->engagedAt = 0;
            m->shatterUsed = false;
            m->lastWarlockLog = 0;
            m->dotTarget = ObjectGuid();
            m->dotPrevAt = 0;
            m->dotLastLifeSec = -1;
            // [2026-11-01 站长口径] 痛苦/恶魔：多目标铺场计数与"宠物在场计时"必须跨轮清零 ——
            //   上一轮的 wlMulti 会让新一轮的日志继续报旧的 multi 值，demoPetAt 更是直接骗过
            //   "不是刚召唤的"那一道门（重开一轮 = 干净开局，宠物还没召）。
            m->wlDots = 0;
            m->wlMulti = 1;
            m->demoPetAt = 0;
            // [2026-11-01 站长口径] 法师·火法：火球计数 / 灼烧记账 / 爆发轮次必须跨轮清零 ——
            //   否则上一轮已经开过起手轮（fireBurstDone & 1），下一轮的起手爆发直接哑掉；
            //   火球计数也会带着上一轮的尾巴，开局就补灼烧。
            //   **保留** `fireScorchSeen`（"有没有强化灼烧天赋"是静态事实，与 ids/flags 同类）。
            m->fireFbSinceScorch = 0;
            m->fireScorchTries = 0;
            m->fireScorchPending = false;
            m->fireScorchBefore = 0;
            m->fireScorchForce = false;
            m->fireScorchResent = false;
            m->fireFlameToggle = 0;
            m->fireBurstDone = 0;
            m->fireBurstRound = 0;
            m->lastFireLog = 0;
            // [2026-11-01 站长口径] 惩戒骑：平砍计数/舞次数/交战计时/爆发窗口/审判补印都必须跨轮清零 ——
            //   否则上一轮的 retSwings 会让新一轮开局就在窗口里"到点必舞"，
            //   retEngagedAt 也会让 fight_length_est 的"稍等"判据直接当成已经等够。
            m->lastRetLog = 0;
            m->retSwings = 0;
            m->retTwistCount = 0;
            m->retPrevSwingMs = -1;
            m->retEngagedAt = 0;
            m->retBurstUntil = 0;
            m->retNeedSeal = false;
            m->retRestoreSeal = false;
            m->retTarget = ObjectGuid();
            // [2026-11-01 站长口径] 萨满：图腾账本必须跨轮清零 —— 上一轮的 totemEntry/totemExpireAt
            //   会让新一轮开局"以为图腾还在"（假人重开后 bot 往往已被挪回原点，旧账本直接骗自己）。
            //   清掉账本 = 新一轮的第一件事就是重铺图腾（这正是干净开局该有的样子）。
            for (int i = 0; i < MAX_TOTEM_SLOT; ++i)
            {
                m->totemCastAt[i] = 0;
                m->totemEntry[i] = 0;
                m->totemExpireAt[i] = 0;
                for (int k = 0; k < 3; ++k)
                    m->totemPos[i][k] = 0.0f;
            }
            m->twistPhase = 0;
            m->twistNextAt = 0;
            m->lastShLog = 0;
            m->lastShDiag = 0;
            m->shClOk = false;
            m->shLightningShield = 0;
            m->lsCastAt = 0;
            m->shEarthShocks = 0;
            m->shTotemCasts = 0;
            m->shAoeNova = false;
            m->shTarget = ObjectGuid();
            m->shEngagedAt = 0;
            m->wfIcdMs = -1;
            m->wfLastProcAt = 0;
            m->wfProcs = 0;
            m->wfReadyPrev = true;
            // [2026-11-01 站长口径] 牧师·暗影：**时间轮的拍钟必须跨轮清零** ——
            //   否则上一轮的 spBeatAt 停在几十秒前，新一轮第一次进 ShadowAdvanceBeat 会看到
            //   "已经过了很多拍"（虽然那里做了不补拍的对齐，但拍号/撞车间隔仍然带着旧相位）；
            //   spMbReadyAt 更直接：会让新一轮开局就"心爆已就绪"，前视窗口整体错位。
            m->lastPriestLog = 0;
            m->spBeat = 0;
            m->spBeatAt = 0;
            m->spMbReadyAt = 0;
            m->spWheelBeat = -1;
            m->spDots = 0;
            m->spBeatAct = "-";
            // [2026-11-01 站长口径] 通用层 `ids` 体检的节流也跨轮清零：新一轮 respec 之后
            //   第一条询问就该立刻体检，别被上一轮的 5 秒窗口拖住。
            m->idsCheckedAt = 0;
            m->idsCursor = 0;
            m->idsNegAt = 0;
            m->holdIntent = nullptr;
            m->spDiagAt = 0;
            // [2026-11-01 站长口径] 德鲁伊：**能量跳相位必须跨轮清零** ——
            //   上一轮的 drTickAt 停在哪里都是旧的相位，新一轮如果接着算，第一手就会按错误的
            //   "距下一跳"去等/去变身（实测会让开局 1~2 秒空转）。能量读数也要重置，否则
            //   第一 tick 的增量会被当成一次假跳。`drFuror` 是静态天赋事实，保留（与 ids/flags 同类）。
            m->drTickAt = 0;
            m->drTickPeriod = 2000;
            m->drEnergyPrev = -1;
            m->drShifts = 0;
            m->drLastLog = 0;
            m->drShiftAt = 0;
            m->drTarget = ObjectGuid();
            m->drEngagedAt = 0;
            // [2026-11-01 站长口径] 治疗侧：速率窗口/待结算治疗/统计也必须跨轮清零 ——
            //   上一轮的 healWatch 会把"还在流血"的旧斜率带进来（预读会凭空提前），
            //   统计不清则 [DPSFSM-HPS] 的第一条窗口把上一轮的数字算进这一轮。
            m->healWatch.clear();
            m->healPending = HealPending();
            m->healCasts = m->healEff = m->healTheory = m->healOver = 0;
            m->hpsWinStart = 0;
            m->hpsWinEff = m->hpsWinTheory = m->hpsWinOver = m->hpsWinCasts = 0;
            m->lastHealLog = 0;
            m->lastHpsLog = 0;
            m->healTargetGuid = 0;
            std::string name;
            if (!sObjectMgr.GetPlayerNameByGUID(ObjectGuid(HIGHGUID_PLAYER, guid), name) || name.empty())
                name = "guid=" + std::to_string(guid);
            sLog.outError("[DPSFSM-RESET] %s runId=%u", name.c_str(), runId);
        }

        void SetScenarioTargets(uint32 guid, std::vector<ObjectGuid> const& targets, uint32 runId)
        {
            // [2026-11-01 站长口径] runId 变化 = 新一轮 ⇒ 清掉上一轮残留的 FSM 记忆。
            //   同一 runId 的重复登记（SpawnDummies / TickDummies 都会调）**不重置**：
            //   tick 内被反复清空的话 planStep/burstUntil 永远长不出来。
            bool newRun = false;
            {
                std::lock_guard<std::mutex> lock(s_disabledMutex);
                auto it = s_resetRun.find(guid);
                newRun = (it == s_resetRun.end() || it->second != runId);
                if (newRun)
                    s_resetRun[guid] = runId;

                if (targets.empty())
                {
                    s_scenarioTargets.erase(guid);
                    s_scenarioRun.erase(guid);
                }
                else
                {
                    s_scenarioTargets[guid] = std::set<ObjectGuid>(targets.begin(), targets.end());
                    s_scenarioRun[guid] = runId;
                }
            }
            if (newRun)
                ResetScenarioState(guid);
        }

        void ClearScenarioTargets(uint32 guid, uint32 runId)
        {
            std::lock_guard<std::mutex> lock(s_disabledMutex);
            auto it = s_scenarioRun.find(guid);
            if (it == s_scenarioRun.end())
                return;
            if (runId && it->second != runId)
                return;      // 这条是新一轮登记的，别动
            s_scenarioTargets.erase(guid);
            s_scenarioRun.erase(guid);
        }

        // [2026-11-01 站长口径] 治疗场景的目标分配：见 DpsFsm.h → SetScenarioHealTargets()。
        //   runId 变化 = 新一轮 ⇒ 清掉上一轮残留的速率窗口/统计（同一局的重复登记不清）。
        void SetScenarioHealTargets(uint32 healerGuid, std::vector<ObjectGuid> const& healTargets,
                                    uint32 mainTankGuid, uint32 runId)
        {
            bool newRun = false;
            {
                std::lock_guard<std::mutex> lock(s_disabledMutex);
                auto it = s_healRun.find(healerGuid);
                newRun = (it == s_healRun.end() || it->second != runId);
                s_healRun[healerGuid] = runId;
                if (healTargets.empty())
                {
                    s_healTargets.erase(healerGuid);
                    s_healTank.erase(healerGuid);
                }
                else
                {
                    s_healTargets[healerGuid] = std::set<ObjectGuid>(healTargets.begin(), healTargets.end());
                    s_healTank[healerGuid] = mainTankGuid;
                }
            }
            if (newRun)
            {
                BotMem* m = Mem(healerGuid);
                m->healWatch.clear();
                m->healPending = HealPending();
                m->healCasts = m->healEff = m->healTheory = m->healOver = 0;
                m->hpsWinStart = 0;
                m->hpsWinEff = m->hpsWinTheory = m->hpsWinOver = m->hpsWinCasts = 0;
                m->lastHealLog = 0;
                m->lastHpsLog = 0;
            }
        }

        void ClearScenarioHealTargets(uint32 healerGuid, uint32 runId)
        {
            {
                std::lock_guard<std::mutex> lock(s_disabledMutex);
                auto it = s_healRun.find(healerGuid);
                if (it == s_healRun.end())
                    return;
                if (runId && it->second != runId)
                    return;
                s_healTargets.erase(healerGuid);
                s_healTank.erase(healerGuid);
                s_healRun.erase(healerGuid);
            }
            // [2026-11-01 站长口径] 收尾汇总：把**本轮 HealCore 自己的账**打一条，验收/报告直接取它。
            //   为什么不只用核心的 `botHealingDone`：HoT（恢复）的跳治疗、以及过疗，
            //   两套口径不完全一致；这里给的是"本状态机算出来的理论量/估算有效量/溢出"。
            //   放在锁外：`Mem()` 自己拿 s_memMutex，别把两把锁叠在一起（避免将来加出死锁）。
            BotMem* m = Mem(healerGuid);
            if (!m)
                return;
            std::string name;
            if (!sObjectMgr.GetPlayerNameByGUID(ObjectGuid(HIGHGUID_PLAYER, healerGuid), name) || name.empty())
                name = "guid=" + std::to_string(healerGuid);
            double const overPct = m->healTheory
                ? 100.0 * double(m->healOver) / double(m->healTheory) : 0.0;
            sLog.outError("[DPSFSM-HPS-SUM] %s casts=%u heal=%u eff=%u over=%u overheal=%.0f%% runId=%u",
                name.c_str(), m->healCasts, m->healTheory, m->healEff, m->healOver, overPct, runId);
        }

        int PackCount(Player* bot, float range)
        {
            if (!bot || !bot->GetPlayerbotAI())
                return 0;
            return CountHostilesNear(bot->GetPlayerbotAI(), bot, range);
        }

        void SetBotForceDps(uint32 guid, bool on)
        {
            std::lock_guard<std::mutex> lock(s_disabledMutex);
            if (on)
                s_forceDps.insert(guid);
            else
                s_forceDps.erase(guid);
        }

        void SetBotEnabled(uint32 guid, bool on)
        {
            std::lock_guard<std::mutex> lock(s_disabledMutex);
            if (on)
                s_disabled.erase(guid);
            else
                s_disabled.insert(guid);
        }

        // [2026-10-29 实测] 只按策略名判专精会错：随机 bot 身上同时挂着好几套策略
        //   （druid 判成 restoration、fury 战士其实没学嗜血），于是状态机拿错技能表。
        //   改成**先按天赋技能判**（学会了才有的技能名），判不出来再用策略名兜底。
        Spec DetectSpecByTalents(PlayerbotAI* ai, BotMem* m);

        Spec DetectSpecByTalents(PlayerbotAI* ai, BotMem* m)
        {
            return DetectSpecByTalentsImpl(ai, m);
        }

        Spec DetectSpec(PlayerbotAI* ai)
        {
            Player* bot = ai ? ai->GetBot() : nullptr;
            if (!bot)
                return Spec::Unknown;

            struct Entry { char const* strategy; Spec spec; };
            static Entry const mage[] = { { "fire", Spec::Fire }, { "arcane", Spec::Arcane }, { "frost", Spec::Frost } };
            static Entry const warrior[] = { { "fury", Spec::Fury }, { "arms", Spec::Arms }, { "protection", Spec::Protection } };
            static Entry const rogue[] = { { "assassination", Spec::Assassination }, { "subtlety", Spec::Subtlety }, { "combat", Spec::Combat } };
            static Entry const hunter[] = { { "beast mastery", Spec::BeastMastery }, { "marksmanship", Spec::Marksmanship }, { "survival", Spec::Survival } };
            static Entry const warlock[] = { { "affliction", Spec::Affliction }, { "demonology", Spec::Demonology }, { "destruction", Spec::Destruction } };
            static Entry const shaman[] = { { "enhancement", Spec::Enhancement }, { "elemental", Spec::Elemental }, { "restoration", Spec::Restoration } };
            static Entry const druid[] = { { "feral", Spec::Feral }, { "balance", Spec::Balance }, { "restoration", Spec::Restoration } };
            static Entry const paladin[] = { { "retribution", Spec::Retribution }, { "protection", Spec::Protection }, { "holy", Spec::Holy } };
            static Entry const priest[] = { { "shadow", Spec::Shadow }, { "discipline", Spec::Discipline }, { "holy", Spec::Holy } };

            Entry const* table = nullptr;
            size_t count = 0;
            switch (bot->getClass())
            {
                case CLASS_MAGE:    table = mage;    count = 3; break;
                case CLASS_WARRIOR: table = warrior; count = 3; break;
                case CLASS_ROGUE:   table = rogue;   count = 3; break;
                case CLASS_HUNTER:  table = hunter;  count = 3; break;
                case CLASS_WARLOCK: table = warlock; count = 3; break;
                case CLASS_SHAMAN:  table = shaman;  count = 3; break;
                case CLASS_DRUID:   table = druid;   count = 3; break;
                case CLASS_PALADIN: table = paladin; count = 3; break;
                case CLASS_PRIEST:  table = priest;  count = 3; break;
                default: return Spec::Unknown;
            }

            // [2026-10-29 实测] 优先级：①只开了一个专精策略 ⇒ 就是它（假人场景会强制成唯一一个，
            //   否则 "holy 天赋的圣骑士" 会被天赋判定带偏，整场不放技能）；
            //   ②天赋标志技能；③开着的第一个专精策略；④职业默认单体循环。
            int enabled = 0;
            Spec onlyEnabled = Spec::Unknown;
            for (size_t i = 0; i < count; ++i)
            {
                if (ai->HasStrategy(table[i].strategy, BotState::BOT_STATE_COMBAT)
                    || ai->HasStrategy(table[i].strategy, BotState::BOT_STATE_NON_COMBAT))
                {
                    ++enabled;
                    onlyEnabled = table[i].spec;
                }
            }
            if (enabled == 1)
                return onlyEnabled;

            Spec const byTalent = DetectSpecByTalents(ai, Mem(bot->GetGUIDLow()));
            if (byTalent != Spec::Unknown)
                return byTalent;

            if (enabled > 1)
                return onlyEnabled;
            // [2026-10-29 实测] 随机 bot 天赋随机，很多人点不出"标志技能"（术士没痛苦无常、
            //   牧师没暗影形态）。这时别什么都不做：按职业给一套只用基础技能的通用单体循环。
            switch (bot->getClass())
            {
                case CLASS_MAGE:    return Spec::Fire;
                case CLASS_WARRIOR: return Spec::Arms;
                case CLASS_ROGUE:   return Spec::Combat;
                case CLASS_HUNTER:  return Spec::BeastMastery;
                case CLASS_WARLOCK: return Spec::Affliction;
                case CLASS_SHAMAN:  return Spec::Enhancement;
                case CLASS_DRUID:   return Spec::Feral;
                case CLASS_PALADIN: return Spec::Retribution;
                case CLASS_PRIEST:  return Spec::Shadow;
                default: break;
            }
            return Spec::Unknown;
        }

        // [2026-10-30 站长口径] 职业=技能空间、专精=策略；实现见 DpsPolicy.inc（纯文本包含，便于与旧处理器 A/B）
    // [2026-10-30] 顺序很重要：DpsValue 必须在 DpsPolicy 之前包含（表内打分调用 dpsvalue::ValueOfId）
#include "playerbot/strategy/DpsValue.inc"
#include "playerbot/strategy/DpsPolicy.inc"




    // [2026-10-30 站长口径] **所有物理近战输出都要找背**（找背是站位问题，跟具体技能无关）；
    //   但"找背"不等于一直调位置：站到背后之后就尽量不动（走动会打断挥击，实测平砍 78→54）。
    bool NeedsBehind(Player* bot)
    {
        if (!bot)
            return false;
        switch (bot->getClass())
        {
            case CLASS_WARRIOR:
            case CLASS_ROGUE:
            case CLASS_PALADIN:
            case CLASS_SHAMAN:
            case CLASS_DRUID:
                return true;
            default:
                return false;
        }
    }

    // [2026-10-30 诊断] 引擎这一 tick 真的跑了 ⇒ 计数（对比 policy=0/1 时引擎被让位的比例）
    void NoteEngineRun(uint32 guid)
    {
        std::lock_guard<std::mutex> lock(s_memMutex);
        auto it = s_mem.find(guid);
        if (it != s_mem.end())
            ++it->second->engRuns;
    }

    // ==========================================================================
    // [2026-10-31 S2] 冰法状态图（试点，mode=5 才用；其它专精仍走原路径）
    //   图见 dev/DESIGN_dps_fsm_graphs.md §2.1
    //   规矩：① 状态只决定"用哪张动作表"，不直接施法；
    //         ② 转移条件只读 Snapshot + TP 参数，不读世界、不打日志；
    //         ③ 没放出去就 return false，把走位/追击交回引擎（共同作用，不是抢活）。
    // ==========================================================================
    // ==========================================================================
    // [2026-10-31 站长口径] 仇恨从"二元门"改成**三档**。
    //   旧 ThreatLead() 只有"等/不等"两态，卡在中间态时只能二选一：要么白等一整轮（DPS 掉），
    //   要么一上来就开爆发把仇恨抢过来（怪转头咬法师）。三档把中间态单列出来：
    //     Open      —— 全力打：开爆发、冰锥这类高威胁牌都能用；
    //     Throttle  —— 照常打（寒冰箭/魔爆/暴风雪这类低威胁填充），但**不开爆发、不用高威胁牌**；
    //     Wait      —— 等坦克把仇恨拉开：Maintain 里用 Hold（不施法，引擎也不许动）。
    //   阈值是**坦克威胁的倍数**，全部走 TP()：threat_open 默认 0.9、threat_floor 默认 0.7
    //   （0.9 意味着"我快追平坦克了"才放开全力，0.7 以上算中间态）。
    //   ⚠️ 没有坦克（假人）或状态机独占（SkipEngine）⇒ **恒 Open**：之前 WaitingForTankThreat
    //      在假人里恒真，和引擎互相推诿把 bot 卡死（实测 1/3 轮次有目标却 0 施法 0 伤害）。
    // ==========================================================================
    enum class ThreatLevel { Wait, Throttle, Open };

    char const* ThreatLevelName(ThreatLevel l)
    {
        switch (l)
        {
            case ThreatLevel::Wait:     return "wait";
            case ThreatLevel::Throttle: return "throttle";
            case ThreatLevel::Open:     return "open";
        }
        return "?";
    }

    ThreatLevel EvalThreat(BotMem* m, Snapshot& s)
    {
        ThreatLevel level;

        // [2026-10-31 修正] 方向搞反过一次：**威胁越高越该收手**，不是越低。
        //   旧写法是 myThreat < 0.7*tank ⇒ Wait（越安全越不打），组队里会永久空转 —— 已改。
        //   新语义：danger（≥95% 坦克威胁）⇒ 等坦克拉开；throttle（≥80%）⇒ 收高威胁牌；否则放开打。
        //   进入 Wait 后因为自己不再产生威胁、坦克继续涨，比值必然回落 ⇒ 会自己退出，不会死锁。
        bool const inDungeon = s.bot->GetMap()
            && (s.bot->GetMap()->IsDungeon() || s.bot->GetMap()->IsRaid());

        if (!s.tankAlive || !inDungeon || dpsfsm::SkipEngine(s.bot->GetGUIDLow()))
        {
            // 假人 / 单人 / 野外：保持旧的"恒放开"行为，绝不因为威胁把 DPS 卡死
            level = ThreatLevel::Open;
        }
        else if (s.tankThreat <= 0.0f)
        {
            level = ThreatLevel::Open;          // 坦克还没建立仇恨，无法判断 ⇒ 先放开
        }
        else if (s.myThreat >= s.tankThreat * float(TP(m, "threat_danger", 0.95)))
        {
            level = ThreatLevel::Wait;          // 快 OT：停手等坦克拉开（会自己退出）
        }
        else if (s.myThreat >= s.tankThreat * float(TP(m, "threat_throttle", 0.80)))
        {
            level = ThreatLevel::Throttle;      // 中间态：打，但把高威胁牌收起来
        }
        else
        {
            level = ThreatLevel::Open;          // 离坦克还远：放开打
        }

        // [2026-10-31 站长口径] 诊断：每 10 秒一条（WorldTimer 节流，别每 tick 刷屏）。
        //   字段固定为 my / tank / tankAlive / level，方便脚本直接切。
        uint32 const nowMs = WorldTimer::getMSTime();
        uint32 const nowSafe = nowMs ? nowMs : 1;
        if (!m->lastThreatLog || WorldTimer::getMSTimeDiff(m->lastThreatLog, nowSafe) >= 10000)
        {
            m->lastThreatLog = nowSafe;
            sLog.outError("[DPSFSM-THREAT] %s my=%.0f tank=%.0f tankAlive=%d level=%s",
                s.bot ? s.bot->GetName() : "-", s.myThreat, s.tankThreat,
                s.tankAlive ? 1 : 0, ThreatLevelName(level));
        }
        return level;
    }

    // [2026-10-31 站长口径] 冰法 Aoe 里**暴风雪已降级为可选**：只有
    //   ① 怪不在近身 aoe_melee_range 内（远程场景），或
    //   ② 被目标追着咬、且它身上还没有减速（chilled）—— 才值得用暴风雪（1 级，减速效果一样、蓝耗低）。
    //   `aoe_blizzard=0` 可整体关掉。
    bool AoeWantBlizzard(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        if (TP(m, "aoe_blizzard", 1.0) < 1.0)
            return false;
        if (s.dist > float(TP(m, "aoe_melee_range", 10.0)))
            return true;
        if (!s.target || s.target->GetVictim() != s.bot)
            return false;
        if (s.dist > float(TP(m, "aoe_slow_range", 15.0)))
            return false;
        return AuraStacks(ai, s.bot, m, "chilled", s.target) == 0;
    }

    State EvalFrost(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        if (!s.target)                                     return State::Idle;

        // [2026-10-31 站长口径] 先保命：血太低先撤（风筝），不做"站桩换血"
        if (s.selfHp < float(TP(m, "flee_hp", 30.0)))
            return State::Flee;

        if (s.power < float(TP(m, "recover_enter", 8.0)))  return State::Recover;
        if (s.pack30 >= AoEPack(m))                        return State::Aoe;

        // [2026-10-31 站长口径] 爆发 = 仇恨稳定后**立刻开**、早用早 CD（不再等目标血量低）。
        //   单 bot 用时间间隔当"CD 门"，`burst_interval` 可调（越小越激进）。
        //   假人场景没有仇恨（SkipEngine 为真），视为仇恨稳定。
        //   爆发窗口内（burst_window 秒）一直留在 Burst：一个 GCD 只放一个技能，
        //   "冰冷血脉 → 饰品 → 水元素 → 急冷"必须跨几个 tick 才放得完（否则只放得出第一手）。
        time_t const now = time(nullptr);
        if (now < m->burstUntil)
            return State::Burst;
        // [2026-10-31 站长口径] 爆发门换成三档里的 Open（原先是二元 ThreatLead）：
        //   只有"可以全力打"才开爆发；Throttle/Wait 一律不开（爆发牌本身全是高威胁）。
        if (EvalThreat(m, s) == ThreatLevel::Open
            && now - m->lastBurst >= time_t(TP(m, "burst_interval", 120.0)))
            return State::Burst;

        if (s.dist > float(TP(m, "range_caster", 30.0)))   return State::Approach;
        return State::Burn;
    }

    // [2026-10-31 站长口径] bool → 三态的小换算：动作放出去了=Acted，没放成=Decline（交回引擎）。
    //   Hold 只在真正"有意空着"的地方单独写，绝不靠这个换算顺手产生。
    inline ai::dpsfsm::TickResult FsmResult(bool acted)
    {
        return acted ? ai::dpsfsm::TickResult::Acted : ai::dpsfsm::TickResult::Decline;
    }

    ai::dpsfsm::TickResult TickFrostFsm(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        State const st = EvalFrost(ai, m, s);
        // [2026-10-31 站长口径] 仇恨档位本 tick 只算一次，Aoe/Burst/Maintain 共用同一份判断
        //   （诊断日志在 EvalThreat 内部按 10 秒节流）。
        ThreatLevel const threat = EvalThreat(m, s);
        // [2026-10-31 站长口径] 急冷只在"第二轮及以后"：每进入一次 Burst 计一轮（不是每 tick）。
        //   进入时同时打点 lastBurst / 开 burstUntil 窗口，本轮爆发才能分几个 tick 依次放完。
        if (st == State::Burst && m->state != State::Burst)
        {
            ++m->burstRounds;
            m->lastBurst = time(nullptr);
            m->burstUntil = m->lastBurst + time_t(TP(m, "burst_window", 6.0));
        }
        m->state = st;

        switch (st)
        {
            case State::Recover:
                // [2026-10-31 站长口径] 有蓝职业通用：蓝缺口 > potion_mana_deficit(3000) 就先喝法力药水
                //   （瞬发、不打断输出），还缺再唤醒；都不行交回引擎喝水/魔杖。
                if (UseManaPotion(m, s))
                    return ai::dpsfsm::TickResult::Acted;
                if (TryCastSelf(ai, m, s, "evocation"))
                    return ai::dpsfsm::TickResult::Acted;
                return ai::dpsfsm::TickResult::Decline;   // 剩下的（喝水/魔杖）交回引擎

            case State::Flee:
                if (CastSelfCentered(ai, m, s, "frost nova"))
                    return ai::dpsfsm::TickResult::Acted;
                if (TryCastSelf(ai, m, s, "ice barrier"))
                    return ai::dpsfsm::TickResult::Acted;
                if (TryCastSelf(ai, m, s, "blink"))
                    return ai::dpsfsm::TickResult::Acted;
                return FsmResult(CastFirst(ai, m, s, { "ice lance" }));

            case State::Approach:
                return ai::dpsfsm::TickResult::Decline;   // 走位归引擎

            case State::Aoe:
            {
                // [2026-10-31 站长口径] 群伤也不能让宠物闲着：掉队/换目标就重新下令（每 tick 检查，
                //   10 秒一条 [DPSFSM-PET] 状态日志）
                CommandPetAttack(s);
                // 动作表：**魔爆术（近身、仇恨安全）→ 冰锥（近身且仇恨 Open）→ 暴风雪（可选）→ 冰枪/寒冰箭**。
                //   暴风雪已降级：只在"不在近身 10 码"或"需要减速"时用（aoe_slow_rank 默认 1 级，
                //   减速效果一样、蓝耗低得多；中心点直接用当前目标坐标，pack30>1 就是这个怪堆）。
                // [2026-10-31 站长口径] 冰锥=高威胁牌，**只在 Open 档用**；Throttle/Wait 时改用
                //   魔爆术/暴风雪这类低威胁群伤 —— 仇恨没拉开时把冰锥砸下去，等于替坦克把怪拉过来。
                bool const threatOpen = (threat == ThreatLevel::Open);
                bool const inMelee = s.dist <= float(TP(m, "aoe_melee_range", 10.0));
                bool const wantBlizzard = AoeWantBlizzard(ai, m, s);
                uint32 const slowRank = uint32(TP(m, "aoe_slow_rank", 1.0) + 0.5);
                uint32 const blizzardId = SpellIdAtRank(ai, m, "blizzard", slowRank ? slowRank : 1);
                if (!m->aoeLogDone)
                {
                    m->aoeLogDone = true;
                    sLog.outError("[DPSFSM-AOE] %s inmelee=%d threatOk=%d threat=%s blizzard rank=%u id=%u use=%d pack30=%d dist=%.1f",
                        s.bot->GetName(), inMelee ? 1 : 0, threatOpen ? 1 : 0, ThreatLevelName(threat),
                        slowRank ? slowRank : 1, blizzardId, wantBlizzard ? 1 : 0, s.pack30, s.dist);
                }
                // 主力：魔爆术（近身群伤，仇恨安全）
                if (inMelee && TryCast(ai, m, s, "arcane explosion", nullptr))
                    return ai::dpsfsm::TickResult::Acted;
                // 冰锥：只在近身**且仇恨 Open**时用（Throttle/Wait 时容易抢坦的仇恨）
                if (inMelee && threatOpen && CastFirst(ai, m, s, { "cone of cold" }))
                    return ai::dpsfsm::TickResult::Acted;
                // 暴风雪（可选、低威胁）：不在近身 / 需要减速才用
                if (wantBlizzard && blizzardId
                    && CastIdAt(ai, m, s, blizzardId, s.target->GetPositionX(),
                        s.target->GetPositionY(), s.target->GetPositionZ()))
                    return ai::dpsfsm::TickResult::Acted;
                if (CastFirst(ai, m, s, { "ice lance" }))
                    return ai::dpsfsm::TickResult::Acted;
                return FsmResult(CastFirst(ai, m, s, { "frostbolt" }));
            }

            case State::Burst:
                // 早用早 CD：lastBurst/burstUntil 已在"进入 Burst"那一刻打点，这里不再每 tick 重置
                // [2026-10-31 站长口径] 无蓝职业（战/贼）的爆发消耗品 = 加速药水；有蓝的这里自动跳过
                if (UseHastePotion(m, s))
                    return ai::dpsfsm::TickResult::Acted;
                // [2026-10-31 站长口径] 爆发顺序（修正）：**水元素 → 饰品 → 冰冷血脉** → 急冷（第 2 轮起）→ 寒冰箭填充。
                //   水元素先出场才来得及打；饰品/冰脉放后面不浪费它们的持续时间。
                // 水元素走统一入口：没有宠物才召，失败留一条可区分的 [DPSFSM-PET] 诊断
                if (SummonPetIfNeeded(ai, m, s, "summon water elemental"))
                    return ai::dpsfsm::TickResult::Acted;
                if (TP(m, "burst_trinket", 1.0) >= 1.0 && UseTrinket(ai, m, s))
                    return ai::dpsfsm::TickResult::Acted;
                if (CastFirst(ai, m, s, { "icy veins" }))
                    return ai::dpsfsm::TickResult::Acted;
                {
                    int const snapRound = int(TP(m, "burst_cold_snap_round", 2.0));
                    if (m->burstRounds >= snapRound && CastFirst(ai, m, s, { "cold snap" }))
                        return ai::dpsfsm::TickResult::Acted;
                }
                return FsmResult(CastFirst(ai, m, s, { "frostbolt" }));

            case State::Burn:
                // [2026-10-31 站长口径] 单体常规输出每 tick 先确认宠物在咬当前目标
                //   （宠物掉队/换目标时要重新指令；宠物仇恨不算在 bot 头上，Wait 档也让它继续咬）。
                CommandPetAttack(s);
                // [2026-10-31 站长口径] Hold 的测试通路：`burn_hold=1` 时本 tick **有意空着**
                //   （用于验证 TickResult::Hold 真的能让引擎收手）。默认 0 ⇒ 行为与改造前一致。
                //   [2026-11-01 站长口径] 走 `HoldWait()` 登记：这是**明确的有意空着**，
                //   不能被 Tick() 的通用兜底降级（否则这个自检通路就失效了）。
                if (TP(m, "burn_hold", 0.0) >= 1.0)
                    return HoldWait(m, "burn_hold(test)");
                // [2026-10-31 站长口径] 仇恨 Wait 档（Maintain）：**不施法、也不让引擎动**。
                //   此时抢在坦克前面打，换来的是一只追着法师的怪（治疗多花蓝、坦克多花时间），
                //   就像把家门让给了对方；等一下让坦克把仇恨拉开，比多打一发寒冰箭划算。
                //   Throttle 档不 Hold：照常打低威胁填充（寒冰箭/冰枪），只是不开爆发、不用冰锥。
                //   [2026-11-01] 登记为有意等（仇恨纪律）；降级成 Decline 会让引擎按它自己的
                //   AttackAction 出手，抢在坦克前面 —— 这条是"有意等"而不是"没判清楚"。
                if (threat == ThreatLevel::Wait)
                    return HoldWait(m, "threat-wait");
                // 平稳核心：寒冰箭是**唯一填充**（节能施法时也照打寒冰箭，不插别的）；
                //   移动中读条会被取消 ⇒ 先试瞬发冰枪。
                if (TP(m, "burn_lance_moving", 1.0) >= 1.0 && s.moving
                    && CastFirst(ai, m, s, { "ice lance" }))
                    return ai::dpsfsm::TickResult::Acted;
                return FsmResult(CastFirst(ai, m, s, { "frostbolt" }));

            case State::Idle:
            default:
                break;
        }
        return FsmResult(CastFirst(ai, m, s, { "frostbolt" }));
    }

    // ==========================================================================
    // [2026-10-31 站长口径] 奥法状态图（mode=5 专用；legacy 的 `TickMage` 与 `DpsPolicy.inc`
    //   里的 3+3（`ExecuteArcane33` / planId=1）**一个字节都不动**）。
    //
    //   平稳核心 = **蓝量驱动**：
    //     蓝够（≥ `burn_mana`，默认 60%）= 无脑奥冲（叠层既加伤害也加耗蓝，这是"烧蓝换 DPS"）；
    //     蓝见底（< `burn_mana`）= 切 3+3：3 发奥冲把 8 秒 debuff 叠满 3 层 → 在窗口里塞寒冰箭
    //       （寒冰箭不吃奥冲的耗蓝加成）→ 卡 debuff 最后 `weave_margin`（默认 500ms）读下一发奥冲
    //       （这一发起手时还吃到 3 层的加速，落地时叠层已回落 ⇒ 高速低费，同时是下一轮的第一发）。
    //   爆发 = 奥强 → 冰脉（学会了才放）→ 全程奥冲连发；恢复 = 宝石 → 大蓝 → 唤醒。
    //
    //   复用 legacy 的现成口径（不重新发明）：
    //     · 叠层 debuff 是**独立 id 36032**，不在 30451 的 spell_chain 里（见 TickMage 的注释）；
    //     · 卡秒判据用 `GetSpellAuraHolder(36032)->GetAuraDuration()`（同 `ExecuteArcane33`）；
    //     · 读条时长用 `GetSpellCastTime(entry, bot)`（同 `Arcane33GroupScore` / `ExecuteArcane33`）。
    // ==========================================================================
    uint32 const kArcaneBlastDebuff = 36032;    // 奥术冲击的叠层 debuff（TBC 独立 id）

    // [2026-10-31 站长口径] 某个法术在**当前急速下**的实际读条毫秒数（奥法预算排程要用的那个数）。
    //   [2026-11-01 站长口径] **全部候选（含 frostbolt / scorch / 奥冲）统一走这一份**，不再分口径：
    //     ① `GetSpellCastTime(entry, bot)`（不传 Spell 对象）**不含** `SPELLMOD_CASTING_TIME`
    //        —— 改良寒冰箭 −0.5 秒这类天赋修正只有核心带 Spell 对象时才套得上，所以自动值会偏高
    //        （实测寒冰箭 3000ms，真实 2500ms）⇒ 这里直接调 `Player::ApplySpellMod`
    //        （与 `GetSpellCastTime` 内部同一个函数、同一条判据，`finalUse=false` 不消耗一次性充能）；
    //     ② 再乘 `UNIT_MOD_CAST_SPEED`（冰脉/饰品/英勇走这个字段；核心只在"普通法术"上乘它）。
    //   fallback：拿不到模板、或读条异常（瞬发 / 数据缺失）时的回落值。
    int CastMsWithMod(PlayerbotAI* ai, BotMem* m, Snapshot& s, char const* name, int fallback)
    {
        uint32 const id = SpellId(ai, s.bot, m, name, false);
        SpellEntry const* entry = id ? sServerFacade.LookupSpellInfo(id) : nullptr;
        if (!entry)
            return fallback;

        int castMs = int(GetSpellCastTime(entry, s.bot));
        // 天赋/装备的 SPELLMOD_CASTING_TIME（改良寒冰箭、强化灼烧…）：核心在
        //   `GetSpellCastTime` 里只对"带 Spell 对象"的调用套这一层，本模块没有 Spell 对象 ⇒ 自己补。
        if (castMs > 0 && s.bot)
        {
            int32 modded = castMs;
            s.bot->ApplySpellMod(id, SPELLMOD_CASTING_TIME, modded, false);
            if (modded > 0)
                castMs = modded;
        }
        if (castMs > 0
            && !entry->HasAttribute(SPELL_ATTR_IS_ABILITY)
            && !entry->HasAttribute(SPELL_ATTR_IS_TRADESKILL)
            && !entry->HasAttribute(SPELL_ATTR_EX3_IGNORE_CASTER_MODIFIERS))
        {
            float const mod = s.bot->GetFloatValue(UNIT_MOD_CAST_SPEED);
            if (mod > 0.2f && mod < 2.0f)
                castMs = int(float(castMs) * mod + 0.5f);
        }
        if (castMs < 300 || castMs > 6000)                  // 异常值 ⇒ 当没有急速，别把窗口算翻
            return fallback;
        return castMs;
    }

    // ==========================================================================
    // [2026-11-01 站长口径] **填充改成"按窗口筛候选"**（奥法先做，冰法/火法复用同一套结构）
    //
    //   旧口径的填充是**写死的技能顺序**（火焰冲击 → 冰枪），两个实测病：
    //     ① 1.5 秒级的技能（`scorch`，本核心 27078 一带）根本没被用上 —— 窗口 1.6~2.4 秒时
    //        它才是唯一放得下的读条填充，写死顺序只会"寒冰箭塞不进 ⇒ 丢一对瞬发 ⇒ 空等"；
    //     ② `fire blast`（火焰冲击）射程只有 **20 码**，站在 25~30 码放必然
    //        `SPELL_FAILED_OUT_OF_RANGE` ⇒ **白丢一个 GCD**（写死顺序里它还永远排第一）。
    //   新口径：候选表 + 过滤器 ——
    //     窗口 W = 叠层 debuff 剩余（不在 3+3 时用一个很大的值）；
    //     ① 过滤：学会了（`SpellId` ≠ 0）、**射程够**（核心 `GetSpellMaxRange(SpellRangeEntry)`）、
    //        `Ready()`、蓝够（`SpellEntry->manaCost` ≤ 当前法力）；
    //     ② 过滤 `实际读条 > W`（瞬发 castMs=0 恒过）；
    //     ③ 剩下的按 `weight` 从高到低排序，逐个别试着放（放不出就下一个，绝不空等）；
    //     ④ 一个都不剩 ⇒ 调用方 Hold。
    //   `castMs` 是**基准**读条（无急速时的值），实际读条一律再走 `CastMsWithMod`
    //   （按当前急速算；`fb_cast_ms` / `fill_margin` 那套 A/B 口径照旧生效）。
    // --------------------------------------------------------------------------
    struct FillCand
    {
        char const* name;
        int castMs;      // 基准读条毫秒（瞬发 = 0）
        float weight;    // 相对价值（越大越优先；默认值按 TBC 常识，用 TP() 可调）
        uint32 gate;     // 额外门槛 FillGate（0 = 通用填充）
    };

    // 通用过滤（学会/射程/冷却/蓝/读条≤W）之外的那几种"只在特定场合才对"的候选。
    enum FillGate : uint32
    {
        kFillAlways   = 0,
        kFillTailBand = 1u << 0,   // 只在"卡秒叠层相位"可用（奥冲：读条正好落在 debuff 尾巴上）
        kFillFrozen   = 1u << 1,   // 只在目标被冻住时可用（冰枪：TBC 只对冰冻目标有 3 倍伤害）
    };

    // 门槛求值要用到的那几个数，由调用方算好传进来（别在过滤器里重算一遍）。
    struct FillGateCtx
    {
        int remain = -1;        // 3+3 debuff 剩余毫秒（-1 = 没有 / 读不到时长）
        int tailCastMs = 0;     // 卡秒相位用的基准读条（奥冲）
        int tailMarginMs = 0;   // 卡秒容差（`fill_margin`）
        bool frozen = false;    // 目标是否被冻住（`Unit::isFrozen()`，与冰枪脚本同一判据）
        bool frozenOnly = true; // `ice_lance_frozen_only`：0 ⇒ 冰枪不看冻结（A/B 兜底）
    };

    // 候选读条（**统一口径**：[2026-11-01 站长口径] 全部候选都走 `CastMsWithMod`，
    //   含天赋的 SPELLMOD_CASTING_TIME + 当前急速；瞬发返回 0，没学会返回 -1）。
    //   旧的 `c.castMs` 只当"表里的基准值/兜底值"用，不再直接参与窗口判定。
    int CandCastMs(PlayerbotAI* ai, BotMem* m, Snapshot& s, FillCand const& c)
    {
        if (c.castMs <= 0)
            return 0;                        // 瞬发：没有读条
        if (!SpellId(ai, s.bot, m, c.name, false))
            return -1;                       // 没学会 / 本体没有这个法术
        return CastMsWithMod(ai, m, s, c.name, c.castMs);
    }

    // 这个候选现在能不能用：通用过滤 + 额外门槛 + 窗口。返回实际读条毫秒；不能放返回 -1。
    int FillCastMs(PlayerbotAI* ai, BotMem* m, Snapshot& s, FillCand const& c, int windowMs, FillGateCtx const& g)
    {
        uint32 const id = SpellId(ai, s.bot, m, c.name, false);
        if (!id)
            return -1;                       // 没学会 / 本体没有这个法术
        if (!Ready(s.bot, id))
            return -1;                       // 冷却 / GCD / 沉默

        // 射程：用核心的 `SpellEntry::rangeIndex` → `SpellRangeEntry.maxRange`，
        //   留 1 码余量（`Snapshot::dist` 是 `GetDistance`，贴着边线会判 OUT_OF_RANGE）。
        //   ⇒ 火焰冲击（20 码）在 25~30 码自动出局，不会再"放失败白丢 GCD"。
        if (SpellEntry const* entry = sServerFacade.LookupSpellInfo(id))
        {
            float const maxRange = GetSpellMaxRange(sSpellRangeStore.LookupEntry(entry->rangeIndex));
            if (maxRange > 0.0f && s.dist > maxRange - 1.0f)
                return -1;
        }

        if (!AffordSpell(ai, m, s, c.name))
            return -1;                       // 蓝不够（`SpellEntry->manaCost` vs 当前法力绝对值）

        if (c.gate & kFillTailBand)
        {
            // 卡秒相位：**现在起手，读条正好落在 debuff 尾巴上**（起手吃 3 层，落地已回落）。
            //   remain < 0 是异常兜底（模板在、holder 却读不到时长）⇒ 直接起手奥冲，绝不空等。
            bool ok = (g.remain < 0);
            if (!ok && g.remain >= g.tailCastMs)
                ok = ((g.remain - g.tailCastMs) < g.tailMarginMs);
            if (!ok)
                return -1;
        }
        if (c.gate & kFillFrozen)
        {
            if (g.frozenOnly && !g.frozen)
                return -1;                   // 普通目标打冰枪 = 极低伤害 + 白占 GCD
        }

        // 瞬发恒过窗口；读条技能按**当前急速 + 天赋修**的实际读条判"这一发塞不塞得进 W"。
        int const castMs = CandCastMs(ai, m, s, c);
        if (castMs < 0)
            return -1;
        if (castMs > windowMs)
            return -1;
        return castMs;
    }

    // 按权重从高到低，把"能放"的候选填进 out[]（最多 maxOut 个），返回个数。
    //   调用方按返回顺序逐个试着放：第一个被拒（移动打断/朝向/核心条件）就试下一个，
    //   一个都放不出才 Hold —— "能读寒冰箭就必须读"这件事由权重保证，不需要再写死顺序。
    int PickFill(PlayerbotAI* ai, BotMem* m, Snapshot& s, FillCand const* cands, int candCount,
                 int windowMs, FillGateCtx const& g, char const* out[], int maxOut)
    {
        float weights[8] = { 0.0f };
        int n = 0;
        if (maxOut > 8)
            maxOut = 8;
        for (int i = 0; i < candCount; ++i)
        {
            if (n >= maxOut)
                break;
            FillCand const& c = cands[i];
            if (FillCastMs(ai, m, s, c, windowMs, g) < 0)
                continue;
            // 插入排序（候选最多 6 个，没必要为它上 std::sort 的 lambda）
            int k = n;
            while (k > 0 && weights[k - 1] < c.weight)
            {
                weights[k] = weights[k - 1];
                out[k] = out[k - 1];
                --k;
            }
            weights[k] = c.weight;
            out[k] = c.name;
            ++n;
        }
        return n;
    }

    // 奥法填充候选表（权重默认值照 TBC 常识，全部可用 TP() 覆盖做 A/B）：
    //   · `arcane blast`（w_fill_ab=1.0）**只在卡秒叠层相位**可用（kFillTailBand）：
    //     平时它是 3+3 的"叠层 / 收尾"那一手，当通用填充会把叠层一直顶满、吃不到回落节奏；
    //   · `frostbolt`（w_fill_frostbolt=0.85，2.5 秒）是主填充；
    //   · `scorch`（w_fill_scorch=0.7，1.5 秒）—— 窗口 1.6~2.4 秒时唯一放得下的读条填充；
    //   · `fire blast`（w_fill_fireblast=0.6，瞬发）射程只有 20 码，靠射程过滤挡掉；
    //   · `ice lance`（w_fill_icelance=0.3，瞬发）只在目标被冻住时（kFillFrozen）；
    //   · `arcane missiles`（w_fill_missiles=0.5，引导 2.5 秒）窗口够才考虑。
    int ArcaneFillCands(BotMem* m, FillCand out[6])
    {
        out[0] = FillCand{ "arcane blast",    2500, float(TP(m, "w_fill_ab", 1.0)),        kFillTailBand };
        out[1] = FillCand{ "frostbolt",       2500, float(TP(m, "w_fill_frostbolt", 0.85)), kFillAlways };
        out[2] = FillCand{ "scorch",          1500, float(TP(m, "w_fill_scorch", 0.7)),     kFillAlways };
        out[3] = FillCand{ "fire blast",         0, float(TP(m, "w_fill_fireblast", 0.6)),  kFillAlways };
        out[4] = FillCand{ "ice lance",          0, float(TP(m, "w_fill_icelance", 0.3)),   kFillFrozen };
        out[5] = FillCand{ "arcane missiles", 2500, float(TP(m, "w_fill_missiles", 0.5)),   kFillAlways };
        return 6;
    }

    // [2026-10-31 站长口径] **冰脉尾巴窗口按"还能不能放下下一发奥冲"动态算**：
    //   tail = 当前急速下奥术冲击的实际读条 + `iv_tail_margin`（默认 0）；
    //   冰脉剩余 `ivRemain > tail` ⇒ 还塞得下一发奥冲 ⇒ 先不放唤醒（继续吃冰脉）；
    //   `ivRemain <= tail` ⇒ 已经放不完一发奥冲 ⇒ 就是开唤醒（拉满引导落在冰脉尾巴里）的时机。
    //   固定的 `iv_tail_window`（2500ms）不再是主判据，只留作"查不到奥冲读条"时的兜底
    //   （例如 bot 没学会奥冲 / SpellEntry 缺失）；正常路径一律用 `iv_tail_margin`。
    //   [2026-10-31 站长口径] 读条本身走 `CastMsWithMod`（与 Maintain 的寒冰箭预算同一份实现），
    //   两种异常（模板缺失 / 读条异常）统一回落到 `iv_tail_window`。
    int IvTailMs(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        int const fallback = int(TP(m, "iv_tail_window", 2500.0));
        return CastMsWithMod(ai, m, s, "arcane blast", fallback)
            + int(TP(m, "iv_tail_margin", 0.0));
    }

    // [2026-10-31 站长口径] 法力宝石：法师 Recover 的第一顺位（瞬发、应急回蓝）。
    //   复用引擎现成的 "mana gem" 动作（`UseManaGemAction` 自己按等级挑宝石；`BOT_CHEATS=item`
    //   时免物品），不自己扫背包——`UseManaPotion` 那条扫包路已经证明了"物品识别"容易走偏。
    //   `DoSpecificAction` 的返回值分不出"没宝石"和"宝石在 CD"，所以 2 秒节流，别每 tick 走引擎。
    bool UseManaGem(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        uint32 const nowMs = WorldTimer::getMSTime();
        uint32 const safeNow = nowMs ? nowMs : 1;
        if (m->lastGemTry && WorldTimer::getMSTimeDiff(m->lastGemTry, safeNow) < 2000)
            return false;
        m->lastGemTry = safeNow;
        if (!ai->DoSpecificAction("mana gem", Event(), true))
            return false;
        sLog.outError("[DPSFSM] %s 吃法力宝石（res=%.0f%%）", s.bot->GetName(), s.power);
        return true;
    }

    // 状态转移（顺序 = 站长口径）：Idle → Recover（蓝见底）→ Aoe（群架）→ Burst（仇恨稳定 + CD 到）
    //   → Approach（够不着）→ Maintain（平稳输出，里面跑 3+3）。
    State EvalArcane(BotMem* m, Snapshot& s, ThreatLevel threat)
    {
        if (!s.target)
            return State::Idle;

        // 蓝见底先回蓝：`recover_enter`（默认 6%）进、`recover_exit`（默认 20%）出。
        //   滞回是必须的：没滞回时只有 6%~7% 这一条缝会留在 Recover，唤醒一落地就被打回 Maintain，
        //   而唤醒是**引导**（8 秒）——引导期间本函数不决策，落地即出，正好回到 3+3 的起点。
        float const recoverEnter = float(TP(m, "recover_enter", 6.0));
        float const recoverExit = float(TP(m, "recover_exit", 20.0));
        bool const alreadyRecover = (m->state == State::Recover);
        if (alreadyRecover ? (s.power < recoverExit) : (s.power < recoverEnter))
            return State::Recover;

        if (s.pack30 >= AoEPack(m))
            return State::Aoe;

        // 爆发：仇恨 Open 且间隔到了就开；进入后 `burst_window` 秒内保持 Burst，
        //   让"奥强 → 冰脉 → 奥冲"能跨几个 tick 依次放完（早用早 CD）。
        time_t const now = time(nullptr);
        if (now < m->burstUntil)
            return State::Burst;
        if (threat == ThreatLevel::Open
            && now - m->lastBurst >= time_t(TP(m, "burst_interval", 120.0)))
            return State::Burst;

        if (s.dist > float(TP(m, "range_caster", 30.0)))
            return State::Approach;

        return State::Maintain;
    }

    // --------------------------------------------------------------------------
    // Maintain = 蓝量驱动 + **预判排程 + 空档填充**（[2026-10-31 站长口径]）
    //
    //   旧口径（3+3 硬上限）的实测病：寒冰箭塞满 3 发后**一律 Hold 到 debuff 掉到
    //   `arc_weave_margin`(500ms)**，日志实况是 `abaRemain=2873` 就已经开始空等 ⇒
    //   60 秒木桩 `mode=5` 只有 27 次施法（2.22 秒/发），而 `mode=3` 有 39 次（1.54 秒/发）。
    //   差的那 12 个 GCD 全是白丢的伤害 —— 不是不会打，是在空等。
    //
    //   新口径照猎人 Maintain 那套（先算窗口还放得下什么，放得下就放，绝不放空等）：
    //   规则表（顺序，命中即停）：
    //     ⓪ 叠层（沿用）：debuff 没满 3 层 ⇒ 奥冲（不叠满就没有 3 层的增伤/加速）
    //     ①~③ [2026-11-01 站长口径] 合成一步：**按窗口筛候选**（候选表 `ArcaneFillCands()`、
    //        过滤器 `PickFill()`）—— 学会了 + 射程够 + 冷却好 + 蓝够 + 读条 ≤ 窗口 W 的候选里，
    //        按权重从高到低逐个别试，挑中的那一手就是本 GCD。
    //        （旧口径是写死顺序"寒冰箭 → 火焰冲击 → 冰枪 → 卡秒奥冲"：1.5 秒的灼烧永远轮不到，
    //          而 20 码的火焰冲击在 25~30 码必然施法失败、白丢一个 GCD。）
    //     ④ 候选一个都不剩且确实什么都放不进 ⇒ Hold（**并自己转向**：Hold 时引擎整 tick 不跑，
    //        朝向会冻结 ⇒ 下一发施法/挥击被 IsInFront 判掉，战士那套的坑对法系一样成立）
    //     ⑤ 蓝不足放任何技能 ⇒ 交回引擎（下一 tick `recover_enter` 判据会把它送进 Recover）
    //
    //   planId=2 保留为"蓝见底、在 3+3 里"的标记（和 legacy 的 planId=1 分开，两边不共用状态）；
    //   planStep 从"上一轮放了几发寒冰箭"退化成**纯诊断计数**（真实节奏由窗口预算决定，不再当上限用）。
    //   打断条件：目标没了/换了、进 Burst 或 Recover（由 TickArcaneFsm 清计划）、
    //             蓝回到 `burn_mana` 以上（回到无脑奥冲）。
    // --------------------------------------------------------------------------
    ai::dpsfsm::TickResult TickArcaneMaintain(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        using ai::dpsfsm::TickResult;

        // 读条/引导中一律不插技能（与 legacy TickMage 一致；CastChecked 也会因为 GCD 拒绝）
        if (s.casting)
        {
            m->arcBranch = 'G';
            return TickResult::Acted;
        }

        // 别的决策模式（mode=3 的 legacy 3+3）可能留下 planId=1，这里不认它，清成"无计划"
        if (m->planId != 0 && m->planId != 2)
        {
            m->planId = 0;
            m->planStep = 0;
        }

        // 目标死亡/切换 ⇒ 计划失效（BuildSnapshot 已经把死目标置空）
        if (m->planId == 2 && (!s.target || m->planTarget != s.target->GetObjectGuid()))
        {
            m->planId = 0;
            m->planStep = 0;
        }

        float const burnMana = float(TP(m, "burn_mana", 60.0));

        // 蓝回到 burn_mana 以上 ⇒ 退出计划，回到无脑奥冲
        if (m->planId == 2 && s.power >= burnMana)
        {
            m->planId = 0;
            m->planStep = 0;
        }

        // 还没进计划：蓝够就无脑奥冲；蓝见底才开 3+3
        if (m->planId != 2)
        {
            if (s.power >= burnMana || !s.target)
            {
                m->arcBranch = 'U';     // 蓝够：无脑奥冲（不进 3+3）
                return FsmResult(CastFirst(ai, m, s, { "arcane blast" }));
            }

            m->planId = 2;
            m->planStep = 0;
            m->planTarget = s.target->GetObjectGuid();
        }

        // ---- 计划执行（每一步都走标准施法入口：记账/退避/朝向都在 CastChecked 里）----
        if (!SpellId(ai, s.bot, m, "arcane blast", false))
            return TickResult::Decline;     // 没学会奥冲 ⇒ 这个专精走不了 3+3，交回引擎

        int const stacks = std::min(3, int(s.bot->GetAuraCount(kArcaneBlastDebuff)));
        int remain = -1;
        if (SpellAuraHolder* holder = s.bot->GetSpellAuraHolder(kArcaneBlastDebuff))
            remain = holder->GetAuraDuration();

        // ⓪ 叠层：没满 3 层就继续奥冲（debuff 掉光也从 0 开始）
        if (stacks < 3)
        {
            m->arcBranch = 'S';
            if (m->planStep >= 3)       // 窗口断了（掉光/被驱散）⇒ 整组从叠层重来
                m->planStep = 0;
            if (!CastFirst(ai, m, s, { "arcane blast" }))
            {
                // [2026-10-31 站长口径] 只在**确知是 GCD 还在转**的时候 Hold：那一 tick 谁也放不出技能，
                //   交回引擎它只会插一手自己的（把叠层节奏打乱）。
                //   其它失败原因（射程/LOS/真没蓝）**必须 Decline**：引擎要负责走位/喝水/魔杖，
                //   这里 Hold 会把它锁死 —— 实测 Hold 版在低蓝段出现过整窗 `holds=48 casts=0` 的死等。
                uint32 const abId = SpellId(ai, s.bot, m, "arcane blast", false);
                return GcdActive(s.bot, abId) ? TickResult::Hold : TickResult::Decline;
            }
            if (m->planStep <= stacks)
                m->planStep = stacks + 1;   // 已经放出去几发 = 当前层数 + 这一发
            return TickResult::Acted;
        }

        // 叠满了 ⇒ 进入填窗相位（进入 Maintain 时本来就带着 3 层也走这条）
        if (m->planStep < 3)
            m->planStep = 3;

        // ---- 窗口预算：这一发读条要多久（当前急速 + 天赋修）+ 填充余量 ----
        //   余量的意义：读条的最后 `fill_margin` 毫秒里 debuff 已经掉光也不亏（寒冰箭本来就不吃叠层），
        //   留这点余量是为了容忍"读条开始到服务器结算"的那点时间差，别把一发读条截在尾巴上。
        //   [2026-11-01 站长口径] `CastMsWithMod` 现在**含 SPELLMOD_CASTING_TIME**（改良寒冰箭 −0.5 秒），
        //    自动值已贴近真实读条（寒冰箭 ≈2500、灼烧 ≈1500），`fb_cast_ms` 只剩"手工钉死做 A/B"的用途。
        int const fillMargin = int(TP(m, "fill_margin", 200.0));
        int const fbCastCfg = int(TP(m, "fb_cast_ms", 0.0));
        int const fbCast = (fbCastCfg >= 300 && fbCastCfg <= 6000)
            ? fbCastCfg : CastMsWithMod(ai, m, s, "frostbolt", 2500);
        // [2026-11-01 站长口径] `fbBudget` 只留给 [DPSFSM-ARC]/[DPSFSM-ARC-DIAG] 的日志口径
        //   （它们要跟旧版数字直接对照）；**决策不再用它** —— 决策一律走下面的候选筛选。
        int const fbBudget = fbCast + fillMargin;

        // ---- 填充：按窗口 W 筛候选（[2026-11-01 站长口径]，表与过滤器见 FillCand / PickFill）----
        //   W = 叠层 debuff 剩余；读不到时长（remain<0）时用"大值"表示窗口足够
        //   （卡秒奥冲另有 remain<0 的兜底，判在 kFillTailBand 里）。
        int const windowMs = (remain >= 0) ? remain : 1000000;

        FillGateCtx g;
        g.remain = remain;
        g.tailCastMs = CastMsWithMod(ai, m, s, "arcane blast", 2500);
        g.tailMarginMs = fillMargin;
        g.frozen = (s.target && s.target->isFrozen());
        g.frozenOnly = (TP(m, "ice_lance_frozen_only", 1.0) >= 0.5);

        FillCand cands[6];
        int const candCount = ArcaneFillCands(m, cands);

        // [2026-11-01 站长口径] `fill_dbg=1`（默认关）：把每个候选的**当前读条**打一条，
        //   用来验收"读条口径统一走 SPELLMOD"——寒冰箭应 ≈2500（旧口径 3000），灼烧 ≈1500。
        //   1 秒节流：窗口判定每 tick 都算，日志不能每 tick 刷。
        if (TP(m, "fill_dbg", 0.0) >= 0.5)
        {
            uint32 const dbgNow = WorldTimer::getMSTime();
            uint32 const dbgSafe = dbgNow ? dbgNow : 1;
            if (!m->lastFillDbg || WorldTimer::getMSTimeDiff(m->lastFillDbg, dbgSafe) >= 1000)
            {
                m->lastFillDbg = dbgSafe;
                for (int i = 0; i < candCount; ++i)
                    sLog.outError("[DPSFSM-FILL] %s cand=%s cast=%d",
                        s.bot->GetName(), cands[i].name, CandCastMs(ai, m, s, cands[i]));
            }
        }

        char const* picks[6];
        int const pickCount = PickFill(ai, m, s, cands, candCount, windowMs, g, picks, 6);

        // 按权重逐个试着放：第一个被拒（移动打断/朝向/核心条件）就试下一个，绝不空等。
        char const* picked = nullptr;
        for (int i = 0; i < pickCount; ++i)
        {
            if (!CastFirst(ai, m, s, { picks[i] }))
                continue;
            picked = picks[i];
            break;
        }

        // [DPSFSM-FILL] 10 秒一条：W / 实际挑中 / 通过过滤的候选数。
        //   验收口径："窗口 1.6~2.4 秒时 pick=scorch"就是这张表在起作用
        //   （旧写死顺序在同一时刻只会 pick=frostbolt 或 none，1.5 秒技能永远轮不到）。
        //   [2026-11-01 站长口径] **窄窗候选（灼烧/瞬发）额外补一条、不受 10 秒节流**：
        //   叠层窗口有 8 秒，`W` 落到 1.6~2.4 秒这种"只够一发 1.5 秒读条"的时刻每轮只出现一两次，
        //   正好落进节流缝里就永远看不到 —— 而验收要的正是这一条。
        //   常态（寒冰箭填充 / 卡秒奥冲）仍然 10 秒一条，不刷屏。
        {
            uint32 const nowMs2 = WorldTimer::getMSTime();
            uint32 const safeNow2 = nowMs2 ? nowMs2 : 1;
            bool const narrowPick = (picked
                && strcmp(picked, "frostbolt") != 0 && strcmp(picked, "arcane blast") != 0);
            if (narrowPick || !m->lastFillLog
                || WorldTimer::getMSTimeDiff(m->lastFillLog, safeNow2) >= 10000)
            {
                m->lastFillLog = safeNow2;
                sLog.outError("[DPSFSM-FILL] %s W=%d pick=%s cands=%d",
                    s.bot->GetName(), windowMs, picked ? picked : "none", pickCount);
            }
        }

        if (picked)
        {
            if (strcmp(picked, "frostbolt") == 0)
            {
                m->arcBranch = 'F';
                ++m->planStep;          // 纯诊断计数（见上面的说明，不再当上限）
            }
            else if (strcmp(picked, "arcane blast") == 0)
            {
                m->arcBranch = 'B';
                m->planStep = 1;        // 卡秒这一发就是下一轮的第 1 发奥冲
            }
            else if (strcmp(picked, "fire blast") == 0 || strcmp(picked, "ice lance") == 0)
                m->arcBranch = 'I';
            else
                m->arcBranch = 'C';     // 其它读条填充（灼烧 / 奥术飞弹）
            return TickResult::Acted;
        }

        // ⑤ 蓝不足放任何技能 ⇒ 交回引擎（下一 tick 的 recover_enter 会把它送进 Recover）
        if (!AffordSpell(ai, m, s, "arcane blast") && !AffordSpell(ai, m, s, "frostbolt"))
        {
            m->arcBranch = 'M';
            return TickResult::Decline;
        }

        // ④ 候选一个都不剩（真的一手都放不进 / 放不出）⇒ 有意空着；但**自己转向**，别让朝向冻住
        //    （Hold 时引擎整 tick 都不跑，而转向本来是引擎在 tick 里做的；见战士那套的注释）
        if (s.target && s.target != s.bot)
            s.bot->SetFacingToObject(s.target);
        m->arcBranch = 'H';
        return TickResult::Hold;
    }

    ai::dpsfsm::TickResult TickArcaneFsm(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        using ai::dpsfsm::TickResult;

        // 仇恨档位本 tick 只算一次（诊断由 EvalThreat 内部按 10 秒节流）
        ThreatLevel const threat = EvalThreat(m, s);
        State const st = EvalArcane(m, s, threat);

        // 进入 Burst 的那一刻打点（不是每 tick 重置）：窗口才能跨 tick 放完"奥强 → 冰脉"
        if (st == State::Burst && m->state != State::Burst)
        {
            ++m->burstRounds;
            m->lastBurst = time(nullptr);
            m->burstUntil = m->lastBurst + time_t(TP(m, "burst_window", 6.0));
        }
        m->state = st;

        // 计划只在 Maintain 里活着：进 Burst / Recover / Aoe / 目标没了 ⇒ 整个 3+3 重来
        if (st != State::Maintain && m->planId == 2)
        {
            m->planId = 0;
            m->planStep = 0;
        }

        // [2026-10-31 站长口径] 冰脉尾巴判据本 tick 只算一次：Recover 用它决定"放不放唤醒"，
        //   [DPSFSM-ARC] 也用它解密 —— 日志里的 ivRemain/ivTail 必须就是刚才判据用的那两个数。
        int const ivRemain = AuraRemain(ai, m, s, "icy veins");
        int const ivTail = IvTailMs(ai, m, s);

        TickResult result = TickResult::Decline;
        m->arcBranch = '?';     // 分支取证（arc_diag）本 tick 重新记；非 Maintain 状态保持 '?'
        switch (st)
        {
            case State::Recover:
                // [站长口径] 恢复顺序：宝石 → 大蓝（缺口 > potion_mana_deficit，默认 3000）→ 唤醒。
                //   唤醒落地 = 切回 3+3 的节点（`EvalArcane` 下一 tick 就会给回 Maintain）。
                //   三样都不行 ⇒ 交回引擎（喝水/魔杖）。
                // [2026-10-31 站长口径] **唤醒要卡在冰脉的尾巴上**：尾巴窗口不再写死，
                //   `ivTail = 当前急速下奥冲实际读条 + iv_tail_margin`（见 IvTailMs）；
                //   `ivRemain > ivTail` ⇒ 还塞得下一发奥冲 ⇒ 先不放，继续吃冰脉；
                //   `ivRemain <= ivTail` ⇒ 放不完一发奥冲了 ⇒ 现在开唤醒，让引导落在冰脉窗口里。
                //   冰脉没开时 ivRemain=0 ⇒ 不等，蓝到阈值就放。
                if (UseManaGem(ai, m, s))
                    result = TickResult::Acted;
                else if (UseManaPotion(m, s))
                    result = TickResult::Acted;
                else
                {
                    // [2026-10-31 站长口径] 冰脉尾巴判据是**预判**，不是"等"：
                    //   `ivRemain > ivTail` ⇒ 这一发读下来还不会跨过冰脉结束 ⇒ **不开唤醒，回去继续打**；
                    //   `ivRemain <= ivTail` ⇒ 再读一发就会跨过 ⇒ 现在开唤醒，让引导落在冰脉窗口里。
                    //   旧口径在中段一律 Hold（"等尾巴"），实测就是冰脉尾巴那串空转的 GCD ——
                    //   与 Maintain 里"等 debuff 掉到 500ms"是同一个病，这里一起改掉。
                    bool const atIvTail = (ivRemain <= ivTail);
                    if (!atIvTail)
                    {
                        TickResult const fill = TickArcaneMaintain(ai, m, s);
                        if (fill != TickResult::Decline)
                            result = fill;                       // 继续吃冰脉输出（填充逻辑与 Maintain 同一份）
                        else if (TryCastSelf(ai, m, s, "evocation"))
                            result = TickResult::Acted;          // 填窗也放不出（蓝空/技能全 CD）⇒ 别干等，唤醒
                        else
                            result = TickResult::Hold;
                    }
                    else if (TryCastSelf(ai, m, s, "evocation"))
                        result = TickResult::Acted;
                    else
                        result = TickResult::Decline;
                }
                break;

            case State::Aoe:
                // 奥法没有专门的群伤循环：近身魔爆术 → 远处暴风雪（学会了才放）→ 兜底奥冲。
                if (s.dist <= float(TP(m, "aoe_melee_range", 10.0))
                    && TryCast(ai, m, s, "arcane explosion", nullptr))
                    result = TickResult::Acted;
                else if (SpellId(ai, s.bot, m, "blizzard", false)
                    && TryCastAt(ai, m, s, "blizzard", s.target->GetPositionX(),
                        s.target->GetPositionY(), s.target->GetPositionZ()))
                    result = TickResult::Acted;
                else
                    result = FsmResult(CastFirst(ai, m, s, { "arcane blast" }));
                break;

            case State::Burst:
                // [站长口径] 奥强 → 冰脉（学会了才放）→ 全程奥冲连发。
                //   冰脉先用 `SpellId(..., false)` 探一次：没点天赋时不打"[DPSFSM] 没有可用技能"的错日志。
                if (CastFirst(ai, m, s, { "arcane power" }))
                    result = TickResult::Acted;
                else if (SpellId(ai, s.bot, m, "icy veins", false)
                    && CastFirst(ai, m, s, { "icy veins" }))
                    result = TickResult::Acted;
                else
                    result = FsmResult(CastFirst(ai, m, s, { "arcane blast" }));
                break;

            case State::Approach:
                result = TickResult::Decline;   // 走位归引擎
                break;

            case State::Maintain:
                result = TickArcaneMaintain(ai, m, s);
                break;

            case State::Idle:
            default:
                result = TickResult::Decline;
                break;
        }

        // [2026-10-31 站长口径] `arc_diag=1` 的分支取证（默认关，1 秒一条）：
        //   回答"这一秒为什么没出手/出的是哪一手" —— 只有窗口预算与分支都在手里，才谈得上调排程。
        uint32 const nowMs = WorldTimer::getMSTime();
        uint32 const safeNow = nowMs ? nowMs : 1;
        if (TP(m, "arc_diag", 0.0) >= 1.0
            && (!m->arcDiagAt || WorldTimer::getMSTimeDiff(m->arcDiagAt, safeNow) >= 1000))
        {
            m->arcDiagAt = safeNow;
            int diagStacksD = std::min(3, int(s.bot->GetAuraCount(kArcaneBlastDebuff)));
            int diagRemainD = -1;
            if (SpellAuraHolder* holder = s.bot->GetSpellAuraHolder(kArcaneBlastDebuff))
                diagRemainD = holder->GetAuraDuration();
            uint32 const abId = SpellId(ai, s.bot, m, "arcane blast", false);
            uint32 const fbIdD = SpellId(ai, s.bot, m, "frostbolt", false);
            int const fbCastD = (int(TP(m, "fb_cast_ms", 0.0)) >= 300)
                ? int(TP(m, "fb_cast_ms", 0.0)) : CastMsWithMod(ai, m, s, "frostbolt", 2500);
            char const* resTxt = (result == TickResult::Acted) ? "acted"
                : (result == TickResult::Hold ? "hold" : "decline");
            sLog.outError("[DPSFSM-ARC-DIAG] %s state=%s branch=%c res=%s remain=%d stacks=%d "
                "fbCast=%d fbBudget=%d mana=%.0f casting=%d gcdAb=%d gcdFb=%d moving=%d dist=%.1f",
                s.bot->GetName(), ai::dpsfsm::StateName(st), m->arcBranch, resTxt, diagRemainD,
                diagStacksD, fbCastD, fbCastD + int(TP(m, "fill_margin", 200.0)),
                s.power, s.casting ? 1 : 0, GcdActive(s.bot, abId) ? 1 : 0,
                GcdActive(s.bot, fbIdD) ? 1 : 0, s.moving ? 1 : 0, s.dist);
        }

        // [2026-10-31 站长口径] 奥法诊断：10 秒一条，字段固定 plan/step/stacks/mana/abaRemain，
        //   便于脚本直接切（abaRemain = 奥冲叠层 debuff 剩余毫秒，没有该 debuff 时为 -1）；
        //   [2026-10-31] 追加 ivRemain/ivTail：唤醒尾巴判据的两个原始数（ivTail 已含急速与 iv_tail_margin），
        //   一眼能看出"这次为什么还在等 / 为什么现在开唤醒"。
        //   [2026-10-31 站长口径] 再追加 holds/casts（**本轮 10 秒窗口**的计数，打一条清一次）：
        //     holds = 本窗口 Hold 的 tick 数，casts = 本窗口成功施法数。
        //     这两个数是"空转 GCD"的直接证据 —— 旧口径的 27 次施法/60 秒就是 holds 堆出来的；
        //     只要 casts 上不去，就说明还在等，而不是"不会打"。
        if (result == TickResult::Hold)
            ++m->arcHolds;
        else if (result == TickResult::Acted && m->castThisTick)
            ++m->arcCasts;

        if (!m->lastArcLog || WorldTimer::getMSTimeDiff(m->lastArcLog, safeNow) >= 10000)
        {
            m->lastArcLog = safeNow;
            int const diagStacks = std::min(3, int(s.bot->GetAuraCount(kArcaneBlastDebuff)));
            int diagRemain = -1;
            if (SpellAuraHolder* holder = s.bot->GetSpellAuraHolder(kArcaneBlastDebuff))
                diagRemain = holder->GetAuraDuration();
            sLog.outError("[DPSFSM-ARC] %s plan=%d step=%d stacks=%d mana=%.0f abaRemain=%d ivRemain=%d ivTail=%d holds=%u casts=%u",
                s.bot->GetName(), m->planId, m->planStep, diagStacks, s.power, diagRemain, ivRemain,
                ivTail, m->arcHolds, m->arcCasts);
            m->arcHolds = 0;            // 本轮窗口的计数打一条清一次（累计值看 [DPSFSM-HB] 的 casts=）
            m->arcCasts = 0;
        }

        return result;
    }

    // ==========================================================================
    // [2026-10-31 站长口径] 猎人状态图（兽王/射击/生存，mode=5；legacy TickHunter 不动）
    //   四计时器：平射隐藏施法 0.5s（受急速）｜平射独立冷却（武器速度−0.5s，受急速）
    //            ｜GCD 1.5s（不受急速）｜稳固读条 1.5s 基础（受急速）。
    //   Maintain 的动作表只读 Snapshot 里的 autoRemainMs / steadyCastMs / *Ready，
    //   窗口不够就返回 Hold（有意空着，引擎也不许插），而不是"随便放一个"把平射顶掉。
    // ==========================================================================
    State EvalHunter(BotMem* m, Snapshot& s, ThreatLevel threat)
    {
        if (!s.target)
            return State::Idle;

        // 资源：蓝 < recover_enter 进 Recover，回到 recover_exit 才出（滞回，避免抖动）
        float const recoverEnter = float(TP(m, "recover_enter", 10.0));
        float const recoverExit = float(TP(m, "recover_exit", 30.0));
        bool const alreadyRecover = (m->state == State::Recover);
        if (alreadyRecover ? (s.power < recoverExit) : (s.power < recoverEnter))
            return State::Recover;

        if (s.pack30 >= AoEPack(m))
            return State::Aoe;

        // 爆发：仇恨 Open 且间隔到了就开；进入后 burst_window 秒内保持 Burst，
        //   让"急速射击 → 狂野怒火 → 杀戮命令 → 饰品"能跨几个 tick 依次放完。
        time_t const now = time(nullptr);
        if (now < m->burstUntil)
            return State::Burst;
        if (threat == ThreatLevel::Open
            && now - m->lastBurst >= time_t(TP(m, "burst_interval", 120.0)))
            return State::Burst;

        if (s.dist > float(TP(m, "hunter_range", 35.0)))
            return State::Approach;

        return State::Maintain;
    }

    // Maintain 动作表（顺序，命中即停）：
    //   1 杀戮命令（兽王）→ 2 auto_remain <= autoshot_land → 奥射/否则 Hold
    //   3 autoshot_land < auto_remain < steady_window → 多重 → 奥射 → 否则 Hold
    //   4 auto_remain >= steady_window → 稳固 → 5 兜底 Hold
    ai::dpsfsm::TickResult TickHunterMaintain(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        using ai::dpsfsm::TickResult;

        // 宠物掉队/换目标就重新下令（宠物仇恨不算在 bot 头上，Wait 档也让它继续咬）
        CommandPetAttack(s);

        // 1) 杀戮命令：GCD 外、最高优先（只有兽王点得出来，且必须有宠物在打）
        if (m->spec == Spec::BeastMastery && s.killCommandReady && PetPresent(s)
            && TryCast(ai, m, s, "kill command", s.target))
            return TickResult::Acted;

        int const land = int(float(TP(m, "autoshot_land_ms", 500.0)) * HunterRangedSpeedMod(s.bot) + 0.5f);
        int const steadyWindow = s.steadyCastMs + land;

        // [站长口径] 瞄准射击只有 boss 策略点名（use_aimed=1）才允许出现；
        //   默认 0 ⇒ 本分支恒不进，瞄准射击不会出现在任何常规循环里。
        if (TP(m, "use_aimed", 0.0) >= 1.0 && !s.moving)
        {
            int const aimedCast = int(TP(m, "aimed_cast_ms", 3000.0));
            if (s.autoRemainMs >= aimedCast + land && CastFirst(ai, m, s, { "aimed shot" }))
                return TickResult::Acted;
        }

        // 2) 平射马上落地：只能打瞬发；奥射没好就**空着等这一发**（读稳固等于白送平射）
        //    [2026-11-01 站长口径] 登记为有意等（平射时间轴窗口）：这一手的价值就是"什么都别放"。
        if (s.autoRemainMs <= land)
        {
            if (s.arcaneReady && TryCast(ai, m, s, "arcane shot", s.target))
                return TickResult::Acted;
            return HoldWait(m, "autoshot-window");
        }

        // 3) 窗口放不下稳固、但够一发瞬发：多重 → 奥射 → 否则空着
        if (s.autoRemainMs < steadyWindow)
        {
            if (s.multiReady && TryCast(ai, m, s, "multi-shot", s.target))
                return TickResult::Acted;
            if (s.arcaneReady && TryCast(ai, m, s, "arcane shot", s.target))
                return TickResult::Acted;
            return HoldWait(m, "autoshot-window");
        }

        // 4) 窗口够：稳固填充（移动中读条会被取消 ⇒ 空着走位）
        if (!s.moving && TryCast(ai, m, s, "steady shot", s.target))
            return TickResult::Acted;

        // 5) 兜底：窗口不明/技能都没好 ⇒ 有意空着，别让引擎插一手把平射顶掉
        return HoldWait(m, "autoshot-window");
    }

    ai::dpsfsm::TickResult TickHunterFsm(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        using ai::dpsfsm::TickResult;

        // [2026-10-31 站长口径] **宠物生命周期守卫放在状态机之前**：宠物在不在，直接决定
        //   兽王的杀戮命令能不能出现（`[DUMMY] ... pet=0` 就是这条链断了）。
        //   守卫放了技能（mend/revive/call）时它已经占掉本 tick 的 GCD ⇒ 直接 Acted，不抢第二手；
        //   创建宠物不占 GCD，但同一个 tick 里也不该再放技能（新宠物要时间上召），同样按 Acted 收尾。
        if (HunterPetGuard(ai, m, s))
            return TickResult::Acted;

        // 仇恨档位本 tick 只算一次（诊断日志在 EvalThreat 内部按 10 秒节流）
        ThreatLevel const threat = EvalThreat(m, s);
        State const st = EvalHunter(m, s, threat);

        // 进入 Burst 的那一刻打点（不是每 tick 重置）：窗口才能跨 tick 放完整套爆发
        if (st == State::Burst && m->state != State::Burst)
        {
            m->lastBurst = time(nullptr);
            m->burstUntil = m->lastBurst + time_t(TP(m, "burst_window", 6.0));
        }
        m->state = st;

        TickResult result = TickResult::Decline;
        switch (st)
        {
            case State::Recover:
                // 蝰蛇守护回蓝（没有该光环才挂）；否则用普通攻击，放不出就空着
                if (AuraStacks(ai, s.bot, m, "aspect of the viper", s.bot) == 0
                    && TryCastSelf(ai, m, s, "aspect of the viper"))
                    result = TickResult::Acted;
                else if (TryCast(ai, m, s, "shoot", s.target))
                    result = TickResult::Acted;
                else
                    result = TickResult::Hold;
                break;

            case State::Aoe:
                CommandPetAttack(s);
                // 箭雨（地面）→ 多重 → 回落到 Maintain 计时器表
                if (!s.moving && TryCastAt(ai, m, s, "volley",
                        s.target->GetPositionX(), s.target->GetPositionY(), s.target->GetPositionZ()))
                    result = TickResult::Acted;
                else if (s.multiReady && TryCast(ai, m, s, "multi-shot", s.target))
                    result = TickResult::Acted;
                else
                    result = TickHunterMaintain(ai, m, s);
                break;

            case State::Burst:
                CommandPetAttack(s);
                // 顺序：急速射击 → 狂野怒火 → 杀戮命令 → 饰品；都放完就回 Maintain 继续输出
                if (CastFirst(ai, m, s, { "rapid fire" }))
                    result = TickResult::Acted;
                else if (CastFirst(ai, m, s, { "bestial wrath" }))
                    result = TickResult::Acted;
                else if (s.killCommandReady && PetPresent(s)
                    && TryCast(ai, m, s, "kill command", s.target))
                    result = TickResult::Acted;
                else if (UseTrinket(ai, m, s))
                    result = TickResult::Acted;
                else
                    result = TickHunterMaintain(ai, m, s);
                break;

            case State::Approach:
                result = TickResult::Decline;   // 走位归引擎
                break;

            case State::Maintain:
                result = TickHunterMaintain(ai, m, s);
                break;

            case State::Idle:
            default:
                result = TickResult::Decline;
                break;
        }

        // ------------------------------------------------------------------
        // L6 不变量：核心 `Player::CastSpell()` 每次施法前都会
        //   `InterruptSpell(CURRENT_AUTOREPEAT_SPELL)` —— 所以**任何施法之后**自动射击
        //   都会被顶掉。只要没挂着就立刻重挂（自动射击不吃 GCD），这一步必须在 Tick 末尾，
        //   否则中间那 0.5 秒起手会把自动射击饿死。
        // ------------------------------------------------------------------
        if (s.target && !s.moving && s.dist >= 8.0f
            && !s.bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
        {
            if (!TryCast(ai, m, s, "auto shot", s.target))
                ai->DoSpecificAction("auto shot", Event(), true);
        }

        // [2026-10-31] `hunter_diag=1` 时的窗口取证（默认关，2 秒一条）：
        //   平射剩余 / 平射落地 / 稳固窗口 / 自动射击是否挂着 / 本 tick 放出了什么。
        if (TP(m, "hunter_diag", 0.0) >= 1.0)
        {
            uint32 const nowMs = WorldTimer::getMSTime();
            uint32 const safeNow = nowMs ? nowMs : 1;
            if (!m->hunterDiagAt || WorldTimer::getMSTimeDiff(m->hunterDiagAt, safeNow) >= 2000)
            {
                m->hunterDiagAt = safeNow;
                char const* res = (result == TickResult::Acted) ? "acted"
                    : (result == TickResult::Hold ? "hold" : "decline");
                int const land = int(float(TP(m, "autoshot_land_ms", 500.0))
                    * HunterRangedSpeedMod(s.bot) + 0.5f);
                sLog.outError("[DPSFSM-HUNTER-DIAG] %s state=%s auto=%dms land=%d win=%d steady=%d "
                    "rawTimer=%ums interval=%ums autoRepeat=%d moving=%d mult=%d arc=%d kc=%d res=%s",
                    s.bot->GetName(), ai::dpsfsm::StateName(m->state), s.autoRemainMs, land,
                    s.steadyCastMs + land, s.steadyCastMs,
                    s.bot->getAttackTimer(RANGED_ATTACK), m->autoShotInterval,
                    s.bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL) ? 1 : 0,
                    s.moving ? 1 : 0, s.multiReady ? 1 : 0, s.arcaneReady ? 1 : 0,
                    s.killCommandReady ? 1 : 0, res);
            }
        }

        return result;
    }

    // ==========================================================================
    // [2026-10-31 站长口径] 近战接进 mode=5：战士（武器/狂暴）+ 盗贼（刺杀/战斗/敏锐）。
    //   legacy 的 `TickWarrior` / `TickRogue` **一个字节都不动**（mode=0 仍走它们），
    //   这里只复用它们已经验证过的口径：
    //     · 战士：怒气是"能不能放这一手"的唯一门槛，`rage_enter` / `rage_exit` 做滞回；
    //       不够放关键技能时**有意空着等普攻回怒**（Hold）—— 交回引擎它就会插自己的技能，
    //       把怒气换成非关键技能，节奏就散了；
    //     · 盗贼：能量同理（`energy_enter` / `energy_exit`）。
    //       满星但能量不够终结技时也 Hold：连击点上限 5，继续攒星全浪费，交回引擎更糟；
    //     · 近战都要找背（`NeedsBehind` + `GetBehind`）：站背后不吃招架/躲闪，毁伤这类还**要求**背后。
    //       走位不占 GCD，但既然这一手已经交给移动，就不再抢第二个动作 ⇒ 按 Acted 收尾。
    //   阈值一律走 TP()：rage_enter/rage_exit/range_melee/execute_hp/cleave_rage/rage_reserve/
    //   energy_enter/energy_exit/snd_refresh/snd_cp/fin_cp/fin_energy/aoe_pack/burst_interval/burst_window。
    //   防护战不接管：`Spec::Protection` 在派发处就被排除，这里再兜一次 Decline（防御有另一套职责）。
    // ==========================================================================

    // [DPSFSM-MELEE] <bot> spec= state= rage= energy= combo= targetHp=：10 秒一条（节流，别每 tick 刷屏）。
    //   字段固定，方便脚本直接切（站长口径的格式按这个念）。
    void MeleeLogTick(BotMem* m, Snapshot& s, State st)
    {
        uint32 const nowMs = WorldTimer::getMSTime();
        uint32 const safeNow = nowMs ? nowMs : 1;
        if (m->lastMeleeLog && WorldTimer::getMSTimeDiff(m->lastMeleeLog, safeNow) < 10000)
            return;
        m->lastMeleeLog = safeNow;
        // 怒气按真实值打（战士的存储值 ×10，见 RageReal 的说明）；盗贼没有怒气恒为 0
        int const rageShown = (s.bot && s.bot->getClass() == CLASS_WARRIOR) ? s.rage / 10 : s.rage;
        sLog.outError("[DPSFSM-MELEE] %s spec=%s state=%s rage=%d energy=%d combo=%u targetHp=%.0f dist=%.1f pack10=%d",
            s.bot ? s.bot->GetName() : "-", ai::dpsfsm::SpecName(m->spec), ai::dpsfsm::StateName(st),
            rageShown, s.energy, uint32(s.combo), s.targetHp, s.dist, s.pack10);
    }

    // "学会了才放"：SpellId(warn=false) 不会往 unresolved 里塞噪音，也不会去试没学的技能。
    bool MeleeLearned(PlayerbotAI* ai, Player* bot, BotMem* m, char const* name)
    {
        uint32 const id = SpellId(ai, bot, m, name, false);
        return id != 0 && bot->HasSpell(id);
    }

    // [2026-10-31 站长口径] **怒气是"×10"存储的，阈值必须换算成真实怒气再比**。
    //   本核心 `POWER_RAGE_DEFAULT = 1000`（Unit.h），怒气增益走
    //   `Player::RewardRage()` → `ModifyPower(POWER_RAGE, addRage * 10)`，
    //   所以 `Unit::GetPower(POWER_RAGE)` 给的是 0..1000 的**存储值**，而技能表里的消耗
    //   （`SpellEntry->manaCost`：英勇 15 / 嗜血 30 / 致死 30）是**真实怒气**。
    //   `Snapshot.rage` 直接存的是存储值 ⇒ 这里统一 /10 之后再跟 TP 的 10/30/60/75 比。
    //   ⚠️ 不直接改 `Snapshot.rage` 本身：legacy `TickWarrior` 也读它，一动就把 mode=0 的行为
    //      一起改了（何况它自己就是按 1/10 在比：`hs_rage=75` 实际只有 7.5 怒就泄英勇打击，
    //      这就是它 40 秒刷 1000+ 次英勇、致死几乎放不出来的原因）。
    //   实测印证（本修正前，mode=5 60 秒单目标）：fury 只有 1 次嗜血、78 次英勇打击、
    //   怒气常年顶在 205（=20.5 真实怒气）；修正后见实测表。
    int RageReal(Snapshot& s)
    {
        return s.rage / 10;
    }

    // --------------------------------------------------------------------------
    // 战士：Idle → Recover（怒气 < enter，回到 exit）→ Aoe（pack10 + 近身）
    //       → Burst（仇恨 Open + 间隔到）→ Execute（目标血 <= execute_hp）→ Approach → Maintain。
    // --------------------------------------------------------------------------
    State EvalWarrior(BotMem* m, Snapshot& s, ThreatLevel threat)
    {
        if (!s.target)
            return State::Idle;

        // 怒气滞回：没滞回时会在 10 怒这条线上反复进出 Recover（每 tick 抖一次）
        //   ⚠️ 单位：RageReal() 是真实怒气，TP 的 10/30 也是真实怒气（见 RageReal 的说明）
        int const rageNow = RageReal(s);
        int const rageEnter = int(TP(m, "rage_enter", 10.0));
        int const rageExit = int(TP(m, "rage_exit", 30.0));
        bool const alreadyRecover = (m->state == State::Recover);
        if (alreadyRecover ? (rageNow < rageExit) : (rageNow < rageEnter))
            return State::Recover;

        float const meleeRange = float(TP(m, "range_melee", 6.5));
        if (s.pack10 >= AoEPack(m) && s.dist <= meleeRange)
            return State::Aoe;

        // 爆发 = 仇恨放开 + 间隔到（早用早 CD）。窗口内保持 Burst，
        //   让"饰品 → 死亡之愿 → 鲁莽 → 核心"跨几个 tick 依次放完（一个 GCD 只能放一个）。
        time_t const now = time(nullptr);
        if (now < m->burstUntil)
            return State::Burst;
        if (threat == ThreatLevel::Open
            && now - m->lastBurst >= time_t(TP(m, "burst_interval", 120.0)))
            return State::Burst;

        if (s.targetHp <= float(TP(m, "execute_hp", 20.0)))
            return State::Execute;

        if (s.dist > meleeRange)
            return State::Approach;

        return State::Maintain;
    }

    // Maintain 动作表（顺序，命中即停）：核心（武器=致死 / 狂暴=嗜血）→ 旋风斩 → 压制 → 猛击(*)
    //   → 英勇打击（怒气 >= `rage_reserve` 才泄怒；它是"下一次挥击附带"，用 TrySwing 防重复挂）。
    //   (*) 猛击默认关：`use_slam`（默认 0）—— 见下面的实测注释。
    ai::dpsfsm::TickResult TickWarriorMaintain(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        using ai::dpsfsm::TickResult;

        bool const fury = (m->spec == Spec::Fury);
        char const* core = fury ? "bloodthirst" : "mortal strike";

        if (CastFirst(ai, m, s, { core }))
            return TickResult::Acted;
        if (CastFirst(ai, m, s, { "whirlwind" }))
            return TickResult::Acted;
        if (CastFirst(ai, m, s, { "overpower" }))
            return TickResult::Acted;
        // [2026-10-31 站长口径] 猛击默认**不读**（`use_slam` 默认 0）：实测它的 1.5 秒读条会把白字压掉
        //   （melee 40 → 63、每轮还要放 8~14 次），读条期间不能挥击 ⇒ 白字和怒气两头亏。
        //   代码路径保留（`use_slam=1` 可开）就是为了 A/B —— 别把这条删了，否则拿不出对照。
        if (TP(m, "use_slam", 0.0) >= 1.0 && CastFirst(ai, m, s, { "slam" }))
            return TickResult::Acted;
        // 英勇打击会顶掉这次普攻（等于用普攻的怒换伤害）⇒ 只有怒气高出保留线才泄
        //   （真实怒气 vs `rage_reserve`，见 RageReal 的说明）
        //   [2026-10-31 站长口径] 保留线默认 75 → **45**：上一轮 A/B 落在噪声里（方向是"敢花"），
        //   怒气压着不用就是手上没有伤害，所以照方向把默认值调下来。
        if (RageReal(s) >= int(TP(m, "rage_reserve", 45.0)) && TrySwing(ai, m, s, "heroic strike"))
            return TickResult::Acted;

        return TickResult::Decline;
    }

    ai::dpsfsm::TickResult TickWarriorFsm(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        using ai::dpsfsm::TickResult;

        if (m->spec == Spec::Protection)
            return TickResult::Decline;     // 防御战不归 DPS 状态机

        // 读条中（猛击有 1.5 秒读条）：一个技能都不插，否则会把这一发顶掉
        if (s.casting)
        {
            MeleeLogTick(m, s, m->state);
            return TickResult::Acted;
        }

        // [2026-10-31 站长口径] 近战**自己负责朝向**：Hold 时引擎整 tick 都不跑，
        //   而引擎的 AttackAction/ChaseAction 正是每 tick 把 bot 转向目标的那个地方。
        //   状态机一 Hold，朝向就冻结 ⇒ `Unit::AttackSwing()` 每发都判
        //   SWING_ERROR_BAD_FACING，只把攻击计时器重置成 100ms、**既不产生伤害也不产生怒气**
        //   ⇒ 战士永远攒不到怒、永远出不了 Recover（实测 40 秒 dmg=594、melee=1、swing 恒 100ms、
        //   rage 只从 0 爬到 26）。所以这里自己转一次，和 CastChecked 里对施法目标转一次同理。
        if (s.target && s.target != s.bot)
            s.bot->SetFacingToObject(s.target);

        // 仇恨档位本 tick 只算一次（诊断日志在 EvalThreat 内部按 10 秒节流）
        ThreatLevel const threat = EvalThreat(m, s);
        State const st = EvalWarrior(m, s, threat);

        // 进入 Burst 的那一刻打点（不是每 tick 重置）：窗口才能跨 tick 放完整套爆发
        if (st == State::Burst && m->state != State::Burst)
        {
            m->lastBurst = time(nullptr);
            m->burstUntil = m->lastBurst + time_t(TP(m, "burst_window", 6.0));
        }
        m->state = st;

        // 狂暴战必须在**狂暴姿态**：旋风斩只要狂暴姿态，实测默认战斗姿态时它整场 0 次
        //   （见 legacy TickWarrior 的注释）。只占一个 GCD，开场切一次就一直保持。
        if ((m->spec == Spec::Fury) && s.bot->GetShapeshiftForm() != FORM_BERSERKERSTANCE
            && (st == State::Aoe || st == State::Burst || st == State::Execute || st == State::Maintain))
        {
            if (TryCastSelf(ai, m, s, "berserker stance"))
            {
                MeleeLogTick(m, s, st);
                return TickResult::Acted;
            }
        }

        TickResult result = TickResult::Decline;

        // 找背：只放走位、不占 GCD，但这一 tick 已经交给移动 ⇒ 不再抢技能
        bool const wantBehind = (st == State::Aoe || st == State::Burst
            || st == State::Execute || st == State::Maintain);
        if (wantBehind && NeedsBehind(s.bot) && GetBehind(ai, m, s))
        {
            result = TickResult::Acted;
        }
        else switch (st)
        {
            case State::Recover:
                // 等怒气：什么都不放，而且**不让引擎插**（否则它会把怒气换成自己的技能）。
                //   唯一例外是"没怒又够不着"：那就先让引擎把位置带过去，否则永远空等（0 怒 + 远距离 = 死锁）。
                //   [2026-11-01 站长口径] 登记为有意等资源（战士每 tick 都 `SetFacingToObject`，
                //   Hold 期间朝向不会冻住）；这一 tick 引擎插一手只会把怒气花在低价值技能上。
                if (s.dist > float(TP(m, "range_melee", 6.5)))
                    result = TickResult::Decline;
                else
                    result = HoldWait(m, "rage-wait");
                break;

            case State::Aoe:
                // 横扫（战斗姿态）→ 旋风斩（狂暴姿态，武器战放不出就往下落）→ 顺劈（怒气够才用）
                if (CastFirst(ai, m, s, { "sweeping strikes" }))
                    result = TickResult::Acted;
                else if (CastFirst(ai, m, s, { "whirlwind" }))
                    result = TickResult::Acted;
                else if (RageReal(s) >= int(TP(m, "cleave_rage", 60.0)) && TrySwing(ai, m, s, "cleave"))
                    result = TickResult::Acted;
                else
                    result = TickWarriorMaintain(ai, m, s);
                break;

            case State::Burst:
                // 饰品 → 死亡之愿（狂暴）→ 鲁莽（学会了才放；要狂暴姿态，放不出就落下一手）
                //   → 核心（武器=致死 / 狂暴=嗜血）→ 都放完就回 Maintain 继续输出
                if (UseTrinket(ai, m, s))
                    result = TickResult::Acted;
                else if (m->spec == Spec::Fury && CastFirst(ai, m, s, { "death wish" }))
                    result = TickResult::Acted;
                else if (MeleeLearned(ai, s.bot, m, "recklessness")
                    && CastFirst(ai, m, s, { "recklessness" }))
                    result = TickResult::Acted;
                else if (CastFirst(ai, m, s, { m->spec == Spec::Fury ? "bloodthirst" : "mortal strike" }))
                    result = TickResult::Acted;
                else
                    result = TickWarriorMaintain(ai, m, s);
                break;

            case State::Execute:
                // 低血段：先打斩杀（核心对斩杀有 TargetAuraState=2 的门，只有目标 <20% 血才成立）。
                //   放不出（怒气不够）就交回引擎 —— 这个窗口里别再拿怒气去换非斩杀技能。
                if (CastFirst(ai, m, s, { "execute" }))
                    result = TickResult::Acted;
                else
                    result = TickResult::Decline;
                break;

            case State::Approach:
                result = TickResult::Decline;   // 走位归引擎
                break;

            case State::Maintain:
                result = TickWarriorMaintain(ai, m, s);
                break;

            case State::Idle:
            default:
                result = TickResult::Decline;
                break;
        }

        MeleeLogTick(m, s, st);
        return result;
    }

    // --------------------------------------------------------------------------
    // 盗贼：Idle → Recover（能量 < enter，回到 exit）→ Aoe（pack10）→ Burst（仇恨 Open + 间隔到）
    //       → Approach（够不着）→ Maintain。
    //   TBC 盗贼没有真正的群伤（刀扇是 WLK）⇒ Aoe 就是把剑刃乱舞/冲动这两张 CD 技顶到最前，
    //   之后完全复用单体表（与 legacy 同一口径）。
    // --------------------------------------------------------------------------
    State EvalRogue(BotMem* m, Snapshot& s, ThreatLevel threat)
    {
        if (!s.target)
            return State::Idle;

        int const energyEnter = int(TP(m, "energy_enter", 20.0));
        int const energyExit = int(TP(m, "energy_exit", 40.0));
        bool const alreadyRecover = (m->state == State::Recover);
        if (alreadyRecover ? (s.energy < energyExit) : (s.energy < energyEnter))
            return State::Recover;

        if (s.pack10 >= AoEPack(m))
            return State::Aoe;

        time_t const now = time(nullptr);
        if (now < m->burstUntil)
            return State::Burst;
        if (threat == ThreatLevel::Open
            && now - m->lastBurst >= time_t(TP(m, "burst_interval", 120.0)))
            return State::Burst;

        if (s.dist > float(TP(m, "range_melee", 6.5)))
            return State::Approach;

        return State::Maintain;
    }

    // Maintain 动作表（顺序，命中即停）：
    //   ① 切割（剩余 < snd_refresh 且星够 snd_cp）→ ② 满星且能量 >= fin_energy 放终结技
    //   → ③ 满星但能量不够 ⇒ Hold（等能量、不攒星）→ ④ 攒星：刺杀毁伤 / 其他邪恶攻击。
    ai::dpsfsm::TickResult TickRogueMaintain(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        using ai::dpsfsm::TickResult;

        bool const assassination = (m->spec == Spec::Assassination);

        // ① 切割常驻：单位目标必须是**当前敌人** —— 核心对连击点法术要求
        //   unitTarget == GetComboTargetGuid()，对自己放会判 NO_COMBO_POINTS(73)（legacy 实测切割整场 0 次）。
        int const sndRefreshMs = int(TP(m, "snd_refresh", 3.0) * 1000.0);
        int const sndCp = int(TP(m, "snd_cp", 4.0));
        if (TryTable(ai, m, s, {
                { "slice and dice", nullptr, nullptr, sndRefreshMs, nullptr, sndCp, 0 } }))
            return TickResult::Acted;

        int const finCp = int(TP(m, "fin_cp", 5.0));
        int const finEnergy = int(TP(m, "fin_energy", 45.0));

        // ② 星星满了：能量够就立刻泄；不够就**有意空着**（上限 5 星，再攒全浪费）
        if (s.combo >= finCp)
        {
            if (s.energy >= finEnergy)
            {
                // 刺杀毒伤：核心要求目标身上有中毒状态（TargetAuraState=16），假人/没上毒的怪永远不成立
                //   （legacy 实测 res=106）⇒ 放不出就剔骨兜底，别让满星烂在手里。
                if (assassination ? CastFirst(ai, m, s, { "envenom", "eviscerate" })
                                  : CastFirst(ai, m, s, { "eviscerate" }))
                    return TickResult::Acted;
                return TickResult::Decline;     // 不在射程/条件不满足 ⇒ 交回引擎走位
            }
            return HoldWait(m, "combo-energy-wait");
        }

        // ③ 攒星：毁伤要求匕首 + 背后（随机 bot 经常不满足）⇒ 一定留邪恶攻击兜底，
        //   否则整场 0 施法（legacy 实测 assassination 40 秒 casts=0）。
        if (assassination ? CastFirst(ai, m, s, { "mutilate", "sinister strike" })
                          : CastFirst(ai, m, s, { "sinister strike" }))
            return TickResult::Acted;

        return TickResult::Decline;
    }

    ai::dpsfsm::TickResult TickRogueFsm(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        using ai::dpsfsm::TickResult;

        if (s.casting)
        {
            MeleeLogTick(m, s, m->state);
            return TickResult::Acted;
        }

        // 朝向同上（战士那段注释）：Hold 时引擎不跑，没人替 bot 转向，普攻会全部 BAD_FACING
        if (s.target && s.target != s.bot)
            s.bot->SetFacingToObject(s.target);

        ThreatLevel const threat = EvalThreat(m, s);
        State const st = EvalRogue(m, s, threat);

        if (st == State::Burst && m->state != State::Burst)
        {
            m->lastBurst = time(nullptr);
            m->burstUntil = m->lastBurst + time_t(TP(m, "burst_window", 6.0));
        }
        m->state = st;

        TickResult result = TickResult::Decline;

        bool const wantBehind = (st == State::Aoe || st == State::Burst || st == State::Maintain);
        if (wantBehind && NeedsBehind(s.bot) && GetBehind(ai, m, s))
        {
            result = TickResult::Acted;
        }
        else switch (st)
        {
            case State::Recover:
                // 等能量：什么都不放、也不让引擎插（同战士的 Recover）；
                //   够不着时例外（让引擎把位置带过去，别空等）。
                //   [2026-11-01 站长口径] 登记为有意等资源（盗贼 tick 开头已 `SetFacingToObject`）。
                if (s.dist > float(TP(m, "range_melee", 6.5)))
                    result = TickResult::Decline;
                else
                    result = HoldWait(m, "energy-wait");
                break;

            case State::Aoe:
                // 剑刃乱舞 → 冲动 → 落回单体表（TBC 盗贼"群伤"就这两张 CD 技）
                if (CastFirst(ai, m, s, { "blade flurry" }))
                    result = TickResult::Acted;
                else if (CastFirst(ai, m, s, { "adrenaline rush" }))
                    result = TickResult::Acted;
                else
                    result = TickRogueMaintain(ai, m, s);
                break;

            case State::Burst:
                // 饰品 → 冷血（刺杀）/ 冲动 → 剑刃乱舞 → 落回单体表
                if (UseTrinket(ai, m, s))
                    result = TickResult::Acted;
                else if (m->spec == Spec::Assassination
                    ? CastFirst(ai, m, s, { "cold blood" })
                    : CastFirst(ai, m, s, { "adrenaline rush" }))
                    result = TickResult::Acted;
                else if (CastFirst(ai, m, s, { "blade flurry" }))
                    result = TickResult::Acted;
                else
                    result = TickRogueMaintain(ai, m, s);
                break;

            case State::Approach:
                result = TickResult::Decline;   // 走位归引擎
                break;

            case State::Maintain:
                result = TickRogueMaintain(ai, m, s);
                break;

            case State::Idle:
            default:
                result = TickResult::Decline;
                break;
        }

        MeleeLogTick(m, s, st);
        return result;
    }

    // ==========================================================================
    // [2026-11-01 站长口径] 术士 · 毁灭（Destruction）—— mode=5 的第五张职业图
    //   毁灭是三系里最简单的一系：**主输出只有暗影箭**（shadow bolt），DoT / 燃烧只是
    //   "这一手比暗影箭更值"时的补充。Affliction / Demonology **不接**：
    //   它们继续走 legacy `TickWarlock`（`Tick()` 里不进这张图 ⇒ 行为一个字节都不动）。
    //
    //   口径（2026-11-01 站长，逐条对应）：
    //     ① 诅咒位：目标身上没有任何诅咒 ⇒ 元素诅咒（法系团体收益，比痛苦诅咒更值）；
    //     ② DoT 只在"每 GCD 预期收益 > 暗影箭"时才上 —— 先用**简化判据**：
    //        目标预期存活 > dot_min_lifetime（默认 8 秒）才值得补腐蚀/献祭；
    //     ③ 生命分流（life tap）= **资源填充**：血 > lifetap_hp(55) 且蓝缺口 > lifetap_deficit(2000)
    //        时，用一个 GCD 换蓝；这一手只在"蓝真的不够继续读暗影箭"时抢，不提前换
    //        （提前换 = 白丢一个 GCD，实测直接把 dps 打下来）；
    //     ④ 仇恨**贴着上限跑**（不是压低）：threat_target_ratio(0.95) 以内照打，
    //        越线才收高威胁牌（Throttle 效果）；进爆发前先**预判**：
    //        myThreat + 预计爆发仇恨 > 坦克 ⇒ 先交灵魂碎裂（soul shatter）再爆发；
    //     ⑤ 灵魂碎裂每场战斗只能交一次（长 CD），且战斗时长必须 ≥ shatter_min_engage_ms(20s)：
    //        开局就交等于把牌打空，真正需要的时候反而不在手上。
    //
    //   可复用的现成口径（不重新发明）：`EvalThreat` 的三档、`CastFirst`/`TryCastSelf`/`Ready`、
    //   `AffordSpell`（SpellEntry->manaCost 与当前法力绝对值比）、`SummonPetIfNeeded`/`CommandPetAttack`、
    //   `AuraStacks`（任意 rank 的 debuff 叠层）、`UseTrinket`/`UseManaPotion`。
    //   `FillCand`/`PickFill` 是**按 GCD 窗口排程**的填充筛选器（奥法 3+3 专用，窗口 = 叠层 debuff 剩余）；
    //   毁灭是按固定优先级出手，没有"窗口放得下什么"的问题 ⇒ 不复用，避免把两套语义缝在一起。
    // ==========================================================================

    // [2026-11-01 站长口径] 仇恨线：术士是"贴着上限跑"，不是"离坦克远一点"。
    //   EvalThreat 的通用 Throttle 档（threat_throttle 默认 0.80）是法师的"高威胁牌"线，
    //   对术士过保守（0.80 就收手 = 白让出 15% 的仇恨空间）⇒ 术士真正要收手的位置单独由
    //   threat_target_ratio（默认 0.95）决定。
    //   假人 / 单人 / 状态机独占（SkipEngine）⇒ 没有坦克就没有"上限"可贴，恒放开。
    bool WarlockThreatOk(BotMem* m, Snapshot& s)
    {
        if (dpsfsm::SkipEngine(s.bot->GetGUIDLow()) || !s.tankAlive || s.tankThreat <= 0.0f)
            return true;
        return s.myThreat <= s.tankThreat * float(TP(m, "threat_target_ratio", 0.95));
    }

    // [2026-11-01 站长口径] "目标预期存活"的简化估计（DoT 判据的输入）：
    //   用**同一目标**最近一次观测的血量斜率推"还要几秒打完"。
    //   为什么要它：DoT 的收益 = 还能跳几下；目标马上要死时补腐蚀等于白丢一个 GCD
    //   （直接暗影箭就打死了）。返回值：<0 = 还没有斜率（开局第一 tick，按"还很久"处理）；
    //   9999 = 血量没在掉（假人/坦克在扛），视为长命目标。
    int WarlockDotLifetimeSec(BotMem* m, Snapshot& s)
    {
        if (!s.target)
            return 0;
        uint32 const nowMs = WorldTimer::getMSTime();
        uint32 const safeNow = nowMs ? nowMs : 1;
        ObjectGuid const guid = s.target->GetObjectGuid();
        if (m->dotTarget != guid || !m->dotPrevAt)
        {
            m->dotTarget = guid;
            m->dotPrevHp = s.targetHp;
            m->dotPrevAt = safeNow;
            m->dotLastLifeSec = -1;
            return -1;
        }
        uint32 const dt = WorldTimer::getMSTimeDiff(m->dotPrevAt, safeNow);
        if (dt < 1000)
            return m->dotLastLifeSec;             // 1 秒采一次，别每 tick 都算斜率
        float const dropped = m->dotPrevHp - s.targetHp;
        m->dotPrevHp = s.targetHp;
        m->dotPrevAt = safeNow;
        if (dropped <= 0.01f)
        {
            m->dotLastLifeSec = 9999;             // 血没掉（被扛着/刚上手）⇒ 长命
            return m->dotLastLifeSec;
        }
        float const perSec = dropped * 1000.0f / float(dt);      // 每秒掉多少 %
        float const life = s.targetHp / std::max(0.01f, perSec);
        m->dotLastLifeSec = int(std::min(9999.0f, life));
        return m->dotLastLifeSec;
    }

    // [2026-11-01 站长口径] 状态转移只看 Snapshot + TP，不读世界、不打日志：
    //   Idle → Recover(蓝见底/该换蓝) → Aoe(群架) → Burst(仇恨有量 && 间隔到) → Approach(射程) → Maintain
    //   两处与口述顺序一致但**有意**保留的细节：
    //     · Prep（护甲/宠物）放在最前：这是"开打前"的准备，缺了它整场都少一块输出；
    //     · Approach 放在 Burst 之前：出射程时先走位，否则爆发牌全打在 OUT_OF_RANGE 上，白丢 GCD。
    State EvalWarlock(PlayerbotAI* ai, BotMem* m, Snapshot& s, ThreatLevel threat)
    {
        if (!s.target)
            return State::Idle;

        // ⓪ 开打前准备：恶魔护甲（fel armor，没学会就不算"缺"）或宠物不在场
        bool const needArmor = SpellId(ai, s.bot, m, "fel armor", false)
            && AuraRemain(ai, m, s, "fel armor") == 0;
        bool const needPet = !PetPresent(s);
        if (needArmor || needPet)
            return State::Prep;

        // ① 资源：蓝见底 ⇒ Recover（life tap 是资源填充，见 Tick 的动作表）
        if (s.power < float(TP(m, "recover_power", 12.0)))
            return State::Recover;

        // ② 群架
        if (s.pack30 >= AoEPack(m))
            return State::Aoe;

        // ③ 射程：先到位再谈爆发
        if (s.dist > float(TP(m, "range_caster", 30.0)))
            return State::Approach;

        // ④ 爆发：仇恨还有余量（贴着 0.95 上限跑）+ burst_interval 到
        //    EvalThreat 的 Wait（≥95% 坦克）是硬线；Throttle 档不拦爆发 —— 术士的收手线由
        //    threat_target_ratio 决定（这就是"贴着上限跑"的含义）。
        time_t const now = time(nullptr);
        if (now < m->burstUntil)
            return State::Burst;
        if (threat != ThreatLevel::Wait && WarlockThreatOk(m, s)
            && now - m->lastBurst >= time_t(TP(m, "burst_interval", 120.0)))
            return State::Burst;

        return State::Maintain;
    }

    // ==========================================================================
    // [2026-11-01 站长口径] 术士 · 痛苦（Affliction）+ 恶魔（Demonology）
    //   两系**复用毁灭的骨架**（同一份 TickWarlockFsmCore / 同一套 TP 键 / 同一条日志前缀），
    //   只把"单体输出表"换成 DoT 续期表、把群架档换个打法：
    //
    //   痛苦：**没有心爆式锚点 ⇒ 不需要时间轮**
    //     DoT 到期续期（剩余 < refresh_dot 就补）+ 剩余 GCD 用 drain life / shadow bolt 填。
    //     顺序：诅咒位（无诅咒 ⇒ 元素诅咒）→ 腐蚀 → 吸血（siphon life）→ 痛苦无常（UA）→ 填充。
    //     每条 DoT 都要过"每 GCD 预期收益 > 填充技"的简化判据：目标预期存活 > dot_min_lifetime(8s)
    //     （存活用血量斜率估，读不到就当长命）。生命分流 = 资源填充。
    //     多目标：先给 30 码内**血量最多的前 multidot_max(5) 个**都铺上腐蚀，再谈单体；超过 5 个不扩。
    //
    //   恶魔：复杂度介于毁灭与痛苦之间
    //     Prep：恶魔护甲（缺则补）+ 召唤恶魔卫士（未学会退小鬼）—— 与毁灭共用同一段 Prep；
    //     Maintain：诅咒位 → 腐蚀 → 暗影箭填充；宠物持续咬当前目标（CommandPetAttack）；
    //     爆发：饰品 + 恶魔牺牲（学会了、且宠物不是刚召唤的）或直接暗影箭；回蓝同样靠生命分流。
    // ==========================================================================

    // [DPSFSM-WARLOCK] 的 spec= 标签（验收脚本按它切日志）。
    char const* WarlockSpecTag(Spec sp)
    {
        switch (sp)
        {
            case Spec::Affliction:  return "aff";
            case Spec::Demonology:  return "demo";
            case Spec::Destruction: return "destro";
            default:                return "?";
        }
    }

    // 目标身上"术士这一套"的 DoT/诅咒已挂几个（只给日志 dots= 用，不参与任何决策）。
    int WarlockDotCount(PlayerbotAI* ai, Player* bot, BotMem* m, Unit* u)
    {
        if (!u)
            return 0;
        static char const* kDots[] = {
            "corruption", "siphon life", "unstable affliction", "immolate",
            "curse of the elements", "curse of shadows", "curse of agony", "curse of doom",
        };
        int n = 0;
        for (char const* d : kDots)
            if (AuraStacks(ai, bot, m, d, u) > 0)
                ++n;
        return n;
    }

    // [2026-11-01 站长口径] 痛苦·多目标：`NearbyEnemy` 里**血量最多的前 multidot_max(5) 个**才铺，
    //   超过 5 个不扩。取列表用的是快照侧已有的 `possible targets`，与 CountHostilesNear() 同一份过滤
    //   （敌对 + 存活 + 射程内 + 假人场景只数自己这一轮声明的目标），所以副本里也用同一套"看得见的敌人"。
    //   排序用**当前绝对血量**降序：血最多的死得最慢 ⇒ 铺上去的腐蚀跳得最多。
    //   返回 true = 本 tick 真的铺出一颗（占掉 GCD）；outCount = 参与铺场的敌人数（日志 multi=）。
    bool WarlockMultiCorruption(PlayerbotAI* ai, BotMem* m, Snapshot& s, int& outCount)
    {
        outCount = 1;
        int const cap = int(TP(m, "multidot_max", 5.0));
        int const minTargets = int(TP(m, "multidot_min_targets", 2.0));
        if (cap <= 1 || minTargets <= 1 || s.pack30 < minTargets)
            return false;

        std::vector<Unit*> list;
        CountHostilesNear(ai, s.bot, float(TP(m, "range_caster", 30.0)), &list);
        if (int(list.size()) < minTargets)
            return false;
        std::sort(list.begin(), list.end(), [](Unit* a, Unit* b)
        {
            if (a->GetHealth() != b->GetHealth())
                return a->GetHealth() > b->GetHealth();
            return a->GetObjectGuid() < b->GetObjectGuid();
        });
        if (int(list.size()) > cap)
            list.resize(size_t(cap));
        outCount = int(list.size());

        // 每个目标只铺一颗：残留 > refresh_dot 的不动（同一目标重复铺会顶掉上一颗 ⇒ 白读一个 GCD）
        int const refreshMs = int(TP(m, "refresh_dot", 3000.0));
        for (Unit* u : list)
        {
            if (TargetAuraRemain(ai, s.bot, m, "corruption", u) > refreshMs)
                continue;
            if (TryCast(ai, m, s, "corruption", u))
                return true;
        }
        return false;
    }

    // [2026-11-01 站长口径] 恶魔牺牲（demonic sacrifice 18788）的"能不能交"判据：
    //   ① 学会了（SpellId 为 0 就静默跳过，不打"没有可用技能"的错日志）；
    //   ② 增益还没在身上（重复交只是白杀一只宠物）；
    //   ③ 宠物在场，而且**不是刚召唤的**（demo_pet_min_ms 默认 8 秒）—— 刚召出来就牺牲等于把
    //      召唤那个 GCD 白扔；④ 开关 demo_sacrifice（默认 1）便于 A/B。
    //   ⚠️ 这个法术的目标是**自己的恶魔**（对着自己放会 BAD_TARGETS）⇒ 必须走 TryCastPet()。
    bool WarlockCanSacrifice(PlayerbotAI* ai, BotMem* m, Snapshot& s, uint32 safeNow)
    {
        if (TP(m, "demo_sacrifice", 1.0) < 1.0)
            return false;
        if (!SpellId(ai, s.bot, m, "demonic sacrifice", false))
            return false;
        if (AuraStacks(ai, s.bot, m, "demonic sacrifice", s.bot) > 0)
            return false;
        if (!PetPresent(s) || !m->demoPetAt)
            return false;
        return WorldTimer::getMSTimeDiff(m->demoPetAt, safeNow)
            >= uint32(TP(m, "demo_pet_min_ms", 8000.0));
    }

    // [2026-11-01 站长口径] 痛苦 / 恶魔的状态转移（骨架与 EvalWarlock 同构，只差两处）：
    //   ① 痛苦的**群架不单开 Aoe 档**：多目标 = "多铺腐蚀"，落在 Maintain 里按血量前 5 铺，
    //      铺满自然回单体表 ⇒ 状态表上少一档、少一处会漂移的分支；
    //   ② 恶魔照旧用 Aoe（种子 + 暗影箭）：它的重心在宠物与暗影箭，不是铺 DoT。
    //   其余（Prep 的护甲/宠物、Recover 的换蓝、Burst 的碎裂预判）与毁灭共用同一份判据与 TP 键。
    State EvalWarlockDots(PlayerbotAI* ai, BotMem* m, Snapshot& s, ThreatLevel threat)
    {
        if (!s.target)
            return State::Idle;

        // ⓪ 开打前准备：恶魔护甲（没学会就不算"缺"）或宠物不在场
        bool const needArmor = SpellId(ai, s.bot, m, "fel armor", false)
            && AuraRemain(ai, m, s, "fel armor") == 0;
        bool const needPet = !PetPresent(s);
        if (needArmor || needPet)
            return State::Prep;

        // ① 资源：蓝见底 ⇒ Recover（life tap 是资源填充，见动作表）
        if (s.power < float(TP(m, "recover_power", 12.0)))
            return State::Recover;

        // ② 群架（只有恶魔单开这一档，理由见函数头）
        if (m->spec == Spec::Demonology && s.pack30 >= AoEPack(m))
            return State::Aoe;

        // ③ 射程：先到位再谈爆发
        if (s.dist > float(TP(m, "range_caster", 30.0)))
            return State::Approach;

        // ④ 爆发：与毁灭同一份判据（仇恨贴着 0.95 上限跑 + burst_interval 到）
        time_t const now = time(nullptr);
        if (now < m->burstUntil)
            return State::Burst;
        if (threat != ThreatLevel::Wait && WarlockThreatOk(m, s)
            && now - m->lastBurst >= time_t(TP(m, "burst_interval", 120.0)))
            return State::Burst;

        return State::Maintain;
    }

    // [2026-11-01 站长口径] 痛苦 / 恶魔的**单体输出表**（两系共用一份骨架，只差 DoT 清单与填充技）：
    //   痛苦：诅咒位 → 腐蚀 → 吸血 → 痛苦无常 → drain life 填充（shadow bolt 兜底）；
    //   恶魔：诅咒位 → 腐蚀 → 暗影箭填充。
    //   每条 DoT 走**同一个续期判据**：`TargetAuraRemain() < refresh_dot(3000ms)` 就补，
    //   不是"没挂才补"—— 这才是站长要的"到期续期"，旧口径会让 DoT 在到期的那一刻才想起来补。
    // [2026-11-01 实测修正] 诅咒位：用 TargetAuraRemain（任意 rank）判"有没有"，
    //   旧的 `AuraStacks()==0` 在"挂上了但读不到层数"时会每 tick 重放诅咒。
    //   另加保护：连续 2 次放出去仍读不到 ⇒ 退避 30 秒，避免整场空转。
    bool WarlockCurseTick(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        if (!s.target)
            return false;
        uint32 const nowMs = WorldTimer::getMSTime();
        if (m->wlCurseSkipUntil && nowMs < m->wlCurseSkipUntil)
            return false;
        bool const cursed =
            TargetAuraRemain(ai, s.bot, m, "curse of the elements", s.target) > 0 ||
            TargetAuraRemain(ai, s.bot, m, "curse of shadows", s.target) > 0 ||
            TargetAuraRemain(ai, s.bot, m, "curse of agony", s.target) > 0 ||
            TargetAuraRemain(ai, s.bot, m, "curse of doom", s.target) > 0;
        if (cursed)
        {
            m->wlCurseTries = 0;
            return false;
        }
        if (!TryCast(ai, m, s, "curse of the elements", nullptr))
            return false;
        if (++m->wlCurseTries >= 2)
        {
            m->wlCurseSkipUntil = nowMs + 30000;
            m->wlCurseTries = 0;
            sLog.outError("[DPSFSM-WARLOCK] %s 连续 2 次放诅咒仍读不到目标 debuff ⇒ 退避 30s",
                s.bot->GetName());
        }
        return true;
    }
    ai::dpsfsm::TickResult TickWarlockDotsMaintain(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        using ai::dpsfsm::TickResult;

        // 宠物：掉队/换目标就重新下令（宠物仇恨不算在 bot 头上）
        CommandPetAttack(s);

        bool const aff = (m->spec == Spec::Affliction);

        // ① 诅咒位：目标身上没有任何诅咒 ⇒ 元素诅咒（判据与退避见 WarlockCurseTick）
        if (WarlockCurseTick(ai, m, s))
            return TickResult::Acted;

        // ② 多目标（痛苦）：先把血量最多的前 multidot_max 个都铺上腐蚀，再谈单体；
        //    multi 只用于日志（铺满之后仍报"这一场有几个目标"）。
        m->wlMulti = 1;
        if (aff)
        {
            int multi = 1;
            bool const spread = WarlockMultiCorruption(ai, m, s, multi);
            m->wlMulti = multi;
            if (spread)
            {
                m->wlDots = WarlockDotCount(ai, s.bot, m, s.target);
                return TickResult::Acted;
            }
        }

        // ③ DoT 续期：只有"目标预期存活 > dot_min_lifetime"才值得花一个 GCD
        int const lifeSec = WarlockDotLifetimeSec(m, s);
        bool const dotWorth = (lifeSec < 0) || (float(lifeSec) > float(TP(m, "dot_min_lifetime", 8.0)));
        int const refreshMs = int(TP(m, "refresh_dot", 3000.0));

        if (dotWorth)
        {
            struct DotEntry { char const* name; bool affOnly; };
            static DotEntry const kDots[] = {
                { "corruption",          false },   // 两系都要
                { "siphon life",         true  },   // 痛苦专属
                { "unstable affliction", true  },   // 痛苦专属
            };
            for (DotEntry const& d : kDots)
            {
                if (d.affOnly && !aff)
                    continue;
                if (!SpellId(ai, s.bot, m, d.name, false))
                    continue;                     // 没学会（天赋没点）：跳过，不刷"没有可用技能"
                if (TargetAuraRemain(ai, s.bot, m, d.name, s.target) > refreshMs)
                    continue;
                if (TryCast(ai, m, s, d.name, nullptr))
                {
                    m->wlDots = WarlockDotCount(ai, s.bot, m, s.target);
                    return TickResult::Acted;
                }
            }
        }

        // ④ 资源填充：填充技都放不起时，用一个 GCD 换蓝（血够 + 蓝缺口 > 阈值）
        char const* fill = aff ? "drain life" : "shadow bolt";
        if (!AffordSpell(ai, m, s, fill)
            && m->wlManaGap > float(TP(m, "lifetap_deficit", 2000.0))
            && s.selfHp > float(TP(m, "lifetap_hp", 55.0))
            && TryCastSelf(ai, m, s, "life tap"))
            return TickResult::Acted;

        // ⑤ 填充：痛苦用吸取生命（引导，顺带回血），恶魔用暗影箭
        m->wlDots = WarlockDotCount(ai, s.bot, m, s.target);
        if (aff)
            return FsmResult(CastFirst(ai, m, s, { "drain life", "shadow bolt" }));
        return FsmResult(CastFirst(ai, m, s, { "shadow bolt" }));
    }

    // 单体输出表（全部施法前都过 Ready()/射程，由 CastChecked 统一判）。
    ai::dpsfsm::TickResult TickWarlockMaintain(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        using ai::dpsfsm::TickResult;

        // 宠物：掉队/换目标就重新下令（宠物仇恨不算在 bot 头上，Wait 档也让它继续咬）
        CommandPetAttack(s);

        bool const threatOk = WarlockThreatOk(m, s);

        // ① 诅咒位：目标身上没有任何诅咒才补（判据与退避见 WarlockCurseTick）
        if (WarlockCurseTick(ai, m, s))
            return TickResult::Acted;

        // ② DoT：只在"目标预期存活 > dot_min_lifetime"时才上（每 GCD 预期收益 > 暗影箭）
        int const lifeSec = WarlockDotLifetimeSec(m, s);
        bool const dotWorth = (lifeSec < 0) || (float(lifeSec) > float(TP(m, "dot_min_lifetime", 8.0)));
        if (dotWorth && AuraStacks(ai, s.bot, m, "corruption", s.target) == 0
            && TryCast(ai, m, s, "corruption", nullptr))
            return TickResult::Acted;
        // 献祭：学会了才放（没点天赋时 SpellId 为 0，静默跳过，不打"没有可用技能"的错日志）
        if (dotWorth && SpellId(ai, s.bot, m, "immolate", false)
            && AuraStacks(ai, s.bot, m, "immolate", s.target) == 0
            && TryCast(ai, m, s, "immolate", nullptr))
            return TickResult::Acted;

        // ③ 燃烧（conflagrate）：目标身上有献祭才值得，且是"高威胁牌"⇒ 仇恨越线时收起来
        if (threatOk && SpellId(ai, s.bot, m, "conflagrate", false)
            && AuraStacks(ai, s.bot, m, "immolate", s.target) > 0
            && TryCast(ai, m, s, "conflagrate", nullptr))
            return TickResult::Acted;

        // ④ 资源填充：暗影箭都放不起时，用一个 GCD 换蓝（血够 + 蓝缺口 > 阈值）
        if (!AffordSpell(ai, m, s, "shadow bolt")
            && m->wlManaGap > float(TP(m, "lifetap_deficit", 2000.0))
            && s.selfHp > float(TP(m, "lifetap_hp", 55.0))
            && TryCastSelf(ai, m, s, "life tap"))
            return TickResult::Acted;

        // ⑤ 主输出：暗影箭（唯一的常规填充）
        return FsmResult(CastFirst(ai, m, s, { "shadow bolt" }));
    }

    // [2026-11-01 站长口径] 术士三系共用**同一份骨架**（`dotSpec=false` = 毁灭，`true` = 痛苦/恶魔）：
    //   差别只有三处 —— ①Eval（EvalWarlock / EvalWarlockDots）；②群架档（种子 vs 多铺腐蚀）；
    //   ③单体表（TickWarlockMaintain / TickWarlockDotsMaintain）+ 恶魔爆发里的"恶魔牺牲"一张牌。
    //   毁灭（dotSpec=false）的每一次判断、每一个 TP 键、日志里的每个数值都与引入本次改动之前一致。
    ai::dpsfsm::TickResult TickWarlockFsmCore(PlayerbotAI* ai, BotMem* m, Snapshot& s, bool dotSpec)
    {
        using ai::dpsfsm::TickResult;

        // 读条/引导中一律不插技能（与 legacy `TickWarlock` 同口径：返回"我占着 GCD"）
        if (s.casting)
            return TickResult::Acted;

        uint32 const nowMs = WorldTimer::getMSTime();
        uint32 const safeNow = nowMs ? nowMs : 1;

        // 本 tick 的原始法力缺口（life tap 与日志共用同一份数）
        uint32 const maxMana = s.bot->GetMaxPower(POWER_MANA);
        m->wlManaGap = maxMana ? float(maxMana) - float(s.bot->GetPower(POWER_MANA)) : 0.0f;

        // [2026-11-01 站长口径] 恶魔牺牲的"不是刚召唤的"门：记宠物**第一次被看见在场**的时刻。
        //   不在场 ⇒ 归零（换目标/宠物死/被牺牲都会走到这里，重召之后从头计时）。
        if (PetPresent(s))
        {
            if (!m->demoPetAt)
                m->demoPetAt = safeNow;
        }
        else
        {
            m->demoPetAt = 0;
        }

        // 交战计时：有目标就开始计（换目标不重计，这是"这一场战斗"）；没目标 ⇒ 清场
        if (s.target)
        {
            if (!m->engagedAt)
                m->engagedAt = safeNow;
        }
        else
        {
            m->engagedAt = 0;
            m->shatterUsed = false;
        }

        ThreatLevel const threat = EvalThreat(m, s);
        State const st = dotSpec ? EvalWarlockDots(ai, m, s, threat) : EvalWarlock(ai, m, s, threat);

        // 进入 Burst 的那一刻打点（不是每 tick 重置）：窗口才能跨 tick 放完"碎裂 → 饰品 → 暗影灼烧"
        if (st == State::Burst && m->state != State::Burst)
        {
            m->lastBurst = time(nullptr);
            m->burstUntil = m->lastBurst + time_t(TP(m, "burst_window", 6.0));
        }
        m->state = st;

        TickResult result = TickResult::Decline;
        switch (st)
        {
            case State::Prep:
            {
                // [2026-11-01 站长口径] 开打前：恶魔护甲（无该光环时）+ 没有宠物就召。
                //   两样都做不了（没学会/CD 中）⇒ 不能卡在 Prep 空转，直接落回单体表继续打。
                bool acted = false;
                if (AuraRemain(ai, m, s, "fel armor") == 0 && TryCastSelf(ai, m, s, "fel armor"))
                    acted = true;
                // 召唤走统一入口：没有宠物才召，失败只留一条可区分的 [DPSFSM-PET] 诊断。
                // 主召恶魔守卫（毁灭天赋通常点不到 ⇒ 取不到 id），落回小鬼：末日守卫/地狱犬/魅魔
                // 都不是"点一下就常驻"的输出宠物，而小鬼是术士唯一"永远学会"的远程输出宠。
                if (!acted && !PetPresent(s))
                {
                    if (SummonPetIfNeeded(ai, m, s, "summon felguard"))
                        acted = true;
                    else if (SummonPetIfNeeded(ai, m, s, "summon imp"))
                        acted = true;
                }
                result = acted ? TickResult::Acted
                    : (dotSpec ? TickWarlockDotsMaintain(ai, m, s) : TickWarlockMaintain(ai, m, s));
                break;
            }

            case State::Recover:
                // [2026-11-01 站长口径] 生命分流 = 资源填充：血 > lifetap_hp 且蓝缺口 > lifetap_deficit
                //   ⇒ 用一个 GCD 换蓝；血不够时先喝法力药水（有就喝），都没有才 Hold 等回蓝。
                if (s.selfHp > float(TP(m, "lifetap_hp", 55.0))
                    && m->wlManaGap > float(TP(m, "lifetap_deficit", 2000.0))
                    && TryCastSelf(ai, m, s, "life tap"))
                    result = TickResult::Acted;
                else if (UseManaPotion(m, s))
                    result = TickResult::Acted;
                else if (s.power >= float(TP(m, "recover_power", 12.0)))
                    result = dotSpec ? TickWarlockDotsMaintain(ai, m, s) : TickWarlockMaintain(ai, m, s);   // 蓝还够打：别把 GCD 空成 0 DPS
                else
                    // [2026-11-01 站长口径] 登记为有意等资源：生命分流与药水都用不了、蓝血双低，
                    //   这一手放什么都是白花。注意 Hold 会把引擎整 tick 一起挡住，所以只在这一格
                    //   （Recover 的最后一档）登记；技能表正常时它下一 tick 就自己回到输出。
                    result = HoldWait(m, "mana-wait");
                break;

            case State::Aoe:
            {
                // [2026-11-01 站长口径] 群伤先做**单体版**：种子（每个目标只种一颗，重复种会顶掉
                //   上一颗 ⇒ 白读一个 GCD）+ 暗影箭兜底。
                CommandPetAttack(s);
                // [2026-11-01 站长口径] 痛苦进不了这一档（EvalWarlockDots 不给它 Aoe），这里只是防御：
                //   真的走到就把"前 multidot_max 个铺腐蚀"当群架，铺完用吸取生命填充。
                if (dotSpec && m->spec == Spec::Affliction)
                {
                    int multi = 1;
                    bool const spread = WarlockMultiCorruption(ai, m, s, multi);
                    m->wlMulti = multi;
                    if (spread)
                        result = TickResult::Acted;
                    else
                        result = FsmResult(CastFirst(ai, m, s, { "drain life", "shadow bolt" }));
                    break;
                }
                if (SpellId(ai, s.bot, m, "seed of corruption", false)
                    && AuraStacks(ai, s.bot, m, "seed of corruption", s.target) == 0
                    && TryCast(ai, m, s, "seed of corruption", nullptr))
                    result = TickResult::Acted;
                else
                    result = FsmResult(CastFirst(ai, m, s, { "shadow bolt" }));
                break;
            }

            case State::Burst:
            {
                CommandPetAttack(s);
                bool const threatOk = WarlockThreatOk(m, s);

                // [2026-11-01 站长口径] **爆发前预判**：myThreat + 预计爆发仇恨 > 坦克 ⇒
                //   先交灵魂碎裂再爆发（不然爆发一落地就 OT，坦克要把怪追回来）。
                //   两个硬门：① 每场只交一次（长 CD，开局就交等于把牌打空）；
                //            ② 战斗时长 ≥ shatter_min_engage_ms（默认 20 秒）：短战斗不值这张牌。
                uint32 const engageMs = m->engagedAt
                    ? WorldTimer::getMSTimeDiff(m->engagedAt, safeNow) : 0;
                int const shatterMinMs = int(TP(m, "shatter_min_engage_ms", 20000.0));
                float const burstThreatEst = float(TP(m, "burst_threat_est", 3000.0));
                bool const tankUp = s.tankAlive && s.tankThreat > 0.0f;
                // ① 真判据：预判会顶到坦克（>100%）
                bool const predictOver = tankUp
                    && (s.myThreat + burstThreatEst) > s.tankThreat * 1.0f;
                // ② 验收钩子（默认 0）：假人场景没有坦克 ⇒ predictOver 恒为假，这条分支永远摸不到。
                //    打开 `destro:shatter_force=1` 就把"越线"当成成立，用来在假人里**真实走一遍**
                //    "20 秒后交一次、每场只交一次"这条路（不是假日志：仍然要真的放出来才算数）。
                bool const forceShatter = TP(m, "shatter_force", 0.0) >= 1.0;
                if (!m->shatterUsed && engageMs >= uint32(shatterMinMs)
                    && (predictOver || forceShatter))
                {
                    if (TryCastSelf(ai, m, s, "soul shatter"))
                    {
                        m->shatterUsed = true;
                        // [2026-11-01 站长口径] 单独一条，便于脚本直接验收
                        sLog.outError("[DPSFSM-SHATTER] %s my=%.0f tank=%.0f",
                            s.bot->GetName(), s.myThreat, s.tankThreat);
                        result = TickResult::Acted;
                        break;
                    }
                }

                // 饰品 → 恶魔牺牲（恶魔，宠物不是刚召唤的）→ 暗影灼烧（毁灭）→ 暗影箭
                if (TP(m, "burst_trinket", 1.0) >= 1.0 && UseTrinket(ai, m, s))
                    result = TickResult::Acted;
                else if (dotSpec && m->spec == Spec::Demonology && WarlockCanSacrifice(ai, m, s, safeNow)
                    && TryCastPet(ai, m, s, "demonic sacrifice"))
                    result = TickResult::Acted;
                else if (!dotSpec && threatOk && SpellId(ai, s.bot, m, "shadowburn", false)
                    && TryCast(ai, m, s, "shadowburn", nullptr))
                    result = TickResult::Acted;
                else
                    result = FsmResult(CastFirst(ai, m, s, { "shadow bolt" }));
                break;
            }

            case State::Approach:
                result = TickResult::Decline;      // 走位归引擎
                break;

            case State::Maintain:
                result = dotSpec ? TickWarlockDotsMaintain(ai, m, s) : TickWarlockMaintain(ai, m, s);
                break;

            case State::Idle:
            default:
                result = TickResult::Decline;
                break;
        }

        // [2026-11-01 站长口径] 每 10 秒一条状态日志（WorldTimer 节流）：
        //   state / threat=自己/坦克 / mana / hp / shatter(0/1) / last=上一手成功施放的法术
        //   [2026-11-01 追加] spec=（aff/demo/destro，验收脚本按它切日志）、
        //   dots=（当前目标已挂的 DoT/诅咒数，证明"到期真的续了"）、multi=（本 tick 铺场的敌人数）。
        if (!m->lastWarlockLog || WorldTimer::getMSTimeDiff(m->lastWarlockLog, safeNow) >= 10000)
        {
            m->lastWarlockLog = safeNow;
            m->wlDots = WarlockDotCount(ai, s.bot, m, s.target);
            char const* last = "-";
            if (m->lastCastId)
                if (SpellEntry const* e = sServerFacade.LookupSpellInfo(m->lastCastId))
                    last = e->SpellName[0];
            sLog.outError("[DPSFSM-WARLOCK] %s spec=%s state=%s threat=%.0f/%.0f mana=%.0f hp=%.0f shatter=%d dots=%d multi=%d last=%s",
                s.bot->GetName(), WarlockSpecTag(m->spec), ai::dpsfsm::StateName(st), s.myThreat, s.tankThreat,
                s.power, s.selfHp, m->shatterUsed ? 1 : 0, m->wlDots, m->wlMulti, last);
        }

        return result;
    }

    // [2026-11-01 站长口径] 三个入口（派发处按专精选）：
    //   毁灭 → TickWarlockFsm()；痛苦 / 恶魔 → TickWarlockDotsFsm()（同一份骨架，dotSpec=true）。
    ai::dpsfsm::TickResult TickWarlockFsm(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        return TickWarlockFsmCore(ai, m, s, false);
    }

    ai::dpsfsm::TickResult TickWarlockDotsFsm(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        return TickWarlockFsmCore(ai, m, s, true);
    }

    // ==========================================================================
    // [2026-11-01 站长口径] 法师 · 火法（Fire）—— mode=5 的第六张职业图
    //   legacy 的 `TickMage`（mode=0/1/2/3/4 的火法路径）**一个字节都不动**：
    //   这张图只在 `mode=5` 且 `Spec::Fire` 时接管（见 `Tick()` 的派发）。
    //
    //   平稳期：**灼烧是轴，火球是体**。
    //     ① 起手 scorch ×5 叠满"强化灼烧"（每层 +5% 火焰伤害、5 层 25%、30 秒、全团火法共享）；
    //        层数读的是**目标身上的 Fire Vulnerability（22959）**，与灼烧本体不是同一个 id（见 SeedFireIds）；
    //     ② 叠满后 fireball 是**唯一填充**（占火法 95% 总伤害），**不用 fire blast 填充**；
    //     ③ 维护节奏：叠满后每 `scorch_refresh_fireballs`(8) 个火球补一次灼烧（≈24 秒）——
    //        **按"已经放了多少个火球"计数**是主判据，剩余时间只当兜底
    //        （`scorch_refresh_ms`(6000) 以下也必须补），**不卡最后 2 秒**；
    //     ④ 被抵抗：补完灼烧后 debuff 既没涨层、也没被续上 ⇒ **立刻再补一次**
    //        （日志 `recast-scorch(resist)`；只重补一次，不退化成链）；
    //     ⑤ 移动中：fire blast（瞬发，读条会被移动取消）；
    //     ⑥ 瞬发炎爆（法术连击）：**绝不打断当前火球读条**（`s.casting` 为真时本函数直接返回），读完再打。
    //
    //   爆发：**按预计战斗时长选三套**（时长是外部输入：`fight_length_est`，读不到才用
    //   `fight_length_default`(180 秒)；**不自己猜**）：
    //     short ≤180s：叠满 5 层 → icy veins + 饰品 打一轮 → 目标 ~25% cold snap 重置
    //                  → 进 20% 斩杀再开 icy veins + combustion + 饰品；
    //     mid 180~210s：起手**直接** icy veins + combustion，**再**叠灼烧；
    //                   结尾约 `fire_tail_sec`(20 秒) 第二轮自然转好，再打一轮；
    //     long >210s：起手叠满 → icy veins + 饰品 + combustion 一起；
    //                 之后 icy veins **单独**按 CD 开（不绑燃烧）；combustion 留给 20% 斩杀。
    //   combustion 机制（暴击叠 3 层即消耗进 3 分钟 CD）⇒ 它只放在"能连续暴击"的火球连发窗口里开，
    //   绝不和"移动/走位/空 GCD"的时段绑一起。
    //
    //   斩杀（目标 < `fire_execute_hp`(20%)）：molten fury 让火球伤害翻倍 ⇒ 所有爆发资源往这个窗口倾斜；
    //   学了 cold snap 就重置一次 icy veins（一场只重置一次，位掩码 8 记账）。
    //
    //   AoE：**起手先等坦克仇恨**（火法没有奥法魔爆那种减仇恨手段）⇒ `EvalThreat != Open` 时不许开；
    //        静止目标 flamestrike 7 级与 6 级交替（两级地面火池互不覆盖，可叠两层）；
    //        近身 blast wave / dragon's breath（学会了才放，仇恨更高，适合收尾）；
    //        blizzard 只用于减速需求（`slow_role`）。
    //   恢复：**宝石 → 大蓝 → 唤醒**（火法耗蓝高于冰法，但唤醒**不改变循环**，与奥法不同）。
    //
    //   可复用的现成口径（不重新发明）：`BuildSnapshot` / `CastFirst` / `TryCast` / `TryCastSelf` /
    //   `CastIdAt` / `CastMsWithMod` 与 `CandCastMs`（同一份真实读条口径）/ `SpellIdAtRank` /
    //   `AuraStacks` / `AuraRemain`（自身 buff）/ `EvalThreat` 三档 / `UseManaPotion` / `UseManaGem` /
    //   `UseTrinket` / `TP` / `TickResult{Acted,Hold,Decline}`。
    //   本图**不用** `FillCand`/`PickFill`：火法没有"按窗口放得下什么"的排程问题
    //   （火球是唯一填充），硬套只会把两套语义缝在一起（与毁灭术同一判断）。
    // ==========================================================================

    // [2026-11-01 站长口径] 强化灼烧挂的 debuff 与灼烧本体**不是同一个 id**：
    //   · 灼烧本体 = Scorch（bot 自己学的，27073/27074 一串 rank 走 `SpellId()` 自动顶最高级）；
    //   · 叠层     = **Fire Vulnerability 22959**（5 层、30 秒；TBC 2.3 起全团火法共享）。
    //     它由天赋"强化灼烧"(Improved Scorch 11095)
    //     的 `SPELL_AURA_ADD_TARGET_TRIGGER`（aura 109 → 触发 22959，见核心 `Spell::ProcSpellAuraTriggers`）
    //     在命中时挂上去 ⇒ **它永远不在 bot 的法术书里**。
    //   而 `AuraStacks()` 是**按名字**反查 id 的（`SpellId()` → "spell id" 值只扫 `bot->GetSpellMap()`）
    //   ⇒ 直接按名字读恒为 0 —— legacy `TickMage` 的火法灼烧分支整段失效就是这个原因
    //   （`scorchStacks` 恒 0，而 `scorch seen` 永远立不起来 ⇒ 一次灼烧都不放）。
    //   修法不是另写一套读层数：把 22959 **播种**进 BotMem 的 id 缓存
    //   （`SpellId()` 命中 `m->ids` 就直接返回、不再问引擎/法术书），之后一律用现成的
    //   `AuraStacks()` 与 spell_chain 遍历。
    uint32 const kFireVulnerability = 22959;

    void SeedFireIds(BotMem* m)
    {
        if (!m->ids.count("fire vulnerability"))
            m->ids["fire vulnerability"] = kFireVulnerability;
    }

    int FireScorchStacks(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        SeedFireIds(m);
        return AuraStacks(ai, s.bot, m, "fire vulnerability", s.target);
    }

    // 目标身上这个 debuff 的剩余毫秒（-1 = 不在 / 读不到）。
    //   `AuraRemain()` 只看 **bot 自己**的 buff，目标身上的 debuff 要自己按链取
    //   （与 `AuraStacks()` 同一条链：`GetFirstSpellInChain` → `GetNextSpellInChain`）。
    int FireDebuffRemain(PlayerbotAI* ai, BotMem* m, Snapshot& s, char const* name)
    {
        if (!s.target)
            return -1;
        SeedFireIds(m);
        uint32 const id = SpellId(ai, s.bot, m, name, false);
        if (!id)
            return -1;
        int best = -1;
        uint32 const first = sSpellMgr.GetFirstSpellInChain(id);
        for (uint32 cur = first ? first : id; cur; cur = sSpellMgr.GetNextSpellInChain(cur))
            if (SpellAuraHolder* holder = s.target->GetSpellAuraHolder(cur))
            {
                int const dur = holder->GetAuraDuration();
                if (dur > best)
                    best = dur;
            }
        return best;
    }

    // ---- 爆发表：由**外部输入**的战斗时长决定（不自己猜） ----
    enum class FirePlan { Short = 0, Mid = 1, Long = 2 };

    char const* FirePlanName(FirePlan p)
    {
        switch (p)
        {
            case FirePlan::Mid:  return "mid";
            case FirePlan::Long: return "long";
            default:             return "short";
        }
    }

    // 预计战斗时长（秒）。`TP()` 的缺省值机制没法区分"没配"和"配成 0"
    //   ⇒ `fight_length_est` 用 **-1 当哨兵**：读不到才落 `fight_length_default`(180 秒)。
    //   两个键都是**外部输入**（boss 战略数据 / 调用方给），状态机不猜、不按已打时长反推。
    double FireFightLength(BotMem* m)
    {
        double est = TP(m, "fight_length_est", -1.0);
        if (est <= 0.0)
            est = TP(m, "fight_length_default", 180.0);
        if (est <= 0.0)
            est = 180.0;
        return est;
    }

    FirePlan FirePlanFor(double sec)
    {
        if (sec <= 180.0)
            return FirePlan::Short;
        if (sec <= 210.0)
            return FirePlan::Mid;
        return FirePlan::Long;
    }

    // [2026-11-01 站长口径] 瞬发炎爆（法术连击）的**检测**：
    //   TBC 2.4.3 里"瞬发炎爆"只有两个真实来源 ——
    //     ① 气定神闲（presence of mind，奥系天赋）："下一发法术瞬发"；
    //     ② Hot Streak 类 proc：**WLK 才有的天赋**（本核心 2.4.3 的 DBC 里没有这个法术 ⇒ 查不到 id）。
    //   两个都按名字取（`SpellId` 取不到就恒 false），绝不自己编一个"hot streak"状态、
    //   也绝不凭感觉打 6 秒读条的炎爆（那是净亏 DPS）。
    bool FireInstantPyroReady(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        if (m->flags.count("fire no pyro"))
            return false;
        if (!SpellId(ai, s.bot, m, "pyroblast", false))
        {
            m->flags.insert("fire no pyro");
            return false;
        }
        if (AuraStacks(ai, s.bot, m, "presence of mind", s.bot) > 0)
            return true;
        if (AuraStacks(ai, s.bot, m, "hot streak", s.bot) > 0)
            return true;
        return false;
    }

    // [2026-11-01 站长口径] 用急冷（cold snap）把冰冷血脉重置回来：**一场只交一次**（位掩码 8）。
    //   core 侧口径（`SpellEffects.cpp → Spell::EffectDummy` 的 11958 分支）：急冷只清
    //   **冰霜系 + 有 RecoveryTime** 的冷却 ⇒ 冰冷血脉会被清掉，**燃烧是火焰系、清不掉**
    //   （所以 combustion 必须另有可用窗口 —— 见三套爆发表）。
    //   只在"冰冷血脉已经进 CD"时才交（还在手上就别浪费这张牌）。
    bool FireColdSnap(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        if (m->fireBurstDone & 8)
            return false;
        bool const dbg = (TP(m, "fire_coldsnap_dbg", 0.0) >= 1.0);
        uint32 const id = SpellId(ai, s.bot, m, "cold snap", false);
        if (!id)
        {
            // 没学急冷 ⇒ 这条分支不存在（一次性留痕，避免"为什么没重置"变成悬案）
            if (dbg && !m->flags.count("fire coldsnap dbg"))
            {
                m->flags.insert("fire coldsnap dbg");
                sLog.outError("[DPSFSM-FIRE] %s 没有 cold snap（11958 未学）⇒ 斩杀窗不会重置冰冷血脉",
                    s.bot->GetName());
            }
            return false;
        }
        uint32 const icyId = SpellId(ai, s.bot, m, "icy veins", false);
        if (!icyId || Ready(s.bot, icyId))
            return false;                        // 冰冷血脉本来就好着（或没学）⇒ 没必要交急冷
        if (!TryCastSelf(ai, m, s, "cold snap"))
        {
            // 一次性诊断（`fire_coldsnap_dbg=1` 才打）：把"到底为什么放不出"留痕。
            if (dbg && !m->flags.count("fire coldsnap dbg"))
            {
                m->flags.insert("fire coldsnap dbg");
                SpellCastResult res = SPELL_FAILED_DONT_REPORT;
                bool const can = ai->CanCastSpell(id, s.bot, 0, false, nullptr, false, false, false, &res);
                sLog.outError("[DPSFSM-FIRE] %s cold snap 放不出：id=%u spellReady=%d canCast=%d res=%d mana=%.0f",
                    s.bot->GetName(), id, Ready(s.bot, id) ? 1 : 0, can ? 1 : 0, int(res), s.power);
            }
            return false;
        }
        m->fireBurstDone |= 8;
        if (dbg)
            sLog.outError("[DPSFSM-FIRE] %s cold snap 已交（icy=%u 重置）", s.bot->GetName(), icyId);
        return true;
    }

    // 灼烧轴的记账与纠错（每 tick 一次，读 Snapshot + 层数，不发技能）：
    //   ① 第一次真的看到层数 ⇒ 记下"有强化灼烧天赋"（静态事实，跨轮保留）；
    //   ② 抵抗判据：补完灼烧后 debuff **既没涨层、也没被续上时长** ⇒ 立刻再补一次；
    //      ⚠️ 只按"层数有没有涨"会误判：满 5 层时的维护性补灼烧**本来就不涨层**
    //      （只续 30 秒时长）⇒ 必须同时看剩余时间是否被续上（`scorch_fresh_ms`）。
    //   ③ 没有天赋的兜底：放了 N 次灼烧目标仍 0 层 ⇒ 别再用 GCD 烧它（只打火球）。
    void FireScorchTick(PlayerbotAI* ai, BotMem* m, Snapshot& s, int stacks, int scorchFull)
    {
        if (stacks > 0 && !m->fireScorchSeen)
        {
            m->fireScorchSeen = true;
            m->fireScorchTries = 0;
            sLog.outError("[DPSFSM-FIRE] %s 强化灼烧 debuff(%u) 已挂上 scorch=%d ⇒ 灼烧轴建立",
                s.bot->GetName(), kFireVulnerability, stacks);
        }

        if (m->fireScorchPending && !s.casting)
        {
            int const remain = FireDebuffRemain(ai, m, s, "fire vulnerability");
            bool const stacksUp = (stacks > m->fireScorchBefore);
            // 只有"续上了时长"才算这一发真的落地：`remain < 0` 是**根本没这个 debuff**
            //   （不是刷新），所以它属于 miss，不能当 refreshed。
            bool const refreshed = (remain >= int(TP(m, "scorch_fresh_ms", 20000.0)));
            m->fireScorchPending = false;
            if (!stacksUp && !refreshed && !m->fireScorchResent)
            {
                m->fireScorchResent = true;
                m->fireScorchForce = true;      // 下一手就补，不再等 8 个火球
                m->fireFbSinceScorch = 0;
                sLog.outError("[DPSFSM-FIRE] %s recast-scorch(resist) scorch=%d before=%d remain=%d",
                    s.bot->GetName(), stacks, m->fireScorchBefore, remain);
            }
        }

        // N 默认 = scorch_stacks + 3：留余量，低等级天赋是"概率挂"，别把它误判成"没天赋"。
        //   [2026-11-01 实测] 没有这个天赋时 debuff **永远不会**出现 ⇒ 起手那 5 发灼烧纯属白丢 GCD
        //   （实测 30 秒单目标：8 发灼烧 0 层 debuff，dps 716 vs 纯火球 938）⇒ 先用**法术书**一票否决，
        //   连 5 发都不试：天赋没点就直接走纯火球，点了一定会挂上（5/5 是 100% 触发）。
        //   诊断字段：天赋 id（0 = 没学）、bot 身上 `ADD_TARGET_TRIGGER` 的 aura 条数（core 侧触发链）。
        if (!m->fireScorchSeen && !m->fireScorchPending && !m->fireScorchForce
            && !m->flags.count("fire scorch absent"))
        {
            uint32 const talent = ScanSpellbookByName(s.bot, "Improved Scorch");
            int const absentTries = int(TP(m, "scorch_absent_tries", double(scorchFull + 3)));
            if (!talent || m->fireScorchTries >= absentTries)
            {
                m->flags.insert("fire scorch absent");
                uint32 triggers = 0;
                for (Aura* aura : s.bot->GetAurasByType(SPELL_AURA_ADD_TARGET_TRIGGER))
                    if (aura)
                        ++triggers;
                sLog.outError("[DPSFSM-FIRE] %s 灼烧轴关闭：Improved Scorch=%u（0=没学）"
                    " ADD_TARGET_TRIGGER=%u 灼烧次数=%d 层数=%d ⇒ 改纯火球填充",
                    s.bot->GetName(), talent, triggers, m->fireScorchTries, stacks);
            }
        }
    }

    // 这一手该不该补灼烧：**按"已经放了几个火球"计数**是主判据（口径 ③），
    //   剩余时间只是兜底（< scorch_refresh_ms 也必须补），**不卡最后 2 秒**。
    bool FireWantScorch(PlayerbotAI* ai, BotMem* m, Snapshot& s, int stacks, int scorchFull)
    {
        if (m->fireScorchForce)
            return true;                                  // 抵抗重补：无条件再来一次
        if (m->flags.count("fire scorch absent"))
            return false;                                 // 没有强化灼烧天赋：灼烧只白占 GCD
        if (!SpellId(ai, s.bot, m, "scorch", false))
            return false;

        int const perFb = int(TP(m, "scorch_refresh_fireballs", 8.0));
        int const floorMs = int(TP(m, "scorch_refresh_ms", 6000.0));
        int const remainMs = FireDebuffRemain(ai, m, s, "fire vulnerability");

        if (scorchFull > 0 && stacks < scorchFull)
            return true;                                  // 起手叠满 / 层数被抵消
        if (perFb > 0 && m->fireFbSinceScorch >= perFb)
            return true;                                  // 已经放了 8 个火球（≈24 秒）：该补
        if (remainMs >= 0 && remainMs < floorMs)
            return true;                                  // 兜底：剩余不足 6 秒
        return false;
    }

    // 该开哪一轮爆发（0 = 不开）。位掩码：1 起手轮 / 2 结尾轮 / 4 斩杀轮。
    //   三套表**只差在"什么时候点火"**，点火之后的出手顺序是同一份（见 TickFireBurst）。
    int FireBurstRound(PlayerbotAI* ai, BotMem* m, Snapshot& s, FirePlan plan,
                       double elapsedSec, double fightLen, int scorchFull)
    {
        if (!s.target)
            return 0;
        int const stacks = FireScorchStacks(ai, m, s);
        bool const execute = (s.targetHp <= float(TP(m, "fire_execute_hp", 20.0)));
        double const remainSec = fightLen - elapsedSec;
        double const tailSec = TP(m, "fire_tail_sec", 20.0);

        switch (plan)
        {
            case FirePlan::Short:
                // 起手：**叠满 5 层之后**才开第一轮（口径：叠满 → icy veins + 饰品）
                if (!(m->fireBurstDone & 1) && (scorchFull <= 0 || stacks >= scorchFull))
                    return 1;
                // 斩杀：进 20% all-in（icy veins + combustion + 饰品）
                if (!(m->fireBurstDone & 4) && execute)
                    return 4;
                break;

            case FirePlan::Mid:
                // 起手**直接**开（不先叠灼烧），叠灼烧放在爆发窗口之后（口径 180~210s）
                if (!(m->fireBurstDone & 1))
                    return 1;
                // 结尾约 `fire_tail_sec`(20 秒)：第二轮自然转好，再打一轮
                if (!(m->fireBurstDone & 2) && remainSec > 0.0 && remainSec <= tailSec)
                    return 2;
                break;

            case FirePlan::Long:
                // 起手叠满 → icy veins + 饰品 + combustion 一起
                if (!(m->fireBurstDone & 1) && (scorchFull <= 0 || stacks >= scorchFull))
                    return 1;
                // combustion 留给 20% 斩杀（icy veins 单独按 CD 开，见 TickFireBurn 的 ⑤）
                if (!(m->fireBurstDone & 4) && execute)
                    return 4;
                break;
        }
        return 0;
    }

    // 点火：记账 + 撑开跨 tick 的爆发窗口（一个 GCD 一手，"饰品 → 冰冷血脉 → 燃烧"要分几个 tick 放完）。
    void FireBurstOpen(BotMem* m, FirePlan plan, int round)
    {
        m->fireBurstDone |= round;
        m->fireBurstRound = round;
        m->lastBurst = time(nullptr);
        m->burstUntil = m->lastBurst + time_t(TP(m, "burst_window", 6.0));

        // 每一轮放哪些牌（逐条对应站长口径）：
        //   short 起手轮   = 冰冷血脉 + 饰品（**combustion 留给 20% 斩杀**）；
        //   short 斩杀轮   = 冰冷血脉 + combustion + 饰品；
        //   mid   两轮     = 冰冷血脉 + combustion（口径没点名饰品，`fire_burst_trinket` 默认开）；
        //   long  起手/斩杀 = 冰冷血脉 + combustion + 饰品（之后 icy veins 单独按 CD 开）。
        m->fireBurstWantIcy = true;
        m->fireBurstWantTrinket = (TP(m, "fire_burst_trinket", 1.0) >= 1.0);
        switch (plan)
        {
            case FirePlan::Short: m->fireBurstWantComb = (round == 4); break;
            case FirePlan::Mid:   m->fireBurstWantComb = true;         break;
            case FirePlan::Long:  m->fireBurstWantComb = true;         break;
        }
    }

    // 状态转移只看 Snapshot + TP + 本 tick 已经点好的爆发窗口（不读世界、不打日志）：
    //   Idle → Recover(蓝见底) → Approach(够不着) → Aoe(群架且仇恨 Open) → Burst → Burn
    State EvalFire(BotMem* m, Snapshot& s, ThreatLevel threat)
    {
        if (!s.target)
            return State::Idle;

        // ① 资源：蓝见底 ⇒ Recover。`fire:recover_power` 是 conf 里的历史键名，两个都认。
        float const recoverEnter = float(TP(m, "recover_enter", TP(m, "recover_power", 10.0)));
        if (s.power < recoverEnter)
            return State::Recover;

        // ② 射程：先到位再谈别的（爆发牌打在 OUT_OF_RANGE 上等于白丢一个 GCD）
        if (s.dist > float(TP(m, "range_caster", 30.0)))
            return State::Approach;

        // ③ 群架：火法**没有减仇恨手段** ⇒ 只有 `EvalThreat == Open` 才许开 AoE。
        //    Throttle/Wait 时继续单点：替坦克把怪拉过来，比多打几发烈焰风暴亏得多。
        if (s.pack30 >= AoEPack(m) && threat == ThreatLevel::Open)
            return State::Aoe;

        // ④ 爆发窗口（`TickFireFsm` 在调用本函数之前已经点好火）
        if (time(nullptr) < m->burstUntil)
            return State::Burst;

        return State::Burn;
    }

    // 群伤动作表：**仇恨不是 Open 一律不开**（上面已经挡过一次，这里再挡一次：
    //   进段之后仇恨可能变差 —— 火法没有奥法魔爆那种减免）。
    ai::dpsfsm::TickResult TickFireAoe(PlayerbotAI* ai, BotMem* m, Snapshot& s, ThreatLevel threat)
    {
        using ai::dpsfsm::TickResult;

        if (threat != ThreatLevel::Open || !s.target)
            return TickResult::Decline;

        bool const inMelee = s.dist <= float(TP(m, "aoe_melee_range", 10.0));
        if (inMelee)
        {
            // 近身收尾牌：冲击波 / 龙息（**学会了才放**：`SpellId` 取不到就自动跳过；
            //   两者仇恨更高，口径把它们定位成"适合收尾"）。
            if (CastFirst(ai, m, s, { "blast wave" }))
                return TickResult::Acted;
            if (CastFirst(ai, m, s, { "dragon's breath" }))
                return TickResult::Acted;
        }

        // 静止目标：烈焰风暴**7 级与 6 级交替**（两级的地面火池互不覆盖，可以叠两层）。
        //   为什么要指定 rank：`SpellId()` 一律顶到最高 rank，只放 7 级就只有一层火。
        uint32 const fsTop = SpellIdAtRank(ai, m, "flamestrike", 7);
        uint32 const fsLow = SpellIdAtRank(ai, m, "flamestrike", 6);
        bool const alternate = (fsTop && fsLow && fsTop != fsLow);
        bool const useTop = ((m->fireFlameToggle % 2) == 0);
        uint32 const id = alternate ? (useTop ? fsTop : fsLow) : (fsTop ? fsTop : fsLow);
        if (!m->aoeLogDone)
        {
            m->aoeLogDone = true;
            sLog.outError("[DPSFSM-AOE] %s spec=fire threat=%s inmelee=%d flamestrike rank7=%u rank6=%u alternate=%d pack30=%d dist=%.1f",
                s.bot->GetName(), ThreatLevelName(threat), inMelee ? 1 : 0, fsTop, fsLow,
                alternate ? 1 : 0, s.pack30, s.dist);
        }
        if (id && CastIdAt(ai, m, s, id, s.target->GetPositionX(), s.target->GetPositionY(),
                s.target->GetPositionZ()))
        {
            ++m->fireFlameToggle;
            return TickResult::Acted;
        }

        // 减速需求（`slow_role`）：暴风雪（1 级一样减速、蓝耗低得多；中心点就是这堆怪）
        if (TP(m, "slow_role", 0.0) >= 1.0)
        {
            uint32 const slowRank = uint32(TP(m, "aoe_slow_rank", 1.0) + 0.5);
            uint32 const blizzardId = SpellIdAtRank(ai, m, "blizzard", slowRank ? slowRank : 1);
            if (blizzardId && CastIdAt(ai, m, s, blizzardId, s.target->GetPositionX(),
                    s.target->GetPositionY(), s.target->GetPositionZ()))
                return TickResult::Acted;
        }

        // 兜底：单体火球（群伤放不出时也别空着一个 GCD）
        return FsmResult(CastFirst(ai, m, s, { "fireball" }));
    }

    // 平稳期动作表：**灼烧是轴，火球是体**（顺序 = 口径 ①②③④⑤⑥）。
    ai::dpsfsm::TickResult TickFireBurn(PlayerbotAI* ai, BotMem* m, Snapshot& s, ThreatLevel threat,
                                        int stacks, int scorchFull, FirePlan plan)
    {
        using ai::dpsfsm::TickResult;

        // 仇恨 Wait 档：**不施法、也不让引擎动**（与冰法同一套口径：抢在坦克前面打，
        //   换来的是一只追着法师的怪；火法还没有冰法那种减速自保）。
        //   Throttle 档照常打（火球是低威胁填充），只是不开爆发、不开群伤。
        if (threat == ThreatLevel::Wait)
            return HoldWait(m, "threat-wait");

        // ⑤ 移动中：只有瞬发打得出去（读条会被移动取消）⇒ 火焰冲击，不是火球
        if (s.moving)
            return FsmResult(CastFirst(ai, m, s, { "fire blast" }));

        // short 表第二步：目标约 `cold_snap_hp`(25%) 时用急冷重置冰冷血脉（一场一次，位 8 记账）
        if (plan == FirePlan::Short && s.targetHp <= float(TP(m, "cold_snap_hp", 25.0))
            && FireColdSnap(ai, m, s))
            return TickResult::Acted;

        // ⑥ 瞬发炎爆（法术连击）：上面已经挡掉 `s.casting` ⇒ 这里只可能在**火球读完**之后打
        if (FireInstantPyroReady(ai, m, s) && CastFirst(ai, m, s, { "pyroblast" }))
            return TickResult::Acted;

        // ①③④ 灼烧轴：起手叠满 / 按"已放火球数"维护 / 剩余时间兜底 / 被抵抗重补
        if (FireWantScorch(ai, m, s, stacks, scorchFull)
            && TryCast(ai, m, s, "scorch", nullptr))
        {
            m->fireScorchForce = false;      // 抵抗重补只补一次
            m->fireScorchPending = true;     // 下一 tick 核对（涨层 / 续时长）
            m->fireScorchBefore = stacks;
            m->fireScorchResent = false;
            m->fireFbSinceScorch = 0;
            ++m->fireScorchTries;
            return TickResult::Acted;
        }

        // long 表：冰冷血脉**单独**按 CD 开（不绑燃烧；燃烧留给 20% 斩杀）。
        //   它不吃 GCD（`NoGcd`）⇒ 开了之后这一 tick 照常读火球，不白丢一手。
        bool actedOffGcd = false;
        if (plan == FirePlan::Long && CastFirst(ai, m, s, { "icy veins" }))
            actedOffGcd = true;

        // ② 火球是**唯一填充**（不用火焰冲击填充：它 8 秒 CD、射程 20 码，只给移动/斩杀用）
        if (CastFirst(ai, m, s, { "fireball" }))
        {
            ++m->fireFbSinceScorch;
            return TickResult::Acted;
        }
        return actedOffGcd ? TickResult::Acted : TickResult::Decline;
    }

    // 爆发动作表：三套表共用这一份顺序 —— **饰品 → 冰冷血脉 → 燃烧** →（瞬发炎爆）→ 火球。
    //   饰品先开不浪费持续时间；燃烧最后开，让"连续暴击"的窗口正好落在火球连发上
    //   （combustion 暴击叠 3 层就被消耗进 3 分钟 CD，开在走位/空 GCD 里等于白开）。
    ai::dpsfsm::TickResult TickFireBurst(PlayerbotAI* ai, BotMem* m, Snapshot& s, ThreatLevel threat)
    {
        using ai::dpsfsm::TickResult;

        if (threat == ThreatLevel::Wait)
            return HoldWait(m, "threat-wait");

        // 斩杀轮（位 4）：冰冷血脉还在 CD ⇒ 先用急冷重置一次（一场一次，位 8 记账）
        if (m->fireBurstRound == 4 && FireColdSnap(ai, m, s))
            return TickResult::Acted;

        if (m->fireBurstWantTrinket && UseTrinket(ai, m, s))
            return TickResult::Acted;
        if (m->fireBurstWantIcy && CastFirst(ai, m, s, { "icy veins" }))
            return TickResult::Acted;
        if (m->fireBurstWantComb && CastFirst(ai, m, s, { "combustion" }))
            return TickResult::Acted;
        if (FireInstantPyroReady(ai, m, s) && CastFirst(ai, m, s, { "pyroblast" }))
            return TickResult::Acted;
        return FsmResult(CastFirst(ai, m, s, { "fireball" }));
    }

    ai::dpsfsm::TickResult TickFireFsm(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        using ai::dpsfsm::TickResult;

        // 读条/引导中一律不插技能 —— **瞬发炎爆也不打断当前火球**（口径 ⑥）：
        //   炎爆的"瞬发"是它自己瞬发，插进去会把读到一半的火球顶掉，净亏一个火球的伤害。
        if (s.casting)
            return TickResult::Acted;

        uint32 const nowMs = WorldTimer::getMSTime();
        uint32 const safeNow = nowMs ? nowMs : 1;

        // 交战计时（"预计战斗时长"要跟**已经打了多久**比）：有目标就开始计，没目标清场
        if (s.target)
        {
            if (!m->engagedAt)
                m->engagedAt = safeNow;
        }
        else
        {
            m->engagedAt = 0;
            m->fireBurstDone = 0;
        }

        double const fightLen = FireFightLength(m);
        FirePlan const plan = FirePlanFor(fightLen);
        double const elapsedSec = m->engagedAt
            ? double(WorldTimer::getMSTimeDiff(m->engagedAt, safeNow)) / 1000.0 : 0.0;

        int const scorchFull = int(TP(m, "scorch_stacks", 5.0));
        int const stacks = FireScorchStacks(ai, m, s);
        FireScorchTick(ai, m, s, stacks, scorchFull);

        // 仇恨档位本 tick 只算一次：AoE / 爆发 / 平稳三处共用同一份判断
        //   （诊断日志在 EvalThreat 内部按 10 秒节流）。
        ThreatLevel const threat = EvalThreat(m, s);

        // 爆发点火：**唯一一处**写 fireBurstDone / burstUntil（窗口从这一刻撑开）。
        //   出射程时不点火：爆发牌打在 OUT_OF_RANGE 上等于白丢（口径的"先到位"同理）。
        if (time(nullptr) >= m->burstUntil && s.target
            && s.dist <= float(TP(m, "range_caster", 30.0)))
        {
            int const round = FireBurstRound(ai, m, s, plan, elapsedSec, fightLen, scorchFull);
            if (round)
                FireBurstOpen(m, plan, round);
        }

        State const st = EvalFire(m, s, threat);
        m->state = st;

        TickResult result = TickResult::Decline;
        switch (st)
        {
            case State::Recover:
                // [2026-11-01 站长口径] 恢复顺序：**宝石 → 大蓝 → 唤醒**。
                //   唤醒是 8 秒引导 ⇒ 只在宝石/药水都不行时才用；它**不改变循环**
                //   （与奥法不同：奥法的唤醒是 3+3 的排程起点，火法的唤醒只是回蓝）。
                if (UseManaGem(ai, m, s))
                    result = TickResult::Acted;
                else if (UseManaPotion(m, s))
                    result = TickResult::Acted;
                else if (TryCastSelf(ai, m, s, "evocation"))
                    result = TickResult::Acted;
                else
                    result = TickResult::Decline;      // 喝水/魔杖交回引擎
                break;

            case State::Approach:
                result = TickResult::Decline;          // 走位归引擎
                break;

            case State::Aoe:
                result = TickFireAoe(ai, m, s, threat);
                break;

            case State::Burst:
                result = TickFireBurst(ai, m, s, threat);
                break;

            case State::Burn:
                result = TickFireBurn(ai, m, s, threat, stacks, scorchFull, plan);
                break;

            case State::Idle:
            default:
                result = TickResult::Decline;
                break;
        }

        // [2026-11-01 站长口径] 每 10 秒一条状态日志（WorldTimer 节流，别每 tick 刷屏）：
        //   state / scorch=层数 / sinceFb=自上次补灼烧放出的火球数 / hotstreak=0|1 / mana=% / burstPlan
        if (!m->lastFireLog || WorldTimer::getMSTimeDiff(m->lastFireLog, safeNow) >= 10000)
        {
            m->lastFireLog = safeNow;
            sLog.outError("[DPSFSM-FIRE] %s state=%s scorch=%d sinceFb=%d hotstreak=%d mana=%.0f burstPlan=%s",
                s.bot->GetName(), ai::dpsfsm::StateName(st), stacks, m->fireFbSinceScorch,
                FireInstantPyroReady(ai, m, s) ? 1 : 0, s.power, FirePlanName(plan));
        }

        return result;
    }

    // ==========================================================================
    // [2026-11-01 站长口径] 圣骑士 · 惩戒（Retribution）—— mode=5 的第七张职业图
    //   核心已经实现了 300/400ms 的圣印窗口（`Unit::AddSpellAuraHolder` 的 `[SEALTWIST]` 分支：
    //   换圣印时旧圣印 aura 保留 `CalculateTime(400)` 毫秒再删）。**我们只负责时机**，
    //   不维护影子圣印、不自己算双印伤害。
    //
    //   站长的口径逐条落到代码：
    //     ① **平砍计时驱动**：读 `getAttackTimer(BASE_ATTACK)`（核心 `Unit::Update` 每 tick 递减，
    //        可用；RANGED 那个是死值别用）。`swing=<剩余ms>` 就是"下一刀还有多久"。
    //     ② **圣印舞**：剩余 ≤ `twist_window_ms`（默认 350）且当前挂的是**命令** ⇒ 切**鲜血**
    //        （联盟切**正义**，站长口径）。命令用 rank1（`command_rank`），鲜血/正义用满级。
    //        切完打一条 `[DPSFSM-TWIST]`，便于验收"真的切了"。
    //     ③ **2 刀 1 舞**：`twist_every_swings`（默认 2）——用"平砍计时器读数跳回攻速"数平砍，
    //        到点才舞；其余平砍正常挂着命令（舞完由 `retRestoreSeal` 在**非窗口**时切回命令，
    //        避免回切本身又触发一次双印、把节奏变成"每刀都舞"）。
    //     ④ **审判禁区**：`judgement` 会吃掉当前圣印 ⇒ 只在 `swing > judge_gcd_ms`（默认 1500，
    //        一个 GCD）时放；放完 `retNeedSeal=true`，下一手立刻补命令圣印。
    //     ⑤ **优先级**：舞 > 十字军打击（6s CD）> 审判 > 奉献/驱邪；CS 转好但与舞同 tick 冲突时先舞。
    //
    //   填充与蓝量：
    //     · `consecration` 单体 rank1（`consecration_rank`）；复仇之怒期间用满级；群架（≥aoe_targets）
    //       也用满级并提到最前；蓝 < `consecration_mana`（25%）⇒ 奉献与驱邪一起停。
    //     · `exorcism` 只对亡灵/恶魔（`GetCreatureType()`）进候选。
    //   爆发：`avenging wrath` + 饰品 + `burst_potion`（1=加速药水（默认，骑士有蓝但爆发窗口换的是伤害）、
    //     2=法力药水、0=不喝）；`fight_length_est`（默认 180）≤120 ⇒ 等到 `burst_delay`（默认 60）
    //     才开（等嗜血后半段）；>120 ⇒ 开场就开。
    //   恢复：蓝 < `recover_mana`（20%）⇒ 先 `UseManaPotion`；`use_seal_of_wisdom`（默认 0，先别开）
    //     =1 时改用智慧圣印替换鲜血/命令。
    //   防护/神圣**不接**：派发处就排除，`TickRetFsm` 里再兜一次 Decline。
    //
    //   可复用（不重新发明）：`BuildSnapshot`、`CastFirst/TryCast/TryCastSelf/CastSelfCentered`、
    //   `SpellIdAtRank`、`AuraRemain`、`TickResult{Acted,Hold,Decline}`、`EvalThreat`、
    //   `UseManaPotion`、`UseTrinket`、`TP`。
    // ==========================================================================

    // [2026-11-01 站长口径] 自身中心 AoE 的**按 id** 版本：`CastSelfCentered` 只吃名字（一律顶到
    //   最高 rank），而惩戒单目标要放 1 级奉献省蓝、复仇之怒期间才用满级 ⇒ 必须能指定 id。
    //   入口与 CastSelfCentered 同一口径（核心 `Unit::CastSpell(self)`：奉献是 TARGET_UNIT_CASTER，
    //   不是 DEST_LOCATION；走 bot 的 CanCastSpell 会被判成"负法术 + 友方目标"）。
    bool CastSelfCenteredId(PlayerbotAI* ai, BotMem* m, Snapshot& s, uint32 id, char const* name)
    {
        if (!Ready(s.bot, id))
            return false;
        SpellEntry const* entry = sServerFacade.LookupSpellInfo(id);
        if (entry && s.bot->HasGCD(entry))
            return false;    // 别顶掉正在转的 GCD

        SpellCastResult const res = s.bot->CastSpell(s.bot, id, uint32(TRIGGERED_NONE));
        if (res != SPELL_CAST_OK)
        {
            time_t const now = time(nullptr);
            if (!m->lastConsLog || now - m->lastConsLog >= 10)
            {
                m->lastConsLog = now;
                sLog.outError("[DPSFSM-RET] %s 放不出 %s：id=%u result=%d",
                    s.bot->GetName(), name, id, int(res));
            }
            return false;
        }
        ++m->castCount;
        m->castThisTick = true;
        m->lastOk[id] = time(nullptr);
        m->lastCastId = id;
        PlayerbotAI::RecordSpellCast(s.bot->GetObjectGuid(), id);
        return true;
    }

    // 惩戒这一轮要用到的法术 id（一次取全：SpellId/SpellIdAtRank 都有 per-bot 缓存，取多几次不贵）。
    struct RetSealIds
    {
        uint32 command = 0;          // 命令圣印 rank1（站长口径：命令只用 1 级）
        uint32 twist = 0;            // 鲜血（部落）/ 正义（联盟），满级
        uint32 wisdom = 0;           // 智慧圣印（恢复开关，默认不用）
        uint32 avenging = 0;
        uint32 crusader = 0;
        uint32 judgement = 0;
        uint32 consecration1 = 0;
        uint32 consecrationMax = 0;
        uint32 holyWrath = 0;
        char const* twistName = "seal of blood";
    };

    void RetLoadIds(PlayerbotAI* ai, BotMem* m, RetSealIds& out)
    {
        Player* bot = ai->GetBot();
        // [2026-11-01 站长口径] 联盟没有鲜血圣印 ⇒ 用正义圣印当"舞"的另一半（站长口径原文：联盟：正义）。
        out.twistName = (bot->GetTeam() == HORDE) ? "seal of blood" : "seal of righteousness";
        double const cmdRank = TP(m, "command_rank", 1.0);
        out.command = SpellIdAtRank(ai, m, "seal of command", uint32(cmdRank < 1.0 ? 1 : int(cmdRank)), false);
        out.twist = SpellId(ai, bot, m, out.twistName, false);
        out.wisdom = SpellId(ai, bot, m, "seal of wisdom", false);
        out.avenging = SpellId(ai, bot, m, "avenging wrath", false);
        out.crusader = SpellId(ai, bot, m, "crusader strike", false);
        out.judgement = SpellId(ai, bot, m, "judgement", false);
        out.consecrationMax = SpellId(ai, bot, m, "consecration", false);
        double const consRank = TP(m, "consecration_rank", 1.0);
        out.consecration1 = SpellIdAtRank(ai, m, "consecration", uint32(consRank < 1.0 ? 1 : int(consRank)), false);
        out.holyWrath = SpellId(ai, bot, m, "holy wrath", false);
    }

    bool RetIsUndeadOrDemon(Unit* u)
    {
        if (!u)
            return false;
        uint32 const type = u->GetCreatureType();
        return type == CREATURE_TYPE_UNDEAD || type == CREATURE_TYPE_DEMON;
    }

    // 自身某个 aura 的剩余毫秒（按**精确 id**——命令是 rank1，`AuraRemain(name)` 会去查最高 rank，
    //   对 rank1 的命令圣印恒返回 0，所以这里不能用它）。
    int RetSealRemainMs(Player* bot, uint32 id)
    {
        if (!id)
            return 0;
        SpellAuraHolder* holder = bot->GetSpellAuraHolder(id);
        if (!holder)
            return 0;
        int const dur = holder->GetAuraDuration();
        return dur < 0 ? 999999 : dur;
    }

    // 按 id 挂自身圣印：圣印是 TARGET_UNIT_CASTER 的自身增益，直接走核心 `Unit::CastSpell(self)`
    //   （与 CastChecked 里 TARGET_FRIENDLY 的 selfFix 同一条路），自己守 GCD/会没会。
    bool RetCastSealId(PlayerbotAI* ai, BotMem* m, Snapshot& s, uint32 id)
    {
        if (!Ready(s.bot, id) || s.bot->HasAura(id))
            return false;
        SpellEntry const* entry = sServerFacade.LookupSpellInfo(id);
        if (entry && s.bot->HasGCD(entry))
            return false;    // GCD 没转好：这一 tick 切不了，等下一 tick

        SpellCastResult const res = s.bot->CastSpell(s.bot, id, uint32(TRIGGERED_NONE));
        if (res != SPELL_CAST_OK)
            return false;
        ++m->castCount;
        m->castThisTick = true;
        m->lastOk[id] = time(nullptr);
        m->lastCastId = id;
        PlayerbotAI::RecordSpellCast(s.bot->GetObjectGuid(), id);
        return true;
    }

    // 日志里的 seal=：当前挂的是哪一个（短名，验收脚本好切）。
    char const* RetSealShortName(Player* bot, RetSealIds const& ids)
    {
        if (ids.wisdom && bot->HasAura(ids.wisdom))  return "wisdom";
        if (ids.command && bot->HasAura(ids.command)) return "command";
        if (ids.twist && bot->HasAura(ids.twist))
            return (ids.twistName && strcmp(ids.twistName, "seal of blood") == 0) ? "blood" : "righteousness";
        return "none";
    }

    // [DPSFSM-RET] <bot> swing=<剩余ms> seal=<name> twists=<n> cs=<0/1> mana=<%> target=<hp%>：10 秒一条。
    //   cs = 十字军打击这一 tick 是否可用（学会 + 冷却/GCD 就绪）；twists = 本场累计舞次数。
    void RetLogTick(BotMem* m, Snapshot& s, RetSealIds const& ids, int swingMs)
    {
        uint32 const nowMs = WorldTimer::getMSTime();
        uint32 const safeNow = nowMs ? nowMs : 1;
        if (m->lastRetLog && WorldTimer::getMSTimeDiff(m->lastRetLog, safeNow) < 10000)
            return;
        m->lastRetLog = safeNow;
        bool const csReady = ids.crusader && Ready(s.bot, ids.crusader);
        sLog.outError("[DPSFSM-RET] %s swing=%d seal=%s twists=%u cs=%d mana=%.0f target=%.0f",
            s.bot ? s.bot->GetName() : "-", swingMs, RetSealShortName(s.bot, ids),
            m->retTwistCount, csReady ? 1 : 0, s.power, s.targetHp);
    }

    // 爆发时间门（站长口径）：
    //   fight_length_est ≤ 120 秒 ⇒ 稍等（等嗜血后半段）：开打后 `burst_delay`（默认 60）秒才开；
    //   > 120 秒 ⇒ 开场就开。
    bool RetBurstTimeOk(BotMem* m, uint32 nowMs)
    {
        if (!m->retEngagedAt)
            return true;
        double const fightLen = TP(m, "fight_length_est", 180.0);
        double const elapsed = double(WorldTimer::getMSTimeDiff(m->retEngagedAt, nowMs)) / 1000.0;
        double delay = 0.0;
        if (fightLen <= 120.0)
            delay = std::min(TP(m, "burst_delay", 60.0), fightLen * 0.5);
        return elapsed >= delay;
    }

    // 状态转移：Idle → Recover（蓝 < recover_mana）→ Aoe（≥aoe_targets）→ Approach（够不着）
    //   → Execute（目标 < execute_hp，锤子）→ Burst（时间门 + 复仇之怒就绪 + 仇恨不是 Wait）→ Maintain。
    State EvalRet(BotMem* m, Snapshot& s, ThreatLevel threat, RetSealIds const& ids)
    {
        if (!s.target)
            return State::Idle;

        // 近战范围用核心判定（与 legacy TickPaladin 同一口径：表面距离会把"中心 5.0 码"误判成在范围内）。
        //   放在最前：够不着就先让引擎把位置带过去，任何自身中心 AoE / 审判都别对着空地放。
        if (!s.bot->CanReachWithMeleeAttack(s.target))
            return State::Approach;

        // 蓝紧：只是"停填充 + 喝药"，不是停手（惩戒的审判/十字军/圣印消耗很低，停手反而白丢 DPS）
        if (s.power < float(TP(m, "recover_mana", 20.0)))
            return State::Recover;

        if (s.pack10 >= int(TP(m, "aoe_targets", 3.0)))
            return State::Aoe;

        if (s.targetHp <= float(TP(m, "execute_hp", 20.0)))
            return State::Execute;

        uint32 const nowMs = WorldTimer::getMSTime();
        uint32 const safeNow = nowMs ? nowMs : 1;
        if (safeNow < m->retBurstUntil)
            return State::Burst;
        if (threat != ThreatLevel::Wait
            && ids.avenging && Ready(s.bot, ids.avenging)
            && RetBurstTimeOk(m, safeNow))
            return State::Burst;

        return State::Maintain;
    }

    ai::dpsfsm::TickResult TickRetFsm(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        using ai::dpsfsm::TickResult;

        // 防护/神圣不归这张图（派发处已排除，这里再兜一次；派发以后若加专精也不会误接管）
        if (m->spec != Spec::Retribution)
            return TickResult::Decline;
        if (!s.target)
            return TickResult::Decline;

        // 读条中（驱邪 1.5 秒）：一个技能都不插，否则会把这一发顶掉
        if (s.casting)
            return TickResult::Acted;

        Player* bot = s.bot;
        if (s.target != bot)
            bot->SetFacingToObject(s.target);   // Acted/Hold 时引擎整 tick 不跑，朝向得自己转

        // 换目标 = 换一场战斗：交战计时/平砍计数/舞次数/爆发窗口都从头来
        uint32 const nowMs = WorldTimer::getMSTime();
        uint32 const safeNow = nowMs ? nowMs : 1;
        ObjectGuid const targetGuid = s.target->GetObjectGuid();
        if (targetGuid != m->retTarget)
        {
            m->retTarget = targetGuid;
            m->retEngagedAt = safeNow;
            m->retBurstUntil = 0;
            m->retSwings = 0;
            m->retTwistCount = 0;
            m->retPrevSwingMs = -1;
            m->retNeedSeal = false;
            m->retRestoreSeal = false;
        }
        if (!m->retEngagedAt)
            m->retEngagedAt = safeNow;

        // ① 平砍计时（站长口径：BASE 计时器可用；RANGED 是死值别用）
        int const swingMs = int(bot->getAttackTimer(BASE_ATTACK));
        // 平砍成功 ⇒ 核心 resetAttackTimer() 把读数跳回攻速；读数"突然变大"就说明刚刚过了一刀。
        //   失败挥击（不在范围/朝向错）核心只把计时器设成 100ms，不会触发这个跳变。
        if (m->retPrevSwingMs >= 0 && swingMs > m->retPrevSwingMs + 800)
            ++m->retSwings;
        m->retPrevSwingMs = swingMs;

        RetSealIds ids;
        RetLoadIds(ai, m, ids);

        int twistWindow = int(TP(m, "twist_window_ms", 350.0));
        if (twistWindow < 0) twistWindow = 0;
        // 核心 [SEALTWIST] 的保留窗口是 400ms（Unit.cpp → `CalculateTime(400)`，注释写的是 300ms）。
        //   这里留 20ms 余量 ⇒ 上限 380；默认 350 稳稳落在窗口里。
        if (twistWindow > 380) twistWindow = 380;
        int const twistEvery = std::max(1, int(TP(m, "twist_every_swings", 2.0)));
        int const judgeGcd = int(TP(m, "judge_gcd_ms", 1500.0));

        bool const wisdomMode = (TP(m, "use_seal_of_wisdom", 0.0) >= 1.0)
            && s.power < float(TP(m, "recover_mana", 20.0)) && ids.wisdom != 0;

        bool const commandUp = ids.command && bot->HasAura(ids.command);
        bool const twistUp = ids.twist && bot->HasAura(ids.twist);
        bool const wisdomUp = ids.wisdom && bot->HasAura(ids.wisdom);
        bool const sealUp = commandUp || twistUp || wisdomUp;
        bool const inWindow = swingMs > 0 && swingMs <= twistWindow;

        ThreatLevel const threat = EvalThreat(m, s);
        State const st = EvalRet(m, s, threat, ids);

        TickResult result = TickResult::Decline;

        // ② 圣印舞（最高优先级）：窗口内且当前挂命令、且"2 刀 1 舞"到点 ⇒ 切鲜血/正义。
        //   核心会保留旧命令 aura 400ms ⇒ 这一刀双印；我们只负责在 400ms 之前把新圣印放出来。
        if (!wisdomMode && inWindow && ids.command && ids.twist && commandUp
            && int(m->retSwings) >= twistEvery)
        {
            if (RetCastSealId(ai, m, s, ids.twist))
            {
                sLog.outError("[DPSFSM-TWIST] %s swing=%d from=seal of command to=%s",
                    bot->GetName(), swingMs, ids.twistName);
                ++m->retTwistCount;
                m->retSwings = 0;
                m->retRestoreSeal = true;
                result = TickResult::Acted;
            }
        }

        // ③ 圣印维护 / 舞后回切 / 审判后补印（审判会吃掉圣印，必须立刻补）
        if (result == TickResult::Decline && wisdomMode)
        {
            // 恢复模式：用智慧圣印换续航（默认关，`use_seal_of_wisdom=1` 才走）
            if (!wisdomUp && RetCastSealId(ai, m, s, ids.wisdom))
                result = TickResult::Acted;
        }
        else if (result == TickResult::Decline)
        {
            if (m->retRestoreSeal && !inWindow && commandUp)
                m->retRestoreSeal = false;      // 已经回到命令（舞完的下一手），不用再补

            bool needSeal = false;
            if (m->retNeedSeal)
            {
                needSeal = true;                // 审判刚吃掉圣印：**不管窗口**，下一手立刻补
            }
            else if (m->retRestoreSeal && !inWindow)
            {
                needSeal = true;                // 舞完回切命令（非窗口，避免回切也双印）
            }
            else if (ids.command ? (!commandUp && !twistUp) : !twistUp)
            {
                needSeal = true;                // 开局 / 圣印掉了
            }
            else if (ids.command && commandUp && !inWindow
                && RetSealRemainMs(bot, ids.command) < 2000)
            {
                needSeal = true;                // 命令快到期，非窗口时续一次
            }

            if (needSeal)
            {
                uint32 const want = ids.command ? ids.command : ids.twist;
                if (want && RetCastSealId(ai, m, s, want))
                {
                    m->retNeedSeal = false;
                    m->retRestoreSeal = false;
                    result = TickResult::Acted;
                }
            }
        }

        // ③.5 回切/补印还没成功（多半是上一个 GCD 还在转）：这一手**先别拿 CS/审判去占 GCD**。
        //   实测（60 秒 5 轮）舞次数只有 4.4/轮：舞完想切回命令，GCD 一转好就被十字军打击/审判抢走，
        //   命令迟迟挂不回去 ⇒ 下一轮"2 刀 1 舞"的计数到点了也没有命令可切。
        //   这里让回切优先；蓝不够放圣印（rank1 只花 65）时不拦，避免低蓝时把自己锁死。
        bool const sealBlocked = !wisdomMode && ids.command && !commandUp
            && (m->retRestoreSeal || m->retNeedSeal)
            && bot->GetPower(POWER_MANA) >= 100;
        if (result == TickResult::Decline && sealBlocked)
        {
            if (st == State::Burst && m->state != State::Burst)
                m->retBurstUntil = safeNow + 5000;
            LogTransition(ai, m, s, st);
            RetLogTick(m, s, ids, swingMs);
            return TickResult::Decline;
        }

        // ④ 爆发（Burst 状态）：复仇之怒 → 饰品 → 药水（一个 GCD 一手，跨几个 tick 放完）
        if (result == TickResult::Decline && st == State::Burst)
        {
            if (ids.avenging && TryCastSelf(ai, m, s, "avenging wrath"))
                result = TickResult::Acted;
            else if (UseTrinket(ai, m, s))
                result = TickResult::Acted;
            else
            {
                int const potion = int(TP(m, "burst_potion", 1.0));
                if (potion == 1)
                {
                    if (UseHastePotion(m, s, true))
                        result = TickResult::Acted;
                }
                else if (potion == 2)
                {
                    if (UseManaPotion(m, s))
                        result = TickResult::Acted;
                }
            }
        }

        // ⑤ 锤子（目标 < execute_hp）：TBC 惩戒的斩杀技；木桩满血时这条**永远不触发**（见回报）。
        if (result == TickResult::Decline && st == State::Execute
            && CastFirst(ai, m, s, { "hammer of wrath" }))
            result = TickResult::Acted;

        // ⑥ 群架：奉献优先级上调（满级），亡灵/恶魔再加神圣之怒
        bool const manaOk = s.power >= float(TP(m, "consecration_mana", 25.0));
        if (result == TickResult::Decline && st == State::Aoe)
        {
            uint32 const consId = ids.consecrationMax ? ids.consecrationMax : ids.consecration1;
            if (manaOk && consId && CastSelfCenteredId(ai, m, s, consId, "consecration"))
                result = TickResult::Acted;
            else if (manaOk && RetIsUndeadOrDemon(s.target) && ids.holyWrath
                && CastSelfCenteredId(ai, m, s, ids.holyWrath, "holy wrath"))
                result = TickResult::Acted;
        }

        // ⑦ 十字军打击（6s CD）—— 排在审判之前
        if (result == TickResult::Decline && ids.crusader
            && CastFirst(ai, m, s, { "crusader strike" }))
            result = TickResult::Acted;

        // ⑧ 审判：只在"平砍剩余 > 一个 GCD"时放（否则吃掉圣印、这一刀落空）；
        //    放完 retNeedSeal=true，下一手立刻补印（③ 里处理）。
        if (result == TickResult::Decline && sealUp && swingMs > judgeGcd
            && CastFirst(ai, m, s, { "judgement" }))
        {
            m->retNeedSeal = true;
            result = TickResult::Acted;
        }

        // ⑨ 单体填充：奉献 rank1（复仇之怒期间满级）→ 驱邪（仅亡灵/恶魔）。蓝 < consecration_mana 时都停。
        if (result == TickResult::Decline && manaOk)
        {
            bool const awUp = ids.avenging && bot->HasAura(ids.avenging);
            uint32 const consId = (awUp || st == State::Aoe)
                ? (ids.consecrationMax ? ids.consecrationMax : ids.consecration1)
                : (ids.consecration1 ? ids.consecration1 : ids.consecrationMax);
            if (consId && CastSelfCenteredId(ai, m, s, consId, "consecration"))
                result = TickResult::Acted;
            else if (RetIsUndeadOrDemon(s.target) && AffordSpell(ai, m, s, "exorcism")
                && CastFirst(ai, m, s, { "exorcism" }))
                result = TickResult::Acted;
        }

        // ⑩ 恢复：蓝 < recover_mana ⇒ 先喝法力药水（UseManaPotion 内部按蓝缺口判断）；
        //    智慧圣印在 ③ 里处理。这里不 return，输出照打（填充已被蓝门挡住）。
        if (result == TickResult::Decline && st == State::Recover)
        {
            if (UseManaPotion(m, s))
                result = TickResult::Acted;
        }

        // 进入 Burst 的那一刻开窗口，让"复仇之怒 → 饰品 → 药水"跨几个 tick 依次放完
        if (st == State::Burst && m->state != State::Burst)
            m->retBurstUntil = safeNow + 5000;

        LogTransition(ai, m, s, st);
        RetLogTick(m, s, ids, swingMs);
        return result;
    }

    // =====================================================================================
    // [2026-11-01 站长口径] 萨满接进 mode=5（第八张职业图）：元素（Elemental）+ 增强（Enhancement）。
    //   恢复（Restoration）**不做 DPS**：派发处不选它，`TickShamanFsm` 里也 Decline，
    //   继续走 legacy `TickShaman`（治疗以后单独做）。
    //
    //   ① 图腾子系统（三系共用，必须先做）—— 核心坑（KNOWN_ISSUES §10.53 实证）：
    //     施放图腾后 `GetTotemGuid(slot)` **仍为空**（召唤走 SPELL_EFFECT_SUMMON，没有登记成
    //     玩家的图腾槽位）⇒ **绝不能按槽位判空**，否则每 tick 都重铺、把 GCD 全吃光
    //     （实测：25 秒放了 20 次法力之泉，元素萨 dps 410~818 → 0~250）。
    //     ⇒ 自己记账（BotMem 的 totemCastAt / totemEntry / totemExpireAt / totemPos），四条判据：
    //         ① 槽位没有登记过（从没插过 / 上一轮清过）
    //         ② 剩余时间 < totem_refresh_ms(3000)
    //         ③ 离插下去的位置 > totem_range(40)（队伍移动，光环照不到）
    //         ④ 想要的法术变了（切图腾组：单体↔群体、元素↔增强、图腾舞 优雅↔风怒）
    //     图腾时长取 `SpellEntry` 的 Duration（sSpellMgr.GetSpellDuration）；拿不到/无限按 120 秒兜底。
    //     补图腾占**一个 GCD**，优先级：空槽/快到期 > 输出技能。
    //   ② 三系图腾组（站长口径）：
    //     元素：大地 stoneskin（或 tremor 功能优先）/ 火焰 totem of wrath / 水 mana spring / 空气 wrath of air
    //     增强：大地 strength of earth / 火焰 magma(群) 或 searing(单) / 水 mana spring / 空气 windfury
    //   ③ 图腾舞（增强，可选）：空气槽插风怒后立刻换优雅，twist_period_ms(10s) 后再换回。
    //     `totem_twist_enable` 默认 -1=自动：本小队近战 ≥ twist_melee_min(2) 才开。
    // =====================================================================================

    char const* ShTotemSlotKey(int slot)
    {
        switch (slot)
        {
            case TOTEM_SLOT_EARTH: return "earth";
            case TOTEM_SLOT_FIRE:  return "fire";
            case TOTEM_SLOT_WATER: return "water";
            case TOTEM_SLOT_AIR:   return "air";
        }
        return "?";
    }

    // 图腾时长（毫秒）：从**召唤法术**的 Duration 取。
    //   TBC 的图腾召唤法术 DurationIndex 大多是 4（=120 秒），灼热/熔岩/新星另有值；
    //   拿不到（没有 entry / DurationIndex 缺失）或 -1（无限）都按 120 秒兜底 —— 宁可早补，不要空着。
    int ShTotemDurationMs(uint32 id)
    {
        SpellEntry const* entry = id ? sServerFacade.LookupSpellInfo(id) : nullptr;
        int32 const d = entry ? GetSpellDuration(entry) : 0;
        if (d <= 0)
            return 120000;
        return int(d);
    }

    bool ShHasTotem(PlayerbotAI* ai, BotMem* m, Snapshot& s, char const* name)
    {
        // warn=false：低等级 bot 本来就可能没学某个图腾，不能每 tick 刷 "没有可用技能"
        return SpellId(ai, s.bot, m, name, false) != 0;
    }

    // 图腾组的上下文（一次 tick 算好，传给规划函数）
    struct ShTotemCtx
    {
        bool elemental = false;
        bool enhance = false;
        bool inAoe = false;      // 群架：火焰槽换成 magma（增强）/ magma（元素）
        bool twist = false;      // 图腾舞开（只影响空气槽，由 ShTickTwist 单独处理）
    };

    // 这一槽这一手**该插哪个**图腾（nullptr = 这槽不插）。
    //   名字一律用 DB 规范名；`SpellId(..., false)` 返回 0（没学）就静默跳过，不报错。
    char const* ShDesiredTotem(PlayerbotAI* ai, BotMem* m, Snapshot& s, ShTotemCtx const& c, int slot)
    {
        if (c.elemental)
        {
            switch (slot)
            {
                case TOTEM_SLOT_EARTH:
                    // 功能优先：自己吃到恐惧/迷惑就先插震颤图腾（解控 > 属性），否则石甲图腾
                    if ((s.bot->isFeared() || s.bot->HasAuraType(SPELL_AURA_MOD_CONFUSE))
                        && ShHasTotem(ai, m, s, "tremor totem"))
                        return "tremor totem";
                    return "stoneskin totem";
                case TOTEM_SLOT_FIRE:
                    // AOE 起手只放一次火元素新星（5 秒后自己炸），之后火焰槽回到熔岩图腾
                    if (c.inAoe && !m->shAoeNova && ShHasTotem(ai, m, s, "fire nova totem"))
                        return "fire nova totem";
                    if (c.inAoe)
                        return ShHasTotem(ai, m, s, "magma totem") ? "magma totem" : "searing totem";
                    // 站长口径：元素火焰槽 = 愤怒图腾（3% 命中 + 3% 法爆）；没这天赋就退回灼热
                    return ShHasTotem(ai, m, s, "totem of wrath") ? "totem of wrath" : "searing totem";
                case TOTEM_SLOT_WATER:
                    return "mana spring totem";
                case TOTEM_SLOT_AIR:
                    return ShHasTotem(ai, m, s, "wrath of air totem") ? "wrath of air totem" : "grace of air totem";
            }
            return nullptr;
        }
        if (c.enhance)
        {
            switch (slot)
            {
                case TOTEM_SLOT_EARTH: return "strength of earth totem";
                case TOTEM_SLOT_FIRE:
                    // 站长口径：群架熔岩图腾、单体灼热图腾；AOE 起手先来一发火元素新星（只一次）
                    if (c.inAoe && !m->shAoeNova && ShHasTotem(ai, m, s, "fire nova totem"))
                        return "fire nova totem";
                    return c.inAoe ? "magma totem" : "searing totem";
                case TOTEM_SLOT_WATER: return "mana spring totem";
                case TOTEM_SLOT_AIR:   return c.twist ? "grace of air totem" : "windfury totem";
            }
            return nullptr;
        }
        return nullptr;
    }

    // 这一槽**要不要补**（四条判据，见节头注释）。
    bool ShTotemNeed(PlayerbotAI* ai, BotMem* m, Snapshot& s, int slot, uint32 wantId, uint32 now)
    {
        if (!wantId)
            return false;
        int const refreshMs = int(TP(m, "totem_refresh_ms", 3000.0));
        float const range = float(TP(m, "totem_range", 40.0));

        // ① 槽位没有登记过
        if (!m->totemEntry[slot])
            return true;
        // ④ 切了图腾组：想要的法术与记着的不一样（单体↔群体 / 元素↔增强 / 舞 优雅↔风怒）
        if (m->totemEntry[slot] != wantId)
            return true;
        // ② 快到期
        if (m->totemExpireAt[slot] && now + uint32(refreshMs > 0 ? refreshMs : 0) >= m->totemExpireAt[slot])
            return true;
        // ③ 队伍移动：离插下去的位置太远，光环照不到（重插比"等它过期"值）
        if (range > 0.0f
            && s.bot->GetDistance(m->totemPos[slot][0], m->totemPos[slot][1], m->totemPos[slot][2]) > range)
            return true;
        // 机会主义校对：**只有**槽位真的登记了（本核心通常为空）才拿它当真相；
        //   槽位为空时绝不据此重插 —— 那就是 §10.53 的坑。
        if (!s.bot->GetTotemGuid(TotemSlot(slot)).IsEmpty())
        {
            Totem* t = s.bot->GetTotem(TotemSlot(slot));
            if (!t || !t->IsAlive())
                return true;
        }
        return false;
    }

    // 补一个图腾（占 1 个 GCD）。**成功了才记账** —— 记账是唯一真相源。
    bool ShCastTotem(PlayerbotAI* ai, BotMem* m, Snapshot& s, int slot, char const* name, uint32 now)
    {
        uint32 const id = SpellId(ai, s.bot, m, name, false);
        if (!id)
            return false;                       // 没学这个图腾：静默跳过，不占 GCD、不报错
        if (!TryCastSelf(ai, m, s, name))
            return false;
        m->totemCastAt[slot] = now;
        m->totemEntry[slot] = id;
        m->totemExpireAt[slot] = now + uint32(ShTotemDurationMs(id));
        m->totemPos[slot][0] = s.bot->GetPositionX();
        m->totemPos[slot][1] = s.bot->GetPositionY();
        m->totemPos[slot][2] = s.bot->GetPositionZ();
        ++m->shTotemCasts;
        if (slot == TOTEM_SLOT_FIRE && strcmp(name, "fire nova totem") == 0)
            m->shAoeNova = true;                // AOE 起手只放一次
        return true;
    }

    // 本小队（同一 sub-group）的近战人数，**含自己**。
    //   站长口径点的是"战/贼/增强/猫德/惩戒"，这里用核心自己的近战判定（PlayerbotAI::IsMelee，
    //   与引擎决定接近距离用的是同一份口径）近似，不另写一张职业表（会跟 IsMeleeSpec 漂移）。
    //   ⚠️ `IsMelee` 不是 static 成员（`IsTank`/`IsHeal` 才是），但实现里只读**参数**那个玩家
    //      （`player->GetPlayerbotAI()`），与 this 无关 ⇒ 借这只 bot 的 ai 当接收者即可；
    //      `inGroup=true` 才能走到"按策略判断"那一支（否则萨满会按职业表被当成远程）。
    int ShMeleeInParty(PlayerbotAI* ai, Player* bot)
    {
        if (!ai || !bot)
            return 0;
        int n = ai->IsMelee(bot, true) ? 1 : 0;
        if (Group* group = bot->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->getSource();
                if (!member || member == bot || !sServerFacade.IsAlive(member))
                    continue;
                if (group->SameSubGroup(bot, member) && ai->IsMelee(member, true))
                    ++n;
            }
        }
        return n;
    }

    // 图腾舞开不开：0=关 / 1=开 / -1(默认)=自动（本小队近战 ≥ twist_melee_min）
    bool ShTwistEnabled(PlayerbotAI* ai, BotMem* m, Snapshot& s, bool enhance)
    {
        if (!enhance)
            return false;
        int const cfg = int(TP(m, "totem_twist_enable", -1.0));
        if (cfg == 0)
            return false;
        if (cfg == 1)
            return true;
        return ShMeleeInParty(ai, s.bot) >= int(TP(m, "twist_melee_min", 2.0));
    }

    // 图腾舞：空气槽 风怒 → 立刻换优雅 → twist_period_ms(10s) 后换回风怒。
    //   ⚠️ 每次换插都占一个 GCD（这是图腾舞的固有代价，不是 bug）；默认只在 ≥2 近战时开。
    bool ShTickTwist(PlayerbotAI* ai, BotMem* m, Snapshot& s, uint32 now)
    {
        int const air = TOTEM_SLOT_AIR;
        uint32 const wfId = SpellId(ai, s.bot, m, "windfury totem", false);
        uint32 const goaId = SpellId(ai, s.bot, m, "grace of air totem", false);
        if (!wfId || !goaId)
            return false;

        int const switchMs = int(TP(m, "twist_switch_ms", 300.0));
        int const periodMs = int(TP(m, "twist_period_ms", 10000.0));

        if (m->twistPhase == 0)
        {
            bool cast = false;
            if (ShTotemNeed(ai, m, s, air, wfId, now))
                cast = ShCastTotem(ai, m, s, air, "windfury totem", now);
            // 风怒已经在（或刚插上）⇒ 进入"等切优雅"相位；**没真施法就返回 false**，
            //   别把这一手当成 Acted 白占一个 GCD。
            if (cast || !ShTotemNeed(ai, m, s, air, wfId, now))
            {
                m->twistPhase = 1;
                m->twistNextAt = now + uint32(switchMs > 0 ? switchMs : 0);
            }
            return cast;
        }
        if (m->twistPhase == 1)
        {
            if (now < m->twistNextAt)
                return false;
            if (!ShCastTotem(ai, m, s, air, "grace of air totem", now))
                return false;
            m->twistPhase = 2;
            m->twistNextAt = now + uint32(periodMs > 0 ? periodMs : 0);
            return true;
        }
        // 相位 2：优雅期到点 ⇒ 换回风怒，回到相位 1（等下一个 GCD 再切优雅）
        if (now < m->twistNextAt)
            return false;
        if (!ShCastTotem(ai, m, s, air, "windfury totem", now))
            return false;
        m->twistPhase = 1;
        m->twistNextAt = now + uint32(switchMs > 0 ? switchMs : 0);
        return true;
    }

    // 一次 tick 最多补**一个**图腾（占一个 GCD）。顺序：大地 → 火焰 → 水 → 空气。
    bool ShTickTotems(PlayerbotAI* ai, BotMem* m, Snapshot& s, ShTotemCtx const& c, uint32 now)
    {
        bool const twist = (c.enhance && c.twist);
        static int const kOrder[] = { TOTEM_SLOT_EARTH, TOTEM_SLOT_FIRE, TOTEM_SLOT_WATER, TOTEM_SLOT_AIR };
        for (int slot : kOrder)
        {
            // 图腾舞开着时空气槽交给 ShTickTwist（它要在 风怒↔优雅 之间来回换）
            if (twist && slot == TOTEM_SLOT_AIR)
                continue;
            char const* name = ShDesiredTotem(ai, m, s, c, slot);
            if (!name)
                continue;
            uint32 const id = SpellId(ai, s.bot, m, name, false);
            if (!ShTotemNeed(ai, m, s, slot, id, now))
                continue;
            if (ShCastTotem(ai, m, s, slot, name, now))
                return true;
            // 放不出（没蓝 / 正被控 / GCD 没转完）⇒ 本 tick 不再试下一槽，
            //   免得把这一手的时间全花在"失败的图腾施放"上
            return false;
        }
        if (twist)
            return ShTickTwist(ai, m, s, now);
        return false;
    }

    // 目标身上某个 debuff（任意 rank）的剩余毫秒；没有 = 0，永久 = 999999。
    int ShTargetDebuffRemain(PlayerbotAI* ai, BotMem* m, Snapshot& s, char const* name)
    {
        uint32 const id = SpellId(ai, s.bot, m, name, false);
        if (!id || !s.target)
            return 0;
        uint32 const first = sSpellMgr.GetFirstSpellInChain(id);
        for (uint32 cur = first ? first : id; cur; cur = sSpellMgr.GetNextSpellInChain(cur))
        {
            if (!s.target->HasAura(cur))
                continue;
            SpellAuraHolder* holder = s.target->GetSpellAuraHolder(cur);
            if (!holder)
                continue;
            int const dur = holder->GetAuraDuration();
            return dur < 0 ? 999999 : dur;
        }
        return 0;
    }

    // 闪电盾的**层数**。站长口径写的是"用 AuraStacks 读自己身上的 lightning shield"，
    //   但本核心的 `AuraStacks()` 走 `Unit::GetAuraCount()` = **stack 数量**，而 TBC 闪电盾的
    //   层数是 **charge**（存在 `Aura::GetAuraCharges()` / m_procCharges）⇒ 只读 stack 永远是 1，
    //   `ls_stacks` 这辈子都不可能满足（实测：ls=1、es=0 整场不发）。所以两个都读，取大的：
    //     ① 先按站长口径取 AuraStacks（stack）；② 再翻这条链上每个 rank 的 aura 取 charge。
    int ShLightningShieldStacks(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        int const stacks = AuraStacks(ai, s.bot, m, "lightning shield", s.bot);
        uint32 const id = SpellId(ai, s.bot, m, "lightning shield", false);
        if (!id)
            return stacks;
        int charges = 0;
        uint32 const first = sSpellMgr.GetFirstSpellInChain(id);
        for (uint32 cur = first ? first : id; cur; cur = sSpellMgr.GetNextSpellInChain(cur))
        {
            if (!s.bot->HasAura(cur))
                continue;
            SpellAuraHolder* holder = s.bot->GetSpellAuraHolder(cur);
            if (!holder)
                continue;
            // charge 记在 **holder** 上（SpellAuraHolder::GetAuraCharges → m_procCharges），
            //   不是记在单个 Aura 上 —— 闪电盾掉一层就是这里 -1。
            charges = std::max(charges, int(holder->GetAuraCharges()));
        }
        return std::max(stacks, charges);
    }

    // 风怒武器 3 秒 ICD：**只做日志**（不做微延迟对齐，进阶先不做）。
    //   本核心不暴露 proc 法术的剩余冷却，只能观测 `Windfury Attack`(25504) 从"就绪"跳到
    //   "冷却中"的边沿 ⇒ wfIcd 是**两次触发之间的观测间隔**（≥3000 说明 ICD 生效）；
    //   观测不到（没附风怒武器 / 核心不给这条法术挂冷却）就是 -1，日志如实反映，不编造。
    void ShNoteWindfury(BotMem* m, Player* bot, uint32 now)
    {
        if (!bot)
            return;
        bool const ready = bot->IsSpellReady(25504);
        if (!ready && m->wfReadyPrev)
        {
            if (m->wfLastProcAt)
                m->wfIcdMs = int(int64(now) - int64(m->wfLastProcAt));
            m->wfLastProcAt = int(now);
            ++m->wfProcs;
        }
        m->wfReadyPrev = ready;
    }

    // [DPSFSM-SH] <bot> spec=<ele|enh> state= totems=<已登记槽数> mana=<%> cl=<0/1> twist=<0/1>
    //   每 10 秒一条（WorldTimer 节流）。增强额外带 wfIcd=（风怒触发间隔观测值，-1 = 观测不到）。
    void ShLogTick(BotMem* m, Snapshot& s, bool elemental, bool twist)
    {
        uint32 const nowMs = WorldTimer::getMSTime();
        uint32 const safeNow = nowMs ? nowMs : 1;
        if (m->lastShLog && WorldTimer::getMSTimeDiff(m->lastShLog, safeNow) < 10000)
            return;
        m->lastShLog = safeNow;

        int registered = 0;
        for (int i = 0; i < MAX_TOTEM_SLOT; ++i)
            if (m->totemEntry[i] && (!m->totemExpireAt[i] || m->totemExpireAt[i] > safeNow))
                ++registered;

        if (elemental)
            sLog.outError("[DPSFSM-SH] %s spec=ele state=%s totems=%d mana=%.0f cl=%d twist=0",
                s.bot ? s.bot->GetName() : "-", StateName(m->state), registered, s.power,
                m->shClOk ? 1 : 0);
        else
            sLog.outError("[DPSFSM-SH] %s spec=enh state=%s totems=%d mana=%.0f cl=0 twist=%d wfIcd=%d",
                s.bot ? s.bot->GetName() : "-", StateName(m->state), registered, s.power,
                twist ? 1 : 0, m->wfIcdMs);

        // `sh_diag=1`：每 10 秒多一条，专门回答验收的两个问题 ——
        //   ① 图腾到底有没有在补（每槽 entry / 剩余秒，以及**槽位登记**的真相）；
        //   ② 闪电盾层数读不读得到（earth shock 的 ls_stacks 判据靠它）。
        if (TP(m, "sh_diag", 0.0) < 1.0)
            return;
        if (m->lastShDiag && WorldTimer::getMSTimeDiff(m->lastShDiag, safeNow) < 10000)
            return;
        m->lastShDiag = safeNow;
        int slotGuid = 0;
        for (int i = 0; i < MAX_TOTEM_SLOT; ++i)
            if (s.bot && !s.bot->GetTotemGuid(TotemSlot(i)).IsEmpty())
                ++slotGuid;
        sLog.outError("[DPSFSM-SH-DIAG] %s slotguid=%d/%d casts=%u earth=%u/%d fire=%u/%d water=%u/%d air=%u/%d ls=%d es=%u tw=%d/%u",
            s.bot ? s.bot->GetName() : "-", slotGuid, int(MAX_TOTEM_SLOT), m->shTotemCasts,
            m->totemEntry[TOTEM_SLOT_EARTH], int(m->totemExpireAt[TOTEM_SLOT_EARTH] > safeNow
                ? (m->totemExpireAt[TOTEM_SLOT_EARTH] - safeNow) / 1000 : 0),
            m->totemEntry[TOTEM_SLOT_FIRE], int(m->totemExpireAt[TOTEM_SLOT_FIRE] > safeNow
                ? (m->totemExpireAt[TOTEM_SLOT_FIRE] - safeNow) / 1000 : 0),
            m->totemEntry[TOTEM_SLOT_WATER], int(m->totemExpireAt[TOTEM_SLOT_WATER] > safeNow
                ? (m->totemExpireAt[TOTEM_SLOT_WATER] - safeNow) / 1000 : 0),
            m->totemEntry[TOTEM_SLOT_AIR], int(m->totemExpireAt[TOTEM_SLOT_AIR] > safeNow
                ? (m->totemExpireAt[TOTEM_SLOT_AIR] - safeNow) / 1000 : 0),
            m->shLightningShield, m->shEarthShocks, m->twistPhase, m->wfProcs);
    }

    ai::dpsfsm::TickResult TickShamanFsm(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        using ai::dpsfsm::TickResult;

        bool const elemental = (m->spec == Spec::Elemental);
        bool const enhance   = (m->spec == Spec::Enhancement);
        // 恢复（Restoration）不做 DPS：不接管，交回引擎
        if (!elemental && !enhance)
            return TickResult::Decline;
        if (!s.target)
            return TickResult::Decline;
        // 读条/引导中（闪电箭 2 秒 / 闪电链 1.5 秒）：一个技能都不插，否则把这一发顶掉
        if (s.casting)
            return TickResult::Acted;

        Player* bot = s.bot;
        if (s.target != bot)
            bot->SetFacingToObject(s.target);   // Acted/Hold 时引擎整 tick 不跑，朝向得自己转

        uint32 const nowMs = WorldTimer::getMSTime();
        uint32 const safeNow = nowMs ? nowMs : 1;

        // 换目标 = 换一场战斗：Aoe 起手（火元素新星只放一次）与交战计时从头来
        ObjectGuid const targetGuid = s.target->GetObjectGuid();
        if (targetGuid != m->shTarget)
        {
            m->shTarget = targetGuid;
            m->shEngagedAt = safeNow;
            m->shAoeNova = false;
            m->shEarthShocks = 0;
        }
        if (!m->shEngagedAt)
            m->shEngagedAt = safeNow;

        ShTotemCtx c;
        c.elemental = elemental;
        c.enhance = enhance;
        c.inAoe = (s.pack10 >= int(TP(m, "aoe_pack", 3.0)));
        c.twist = ShTwistEnabled(ai, m, s, enhance);
        m->shClOk = elemental && (s.power >= float(TP(m, "cl_mana_min", 60.0)));

        // ------------------------------------------------------------------
        // ① 图腾优先（占一个 GCD）：空槽 / 快到期 / 换组 / 队伍走远
        // ------------------------------------------------------------------
        TickResult result = TickResult::Decline;
        if (ShTickTotems(ai, m, s, c, safeNow))
            result = TickResult::Acted;

        // ------------------------------------------------------------------
        // ② 状态转移（阈值全部走 TP()）
        // ------------------------------------------------------------------
        State st = State::Maintain;
        if (elemental)
        {
            // 闪电盾：earth shock 的层数来源，也是减伤；没了就是 Prep（开局/被驱散后）。
            //   注意顺序：先判"够不着 / 蓝低"，最后才判盾 —— 否则蓝不够放盾时会卡在 Prep 里不出手。
            m->shLightningShield = ShLightningShieldStacks(ai, m, s);
            if (s.dist > float(TP(m, "range_cast", 25.0)))
                st = State::Approach;      // > 25 码：交给移动层，自己不空放
            else if (s.power < float(TP(m, "recover_power", 10.0)))
                st = State::Recover;
            else if (m->shLightningShield == 0)
                st = State::Prep;
            else if (c.inAoe)
                st = State::Aoe;
            else
            {
                // 元素爆发 = 元素掌握 + 饰品，**早用早 CD**（不是攒到最后）；
                //   `burst_delay`（默认 0 秒）留给坦克拉仇恨；目标 < burst_hp 也进 Burst。
                uint32 const emId = SpellId(ai, bot, m, "elemental mastery", false);
                bool const emReady = emId && Ready(bot, emId);
                double const elapsed = double(WorldTimer::getMSTimeDiff(m->shEngagedAt, safeNow)) / 1000.0;
                bool const burstDoor = emReady && elapsed >= TP(m, "burst_delay", 0.0);
                st = (s.targetHp <= BurstHp(m) || burstDoor) ? State::Burst : State::Maintain;
            }
        }
        else
        {
            if (!bot->CanReachWithMeleeAttack(s.target))
                st = State::Approach;
            else if (s.power < float(TP(m, "recover_power", 20.0)))
                st = State::Recover;
            else if (c.inAoe)
                st = State::Aoe;
            else
                st = State::Maintain;
        }

        LogTransition(ai, m, s, st);

        // ------------------------------------------------------------------
        // ③ 各状态动作（一 tick 一个 GCD，放成就返回 Acted）
        // ------------------------------------------------------------------
        if (result == TickResult::Decline && st == State::Prep)
        {
            // 元素开场：闪电盾（earth shock 靠它叠层，也是减伤）。
            //   ⚠️ 被顶掉时**别紧着补**：本核心的引擎在"状态机 Decline 的 tick"仍会跑它自己的
            //   萨满 buff（实测插 Water Shield 33736），两个盾互顶 ⇒ 45 秒里闪电盾放 7 次、
            //   水盾放 7 次，元素萨 dps 从 900 掉到 450。这里 `ls_recast_ms`(5 秒) 内补过就等一轮，
            //   把互顶的代价封顶；真要根治得把"自身护盾"也收进 FSM 并让引擎闭嘴（后续）。
            uint32 const lsGap = uint32(std::max(0, int(TP(m, "ls_recast_ms", 5000.0))));
            if (elemental
                && (!m->lsCastAt || WorldTimer::getMSTimeDiff(m->lsCastAt, safeNow) >= lsGap)
                && TryCastSelf(ai, m, s, "lightning shield"))
            {
                m->lsCastAt = safeNow;
                result = TickResult::Acted;
            }
        }

        // ---------------- 元素萨（蓝色驱动） ----------------
        if (result == TickResult::Decline && elemental)
        {
            if (st == State::Maintain || st == State::Burst)
            {
                if (s.moving)
                {
                    // 移动中读条会被打断 ⇒ 只有瞬发烈焰震击能放；其余交回引擎走位
                    if (AuraStacks(ai, bot, m, "flame shock", s.target) == 0
                        && CastFirst(ai, m, s, { "flame shock" }))
                        result = TickResult::Acted;
                }
                else if (st == State::Burst && UseTrinket(ai, m, s))
                {
                    result = TickResult::Acted;      // 爆发窗口：先开饰品
                }
                else if (st == State::Burst && TryCastSelf(ai, m, s, "elemental mastery"))
                {
                    // 元素掌握**绑给下一发闪电链/闪电箭**，不空交（这一手只交掌握，下一手交技能）
                    result = TickResult::Acted;
                }
                else
                {
                    // 震击**不是常规填充**：只有闪电盾叠到 ls_stacks 才把它震出去换伤害
                    //   ⚠️ TBC 闪电盾是 3 层（charge），默认 ls_stacks=7 达不到 ⇒ 这条默认不触发；
                    //     把 ls_stacks 调到 3 就是"每满盾震一次"。层数的读法见 ShLightningShieldStacks()。
                    int const lsStacks = ShLightningShieldStacks(ai, m, s);
                    m->shLightningShield = lsStacks;
                    if (lsStacks >= int(TP(m, "ls_stacks", 7.0))
                        && CastFirst(ai, m, s, { "earth shock" }))
                    {
                        ++m->shEarthShocks;
                        result = TickResult::Acted;
                    }
                    // 蓝够 ⇒ 闪电链优先（1.5 秒读条，DPS 更高）；蓝吃紧 ⇒ 无脑闪电箭
                    else if (m->shClOk
                        && (CastFirst(ai, m, s, { "chain lightning" })
                            || CastFirst(ai, m, s, { "lightning bolt" })))
                        result = TickResult::Acted;
                    else if (!m->shClOk && CastFirst(ai, m, s, { "lightning bolt" }))
                        result = TickResult::Acted;
                }
            }
            else if (st == State::Aoe)
            {
                // 站长口径：fire nova totem → chain lightning → magma totem → lightning bolt
                //   （两个图腾已由图腾子系统按"火焰槽 = 火元素新星一次 → 熔岩"的顺序补完）
                if (!s.moving && CastFirst(ai, m, s, { "chain lightning" }))
                    result = TickResult::Acted;
                else if (!s.moving && m->shClOk && CastFirst(ai, m, s, { "lightning bolt" }))
                    result = TickResult::Acted;
            }
            else if (st == State::Recover)
            {
                // 蓝低：先喝药；还能放就继续闪电箭（空 GCD 比低效填充更亏），最后才交回引擎
                if (UseManaPotion(m, s))
                    result = TickResult::Acted;
                else if (!s.moving && CastFirst(ai, m, s, { "lightning bolt" }))
                    result = TickResult::Acted;
            }
        }

        // ---------------- 增强萨 ----------------
        if (result == TickResult::Decline && enhance
            && (st == State::Maintain || st == State::Aoe || st == State::Burst))
        {
            // ① 空蓝 ⇒ 萨满之怒（这是**回蓝手段**，不是"减伤窗口"；不吃 GCD）
            if (s.power < float(TP(m, "rage_power", 20.0)) && TryCastSelf(ai, m, s, "shamanistic rage"))
                result = TickResult::Acted;
            // ② 风暴打击卡 CD
            else if (CastFirst(ai, m, s, { "stormstrike" }))
                result = TickResult::Acted;
            else
            {
                // ③ 震击（共享 6 秒冷却）
                //   站长口径的字面顺序是"目标有风暴打击 debuff ⇒ 地震，否则烈焰震击维持 DoT"。
                //   但风暴打击 debuff 12 秒 > 风暴打击 10 秒 CD ⇒ 它几乎**永远**是 up，
                //   字面照做烈焰震击会一发都不放 ⇒ 把"维持 DoT"的判断抬到前面：
                //   DoT 缺 / 快到期 ⇒ 烈焰震击；否则按字面走地震（两者不冲突）。
                int const fsRemain = ShTargetDebuffRemain(ai, m, s, "flame shock");
                bool const fsNeed = (fsRemain < int(TP(m, "fs_refresh_ms", 2000.0)));
                bool const ssUp = AuraStacks(ai, bot, m, "stormstrike", s.target) > 0;
                if (fsNeed && CastFirst(ai, m, s, { "flame shock" }))
                    result = TickResult::Acted;
                else if (ssUp && CastFirst(ai, m, s, { "earth shock" }))
                    result = TickResult::Acted;
                else if (!ssUp)
                    // [2026-11-01 站长口径] 登记为有意等（震击共享 6 秒 CD、两条都用不上）：
                    //   这一手放别的会把下一次震击预算打乱，等 CD 比硬放值。
                    result = HoldWait(m, "shock-cd-wait");
            }
        }

        // ------------------------------------------------------------------
        // ④ 收尾：日志（10 秒一条）+ 风怒 ICD 观测（只记日志）
        // ------------------------------------------------------------------
        if (enhance)
            ShNoteWindfury(m, bot, safeNow);
        ShLogTick(m, s, elemental, c.twist);
        return result;
    }

    // ==========================================================================
    // [2026-11-01 站长口径] 牧师 · 暗影（Shadow）—— mode=5 的第九张职业图
    //   （口径来源：dev/DESIGN_dps_fsm_graphs.md §6.9 + §10.7，站长 2026-11-01 定稿）
    //
    //   **这张图与前面八张有本质区别：暗牧不是优先级表，是 GCD 时间轮。**
    //   legacy 的 `TickPriest`（mode=0/1/2/3/4 的暗牧路径）一个字节都不动：
    //   这里只在 `mode=5` 且 `Spec::Shadow` 时接管（见 `Tick()` 的派发）。
    //
    //   为什么必须按拍排：心爆 6 秒 CD 正好 4 个 GCD（GCD 1.5s），触/痛是十几秒量级，
    //   三者的"到期拍"会周期性相撞。优先级表只能看"这一拍挑一个"，看不见"下一拍撞车"
    //   ⇒ 实测表现就是周期性丢心爆、或者两个 DoT 在同一拍一起到期（一次挤掉两个 GCD）。
    //
    //   时间轮本体（**一拍 = 一个 GCD**）：
    //     · 拍钟：`BotMem::spBeat` / `spBeatAt`（下一拍开始的毫秒时刻）。挂钟过一拍 +1；
    //       引导技（精神鞭笞 3 秒）额外再吃一拍（`spBeatAt += gcd`）。
    //       停过太久（> 4 拍，例如换场景/假人重开）**不补拍**、直接对齐到现在。
    //     · 三个时间点，全部换算成"还有几拍"（见 ShadowWheel）：
    //         swp —— 目标身上**痛**的剩余毫秒：**严格等到到期之后**才续，不提前覆盖；
    //         vt  —— 目标身上**触**的剩余毫秒 **减去一个读条**（实际 1.5s，按急速算）：
    //                提前一个 GCD 起手，让新触正好落在旧触到期的瞬间；
    //         mb  —— 心爆"记账"的剩余（CD 取自模板 categoryRecoveryTime 经 SPELLMOD_COOLDOWN 修正）。
    //     · **两拍前视**：把三个"还有几拍"摊在 [0, 2] 窗口里，若两件事落在**同一拍**
    //       ⇒ 低优先者**延后一拍**，并打 `[DPSFSM-WHEEL] <bot> defer=<swp|vt> reason=collision`。
    //       优先级：**心爆 > 触 > 痛**（心爆是 4 拍一次的锚点；触有读条前置、拖一发就断 1.5 秒；
    //       痛是瞬发，晚一拍只少一跳 —— "宁可少一跳也不撞车"正是站长口径）。
    //     · 拍里没排到东西 ⇒ 填充：先 **灭**（带反伤守卫），再 **精神鞭笞**（引导 3s，占 2 拍）。
    //     · 仇恨到 Throttle/Wait ⇒ **渐隐（fade）**占这一拍：暗牧是仇恨大户，用一个空 GCD
    //       换后面能多打的 GCD，比硬打一下更值（`fade_throttle=1` 可关）。
    //     · 多目标：上限 `multidot_max`(5)、**优先血量最多的**；铺不铺看"每 GCD 收益"
    //       （见 ShadowMultiDot 的判据与它记录的取舍）。
    // ==========================================================================

    // 一拍里"该发生什么"的判定结果（纯数据：不碰世界、不改状态）。
    struct ShadowWheel
    {
        int  gcdMs = 1500;          // 一拍多长（TP: gcd_ms）
        int  vtCastMs = 1500;       // 触的**实际**读条（按当前急速算）
        int  swpRemain = 0;         // 目标身上"痛"的剩余毫秒（0 = 没挂/已到期）
        int  vtRemain = 0;          // 目标身上"触"的剩余毫秒
        int  mbRemain = 0;          // 心爆还差多少毫秒可用（我方记账）
        int  swpBeats = 0;          // 距"痛到期"还有几拍
        int  vtBeats = 0;           // 距"该起手触"还有几拍
        int  mbBeats = 0;           // 距心爆可用还有几拍
        bool swpDue = false;        // 本拍该续痛
        bool vtDue = false;         // 本拍该起手触
        bool mbDue = false;         // 本拍该打心爆
        bool deferSwp = false;      // 前视判定：本拍让给别的动作（撞车）
        bool deferVt = false;
    };

    // 拍钟推进。`spBeatAt` = "下一拍开始的毫秒时刻"。
    //   注：`WorldTimer::getMSTimeDiff` 是 uint32，异常跳变（回绕/时钟源切换）时按"对齐"处理，
    //   绝不让 `spBeat` 一次跳几十拍 —— 那样前视窗口就失去意义了。
    void ShadowAdvanceBeat(BotMem* m, uint32 safeNow, int gcdMs)
    {
        if (!m->spBeatAt || gcdMs <= 0)
        {
            m->spBeatAt = safeNow;
            m->spBeat = 0;
            return;
        }
        int32 const elapsed = int32(WorldTimer::getMSTimeDiff(m->spBeatAt, safeNow));
        if (elapsed < 0 || elapsed > gcdMs * 4)
        {
            m->spBeatAt = safeNow;      // 停过/换场景：对齐，**不补拍**
            return;
        }
        while (int32(WorldTimer::getMSTimeDiff(m->spBeatAt, safeNow)) >= gcdMs)
        {
            m->spBeatAt += uint32(gcdMs);
            ++m->spBeat;
        }
    }

    // 心爆的真实 CD（毫秒）：模板 `CategoryRecoveryTime` 经 `SPELLMOD_COOLDOWN`（强化心爆）修正，
    //   与核心 `Player::AddCooldown` 走的是同一条判据（见 Player.cpp → AddCooldown 的
    //   "blizzlike code for choosing which is recTime > categoryRecTime"）。
    //   `mb_cd_ms` > 0 时直接用它（调参/对照用）；读不到模板才退回 6000。
    int ShadowMindBlastCdMs(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        double const forced = TP(m, "mb_cd_ms", 0.0);
        if (forced > 0.0)
            return int(forced);
        uint32 const id = SpellId(ai, s.bot, m, "mind blast", false);
        SpellEntry const* e = id ? sServerFacade.LookupSpellInfo(id) : nullptr;
        if (!e)
            return 6000;
        int32 cd = 6000;
        if (e->CategoryRecoveryTime)
        {
            cd = int32(e->CategoryRecoveryTime);
            if (s.bot)
                s.bot->ApplySpellMod(id, SPELLMOD_COOLDOWN, cd, false);
        }
        if (cd < 1000 || cd > 60000)
            cd = 6000;
        return int(cd);
    }

    // 把三个时间点摊成"还有几拍"，再做两拍前视（结果只写进 ShadowWheel，不改 BotMem 的决策状态）。
    void ShadowPlanBeat(PlayerbotAI* ai, BotMem* m, Snapshot& s, ShadowWheel& w)
    {
        uint32 const nowMs = WorldTimer::getMSTime();
        uint32 const safeNow = nowMs ? nowMs : 1;

        w.gcdMs = int(TP(m, "gcd_ms", 1500.0));
        if (w.gcdMs < 500)
            w.gcdMs = 500;

        w.vtCastMs = CastMsWithMod(ai, m, s, "vampiric touch", w.gcdMs);
        if (w.vtCastMs <= 0)
            w.vtCastMs = w.gcdMs;

        ShadowAdvanceBeat(m, safeNow, w.gcdMs);

        uint32 const swpId = SpellId(ai, s.bot, m, "shadow word: pain", false);
        uint32 const vtId = SpellId(ai, s.bot, m, "vampiric touch", false);
        uint32 const mbId = SpellId(ai, s.bot, m, "mind blast", false);

        // ① 两个 DoT：读**目标身上**的实测剩余（任意 rank，见 TargetAuraRemain）
        w.swpRemain = swpId ? TargetAuraRemain(ai, s.bot, m, "shadow word: pain", s.target) : 0;
        w.vtRemain = vtId ? TargetAuraRemain(ai, s.bot, m, "vampiric touch", s.target) : 0;

        // ② 心爆：我方记账。记账说"该好了"但核心说不行（真实 CD 更长 / 天赋没点）⇒
        //    至少再等一拍，避免每 tick 都白试一次。
        w.mbRemain = 0;
        if (m->spMbReadyAt)
        {
            int32 const d = int32(m->spMbReadyAt) - int32(safeNow);
            w.mbRemain = d > 0 ? d : 0;
        }
        bool const mbReady = mbId && Ready(s.bot, mbId);
        if (mbId && w.mbRemain <= 0 && !mbReady)
        {
            m->spMbReadyAt = safeNow + uint32(w.gcdMs);
            w.mbRemain = w.gcdMs;
        }

        auto beatsTo = [&w](int remainMs) -> int
        {
            if (remainMs <= 0)
                return 0;
            return (remainMs + w.gcdMs - 1) / w.gcdMs;
        };

        // 触的起手拍 = 到期 − 读条 − (dot_lead_gcd − 1) 个 GCD。
        //   `dot_lead_gcd=1`（默认）就是"把读条本身当成唯一的提前量"：正好落在旧触到期的瞬间。
        int const leadGcd = int(TP(m, "dot_lead_gcd", 1.0));
        int const vtLeadMs = w.vtCastMs + (leadGcd > 1 ? (leadGcd - 1) * w.gcdMs : 0);

        w.swpBeats = beatsTo(w.swpRemain);
        w.vtBeats = beatsTo(w.vtRemain - vtLeadMs);
        w.mbBeats = beatsTo(w.mbRemain);

        w.swpDue = (swpId != 0) && (w.swpRemain <= 0);      // 严格等到期之后
        w.vtDue = (vtId != 0) && (w.vtRemain <= vtLeadMs);  // 提前一个读条起手
        w.mbDue = (mbId != 0) && (w.mbRemain <= 0) && mbReady;

        // ③ **两拍前视**：只修 [0, 2] 窗口里的"同一拍撞车"。
        //    优先级 心爆 > 触 > 痛 —— 低优先者延后一拍（下一拍立刻补，只少一跳）。
        int const mbBeat = w.mbDue ? 0 : w.mbBeats;
        int const vtBeat = w.vtDue ? 0 : w.vtBeats;
        int const swpBeat = w.swpDue ? 0 : w.swpBeats;
        bool const mbFirst = TP(m, "mb_first", 1.0) >= 1.0;
        int const look = 2;

        auto sameBeat = [look](int a, int b)
        {
            return a <= look && b <= look && a == b;
        };

        // 心爆的那一拍不能被 DoT 占掉（站长口径："不许卡在心爆的拍上"）
        if (mbFirst && mbId && sameBeat(mbBeat, swpBeat))
            w.deferSwp = true;
        if (mbFirst && mbId && sameBeat(mbBeat, vtBeat))
            w.deferVt = true;
        // 两个 DoT 自己撞车：触先（有读条前置，拖一发就断档），痛让一拍（瞬发，晚一拍只少一跳）
        if (sameBeat(vtBeat, swpBeat))
            w.deferSwp = true;
    }

    // 目标身上"暗牧这一套"DoT 已挂几个（只给 `[DPSFSM-PRIEST]` 的 dots= 用，不参与决策）。
    int ShadowDotCount(PlayerbotAI* ai, Player* bot, BotMem* m, Unit* u)
    {
        if (!u)
            return 0;
        static char const* kDots[] = { "shadow word: pain", "vampiric touch", "devouring plague" };
        int n = 0;
        for (char const* d : kDots)
            if (SpellId(ai, bot, m, d, false) && AuraStacks(ai, bot, m, d, u) > 0)
                ++n;
        return n;
    }

    // 一个 DoT 在目标身上"还能跳几下"：有 aura 用**实测剩余 / 实测跳间隔**，
    //   没 aura 按"满时长 / TP(dot_tick_ms)"（默认 3000ms 一跳，读不到就按这个）。
    int ShadowDotTicksLeft(PlayerbotAI* ai, BotMem* m, Snapshot& s, char const* name, Unit* u)
    {
        int const fallbackTick = int(TP(m, "dot_tick_ms", 3000.0));
        uint32 const id = SpellId(ai, s.bot, m, name, false);
        int period = fallbackTick ? fallbackTick : 3000;
        if (id && u)
        {
            uint32 const first = sSpellMgr.GetFirstSpellInChain(id);
            for (uint32 cur = first ? first : id; cur; cur = sSpellMgr.GetNextSpellInChain(cur))
            {
                SpellAuraHolder* holder = u->GetSpellAuraHolder(cur);
                if (!holder)
                    continue;
                for (int i = 0; i < MAX_EFFECT_INDEX; ++i)
                {
                    Aura* aura = holder->GetAuraByEffectIndex(SpellEffectIndex(i));
                    if (!aura || !aura->GetModifier() || !aura->GetModifier()->periodictime)
                        continue;
                    period = int(aura->GetModifier()->periodictime);
                    break;
                }
                break;
            }
        }
        int remain = TargetAuraRemain(ai, s.bot, m, name, u);
        if (remain <= 0)
        {
            SpellEntry const* e = id ? sServerFacade.LookupSpellInfo(id) : nullptr;
            int const full = e ? int(GetSpellDuration(e)) : 0;
            remain = full > 0 ? full : 15000;
        }
        if (period <= 0)
            period = 3000;
        return remain / period;
    }

    // [2026-11-01 站长口径] 暗牧·多目标：`possible targets` 里**血量最多的前 multidot_max(5) 个**，
    //   超过 5 个不再扩（回头集火血量最高的）。排序用**绝对血量降序**：血最多的死得最慢 ⇒ DoT 跳得最多。
    //   铺不铺走**每 GCD 收益**判据（站长原话：剩余跳数 × 单跳 ÷ 占用 GCD > 填充技收益）。
    //   ⚠️ 记录这里的取舍（不编数字）：DoT 的单跳伤害能从目标身上的 aura 读到（`Modifier::m_amount`，
    //      含法伤），但**填充技**（心爆/鞭/灭）的"每 GCD 实际伤害"本模块读不到 ——
    //      没有伤害事件接口，只有施法次数（`botCasts`）。拿模板基础值去比含法伤的 DoT 每跳会
    //      系统性低估填充 ⇒ 反而错误地多铺。所以把"填充技一个 GCD"折算成 **DoT 跳数**：
    //      `multidot_fill_ticks`(默认 4)：**剩余跳数 > 它才铺**。这是这条判据唯一的自由参数，
    //      conf 里可调（`shadow:multidot_fill_ticks=N`），不是从真实伤害模型推出来的。
    bool ShadowMultiDot(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        int const cap = int(TP(m, "multidot_max", 5.0));
        float const fillTicks = float(TP(m, "multidot_fill_ticks", 4.0));
        if (cap <= 1 || !s.target)
            return false;
        if (s.pack30 < 2)               // 单目标不谈多目标：那条路归时间轮
            return false;

        std::vector<Unit*> list;
        CountHostilesNear(ai, s.bot, float(TP(m, "range_caster", 30.0)), &list);
        if (list.size() < 2)
            return false;
        std::sort(list.begin(), list.end(), [](Unit* a, Unit* b)
        {
            if (a->GetHealth() != b->GetHealth())
                return a->GetHealth() > b->GetHealth();
            return a->GetObjectGuid() < b->GetObjectGuid();
        });
        if (int(list.size()) > cap)
            list.resize(size_t(cap));

        for (Unit* u : list)
        {
            if (!u || u == s.target || !u->IsAlive())
                continue;               // 当前目标交给轮（那里有撞车/到期判据），这里只照顾别人
            for (int kind = 0; kind < 2; ++kind)
            {
                char const* name = kind ? "vampiric touch" : "shadow word: pain";
                if (!SpellId(ai, s.bot, m, name, false))
                    continue;           // 没学会（天赋没点）：跳过，不刷"没有可用技能"
                if (TargetAuraRemain(ai, s.bot, m, name, u) > 0)
                    continue;           // 已经挂着 ⇒ 不重铺（重复铺会顶掉，白读一个 GCD）
                int const ticks = ShadowDotTicksLeft(ai, m, s, name, u);
                if (float(ticks) <= fillTicks)
                    continue;           // 跳不满 ⇒ 这个 GCD 不如拿去打填充
                if (TryCast(ai, m, s, name, u))
                    return true;
            }
        }
        return false;
    }

    // 灭的反伤守卫（站长口径）：灭对**没死**的目标会反弹伤害给自己 ⇒
    //   只在"自身血量 > `swd_min_self_hp`(50%)"或"目标血量低于灭的单发伤害（能打死）"时才放。
    //   单发伤害取模板基础值（**不含法伤** ⇒ 保守：宁可晚一点进斩杀，也不把自己反死）。
    bool ShadowDeathOk(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        if (!s.target)
            return false;
        uint32 const id = SpellId(ai, s.bot, m, "shadow word: death", false);
        if (!id)
            return false;
        if (s.selfHp > float(TP(m, "swd_min_self_hp", 50.0)))
            return true;
        SpellEntry const* e = sServerFacade.LookupSpellInfo(id);
        if (!e)
            return false;
        int32 const dmg = e->EffectBasePoints[EFFECT_INDEX_0] + 1;
        if (dmg <= 0)
            return false;
        return int32(s.target->GetHealth()) <= dmg;
    }

    // 一个 GCD 的动作（Maintain / Aoe / Burst 共用的"拍"）。
    ai::dpsfsm::TickResult TickShadowBeat(PlayerbotAI* ai, BotMem* m, Snapshot& s,
        ThreatLevel threat, ShadowWheel& w)
    {
        using ai::dpsfsm::TickResult;

        uint32 const nowMs = WorldTimer::getMSTime();
        uint32 const safeNow = nowMs ? nowMs : 1;
        bool const mbFirst = TP(m, "mb_first", 1.0) >= 1.0;

        // ⓪ 本拍确实"因撞车而延后"⇒ 打一条 [DPSFSM-WHEEL]（同一拍只报一行，便于脚本验收轮在转）
        if (((w.deferSwp && w.swpDue) || (w.deferVt && w.vtDue)) && m->spWheelBeat != m->spBeat)
        {
            m->spWheelBeat = m->spBeat;
            sLog.outError("[DPSFSM-WHEEL] %s defer=%s reason=collision",
                s.bot->GetName(), w.deferSwp ? "swp" : "vt");
        }

        // ① 仇恨：Throttle/Wait ⇒ 渐隐占这一拍（暗牧是仇恨大户；省下的仇恨 = 后面能多打的 GCD）
        if (TP(m, "fade_throttle", 1.0) >= 1.0 && threat != ThreatLevel::Open
            && SpellId(ai, s.bot, m, "fade", false) && TryCastSelf(ai, m, s, "fade"))
        {
            m->spBeatAct = "fade";
            return TickResult::Acted;
        }

        // ② 心爆优先（mb_first=1）：这一拍排给心爆，同拍的 DoT 已在前视里让开
        if (mbFirst && w.mbDue && TryCast(ai, m, s, "mind blast", nullptr))
        {
            m->spMbReadyAt = safeNow + uint32(ShadowMindBlastCdMs(ai, m, s));
            m->spBeatAct = "mb";
            return TickResult::Acted;
        }

        // ③ 触：提前一个读条起手，让新触正好落在旧触到期的瞬间
        if (w.vtDue && !w.deferVt && TryCast(ai, m, s, "vampiric touch", nullptr))
        {
            m->spBeatAct = "vt";
            return TickResult::Acted;
        }

        // ④ 痛：严格等到期之后再续（瞬发；被前视判让路时晚一拍，只少一跳）
        if (w.swpDue && !w.deferSwp && TryCast(ai, m, s, "shadow word: pain", nullptr))
        {
            m->spBeatAct = "swp";
            return TickResult::Acted;
        }

        // ⑤ mb_first=0（A/B 用）：DoT 续满之后才排心爆
        if (!mbFirst && w.mbDue && TryCast(ai, m, s, "mind blast", nullptr))
        {
            m->spMbReadyAt = safeNow + uint32(ShadowMindBlastCdMs(ai, m, s));
            m->spBeatAct = "mb";
            return TickResult::Acted;
        }

        // ⑥ 多目标铺 DoT（每 GCD 收益判据；见 ShadowMultiDot 的取舍说明）
        if (ShadowMultiDot(ai, m, s))
        {
            m->spBeatAct = "multi";
            return TickResult::Acted;
        }

        // ⑦ 填充：先灭（带反伤守卫），守卫/CD 不过再精神鞭笞（引导 3 秒 = 占 2 拍）
        if (ShadowDeathOk(ai, m, s) && TryCast(ai, m, s, "shadow word: death", nullptr))
        {
            m->spBeatAct = "swd";
            return TickResult::Acted;
        }
        if (SpellId(ai, s.bot, m, "mind flay", false) && TryCast(ai, m, s, "mind flay", nullptr))
        {
            // 引导 3 秒 = 占 2 拍：把下一拍推后一个 GCD（否则轮会把引导中间当成空拍）
            m->spBeatAt += uint32(w.gcdMs);
            m->spBeatAct = "flay";
            return TickResult::Acted;
        }

        // ⑧ 这一拍什么都没放出去。**只有"GCD 还没转好"才算有意空着**（此时引擎也不该插）；
        //    其它原因（射程/目标非法/技能解析失败/被拒）一律交回引擎 —— Hold 会把引擎**完全挡住**，
        //    实测出现过"victim 正常、dist 17.5、45 秒 0 施法 0 伤害、eng 计数也不涨"的一轮，
        //    根因就是这里把结构性失败当成了"有意空着"。
        m->spBeatAct = "hold";
        if (TP(m, "sp_diag", 0.0) >= 1.0
            && (!m->spDiagAt || WorldTimer::getMSTimeDiff(m->spDiagAt, safeNow) >= 10000))
        {
            m->spDiagAt = safeNow;
            uint32 const swpId = SpellId(ai, s.bot, m, "shadow word: pain", false);
            uint32 const mbId = SpellId(ai, s.bot, m, "mind blast", false);
            SpellCastResult res = SPELL_FAILED_DONT_REPORT;
            bool can = false;
            if (swpId && s.target)
                can = ai->CanCastSpell(swpId, s.target, 0, false, nullptr, false, false, false, &res);
            sLog.outError("[DPSFSM-PRIEST-DIAG] %s tick=%u beat=%d dist=%.1f casting=%d "
                "ids(swp=%u mb=%u vt=%u flay=%u swd=%u) has(swp=%d) ready(mb=%d) cancast(swp)=%d res=%d "
                "tgt=%s tgtAlive=%d",
                s.bot->GetName(), m->tickCount, m->spBeat, s.dist, s.casting ? 1 : 0,
                swpId, mbId,
                SpellId(ai, s.bot, m, "vampiric touch", false),
                SpellId(ai, s.bot, m, "mind flay", false),
                SpellId(ai, s.bot, m, "shadow word: death", false),
                swpId ? (s.bot->HasSpell(swpId) ? 1 : 0) : 0,
                mbId ? (Ready(s.bot, mbId) ? 1 : 0) : 0,
                can ? 1 : 0, int(res),
                s.target ? s.target->GetName() : "-",
                (s.target && s.target->IsAlive()) ? 1 : 0);
        }
        // GCD 没转好 ⇒ 有意空着（Hold）；否则交回引擎（Decline），别把它挡在外面。
        return GcdActive(s.bot, SpellId(ai, s.bot, m, "mind blast", false))
            ? TickResult::Hold : TickResult::Decline;
    }

    // 状态转移：只看 Snapshot + TP（Prep 的形态门是唯一"读自己身上 aura"的地方）。
    State ShadowEval(PlayerbotAI* ai, BotMem* m, Snapshot& s, ThreatLevel threat)
    {
        if (!s.target)
            return State::Idle;
        // Prep：暗影形态是整张图的前提（不在形态里既没有 15% 暗影加成，技能表也不对）
        if (SpellId(ai, s.bot, m, "shadowform", false)
            && AuraStacks(ai, s.bot, m, "shadowform", s.bot) == 0)
            return State::Prep;
        if (s.power < float(TP(m, "recover_power", 10.0)))
            return State::Recover;
        if (s.dist > float(TP(m, "range_caster", 30.0)))
            return State::Approach;
        if (s.pack30 >= AoEPack(m))
            return State::Aoe;
        if (s.targetHp <= BurstHp(m) && threat != ThreatLevel::Wait)
            return State::Burst;
        return State::Maintain;
    }

    // [DPSFSM-PRIEST] 每 10 秒一条（WorldTimer 节流）：
    //   swp/vt/mb = 轮上的三个时间点（毫秒；0 = 已到期/可用），beats = 本拍动作，
    //   dots = 当前目标已挂的 DoT 数，mana/threat 便于切日志。
    void ShadowLogTick(PlayerbotAI* ai, BotMem* m, Snapshot& s, State st,
        ShadowWheel const& w, ThreatLevel threat, uint32 safeNow)
    {
        if (m->lastPriestLog && WorldTimer::getMSTimeDiff(m->lastPriestLog, safeNow) < 10000)
            return;
        m->lastPriestLog = safeNow;
        m->spDots = ShadowDotCount(ai, s.bot, m, s.target);
        sLog.outError("[DPSFSM-PRIEST] %s state=%s swp=%d vt=%d mb=%d beats=%s dots=%d mana=%.0f threat=%s",
            s.bot->GetName(), ai::dpsfsm::StateName(st), w.swpRemain, w.vtRemain, w.mbRemain,
            m->spBeatAct ? m->spBeatAct : "-", m->spDots, s.power, ThreatLevelName(threat));
    }

    ai::dpsfsm::TickResult TickShadowFsm(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        using ai::dpsfsm::TickResult;

        uint32 const nowMs = WorldTimer::getMSTime();
        uint32 const safeNow = nowMs ? nowMs : 1;

        // 读条/引导中一律不插技能（与 TickArcaneFsm 同一口径；CastChecked 也会因 GCD 拒绝）
        if (s.casting)
            return TickResult::Acted;

        // [2026-11-01 站长口径] **法术缓存体检已提到通用层**（`SpellId()` 上的 IdsHealthTick）：
        //   这里原来有一份"5 秒一次、检查 7 个暗牧技能"的专用体检（当时只救了暗牧）。
        //   同一个病存在于所有专精，所以搬走了 —— 通用层是轮转（一次一条）+ 负缓存即时修，
        //   对暗牧的效果不减：0 缓存那条 5 秒内就会被重解析，不再需要这份局部表。

        ThreatLevel const threat = EvalThreat(m, s);
        State st = ShadowEval(ai, m, s, threat);

        // 轮每 tick 都算一次：即使这一拍什么都不放，日志里的三个时间点也要是当前的。
        ShadowWheel w;
        ShadowPlanBeat(ai, m, s, w);

        TickResult result = TickResult::Decline;
        switch (st)
        {
            case State::Prep:
                if (TryCastSelf(ai, m, s, "shadowform"))
                {
                    m->spBeatAct = "form";
                    result = TickResult::Acted;
                }
                break;

            case State::Approach:
                result = TickResult::Decline;      // 走位归引擎
                break;

            case State::Recover:
                // 有蓝职业通用：蓝缺口 > potion_mana_deficit 先喝药水（瞬发、不打断），
                // 剩下的（喝水/魔杖）交回引擎 —— 与冰法/奥法同一口径。
                if (UseManaPotion(m, s))
                {
                    m->spBeatAct = "potion";
                    result = TickResult::Acted;
                }
                else if (CastFirst(ai, m, s, { "shoot" }))
                {
                    m->spBeatAct = "wand";
                    result = TickResult::Acted;
                }
                break;

            case State::Aoe:
                // 群架：先把**血量最多的前 multidot_max 个**铺上 DoT（每 GCD 收益达标才铺），
                //   铺完的拍回到与单体完全相同的轮（心爆/触/痛/灭/鞭）。
                if (ShadowMultiDot(ai, m, s))
                {
                    m->spBeatAct = "multi";
                    result = TickResult::Acted;
                }
                else
                    result = TickShadowBeat(ai, m, s, threat, w);
                break;

            case State::Burst:
                // 斩杀段：饰品 → 灭（守卫此时基本放行：目标血低）→ 轮
                if (UseTrinket(ai, m, s))
                {
                    m->spBeatAct = "trinket";
                    result = TickResult::Acted;
                }
                else if (ShadowDeathOk(ai, m, s) && TryCast(ai, m, s, "shadow word: death", nullptr))
                {
                    m->spBeatAct = "swd";
                    result = TickResult::Acted;
                }
                else
                    result = TickShadowBeat(ai, m, s, threat, w);
                break;

            case State::Idle:
            case State::Maintain:
            default:
                st = (st == State::Idle) ? State::Idle : State::Maintain;
                if (st == State::Idle)
                    result = TickResult::Decline;
                else
                    result = TickShadowBeat(ai, m, s, threat, w);
                break;
        }

        LogTransition(ai, m, s, st);
        ShadowLogTick(ai, m, s, st, w, threat, safeNow);
        return result;
    }

    // =====================================================================================
    // [2026-11-01 站长口径] 德鲁伊接进 mode=5（第十张职业图）：野性（Feral，猫）+ 平衡（Balance）。
    //   恢复（Restoration）**不做 DPS**：派发处把它也选进这张图、`TickDruidFsm` 里第一行就 Decline，
    //   这样 mode=5 的恢复德不会被 legacy `TickDruid` 的平衡循环误接管；
    //   mode=0/1/2/3/4 与 legacy `TickDruid` 一个字节都不动。
    //
    //   一、野性（猫）—— **能量 tick 相位的排程机**，不是优先级表：
    //     状态的核心不是"现在有多少能量"，而是"距离下一跳回能还有多久"（能量 2 秒一跳、20 点，
    //     见核心 `Player::Regenerate(POWER_ENERGY)`；`m_regenTimer` 是 protected，模块侧只能观测）。
    //     循环 = 变身（Power Shift：取消形态 + 立即变回猫，**单 tick 原子**；有激怒 Furor 时给 40 能量）
    //            → 在回能第一秒把技能打出去 → 下一跳回能前完成下一次变身。
    //     判据表（逐条对应站长口径）：
    //       · 撕碎 shred：**一跳不够就不等**（差 2 点要等两跳 ⇒ 直接变身，变身立刻给 40）；
    //         ⚠️ 口径写的是 42 能量，但**本服 TBC DBC 里 Shred = 60**（Mangle 45 / Rip 30 /
    //         Ferocious Bite 35）；代码一律读 `spell_template.ManaCost`（DrPowerCost），
    //         所以"等一跳 / 变身"的实际判据按 60 算（见文件末尾的判定式）。
    //       · 割裂 rip(30)：一跳够就走"等一跳"（比变身省一次蓝）；
    //       · 清晰预兆 clearcasting（免费技能）：**第二秒里不抢**，多等一跳对齐相位再打（不是白赚）；
    //       · 裂伤 mangle：队里有熊形态德鲁伊 ⇒ 熊坦的裂伤 debuff 已经在，自己不补；
    //         读不到就按"没有"处理；`mangle_self=1` 强制自己补、`=0` 强制不补；
    //       · 变身蓝耗：只要"还够一次变身"就继续变；不够就走 Recover 喝法力药水
    //         （原先只看"够不够一次变身"；[2026-11-01 实测] 新增 `powershift_min_mana_pct`
    //          （默认 35）：**变完还剩多少蓝**低于该比例就不再变身、改等一跳 —— 实测每次变身
    //          约 590 蓝，45 秒 10~11 次会把蓝抽干，必然进 Recover 喝药，dps 反而比 mode=3 低 5%）；
    //       · 都不行 ⇒ Hold 等 tick，并且每个 tick 都 `SetFacingToObject`（否则白字全 BAD_FACING）。
    //     星/终结技：割裂 5 星（`rip_refresh` 到期才补）；凶猛撕咬只在目标 ≤ `fb_hp`(25%) 用；
    //       rip 还在、血又不低时攒着（能量 ≥ `fb_dump_energy`(80) 才泄一手，避免回能溢出）。
    //     状态：Idle / Prep(猫形态) / Recover(蓝低且不能变身) / Approach(近战够不着) / Aoe(横扫)
    //           / Maintain(能量轮)。
    //     找背：`GetBehind()`（撕碎需要背面）；正面时不硬放 shred（会 BAD_FACING + 白进退避）。
    //
    //   二、平衡（鸟德）：
    //     形态 moonkin form（Prep 补）；单体 moonfire / insect swarm 按剩余时间续（`refresh_dot`）
    //     → starfire 填充（读条长，急速由核心算）；wrath 只在移动或蓝紧时用。
    //     force of nature 在爆发（目标 ≤ `burst_hp`）开；barkskin 不进循环（保命交给上层）。
    //     AOE：hurricane（引导，pack ≥ `aoe_pack`，走 `TryCastAt` —— 位置技必须有完整校验，
    //       见 TryCastAt 上面"平衡德 40 秒 0 伤害 = 飓风幽灵施法"的实测注释）
    //       + 目标身上 moonfire/insect swarm 铺。
    //     蓝量：`recover_power`(10) 以下喝药；鸟德不缺蓝（月火回蓝天赋），不做复杂回蓝逻辑。
    //
    //   可复用（不重新发明）：`BuildSnapshot`、`CastFirst/TryCast/TryCastSelf/TryCastAt`、
    //   `SpellIdAtRank`、`TargetAuraRemain`、`AuraStacks`、`TickResult{Acted,Hold,Decline}`、
    //   `EvalThreat`、`UseManaPotion`、`UseTrinket`、`TP`。
    // =====================================================================================

    // 自己身上有没有**某个名字**的 aura（按 spell_template 的名字逐条比，rank 无关）。
    //   为什么不用 AuraStacks/AuraRemain：`clearcasting`（清晰预兆的 proc）根本不在法术书里，
    //   `SpellId(..., "clearcasting")` 会解析成 0 ⇒ 那两个入口永远返回 0。
    bool DrHasAuraByName(Player* bot, char const* name)
    {
        if (!bot || !name)
            return false;
        for (auto const& itr : bot->GetSpellAuraHolderMap())
        {
            SpellAuraHolder* holder = itr.second;
            SpellEntry const* proto = holder ? holder->GetSpellProto() : nullptr;
            if (proto && proto->SpellName[0] && strcasecmp(proto->SpellName[0], name) == 0)
                return true;
        }
        return false;
    }

    // 有没有"激怒"（Furor）天赋：核心 `Aura::HandleAuraModShapeshift` 用的正是这条判据
    //   （被动 dummy aura 的 SpellIconID == 238）。没有这个天赋，变身会把能量清零却不给 40
    //   ⇒ **绝不为回能去变身**（那是负收益）。
    bool DrHasFuror(Player* bot)
    {
        if (!bot)
            return false;
        for (Aura* aura : bot->GetAurasByType(SPELL_AURA_DUMMY))
            if (aura && aura->GetSpellProto() && aura->GetSpellProto()->SpellIconID == 238)
                return true;
        return false;
    }

    // 法术的**真实**资源消耗：`manaCost` 对能量/怒气/法力是同一个字段（`Spell::CalculatePowerCost`），
    //   形态类还有 `ManaCostPercentage`（猫形态就是按基础法力百分比收）⇒ 只读 manaCost 会低估。
    int DrPowerCost(PlayerbotAI* ai, BotMem* m, char const* name, int def)
    {
        Player* bot = ai->GetBot();
        uint32 const id = SpellId(ai, bot, m, name, false);
        SpellEntry const* entry = id ? sServerFacade.LookupSpellInfo(id) : nullptr;
        if (!entry)
            return def;
        int cost = int(entry->manaCost);
        if (entry->powerType == POWER_MANA && entry->ManaCostPercentage)
            cost += int(uint32(entry->ManaCostPercentage) * bot->GetCreateMana() / 100);
        return cost > 0 ? cost : def;
    }

    // 能量跳相位（观测值）。核心没有 m_regenTimer 的公开接口，只能看"能量跳变"：
    //   本 tick 能量比上一 tick 多、且增量是一跳的量（5~50）⇒ 记一次回能跳。
    struct DrEnergyPhase
    {
        int  energy = 0;      // 当前能量
        int  remainMs = 0;    // 距**下一跳**回能的毫秒（0 = 相位未知/现在就够）
        int  periodMs = 2000; // 观测到的跳间隔
        bool known = false;   // 相位是否可信（长时间没观测到跳就置 false，别拿旧相位骗自己）
    };

    void DrObserveEnergy(BotMem* m, Snapshot& s, uint32 safeNow, DrEnergyPhase& out)
    {
        int const e = s.energy;
        if (m->drEnergyPrev >= 0 && e > m->drEnergyPrev)
        {
            int const delta = e - m->drEnergyPrev;
            // [2026-11-01 实测修] 只把"**一跳**的量"当回能跳：5~25（默认 20）。
            //   ① 猛虎之怒(+60)/药水这类大增量不算；
            //   ② **激怒变身给的 +40 也不算**（第一版把 40 当成了一跳，相位直接错位，
            //      实测观测到 2999ms/3420ms 的"假周期"）。
            //   ③ 变身前后 500ms 内的增量一律不认（消形态→回形态期间读数会跳）。
            bool const afterShift = m->drShiftAt
                && WorldTimer::getMSTimeDiff(m->drShiftAt, safeNow) < 500;
            if (delta >= 5 && delta <= 25 && !afterShift)
            {
                // [2026-11-01 实测修] **跳间隔不学观测值**：核心 `Player::Update` 是
                //   `m_regenTimer += diff; if (>= REGEN_TIME_FULL) RegenerateAll(...)` ⇒
                //   回能周期**恒为 2000ms**（Rate.Energy 只改每跳的量，不改间隔）。
                //   第一版用"两次观测的间隔"去学周期，结果把"变身清零 → 下一次真跳"的
                //   半截间隔（1554/2999ms）当成了周期 ⇒ 相位越算越偏。固定 2000。
                //   另外：距上次记跳不足 1200ms 的增量不认（那是同一跳被看见两次 / 零散回能），
                //   否则每 tick 都把 drTickAt 刷成"刚刚"，remain 永远是 2000。
                bool const fresh = !m->drTickAt
                    || WorldTimer::getMSTimeDiff(m->drTickAt, safeNow) >= 1200;
                if (fresh)
                    m->drTickAt = safeNow;
            }
        }
        m->drEnergyPrev = e;

        out.energy = e;
        out.periodMs = m->drTickPeriod > 0 ? m->drTickPeriod : 2000;
        out.known = false;
        out.remainMs = 0;
        if (!m->drTickAt)
            return;
        uint32 const since = WorldTimer::getMSTimeDiff(m->drTickAt, safeNow);
        // 3 个周期都没看到跳（能量满了 / 长时间没出手）⇒ 相位不可信，按"现在"处理
        if (since > uint32(out.periodMs) * 3)
            return;
        int const phase = int(since) % out.periodMs;
        out.remainMs = out.periodMs - phase;
        if (out.remainMs <= 0 || out.remainMs > out.periodMs)
            out.remainMs = out.periodMs;
        out.known = true;
    }

    // 队里有熊形态德鲁伊（熊坦）⇒ 熊坦的裂伤 debuff 与猫的裂伤是同一个减益效果，自己不补。
    //   `mangle_self=1` 强制自己补；`=0` 强制不补；不配（-1）= 自动。
    bool DrMangleSelf(PlayerbotAI* ai, BotMem* m)
    {
        double const forced = TP(m, "mangle_self", -1.0);
        if (forced >= 0.0)
            return forced >= 0.5;
        Player* bot = ai->GetBot();
        Group* group = bot->GetGroup();
        if (!group)
            return true;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->getSource();
            if (!member || member == bot || !sServerFacade.IsAlive(member))
                continue;
            if (member->getClass() != CLASS_DRUID)
                continue;
            uint8 const form = member->GetShapeshiftForm();
            if (form == FORM_BEAR || form == FORM_DIREBEAR)
                return false;
        }
        return true;
    }

    // 变身（Power Shift）：单 tick 原子动作 —— 取消形态 + 立即变回猫，**不拆成两个 tick**。
    //   与 DpsPolicy.inc 的 PKind::PowerShift 同一条实现（那边实测过："先取消、下个 tick 再变"
    //   等于 2 个 GCD，9 次变形 603 dps vs 0 次 723 dps）。代价只有法力 + 形态 1 秒冷却。
    bool DrPowerShift(PlayerbotAI* ai, BotMem* m, Snapshot& s, DrEnergyPhase const& p)
    {
        uint32 const catId = SpellId(ai, s.bot, m, "cat form", false);
        if (!catId || !s.bot->HasSpell(catId))
            return false;

        uint32 const nowMs = WorldTimer::getMSTime();
        uint32 const safeNow = nowMs ? nowMs : 1;
        // 形态有 1 秒冷却：刚变过就别再空放（假人里每 tick 都试会白吃施法失败退避）
        if (m->drShiftAt && WorldTimer::getMSTimeDiff(m->drShiftAt, safeNow) < 1000)
            return false;

        // [2026-11-01 实测结论] 猫形态的"原地重放"会被核心拒绝：**res=64 = SPELL_FAILED_NOT_SHAPESHIFT**
        //   （已在该形态里，`CheckCast` 只认"不在这个形态"时才允许施放）⇒ 必须先摘形态再放。
        //   但**两件事必须同时成立**才允许摘：
        //     ① `Ready()`：技能自身冷却 / 变形类 1 秒恢复已过；
        //     ② `!GcdActive()`：核心 `Spell::CheckCast` 的 strict GCD 判据（`HasGCD(entry)`，
        //        按 StartRecoveryCategory 查，和 `Ready()` 的全局 GCD 不是同一张表）。
        //   第一版只查 ① ⇒ 摘掉形态后 CastSpell 返回 **res=63 = SPELL_FAILED_NOT_READY**，
        //   形态没了、能量也没拿到，下一 tick 只能再花 590 蓝补形态（实测每轮 Cat Form 10 次、
        //   powershifts=0、DPS 掉到 554）。两道闸一起过了，摘→放 才是原子的。
        bool const ready = Ready(s.bot, catId) && !GcdActive(s.bot, catId);
        if (!ready)
        {
            // `dr_diag=1` 才打（2 秒一条）：取证"为什么没变身"——是 GCD/冷却挡的
            if (TP(m, "dr_diag", 0.0) >= 1.0
                && (!m->drDiagAt || WorldTimer::getMSTimeDiff(m->drDiagAt, safeNow) >= 2000))
            {
                m->drDiagAt = safeNow;
                sLog.outError("[DPSFSM-DRUID-DIAG] %s powershift blocked ready=0 energy=%d tick=%dms mana=%.0f",
                    s.bot->GetName(), p.energy, p.remainMs, s.power);
            }
            return false;
        }

        s.bot->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);
        SpellCastResult const res = s.bot->CastSpell(s.bot, catId, uint32(TRIGGERED_NONE));
        if (res != SPELL_CAST_OK)
        {
            // `dr_diag=1`：取证"摘了形态却变不回来"的结果码（不该再出现，出现就是回归）
            if (TP(m, "dr_diag", 0.0) >= 1.0
                && (!m->drDiagAt || WorldTimer::getMSTimeDiff(m->drDiagAt, safeNow) >= 2000))
            {
                m->drDiagAt = safeNow;
                sLog.outError("[DPSFSM-DRUID-DIAG] %s powershift cast-fail res=%d energy=%d mana=%.0f form=%u",
                    s.bot->GetName(), int(res), p.energy, s.power, uint32(s.bot->GetShapeshiftForm()));
            }
            return false;
        }

        time_t const now = time(nullptr);
        m->drShiftAt = safeNow;
        ++m->drShifts;
        ++m->castCount;
        m->castThisTick = true;
        m->lastOk[catId] = now;
        m->lastCastId = catId;
        PlayerbotAI::RecordSpellCast(s.bot->GetObjectGuid(), catId);
        sLog.outError("[DPSFSM-DRUID] %s powershift #%u energy=%d tick=%dms combo=%u mana=%.0f",
            s.bot->GetName(), m->drShifts, p.energy, p.remainMs, uint32(s.combo), s.power);
        return true;
    }

    // [DPSFSM-DRUID] 每 10 秒一条：
    //   猫：`spec=feral state= energy=<n> tick=<距下一跳ms> combo=<n> powershifts=<n> mana=<%>`
    //   鸟：`spec=balance state= dots=<n> mana=<%>`
    void DrLogTick(PlayerbotAI* ai, BotMem* m, Snapshot& s, State st, DrEnergyPhase const& p)
    {
        uint32 const nowMs = WorldTimer::getMSTime();
        uint32 const safeNow = nowMs ? nowMs : 1;
        if (m->drLastLog && WorldTimer::getMSTimeDiff(m->drLastLog, safeNow) < 10000)
            return;
        m->drLastLog = safeNow;

        if (m->spec == Spec::Feral)
        {
            // [2026-11-01 实测] `shiftGate=` = `powershift_min_mana_pct` 这道门的本 tick 判定
            //   （1 = 变完还剩够用的蓝、允许变身；0 = 蓝量门把变身挡住了，改走"等一跳"）。
            sLog.outError("[DPSFSM-DRUID] %s spec=feral state=%s energy=%d tick=%d combo=%u powershifts=%u mana=%.0f shiftGate=%d",
                s.bot->GetName(), StateName(st), s.energy, p.remainMs, uint32(s.combo), m->drShifts, s.power,
                m->drShiftGate);
        }
        else
        {
            int dots = 0;
            if (s.target)
            {
                if (TargetAuraRemain(ai, s.bot, m, "moonfire", s.target) > 0)     ++dots;
                if (TargetAuraRemain(ai, s.bot, m, "insect swarm", s.target) > 0) ++dots;
            }
            sLog.outError("[DPSFSM-DRUID] %s spec=balance state=%s dots=%d mana=%.0f",
                s.bot->GetName(), StateName(st), dots, s.power);
        }
    }

    ai::dpsfsm::TickResult TickFeralFsm(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        using ai::dpsfsm::TickResult;

        if (!s.target)
            return TickResult::Decline;

        Player* bot = s.bot;
        uint32 const nowMs = WorldTimer::getMSTime();
        uint32 const safeNow = nowMs ? nowMs : 1;

        // [2026-11-01 实测修] **坐着（喝水/休息）时必须先站起来**。
        //   假人连跑时上一轮结束后 bot 会坐下回蓝，而 SimScenario 只重置血/蓝/能量、不管站姿；
        //   核心 `Spell::CheckCast` 第一条就会判 SPELL_FAILED_NOT_STANDING(65) ⇒ 整轮 45 秒
        //   0 施法 0 伤害（实测 balance 每批第 2 轮必现；诊断行里 `res=65` 就是它）。
        //   站起来不是施法、不占 GCD，所以放在最前面、无条件做。
        if (!bot->IsStandState())
            bot->SetStandState(UNIT_STAND_STATE_STAND);

        // 先转向：Acted/Hold 时引擎整 tick 不跑（与惩戒骑同一口径）。
        //   猫的每一手都要判朝向，不转就是 BAD_FACING 白字全丢。
        if (s.target && s.target != bot)
            bot->SetFacingToObject(s.target);

        // 换目标 = 换一场战斗：只重置交战计时；累计 powershifts 保留（那是验收"真的变了"的数）
        if (s.target->GetObjectGuid() != m->drTarget)
        {
            m->drTarget = s.target->GetObjectGuid();
            m->drEngagedAt = safeNow;
        }
        if (!m->drEngagedAt)
            m->drEngagedAt = safeNow;

        DrEnergyPhase p;
        DrObserveEnergy(m, s, safeNow, p);

        // [2026-11-01 实测修] **激怒（Furor）检测必须在"已经在猫形态"时也做一次**：
        //   第一版只在 Prep 里查，而假人第二/第三轮开局 bot 往往还保持着上一轮的猫形态
        //   （SimScenario 只重置血/蓝/能量，不消形态）⇒ 永远进不到 Prep ⇒ `drFuror` 一直是 false
        //   ⇒ **整轮一次变身都没有**（实测 20 秒 powershifts=0，日志里 furor=0）。
        //   天赋是静态事实，查一次就够（`drFurorChecked`，跨轮保留）。
        if (!m->drFurorChecked)
        {
            m->drFuror = DrHasFuror(bot);
            m->drFurorChecked = true;
        }

        // ---- Prep：猫形态 ----
        if (bot->GetShapeshiftForm() != FORM_CAT)
        {
            if (TP(m, "dr_diag", 0.0) >= 1.0
                && (!m->drDiagAt || WorldTimer::getMSTimeDiff(m->drDiagAt, safeNow) >= 2000))
            {
                m->drDiagAt = safeNow;
                sLog.outError("[DPSFSM-DRUID-DIAG] %s form-loss form=%u aura=%d lastCast=%u energy=%d mana=%.0f auras=%u",
                    bot->GetName(), uint32(bot->GetShapeshiftForm()),
                    DrHasAuraByName(bot, "cat form") ? 1 : 0, m->lastCastId, s.energy, s.power,
                    uint32(bot->GetSpellAuraHolderMap().size()));
            }
            if (DrHasAuraByName(bot, "cat form"))
            {
                // 光环在、形态字段却没同步（核心形态切换的中间态）⇒ 别每 tick 重放
                //   （legacy 实测 40 秒刷了 140 次 Cat Form）
                if (TP(m, "dr_diag", 0.0) >= 1.0
                    && (!m->drDiagAt || WorldTimer::getMSTimeDiff(m->drDiagAt, safeNow) >= 2000))
                {
                    m->drDiagAt = safeNow;
                    sLog.outError("[DPSFSM-DRUID-DIAG] %s prep desync form=%u aura=1 energy=%d mana=%.0f auras=%u",
                        bot->GetName(), uint32(bot->GetShapeshiftForm()), s.energy, s.power,
                        uint32(bot->GetSpellAuraHolderMap().size()));
                }
                LogTransition(ai, m, s, State::Prep);
                DrLogTick(ai, m, s, State::Prep, p);
                return TickResult::Decline;
            }
            bool const ok = TryCastSelf(ai, m, s, "cat form");
            if (!ok && TP(m, "dr_diag", 0.0) >= 1.0
                && (!m->drDiagAt || WorldTimer::getMSTimeDiff(m->drDiagAt, safeNow) >= 2000))
            {
                m->drDiagAt = safeNow;
                sLog.outError("[DPSFSM-DRUID-DIAG] %s prep cast-fail form=%u aura=0 energy=%d mana=%.0f gdc=%d",
                    bot->GetName(), uint32(bot->GetShapeshiftForm()), s.energy, s.power,
                    Ready(bot, SpellId(ai, bot, m, "cat form", false)) ? 0 : 1);
            }
            LogTransition(ai, m, s, State::Prep);
            DrLogTick(ai, m, s, State::Prep, p);
            return ok ? TickResult::Acted : TickResult::Decline;
        }

        // 第一次确定 furor / 观测到的跳间隔：只打一条诊断（验收要知道"为什么不变身"）
        if (m->drFurorChecked && m->drTickAt && m->drFurorLoggedPeriod != m->drTickPeriod)
        {
            m->drFurorLoggedPeriod = m->drTickPeriod;
            sLog.outError("[DPSFSM-DRUID] %s furor=%d tickPeriod=%dms energy=%d tick=%dms mana=%.0f",
                bot->GetName(), m->drFuror ? 1 : 0, m->drTickPeriod, p.energy, p.remainMs, s.power);
        }

        // ---- 资源/距离 ----
        int const regen       = int(TP(m, "energy_tick", 20.0));
        int const shredCost   = DrPowerCost(ai, m, "shred", 42);
        int const ripCost     = DrPowerCost(ai, m, "rip", 30);
        int const fbCost      = DrPowerCost(ai, m, "ferocious bite", 35);
        int const mangleCost  = DrPowerCost(ai, m, "mangle (cat)", 40);
        int const swipeCost   = DrPowerCost(ai, m, "swipe (cat)", 45);
        int const shiftCost   = DrPowerCost(ai, m, "cat form", 400);
        // [2026-11-01 实测] **变身换能量的蓝量门**（`powershift_min_mana_pct`，默认 35）：
        //   实测 45 秒 10~11 次变身、每次约 590 蓝 ⇒ 蓝量必然见底、进 Recover 喝药，
        //   dps 反而比 mode=3 低 5%。这里估算"变完还剩多少蓝"，低于该比例就**不再变身**
        //   （改走"等一跳"分支），让蓝量不至于被变身抽干。
        //   判据与核心同一条口径：花费取 `DrPowerCost()`（`spell_template.ManaCost` +
        //   `ManaCostPercentage × GetCreateMana()`，也就是 `Spell::CalculatePowerCost` 的算法）。
        uint32 const maxMana = bot->GetMaxPower(POWER_MANA);
        int const curMana = int(bot->GetPower(POWER_MANA));
        int const manaAfter = curMana - shiftCost;
        int const shiftGatePct = int(TP(m, "powershift_min_mana_pct", 35.0));
        float const manaAfterPct = (maxMana && manaAfter > 0)
            ? 100.0f * float(manaAfter) / float(maxMana) : 0.0f;
        bool const manaShiftOk = maxMana && manaAfter >= 0
            && manaAfterPct >= float(shiftGatePct);
        m->drShiftGate = manaShiftOk ? 1 : 0;      // 日志 shiftGate=（A/B 用）

        bool const reach = bot->CanReachWithMeleeAttack(s.target);

        State st = State::Maintain;
        if (!reach)
            st = State::Approach;
        else if (m->drFuror && !manaShiftOk && s.energy < shredCost)
            st = State::Recover;          // 蓝低到连一次变身都不够，且能量打不出下一手
        else if (s.pack10 >= AoEPack(m))
            st = State::Aoe;

        TickResult result = TickResult::Decline;
        char const* want = nullptr;
        int wantCost = 0;
        bool needBehind = false;
        bool waitingForRip = false;

        if (st == State::Approach)
        {
            CloseToMelee(ai, m, s);       // 引擎的 reach melee 是零长度路径（见 CloseToMelee 注释）
            result = TickResult::Decline;
        }
        else if (st == State::Recover)
        {
            // [2026-11-01 站长口径] 登记为有意等资源（蓝低到不能变身、能量又打不出下一手）：
            //   猫每 tick 都 `SetFacingToObject`，Hold 期间朝向不会冻住。
            result = HoldWait(m, "shift-mana-wait");
            if (UseManaPotion(m, s))
                result = TickResult::Acted;
        }
        else if (st == State::Aoe)
        {
            want = "swipe (cat)";
            wantCost = swipeCost;
        }
        else
        {
            int const refresh = int(TP(m, "rip_refresh", 1500.0));
            bool const ripUp = TargetAuraRemain(ai, bot, m, "rip", s.target) > refresh;
            if (s.combo >= 5)
            {
                if (!ripUp)
                {
                    want = "rip";
                    wantCost = ripCost;
                }
                else if (s.targetHp <= float(TP(m, "fb_hp", 25.0)))
                {
                    want = "ferocious bite";
                    wantCost = fbCost;
                }
                else if (s.energy >= int(TP(m, "fb_dump_energy", 80.0)))
                {
                    // 5 星 + rip 还在 + 血不低：默认攒着等 rip 到期；只有能量快溢出才泄一手
                    want = "ferocious bite";
                    wantCost = fbCost;
                }
                else
                    waitingForRip = true;
            }
            else
            {
                uint32 const mangleId = SpellId(ai, bot, m, "mangle (cat)", false);
                int const mangleRefresh = int(TP(m, "mangle_refresh", 3000.0));
                bool const mangleNeed = mangleId && DrMangleSelf(ai, m)
                    && TargetAuraRemain(ai, bot, m, "mangle (cat)", s.target) < mangleRefresh;
                if (mangleNeed)
                {
                    want = "mangle (cat)";
                    wantCost = mangleCost;
                }
                else
                {
                    want = "shred";
                    wantCost = shredCost;
                    needBehind = true;
                }
            }
        }

        if (st == State::Aoe || st == State::Maintain)
        {
            bool const cc = DrHasAuraByName(bot, "clearcasting");
            int const period = p.periodMs > 0 ? p.periodMs : 2000;
            bool const firstSecond = !p.known || p.remainMs > period / 2;
            bool const afford = want && (s.energy >= wantCost || cc);

            if (want && needBehind && !bot->IsFacingTargetsBack(s.target))
            {
                GetBehind(ai, m, s);   // 只走位，不占 GCD；正面硬放 shred 会 BAD_FACING
                result = HoldWait(m, "get-behind");
            }
            else if (afford)
            {
                if (cc && !firstSecond)
                {
                    // [2026-11-01 站长口径] 清晰预兆这一发免费技**不在第二秒抢**：
                    //   多等一跳把它对齐到回能相位（否则后面每一手的 GCD 都会与跳相位错开）。
                    result = HoldWait(m, "cc-phase-wait");
                }
                else if (TryCast(ai, m, s, want, nullptr))
                    result = TickResult::Acted;
                else
                    // [2026-11-01 站长口径] **施法失败不登记**：这是"没放出去"而不是"有意等"，
                    //   交给 Tick() 的通用兜底降级成 Decline，让引擎能走位/喝水（否则一发
                    //   反复被拒的技能会把整轮锁成 0 施法）。
                    result = TickResult::Hold;
            }
            else if (waitingForRip)
            {
                // 5 星但 rip 还没到期、血也不低：攒星攒能量，等 rip 掉了再补（硬放 bite 会白扔星）
                result = HoldWait(m, "rip-wait");
            }
            else if (want)
            {
                // [2026-11-01 站长口径] "等一跳还是变身"的判定（实测按本服 DBC 的能量消耗）：
                //   · 一跳就够（energy + 20 >= cost）⇒ **等一跳**（比变身省一次蓝；
                //     裂伤 rip(30) 的"能量接近 30 可以多等一跳"就是这一条）；
                //   · 一跳不够 ⇒ 只有"变身能让自己更快凑够这一手"才变：
                //     变身净增 = max(0, 激怒 40 − 当前能量)，且 当前 + 净增 + 20 >= cost；
                //   · 变身也凑不够 ⇒ 老实等（绝不空放变身，那只会白花法力）。
                //   ⚠️ 本服 TBC 的 Shred 是 **60 能量**（spell_template.ManaCost，不是口径里的 42），
                //      Mangle 45、Rip 30、Ferocious Bite 35；所以"差 2 点等两跳"的典型场景
                //      在本服不会出现，规则按上面的通用式落地（见回报里的说明）。
                bool const oneTickEnough = (s.energy + regen >= wantCost);
                int const furorEnergy = int(TP(m, "furor_energy", 40.0));
                int const gain = furorEnergy > s.energy ? furorEnergy - s.energy : 0;
                bool const shiftHelps = gain > 0 && (s.energy + gain + regen >= wantCost);

                if (oneTickEnough)
                    result = HoldWait(m, "energy-tick-wait");   // 下一跳就够：等
                else if (m->drFuror && manaShiftOk && shiftHelps
                    && DrPowerShift(ai, m, s, p))
                    result = TickResult::Acted;
                else
                    // [2026-11-01 站长口径] 登记为有意等资源：没有激怒 / 蓝被 `powershift_min_mana_pct`
                    //   的门挡住 / 变了也不够 —— 一律等自然回能（绝不空放变身，那只会白花法力）。
                    result = HoldWait(m, "energy-wait");

                // `dr_diag=1`：取证"等一跳还是变身"这一判据（2 秒一条，默认关）
                if (TP(m, "dr_diag", 0.0) >= 1.0
                    && (!m->drDiagAt || WorldTimer::getMSTimeDiff(m->drDiagAt, safeNow) >= 2000))
                {
                    m->drDiagAt = safeNow;
                    sLog.outError("[DPSFSM-DRUID-DIAG] %s wheel want=%s cost=%d energy=%d tick=%dms "
                        "oneTick=%d furor=%d gain=%d shiftHelps=%d manaOk=%d manaAfter=%.0f%% shiftGate=%d shifts=%u behind=%d",
                        bot->GetName(), want ? want : "-", wantCost, s.energy, p.remainMs,
                        oneTickEnough ? 1 : 0, m->drFuror ? 1 : 0, gain, shiftHelps ? 1 : 0,
                        manaShiftOk ? 1 : 0, double(manaAfterPct), m->drShiftGate, m->drShifts,
                        (s.target && bot->IsFacingTargetsBack(s.target)) ? 1 : 0);
                }
            }
            else
                result = TickResult::Hold;
        }

        LogTransition(ai, m, s, st);
        DrLogTick(ai, m, s, st, p);
        return result;
    }

    ai::dpsfsm::TickResult TickBalanceFsm(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        using ai::dpsfsm::TickResult;

        if (!s.target)
            return TickResult::Decline;

        Player* bot = s.bot;
        DrEnergyPhase p;   // 鸟德不用能量相位，只为共用 DrLogTick 的接口
        p.energy = s.energy;

        // [2026-11-01 实测修] 同野性：坐着的时候核心一律判 SPELL_FAILED_NOT_STANDING(65)。
        //   假人连跑的第 2 轮整轮 0 施法就是这个（诊断行 `res=65`），先站起来再说。
        if (!bot->IsStandState())
            bot->SetStandState(UNIT_STAND_STATE_STAND);

        // [2026-11-01 实测修] **换目标 = 换一场战斗：上一轮残留的读条/引导必须清掉**。
        //   假人连跑时同一只 bot 会被复用，上一轮收尾时若正处在引导（飓风 10 秒）里，
        //   旧 Spell 对象可能还挂在 `CURRENT_CHANNELED_SPELL` 上 ⇒ 本函数会一直早退。
        //   只在"换了目标"时动手（正常战斗里换目标也应当放弃旧目标上的读条）。
        if (s.target->GetObjectGuid() != m->drTarget)
        {
            m->drTarget = s.target->GetObjectGuid();
            m->drEngagedAt = WorldTimer::getMSTime();
            if (s.casting)
                bot->InterruptNonMeleeSpells(false);
        }

        if (s.casting)
        {
            // 新目标刚清过读条还残留（或者这一轮确实在读条）：本 tick 不插技能。
            // `dr_diag=1` 时留痕，便于区分"正常读条"和"卡死的读条"。
            if (TP(m, "dr_diag", 0.0) >= 1.0
                && (!m->drDiagAt || WorldTimer::getMSTimeDiff(m->drDiagAt,
                    WorldTimer::getMSTime() ? WorldTimer::getMSTime() : 1) >= 2000))
            {
                m->drDiagAt = WorldTimer::getMSTime() ? WorldTimer::getMSTime() : 1;
                Spell* gen = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
                Spell* chn = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
                sLog.outError("[DPSFSM-DRUID-DIAG] %s balance casting=1 generic=%u channel=%u mana=%.0f casts=%u",
                    bot->GetName(), gen && gen->m_spellInfo ? gen->m_spellInfo->Id : 0,
                    chn && chn->m_spellInfo ? chn->m_spellInfo->Id : 0, s.power, m->castCount);
            }
            return TickResult::Acted;
        }

        // ---- Prep：枭兽形态（没这个天赋的 bot 就在法师形态打，循环照旧） ----
        if (SpellId(ai, bot, m, "moonkin form", false) && bot->GetShapeshiftForm() != FORM_MOONKIN)
        {
            if (DrHasAuraByName(bot, "moonkin form"))
            {
                LogTransition(ai, m, s, State::Prep);
                DrLogTick(ai, m, s, State::Prep, p);
                return TickResult::Decline;
            }
            bool const ok = TryCastSelf(ai, m, s, "moonkin form");
            LogTransition(ai, m, s, State::Prep);
            DrLogTick(ai, m, s, State::Prep, p);
            return ok ? TickResult::Acted : TickResult::Decline;
        }

        ThreatLevel const threat = EvalThreat(m, s);
        int const refresh = int(TP(m, "refresh_dot", 1500.0));
        bool const reach = s.dist <= float(TP(m, "range_caster", 30.0));
        bool const burstWindow = (s.targetHp <= float(TP(m, "burst_hp", 20.0)));

        State st = State::Maintain;
        if (!reach)
            st = State::Approach;
        else if (s.power < float(TP(m, "recover_power", 10.0)))
            st = State::Recover;
        else if (s.pack30 >= AoEPack(m))
            st = State::Aoe;
        else if (burstWindow && threat != ThreatLevel::Wait)
            st = State::Burst;

        TickResult result = TickResult::Decline;
        switch (st)
        {
            case State::Approach:
                result = TickResult::Decline;   // 走位归引擎
                break;

            case State::Recover:
                if (UseManaPotion(m, s))
                    result = TickResult::Acted;
                else if (CastFirst(ai, m, s, { "shoot" }))
                    result = TickResult::Acted;
                else
                    result = TickResult::Hold;
                break;

            case State::Aoe:
                // 先铺两个瞬发 DoT（目标身上），再引导飓风 —— 位置技必须走 TryCastAt 的完整校验
                if (TargetAuraRemain(ai, bot, m, "moonfire", s.target) < refresh
                    && TryCast(ai, m, s, "moonfire", nullptr))
                    result = TickResult::Acted;
                else if (TargetAuraRemain(ai, bot, m, "insect swarm", s.target) < refresh
                    && TryCast(ai, m, s, "insect swarm", nullptr))
                    result = TickResult::Acted;
                else if (TryCastAt(ai, m, s, "hurricane",
                    s.target->GetPositionX(), s.target->GetPositionY(), s.target->GetPositionZ()))
                    result = TickResult::Acted;
                else if (CastFirst(ai, m, s, { "starfire" }))
                    result = TickResult::Acted;
                else
                    result = TickResult::Hold;
                break;

            case State::Burst:
                if (UseTrinket(ai, m, s))
                {
                    result = TickResult::Acted;
                    break;
                }
                if (CastFirst(ai, m, s, { "force of nature" }))
                {
                    result = TickResult::Acted;
                    break;
                }
                // 落回单体循环（不 break 出 switch 的写法：与上面两段同构）
                if (TargetAuraRemain(ai, bot, m, "moonfire", s.target) < refresh
                    && TryCast(ai, m, s, "moonfire", nullptr))
                    result = TickResult::Acted;
                else if (TargetAuraRemain(ai, bot, m, "insect swarm", s.target) < refresh
                    && TryCast(ai, m, s, "insect swarm", nullptr))
                    result = TickResult::Acted;
                else if (CastFirst(ai, m, s, { "starfire" }))
                    result = TickResult::Acted;
                else
                    result = TickResult::Hold;
                break;

            case State::Maintain:
            case State::Idle:
            default:
                // 月火 → 虫群 → 填充：starfire 读条长；移动中 / 蓝紧时改 wrath（读条短）
                if (TargetAuraRemain(ai, bot, m, "moonfire", s.target) < refresh
                    && TryCast(ai, m, s, "moonfire", nullptr))
                    result = TickResult::Acted;
                else if (TargetAuraRemain(ai, bot, m, "insect swarm", s.target) < refresh
                    && TryCast(ai, m, s, "insect swarm", nullptr))
                    result = TickResult::Acted;
                else if (s.moving && CastFirst(ai, m, s, { "wrath" }))
                    result = TickResult::Acted;
                else if (s.power < float(TP(m, "wrath_mana", 30.0))
                    && CastFirst(ai, m, s, { "wrath" }))
                    result = TickResult::Acted;
                else if (CastFirst(ai, m, s, { "starfire" }))
                    result = TickResult::Acted;
                else if (CastFirst(ai, m, s, { "wrath" }))
                    result = TickResult::Acted;
                else
                    result = TickResult::Hold;
                break;
        }

        LogTransition(ai, m, s, st);
        DrLogTick(ai, m, s, st, p);

        // [2026-11-01 实测取证] `dr_diag=1`：当这一 tick **什么都没放出去**时，把"为什么"留痕。
        //   背景：假人连跑时出现过整轮 45 秒 casts=0/dps=0（victim、距离、蓝量、存活全部正常），
        //   而 `TryCast` 内部不暴露结果码 ⇒ 这里用 `CanCastSpell` 再问一次，只为日志、不改行为。
        if (result != TickResult::Acted && TP(m, "dr_diag", 0.0) >= 1.0)
        {
            uint32 const nowMsD = WorldTimer::getMSTime();
            uint32 const safeNowD = nowMsD ? nowMsD : 1;
            if (!m->drDiagAt || WorldTimer::getMSTimeDiff(m->drDiagAt, safeNowD) >= 2000)
            {
                m->drDiagAt = safeNowD;
                uint32 const sf = SpellId(ai, bot, m, "starfire", false);
                uint32 const mf = SpellId(ai, bot, m, "moonfire", false);
                SpellCastResult cr = SPELL_FAILED_DONT_REPORT;
                bool const can = ai->CanCastSpell(sf, s.target, 0, true, nullptr, false, false, false, &cr);
                sLog.outError("[DPSFSM-DRUID-DIAG] %s balance no-cast state=%s sf=%u has=%d ready=%d gcd=%d "
                    "can=%d res=%d mf=%u mfReady=%d mana=%.0f casting=%d dist=%.1f",
                    bot->GetName(), StateName(st), sf, (sf && bot->HasSpell(sf)) ? 1 : 0,
                    (sf && Ready(bot, sf)) ? 1 : 0, (sf && GcdActive(bot, sf)) ? 1 : 0,
                    can ? 1 : 0, int(cr), mf, (mf && Ready(bot, mf)) ? 1 : 0,
                    s.power, s.casting ? 1 : 0, s.dist);
            }
        }
        return result;
    }
    ai::dpsfsm::TickResult TickDruidFsm(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        using ai::dpsfsm::TickResult;
        if (m->spec == Spec::Feral)
            return TickFeralFsm(ai, m, s);
        if (m->spec == Spec::Balance)
            return TickBalanceFsm(ai, m, s);
        return TickResult::Decline;
    }

    // ==========================================================================
    // [2026-11-01 站长口径] **HealCore —— 四治疗共用的一套治疗状态机**
    // --------------------------------------------------------------------------
    //   状态：Idle → Prep(自身 buff) → Dispel(分配的驱散，最高优先) → Emergency → Tank
    //         → Raid(团队) → Recover(蓝 < recover_mana 只放最省的)
    //   四治疗（神圣牧/戒律牧/神圣骑/恢复萨/恢复德）**只差一张配置表**：`HealCfgFor()`。
    //   现在只填了神圣牧一行，其余返回 nullptr ⇒ TickHealFsm 直接 Decline（不改变 legacy 行为）。
    //
    //   三条站长口径在这里落地：
    //   ① **预读**：用目标最近 `heal_rate_window_ms`(3000) 的**受伤速率**外推
    //      `预计落地血量 = hp - rate × 读条`；落地时会低于阈值 ⇒ **现在起手**。
    //      读不到速率（窗口没满/一直没掉血）⇒ 退化成"掉血才加"（逐 tick 比阈值）。
    //   ② **治疗术降级（rank）**：按缺口选"能覆盖缺口的**最小**等级"往上试
    //      （缺口 = 目标血量 - 目标血量目标值；`rank_safety` < 1 表示理论值先打折再比，
    //      默认 1.0 = 不打折）。用 `SpellIdAtRank` + 每等级的**实际**治疗量
    //      （`EffectBasePoints × 系数 + 治疗强度`，见 HealTheory()）。
    //   ③ **救急**：目标 < `emergency_hp`(35%) ⇒ 先交大 CD（守护之魂/痛苦压制，若学会），不省蓝。
    //
    //   ⚠️ 本服给战士坦克套盾**不会断怒气**（核心自定义行为，DESIGN_dps_fsm_graphs.md §12），
    //      所以 `power word: shield` 在坦克身上可以正常用，不需要回避。
    // ==========================================================================
    struct HealCfg
    {
        char const* tag;          // 专精名（日志用）
        char const* renew;        // 常驻 HoT
        char const* bigHeal;      // 大治疗（按 rank 降级/升级的那一个）
        char const* flashHeal;    // 救急
        char const* shield;       // 盾（可空）
        char const* groupHeal;    // 群体治疗（小队集中掉血）
        char const* groupHeal2;   // 第二个群体治疗（可空）
        char const* linkHeal;     // 自己与队友同时告急（可空）
        char const* dispelMagic;  // 驱散魔法（可空）
        char const* cureDisease;  // 驱散疾病（可空）
        char const* emergencyA;   // 大 CD（守护之魂 / 痛苦压制，可空）
        char const* emergencyB;
        char const* selfBuffA;    // 自身 buff（心灵之火）
        char const* selfBuffB;    // 团队 buff（真言术：韧）
    };

    // [2026-11-01 站长口径] 神圣牧配置表。**只此一行**：其余治疗专精还没有表 ⇒ Decline。
    static HealCfg const kHealPriestHoly =
    {
        "holy priest",
        "renew",                    // 坦克：renew 常驻
        "greater heal",             // 坦克：低级维持 → 预判尖刺用高级（rank 由缺口选）
        "flash heal",               // 濒死
        "power word: shield",       // 濒死（本服不断怒气，可正常用）
        "prayer of healing",        // 小队集中掉血
        "circle of healing",        // 若学会（41 点天赋，没点就解析成 0，自动跳过）
        "binding heal",             // 自己与队友同时告急
        "dispel magic",
        "cure disease",
        "guardian spirit",          // 大 CD（天赋，没学会就算了）
        "pain suppression",         // 戒律系顺带学会也能用（神圣牧一般不点，解析 0 即跳过）
        "inner fire",
        "power word: fortitude",
    };

    // 专精 → 配置表。**判职业**：`Spec::Holy` 同时是"圣骑士·神圣"和"牧师·神圣"，
    //   只看 spec 会把神圣骑也塞进这套牧师技能表（解析全 0 ⇒ 整场空转）。见 DpsFsm.h 说明。
    static HealCfg const* HealCfgFor(uint32 cls, Spec spec)
    {
        if (cls == CLASS_PRIEST && spec == Spec::Holy)
            return &kHealPriestHoly;
        return nullptr;
    }

    // 一个"病人"的现场读数（HealCollectTargets 填）
    struct HealTgt
    {
        Unit* u = nullptr;
        float hpPct = 0.0f;
        uint32 hp = 0;
        uint32 maxHp = 0;
        float rate = 0.0f;      // 最近窗口的受伤速率（HP 点/秒；0 = 读不到 ⇒ 退化"掉血才加"）
        bool tank = false;      // 是不是"分配的主坦"
    };

    static uint32 HealSafeNow()
    {
        uint32 const ms = WorldTimer::getMSTime();
        return ms ? ms : 1;
    }

    // 受伤速率：窗口内只累计**血量下降**的增量（自己的治疗不算"受伤"）。
    //   窗口走满 `windowMs` 结算一次，得到"HP 点/秒"；窗口没满就沿用上一次结算值。
    static float HealSampleRate(BotMem* m, Unit* u, uint32 now, uint32 windowMs)
    {
        if (!u)
            return 0.0f;
        uint32 const guid = u->GetGUIDLow();
        HealWatch& w = m->healWatch[guid];
        uint32 const hp = u->GetHealth();
        if (!w.prevAt)
        {
            w.prevAt = now;
            w.winStart = now;
            w.prevHp = hp;
            return 0.0f;
        }
        if (hp < w.prevHp)
            w.dmgAcc += (w.prevHp - hp);
        w.prevHp = hp;
        w.prevAt = now;
        uint32 const span = WorldTimer::getMSTimeDiff(w.winStart, now);
        if (windowMs && span >= windowMs)
        {
            w.rate = float(w.dmgAcc) * 1000.0f / float(span);
            w.dmgAcc = 0;
            w.winStart = now;
        }
        return w.rate;
    }

    // 一个法术对**这个目标**的理论治疗量：EffectBasePoints × 系数 + 治疗强度。
    //   系数口径与核心 `Spell::EffectHeal` 一致：`CalculateSimpleValue()`（= BasePoints + BaseDice）
    //   加 `EffectRealPointsPerLevel × 施法者等级`，再走 `SpellHealingBonusDone`（含治疗强度/天赋）。
    //   ⚠️ 这只是**理论值**：暴击、过疗、减益（降低受到治疗）都不在里面 ——
    //      `[DPSFSM-HEAL]` 的 eff 才是落地后的估算有效量。
    static int32 HealTheory(Player* bot, Unit* tgt, uint32 spellId)
    {
        SpellEntry const* entry = spellId ? sServerFacade.LookupSpellInfo(spellId) : nullptr;
        if (!entry || !bot || !tgt)
            return 0;
        int32 total = 0;
        for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
        {
            // 直接治疗（神圣牧的强效/快速/治疗祷言）
            if (entry->Effect[i] == SPELL_EFFECT_HEAL || entry->Effect[i] == SPELL_EFFECT_HEAL_MECHANICAL)
            {
                int32 base = entry->CalculateSimpleValue(SpellEffectIndex(i));
                base += int32(entry->EffectRealPointsPerLevel[i] * float(bot->GetLevel()));
                if (base <= 0)
                    continue;
                total += int32(bot->SpellHealingBonusDone(tgt, entry, SpellEffectIndex(i), base, HEAL));
                continue;
            }
            // HoT（神圣牧的恢复）：理论量 = 每跳 × 跳数（duration / amplitude）。
            //   ⚠️ 它**不是**落地瞬间的治疗量，而是整条 HoT 的累计值 —— 所以 `over`（溢出）
            //      对 HoT 只是粗略估计（HoF 会被后续治疗顶掉一部分），日志里按技能名可分辨。
            if (entry->Effect[i] == SPELL_EFFECT_APPLY_AURA
                && entry->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_HEAL)
            {
                int32 base = entry->CalculateSimpleValue(SpellEffectIndex(i));
                base += int32(entry->EffectRealPointsPerLevel[i] * float(bot->GetLevel()));
                if (base <= 0)
                    continue;
                int32 const perTick = int32(bot->SpellHealingBonusDone(tgt, entry, SpellEffectIndex(i), base, HEAL));
                int32 const amplitude = int32(entry->EffectAmplitude[i]);
                int32 const duration = int32(::GetSpellDuration(entry));
                int32 ticks = 1;
                if (amplitude > 0 && duration > 0)
                    ticks = duration / amplitude;
                if (ticks < 1)
                    ticks = 1;
                total += perTick * ticks;
            }
        }
        return total;
    }

    // 按缺口挑 rank：**能覆盖缺口的最小等级**往上试。
    //   `safety` < 1 = 理论值先打折再比（更保守，宁可高一档）。
    //   没有一档覆盖得住（缺口比满级还大）⇒ 直接用最高 rank。
    static uint32 HealPickRank(PlayerbotAI* ai, BotMem* m, Unit* tgt, char const* name,
                               int32 deficit, double safety, int32* outTheory)
    {
        uint32 const top = SpellId(ai, ai->GetBot(), m, name, false);
        if (!top)
            return 0;
        if (deficit <= 0)
            return 0;

        SpellChainNode const* node = sSpellMgr.GetSpellChainNode(top);
        uint32 const topRank = node ? uint32(node->rank) : 1u;
        if (topRank <= 1)
        {
            if (outTheory)
                *outTheory = HealTheory(ai->GetBot(), tgt, top);
            return top;
        }

        double const safetyMul = (safety > 0.05 && safety <= 1.0) ? safety : 1.0;
        uint32 bestId = top;
        int32 bestTheory = 0;
        for (uint32 rank = 1; rank <= topRank; ++rank)
        {
            uint32 const id = SpellIdAtRank(ai, m, name, rank, false);
            if (!id)
                continue;
            int32 const theory = HealTheory(ai->GetBot(), tgt, id);
            if (theory <= 0)
                continue;
            if (double(theory) * safetyMul >= double(deficit))
            {
                if (outTheory)
                    *outTheory = theory;
                return id;          // 最小能覆盖的那一档
            }
            bestId = id;
            bestTheory = theory;    // 一直没覆盖住 ⇒ 记住最高档
        }
        if (outTheory)
            *outTheory = bestTheory;
        return bestId;
    }

    // 按**具体 id** 施法（rank 选出来的是 id，不是名字）。
    //   ⚠️ 为什么不复用 `CastChecked()`：那个入口只收名字、一律顶到最高 rank，
    //      而"治疗术降级"要的正是**低级**强效治疗术。失败退避/朝向/记录口径与 CastChecked 一致。
    static bool HealCastId(PlayerbotAI* ai, BotMem* m, Snapshot& s, uint32 id, char const* name, Unit* target)
    {
        if (!id || !Ready(s.bot, id))
            return false;
        time_t const now = time(nullptr);
        auto it = m->failUntil.find(id);
        if (it != m->failUntil.end() && now < it->second)
            return false;

        Unit* tgt = target ? target : s.bot;
        if (tgt != s.bot)
            s.bot->SetFacingToObject(tgt);   // 核心的 CastSpellBody 会因"背对"先转向再放弃这一手

        SpellCastResult res = SPELL_FAILED_DONT_REPORT;
        if (!ai->CanCastSpell(id, tgt, 0, false, nullptr, false, false, false, &res))
        {
            bool const selfFix = (res == SPELL_FAILED_TARGET_FRIENDLY)
                || (res == SPELL_FAILED_TARGET_ENEMY && tgt != s.bot);
            if (selfFix)
            {
                SpellCastResult const r2 = s.bot->CastSpell(s.bot, id, uint32(TRIGGERED_NONE));
                if (r2 == SPELL_CAST_OK)
                {
                    ++m->castCount;
                    m->lastOk[id] = now;
                    m->lastCastId = id;
                    m->castThisTick = true;
                    PlayerbotAI::RecordSpellCast(s.bot->GetObjectGuid(), id);
                    return true;
                }
                res = r2;
            }
            bool const waiting = (res == SPELL_FAILED_NO_POWER || res == SPELL_FAILED_OUT_OF_RANGE
                || res == SPELL_FAILED_LINE_OF_SIGHT || res == SPELL_FAILED_NOT_READY);
            m->failUntil[id] = now + (waiting ? 1 : 2);
            (void)name;
            return false;
        }

        if (tgt != s.bot)
        {
            s.bot->SetFacingToObject(tgt);
            SpellCastResult const r2 = s.bot->CastSpell(tgt, id, uint32(TRIGGERED_NONE));
            if (r2 != SPELL_CAST_OK)
            {
                m->failUntil[id] = now;      // 不退避，下一 tick 立刻重试
                return false;
            }
            PlayerbotAI::RecordSpellCast(s.bot->GetObjectGuid(), id);
        }
        else if (!ai->CastSpell(id, tgt))
            return false;

        ++m->castCount;
        m->lastOk[id] = now;
        m->lastCastId = id;
        m->castThisTick = true;
        return true;
    }

    // 采集"病人"：场景分配的（假人实测）优先；没有分配就扫队伍成员（副本里的正常路径）。
    //   去重 + 只要活着的、在同一个地图上、够得着的。
    static void HealCollectTargets(PlayerbotAI* ai, BotMem* m, Snapshot& s, std::vector<HealTgt>& out)
    {
        Player* bot = s.bot;
        float const range = float(TP(m, "heal_range", 35.0));

        std::set<ObjectGuid> ids;
        uint32 tankGuid = 0;
        {
            std::lock_guard<std::mutex> lock(s_disabledMutex);
            auto it = s_healTargets.find(bot->GetGUIDLow());
            if (it != s_healTargets.end())
                ids = it->second;
            auto tj = s_healTank.find(bot->GetGUIDLow());
            if (tj != s_healTank.end())
                tankGuid = tj->second;
        }

        std::vector<Unit*> units;
        if (!ids.empty())
        {
            for (ObjectGuid const& g : ids)
            {
                Player* p = sObjectMgr.GetPlayer(g);
                if (p && p->IsInWorld() && p != bot)
                    units.push_back(p);
            }
        }
        else if (Group* group = bot->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->getSource();
                if (member && member != bot && member->IsInWorld())
                    units.push_back(member);
            }
        }

        // 速率窗口要在**每个 tick** 都喂（哪怕这一 tick 不出手），否则"最近 3 秒掉血速率"
        //   永远是启动时那一段的读数，预读会彻底失效。
        for (Unit* u : units)
        {
            if (!u->IsAlive())
                continue;
            if (bot->GetMap() != u->GetMap())
                continue;
            HealTgt h;
            h.u = u;
            h.hp = u->GetHealth();
            h.maxHp = u->GetMaxHealth() ? u->GetMaxHealth() : 1;
            h.hpPct = 100.0f * float(h.hp) / float(h.maxHp);
            h.rate = HealSampleRate(m, u, HealSafeNow(), uint32(TP(m, "heal_rate_window_ms", 3000.0)));
            {
                Player* mp = u->IsPlayer() ? static_cast<Player*>(u) : nullptr;
                h.tank = (tankGuid && u->GetGUIDLow() == tankGuid)
                    || (!tankGuid && mp && PlayerbotAI::IsTank(mp));
            }
            bool const reachable = bot->IsWithinDistInMap(u, range) && bot->IsWithinLOSInMap(u, true);
            if (!reachable)
                continue;
            out.push_back(h);
        }
    }

    // 落地结算：[DPSFSM-HEAL] 每次治疗落点一条（节流 1 秒）。
    //   ⚠️ **这是估算值**：`eff` 按 `min(理论治疗量, maxHp - 落地前hp)` 算 —— 即"这一发
    //      理论上最多能填多少空血"。它把暴击、降低受到治疗之类的修正全部忽略，
    //      真实有效量以 `botHealingDone`（核心 `Unit::DealHeal` 的 gain）为准。
    //      `preHp` 取施法完成前最后一次采样，所以"落地前hp"本身也有一个 tick 的粒度误差。
    static void HealFlushPending(PlayerbotAI* ai, BotMem* m, Snapshot& s, uint32 now)
    {
        if (!m->healPending.active)
            return;
        if (s.casting)
        {
            // 读条中：持续刷新"落地前血量"（最后一次采样最接近落地瞬间）
            if (Unit* u = s.bot->GetMap() ? s.bot->GetMap()->GetUnit(m->healPending.tgt) : nullptr)
                m->healPending.preHp = u->GetHealth();
            return;
        }
        if (now < m->healPending.castEndAt)
            return;

        HealPending& p = m->healPending;
        Unit* u = s.bot->GetMap() ? s.bot->GetMap()->GetUnit(p.tgt) : nullptr;
        if (u)
        {
            uint32 const maxHp = u->GetMaxHealth() ? u->GetMaxHealth() : 1;
            uint32 const hp1 = u->GetHealth();
            int32 eff = p.theory;
            int32 const room = int32(maxHp) - int32(p.preHp);
            if (eff > room)
                eff = room;
            if (eff < 0)
                eff = 0;
            int32 over = p.theory - eff;
            if (over < 0)
                over = 0;

            m->healCasts += 1;
            m->healEff += uint32(eff);
            m->healTheory += uint32(p.theory > 0 ? p.theory : 0);
            m->healOver += uint32(over);
            m->hpsWinCasts += 1;
            m->hpsWinEff += uint32(eff);
            m->hpsWinTheory += uint32(p.theory > 0 ? p.theory : 0);
            m->hpsWinOver += uint32(over);

            if (!m->lastHealLog || WorldTimer::getMSTimeDiff(m->lastHealLog, now) >= 1000)
            {
                m->lastHealLog = now;
                sLog.outError("[DPSFSM-HEAL] %s cast=%s tgt=%s hp0=%.0f%% hp1=%.0f%% heal=%d eff=%d over=%d",
                    s.bot->GetName(), p.name[0] ? p.name : "-", u->GetName(),
                    double(p.hp0Pct), double(100.0f * float(hp1) / float(maxHp)),
                    int(p.theory), int(eff), int(over));
            }
        }
        (void)ai;
        p.active = false;
    }

    // 汇总：[DPSFSM-HPS] 每 10 秒一条（hps = 估算有效量/窗口秒数，overheal = 估算溢出占比，
    //   hpm = 每分钟有效治疗）。同样**是估算值**，与核心 botHealingDone 会有小差异。
    static void HealLogHps(BotMem* m, Snapshot& s, uint32 now)
    {
        if (!m->hpsWinStart)
        {
            m->hpsWinStart = now;
            return;
        }
        if (WorldTimer::getMSTimeDiff(m->hpsWinStart, now) < 10000)
            return;

        double const sec = double(WorldTimer::getMSTimeDiff(m->hpsWinStart, now)) / 1000.0;
        double const hps = sec > 0.01 ? double(m->hpsWinEff) / sec : 0.0;
        double const overPct = m->hpsWinTheory
            ? 100.0 * double(m->hpsWinOver) / double(m->hpsWinTheory) : 0.0;
        sLog.outError("[DPSFSM-HPS] %s hps=%.0f overheal=%.0f%% hpm=%.0f casts=%u eff=%u heal=%u",
            s.bot->GetName(), hps, overPct, hps * 60.0, m->hpsWinCasts, m->hpsWinEff, m->hpsWinTheory);

        m->hpsWinStart = now;
        m->hpsWinEff = m->hpsWinTheory = m->hpsWinOver = m->hpsWinCasts = 0;
    }

    // 有没有"该我驱散"的负面效果：分配给谁就驱散谁（神圣牧 = 团队 + 驱散）。
    //   `heal_role` 3 = 只做驱散；0/1/2 都允许驱散（站长口径：神圣牧 = 团队 + 驱散，驱散优先级最高）。
    static bool HealTryDispel(PlayerbotAI* ai, BotMem* m, Snapshot& s, HealCfg const* cfg,
                              std::vector<HealTgt> const& list)
    {
        if (!cfg->dispelMagic && !cfg->cureDisease)
            return false;
        for (HealTgt const& h : list)
        {
            if (!h.u)
                continue;
            if (cfg->dispelMagic && ai->HasAuraToDispel(h.u, DISPEL_MAGIC))
            {
                if (CastChecked(ai, m, s, cfg->dispelMagic, h.u))
                {
                    m->state = State::Dispel;
                    return true;
                }
            }
            if (cfg->cureDisease && ai->HasAuraToDispel(h.u, DISPEL_DISEASE))
            {
                if (CastChecked(ai, m, s, cfg->cureDisease, h.u))
                {
                    m->state = State::Dispel;
                    return true;
                }
            }
        }
        return false;
    }

    // 单目标治疗：预读 + rank 降级 + 记 pending。返回 true = 本 tick 出了手。
    //   `targetPct` = 这个目标的治疗阈值（主坦比团队更保守）；`hpGoalPct` = 治到多少算够。
    static bool HealOneTarget(PlayerbotAI* ai, BotMem* m, Snapshot& s, HealCfg const* cfg,
                              HealTgt const& h, double targetPct, double hpGoalPct, bool lowMana,
                              State st)
    {
        if (!h.u || !h.maxHp)
            return false;

        uint32 const now = HealSafeNow();
        double const maxHp = double(h.maxHp);
        double const hpNow = double(h.hp);
        double const rate = double(h.rate);       // HP 点/秒（0 = 读不到速率）

        // 先按"大治疗"的实际读条做预读（读条时长受急速/天赋影响，用 CastMsWithMod 同一口径）。
        int const castMs = CastMsWithMod(ai, m, s, cfg->bigHeal, 2500);
        double const lead = double(castMs) / 1000.0;
        double const predicted = hpNow - rate * lead;     // 预计落地时的血量
        double const predictedPct = 100.0 * predicted / maxHp;

        // 窗口里读不到速率（rate == 0）⇒ 退化"掉血才加"：直接看当前血量。
        bool const needNow = (rate > 0.5) ? (predictedPct < targetPct) : (h.hpPct < targetPct);
        if (!needNow)
            return false;

        // 缺口 = "治到 hpGoal" 还差多少（预读落地血量算）；至少 1 点，避免 0 缺口仍出手。
        double const goalHp = maxHp * hpGoalPct / 100.0;
        double deficitD = goalHp - predicted;
        if (deficitD < 1.0)
            deficitD = 1.0;

        // 蓝 < recover_mana ⇒ 只放最省的：rank 1、不用盾、不用群体治疗
        int32 theory = 0;
        uint32 id = 0;
        if (lowMana)
            id = SpellIdAtRank(ai, m, cfg->bigHeal, 1, false);
        else
            id = HealPickRank(ai, m, h.u, cfg->bigHeal,
                int32(deficitD), TP(m, "rank_safety", 1.0), &theory);
        if (!id)
            return false;
        if (!theory)
            theory = HealTheory(s.bot, h.u, id);

        // 记 pending（落地后再结算）：hp0 是施法开始时的血量百分比。
        char const* spellName = cfg->bigHeal;
        int const actualCastMs = CastMsWithMod(ai, m, s, spellName, castMs);
        if (HealCastId(ai, m, s, id, spellName, h.u))
        {
            HealPending& p = m->healPending;
            p.active = true;
            p.tgt = h.u->GetObjectGuid();
            p.spellId = id;
            snprintf(p.name, sizeof(p.name), "%s", spellName);
            p.theory = theory;
            p.startAt = now;
            p.castEndAt = now + uint32(actualCastMs > 0 ? actualCastMs : 0);
            p.preHp = h.hp;
            p.hp0Pct = h.hpPct;
            m->healTargetGuid = h.u->GetGUIDLow();
            m->state = st;
            return true;
        }
        return false;
    }

    // 按**名字**放一发并记账（不需要选 rank 的那些：盾 / 快速治疗 / 群体治疗 / HoT）。
    static bool HealCastNamed(PlayerbotAI* ai, BotMem* m, Snapshot& s, char const* name,
                              HealTgt const& h, State st)
    {
        if (!name || !h.u)
            return false;
        uint32 const id = SpellId(ai, s.bot, m, name, false);
        if (!id)
            return false;
        int32 const theory = HealTheory(s.bot, h.u, id);
        int const castMs = CastMsWithMod(ai, m, s, name, 1500);
        if (!HealCastId(ai, m, s, id, name, h.u))
            return false;
        uint32 const now = HealSafeNow();
        HealPending& p = m->healPending;
        p.active = true;
        p.tgt = h.u->GetObjectGuid();
        p.spellId = id;
        snprintf(p.name, sizeof(p.name), "%s", name);
        p.theory = theory;
        p.startAt = now;
        p.castEndAt = now + uint32(castMs > 0 ? castMs : 0);
        p.preHp = h.hp;
        p.hp0Pct = h.hpPct;
        m->healTargetGuid = h.u->GetGUIDLow();
        m->state = st;
        return true;
    }

    // Prep：没人需要治疗时补自身/团队 buff（低优先，绝不抢治疗的 GCD）。
    static bool HealPrep(PlayerbotAI* ai, BotMem* m, Snapshot& s, HealCfg const* cfg)
    {
        if (cfg->selfBuffA && AuraStacks(ai, s.bot, m, cfg->selfBuffA, s.bot) == 0)
        {
            if (TryCastSelf(ai, m, s, cfg->selfBuffA))
            {
                m->state = State::Prep;
                return true;
            }
        }
        if (cfg->selfBuffB && AuraStacks(ai, s.bot, m, cfg->selfBuffB, s.bot) == 0)
        {
            if (TryCastSelf(ai, m, s, cfg->selfBuffB))
            {
                m->state = State::Prep;
                return true;
            }
        }
        return false;
    }

    // ==========================================================================
    // HealCore 主入口（mode=5、神圣牧）
    // ==========================================================================
    ai::dpsfsm::TickResult TickHealFsm(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        using ai::dpsfsm::TickResult;
        HealCfg const* cfg = HealCfgFor(s.bot->getClass(), m->spec);
        if (!cfg)
            return TickResult::Decline;

        uint32 const now = HealSafeNow();

        // 病人现场（同时喂速率窗口 —— 哪怕这一 tick 不出手也必须喂）
        std::vector<HealTgt> list;
        HealCollectTargets(ai, m, s, list);

        // 落地结算 + 10 秒汇总（都只写统计，不影响决策）
        HealFlushPending(ai, m, s, now);
        HealLogHps(m, s, now);

        double const lowManaPct = TP(m, "recover_mana", 20.0);
        bool const lowMana = s.power < lowManaPct;
        int const role = int(TP(m, "heal_role", 0.0) + 0.5);    // 0=auto 1=tank 2=raid 3=dispel
        double const emergencyHp = TP(m, "emergency_hp", 35.0);
        double const tankPct = TP(m, "heal_tank_target", 85.0);
        double const raidPct = TP(m, "heal_target", 80.0);
        double const goalPct = TP(m, "heal_cap", 90.0);

        if (list.empty())
        {
            // 没有病人：先补 buff；蓝紧就喝药，然后交回引擎（喝水/魔杖归它）
            if (HealPrep(ai, m, s, cfg))
                return TickResult::Acted;
            if (lowMana && UseManaPotion(m, s))
                return TickResult::Acted;
            m->state = State::Idle;
            return TickResult::Decline;
        }

        // 读条中：这一手还没落地，别插技能（Hold 的合法理由）
        if (s.casting)
            return HoldWait(m, "healing");

        // ---- ① 驱散（最高优先）----
        if (role != 2 && HealTryDispel(ai, m, s, cfg, list))
            return TickResult::Acted;
        if (role == 3)
            return TickResult::Decline;    // 只做驱散的分配：加血不归我

        // ---- ② 救急（< emergency_hp：开大 CD，不省蓝）----
        HealTgt const* lowest = nullptr;
        for (HealTgt const& h : list)
            if (!lowest || h.hpPct < lowest->hpPct)
                lowest = &h;
        if (lowest && lowest->hpPct < emergencyHp)
        {
            // 大 CD 先交（守护之魂 → 痛苦压制；没学会的解析成 0 ⇒ 自动跳过）
            if (role != 1)
            {
                if (cfg->emergencyA && CastChecked(ai, m, s, cfg->emergencyA, lowest->u))
                {
                    m->state = State::Emergency;
                    return TickResult::Acted;
                }
                if (cfg->emergencyB && CastChecked(ai, m, s, cfg->emergencyB, lowest->u))
                {
                    m->state = State::Emergency;
                    return TickResult::Acted;
                }
            }
            // 盾（本服坦克套盾不会断怒气，可正常用）；盾上已经有了就直接快速治疗
            if (cfg->shield && !lowMana
                && TargetAuraRemain(ai, s.bot, m, cfg->shield, lowest->u) <= 0)
            {
                if (HealCastNamed(ai, m, s, cfg->shield, *lowest, State::Emergency))
                    return TickResult::Acted;
            }
            if (HealCastNamed(ai, m, s, cfg->flashHeal, *lowest, State::Emergency))
                return TickResult::Acted;
        }

        // ---- ③ 主坦维护（只盯"分配的主坦"）----
        if (role != 2)
        {
            for (HealTgt const& h : list)
            {
                if (!h.tank)
                    continue;
                // renew 常驻：剩余 < `renew_refresh_ms` 才续（别每 tick 重挂）
                if (cfg->renew && !lowMana && h.hpPct < 95.0f)
                {
                    int const remain = TargetAuraRemain(ai, s.bot, m, cfg->renew, h.u);
                    if (remain < int(TP(m, "renew_refresh_ms", 3000.0)))
                    {
                        if (HealCastNamed(ai, m, s, cfg->renew, h, State::Tank))
                            return TickResult::Acted;
                    }
                }
                // 预读 + rank 降级（低级强效维持 → 高级强效顶尖刺）
                if (HealOneTarget(ai, m, s, cfg, h, tankPct, goalPct, lowMana, State::Tank))
                    return TickResult::Acted;
            }
        }

        // ---- ④ 团队 ----
        if (role != 1)
        {
            // 群体治疗：小队里**多个人**同时掉血才值得（单人掉血放群体治疗是纯浪费蓝）
            if (!lowMana && cfg->groupHeal)
            {
                int const need = int(TP(m, "group_heal_count", 3.0));
                double const groupHp = TP(m, "group_heal_hp", 85.0);
                int hurt = 0;
                for (HealTgt const& h : list)
                    if (h.hpPct < groupHp)
                        ++hurt;
                if (hurt >= need && s.selfHp < groupHp + 5.0)
                {
                    if (HealCastNamed(ai, m, s, cfg->groupHeal, *lowest, State::Raid))
                        return TickResult::Acted;
                }
            }
            // 联结治疗：自己与队友同时告急（只治一个 GCD 能救两个）
            if (cfg->linkHeal && !lowMana && lowest && lowest->u != s.bot
                && s.selfHp < 60.0f && lowest->hpPct < 60.0f)
            {
                if (HealCastNamed(ai, m, s, cfg->linkHeal, *lowest, State::Raid))
                    return TickResult::Acted;
            }
            // 其余队友：血量最低者优先（预读 + rank 降级 同一套）
            std::vector<HealTgt> sorted(list);
            std::sort(sorted.begin(), sorted.end(),
                [](HealTgt const& a, HealTgt const& b) { return a.hpPct < b.hpPct; });
            for (HealTgt const& h : sorted)
            {
                if (h.tank)
                    continue;                      // 主坦已经在上面的 Tank 状态处理过
                if (HealOneTarget(ai, m, s, cfg, h, raidPct, goalPct, lowMana, State::Raid))
                    return TickResult::Acted;
            }
        }

        // ---- ⑤ 没人需要治疗：buff / 蓝 -----
        if (HealPrep(ai, m, s, cfg))
            return TickResult::Acted;
        if (lowMana && UseManaPotion(m, s))
            return TickResult::Acted;

        m->state = lowMana ? State::Recover : State::Idle;
        return TickResult::Decline;
    }

    bool Tick(PlayerbotAI* ai)
        {
            if (!ai || !ai->GetBot())
                return false;

            Player* bot = ai->GetBot();
            if (!bot->IsInWorld() || !bot->IsAlive())
            {
                NoteBail(bot, !bot->IsInWorld() ? "not-in-world" : "dead");
                return false;
            }
            {
                std::lock_guard<std::mutex> lock(s_disabledMutex);
                if (s_disabled.count(bot->GetGUIDLow()))
                {
                    NoteBail(bot, "disabled");
                    return false;
                }
            }
            BotMem* m = Mem(bot->GetGUIDLow());
            m->castThisTick = false;
            // [2026-10-31 站长口径] Hold 只活一个 tick：本 tick 重新决定。
            //   上一 tick 的标记要一直留到引擎读（引擎的让位判断发生在 Tick 返回之后），所以在这里清、不在返回前清。
            m->holdThisTick = false;
            // [2026-11-01 站长口径] "有意空着"的登记也每 tick 重来：`HoldWait()` 只对本 tick 有效，
            //   Tick() 的兜底读它（见派发处"Hold 的通用兜底"）。
            m->holdIntent = nullptr;
            ReloadTune();

            uint32 const hbNow = WorldTimer::getMSTime();
            bool const hbDue = (!m->hbLast || WorldTimer::getMSTimeDiff(m->hbLast, hbNow ? hbNow : 1) >= 10000);
            if (hbDue)
                m->hbLast = hbNow ? hbNow : 1;

            // [2026-11-01 站长口径] **专精判定上移到所有"必须进战斗"的门之前**。
            //   原来它排在 no-victim / 坦奶让位之后 ⇒ 治疗分支根本走不到（治疗者常常没有 victim）。
            //   判定本身与位置无关（纯天赋/法术书判定，自带 5 秒节流），DPS 侧行为不受影响。
            {
                time_t const nowT = time(nullptr);
                if (m->spec == Spec::Unknown || !m->specCheckedAt || nowT - m->specCheckedAt >= 5)
                {
                    m->specCheckedAt = nowT;
                    m->spec = DetectSpec(ai);
                }
            }

            // ==================================================================
            // [2026-11-01 站长口径] **治疗派发（mode=5 · 神圣牧 · HealCore）**
            //   放在三道门之前：
            //     ① `no-victim`（治疗者没有敌人也要工作）；
            //     ② `stunned/rooted`（治疗者被定身也该继续奶，核心会自己处理读条）；
            //     ③ `坦/奶让位`（这一条本来就把所有治疗职业挡在 DPS 状态机外）。
            //   只接**牧师·神圣**：`Spec::Holy` 同时是圣骑士神圣，必看职业。
            //   mode != 5 ⇒ 原样落回 legacy（legacy 引擎的加血逻辑一个字节不动）。
            // ==================================================================
            if (bot->getClass() == CLASS_PRIEST && m->spec == Spec::Holy)
            {
                bool healDerived = false;
                DecisionMode const healMode = ResolveMode(m, healDerived);
                if (healMode == DecisionMode::Fsm)
                {
                    m->mode = int(healMode);
                    if (!m->modeLogged)
                    {
                        m->modeLogged = true;
                        sLog.outError("[DPSFSM-MODE] %s spec=%s mode=%d(%s) heal=1",
                            bot->GetName(), SpecName(m->spec), int(healMode), DecisionModeName(healMode));
                    }
                    if (!bot->IsStandState())
                        bot->SetStandState(UNIT_STAND_STATE_STAND);
                    ++m->tickCount;
                    Snapshot hs;
                    BuildSnapshot(ai, m, hs);
                    ai::dpsfsm::TickResult const hr = TickHealFsm(ai, m, hs);
                    ManaLogTick(m, bot);
                    switch (hr)
                    {
                        case ai::dpsfsm::TickResult::Acted:
                            return true;
                        case ai::dpsfsm::TickResult::Hold:
                            m->holdThisTick = true;
                            return true;
                        case ai::dpsfsm::TickResult::Decline:
                        default:
                            return false;
                    }
                }
            }

            // [2026-10-29 诊断结论] 原来只认 IsInCombat()：本核心里"有目标但还没造成/受到伤害"不算战斗，
            //   于是远程 bot 永远不挥刀 -> 不进战斗 -> 状态机拒绝运行 -> 死锁（实测整场 incombat=0）。
            //   只要有可攻击目标就进来，由状态机自己放出第一发伤害技能来起战斗。
            // 假人场景：没有 victim 就自己咬上去（否则远程 bot 永远不造成伤害 ⇒ 不进战斗 ⇒ 死锁）
            if (!bot->GetVictim())
                EngageScenarioTarget(ai);

            if (!bot->IsInCombat() && !bot->GetVictim())
            {
                if (m->state != State::Idle)
                    m->state = State::Idle;
                NoteBail(bot, "no-victim");
                return false;
            }

            // 被控�?被打断到不能自己行动时，交回引擎（解控、逃跑、驱散都更重要）
            if (bot->hasUnitState(UNIT_STAT_STUNNED | UNIT_STAT_FLEEING | UNIT_STAT_CONFUSED | UNIT_STAT_ROOT))
            {
                NoteBail(bot, "stunned/rooted");
                return false;
            }

            // [2026-10-29] 坦/奶不归 DPS 状态机管（副本里奶和坦另有职责，这里只做输出）
            {
                bool forced = false;
                {
                    std::lock_guard<std::mutex> lock(s_disabledMutex);
                    forced = s_forceDps.count(bot->GetGUIDLow()) != 0;
                }
                if (!forced && (PlayerbotAI::IsHeal(bot) || ai->IsTank(bot)))
                    return false;
            }

            // 专精每 5 秒重判一次（**已在上面上移**，见"专精判定上移"注释：治疗分支必须排在门之前）

            // [2026-10-29 站长口径] 之前做的仇恨门不能因为状态机被绕过：
            //   门禁与 AttackAction 完全同一份实现（WaitingForTankThreat），不复制判断。
            // [2026-10-31 修] 假人场景**必须绕过**这道门：假人没有仇恨，`WaitingForTankThreat`
            //   会一直为真 ⇒ 状态机 return false、引擎的 AttackAction 也在等 ⇒ **互相推诿的死锁**。
            //   实测表现：bot 有目标、canattack=1、站 17.4 码，但 40 秒 0 施法 0 伤害（约 1/3 轮次）。
            if (!dpsfsm::SkipEngine(bot->GetGUIDLow())
                && AttackAction::WaitingForTankThreat(ai, bot, bot->GetVictim()))
                return false;

            ++m->tickCount;
            if (bot->IsMoving())
                ++m->movTicks;

            // [2026-10-30] 覆盖率断言：一次运行里每只 bot 打一条"表外候选"日志（漏配立刻可见）
            if (dpspolicy::PPolicy const* covPol = dpspolicy::PolicyFor(bot->getClass(), m->spec))
                dpspolicy::LogCoverage(ai, m, bot, covPol);

            // [2026-10-31 S1] 决策模式在这里**唯一一次**解析（新键 `mode` 优先；旧四键只用于
            //   自动迁移并给一条 WARN）。TickPolicy 读 m->mode，不再自己去看 pselect/pauto。
            bool derivedFromLegacy = false;
            DecisionMode const dmode = ResolveMode(m, derivedFromLegacy);
            m->mode = int(dmode);
            if (!m->modeLogged)
            {
                m->modeLogged = true;
                sLog.outError("[DPSFSM-MODE] %s spec=%s mode=%d(%s)%s",
                    bot->GetName(), SpecName(m->spec), int(dmode), DecisionModeName(dmode),
                    derivedFromLegacy
                        ? " WARN:旧键 value/policy/pselect/pauto 已废弃，请改成 mode=N（见 dev/DESIGN_dps_fsm.md）"
                        : "");
            }

            if (hbDue)
            {
                // [2026-10-30] 心跳，10 秒一条；pol/eng/mov/melee 用于定位"白字平砍变少"。
                //   [2026-10-31 S1] pol 改为"这一轮是否走策略表"（mode=1/2/3）——原先是读 policy 布尔，
                //   现在只是 DecisionMode 的一个投影；字段名保持不变，避免打断既有解析脚本。
                sLog.outError("[DPSFSM-HB] %s spec=%s state=%s pol=%d incombat=%d target=%s hp=%.0f res=%.0f auras=%u dup=%u ticks=%u casts=%u eng=%u mov=%u melee=%u",
                    bot->GetName(), SpecName(m->spec), StateName(m->state),
                    ModeIsTable(dmode) ? 1 : 0, bot->IsInCombat() ? 1 : 0,
                    bot->GetVictim() ? bot->GetVictim()->GetName() : "-", bot->GetHealthPercent(), bot->GetPowerPercent(),
                    bot->GetVictim() ? uint32(bot->GetVictim()->GetSpellAuraHolderMap().size()) : 0u,
                    m->dupDebuff, m->tickCount, m->castCount, m->engRuns, m->movTicks, bot->botMeleeHits);

                // [2026-10-31 S1] 唯一一处"语义会变"的旧组合：value=1 且 policy=1。
                //   旧代码是 value 先跑、失败后落 policy（两段串联）；新口径只保留 value 引擎。
                //   仓库里没有任何配置这样配过（conf 与 arms 都不带 value=），但要留痕，别让人以为是静默改行为。
                if (derivedFromLegacy && TP(m, "value", 0.0) >= 1.0 && TP(m, "policy", 0.0) >= 1.0)
                    sLog.outError("[DPSFSM-MODE] %s WARN:value=1 与 policy=1 同时设置在新口径里不再串联，已按 mode=4(value) 处理",
                        bot->GetName());
            }

            // [2026-10-30] 价值驱动：mode=4 时由价值引擎选技能；
            //   它没有伤害技能可用（或都放不出）时返回 false，落回下面做 buff/圣印/形态维护。
            if (dmode == DecisionMode::Value)
            {
                if (TickValue(ai, m))
                    return true;
                NoteBail(bot, "value-engine-declined");
            }

            // [2026-10-30] 职业技能空间 + 专精策略（mode=1/2/3 开启；默认 legacy 走原分专精处理器）
            if (ModeIsTable(dmode))
            {
                bool const handled = TickPolicy(ai, m);
                // [2026-10-31 站长口径] 本 tick 结束再结一次 [DPSFSM-MANA]（只统计，不改策略表行为）：
                //   有蓝职业的 `spent` 按 `botSpellCounts` 的成功施法次数差累加（mode=3 的评分路径
                //   走 dpsvalue::CastId，不经过 CastChecked，所以统一在这里结算），模式之间口径一致 ⇒ 可比 DPM。
                ManaLogTick(m, bot);
                return handled;
            }

            // [2026-10-31 S2] mode=5：显式状态图。冰法（试点）+ 猎人三系（兽王/射击/生存）
            //   + [2026-10-31 站长口径] 奥法（蓝量驱动 + 3+3）
            //   + [2026-10-31 站长口径] 战士两系（武器/狂暴）+ 盗贼三系（刺杀/战斗/敏锐）。
            //   **防护战不在名单里**（防御另有职责，继续走 legacy 的空转早退路径）。
            //   其它专精还没有图 ⇒ 继续往下走手写处理器，行为与 legacy 相同。
            bool const warriorFsm = (m->spec == Spec::Arms || m->spec == Spec::Fury);
            bool const rogueFsm = (m->spec == Spec::Combat || m->spec == Spec::Assassination
                || m->spec == Spec::Subtlety);
            // [2026-11-01 站长口径] 术士**三系全部进图**：毁灭 → TickWarlockFsm()，
            //   痛苦 / 恶魔 → TickWarlockDotsFsm()（共用 TickWarlockFsmCore 的骨架）。
            //   mode=0/1/2/3/4 与 legacy `TickWarlock` **一个字节都不动**。
            bool const warlockFsm = (m->spec == Spec::Destruction
                || m->spec == Spec::Affliction || m->spec == Spec::Demonology);
            // [2026-11-01 站长口径] 火法接进 mode=5（第六张职业图）：`TickFireFsm`。
            //   mode=0/1/2/3/4 的火法照旧走 legacy `TickMage`，这条判断只影响 mode=5。
            bool const mageFsm = (m->spec == Spec::Frost || m->spec == Spec::Arcane
                || m->spec == Spec::Fire);
            // [2026-11-01 站长口径] 惩戒骑接进 mode=5（第七张职业图）：`TickRetFsm`。
            //   **只接 Retribution**：Protection/Holy 继续走 legacy `TickPaladin`（并在那里 Decline），
            //   mode=0/1/2/3/4 的惩戒也照旧走 `TickPaladin`，行为不变。
            bool const retFsm = (m->spec == Spec::Retribution);
            // [2026-11-01 站长口径] 萨满接进 mode=5（第八张职业图）：`TickShamanFsm`。
            //   **元素 / 增强**进图；**恢复（Restoration）不做 DPS**（治疗以后单独做）⇒ 不进图，
            //   继续走 legacy `TickShaman`（在那里保持原有行为）。mode=0/1/2/3/4 的萨满也不变。
            bool const shamanFsm = (m->spec == Spec::Elemental || m->spec == Spec::Enhancement);
            // [2026-11-01 站长口径] 暗牧接进 mode=5（第九张职业图）：`TickShadowFsm`。
            //   **只接 Shadow**：戒律/神圣继续走 legacy `TickPriest`（在那里 Decline，
            //   它们不是 DPS 循环问题）⇒ 行为与 mode=0/1/2/3/4 完全一致。
            //   这张图不是优先级表，是 GCD 时间轮（详见 TickShadowFsm 上面的注释）。
            bool const priestFsm = (m->spec == Spec::Shadow);
            // [2026-11-01 站长口径] 德鲁伊接进 mode=5（第十张职业图）：野性 + 平衡。
            //   恢复（Restoration）**也选进来**，但 `TickDruidFsm` 第一行就 Decline ——
            //   这样 mode=5 的恢复德不会被 legacy `TickDruid` 的平衡循环误接管；
            //   mode=0/1/2/3/4 的德鲁伊照旧走 legacy，一个字节不动。
            bool const druidFsm = (m->spec == Spec::Feral || m->spec == Spec::Balance
                || m->spec == Spec::Restoration);
            if (dmode == DecisionMode::Fsm
                && (mageFsm || IsHunterSpec(m->spec) || warriorFsm || rogueFsm || warlockFsm || retFsm
                    || shamanFsm || priestFsm || druidFsm))
            {
                // [2026-11-01 站长口径] **坐姿是通用事故，不是某个专精的私事**：
                //   假人连跑时上一轮结束 bot 会坐下回蓝/休息，而 SimScenario 只重置血/蓝/能量、
                //   不管站姿 ⇒ 下一轮核心 `Spell::CheckCast` 第一条就判
                //   `SPELL_FAILED_NOT_STANDING(65)` ⇒ **整轮 0 施法 0 伤害**（实测 balance 每批
                //   第 2 轮必现，诊断行里 `res=65`）。德鲁伊两条路径上原来各补过一次，现在提到这里，
                //   进各专精之前统一做（站姿/躺姿才调，站着不调 —— 不产生多余的 SetStandState）。
                //   站起来不是施法、不占 GCD，所以放在 BuildSnapshot 之前，无条件。
                if (!bot->IsStandState())
                    bot->SetStandState(UNIT_STAND_STATE_STAND);

                Snapshot s;
                BuildSnapshot(ai, m, s);
                // [2026-10-31 站长口径] 三态返回：Acted=我出手了；Hold=我有意空着（引擎也别插）；
                //   Decline=我不处理，交回引擎（走位/追击/开怪/喝水）。
                //   改成 if/else 链而不是嵌套三元：专精越加越多，三元的括号已经很难数
                //   （行为与原来的三元完全一致，逐个专精对应不变）。
                ai::dpsfsm::TickResult fsmResult = ai::dpsfsm::TickResult::Decline;
                if (m->spec == Spec::Frost)
                    fsmResult = TickFrostFsm(ai, m, s);
                else if (m->spec == Spec::Arcane)
                    fsmResult = TickArcaneFsm(ai, m, s);
                else if (m->spec == Spec::Fire)
                    fsmResult = TickFireFsm(ai, m, s);
                else if (IsHunterSpec(m->spec))
                    fsmResult = TickHunterFsm(ai, m, s);
                else if (warriorFsm)
                    fsmResult = TickWarriorFsm(ai, m, s);
                else if (rogueFsm)
                    fsmResult = TickRogueFsm(ai, m, s);
                else if (retFsm)
                    fsmResult = TickRetFsm(ai, m, s);
                else if (shamanFsm)
                    fsmResult = TickShamanFsm(ai, m, s);
                else if (priestFsm)
                    fsmResult = TickShadowFsm(ai, m, s);
                else if (druidFsm)
                    fsmResult = TickDruidFsm(ai, m, s);
                else
                    fsmResult = (m->spec == Spec::Destruction)
                        ? TickWarlockFsm(ai, m, s) : TickWarlockDotsFsm(ai, m, s);
                // [2026-10-31 站长口径] 状态机本 tick 结束时结一次耗蓝账（10 秒节流，不每 tick 刷屏）
                ManaLogTick(m, bot);
                // [2026-11-01 站长口径] **Hold 的通用兜底（在派发层统一做，不再指望每个专精自己判）**：
                //   只有 GCD 真被锁（`s.casting` 或 GCD 未转好）或专精**显式登记**了"有意等"
                //   （`HoldWait()`）时 Hold 才成立；其余一律降级 Decline —— 让引擎继续走位/喝水/魔杖。
                //   病根：一拍什么都放不出就 Hold ⇒ 引擎整 tick 被挡死（`skipEngine = fsmOwns &&
                //   DidCastThisTick`，Hold 也算"占了这一手"）⇒ 实测暗牧/平衡出现整轮 45 秒
                //   0 施法 0 伤害，而 victim/距离/蓝量全部正常。
                //   `hold_strict=0` 可回滚到旧行为（A/B 用，默认 1）。
                if (fsmResult == ai::dpsfsm::TickResult::Hold && !GcdLocked(s) && !m->holdIntent
                    && TP(m, "hold_strict", 1.0) >= 1.0)
                    fsmResult = ai::dpsfsm::TickResult::Decline;
                switch (fsmResult)
                {
                    case ai::dpsfsm::TickResult::Acted:
                        return true;
                    case ai::dpsfsm::TickResult::Hold:
                        m->holdThisTick = true;
                        return true;
                    case ai::dpsfsm::TickResult::Decline:
                    default:
                        return false;
                }
            }

            bool handled = false;
            switch (m->spec)
            {
                case Spec::Fire:
                case Spec::Frost:
                case Spec::Arcane:
                    handled = TickMage(ai, m);
                    break;

                case Spec::Arms:
                case Spec::Fury:
                case Spec::Protection:
                    handled = TickWarrior(ai, m);
                    break;

                case Spec::Combat:
                case Spec::Assassination:
                case Spec::Subtlety:
                    handled = TickRogue(ai, m);
                    break;

                case Spec::BeastMastery:
                case Spec::Marksmanship:
                case Spec::Survival:
                    handled = TickHunter(ai, m);
                    break;

                case Spec::Affliction:
                case Spec::Demonology:
                case Spec::Destruction:
                    handled = TickWarlock(ai, m);
                    break;

                case Spec::Enhancement:
                case Spec::Elemental:
                case Spec::Restoration:
                    handled = TickShaman(ai, m);
                    break;

                case Spec::Feral:
                case Spec::Balance:
                    handled = TickDruid(ai, m);
                    break;

                case Spec::Retribution:
                case Spec::Holy:
                    handled = TickPaladin(ai, m);
                    break;

                case Spec::Shadow:
                case Spec::Discipline:
                    handled = TickPriest(ai, m);
                    break;

                default:
                    break;
            }

            if (handled)
                return true;

            if (m->flags.insert("unimplemented logged").second)
                sLog.outError("[DPSFSM] %s 职业/专精 %s 状态机还没写，先回退原引擎",
                    bot->GetName(), SpecName(m->spec));
            return false;
        }
    }
}
