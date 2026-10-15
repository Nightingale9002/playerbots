#pragma once

#include "playerbot/PlayerbotAIAware.h"
#include "Action.h"
#include "Value.h"
#include "NamedObjectContext.h"
#include "Strategy.h"
#include <set>

namespace ai
{
    class UntypedValue;
    template<class T> class Value;
}

namespace ai
{
    class AiObjectContext : public PlayerbotAIAware
    {
    public:
        AiObjectContext(PlayerbotAI* ai);
        virtual ~AiObjectContext() {}

    public:
        virtual Strategy* GetStrategy(const std::string& name) { return strategyContexts.GetObject(name, ai); }
        virtual std::set<std::string> GetSiblingStrategy(const std::string& name) { return strategyContexts.GetSiblings(name); }
        virtual Trigger* GetTrigger(const std::string& name) { return triggerContexts.GetObject(name, ai); }
        virtual Action* GetAction(const std::string& name) { return actionContexts.GetObject(name, ai); }
        virtual UntypedValue* GetUntypedValue(const std::string& name)
        {
            if (name.empty())
                return nullptr;

            return valueContexts.GetObject(name, ai);
        }

        template<class T>
        Value<T>* GetValue(const std::string& name)
        {
            if (name.empty())
                return nullptr;

            return dynamic_cast<Value<T>*>(GetUntypedValue(name));
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

        bool HasValue(const std::string& name)
        {
            return valueContexts.IsCreated(name);
        }

        // [crash-fix 2026-10-16] 弱引用自检：某个值名对应的对象是否**仍然**登记在上下文里。
        // 用 ClearValues()/ClearExpiredValues()/ClearValue() 摘掉的值会返回 false —— 调用方如果
        // 手里攥着之前取到的裸指针，应先过这一关再用，不要再无条件解引用。
        bool IsValueAlive(const std::string& name)
        {
            UntypedValue* value = GetUntypedValue(name);
            return value != nullptr && value->IsRegistered();
        }

        bool HasValue(const std::string& name, const std::string& param)
        {
            return HasValue((std::string(name) + "::" + param));
        }

        bool HasValue(const std::string& name, int32 param)
        {
            std::ostringstream out; out << param;
            return HasValue(name, out.str());
        }

        bool HasSupportedValue(const std::string& name)
        {
            std::string nameView = name;
            std::string qualifierView;
            if (size_t pos = nameView.find("::"); pos != std::string::npos)
            {
                qualifierView = nameView.substr(pos + 2);
                nameView = nameView.substr(0, pos);
            }

            std::set<std::string> supportedKeys;
            GetSupportedValues(supportedKeys);
            return supportedKeys.find(nameView) != supportedKeys.end();
        }

        std::set<std::string> GetValues()
        {
            return valueContexts.GetCreated();
        }

        void GetSupportedStrategies(std::set<std::string>& strategies)
        {
            return strategyContexts.GetSupportedKeys(strategies);
        }

        void GetSupportedTriggers(std::set<std::string>& triggers)
        {
            return triggerContexts.GetSupportedKeys(triggers);
        }

        void GetSupportedActions(std::set<std::string>& actions)
        {
            return actionContexts.GetSupportedKeys(actions);
        }

        void GetSupportedValues(std::set<std::string>& values)
        {
            return valueContexts.GetSupportedKeys(values);
        }

        void ClearValues(std::string findName = "");

        void ClearExpiredValues(std::string findName = "", uint32 interval = 0);

        std::string FormatValues(std::string findName = "");

    public:
        virtual void Update();
        virtual void Reset();
        virtual void AddShared(NamedObjectContext<UntypedValue>* sharedValues)
        {
            valueContexts.Add(sharedValues);
        }
        std::list<std::string> Save();
        void Load(std::list<std::string> data);

        std::vector<std::string> performanceStack;
    protected:
        // [降峰值 2026-10-15] 这些 Context 都是巨型注册表类，构造时会把成百上千的
        // strategy/action/trigger/value 模板实例化出来。原本全挤在一个 TU 里，gcc 峰值 >1.5GB，
        // 在 2 核 1.87GB 的云端直接被 OOM-killer 杀掉。拆成 6 个 TU 后每个只担一部分，
        // 峰值约降到 1/6，行为完全不变。
        void BuildStrategyContextsA();
        void BuildStrategyContextsB();
        void BuildActionContextsA();
        void BuildActionContextsB();
        void BuildTriggerContexts();
        void BuildValueContexts();

        NamedObjectContextList<Strategy> strategyContexts;
        NamedObjectContextList<Action> actionContexts;
        NamedObjectContextList<Trigger> triggerContexts;
        NamedObjectContextList<UntypedValue> valueContexts;
    };

    // -------------------------------------------------------------------------------------------
    // [crash-fix 2026-10-16] 取值入口的空指针/失效防护。
    //
    // 线上崩溃（#0 rip 落在数据段某个 std::string 缓冲区，rax == rip，error 15）的那条链是
    //     CalculatedValue<bool>::Get() -> BoolAndValue::Calculate() -> 间接调用跳进字符串
    // 也就是说 BoolAndValue::Calculate() 里 AI_VALUE(bool, <子值名>) 拿到的 Value<bool>* 并不可信。
    // 旧宏 `context->GetValue<T>(name)->Get()` 对 nullptr 是直接解引用；名字不存在时
    // NamedObjectContext::Create() 还会把 nullptr 记进表，之后每次取都命中 nullptr。
    // 下面把"取指针"与"解引用"合并成一个带判空的函数，任何一步拿不到就返回默认值而不是崩。
    //
    // 注意：这是止血（把崩溃换成默认值），不是让悬垂指针重新有效。已经在其它地方持有裸
    // Value<T>* 的代码仍应改用 IsValueAlive() 自检，或者干脆每次重新取。
    // -------------------------------------------------------------------------------------------
    // NOTE: 这些辅助函数在全局命名空间，不在 namespace ai 里。
    // 原因：ai::AiObjectContext 有个成员变量就叫 `ai`（PlayerbotAIAware::ai），在 AiObjectContext
    // 的成员函数体里 `ai::Xxx` 会被解析成这个成员而不是命名空间，限定名解析直接编译失败。
    // 放全局命名空间 + `::` 前缀就绕开了这个歧义。
    // NOTE 2: the shared-value helpers (AiSharedValueOr / AiSharedValue2Or / AiSetSharedValue /
    // AiSetSharedValue2 / AiSharedValueAlive) deliberately live in strategy/values/SharedValueContext.h,
    // right next to the sSharedObjectContext singleton they use, and — like the helpers above —
    // they are defined in the **global** namespace, because the GAI_VALUE* macros below use `::`.
}

// 取"名字对应的、仍然活着的"值对象。名字不存在或已被 Clear* 摘掉（WeakRef 失效）时返回 nullptr。
// 注意：IsRegistered() 在 UntypedValue 上（Value<T> 只是接口，看不到它），所以先取 UntypedValue 再转换。
inline ai::UntypedValue* AiAliveUntypedValue(ai::AiObjectContext* context, const std::string& name)
{
    ai::UntypedValue* value = context ? context->GetUntypedValue(name) : nullptr;
    return (value && value->IsRegistered()) ? value : nullptr;
}

template<class T>
inline ai::Value<T>* AiAliveValue(ai::AiObjectContext* context, const std::string& name)
{
    ai::UntypedValue* untyped = AiAliveUntypedValue(context, name);
    // 注意：dynamic_cast 的目标可能是引用型（AI_VALUE(LastSpellCast&, ...)）。
    // Value<T&> 是合法类型（Value<T> 只是接口），所以这里不能对它做任何"按值构造"的动作。
    return untyped ? dynamic_cast<ai::Value<T>*>(untyped) : nullptr;
}

// 引用安全落地：原来三个取值入口都写 `value ? value->Get() : T()`，
// 而 AI_VALUE 大量以**引用**为模板参数（AI_VALUE(LastSpellCast&, …) / (PositionMap&, …) /
// (LastMovement&, …) / (std::list<std::string>&, …)），`T()` 对引用类型不是合法表达式
// ⇒ "value-initialization of reference type 'ai::LastSpellCast&'"（PlayerbotAI.cpp 4 处）。
//
// 约定：**引用型模板参数仍然返回引用**（保留 `AI_VALUE(X&, …).Foo()` 的原地写语义），
// 值型仍然返回值。取不到时，引用型给一个共享的空对象、值型给值初始化的临时对象 —— 都不是空指针解引用。
template<class T>
inline T& AiValueFallbackRef()
{
    static T fallback{};
    return fallback;
}

// 引用型 T（T&）：返回静态空对象
template<class T>
inline T& AiMissingValue(std::true_type)
{
    return AiValueFallbackRef<typename std::remove_reference<T>::type>();
}

// 值型 T：返回值初始化的临时对象
template<class T>
inline T AiMissingValue(std::false_type)
{
    return T();
}

template<class T>
inline T AiMissingValue()
{
    return AiMissingValue<T>(typename std::is_reference<T>::type());
}

template<class T>
inline decltype(auto) AiValueOr(ai::AiObjectContext* context, const std::string& name)
{
    ai::Value<T>* value = AiAliveValue<T>(context, name);
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
inline decltype(auto) AiValue2Or(ai::AiObjectContext* context, const std::string& name, const std::string& param)
{
    return AiValueOr<T>(context, name + "::" + param);
}

template<class T>
inline decltype(auto) AiValue2Or(ai::AiObjectContext* context, const std::string& name, int32 param)
{
    std::ostringstream out; out << param;
    return AiValue2Or<T>(context, name, out.str());
}

template<class T>
inline decltype(auto) AiLazyOr(ai::AiObjectContext* context, const std::string& name)
{
    ai::Value<T>* value = AiAliveValue<T>(context, name);
    if constexpr (std::is_reference<T>::value)
    {
        if (!value)
            return AiMissingValue<T>();

        return value->LazyGet();
    }
    else
    {
        return value ? value->LazyGet() : AiMissingValue<T>();
    }
}

// [收敛 2026-10-16] "带限定符的懒取值"必须有 **字符串限定符** 这一版。
// AI_VALUE2_LAZY 的调用方大量传 std::string / const char* / 由 Qualify(int32) 转成字符串的限定符
// （ItemVisitors.h 的 "item usage"+GetQualifier()、RpgTriggers.cpp 的 "group or"+逗号串、
// LootObjectStack.cpp 的 std::to_string(guid)），而这里过去只声明了 int32 版
// ⇒ "no matching function for call to 'AiLazyOr<...>(..., const char [11], std::string)'"。
template<class T>
inline decltype(auto) AiLazyOr(ai::AiObjectContext* context, const std::string& name, const std::string& param)
{
    return AiLazyOr<T>(context, name + "::" + param);
}

template<class T>
inline decltype(auto) AiLazyOr(ai::AiObjectContext* context, const std::string& name, int32 param)
{
    std::ostringstream out; out << param;
    return AiLazyOr<T>(context, name, out.str());
}

template<class T>
inline void AiSetValue(ai::AiObjectContext* context, const std::string& name, T value)
{
    ai::Value<T>* target = AiAliveValue<T>(context, name);
    if (target)
        target->Set(value);
}

template<class T>
inline void AiSetValue2(ai::AiObjectContext* context, const std::string& name, const std::string& param, T value)
{
    ai::Value<T>* target = AiAliveValue<T>(context, name + "::" + param);
    if (target)
        target->Set(value);
}

// [收敛 2026-10-16] 非字符串限定符版（与 AiResetValue 的变参版同一口径）。
// SET_AI_VALUE2 的限定符同样是五花八门：uint32（KeepItemAction.cpp 的
// `SET_AI_VALUE2(ForceItemUsage, "force item usage", id, usage)`、
// SetValueAction.cpp 的 `uint32(type)`）、字面量数组（`"grouper override"`）——
// 只给 `const std::string&` 版时前者报 "invalid initialization of reference of type
// 'const string&' from expression of type 'const unsigned int'"。
//
// 这一版接的是"限定符恰好一个"的常见形态；两段式限定符（`name + "::" + a + "::" + b`）仍走上面那版，
// 传进来的是一整个拼好的 std::string。
template<class T, class Q>
inline void AiSetValue2(ai::AiObjectContext* context, const std::string& name, Q qualifier, T value)
{
    std::ostringstream out;
    out << name << "::" << qualifier;

    ai::Value<T>* target = AiAliveValue<T>(context, out.str());
    if (target)
        target->Set(value);
}

inline void AiResetValue(ai::AiObjectContext* context, const std::string& name)
{
    ai::UntypedValue* value = AiAliveUntypedValue(context, name);
    if (value)
        value->Reset();
}

// [收敛 2026-10-16] 非字符串限定符版。原来 RESET_AI_VALUE2 的宏体自己做
// `std::string(name) + "::" + param`：整数限定符（AhAction.cpp 的
// `RESET_AI_VALUE2(uint32, "free money for", (uint32)NeedMoneyFor::ah)`、`freeMoney[usage]`、
// BankAction.cpp 的 `itemId`）是 `std::string + uint32` ⇒ "no match for 'operator+'"；
// 而字面量限定符（`"future travel purpose"`）连字符串重载都会因数组类型而落到失败的重载上。
// 这里统一用"流进 ostringstream 再拼接"，整数/枚举/字符串字面量一律合法。
template<class... Qualifiers>
inline void AiResetValue(ai::AiObjectContext* context, const std::string& name, Qualifiers... qualifiers)
{
    std::ostringstream out;
    out << name;
    ((out << "::" << qualifiers), ...);

    AiResetValue(context, out.str());
}

inline ai::UntypedValue* AiUntypedValue(ai::AiObjectContext* context, const std::string& name)
{
    return context ? context->GetUntypedValue(name) : nullptr;
}


#define AI_VALUE(type, name) ::AiValueOr<type>(context, name)
#define AI_VALUE2(type, name, param) ::AiValue2Or<type>(context, name, param)

#define AI_VALUE_SAFE(type, name) ::AiValueOr<type>(context, name)
#define AI_VALUE2_SAFE(type, name, param) ::AiValue2Or<type>(context, name, param)

#define AI_VALUE_LAZY(type, name) ::AiLazyOr<type>(context, name)
#define AI_VALUE2_LAZY(type, name, param) ::AiLazyOr<type>(context, name, param)

#define HAS_AI_VALUE(name) context->HasValue(name)
#define HAS_AI_VALUE2(name, param) context->HasValue(name, param)
#define AI_VALUE_EXISTS(type, name, emptyval) (HAS_AI_VALUE(name) ? AI_VALUE(type, name) : emptyval)
#define AI_VALUE2_EXISTS(type, name, param, emptyval) (HAS_AI_VALUE2(name, param) ? AI_VALUE2(type, name, param) : emptyval)

#define SET_AI_VALUE(type, name, value) ::AiSetValue<type>(context, name, value)
#define SET_AI_VALUE2(type, name, param, value) ::AiSetValue2<type>(context, name, param, value)
#define RESET_AI_VALUE(type, name) ::AiResetValue(context, name)
#define RESET_AI_VALUE2(type, name, param) ::AiResetValue(context, name, param)

#define PAI_VALUE(type, name) ::AiValueOr<type>(player->GetPlayerbotAI()->GetAiObjectContext(), name)
#define PAI_VALUE2(type, name, param) ::AiValue2Or<type>(player->GetPlayerbotAI()->GetAiObjectContext(), name, param)
#define SET_PAI_VALUE(type, name, value) ::AiSetValue<type>(player->GetPlayerbotAI()->GetAiObjectContext(), name, value)
#define SET_PAI_VALUE2(type, name, param, value) ::AiSetValue2<type>(player->GetPlayerbotAI()->GetAiObjectContext(), name, param, value)
#define PHAS_AI_VALUE(name) player->GetPlayerbotAI()->GetAiObjectContext()->HasValue(name)
#define PHAS_AI_VALUE2(name, param) player->GetPlayerbotAI()->GetAiObjectContext()->HasValue(name, param)
#define MAI_VALUE(type, name) ::AiValueOr<type>(master->GetPlayerbotAI()->GetAiObjectContext(), name)
#define MAI_VALUE2(type, name, param) ::AiValue2Or<type>(master->GetPlayerbotAI()->GetAiObjectContext(), name, param)

#define GAI_VALUE(type, name) ::AiSharedValueOr<type>(name)
#define GAI_VALUE2(type, name, param) ::AiSharedValue2Or<type>(name, param)
// [crash-fix 2026-10-16] 这里原来有 `GAI_VALUE_REF(type, name) -> Value<T>::GetRef()`（"零拷贝取共享大表"）。
// 已删除：返回的是共享值对象**内部成员**的引用，而该对象会被 SharedObjectContext::ClearValue()
// （启动建完旅行图后释放 "entry guidps"）或本地 context 的 ClearValues() 直接 delete ⇒ 引用立刻悬垂。
// 需要大表的调用方一律用 GAI_VALUE 取一份拷贝：一次拷贝的价格换掉一整类 use-after-free。
#define SET_GAI_VALUE(type, name, value) ::AiSetSharedValue<type>(name, value)
#define SET_GAI_VALUE2(type, name, param, value) ::AiSetSharedValue2<type>(name, param, value)

#define MEM_AI_VALUE(type, name) dynamic_cast<MemoryCalculatedValue<type>*>(::AiUntypedValue(context, name))
#define LOG_AI_VALUE(type, name) dynamic_cast<LogCalculatedValue<type>*>(::AiUntypedValue(context, name))