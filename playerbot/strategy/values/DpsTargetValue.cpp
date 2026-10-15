
#include "playerbot/playerbot.h"
#include "DpsTargetValue.h"
#include "LeastHpTargetValue.h"

using namespace ai;


ObjectGuid DpsTargetValue::Calculate()
{
    ObjectGuid rti = RtiTargetValue::Calculate();
    if (rti) return rti;

    // [aggro 2026-10-17 站长口径] 原来这里用 FindLeastHpTargetStrategy（**只看残血**），
    //   于是"正在加血/正在读条的法系怪"和"正在咬治疗的近战怪"都会被一只远处残血怪抢走。
    //   现在改走威胁分级链（谁威胁高打谁，残血只是最后的 tie-break）。
    FindDpsThreatTargetStrategy strategy(ai);
    Unit* target = TargetValue::FindTarget(&strategy);
    return target ? target->GetObjectGuid() : ObjectGuid();
}

ObjectGuid DpsAoeTargetValue::Calculate()
{
    ObjectGuid rti = RtiTargetValue::Calculate();
    if (rti) return rti;

    // AOE 也一样：优先把"会加血/可打断的施法怪"和"正在打队友的怪"拉进 AOE 范围。
    FindDpsThreatTargetStrategy strategy(ai);
    Unit* target = TargetValue::FindTarget(&strategy);
    return target ? target->GetObjectGuid() : ObjectGuid();
}
