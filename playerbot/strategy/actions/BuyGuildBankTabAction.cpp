
#include "playerbot/playerbot.h"
#include "BuyGuildBankTabAction.h"
#include "Guilds/Guild.h"
#include "Guilds/GuildMgr.h"

using namespace ai;

#ifndef MANGOSBOT_ZERO
bool BuyGuildBankTabAction::isPossible()
{
    return bot->GetGuildId() != 0;
}

bool BuyGuildBankTabAction::isUseful()
{
    if (!bot->GetGuildId())
        return false;

    Guild* guild = sGuildMgr.GetGuildById(bot->GetGuildId());
    if (!guild)
        return false;

    // Only guild leader should buy tabs
    if (guild->GetLeaderGuid() != bot->GetObjectGuid())
        return false;

    // Only buy the first tab
    if (guild->GetPurchasedTabs() > 0)
        return false;

    uint32 tabCost = GetGuildBankTabPrice(0) * GOLD;
    if (bot->GetMoney() < tabCost)
        return false;

    return true;
}

bool BuyGuildBankTabAction::Execute(Event& event)
{
    if (!bot->GetGuildId())
        return false;

    Guild* guild = sGuildMgr.GetGuildById(bot->GetGuildId());
    if (!guild)
        return false;

    if (guild->GetLeaderGuid() != bot->GetObjectGuid())
        return false;

    uint8 nextTab = guild->GetPurchasedTabs();
    if (nextTab >= GUILD_BANK_MAX_TABS)
        return false;

    uint32 tabCost = GetGuildBankTabPrice(nextTab) * GOLD;
    if (bot->GetMoney() < tabCost)
        return false;

    // Buy the tab
    guild->CreateNewBankTab();
    bot->ModifyMoney(-int32(tabCost));

    // Set permissions for all ranks
    uint32 rankCount = guild->GetRanksSize();
    for (uint32 rankId = 0; rankId < rankCount; ++rankId)
    {
        if (rankId == GR_GUILDMASTER)
        {
            // Guild master gets full rights
            guild->SetBankRightsAndSlots(rankId, nextTab, GUILD_BANK_RIGHT_FULL, WITHDRAW_SLOT_UNLIMITED, true);
        }
        else if (rankId == 1) // Officer rank
        {
            guild->SetBankRightsAndSlots(rankId, nextTab, GUILD_BANK_RIGHT_FULL, WITHDRAW_SLOT_UNLIMITED, true);
        }
        else
        {
            // Regular members get deposit rights with limited daily withdrawals
            uint32 dailySlots = (rankId <= 2) ? 10 : 5;
            guild->SetBankRightsAndSlots(rankId, nextTab, GUILD_BANK_RIGHT_DEPOSIT_ITEM, dailySlots, true);
        }
    }

    return true;
}
#endif
