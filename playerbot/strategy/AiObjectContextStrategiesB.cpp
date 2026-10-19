// [降峰值 2026-10-15] AiObjectContext 的注册表拆分（B）：任务/钓鱼等次要策略。
#include "playerbot/playerbot.h"
#include "AiObjectContext.h"
#include "StrategyContext.h"

using namespace ai;

void AiObjectContext::BuildStrategyContextsB()
{
    strategyContexts.Add(new QuestStrategyContext());
    strategyContexts.Add(new FishStrategyContext());
}
