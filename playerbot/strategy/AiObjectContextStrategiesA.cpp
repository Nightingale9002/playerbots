// [降峰值 2026-10-15] AiObjectContext 的注册表拆分（A）：核心策略。
// 单独一个 TU 的目的：Context 类构造时模板实例化量极大，集中在一个 TU 会让 gcc 峰值 >1.5GB。
#include "playerbot/playerbot.h"
#include "AiObjectContext.h"
#include "StrategyContext.h"

using namespace ai;

void AiObjectContext::BuildStrategyContextsA()
{
    strategyContexts.Add(new StrategyContext());
    strategyContexts.Add(new MovementStrategyContext());
    strategyContexts.Add(new AssistStrategyContext());
}
