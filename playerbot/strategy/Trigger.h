#pragma once
#include "Event.h"
#include "Value.h"
#include "AiObject.h"

class Unit;

namespace ai
{
    class NextAction;
    template<class T> class Value;
}

namespace ai
{
    class Trigger : public AiNamedObject
	{
	public:
        Trigger(PlayerbotAI* ai, std::string name = "trigger", int checkInterval = 1) : AiNamedObject(ai, name) {
            this->triggered = false;
			this->checkInterval = checkInterval;
            this->lastCheckTime = time(0) - rand() % checkInterval;
		}
        virtual ~Trigger() {}

	public:
        virtual Event Check();
        virtual void ExternalEvent(std::string param, Player* owner = NULL) {}
        virtual void ExternalEvent(WorldPacket &packet, Player* owner = NULL) {}
        virtual void ExternalEventForce(std::string param, Player* owner = NULL)
        {
            this->param = param;
            this->owner = owner;
            this->triggered = true;
        }
        virtual bool IsActive() { return false; }
        virtual bool IsAlreadyTriggered() { return triggered; }
        // True for a trigger fed by an external packet (WorldPacketTrigger). Such an event is a
        // one-shot obligation the bot has to answer, so the engine must not drop it just because its
        // action did not get to run in the tick that delivered it.
        virtual bool IsExternalEvent() const { return false; }
        virtual NextAction** getHandlers() { return NULL; }
        void Update() {}  //Nonfunctional see AiObjectContext::Update() to enable.
        virtual void Reset() { triggered = false; }
        virtual Unit* GetTarget();
        virtual Value<ObjectGuid>* GetTargetValue();
        virtual std::string GetTargetName() { return "self target"; }

		bool needCheck() {
		    if (checkInterval < 2) return true;

		    time_t now = time(0);
			if (!lastCheckTime || now - lastCheckTime >= checkInterval) {
			    lastCheckTime = now;
				return true;
			}
			return false;
		}

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "dummy"; } //Must equal iternal name
        virtual std::string GetHelpDescription() { return "This is a trigger."; }
        virtual std::vector<std::string> GetUsedTriggers() { return {}; }
        virtual std::vector<std::string> GetUsedValues() { return {}; }
#endif

    protected:
		int checkInterval;
		time_t lastCheckTime;
        std::string param;
        bool triggered;
        Player* owner = nullptr;
	};


    class TriggerNode
    {
    public:
        TriggerNode(std::string name, NextAction** handlers = NULL)
        {
            this->name = name;
            this->handlers = handlers;
            this->trigger = NULL;
        }
        virtual ~TriggerNode();

    public:
        Trigger* getTrigger() { return trigger; }
        void setTrigger(Trigger* trigger) { this->trigger = trigger; }
        std::string getName() { return name; }

        // [2026-11-01 registry-audit] 只读窥视已挂在节点上的 NextAction 数组：
        //   getHandlers() 会 clone+merge 且要求 trigger 非空（会把 Trigger 实例化时机提前到
        //   Engine::Init），而注册表自检只要"名字"，所以给一个不创建对象、不分配内存的入口。
        //   没有显式数组的节点返回 NULL（其默认动作由 Strategy::getDefaultActions() 那条覆盖）。
        NextAction** peekHandlers() { return handlers; }

    public:
        NextAction** getHandlers();
        float getFirstRelevance();
    private:
        std::string name;
        Trigger* trigger;
        NextAction** handlers;
    };
}
