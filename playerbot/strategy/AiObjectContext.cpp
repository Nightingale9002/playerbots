
#include "playerbot/playerbot.h"
#include "Action.h"
#include "AiObjectContext.h"
#include "NamedObjectContext.h"

#include <atomic>
#include <mutex>
#include <typeinfo>
#include <cstdlib>


using namespace ai;

// -----------------------------------------------------------------------------------------------
// [registry-audit 2026-11-01] D1：同名 action/trigger 的重复定义检测（惰性、只打日志）。
//
// 背景：CreateActionNode() -> AiObjectContext::GetAction() 在多个 NamedObjectContext 里按
// 链表顺序找第一个能创建对象的 creator，所以 **同名** creator 谁生效取决于注册顺序，且原来
// 没有任何地方知道"这个名字出现了两次"。
//
// 做法：进程内第一次解析 action（或 trigger）名字时，把**本类**注册表扫一遍，数出每个名字被
// 几个 context 注册；>1 的打一条 [REGISTRY] DUP ...。之后只剩一次原子读，热路径几乎零成本。
// **不改解析顺序、不改返回值**，选择结果与改动前完全一致。
//
// 为什么要按 typeid 分桶：每个职业的 AiObjectContext 子类会往同一个容器里追加自己的 context，
// 只有同类型的注册表才可比。分桶后计数对每个 bot 都准确，也不会随 bot 数量膨胀。
//
// 去重/上限：每个 "类型名+名字" 只报一次；进程内每类最多 kRegistryAuditDupLogCap 条，多的只
// 累计，最后打一行汇总。
// -----------------------------------------------------------------------------------------------
namespace
{
    const size_t kRegistryAuditDupLogCap = 200;

    // 一份注册表（= 一个具体的 AiObjectContext 子类）的只读索引。
    struct RegistryAuditBucket
    {
        std::map<std::string, int> actionCounts;   // 名字 -> 注册它的 context 数
        std::map<std::string, int> triggerCounts;
        bool actionsBuilt = false;
        bool triggersBuilt = false;
    };

    struct RegistryAuditState
    {
        std::mutex mutex;
        std::map<std::string, RegistryAuditBucket*> buckets;   // key = typeid(*this).name()
        std::set<std::string> dupLogged;                       // 全局去重："action|名字"
        size_t logged = 0;
        size_t dupTotal = 0;
        size_t suppressed = 0;
        bool summaryReported = false;

        ~RegistryAuditState()
        {
            for (std::map<std::string, RegistryAuditBucket*>::iterator i = buckets.begin(); i != buckets.end(); ++i)
                delete i->second;
        }
    };

    RegistryAuditState& RegistryAudit()
    {
        static RegistryAuditState state;
        return state;
    }

    // 需要临时关掉注册表自省时：环境变量 BOT_REGISTRY_AUDIT=0（与 Engine.cpp 同一开关）。
    // 关掉后索引保持空，Has*Creator 一律按"未知"返回 true，不会误报 MISSING。
    bool RegistryAuditEnabled()
    {
        static const bool enabled = []() -> bool
        {
            const char* value = std::getenv("BOT_REGISTRY_AUDIT");
            return !(value && std::string(value) == "0");
        }();

        return enabled;
    }

    // creator 表里的键是"基础名"（NamedObjectFactory::Create 会先把 "::" 后面的限定符切掉）。
    std::string RegistryBaseName(const std::string& name)
    {
        size_t pos = name.find("::");
        return (pos == std::string::npos) ? name : name.substr(0, pos);
    }

    // 调用方必须已持有 audit.mutex。
    void ReportDuplicateDefinitions(const char* kind, const std::map<std::string, int>& counts, RegistryAuditState& audit)
    {
        for (std::map<std::string, int>::const_iterator i = counts.begin(); i != counts.end(); ++i)
        {
            if (i->second < 2)
                continue;

            if (!audit.dupLogged.insert(std::string(kind) + "|" + i->first).second)
                continue;

            ++audit.dupTotal;

            if (audit.logged >= kRegistryAuditDupLogCap)
            {
                ++audit.suppressed;
                continue;
            }

            ++audit.logged;
            sLog.outError("[REGISTRY] DUP %s=%s contexts=%d", kind, i->first.c_str(), i->second);
        }

        if (audit.suppressed && !audit.summaryReported)
        {
            audit.summaryReported = true;
            sLog.outError("[REGISTRY] DUP summary: total=%u, suppressed=%u (only first %u printed)",
                (uint32)audit.dupTotal, (uint32)audit.suppressed, (uint32)kRegistryAuditDupLogCap);
        }
    }
}

Action* AiObjectContext::GetAction(const std::string& name)
{
    // [registry-audit] 第一次解析时自省一次；之后 actionCreatorIndex 非空，只多一次原子读。
    if (!actionCreatorIndex.load(std::memory_order_acquire))
        EnsureActionRegistryAudit();

    return actionContexts.GetObject(name, ai);
}

Trigger* AiObjectContext::GetTrigger(const std::string& name)
{
    if (!triggerCreatorIndex.load(std::memory_order_acquire))
        EnsureTriggerRegistryAudit();

    return triggerContexts.GetObject(name, ai);
}

void AiObjectContext::EnsureActionRegistryAudit()
{
    if (actionCreatorIndex.load(std::memory_order_acquire) || !RegistryAuditEnabled())
        return;

    RegistryAuditState& audit = RegistryAudit();
    const std::string typeKey = typeid(*this).name();

    std::lock_guard<std::mutex> lock(audit.mutex);

    if (actionCreatorIndex.load(std::memory_order_relaxed))
        return;

    RegistryAuditBucket*& slot = audit.buckets[typeKey];
    if (!slot)
        slot = new RegistryAuditBucket();

    if (!slot->actionsBuilt)
    {
        actionContexts.CountCreators(slot->actionCounts);
        slot->actionsBuilt = true;
        ReportDuplicateDefinitions("action", slot->actionCounts, audit);
    }

    actionCreatorIndex.store(&slot->actionCounts, std::memory_order_release);
}

void AiObjectContext::EnsureTriggerRegistryAudit()
{
    if (triggerCreatorIndex.load(std::memory_order_acquire) || !RegistryAuditEnabled())
        return;

    RegistryAuditState& audit = RegistryAudit();
    const std::string typeKey = typeid(*this).name();

    std::lock_guard<std::mutex> lock(audit.mutex);

    if (triggerCreatorIndex.load(std::memory_order_relaxed))
        return;

    RegistryAuditBucket*& slot = audit.buckets[typeKey];
    if (!slot)
        slot = new RegistryAuditBucket();

    if (!slot->triggersBuilt)
    {
        triggerContexts.CountCreators(slot->triggerCounts);
        slot->triggersBuilt = true;
        ReportDuplicateDefinitions("trigger", slot->triggerCounts, audit);
    }

    triggerCreatorIndex.store(&slot->triggerCounts, std::memory_order_release);
}

bool AiObjectContext::HasActionCreator(const std::string& name)
{
    if (name.empty())
        return false;

    EnsureActionRegistryAudit();

    const std::map<std::string, int>* index = actionCreatorIndex.load(std::memory_order_acquire);
    if (!index)     // 审计没建成：按"未知"处理，绝不当成 MISSING 去误报
        return true;

    return index->find(RegistryBaseName(name)) != index->end();
}

bool AiObjectContext::HasTriggerCreator(const std::string& name)
{
    if (name.empty())
        return false;

    EnsureTriggerRegistryAudit();

    const std::map<std::string, int>* index = triggerCreatorIndex.load(std::memory_order_acquire);
    if (!index)
        return true;

    return index->find(RegistryBaseName(name)) != index->end();
}

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
