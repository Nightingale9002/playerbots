#pragma once

// ---------------------------------------------------------------------------
// [跑位 2026-10-18 站长口径] 跑位是**队伍整体策略**：同一时刻每队只由**一个决策者**算阵型，
// 再把"占位槽"下发给每个机器人；bot 不各自判断该站哪。
//
// 决策层把跑位规则统一表达成**槽位约束**（角色 × 相对 BOSS 的距离带 × 相对 BOSS 朝向的角度），
// 一次算出每人坐标，再由 4 条**硬约束**统一后处理：
//   ① 不跑出 BOSS 战斗范围（leash 默认 40 码）且不贴脸（≥3 码）；
//   ② 槽位必须可达（有 navmesh 路径，否则不下发）—— 由 move to party slot 的寻路兜底；
//   ③ 不站不可站立点（水面/墙内，靠可达性 + z 兜底）；
//   ④ **不能引到未进入战斗的怪**：槽位到"还没参战的敌对怪"必须 ≥ 它的战斗半径 + 8 码，
//      不满足就在周围找安全点，找不到就不下发（宁可不动，也不引怪）。
//
// 模式（数据来自 ai_playerbot_boss_ability.category，优先级从高到低）：
//   los_all   卡视野（采样掩体点，全队非坦克进掩体）
//   frontal   避开怪物正面（非坦克绕到背后/侧面，坦克正面拉住让 BOSS 背对队伍）
//   knockback 击退/冲锋（最远者当沙包占最远点，其余横向错开）
//   stack     集合（分摊/治疗链/吃盾）
//   spread    按角度分散（每人均分方位，防链式 AOE）
// 后续：los_backline / los_pull / los_break / wall（贴墙防击飞）/ kite（风筝）。
//
// 用法：
//   - 决策者（队内 guid 最小的 bot）每 500ms 调 ComputeIfLeader()；
//   - 其他 bot 用 SlotFor(bot, slot) 读自己的目标点，交给 "move to party slot" 动作执行。
// ---------------------------------------------------------------------------

#include "playerbot/playerbot.h"
#include "playerbot/DecisionWeights.h"

#include <cmath>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "Grids/GridNotifiers.h"
#include "Grids/GridNotifiersImpl.h"
#include "Grids/CellImpl.h"
#include "playerbot/AiFactory.h"
#include "playerbot/strategy/AiObjectContext.h"
#include "playerbot/strategy/values/HazardsValue.h"
#include "Database/DatabaseEnv.h"

namespace partypos
{
    enum Role : uint8
    {
        ROLE_TANK = 0,
        ROLE_MELEE = 1,      // 近战输出（≤5 码）
        ROLE_MID = 2,        // 中距离输出（24-30 码：暗牧/奥法/术士/鸟德/元素萨等）
        ROLE_RANGED = 3,     // 远距离输出（36-40 码：猎人）
        ROLE_HEALER = 4,     // 治疗（36-40 码）
        ROLE_SANDBAG = 5,
        ROLE_NONE = 6,
    };

    enum Mode : uint8
    {
        MODE_NONE = 0,
        MODE_LOS_ALL = 1,
        MODE_FRONTAL = 2,
        MODE_KNOCKBACK = 3,
        MODE_STACK = 4,
        MODE_SPREAD = 5,
        MODE_WALL = 6,        // 贴墙防击飞
        MODE_LOS_PULL = 7,    // 卡视角拉怪（坦克把怪拉过来）
        MODE_LOS_BACKLINE = 8,// 只有远程/治疗卡视野
        MODE_LOS_BREAK = 9,   // 断读条卡视野
        MODE_ROLE_BANDS = 10, // 分带阵型（独立策略；未登记 BOSS 的兜底）（未登记 ai_playerbot_boss_ability 的 BOSS）
    };

    struct Slot
    {
        uint32 guid;
        uint8 role;
        float x, y, z;

        Slot() : guid(0), role(ROLE_NONE), x(0), y(0), z(0) {}
    };

    struct Plan
    {
        uint32 groupId;
        uint32 mapId;
        uint32 instanceId;
        uint32 bossEntry;
        uint8 mode;
        uint32 computedAt;
        std::vector<Slot> slots;

        Plan() : groupId(0), mapId(0), instanceId(0), bossEntry(0), mode(MODE_NONE), computedAt(0) {}
    };

    // 硬约束参数（后续可用配置/数据调）
    const float LEASH_DIST = 40.0f;       // 距 BOSS 最远（不脱战）
    const float MIN_BOSS_DIST = 3.0f;     // 距 BOSS 最近（不贴脸）
    const float HAZARD_MARGIN = 2.0f;     // 槽位/路径离地面 AOE 的余量（码）
    const float IDLE_MOB_MARGIN = 8.0f;   // 与"未参战怪"的额外安全余量

    std::mutex& PlanMutex();
    std::map<uint32, Plan>& Plans();                      // groupId -> Plan
    std::map<uint8, std::set<uint32>>& ModeEntries();     // mode -> 触发 entry
    void SetModeEntries(std::map<uint8, std::set<uint32>> const& entries);
    bool SlotFor(Player* bot, Slot& out);
    bool Compute(Player* leader);
    void ComputeIfLeader(Player* bot);

    // ===== 实现（header-only，与 DungeonRoute.h 同风格）=======================
    inline std::mutex& PlanMutex() { static std::mutex m; return m; }
    inline std::map<uint32, Plan>& Plans() { static std::map<uint32, Plan> p; return p; }
    inline std::map<uint8, std::set<uint32>>& ModeEntries() { static std::map<uint8, std::set<uint32>> e; return e; }

    inline void SetModeEntries(std::map<uint8, std::set<uint32>> const& entries)
    {
        std::scoped_lock lock(PlanMutex());
        ModeEntries() = entries;
    }

    inline char const* ModeName(uint8 mode)
    {
        switch (mode)
        {
            case MODE_LOS_ALL:   return "los_all";
            case MODE_FRONTAL:   return "frontal";
            case MODE_KNOCKBACK: return "knockback";
            case MODE_STACK:     return "stack";
            case MODE_SPREAD:    return "spread";
            case MODE_WALL:      return "wall";
            case MODE_LOS_PULL:  return "los_pull";
            case MODE_LOS_BACKLINE: return "los_backline";
            case MODE_LOS_BREAK: return "los_break";
            case MODE_ROLE_BANDS: return "role_bands";
            default:             return "none";
        }
    }

    inline float Dist2d(float x1, float y1, float x2, float y2)
    {
        float dx = x1 - x2, dy = y1 - y2;
        return sqrtf(dx * dx + dy * dy);
    }

    // ---- 场地几何（站位决策层直接用，不靠动作层猜）-------------------------------
    //   ① 同层地面：地形高度与 BOSS 高度差 ≤3 码（排除悬崖/二层/屋顶）；
    //   ② 不在实心几何里：同一高度四面 2 码短射线全被挡 ⇒ 判定在墙/模型内部，拒绝；
    //   ③ 墙边检测：某方向 3 码内被挡 ⇒ 该方向有墙（贴墙防击飞用）。
    inline bool TerrainOK(Map* map, float bz, float x, float y, float& outZ)
    {
        if (!map)
            return false;

        float ground = map->GetHeight(x, y, bz, false);
        if (fabsf(ground - bz) > 3.0f)
            return false;

        float zProbe = ground + 0.5f;
        int blocked = 0;
        static float const dirs[4][2] = { {1,0}, {-1,0}, {0,1}, {0,-1} };
        for (int i = 0; i < 4; ++i)
            if (!map->IsInLineOfSight(x, y, zProbe, x + dirs[i][0] * 2.0f, y + dirs[i][1] * 2.0f, zProbe, false))
                ++blocked;
        if (blocked >= 4)
            return false;

        outZ = ground;
        return true;
    }

    // 行走可达性近似：沿直线每 2 码采样，逐段检查"地形高度连续 + vmap 无遮挡"
    //   （全用服务端地形/vmap 数据；直线不可走 ⇒ 不把这个槽位下发，避免穿墙/跳崖）
    inline bool PathLooksWalkable(Map* map, float x1, float y1, float z1, float x2, float y2, float z2)
    {
        float dist = Dist2d(x1, y1, x2, y2);
        if (dist < 1.0f)
            return true;

        int steps = (int)(dist / 2.0f) + 1;
        if (steps > 24)
            steps = 24;

        float prevX = x1, prevY = y1, prevZ = z1;
        for (int i = 1; i <= steps; ++i)
        {
            float t = float(i) / float(steps);
            float x = x1 + (x2 - x1) * t;
            float y = y1 + (y2 - y1) * t;

            float ground = map->GetHeight(x, y, prevZ, false);
            if (fabsf(ground - prevZ) > 1.5f)
                return false;                                   // 台阶/悬崖

            if (!map->IsInLineOfSight(prevX, prevY, prevZ + 1.0f, x, y, ground + 1.0f, false))
                return false;                                   // 中间有墙/模型挡住

            prevX = x; prevY = y; prevZ = ground;
        }
        return true;
    }

    inline bool WallAt(Map* map, float x, float y, float z, float dx, float dy)
    {
        float zProbe = z + 0.5f;
        return !map->IsInLineOfSight(x, y, zProbe, x + dx * 3.0f, y + dy * 3.0f, zProbe, false);
    }

    // 贴墙点：BOSS 周围找"背后有墙、还能看见 BOSS"的点（防击飞）
    inline bool FindWallPoint(Map* map, Unit* boss, float radius, float& ox, float& oy, float& oz)
    {
        const float bx = boss->GetPositionX(), by = boss->GetPositionY(), bz = boss->GetPositionZ();
        for (int i = 0; i < 16; ++i)
        {
            float a = float(i) * 6.2831853f / 16.0f;
            float x = bx + radius * cosf(a);
            float y = by + radius * sinf(a);
            float z = 0.0f;
            if (!TerrainOK(map, bz, x, y, z))
                continue;
            if (!WallAt(map, x, y, z, cosf(a), sinf(a)))
                continue;
            if (!map->IsInLineOfSight(x, y, z + 0.5f, bx, by, bz + 0.5f, false))
                continue;
            ox = x; oy = y; oz = z;
            return true;
        }
        return false;
    }
    // ---- 禁站扇区（60° 一档 × 6）------------------------------------------------
    //   avoid_sectors: front/right/back/left（逗号分隔）；bit: front=1 right=2 back=4 left=8
    //   扇区索引 si：0=正前, ±1=侧前, ±2=侧后, ±3=正后（±60° 一档）
    inline std::map<uint32, uint8>& AvoidMasks() { static std::map<uint32, uint8> m; return m; }

    inline uint8 ParseAvoidMask(std::string const& text)
    {
        uint8 mask = 0;
        if (text.find("front") != std::string::npos) mask |= 1;
        if (text.find("right") != std::string::npos) mask |= 2;
        if (text.find("back") != std::string::npos) mask |= 4;
        if (text.find("left") != std::string::npos) mask |= 8;
        return mask;
    }

    inline void SetAvoidMasks(std::map<uint32, uint8> const& masks)
    {
        std::scoped_lock lock(PlanMutex());
        AvoidMasks() = masks;
    }

    inline uint8 AvoidMaskFor(uint32 entry)
    {
        std::scoped_lock lock(PlanMutex());
        auto it = AvoidMasks().find(entry);
        return it == AvoidMasks().end() ? 0 : it->second;
    }

    // ---- 朝向规则（2026-10-18 站长口径）-----------------------------------------
    //   away_group 默认：背对队伍；fixed：固定世界角度；open：面向最空旷方向；wall：面向最近的墙。
    //   数据列：ai_playerbot_boss_ability.facing_mode / facing_deg
    enum FacingMode : uint8
    {
        FACING_AWAY_GROUP = 0,
        FACING_FIXED = 1,
        FACING_OPEN = 2,
        FACING_WALL = 3,
    };

    struct FacingRule
    {
        uint8 mode;
        float deg;
        FacingRule() : mode(FACING_AWAY_GROUP), deg(0) {}
    };

    inline std::map<uint32, FacingRule>& FacingRules() { static std::map<uint32, FacingRule> f; return f; }
    inline void SetFacingRules(std::map<uint32, FacingRule> const& rules)
    {
        std::scoped_lock lock(PlanMutex());
        FacingRules() = rules;
    }
    inline FacingRule FacingRuleFor(uint32 entry)
    {
        std::scoped_lock lock(PlanMutex());
        auto it = FacingRules().find(entry);
        return it == FacingRules().end() ? FacingRule() : it->second;
    }
    inline char const* FacingName(uint8 mode)
    {
        switch (mode)
        {
            case FACING_FIXED: return "fixed";
            case FACING_OPEN:  return "open";
            case FACING_WALL:  return "wall";
            default:           return "away_group";
        }
    }

    // 从 BOSS 往 dir 方向能走多远（最远 20 码；地形/遮挡都算）——"空旷度"
    inline float Openness(Map* map, Unit* boss, float dx, float dy)
    {
        float bx = boss->GetPositionX(), by = boss->GetPositionY(), bz = boss->GetPositionZ();
        float best = 0.0f;
        for (float step = 2.0f; step <= 20.0f; step += 2.0f)
        {
            float x = bx + dx * step, y = by + dy * step;
            float z = 0.0f;
            if (!TerrainOK(map, bz, x, y, z))
                break;
            if (!map->IsInLineOfSight(bx, by, bz + 1.0f, x, y, z + 1.0f, false))
                break;
            best = step;
        }
        return best;
    }

    // 计算"BOSS 应该面向的方向"（单位向量）。awayFallback = 背对队伍的基准方向。
    inline void ComputeFaceDir(Map* map, Unit* boss, float baseDX, float baseDY,
                               float& outX, float& outY, uint8& usedMode)
    {
        FacingRule rule = FacingRuleFor(boss->GetEntry());
        usedMode = rule.mode;

        if (rule.mode == FACING_FIXED)
        {
            float rad = rule.deg * 3.14159265f / 180.0f;
            outX = cosf(rad); outY = sinf(rad);
            return;
        }

        if (rule.mode == FACING_OPEN || rule.mode == FACING_WALL)
        {
            float bestX = 0, bestY = 0, bestScore = -1e9f;
            for (int i = 0; i < 16; ++i)
            {
                float a = float(i) * 6.2831853f / 16.0f;
                float dx = cosf(a), dy = sinf(a);
                float openness = Openness(map, boss, dx, dy);
                // open：越空旷越好；wall：越早撞墙越好（但至少 3 码，别把怪顶进模型）
                float score = (rule.mode == FACING_OPEN) ? openness : (openness >= 2.0f && openness <= 10.0f ? 20.0f - openness : -1.0f);
                if (score <= -0.5f) continue;
                // 同分时偏向原基准方向（背对队伍），避免转向后把队伍包住
                score += 0.5f * (dx * baseDX + dy * baseDY);
                if (score > bestScore) { bestScore = score; bestX = dx; bestY = dy; }
            }
            if (bestScore > -0.5f) { outX = bestX; outY = bestY; return; }
            usedMode = FACING_AWAY_GROUP;
        }

        outX = baseDX; outY = baseDY;
    }

    inline uint8 SectorZone(int si)
    {
        if (si == 0) return 1;                 // front
        if (si >= 3 || si <= -3) return 4;     // back
        return si > 0 ? 2 : 8;                 // right / left
    }

    // [硬约束⑤] 非坦克不许站进"禁站扇区"：按 ±60° 逐档跳到最近的允许扇区（半径不变）
    inline void EnforceSectors(float bx, float by, float facing, uint8 mask, float& x, float& y)
    {
        if (!mask)
            return;

        const float PI = 3.14159265f;
        float radius = Dist2d(x, y, bx, by);
        if (radius < 0.1f)
            return;

        float rel = atan2f(y - by, x - bx) - facing;
        while (rel > PI) rel -= 2.0f * PI;
        while (rel < -PI) rel += 2.0f * PI;

        int si = (int)lroundf(rel / (PI / 3.0f));
        if (si > 3) si = 3;
        if (si < -3) si = -3;

        int dir = (rel >= 0.0f) ? 1 : -1;
        for (int step = 0; step < 6 && (mask & SectorZone(si)); ++step)
        {
            si += dir;
            if (si > 3) { si = 3; dir = -1; }
            if (si < -3) { si = -3; dir = 1; }
        }
        if (mask & SectorZone(si))
            return;   // 理论上不会（最多禁 front+back 或 right+left）

        float angle = facing + float(si) * (PI / 3.0f);
        x = bx + radius * cosf(angle);
        y = by + radius * sinf(angle);
    }

    // 从 world 库加载"按模式分类的触发 entry"，每 300s 刷新；表不存在则静默
    inline float RoleBand(uint8 role)
    {
        switch (role)
        {
            case ROLE_MELEE:   return dw::Get("position.band.melee");
            case ROLE_MID:     return dw::Get("position.band.mid_range");
            case ROLE_RANGED:  return dw::Get("position.band.ranged");
            case ROLE_HEALER:  return dw::Get("position.band.healer");
            case ROLE_SANDBAG: return 32.0f;
            default:           return dw::Get("position.band.ranged");
        }
    }

    // ---- 有近战 AOE 的 BOSS（数据：ai_playerbot_boss_ability.category='melee_aoe'）----
    inline std::set<uint32>& MeleeAoeBosses() { static std::set<uint32> s; return s; }

    inline void SetMeleeAoeBosses(std::set<uint32> const& s)
    {
        std::scoped_lock lock(PlanMutex());
        MeleeAoeBosses() = s;
    }

    inline bool IsMeleeAoeBoss(uint32 entry)
    {
        std::scoped_lock lock(PlanMutex());
        return MeleeAoeBosses().count(entry) != 0;
    }

    // 远程/治疗的下限（默认 12；近战 AOE 的 BOSS 抬高）
    inline float RoleMinDistance(uint8 role, uint32 bossEntry)
    {
        if (role != ROLE_MID && role != ROLE_RANGED && role != ROLE_HEALER)
            return 0.0f;
        float lower = dw::Get("position.band.min_range");
        if (IsMeleeAoeBoss(bossEntry))
            lower = std::max(lower, dw::Get("position.melee_aoe_min"));
        return lower;
    }

    // 把 (x,y) 夹进 [下限, 上限] 环带（只对中距/远程/治疗；近战/坦克不动）
    inline void FitRoleBand(uint8 role, uint32 bossEntry, float bx, float by, float& x, float& y)
    {
        if (role != ROLE_MID && role != ROLE_RANGED && role != ROLE_HEALER)
            return;
        float upper = RoleBand(role);
        float lower = RoleMinDistance(role, bossEntry);
        if (upper < lower)
            upper = lower;

        float dx = x - bx, dy = y - by;
        float d = sqrtf(dx * dx + dy * dy);
        if (d < 1e-3f) { dx = 1.0f; dy = 0.0f; d = 1.0f; }
        float want = d;
        if (want > upper) want = upper;
        if (want < lower) want = lower;
        if (fabsf(want - d) > 0.01f)
        {
            x = bx + dx / d * want;
            y = by + dy / d * want;
        }
    }

    inline void EnsureModeEntries()
    {
        static std::mutex m;
        static time_t loadedAt = 0;
        static bool tried = false;

        std::scoped_lock lock(m);
        const time_t now = time(nullptr);
        if (tried && now - loadedAt < 300)
            return;
        tried = true;
        loadedAt = now;

        std::unique_ptr<QueryResult> table = WorldDatabase.Query(
            "SELECT 1 FROM information_schema.tables WHERE table_schema = DATABASE() "
            "AND table_name = 'ai_playerbot_boss_ability'");
        if (!table)
            return;

        std::map<uint8, std::set<uint32>> entries;
        std::unique_ptr<QueryResult> res = WorldDatabase.Query(
            "SELECT DISTINCT `category`, `boss_entry`, `owner_entry` FROM `ai_playerbot_boss_ability` "
            "WHERE `category` IN ('los','frontal','knockback','stack','spread','wall','los_pull','los_backline','los_break')");
        if (res)
        {
            do
            {
                Field* fields = res->Fetch();
                std::string category = fields[0].GetString();
                uint8 mode = MODE_NONE;
                if (category == "los") mode = MODE_LOS_ALL;
                else if (category == "frontal") mode = MODE_FRONTAL;
                else if (category == "knockback") mode = MODE_KNOCKBACK;
                else if (category == "stack") mode = MODE_STACK;
                else if (category == "spread") mode = MODE_SPREAD;
                else if (category == "wall") mode = MODE_WALL;
                else if (category == "los_pull") mode = MODE_LOS_PULL;
                else if (category == "los_backline") mode = MODE_LOS_BACKLINE;
                else if (category == "los_break") mode = MODE_LOS_BREAK;
                else if (category == "role_bands" || category == "default") mode = MODE_ROLE_BANDS;
                if (!mode)
                    continue;
                if (uint32 boss = fields[1].GetUInt32())
                    entries[mode].insert(boss);
                if (uint32 owner = fields[2].GetUInt32())
                    entries[mode].insert(owner);
            } while (res->NextRow());
        }

        std::map<uint32, uint8> masks;
        std::unique_ptr<QueryResult> mres = WorldDatabase.Query(
            "SELECT DISTINCT `boss_entry`,`owner_entry`,`avoid_sectors` FROM `ai_playerbot_boss_ability` "
            "WHERE `avoid_sectors` <> ''");
        if (mres)
        {
            do
            {
                Field* fields = mres->Fetch();
                uint8 mask = ParseAvoidMask(fields[2].GetString());
                if (!mask)
                    continue;
                if (uint32 boss = fields[0].GetUInt32())
                    masks[boss] = mask;
                if (uint32 owner = fields[1].GetUInt32())
                    masks[owner] = mask;
            } while (mres->NextRow());
        }
        SetAvoidMasks(masks);

        std::map<uint32, FacingRule> facingRules;
        std::unique_ptr<QueryResult> fres = WorldDatabase.Query(
            "SELECT DISTINCT `boss_entry`,`owner_entry`,`facing_mode`,`facing_deg` FROM `ai_playerbot_boss_ability` "
            "WHERE `facing_mode` <> 'away_group'");
        if (fres)
        {
            do
            {
                Field* fields = fres->Fetch();
                FacingRule rule;
                std::string mode = fields[2].GetString();
                if (mode == "fixed") rule.mode = FACING_FIXED;
                else if (mode == "open") rule.mode = FACING_OPEN;
                else if (mode == "wall") rule.mode = FACING_WALL;
                else continue;
                rule.deg = float(fields[3].GetUInt32());
                if (uint32 boss = fields[0].GetUInt32()) facingRules[boss] = rule;
                if (uint32 owner = fields[1].GetUInt32()) facingRules[owner] = rule;
            } while (fres->NextRow());
        }
        SetFacingRules(facingRules);

        std::set<uint32> meleeAoe;
        std::unique_ptr<QueryResult> ares = WorldDatabase.Query(
            "SELECT DISTINCT `boss_entry`,`owner_entry` FROM `ai_playerbot_boss_ability` WHERE `category` = 'melee_aoe'");
        if (ares)
        {
            do
            {
                Field* fields = ares->Fetch();
                if (uint32 boss = fields[0].GetUInt32()) meleeAoe.insert(boss);
                if (uint32 owner = fields[1].GetUInt32()) meleeAoe.insert(owner);
            } while (ares->NextRow());
        }
        SetMeleeAoeBosses(meleeAoe);

        SetModeEntries(entries);
        uint32 count = 0;
        for (auto const& kv : entries)
            count += (uint32)kv.second.size();
        sLog.outString("[POSITION] 跑位触发集合: %u 个 entry（%u 种模式）", count, (uint32)entries.size());
    }


    // 硬约束④：与"还没参战的敌对怪"保持距离
    inline bool SafeFromIdleHostiles(Player* leader, float x, float y, std::list<Unit*> const& idle)
    {
        for (Unit* unit : idle)
        {
            if (!unit || !unit->IsInWorld() || !unit->IsAlive() || unit->IsInCombat())
                continue;
            float safe = unit->GetCombatReach() + IDLE_MOB_MARGIN;
            if (Dist2d(x, y, unit->GetPositionX(), unit->GetPositionY()) < safe)
                return false;
        }
        return true;
    }

    // 附近敌对单位扫描（只取"未参战"的用于安全距离；参战的由触发链路处理）
    // ---- 硬约束：当前地面 AOE（hazards）不能站、不能穿 -----------------------
    inline bool SafeFromHazards(Player* bot, float x, float y)
    {
        if (!bot)
            return true;
        PlayerbotAI* ai = bot->GetPlayerbotAI();
        if (!ai)
            return true;
        Value<std::list<HazardPosition>>* value =
            ai->GetAiObjectContext()->GetValue<std::list<HazardPosition>>("hazards");
        if (!value)
            return true;
        for (HazardPosition const& hp : value->Get())
        {
            if (hp.first.mapid != bot->GetMapId())
                continue;
            if (Dist2d(x, y, hp.first.coord_x, hp.first.coord_y) < hp.second + HAZARD_MARGIN)
                return false;
        }
        return true;
    }

    // 槽位被 hazard 覆盖时就近找安全点（3/5/8 码，8 方向）
    inline bool AdjustForHazards(Player* bot, float& x, float& y)
    {
        if (SafeFromHazards(bot, x, y))
            return true;

        static float const radius[] = { 3.0f, 5.0f, 8.0f };
        for (float r : radius)
        {
            for (int i = 0; i < 8; ++i)
            {
                float a = float(i) * 3.14159265f / 4.0f;
                float nx = x + r * cosf(a);
                float ny = y + r * sinf(a);
                if (SafeFromHazards(bot, nx, ny))
                {
                    x = nx;
                    y = ny;
                    return true;
                }
            }
        }
        return false;
    }

    // 路径 hazard 校验（给 MoveToPartySlotAction 用）
    inline bool PathSafeFromHazards(Player* bot, float x1, float y1, float z1,
                                    float x2, float y2, float z2)
    {
        if (!SafeFromHazards(bot, x1, y1) || !SafeFromHazards(bot, x2, y2))
            return false;
        const int steps = 8;
        for (int i = 1; i < steps; ++i)
        {
            float f = float(i) / float(steps);
            float x = x1 + (x2 - x1) * f;
            float y = y1 + (y2 - y1) * f;
            if (!SafeFromHazards(bot, x, y))
                return false;
        }
        return true;
    }

    inline std::list<Unit*> ScanHostiles(Player* leader, float radius)
    {
        std::list<Unit*> units;
        MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck check(leader, radius);
        MaNGOS::UnitListSearcher<MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck> searcher(units, check);
        Cell::VisitAllObjects((const WorldObject*)leader, searcher, radius);
        return units;
    }

    // 硬约束①：clamp 到 leash 内、不贴脸
    inline void ClampToLeash(float bx, float by, float& x, float& y)
    {
        float d = Dist2d(x, y, bx, by);
        if (d < 0.01f)
            return;
        float want = d;
        if (want > LEASH_DIST) want = LEASH_DIST;
        if (want < MIN_BOSS_DIST) want = MIN_BOSS_DIST;
        if (want != d)
        {
            float k = want / d;
            x = bx + (x - bx) * k;
            y = by + (y - by) * k;
        }
    }

    // 硬约束④：目标点不安全时，在周围试 8 个方向找安全点；找不到返回 false
    inline bool AdjustForIdleHostiles(Player* leader, std::list<Unit*> const& idle, float& x, float& y)
    {
        if (SafeFromIdleHostiles(leader, x, y, idle))
            return true;

        static float const radius[] = { 3.0f, 5.0f, 8.0f };
        for (float r : radius)
        {
            for (int i = 0; i < 8; ++i)
            {
                float a = float(i) * 3.14159265f / 4.0f;
                float nx = x + r * cosf(a);
                float ny = y + r * sinf(a);
                if (SafeFromIdleHostiles(leader, nx, ny, idle))
                {
                    x = nx;
                    y = ny;
                    return true;
                }
            }
        }
        return false;
    }

    // [跑位分带 2026-10-19 站长口径 v2]
    //   中距离 = 只有暗牧 / 奥法（上限 20 码）
    //   远程   = 其余远程输出 + 治疗（上限 30 码）
    //   下限统一 12 码；有近战 AOE 的 BOSS 再抬高最近距离
    inline uint8 RoleOf(Player* member, Unit* boss)
    {
        if (!member || !boss)
            return ROLE_NONE;

        if (PlayerbotAI::IsHeal(member))
            return ROLE_HEALER;
        if (PlayerbotAI::IsTank(member))
            return ROLE_MELEE;

        int spec = AiFactory::GetPlayerSpecTab(member);
        switch (member->getClass())
        {
            case CLASS_MAGE:
                return spec == 0 ? ROLE_MID : ROLE_RANGED;   // 奥术=中距，火焰/冰霜=远程
            case CLASS_PRIEST:
                return spec == 2 ? ROLE_MID : ROLE_RANGED;   // 暗影=中距，戒律/神圣=远程
            case CLASS_HUNTER:
            case CLASS_WARLOCK:
                return ROLE_RANGED;
            case CLASS_DRUID:
                return spec == 1 ? ROLE_MELEE : ROLE_RANGED; // 野性=近战，平衡=远程
            case CLASS_SHAMAN:
                return spec == 1 ? ROLE_MELEE : ROLE_RANGED; // 增强=近战，元素=远程
            case CLASS_PALADIN:
                return spec == 2 ? ROLE_MELEE : ROLE_RANGED; // 惩戒=近战
            case CLASS_WARRIOR:
            case CLASS_ROGUE:
            default:
                return ROLE_MELEE;
        }
    }


    inline bool SlotFor(Player* bot, Slot& out)
    {
        if (!bot)
            return false;
        Group* group = bot->GetGroup();
        if (!group)
            return false;

        std::scoped_lock lock(PlanMutex());
        auto it = Plans().find(group->GetId());
        if (it == Plans().end())
            return false;

        // 阵型过期（决策者没在刷新）⇒ 不再按旧点走，避免"追着已经作废的槽位跑"
        if (it->second.computedAt &&
            WorldTimer::getMSTimeDiff(it->second.computedAt, WorldTimer::getMSTime()) > 3000)
            return false;

        for (std::vector<Slot>::const_iterator i = it->second.slots.begin(); i != it->second.slots.end(); ++i)
            if (i->guid == bot->GetGUIDLow()) { out = *i; return true; }
        return false;
    }

    // ---- 未登记阵型数据的 BOSS 兜底（2026-10-19 站长口径）------------------
    //   数据源：tbccharacters.ai_playerbot_boss_strategy（登记表，5 分钟重扫）
    //   判定：登记表 / ScriptName "boss_" / 世界 BOSS（rank>=3）⇒ 进默认阵型
    inline std::set<uint32>& RegisteredBosses() { static std::set<uint32> s; return s; }

    inline void EnsureRegisteredBosses()
    {
        static bool tried = false;
        static time_t loadedAt = 0;
        time_t now = time(0);
        if (tried && now - loadedAt < 300)
            return;
        tried = true;
        loadedAt = now;

        std::set<uint32> set;
        std::unique_ptr<QueryResult> res = CharacterDatabase.Query("SELECT entry FROM ai_playerbot_boss_strategy");
        if (res)
        {
            do
            {
                set.insert(res->Fetch()[0].GetUInt32());
            } while (res->NextRow());
        }

        std::scoped_lock lock(PlanMutex());
        RegisteredBosses() = set;
    }

    inline bool IsBossLike(Unit* unit)
    {
        Creature* creature = dynamic_cast<Creature*>(unit);
        if (!creature)
            return false;
        CreatureInfo const* info = creature->GetCreatureInfo();
        if (!info)
            return false;

        {
            std::scoped_lock lock(PlanMutex());
            if (RegisteredBosses().count(creature->GetEntry()))
                return true;
        }

        std::string script = creature->GetScriptName();
        if (script.size() > 5 && script.compare(0, 5, "boss_") == 0)
            return true;

        return info->Rank >= 3;      // CREATURE_ELITE_WORLDBOSS
    }

    inline bool Compute(Player* leader)
    {
        if (!leader || !leader->IsInWorld())
            return false;
        Group* group = leader->GetGroup();
        Map* map = leader->GetMap();
        if (!group || !map)
            return false;

        std::map<uint8, std::set<uint32>> const& modeEntries = ModeEntries();
        EnsureRegisteredBosses();

        // 1) 危险怪：队里任何人正在被它打，且 entry 命中某模式（按优先级 los > frontal > knockback > stack > spread）
        Unit* danger = nullptr;
        uint8 mode = MODE_NONE;
        for (GroupReference* ref = group->GetFirstMember(); ref && !danger; ref = ref->next())
        {
            Player* member = ref->getSource();
            if (!member || !member->IsInWorld() || !member->IsAlive())
                continue;
            for (Unit* attacker : member->getAttackers())
            {
                if (!attacker || !attacker->IsAlive() || attacker->GetTypeId() != TYPEID_UNIT)
                    continue;
                // [决策参数化 2026-10-18] 模式选择也走权重表（position.mode.*）
                static uint8 const candidates[] = { MODE_LOS_ALL, MODE_LOS_PULL, MODE_LOS_BACKLINE, MODE_LOS_BREAK,
                                                    MODE_FRONTAL, MODE_KNOCKBACK, MODE_STACK, MODE_SPREAD, MODE_WALL,
                                                    MODE_ROLE_BANDS };
                float bestModeScore = -1e18f;
                for (uint8 want : candidates)
                {
                    auto it = modeEntries.find(want);
                    if (it == modeEntries.end() || !it->second.count(attacker->GetEntry()))
                        continue;

                    float score = dw::Get(std::string("position.mode.") + ModeName(want));
                    if (score > bestModeScore)
                    {
                        bestModeScore = score;
                        danger = attacker;
                        mode = want;
                    }
                }
                if (!danger && IsBossLike(attacker))    // 未登记阵型的 BOSS ⇒ 默认阵型
                {
                    danger = attacker;
                    mode = MODE_ROLE_BANDS;
                }
                if (danger)
                    break;
            }
        }

        uint32 groupId = group->GetId();
        if (!danger)
        {
            std::scoped_lock lock(PlanMutex());
            Plans().erase(groupId);
            return false;
        }

        const float bx = danger->GetPositionX();
        const float by = danger->GetPositionY();
        const float bz = danger->GetPositionZ();
        const float facing = danger->GetOrientation();
        const float fx = cosf(facing), fy = sinf(facing);   // BOSS 正面方向

        std::list<Unit*> hostiles = ScanHostiles(leader, 40.0f);

        // 硬约束⑤的数据：这只怪禁站哪些扇区（攻略里"正面锥形 / 甩尾"等）
        const uint8 avoidMask = AvoidMaskFor(danger->GetEntry());

        // 2) 成员 / 坦克
        Player* tank = nullptr;
        std::vector<Player*> members;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->getSource();
            if (!member || !member->IsInWorld() || !member->IsAlive())
                continue;
            members.push_back(member);
            if (!tank && (member->GetVictim() == danger || member->getAttackers().count(danger)))
                tank = member;
        }
        if (members.size() < 2)
            return false;

        Plan plan;
        plan.groupId = groupId;
        plan.mapId = map->GetId();
        plan.instanceId = map->GetInstanceId();
        plan.bossEntry = danger->GetEntry();
        plan.mode = mode;
        plan.computedAt = WorldTimer::getMSTime();

        // [调整朝向 2026-10-18 站长口径] 坦克站到"队伍质心的反方向"一侧：
        //   BOSS 面对坦克 ⇒ 自然转身、把正面/吐息背对队伍（近战可安全打背）。
        float gx = 0.0f, gy = 0.0f;
        int gn = 0;
        for (Player* member : members)
        {
            if (!member) continue;
            gx += member->GetPositionX(); gy += member->GetPositionY(); ++gn;
        }
        if (gn) { gx /= float(gn); gy /= float(gn); }

        // 基准方向 = 背对队伍（away_group 的默认行为）
        float baseX = bx - gx, baseY = by - gy;
        float baseLen = sqrtf(baseX * baseX + baseY * baseY);
        if (baseLen < 1.0f) { baseX = fx; baseY = fy; baseLen = 1.0f; }
        baseX /= baseLen; baseY /= baseLen;

        // [朝向 2026-10-18] 按数据规则算"BOSS 应面向的方向"（away_group/fixed/open/wall）
        float awayX = baseX, awayY = baseY;
        uint8 facingMode = FACING_AWAY_GROUP;
        ComputeFaceDir(map, danger, baseX, baseY, awayX, awayY, facingMode);

        const float behindX = bx - awayX * 8.0f;
        const float behindY = by - fy * 8.0f;
        int nonTank = 0;
        for (Player* member : members)
            if (member != tank)
                ++nonTank;

        // 卡视野：采样掩体点（对危险怪不可见 + 不引怪）
        float coverX = 0, coverY = 0, coverZ = 0;
        bool hasCover = false;
        bool const losMode = (mode == MODE_LOS_ALL || mode == MODE_LOS_PULL ||
                              mode == MODE_LOS_BACKLINE || mode == MODE_LOS_BREAK);
        if (losMode)
        {
            float bestScore = -1e18f;
            for (int ring = 0; ring < 2 && !hasCover; ++ring)
            {
                float radius = (ring == 0) ? 8.0f : 14.0f;
                for (int i = 0; i < 8; ++i)
                {
                    float angle = float(i) * 3.14159265f / 4.0f;
                    float x = leader->GetPositionX() + radius * cosf(angle);
                    float y = leader->GetPositionY() + radius * sinf(angle);
                    if (map->IsInLineOfSight(x, y, leader->GetPositionZ(), bx, by, bz, false))
                        continue;
                    if (!SafeFromIdleHostiles(leader, x, y, hostiles))
                        continue;
                    float d = Dist2d(x, y, leader->GetPositionX(), leader->GetPositionY());
                    float score = dw::Get("position.cover_distance") * d;
                    if (!hasCover || score > bestScore)
                    {
                        bestScore = score;
                        coverX = x; coverY = y; coverZ = leader->GetPositionZ();
                        hasCover = true;
                    }
                }
            }
        }

        // 贴墙防击飞：先找好背墙点（找不到就退回普通站位）
        float wallX = 0, wallY = 0, wallZ = 0;
        bool hasWallPoint = false;
        if (mode == MODE_WALL)
            hasWallPoint = FindWallPoint(map, danger, 26.0f, wallX, wallY, wallZ);

        int spreadIndex = 0;
        for (Player* member : members)
        {
            Slot slot;
            slot.guid = member->GetGUIDLow();
            uint8 role = (member == tank) ? ROLE_TANK : RoleOf(member, danger);
            slot.role = role;

            float x = member->GetPositionX(), y = member->GetPositionY(), z = member->GetPositionZ();

            if (member == tank && mode != MODE_LOS_PULL)
            {
                // 坦克：站"远离队伍"那一侧（BOSS 转身面对坦克 ⇒ 背对队伍）
                x = bx + awayX * 3.2f;
                y = by + awayY * 3.2f;
                z = bz;
            }
            else if (losMode && hasCover)
            {
                bool goCover = true;
                if (mode == MODE_LOS_PULL)
                    goCover = (member == tank);
                else if (mode == MODE_LOS_BACKLINE)
                    goCover = (role == ROLE_RANGED || role == ROLE_HEALER);

                if (!goCover)
                {
                    float band = RoleBand(role);
                    x = bx - fx * band;
                    y = by - fy * band;
                    z = bz;
                }
                else
                {
                    float a = float(spreadIndex++) * 2.0f;
                    x = coverX + 1.5f * cosf(a);
                    y = coverY + 1.5f * sinf(a);
                    z = coverZ;
                }
            }
            else if (mode == MODE_FRONTAL)
            {
                // 非坦克：绕到背后（与正面夹角 ≥ 120°）
                float band = RoleBand(role);
                float lateral = float(spreadIndex++ - (nonTank - 1) / 2.0f) * 2.5f;
                x = bx - fx * band - fy * lateral;
                y = by - fy * band + fx * lateral;
                z = bz;
            }
            else if (mode == MODE_KNOCKBACK)
            {
                bool sandbag = (spreadIndex == 0);
                float band = sandbag ? 32.0f : RoleBand(role);
                float lateral = sandbag ? 0.0f : float(spreadIndex) * 3.0f;
                x = bx - fx * band - fy * lateral;
                y = by - fy * band + fx * lateral;
                z = bz;
                if (sandbag)
                    slot.role = ROLE_SANDBAG;
                ++spreadIndex;
            }
            else if (mode == MODE_STACK)
            {
                float a = float(spreadIndex++) * 2.0f;
                x = behindX + 1.5f * cosf(a);
                y = behindY + 1.5f * sinf(a);
                z = bz;
            }
            else if (mode == MODE_WALL)
            {
                if (hasWallPoint)
                {
                    float a = float(spreadIndex++) * 1.0f;
                    x = wallX + 1.0f * cosf(a);
                    y = wallY + 1.0f * sinf(a);
                    z = wallZ;
                }
            }
            else if (mode == MODE_SPREAD)
            {
                // 按角度均分：从背后开始，每 360/nonTank 度一个方位
                float step = 6.2831853f / float(nonTank > 0 ? nonTank : 1);
                float a = 3.14159265f + step * float(spreadIndex++);
                float band = RoleBand(role);
                x = bx + band * cosf(a);
                y = by + band * sinf(a);
                z = bz;
            }
            else if (mode == MODE_ROLE_BANDS)
            {
                // 默认阵型：非坦克在 BOSS 背后按角色分带（近战 4.5 / 中距离 27 / 远程+治疗 38），横向错开
                float band = RoleBand(role);
                float lateral = float(spreadIndex++ - (nonTank - 1) / 2.0f) * 2.2f;
                x = bx - fx * band - fy * lateral;
                y = by - fy * band + fx * lateral;
                z = bz;
            }
            else
            {
                float band = RoleBand(role);
                x = bx - fx * band;
                y = by - fy * band;
                z = bz;
            }

            // 硬约束⑤：非坦克不许站禁站扇区（正前/正后/侧面，60° 一档）；坦克在正面拉住 BOSS，豁免
            if (member != tank)
                EnforceSectors(bx, by, facing, avoidMask, x, y);

            // [跑位 2026-10-19] 通用分带阵型才夹距离带（stack/knockback 等特殊队形保持原样）
            if (mode == MODE_SPREAD || mode == MODE_ROLE_BANDS)
                FitRoleBand(role, danger->GetEntry(), bx, by, x, y);

            ClampToLeash(bx, by, x, y);                                     // 硬约束①（不跑出战斗范围）

            if (!AdjustForIdleHostiles(leader, hostiles, x, y))             // 硬约束④（不引到未参战怪）
                continue;                                                   // 找不到安全点 ⇒ 这格不下发
            if (!AdjustForHazards(member, x, y))                            // 硬约束：不站在地面 AOE 里
                continue;

            if (mode == MODE_SPREAD || mode == MODE_ROLE_BANDS)             // 安全点挪动后仍保持距离带
                FitRoleBand(role, danger->GetEntry(), bx, by, x, y);
            if (!SafeFromIdleHostiles(leader, x, y, hostiles) || !SafeFromHazards(member, x, y))
                continue;                                                   // 夹带后不满足安全 ⇒ 这格不下发

            // 硬约束③：场地几何 —— 同层 + 不在实体里 + 直线可走（全用服务端 terrain/vmap 数据）
            float groundZ = z;
            if (!TerrainOK(map, bz, x, y, groundZ))
                continue;
            z = groundZ;
            if (!PathLooksWalkable(map, member->GetPositionX(), member->GetPositionY(),
                                   member->GetPositionZ(), x, y, z))
                continue;

            slot.x = x;
            slot.y = y;
            slot.z = z;
            plan.slots.push_back(slot);
        }

        // 硬约束⑥：远程/治疗两两最小间距（防链式 AOE），2 轮互推；推完重新校验，非法则恢复
        std::vector<Slot> const beforeSpacing = plan.slots;
        for (int iter = 0; iter < 2; ++iter)
        {
            for (size_t i = 0; i < plan.slots.size(); ++i)
            {
                Slot& a = plan.slots[i];
                if (a.role != ROLE_RANGED && a.role != ROLE_HEALER)
                    continue;
                for (size_t j = i + 1; j < plan.slots.size(); ++j)
                {
                    Slot& b = plan.slots[j];
                    if (b.role != ROLE_RANGED && b.role != ROLE_HEALER)
                        continue;
                    float dx = b.x - a.x, dy = b.y - a.y;
                    float d = sqrtf(dx * dx + dy * dy);
                    if (d >= 5.0f)
                        continue;
                    if (d < 0.1f)
                    {
                        float ang = float(i + j) * 2.0f;
                        dx = cosf(ang); dy = sinf(ang); d = 1.0f;
                    }
                    float push = (5.0f - d) * 0.5f;
                    dx /= d; dy /= d;
                    a.x -= dx * push; a.y -= dy * push;
                    b.x += dx * push; b.y += dy * push;
                }
            }
        }

        for (size_t i = 0; i < plan.slots.size(); ++i)
        {
            Slot& s = plan.slots[i];
            if (s.role != ROLE_RANGED && s.role != ROLE_HEALER)
                continue;
            float gz = s.z;
            if (TerrainOK(map, bz, s.x, s.y, gz) && SafeFromIdleHostiles(leader, s.x, s.y, hostiles))
                s.z = gz;
            else
                plan.slots[i] = beforeSpacing[i];        // 推坏了就恢复原槽位
        }

        if (plan.slots.empty())
            return false;

        {
            std::scoped_lock lock(PlanMutex());
            Plans()[groupId] = plan;
        }
        sLog.outString("[POSITION] group=%u plan=%s facing=%s danger=%u slots=%u",
            groupId, ModeName(mode), FacingName(facingMode), plan.bossEntry, uint32(plan.slots.size()));
        return true;
    }

    inline void ComputeIfLeader(Player* bot)
    {
        if (!bot || !bot->IsInWorld())
            return;
        Group* group = bot->GetGroup();
        if (!group)
            return;

        // 队内最小 guid 的 bot = 决策者（真人玩家不当决策者）
        uint32 leaderGuid = 0;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->getSource();
            if (!member || !member->IsInWorld() || !member->GetPlayerbotAI())
                continue;
            if (!leaderGuid || member->GetGUIDLow() < leaderGuid)
                leaderGuid = member->GetGUIDLow();
        }
        if (!leaderGuid || bot->GetGUIDLow() != leaderGuid)
            return;

        static std::mutex throttleMutex;
        static std::map<uint32, uint32> lastAt;
        const uint32 now = WorldTimer::getMSTime();
        {
            std::scoped_lock lock(throttleMutex);
            uint32& last = lastAt[group->GetId()];
            if (last && WorldTimer::getMSTimeDiff(last, now) < 500)
                return;
            last = now;
        }

        EnsureModeEntries();
        EnsureRegisteredBosses();
        Compute(bot);
    }
}
