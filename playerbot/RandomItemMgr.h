#ifndef _RandomItemMgr_H
#define _RandomItemMgr_H

#include "Common.h"
#include "PlayerbotAIBase.h"
#include "playerbot/AiFactory.h"
#ifdef CMANGOS
#include "Entities/Player.h"
#endif
#ifdef MANGOS
#include "Object/Player.h"
#endif
#include "strategy/values/ItemUsageValue.h"

enum RandomItemType
{
    RANDOM_ITEM_GUILD_TASK,
    RANDOM_ITEM_GUILD_TASK_REWARD_EQUIP_BLUE,
    RANDOM_ITEM_GUILD_TASK_REWARD_EQUIP_GREEN,
    RANDOM_ITEM_GUILD_TASK_REWARD_TRADE,
    RANDOM_ITEM_GUILD_TASK_REWARD_TRADE_RARE
};

#define MAX_STAT_SCALES 32

enum ItemSource
{
    ITEM_SOURCE_NONE,
    ITEM_SOURCE_DROP,
    ITEM_SOURCE_VENDOR,
    ITEM_SOURCE_QUEST,
    ITEM_SOURCE_CRAFT,
    ITEM_SOURCE_PVP
};

enum ItemSpecType
{
    ITEM_SPEC_NONE = 0x0,
    ITEM_SPEC_SPELL_DAMAGE = 0x01,
    ITEM_SPEC_SPELL_HEALING = 0x02,
    ITEM_SPEC_ATTACK = 0x04,
    ITEM_SPEC_TANK = 0x08,
    ITEM_SPEC_CASTER = 0x10,
};

struct WeightScaleInfo
{
    uint32 id;
    std::string name;
    uint8 classId;
};

struct WeightScaleStat
{
    std::string stat;
    uint32 weight;
};

struct ItemInfoEntry
{
    ItemInfoEntry() : minLevel(0), itemLevel(0), source(0), team(0), repRank(0), repFaction(0), reqSkill(0), reqSkillRank(0), pvpRank(0), quality(0), slot(0), itemId(0)
    {
        for (int i = 1; i <= MAX_STAT_SCALES; ++i)
        {
            weights[i] = 0;
        }
        itemSpec = ITEM_SPEC_NONE;
    }

    std::map<uint32, uint32> weights;
    uint32 minLevel;
    uint32 itemLevel;
    uint32 source;
    std::list<uint32> sourceIds;
    uint32 team;
    uint32 repRank;
    uint32 repFaction;
    uint32 reqSkill;
    uint32 reqSkillRank;
    uint32 pvpRank;
    uint32 quality;
    uint32 slot;
    uint32 itemId;
    ItemSpecType itemSpec;
};

typedef std::vector<WeightScaleStat> WeightScaleStats;
//typedef std::map<WeightScaleInfo, WeightScaleStats> WeightScaleList;

struct WeightScale
{
    WeightScaleInfo info;
    WeightScaleStats stats;
};

//typedef std::map<uint32, WeightScale> WeightScales;

class RandomItemPredicate
{
public:
    virtual ~RandomItemPredicate(){};
    virtual bool Apply(ItemPrototype const* proto) = 0;
};

typedef std::vector<uint32> RandomItemList;
typedef std::map<RandomItemType, RandomItemList> RandomItemCache;

class BotEquipKey
{
public:
    uint32 level;
    uint8 clazz;
    uint8 spec;
    uint8 slot;
    uint32 quality;
    uint64 key;

public:
    BotEquipKey() : level(0), clazz(0), spec(0), slot(0), quality(0), key(GetKey()) {}
    BotEquipKey(uint32 level, uint8 clazz, uint8 spec, uint8 slot, uint32 quality) : level(level), clazz(clazz), spec(spec), slot(slot), quality(quality), key(GetKey()) {}
    BotEquipKey(BotEquipKey const& other)  : level(other.level), clazz(other.clazz), spec(other.spec), slot(other.slot), quality(other.quality), key(GetKey()) {}

private:
    uint64 GetKey();

public:
    bool operator< (const BotEquipKey& other) const
    {
        return other.key < this->key;
    }
};

typedef std::map<BotEquipKey, RandomItemList> BotEquipCache;

class RandomItemMgr
{
    public:
        RandomItemMgr();
        virtual ~RandomItemMgr();
        static RandomItemMgr& instance()
        {
            static RandomItemMgr instance;
            return instance;
        }

	public:
        void Init();
        static bool HandleConsoleCommand(ChatHandler* handler, char const* args);
        RandomItemList Query(uint32 level, RandomItemType type, RandomItemPredicate* predicate);
        RandomItemList Query(uint32 level, uint8 clazz, uint8 specId, uint8 slot, uint32 quality);
        uint32 GetUpgrade(Player* player, std::string spec, uint8 slot, uint32 quality, uint32 itemId);
        std::vector<uint32> GetUpgradeList(Player* player, uint32 specId, uint8 slot, uint32 quality, uint32 itemId, uint32 amount = 1);
        bool HasStatWeight(uint32 itemId);
        bool CanBuyFromVendor(Player* player, uint32 itemId, uint32 creatureId);
        bool HasSameQuestRewards(Player* player, uint32 itemId);
        uint32 GetMinLevelFromCache(uint32 itemId);
        uint32 GetStatWeight(Player* player, uint32 itemId);
        uint32 GetLiveStatWeight(Player* player, uint32 itemId, uint32 specId = 0);
        uint32 GetStatWeight(uint32 itemId, uint32 specId);
        uint32 GetBestRandomEnchantStatWeight(uint32 itemId, uint32 specId);
        uint32 GetRandomItem(uint32 level, RandomItemType type, RandomItemPredicate* predicate = NULL);
        uint32 GetAmmo(uint32 level, uint32 subClass);
        uint32 GetRandomPotion(uint32 level, uint32 effect);
        uint32 GetRandomFood(uint32 level, uint32 category);
        uint32 GetFood(uint32 level, uint32 category);
        uint32 GetRandomTrade(uint32 level);
        std::vector<uint32> GetGemsList();

        uint32 CalculateRandomEnchantId(uint8 playerclass, uint8 spec, ItemPrototype const* proto);
        uint32 CalculateBestRandomEnchantId(uint8 playerclass, uint8 spec, uint32 itemId);
        // [cap 2026-10-16] 站长口径（方案B）：上限型属性"达标降权"。
        // 传入 Player* 时，若该 (专精,属性) 有上限记录且角色当前已达标，就用记录里的"达标后权重"。
        uint32 CalculateEnchantWeight(uint8 playerclass, uint8 spec, uint32 enchantId, Player* player = nullptr);
        uint32 CalculateRandomPropertyWeight(uint8 playerclass, uint8 spec, int32 randomPropertyId);
        uint32 CalculateGemWeight(uint8 playerclass, uint8 spec, uint32 gemId);
        uint32 CalculateSocketWeight(uint8 playerclass, ItemQualifier& qualifier, uint8 spec, Player* player = nullptr);

        uint32 CalculateStatWeight(uint8 playerclass, uint8 spec, ItemPrototype const* proto, ItemSpecType &itSpec, Player* player = nullptr);
        uint32 ItemStatWeight(Player* player, ItemQualifier& qualifier);
        uint32 ItemStatWeight(Player* player, Item* item);

        uint32 CalculateSingleStatWeight(uint8 playerclass, uint8 spec, std::string stat, uint32 value, Player* player = nullptr);
        // [cap 2026-10-16] 上限表：specId + 属性名 -> (达标所需等级值, 达标后权重[, 备选属性][, 二级上限])
        //   cap == 0 表示"该属性自己没有上限，由四维(avoid4)统一门控"（闪避/招架/格挡）
        //   altField/altCap = 备选达标途径（如熊德"韧性 103"可替代"防御等级 156"免暴）
        //   cap2/capped2    = 二级硬上限（如防战精准硬上限 56≈221 等级：消掉 BOSS 招架后再堆精准收益为 0）
        //   field == "armor" 时比较的是角色护甲值（Unit::GetArmor()），不是战斗等级
        struct WeightCap { uint32 cap; uint32 capped; std::string altField; uint32 altCap; uint32 cap2; uint32 capped2; };
        std::map<std::pair<uint32, std::string>, WeightCap> m_weightCaps;
        // [cap 2026-10-16] "达标降权"上下文：加权函数原本只有 (职业,专精)，判不了是否达标，
        // 用一个作用域守卫把当前角色挂进来（模块这段逻辑只跑在世界线程，单线程使用）。
        Player* m_weightPlayer = nullptr;
        struct WeightPlayerScope
        {
            explicit WeightPlayerScope(Player* p) : m_prev(instance().m_weightPlayer) { instance().m_weightPlayer = p; }
            ~WeightPlayerScope() { instance().m_weightPlayer = m_prev; }
            WeightPlayerScope(WeightPlayerScope const&) = delete;
            WeightPlayerScope& operator=(WeightPlayerScope const&) = delete;
            Player* m_prev;
        };
        void LoadWeightCaps();
        bool HasWeightCaps(uint32 spec);
        uint32 ApplyWeightCap(Player* player, uint32 spec, std::string const& stat, uint32 precapWeight);
        // [cap 2026-10-16] 四维（未命中+闪避+招架+格挡），单位：万分之一（10240 = 102.4%）
        // spec 用于站长口径的差异：防骑未命中基准 365 且"开神盾后"格挡 +30%
        uint32 GetAvoidanceBasisPoints(Player* player, uint32 spec = 0);
        bool CanEquipArmor(uint8 clazz, uint8 spec, uint32 level, ItemPrototype const* proto);
        bool ShouldEquipArmorForSpec(uint8 playerclass, uint8 spec, ItemPrototype const* proto);
        bool CanEquipWeapon(uint8 clazz, ItemPrototype const* proto);
        bool ShouldEquipWeaponForSpec(uint8 playerclass, uint8 spec, ItemPrototype const* proto);
        bool CheckItemSpec(uint8 spec, ItemSpecType itSpec);
        float GetItemRarity(uint32 itemId);
        uint32 GetQuestIdForItem(uint32 itemId);
        std::vector<uint32> GetQuestIdsForItem(uint32 itemId);
        std::string GetPlayerSpecName(Player* player);
        uint32 GetPlayerSpecId(Player* player);
    // [gear 2026-10-16] 本职业第一个已定义专精 id（兜底，见 RandomItemMgr.cpp）
    uint32 GetDefaultSpecIdForClass(uint8 cls);
        std::vector<uint32> GetGlyphs(uint8 playerClass) { return glyphCache.at(playerClass); }
    private:
        void BuildRandomItemCache();
        void BuildEquipCache();
        void BuildItemInfoCache();
        void BuildAmmoCache();
        void BuildFoodCache();
        void BuildPotionCache();
        void BuildTradeCache();
        void BuildRarityCache();
        void BuildGlyphCache();
        void LoadRandomEnchantments();
        bool CanEquipItem(BotEquipKey key, ItemPrototype const* proto);
        bool CanEquipItemNew(ItemPrototype const* proto);
        void AddItemStats(uint32 mod, uint8 &sp, uint8 &ap, uint8 &tank);
        bool CheckItemStats(uint8 clazz, uint8 sp, uint8 ap, uint8 tank);

    private:
        std::map<uint32, RandomItemCache> randomItemCache;
        std::map<RandomItemType, RandomItemPredicate*> predicates;
        BotEquipCache equipCache;
        std::map<EquipmentSlots, std::set<InventoryType> > viableSlots;
        std::map<uint32, std::map<uint32, uint32> > ammoCache;
        std::map<uint32, std::map<uint32, std::vector<uint32> > > potionCache;
        std::map<uint32, std::map<uint32, std::vector<uint32> > > foodCache;
        std::map<uint32, std::vector<uint32> > tradeCache;
        std::map<uint32, float> rarityCache;
        std::map<uint32, WeightScale> m_weightScales;
        std::map<std::string, uint32 > weightStatLink;
        std::map<uint32, std::string > ItemStatLink;
        std::map<std::string, uint32 > weightRatingLink;
        std::map<uint32, ItemInfoEntry*> itemInfoCache;
        std::map<uint32, std::vector<uint32> > randomEnchantsCache;
        std::map<uint8, std::vector<uint32>> glyphCache;
};

#define sRandomItemMgr RandomItemMgr::instance()

#endif
