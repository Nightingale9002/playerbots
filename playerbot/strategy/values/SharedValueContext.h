#pragma once

#include "PvpValues.h"
#include "QuestValues.h"
#include "TrainerValues.h"
#include "VendorValues.h"
#include "TravelValues.h"
#include "LootValues.h"
#include "MountValues.h"
#include "playerbot/PlayerbotAI.h"

namespace ai
{
    class SharedValueContext : public NamedObjectContext<UntypedValue>
    {
    public:
        SharedValueContext() : NamedObjectContext(true)
        {
            creators["bg masters"] = [](PlayerbotAI* ai) { return new BgMastersValue(ai); };

            creators["item drop map"] = [](PlayerbotAI* ai) { return new ItemDropMapValue(ai); };
            creators["drop map"] = [](PlayerbotAI* ai) { return new DropMapValue(ai); };
            creators["item drop list"] = [](PlayerbotAI* ai) { return new ItemDropListValue(ai); };
            creators["entry loot list"] = [](PlayerbotAI* ai) { return new EntryLootListValue(ai); };
            creators["loot chance"] = [](PlayerbotAI* ai) { return new LootChanceValue(ai); };

            creators["vendor map"] = [](PlayerbotAI* ai) { return new VendorMapValue(ai); };
            creators["item vendor list"] = [](PlayerbotAI* ai) { return new ItemVendorListValue(ai); };

            creators["entry quest relation"] = [](PlayerbotAI* ai) { return new EntryQuestRelationMapValue(ai); };

            creators["quest guidp map"] = [](PlayerbotAI* ai) { return new QuestGuidpMapValue(ai); };
            creators["quest givers"] = [](PlayerbotAI* ai) { return new QuestGiversValue(ai); };

            creators["trainable spell map"] = [](PlayerbotAI* ai) { return new TrainableSpellMapValue(ai); };

          

            creators["entry travel purpose"] = [](PlayerbotAI* ai) { return new EntryTravelPurposeMapValue(ai); };
            creators["entry guidps"] = [](PlayerbotAI* ai) { return new EntryGuidpsValue(ai); };

            creators["full mount list"] = [](PlayerbotAI* ai) { return new FullMountListValue(ai); };

            creators["global string"] = [](PlayerbotAI* ai) { return new StringManualSetValue(ai); };
        }
    };


    class SharedObjectContext
    {
    public:
        SharedObjectContext() { valueContexts.Add(new SharedValueContext()); };

    public:
        virtual UntypedValue* GetUntypedValue(const std::string& name)
        {
            if (name.empty())
                return nullptr;

            PlayerbotAI* ai = new PlayerbotAI();
            UntypedValue* value = valueContexts.GetObject(name, ai);
            delete ai;
            return value;
        }

        template<class T>
        Value<T>* GetValue(const std::string& name)
        {
            return dynamic_cast<Value<T>*>(GetUntypedValue(name));
        }

        // [降峰 2026-10-15] 允许释放某个共享值（原 SharedObjectContext 没有清空接口）。
        // 用途：启动建完旅行图后释放 "entry guidps"（EntryGuidps，实测占用约 500MB 且属于永久缓存）。
        //
        // [crash-fix 2026-10-16] 这里必须两步走，而且调用方必须确认没有人还在用这个值：
        //   ① Unregister()：把对象标记成"已失效"，任何之前拿到裸指针的代码可以用
        //      IsRegistered()/AiObjectContext::IsValueAlive() 判断出来；
        //   ② 再 Erase()（Erase 只删"本 context 自己 new 的"对象）。
        // 之前是裸的 valueContexts.Erase(name)：对象被 delete 掉，而调用方手里可能还留着
        // 它内部容器的引用（GAI_VALUE_REF 那批）⇒ use-after-free。
        void ClearValue(const std::string& name)
        {
            if (valueContexts.IsCreated(name))
            {
                if (UntypedValue* value = GetUntypedValue(name))
                    value->Unregister();
            }

            valueContexts.Erase(name);
        }

        template<class T>
        Value<T>* GetValue(const std::string& name, const std::string& param)
        {
            return GetValue<T>((std::string(name) + "::" + param));
        }

        template<class T>
        Value<T>* GetValue(const std::string& name, int32 param)
        {
            std::ostringstream out; out << param;
            return GetValue<T>(name, out.str());
        }

        // [收敛 2026-10-16] AiSharedValueAlive() 需要"这个名字登记过没有"。
        // 用 IsCreated（= valueContexts 的登记表），语义与 AiObjectContext::HasValue 一致。
        bool HasValue(const std::string& name)
        {
            return valueContexts.IsCreated(name);
        }
    protected:
        NamedObjectContextList<UntypedValue> valueContexts;
    };
}

#define sSharedObjectContext MaNGOS::Singleton<SharedObjectContext>::Instance()

// [收敛 2026-10-16] 这些 GAI_VALUE* / SET_GAI_VALUE* 的落地函数**必须留在全局命名空间**。
// 原因：AiObjectContext.h 里的宏全部写成 `::AiSharedValueOr<type>(...)` 这种全局限定形式
// （那边不能写 `ai::` —— ai::AiObjectContext 有个成员变量就叫 ai，命名空间限定名会被解析成那个成员）。
// 上一次重构把这几个函数挪进了 namespace ai，于是所有 GAI_VALUE 宏在**成员函数体内**展开时
// `::AiSharedValue2Or` 找不到声明 ⇒ "'::AiSharedValue2Or' has not been declared; did you mean 'ai::ai::AiSharedValue2Or'?"
// 以及 "'AiSharedValueAlive' was not declared in this scope; did you mean 'ai::ai::AiSharedValueAlive'?"。
// 语义不变，只是换个可见性位置。
//
// [收敛 2026-10-16 之二] 存活判据不能写成 `Value<T>*->IsRegistered()`：
// IsRegistered() 声明在 AiNamedObject（= UntypedValue 那一支）上，而 GetValue<T>() 返回的是
// **另一支** `Value<T>*`，通过它看不到那个成员 ⇒ "class ai::Value<...> has no member named
// 'IsRegistered'"。所以统一先拿 UntypedValue* 判存活，再 dynamic_cast 到 Value<T>。
template<class T>
inline decltype(auto) AiSharedValueOr(const std::string& name)
{
    ai::UntypedValue* untyped = sSharedObjectContext.GetUntypedValue(name);
    ai::Value<T>* value = (untyped && untyped->IsRegistered()) ? dynamic_cast<ai::Value<T>*>(untyped) : nullptr;

    if constexpr (std::is_reference<T>::value)
    {
        if (!value)
            return AiMissingValue<T>();

        return value->Get();
    }
    else
    {
        return value ? value->Get() : AiMissingValue<T>();
    }
}

template<class T>
inline decltype(auto) AiSharedValue2Or(const std::string& name, const std::string& param)
{
    return AiSharedValueOr<T>(name + "::" + param);
}

// [收敛 2026-10-16] 整数限定符版。GAI_VALUE2 的调用方会直接传 int/uint32（例：
// LootObjectStack.cpp 的 `GAI_VALUE2(std::list<uint32>, "entry loot list", -1*int(entry))`），
// 只给 `const std::string&` 版时 gcc 拒绝把 int 当字符串 ⇒ "invalid initialization of reference of
// type 'const string&' ... from expression of type 'int'"。用模板按"能流进 ostream"约束，
// 与 AiObjectContext.h → AiValue2Or() 的 int32 重载口径一致。
template<class T, class Q, typename std::enable_if<!std::is_convertible<Q, std::string>::value, int>::type = 0>
inline decltype(auto) AiSharedValue2Or(const std::string& name, Q param)
{
    std::ostringstream out; out << param;
    return AiSharedValue2Or<T>(name, out.str());
}

template<class T>
inline void AiSetSharedValue(const std::string& name, T value)
{
    ai::UntypedValue* untyped = sSharedObjectContext.GetUntypedValue(name);
    if (!untyped || !untyped->IsRegistered())
        return;

    if (ai::Value<T>* target = dynamic_cast<ai::Value<T>*>(untyped))
        target->Set(value);
}

template<class T>
inline void AiSetSharedValue2(const std::string& name, const std::string& param, T value)
{
    AiSetSharedValue<T>(name + "::" + param, value);
}

// 共享值的存活自检（GetValue<T>() 返回的 Value<T>* 看不到 IsRegistered，所以走 UntypedValue）。
inline bool AiSharedValueAlive(const std::string& name)
{
    if (!sSharedObjectContext.HasValue(name))
        return false;

    ai::UntypedValue* value = sSharedObjectContext.GetUntypedValue(name);
    return value != nullptr && value->IsRegistered();
}