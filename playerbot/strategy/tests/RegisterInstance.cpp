#include "playerbot/playerbot.h"
#include "TestRegistry.h"
#include "playerbot/TravelMgr.h"
#include "playerbot/TravelNode.h"

#include <algorithm>
#include <functional>
#include <sstream>

#include "Server/DBCStores.h"

using namespace ai;

namespace
{
    using ScenarioParams = std::map<std::string, std::string>;

    bool ResolveInstanceEntry(const MapEntry* mapEntry, std::string& mapName, GuidPosition& entry)
    {


        TravelNode* best = nullptr;
        for (auto& node : sTravelNodeMap.getNodes())
        {
            if (node->getMapId() != mapEntry->MapID)
                continue;

            std::string nodeName = node->getName();
            std::transform(nodeName.begin(), nodeName.end(), nodeName.begin(), ::tolower);
            if (nodeName.find("entrance") != std::string::npos)
            {
                best = node;
                break;
            }

            if (!best && node->isPortal())
                best = node;
        }

        if (!best)
        {
            static std::set<uint32> loggedMaps;
            if (loggedMaps.insert(mapEntry->MapID).second)
                sLog.outError("[TESTGEN] no travel node on instance map %u (%s) - scenario tests skipped for it",
                    mapEntry->MapID, mapEntry->name[0]);
            return false;
        }


        mapName = mapEntry->name[0];
        mapName.erase(std::remove_if(mapName.begin(), mapName.end(), ::isspace), mapName.end());
        mapName += "_inside";
        TestRegistry::RegisterNamedLocation(mapName, GuidPosition(ObjectGuid(), *best->getPosition()));
        return TestRegistry::ParseLocation(mapName, entry) && entry.getMapId() == mapEntry->MapID;
    }
}

std::string TestRegistry::ApplyScenarioParams(const std::string& line, const std::map<std::string, std::string>& params)
{
    std::string expanded = line;
    for (const auto& pair : params)
    {
        const std::string token = "$(" + pair.first + ")";
        size_t pos = 0;
        while ((pos = expanded.find(token, pos)) != std::string::npos)
        {
            expanded.replace(pos, token.length(), pair.second);
            pos += pair.second.length();
        }
    }

    return expanded;
}

std::vector<std::string> TestRegistry::ApplyScenarioParams(const std::vector<std::string>& script,
    const std::map<std::string, std::string>& params)
{
    std::vector<std::string> expanded;
    expanded.reserve(script.size());
    for (const std::string& line : script)
        expanded.push_back(ApplyScenarioParams(line, params));

    return expanded;
}

namespace
{
    // File-local duplicate of the (private) TestRegistry::ApplyScenarioParams line expansion;
    // kept here so the header stays untouched.
    std::string ExpandScenarioLine(const std::string& line, const ScenarioParams& params)
    {
        std::string expanded = line;
        for (const auto& pair : params)
        {
            const std::string token = "$(" + pair.first + ")";
            size_t pos = 0;
            while ((pos = expanded.find(token, pos)) != std::string::npos)
            {
                expanded.replace(pos, token.length(), pair.second);
                pos += token.length();
            }
        }

        return expanded;
    }

    // Encounter-teleport approach point (operator request, 2026-09-30): instead of dropping the raid
    // on top of the boss, place it 1.5x the boss's aggro radius away, on the travel-node path from
    // the boss toward an instance exit (i.e. where a group arriving through the instance would stand).
    // Aggro radius mirrors the core's documented model (Unit::GetAttackDistance): 18 yd at equal
    // level, -1 yd per player level above the mob, floored at 5 yd, scaled by the aggro rate config.
    // Returns the point as a "mapId;x;y;z;0" string; falls back to the boss position itself when no
    // route/exit can be resolved (with a [TESTGEN] note).
    std::string ComputeBossApproachCoords(const MapEntry* mapEntry, const WorldPosition& bossPos,
                                          uint32 bossLevel, uint32 botLevel)
    {
        float aggroRate = sWorld.getConfig(CONFIG_FLOAT_RATE_CREATURE_AGGRO);
        if (aggroRate <= 0.0f)
            aggroRate = 1.0f;

        const float aggroRadius = std::max(5.0f, 18.0f - float(int32(botLevel) - int32(bossLevel))) * aggroRate;
        const float approachDist = 1.5f * aggroRadius;

        auto formatCoords = [](const WorldPosition& pos)
        {
            std::ostringstream out;
            out << pos.getMapId() << ";" << pos.getX() << ";" << pos.getY() << ";" << pos.getZ() << ";0";
            return out.str();
        };

        // Boss node: nearest travel node on the instance map. Exit node: name contains "exit".
        TravelNode* bossNode = nullptr;
        TravelNode* exitNode = nullptr;
        float bossNodeDist = FLT_MAX;

        for (TravelNode* node : sTravelNodeMap.getNodes())
        {
            if (!node || node->getMapId() != mapEntry->MapID)
                continue;

            const float dist = node->getDistance(bossPos);
            if (dist < bossNodeDist)
            {
                bossNodeDist = dist;
                bossNode = node;
            }

            std::string nodeName = node->getName();
            std::transform(nodeName.begin(), nodeName.end(), nodeName.begin(), ::tolower);
            if (nodeName.find("exit") != std::string::npos && (!exitNode || dist < exitNode->getDistance(bossPos)))
                exitNode = node;
        }

        if (!exitNode)
        {
            static std::set<uint32> loggedMaps;
            if (loggedMaps.insert(mapEntry->MapID).second)
                sLog.outError("[TESTGEN] no instance-exit travel node on map %u (%s) - encounter approaches via entrance node",
                    mapEntry->MapID, mapEntry->name[0]);

            std::string entranceName = mapEntry->name[0];
            entranceName.erase(std::remove_if(entranceName.begin(), entranceName.end(), ::isspace), entranceName.end());
            entranceName += "_inside";
            GuidPosition entry;
            if (TestRegistry::ParseLocation(entranceName, entry))
            {
                for (TravelNode* node : sTravelNodeMap.getNodes())
                {
                    if (node && node->getMapId() == mapEntry->MapID && node->getDistance(entry) < 5.0f)
                        exitNode = node;
                }
            }
        }

        if (!exitNode || !bossNode)
            return formatCoords(bossPos);

        // Polyline: boss position -> (route node positions) -> exit node position.
        std::vector<WorldPosition> line;
        line.push_back(bossPos);

        TravelNodeRoute route = sTravelNodeMap.getRoute(bossNode, exitNode);
        if (!route.isEmpty())
        {
            bool first = true;
            for (TravelNode* node : route.getNodes())
            {
                if (first && node == bossNode)
                    continue;
                line.push_back(*node->getPosition());
                first = false;
            }
        }

        line.push_back(*exitNode->getPosition());

        // Walk the polyline from the boss and interpolate where the approach distance is crossed.
        float travelled = 0.0f;
        for (size_t i = 0; i + 1 < line.size(); ++i)
        {
            const WorldPosition& from = line[i];
            const WorldPosition& to = line[i + 1];
            const float segLen = from.distance(to);
            if (segLen <= 0.0f)
                continue;

            if (travelled + segLen >= approachDist)
            {
                const float t = (approachDist - travelled) / segLen;
                const WorldPosition approachPos(bossPos.getMapId(),
                    from.getX() + (to.getX() - from.getX()) * t,
                    from.getY() + (to.getY() - from.getY()) * t,
                    from.getZ() + (to.getZ() - from.getZ()) * t,
                    from.getO());
                return formatCoords(approachPos);
            }
            travelled += segLen;
        }

        // Route shorter than the approach distance: use its midpoint (still on the real path).
        static std::set<uint32> loggedShort;
        if (loggedShort.insert(mapEntry->MapID).second)
            sLog.outError("[TESTGEN] approach distance %.1f exceeds route on map %u (%s) - using route midpoint",
                approachDist, mapEntry->MapID, mapEntry->name[0]);

        return formatCoords(line[line.size() / 2]);
    }

    // Difficulty variants (operator request, 2026-09-30): every instance scenario registers three
    // times, differing only in the bots' level:
    //   MIN   = the dungeon's levelMin            -> test name gets a "_min" suffix
    //   BOOST = levelMin + 10 (the old default)   -> keeps the untagged name, so existing test
    //                                              names and result history stay valid
    //   MAX   = the tree's player level cap       -> test name gets a "_max" suffix
    //           (DEFAULT_MAX_LEVEL is 60/70/80 across classic/tbc/wotlk trees)
    // MIN is the only variant that fights the content at true level (the other two outlevel it);
    // expect the trash templates' pass monitors (dead mobs / party xp) to be the hard ones there.
    void RegisterInstanceScenario(const char* prefix, const std::string& bossName,
                                  const std::vector<std::string>& tmpl, const ScenarioParams& params, uint32 levelMin,
                                  const std::function<void(ScenarioParams&, uint32 botLevel)>& adjustPerVariant = {})
    {
        struct Variant { const char* suffix; uint32 level; };
        std::vector<Variant> variants = {
            { "_min", levelMin },
            { "",     levelMin + 10 },
            { "_max", uint32(DEFAULT_MAX_LEVEL) },
        };
#ifdef MANGOSBOT_TWO
        if (70 <= DEFAULT_MAX_LEVEL)
            variants.push_back({ "_70", 70 });
#endif

        for (const Variant& variant : variants)
        {
            ScenarioParams leveled = params;
            leveled["level"] = std::to_string(variant.level);
            if (adjustPerVariant)
                adjustPerVariant(leveled, variant.level);

            std::vector<std::string> expanded;
            expanded.reserve(tmpl.size());
            for (const std::string& line : tmpl)
                expanded.push_back(ExpandScenarioLine(line, leveled));

            TestRegistry::RegisterTest(std::string(prefix) + bossName + variant.suffix, expanded);
        }
    }
}

void TestRegistry::GenerateBossWalkTest()
{
    // maxDistance 0: the generator uses an EMPTY PlayerTravelInfo whose center is the map-0 origin
    // (0,0,0). The default maxDistance of 10000 yd then silently dropped every destination whose
    // portal was farther than that from the origin - Zul'Gurub (map 309) bosses were all >10000 and
    // generated no tests at all (BL-45). The generator teleports bots to the instance, so travel
    // distance is irrelevant here; only the FLT_MAX unpathable-map check should apply.
    DestinationList bossDestinations = sTravelMgr.GetDestinations(PlayerTravelInfo(), (uint32)TravelDestinationPurpose::Boss, {}, false, 0);

    std::vector<std::string> instanceGroupTemplate = {
        "# instance progression with large group and dead-mob observation",
        "require bot is level=$(level)",
        "monitor dead mobs > $(dead_mobs_min) => pass \"Observed dead mobs in instance\"",
        "monitor party xp > $(party_xp_min) => pass \"Party gained xp clearing trash\"",
        "monitor time > $(timeout_s) => fail \"Timeout while traversing instance after <time elapsed> (mobs <mobs killed>, traveled <distance traveled> / wanted <distance wanted>)\"",
        "mgroup size=$(group_size) gear=best",
        "gm visible on",
        "gm off",
        "wait group $(group_size) 180",
        "teleport $(instance_entry)",
        // BL-42 follow-up: a lost mgroup join used to surface as the group-size monitor's
        // misleading "Group dissolved" (party formed one short but otherwise healthy).
        // Abort honestly at delivery time instead: "group never formed (size 4 < expected 5)".
        "teleport group expect=$(group_size)",
        "wait 10",
        "$(start_command)",
        "set destination $(boss_destination)",
        "wait 60",
        "monitor not on map $(instance_entry) => abort \"Bot left instance map\"",
        "monitor group size < $(group_size) => abort \"Group dissolved\"",
        "observe"};

    for (auto& destination : bossDestinations)
    {
        for (auto& point : destination->GetPoints())
        {
            if (!point->getMapEntry())
                continue;

            const MapEntry* mapEntry = point->getMapEntry();

            if (!mapEntry->IsDungeon())
                continue;

            const InstanceTemplate* instanceTemplate = point->getInstanceTemplate();
            if (!instanceTemplate)
                continue;

            CreatureInfo const* bossInfo = static_cast<BossTravelDestination*>(destination)->GetCreatureInfo();
            if (!bossInfo)
                continue;

            GuidPosition entry;
            std::string mapName = mapEntry->name[0];

            if (!ParseLocation(mapName, entry))
            {
                mapName.erase(remove_if(mapName.begin(), mapName.end(), isspace), mapName.end());
            }

            if (!ParseLocation(mapName, entry))
            {
                if (mapName.find(" ") != std::string::npos)
                    mapName = mapName.substr(0, mapName.find(" "));
            }

            if (!ResolveInstanceEntry(mapEntry, mapName, entry))
                continue;

            std::string startCommand = ".bot p @tank co + mark rti";
            std::string maxPlayers = "5";

            if (mapEntry->IsRaid())
            {
#if defined(MANGOSBOT_ZERO) || defined(MANGOSBOT_ONE)
                maxPlayers = std::to_string(instanceTemplate->maxPlayers);
#else
                if (MapDifficultyEntry const* mapDiff = GetMapDifficultyData(mapEntry->MapID, REGULAR_DIFFICULTY))
                    maxPlayers = std::to_string(mapDiff->maxPlayers);
                else
                    maxPlayers = "25";
#endif
                startCommand = ".bot r @tank co + mark rti";
            }

            std::string bossName = mapEntry->name[0] + std::string("_") + bossInfo->Name;
            std::replace(bossName.begin(), bossName.end(), ' ', '_');
            std::replace(bossName.begin(), bossName.end(), '\'', '_');
            std::transform(bossName.begin(), bossName.end(), bossName.begin(), ::tolower);

            RegisterNamedLocation(bossName, GuidPosition(ObjectGuid(), *point));

            RegisterInstanceScenario("scenario_trash_", bossName, instanceGroupTemplate, ScenarioParams{
                {"timeout_s",        "1200"                                         },
                {"start_command",    startCommand                                   },
                {"group_size",       maxPlayers                                     },
                {"dead_mobs_min",    "5"                                            },
                {"party_xp_min",     "5000"                                         },
                {"instance_entry",   mapName                                        },
                {"boss_destination", bossName                                       }
            }, instanceTemplate->levelMin);
        }
    }
}

void TestRegistry::GenerateBossEncounterTest()
{
    // maxDistance 0: the generator uses an EMPTY PlayerTravelInfo whose center is the map-0 origin
    // (0,0,0). The default maxDistance of 10000 yd then silently dropped every destination whose
    // portal was farther than that from the origin - Zul'Gurub (map 309) bosses were all >10000 and
    // generated no tests at all (BL-45). The generator teleports bots to the instance, so travel
    // distance is irrelevant here; only the FLT_MAX unpathable-map check should apply.
    DestinationList bossDestinations = sTravelMgr.GetDestinations(PlayerTravelInfo(), (uint32)TravelDestinationPurpose::Boss, {}, false, 0);

    std::vector<std::string> bossEncounterTemplate = {
        "# Boss encounter test - teleport near boss, clear trash, fight boss",
        "require bot is level=$(level)",
        "monitor mob $(boss_entry) is dead => pass \"Boss $(boss_name) killed\"",
        "monitor time > $(timeout_s) => fail \"Timeout: boss not killed after <time elapsed>\"",
        "monitor party wiped => fail \"Party wiped on $(boss_name)\"",
        "monitor bot dead => abort \"Bot died: released to graveyard and timed out in revive\"",
        "monitor not on map $(instance_entry) => abort \"Bot left instance map\"",
        "monitor group size < $(group_size) => abort \"Group dissolved\"",
        "mgroup size=$(group_size) gear=best",
        "gm on",
        "wait group $(group_size) 180",
        "teleport $(instance_entry)",
        "wait 5",
        "teleport $(boss_destination)",
        "teleport group expect=$(group_size)",
        "wait 5",
        "clear except=$(boss_entry) radius500",
        "pull $(boss_entry)",
        "clear except=$(boss_entry) radius500",
        "$(start_command)",
        "wait 3",
        "gm off",
        "pull $(boss_entry)",
        "observe"};

    for (auto& destination : bossDestinations)
    {
        for (auto& point : destination->GetPoints())
        {
            if (!point->getMapEntry())
                continue;

            const MapEntry* mapEntry = point->getMapEntry();

            if (!mapEntry->IsDungeon())
                continue;

            const InstanceTemplate* instanceTemplate = point->getInstanceTemplate();
            if (!instanceTemplate)
                continue;

            CreatureInfo const* bossInfo = static_cast<BossTravelDestination*>(destination)->GetCreatureInfo();
            if (!bossInfo)
                continue;

            GuidPosition entry;
            std::string mapName = mapEntry->name[0];

            if (!ParseLocation(mapName, entry))
            {
                mapName.erase(remove_if(mapName.begin(), mapName.end(), isspace), mapName.end());
            }

            if (!ParseLocation(mapName, entry))
            {
                if (mapName.find(" ") != std::string::npos)
                    mapName = mapName.substr(0, mapName.find(" "));
            }

            if (!ResolveInstanceEntry(mapEntry, mapName, entry))
                continue;

            std::string startCommand = ".bot p @tank co + mark rti";
            std::string maxPlayers = "5";

            if (mapEntry->IsRaid())
            {
#if defined(MANGOSBOT_ZERO) || defined(MANGOSBOT_ONE)
                maxPlayers = std::to_string(instanceTemplate->maxPlayers);
#else
                if (MapDifficultyEntry const* mapDiff = GetMapDifficultyData(mapEntry->MapID, REGULAR_DIFFICULTY))
                    maxPlayers = std::to_string(mapDiff->maxPlayers);
                else
                    maxPlayers = "25";
#endif
                startCommand = ".bot r @tank co + mark rti";
            }

            std::string bossName = mapEntry->name[0] + std::string("_") + bossInfo->Name;
            std::replace(bossName.begin(), bossName.end(), ' ', '_');
            std::replace(bossName.begin(), bossName.end(), '\'', '_');
            std::transform(bossName.begin(), bossName.end(), bossName.begin(), ::tolower);

            // Encounter tests drop the raid on the boss -> replaced by the approach point
            // (1.5x aggro radius from the boss, on the path toward an instance exit).
            RegisterInstanceScenario("scenario_boss_", bossName, bossEncounterTemplate, ScenarioParams{
                {"timeout_s",        "600"                                          },
                {"start_command",    startCommand                                   },
                {"group_size",       maxPlayers                                     },
                {"instance_entry",   mapName                                        },
                {"boss_entry",       std::to_string(bossInfo->Entry)                },
                {"boss_name",        bossInfo->Name                                 }
            }, instanceTemplate->levelMin, [&](ScenarioParams& leveled, uint32 botLevel)
            {
                leveled["boss_destination"] = ComputeBossApproachCoords(mapEntry, *point, bossInfo->MinLevel, botLevel);
            });
        }
    }
}

// BL-45 part 2: the layer-2 reach test. The trash template observes clearing, the encounter
// template teleports next to the boss and fights - but "enter the dungeon and TRAVEL to the boss
// (alive)" had no coverage. The reach template enters at the instance entry, sets the boss as the
// travel destination and passes when the party is near the boss while it is still alive.
void TestRegistry::GenerateBossReachTest()
{
    DestinationList bossDestinations = sTravelMgr.GetDestinations(PlayerTravelInfo(), (uint32)TravelDestinationPurpose::Boss, {}, false, 0);

    std::vector<std::string> bossReachTemplate = {
        "# reach boss test - enter instance, travel to the boss, party near boss with boss alive",
        "require bot is level=$(level)",
        "monitor distance to $(boss_location) < $(reach_dist) => pass \"Party reached $(boss_name) after <time elapsed>\"",
        "monitor mob $(boss_entry) is dead => fail \"Boss $(boss_name) died before the party reached it\"",
        "monitor time > $(timeout_s) => fail \"Timeout: boss not reached after <time elapsed> (traveled <distance traveled>)\"",
        "monitor party wiped => fail \"Party wiped on the way to $(boss_name)\"",
        "monitor bot dead => abort \"Bot died on the way to $(boss_name)\"",
        "monitor not on map $(instance_entry) => abort \"Bot left instance map\"",
        "monitor group size < 2 => abort \"Group dissolved\"",
        "mgroup size=$(group_size) gear=best",
        "gm visible on",
        "gm off",
        "wait group $(group_size) 180",
        "teleport $(instance_entry)",
        // BL-42 follow-up: see instanceGroupTemplate - abort honestly on a short group.
        "teleport group expect=$(group_size)",
        "wait 10",
        "$(start_command)",
        "set destination $(boss_destination)",
        "observe"};

    for (auto& destination : bossDestinations)
    {
        for (auto& point : destination->GetPoints())
        {
            if (!point->getMapEntry())
                continue;

            const MapEntry* mapEntry = point->getMapEntry();

            if (!mapEntry->IsDungeon())
                continue;

            const InstanceTemplate* instanceTemplate = point->getInstanceTemplate();
            if (!instanceTemplate)
                continue;

            CreatureInfo const* bossInfo = static_cast<BossTravelDestination*>(destination)->GetCreatureInfo();
            if (!bossInfo)
                continue;

            GuidPosition entry;
            std::string mapName = mapEntry->name[0];

            if (!ResolveInstanceEntry(mapEntry, mapName, entry))
                continue;

            std::string startCommand = ".bot p @tank co + mark rti";
            std::string maxPlayers = "5";

            if (mapEntry->IsRaid())
            {
#if defined(MANGOSBOT_ZERO) || defined(MANGOSBOT_ONE)
                maxPlayers = std::to_string(instanceTemplate->maxPlayers);
#else
                if (MapDifficultyEntry const* mapDiff = GetMapDifficultyData(mapEntry->MapID, REGULAR_DIFFICULTY))
                    maxPlayers = std::to_string(mapDiff->maxPlayers);
                else
                    maxPlayers = "25";
#endif
                startCommand = ".bot r @tank co + mark rti";
            }

            std::string bossName = mapEntry->name[0] + std::string("_") + bossInfo->Name;
            std::replace(bossName.begin(), bossName.end(), ' ', '_');
            std::replace(bossName.begin(), bossName.end(), 39, 95); // 39 = apostrophe, 95 = underscore
            std::transform(bossName.begin(), bossName.end(), bossName.begin(), ::tolower);

            RegisterNamedLocation(bossName, GuidPosition(ObjectGuid(), *point));

            RegisterInstanceScenario("scenario_reach_", bossName, bossReachTemplate, ScenarioParams{
                {"timeout_s",        "1800"                                         },
                {"start_command",    startCommand                                   },
                {"group_size",       maxPlayers                                     },
                {"reach_dist",       "60"                                           },
                {"instance_entry",   mapName                                        },
                {"boss_location",    bossName                                       },
                {"boss_destination", bossName                                       },
                {"boss_entry",       std::to_string(bossInfo->Entry)                },
                {"boss_name",        bossInfo->Name                                 }
            }, instanceTemplate->levelMin);
        }
    }
}
void TestRegistry::RegisterInstanceTests()
{
    GenerateBossWalkTest();
    GenerateBossEncounterTest();
    GenerateBossReachTest();

    // =====================================================================================
    // [BOT-ML 2026-10-17 站长口径] 两个"战斗场景"测试：单目标 / 五目标，各跑 6 分钟。
    //   目标物 = 训练假人 190013（CommandSetup 的 pull <entry> 会在找不到时随时 SummonCreature
    //   就地刷一个，见 CommandSetup.cpp 里 "pull: creature not found ... spawning it"），
    //   engage spawn <entry> 让 bot 交战。
    //   细粒度指标（躲地板 bm_dodge / 打断 bm_interrupt / GCD 挂起 bm_gcd_wait，以及
    //   Engine 写入的动作名 = 治疗/输出代理）都落在 bot_events.csv：启动服务时设 BOT_METRICS=1 即可。
    //   开跑方式（三选一，2026-10-17 查证）：
    //     · GM 命令：`.playerbots test combat_single_target`（DebugAction.cpp 的 test 分支）
    //     · 或给 bot 挂策略：`test::combat_single_target`（TestStrategy 用 test::<名字> 做限定词）
    // =====================================================================================
    RegisterTest("combat_single_target", {
        "monitor time > 375 => pass \"single-target scenario ran 6 minutes\"",
        "monitor time > 420 => fail \"Timeout: single-target scenario did not finish\"",
        // 先造一队 bot 并拉到同一处（木桩是友善目标，随机 bot 不会主动打它，必须显式 engage）
        // 每职业一个（专精由 AiPlayerbot.PremadeSpec* 表自动分配）：坦克 / 治疗 / 法系 / 混合
        "spawn level=70 class=warrior temporary=1 login=1",
        "spawn level=70 class=paladin temporary=1 login=1",
        "spawn level=70 class=priest temporary=1 login=1",
        "spawn level=70 class=mage temporary=1 login=1",
        "spawn level=70 class=druid temporary=1 login=1",
        "form",
        "wait 8",
        "pull 190013",
        "wait 2",
        "engage spawn 190013",
        "observe"
    });

    RegisterTest("combat_five_targets", {
        "monitor time > 375 => pass \"five-target scenario ran 6 minutes\"",
        "monitor time > 420 => fail \"Timeout: five-target scenario did not finish\"",
        // 每职业一个（专精由 AiPlayerbot.PremadeSpec* 表自动分配）
        "spawn level=70 class=warrior temporary=1 login=1",
        "spawn level=70 class=paladin temporary=1 login=1",
        "spawn level=70 class=priest temporary=1 login=1",
        "spawn level=70 class=mage temporary=1 login=1",
        "spawn level=70 class=druid temporary=1 login=1",
        "form",
        "wait 8",
        "pull 190013",                                    // 唯一的"有技能"目标（7 个机制法术都在 190013 上）
        "pull 190014", "pull 190014", "pull 190015", "pull 190015",   // 4 个无技能目标
        "wait 2",
        "engage spawn 190013",
        "engage spawn 190014",
        "engage spawn 190015",
        "observe"
    });

    // [bottest 2026-10-19] 最小可测场景：**不需要组队**（不依赖在线机器人数量 / 账号槽位），
    //   只用主机 bot 自己打 190013，用来验证"度量链路"本身是否通：
    //   pull 会在找不到目标时自己召唤，所以只要数据库里有 190013 的 creature_template 就能打。
    //   期望产出：bm_dmg / bm_heal / bm_kills / bm_deaths / bm_rule + role 列。
    RegisterTest("combat_smoke", {
        "pull 190013",
        "wait 2",
        "engage spawn 190013",
        "observe",
        "monitor time > 150 => pass \"smoke: combat metrics produced\"",
        "monitor time > 210 => fail \"Timeout: smoke scenario did not produce combat\""
    });
}
