// [降峰值 2026-10-15] AiObjectContext 的注册表拆分：Value 注册表。
#include "playerbot/playerbot.h"
#include "AiObjectContext.h"
#include "values/ValueContext.h"

using namespace ai;

void AiObjectContext::BuildValueContexts()
{
    valueContexts.Add(new ValueContext());
}
