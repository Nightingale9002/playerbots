#include "playerbot/playerbot.h"
#include "PartyPositionActions.h"
#include "playerbot/PartyPositionCoordinator.h"

using namespace ai;

bool MoveToPartySlotAction::isPossible()
{
    partypos::Slot slot;
    return partypos::SlotFor(bot, slot);
}

bool MoveToPartySlotAction::Execute(Event& event)
{
    partypos::Slot slot;
    if (!partypos::SlotFor(bot, slot))
        return false;

    // 已到位（1.5 码内）⇒ 不再动，交给其它动作（打怪/打断等）
    if (bot->GetDistance(slot.x, slot.y, slot.z) <= 1.5f)
        return false;

    // [跑位 2026-10-18] 决策层已经保证"槽位"安全；这里对**走过去的路**再用服务端地形数据兜一次：
    //   ① 直线可走（逐段地形高度 + vmap 遮挡；穿墙/跳崖 ⇒ 不走）；
    //   ② 路上不贴近"还没参战的怪"（避免路过引怪）。
    Map* map = bot->GetMap();
    if (!map)
        return false;

    if (!partypos::PathLooksWalkable(map, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                                     slot.x, slot.y, slot.z))
    {
        sLog.outDebug("[POSITION] path blocked by terrain (bot=%s) -> skip move", bot->GetName());
        return false;
    }

    std::list<Unit*> hostiles = partypos::ScanHostiles(bot, 40.0f);
    if (!hostiles.empty())
    {
        const int steps = 8;
        for (int i = 1; i <= steps; ++i)
        {
            float t = float(i) / float(steps);
            float x = bot->GetPositionX() + (slot.x - bot->GetPositionX()) * t;
            float y = bot->GetPositionY() + (slot.y - bot->GetPositionY()) * t;
            if (!partypos::SafeFromIdleHostiles(bot, x, y, hostiles))
            {
                sLog.outDebug("[POSITION] path too close to idle hostiles (bot=%s) -> skip move", bot->GetName());
                return false;
            }
        }
    }

    return MoveTo(bot->GetMapId(), slot.x, slot.y, slot.z);
}
