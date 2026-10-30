// =====================================================================================
// [2026-10-30 站长口径] **价值驱动（value-driven）重写**：不再"按专精查优先级表"，
//   而是每个决策点对 bot **法术书里所有输出技能**打分，取价值最高的一个。
//
//   value(skill) = 期望伤害 / 行动成本
//     期望伤害 = 基础伤害(DB) + 武器伤害×倍数；DoT 用"整段总量"
//     行动成本 = max(施法时间, 1.5s GCD) + 资源折算时间（怒气/能量/法力 ÷ 收入速度）
//   过滤条件全部来自 DB 与局面，不来自"我猜的职业分类"：
//     · 已学会 & 冷却好（Ready）
//     · 斩杀类（SpellEntry.TargetAuraState == 2）只在目标 < 20% 血时可用
//     · 连击点法术要求有星，伤害按星数放大
//     · DoT 类：目标身上已在跳就跳过（避免刷爆）
//     · 有半径的伤害法术按目标数放大（AoE）
//   状态不再是输入：Burst/Aoe 只是结果（我们会把"当前行为"记成遥测标签）。
//
//   开关：`value=1`（`*:value=1` 或 `spec:value=1`）。默认 0，便于与旧实现同轮 A/B。
//   分工：**伤害技能由价值引擎选**；自身 buff/圣印/图腾等维护交给原处理器（fallback），
//         因为它们的"价值"是提升后续伤害，用一次性 DB 伤害估不出来。
// =====================================================================================
namespace dpsvalue
{
    struct Cand
    {
        uint32 id = 0;
        std::string name;          // 法术名（覆盖率日志用）
        float  base = 0.0f;        // 基础伤害（DoT 为整段总量）
        float  weaponMul = 0.0f;   // 武器伤害倍数（物理技能）
        bool   dot = false;        // 周期性伤害
        bool   needsCombo = false; // 连击点法术
        uint32 auraState = 0;      // 需要的目标 aura state（2 = 低血斩杀）
        uint32 castMs = 0;
        uint32 durationMs = 0;
        int32  cost = 0;
        uint8  powerType = 0;      // 0=none 1=mana 2=rage 3=energy
        int    aoeTargets = 1;     // >1 表示可打多目标（按半径估算）
        // [2026-10-30 站长口径] 不是所有有用的技能都带伤害：切割/破甲/精灵之火这类是"吞吐修正"。
        //   识别靠 DB 的 aura 类型（机械映射），不靠手写清单：
        //     modKind: 1=攻速/急速  2=攻强  3=削护甲（负抗性）  4=易伤(受伤百分比)
        //   modValue 是该效果的幅度（正数），upkeep=true 表示"长期挂着、临近过期才续"。
        uint8  modKind = 0;
        float  modValue = 0.0f;
        bool   upkeep = false;
        bool   isDebuff = false;   // true = 挂在目标身上（破甲/精灵之火）
        // [2026-10-30 站长口径] 形态是外层状态机的选择，不能交给伤害分数：
        //   变形会清空能量/怒气、耗 GCD 与法力 ⇒ 候选里只允许"当前形态可用"的技能。
        uint32 schoolMask = 0;     // [2026-10-30 站长口径] 按角色实际属性估值：需要学派（法伤加成按学派算）
        float  coeff = 0.0f;       // 法术强度系数（读条技能 ≈ 施法时间/3.5；DoT ≈ 时长/15）
        uint32 requiredForm = 0;    // 0 = 无形态要求；否则是 Stances 位掩码
    };

    inline std::unordered_map<uint32, std::vector<Cand>>& Cache() { static std::unordered_map<uint32, std::vector<Cand>> c; return c; }
    inline std::unordered_map<uint32, time_t>& BuiltAt() { static std::unordered_map<uint32, time_t> b; return b; }

    // 法术书扫描：只留"能造成伤害"的法术（直接伤害/武器伤害/周期性伤害），且只留最高 rank。
    inline void Rebuild(Player* bot, uint32 guid)
    {
        std::vector<Cand> out;
        PlayerSpellMap const& spells = bot->GetSpellMap();
        for (auto const& kv : spells)
        {
            PlayerSpell const& ps = kv.second;
            if (!ps.active || ps.disabled)
                continue;
            uint32 const id = kv.first;
            SpellEntry const* sp = sSpellTemplate.LookupEntry<SpellEntry>(id);
            if (!sp)
                continue;
            // 只保留最高 rank（低 rank 只在没学高 rank 时才考虑）
            uint32 const next = sSpellMgr.GetNextSpellInChain(id);
            if (next && bot->HasSpell(next))
                continue;

            Cand c;
            c.id = id;
            c.name = sp->SpellName[0] ? sp->SpellName[0] : "?";
            c.auraState = sp->TargetAuraState;
            c.castMs = GetSpellCastTime(sp, bot);
            c.durationMs = GetSpellDuration(sp) > 0 ? uint32(GetSpellDuration(sp)) : 0;
            c.powerType = uint8(sp->powerType);
            c.requiredForm = sp->Stances;
            c.schoolMask = sp->SchoolMask;
            // 系数用经典近似：直接伤害 = 施法时间/3.5；周期性 = 总时长/15；瞬发按 0.43
            if (c.dot && c.durationMs)
                c.coeff = float(c.durationMs) / 15000.0f;
            else if (c.castMs > 0)
                c.coeff = float(c.castMs) / 3500.0f;
            else
                c.coeff = 0.43f;
            c.coeff = std::min(1.6f, std::max(0.15f, c.coeff));
            c.cost = int32(sp->manaCost);

            bool anyDamage = false;
            for (int i = 0; i < MAX_EFFECT_INDEX; ++i)
            {
                uint32 const eff = sp->Effect[i];
                int32 const simple = sp->CalculateSimpleValue(SpellEffectIndex(i));
                if (eff == SPELL_EFFECT_SCHOOL_DAMAGE)
                {
                    c.base += float(simple > 0 ? simple : 0);
                    anyDamage = true;
                    if (sp->EffectRadiusIndex[i])
                        c.aoeTargets = 5;         // 地面/范围伤害：按目标数放大
                }
                else if (eff == SPELL_EFFECT_WEAPON_DAMAGE || eff == SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL)
                {
                    c.weaponMul += 1.0f;
                    c.base += float(simple > 0 ? simple : 0);
                    anyDamage = true;
                }
                else if (eff == SPELL_EFFECT_WEAPON_PERCENT_DAMAGE)
                {
                    c.weaponMul += float(simple > 0 ? simple : 0) / 100.0f;
                    anyDamage = true;
                }
                else if (eff == SPELL_EFFECT_APPLY_AURA && sp->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_DAMAGE)
                {
                    int32 const amp = int32(sp->EffectAmplitude[i]);
                    int32 const dur = GetSpellDuration(sp);
                    int ticks = (amp > 0 && dur > 0) ? (dur / amp) : 1;
                    if (ticks < 1) ticks = 1;
                    c.base += float(simple > 0 ? simple : 0) * float(ticks);
                    c.dot = true;
                    anyDamage = true;
                }
            }
            // [2026-10-30] 没有直接伤害也没关系：识别"吞吐修正类"（只放开收益可估的三类）
            if (!anyDamage || c.base <= 0.0f)
            {
                for (int i = 0; i < MAX_EFFECT_INDEX; ++i)
                {
                    if (sp->Effect[i] != SPELL_EFFECT_APPLY_AURA)
                        continue;
                    uint32 const aura = sp->EffectApplyAuraName[i];
                    int32 const val = sp->CalculateSimpleValue(SpellEffectIndex(i));
                    uint8 kind = 0;
                    if (aura == SPELL_AURA_MOD_MELEE_HASTE)                       kind = 1;   // 切割类
                    else if (aura == SPELL_AURA_MOD_ATTACK_POWER
                        || aura == SPELL_AURA_MOD_RANGED_ATTACK_POWER)           kind = 2;   // 攻强 buff
                    else if (aura == SPELL_AURA_MOD_RESISTANCE && val < 0)       kind = 3;   // 削护甲（破甲/精灵之火）
                    else if (aura == SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN)        kind = 4;   // 易伤
                    if (!kind)
                        continue;
                    c.modKind = kind;
                    c.modValue = float(val < 0 ? -val : val) / (kind == 2 ? 1.0f : 100.0f);
                    c.upkeep = (c.durationMs >= 8000);
                    // 目标型 debuff：需要选中敌人（AuraState/范围判定交给 CheckCast）
                    c.isDebuff = (kind == 3 || kind == 4);
                    break;
                }
                if (!c.modKind)
                    continue;
            }
            // 连击点法术：DB 的 AttributesEx 带 REQ_COMBO_POINTS(0x00040000) 位
            if (sp->AttributesEx & 0x00040000)
                c.needsCombo = true;

            out.push_back(c);
        }
        Cache()[guid] = out;
        BuiltAt()[guid] = time(nullptr);
        sLog.outError("[DPSVALUE] %s 候选技能 %u 个（法术书 %u）",
            bot->GetName(), uint32(out.size()), uint32(spells.size()));
    }

    inline std::vector<Cand>& Candidates(Player* bot, uint32 guid)
    {
        auto it = BuiltAt().find(guid);
        if (it == BuiltAt().end() || time(nullptr) - it->second > 120)
            Rebuild(bot, guid);
        return Cache()[guid];
    }

    // 资源折算成"秒"（收入速度按经验常数；目的只是让不同技能的消耗可比）
    inline float ResourceSeconds(Player* bot, Cand const& c)
    {
        if (c.cost <= 0)
            return 0.0f;
        switch (c.powerType)
        {
            case POWER_RAGE:   return float(c.cost) / 3.0f;      // 约 3 怒/秒
            case POWER_ENERGY: return float(c.cost) / 10.0f;     // 10 能量/秒
            case POWER_MANA:
            {
                float const maxMana = float(bot->GetMaxPower(POWER_MANA));
                float const regen = std::max(30.0f, maxMana * 0.02f);
                return float(c.cost) / regen;
            }
            default: return 0.0f;
        }
    }

    inline bool HasResource(Player* bot, Cand const& c)
    {
        if (c.cost <= 0)
            return true;
        if (c.powerType == POWER_RAGE)
            return bot->GetPower(POWER_RAGE) >= uint32(c.cost);
        if (c.powerType == POWER_ENERGY)
            return bot->GetPower(POWER_ENERGY) >= uint32(c.cost);
        if (c.powerType == POWER_MANA)
            return bot->GetPower(POWER_MANA) >= uint32(c.cost);
        return true;
    }
}

namespace dpsvalue
{
    // 按 id 施放（复制 CastChecked 的校验/退避逻辑，但不再走"名字→id"这一步）：
    //   价值引擎手里已经是 id，而且必须用**已学会的最高 rank**。
    inline bool CastId(PlayerbotAI* ai, BotMem* m, Snapshot& s, uint32 id, Unit* target)
    {
        if (!id || !s.bot->HasSpell(id))
            return false;
        if (!Ready(s.bot, id))
            return false;

        time_t const now = time(nullptr);
        auto it = m->failUntil.find(id);
        if (it != m->failUntil.end() && now < it->second)
            return false;

        Unit* tgt = target ? target : s.target;
        SpellCastResult res = SPELL_FAILED_DONT_REPORT;
        if (!ai->CanCastSpell(id, tgt, 0, false, nullptr, false, false, false, &res))
        {
            // 自身增益/形态类：对敌施放会被判 108/110，改走核心自放（同 CastChecked 的处理）
            bool const selfFix = (res == SPELL_FAILED_TARGET_FRIENDLY)
                || (res == SPELL_FAILED_TARGET_ENEMY && tgt != s.bot);
            if (selfFix)
            {
                SpellCastResult const res2 = s.bot->CastSpell(s.bot, id, uint32(TRIGGERED_NONE));
                if (res2 == SPELL_CAST_OK)
                {
                    ++m->castCount;
                    m->lastOk[id] = now;
                    m->castThisTick = true;
                    PlayerbotAI::RecordSpellCast(s.bot->GetObjectGuid(), id);
                    return true;
                }
                res = res2;
            }
            bool const waitingResource = (res == SPELL_FAILED_NO_POWER
                || res == SPELL_FAILED_OUT_OF_RANGE || res == SPELL_FAILED_LINE_OF_SIGHT
                || res == SPELL_FAILED_NOT_READY);
            m->failUntil[id] = now + (waitingResource ? 1 : 2);
            return false;
        }
        if (!ai->CastSpell(id, tgt))
            return false;
        ++m->castCount;
        m->lastOk[id] = now;
        m->castThisTick = true;
        PlayerbotAI::RecordSpellCast(s.bot->GetObjectGuid(), id);
        return true;
    }
}

    // [2026-10-30] 价值驱动主循环：对所有候选技能打分，放最高的那个。
    //   返回 true = 本 tick 已决定（放了技能，或资源不够"有意等"）。
    //   返回 false = 没有可用的伤害技能 ⇒ 交给原处理器做 buff/圣印/图腾等维护。
    bool TickValue(PlayerbotAI* ai, BotMem* m)
    {
        Snapshot s;
        BuildSnapshot(ai, m, s);
        if (!s.target)
            return false;
        if (s.casting)
            return true;

        std::vector<dpsvalue::Cand>& cands = dpsvalue::Candidates(s.bot, s.bot->GetGUIDLow());
        float const weaponHit = s.bot->CalculateDamage(BASE_ATTACK, false);
        int const pack = std::max(s.pack10, 1);

        int bestIdx = -1;
        float bestVal = -1.0f;
        bool bestHasResource = true;

        for (size_t i = 0; i < cands.size(); ++i)
        {
            dpsvalue::Cand const& c = cands[i];
            if (!Ready(s.bot, c.id))
                continue;
            if (c.auraState == 2 && s.targetHp > 20.0f)          // 斩杀类：目标血不够低
                continue;
            if (c.needsCombo && s.combo == 0)
                continue;
            if (c.dot && s.target->GetSpellAuraHolder(c.id))     // DoT 还在跳
                continue;

            float dmg = c.base + c.weaponMul * weaponHit;
            if (c.needsCombo)
                dmg *= 0.35f + 0.13f * float(s.combo);           // 星越多伤害越高（近似）
            if (c.aoeTargets > 1)
                dmg *= float(std::min(pack, c.aoeTargets));

            float const actSec = std::max(1.5f, float(c.castMs) / 1000.0f)
                + dpsvalue::ResourceSeconds(s.bot, c);
            float const val = dmg / std::max(0.2f, actSec);
            if (val > bestVal)
            {
                bestVal = val;
                bestIdx = int(i);
                bestHasResource = dpsvalue::HasResource(s.bot, c);
            }
        }

        if (bestIdx < 0)
            return false;

        dpsvalue::Cand const& best = cands[size_t(bestIdx)];
        if (!bestHasResource)
        {
            // 资源不够：有意等（能量/怒气职业等资源常常比硬塞低效技能更划算）
            m->state = State::Recover;
            return true;
        }
        if (dpsvalue::CastId(ai, m, s, best.id, s.target))
        {
            m->state = State::Burn;
            return true;
        }
        return false;
    }

namespace dpsvalue
{
    // [2026-10-30 站长口径] 表内价值驱动：给策略表里的一步算"值多少"。
    //   返回 false = 这一步没有伤害元数据（buff/圣印/姿态这类），调用方按顺序兜底。
    inline bool ValueOfId(PlayerbotAI* ai, BotMem* m, Snapshot& s, uint32 id, float& outValue)
    {
        if (!id)
            return false;
        std::vector<Cand>& cands = Candidates(s.bot, s.bot->GetGUIDLow());
        for (Cand const& c : cands)
        {
            if (c.id != id)
                continue;
            if (c.modKind)
            {
                // [2026-10-30] 吞吐修正类：value = 参考DPS × 幅度 × 覆盖权重
                //   参考DPS 由调用方（策略表打分）用"当前最高伤害价值"传入；这里用 Unity 占位 1.0，
                //   实际乘在 TickPolicy 里完成（那里能拿到 Vmax）。
                float cover = 1.0f;
                if (c.upkeep && c.durationMs > 0)
                {
                    SpellAuraHolder* holder = (c.isDebuff ? s.target : s.bot)->GetSpellAuraHolder(c.id);
                    if (holder)
                    {
                        int const remain = holder->GetAuraDuration();
                        // 剩余 > 40% 时长 ⇒ 现在补是浪费 GCD（覆盖权重 0）
                        cover = (remain < 0 || remain > int(c.durationMs) * 2 / 5) ? 0.0f
                              : (1.0f - float(remain) / float(c.durationMs));
                    }
                }
                if (cover <= 0.0f)
                    return false;
                outValue = c.modValue * cover;      // 归一化幅度（0~1 量级），调用方乘 Vmax
                return true;
            }

            float dmg = c.base + c.weaponMul * s.bot->CalculateDamage(BASE_ATTACK, false);
            if (c.needsCombo)
                dmg *= 0.35f + 0.13f * float(s.combo);
            if (c.aoeTargets > 1)
                dmg *= float(std::min(std::max(s.pack10, 1), c.aoeTargets));
            if (dmg <= 0.0f)
                return false;
            float const actSec = std::max(1.5f, float(c.castMs) / 1000.0f) + ResourceSeconds(s.bot, c);
            outValue = dmg / std::max(0.2f, actSec);
            return true;
        }
        return false;
    }

    inline bool ResourceOk(PlayerbotAI* ai, BotMem* m, Snapshot& s, uint32 id)
    {
        std::vector<Cand>& cands = Candidates(s.bot, s.bot->GetGUIDLow());
        for (Cand const& c : cands)
            if (c.id == id)
                return HasResource(s.bot, c);
        return true;        // 没有元数据（buff 类）：交给 CastChecked 自己判
    }
}

namespace dpsvalue
{
    // 这个 id 是不是"吞吐修正类"（切割/破甲/精灵之火这类没有伤害但有收益的）
    inline bool IsUpkeepId(PlayerbotAI* ai, BotMem* m, Snapshot& s, uint32 id, float& outMagnitude)
    {
        std::vector<Cand>& cands = Candidates(s.bot, s.bot->GetGUIDLow());
        for (Cand const& c : cands)
        {
            if (c.id != id || !c.modKind)
                continue;
            float v = 0.0f;
            if (!ValueOfId(ai, m, s, id, v))
                return false;
            outMagnitude = v;
            return true;
        }
        return false;
    }
}

namespace dpsvalue
{
    // =====================================================================================
    // [2026-10-30 站长口径] boss 身上 debuff 的互斥性分析：
    //   ① 默认允许叠加（同类不一定互斥：破甲与精灵之火就叠）
    //   ② 只在"能证明会被顶掉/无收益"时判 0：同 aura 类型 + 同 EffectMiscValue（同一效果槽）
    //      且目标身上已有幅度 >= 我的
    //   ③ 判不了的组合不猜，只计数（dup），交给实测数据决定后续规则
    // =====================================================================================
    inline bool DebuffSuperseded(Unit* target, uint32 myId)
    {
        if (!target)
            return false;
        SpellEntry const* mine = sSpellTemplate.LookupEntry<SpellEntry>(myId);
        if (!mine)
            return false;
        for (auto const& kv : target->GetSpellAuraHolderMap())
        {
            SpellAuraHolder* h = kv.second;
            if (!h || h->GetId() == myId)
                continue;
            SpellEntry const* other = h->GetSpellProto();
            if (!other)
                continue;
            for (int i = 0; i < MAX_EFFECT_INDEX; ++i)
            {
                uint32 const a = mine->EffectApplyAuraName[i];
                if (a != SPELL_AURA_MOD_RESISTANCE && a != SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN)
                    continue;
                int32 const mv = mine->CalculateSimpleValue(SpellEffectIndex(i));
                int32 const mm = int32(mine->EffectMiscValue[i]);
                if (!mv)
                    continue;
                for (int j = 0; j < MAX_EFFECT_INDEX; ++j)
                {
                    if (other->EffectApplyAuraName[j] != a)
                        continue;
                    if (int32(other->EffectMiscValue[j]) != mm)
                        continue;                  // 不同效果槽 ⇒ 允许叠加
                    int32 const ov = other->CalculateSimpleValue(SpellEffectIndex(j));
                    if (ov && std::abs(ov) >= std::abs(mv))
                        return true;
                }
            }
        }
        return false;
    }

    // 队友重复：认领表（同一目标 + 同一效果槽，只让一个 bot 负责刷）
    struct Claim
    {
        uint32 owner = 0;
        time_t until = 0;
    };

    inline std::map<std::pair<uint32, uint32>, Claim>& ClaimMap()
    {
        static std::map<std::pair<uint32, uint32>, Claim> m;
        return m;
    }

    inline uint32 SlotSignature(uint32 myId)
    {
        SpellEntry const* sp = sSpellTemplate.LookupEntry<SpellEntry>(myId);
        if (!sp)
            return 0;
        for (int i = 0; i < MAX_EFFECT_INDEX; ++i)
        {
            uint32 const a = sp->EffectApplyAuraName[i];
            if (a != SPELL_AURA_MOD_RESISTANCE && a != SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN)
                continue;
            return a * 100000u + uint32(sp->EffectMiscValue[i] & 0xFFFF);
        }
        return 0;
    }

    inline bool ClaimedByOther(Unit* target, uint32 myId, uint32 meGuid)
    {
        if (!target)
            return false;
        uint32 const sig = SlotSignature(myId);
        if (!sig)
            return false;
        auto const it = ClaimMap().find(std::make_pair(uint32(target->GetGUIDLow()), sig));
        if (it == ClaimMap().end() || it->second.owner == meGuid)
            return false;
        return it->second.until > time(nullptr);
    }

    inline void ClaimSlot(Unit* target, uint32 myId, uint32 meGuid, uint32 durationMs)
    {
        if (!target)
            return;
        uint32 const sig = SlotSignature(myId);
        if (!sig)
            return;
        Claim& c = ClaimMap()[std::make_pair(uint32(target->GetGUIDLow()), sig)];
        c.owner = meGuid;
        c.until = time(nullptr) + (time_t)std::max<uint32>(1000, durationMs) / 1000;
    }
}

namespace dpsvalue
{
    inline Cand const* FindCand(Player* bot, uint32 guid, uint32 id)
    {
        std::vector<Cand>& cands = Candidates(bot, guid);
        for (Cand const& c : cands)
            if (c.id == id)
                return &c;
        return nullptr;
    }

    // [2026-10-30 修] 吞吐类技能的**正确量纲**：整段时长带来的等效伤害 ÷ 一个 GCD。
    //   之前写成 "Vmax × 幅度" ⇒ 切割(30%) 只有 0.3×Vmax，永远抢不过攒星技，
    //   结果 policy/pauto 模式下切割与剔骨全丢（实测只剩邪恶攻击 12 次、dps 428）。
    //   现在：value = Vmax × 幅度 × (新时长 − 已剩时长) / GCD
    //     · 切割 30% × 20s / 1.5s = 4.0 × Vmax ⇒ 该补
    //     · 5% × 10s / 1.5s = 0.33 × Vmax ⇒ 不该补
    inline float UpkeepValue(Player* bot, uint32 guid, float vmax, float magnitude, uint32 id)
    {
        Cand const* c = FindCand(bot, guid, id);
        float durSec = 0.0f;
        if (c && c->durationMs)
            durSec = float(c->durationMs) / 1000.0f;
        if (durSec <= 0.0f)
            durSec = 10.0f;                      // 无时长信息（自身瞬发增益）：按 10 秒估
        float remainSec = 0.0f;
        if (c && c->upkeep)
        {
            Unit* who = c->isDebuff ? nullptr : bot;
            if (who)
            {
                SpellAuraHolder* h = who->GetSpellAuraHolder(id);
                if (h)
                {
                    int const rem = h->GetAuraDuration();
                    if (rem > 0)
                        remainSec = float(rem) / 1000.0f;
                }
            }
        }
        float const gainSec = std::max(0.0f, durSec - remainSec);
        return vmax * magnitude * gainSec / 1.5f;
    }
}

namespace dpsvalue
{
    // 当前形态是否满足技能要求（0 = 任意形态）。用核心的 SpellEntry::CanBeUsedInForm 判，
    //   避免自己拼 Stances 位掩码（之前想用 GetShapeshiftFormMask，核心没有这个接口）。
    inline bool FormAllowed(Player* bot, Cand const& c)
    {
        SpellEntry const* sp = sSpellTemplate.LookupEntry<SpellEntry>(c.id);
        if (!sp)
            return true;
        ShapeshiftForm const form = bot->GetShapeshiftForm();
        uint32 const mask = (form == FORM_NONE) ? 0u : (1u << (uint32(form) - 1));

        // ① 有 StancesNot 且命中当前形态 ⇒ 施放会打断形态（实测野德 40 秒变形 6 次就是这么来的）
        if (mask && (sp->StancesNot & mask))
            return false;
        // ② 有形态要求 ⇒ 当前形态必须满足
        if (sp->Stances)
        {
            if (!mask)
                return false;                   // 人形放不了
            return (sp->Stances & mask) != 0;
        }
        return true;
    }
}

namespace dpsvalue
{
    // [2026-10-30 站长口径] **按角色实际属性估值**，而不是只看技能基础伤害：
    //   伤害 = 基础伤害 + 武器伤害×倍数 + 法伤×系数，再乘暴击期望。
    //   法伤从 SpellBaseDamageBonusDone(学派) 取（这就是角色面板上的法伤）；
    //   暴击用 CalculateSpellCritChance(victim, 学派, 法术)。
    inline float EstDamage(Player* bot, Unit* target, Cand const& c, float weaponHit, uint8 combo, int pack)
    {
        float dmg = c.base + c.weaponMul * weaponHit;
        if (c.schoolMask && c.coeff > 0.0f)
        {
            int32 const sp = bot->SpellBaseDamageBonusDone(SpellSchoolMask(c.schoolMask));
            if (sp > 0)
                dmg += float(sp) * c.coeff;
        }
        if (c.needsCombo)
            dmg *= 0.35f + 0.13f * float(combo);
        if (c.aoeTargets > 1)
            dmg *= float(std::min(std::max(pack, 1), c.aoeTargets));

        // 暴击期望（法术暴击；物理技能用同一接口的近似，误差可接受）
        if (target)
        {
            SpellEntry const* sp = sSpellTemplate.LookupEntry<SpellEntry>(c.id);
            if (sp)
            {
                float const crit = bot->CalculateSpellCritChance(target, SpellSchoolMask(c.schoolMask ? c.schoolMask : SPELL_SCHOOL_MASK_NORMAL), sp);
                if (crit > 0.0f)
                    dmg *= 1.0f + crit / 100.0f;
            }
        }
        return dmg;
    }
}
