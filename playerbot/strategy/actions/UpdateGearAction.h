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

    private:
        std::vector<EnchantTemplate> enchants;
    };
}
