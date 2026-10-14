
#include "playerbot/playerbot.h"
#include "UpdateGearAction.h"
#include "playerbot/RandomPlayerbotMgr.h"
#include "playerbot/AiFactory.h"

#include <set>          // [GEARPROG-EMPTY-SLOT 2026-10-15] 用于"同一个缺失组合只提示一次"

using namespace ai;

UpdateGearAction::UpdateGearAction(PlayerbotAI* ai): Action(ai, "update gear")
{
    enchants.clear();
}

bool UpdateGearAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();

    // Get bot class and spec
    const uint8 cls = bot->getClass();
    uint8 spec = AiFactory::GetPlayerSpecTab(bot);

    // Spec is overridden by bot role
    if (cls == CLASS_WARRIOR)
    {
        if (ai->IsTank(bot))
        {
            spec = 2;
        }
    }
    else if (cls == CLASS_PALADIN)
    {
        if (ai->IsHeal(bot))
        {
            spec = 0;
        }
        else if (ai->IsTank(bot))
        {
            spec = 1;
        }
    }
    else if (cls == CLASS_PRIEST)
    {
        if (ai->IsHeal(bot))
        {
            spec = 1;
        }
    }
    else if (cls == CLASS_SHAMAN)
    {
        if (ai->IsHeal(bot))
        {
            spec = 2;
        }
    }
    else if (cls == CLASS_DRUID)
    {
        if (ai->IsHeal(bot))
        {
            spec = 2;
        }
        else if (ai->IsTank(bot))
        {
            spec = 3;
        }
    }

    const uint8 avgProgressionLevel = GetMasterAverageProgressionLevel();

    // Get gear for the progression level
    for (uint8 slot = 0; slot < SLOT_EMPTY; slot++)
    {
        // Get the master item phase for this slot
        const uint8 itemProgressionLevel = GetMasterItemProgressionLevel(slot, avgProgressionLevel);

        // Retrieve the desired item from the progression levels
        const int32 itemId = sPlayerbotAIConfig.gearProgressionSystemItems[itemProgressionLevel][cls][spec][slot];

        // [GEARPROG-EMPTY-SLOT 2026-10-15] 约定：itemId <= 0 表示"这个槽位按阶段不配装备"，
        // 不是物品编号。实测 aiplayerbot.conf 里 0.9.0.3 = 0（槽位 3 = EQUIPMENT_SLOT_BODY 衬衫，
        // 装饰件）与 0.9.0.16 = 0（槽位 16 = EQUIPMENT_SLOT_OFFHAND，术士用法杖时为空）。
        // 旧代码用 `itemId >= 0` 判断，于是 0 被当成物品 entry 去 equip：
        //   CanEquipUnseenItem(bot, slot, eDest, 0) 找不到物品原型 ⇒ 失败 ⇒
        //   TellError("Failed to equip item 0 specified in AiPlayerbot.GearProgressionSystem.0.9.0.3")
        // 这就是聊天框里那一串告警的来源（并非"缺配置"）。
        // 现在：>0 才装备；<=0 只清理旧件；== -1（矩阵没这一项）才提示"缺配置"，且每个组合只提示一次。
        if (itemId >= 0)
        {
            const uint32 pItemId = (uint32)itemId;
            Item* oldItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (oldItem)
            {
                // Ignore this item as it is already equipped
                if (oldItem->GetEntry() == pItemId)
                {
                    continue;
                }

                // Destroy the old item
                bot->DestroyItem(oldItem->GetBagSlot(), oldItem->GetSlot(), true);
            }

            if (pItemId > 0)
            {
                // Try to equip and enchant the item
                uint16 eDest;
                if (RandomPlayerbotMgr::CanEquipUnseenItem(bot, slot, eDest, pItemId) == EQUIP_ERR_OK)
                {
                    Item* pItem = bot->EquipNewItem(eDest, pItemId, true);
                    if (pItem)
                    {
                        pItem->SetOwnerGuid(bot->GetObjectGuid());
                        EnchantItem(pItem);
                    }
                }
                else
                {
                    std::ostringstream ss;
                    ss << "Failed to equip item " << std::to_string(pItemId) << " specified in AiPlayerbot.GearProgressionSystem." << std::to_string(itemProgressionLevel) << "." << std::to_string(cls) << "." << std::to_string(spec) << "." << std::to_string(slot);
                    ai->TellError(requester, ss.str());
                }
            }
            // pItemId == 0：该槽位按阶段留空，旧件已清理，无需装备也不再告警。
        }
        else
        {
            // -1（矩阵里没有这一项）才提示"缺配置"；同一个 phase/class/spec/slot 只提示一次，
            // 避免每个 bot 每次换装都刷同一串告警（原实现会让聊天框刷满）。
            static std::set<uint32> reportedMissing;
            uint32 const key = ((((uint32)itemProgressionLevel * MAX_CLASSES + (uint32)cls) * 4 + (uint32)spec) * SLOT_EMPTY) + (uint32)slot;
            if (reportedMissing.insert(key).second)
            {
                std::ostringstream ss;
                ss << "Missing configuration for AiPlayerbot.GearProgressionSystem." << std::to_string(itemProgressionLevel) << "." << std::to_string(cls) << "." << std::to_string(spec) << "." << std::to_string(slot);
                ai->TellError(requester, ss.str());
            }
        }

    }

    return true;
}

bool UpdateGearAction::isUseful()
{
    if (sPlayerbotAIConfig.gearProgressionSystemEnabled)
    {
        // Only for max level random bots that are playing with a real player
        Player* master = GetMaster();
        if (master && master->isRealPlayer() && bot->IsInGroup(master) && sRandomPlayerbotMgr.IsRandomBot(bot))
        {
            return bot->GetLevel() >= DEFAULT_MAX_LEVEL;
        }
    }

    return false;
}

uint8 UpdateGearAction::GetProgressionLevel(uint32 itemLevel)
{
    uint8 itemProgressionLevel = 0;
    for (uint8 progressionLevel = 1; progressionLevel < MAX_GEAR_PROGRESSION_LEVEL; progressionLevel++)
    {
        const uint32 minIlvl = sPlayerbotAIConfig.gearProgressionSystemItemLevels[progressionLevel][0];
        const uint32 maxIlvl = sPlayerbotAIConfig.gearProgressionSystemItemLevels[progressionLevel][1];
        if (itemLevel >= minIlvl && itemLevel < maxIlvl)
        {
            itemProgressionLevel = progressionLevel;
            break;
        }
    }

    return itemProgressionLevel;
}

uint8 UpdateGearAction::GetMasterAverageProgressionLevel()
{
    return GetProgressionLevel(ai->GetEquipGearScore(ai->GetMaster(), false, false));
}

uint8 UpdateGearAction::GetMasterItemProgressionLevel(uint8 slot, uint8 avgProgressionLevel)
{
    // If checking weapons or ranged weapons use the average item lvl
    if (slot == EQUIPMENT_SLOT_MAINHAND ||
        slot == EQUIPMENT_SLOT_OFFHAND || 
        slot == EQUIPMENT_SLOT_RANGED || 
        slot == EQUIPMENT_SLOT_TRINKET1 ||
        slot == EQUIPMENT_SLOT_TRINKET2 ||
        slot == EQUIPMENT_SLOT_BODY ||
        slot == EQUIPMENT_SLOT_TABARD)
    {
        return avgProgressionLevel;
    }
    else
    {
        uint32 itemLevel = 0;
        const Item* item = GetMaster()->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (item)
        {
            itemLevel = item->GetProto()->ItemLevel;
        }

        uint8 itemProgressionLevel = GetProgressionLevel(itemLevel);

        /*
        // TO DO: Check if a player has a item with low item lvl in purpose even if he is on a different progression level
        // Use average progression level if the item progression level is lower than the average progression level
        if (itemProgressionLevel < averageProgressionLevel)
        {
            itemProgressionLevel = averageProgressionLevel;
        }
        */

        return itemProgressionLevel;
    }
}

void UpdateGearAction::EnchantItem(Item* item)
{
    if (item)
    {
        int tab = AiFactory::GetPlayerSpecTab(bot);
        uint32 tempId = uint32((uint32)bot->getClass() * (uint32)10);
        uint8 spec = tempId += (uint32)tab;

        if (enchants.empty())
        {
            auto result = WorldDatabase.PQuery("SELECT class, spec, spellid, slotid FROM ai_playerbot_enchants");
            if (result)
            {
                do
                {
                    Field* fields = result->Fetch();

                    EnchantTemplate pEnchant;
                    pEnchant.ClassId = fields[0].GetUInt8();
                    pEnchant.SpecId = fields[1].GetUInt8();
                    pEnchant.SpellId = fields[2].GetUInt32();
                    pEnchant.SlotId = fields[3].GetUInt8();
                    enchants.push_back(pEnchant);
                } 
                while (result->NextRow());
            }
        }

        for (const auto& enchant : enchants)
        {
            if (enchant.ClassId == bot->getClass() && enchant.SpecId == spec)
            {
                ai->EnchantItemT(enchant.SpellId, enchant.SlotId, item);
            }
        }
    }
}