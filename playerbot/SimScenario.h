#pragma once
#include "playerbot/playerbot.h"
#include "playerbot/PlayerbotAIConfig.h"

#include <list>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// [仿真 2026-10-18 站长口径] 可控场景注入（**只在本地**用，云端 AiPlayerbot.SimMode = 0）
//
// 目的：不再依赖"随机 bot 自己组队 / 自己进本"，一条命令就能注入可复现场景：
//   `.bot sim start <mapId> [groups=1] [partySize=5]`
//     → 按职业/职能从候选池挑 bot（优先复用库里已有的 bot 角色，不新建）
//     → 逐个 AddPlayerBot 上线（走的是随机 bot 的登录链，同 `.bot add`）
//     → 直接组队（Group::Create + AddMember，不经过邀请/接受）
//     → 先传队长进本（由队长创建/绑定 instance），其余成员再传（进同一个 instance）
//     → 打开路线运行时开关（RouteRuntimeSwitch = 1，允许未 verified 的图）
//   `.bot sim stop [runId|all]`
//   `.bot sim status`
//
// 采集：沿用现成链路（ai_playerbot_pull_log 的 instance_id、combatlog 的 inst、[DUNGEONRUN]/[PULL]/[POSITION]），
//   本文件只额外打 `[SIM]` 前缀日志，便于按 run/组复盘。
//
// 线程：状态机只在**世界线程**推进（RandomPlayerbotMgr::UpdateAIInternal 开头调用 Update()），
//   控制台 `.rndbot sim ...` 最终也是世界线程（World::ProcessCliCommands），所以内部不需要加锁。
// ---------------------------------------------------------------------------

namespace simscenario
{
    // `.bot sim ...` 的命令入口（薄壳在 PlayerbotHolder::HandleSim）
    std::list<std::string> HandleCommand(Player* master, const std::string& param, AccountTypes security);

    // 每个世界 tick 调一次（空转时只做一次 empty() 判断）
    void Update();

    // 关服/进程退出兜底：把本进程注入的 bot 全部退队 + 下线
    void Shutdown();

    // `[SIM]` 关键字：是否正在跑（后续采集脚本用）
    bool Running();
}
