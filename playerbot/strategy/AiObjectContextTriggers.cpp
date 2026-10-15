// [降峰值 2026-10-15] AiObjectContext 的注册表拆分：Trigger 注册表。
#include "playerbot/playerbot.h"
#include "AiObjectContext.h"
#include "triggers/TriggerContext.h"
#include "triggers/ChatTriggerContext.h"
#include "triggers/WorldPacketTriggerContext.h"

using namespace ai;

void AiObjectContext::BuildTriggerContexts()
{
    triggerContexts.Add(new TriggerContext());
    triggerContexts.Add(new ChatTriggerContext());
    triggerContexts.Add(new WorldPacketTriggerContext());
}
