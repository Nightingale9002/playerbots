// [降峰值 2026-10-15] AiObjectContext 的注册表拆分：聊天/世界包 Action 注册表。
#include "playerbot/playerbot.h"
#include "AiObjectContext.h"
#include "actions/ChatActionContext.h"
#include "actions/WorldPacketActionContext.h"

using namespace ai;

void AiObjectContext::BuildActionContextsB()
{
    actionContexts.Add(new ChatActionContext());
    actionContexts.Add(new WorldPacketActionContext());
}
