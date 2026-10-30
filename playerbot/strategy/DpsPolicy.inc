// =====================================================================================
// [2026-10-30 站长口径] **职业 = 一个技能空间；专精 = 从空间里挑一条策略，分别调优。**
//
//   为什么这么改：
//     随机 bot 的法术书是按等级把**整个职业**的训练师技能全学会的（实测武器战 bot 的法术书里
//     同时有 致死/嗜血/猛击/旋风斩/斩杀/压制/英勇/盾猛/复仇）。所以"专精"不该决定"能用什么技能"，
//     只该决定"优先用哪些、怎么组合"。
//
//   模型：
//     - 空间 AbilitySpace(class)：该职业所有输出相关技能名（跨三系；没学会的会被自动跳过）。
//     - 策略 Policy：某专精从空间里取一条有序步骤表（可以包含**别的系**的技能）。
//     - 调优：用 `.rndbot sim arms` 对同一专精并发扫不同策略变体，只采纳实测更优的。
//
//   开关：`*:policy=1`（或 `spec:policy=1`）才走这里；默认 0 = 走原来的分专精处理器，
//         这样两条路线可以同轮 A/B，风险可控。
// =====================================================================================
namespace dpspolicy
{
    enum class PKind : uint8
    {
        Target,     // 对当前目标施放
        Self,       // 对自己施放（圣印/姿态/形态/自身增益）
        Ground,     // 对目标脚下地面施放（暴风雪/烈焰风暴/火雨/飓风/箭雨）
        Swing       // 下一次挥击附带（英勇打击/顺劈）
    };

    struct PStep
    {
        char const* name = nullptr;
        PKind kind = PKind::Target;
        char const* needAuraGone = nullptr;   // 目标身上没有这个 debuff 才放（DoT/破甲/灼烧）
        int minCombo = 0;
        int minEnergy = 0;
        int minRage = 0;
        char const* needForm = nullptr;       // 这个技能要求的形态（不在形态里就跳过）
        int minPack = 0;                      // 目标数下限（AoE 才放）
        float targetHpBelow = 100.0f;         // 斩杀类：目标血量百分比上限
        char const* note = nullptr;           // 调优备注（日志用）
        char const* enterForm = nullptr;      // 这一手是"切形态/姿态"：已经在该形态就跳过
        // [2026-10-30] 让"用不用这一手/门槛多少"变成可并发扫描的参数（按专精分别调优）
        char const* enableParam = nullptr;    // 非空时：TP(param, default) >= 0.5 才允许这一手
        double enableDefault = 1.0;
        char const* rageParam = nullptr;      // 非空时：minRage 取 TP(param, default)
        double rageDefault = 60.0;
    };

    struct PPolicy
    {
        char const* label = nullptr;
        PStep const* steps = nullptr;
        int count = 0;
    };

    // ---- 职业技能空间（全三系输出技能；学到才会用）----
    inline std::vector<char const*> AbilitySpace(uint8 cls)
    {
        switch (cls)
        {
            case CLASS_WARRIOR: return { "berserker stance", "battle shout", "rend", "mortal strike",
                                         "bloodthirst", "whirlwind", "slam", "overpower", "execute",
                                         "heroic strike", "cleave", "sweeping strikes", "death wish", "recklessness" };
            case CLASS_ROGUE:   return { "slice and dice", "sinister strike", "mutilate", "backstab", "hemorrhage",
                                         "eviscerate", "envenom", "rupture", "blade flurry", "adrenaline rush", "cold blood" };
            case CLASS_MAGE:    return { "fireball", "frostbolt", "arcane blast", "arcane missiles", "scorch",
                                         "fire blast", "pyroblast", "ice lance", "blizzard", "flamestrike",
                                         "cone of cold", "arcane explosion", "icy veins", "arcane power",
                                         "combustion", "evocation", "summon water elemental" };
            case CLASS_WARLOCK: return { "immolate", "corruption", "curse of agony", "curse of elements",
                                         "siphon life", "unstable affliction", "seed of corruption", "conflagrate",
                                         "rain of fire", "shadow bolt", "incinerate", "drain life" };
            case CLASS_SHAMAN:  return { "stormstrike", "earth shock", "flame shock", "frost shock",
                                         "lightning bolt", "chain lightning", "searing totem", "fire nova" };
            case CLASS_DRUID:   return { "cat form", "mangle", "shred", "rake", "rip", "ferocious bite", "tiger's fury",
                                         "wrath", "starfire", "moonfire", "insect swarm", "hurricane" };
            case CLASS_PRIEST:  return { "shadow word: pain", "vampiric touch", "mind blast", "mind flay",
                                         "shadow word: death", "smite", "holy fire" };
            case CLASS_PALADIN: return { "seal of blood", "seal of command", "seal of righteousness", "judgement",
                                         "crusader strike", "consecration", "exorcism", "holy shock", "hammer of wrath" };
            case CLASS_HUNTER:  return { "serpent sting", "steady shot", "multi-shot", "arcane shot", "aimed shot",
                                         "volley", "kill command" };
            default:            return {};
        }
    }

    // ---- 分专精策略（默认值 = 当前实测最好的顺序；跨系步骤已在各条里标出）----

    // 武器战策略（跨系可选件：旋风斩、拦腰斩/Rend、英勇打击门槛都可扫）
    static PStep const kArms[] = {
        { "battle shout",       PKind::Self,  "battle shout" },
        { "execute",            PKind::Target, nullptr, 0, 0, 0, nullptr, 0, 20.0f, "execute" },
        { "mortal strike",      PKind::Target },
        { "whirlwind",          PKind::Target, nullptr, 0, 0, 25, nullptr, 0, 100.0f, "cross:ww" },
        { "overpower",          PKind::Target },
        { "slam",               PKind::Target },
        { "rend",               PKind::Target, "rend", 0, 0, 0, nullptr, 0, 100.0f, "dot:rend", nullptr, "rend", 0.0 },
        { "heroic strike",      PKind::Swing,  nullptr, 0, 0, 0, nullptr, 0, 100.0f, "swing", nullptr, nullptr, 1.0, "hs_rage", 75.0 },
    };

    // 狂暴战：狂暴姿态 > 嗜血 > 旋风斩 > 斩杀 > 英勇(怒气富余时)
    static PStep const kFury[] = {
        { "battle shout",       PKind::Self,  "battle shout" },
        { "berserker stance",   PKind::Self,  nullptr, 0, 0, 0, nullptr, 0, 100.0f, "stance", "berserker" },
        { "death wish",         PKind::Self },
        { "execute",            PKind::Target, nullptr, 0, 0, 0, nullptr, 0, 20.0f, "execute" },
        { "bloodthirst",        PKind::Target },
        { "whirlwind",          PKind::Target, nullptr, 0, 0, 25 },
        { "heroic strike",      PKind::Swing,  nullptr, 0, 0, 45 },
    };

    // 战斗贼：切割 > 剑刃乱舞 > 冲动 > 邪恶攻击 > 剔骨
    static PStep const kCombat[] = {
        { "slice and dice",     PKind::Self,  "slice and dice", 1 },
        { "blade flurry",       PKind::Self },
        { "adrenaline rush",    PKind::Self },
        { "sinister strike",    PKind::Target },
        { "eviscerate",         PKind::Target, nullptr, 5 },
    };

    // 刺杀贼：切割 > 冷血 > 毁伤 > 毒伤(不可用则剔骨)
    static PStep const kAssa[] = {
        { "slice and dice",     PKind::Self,  "slice and dice", 1 },
        { "cold blood",         PKind::Self },
        { "mutilate",           PKind::Target },
        { "envenom",            PKind::Target, nullptr, 4 },
        { "eviscerate",         PKind::Target, nullptr, 4 },
    };

    // 火法：灼烧叠 5 层 > 燃烧 > 火球 > 火焰冲击 > 烈焰风暴(群)
    static PStep const kFire[] = {
        { "combustion",         PKind::Self },
        { "scorch",             PKind::Target, "fire vulnerability" },
        { "fireball",           PKind::Target },
        { "fire blast",         PKind::Target },
        { "flamestrike",        PKind::Ground, nullptr, 0, 0, 0, nullptr, 2 },
    };

    // 冰法：冰冷血脉 > 水元素 > 寒冰箭 > 冰枪术 > 暴风雪(群)
    static PStep const kFrost[] = {
        { "icy veins",          PKind::Self },
        { "summon water elemental", PKind::Self },
        { "frostbolt",          PKind::Target },
        { "ice lance",          PKind::Target },
        { "blizzard",           PKind::Ground, nullptr, 0, 0, 0, nullptr, 2 },
    };

    // 奥法：奥强 > 奥术冲击 > 奥术飞弹 > 魔爆(群)
    static PStep const kArcane[] = {
        { "arcane power",       PKind::Self },
        { "arcane blast",       PKind::Target },
        { "arcane missiles",    PKind::Target },
        { "arcane explosion",   PKind::Target, nullptr, 0, 0, 0, nullptr, 2 },
    };

    // 痛苦术：痛苦无常 > 腐蚀 > 痛苦诅咒 > 生命虹吸 > 暗影箭
    static PStep const kAffl[] = {
        { "unstable affliction", PKind::Target, "unstable affliction" },
        { "corruption",         PKind::Target, "corruption" },
        { "curse of agony",     PKind::Target, "curse of agony" },
        { "siphon life",        PKind::Target, "siphon life" },
        { "shadow bolt",        PKind::Target },
    };

    // 毁灭术：元素诅咒 > 献祭 > 燃烧 > 腐蚀(跨系填充) > 暗影箭 > 火雨(群)
    static PStep const kDestro[] = {
        { "curse of elements",  PKind::Target, "curse of elements" },
        { "immolate",           PKind::Target, "immolate" },
        { "conflagrate",        PKind::Target },
        { "corruption",         PKind::Target, "corruption" },
        { "shadow bolt",        PKind::Target },
        { "rain of fire",       PKind::Ground, nullptr, 0, 0, 0, nullptr, 2 },
    };

    // 恶魔术：献祭 > 腐蚀 > 暗影箭
    static PStep const kDemo[] = {
        { "immolate",           PKind::Target, "immolate" },
        { "corruption",         PKind::Target, "corruption" },
        { "shadow bolt",        PKind::Target },
    };

    // 元素萨：火震维护 > 闪电链(群) > 闪电箭 > 地震
    static PStep const kEle[] = {
        { "flame shock",        PKind::Target, "flame shock" },
        { "chain lightning",    PKind::Target, nullptr, 0, 0, 0, nullptr, 2 },
        { "lightning bolt",     PKind::Target },
        { "earth shock",        PKind::Target },
    };

    // 增强萨：风暴打击 > 火震 > 地震 > 冰震
    static PStep const kEnh[] = {
        { "stormstrike",        PKind::Target },
        { "flame shock",        PKind::Target, "flame shock" },
        { "earth shock",        PKind::Target },
        { "frost shock",        PKind::Target },
    };

    // 平衡德：虫群 > 月火 > 星火 > 愤怒 > 飓风(群)
    static PStep const kBalance[] = {
        { "insect swarm",       PKind::Target, "insect swarm" },
        { "moonfire",           PKind::Target, "moonfire" },
        { "starfire",           PKind::Target },
        { "wrath",              PKind::Target },
        { "hurricane",          PKind::Ground, nullptr, 0, 0, 0, nullptr, 2 },
    };

    // 野德：猫形态 > 猛虎 > 扫击 > 割裂 > 撕碎 > 凶猛撕咬
    static PStep const kFeral[] = {
        { "cat form",           PKind::Self,  nullptr, 0, 0, 0, nullptr, 0, 100.0f, "form", "cat" },
        { "tiger's fury",       PKind::Self,  nullptr, 0, 0, 0, nullptr, 0, 100.0f, "energy>=80" },
        { "rake",               PKind::Target, "rake" },
        { "rip",                PKind::Target, "rip", 4 },
        { "mangle",             PKind::Target },
        { "shred",              PKind::Target },
        { "ferocious bite",     PKind::Target, nullptr, 5 },
    };

    // 暗牧：痛 > 吸血鬼之触 > 心灵震爆 > 心灵尖刺 > 死亡(斩杀)
    static PStep const kShadow[] = {
        { "shadow word: pain",  PKind::Target, "shadow word: pain" },
        { "vampiric touch",     PKind::Target, "vampiric touch" },
        { "shadow word: death", PKind::Target, nullptr, 0, 0, 0, nullptr, 0, 20.0f, "execute" },
        { "mind blast",         PKind::Target },
        { "mind flay",          PKind::Target },
    };

    // 惩戒骑：圣印维护 > 审判 > 十字军打击 > 奉献 > 愤怒之锤(低血)
    static PStep const kRet[] = {
        { "seal of blood",      PKind::Self,  "seal of blood" },
        { "seal of command",    PKind::Self,  "seal of command" },
        { "judgement",          PKind::Target },
        { "crusader strike",    PKind::Target },
        { "consecration",       PKind::Self },
        { "hammer of wrath",    PKind::Target, nullptr, 0, 0, 0, nullptr, 0, 20.0f, "execute" },
    };

    // 兽王猎：毒蛇钉刺 > 杀戮命令 > 稳固 > 多重 > 奥射 > 箭雨(群)
    static PStep const kBM[] = {
        { "serpent sting",      PKind::Target, "serpent sting" },
        { "kill command",       PKind::Target },
        { "steady shot",        PKind::Target },
        { "multi-shot",         PKind::Target },
        { "arcane shot",        PKind::Target },
        { "volley",             PKind::Ground, nullptr, 0, 0, 0, nullptr, 2 },
    };

    // 射击猎：毒蛇 > 瞄准 > 稳固 > 多重 > 奥射
    static PStep const kMarks[] = {
        { "serpent sting",      PKind::Target, "serpent sting" },
        { "aimed shot",         PKind::Target },
        { "steady shot",        PKind::Target },
        { "multi-shot",         PKind::Target },
        { "arcane shot",        PKind::Target },
    };

    inline PPolicy const* PolicyFor(uint8 cls, Spec spec)
    {
        static PPolicy const arms   { "arms",    kArms,    int(sizeof(kArms) / sizeof(kArms[0])) };
        static PPolicy const fury   { "fury",    kFury,    int(sizeof(kFury) / sizeof(kFury[0])) };
        static PPolicy const combat { "combat",  kCombat,  int(sizeof(kCombat) / sizeof(kCombat[0])) };
        static PPolicy const assa   { "assa",    kAssa,    int(sizeof(kAssa) / sizeof(kAssa[0])) };
        static PPolicy const fire   { "fire",    kFire,    int(sizeof(kFire) / sizeof(kFire[0])) };
        static PPolicy const frost  { "frost",   kFrost,   int(sizeof(kFrost) / sizeof(kFrost[0])) };
        static PPolicy const arcane { "arcane",  kArcane,  int(sizeof(kArcane) / sizeof(kArcane[0])) };
        static PPolicy const affl   { "affl",    kAffl,    int(sizeof(kAffl) / sizeof(kAffl[0])) };
        static PPolicy const destro { "destro",  kDestro,  int(sizeof(kDestro) / sizeof(kDestro[0])) };
        static PPolicy const demo   { "demo",    kDemo,    int(sizeof(kDemo) / sizeof(kDemo[0])) };
        static PPolicy const ele    { "ele",     kEle,     int(sizeof(kEle) / sizeof(kEle[0])) };
        static PPolicy const enh    { "enh",     kEnh,     int(sizeof(kEnh) / sizeof(kEnh[0])) };
        static PPolicy const balance{ "balance", kBalance, int(sizeof(kBalance) / sizeof(kBalance[0])) };
        static PPolicy const feral  { "feral",   kFeral,   int(sizeof(kFeral) / sizeof(kFeral[0])) };
        static PPolicy const shadow { "shadow",  kShadow,  int(sizeof(kShadow) / sizeof(kShadow[0])) };
        static PPolicy const ret    { "ret",     kRet,     int(sizeof(kRet) / sizeof(kRet[0])) };
        static PPolicy const bm     { "bm",      kBM,      int(sizeof(kBM) / sizeof(kBM[0])) };
        static PPolicy const marks  { "marks",   kMarks,   int(sizeof(kMarks) / sizeof(kMarks[0])) };

        switch (spec)
        {
            case Spec::Arms:          return &arms;
            case Spec::Fury:          return &fury;
            case Spec::Protection:    return &arms;
            case Spec::Combat:        return &combat;
            case Spec::Assassination: return &assa;
            case Spec::Subtlety:      return &assa;
            case Spec::Fire:          return &fire;
            case Spec::Frost:         return &frost;
            case Spec::Arcane:        return &arcane;
            case Spec::Affliction:    return &affl;
            case Spec::Destruction:   return &destro;
            case Spec::Demonology:    return &demo;
            case Spec::Elemental:     return &ele;
            case Spec::Enhancement:   return &enh;
            case Spec::Restoration:   return &enh;
            case Spec::Balance:       return &balance;
            case Spec::Feral:         return &feral;
            case Spec::Shadow:        return &shadow;
            case Spec::Retribution:   return &ret;
            case Spec::BeastMastery:  return &bm;
            case Spec::Marksmanship:  return &marks;
            case Spec::Survival:      return &marks;
            default:                  return nullptr;
        }
    }

    // 该专精策略里点了名、但 bot 法术书里没有的技能（只记一次，排查"为什么某个技能没放"用）
    inline void LogMissingOnce(PlayerbotAI* ai, BotMem* m, Player* bot, PPolicy const* pol)
    {
        if (!pol || m->policyLogged)
            return;
        m->policyLogged = true;
        std::string missing;
        for (int i = 0; i < pol->count; ++i)
        {
            uint32 id = SpellId(ai, bot, m, pol->steps[i].name, false);
            if (!id || !bot->HasSpell(id))
            {
                missing += pol->steps[i].name;
                missing += ' ';
            }
        }
        sLog.outError("[DPSFSM-POLICY] %s class=%u spec=%s policy=%s 未学会: [%s]",
            bot->GetName(), uint32(bot->getClass()), SpecName(m->spec), pol->label, missing.c_str());
    }

    inline bool HasFormOrShift(Player* bot, char const* formName)
    {
        if (!formName)
            return true;
        uint32 const form = bot->GetShapeshiftForm();
        if (!strcmp(formName, "cat"))
            return form == FORM_CAT;
        return true;
    }

    // 一步策略能否执行（只看状态条件；能不能放交给 CastChecked 判）
    inline bool StepAllowed(PlayerbotAI* ai, BotMem* m, Snapshot& s, PStep const& st)
    {
        if (!st.name)
            return false;
        if (st.minCombo > 0 && s.combo < st.minCombo)
            return false;
        if (st.minEnergy > 0 && s.energy < st.minEnergy)
            return false;
        int minRage = st.minRage;
        if (st.rageParam)
            minRage = int(TP(m, st.rageParam, st.rageDefault));
        if (minRage > 0 && s.rage < minRage)
            return false;

        if (st.enableParam && TP(m, st.enableParam, st.enableDefault) < 0.5)
            return false;

        // [2026-10-30 实测] DoT/buff 类步骤靠"目标身上有没有这个 aura"判断，但 SpellId 取得的是
        //   "已学会的最高 rank"，和实际挂上的 aura id 可能对不上 ⇒ 实测戒律/战斗怒吼被反复重放
        //   （武器战 Rend 40 秒放了 14 次，GCD 全废）。加一道"同一手最少间隔 8 秒"的硬门。
        if (st.needAuraGone)
        {
            uint32 const gid = SpellId(ai, s.bot, m, st.name, false);
            auto const lo = m->lastOk.find(gid);
            if (gid && lo != m->lastOk.end() && time(nullptr) - lo->second < 8)
                return false;
        }
        if (st.minPack > 0 && s.pack10 < st.minPack && s.pack30 < st.minPack)
            return false;
        if (st.targetHpBelow < 100.0f && s.targetHp > st.targetHpBelow)
            return false;
        if (st.enterForm)
        {
            // 切形态这一手：已经在该形态就跳过
            if (strcmp(st.enterForm, "cat") == 0 && s.bot->GetShapeshiftForm() == FORM_CAT)
                return false;
            if (strcmp(st.enterForm, "berserker") == 0 && s.bot->GetShapeshiftForm() == FORM_BERSERKERSTANCE)
                return false;
        }
        if (st.needForm && !HasFormOrShift(s.bot, st.needForm))
            return false;
        if (st.needAuraGone)
        {
            // [2026-10-30 实测] 自身增益（战斗怒吼/圣印）的"已有就跳过"必须查**自己**，
            //   原来一律查目标 ⇒ 战斗怒吼 40 秒刷了 15 次、把 GCD 全占了（policy 223 dps vs 旧 510）。
            Unit* auraUnit = (st.kind == PKind::Self) ? s.bot : s.target;
            if (AuraStacks(ai, s.bot, m, st.needAuraGone, auraUnit) > 0)
                return false;
        }
        return true;
    }

    inline bool TryStep(PlayerbotAI* ai, BotMem* m, Snapshot& s, PStep const& st)
    {
        switch (st.kind)
        {
            case PKind::Self:
                return TryCastSelf(ai, m, s, st.name);
            case PKind::Ground:
                if (!s.target)
                    return false;
                return TryCastAt(ai, m, s, st.name,
                    s.target->GetPositionX(), s.target->GetPositionY(), s.target->GetPositionZ());
            case PKind::Swing:
                return TrySwing(ai, m, s, st.name);
            case PKind::Target:
            default:
                return TryCast(ai, m, s, st.name, nullptr);
        }
    }
}

    // [2026-10-30 站长口径] 策略执行入口：一次 tick 从上往下找**第一条能放的**策略步骤。
    //   返回 true = 这一 tick 状态机真的放出了技能（引擎这一 tick 不再插自己的施法）。
    //   返回 false = 没放出（没目标/都在冷却/资源不够/够不着）⇒ 交回引擎负责追击、走位、上马等。
    bool TickPolicy(PlayerbotAI* ai, BotMem* m)
    {
        Snapshot s;
        BuildSnapshot(ai, m, s);
        if (!s.target)
        {
            m->state = State::Idle;
            return false;
        }
        if (s.casting)              // 正在读条：不要打断自己
            return true;

        dpspolicy::PPolicy const* pol = dpspolicy::PolicyFor(s.bot->getClass(), m->spec);
        if (!pol)
            return false;

        dpspolicy::LogMissingOnce(ai, m, s.bot, pol);

        for (int i = 0; i < pol->count; ++i)
        {
            dpspolicy::PStep const& st = pol->steps[i];
            if (!dpspolicy::StepAllowed(ai, m, s, st))
                continue;
            if (dpspolicy::TryStep(ai, m, s, st))
            {
                m->state = (s.pack10 >= AoEPack(m)) ? State::Aoe : State::Burn;
                return true;
            }
        }
        return false;
    }
