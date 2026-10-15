
#include "playerbot/playerbot.h"
#include "EmoteStrategy.h"
#include "playerbot/PlayerbotAIConfig.h"

using namespace ai;

void EmoteStrategy::InitNonCombatTriggers(std::list<TriggerNode*> &triggers)
{
    // [emote 2026-10-18 站长口径] "表情太多，适当减少"：
    //   原来主动表情走 "random"（1/20，每 5 秒掷一次 ⇒ 12 只 bot 平均 8 秒就有人做个动作/说句话），
    //   suggest 类走 "often"（1/5，等于一直刷）。这里统一降频：
    //     主动表情  random(1/20) → rare(1/100)      ⇒ 约 1/5
    //     闲聊 talk seldom(1/300) → very rare(1/600) ⇒ 约 1/2
    //     suggest   often(1/5)   → seldom(1/300)     ⇒ 约 1/60
    //     mount anim random      → seldom
    //   回应真人表情（receive text emote / receive emote）保持原样，不降。
    triggers.push_back(new TriggerNode(
        "rare",
        NextAction::array(0, new NextAction("emote", 0.5f), NULL)));

    triggers.push_back(new TriggerNode(
        "seldom",
        NextAction::array(0, new NextAction("suggest what to do", 1.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "seldom",
        NextAction::array(0, new NextAction("suggest trade", 1.0f), NULL)));

    if (sPlayerbotAIConfig.enableGreet)
    {
        triggers.push_back(new TriggerNode(
            "new player nearby",
            NextAction::array(0, new NextAction("greet", 1.0f), NULL)));
    }

    triggers.push_back(new TriggerNode(
        "very rare",
        NextAction::array(0, new NextAction("talk", 0.5f), NULL)));

    triggers.push_back(new TriggerNode(
        "receive text emote",
        NextAction::array(0, new NextAction("emote", 10.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "receive emote",
        NextAction::array(0, new NextAction("emote", 10.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "seldom",
        NextAction::array(0, new NextAction("mount anim", 1.0f), NULL)));
}
