#pragma once

// ---------------------------------------------------------------------------
// [2026-11-01 站长口径] 副本拉怪诊断日志的"节流 + 结构化"（header-only，无 .cpp）
//
// 背景：PULLWHY / PULLSAFE / PULLCOST / MOVETGT / PULLDIAG 这几类诊断行是
//   "每个 bot × 每个 tick × 每个 map" 都会打一次的，用 sLog.outError 直出会把真正的
//   error 淹掉（DungeonRouteActions.cpp 45 处、DungeonRoute.h 49 处），而且没人读得完。
//
// 这里**只改"怎么打"**，不改任何判断 / 返回值 / 状态写入：
//   ① 每 (tag,key) 在窗口内最多输出 1 条；被压掉的条数在**下一条真正输出的**里带 suppressed=N；
//   ② 每 REPORT_MS（60s）在首次触发的 Event 里顺带打一行 [ATTR] 汇总（各 tag 本窗口 out/sup）。
//
// 输出格式（一行一条；tag 保持在行首，`grep '^\[PULLWHY\]'` 即可）：
//   [PULLWHY] tag=PULLWHY key=1234 suppressed=7 bot=xxx reason=no_route dist=-1.0 ord=0 combat=0
//   [ATTR] window=60s PULLWHY out=3 sup=1180; PULLSAFE out=0 sup=344
//
// 窗口默认 30000ms；环境变量 DUNGEON_LOG_THROTTLE_MS 可覆盖（不设 / 非法 / 0 ⇒ 用默认）。
//
// key 取"能区分谁在哪"的最小键（见 dungeonroute::TeleKeyBot / TeleKeyWave）：
//   bot 维度的决策 ⇒ bot guid low；波/boss 阶段维度 ⇒ (map<<20)|ord。同一 tag 下不同 key 互不影响。
//
// 线程安全：Map::Update 是"每个 map 一个 worker"，这里用**一把 std::mutex** 护住整张表；
//   锁里只做查表 + 计数（unordered_map，O(1)），格式化与写日志一律放到锁外，
//   免得一条慢 IO 把别的 map 线程也堵住。
// ---------------------------------------------------------------------------

#include "Common.h"
#include "Log/Log.h"
#include "Util/Timer.h"

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace dtelemetry
{
    // 节流窗口（毫秒）：同一 (tag,key) 这么久内最多输出一条
    const uint32 DEFAULT_THROTTLE_MS = 30000;
    // 汇总窗口（毫秒）：[ATTR] 一行的统计周期
    const uint32 REPORT_MS = 60000;
    // 节流表规模上限（防御）：键空间异常膨胀时丢掉节流状态（只会短暂放开日志，不影响任何逻辑）
    const uint32 MAX_ENTRIES = 8192;

    namespace detail
    {
        // 单个 (tag,key) 的节流状态
        struct Entry
        {
            uint32 lastMs = 0;        // 上次**真正输出**的时刻（0 = 还没输出过 ⇒ 首条必放行）
            uint32 suppressed = 0;    // 本窗口内被压掉的条数
        };

        // 单个 tag 在本报告窗口内的统计
        struct TagStat
        {
            uint32 out = 0;   // 真正输出的条数
            uint32 sup = 0;   // 被压掉的条数
        };

        struct Table
        {
            std::mutex mutex;
            std::unordered_map<std::string, uint32> tagIds;   // tag 名 -> 内部 id（从 1 起）
            std::vector<std::string> tagNames;                // 内部 id-1 -> tag 名
            std::unordered_map<uint64, Entry> entries;        // (tagId<<32)|key -> 节流状态
            std::vector<TagStat> stats;                       // 内部 id-1 -> 本窗口 out/sup
            uint32 lastReportMs = 0;                          // 上次 [ATTR] 的时刻
        };

        inline Table& TableOf()
        {
            static Table t;
            return t;
        }

        // WorldTimer::getMSTime() 偶尔返回 0（刚起服），沿用文件里的老写法：0 当 1，保证"0 = 未初始化"的含义
        inline uint32 NowMs()
        {
            uint32 const t = WorldTimer::getMSTime();
            return t ? t : 1;
        }

        inline uint32 ThrottleMs()
        {
            static uint32 const ms = []() -> uint32
            {
                char const* env = std::getenv("DUNGEON_LOG_THROTTLE_MS");
                if (!env || !*env)
                    return DEFAULT_THROTTLE_MS;
                char* end = nullptr;
                unsigned long const v = std::strtoul(env, &end, 10);
                if (end == env || v == 0)
                    return DEFAULT_THROTTLE_MS;
                if (v < 100)             // 太小等于没节流
                    return 100;
                if (v > 3600000ul)       // 太大等于静音，封顶 1 小时
                    return 3600000ul;
                return uint32(v);
            }();
            return ms;
        }

        // tag 名 -> 内部 id（内容比较，不按字符串指针：同一个 tag 在不同调用点的字面量地址可能不同）
        inline uint32 InternLocked(Table& t, char const* tag)
        {
            std::string const name(tag);
            auto it = t.tagIds.find(name);
            if (it != t.tagIds.end())
                return it->second;

            uint32 const id = uint32(t.tagNames.size()) + 1;
            t.tagIds.emplace(name, id);
            t.tagNames.push_back(name);
            t.stats.push_back(TagStat());
            return id;
        }

        // 生成 [ATTR] 一行并**清零本窗口统计**；force=false 且本窗口没有任何计数时返回空串（不打空行）
        inline std::string BuildReportLocked(Table& t, bool force)
        {
            std::string body;
            for (uint32 id = 1; id <= t.tagNames.size(); ++id)
            {
                TagStat& s = t.stats[id - 1];
                if (!s.out && !s.sup)
                    continue;

                char one[128];
                std::snprintf(one, sizeof(one), "%s out=%u sup=%u", t.tagNames[id - 1].c_str(), s.out, s.sup);
                if (!body.empty())
                    body += "; ";
                body += one;
                s.out = 0;
                s.sup = 0;
            }
            if (body.empty())
                return force ? std::string("(no events)") : std::string();

            return std::string("[ATTR] window=") + std::to_string(REPORT_MS / 1000) + "s " + body;
        }

        // 到点就生成汇总（调用方在锁外打印）；返回空串 = 这次不打
        inline std::string MaybeReportLocked(Table& t, uint32 now)
        {
            if (!t.lastReportMs)
            {
                t.lastReportMs = now;
                return std::string();
            }
            if (WorldTimer::getMSTimeDiff(t.lastReportMs, now) < REPORT_MS)
                return std::string();

            t.lastReportMs = now;
            return BuildReportLocked(t, false);
        }
    }

    // 节流输出一条诊断：内容照抄原来的 printf 风格，只是被套上 (tag,key) 的窗口
    inline void Event(char const* tag, uint32 key, char const* fmt, ...)
    {
        if (!tag || !*tag || !fmt)
            return;

        bool emit = false;
        uint32 suppressed = 0;
        std::string report;

        detail::Table& t = detail::TableOf();
        {
            std::lock_guard<std::mutex> lock(t.mutex);
            uint32 const now = detail::NowMs();
            if (t.entries.size() > MAX_ENTRIES)
                t.entries.clear();

            uint32 const id = detail::InternLocked(t, tag);
            detail::Entry& e = t.entries[(uint64(id) << 32) | uint64(key)];
            if (!e.lastMs || WorldTimer::getMSTimeDiff(e.lastMs, now) >= detail::ThrottleMs())
            {
                e.lastMs = now;              // 本窗口这一条放行
                suppressed = e.suppressed;   // 把窗口内压掉的条数带给下一条
                e.suppressed = 0;
                emit = true;
                ++t.stats[id - 1].out;       // 被压掉的条数在压制那一刻已经计入 sup，这里不要再加一遍
            }
            else
            {
                ++e.suppressed;
                ++t.stats[id - 1].sup;
            }

            report = detail::MaybeReportLocked(t, now);
        }

        if (!report.empty())
            sLog.outError("%s", report.c_str());
        if (!emit)
            return;

        char line[2048];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(line, sizeof(line), fmt, ap);
        va_end(ap);

        sLog.outError("[%s] tag=%s key=%u suppressed=%u %s", tag, tag, key, suppressed, line);
    }

    // 手动打一次汇总（把本窗口统计清零）；Event 里也会按 REPORT_MS 自动打
    inline void Report()
    {
        detail::Table& t = detail::TableOf();
        std::string line;
        {
            std::lock_guard<std::mutex> lock(t.mutex);
            t.lastReportMs = detail::NowMs();
            line = detail::BuildReportLocked(t, true);
        }
        if (!line.empty())
            sLog.outError("%s", line.c_str());
    }
}
