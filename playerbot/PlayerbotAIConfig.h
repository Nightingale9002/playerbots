#pragma once

#include "Config/Config.h"
#include "Talentspec.h"
#include "Globals/SharedDefines.h"
#include "SystemConfig.h"
#include <map>

class Player;
class PlayerbotMgr;
class ChatHandler;

// [diagnostic 2026-10-15] Log current process RSS (working set) with a tag.
// Enabled only when env PB_RSS_LOG is set; no-op otherwise. Used to quantify
// startup memory per phase on the local 32GB box before touching the cloud.
void PlayerbotLogRss(const char* tag);

#if PLATFORM == PLATFORM_WINDOWS
inline std::string _D_AIPLAYERBOT_CONFIG = "aiplayerbot.conf";
#else
inline std::string _D_AIPLAYERBOT_CONFIG = SYSCONFDIR "aiplayerbot.conf";
#endif

enum class BotCheatMask : uint32
{
    none = 0,
    taxi = 1 << 0,
    gold = 1 << 1,
    health = 1 << 2,
    mana = 1 << 3,
    power = 1 << 4,
    item = 1 << 5,
    cooldown = 1 << 6,
    repair = 1 << 7,
    movespeed = 1 << 8,
    attackspeed = 1 << 9,
    breath = 1 << 10,
    glyph = 1 << 11,
    quest = 1 << 12,    
    maxMask = 1 << 13
};

enum class BotAutoLogin : uint32
{
    DISABLED = 0,
    LOGIN_ALL_WITH_MASTER = 1,
    LOGIN_ONLY_ALWAYS_ACTIVE = 2
};

enum class BotSelfBotLevel : uint32
{
    DISABLED = 0,
    GM_ONLY = 1,
    ACTIVE_BY_COMMAND = 2,
    ALWAYS_ALLOWED = 3,
    ACTIVE_BY_LOGIN = 4, 
    ALWAYS_ACTIVE = 5
};

enum class BotAlwaysOnline : uint32
{
    DISABLED = 0,
    ACTIVE = 1,
    DISABLED_BY_COMMAND = 2
};

enum class BotLoginCriteriaType : uint8
{
    RACECLASS = 0,
    LEVEL = 1,
    RANGE_TO_PLAYER = 2,
    MAX_LOGIN_CRITERIA = 3
};

#define MAX_GEAR_PROGRESSION_LEVEL 6

class ConfigAccess
{
private:
    std::string m_filename;
    std::string m_envVarPrefix;
    std::unordered_map<std::string, std::string> m_entries; // keys are converted to lower case.  values cannot be.

public:
    std::vector<std::string> GetValues(const std::string& name) const;
    std::mutex m_configLock;
};

struct ParsedUrl {
    std::string hostname;
    std::string path;
    int port;
    bool https;
};

//GlyphPrioritySpecMap[specId][level] = {{glyphItemId, prereqTalentSpell}};
using GlyphPriority = std::pair<uint32, uint32>;
using GlyphPriorityList = std::vector<GlyphPriority>;
using GlyphPriorityLevelMap = std::unordered_map<uint32, GlyphPriorityList>;
using GlyphPrioritySpecMap = std::unordered_map<uint32, GlyphPriorityLevelMap>;

class PlayerbotAIConfig
{
public:
    PlayerbotAIConfig();
    static PlayerbotAIConfig& instance()
    {
        static PlayerbotAIConfig instance;
        return instance;
    }

public:
    bool Initialize();
    bool IsInRandomAccountList(uint32 id);
    bool IsFreeAltBot(uint32 guid);
    bool IsFreeAltBot(Player* player) {return IsFreeAltBot(player->GetGUIDLow());}
    bool IsInRandomQuestItemList(uint32 id);
	bool IsInPvpProhibitedZone(uint32 id);

    bool enabled;
    bool allowGuildBots;
    bool allowMultiAccountAltBots;
    uint32 globalCoolDown, reactDelay, maxWaitForMove, expireActionTime, dispelAuraDuration, passiveDelay, repeatDelay,
        errorDelay, rpgDelay, sitDelay, returnDelay, lootDelay;
    float sightDistance, spellDistance, reactDistance, grindDistance, lootDistance, groupMemberLootDistance, groupMemberLootDistanceWithActiveMaster,
        gatheringDistance, groupMemberGatheringDistance, groupMemberGatheringDistanceWithActiveMaster, shootDistance,
        fleeDistance, tooCloseDistance, meleeDistance, followDistance, raidFollowDistance, wanderMinDistance, wanderMaxDistance, whisperDistance, contactDistance,
        aoeRadius, rpgDistance, targetPosRecalcDistance, farDistance, healDistance, aggroDistance, proximityDistance, maxFreeMoveDistance, freeMoveDelay, walkDistance;
    uint32 criticalHealth, lowHealth, mediumHealth, almostFullHealth;
    uint32 lowMana, mediumMana;

    uint32 openGoSpell;
    bool randomBotAutologin;
    BotAutoLogin botAutologin;
    std::string randomBotMapsAsString;
    std::vector<uint32> randomBotMaps;
    std::list<uint32> randomBotQuestItems;
    std::list<uint32> randomBotAccounts;
    std::list<uint32> randomBotSpellIds;
    std::list<uint32> randomBotQuestIds;
    std::list<uint32> immuneSpellIds;
    std::list<std::pair<uint32, uint32>> freeAltBots;
    std::list<std::string> toggleAlwaysOnlineAccounts;
    std::list<std::string> toggleAlwaysOnlineChars;
    bool enableRandomTeleports;
    bool enableMinimalMove;
    uint32 randomBotTeleportDistance;
    // [spawn 2026-10-16] 站长口径：bot 固定出生点（沙塔斯），不再散布全世界
    bool fixedSpawnEnabled;
    uint32 fixedSpawnMapId;
    float fixedSpawnX, fixedSpawnY, fixedSpawnZ, fixedSpawnO;
    float fixedSpawnRadius;
    bool randomBotTeleportNearPlayer;
    uint32 transportTeleportType;
    uint32 randomBotTeleportNearPlayerMaxAmount;
    float randomBotTeleportNearPlayerMaxAmountRadius;
    uint32 randomBotTeleportMinInterval, randomBotTeleportMaxInterval;
    uint32 randomGearMaxLevel;
    uint32 randomGearMaxDiff;
    bool randomGearUpgradeEnabled;
    bool randomGearTabards;
    bool randomGearTabardsReplaceGuild;
    bool randomGearTabardsUnobtainable;
    float randomGearTabardsChance;
    std::list<uint32> randomGearBlacklist;
    std::list<uint32> randomGearWhitelist;
    bool randomGearProgression;
    float randomGearLoweringChance;
    bool rollBadItemsWithPlayer;
    // [不抢装备 2026-10-14] 组里有真人玩家时，机器人一律 PASS 装备竞拍（默认开启；站长口径"不抢装备"）
    bool passRollsWithRealPlayer;
    // [按副本换阶段 2026-10-14] mapId -> 装备阶段（AiPlayerbot.GearProgressionSystem.InstancePhases 解析结果）
    std::map<uint32, uint8> gearProgressionInstancePhases;
    // [性能 2026-10-14] 构建装备缓存时只建这些等级（AiPlayerbot.GearProgressionSystem.CacheLevels，逗号分隔；空=全部等级）。
    // 固定等级的服务器（DisableRandomLevels=1 + randombotStartingLevel=70）填 "70" 可把构建耗时降约 70 倍。
    std::vector<uint32> gearProgressionCacheLevels;
    // [按阶段生成 2026-10-15] 随机生成装备时使用的默认阶段（AiPlayerbot.GearProgressionSystem.DefaultPhase，0=团本前）
    uint8 gearProgressionDefaultPhase;
    float randomBotMaxLevelChance;
    float randomBotRpgChance;
    float usePotionChance;
    float attackEmoteChance;
    bool randomBotAutoCreate;
    uint32 minRandomBots, maxRandomBots;
    uint32 randomBotUpdateInterval, randomBotCountChangeMinInterval, randomBotCountChangeMaxInterval;
    bool randomBotTimedLogout, randomBotTimedOffline;
    uint32 minRandomBotInWorldTime, maxRandomBotInWorldTime;
    uint32 minRandomBotRandomizeTime, maxRandomBotRandomizeTime;
    uint32 minRandomBotChangeStrategyTime, maxRandomBotChangeStrategyTime;
    uint32 minRandomBotReviveTime, maxRandomBotReviveTime;
    uint32 minRandomBotPvpTime, maxRandomBotPvpTime;
    uint32 randomBotsMaxLoginsPerInterval;
    uint32 loginDatabaseDelayMs;
    uint32 randomBotsPerInterval;
    uint32 minRandomBotsPriceChangeInterval, maxRandomBotsPriceChangeInterval;
    // Fish location loading (world-wide fish points); off by default for this server.
    bool enableFishLocations;
    //Auction house settings
    bool shouldQueryAHListingsOutsideOfAH;
    std::list<uint32> ahOverVendorItemIds;
    std::list<uint32> vendorOverAHItemIds;
    bool botCheckAllAuctionListings;
    bool botsSaveEpics;
    //
    bool randomBotJoinLfg;
    bool logRandomBotJoinLfg;
    bool randomBotJoinBG;
    bool randomBotAutoJoinBG;
    uint32 randomBotBracketCount;
    bool randomBotLoginAtStartup;
    uint32 randomBotTeleLevel;
    bool logInGroupOnly, logValuesPerTick;
    uint32 actionHistorySize;   // per-bot action history ring buffer (0 = off)
    bool fleeingEnabled;
    bool summonAtInnkeepersEnabled;
    std::string combatStrategies, nonCombatStrategies, reactStrategies, deadStrategies;
    std::string randomBotCombatStrategies, randomBotNonCombatStrategies, randomBotReactStrategies, randomBotDeadStrategies;
    uint32 randomBotMinLevel, randomBotMaxLevel;
    float randomChangeMultiplier;
    uint32 specProbability[MAX_CLASSES][10];
    // [roster 2026-10-16] 站长口径：按「职业+种族」直接点名专精（-1 = 未指定，走原逻辑）
    int32 specPick[MAX_CLASSES][MAX_RACES];
    // [roster 2026-10-16] 入队时的中文自我介绍（每个专精一句，空则退回"我是<职业名>"）
    std::string specSayCn[MAX_CLASSES][10];
    // [gems 2026-10-16] 站长口径：优先用哪些宝石（物品id列表），仍然按插槽颜色匹配，匹配不上才放宽颜色
    std::vector<uint32> gemIds[MAX_CLASSES];
    std::string premadeLevelSpec[MAX_CLASSES][10][91]; //lvl 10 - 100
    uint32 classRaceProbabilityTotal;
    uint32 classRaceProbability[MAX_CLASSES][MAX_RACES];
    bool useFixedClassRaceCounts;
    using ClassRacePair = std::pair<uint8, uint8>;
    std::map<ClassRacePair, uint32> fixedClassRaceCounts;
    uint32 levelProbability[DEFAULT_MAX_LEVEL + 1];
    ClassSpecs classSpecs[MAX_CLASSES];
    GlyphPrioritySpecMap glyphPriorityMap[MAX_CLASSES];
    bool gearProgressionSystemEnabled;
    uint32 gearProgressionSystemItemLevels[MAX_GEAR_PROGRESSION_LEVEL][2];
    int32 gearProgressionSystemItems[MAX_GEAR_PROGRESSION_LEVEL][MAX_CLASSES][4][SLOT_EMPTY];
    std::string commandPrefix, commandSeparator;

    // [.bot 命令闸门 2026-10-17 站长口径] 非 GM（gmlevel < SEC_GAMEMASTER）玩家的 bot 命令黑/白名单。
    //   逗号分隔、大小写不敏感；黑名单先判（拒绝），再判白名单（不在白名单里也拒绝）。
    //   两个面都过这里：① `.bot <子命令>`（PlayerbotHolder::HandlePlayerbotCommand）
    //                  ② 对 bot 说话的聊天命令（PlayerbotAI::HandleCommand）
    std::vector<std::string> playerCommandBlacklist;
    std::vector<std::string> playerCommandWhitelist;
    std::string randomBotAccountPrefix;
    uint32 randomBotAccountCount;
    bool deleteRandomBotAccounts;
    uint32 randomBotGuildCount;
    bool deleteRandomBotGuilds;
    uint32 randomBotArenaTeamCount;
    bool deleteRandomBotArenaTeams;
    std::list<uint32> randomBotArenaTeams;
	bool RandombotsWalkingRPG;
	bool RandombotsWalkingRPGInDoors;
    bool boostFollow;
    bool turnInRpg;
    bool globalSoundEffects;
    bool shareTargets;
    std::list<uint32> randomBotGuilds;
	std::list<uint32> pvpProhibitedZoneIds;
    bool enableGreet;
    bool randomBotShowHelmet;
    bool randomBotShowCloak;
    bool disableRandomLevels;
    bool instantRandomize;
    bool gearscorecheck;
    int32 levelCheck;
	bool randomBotPreQuests;
    float playerbotsXPrate;
    bool disableBotOptimizations;
    bool disableActivityPriorities;
    bool forceActiveWhenNearPlayer;
    bool limitCombatActivity;
    bool guildOrderAlwaysActive;
    uint32 botActiveAlone;
    uint32 diffWithPlayer;
    uint32 diffEmpty;
    uint32 minEnchantingBotLevel;
    uint32 randombotStartingLevel;
    bool randomBotSayWithoutMaster;
    bool randomBotInvitePlayer;
    bool randomBotGroupNearby;
    // [threat 2026-10-16 站长口径] 自己仇恨达到坦克仇恨的这个百分比就压着不出手（0 = 关）
    uint8 waitForAttackThreatPercent;
    // [threat 2026-10-16 站长口径] 开场"等坦克 pull back"的秒数上限（默认值，游戏内可单 bot 覆盖）
    uint8 waitForAttackTime;
    // [threat 2026-10-16 站长口径] 坦克离我 ≤ 这个距离且对该目标有仇恨 ⇒ 视为"已拉回来"，立刻开打
    float waitForAttackTankBackDistance;
    // [pull-gate 2026-10-19 站长口径] "坦克乱开怪"：还没进战斗时，附近敌对数量达到这个值就不主动开怪
    //   （半径由 pullHostileRadius 决定；0 = 关，退回旧行为）
    uint8 pullHostileLimit;
    // [pull-gate 2026-10-19] 上面那个判据的半径（码）
    float pullHostileRadius;
    // [defend-self 2026-10-19 站长口径] "一直挨打也不知道跑"：非坦克被近战贴脸且血量低于这个百分比时走位
    //   （0 = 关）
    uint8 selfDefendHealth;
    // [critter 2026-10-18 站长口径] "t 的战斗行为应该跳过小动物等"：
    //   目标筛选阶段直接不把 critter（小动物/无害 NPC）当候选，1 = 跳过（默认），0 = 老行为。
    bool skipCritters;
    // [threat 2026-10-16 站长口径] 等坦克时保持的安全距离（站长口径：一般 15 码合适）
    float waitForAttackSafeDistance;
    // [tank-wait 2026-10-18 站长口径] "如果有人掉队 T 不应该继续往前走"：
    //   坦克在队伍里有人掉队(同图/同副本、活着、落在队伍参考点和自己后面超过这个距离)时停下原地等。
    //   0 = 关（退回旧行为：坦克一直往前走）。判定/行为见 PlayerbotAI::ShouldTankWaitForGroup()。
    bool tankWaitForGroupEnabled;
    // [tank-wait 2026-10-18 站长口径] 掉队判定的距离阈值（码）：队友离坦克 > 这个值才算掉队
    float tankWaitForGroupDistance;
    // [tank-wait 2026-10-18 站长口径] 坦克最多等多少秒；超时后恢复前进，并给一个等长的冷却期（防卡死/防抖）
    uint32 tankWaitForGroupTimeout;
    // [safe-move 2026-10-18 站长口径] "远程 DPS 乱跑引到旁边怪堆"：
    //   战斗内移动选点前，候选点必须离"最近的其它敌对单位" > 它的仇恨半径(Unit::GetAttackDistance，
    //   兜底 AiPlayerbot.AggroDistance) + CombatMoveSafetyMargin；不满足就退化为原地。
    //   判定在 MovementAction::IsCombatMovePointSafe()（附近敌对单位列表按 bot 缓存 1 秒）。
    bool combatMoveSafetyEnabled;
    // [safe-move 2026-10-18 站长口径] 仇恨半径之外再留的余量（码，默认 5）
    float combatMoveSafetyMargin;
    // [pull 2026-10-18 站长口径] 坦克"开怪目标必须同一层"判据：|bot.z - target.z| 超过这个值就不当开怪目标
    //   （多楼层副本：卡拉赞 / 黑石深渊 / 厄运之槌）。0 = 关（退回旧行为）。
    float pullSameLevelZDistance;
    // [pull 2026-10-18 站长口径] 2D 距离超过这个值才为候选目标做一次 navmesh 路径校验（带缓存 + 每秒限流）
    float pullPathCheckDistance;
    // [group 2026-10-16] 队伍里没有真人时的自动解散：0 关 / 1 只处理"真人已离开"的队伍 / 2 任何纯机器人队伍
    uint8 autoDisbandBotOnlyGroups;
    // [BOSS 战策略 2026-10-16] 按队伍当前目标的 BOSS entry 自动挂载 custom::boss_<entry>（见功能更新手册「副本/BOSS 战策略」）
    bool bossStrategies;
    // [副本推进 2026-10-18] 运行期按波推进总开关（只对 ai_playerbot_dungeon_route.verified=1 的图生效）
    bool dungeonRoute;
    // [自动进本 2026-10-18] 默认是否自动进本（运行时开关与 .bot route on/off 共用）
    bool autoDungeonRun;
    // 目标副本 map id 列表（逗号分隔），空 = 不自动进本
    std::string autoDungeonMaps;
    // [仿真 2026-10-18] 可控场景注入总开关（`.bot sim start/stop/status`）：
    //   **只在本地 WSL 仿真开 1**；云端必须 0（云端开了也只允许 GM 手动执行，仍不该开）。
    bool simMode;
    // [2026-10-19] 允许"在水里/游泳时也走 mmap 路径"。
    //   上游 playerbots 在 IsInWater/游泳时把 generatePath 关掉（退化成直线游），
    //   水池/落差地形会卡住；默认 0 保持上游行为，本地仿真开 1。
    bool waterPath;
    // [2026-10-19] 副本推进的移动自救：卡住 2.5 秒小跳一次（跳点表/DungeonRoute 用）
    bool routeJumpAssist;
    // [2026-10-19] 副本推进的升降台/传送点：到连接点等 3 秒后传送到对岸（本地仿真/脚本化）
    bool routeLiftAssist;
    bool randomBotRaidNearby;
    bool randomBotGuildNearby;
    bool randomBotFormGuild;
    bool randomBotRandomPassword;
    bool inviteChat;
    bool enableOffSpecStrategies;
    bool useWanderAsDefaultFollowStrategy;
    std::string defaultFormation;

    uint32 guildMaxBotLimit;

    bool enableBroadcasts;
    uint32 broadcastChanceMaxValue;

    uint32 broadcastToGuildGlobalChance;
    uint32 broadcastToWorldGlobalChance;
    uint32 broadcastToGeneralGlobalChance;
    uint32 broadcastToTradeGlobalChance;
    uint32 broadcastToLFGGlobalChance;
    uint32 broadcastToLocalDefenseGlobalChance;
    uint32 broadcastToWorldDefenseGlobalChance;
    uint32 broadcastToGuildRecruitmentGlobalChance;
    uint32 broadcastToSayGlobalChance;
    uint32 broadcastToYellGlobalChance;

    uint32 broadcastChanceLootingItemPoor;
    uint32 broadcastChanceLootingItemNormal;
    uint32 broadcastChanceLootingItemUncommon;
    uint32 broadcastChanceLootingItemRare;
    uint32 broadcastChanceLootingItemEpic;
    uint32 broadcastChanceLootingItemLegendary;
    uint32 broadcastChanceLootingItemArtifact;

    uint32 broadcastChanceQuestAccepted;
    uint32 broadcastChanceQuestUpdateObjectiveCompleted;
    uint32 broadcastChanceQuestUpdateObjectiveProgress;
    uint32 broadcastChanceQuestUpdateFailedTimer;
    uint32 broadcastChanceQuestUpdateComplete;
    uint32 broadcastChanceQuestTurnedIn;

    uint32 broadcastChanceKillNormal;
    uint32 broadcastChanceKillElite;
    uint32 broadcastChanceKillRareelite;
    uint32 broadcastChanceKillWorldboss;
    uint32 broadcastChanceKillRare;
    uint32 broadcastChanceKillUnknown;
    uint32 broadcastChanceKillPet;
    uint32 broadcastChanceKillPlayer;

    uint32 broadcastChanceLevelupGeneric;
    uint32 broadcastChanceLevelupTenX;
    uint32 broadcastChanceLevelupMaxLevel;

    uint32 broadcastChanceSuggestInstance;
    uint32 broadcastChanceSuggestQuest;
    uint32 broadcastChanceSuggestGrindMaterials;
    uint32 broadcastChanceSuggestGrindReputation;
    uint32 broadcastChanceSuggestSell;
    uint32 broadcastChanceSuggestSomething;

    uint32 broadcastChanceSuggestSomethingToxic;

    uint32 broadcastChanceSuggestToxicLinks;
    std::string toxicLinksPrefix;
    uint32 toxicLinksRepliesChance;

    uint32 broadcastChanceSuggestThunderfury;
    uint32 thunderfuryRepliesChance;

    uint32 broadcastChanceGuildManagement;

    uint32 guildRepliesRate;

    uint32 botAcceptDuelMinimumLevel;

    bool talentsInPublicNote;
    bool nonGmFreeSummon;

    BotSelfBotLevel selfBotLevel;
    uint32 iterationsPerTick;

    std::string autoPickReward;
    bool autoEquipUpgradeLoot;
    bool autoEnchantUpgradeLoot;
    bool syncQuestWithPlayer;
    bool syncQuestForPlayer;
    std::string autoTrainSpells;
    std::string autoPickTalents;
    bool autoLearnTrainerSpells;
    bool autoLearnQuestSpells;
    bool autoLearnDroppedSpells;
    bool autoDoQuests;
    bool syncLevelWithPlayers;
    uint32 syncLevelMaxAbove, syncLevelNoPlayer;
    bool syncAltLevelToMaster;
    uint32 tweakValue; //Debugging config
    float respawnModNeutral, respawnModHostile;
    uint32 respawnModThreshold, respawnModMax;
    bool respawnModForPlayerBots, respawnModForInstances;

    bool randomBotLoginWithPlayer;
    bool asyncBotLogin, preloadHolders;
    uint32 freeRoomForNonSpareBots;
    uint32 loginBotsNearPlayerRange;
    std::vector<std::string> defaultLoginCriteria;
    std::vector<std::vector<std::string>> loginCriteria;
    std::vector<std::string> startupRunTests;
    bool startupRunTestsPending = false;

    bool jumpInBg;
    bool jumpWithPlayer;
    bool jumpFollow;
    bool jumpChase;
    bool useKnockback;
    float jumpNoCombatChance;
    float jumpMeleeInCombatChance;
    float jumpRandomChance;
    float jumpInPlaceChance;
    float jumpBackwardChance;
    float jumpHeightLimit;
    float jumpVSpeed;
    float jumpHSpeed;

    std::mutex m_logMtx;

    std::list<std::string> allowedLogFiles;
    std::list<std::string> debugFilter;

    std::unordered_map <std::string, std::pair<FILE*, bool>> logFiles;

    uint32 botCheatMask = 0;
    uint32 rndBotCheatMask = 0;

    std::vector<std::string> BotCheatMaskName = { "taxi", "gold", "health", "mana", "power", "item", "cooldown", "repair", "movespeed", "attackspeed", "breath", "glyph", "quest", "maxMask" };

    struct worldBuff{
        uint32 spellId;
        uint32 factionId = 0;
        uint32 classId = 0;
        uint32 specId = 0;
        uint32 minLevel = 0;
        uint32 maxLevel = 0;
        uint32 eventId = 0;
    };

    std::vector<worldBuff> worldBuffs;

    int commandServerPort;
    bool perfMonEnabled;
    bool bExplicitDbStoreSave = false;

    //LM BEGIN
    std::string llmApiEndpoint, llmApiKey, llmApiJson, llmPrePrompt, llmPreRpgPrompt, llmPrompt, llmPostPrompt, llmResponseStartPattern, llmResponseEndPattern, llmResponseDeletePattern, llmResponseSplitPattern;
    uint32 llmEnabled, llmContextLength, llmBotToBotChatChance, llmGenerationTimeout, llmMaxSimultaniousGenerations, llmRpgAIChatChance;
    bool llmGlobalContext;
    ParsedUrl llmEndPointUrl;
    std::set<uint32> llmBlockedReplyChannels;
    //LM END

    uint32 EatDrinkMinDistance = 5;
    uint32 EatDrinkMaxDistance = 1000;

    std::string GetValue(std::string name);
    void SetValue(std::string name, std::string value);

    void loadFreeAltBotAccounts();

    std::string GetTimestampStr();

    bool hasLog(std::string fileName) { return std::find(allowedLogFiles.begin(), allowedLogFiles.end(), fileName) != allowedLogFiles.end(); };
    bool openLog(std::string fileName, char const* mode = "a", bool haslog = false);
    bool isLogOpen(std::string fileName) { auto it = logFiles.find(fileName); return it != logFiles.end() && it->second.second;}
    void log(std::string fileName, const char* str, ...);

    void logEvent(PlayerbotAI* ai, std::string eventName, std::string info1 = "", std::string info2 = "");
    void logEvent(PlayerbotAI* ai, std::string eventName, ObjectGuid guid, std::string info2);

    bool CanLogAction(PlayerbotAI* ai, std::string actionName, bool isExecute, std::string lastActionName);

private:
    void LoadTalentSpecs();
    void LoadLLMDefaultPrompts(const std::string& fileName);

    Config config;
};

#define sPlayerbotAIConfig MaNGOS::Singleton<PlayerbotAIConfig>::Instance()

