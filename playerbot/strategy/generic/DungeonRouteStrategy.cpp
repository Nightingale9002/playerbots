
#include "playerbot/playerbot.h"
#include "playerbot/DungeonRoute.h"
#include "DungeonRouteStrategy.h"

using namespace ai;

// [2026-10-25 站长口径] 行为全部落到**策略 + 优先级**上（数字越大越先执行）：
//   战斗：坦克救场 100 > 怪没聚齐/仇恨不够 DPS 停手 95 > 抓 OT 90 > 站位 50 > 群拉 45
//   非战斗：开怪 40 > 走位 30
void DungeonRouteStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{
    // 到位 ⇒ 起手开怪
    triggers.push_back(new TriggerNode(
        "dungeon route pull",
        NextAction::array(0, new NextAction("dungeon route pull", 40.0f), NULL)));

    // 朝下一段（stage/boss 的 mmap 路径）走
    triggers.push_back(new TriggerNode(
        "dungeon route move",
        NextAction::array(0, new NextAction("dungeon route move", 30.0f), NULL)));
}


void DungeonRouteStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{
    // ① 坦克是队伍核心：任意队友被敌对怪攻击 ⇒ 立刻救场（不管有没有目标/正在打谁/在走哪条路）
    triggers.push_back(new TriggerNode(
        "dungeon tank engage",
        NextAction::array(0, new NextAction("dungeon tank engage", 100.0f), NULL)));

    // ② 怪必须聚到一起才能开始打：坦克仇恨不够（含 0）或还有怪没聚到 5 码内 ⇒ DPS/法系停手
    triggers.push_back(new TriggerNode(
        "dungeon dps hold fire",
        NextAction::array(0, new NextAction("dungeon dps hold fire", 95.0f), NULL)));

    // ②.5 控制模式：有 CC 的职业控一只高威胁怪（每人一只、不重复，排除 priority 5）
    //   必须**高于**停手门（95）：否则停手先把这一 tick 吃掉，永远轮不到控。
    triggers.push_back(new TriggerNode(
        "dungeon cc cast",
        NextAction::array(0, new NextAction("dungeon cc cast", 97.0f), NULL)));

    // ③ 抓 OT（威胁差额最大的那只 ⇒ 嘲讽）
    triggers.push_back(new TriggerNode(
        "dungeon tank ot",
        NextAction::array(0, new NextAction("dungeon tank taunt", 90.0f), NULL)));

    // ④ 站位安全（别踩未进战斗怪的仇恨圈）
    triggers.push_back(new TriggerNode(
        "dungeon tank reposition",
        NextAction::array(0, new NextAction("dungeon tank reposition", 50.0f), NULL)));

    // ⑤ 群拉：>=3 只未被控制的怪在战斗 ⇒ 复用 aoe 策略
    //   [2026-10-25 站长口径] 群拉（群体仇恨）**不占走位**：瞬发技能，放完下一 tick 继续移动。
    //   优先级提到 85（救场 100 > 停手/控制 95~97 > 群拉 85 > 站位 50 > 推进 30）。
    triggers.push_back(new TriggerNode(
        "dungeon tank aoe",
        NextAction::array(0, new NextAction("dungeon tank aoe", 85.0f), NULL)));

    // ⑥ 低蓝过渡（不能让治疗/坦克停手）
    triggers.push_back(new TriggerNode(
        "low mana",
        NextAction::array(0, new NextAction("dungeon mana sustain", 40.0f), NULL)));
}
