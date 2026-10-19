#pragma once
#include "playerbot/strategy/Value.h"
#include "playerbot/PlayerbotAIConfig.h"

namespace ai
{
    class WaitForAttackTimeValue : public ManualSetValue<uint8>, public Qualified
	{
	public:
        // [threat 2026-10-16 站长口径] 默认值改成配置项 AiPlayerbot.WaitForAttackTime（秒）：
        // "等坦克 pull back" 的时长由站长调，不用改代码。仍可用游戏内 "wait for attack time <秒>" 单 bot 覆盖。
        WaitForAttackTimeValue(PlayerbotAI* ai) : ManualSetValue<uint8>(ai, sPlayerbotAIConfig.waitForAttackTime), Qualified() {}
        virtual std::string Save() override { return std::to_string(value); }
        virtual bool Load(std::string inValue) override { value = stoi(inValue); return true; }
    };
}
