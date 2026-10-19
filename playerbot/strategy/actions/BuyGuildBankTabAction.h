#pragma once
#include "GenericActions.h"

namespace ai
{
#ifndef MANGOSBOT_ZERO
    class BuyGuildBankTabAction : public Action
    {
    public:
        BuyGuildBankTabAction(PlayerbotAI* ai) : Action(ai, "buy guild bank tab") {}
        virtual bool Execute(Event& event) override;
        virtual bool isPossible() override;
        virtual bool isUseful() override;
    };
#endif
}
