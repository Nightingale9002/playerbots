#include "playerbot/playerbot.h"
#include "playerbot/strategy/DpsFsm.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/strategy/actions/AttackAction.h"
#include "Grids/CellImpl.h"
#include "Maps/Map.h"
#include "Entities/Pet.h"           // [2026-10-31 站长口径] 猎人宠物生命周期守卫要现场建宠物（Pet::Create）
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

    uint32 SpellId(PlayerbotAI* ai, Player* bot, BotMem* m, char const* name, bool warn = true)
    {
        auto it = m->ids.find(name);
        if (it != m->ids.end())
            return it->second;

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

    int CountHostilesNear(PlayerbotAI* ai, Player* bot, float range)
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
    bool UseHastePotion(BotMem* m, Snapshot& s)
    {
        if (s.bot->GetMaxPower(POWER_MANA) > 0)
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
            return s_scenarioTargets.find(guid) != s_scenarioTargets.end();
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
                if (TP(m, "burn_hold", 0.0) >= 1.0)
                    return ai::dpsfsm::TickResult::Hold;
                // [2026-10-31 站长口径] 仇恨 Wait 档（Maintain）：**不施法、也不让引擎动**。
                //   此时抢在坦克前面打，换来的是一只追着法师的怪（治疗多花蓝、坦克多花时间），
                //   就像把家门让给了对方；等一下让坦克把仇恨拉开，比多打一发寒冰箭划算。
                //   Throttle 档不 Hold：照常打低威胁填充（寒冰箭/冰枪），只是不开爆发、不用冰锥。
                if (threat == ThreatLevel::Wait)
                    return ai::dpsfsm::TickResult::Hold;
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
        if (s.autoRemainMs <= land)
        {
            if (s.arcaneReady && TryCast(ai, m, s, "arcane shot", s.target))
                return TickResult::Acted;
            return TickResult::Hold;
        }

        // 3) 窗口放不下稳固、但够一发瞬发：多重 → 奥射 → 否则空着
        if (s.autoRemainMs < steadyWindow)
        {
            if (s.multiReady && TryCast(ai, m, s, "multi-shot", s.target))
                return TickResult::Acted;
            if (s.arcaneReady && TryCast(ai, m, s, "arcane shot", s.target))
                return TickResult::Acted;
            return TickResult::Hold;
        }

        // 4) 窗口够：稳固填充（移动中读条会被取消 ⇒ 空着走位）
        if (!s.moving && TryCast(ai, m, s, "steady shot", s.target))
            return TickResult::Acted;

        // 5) 兜底：窗口不明/技能都没好 ⇒ 有意空着，别让引擎插一手把平射顶掉
        return TickResult::Hold;
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
                if (s.dist > float(TP(m, "range_melee", 6.5)))
                    result = TickResult::Decline;
                else
                    result = TickResult::Hold;
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
            return TickResult::Hold;
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
                if (s.dist > float(TP(m, "range_melee", 6.5)))
                    result = TickResult::Decline;
                else
                    result = TickResult::Hold;
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

    // 单体输出表（全部施法前都过 Ready()/射程，由 CastChecked 统一判）。
    ai::dpsfsm::TickResult TickWarlockMaintain(PlayerbotAI* ai, BotMem* m, Snapshot& s)
    {
        using ai::dpsfsm::TickResult;

        // 宠物：掉队/换目标就重新下令（宠物仇恨不算在 bot 头上，Wait 档也让它继续咬）
        CommandPetAttack(s);

        bool const threatOk = WarlockThreatOk(m, s);

        // ① 诅咒位：目标身上没有任何诅咒才补（元素/暗影/痛苦/末日任一在场就不动）
        if (AuraStacks(ai, s.bot, m, "curse of the elements", s.target) == 0
            && AuraStacks(ai, s.bot, m, "curse of shadows", s.target) == 0
            && AuraStacks(ai, s.bot, m, "curse of agony", s.target) == 0
            && AuraStacks(ai, s.bot, m, "curse of doom", s.target) == 0
            && TryCast(ai, m, s, "curse of the elements", nullptr))
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

    ai::dpsfsm::TickResult TickWarlockFsm(PlayerbotAI* ai, BotMem* m, Snapshot& s)
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
        State const st = EvalWarlock(ai, m, s, threat);

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
                result = acted ? TickResult::Acted : TickWarlockMaintain(ai, m, s);
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
                    result = TickWarlockMaintain(ai, m, s);   // 蓝还够打：别把 GCD 空成 0 DPS
                else
                    result = TickResult::Hold;                // 蓝血都见底：等回蓝（不吃药就别动这一手）
                break;

            case State::Aoe:
            {
                // [2026-11-01 站长口径] 群伤先做**单体版**：种子（每个目标只种一颗，重复种会顶掉
                //   上一颗 ⇒ 白读一个 GCD）+ 暗影箭兜底。
                CommandPetAttack(s);
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

                // 饰品 → 暗影灼烧（学会了才放，且仇恨越线时收起）→ 暗影箭
                if (TP(m, "burst_trinket", 1.0) >= 1.0 && UseTrinket(ai, m, s))
                    result = TickResult::Acted;
                else if (threatOk && SpellId(ai, s.bot, m, "shadowburn", false)
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
                result = TickWarlockMaintain(ai, m, s);
                break;

            case State::Idle:
            default:
                result = TickResult::Decline;
                break;
        }

        // [2026-11-01 站长口径] 每 10 秒一条状态日志（WorldTimer 节流）：
        //   state / threat=自己/坦克 / mana / hp / shatter(0/1) / last=上一手成功施放的法术
        if (!m->lastWarlockLog || WorldTimer::getMSTimeDiff(m->lastWarlockLog, safeNow) >= 10000)
        {
            m->lastWarlockLog = safeNow;
            char const* last = "-";
            if (m->lastCastId)
                if (SpellEntry const* e = sServerFacade.LookupSpellInfo(m->lastCastId))
                    last = e->SpellName[0];
            sLog.outError("[DPSFSM-WARLOCK] %s state=%s threat=%.0f/%.0f mana=%.0f hp=%.0f shatter=%d last=%s",
                s.bot->GetName(), ai::dpsfsm::StateName(st), s.myThreat, s.tankThreat,
                s.power, s.selfHp, m->shatterUsed ? 1 : 0, last);
        }

        return result;
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
            return TickResult::Hold;

        // ⑤ 移动中：只有瞬发打得出去（读条会被移动取消）⇒ 火焰冲击，不是火球
        if (s.moving)
            return FsmResult(CastFirst(ai, m, s, { "fire blast" }));

        // short 表第二步：目标约 25% 时用急冷重置冰冷血脉/燃烧（一场只重置一次，位 8 记账）
        if (plan == FirePlan::Short && !(m->fireBurstDone & 8)
            && s.targetHp <= float(TP(m, "cold_snap_hp", 25.0))
            && !Ready(s.bot, SpellId(ai, s.bot, m, "icy veins", false))
            && TryCastSelf(ai, m, s, "cold snap"))
        {
            m->fireBurstDone |= 8;
            return TickResult::Acted;
        }

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
            return TickResult::Hold;

        // 斩杀轮（位 4）：冰冷血脉还在 CD ⇒ 先用急冷重置一次（一场一次，位 8 记账）
        uint32 const icyId = SpellId(ai, s.bot, m, "icy veins", false);
        if (m->fireBurstRound == 4 && !(m->fireBurstDone & 8)
            && icyId && !Ready(s.bot, icyId)
            && TryCastSelf(ai, m, s, "cold snap"))
        {
            m->fireBurstDone |= 8;
            return TickResult::Acted;
        }

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
            ReloadTune();

            uint32 const hbNow = WorldTimer::getMSTime();
            bool const hbDue = (!m->hbLast || WorldTimer::getMSTimeDiff(m->hbLast, hbNow ? hbNow : 1) >= 10000);
            if (hbDue)
                m->hbLast = hbNow ? hbNow : 1;

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

            // 专精每 5 秒重判一次：假人场景/命令行动态换专精时，缓存的旧专精会让技能表全错
            {
                time_t const nowT = time(nullptr);
                if (m->spec == Spec::Unknown || !m->specCheckedAt || nowT - m->specCheckedAt >= 5)
                {
                    m->specCheckedAt = nowT;
                    m->spec = DetectSpec(ai);
                }
            }

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
            // [2026-11-01 站长口径] 术士只接**毁灭**（三系里最简单：主输出只有暗影箭）；
            //   Affliction / Demonology 继续走 legacy `TickWarlock`（保持 Decline ⇒ 不改变任何行为）。
            bool const warlockFsm = (m->spec == Spec::Destruction);
            // [2026-11-01 站长口径] 火法接进 mode=5（第六张职业图）：`TickFireFsm`。
            //   mode=0/1/2/3/4 的火法照旧走 legacy `TickMage`，这条判断只影响 mode=5。
            bool const mageFsm = (m->spec == Spec::Frost || m->spec == Spec::Arcane
                || m->spec == Spec::Fire);
            if (dmode == DecisionMode::Fsm
                && (mageFsm || IsHunterSpec(m->spec) || warriorFsm || rogueFsm || warlockFsm))
            {
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
                else
                    fsmResult = TickWarlockFsm(ai, m, s);
                // [2026-10-31 站长口径] 状态机本 tick 结束时结一次耗蓝账（10 秒节流，不每 tick 刷屏）
                ManaLogTick(m, bot);
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
