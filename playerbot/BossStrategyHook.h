#pragma once

// ---------------------------------------------------------------------------
// [BOSS 战策略 2026-10-16] 按"队伍当前正在打的 BOSS"自动挂载 / 卸载自定义策略。
//
// 为什么单独一个头文件：本批工作期间 `PlayerbotAI.cpp` 被**外部批量改写**覆盖过一次，
// 打进去的钩子没了。把实现放在独立头里、`PlayerbotAI.cpp` 只留 3 行接入
//   （include + UpdateAIInternal 末尾调用 + ~PlayerbotAI() 里清理），
// 这样被覆盖后重打成本最低。
//
// 数据口径（详见 dev/功能更新手册_卡布魔兽.md 的「副本/BOSS 战策略」一章）：
//   tbccharacters.ai_playerbot_custom_strategy(owner=0)：
//     通用集 boss_common*          打"登记过的 BOSS"时挂（只写一份，所有 BOSS 复用）
//     专属集 boss_<entry>          该 BOSS 特有的
//     **角色后缀**（2026-10-16 追加）：集名后可跟 _dps/_tank/_healer/_melee/_ranged，
//       多个=与关系（例 boss_18371_dps_melee）；**只有匹配角色的 bot 才挂那个集**。
//       换句话说："转火"这类规则写在 _dps 集里，就天然对坦克/治疗不生效。
//   tbccharacters.ai_playerbot_boss_strategy(entry, note)：登记表（只有通用行为的 BOSS 用）。
//   开关：AiPlayerbot.BossStrategies（默认 1）。日志：[BOSSSTRAT]（sLog.outString → Server.log）。
//   限流：每个 bot 每 5 秒看一次；名单每 5 分钟重扫（改数据不用重启）。
//   线程：MapUpdate.Threads 是多线程，所以共享表加锁；状态键用 PlayerbotAI*，析构里清。
// ---------------------------------------------------------------------------

namespace bossstrategy
{
    const uint32 CHECK_INTERVAL = 5000;      // 每个 bot 的检查间隔（毫秒）
    const uint32 LIST_REFRESH = 300000;      // BOSS 名单重扫间隔（毫秒）
    const char* COMMON_PREFIX = "boss_common";   // 通用集前缀

    std::mutex mutex;
    std::map<uint32, std::vector<std::string>> byEntry;   // entry -> 该 BOSS 的所有集名（含角色后缀）
    std::vector<std::string> commonSets;                  // 通用集名（含角色后缀）
    bool listLoaded = false;
    uint32 listCheckedAt = 0;

    struct State
    {
        uint32 lastCheck = 0;
        std::set<std::string> mounted;    // 已挂上的策略名（DB 口径）
    };

    // 键用 PlayerbotAI 指针：清理时不需要再解引用 bot（bot 可能正在析构）
    std::map<PlayerbotAI*, State> states;

    // 集名尾部的角色后缀是否匹配这个 bot（不认识的 token ⇒ 不挂，宁缺勿滥）
    inline bool RoleAllows(PlayerbotAI* ai, Player* bot, const std::string& name)
    {
        std::vector<std::string> tokens = split(name, '_');
        size_t i = 0;
        for (; i < tokens.size(); ++i)
        {
            if (tokens[i] == "boss")
                continue;
            if (tokens[i] == "common" || (!tokens[i].empty() &&
                tokens[i].find_first_not_of("0123456789") == std::string::npos))
                break;
        }

        for (size_t j = i + 1; j < tokens.size(); ++j)
        {
            const std::string& role = tokens[j];
            if (role == "dps")
            {
                if (PlayerbotAI::IsTank(bot) || PlayerbotAI::IsHeal(bot))
                    return false;
            }
            else if (role == "tank")
            {
                if (!PlayerbotAI::IsTank(bot))
                    return false;
            }
            else if (role == "healer")
            {
                if (!PlayerbotAI::IsHeal(bot))
                    return false;
            }
            else if (role == "melee")
            {
                if (!ai->IsMelee(bot))
                    return false;
            }
            else if (role == "ranged")
            {
                if (!ai->IsRanged(bot))
                    return false;
            }
            else
                return false;
        }

        return true;
    }

    // 调用方必须已持有 mutex
    inline void Load()
    {
        byEntry.clear();
        commonSets.clear();
        listLoaded = true;
        listCheckedAt = WorldTimer::getMSTime();

        // owner=0 = 全服共享；owner 非 0 的是单 bot 自己写的，不参与 BOSS 策略
        auto results = CharacterDatabase.Query("SELECT DISTINCT name FROM ai_playerbot_custom_strategy WHERE owner = 0");
        if (results)
        {
            do
            {
                Field* fields = results->Fetch();
                std::string name = fields[0].GetString();
                if (name.empty() || name.compare(0, 5, "boss_") != 0)
                    continue;

                if (name.rfind(COMMON_PREFIX, 0) == 0)   // 通用集（别去解析 entry）
                {
                    commonSets.push_back(name);
                    continue;
                }

                uint32 entry = (uint32)atoi(name.c_str() + 5);
                if (!entry)
                    continue;

                byEntry[entry].push_back(name);
            } while (results->NextRow());
        }

        // 登记表：只吃通用集、没有专属规则的 BOSS 也登记
        auto listed = CharacterDatabase.Query("SELECT entry FROM ai_playerbot_boss_strategy");
        if (listed)
        {
            do
            {
                Field* fields = listed->Fetch();
                uint32 entry = fields[0].GetUInt32();
                if (entry)
                    byEntry[entry];   // 只登记（可能没有任何专属集）
            } while (listed->NextRow());
        }
    }

    // 只认"名单里有的"生物 entry
    inline uint32 EntryOfUnit(Unit* unit)
    {
        if (!unit || unit->GetTypeId() != TYPEID_UNIT || !unit->IsAlive())
            return 0;

        uint32 entry = unit->GetEntry();
        return byEntry.find(entry) != byEntry.end() ? entry : 0;
    }

    // 队伍（含真人玩家）正在打的 BOSS entry；没有就返回 0
    inline uint32 FightTargetEntry(Player* bot)
    {
        if (uint32 entry = EntryOfUnit(bot->GetVictim()))
            return entry;
        if (uint32 entry = EntryOfUnit(bot->GetMap()->GetCreature(bot->GetSelectionGuid())))
            return entry;

        Group* group = bot->GetGroup();
        if (!group)
            return 0;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->getSource();
            if (!member || member == bot || !member->IsInWorld())
                continue;

            if (uint32 entry = EntryOfUnit(member->GetVictim()))
                return entry;
            if (uint32 entry = EntryOfUnit(member->GetMap()->GetCreature(member->GetSelectionGuid())))
                return entry;
        }

        return 0;
    }

    // 每个 bot 每 ~5 秒跑一次（由 PlayerbotAI::UpdateAIInternal 末尾调用）
    inline void Update(PlayerbotAI* ai)
    {
        if (!sPlayerbotAIConfig.bossStrategies)
            return;

        Player* bot = ai->GetBot();
        if (!bot || !bot->IsInWorld())
            return;

        uint32 now = WorldTimer::getMSTime();
        std::vector<std::string> added;
        std::vector<std::string> removed;

        {
            std::scoped_lock lock(mutex);

            State& state = states[ai];
            if (state.lastCheck && WorldTimer::getMSTimeDiff(state.lastCheck, now) < CHECK_INTERVAL)
                return;

            state.lastCheck = now;

            if (!listLoaded || WorldTimer::getMSTimeDiff(listCheckedAt, now) > LIST_REFRESH)
                Load();

            // 想要哪些：登记过的 BOSS → 通用集 + 该 BOSS 的所有集，且只挑"角色后缀匹配本 bot"的
            std::set<std::string> desired;
            uint32 entry = FightTargetEntry(bot);
            auto it = byEntry.find(entry);
            if (it != byEntry.end())
            {
                for (const std::string& name : commonSets)
                {
                    if (RoleAllows(ai, bot, name))
                        desired.insert(name);
                }

                for (const std::string& name : it->second)
                {
                    if (RoleAllows(ai, bot, name))
                        desired.insert(name);
                }
            }

            // 该摘的：不想要了，或引擎里已经被 ResetStrategies/重登清掉了（HasStrategy 是权威）
            for (auto i = state.mounted.begin(); i != state.mounted.end(); )
            {
                if (!desired.count(*i) || !ai->HasStrategy("custom::" + *i, BotState::BOT_STATE_COMBAT))
                {
                    removed.push_back(*i);
                    i = state.mounted.erase(i);
                }
                else
                    ++i;
            }

            // 该挂的
            for (const std::string& name : desired)
            {
                if (!state.mounted.count(name))
                {
                    added.push_back(name);
                    state.mounted.insert(name);
                }
            }
        }

        for (const std::string& name : removed)
        {
            ai->ChangeStrategy("-custom::" + name, BotState::BOT_STATE_COMBAT);
            sLog.outString("[BOSSSTRAT] %s -custom::%s", bot->GetName(), name.c_str());
        }

        for (const std::string& name : added)
        {
            ai->ChangeStrategy("+custom::" + name, BotState::BOT_STATE_COMBAT);
            sLog.outString("[BOSSSTRAT] %s +custom::%s", bot->GetName(), name.c_str());
        }
    }

    // 由 ~PlayerbotAI() 调用
    inline void Forget(PlayerbotAI* ai)
    {
        std::scoped_lock lock(mutex);
        states.erase(ai);
    }
}
