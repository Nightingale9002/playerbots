#pragma once
#include "playerbot/strategy/Action.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/PlayerbotFactory.h"

namespace ai
{
    class UpdateGearAction : public Action
    {
    public:
        UpdateGearAction(PlayerbotAI* ai);
        virtual bool Execute(Event& event) override;
        virtual bool isUseful() override;

    private:
        uint8 GetProgressionLevel(uint32 itemLevel);
        uint8 GetMasterAverageProgressionLevel();
        uint8 GetMasterItemProgressionLevel(uint8 slot, uint8 avgProgressionLevel);
        // [按副本换阶段 2026-10-14] 当前副本是否配置了固定阶段（无配置返回 NO_INSTANCE_PHASE）
        uint8 GetInstanceProgressionLevel();
       
        void EnchantItem(Item* item);

        // [成对槽位 2026-10-17] 成对槽位（戒指1/2、饰品1/2）配了同一件唯一/已装备物品时，
        // 复用 RandomItemMgr 的"按槽位+专精权重"取件接口另挑一件同 InventoryType 的替代品。
        // 挑不到返回 0（调用方跳过该格，不硬装）。
        uint32 PickAlternativeItem(uint8 slot, uint32 desiredItemId);

    private:
        std::vector<EnchantTemplate> enchants;
    };
}
