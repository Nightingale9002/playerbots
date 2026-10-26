#pragma once
#include "MovementActions.h"

namespace ai
{
    // [副本推进 2026-10-18 站长口径] 运行期按波推进（只对 verified=1 的图生效）：
    //   驱动坦克先走到本波的"拉怪位"（见 DungeonRoute.h），到位后由 DungeonRoutePullAction 起手。
    class DungeonRouteMoveAction : public MovementAction
    {
    public:
        DungeonRouteMoveAction(PlayerbotAI* ai, std::string name = "dungeon route move") : MovementAction(ai, name) {}
        bool Execute(Event& event) override;
        bool isPossible() override;

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "dungeon route move"; }
        virtual std::string GetHelpDescription()
        {
            return "Moves the driving tank to the pull position of the current wave in a verified dungeon route.";
        }
        virtual std::vector<std::string> GetUsedActions() { return {}; }
        virtual std::vector<std::string> GetUsedValues() { return {}; }
#endif
    };

    //   站在拉怪位后，直接复用现有 pull 链路（"pull my target"：负责 posMap/OnCombatStarted）
    //   对"当前波最近的怪 / boss 阶段最近的 boss"起手。
    // [2026-10-24 站长口径] 漏怪补拉：**绑定策略**的主动作，不再由仿真强制 Attack。
    //   触发条件（见 DungeonLooseMobTrigger）：战斗中有怪选了队友、而自己没有目标。
    // [2026-10-24 站长口径] 低蓝分阶段：先换省蓝手段（骑士智慧审判/术士生命分流），
    //   只有**战斗中且极低蓝**才喝蓝药水；不在战斗时交给喝水策略。
    class DungeonManaSustainAction : public Action
    {
    public:
        DungeonManaSustainAction(PlayerbotAI* ai, std::string name = "dungeon mana sustain") : Action(ai, name) {}
        bool Execute(Event& event) override;

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "dungeon mana sustain"; }
        virtual std::string GetHelpDescription()
        {
            return "Staged mana sustain in combat: class mana-efficient ability first, potion only when critical.";
        }
        virtual std::vector<std::string> GetUsedActions() { return {}; }
        virtual std::vector<std::string> GetUsedValues() { return {}; }
#endif
    };

    // [2026-10-24 站长口径] 坦克"抓 OT"：按仇恨缺口挑一只，用嘲讽/正义防御/高仇恨技能拉回来。
    class DungeonTankTauntAction : public Action
    {
    public:
        DungeonTankTauntAction(PlayerbotAI* ai, std::string name = "dungeon tank taunt") : Action(ai, name) {}
        bool Execute(Event& event) override;
#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "dungeon tank taunt"; }
        virtual std::vector<std::string> GetUsedActions() { return {}; }
        virtual std::vector<std::string> GetUsedValues() { return {}; }
#endif
    };

    // [2026-10-24 站长口径] 群拉：怪多（>=3 只未被控制的怪在战斗）⇒ 复用模块自带的 aoe 策略
    // [2026-10-24 站长口径] 战斗中调整站位：挪到一个"不踩未战斗怪仇恨圈"的点（面朝当前目标）
    class DungeonTankRepositionAction : public Action
    {
    public:
        DungeonTankRepositionAction(PlayerbotAI* ai, std::string name = "dungeon tank reposition") : Action(ai, name) {}
        bool Execute(Event& event) override;
#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "dungeon tank reposition"; }
        virtual std::vector<std::string> GetUsedActions() { return {}; }
        virtual std::vector<std::string> GetUsedValues() { return {}; }
#endif
    };

    class DungeonTankAoeAction : public Action
    {
    public:
        DungeonTankAoeAction(PlayerbotAI* ai, std::string name = "dungeon tank aoe") : Action(ai, name) {}
        bool Execute(Event& event) override;
#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "dungeon tank aoe"; }
        virtual std::vector<std::string> GetUsedActions() { return {}; }
        virtual std::vector<std::string> GetUsedValues() { return {}; }
#endif
    };

    class DungeonAssistLooseAction : public Action
    {
    public:
        DungeonAssistLooseAction(PlayerbotAI* ai, std::string name = "dungeon assist loose") : Action(ai, name) {}
        bool Execute(Event& event) override;

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "dungeon assist loose"; }
        virtual std::string GetHelpDescription()
        {
            return "Picks up a loose mob that is attacking a party member while the bot has no target.";
        }
        virtual std::vector<std::string> GetUsedActions() { return {}; }
        virtual std::vector<std::string> GetUsedValues() { return {}; }
#endif
    };

    class DungeonRoutePullAction : public Action
    {
    public:
        DungeonRoutePullAction(PlayerbotAI* ai, std::string name = "dungeon route pull") : Action(ai, name) {}
        bool Execute(Event& event) override;
        bool isPossible() override;

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "dungeon route pull"; }
        virtual std::string GetHelpDescription()
        {
            return "Pulls the current wave (or boss) target of a verified dungeon route from the wave pull position.";
        }
        virtual std::vector<std::string> GetUsedActions() { return {"pull my target"}; }
        virtual std::vector<std::string> GetUsedValues() { return {}; }
#endif
    };

    // [2026-10-25 站长口径] 坦克救场（最高优先级）：任意队友被敌对怪攻击 ⇒ 立刻冲去那只，
    //   嘲讽 + 攻击；如果它不朝坦克走（远程/法系/站桩），坦克负责**卡视野**把它逼过来。
    class DungeonTankEngageAction : public Action
    {
    public:
        DungeonTankEngageAction(PlayerbotAI* ai, std::string name = "dungeon tank engage") : Action(ai, name) {}
        bool Execute(Event& event) override;
#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "dungeon tank engage"; }
        virtual std::string GetHelpDescription()
        {
            return "Tank takes over any mob attacking a party member, and LOS-breaks mobs that do not approach.";
        }
        virtual std::vector<std::string> GetUsedActions() { return {}; }
        virtual std::vector<std::string> GetUsedValues() { return {}; }
#endif
    };

    // [2026-10-25 站长口径] 控制模式：有 CC 的职业控一只高威胁怪（每人一只、不重复，排除 priority 5）。
    class DungeonCcCastAction : public Action
    {
    public:
        DungeonCcCastAction(PlayerbotAI* ai, std::string name = "dungeon cc cast") : Action(ai, name) {}
        bool Execute(Event& event) override;
    };

    // [2026-10-25 站长口径] 怪没聚齐 / 坦克仇恨不够 ⇒ DPS 与法系停手，等坦克把仇恨和聚怪做完。
    class DungeonDpsHoldFireAction : public Action
    {
    public:
        DungeonDpsHoldFireAction(PlayerbotAI* ai, std::string name = "dungeon dps hold fire") : Action(ai, name) {}
        bool Execute(Event& event) override;
#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "dungeon dps hold fire"; }
        virtual std::string GetHelpDescription()
        {
            return "DPS holds fire until the tank has threat and every mob is gathered within 5 yards.";
        }
        virtual std::vector<std::string> GetUsedActions() { return {}; }
        virtual std::vector<std::string> GetUsedValues() { return {}; }
#endif
    };
}
