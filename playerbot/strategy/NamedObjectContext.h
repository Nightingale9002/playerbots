#pragma once
#include <cstdarg>
#include <string>
#include <iosfwd>
#include <set>
#include <list>
#include <map>
#include <type_traits>
#include "AiObject.h"

namespace ai
{
    class Qualified
    {
    public:
        Qualified() {};
        Qualified(const std::string& qualifier) : qualifier(qualifier) {}
        Qualified(int32 qualifier1) { Qualify(qualifier1); }

    public:
        virtual void Qualify(int32 qualifier) { std::ostringstream out; out << qualifier; this->qualifier = out.str(); }
        virtual void Qualify(const std::string& qualifier) { this->qualifier = qualifier; }
        std::string getQualifier() const { return qualifier; }
        void Reset() { qualifier.clear(); }

        static std::string MultiQualify(const std::vector<std::string>& qualifiers, const std::string& separator, const std::string_view brackets = "{}")
        { 
            std::stringstream out;
            for (uint8 i = 0; i < qualifiers.size(); i++)
            {
                const std::string& qualifier = qualifiers[i];
                if (i == qualifiers.size() - 1)
                {
                    out << qualifier;
                }
                else
                {
                    out << qualifier << separator;
                }
            }

            if (brackets.empty())
            {
                return out.str();
            }
            else
            {
                return brackets[0] + out.str() + brackets[1];
            }
        }

        static std::vector<std::string> getMultiQualifiers(const std::string& qualifier1, const std::string& separator, const std::string_view brackets = "{}")
        { 
            std::vector<std::string> result;

            std::string view = qualifier1;

            if(view.find(brackets[0]) == 0)
                view = qualifier1.substr(1, qualifier1.size()-2);

            size_t last = 0; 
            size_t next = 0; 

            if (view.find(brackets[0]) == std::string::npos)
            {
                while ((next = view.find(separator, last)) != std::string::npos)
                {

                    result.push_back((std::string)view.substr(last, next - last));
                    last = next + separator.length();
                }

                result.push_back(view.substr(last));
            }
            else
            {
                int8 level = 0;
                std::string sub;
                while (next < view.size() || level < 0)
                {
                    if (view[next] == brackets[0])
                        level++;
                    else if (view[next] == brackets[1])
                        level--;
                    else if (!level && view.substr(next, separator.size()) == separator)
                    {
                        result.push_back(sub);
                        sub.clear();
                        next += separator.size();
                        continue;
                    }
                    
                    sub += view[next];

                    next++;
                }

                result.push_back(sub);
            }

            return result;
        }

        static bool isValidNumberString(const std::string& str)
        {
            bool valid = !str.empty();
            if (valid)
            {
                // Check for sign character at the beginning
                size_t start = 0;
                if (str[0] == '+' || str[0] == '-')
                {
                    start = 1;
                }

                // Loop through each character to check if it's a digit
                for (size_t i = start; i < str.size(); ++i) 
                {
                    if (!std::isdigit(str[i])) 
                    {
                        // Non-numeric character found
                        valid = false;
                        break;
                    }
                }
            }

            return valid;
        }
        
        static int32 getMultiQualifierInt(const std::string& qualifier1, uint32 pos, const std::string& separator)
        { 
            std::vector<std::string> qualifiers = getMultiQualifiers(qualifier1, separator);
            if (qualifiers.size() > pos && isValidNumberString(qualifiers[pos]))
            {
                return stoi(qualifiers[pos]);
            }

            return 0;
        }

        static std::string getMultiQualifierStr(const std::string& qualifier1, uint32 pos, const std::string& separator)
        { 
            std::vector<std::string> qualifiers = getMultiQualifiers(qualifier1, separator);
            return (qualifiers.size() > pos) ? qualifiers[pos] : "";
        }
    
    protected:
        std::string qualifier;
    };

    template <class T>
    class NamedObjectFactory
    {
    protected:
        using ActionCreator = std::function<T* (PlayerbotAI* ai)>;
        std::map<std::string, ActionCreator> creators;

    public:
        T* Create(std::string_view name, PlayerbotAI* ai)
        {
            std::string_view nameView = name;
            std::string_view qualifierView;

            if (size_t pos = nameView.find("::"); pos != std::string::npos)
            {
                qualifierView = nameView.substr(pos + 2);
                nameView = nameView.substr(0, pos);
            }

            auto it = creators.find(std::string(nameView));
            if (it == creators.end())
                return nullptr;

            T* object = it->second(ai);
            if (object == nullptr)
                return nullptr;

            if (!qualifierView.empty())
            {
                if (auto* q = dynamic_cast<Qualified*>(object))
                    q->Qualify(std::string(qualifierView));
            }

            return object;
        }

        void GetSupportedKeys(std::set<std::string>& keys) const
        {
            for (const auto& entry : creators)
                keys.insert(entry.first);
        }
    };


    // [crash-fix 2026-10-16] 只有 AiNamedObject 那一族（Action/Trigger/UntypedValue）有
    // SetOwner/Register/Unregister；Strategy 继承的是 PlayerbotAIAware，没有这些。
    // 用 SFINAE 分流，避免给 Strategy 也强加这套接口。
    template<class T>
    inline void NamedObjectAttach(T* object, void* owner, std::true_type)
    {
        object->SetOwner(owner);
        object->Register();
    }

    template<class T>
    inline void NamedObjectAttach(T*, void*, std::false_type) {}

    template<class T>
    inline void NamedObjectDetach(T* object, std::true_type)
    {
        object->Unregister();
    }

    template<class T>
    inline void NamedObjectDetach(T*, std::false_type) {}

    template<class T>
    inline bool NamedObjectOwnedBy(T* object, void* owner, std::true_type)
    {
        return object->IsOwnedBy(owner);
    }

    template<class T>
    inline bool NamedObjectOwnedBy(T*, void*, std::false_type)
    {
        // 没有归属概念的 context（Strategy）：保持旧行为（Clear/Erase 时删掉它自己 new 的对象）。
        return true;
    }

    template <class T>
    class NamedObjectContext : public NamedObjectFactory<T>
    {
    public:
        using IsNamedObject = std::is_base_of<AiNamedObject, T>;

        NamedObjectContext(bool shared = false, bool supportsSiblings = false) :
            NamedObjectFactory<T>(), shared(shared), supportsSiblings(supportsSiblings) {}

        T* Create(std::string name, PlayerbotAI* ai)
        {
            // [crash-fix 2026-10-16] 原来这里是 created[name] = ...：等号左边的下标的求值先于右边，
            // 而 operator[] 会把 key 插进 map。若工厂返回 nullptr（未注册的名字），map 里就留下一个
            // 空指针条目 ⇒ 之后每次 look_up 都命中 nullptr，调用方拿到 nullptr 直接解引用（崩在 0）。
            // 改成"先创建、成功才登记"，并且不覆盖已有条目（覆盖会把旧对象泄漏/悬垂）。
            typename std::map<std::string, T*>::iterator i = created.find(name);
            if (i != created.end())
                return i->second;

            T* object = NamedObjectFactory<T>::Create(name, ai);
            if (object == nullptr)
                return nullptr;

            created[name] = object;

            // [crash-fix 2026-10-16] 登记归属 + 存活标记（WeakRef，见 Value.h）。
            NamedObjectAttach(object, (void*)this, IsNamedObject());

            return object;
        }

        virtual ~NamedObjectContext()
        {
            Clear();
        }

        void Clear()
        {
            for (typename std::map<std::string, T*>::iterator i = created.begin(); i != created.end(); i++)
            {
                if (i->second)
                {
                    NamedObjectDetach(i->second, IsNamedObject());
                    delete i->second;
                }
            }

            created.clear();
        }

        void Erase(const std::string& name)
        {
            typename std::map<std::string, T*>::iterator i = created.find(name);
            if (i == created.end())
                return;

            T* object = i->second;
            created.erase(i);

            if (object == nullptr)
                return;

            NamedObjectDetach(object, IsNamedObject());

            // [crash-fix 2026-10-16] 只删"本 context 真正 new 出来的"对象。
            // SharedValueContext 是共享的：本地 AiObjectContext 的 ClearValues("...") 会走到这里，
            // 以前会把共享对象 delete 掉，而 SharedValueContext::created 里还留着它的指针
            // ⇒ 双重释放 / 悬垂引用（正是线上 AI 取值链崩溃那一族）。共享对象只从自己的 map 上摘掉。
            if (!NamedObjectOwnedBy(object, (void*)this, IsNamedObject()))
                return;

            delete object;
        }

        void Update()
        {
            for (typename std::map<std::string, T*>::iterator i = created.begin(); i != created.end(); i++)
            {
                if (i->second)
                    i->second->Update();
            }
        }

        void Reset()
        {
            for (typename std::map<std::string, T*>::iterator i = created.begin(); i != created.end(); i++)
            {
                if (i->second)
                    i->second->Reset();
            }
        }

        bool IsShared() { return shared; }
        bool IsSupportsSiblings() { return supportsSiblings; }

        bool IsCreated(const std::string& name) { return created.find(name) != created.end(); }

        std::set<std::string> GetCreated()
        {
            std::set<std::string> keys;
            for (typename std::map<std::string, T*>::iterator it = created.begin(); it != created.end(); it++)
                keys.insert(it->first);
            return keys;
        }

    protected:
        std::map<std::string, T*> created;
        bool shared;
        bool supportsSiblings;
    };

    template <class T> class NamedObjectContextList
    {
    public:
        virtual ~NamedObjectContextList()
        {
            for (typename std::list<NamedObjectContext<T>*>::iterator i = contexts.begin(); i != contexts.end(); i++)
            {
                NamedObjectContext<T>* context = *i;
                if (!context->IsShared())
                    delete context;
            }
        }

        void Add(NamedObjectContext<T>* context)
        {
            contexts.push_back(context);
        }

        T* GetObject(const std::string& name, PlayerbotAI* ai)
        {
            for (typename std::list<NamedObjectContext<T>*>::iterator i = contexts.begin(); i != contexts.end(); i++)
            {
                T* object = (*i)->Create(name, ai);
                if (object) return object;
            }
            return NULL;
        }

        void Update()
        {
            for (typename std::list<NamedObjectContext<T>*>::iterator i = contexts.begin(); i != contexts.end(); i++)
            {
                if (!(*i)->IsShared())
                    (*i)->Update();
            }
        }

        void Reset()
        {
            for (typename std::list<NamedObjectContext<T>*>::iterator i = contexts.begin(); i != contexts.end(); i++)
            {
                (*i)->Reset();
            }
        }

        std::set<std::string> GetSiblings(const std::string& name)
        {
            std::set<std::string> siblings;
            for (typename std::list<NamedObjectContext<T>*>::iterator i = contexts.begin(); i != contexts.end(); i++)
            {
                if ((*i)->IsSupportsSiblings())
                {
                    std::set<std::string> supported;
                    (*i)->GetSupportedKeys(supported);
                    std::set<std::string>::iterator found = supported.find(name);
                    if (found != supported.end())
                    {
                        supported.erase(found);
                        siblings.insert(supported.begin(), supported.end());
                    }
                }
            }

            return siblings;
        }

        void GetSupportedKeys(std::set<std::string>& keys) const
        {
            for (typename std::list<NamedObjectContext<T>*>::const_iterator i = contexts.begin(); i != contexts.end(); i++)
            {
                (*i)->GetSupportedKeys(keys);
            }
        }

        bool IsCreated(const std::string& name) const
        {
            for (typename std::list<NamedObjectContext<T>*>::const_iterator i = contexts.begin(); i != contexts.end(); i++)
            {
                if ((*i)->IsCreated(name))
                    return true;
            }
            return false;
        }

        std::set<std::string> GetCreated()
        {
            std::set<std::string> result;

            for (typename std::list<NamedObjectContext<T>*>::iterator i = contexts.begin(); i != contexts.end(); i++)
            {
                std::set<std::string> createdKeys = (*i)->GetCreated();

                for (std::set<std::string>::iterator j = createdKeys.begin(); j != createdKeys.end(); j++)
                    result.insert(*j);
            }
            return result;
        }

        void Erase(const std::string& name)
        {
            for (typename std::list<NamedObjectContext<T>*>::iterator i = contexts.begin(); i != contexts.end(); i++)
            {
                (*i)->Erase(name);
            }
        }

    private:
        std::list<NamedObjectContext<T>*> contexts;
    };

    template <class T>
    class NamedObjectFactoryList
    {
    public:
        void Add(std::unique_ptr<NamedObjectFactory<T>> factory)
        {
            factories.emplace_back(std::move(factory));
        }

        T* GetObject(std::string_view name, PlayerbotAI* ai)
        {
            for (auto it = factories.rbegin(); it != factories.rend(); ++it)
            {
                if (T* obj = (*it)->Create(name, ai))
                    return obj;
            }
            return nullptr;
        }

    private:
        std::vector<std::unique_ptr<NamedObjectFactory<T>>> factories;
    };
};
