
#include "playerbot/playerbot.h"
#include "CustomStrategy.h"
#include <regex>

using namespace ai;

std::map<std::string, std::string> CustomStrategy::actionLinesCache;

// [BOSS 战策略 2026-10-16] 规则行里的名字必须 trim：
//   Helpers.cpp → split() 是 std::getline 版、**不 trim**，而 NextAction/TriggerNode 也直接存名字 ⇒
//   写成可读的 `触发器 > 动作`（两边带空格）时，名字会带尾/前导空格，GetTrigger()/GetAction() 查不到 ⇒ **静默失效**；
//   val:: 复合表达式末尾多个空格还会让 Qualified::getMultiQualifiers() 的 substr(1,size-2) 剥错括号，
//   把 `}` 留在最后一个元素里 → 该元素查不到值 → AI_VALUE 可能对 nullptr 解引用（崩服）。
//   这里统一 trim，两种写法（`a>b` 与 `a > b`）都能用。
static std::string TrimRuleToken(const std::string& s)
{
    size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return "";

    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

NextAction* toNextAction(std::string action)
{
    std::vector<std::string> tokens = split(action, '!');
    if (tokens.size() == 2 && !TrimRuleToken(tokens[0]).empty())
        return new NextAction(TrimRuleToken(tokens[0]), atof(tokens[1].c_str()));
    else if (tokens.size() == 1 && !TrimRuleToken(tokens[0]).empty())
        return new NextAction(TrimRuleToken(tokens[0]), ACTION_NORMAL);

    sLog.outError("Invalid action '%s'", action.c_str());
    return NULL;
}

NextAction** toNextActionArray(std::string actions)
{
    std::vector<std::string> tokens;
    tokens.push_back("");
    int stack = 0;
    for (char a : actions)
    {
       if (a == '{') ++stack;
       if (a == '}') --stack;
       if (a == ',' && stack == 0) tokens.push_back("");
       tokens.back() += a;
    }

    NextAction** res = new NextAction*[tokens.size() + 1];
    int index = 0;
    for (std::vector<std::string>::iterator i = tokens.begin(); i != tokens.end(); ++i)
    {
        NextAction* na = toNextAction(*i);
        if (na)
            res[index++] = na;
    }
	res[index++] = NULL;
    return res;
}

TriggerNode* toTriggerNode(std::string actionLine)
{
    std::vector<std::string> tokens = split(actionLine, '>');
    if (tokens.size() == 2)
        return new TriggerNode(TrimRuleToken(tokens[0]), toNextActionArray(tokens[1]));

    sLog.outError("Invalid action line '%s'", actionLine.c_str());
    return NULL;
}

void CustomStrategy::InitNonCombatTriggers(std::list<TriggerNode*> &triggers)
{
    if (actionLines.empty())
    {
        if (actionLinesCache[qualifier].empty())
        {
            LoadActionLines((uint32)ai->GetBot()->GetGUIDLow());
            if (this->actionLines.empty())
                LoadActionLines(0);
        }
        else
        {
            std::vector<std::string> tokens = split(actionLinesCache[qualifier], '\n');
            std::regex tpl("\\(NULL,\\s*'.+',\\s*'(.+)'\\)(,|;)");
            for (std::vector<std::string>::iterator i = tokens.begin(); i != tokens.end(); ++i)
            {
                std::string line = *i;
                for (std::sregex_iterator j = std::sregex_iterator(line.begin(), line.end(), tpl); j != std::sregex_iterator(); ++j)
                {
                    std::smatch match = *j;
                    std::string actionLine = match[1].str();
                    if (!actionLine.empty())
                        this->actionLines.push_back(actionLine);
                }
            }
        }
    }

    for (std::list<std::string>::iterator i = actionLines.begin(); i != actionLines.end(); ++i)
    {
        TriggerNode* tn = toTriggerNode(*i);
        if (tn)
            triggers.push_back(tn);
    }
}

void CustomStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{
    InitNonCombatTriggers(triggers);
}

void CustomStrategy::LoadActionLines(uint32 owner)
{
    auto results = CharacterDatabase.PQuery("SELECT action_line FROM ai_playerbot_custom_strategy WHERE name = '%s' and owner = '%u' order by idx",
            qualifier.c_str(), owner);
    if (results)
    {
        do
        {
            Field* fields = results->Fetch();
            std::string action = fields[0].GetString();
            this->actionLines.push_back(action);
        } while (results->NextRow());
    }
}

void CustomStrategy::Reset()
{
    actionLines.clear();
    actionLinesCache[qualifier].clear();
}