
#include "playerbot/playerbot.h"
#include "GuildBankAction.h"

#include "playerbot/strategy/values/ItemCountValue.h"
#include "playerbot/strategy/values/ItemUsageValue.h"
#include "Guilds/Guild.h"
#include "Guilds/GuildMgr.h"
#include "playerbot/strategy/values/GuildValues.h"

using namespace ai;

bool GuildBankAction::Execute(Event& event)
{
#ifndef MANGOSBOT_ZERO
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    std::string text = event.getParam();
    if (text.empty())
        return false;

    if (!bot->GetGuildId() || (requester && requester->GetGuildId() != bot->GetGuildId()))
    {
        ai->TellPlayer(requester,BOT_TEXT("I'm not in your guild!"));
            return false;
    }

    std::list<ObjectGuid> gos = *ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid> >("nearest game objects no los");
    for (std::list<ObjectGuid>::iterator i = gos.begin(); i != gos.end(); ++i)
    {
        GameObject* go = ai->GetGameObject(*i);
        if (!go || !bot->GetGameObjectIfCanInteractWith(go->GetObjectGuid(), GAMEOBJECT_TYPE_GUILD_BANK))
            continue;

        return Execute(text, go, requester);
    }

    ai->TellPlayer(requester, BOT_TEXT("error_gbank_found"));
    return false;
#else
    return false;
#endif
}

bool GuildBankAction::Execute(std::string text, GameObject* bank, Player* requester)
{
    bool result = true;

    IterateItemsMask mask = IterateItemsMask((uint8)IterateItemsMask::ITERATE_ITEMS_IN_EQUIP | (uint8)IterateItemsMask::ITERATE_ITEMS_IN_BAGS);

    std::list<Item*> found = ai->InventoryParseItems(text, mask);
    if (found.empty())
        return false;

    for (std::list<Item*>::iterator i = found.begin(); i != found.end(); i++)
    {
        Item* item = *i;
        if (item)
            result &= MoveFromCharToBank(item, bank, requester);
    }

    return result;
}

bool GuildBankAction::MoveFromCharToBank(Item* item, GameObject* bank, Player* requester)
{
#ifndef MANGOSBOT_ZERO
    uint32 playerSlot = item->GetSlot();
    uint32 playerBag = item->GetBagSlot();
    std::ostringstream out;

    Guild* guild = sGuildMgr.GetGuildById(bot->GetGuildId());
    //guild->SwapItems(bot, 0, playerSlot, 0, INVENTORY_SLOT_BAG_0, 0);

    // check source pos rights (item moved to bank)
    if (!guild->IsMemberHaveRights(bot->GetGUIDLow(), 0, GUILD_BANK_RIGHT_DEPOSIT_ITEM))
        out << BOT_TEXT("error_cant_put") << chat->formatItem(item) << BOT_TEXT("error_gbank_rights");
    else
    {
        out << chat->formatItem(item) << BOT_TEXT("gbank_put");
        guild->MoveFromCharToBank(bot, playerBag, playerSlot, 0, 255, 0);
    }

    ai->TellPlayer(requester, out);
    return true;
#else
    return false;
#endif
}

bool GuildBankAction::AutoDeposit(GameObject* bank)
{
#ifndef MANGOSBOT_ZERO
    if (!bot->GetGuildId())
        return false;

    Guild* guild = sGuildMgr.GetGuildById(bot->GetGuildId());
    if (!guild || guild->GetPurchasedTabs() == 0)
        return false;

    if (!guild->IsMemberHaveRights(bot->GetGUIDLow(), 0, GUILD_BANK_RIGHT_DEPOSIT_ITEM))
        return false;

    bool deposited = false;

    // Deposit surplus trade goods and reagents
    for (uint8 usageType : {(uint8)ItemUsage::ITEM_USAGE_VENDOR, (uint8)ItemUsage::ITEM_USAGE_AH})
    {
        std::list<Item*> items = AI_VALUE2(std::list<Item*>, "inventory items", "usage " + std::to_string(usageType));
        for (auto item : items)
        {
            if (!item)
                continue;

            ItemPrototype const* proto = item->GetProto();
            if (!proto)
                continue;

            // Only deposit trade goods and reagents
            if (proto->Class != ITEM_CLASS_TRADE_GOODS && proto->Class != ITEM_CLASS_REAGENT)
                continue;

            if (proto->Quality < ITEM_QUALITY_NORMAL)
                continue;

            if (proto->Bonding == BIND_WHEN_PICKED_UP)
                continue;

            // Don't deposit guild task items
            ItemQualifier qualifier(item);
            std::string qualStr = qualifier.GetQualifier();
            ItemUsage currentUsage = AI_VALUE2(ItemUsage, "item usage", qualStr);
            if (currentUsage == ItemUsage::ITEM_USAGE_GUILD_TASK)
                continue;

            uint32 playerSlot = item->GetSlot();
            uint32 playerBag = item->GetBagSlot();
            guild->MoveFromCharToBank(bot, playerBag, playerSlot, 0, 255, 0);
            deposited = true;
        }
    }

    return deposited;
#else
    return false;
#endif
}

bool GuildBankAction::AutoWithdraw(GameObject* bank)
{
#ifndef MANGOSBOT_ZERO
    if (!bot->GetGuildId())
        return false;

    if (AI_VALUE(uint8, "bag space") > 80)
        return false;

    Guild* guild = sGuildMgr.GetGuildById(bot->GetGuildId());
    if (!guild || guild->GetPurchasedTabs() == 0)
        return false;

    bool withdrew = false;

    for (uint8 tabId = 0; tabId < guild->GetPurchasedTabs(); ++tabId)
    {
        if (!guild->IsMemberHaveRights(bot->GetGUIDLow(), tabId, GUILD_BANK_RIGHT_VIEW_TAB))
            continue;

        for (uint8 slotId = 0; slotId < GUILD_BANK_MAX_SLOTS; ++slotId)
        {
            GuildAccess* guildAccess = static_cast<GuildAccess*>(guild);
            Item* item = guildAccess->GetGuildItem(tabId, slotId);

            if (!item)
                continue;

            ItemPrototype const* proto = item->GetProto();
            if (!proto)
                continue;

            bool shouldWithdraw = false;

            // Check if item is an equip upgrade
            if (proto->InventoryType != INVTYPE_NON_EQUIP && bot->CanUseItem(proto) == EQUIP_ERR_OK)
            {
                ItemQualifier qualifier(item);
                ItemUsage equipUsage = ItemUsageValue::QueryItemUsageForEquip(qualifier, bot);
                if (equipUsage == ItemUsage::ITEM_USAGE_EQUIP)
                    shouldWithdraw = true;
            }

            // Check if item is needed for crafting
            if (!shouldWithdraw && (proto->Class == ITEM_CLASS_TRADE_GOODS || proto->Class == ITEM_CLASS_REAGENT))
            {
                ItemQualifier qualifier(item);
                std::string qualStr = qualifier.GetQualifier();
                ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", qualStr);
                if (usage == ItemUsage::ITEM_USAGE_SKILL)
                    shouldWithdraw = true;
            }

            if (!shouldWithdraw)
                continue;

            guild->MoveFromBankToChar(bot, tabId, slotId, INVENTORY_SLOT_BAG_0, NULL_SLOT, 0);
            withdrew = true;

            if (AI_VALUE(uint8, "bag space") > 80)
                return withdrew;
        }
    }

    return withdrew;
#else
    return false;
#endif
}
