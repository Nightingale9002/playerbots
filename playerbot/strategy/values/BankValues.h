#pragma once
#include "playerbot/strategy/Value.h"
#include "ItemUsageValue.h"

namespace ai
{
    class ShouldBankDepositValue : public BoolCalculatedValue
    {
    public:
        ShouldBankDepositValue(PlayerbotAI* ai) : BoolCalculatedValue(ai, "should bank deposit", 5) {}
        virtual bool Calculate() override;
    };

    class ShouldBankWithdrawValue : public BoolCalculatedValue
    {
    public:
        ShouldBankWithdrawValue(PlayerbotAI* ai) : BoolCalculatedValue(ai, "should bank withdraw", 5) {}
        virtual bool Calculate() override;
    };

#ifndef MANGOSBOT_ZERO
    class ShouldGuildBankDepositValue : public BoolCalculatedValue
    {
    public:
        ShouldGuildBankDepositValue(PlayerbotAI* ai) : BoolCalculatedValue(ai, "should guild bank deposit", 5) {}
        virtual bool Calculate() override;
    };

    class ShouldGuildBankWithdrawValue : public BoolCalculatedValue
    {
    public:
        ShouldGuildBankWithdrawValue(PlayerbotAI* ai) : BoolCalculatedValue(ai, "should guild bank withdraw", 5) {}
        virtual bool Calculate() override;
    };
#endif
}
