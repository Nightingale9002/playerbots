
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/playerbot.h"
#include "RandomPlayerbotFactory.h"
#include "Accounts/AccountMgr.h"
#include "playerbot/PlayerbotFactory.h"
#include "RandomItemMgr.h"
#include "World/WorldState.h"
#include "playerbot/PlayerbotHelpMgr.h"
#include "playerbot/strategy/actions/CheatAction.h"

#include "playerbot/TravelMgr.h"
#include "playerbot/strategy/values/SharedValueContext.h"
#include "playerbot/strategy/values/TravelValues.h"

#include <iostream>
#include <numeric>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <boost/algorithm/string.hpp>
#include <regex>
#include <fstream>
#include <sstream>
#include "PlayerbotLoginMgr.h"

#if PLATFORM == PLATFORM_WINDOWS
#include <windows.h>
#include <psapi.h>
#else
#include <cstdio>
#include <unistd.h>
#endif

// [诊断 2026-10-15] 启动/登录期内存打点：只在 PB_RSS_LOG=1 时输出，不开就是空操作。
// 用途：把"每阶段吃多少 RSS"量化出来，避免凭猜测改代码。上云前本机验证。
void PlayerbotLogRss(const char* tag)
{
    static bool enabled = (std::getenv("PB_RSS_LOG") != nullptr);
    if (!enabled)
        return;

    uint32 rssMb = 0;
#if PLATFORM == PLATFORM_WINDOWS
    PROCESS_MEMORY_COUNTERS pmc;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        rssMb = (uint32)(pmc.WorkingSetSize / (1024 * 1024));
#else
    if (FILE* f = fopen("/proc/self/statm", "r"))
    {
        unsigned long total = 0, resident = 0;
        if (fscanf(f, "%lu %lu", &total, &resident) == 2)
            rssMb = (uint32)((resident * (unsigned long)sysconf(_SC_PAGESIZE)) / (1024 * 1024));
        fclose(f);
    }
#endif
    sLog.outString("[RSS] %-34s %6u MB", tag, rssMb);
}

std::vector<std::string> ConfigAccess::GetValues(const std::string& name) const
{
    std::vector<std::string> values;
    auto const nameLower = boost::algorithm::to_lower_copy(name);
    for (auto entry : m_entries)
        if (entry.first.find(nameLower) != std::string::npos)
            values.push_back(entry.first);
    return values;
};
INSTANTIATE_SINGLETON_1(PlayerbotAIConfig);
PlayerbotAIConfig::PlayerbotAIConfig()
: enabled(false)
{
}
template <class T>
void LoadList(std::string value, T &list)
{
    list.clear();
    std::vector<std::string> ids = split(value, ',');
    for (std::vector<std::string>::iterator i = ids.begin(); i != ids.end(); i++)
    {
        std::string string = *i;
        if (string.empty())
            continue;

        uint32 id = atoi(string.c_str());

        list.push_back(id);
    }
}

template <class T>
void LoadListString(std::string value, T& list)
{
    list.clear();
    std::vector<std::string> strings = split(value, ',');
    for (std::vector<std::string>::iterator i = strings.begin(); i != strings.end(); i++)
    {
        std::string string = *i;
        if (string.empty())
            continue;

        list.push_back(string);
    }
}

inline ParsedUrl parseUrl(const std::string& url) {
    std::regex urlRegex(R"((http|https)://([^:/]+)(:([0-9]+))?(/.*)?)");
    std::smatch match;
    if (!std::regex_match(url, match, urlRegex)) {
        throw std::invalid_argument("Invalid URL format");
    }

    ParsedUrl parsed;
    parsed.hostname = match[2];
    parsed.https = match[1] == "https";
    parsed.port = parsed.https ? 443 : (match[4].length() ? std::stoi(match[4]) : 80);
    parsed.path = match[5].length() ? match[5] : std::string("/");
    return parsed;
}

bool PlayerbotAIConfig::Initialize()
{
    sLog.outString("Initializing AI Playerbot by ike3, based on the original Playerbot by blueboy");
    PlayerbotLogRss("core: world loaded (bot init start)");

    if (!config.SetSource(_D_AIPLAYERBOT_CONFIG, "PlayerBots_"))
    {
        sLog.outString("AI Playerbot is Disabled. Unable to open configuration file aiplayerbot.conf");
        return false;
    }

    enabled = config.GetBoolDefault("AiPlayerbot.Enabled", false);
    if (!enabled)
    {
        sLog.outString("AI Playerbot is Disabled in aiplayerbot.conf");
        return false;
    }

    ConfigAccess* configA = reinterpret_cast<ConfigAccess*>(&config);

    BarGoLink::SetOutputState(config.GetBoolDefault("AiPlayerbot.ShowProgressBars", false));
    globalCoolDown = (uint32) config.GetIntDefault("AiPlayerbot.GlobalCooldown", 500);
    maxWaitForMove = config.GetIntDefault("AiPlayerbot.MaxWaitForMove", 3000);
    expireActionTime = config.GetIntDefault("AiPlayerbot.ExpireActionTime", 5000);
    dispelAuraDuration = config.GetIntDefault("AiPlayerbot.DispelAuraDuration", 2000);
    reactDelay = (uint32) config.GetIntDefault("AiPlayerbot.ReactDelay", 100);
    passiveDelay = (uint32) config.GetIntDefault("AiPlayerbot.PassiveDelay", 4000);
    repeatDelay = (uint32) config.GetIntDefault("AiPlayerbot.RepeatDelay", 5000);
    errorDelay = (uint32) config.GetIntDefault("AiPlayerbot.ErrorDelay", 5000);
    rpgDelay = (uint32) config.GetIntDefault("AiPlayerbot.RpgDelay", 3000);
    sitDelay = (uint32) config.GetIntDefault("AiPlayerbot.SitDelay", 30000);
    returnDelay = (uint32) config.GetIntDefault("AiPlayerbot.ReturnDelay", 7000);
    lootDelay = (uint32)config.GetIntDefault("AiPlayerbot.LootDelayDelay", 750);

    farDistance = config.GetFloatDefault("AiPlayerbot.FarDistance", 20.0f);
    sightDistance = config.GetFloatDefault("AiPlayerbot.SightDistance", 75.0f);
    spellDistance = config.GetFloatDefault("AiPlayerbot.SpellDistance", 25.0f);
    shootDistance = config.GetFloatDefault("AiPlayerbot.ShootDistance", 25.0f);
    healDistance = config.GetFloatDefault("AiPlayerbot.HealDistance", 125.0f);
    reactDistance = config.GetFloatDefault("AiPlayerbot.ReactDistance", 150.0f);
    maxFreeMoveDistance = config.GetFloatDefault("AiPlayerbot.MaxFreeMoveDistance", 150.0f);
    freeMoveDelay = config.GetFloatDefault("AiPlayerbot.FreeMoveDelay", 30.0f);
    grindDistance = config.GetFloatDefault("AiPlayerbot.GrindDistance", 75.0f);
    aggroDistance = config.GetFloatDefault("AiPlayerbot.AggroDistance", 22.0f);
    lootDistance = config.GetFloatDefault("AiPlayerbot.LootDistance", 25.0f);
    groupMemberLootDistance = config.GetFloatDefault("AiPlayerbot.GroupMemberLootDistance", 15.0f);
    groupMemberLootDistanceWithActiveMaster = config.GetFloatDefault("AiPlayerbot.GroupMemberLootDistanceWithActiveMaster", 10.0f);
    gatheringDistance = config.GetFloatDefault("AiPlayerbot.GatheringDistance", 15.0f);
    groupMemberGatheringDistance = config.GetFloatDefault("AiPlayerbot.GroupMemberGatheringDistance", 10.0f);
    groupMemberGatheringDistanceWithActiveMaster = config.GetFloatDefault("AiPlayerbot.GroupMemberGatheringDistanceWithActiveMaster", 5.0f);
    fleeDistance = config.GetFloatDefault("AiPlayerbot.FleeDistance", 8.0f);
    tooCloseDistance = config.GetFloatDefault("AiPlayerbot.TooCloseDistance", 5.0f);
    meleeDistance = config.GetFloatDefault("AiPlayerbot.MeleeDistance", 1.5f);
    followDistance = config.GetFloatDefault("AiPlayerbot.FollowDistance", 1.5f);
    raidFollowDistance = config.GetFloatDefault("AiPlayerbot.RaidFollowDistance", 5.0f);
    wanderMinDistance = config.GetFloatDefault("AiPlayerbot.WanderMinDistance", 5.0f);
    wanderMaxDistance = config.GetFloatDefault("AiPlayerbot.WanderMaxDistance", 50.0f);
    whisperDistance = config.GetFloatDefault("AiPlayerbot.WhisperDistance", 6000.0f);
    contactDistance = config.GetFloatDefault("AiPlayerbot.ContactDistance", 0.5f);
    aoeRadius = config.GetFloatDefault("AiPlayerbot.AoeRadius", 5.0f);
    rpgDistance = config.GetFloatDefault("AiPlayerbot.RpgDistance", 80.0f);
    proximityDistance = config.GetFloatDefault("AiPlayerbot.ProximityDistance", 20.0f);
    walkDistance = config.GetFloatDefault("AiPlayerbot.WalkDistance", 5.0f);

    criticalHealth = config.GetIntDefault("AiPlayerbot.CriticalHealth", 20);
    lowHealth = config.GetIntDefault("AiPlayerbot.LowHealth", 50);
    mediumHealth = config.GetIntDefault("AiPlayerbot.MediumHealth", 70);
    almostFullHealth = config.GetIntDefault("AiPlayerbot.AlmostFullHealth", 90);
    lowMana = config.GetIntDefault("AiPlayerbot.LowMana", 15);
    mediumMana = config.GetIntDefault("AiPlayerbot.MediumMana", 40);

    randomGearMaxLevel = config.GetIntDefault("AiPlayerbot.RandomGearMaxLevel", 500);
    randomGearMaxDiff = config.GetIntDefault("AiPlayerbot.RandomGearMaxDiff", 9);
    randomGearUpgradeEnabled = config.GetBoolDefault("AiPlayerbot.RandomGearUpgradeEnabled", false);
    staticBots = config.GetBoolDefault("AiPlayerbot.StaticBots", false);
    equipBestGear = config.GetBoolDefault("AiPlayerbot.EquipBestGear", false);
    randomGearTabards = config.GetBoolDefault("AiPlayerbot.RandomGearTabards", false);
    randomGearTabardsChance = config.GetFloatDefault("AiPlayerbot.RandomGearTabardsChance", 0.1f);
    randomGearTabardsReplaceGuild = config.GetBoolDefault("AiPlayerbot.RandomGearTabardsReplaceGuild", false);
    randomGearTabardsUnobtainable = config.GetBoolDefault("AiPlayerbot.RandomGearTabardsUnobtainable", false);
    LoadList<std::list<uint32> >(config.GetStringDefault("AiPlayerbot.RandomGearBlacklist", ""), randomGearBlacklist);
    LoadList<std::list<uint32> >(config.GetStringDefault("AiPlayerbot.RandomGearWhitelist", ""), randomGearWhitelist);
    randomGearProgression = config.GetBoolDefault("AiPlayerbot.RandomGearProgression", true);
    randomGearLoweringChance = config.GetFloatDefault("AiPlayerbot.RandomGearLoweringChance", 0.15f);
    randomBotMaxLevelChance = config.GetFloatDefault("AiPlayerbot.RandomBotMaxLevelChance", 0.15f);
    rollBadItemsWithPlayer = config.GetBoolDefault("AiPlayerbot.RollBadItemsWithPlayer", false);
    // [不抢装备 2026-10-14] 默认开启：组里有真人玩家时不参与装备竞拍
    passRollsWithRealPlayer = config.GetBoolDefault("AiPlayerbot.PassRollsWithRealPlayer", true);
    randomBotRpgChance = config.GetFloatDefault("AiPlayerbot.RandomBotRpgChance", 0.35f);
    usePotionChance = config.GetFloatDefault("AiPlayerbot.UsePotionChance", 1.0f);
    attackEmoteChance = config.GetFloatDefault("AiPlayerbot.AttackEmoteChance", 0.0f);

    jumpNoCombatChance = config.GetFloatDefault("AiPlayerbot.JumpNoCombatChance", 0.5f);
    jumpMeleeInCombatChance = config.GetFloatDefault("AiPlayerbot.JumpMeleeInCombatChance", 0.5f);
    jumpRandomChance = config.GetFloatDefault("AiPlayerbot.JumpRandomChance", 0.20f);
    jumpInPlaceChance = config.GetFloatDefault("AiPlayerbot.JumpInPlaceChance", 0.50f);
    jumpBackwardChance = config.GetFloatDefault("AiPlayerbot.JumpBackwardChance", 0.10f);
    jumpHeightLimit = config.GetFloatDefault("AiPlayerbot.JumpHeightLimit", 60.f);
    jumpVSpeed = config.GetFloatDefault("AiPlayerbot.JumpVSpeed", 7.96f);
    jumpHSpeed = config.GetFloatDefault("AiPlayerbot.JumpHSpeed", 7.0f);
    jumpInBg = config.GetBoolDefault("AiPlayerbot.JumpInBg", false);
    jumpWithPlayer = config.GetBoolDefault("AiPlayerbot.JumpWithPlayer", false);
    jumpFollow = config.GetBoolDefault("AiPlayerbot.JumpFollow", true);
    jumpChase = config.GetBoolDefault("AiPlayerbot.JumpChase", true);
    useKnockback = config.GetBoolDefault("AiPlayerbot.UseKnockback", true);

    iterationsPerTick = config.GetIntDefault("AiPlayerbot.IterationsPerTick", 100);

    allowGuildBots = config.GetBoolDefault("AiPlayerbot.AllowGuildBots", true);
    allowMultiAccountAltBots = config.GetBoolDefault("AiPlayerbot.AllowMultiAccountAltBots", true);

    randomBotMapsAsString = config.GetStringDefault("AiPlayerbot.RandomBotMaps", "0,1,530,571");
    LoadList<std::vector<uint32> >(randomBotMapsAsString, randomBotMaps);
    LoadList<std::list<uint32> >(config.GetStringDefault("AiPlayerbot.RandomBotQuestItems", "6948,5175,5176,5177,5178,16309,12382,13704,11000,22754"), randomBotQuestItems);
    LoadList<std::list<uint32> >(config.GetStringDefault("AiPlayerbot.RandomBotSpellIds", "54197"), randomBotSpellIds);
	LoadList<std::list<uint32> >(config.GetStringDefault("AiPlayerbot.PvpProhibitedZoneIds", "2255,656,2361,2362,2363,976,35,2268,3425,392,541,1446,3828,3712,3738,3565,3539,3623,4152,3988,4658,4284,4418,4436,4275,4323"), pvpProhibitedZoneIds);

#ifndef MANGOSBOT_ZERO
    // disable pvp near dark portal if event is active
    if (sWorldState.GetExpansion() == EXPANSION_NONE)
        pvpProhibitedZoneIds.insert(pvpProhibitedZoneIds.begin(), 72);
#endif

    LoadList<std::list<uint32> >(config.GetStringDefault("AiPlayerbot.RandomBotQuestIds", "7848,3802,5505,6502,7761,9378"), randomBotQuestIds);
    LoadList<std::list<uint32> >(config.GetStringDefault("AiPlayerbot.ImmuneSpellIds", ""), immuneSpellIds);

    botAutologin = BotAutoLogin(config.GetIntDefault("AiPlayerbot.BotAutologin", 0));
    randomBotAutologin = config.GetBoolDefault("AiPlayerbot.RandomBotAutologin", true);
    randomBotAutoCreate = config.GetBoolDefault("AiPlayerbot.RandomBotAutoCreate", true);
    minRandomBots = config.GetIntDefault("AiPlayerbot.MinRandomBots", 50);
    maxRandomBots = config.GetIntDefault("AiPlayerbot.MaxRandomBots", 200);
    randomBotUpdateInterval = config.GetIntDefault("AiPlayerbot.RandomBotUpdateInterval", 1 * 1000);
    randomBotCountChangeMinInterval = config.GetIntDefault("AiPlayerbot.RandomBotCountChangeMinInterval", 1 * 1800);
    randomBotCountChangeMaxInterval = config.GetIntDefault("AiPlayerbot.RandomBotCountChangeMaxInterval", 2 * 3600);
    randomBotTimedLogout = config.GetBoolDefault("AiPlayerbot.RandomBotTimedLogout", true);
    randomBotTimedOffline = config.GetBoolDefault("AiPlayerbot.RandomBotTimedOffline", false);
    minRandomBotInWorldTime = config.GetIntDefault("AiPlayerbot.MinRandomBotInWorldTime", 1 * 1800);
    maxRandomBotInWorldTime = config.GetIntDefault("AiPlayerbot.MaxRandomBotInWorldTime", 6 * 3600);
    minRandomBotRandomizeTime = config.GetIntDefault("AiPlayerbot.MinRandomBotRandomizeTime", 6 * 3600);
    maxRandomBotRandomizeTime = config.GetIntDefault("AiPlayerbot.MaxRandomRandomizeTime", 24 * 3600);
    minRandomBotChangeStrategyTime = config.GetIntDefault("AiPlayerbot.MinRandomBotChangeStrategyTime", 1800);
    maxRandomBotChangeStrategyTime = config.GetIntDefault("AiPlayerbot.MaxRandomBotChangeStrategyTime", 2 * 3600);
    minRandomBotReviveTime = config.GetIntDefault("AiPlayerbot.MinRandomBotReviveTime", 60);
    maxRandomBotReviveTime = config.GetIntDefault("AiPlayerbot.MaxRandomReviveTime", 300);
    enableRandomTeleports = config.GetBoolDefault("AiPlayerbot.EnableRandomTeleports", true);
    enableMinimalMove = config.GetBoolDefault("AiPlayerbot.EnableMinimalMove", true);
    
    randomBotTeleportDistance = config.GetIntDefault("AiPlayerbot.RandomBotTeleportDistance", 1000);
    // [spawn 2026-10-16] 站长口径：bot 固定出生点（沙塔斯 A'dal：530 / -1863.7 / 5430.0 / -9.7）
    fixedSpawnEnabled = config.GetBoolDefault("AiPlayerbot.FixedSpawn.Enable", false);
    fixedSpawnMapId = (uint32)config.GetIntDefault("AiPlayerbot.FixedSpawn.MapId", 530);
    fixedSpawnX = config.GetFloatDefault("AiPlayerbot.FixedSpawn.X", -1863.7f);
    fixedSpawnY = config.GetFloatDefault("AiPlayerbot.FixedSpawn.Y", 5430.0f);
    fixedSpawnZ = config.GetFloatDefault("AiPlayerbot.FixedSpawn.Z", -9.7f);
    fixedSpawnO = config.GetFloatDefault("AiPlayerbot.FixedSpawn.O", 3.72f);
    fixedSpawnRadius = config.GetFloatDefault("AiPlayerbot.FixedSpawn.Radius", 25.0f);
    if (fixedSpawnEnabled)
        sLog.outString("[SPAWN] fixed bot spawn: map %u (%.1f, %.1f, %.1f) r=%.0f",
            fixedSpawnMapId, fixedSpawnX, fixedSpawnY, fixedSpawnZ, fixedSpawnRadius);
    transportTeleportType = config.GetIntDefault("AiPlayerbot.TransportTeleportType", 2);
    randomBotTeleportNearPlayer = config.GetBoolDefault("AiPlayerbot.RandomBotTeleportNearPlayer", false);
    randomBotTeleportNearPlayerMaxAmount = config.GetIntDefault("AiPlayerbot.RandomBotTeleportNearPlayerMaxAmount", 0);
    randomBotTeleportNearPlayerMaxAmountRadius = config.GetFloatDefault("AiPlayerbot.RandomBotTeleportNearPlayerMaxAmountRadius", 0.0f);
    randomBotTeleportMinInterval = config.GetIntDefault("AiPlayerbot.RandomBotTeleportTeleportMinInterval", 2 * 3600);
    randomBotTeleportMaxInterval = config.GetIntDefault("AiPlayerbot.RandomBotTeleportTeleportMaxInterval", 48 * 3600);
    randomBotsMaxLoginsPerInterval = config.GetIntDefault("AiPlayerbot.RandomBotsMaxLoginsPerInterval", 10);
    loginDatabaseDelayMs = config.GetIntDefault("AiPlayerbot.LoginDatabaseDelayMs", 100);
    randomBotsPerInterval = config.GetIntDefault("AiPlayerbot.RandomBotsPerInterval", 0);
    minRandomBotsPriceChangeInterval = config.GetIntDefault("AiPlayerbot.MinRandomBotsPriceChangeInterval", 2 * 3600);
    maxRandomBotsPriceChangeInterval = config.GetIntDefault("AiPlayerbot.MaxRandomBotsPriceChangeInterval", 48 * 3600);
    //Auction house settings
    shouldQueryAHListingsOutsideOfAH = config.GetBoolDefault("AiPlayerbot.ShouldQueryAHListingsOutsideOfAH", true);
    LoadList<std::list<uint32> >(config.GetStringDefault("AiPlayerbot.AhOverVendorItemIds", ""), ahOverVendorItemIds);
    LoadList<std::list<uint32> >(config.GetStringDefault("AiPlayerbot.VendorOverAHItemIds", ""), vendorOverAHItemIds);
    botCheckAllAuctionListings = config.GetBoolDefault("AiPlayerbot.BotCheckAllAuctionListings", false);
    botsSaveEpics = config.GetBoolDefault("AiPlayerbot.BotsSaveEpics", true);
    //
    randomBotJoinLfg = config.GetBoolDefault("AiPlayerbot.RandomBotJoinLfg", true);
    logRandomBotJoinLfg = config.GetBoolDefault("AiPlayerbot.LogRandomBotJoinLfg", false);
    randomBotJoinBG = config.GetBoolDefault("AiPlayerbot.RandomBotJoinBG", true);
    randomBotAutoJoinBG = config.GetBoolDefault("AiPlayerbot.RandomBotAutoJoinBG", false);
    randomBotBracketCount = config.GetIntDefault("AiPlayerbot.RandomBotBracketCount", 3);
    logInGroupOnly = config.GetBoolDefault("AiPlayerbot.LogInGroupOnly", true);
    logValuesPerTick = config.GetBoolDefault("AiPlayerbot.LogValuesPerTick", false);
    actionHistorySize = (uint32)config.GetIntDefault("AiPlayerbot.ActionHistorySize", 0);
    fleeingEnabled = config.GetBoolDefault("AiPlayerbot.FleeingEnabled", true);
    summonAtInnkeepersEnabled = config.GetBoolDefault("AiPlayerbot.SummonAtInnkeepersEnabled", true);
    randomBotMinLevel = config.GetIntDefault("AiPlayerbot.RandomBotMinLevel", 1);
    randomBotMaxLevel = config.GetIntDefault("AiPlayerbot.RandomBotMaxLevel", 255);
    randomBotLoginAtStartup = config.GetBoolDefault("AiPlayerbot.RandomBotLoginAtStartup", true);
    randomBotTeleLevel = config.GetIntDefault("AiPlayerbot.RandomBotTeleLevel", 5);
    openGoSpell = config.GetIntDefault("AiPlayerbot.OpenGoSpell", 6477);

    randomChangeMultiplier = config.GetFloatDefault("AiPlayerbot.RandomChangeMultiplier", 1.0);

    randomBotCombatStrategies = config.GetStringDefault("AiPlayerbot.RandomBotCombatStrategies", "-threat,+custom::say");
    randomBotNonCombatStrategies = config.GetStringDefault("AiPlayerbot.RandomBotNonCombatStrategies", "+custom::say");
    randomBotReactStrategies = config.GetStringDefault("AiPlayerbot.RandomBotReactStrategies", "");
    randomBotDeadStrategies = config.GetStringDefault("AiPlayerbot.RandomBotDeadStrategies", "");
    combatStrategies = config.GetStringDefault("AiPlayerbot.CombatStrategies", "");
    nonCombatStrategies = config.GetStringDefault("AiPlayerbot.NonCombatStrategies", "+return,+delayed roll");
    reactStrategies = config.GetStringDefault("AiPlayerbot.ReactStrategies", "");
    deadStrategies = config.GetStringDefault("AiPlayerbot.DeadStrategies", "");

    commandPrefix = config.GetStringDefault("AiPlayerbot.CommandPrefix", "");
    commandSeparator = config.GetStringDefault("AiPlayerbot.CommandSeparator", "\\\\");

    // [.bot 命令闸门 2026-10-17 站长口径] 非 GM 玩家的 bot 命令黑/白名单（见 PlayerbotAIConfig.h 注释）。
    //   黑名单默认：加/删机器人、self（把自己变 bot）、always（常驻）、踢人、公会级操作。
    //   白名单默认：站长口径那一批 —— 跟随/站位/攻击/拉怪/标记/策略(co)/施法/复活/拾取/掷点/修理/
    //   训练/天赋/银行/读状态/说话/入队离队移交队长/summon，外加 .bot 里只影响自己机器人外观与补给的
    //   子命令（list/help/gear/prepare/train/food/pet…）。**未列出的已注册命令一律拒绝**。
    LoadListString<std::vector<std::string>>(config.GetStringDefault("AiPlayerbot.PlayerCommandBlacklist",
        "add,self,always,delete,remove,kick,guild"), playerCommandBlacklist);
    LoadListString<std::vector<std::string>>(config.GetStringDefault("AiPlayerbot.PlayerCommandWhitelist",
        "follow,stay,guard,free,wander,flee,runaway,grind,position,formation,stance,move style,"
        "follow target,boost target,max dps,tank attack,"
        "attack,attack rti,attack rti target,attack my target,pull,pull rti,rti,"
        "possible attack targets,attackers,avoid creature,chat,save mana,wait for attack time,"
        "cast,cast custom nc spell,spell,spells,buff,buff target,focus heal,glyph,pet,"
        "release,revive,revive target,self res,spirit healer,repop,"
        "loot,add all loot,roll,keep,use,item count,items,inv,equip,equip upgrades,unequip,"
        "trade,sell,buy,buy back,reward,repair,trainer,talents,bank,"
        "stats,range,quests,reputation,los,help,who,where,log,speak,emote,wts,rep,"
        "invite,join,leave,lfg,give leader,accept,ready,talk,summon,"
        "go,jump,skill,faction,home,taxi,teleport,doquest,query quest,query item usage,quest reward,share,drop,"
        "clean quest log,outfit,warning,"
        // [2026-10-20 站长口径] `.bot prio <0-5>`：GM 和非 GM 都能用（每次调用写 [BOTCMD] 日志留痕）
        "prio,"
        "list,gear,prepare,prep,init,train,learn,food,drink,potions,pots,consumes,consumables,consums,"
        "regs,reg,reagents,enchants,ammo"), playerCommandWhitelist);

    // 统一转小写（大小写不敏感）；只去掉首尾空白，**不能**去掉词中间的空格
    // （白名单里有 "move style"/"give leader"/"clean quest log" 这类多词命令）
    auto toLowerList = [](std::vector<std::string>& list)
    {
        for (auto& s : list)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });

            size_t b = s.find_first_not_of(" \t\r\n");
            size_t e = s.find_last_not_of(" \t\r\n");
            s = (b == std::string::npos) ? std::string() : s.substr(b, e - b + 1);
        }
    };
    toLowerList(playerCommandBlacklist);
    toLowerList(playerCommandWhitelist);

    commandServerPort = config.GetIntDefault("AiPlayerbot.CommandServerPort", 0);
    perfMonEnabled = config.GetBoolDefault("AiPlayerbot.PerfMonEnabled", false);
    bExplicitDbStoreSave = config.GetBoolDefault("AiPlayerbot.ExplicitDbStoreSave", false);

    randomBotLoginWithPlayer = config.GetBoolDefault("AiPlayerbot.RandomBotLoginWithPlayer", false);
    asyncBotLogin = config.GetBoolDefault("AiPlayerbot.AsyncBotLogin", false);
    preloadHolders = config.GetBoolDefault("AiPlayerbot.PreloadHolders", false);
    
    freeRoomForNonSpareBots = config.GetIntDefault("AiPlayerbot.FreeRoomForNonSpareBots", 1);

    loginBotsNearPlayerRange = config.GetIntDefault("AiPlayerbot.LoginBotsNearPlayerRange", 1000);
    
    LoadListString<std::vector<std::string> >(config.GetStringDefault("AiPlayerbot.DefaultLoginCriteria", "maxbots,spareroom,offline"), defaultLoginCriteria);

    std::vector<std::string> criteriaValues = configA->GetValues("AiPlayerbot.LoginCriteria");
    std::sort(criteriaValues.begin(), criteriaValues.end());
    loginCriteria.clear();
    for (auto& value : criteriaValues)
    {
        loginCriteria.push_back({});
        LoadListString<std::vector<std::string> >(config.GetStringDefault(value, ""), loginCriteria.back());
    }

    if (criteriaValues.empty())
    {
        loginCriteria.push_back({ "group" });
        loginCriteria.push_back({ "arena" });
        loginCriteria.push_back({ "bg" });
        loginCriteria.push_back({ "guild" });
        loginCriteria.push_back({ "logoff,classrace,level,online" });
        loginCriteria.push_back({ "logoff,classrace,level" });
        loginCriteria.push_back({ "logoff,classrace" });
    }

#ifdef GenerateBotTests
    startupRunTests.clear();
    startupRunTestsPending = false;

    std::vector<std::string> runTestValues = configA->GetValues("AiPlayerbot.RunTest");
    std::sort(runTestValues.begin(), runTestValues.end(), [](const std::string& left, const std::string& right)
    {
        auto readRunTestIndex = [](const std::string& key, uint32& index) -> bool
        {
            const size_t dotPos = key.find_last_of('.');
            const std::string suffix = dotPos == std::string::npos ? key : key.substr(dotPos + 1);

            if (suffix.find("runtest") != 0)
                return false;

            const std::string number = suffix.substr(7);
            if (number.empty())
                return false;

            if (!std::all_of(number.begin(), number.end(), ::isdigit))
                return false;

            index = static_cast<uint32>(std::stoul(number));
            return true;
        };

        uint32 leftIndex = 0;
        uint32 rightIndex = 0;
        const bool leftHasIndex = readRunTestIndex(left, leftIndex);
        const bool rightHasIndex = readRunTestIndex(right, rightIndex);

        if (leftHasIndex != rightHasIndex)
            return leftHasIndex;

        if (leftHasIndex && leftIndex != rightIndex)
            return leftIndex < rightIndex;

        return left < right;
    });

    for (const std::string& runTestKey : runTestValues)
    {
        std::string runTestParam = boost::algorithm::trim_copy(config.GetStringDefault(runTestKey, ""));
        if (!runTestParam.empty())
            startupRunTests.push_back(runTestParam);
    }

    startupRunTestsPending = !startupRunTests.empty();
    if (startupRunTestsPending)
        sLog.outString("Loaded %u startup runtest entries from config", static_cast<uint32>(startupRunTests.size()));
#endif
    

    for (uint32 level = 1; level <= DEFAULT_MAX_LEVEL; ++level)
    {
        levelProbability[level] = config.GetIntDefault("AiPlayerbot.LevelProbability." + std::to_string(level), 100);
    }

    sLog.outString("Loading Race/Class probabilities");

    classRaceProbabilityTotal = 0;

    useFixedClassRaceCounts = config.GetBoolDefault("AiPlayerbot.ClassRace.UseFixedClassRaceCounts", false);
    RandomPlayerbotFactory factory(0);

    for (uint32 race = 1; race < MAX_RACES; ++race)
    {
        //Set race defaults
        if (race > 0)
        {
            int rProb = config.GetIntDefault("AiPlayerbot.ClassRaceProb.0." + std::to_string(race), 100);

            for (uint32 cls = 1; cls < MAX_CLASSES; ++cls)
            {
                classRaceProbability[cls][race] = rProb;
            }
        }
    }

    //Class overrides
    for (uint32 cls = 1; cls < MAX_CLASSES; ++cls)
    {
        int cProb = config.GetIntDefault("AiPlayerbot.ClassRaceProb." + std::to_string(cls), -1);

        if (cProb >= 0)
        {
            for (uint32 race = 1; race < MAX_RACES; ++race)
            {
                classRaceProbability[cls][race] = cProb;
            }
        }
    }

    //Race Class overrides
    for (uint32 race = 1; race < MAX_RACES; ++race)
    {
        for (uint32 cls = 1; cls < MAX_CLASSES; ++cls)
        {
            int rcProb = config.GetIntDefault("AiPlayerbot.ClassRaceProb." + std::to_string(cls) + "." + std::to_string(race), -1);
            if (rcProb >= 0)
                classRaceProbability[cls][race] = rcProb;

            if (!factory.isAvailableRace(cls, race))
                classRaceProbability[cls][race] = 0;
            else
                classRaceProbabilityTotal += classRaceProbability[cls][race];
        }
    }

    if (useFixedClassRaceCounts)
    {

        // Warn about unsupported config keys
        for (uint32 race = 1; race < MAX_RACES; ++race)
        {
            std::string raceKey = "AiPlayerbot.ClassRaceProb.0." + std::to_string(race);
            int val = config.GetIntDefault(raceKey.c_str(), -1);
            if (val >= 0)
                sLog.outError("Fixed class/race counts does not yet support '%s' (race-only). This config entry will be ignored.", raceKey.c_str());
        }

        for (uint32 cls = 1; cls < MAX_CLASSES; ++cls)
        {
            std::string classKey = "AiPlayerbot.ClassRaceProb." + std::to_string(cls);
            int val = config.GetIntDefault(classKey.c_str(), -1);
            if (val >= 0)
                sLog.outError("Fixed class/race counts does not yet support '%s' (class-only). This config entry will be ignored.", classKey.c_str());
        }

        //Parse and build fixedClassRacesCounts
        {
            for (uint32 cls = 1; cls < MAX_CLASSES; ++cls)
	    {
	        for (uint32 race = 1; race < MAX_RACES; ++race)
	        {
		    std::string key = "AiPlayerbot.ClassRaceProb." + std::to_string(cls) + "." + std::to_string(race);
		    int count = config.GetIntDefault(key, -1);

		    if (count >= 0 && factory.isAvailableRace(cls, race))
		    {
		        fixedClassRaceCounts[{cls, race}] = count;
		    }
	        }
	    }
        }
    }

    // [roster 2026-10-16] 站长口径：AiPlayerbot.PremadeSpecPick.<cls>.<race> = <specNo>
    for (uint32 cls = 0; cls < MAX_CLASSES; ++cls)
        for (uint32 race = 0; race < MAX_RACES; ++race)
            specPick[cls][race] = -1;
    for (uint32 cls = 1; cls < MAX_CLASSES; ++cls)
    {
        for (uint32 race = 1; race < MAX_RACES; ++race)
        {
            std::string key = "AiPlayerbot.PremadeSpecPick." + std::to_string(cls) + "." + std::to_string(race);
            int32 pick = config.GetIntDefault(key.c_str(), -1);
            if (pick >= 0)
            {
                specPick[cls][race] = pick;
                sLog.outString("[ROSTER] class %u race %u -> premade spec %d", cls, race, pick);
            }
        }
    }

    // [gems 2026-10-16] 站长口径：AiPlayerbot.Gems.<职业id> = 宝石物品id,...
    for (uint32 cls = 1; cls < MAX_CLASSES; ++cls)
    {
        std::string key = "AiPlayerbot.Gems." + std::to_string(cls);
        std::string val = config.GetStringDefault(key.c_str(), "");
        if (!val.empty())
            LoadList<std::vector<uint32>>(val, gemIds[cls]);
    }

    botCheatMask = uint32(CheatAction::GetCheatMask(config.GetStringDefault("AiPlayerbot.BotCheats", "taxi,item,breath")));

    rndBotCheatMask = uint32(CheatAction::GetCheatMask(config.GetStringDefault("AiPlayerbot.RndBotCheats", "taxi,item,breath")));    

    LoadListString<std::list<std::string>>(config.GetStringDefault("AiPlayerbot.AllowedLogFiles", ""), allowedLogFiles);
    LoadListString<std::list<std::string>>(config.GetStringDefault("AiPlayerbot.DebugFilter", "add gathering loot,check values,emote,check mount state,jump"), debugFilter);

    worldBuffs.clear();

    //Get all config values starting with AiPlayerbot.WorldBuff
    std::vector<std::string> values = configA->GetValues("AiPlayerbot.WorldBuff");

    if (values.size())
    {
        sLog.outString("Loading WorldBuffs");
        BarGoLink wbuffBar(values.size());

        for (auto value : values)
        {
            std::vector<std::string> ids = split(value, '.');
            std::vector<uint32> params = { 0,0,0,0,0,0 };

            //Extract faction, class, spec, minlevel, maxlevel
            for (uint8 i = 0; i < 6; i++)
                if (ids.size() > i + 2)
                    params[i] = stoi(ids[i + 2]);

            //Get list of buffs for this combination.
            std::list<uint32> buffs;
            LoadList<std::list<uint32>>(config.GetStringDefault(value, ""), buffs);

            //Store buffs for later application.
            for (auto buff : buffs)
            {
                worldBuff wb = { buff, params[0], params[1], params[2], params[3], params[4], params[5] };
                worldBuffs.push_back(wb);
            }

            wbuffBar.step();
        }
    }

    randomBotAccountPrefix = config.GetStringDefault("AiPlayerbot.RandomBotAccountPrefix", "rndbot");
    randomBotAccountCount = config.GetIntDefault("AiPlayerbot.RandomBotAccountCount", 50);
    deleteRandomBotAccounts = config.GetBoolDefault("AiPlayerbot.DeleteRandomBotAccounts", false);
    randomBotGuildCount = config.GetIntDefault("AiPlayerbot.RandomBotGuildCount", 20);
    deleteRandomBotGuilds = config.GetBoolDefault("AiPlayerbot.DeleteRandomBotGuilds", false);

    //arena
    randomBotArenaTeamCount = config.GetIntDefault("AiPlayerbot.RandomBotArenaTeamCount", 20);
    deleteRandomBotArenaTeams = config.GetBoolDefault("AiPlayerbot.DeleteRandomBotArenaTeams", false);

    //cosmetics (by lidocain)
    randomBotShowCloak = config.GetBoolDefault("AiPlayerbot.RandomBotShowCloak", false);
    randomBotShowHelmet = config.GetBoolDefault("AiPlayerbot.RandomBotShowHelmet", false);

	//SPP switches
    enableGreet = config.GetBoolDefault("AiPlayerbot.EnableGreet", false);
	disableRandomLevels = config.GetBoolDefault("AiPlayerbot.DisableRandomLevels", false);
    instantRandomize = config.GetBoolDefault("AiPlayerbot.InstantRandomize", true);
    randomBotRandomPassword = config.GetBoolDefault("AiPlayerbot.RandomBotRandomPassword", true);
    playerbotsXPrate = config.GetFloatDefault("AiPlayerbot.XPRate", 1.0f);
    disableBotOptimizations = config.GetBoolDefault("AiPlayerbot.DisableBotOptimizations", false);
    disableActivityPriorities = config.GetBoolDefault("AiPlayerbot.DisableActivityPriorities", false);
    forceActiveWhenNearPlayer = config.GetBoolDefault("AiPlayerbot.ForceActiveWhenNearPlayer", false);
    limitCombatActivity = config.GetBoolDefault("AiPlayerbot.LimitCombatActivity", false);
    guildOrderAlwaysActive = config.GetBoolDefault("AiPlayerbot.GuildOrderAlwaysActive", true);
    botActiveAlone = config.GetIntDefault("AiPlayerbot.botActiveAlone", 10);
    diffWithPlayer = config.GetIntDefault("AiPlayerbot.DiffWithPlayer", 100);
    diffEmpty = config.GetIntDefault("AiPlayerbot.DiffEmpty", 200);
    RandombotsWalkingRPG = config.GetBoolDefault("AiPlayerbot.RandombotsWalkingRPG", false);
    RandombotsWalkingRPGInDoors = config.GetBoolDefault("AiPlayerbot.RandombotsWalkingRPG.InDoors", false);
    minEnchantingBotLevel = config.GetIntDefault("AiPlayerbot.minEnchantingBotLevel", 60);
    randombotStartingLevel = config.GetIntDefault("AiPlayerbot.randombotStartingLevel", 5);
    gearscorecheck = config.GetBoolDefault("AiPlayerbot.GearScoreCheck", false);
    levelCheck = config.GetIntDefault("AiPlayerbot.LevelCheck", 30);
	randomBotPreQuests = config.GetBoolDefault("AiPlayerbot.PreQuests", true);
    randomBotSayWithoutMaster = config.GetBoolDefault("AiPlayerbot.RandomBotSayWithoutMaster", false);
    randomBotInvitePlayer = config.GetBoolDefault("AiPlayerbot.RandomBotInvitePlayer", true);
    randomBotGroupNearby = config.GetBoolDefault("AiPlayerbot.RandomBotGroupNearby", true);
    // [group 2026-10-16] 队伍里没有真人时的自动解散（0 关 / 1 真人离开后解散 / 2 任何纯机器人队伍）
    autoDisbandBotOnlyGroups = (uint8)config.GetIntDefault("AiPlayerbot.AutoDisbandBotOnlyGroups", 1);
    // [threat 2026-10-16 站长口径] 自己仇恨 ≥ 坦克仇恨的这个百分比就压着不出手（0 = 关）
    waitForAttackThreatPercent = (uint8)config.GetIntDefault("AiPlayerbot.WaitForAttackThreatPercent", 90);
    // [threat 2026-10-16 站长口径] 开场"等坦克 pull back"的秒数上限（默认 5；游戏内可单 bot 覆盖）
    waitForAttackTime = (uint8)config.GetIntDefault("AiPlayerbot.WaitForAttackTime", 5);
    // [threat 2026-10-16 站长口径] 坦克离我 ≤ 这个距离且对该目标有仇恨 ⇒ 视为"已拉回来"，立刻开打
    waitForAttackTankBackDistance = config.GetFloatDefault("AiPlayerbot.WaitForAttackTankBackDistance", 12.0f);
    // [threat 2026-10-16 站长口径] 等坦克时保持的安全距离（站长口径：一般 15 码合适）
    waitForAttackSafeDistance = config.GetFloatDefault("AiPlayerbot.WaitForAttackSafeDistance", 15.0f);
    // [pull-gate 2026-10-19 站长口径] "坦克乱开怪"：未进战斗时附近敌对达到这个数量就不主动开怪（0 = 关）
    pullHostileLimit = (uint8)config.GetIntDefault("AiPlayerbot.PullHostileLimit", 3);
    pullHostileRadius = config.GetFloatDefault("AiPlayerbot.PullHostileRadius", 15.0f);
    // [defend-self 2026-10-19] 非坦克被贴脸且血量低于此百分比时走位（0 = 关）
    selfDefendHealth = (uint8)config.GetIntDefault("AiPlayerbot.SelfDefendHealth", 45);
    // [critter 2026-10-18 站长口径] t 的战斗行为跳过小动物等无害目标（1 = 跳过，默认）
    skipCritters = config.GetBoolDefault("AiPlayerbot.SkipCritters", true);
    // [tank-wait 2026-10-18 站长口径] "如果有人掉队 T 不应该继续往前走"：坦克等掉队队友的总开关
    tankWaitForGroupEnabled = config.GetBoolDefault("AiPlayerbot.TankWaitForGroup", true);
    // [tank-wait 2026-10-18 站长口径] 掉队判定距离阈值（码，默认 30）
    tankWaitForGroupDistance = config.GetFloatDefault("AiPlayerbot.TankWaitForGroupDistance", 30.0f);
    // [tank-wait 2026-10-18 站长口径] 坦克等待上限（秒，默认 20）；超时后恢复前进并进入等长冷却
    tankWaitForGroupTimeout = (uint32)config.GetIntDefault("AiPlayerbot.TankWaitForGroupTimeout", 20);
    // [safe-move 2026-10-18 站长口径] 战斗内移动选点的"别引到旁边怪"安全检查总开关
    combatMoveSafetyEnabled = config.GetBoolDefault("AiPlayerbot.CombatMoveSafety", true);
    // [safe-move 2026-10-18 站长口径] 仇恨半径之外再留的余量（码，默认 5）
    combatMoveSafetyMargin = config.GetFloatDefault("AiPlayerbot.CombatMoveSafetyMargin", 5.0f);
    // [pull 2026-10-18 站长口径] 坦克开怪目标的"同层"判据：|Δz| 上限（默认 6 码；0 = 关）
    pullSameLevelZDistance = config.GetFloatDefault("AiPlayerbot.PullSameLevelZDistance", 6.0f);
    // [pull 2026-10-18 站长口径] 2D 距离超过这个值才做 navmesh 路径校验（默认 20 码）
    pullPathCheckDistance = config.GetFloatDefault("AiPlayerbot.PullPathCheckDistance", 20.0f);
    // [BOSS 战策略 2026-10-16] 战斗中按队伍目标 BOSS entry 自动挂载 custom::boss_<entry>（数据 owner=0）
    bossStrategies = config.GetBoolDefault("AiPlayerbot.BossStrategies", true);
    // [副本推进 2026-10-18] 运行期按波推进总开关（只对 route 表 verified=1 的图生效）
    // [2026-10-18 站长口径] "bot 自动开怪应该做成开关，需要手动打开" ⇒ 默认 **false**。
    //   运行时用 GM 命令 `.bot route on` 打开（不需要重启；reset = 回到跟配置）。
    //   另外还有第二道闸：目标图的 `ai_playerbot_dungeon_route.verified=1`（实测通过才放行）。
    dungeonRoute = config.GetBoolDefault("AiPlayerbot.DungeonRoute", false);
    // [自动进本 2026-10-18] 目标副本列表（例 "543,542,547"）；空 = 不自动进本
    autoDungeonRun = config.GetBoolDefault("AiPlayerbot.AutoDungeonRun", false);
    autoDungeonMaps = config.GetStringDefault("AiPlayerbot.AutoDungeonMaps", "");
    // [仿真 2026-10-18] 可控场景注入：本地 WSL 仿真开 1，云端必须 0（默认 0）
    simMode = config.GetBoolDefault("AiPlayerbot.SimMode", false);
    if (simMode)
        sLog.outError("[SIM] AiPlayerbot.SimMode = 1 —— 仿真模式开启（.bot sim start/stop/status；仅本地使用）");
    // [2026-10-19] 水里也走 mmap 路径 / 移动自救（跳跃、升降台传送），默认关，本地仿真打开
    waterPath = config.GetBoolDefault("AiPlayerbot.WaterPath", false);
    routeJumpAssist = config.GetBoolDefault("AiPlayerbot.RouteJumpAssist", true);
    routeLiftAssist = config.GetBoolDefault("AiPlayerbot.RouteLiftAssist", true);
    randomBotRaidNearby = config.GetBoolDefault("AiPlayerbot.RandomBotRaidNearby", true);
    randomBotGuildNearby = config.GetBoolDefault("AiPlayerbot.RandomBotGuildNearby", true);
    inviteChat = config.GetBoolDefault("AiPlayerbot.InviteChat", true);
    enableOffSpecStrategies = config.GetBoolDefault("AiPlayerbot.EnableOffSpecStrategies", true);
    useWanderAsDefaultFollowStrategy = config.GetBoolDefault("AiPlayerbot.UseWanderAsDefaultFollowStrategy", true);
    defaultFormation = config.GetStringDefault("AiPlayerbot.DefaultFormation", "near");

    guildMaxBotLimit = config.GetIntDefault("AiPlayerbot.GuildMaxBotLimit", 1000);

    ////////////////////////////
    enableBroadcasts = config.GetBoolDefault("AiPlayerbot.EnableBroadcasts", true);

    //broadcastChanceMaxValue is used in urand(1, broadcastChanceMaxValue) for broadcasts,
    //lowering it will increase the chance, setting it to 0 will disable broadcasts
    //for internal use, not intended to be change by the user
    broadcastChanceMaxValue = enableBroadcasts ? 30000 : 0;

    //all broadcast chances should be in range 1-broadcastChanceMaxValue, value of 0 will disable this particular broadcast
    //setting value to max does not guarantee the broadcast, as there are some internal randoms as well
    broadcastToGuildGlobalChance = config.GetIntDefault("AiPlayerbot.BroadcastToGuildGlobalChance", 30000);
    broadcastToWorldGlobalChance = config.GetIntDefault("AiPlayerbot.BroadcastToWorldGlobalChance", 30000);
    broadcastToGeneralGlobalChance = config.GetIntDefault("AiPlayerbot.BroadcastToGeneralGlobalChance", 30000);
    broadcastToTradeGlobalChance = config.GetIntDefault("AiPlayerbot.BroadcastToTradeGlobalChance", 30000);
    broadcastToLFGGlobalChance = config.GetIntDefault("AiPlayerbot.BroadcastToLFGGlobalChance", 30000);
    broadcastToLocalDefenseGlobalChance = config.GetIntDefault("AiPlayerbot.BroadcastToLocalDefenseGlobalChance", 30000);
    broadcastToWorldDefenseGlobalChance = config.GetIntDefault("AiPlayerbot.BroadcastToWorldDefenseGlobalChance", 30000);
    broadcastToGuildRecruitmentGlobalChance = config.GetIntDefault("AiPlayerbot.BroadcastToGuildRecruitmentGlobalChance", 30000);
    broadcastToSayGlobalChance = config.GetIntDefault("AiPlayerbot.BroadcastToSayGlobalChance", 30000);
    broadcastToYellGlobalChance = config.GetIntDefault("AiPlayerbot.BroadcastToYellGlobalChance", 30000);

    broadcastChanceLootingItemPoor = config.GetIntDefault("AiPlayerbot.BroadcastChanceLootingItemPoor", 30);
    broadcastChanceLootingItemNormal = config.GetIntDefault("AiPlayerbot.BroadcastChanceLootingItemNormal", 300);
    broadcastChanceLootingItemUncommon = config.GetIntDefault("AiPlayerbot.BroadcastChanceLootingItemUncommon", 10000);
    broadcastChanceLootingItemRare = config.GetIntDefault("AiPlayerbot.BroadcastChanceLootingItemRare", 20000);
    broadcastChanceLootingItemEpic = config.GetIntDefault("AiPlayerbot.BroadcastChanceLootingItemEpic", 30000);
    broadcastChanceLootingItemLegendary = config.GetIntDefault("AiPlayerbot.BroadcastChanceLootingItemLegendary", 30000);
    broadcastChanceLootingItemArtifact = config.GetIntDefault("AiPlayerbot.BroadcastChanceLootingItemArtifact", 30000);

    broadcastChanceQuestAccepted = config.GetIntDefault("AiPlayerbot.BroadcastChanceQuestAccepted", 6000);
    broadcastChanceQuestUpdateObjectiveCompleted = config.GetIntDefault("AiPlayerbot.BroadcastChanceQuestUpdateObjectiveCompleted", 300);
    broadcastChanceQuestUpdateObjectiveProgress = config.GetIntDefault("AiPlayerbot.BroadcastChanceQuestUpdateObjectiveProgress", 300);
    broadcastChanceQuestUpdateFailedTimer = config.GetIntDefault("AiPlayerbot.BroadcastChanceQuestUpdateFailedTimer", 300);
    broadcastChanceQuestUpdateComplete = config.GetIntDefault("AiPlayerbot.BroadcastChanceQuestUpdateComplete", 1000);
    broadcastChanceQuestTurnedIn = config.GetIntDefault("AiPlayerbot.BroadcastChanceQuestTurnedIn", 10000);

    broadcastChanceKillNormal = config.GetIntDefault("AiPlayerbot.BroadcastChanceKillNormal", 30);
    broadcastChanceKillElite = config.GetIntDefault("AiPlayerbot.BroadcastChanceKillElite", 300);
    broadcastChanceKillRareelite = config.GetIntDefault("AiPlayerbot.BroadcastChanceKillRareelite", 3000);
    broadcastChanceKillWorldboss = config.GetIntDefault("AiPlayerbot.BroadcastChanceKillWorldboss", 20000);
    broadcastChanceKillRare = config.GetIntDefault("AiPlayerbot.BroadcastChanceKillRare", 10000);
    broadcastChanceKillUnknown = config.GetIntDefault("AiPlayerbot.BroadcastChanceKillUnknown", 100);
    broadcastChanceKillPet = config.GetIntDefault("AiPlayerbot.BroadcastChanceKillPet", 10);
    broadcastChanceKillPlayer = config.GetIntDefault("AiPlayerbot.BroadcastChanceKillPlayer", 30);

    broadcastChanceLevelupGeneric = config.GetIntDefault("AiPlayerbot.BroadcastChanceLevelupGeneric", 20000);
    broadcastChanceLevelupTenX = config.GetIntDefault("AiPlayerbot.BroadcastChanceLevelupTenX", 30000);
    broadcastChanceLevelupMaxLevel = config.GetIntDefault("AiPlayerbot.BroadcastChanceLevelupMaxLevel", 30000);

    broadcastChanceSuggestInstance = config.GetIntDefault("AiPlayerbot.BroadcastChanceSuggestInstance", 5000);
    broadcastChanceSuggestQuest = config.GetIntDefault("AiPlayerbot.BroadcastChanceSuggestQuest", 10000);
    broadcastChanceSuggestGrindMaterials = config.GetIntDefault("AiPlayerbot.BroadcastChanceSuggestGrindMaterials", 5000);
    broadcastChanceSuggestGrindReputation = config.GetIntDefault("AiPlayerbot.BroadcastChanceSuggestGrindReputation", 5000);
    broadcastChanceSuggestSell = config.GetIntDefault("AiPlayerbot.BroadcastChanceSuggestSell", 300);
    broadcastChanceSuggestSomething = config.GetIntDefault("AiPlayerbot.BroadcastChanceSuggestSomething", 30000);

    broadcastChanceSuggestSomethingToxic = config.GetIntDefault("AiPlayerbot.BroadcastChanceSuggestSomethingToxic", 0);

    broadcastChanceSuggestToxicLinks = config.GetIntDefault("AiPlayerbot.BroadcastChanceSuggestToxicLinks", 0);
    toxicLinksPrefix = config.GetStringDefault("AiPlayerbot.ToxicLinksPrefix", "gnomes");

    broadcastChanceSuggestThunderfury = config.GetIntDefault("AiPlayerbot.BroadcastChanceSuggestThunderfury", 1);

    //does not depend on global chance
    broadcastChanceGuildManagement = config.GetIntDefault("AiPlayerbot.BroadcastChanceGuildManagement", 30000);
    ////////////////////////////

    toxicLinksRepliesChance = config.GetIntDefault("AiPlayerbot.ToxicLinksRepliesChance", 30); //0-100
    thunderfuryRepliesChance = config.GetIntDefault("AiPlayerbot.ThunderfuryRepliesChance", 40); //0-100
    guildRepliesRate = config.GetIntDefault("AiPlayerbot.GuildRepliesRate", 100); //0-100

    botAcceptDuelMinimumLevel = config.GetIntDefault("AiPlayerbot.BotAcceptDuelMinimumLevel", 10);

    randomBotFormGuild = config.GetBoolDefault("AiPlayerbot.RandomBotFormGuild", true);

    boostFollow = config.GetBoolDefault("AiPlayerbot.BoostFollow", false);
    turnInRpg = config.GetBoolDefault("AiPlayerbot.TurnInRpg", false);
    shareTargets = config.GetBoolDefault("AiPlayerbot.ShareTargets", true);
    globalSoundEffects = config.GetBoolDefault("AiPlayerbot.GlobalSoundEffects", false);
    nonGmFreeSummon = config.GetBoolDefault("AiPlayerbot.NonGmFreeSummon", false);

    //SPP automation
    autoPickReward = config.GetStringDefault("AiPlayerbot.AutoPickReward", "no");
    autoEquipUpgradeLoot = config.GetBoolDefault("AiPlayerbot.AutoEquipUpgradeLoot", false);
    autoEnchantUpgradeLoot = config.GetBoolDefault("AiPlayerbot.AutoEnchantUpgradeLoot", false);
    syncQuestWithPlayer = config.GetBoolDefault("AiPlayerbot.SyncQuestWithPlayer", false);
    syncQuestForPlayer = config.GetBoolDefault("AiPlayerbot.SyncQuestForPlayer", false);
    autoTrainSpells = config.GetStringDefault("AiPlayerbot.AutoTrainSpells", "no");
    autoPickTalents = config.GetStringDefault("AiPlayerbot.AutoPickTalents", "no");
    autoLearnTrainerSpells = config.GetBoolDefault("AiPlayerbot.AutoLearnTrainerSpells", false);
    autoLearnQuestSpells = config.GetBoolDefault("AiPlayerbot.AutoLearnQuestSpells", false);
    autoLearnDroppedSpells = config.GetBoolDefault("AiPlayerbot.AutoLearnDroppedSpells", false);
    autoDoQuests = config.GetBoolDefault("AiPlayerbot.AutoDoQuests", true);
    // [必要功能裁剪 2026-10-15] 鱼点：我们只保留必要功能，默认关闭（详见 TravelMgr::LoadFishLocations）
    enableFishLocations = config.GetBoolDefault("AiPlayerbot.EnableFishLocations", false);
    syncLevelWithPlayers = config.GetBoolDefault("AiPlayerbot.SyncLevelWithPlayers", false);
    syncLevelMaxAbove = config.GetIntDefault("AiPlayerbot.SyncLevelMaxAbove", 5);
    syncLevelNoPlayer = config.GetIntDefault("AiPlayerbot.SyncLevelNoPlayer", randombotStartingLevel);
    syncAltLevelToMaster = config.GetBoolDefault("AiPlayerbot.SyncAltLevelToMaster", false);
    tweakValue = config.GetIntDefault("AiPlayerbot.TweakValue", 0);
    talentsInPublicNote = config.GetBoolDefault("AiPlayerbot.TalentsInPublicNote", false);
    respawnModNeutral = config.GetFloatDefault("AiPlayerbot.RespawnModNeutral", 10.0f);
    respawnModHostile = config.GetFloatDefault("AiPlayerbot.RespawnModHostile", 5.0f);
    respawnModThreshold = config.GetIntDefault("AiPlayerbot.RespawnModThreshold", 10);
    respawnModMax = config.GetIntDefault("AiPlayerbot.RespawnModMax", 18);
    respawnModForPlayerBots = config.GetBoolDefault("AiPlayerbot.RespawnModForPlayerBots", false);
    respawnModForInstances = config.GetBoolDefault("AiPlayerbot.RespawnModForInstances", false);

    //LLM START
    llmEnabled = config.GetIntDefault("AiPlayerbot.LLMEnabled", 1);
    llmApiEndpoint = config.GetStringDefault("AiPlayerbot.LLMApiEndpoint", "http://127.0.0.1:5001/api/v1/generate");
    try {
        llmEndPointUrl = parseUrl(llmApiEndpoint);
    }
    catch (const std::invalid_argument& e) {
        sLog.outError("Unable to parse LLMApiEndpoint url: %s", e.what());
    }
    llmApiKey = config.GetStringDefault("AiPlayerbot.LLMApiKey", "");    
    llmApiJson = config.GetStringDefault("AiPlayerbot.LLMApiJson", "{ \"max_length\": 100, \"prompt\": \"[<pre prompt>]<context> <prompt> <post prompt>\"}");
    llmContextLength = config.GetIntDefault("AiPlayerbot.LLMContextLength", 4096);
    llmGenerationTimeout = config.GetIntDefault("AiPlayerbot.LLMGenerationTimeout", 600);
    llmMaxSimultaniousGenerations = config.GetIntDefault("AiPlayerbot.LLMMaxSimultaniousGenerations", 100);
        
    
    llmPrePrompt = config.GetStringDefault("AiPlayerbot.LLMPrePrompt", "You are a roleplaying character in World of Warcraft: <expansion name>. Your name is <bot name>. The <other type> <other name> is speaking to you <channel name> and is an <other gender> <other race> <other class> of level <other level>. You are level <bot level> and play as a <bot gender> <bot race> <bot class> that is currently in <bot subzone> <bot zone>. Answer as a roleplaying character. Limit responses to 100 characters.");

    llmPreRpgPrompt = config.GetStringDefault("AiPlayerbot.LLMRpgPrompt", "In World of Warcraft: <expansion name> in <bot zone> <bot subzone> stands <bot type> <bot name> a level <bot level> <bot gender> <bot race> <bot class>."
        " Standing nearby is <unit type> <unit name> <unit subname> a level <unit level> <unit gender> <unit race> <unit faction> <unit class>. Answer as a roleplaying character. Limit responses to 100 characters.");



    llmPrompt = config.GetStringDefault("AiPlayerbot.LLMPrompt", "<receiver name>:<initial message>");
    llmPostPrompt = config.GetStringDefault("AiPlayerbot.LLMPostPrompt", "<sender name>:");

    llmResponseStartPattern = config.GetStringDefault("AiPlayerbot.LLMResponseStartPattern", R"(("text":\s*"))");
    llmResponseEndPattern = config.GetStringDefault("AiPlayerbot.LLMResponseEndPattern", R"(("|\b(?!<sender name>\b)(\w+):))");
    llmResponseDeletePattern = config.GetStringDefault("AiPlayerbot.LLMResponseDeletePattern", R"((\\n|<sender name>:|\\[^ ]+))");
    llmResponseSplitPattern = config.GetStringDefault("AiPlayerbot.LLMResponseSplitPattern", R"((\*.*?\*)|(\[.*?\])|(\'.*\')|([^\*\[\] ][^\*\[\]]+?[.?!]))");

    if (false) //Disable for release
    {
        sLog.outError("# AiPlayerbot.LLMResponseStartPattern = %s", llmResponseStartPattern.c_str());
        sLog.outError("# AiPlayerbot.LLMResponseEndPattern = %s", llmResponseEndPattern.c_str());
        sLog.outError("# AiPlayerbot.LLMResponseDeletePattern = %s", llmResponseDeletePattern.c_str());
        sLog.outError("# AiPlayerbot.LLMResponseSplitPattern = %s", llmResponseSplitPattern.c_str());
    }

    try {
        std::regex pattern(llmResponseStartPattern);
    }
    catch (const std::regex_error& e) {        
        sLog.outError("Regex error in %s: %s", llmResponseStartPattern.c_str(), e.what());
    }

    try {
        std::regex pattern(llmResponseEndPattern);
    }
    catch (const std::regex_error& e) {
        sLog.outError("Regex error in %s: %s", llmResponseEndPattern.c_str(), e.what());
    }

    try {
        std::regex pattern(llmResponseDeletePattern);
    }
    catch (const std::regex_error& e) {
        sLog.outError("Regex error in %s: %s", llmResponseDeletePattern.c_str(), e.what());
    }

    try {
        std::regex pattern(llmResponseSplitPattern);
    }
    catch (const std::regex_error& e) {
        sLog.outError("Regex error in %s: %s", llmResponseSplitPattern.c_str(), e.what());
    }

    llmGlobalContext = config.GetBoolDefault("AiPlayerbot.LLMGlobalContext", false);
    llmBotToBotChatChance = config.GetIntDefault("AiPlayerbot.LLMBotToBotChatChance", 0);
    llmRpgAIChatChance = config.GetIntDefault("AiPlayerbot.LLMRpgAIChatChance", 100);

    std::list<std::string> blockedChannels;
    LoadListString<std::list<std::string>>(config.GetStringDefault("AiPlayerbot.LLMBlockedReplyChannels", ""), blockedChannels);
    std::map<std::string, ChatChannelSource> sourceName;
    sourceName["guild"] = ChatChannelSource::SRC_GUILD;
    sourceName["world"] = ChatChannelSource::SRC_WORLD;
    sourceName["general"] = ChatChannelSource::SRC_GENERAL;
    sourceName["trade"] = ChatChannelSource::SRC_TRADE;
    sourceName["lfg"] = ChatChannelSource::SRC_LOOKING_FOR_GROUP;
    sourceName["ldefence"] = ChatChannelSource::SRC_LOCAL_DEFENSE;
    sourceName["wdefence"] = ChatChannelSource::SRC_WORLD_DEFENSE;
    sourceName["grecruitement"] = ChatChannelSource::SRC_GUILD_RECRUITMENT;
    sourceName["say"] = ChatChannelSource::SRC_SAY;
    sourceName["whisper"] = ChatChannelSource::SRC_WHISPER;
    sourceName["emote"] = ChatChannelSource::SRC_EMOTE;
    sourceName["temote"] = ChatChannelSource::SRC_TEXT_EMOTE;
    sourceName["yell"] = ChatChannelSource::SRC_YELL;
    sourceName["party"] = ChatChannelSource::SRC_PARTY;
    sourceName["raid"] = ChatChannelSource::SRC_RAID;

    for (auto& channelName : blockedChannels)
        llmBlockedReplyChannels.insert(sourceName[channelName]);

    {
        std::string promptsFile = config.GetStringDefault("AiPlayerbot.LLMDefaultPromptsFile", "llm_character_card");
        LoadLLMDefaultPrompts(promptsFile);
    }

    // Gear progression system
    gearProgressionSystemEnabled = config.GetBoolDefault("AiPlayerbot.GearProgressionSystem.Enable", false);

    // Gear progression phase
    for (uint8 phase = 0; phase < MAX_GEAR_PROGRESSION_LEVEL; phase++)
    {
        std::ostringstream os; os << "AiPlayerbot.GearProgressionSystem." << std::to_string(phase) << ".MinItemLevel";
        gearProgressionSystemItemLevels[phase][0] = config.GetIntDefault(os.str().c_str(), 9999999);
        os.str(""); os << "AiPlayerbot.GearProgressionSystem." << std::to_string(phase) << ".MaxItemLevel";
        gearProgressionSystemItemLevels[phase][1] = config.GetIntDefault(os.str().c_str(), 9999999);

        // Gear progression class
        for (uint8 cls = 1; cls < MAX_CLASSES; cls++)
        {
            // Gear progression spec
            for (uint8 spec = 0; spec < 4; spec++)
            {
                // Gear progression slot
                for (uint8 slot = 0; slot < SLOT_EMPTY; slot++)
                {
                    std::ostringstream os; os << "AiPlayerbot.GearProgressionSystem." << std::to_string(phase) << "." << std::to_string(cls) << "." << std::to_string(spec) << "." << std::to_string(slot);
                    gearProgressionSystemItems[phase][cls][spec][slot] = config.GetIntDefault(os.str().c_str(), -1);
                }
            }
        }
    }

    // [按副本换阶段 2026-10-14] AiPlayerbot.GearProgressionSystem.InstancePhases = "mapId:phase,mapId:phase,..."
    // 站长口径：五人本+卡拉赞/格鲁尔/玛瑟里顿 = 0(团本前毕业)；毒蛇/风暴+祖阿曼 = 1(P1毕业)；
    // 海山/黑庙 = 2(P2毕业)；太阳井 = 3(P4毕业)。留空则沿用原行为（按队长装备等级判断阶段）。
    {
        std::string instancePhases = config.GetStringDefault("AiPlayerbot.GearProgressionSystem.InstancePhases", "");
        std::stringstream ss(instancePhases);
        std::string pair;
        while (std::getline(ss, pair, ','))
        {
            std::string::size_type colon = pair.find(':');
            if (colon == std::string::npos)
                continue;

            uint32 mapId = (uint32)atoi(pair.substr(0, colon).c_str());
            uint8 phase = (uint8)atoi(pair.substr(colon + 1).c_str());
            if (mapId && phase < MAX_GEAR_PROGRESSION_LEVEL)
                gearProgressionInstancePhases[mapId] = phase;
        }

        if (!gearProgressionInstancePhases.empty())
            sLog.outString("GearProgression: %u instance->phase mappings loaded", (uint32)gearProgressionInstancePhases.size());
    }

    // [性能 2026-10-14] AiPlayerbot.GearProgressionSystem.CacheLevels = "70" 或 "60,70"（空=全部等级）
    // 固定等级的服务器只建用得到的等级 ⇒ 装备缓存构建的外层组合数从 287,280 降到 4,104（70 倍）
    {
        std::string cacheLevels = config.GetStringDefault("AiPlayerbot.GearProgressionSystem.CacheLevels", "");
        std::stringstream ss(cacheLevels);
        std::string token;
        while (std::getline(ss, token, ','))
        {
            uint32 lvl = (uint32)atoi(token.c_str());
            if (lvl >= 1 && lvl <= DEFAULT_MAX_LEVEL)
                gearProgressionCacheLevels.push_back(lvl);
        }

        if (!gearProgressionCacheLevels.empty())
            sLog.outString("GearProgression: equipment cache will be built for %u level(s) only (CacheLevels)", (uint32)gearProgressionCacheLevels.size());
    }

    // [按阶段生成 2026-10-15] 随机生成装备时使用的默认阶段（0 = 团本前；站长口径 ilvl 100-115）⇒ 见 RandomItemMgr::Query
    gearProgressionDefaultPhase = (uint8)config.GetIntDefault("AiPlayerbot.GearProgressionSystem.DefaultPhase", 0);
    if (gearProgressionDefaultPhase >= MAX_GEAR_PROGRESSION_LEVEL)
        gearProgressionDefaultPhase = 0;

    sLog.outString("Loading free bots.");
    selfBotLevel = BotSelfBotLevel(config.GetIntDefault("AiPlayerbot.SelfBotLevel", uint32(BotSelfBotLevel::GM_ONLY)));
    LoadListString<std::list<std::string>>(config.GetStringDefault("AiPlayerbot.ToggleAlwaysOnlineAccounts", ""), toggleAlwaysOnlineAccounts);
    LoadListString<std::list<std::string>>(config.GetStringDefault("AiPlayerbot.ToggleAlwaysOnlineChars", ""), toggleAlwaysOnlineChars);

    for (std::string& nm : toggleAlwaysOnlineAccounts)
        std::transform(nm.begin(), nm.end(), nm.begin(), toupper);

    for (std::string& nm : toggleAlwaysOnlineChars)
    {
        std::transform(nm.begin(), nm.end(), nm.begin(), tolower);
        nm[0] = toupper(nm[0]);
    }

    loadFreeAltBotAccounts();

    targetPosRecalcDistance = config.GetFloatDefault("AiPlayerbot.TargetPosRecalcDistance", 0.1f),

    sLog.outString("Loading area levels.");
    sTravelMgr.LoadAreaLevels();
    sLog.outString("Loading spellIds.");
    ChatHelper::PopulateSpellNameList();
    ItemUsageValue::PopulateProfessionReagentIds();
    ItemUsageValue::PopulateSoldByVendorItemIds();
    ItemUsageValue::PopulateReagentItemIdsForCraftableItemIds();

    RandomPlayerbotFactory::CreateRandomBots();
    PlayerbotLogRss("after CreateRandomBots");
    PlayerbotFactory::Init();
    PlayerbotLogRss("after PlayerbotFactory::Init");
    sRandomItemMgr.Init();
    PlayerbotLogRss("after RandomItemMgr::Init");
    sPlayerbotTextMgr.LoadBotTexts();
    sPlayerbotTextMgr.LoadBotTextChance();
    sPlayerbotHelpMgr.LoadBotHelpTexts();
    PlayerbotLogRss("after texts/help");

    LoadTalentSpecs();
    PlayerbotLogRss("after LoadTalentSpecs");

    // [BOT-METRICS 2026-10-17] 数据采集出口：`BOT_METRICS=1` 时强制打开 bot_events.csv。
    //   logEvent() 本身被 hasLog("bot_events.csv") 门控，而该文件名默认不在启用列表里
    //   ⇒ 过去 bot_events.csv 根本不会生成（本地实测不存在/0 字节）。
    //   openLog(file, mode, haslog=true) 会绕过 hasLog 检查（见 openLog 实现）。
    //   不设 BOT_METRICS 时这一段完全跳过 ⇒ 线上零成本、零行为变化。
    if (std::getenv("BOT_METRICS"))
    {
        openLog("bot_events.csv", "a", true);
        sLog.outString("[BM] metrics enabled -> bot_events.csv (LogsDir=%s)", sConfig.GetStringDefault("LogsDir").c_str());
    }


    if (sPlayerbotAIConfig.autoDoQuests)
    {
        sLog.outString("Loading Quest Detail Data...");
        sTravelMgr.LoadQuestTravelTable();
        PlayerbotLogRss("after LoadQuestTravelTable");

        // [诊断 2026-10-15] 清空前先量一下这个共享值有多大（实测清掉后 RSS 掉 ~590MB，要钉死是谁）
        // [crash-fix 2026-10-16] 原来这里用 guidpsValue->GetRef() 取引用。GetRef() 已删除（它交出的
        // 引用会随宿主被 ClearValue() 释放而悬垂）。这里只需"看一眼规模"，改成 GetRef() 的等价安全写法：
        // 拿指针 → 确认还登记着 → 用一次 Get() 的返回值（拷贝）统计。统计块本来就是诊断用途，
        // 顺手用 PB_RSS_LOG 门控，默认不开时连这次拷贝都不做（EntryGuidps 可能有几十 MB）。
        static bool const dumpGuidpsSize = (std::getenv("PB_RSS_LOG") != nullptr);
        // [收敛 2026-10-16] AiSharedValueAlive 现在在全局命名空间（见 SharedValueContext.h），用 :: 限定。
        if (dumpGuidpsSize && ::AiSharedValueAlive("entry guidps"))
        {
            if (auto* guidpsValue = sSharedObjectContext.GetValue<EntryGuidps>("entry guidps"))
            {
                EntryGuidps const guidps = guidpsValue->Get();
                size_t points = 0;
                for (auto& entryPair : guidps)
                    points += entryPair.second.size();
                sLog.outString("[SZ] before clear: entry guidps = %zu entries / %zu positions",
                    guidps.size(), points);
            }
        }
        PlayerbotLogRss("before clear entry guidps");

        // [降峰 2026-10-15] 旅行图建完后立刻释放共享值 "entry guidps"（EntryGuidps，实测约 500MB 永久缓存）
        sSharedObjectContext.ClearValue("entry guidps");
        PlayerbotLogRss("after clear entry guidps");
    }

    sLog.outString("Loading named locations...");
    sRandomPlayerbotMgr.LoadNamedLocations();
    PlayerbotLogRss("after LoadNamedLocations");

    if (sPlayerbotAIConfig.randomBotJoinBG)
        sRandomPlayerbotMgr.LoadBattleMastersCache();
    PlayerbotLogRss("bot init done");

    sLog.outString("---------------------------------------");
    sLog.outString("        AI Playerbot initialized       ");
    sLog.outString("---------------------------------------");
    sLog.outString();

    return true;
}

bool PlayerbotAIConfig::IsInRandomAccountList(uint32 id)
{
    return find(randomBotAccounts.begin(), randomBotAccounts.end(), id) != randomBotAccounts.end();
}

bool PlayerbotAIConfig::IsFreeAltBot(uint32 guid)
{
    for (auto bot : freeAltBots)
        if (bot.second == guid)
            return true;

    return false;
}

bool PlayerbotAIConfig::IsInRandomQuestItemList(uint32 id)
{
    return find(randomBotQuestItems.begin(), randomBotQuestItems.end(), id) != randomBotQuestItems.end();
}

bool PlayerbotAIConfig::IsInPvpProhibitedZone(uint32 id)
{
	return find(pvpProhibitedZoneIds.begin(), pvpProhibitedZoneIds.end(), id) != pvpProhibitedZoneIds.end();
}

std::string PlayerbotAIConfig::GetValue(std::string name)
{
    std::ostringstream out;

    if (name == "GlobalCooldown")
        out << globalCoolDown;
    else if (name == "ReactDelay")
        out << reactDelay;

    else if (name == "SightDistance")
        out << sightDistance;
    else if (name == "SpellDistance")
        out << spellDistance;
    else if (name == "ReactDistance")
        out << reactDistance;
    else if (name == "GrindDistance")
        out << grindDistance;
    else if (name == "LootDistance")
        out << lootDistance;
    else if (name == "FleeDistance")
        out << fleeDistance;

    else if (name == "CriticalHealth")
        out << criticalHealth;
    else if (name == "LowHealth")
        out << lowHealth;
    else if (name == "MediumHealth")
        out << mediumHealth;
    else if (name == "AlmostFullHealth")
        out << almostFullHealth;
    else if (name == "LowMana")
        out << lowMana;

    else if (name == "IterationsPerTick")
        out << iterationsPerTick;

    return out.str();
}



void PlayerbotAIConfig::SetValue(std::string name, std::string value)
{
    std::istringstream out(value, std::istringstream::in);

    if (name == "GlobalCooldown")
        out >> globalCoolDown;
    else if (name == "ReactDelay")
        out >> reactDelay;

    else if (name == "SightDistance")
        out >> sightDistance;
    else if (name == "SpellDistance")
        out >> spellDistance;
    else if (name == "ReactDistance")
        out >> reactDistance;
    else if (name == "GrindDistance")
        out >> grindDistance;
    else if (name == "LootDistance")
        out >> lootDistance;
    else if (name == "FleeDistance")
        out >> fleeDistance;

    else if (name == "CriticalHealth")
        out >> criticalHealth;
    else if (name == "LowHealth")
        out >> lowHealth;
    else if (name == "MediumHealth")
        out >> mediumHealth;
    else if (name == "AlmostFullHealth")
        out >> almostFullHealth;
    else if (name == "LowMana")
        out >> lowMana;

    else if (name == "IterationsPerTick")
        out >> iterationsPerTick;
}

void PlayerbotAIConfig::loadFreeAltBotAccounts()
{
    bool allCharsOnline = (selfBotLevel == BotSelfBotLevel::ALWAYS_ACTIVE);

    freeAltBots.clear();

    auto results = LoginDatabase.PQuery("SELECT username, id FROM account where username not like '%s%%'", randomBotAccountPrefix.c_str());
    if (results)
    {
        do
        {
            bool accountToggle = false;

            Field* fields = results->Fetch();
            std::string accountName = fields[0].GetString();
            uint32 accountId = fields[1].GetUInt32();

            if (std::find(toggleAlwaysOnlineAccounts.begin(), toggleAlwaysOnlineAccounts.end(), accountName) != toggleAlwaysOnlineAccounts.end())
                accountToggle = true;

            auto result = CharacterDatabase.PQuery("SELECT name, guid FROM characters WHERE account = '%u'", accountId);
            if (!result)
                continue;

            do
            {
                bool charToggle = false;

                Field* fields = result->Fetch();
                std::string charName = fields[0].GetString();
                uint32 guid = fields[1].GetUInt32();

                BotAlwaysOnline always = BotAlwaysOnline(sRandomPlayerbotMgr.GetValue(guid, "always"));

                if (always == BotAlwaysOnline::DISABLED_BY_COMMAND)
                    continue;

                if (std::find(toggleAlwaysOnlineChars.begin(), toggleAlwaysOnlineChars.end(), charName) != toggleAlwaysOnlineChars.end())
                    charToggle = true;

                bool thisCharAlwaysOnline = allCharsOnline;

                if (accountToggle || charToggle)
                    thisCharAlwaysOnline = !thisCharAlwaysOnline;

                if ((thisCharAlwaysOnline && always != BotAlwaysOnline::DISABLED_BY_COMMAND) || always == BotAlwaysOnline::ACTIVE)
                {
                    sLog.outString("Enabling always online for %s", charName.c_str());
                    freeAltBots.push_back(std::make_pair(accountId, guid));
                }

            } while (result->NextRow());


        } while (results->NextRow());
    }
}

std::string PlayerbotAIConfig::GetTimestampStr()
{
    time_t t = time(nullptr);
    tm* aTm = localtime(&t);
    //       YYYY   year
    //       MM     month (2 digits 01-12)
    //       DD     day (2 digits 01-31)
    //       HH     hour (2 digits 00-23)
    //       MM     minutes (2 digits 00-59)
    //       SS     seconds (2 digits 00-59)
    char buf[20];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", aTm);

    return std::string(buf);
}

bool PlayerbotAIConfig::openLog(std::string fileName, char const* mode, bool haslog)
{
    if (!haslog && !hasLog(fileName))
        return false;

    auto logFileIt = logFiles.find(fileName);
    if (logFileIt == logFiles.end())
    {
        logFiles.insert(make_pair(fileName, std::make_pair(nullptr, false)));
        logFileIt = logFiles.find(fileName);
    }

    FILE* file = logFileIt->second.first;
    bool fileOpen = logFileIt->second.second;

    if (fileOpen) //close log file
        fclose(file);

    std::string m_logsDir = sConfig.GetStringDefault("LogsDir");
    if (!m_logsDir.empty())
    {
        if ((m_logsDir.at(m_logsDir.length() - 1) != '/') && (m_logsDir.at(m_logsDir.length() - 1) != '\\'))
            m_logsDir.append("/");
    }


    file = fopen((m_logsDir + fileName).c_str(), mode);
    fileOpen = true;

    logFileIt->second.first = file;
    logFileIt->second.second = fileOpen;

    return true;
}

void PlayerbotAIConfig::log(std::string fileName, const char* str, ...)
{
    if (!str)
        return;

    std::lock_guard<std::mutex> guard(m_logMtx);

    if (!isLogOpen(fileName))
        if (!openLog(fileName, "a"))
            return;

    FILE* file = logFiles.find(fileName)->second.first;

    va_list ap;
    va_start(ap, str);
    vfprintf(file, str, ap);
    fprintf(file, "\n");
    va_end(ap);
    fflush(file);

    fflush(stdout);
}

void PlayerbotAIConfig::logEvent(PlayerbotAI* ai, std::string eventName, std::string info1, std::string info2)
{
    // [BOT-ML 2026-10-17] BOT_METRICS=1 时强制启用 bot_events.csv：
    //   openLog(..., haslog=true) 只把文件打开，并不会让 hasLog() 变 true，
    //   所以这里必须显式放行，否则 44 处 logEvent() 仍然全部静默（实测 0 字节）。
    char const* bmMetricsEnv = std::getenv("BOT_METRICS");
    if (hasLog("bot_events.csv") || (bmMetricsEnv && bmMetricsEnv[0] != '\0'))
    {
        Player* bot = ai->GetBot();

        std::ostringstream out;
        out << sPlayerbotAIConfig.GetTimestampStr() << "+00,";
        out << bot->GetName() << ",";
        out << eventName << ",";
        out << std::fixed << std::setprecision(2);
        WorldPosition(bot).printWKT(out);

        out << std::to_string(bot->getRace()) << ",";
        out << std::to_string(bot->getClass()) << ",";
        float subLevel = ai->GetLevelFloat();

        out << subLevel << ",";

        out << "\"" << info1 << "\",";
        out << "\"" << info2 << "\",";

        // [rewards 2026-10-19] 角色列（追加在最后，保持前面列的位置不变）：
        //   "按职责分别算奖励"必须有它；取值 tank / heal / dps。
        //   判据复用 PlayerbotAI::IsTank/IsHeal（读的是 bot 身上的策略类型，不是名字）。
        char const* role = PlayerbotAI::IsTank(bot) ? "tank" : (PlayerbotAI::IsHeal(bot) ? "heal" : "dps");
        out << role;

        // [rewards 2026-10-19] 队伍列（追加在最后）：坦克/治疗的“保住队友不死”需要按队伍统计。
        out << "," << (bot->GetGroup() ? bot->GetGroup()->GetId() : 0);

        log("bot_events.csv", out.str().c_str());
    }
};

void PlayerbotAIConfig::logEvent(PlayerbotAI* ai, std::string eventName, ObjectGuid guid, std::string info2)
{
    std::string info1 = "";

    Unit* victim;
    if (guid)
    {
        victim = ai->GetUnit(guid);
        if (victim)
            info1 = victim->GetName();
    }

    logEvent(ai, eventName, info1, info2);
};

bool PlayerbotAIConfig::CanLogAction(PlayerbotAI* ai, std::string actionName, bool isExecute, std::string lastActionName)
{
    bool forRpg = (actionName.find("rpg") == 0) && ai->HasStrategy("debug rpg", BotState::BOT_STATE_NON_COMBAT);

    if (!forRpg)
    {
        if (isExecute && !ai->HasStrategy("debug", BotState::BOT_STATE_NON_COMBAT))
            return false;

        if (!isExecute && !ai->HasStrategy("debug action", BotState::BOT_STATE_NON_COMBAT))
            return false;

        if ((lastActionName == actionName) && (actionName == "melee"))
        {
            return false;
        }
    }

    return std::find(debugFilter.begin(), debugFilter.end(), actionName) == debugFilter.end();
}

void PlayerbotAIConfig::LoadTalentSpecs()
{
    sLog.outString("Loading TalentSpecs");

    uint32 maxSpecLevel = 0;
    uint32 talentSpecsBuilt = 0;   // [A/B 诊断] 统计实际构建的 TalentSpec 数量

    for (uint32 cls = 1; cls < MAX_CLASSES; ++cls)
    {
        classSpecs[cls] = ClassSpecs(1 << (cls - 1));
        for (uint32 spec = 0; spec < MAX_LEVEL; ++spec)
        {
            std::ostringstream os; os << "AiPlayerbot.PremadeSpecName." << cls << "." << spec;
            std::string specName = config.GetStringDefault(os.str().c_str(), "");
            if (!specName.empty())
            {
                std::ostringstream os; os << "AiPlayerbot.PremadeSpecProb." << cls << "." << spec;
                int probability = config.GetIntDefault(os.str().c_str(), 100);

                // [roster 2026-10-16] 入队自我介绍的中文台词
                {
                    // [fix 2026-10-16] spec 循环上限是 MAX_LEVEL(100)，而 specSayCn 只有 10 列 ⇒ 必须夹住索引，
                    // 否则越界写坏堆：表现为启动期 std::string::_M_assign 段错误（gdb 栈 #1 = LoadTalentSpecs）。
                    if (spec < 10)
                    {
                        std::ostringstream osk; osk << "AiPlayerbot.PremadeSpecSayCn." << cls << "." << spec;
                        specSayCn[cls][spec] = config.GetStringDefault(osk.str().c_str(), "");
                    }
                }

                TalentPath talentPath(spec, specName, probability);

                for (uint32 level = 10; level <= 100; level++)
                {
                    std::ostringstream os; os << "AiPlayerbot.PremadeSpecLink." << cls << "." << spec << "." << level;
                    std::string specLink = config.GetStringDefault(os.str().c_str(), "");
                    specLink = specLink.substr(0, specLink.find("#", 0));
                    specLink = specLink.substr(0, specLink.find(" ", 0));

                    if (!specLink.empty())
                    {
                        // [降峰 2026-10-15] 只为本服实际用到的等级区间建 TalentSpec。
                        // 原实现为每个职业×专精×等级(10..100) 都建一个完整 TalentSpec（约 2500 个），
                        // 本机剖析实测该步骤 +243MB；本站 bot 全部固定 70 级
                        // （RandomBotMinLevel=RandomBotMaxLevel=70）⇒ 其余等级纯属浪费。
                        // [A/B 诊断 2026-10-15] PB_NO_TALENT_FILTER=1 时不做等级过滤，用于量化该过滤的内存收益
                        static bool const noTalentFilter = std::getenv("PB_NO_TALENT_FILTER") != nullptr;
                        if (!noTalentFilter && (level < sPlayerbotAIConfig.randomBotMinLevel || level > sPlayerbotAIConfig.randomBotMaxLevel))
                            continue;

                        if (maxSpecLevel < level)
                            maxSpecLevel = level;

                        std::ostringstream out;

                        //Ignore bad specs.
                        if (!classSpecs[cls].baseSpec.CheckTalentLink(specLink, &out))
                        {
                            sLog.outErrorDb("Error with premade spec link: %s", specLink.c_str());
                            sLog.outErrorDb("%s", out.str().c_str());
                            continue;
                        }

                        TalentSpec linkSpec(&classSpecs[cls].baseSpec, specLink);
                        ++talentSpecsBuilt;

                        if (!linkSpec.CheckTalents(TalentSpec::LeveltoPoints(level), &out))
                        {
                            sLog.outErrorDb("Error with premade spec: %s", specLink.c_str());
                            sLog.outErrorDb("%s", out.str().c_str());
                            continue;
                        }


                        talentPath.talentSpec.push_back(linkSpec);
                    }

                    {
                        //Glyphs

                        using GlyphPriority = std::pair<std::string, uint32>;
                        using GlyphPriorityList = std::vector<GlyphPriority>;
                        using GlyphPriorityLevelMap = std::unordered_map<uint32, GlyphPriorityList>;
                        using GlyphPrioritySpecMap = std::unordered_map<uint32, GlyphPriorityLevelMap>;

                        std::ostringstream os; os << "AiPlayerbot.PremadeSpecGlyp." << cls << "." << spec << "." << level;

                        std::string glyphList = config.GetStringDefault(os.str().c_str(), "");
                        glyphList = glyphList.substr(0, glyphList.find("#", 0));
                        boost::trim_right(glyphList);

                        if (!glyphList.empty())
                        {
                            Tokens premadeSpecGlyphs = Qualified::getMultiQualifiers(glyphList, ",");

                            for (auto& glyph : premadeSpecGlyphs)
                            {
                                Tokens tokens = Qualified::getMultiQualifiers(glyph, "|");
                                std::string glyphName = "Glyph of " + tokens[0];
                                uint32 talentId = tokens.size() > 1 ? stoi(tokens[1]) : 0;

                                bool glyphFound = false;
                                for (auto& itemId : sRandomItemMgr.GetGlyphs(1 << (cls - 1)))
                                {
                                    ItemPrototype const* proto = sObjectMgr.GetItemPrototype(itemId);

                                    if (!proto)
                                        continue;

                                    if (proto->Name1 == glyphName)
                                    {
                                        glyphPriorityMap[cls][spec][level].push_back(std::make_pair(itemId, talentId));
                                        glyphFound = true;
                                        break;
                                    }
                                }

                                if (!glyphFound)
                                {
                                    sLog.outError("%s is not found for class %d (spec %d level %d)", glyphName.c_str(), cls, spec, level);
                                }

                            }
                        }
                    }
                }

                //Only add paths that have atleast 1 spec.
                if (talentPath.talentSpec.size() > 0)
                    classSpecs[cls].talentPath.push_back(talentPath);
            }
        }
    }

    if (classSpecs[1].talentPath.empty())
        sLog.outErrorDb("No premade specs found!!");
    else
    {
        if (maxSpecLevel < DEFAULT_MAX_LEVEL && randomBotMaxLevel < DEFAULT_MAX_LEVEL)
            sLog.outErrorDb("!!!!!!!!!!! randomBotMaxLevel and the talentspec levels are below this expansions max level. Please check if you have the correct config file!!!!!!");

    }
    sLog.outString("[TALENT] TalentSpecs built: %u (%s)", talentSpecsBuilt,
        std::getenv("PB_NO_TALENT_FILTER") ? "level filter OFF" : "level filter ON");
}

void PlayerbotAIConfig::LoadLLMDefaultPrompts(const std::string& fileName)
{
    std::ifstream file(fileName);
    if (!file.is_open())
    {
        sLog.outString("LLM default prompts file '%s' not found or unreadable.", fileName.c_str());
        return;
    }

    std::string line;
    uint32 loaded = 0;

    std::string likePattern = std::string("manual saved string::llmdefaultprompt>%");
    CharacterDatabase.escape_string(likePattern);

    while (std::getline(file, line))
    {
        boost::trim(line);
        if (line.empty() || line.front() == '#')
            continue;

        size_t delim = line.find("::");
        if (delim == std::string::npos)
        {
            sLog.outError("LLM prompts file '%s' contains invalid line (missing '::'): %s", fileName.c_str(), line.c_str());
            continue;
        }

        std::string name = line.substr(0, delim);
        std::string text = line.substr(delim + 2);
        boost::trim(name);
        boost::trim(text);

        if (name.empty())
        {
            sLog.outError("LLM prompts file '%s' contains empty name: %s", fileName.c_str(), line.c_str());
            continue;
        }

        auto result = CharacterDatabase.PQuery("SELECT guid FROM characters WHERE name = '%s' LIMIT 1", name.c_str());
        if (!result)
        {
            sLog.outError("Character '%s' not found in characters DB while loading '%s'.", name.c_str(), fileName.c_str());
            continue;
        }

        Field* fields = result->Fetch();
        uint32 guid = fields[0].GetUInt32();

        CharacterDatabase.PExecute(
            "DELETE FROM `ai_playerbot_db_store` WHERE `guid` = '%u' AND `key` = '%s' AND `value` LIKE '%s'",
            guid, "value", likePattern.c_str());

        std::string dbValue = std::string("manual saved string::llmdefaultprompt>") + text;
        CharacterDatabase.escape_string(dbValue);

        CharacterDatabase.PExecute(
            "INSERT INTO `ai_playerbot_db_store` (`guid`, `preset`, `key`, `value`) VALUES ('%u', '%s', '%s', '%s')",
            guid, "", "value", dbValue.c_str());

        ++loaded;
    }

    sLog.outString("Loaded %u LLM character personalities from %s", loaded, fileName.c_str());
}
