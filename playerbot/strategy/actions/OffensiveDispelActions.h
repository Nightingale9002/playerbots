#pragma once
#include "GenericActions.h"

// [BOSS 战策略 2026-10-18 站长口径] **进攻驱散**：把敌人身上的魔法增益驱掉/偷掉/净化掉。
//   已有实现是**各职业私有**的（法师 spellsteal、萨满 purge、牧师 dispel magic、猎人 tranquilizing shot），
//   但没有一个"共享层可写规则"的动作名 ⇒ BOSS/副本策略表写不出来。
//   这里提供一个通用动作：按 bot 职业自动选对应驱散法术，目标 = 敌方带魔法增益的单位。
//   没对应法术的职业 ⇒ 动作直接不可用（不误指挥）。
namespace ai
{
    // 找"敌方第一个带魔法增益、且在当前射程/可见范围内"的目标；spellName 回填该职业的驱散法术名。
    Unit* FindDispelEnemy(PlayerbotAI* ai, std::string& spellName, bool requireCastable = false);

    class DispelEnemyAction : public Action
    {
    public:
        DispelEnemyAction(PlayerbotAI* ai) : Action(ai, "dispel enemy") {}

        bool Execute(Event& event) override;
        bool isPossible() override;
    };
}
