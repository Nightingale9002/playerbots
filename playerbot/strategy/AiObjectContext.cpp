
#include "playerbot/playerbot.h"
#include "Action.h"
#include "AiObjectContext.h"
#include "NamedObjectContext.h"


using namespace ai;

AiObjectContext::AiObjectContext(PlayerbotAI* ai) : PlayerbotAIAware(ai)
{
    // [降峰值 2026-10-15] Context 的实例化被拆到 6 个独立 TU（见 AiObjectContext*.cpp），
    // 避免单个 TU 实例化全部注册表导致 gcc 峰值 >1.5GB 而被 OOM-killer 杀掉。
    BuildStrategyContextsA();
    BuildStrategyContextsB();
    BuildActionContextsA();
    BuildActionContextsB();
    BuildTriggerContexts();
    BuildValueContexts();
}

void AiObjectContext::ClearValues(std::string findName)
{
    // [crash-fix 2026-10-16] 名字要先快照（erase 会改 map），并且删之前把值对象标记成"已失效"
    // （UntypedValue::Unregister）。如果别处还攥着这个值的裸指针，至少可以通过
    // AiObjectContext::IsValueAlive() 判断出来，而不是继续解引用一块已被 std::string 复用的内存。
    std::set<std::string> names = valueContexts.GetCreated();
    for (std::set<std::string>::iterator i = names.begin(); i != names.end(); ++i)
    {
        if (!findName.empty() && i->find(findName) != 0)
            continue;

        UntypedValue* value = GetUntypedValue(*i);
        if (value)
            value->Unregister();

        valueContexts.Erase(*i);
    }
}

void AiObjectContext::ClearExpiredValues(std::string findName, uint32 interval)
{
    std::vector<std::string> namesToErase;
    std::set<std::string> names = valueContexts.GetCreated();

    for (const auto& name : names)
    {
        UntypedValue* value = GetUntypedValue(name);
        if (!value || value->Protected())
            continue;

        if (!findName.empty() && name.find(findName) == std::string::npos)
            continue;

        if ((!interval && !value->Expired()) || (interval && !value->Expired(interval)))
            continue;

        namesToErase.push_back(name);
    }

    for (const auto& name : namesToErase)
    {
        UntypedValue* value = GetUntypedValue(name);
        if (value)
            value->Unregister();

        valueContexts.Erase(name);
    }
}


std::string AiObjectContext::FormatValues(std::string findName)
{
    std::ostringstream out;
    std::set<std::string> names = valueContexts.GetCreated();
    for (std::set<std::string>::iterator i = names.begin(); i != names.end(); ++i)
    {
        UntypedValue* value = GetUntypedValue(*i);
        if (!value)
            continue;

        if (!findName.empty() && i->find(findName) == std::string::npos)
            continue;

        std::string text = value->Format();
        if (text == "?")
            continue;

        out << "{" << *i << "=" << text << "}|";
    }
    out.seekp(-1, out.cur);
    return out.str();
}

void AiObjectContext::Update()
{
    /* Disabled until there is actually a strategy, trigger, action or value that has the Update() method. Currently this takes 8% cpu and does 'NOTHING'.
    strategyContexts.Update();
    triggerContexts.Update();
    actionContexts.Update();
    valueContexts.Update();
    */
}

void AiObjectContext::Reset()
{
    strategyContexts.Reset();
    triggerContexts.Reset();
    actionContexts.Reset();
    valueContexts.Reset();
}

std::list<std::string> AiObjectContext::Save()
{
    std::list<std::string> result;

    std::set<std::string> names = valueContexts.GetCreated();
    for (std::set<std::string>::iterator i = names.begin(); i != names.end(); ++i)
    {
        UntypedValue* value = GetUntypedValue(*i);
        if (!value)
            continue;

        std::string data = value->Save();
        if (data == "?")
            continue;

        std::string name = *i;
        std::ostringstream out;
        out << name;

        out << ">" << data;
        result.push_back(out.str());
    }
    return result;
}

void AiObjectContext::Load(std::list<std::string> data)
{
    for (std::list<std::string>::iterator i = data.begin(); i != data.end(); ++i)
    {
        std::string row = *i;
        std::vector<std::string> parts = split(row, '>');
        if (parts.empty() || parts.size() > 2) continue;

        std::string name = parts[0];
        std::string text = (parts.size() == 2) ? parts[1] : "";

        UntypedValue* value = GetUntypedValue(name);
        if (!value) continue;

        value->Load(text);
    }
}
