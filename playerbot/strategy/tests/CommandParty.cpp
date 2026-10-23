#include "playerbot/playerbot.h"
#include "CommandParty.h"
#include "Grids/GridNotifiers.h"
#include "Grids/GridNotifiersImpl.h"
#include "Grids/CellImpl.h"
#include "TestContext.h"
#include "playerbot/PlayerbotMgr.h"
#include "Groups/Group.h"

using namespace ai;

namespace
{
    // [bottest 2026-10-19] 测试场景里"由谁创建新 bot"。
    //   原来只认 ai->GetHolder()：随机机器人（sRandomPlayerbotMgr 代管）能拿到，
    //   但**既不是随机机器人、也没有 master 的 bot** 拿到的是 nullptr ⇒
    //   场景第一句 `spawn level=70 class=warrior ...` 直接 IMPOSSIBLE，整个战斗测试一条命令都不执行
    //   （实测 `[BOTTEST] ... IMPOSSIBLE | Failed to spawn bot with params: ...`）。
    //   这里补一个兜底：拿不到 holder 时用随机机器人管理器创建（它本来就是标准的 bot 创建入口）。
    PlayerbotHolder* ResolveTestSpawnHolder(PlayerbotAI* ai)
    {
        if (!ai)
            return nullptr;

        if (PlayerbotHolder* holder = ai->GetHolder())
            return holder;

        return &sRandomPlayerbotMgr;
    }

    // [bottest 2026-10-19 站长口径] "不新建角色，直接拉在线的 bot 入队"。
    //   原来 spawn 一定新建角色，在本机卡在三个数据问题上：账号槽位满（20×9=180 全满）、
    //   AccountMgr 角色数缓存与 DB 不一致、CreateAccount 失败 —— 场景第一步就 IMPOSSIBLE。
    //   现在先尝试**借用已经在线**的随机机器人（同图、活着、还没被占用），优先级：
    //     ① 职业与 `class=xxx` 匹配的（脚本里通常指定了职业组合）
    //     ② 其它任意可用机器人
    //   只有找不到可用机器人才退回"新建角色"的老路径。
    Player* FindBorrowableBot(Player* host, PlayerbotAI* hostAi, TestContext const& ctx, uint8 wantedClass)
    {
        if (!host)
            return nullptr;

        auto isTaken = [&ctx](ObjectGuid guid)
        {
            for (ObjectGuid const& used : ctx.spawnedBots)
            {
                if (used == guid)
                    return true;
            }
            return false;
        };

        // [bottest 2026-10-19] 三档放宽，避免"同图 + 指定职业"太难凑（实测只借到 3 个，
        //   然后 `class=mage` 找不到就退回新建角色、又撞账号上限 ⇒ 整个场景 IMPOSSIBLE）：
        //   ① 同图 + 指定职业   ② 任意地图 + 指定职业   ③ 任意地图 + 任意职业
        //   脚本后面本来就有 `teleport` 把队伍拉到一起，所以跨图借用是安全的。
        Player* pass2 = nullptr;    // 任意地图 + 指定职业
        Player* pass3 = nullptr;    // 任意地图 + 任意职业

        HashMapHolder<Player>::MapType const& players = sObjectAccessor.GetPlayers();
        for (auto const& itr : players)
        {
            Player* candidate = itr.second;
            if (!candidate || candidate == host)
                continue;

            if (!candidate->IsInWorld() || !candidate->GetPlayerbotAI() || !candidate->IsAlive())
                continue;

            if (candidate->GetGroup())
                continue;   // 已经在别的队伍里，别拆人家

            if (isTaken(candidate->GetObjectGuid()))
                continue;

            bool const sameMap = (candidate->GetMapId() == host->GetMapId());
            bool const classOk = (!wantedClass || candidate->getClass() == wantedClass);

            if (sameMap && classOk)
                return candidate;

            if (classOk && !pass2)
                pass2 = candidate;

            if (!pass3)
                pass3 = candidate;
        }

        return pass2 ? pass2 : pass3;
    }

    // 从 "level=70 class=warrior temporary=1 login=1" 里取 class
    uint8 ParseWantedClass(std::string const& params)
    {
        std::vector<std::string> args = Qualified::getMultiQualifiers(params, " ");
        for (std::string const& arg : args)
        {
            size_t eq = arg.find('=');
            if (eq == std::string::npos || arg.substr(0, eq) != "class")
                continue;

            return ChatHelper::parseClass(arg.substr(eq + 1));
        }

        return 0;
    }
}

TestResult CommandPartySpawnBot::Execute(const std::string& params, Player* bot, PlayerbotAI* ai, TestContext& ctx, std::string& message)
{
    // ① 先借用在线机器人（不新建角色）
    uint8 const wantedClass = ParseWantedClass(params);
    if (Player* borrowed = FindBorrowableBot(bot, ai, ctx, wantedClass))
    {
        ctx.spawnedBots.push_back(borrowed->GetObjectGuid());
        ctx.borrowedBots.insert(borrowed->GetObjectGuid());
        sLog.outError("[BOTTEST] borrowed online bot %s (class=%u) for test party",
                       borrowed->GetName(), (uint32)borrowed->getClass());
        return TestResult::PASS;
    }

    // ② 没有可用机器人，才走"新建角色"的老路径（可能因账号/角色上限失败）
    PlayerbotHolder* holder = ResolveTestSpawnHolder(ai);
    if (!holder)
    {
        message = "Failed to spawn bot with params: " + params + " | reason: no online bot available and no holder";
        sLog.outError("[BOTTEST] spawn failed: %s", message.c_str());
        return TestResult::IMPOSSIBLE;
    }
    std::list<std::string> messages;
    ObjectGuid guid;
    holder->CreateBot(bot, params + " temporary=true", messages, guid);

    if (!guid)
    {
        // [bottest 2026-10-19] 把 CreateBot 内部的失败原因带出来：
        //   原来只报"Failed to spawn bot with params"，看不到到底是"账号满"、"名字重复"还是"
        //   创建角色失败"，只能靠猜。现在把 messages 拼进诊断信息，同时写一条 error 日志。
        message = "Failed to spawn bot with params: " + params;
        if (!messages.empty())
        {
            message += " | reason: ";
            bool first = true;
            for (auto const& m : messages)
            {
                if (!first)
                    message += "; ";
                message += m;
                first = false;
            }
        }
        sLog.outError("[BOTTEST] spawn failed: %s", message.c_str());
        return TestResult::IMPOSSIBLE;
    }

    ctx.spawnedBots.push_back(guid);

    return TestResult::PASS;
}

TestResult CommandPartyDespawnBot::Execute(const std::string& params, Player* bot,
                    PlayerbotAI* ai, TestContext& ctx, std::string& message)
{
    PlayerbotHolder* holder = ResolveTestSpawnHolder(ai);
    if (!holder)
    {
        message = "Failed to delete spawned bots";
        return TestResult::IMPOSSIBLE;
    }

    for (auto& guid : ctx.spawnedBots)
    {
        // [bottest 2026-10-19] 借来的在线机器人**不能删**（删了等于删掉世界里的机器人账号/角色），
        //   只把他们从队伍里踢出去，交还给随机机器人管理器。
        if (ctx.borrowedBots.find(guid) != ctx.borrowedBots.end())
        {
            if (Player* borrowed = sObjectMgr.GetPlayer(guid))
            {
                if (Group* group = borrowed->GetGroup())
                    group->RemoveMember(guid, 0);   // 0 = 仅移除，不当作 kick

                if (PlayerbotAI* borrowedAi = borrowed->GetPlayerbotAI())
                    borrowedAi->HandleCommand(CHAT_MSG_WHISPER, "reset", *borrowed);
            }
            continue;
        }

        holder->DeleteBot(guid);
    }
    ctx.spawnedBots.clear();
    ctx.borrowedBots.clear();
    return TestResult::PASS;
}

TestResult CommandPartyForm::Execute(const std::string& params, Player* bot,
                    PlayerbotAI* ai, TestContext& ctx, std::string& message)
{
    Group* group = bot->GetGroup();
    if (!group)
    {
        group = new Group;
        if (!group->Create(bot->GetObjectGuid(), bot->GetName()))
        {
            delete group;
            message = "Failed to create group";
            return TestResult::IMPOSSIBLE;
        }
        sObjectMgr.AddGroup(group);
    }

    if (!ctx.spawnedBots.empty())
    {
        for (const auto& guid : ctx.spawnedBots)
        {
            if (!guid || !guid.IsPlayer())
                continue;

            Player* member = sObjectMgr.GetPlayer(guid);
            if (!member || !member->IsInWorld())
            {
                message = "Waiting for spawned bots to enter world before forming party";
                return TestResult::PENDING;
            }
        }
    }

    for (const auto& guid : ctx.spawnedBots)
    {
        if (!guid || !guid.IsPlayer())
            continue;

        Player* member = sObjectMgr.GetPlayer(guid);
        if (!member)
            continue;

        if (!group->IsMember(guid) && !group->AddMember(guid, member->GetName()))
        {
            message = "Failed to add member to group: " + std::string(member->GetName());
            return TestResult::IMPOSSIBLE;
        }

        if (PlayerbotAI* memberAi = member->GetPlayerbotAI())
            memberAi->HandleCommand(CHAT_MSG_WHISPER, "follow " + std::string(bot->GetName()), *bot);
    }

    return TestResult::PASS;
}

TestResult CommandPartySpawnGroup::Execute(const std::string& params, Player* bot,
                    PlayerbotAI* ai, TestContext& ctx, std::string& message)
{
    sRandomPlayerbotMgr.HandleGroup(bot, params + " temporary=true", SEC_PLAYER);

    return TestResult::PASS;
}