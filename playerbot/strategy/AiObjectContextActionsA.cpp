// [降峰值 2026-10-15] AiObjectContext 的注册表拆分：主 Action 注册表（最大的一块，单独一个 TU）。
#include "playerbot/playerbot.h"
#include "AiObjectContext.h"
#include "actions/ActionContext.h"

using namespace ai;

void AiObjectContext::BuildActionContextsA()
{
    actionContexts.Add(new ActionContext());
}
