#pragma once

#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/RandomPlayerbotMgr.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/strategy/Action.h"
#include "playerbot/strategy/NamedObjectContext.h"

namespace ai
{
    // [rules 2026-10-17] `delay` —— 让 bot 原地不动一会儿。
    //
    // 以前它只服务"机器人被暂停活动"这一种场景：duration 硬编码 `passiveDelay + globalCoolDown`，
    // 且 `isUseful()` 要求 `!AllowActivity(ALL_ACTIVITY)` ⇒ **规则行里写不了"等 N 秒"**，
    // 于是"被击退/被拉过去之后先别急着回去"这类攻略要求无法用数据表达。
    //
    // 现在支持限定词：`delay::<秒数>` 或 `delay::<秒数>:<毫秒>`（例 `delay::3` / `delay::1:500`）。
    //   · 有合法秒数 ⇒ 在任何时候都可用（规则行自己负责触发条件）；
    //   · 没有限定词 ⇒ 保持旧行为（只在活动被暂停时用被动延迟）。
    class DelayAction : public Action, public Qualified
    {
    public:
        DelayAction(PlayerbotAI* ai) : Action(ai, "delay"), Qualified() {}

        // 解析结果：seconds >= 0 表示规则行给了秒数；-1 表示没给（走旧行为）
        int ParseSeconds() const
        {
            const std::string& q = getQualifier();
            if (q.empty())
                return -1;

            // 允许 "3"、"1:500"（秒:毫秒）
            std::vector<std::string> parts = Qualified::getMultiQualifiers(q, ":", "");
            if (parts.empty() || parts[0].empty())
                return -1;

            for (char c : parts[0])
                if (!std::isdigit((unsigned char)c))
                    return -1;

            int seconds = atoi(parts[0].c_str());
            if (seconds <= 0)
                return -1;

            // 上界：避免规则行写出"等一小时"把 bot 卡死
            if (seconds > 30)
                seconds = 30;

            return seconds;
        }

        int ParseMilliseconds() const
        {
            const std::string& q = getQualifier();
            size_t pos = q.find(':');
            if (pos == std::string::npos)
                return 0;

            std::string ms = q.substr(pos + 1);
            if (ms.empty())
                return 0;

            for (char c : ms)
                if (!std::isdigit((unsigned char)c))
                    return 0;

            int value = atoi(ms.c_str());
            return (value < 0 || value >= 1000) ? 0 : value;
        }

        virtual bool Execute(Event& event) override
        {
            int seconds = ParseSeconds();
            if (seconds > 0)
            {
                SetDuration((uint32)(seconds * 1000 + ParseMilliseconds()));
                return true;
            }

            SetDuration(sPlayerbotAIConfig.passiveDelay + sPlayerbotAIConfig.globalCoolDown);
            return true;
        }

        virtual bool isUseful() override
        {
            // 规则行给了秒数 ⇒ 由规则行决定何时用
            if (ParseSeconds() > 0)
                return true;

            return !ai->AllowActivity(ALL_ACTIVITY);
        }
    };
}
