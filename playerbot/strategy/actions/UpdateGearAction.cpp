
#include "playerbot/playerbot.h"
#include "UpdateGearAction.h"
#include "playerbot/RandomPlayerbotMgr.h"
#include "playerbot/RandomItemMgr.h"
#include "playerbot/AiFactory.h"

using namespace ai;

// [按副本换阶段 2026-10-14] 哨兵值：当前副本没有配置固定阶段
static const uint8 NO_INSTANCE_PHASE = 0xFF;

UpdateGearAction::UpdateGearAction(PlayerbotAI* ai): Action(ai, "update gear")
{
    enchants.clear();
}

bool UpdateGearAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();

    // [2026-10-22 站长口径] 开启"最好装备"配置 ⇒ 忽略装等/副本阶段限制：
    //   不走 gearProgressionSystemItems[phase] 那张表，直接按专精权重整套生成最好装备（再补宝石）。
    //   这个函数就是原来的"进本按副本阶段换装"入口。
    if (sPlayerbotAIConfig.equipBestGear && bot && bot->GetLevel() >= DEFAULT_MAX_LEVEL)
    {
        PlayerbotFactory factory(bot, bot->GetLevel());
        factory.EquipGearBest();
        uint32 const filledGems = PlayerbotFactory::FillGems(bot, true);
        bot->SaveToDB();
        sLog.outString("[GEAR] %s 进本换装 → 最佳装备（忽略装等限制，补宝石 %u）", bot->GetName(), filledGems);
        return true;
    }

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

    // [按副本换阶段 2026-10-14] 若当前副本配置了固定阶段，则整角色按该阶段换装（站长口径的"按副本难度"）
    const uint8 instanceProgressionLevel = GetInstanceProgressionLevel();
    const uint8 avgProgressionLevel = (instanceProgressionLevel != NO_INSTANCE_PHASE)
        ? instanceProgressionLevel : GetMasterAverageProgressionLevel();

    // Get gear for the progression level
    for (uint8 slot = 0; slot < SLOT_EMPTY; slot++)
    {
        // Get the master item phase for this slot（副本配置了阶段时，所有部位统一用该阶段）
        const uint8 itemProgressionLevel = (instanceProgressionLevel != NO_INSTANCE_PHASE)
            ? instanceProgressionLevel : GetMasterItemProgressionLevel(slot, avgProgressionLevel);

        // Retrieve the desired item from the progression levels
        const int32 itemId = sPlayerbotAIConfig.gearProgressionSystemItems[itemProgressionLevel][cls][spec][slot];
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
            }

            if (pItemId > 0)
            {
                uint16 eDest = 0;
                uint32 itemToEquip = pItemId;

                // [成对槽位 2026-10-17 站长口径] 成对槽位（Finger1/2、Trinket1/2）在
                // GearProgressionSystem 里常配同一件唯一饰品/戒指 ⇒ 第二格必然装备失败
                // （EQUIP_ERR_ITEM_UNIQUE_EQUIPPABLE / 该物品已装备），旧代码就刷一条
                // "Failed to equip item ..." 并**先毁掉旧件**、把槽位留空。
                // 现在：失败时按专精权重在同槽位、同 InventoryType 的候选里另挑一件；
                // 挑不到就跳过该格（不硬装、不毁旧件）。
                if (RandomPlayerbotMgr::CanEquipUnseenItem(bot, slot, eDest, pItemId) != EQUIP_ERR_OK)
                {
                    uint32 altItemId = PickAlternativeItem(slot, pItemId);
                    if (altItemId && oldItem && altItemId == oldItem->GetEntry())
                    {
                        // 旧件本身就是合格替代品（它没被别的格子占用）⇒ 保持不动
                        sLog.outDetail("[GEAR] %s slot %u: item %u 已被别的格子占用，沿用旧件 %u",
                            bot->GetName(), uint32(slot), pItemId, altItemId);
                        continue;
                    }

                    if (!altItemId || RandomPlayerbotMgr::CanEquipUnseenItem(bot, slot, eDest, altItemId) != EQUIP_ERR_OK)
                    {
                        // 降为 detail：不再刷玩家频道，也不再刷 ERROR 级日志
                        sLog.outDetail("[GEAR] Failed to equip item %u specified in AiPlayerbot.GearProgressionSystem.%u.%u.%u.%u (slot skipped, no alternative)",
                            pItemId, uint32(itemProgressionLevel), uint32(cls), uint32(spec), uint32(slot));
                        continue;
                    }

                    sLog.outDetail("[GEAR] %s slot %u: item %u 与已装备件冲突，按专精权重改用替代品 %u",
                        bot->GetName(), uint32(slot), pItemId, altItemId);
                    itemToEquip = altItemId;
                }

                // Destroy the old item（替换品已经定下来了才毁旧件）
                if (oldItem)
                {
                    bot->DestroyItem(oldItem->GetBagSlot(), oldItem->GetSlot(), true);
                }

                // Try to equip and enchant the item
                Item* pItem = bot->EquipNewItem(eDest, itemToEquip, true);
                if (pItem)
                {
                    pItem->SetOwnerGuid(bot->GetObjectGuid());
                    EnchantItem(pItem);
                }
            }
        }
        else
        {
            std::ostringstream ss;
            ss << "Missing configuration for AiPlayerbot.GearProgressionSystem." << std::to_string(itemProgressionLevel) << "." << std::to_string(cls) << "." << std::to_string(spec) << "." << std::to_string(slot);
            ai->TellError(requester, ss.str());
        }

    }

    // [gems 2026-10-17 站长口径] 换完装备（含上面 PickAlternativeItem 挑的替代品、进本按阶段整套换装）
    // 立刻补插空槽宝石：只补空槽（onlyEmpty=true）⇒ 幂等，已插满/插对的物品由 FillGems 内部跳过，不拆已有宝石。
    // 新装备的插槽本来是空的，这一步就是"每次换装备后再补插一次"。
    uint32 const filledGems = PlayerbotFactory::FillGems(bot, true);
    if (filledGems)
        sLog.outString("[GEMS] %s refilled %u gems", bot->GetName(), filledGems);

    return true;
}

bool UpdateGearAction::isUseful()
{
    // [2026-10-22 站长口径] 最好装备模式：在副本/团本里就允许换（不看阶段配置，也不看装等上限）
    if (sPlayerbotAIConfig.equipBestGear && bot->GetLevel() >= DEFAULT_MAX_LEVEL)
    {
        Map* map = bot->FindMap();
        if (map && (map->IsDungeon() || map->IsRaid()))
            return true;
    }

    if (sPlayerbotAIConfig.gearProgressionSystemEnabled)
    {
        // [进本换装 2026-10-15] 只要身处"配置了阶段的副本"里，就允许换装（不必等真人组队）。
        // Execute() 会跳过已经穿对的部位 ⇒ 等效于"进本时触发一次阶段换装"。
        if (GetInstanceProgressionLevel() != NO_INSTANCE_PHASE)
            return bot->GetLevel() >= DEFAULT_MAX_LEVEL;

        // Only for max level random bots that are playing with a real player
        Player* master = GetMaster();
        if (master && master->isRealPlayer() && bot->IsInGroup(master) && sRandomPlayerbotMgr.IsRandomBot(bot))
        {
            return bot->GetLevel() >= DEFAULT_MAX_LEVEL;
        }
    }

    return false;
}

uint8 UpdateGearAction::GetInstanceProgressionLevel()
{
    Map* map = bot ? bot->GetMap() : nullptr;
    if (!map || !(map->IsDungeon() || map->IsRaid()))
        return NO_INSTANCE_PHASE;

    auto itr = sPlayerbotAIConfig.gearProgressionInstancePhases.find(map->GetId());
    if (itr == sPlayerbotAIConfig.gearProgressionInstancePhases.end())
        return NO_INSTANCE_PHASE;

    return itr->second;
}

uint8 UpdateGearAction::GetProgressionLevel(uint32 itemLevel)
{
    uint8 itemProgressionLevel = 0;
    for (uint8 progressionLevel = 1; progressionLevel < MAX_GEAR_PROGRESSION_LEVEL; progressionLevel++)
    {
        const uint32 minIlvl = sPlayerbotAIConfig.gearProgressionSystemItemLevels[progressionLevel][0];
        const uint32 maxIlvl = sPlayerbotAIConfig.gearProgressionSystemItemLevels[progressionLevel][1];
        if (itemLevel >= minIlvl && itemLevel <= maxIlvl)
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

// [成对槽位 2026-10-17 站长口径] 成对槽位（Finger1/2、Trinket1/2）配了同一件唯一物品时，
// 第二格必然装不上。这里复用 RandomItemMgr 现成的"按槽位 + 专精权重"接口另挑一件：
//   · sRandomItemMgr.Query(等级, 职业, 专精, 部位, 品质)  ← 该槽位的候选（按 viableSlots 过滤过 InventoryType）
//   · sRandomItemMgr.GetLiveStatWeight(bot, itemId, specId) ← 专精权重（与 PlayerbotFactory::InitEquipment 同一口径）
// 约束：同 InventoryType；排除已被别的装备格占用/包里已达 MaxCount/唯一上限的 entry；
//       同品质优先，其次专精权重，再次物品等级。挑不到返回 0（调用方跳过该格，不硬装）。
uint32 UpdateGearAction::PickAlternativeItem(uint8 slot, uint32 desiredItemId)
{
    if (!bot)
        return 0;

    ItemPrototype const* desiredProto = sObjectMgr.GetItemPrototype(desiredItemId);
    if (!desiredProto)
        return 0;

    uint32 specId = sRandomItemMgr.GetPlayerSpecId(bot);
    if (!specId)
        specId = sRandomItemMgr.GetDefaultSpecIdForClass(bot->getClass());
    if (!specId)
        return 0;

    // 别的装备格已经占用的 entry（本格旧件不排除：它本来就是合格候选）
    std::set<uint32> taken;
    for (uint8 s = EQUIPMENT_SLOT_START; s < EQUIPMENT_SLOT_END; ++s)
    {
        if (s == slot)
            continue;

        Item* other = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, s);
        if (other)
            taken.insert(other->GetEntry());
    }

    std::vector<uint32> ids;
    for (uint32 q = ITEM_QUALITY_POOR; q <= ITEM_QUALITY_ARTIFACT; ++q)
    {
        RandomItemList list = sRandomItemMgr.Query(bot->GetLevel(), bot->getClass(), uint8(specId), slot, q);
        for (RandomItemList::const_iterator itr = list.begin(); itr != list.end(); ++itr)
        {
            if (std::find(ids.begin(), ids.end(), *itr) == ids.end())
                ids.push_back(*itr);
        }
    }

    uint32 best = 0;
    uint32 bestWeight = 0;
    uint32 bestItemLevel = 0;
    uint32 bestSameQuality = 0;

    for (std::vector<uint32>::const_iterator itr = ids.begin(); itr != ids.end(); ++itr)
    {
        const uint32 candidate = *itr;

        if (candidate == desiredItemId || taken.find(candidate) != taken.end())
            continue;

        ItemPrototype const* proto = sObjectMgr.GetItemPrototype(candidate);
        if (!proto)
            continue;

        if (proto->InventoryType != desiredProto->InventoryType)
            continue;

        if ((proto->Flags & ITEM_FLAG_UNIQUE_EQUIPPABLE) && bot->HasItemCount(candidate, 1))
            continue;

        if (proto->MaxCount && bot->HasItemCount(candidate, proto->MaxCount))
            continue;

        const uint32 sameQuality = (proto->Quality == desiredProto->Quality) ? 1 : 0;
        const uint32 weight = sRandomItemMgr.GetLiveStatWeight(bot, candidate, specId);

        bool better = false;
        if (!best)
            better = true;
        else if (sameQuality != bestSameQuality)
            better = (sameQuality > bestSameQuality);
        else
            better = (weight > bestWeight) || (weight == bestWeight && proto->ItemLevel > bestItemLevel);

        if (better)
        {
            best = candidate;
            bestWeight = weight;
            bestItemLevel = proto->ItemLevel;
            bestSameQuality = sameQuality;
        }
    }

    return best;
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