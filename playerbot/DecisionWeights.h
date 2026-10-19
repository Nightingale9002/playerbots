#pragma once
#include "playerbot/playerbot.h"

#include <map>
#include <mutex>
#include <string>

// ---------------------------------------------------------------------------
// [决策参数化 2026-10-18 站长口径] 把"策略里写死的权重"挪到数据表，方便离线调参/后续 RL：
//   表：tbcmangos.ai_playerbot_decision_weight(`key`,`weight`,`note`)
//   读路径：thread_local 缓存 + 300s 重读（改数据不用重启）
//   缺表 / 查询失败 ⇒ 用代码里的默认值（绝不因为缺数据影响出怪）
//
// 约定 key（当前已接入）：
//   target.threat_tier      目标：威胁等级权重（默认 100）
//   target.distance         目标：距离权重（默认 -1，每码）
//   target.casting          目标：正在读条加成（默认 30）
//   target.skull            目标：被标骷髅加成（默认 800）
//   position.cover_distance 卡视野掩体点：距离权重（默认 -1，取最近）
// 未列出的 key 可以随时加行，代码里用 dw::Get(key, default)。
// ---------------------------------------------------------------------------

namespace dw
{
    inline std::mutex& WeightMutex() { static std::mutex m; return m; }
    inline std::map<std::string, float>& Weights() { static std::map<std::string, float> w; return w; }

    // 代码默认值（表里没有该 key 时用）
    inline float DefaultWeight(std::string const& key)
    {
        if (key == "target.threat_tier")      return 100.0f;
        if (key == "target.distance")         return -1.0f;
        if (key == "target.casting")          return 30.0f;
        if (key == "position.cover_distance") return -1.0f;
        if (key == "log.decision")            return 0.0f;

        // 控制（CC）目标排序
        if (key == "cc.needs_control")        return 100.0f;
        if (key == "cc.dps")                  return 1.0f;
        if (key == "cc.priority_rank")        return 15.0f;
        if (key == "cc.distance")             return -1.0f;

        // 治疗目标排序
        if (key == "heal.missing_health")     return 100.0f;
        if (key == "heal.tank_bonus")         return 30.0f;
        if (key == "heal.incoming_damage")    return 1.0f;
        if (key == "heal.distance")           return -0.5f;

        // 进攻驱散排序
        if (key == "dispel.buff_count")       return 50.0f;
        if (key == "dispel.casting")          return 30.0f;
        if (key == "dispel.priority_rank")    return 20.0f;
        if (key == "dispel.distance")         return -1.0f;

        // 跑位模式选择（同一只怪同时命中多种模式时用）
        if (key == "position.mode.los_all")      return 100.0f;
        if (key == "position.mode.los_pull")     return 95.0f;
        if (key == "position.mode.los_backline") return 90.0f;
        if (key == "position.mode.los_break")    return 85.0f;
        if (key == "position.mode.frontal")      return 70.0f;
        if (key == "position.mode.knockback")    return 60.0f;
        if (key == "position.mode.stack")        return 40.0f;
        if (key == "position.mode.spread")       return 30.0f;
        if (key == "position.mode.wall")         return 20.0f;

        // [跑位 2026-10-19] 每种阵型一条权重（以后靠权重选站位）
        if (key == "position.mode.role_bands")    return 10.0f;
        // [跑位 2026-10-19] 距离带：上限 + 下限（码）
        if (key == "position.band.min_range")     return 12.0f;
        if (key == "position.band.melee")         return 4.5f;
        if (key == "position.band.mid_range")     return 20.0f;   // 暗牧/奥法
        if (key == "position.band.ranged")        return 30.0f;   // 其它远程
        if (key == "position.band.healer")        return 30.0f;   // 治疗
        // 有近战 AOE 的 BOSS：远程/治疗不能站太近（默认 18 码）
        if (key == "position.melee_aoe_min")      return 18.0f;

        return 0.0f;
    }

    inline void EnsureWeightsLoaded()
    {
        static std::mutex loadMutex;
        static time_t loadedAt = 0;
        static bool tried = false;

        std::scoped_lock lock(loadMutex);
        const time_t now = time(nullptr);
        if (tried && now - loadedAt < 300)
            return;
        tried = true;
        loadedAt = now;

        std::unique_ptr<QueryResult> table = WorldDatabase.Query(
            "SELECT 1 FROM information_schema.tables WHERE table_schema = DATABASE() "
            "AND table_name = 'ai_playerbot_decision_weight'");
        if (!table)
            return;

        std::unique_ptr<QueryResult> res = WorldDatabase.Query(
            "SELECT `key`,`weight` FROM `ai_playerbot_decision_weight`");
        if (!res)
            return;

        std::map<std::string, float> loaded;
        do
        {
            Field* fields = res->Fetch();
            loaded[fields[0].GetString()] = fields[1].GetFloat();
        } while (res->NextRow());

        {
            std::scoped_lock wlock(WeightMutex());
            Weights() = loaded;
        }

        static size_t lastCount = (size_t)-1;
        if (loaded.size() != lastCount)
        {
            lastCount = loaded.size();
            sLog.outString("[WEIGHTS] 决策权重已载入 %u 项（其余用代码默认值）", (uint32)loaded.size());
        }
    }

    inline float Get(std::string const& key)
    {
        EnsureWeightsLoaded();

        std::scoped_lock lock(WeightMutex());
        auto it = Weights().find(key);
        return it == Weights().end() ? DefaultWeight(key) : it->second;
    }
}
