#pragma once
#include "playerbot/playerbot.h"

// ---------------------------------------------------------------------------
// [2026-10-21 站长口径] BOSS 场地数据化行为（表：ai_playerbot_boss_arena）
//   hold   : 只有特殊机制的 BOSS 才有的固定拉怪点（坦克把 BOSS 带到这里）
//   bounds : 场地圆（圆心 + 半径）：允许场内补拉，但绝不追出场地
//   普通 BOSS 没有数据 ⇒ 完全不改变原有行为。
// ---------------------------------------------------------------------------
namespace bossarena
{
    struct Arena
    {
        bool hasHold = false;
        float hx = 0, hy = 0, hz = 0, holdTol = 3.0f;
        bool hasBounds = false;
        float bx = 0, by = 0, bz = 0, brad = 0;
    };

    inline std::mutex& Mutex() { static std::mutex m; return m; }
    inline std::map<uint64, Arena>& Cache() { static std::map<uint64, Arena> c; return c; }
    inline time_t& LoadedAt() { static time_t t = 0; return t; }

    inline void Load()
    {
        Cache().clear();
        std::unique_ptr<QueryResult> res = WorldDatabase.Query(
            "SELECT map,boss_entry,x,y,z,tol FROM ai_playerbot_boss_arena "
            "WHERE kind='hold' AND role='tank'");
        if (res)
        {
            do
            {
                Field* f = res->Fetch();
                Arena& a = Cache()[(uint64)f[0].GetUInt32() * 100000u + f[1].GetUInt32()];
                a.hasHold = true;
                a.hx = f[2].GetFloat(); a.hy = f[3].GetFloat(); a.hz = f[4].GetFloat();
                a.holdTol = f[5].GetFloat();
            } while (res->NextRow());
        }

        res = WorldDatabase.Query(
            "SELECT map,boss_entry,x,y,z,tol FROM ai_playerbot_boss_arena "
            "WHERE kind='bounds'");
        if (res)
        {
            do
            {
                Field* f = res->Fetch();
                Arena& a = Cache()[(uint64)f[0].GetUInt32() * 100000u + f[1].GetUInt32()];
                a.hasBounds = true;
                a.bx = f[2].GetFloat(); a.by = f[3].GetFloat(); a.bz = f[4].GetFloat();
                a.brad = f[5].GetFloat();
            } while (res->NextRow());
        }
    }

    inline Arena const* Get(uint32 mapId, uint32 bossEntry)
    {
        if (!mapId || !bossEntry)
            return nullptr;
        std::scoped_lock lock(Mutex());
        uint64 const key = (uint64)mapId * 100000u + bossEntry;
        if (!LoadedAt() || time(nullptr) - LoadedAt() >= 300)
        {
            LoadedAt() = time(nullptr);
            Load();
        }
        auto it = Cache().find(key);
        return it == Cache().end() ? nullptr : &it->second;
    }

    inline float Dist2d(float x1, float y1, float x2, float y2)
    {
        float const dx = x1 - x2, dy = y1 - y2;
        return sqrtf(dx * dx + dy * dy);
    }

    inline bool Inside(Arena const& a, Unit* u)
    {
        if (!a.hasBounds || !u)
            return true;
        return Dist2d(u->GetPositionX(), u->GetPositionY(), a.bx, a.by) <= a.brad;
    }

    // 每个 bot 的低频钩子（挂在 PlayerbotAI 的 boss 策略更新处）：
    //   · 目标越界（跑到场地外）⇒ 放弃追击，绝不拉出场地；
    //   · 自己越界 ⇒ 回场地 hold 点；
    //   · 坦克在 hold 点附近 ⇒ 只微调/补拉场内怪。
    inline void Tick(PlayerbotAI* ai)
    {
        if (!ai)
            return;
        Player* bot = ai->GetBot();
        if (!bot || !bot->IsInWorld() || !bot->IsAlive() || !bot->IsInCombat())
            return;
        Map* map = bot->GetMap();
        if (!map || (!map->IsDungeon() && !map->IsRaid()))
            return;
        Unit* victim = bot->GetVictim();
        if (!victim)
            return;
        Arena const* a = Get(map->GetId(), victim->GetEntry());
        if (!a || (!a->hasHold && !a->hasBounds))
            return;

        float const px = bot->GetPositionX(), py = bot->GetPositionY(), pz = bot->GetPositionZ();

        if (a->hasBounds)
        {
            // 目标越界 ⇒ 不追
            if (!Inside(*a, victim))
            {
                bot->AttackStop();
                bot->SetSelectionGuid(ObjectGuid());
                return;
            }
            // 自己越界 ⇒ 拉回场地
            if (Dist2d(px, py, a->bx, a->by) > a->brad)
            {
                float const tx = a->hasHold ? a->hx : a->bx;
                float const ty = a->hasHold ? a->hy : a->by;
                float const tz = a->hasHold ? a->hz : a->bz;
                bot->GetMotionMaster()->MovePoint(0, tx, ty, tz);
                return;
            }
        }

        if (!a->hasHold || !ai->IsTank(bot))
            return;

        // 坦克：偏离 hold 点就回去（允许朝向/微调：只在超容忍且未在移动时触发）
        if (Dist2d(px, py, a->hx, a->hy) > a->holdTol && !bot->IsMoving())
            bot->GetMotionMaster()->MovePoint(0, a->hx, a->hy, a->hz);
    }
}
