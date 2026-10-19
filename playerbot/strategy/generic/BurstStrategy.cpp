#include "playerbot/playerbot.h"
#include "playerbot/AiFactory.h"
#include "BurstStrategy.h"

using namespace ai;

namespace
{
    // 只挂**TBC 真实存在**的爆发/大 CD，并且按专精来选；没有爆发的职业（术士/牧师/野德等）不挂任何东西。
    //   ⚠️ 这里只放"已在各职业 AiObjectContext 注册过触发器+动作"的名字，避免死条目。
    void AddBurst(std::list<TriggerNode*>& triggers, char const* name, float priority)
    {
        triggers.push_back(new TriggerNode(
            name,
            NextAction::array(0, new NextAction(name, priority), NULL)));
    }
}

void BurstStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{
    Player* bot = ai->GetBot();
    if (!bot)
        return;

    int const tab = AiFactory::GetPlayerSpecTab(bot);
    const float p = ACTION_HIGH + 5;    // 高于普通输出，低于打断/驱散

    switch (bot->getClass())
    {
        case CLASS_MAGE:
            if (tab == 1)                        // 火焰
                AddBurst(triggers, "combustion", p + 2);
            else if (tab == 0)                   // 奥术
            {
                AddBurst(triggers, "arcane power", p + 2);
                AddBurst(triggers, "presence of mind", p + 1);
            }
            else                                 // 冰霜：TBC 没有冰冷血脉，只有急速冷却/水元素
                AddBurst(triggers, "cold snap", p + 1);
            break;

        case CLASS_ROGUE:
            if (tab == 0)                        // 刺杀
                AddBurst(triggers, "cold blood", p + 1);
            else if (tab == 1)                   // 战斗
            {
                AddBurst(triggers, "adrenaline rush", p + 2);
                AddBurst(triggers, "blade flurry", p + 1);
            }
            break;                               // 敏锐：TBC 没有可挂的 DPS 爆发

        case CLASS_WARRIOR:
            if (tab != 2)                        // 非防护才需要
            {
                AddBurst(triggers, "recklessness", p + 2);
                AddBurst(triggers, "death wish", p + 1);
            }
            break;

        case CLASS_HUNTER:
            AddBurst(triggers, "rapid fire", p + 1);
            break;

        case CLASS_SHAMAN:
            // 团队爆发（同一 CD 组，触发器自己判冷却/是否已用）
            AddBurst(triggers, "bloodlust", p + 3);
            AddBurst(triggers, "heroism", p + 3);
            break;

        case CLASS_PALADIN:
            if (tab == 2)                        // 惩戒
                AddBurst(triggers, "avenging wrath", p + 1);
            break;

        // 术士 / 牧师 / 德鲁伊：TBC 没有"DPS 爆发 CD"这一类，不挂（德鲁伊的树人只算召唤物，
        //   而且动作未注册，先不放进爆发策略）
        default:
            break;
    }
}
