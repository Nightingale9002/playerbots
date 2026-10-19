#pragma once
#include "MovementActions.h"
#include "GenericActions.h"

namespace ai
{
   class WaitForAttackKeepSafeDistanceAction : public MovementAction
   {
   public:
       WaitForAttackKeepSafeDistanceAction(PlayerbotAI* ai) : MovementAction(ai, "wait for attack keep safe distance") {}
       virtual bool Execute(Event& event) override;

   private:
       // [safe-move 2026-10-18 站长口径] hostiles = 附近全部敌对单位（缓存 1 秒，由 MovementAction 提供），
       //   ignoreUnit = 正在处理的怪（已经引到了，不用再按它的仇恨半径躲）
       const WorldPosition GetBestPoint(const WorldPosition& pos, float minDistance, float maxDistance,
           const std::vector<CombatHostileUnit>& hostiles, Unit* ignoreUnit) const;
       bool IsEnemyClose(const WorldPosition& point, const std::list<ObjectGuid>& enemies) const;
       bool isUseful() override;
       virtual bool isUsefulWhenStunned() override { return true; }
   };
}
