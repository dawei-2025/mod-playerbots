/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license,
 * you may redistribute it and/or modify it under version 3 of the License, or (at your option),
 * any later version.
 */

#include "BotSmartStrategyMgr.h"

#include "AiObjectContext.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "Event.h"
#include "GameObject.h"
#include "Group.h"
#include "GroupReference.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Random.h"
#include "ServerFacade.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "Timer.h"
#include "Unit.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <cmath>
#include <list>
#include <limits>
#include <sstream>

namespace
{
constexpr float SMART_DEFAULT_SEARCH_RANGE = 120.0f;
constexpr float SMART_BOSS_SEARCH_RANGE = 220.0f;
constexpr float SMART_POSITION_EPSILON = 1.5f;
constexpr float SMART_ACTION15_DISTANCE_TOLERANCE = 2.0f;
constexpr float SMART_ACTION15_DEFAULT_FRONT_DANGER_ARC = 2.0f * static_cast<float>(M_PI) / 3.0f;
constexpr float SMART_ACTION15_MIN_EDGE_HOLD_ANGLE = static_cast<float>(M_PI) / 9.0f;
constexpr uint32 SMART_DEFAULT_ACTION_COOLDOWN = 1000;
constexpr uint32 SMART_MOVE_ACTION_COOLDOWN = 750;
constexpr uint32 SMART_MIN_ACTION_COOLDOWN = 250;
constexpr uint32 SMART_WARNING_DUMMY_ENTRY = 20755;
constexpr uint32 SMART_GATHER_DUMMY_ENTRY = 18896;

float Pi()
{
    static float const value = std::acos(-1.0f);
    return value;
}

uint32 AsUInt(int32 value)
{
    return value > 0 ? static_cast<uint32>(value) : 0;
}

float AsRange(int32 value, float fallback)
{
    return value > 0 ? static_cast<float>(value) : fallback;
}

float NormalizeSignedAngle(float angle)
{
    angle = Position::NormalizeOrientation(angle);
    if (angle > Pi())
        angle -= 2.0f * Pi();

    return angle;
}

float AbsAngleDiff(float left, float right)
{
    return std::fabs(NormalizeSignedAngle(left - right));
}

uint32 GetBotLowGuid(Player* bot)
{
    return bot ? bot->GetGUID().GetCounter() : 0;
}

bool IsBot(Player* player)
{
    if (!player)
        return false;

    PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
    return ai && !ai->IsRealPlayer();
}

bool FieldBool(QueryResult result)
{
    return static_cast<bool>(result);
}
}

bool BotSmartEvent::IsConfigured(bool primary) const
{
    if (primary)
        return true;

    if (type != 0)
        return true;

    for (int32 value : params)
        if (value != 0)
            return true;

    return false;
}

BotSmartStrategyMgr& BotSmartStrategyMgr::Instance()
{
    static BotSmartStrategyMgr instance;
    return instance;
}

bool BotSmartStrategyMgr::Initialize()
{
    bool loaded = Reload();
    if (!loaded)
        LOG_WARN("playerbots", "Bot smart strategy tables were not loaded. The system will stay idle until tables exist.");

    return true;
}

bool BotSmartStrategyMgr::Reload()
{
    return LoadFromDB();
}

void BotSmartStrategyMgr::Clear()
{
    _scriptsByMap.clear();
    _waypointsBySmartId.clear();
    _destinationsBySmartId.clear();
    _runtime.clear();
    _encounters.clear();
    _forbiddenEntriesByBot.clear();
    _scriptCount = 0;
    _waypointCount = 0;
    _destinationCount = 0;
}

std::size_t BotSmartStrategyMgr::GetScriptCount() const
{
    std::lock_guard<std::mutex> guard(_lock);
    return _scriptCount;
}

std::size_t BotSmartStrategyMgr::GetWaypointCount() const
{
    std::lock_guard<std::mutex> guard(_lock);
    return _waypointCount;
}

std::size_t BotSmartStrategyMgr::GetDestinationCount() const
{
    std::lock_guard<std::mutex> guard(_lock);
    return _destinationCount;
}

bool BotSmartStrategyMgr::LoadFromDB()
{
    if (!FieldBool(PlayerbotsDatabase.Query("SHOW TABLES LIKE 'bot_smart_script'")))
    {
        std::lock_guard<std::mutex> guard(_lock);
        Clear();
        LOG_WARN("playerbots", "Table bot_smart_script does not exist in Playerbots database.");
        return false;
    }

    std::unordered_map<uint32, std::vector<BotSmartScript>> scriptsByMap;
    std::unordered_map<uint32, std::map<uint32, std::vector<BotSmartWaypoint>>> waypointsBySmartId;
    std::unordered_map<uint32, std::vector<BotSmartDestination>> destinationsBySmartId;
    std::size_t scriptCount = 0;
    std::size_t waypointCount = 0;
    std::size_t destinationCount = 0;

    QueryResult scriptResult = PlayerbotsDatabase.Query(
        "SELECT "
        "`id`, `mapId`, `groupId`, `creatureId`, `actionType`, "
        "`actionParam1`, `actionParam2`, `actionParam3`, `actionParam4`, `actionParam5`, `actionParam6`, "
        "COALESCE(`actionParam7`, 0), COALESCE(`actionParam8`, 0), "
        "`eventType`, `eventParam1`, `eventParam2`, `eventParam3`, `eventParam4`, `eventParam5`, `eventParam6`, "
        "COALESCE(`event2Type`, 0), COALESCE(`event2Param1`, 0), COALESCE(`event2Param2`, 0), "
        "COALESCE(`event2Param3`, 0), COALESCE(`event2Param4`, 0), "
        "COALESCE(`event3Type`, 0), COALESCE(`event3Param1`, 0), COALESCE(`event3Param2`, 0), "
        "COALESCE(`event3Param3`, 0), COALESCE(`event3Param4`, 0), "
        "COALESCE(`event4Type`, 0), COALESCE(`event4Param1`, 0), COALESCE(`event4Param2`, 0), "
        "COALESCE(`event4Param3`, 0), COALESCE(`event4Param4`, 0), "
        "COALESCE(`remark`, ''), COALESCE(`extraType`, 0), COALESCE(`extraParam1`, 0), "
        "COALESCE(`extraParam2`, 0), COALESCE(`extraParam3`, 0), COALESCE(`extraParam4`, 0), "
        "COALESCE(`extraParam5`, 0), COALESCE(`extraParam6`, 0), COALESCE(`orderNum`, 0), "
        "COALESCE(`targetType`, 0), COALESCE(`targetParam1`, 0), COALESCE(`targetParam2`, 0), "
        "COALESCE(`targetParam3`, 0), COALESCE(`targetParam4`, 0), COALESCE(`bossPhase`, 0), COALESCE(`diff`, 0) "
        "FROM `bot_smart_script` ORDER BY `mapId`, `groupId`, COALESCE(`orderNum`, 0), `id`");

    if (scriptResult)
    {
        do
        {
            Field* fields = scriptResult->Fetch();
            BotSmartScript script;
            script.id = fields[0].Get<uint32>();
            script.mapId = fields[1].Get<uint32>();
            script.groupId = fields[2].Get<uint32>();
            script.creatureId = fields[3].Get<uint32>();
            script.actionType = fields[4].Get<int32>();

            for (uint8 i = 0; i < 8; ++i)
                script.actionParams[i] = fields[5 + i].Get<int32>();

            script.events[0].type = fields[13].Get<int32>();
            for (uint8 i = 0; i < 6; ++i)
                script.events[0].params[i] = fields[14 + i].Get<int32>();

            script.events[1].type = fields[20].Get<int32>();
            for (uint8 i = 0; i < 4; ++i)
                script.events[1].params[i] = fields[21 + i].Get<int32>();

            script.events[2].type = fields[25].Get<int32>();
            for (uint8 i = 0; i < 4; ++i)
                script.events[2].params[i] = fields[26 + i].Get<int32>();

            script.events[3].type = fields[30].Get<int32>();
            for (uint8 i = 0; i < 4; ++i)
                script.events[3].params[i] = fields[31 + i].Get<int32>();

            script.remark = fields[35].Get<std::string>();
            script.extraType = fields[36].Get<int32>();
            for (uint8 i = 0; i < 6; ++i)
                script.extraParams[i] = fields[37 + i].Get<int32>();

            script.orderNum = fields[43].Get<int32>();
            script.targetType = fields[44].Get<int32>();
            for (uint8 i = 0; i < 4; ++i)
                script.targetParams[i] = fields[45 + i].Get<int32>();

            script.bossPhase = fields[49].Get<int32>();
            script.diff = fields[50].Get<int32>();

            scriptsByMap[script.mapId].push_back(script);
            ++scriptCount;
        } while (scriptResult->NextRow());
    }

    if (FieldBool(PlayerbotsDatabase.Query("SHOW TABLES LIKE 'bot_smart_waypoint'")))
    {
        if (QueryResult waypointResult = PlayerbotsDatabase.Query(
                "SELECT `id`, COALESCE(`pos_x`, 0), COALESCE(`pos_y`, 0), COALESCE(`pos_z`, 0), "
                "COALESCE(`smartId`, 0), COALESCE(`type`, 0), COALESCE(`orderNum`, 0), "
                "COALESCE(`groupId`, 0), COALESCE(`param1`, 0), COALESCE(`param2`, 0), COALESCE(`param3`, 0) "
                "FROM `bot_smart_waypoint` ORDER BY `smartId`, `groupId`, `orderNum`, `id`"))
        {
            do
            {
                Field* fields = waypointResult->Fetch();
                BotSmartWaypoint waypoint;
                waypoint.id = fields[0].Get<uint32>();
                waypoint.x = static_cast<float>(fields[1].Get<int32>());
                waypoint.y = static_cast<float>(fields[2].Get<int32>());
                waypoint.z = static_cast<float>(fields[3].Get<int32>());
                waypoint.smartId = fields[4].Get<uint32>();
                waypoint.type = fields[5].Get<int32>();
                waypoint.orderNum = fields[6].Get<int32>();
                waypoint.groupId = fields[7].Get<int32>();
                waypoint.params[0] = fields[8].Get<int32>();
                waypoint.params[1] = fields[9].Get<int32>();
                waypoint.params[2] = fields[10].Get<int32>();

                waypointsBySmartId[waypoint.smartId][waypoint.groupId].push_back(waypoint);
                ++waypointCount;
            } while (waypointResult->NextRow());
        }
    }

    if (FieldBool(PlayerbotsDatabase.Query("SHOW TABLES LIKE 'bot_smart_destination'")))
    {
        if (QueryResult destinationResult = PlayerbotsDatabase.Query(
                "SELECT `id`, COALESCE(`pos_x`, 0), COALESCE(`pos_y`, 0), COALESCE(`pos_z`, 0), "
                "COALESCE(`smartId`, 0), COALESCE(`role`, 0), COALESCE(`allotIndex`, 0), "
                "COALESCE(`stayTime`, 0), COALESCE(`type`, 0), COALESCE(`param1`, 0), "
                "COALESCE(`param2`, 0), COALESCE(`param3`, 0) "
                "FROM `bot_smart_destination` ORDER BY `smartId`, `role`, `allotIndex`, `id`"))
        {
            do
            {
                Field* fields = destinationResult->Fetch();
                BotSmartDestination destination;
                destination.id = fields[0].Get<uint32>();
                destination.x = static_cast<float>(fields[1].Get<int32>());
                destination.y = static_cast<float>(fields[2].Get<int32>());
                destination.z = static_cast<float>(fields[3].Get<int32>());
                destination.smartId = fields[4].Get<uint32>();
                destination.role = fields[5].Get<int32>();
                destination.allotIndex = fields[6].Get<int32>();
                destination.stayTime = fields[7].Get<int32>();
                destination.type = fields[8].Get<int32>();
                destination.params[0] = fields[9].Get<int32>();
                destination.params[1] = fields[10].Get<int32>();
                destination.params[2] = fields[11].Get<int32>();

                destinationsBySmartId[destination.smartId].push_back(destination);
                ++destinationCount;
            } while (destinationResult->NextRow());
        }
    }

    for (auto& mapPair : scriptsByMap)
    {
        std::sort(mapPair.second.begin(), mapPair.second.end(),
                  [](BotSmartScript const& left, BotSmartScript const& right)
                  {
                      if (left.groupId != right.groupId)
                          return left.groupId < right.groupId;

                      if (left.orderNum != right.orderNum)
                          return left.orderNum < right.orderNum;

                      return left.id < right.id;
                  });
    }

    for (auto& smartPair : waypointsBySmartId)
    {
        for (auto& groupPair : smartPair.second)
        {
            std::sort(groupPair.second.begin(), groupPair.second.end(),
                      [](BotSmartWaypoint const& left, BotSmartWaypoint const& right)
                      {
                          if (left.orderNum != right.orderNum)
                              return left.orderNum < right.orderNum;

                          return left.id < right.id;
                      });
        }
    }

    {
        std::lock_guard<std::mutex> guard(_lock);
        Clear();
        _scriptsByMap = std::move(scriptsByMap);
        _waypointsBySmartId = std::move(waypointsBySmartId);
        _destinationsBySmartId = std::move(destinationsBySmartId);
        _scriptCount = scriptCount;
        _waypointCount = waypointCount;
        _destinationCount = destinationCount;
    }

    LOG_INFO("playerbots", "Loaded bot smart strategy: {} scripts, {} waypoints, {} destinations", scriptCount,
             waypointCount, destinationCount);
    return true;
}

void BotSmartStrategyMgr::Update(PlayerbotAI* botAI, uint32 /*diff*/)
{
    if (!botAI)
        return;

    Player* bot = botAI->GetBot();
    if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported() || !bot->IsAlive() || !bot->GetMap())
        return;

    std::lock_guard<std::mutex> guard(_lock);
    auto itr = _scriptsByMap.find(bot->GetMapId());
    if (itr == _scriptsByMap.end() || itr->second.empty())
        return;

    uint32 now = getMSTime();
    for (BotSmartScript const& script : itr->second)
        ProcessScript(botAI, script, now);
}

bool BotSmartStrategyMgr::ProcessScript(PlayerbotAI* botAI, BotSmartScript const& script, uint32 now)
{
    Player* bot = botAI->GetBot();
    if (!MatchesBasicFilters(bot, script))
        return false;

    Unit* boss = ResolveBoss(botAI, script);
    if (script.creatureId && !boss)
        return false;

    UpdateEncounterState(bot, script, boss, now);

    if (script.bossPhase > 0 && static_cast<uint32>(script.bossPhase) != GetPhase(bot, script, boss))
        return false;

    BotSmartRuntimeKey runtimeKey{GetBotLowGuid(bot), script.id};
    BotSmartRuntimeState& runtime = _runtime[runtimeKey];

    if (!MatchesEvents(botAI, script, boss, now))
    {
        runtime.eventMatchedAt = 0;
        runtime.executeAt = 0;
        runtime.activeUntil = 0;
        runtime.extraOneShotConsumed = false;
        runtime.waypointInitialized = false;
        return false;
    }

    if (!ApplyExtra(script, runtime, now))
        return false;

    if (!MatchesTargetFilter(botAI, script))
        return false;

    if (!MatchesActionFilter(botAI, script))
        return false;

    if (runtime.nextActionAt && now < runtime.nextActionAt)
        return false;

    bool executed = ExecuteAction(botAI, script, boss, runtime, now);
    if (executed)
    {
        runtime.nextActionAt = now + GetActionCooldown(script);
        if (script.extraType == 1 && script.extraParams[1] <= 0)
            runtime.extraOneShotConsumed = true;
    }

    return executed;
}

bool BotSmartStrategyMgr::MatchesBasicFilters(Player* bot, BotSmartScript const& script) const
{
    if (!bot || !bot->GetMap())
        return false;

    if (script.mapId != bot->GetMapId())
        return false;

    if (script.diff > 0 && static_cast<uint32>(script.diff) != static_cast<uint32>(bot->GetMap()->GetDifficulty()))
        return false;

    return true;
}

bool BotSmartStrategyMgr::MatchesEvents(PlayerbotAI* botAI, BotSmartScript const& script, Unit* boss, uint32 now)
{
    for (uint8 i = 0; i < script.events.size(); ++i)
    {
        bool primary = i == 0;
        BotSmartEvent const& event = script.events[i];
        if (!event.IsConfigured(primary))
            continue;

        if (!MatchesEvent(botAI, script, event, boss, now))
            return false;
    }

    return true;
}

bool BotSmartStrategyMgr::MatchesEvent(PlayerbotAI* botAI, BotSmartScript const& script, BotSmartEvent const& event,
                                       Unit* boss, uint32 now)
{
    Player* bot = botAI->GetBot();

    switch (event.type)
    {
        case 0:
            return bot->IsInCombat() || (boss && boss->IsInCombat());
        case 1:
        {
            if (!boss || !boss->IsAlive())
                return false;

            float hp = boss->GetHealthPct();
            float maxPct = event.params[0] > 0 ? static_cast<float>(event.params[0]) : 100.0f;
            float minPct = static_cast<float>(std::max<int32>(0, event.params[1]));
            return hp <= maxPct && hp >= minPct;
        }
        case 2:
        {
            uint32 elapsed = GetEncounterElapsed(bot, script, boss, now);
            if (!elapsed)
                return false;

            uint32 maxTime = AsUInt(event.params[0]);
            uint32 minTime = AsUInt(event.params[1]);
            if (elapsed < minTime)
                return false;

            return maxTime == 0 || elapsed <= maxTime;
        }
        case 3:
        {
            uint32 spellId = AsUInt(event.params[0]);
            if (!boss || !spellId)
                return false;

            if (GetCurrentCastSpell(boss) != spellId)
                return false;

            uint32 wantedCast = AsUInt(event.params[1]);
            return wantedCast == 0 || GetCastCount(bot, script, boss, spellId) == wantedCast;
        }
        case 4:
            return AuraMatches(boss, AsUInt(event.params[0]), event.params[1], event.params[2], event.params[3]);
        case 5:
            return AuraMatches(bot, AsUInt(event.params[0]), event.params[1], event.params[2], event.params[3]);
        case 6:
            return PowerInRange(bot, event.params[0], event.params[1]);
        case 7:
            return PowerInRange(boss, event.params[0], event.params[1]);
        case 8:
            return AuraStackLess(boss, AsUInt(event.params[0]), event.params[1]);
        case 9:
        {
            uint32 entry = script.creatureId;
            for (uint8 i = 0; i < 4; ++i)
                if (event.params[i] > 0)
                    entry = static_cast<uint32>(event.params[i]);

            if (!entry)
                return boss && !boss->IsAlive();

            Creature* creature = FindNearestCreature(bot, entry, SMART_BOSS_SEARCH_RANGE, false);
            return creature && !creature->IsAlive();
        }
        case 10:
            return PositionInRange(boss, static_cast<float>(event.params[0]), static_cast<float>(event.params[1]),
                                   static_cast<float>(event.params[2]), AsRange(event.params[3], 5.0f));
        case 11:
        {
            for (uint8 i = 0; i < 4; ++i)
            {
                uint32 entry = AsUInt(event.params[i]);
                if (entry && FindNearestCreature(bot, entry, SMART_DEFAULT_SEARCH_RANGE, true))
                    return true;
            }
            return false;
        }
        case 12:
        {
            bool hasAnyEntry = false;
            for (uint8 i = 0; i < 4; ++i)
            {
                uint32 entry = AsUInt(event.params[i]);
                if (!entry)
                    continue;

                hasAnyEntry = true;
                if (FindNearestCreature(bot, entry, SMART_DEFAULT_SEARCH_RANGE, true))
                    return false;
            }
            return hasAnyEntry;
        }
        default:
            LOG_DEBUG("playerbots", "Unsupported bot_smart_script eventType {} on script {}", event.type, script.id);
            return false;
    }
}

bool BotSmartStrategyMgr::ApplyExtra(BotSmartScript const& script, BotSmartRuntimeState& state, uint32 now)
{
    if (script.extraType != 1)
        return true;

    uint32 delayMs = AsUInt(script.extraParams[0]);
    uint32 durationMs = AsUInt(script.extraParams[1]);

    if (!state.eventMatchedAt)
    {
        state.eventMatchedAt = now;
        state.executeAt = now + delayMs;
        state.activeUntil = durationMs > 0 ? state.executeAt + durationMs : 0;
        state.extraOneShotConsumed = false;
    }

    if (now < state.executeAt)
        return false;

    if (durationMs > 0 && now > state.activeUntil)
        return false;

    if (durationMs == 0 && state.extraOneShotConsumed)
        return false;

    return true;
}

uint32 BotSmartStrategyMgr::GetActionCooldown(BotSmartScript const& script) const
{
    switch (script.actionType)
    {
        case 4:
        case 14:
        case 15:
        case 16:
        case 18:
        case 19:
        case 20:
        case 21:
        case 22:
        case 23:
        case 24:
        case 25:
        case 29:
        case 32:
            return SMART_MOVE_ACTION_COOLDOWN;
        case 10:
            return std::max<uint32>(SMART_MIN_ACTION_COOLDOWN, AsUInt(script.actionParams[0]));
        case 12:
            return std::max<uint32>(SMART_MIN_ACTION_COOLDOWN, AsUInt(script.actionParams[0]));
        case 28:
            return std::max<uint32>(SMART_MIN_ACTION_COOLDOWN, AsUInt(script.actionParams[1]));
        default:
            return SMART_DEFAULT_ACTION_COOLDOWN;
    }
}

bool BotSmartStrategyMgr::MatchesTargetFilter(PlayerbotAI* botAI, BotSmartScript const& script) const
{
    Player* bot = botAI->GetBot();

    switch (script.targetType)
    {
        case 0:
            return true;
        case 1:
            return MatchesRole(bot, script.targetParams[0]) && MatchesClass(bot, script.targetParams[1]);
        case 2:
            return AuraMatches(bot, AsUInt(script.targetParams[0]), script.targetParams[1], script.targetParams[2],
                               script.targetParams[3]);
        case 3:
            return PowerInRange(bot, script.targetParams[0], script.targetParams[1]);
        case 4:
            return AuraStackLess(bot, AsUInt(script.targetParams[0]), script.targetParams[1]);
        case 5:
            return PositionInRange(bot, static_cast<float>(script.targetParams[0]), static_cast<float>(script.targetParams[1]),
                                   static_cast<float>(script.targetParams[2]), AsRange(script.targetParams[3], 5.0f));
        default:
            LOG_DEBUG("playerbots", "Unsupported bot_smart_script targetType {} on script {}", script.targetType,
                      script.id);
            return false;
    }
}

bool BotSmartStrategyMgr::MatchesActionFilter(PlayerbotAI* botAI, BotSmartScript const& script) const
{
    int32 const p1 = script.actionParams[0];
    int32 const p2 = script.actionParams[1];
    int32 const p3 = script.actionParams[2];
    int32 const p4 = script.actionParams[3];
    int32 const p5 = script.actionParams[4];
    int32 const p6 = script.actionParams[5];

    switch (script.actionType)
    {
        case 1:
        case 2:
            return MatchesActionRole(botAI, script, p3);
        case 3:
            return MatchesActionRole(botAI, script, p3) && IsAssigned(botAI, script, p3, p2);
        case 4:
        case 19:
            return MatchesActionRole(botAI, script, p1) && MatchesClass(botAI->GetBot(), p6);
        case 5:
            return MatchesActionRole(botAI, script, p3);
        case 10:
            return MatchesActionRole(botAI, script, p2);
        case 14:
            return MatchesActionRole(botAI, script, p5);
        case 16:
            return MatchesActionRole(botAI, script, p1);
        case 21:
            return MatchesActionRole(botAI, script, p1) && MatchesClass(botAI->GetBot(), p3);
        case 24:
        case 25:
        case 28:
            return MatchesActionRole(botAI, script, p1);
        case 26:
            return MatchesActionRole(botAI, script, p1) && IsAssigned(botAI, script, p1, p2);
        case 27:
            return MatchesClass(botAI->GetBot(), p1) && MatchesActionRole(botAI, script, p4);
        case 29:
            return MatchesActionRole(botAI, script, p1) && MatchesClass(botAI->GetBot(), p6);
        default:
            return true;
    }
}

bool BotSmartStrategyMgr::MatchesRole(Player* player, int32 role) const
{
    if (!player)
        return false;

    switch (role)
    {
        case 0:
            return true;
        case 1:
            return PlayerbotAI::IsRanged(player) && !PlayerbotAI::IsHeal(player);
        case 2:
            return PlayerbotAI::IsMelee(player) && !PlayerbotAI::IsHeal(player);
        case 3:
            return PlayerbotAI::IsDps(player) && !PlayerbotAI::IsCaster(player);
        case 4:
            return PlayerbotAI::IsDps(player) && PlayerbotAI::IsCaster(player);
        case 5:
            return PlayerbotAI::IsHeal(player);
        case 6:
            return PlayerbotAI::IsAssistTankOfIndex(player, 0, true);
        case 7:
            return PlayerbotAI::IsAssistTankOfIndex(player, 1, true);
        case 8:
            return PlayerbotAI::IsAssistTankOfIndex(player, 2, true);
        case 9:
            return PlayerbotAI::IsMainTank(player);
        default:
            return true;
    }
}

bool BotSmartStrategyMgr::MatchesClass(Player* player, int32 classId) const
{
    return !classId || (player && player->getClass() == static_cast<uint8>(classId));
}

bool BotSmartStrategyMgr::MatchesActionRole(PlayerbotAI* botAI, BotSmartScript const& script, int32 role) const
{
    Player* bot = botAI->GetBot();

    switch (script.actionType)
    {
        case 3:
            switch (role)
            {
                case 0:
                    return true;
                case 1:
                    return !PlayerbotAI::IsMainTank(bot);
                case 2:
                    return PlayerbotAI::IsRangedDps(bot);
                case 3:
                    return PlayerbotAI::IsMelee(bot) && PlayerbotAI::IsDps(bot);
                case 4:
                    return PlayerbotAI::IsDps(bot) && !PlayerbotAI::IsCaster(bot);
                case 5:
                    return PlayerbotAI::IsDps(bot) && PlayerbotAI::IsCaster(bot);
                case 6:
                    return PlayerbotAI::IsMainTank(bot);
                case 7:
                    return !PlayerbotAI::IsAssistTank(bot);
                default:
                    return true;
            }
        case 5:
            if (role == 3)
                return PlayerbotAI::IsTank(bot);
            break;
        case 10:
            switch (role)
            {
                case 0:
                    return true;
                case 1:
                    return PlayerbotAI::IsRanged(bot);
                case 2:
                    return PlayerbotAI::IsMelee(bot) && PlayerbotAI::IsDps(bot);
                case 3:
                    return PlayerbotAI::IsDps(bot) && !PlayerbotAI::IsCaster(bot);
                case 4:
                    return PlayerbotAI::IsDps(bot) && PlayerbotAI::IsCaster(bot);
                default:
                    return true;
            }
        case 14:
            if (role == 2)
                return PlayerbotAI::IsMelee(bot) && !PlayerbotAI::IsTank(bot);
            if (role == 3)
                return PlayerbotAI::IsTank(bot);
            break;
        case 16:
            switch (role)
            {
                case 0:
                    return true;
                case 1:
                    return !PlayerbotAI::IsMainTank(bot) &&
                           ((PlayerbotAI::IsMelee(bot) && PlayerbotAI::IsDps(bot)) || PlayerbotAI::IsAssistTank(bot));
                case 2:
                    return PlayerbotAI::IsMelee(bot) && PlayerbotAI::IsDps(bot) && !PlayerbotAI::IsAssistTank(bot);
                case 3:
                    return PlayerbotAI::IsRanged(bot);
                default:
                    return true;
            }
        default:
            break;
    }

    return MatchesRole(bot, role);
}

bool BotSmartStrategyMgr::IsAssigned(PlayerbotAI* botAI, BotSmartScript const& script, int32 role, int32 count,
                                     int32 classId) const
{
    if (count <= 0)
        return true;

    Player* bot = botAI->GetBot();
    std::vector<Player*> members = GetGroupMembers(bot, true, true);
    int32 assigned = 0;
    for (Player* member : members)
    {
        PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member);
        if (!memberAI)
            continue;

        if (!MatchesActionRole(memberAI, script, role) || !MatchesClass(member, classId))
            continue;

        if (++assigned > count)
            return false;

        if (member == bot)
            return true;
    }

    return false;
}

bool BotSmartStrategyMgr::ExecuteAction(PlayerbotAI* botAI, BotSmartScript const& script, Unit* boss,
                                        BotSmartRuntimeState& state, uint32 now)
{
    Player* bot = botAI->GetBot();
    auto const& p = script.actionParams;

    switch (script.actionType)
    {
        case 1:
        {
            WorldObject* source = p[0] > 0 ? FindNearestCreatureOrGameObject(bot, AsUInt(p[0]), AsRange(p[1], 20.0f) + 10.0f) : nullptr;
            if (!source)
                return botAI->DoSpecificAction("avoid aoe", Event(), true);

            return ExecuteMoveAwayFrom(botAI, source, AsRange(p[1], 10.0f));
        }
        case 2:
            if (!bot->HasAura(AsUInt(p[0])))
                return false;
            return ExecuteMoveFromGroup(botAI, AsRange(p[1], 20.0f));
        case 3:
            return ExecuteAttack(botAI, FindPriorityAttackTarget(bot, script));
        case 4:
        case 19:
            return ExecuteMoveTo(botAI, static_cast<float>(p[1]), static_cast<float>(p[2]), static_cast<float>(p[3]),
                                 AsUInt(p[4]));
        case 5:
        {
            WorldObject* source = FindNearestCreatureOrGameObject(bot, AsUInt(p[0]), AsRange(p[1], 20.0f) + 10.0f);
            return source && ExecuteMoveAwayFrom(botAI, source, AsRange(p[1], 20.0f));
        }
        case 6:
            return ExecuteDispel(botAI);
        case 7:
        {
            if (!PlayerbotAI::IsAssistTank(bot))
                return false;

            Player* mainTank = FindRolePlayer(botAI, 1);
            if (!mainTank || !AuraMatches(mainTank, AsUInt(p[0]), p[1], 0, 0))
                return false;

            return ExecuteAttack(botAI, boss) || ExecuteTaunt(botAI, boss);
        }
        case 8:
        {
            Unit* target = FindNearestCreature(bot, AsUInt(p[0]), SMART_BOSS_SEARCH_RANGE, true);
            if (!target)
                return false;

            bool tankMatches = false;
            switch (p[4])
            {
                case 0:
                case 1:
                    tankMatches = PlayerbotAI::IsMainTank(bot);
                    break;
                case 2:
                    tankMatches = PlayerbotAI::IsAssistTankOfIndex(bot, 0, true);
                    break;
                case 3:
                    tankMatches = PlayerbotAI::IsAssistTankOfIndex(bot, 1, true);
                    break;
                case 4:
                    tankMatches = PlayerbotAI::IsAssistTankOfIndex(bot, 2, true);
                    break;
                case 5:
                    tankMatches = PlayerbotAI::IsTank(bot);
                    break;
                default:
                    tankMatches = PlayerbotAI::IsTank(bot);
                    break;
            }

            if (!tankMatches)
                return false;

            ExecuteAttack(botAI, target);
            if (target->GetVictim() == bot)
                return ExecuteMoveTo(botAI, static_cast<float>(p[1]), static_cast<float>(p[2]), static_cast<float>(p[3]));

            return ExecuteTaunt(botAI, target);
        }
        case 9:
        {
            Unit* target = FindNearestCreature(bot, AsUInt(p[0]), SMART_BOSS_SEARCH_RANGE, true);
            if (!target)
                return false;

            bool tankMatches = (p[1] <= 1 && PlayerbotAI::IsMainTank(bot)) ||
                               (p[1] == 2 && PlayerbotAI::IsAssistTankOfIndex(bot, 0, true)) ||
                               (p[1] == 3 && PlayerbotAI::IsAssistTankOfIndex(bot, 1, true)) ||
                               (p[1] == 4 && PlayerbotAI::IsAssistTankOfIndex(bot, 2, true));
            return tankMatches && ExecuteAttack(botAI, target);
        }
        case 10:
            return ExecuteStopAttack(botAI, AsUInt(p[0]));
        case 11:
        {
            if (!PlayerbotAI::IsHeal(bot))
                return false;

            Player* healTarget = FindRolePlayer(botAI, p[0]);
            if (!healTarget)
                return false;

            return IsAssigned(botAI, script, 5, p[1]) && ExecuteFocusHeal(botAI, healTarget);
        }
        case 12:
        {
            uint32 duration = AsUInt(p[0]);
            botAI->ChangeStrategy("-aoe", BOT_STATE_COMBAT);
            if (duration)
                botAI->AddTimedEvent([botGuid = bot->GetGUID()]()
                {
                    if (Player* delayedBot = ObjectAccessor::FindPlayer(botGuid))
                        if (PlayerbotAI* delayedAI = GET_PLAYERBOT_AI(delayedBot))
                            delayedAI->ChangeStrategy("+aoe", BOT_STATE_COMBAT);
                }, duration);
            return true;
        }
        case 13:
            if (!PlayerbotAI::IsTank(bot) && boss && boss->GetVictim() == bot)
                return ExecuteStopAttack(botAI, 1000);
            return false;
        case 14:
        {
            if (!bot->HasAura(AsUInt(p[0])))
                return false;

            if (BotSmartWaypoint const* waypoint = SelectWaypoint(botAI, script, state))
            {
                if (bot->GetExactDist(waypoint->x, waypoint->y, waypoint->z) <= SMART_POSITION_EPSILON)
                    ++state.waypointIndex;

                return ExecuteMoveTo(botAI, waypoint->x, waypoint->y, waypoint->z, AsUInt(p[7]));
            }

            switch (p[5])
            {
                case 0:
                    return ExecuteMoveTo(botAI, static_cast<float>(p[1]), static_cast<float>(p[2]), static_cast<float>(p[3]),
                                         AsUInt(p[7]));
                case 1:
                    if (Player* master = FindMasterOrLeader(botAI))
                        return ExecuteMoveTo(botAI, master->GetPositionX(), master->GetPositionY(), master->GetPositionZ(),
                                             AsUInt(p[7]));
                    return false;
                case 2:
                case 3:
                case 4:
                case 5:
                    if (Player* target = FindRolePlayer(botAI, p[5] == 2 ? 1 : p[5] - 1))
                        return ExecuteMoveTo(botAI, target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(),
                                             AsUInt(p[7]));
                    return false;
                default:
                {
                    WorldObject* target = FindNearestCreatureOrGameObject(bot, AsUInt(p[5]), SMART_DEFAULT_SEARCH_RANGE);
                    return target && ExecuteMoveTo(botAI, target->GetPositionX(), target->GetPositionY(),
                                                   target->GetPositionZ(), AsUInt(p[7]));
                }
            }
        }
        case 15:
        {
            if (!boss || !PlayerbotAI::IsRanged(bot))
                return false;

            std::vector<Player*> members = GetGroupMembers(bot, true, true);
            std::vector<Player*> ranged;
            for (Player* member : members)
                if (PlayerbotAI::IsRanged(member))
                    ranged.push_back(member);

            if (ranged.empty())
                return false;

            std::sort(ranged.begin(), ranged.end(),
                      [](Player* left, Player* right) { return left->GetGUID() < right->GetGUID(); });
            auto itr = std::find(ranged.begin(), ranged.end(), bot);
            if (itr == ranged.end())
                return false;

            float sector = std::max<int32>(1, std::min<int32>(4, p[0])) * (Pi() / 2.0f);
            float minDistance = AsRange(p[1], 15.0f);
            float maxDistance = AsRange(p[2], std::max(25.0f, minDistance + 10.0f));
            float distance = (minDistance + maxDistance) * 0.5f;
            float index = static_cast<float>(std::distance(ranged.begin(), itr));
            float centerAngle = boss->GetOrientation() + Pi();
            if (Player* mainTank = FindRolePlayer(botAI, 1))
                centerAngle = std::atan2(mainTank->GetPositionY() - boss->GetPositionY(),
                                         mainTank->GetPositionX() - boss->GetPositionX()) + Pi();
            else if (Unit* victim = boss->GetVictim())
                centerAngle = std::atan2(victim->GetPositionY() - boss->GetPositionY(),
                                         victim->GetPositionX() - boss->GetPositionX()) + Pi();

            float angle = centerAngle - sector * 0.5f + sector * ((index + 0.5f) / ranged.size());
            float currentDistance = bot->GetExactDist2d(boss);
            float distanceTolerance = std::max(SMART_POSITION_EPSILON, SMART_ACTION15_DISTANCE_TOLERANCE);
            bool withinDistanceBand = currentDistance >= std::max(0.0f, minDistance - distanceTolerance) &&
                                      currentDistance <= (maxDistance + distanceTolerance);

            float frontDangerArc = SMART_ACTION15_DEFAULT_FRONT_DANGER_ARC;
            if (p[3] > 0)
                frontDangerArc = std::clamp(static_cast<float>(p[3]), 30.0f, 180.0f) * (Pi() / 180.0f);

            bool inFrontDanger = boss->HasInArc(frontDangerArc, bot);
            float currentAngle = boss->GetAngle(bot);
            float sliceWidth = sector / static_cast<float>(ranged.size());
            float slotTolerance = std::min(sector * 0.5f, (sliceWidth * 0.5f) + (Pi() / 12.0f));
            bool nearAssignedSlot = AbsAngleDiff(currentAngle, angle) <= slotTolerance;

            float currentFromCenter = NormalizeSignedAngle(currentAngle - centerAngle);
            float desiredFromCenter = NormalizeSignedAngle(angle - centerAngle);
            bool edgeBot = itr == ranged.begin() || std::next(itr) == ranged.end();
            float edgeHoldThreshold = std::max(SMART_ACTION15_MIN_EDGE_HOLD_ANGLE, sliceWidth * 0.35f);
            bool edgeStillOnAssignedSide =
                (desiredFromCenter < 0.0f && currentFromCenter <= -edgeHoldThreshold) ||
                (desiredFromCenter > 0.0f && currentFromCenter >= edgeHoldThreshold);

            // Hold a stable ranged flank when the boss makes small position/orientation changes.
            // After the spread is formed, edge bots only need to move again once they drift into the
            // boss frontal danger arc or leave the allowed distance band.
            if (withinDistanceBand && !inFrontDanger && (nearAssignedSlot || (edgeBot && edgeStillOnAssignedSide)))
                return false;

            return ExecuteMoveTo(botAI, boss->GetPositionX() + std::cos(angle) * distance,
                                 boss->GetPositionY() + std::sin(angle) * distance, boss->GetPositionZ());
        }
        case 16:
        {
            if (!boss)
                return false;

            float distance = std::max(bot->GetMeleeRange(boss), 5.0f);
            float angle = boss->GetOrientation() + Pi();
            return ExecuteMoveTo(botAI, boss->GetPositionX() + std::cos(angle) * distance,
                                 boss->GetPositionY() + std::sin(angle) * distance, boss->GetPositionZ());
        }
        case 18:
            return ExecuteMoveFromGroup(botAI, 10.0f);
        case 20:
        {
            if (!boss || !PlayerbotAI::IsTank(bot))
                return false;

            std::vector<Player*> members = GetGroupMembers(bot, true, false);
            if (members.empty())
                return false;

            float avgX = 0.0f;
            float avgY = 0.0f;
            uint32 count = 0;
            for (Player* member : members)
            {
                if (member == bot)
                    continue;

                avgX += member->GetPositionX();
                avgY += member->GetPositionY();
                ++count;
            }

            if (!count)
                return false;

            avgX /= count;
            avgY /= count;
            float angleFromBossToGroup = std::atan2(avgY - boss->GetPositionY(), avgX - boss->GetPositionX());
            float tankAngle = angleFromBossToGroup + Pi();
            float distance = std::max(bot->GetMeleeRange(boss), 5.0f);
            return ExecuteMoveTo(botAI, boss->GetPositionX() + std::cos(tankAngle) * distance,
                                 boss->GetPositionY() + std::sin(tankAngle) * distance, boss->GetPositionZ());
        }
        case 21:
            if (Player* master = FindMasterOrLeader(botAI))
                return ExecuteMoveTo(botAI, master->GetPositionX(), master->GetPositionY(), master->GetPositionZ(),
                                     AsUInt(p[1]));
            return false;
        case 22:
        {
            WorldObject* source = nullptr;
            if (p[0] > 0)
                source = FindNearestCreatureOrGameObject(bot, SMART_WARNING_DUMMY_ENTRY, AsRange(p[1], 15.0f) + 15.0f);
            if (!source)
                source = FindNearestCreatureOrGameObject(bot, SMART_WARNING_DUMMY_ENTRY, AsRange(p[1], 15.0f) + 15.0f);

            return source && ExecuteMoveAwayFrom(botAI, source, AsRange(p[1], 15.0f));
        }
        case 23:
        {
            WorldObject* source = FindNearestCreatureOrGameObject(bot, SMART_GATHER_DUMMY_ENTRY, AsRange(p[1], 15.0f) + 30.0f);
            return source && ExecuteMoveTo(botAI, source->GetPositionX(), source->GetPositionY(), source->GetPositionZ());
        }
        case 24:
        {
            Unit* target = FindTargetByCreatureOrDebuff(botAI, script, p[6] != 0);
            return target && ExecuteMoveTo(botAI, target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(),
                                           AsUInt(p[7]));
        }
        case 25:
        {
            Unit* target = FindTargetByCreatureOrDebuff(botAI, script, false);
            return target && ExecuteMoveAwayFrom(botAI, target, AsRange(p[4], 15.0f));
        }
        case 26:
        {
            GameObject* go = FindNearestGameObject(bot, AsUInt(p[2]), AsRange(p[3], 40.0f));
            if (!go)
                return false;

            if (!go->IsWithinDistInMap(bot, INTERACTION_DISTANCE))
                return ExecuteMoveTo(botAI, go->GetPositionX(), go->GetPositionY(), go->GetPositionZ());

            return ExecuteUseGameObject(botAI, go);
        }
        case 27:
        {
            Unit* target = nullptr;
            if (p[2] == 0)
                target = bot;
            else
                target = boss ? boss : bot->GetVictim();

            if (!target)
                return false;

            if (p[4] && boss && !boss->HasInArc(Pi(), bot))
                return false;

            return botAI->CastSpell(AsUInt(p[1]), target);
        }
        case 28:
            bot->StopMoving();
            botAI->SetNextCheckDelay(AsUInt(p[1]));
            return true;
        case 29:
        {
            float x = static_cast<float>(p[1]);
            float y = static_cast<float>(p[2]);
            float z = static_cast<float>(p[3]);
            if (p[6] == 1)
            {
                if (Player* master = FindMasterOrLeader(botAI))
                {
                    x = master->GetPositionX();
                    y = master->GetPositionY();
                    z = master->GetPositionZ();
                }
            }

            float radius = AsRange(p[4], 5.0f);
            if (bot->GetExactDist(x, y, z) <= radius)
                return false;

            return ExecuteMoveTo(botAI, x, y, z);
        }
        case 31:
        {
            Creature* target = FindNearestCreature(bot, AsUInt(p[0]), SMART_DEFAULT_SEARCH_RANGE, true);
            if (!target)
                return false;

            uint32 threshold = p[1] > 0 ? static_cast<uint32>(p[1]) : 99;
            if (target->GetHealthPct() > threshold)
                return false;

            return ExecuteCastHeal(botAI, target);
        }
        case 32:
        {
            BotSmartDestination const* destination = SelectDestination(botAI, script);
            if (!destination)
                return false;

            return ExecuteMoveTo(botAI, destination->x, destination->y, destination->z, AsUInt(destination->stayTime));
        }
        case 33:
        {
            uint32 creatureId = AsUInt(p[0]);
            if (!creatureId && boss)
                creatureId = boss->GetEntry();

            if (!creatureId)
                creatureId = script.creatureId;

            SetPhase(bot, creatureId, AsUInt(p[1]));
            return true;
        }
        case 34:
            AddForbidden(bot, script.actionParams, 0, 3);
            if (bot->GetVictim() && IsAttackForbidden(bot, bot->GetVictim()))
                ExecuteStopAttack(botAI, 1000);
            return true;
        case 35:
            RemoveForbidden(bot, script.actionParams, 0, 3);
            return true;
        case 36:
            return ExecuteAttack(botAI, FindNearestAttackTarget(bot, script));
        default:
            LOG_DEBUG("playerbots", "Unsupported bot_smart_script actionType {} on script {}", script.actionType,
                      script.id);
            return false;
    }
}

bool BotSmartStrategyMgr::ExecuteMoveTo(PlayerbotAI* botAI, float x, float y, float z, uint32 waitMs) const
{
    Player* bot = botAI->GetBot();
    if (!bot || !botAI->CanMove() || bot->HasUnitState(UNIT_STATE_IN_FLIGHT))
        return false;

    if (bot->GetExactDist(x, y, z) <= SMART_POSITION_EPSILON)
    {
        if (waitMs)
            botAI->SetNextCheckDelay(waitMs);
        return true;
    }

    bot->UpdateAllowedPositionZ(x, y, z);

    if (bot->IsSitState())
        bot->SetStandState(UNIT_STAND_STATE_STAND);

    bot->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_NONE, 0.0f, 0.0f, true, false);

    float speed = std::max(0.1f, bot->GetSpeed(MOVE_RUN));
    uint32 delay = static_cast<uint32>((bot->GetExactDist(x, y, z) / speed) * 1000.0f);
    delay = std::min<uint32>(delay + waitMs + sPlayerbotAIConfig.reactDelay, sPlayerbotAIConfig.maxWaitForMove);
    botAI->SetNextCheckDelay(delay);
    return true;
}

bool BotSmartStrategyMgr::ExecuteMoveAwayFrom(PlayerbotAI* botAI, WorldObject* source, float safeDistance) const
{
    if (!source)
        return false;

    return ExecuteMoveAwayFromPosition(botAI, source->GetPositionX(), source->GetPositionY(), source->GetPositionZ(),
                                       safeDistance);
}

bool BotSmartStrategyMgr::ExecuteMoveAwayFromPosition(PlayerbotAI* botAI, float sourceX, float sourceY, float sourceZ,
                                                      float safeDistance) const
{
    Player* bot = botAI->GetBot();
    if (!bot || safeDistance <= 0.0f)
        return false;

    float currentDistance = bot->GetExactDist(sourceX, sourceY, sourceZ);
    if (currentDistance >= safeDistance)
        return false;

    float angle = std::atan2(bot->GetPositionY() - sourceY, bot->GetPositionX() - sourceX);
    if (std::fabs(bot->GetPositionX() - sourceX) < 0.01f && std::fabs(bot->GetPositionY() - sourceY) < 0.01f)
        angle = static_cast<float>(rand_norm()) * 2.0f * Pi();

    float destinationX = sourceX + std::cos(angle) * (safeDistance + 3.0f);
    float destinationY = sourceY + std::sin(angle) * (safeDistance + 3.0f);
    float destinationZ = sourceZ;

    if (bot->GetMap())
        bot->GetMap()->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                       bot->GetPositionZ(), destinationX, destinationY,
                                                       destinationZ);

    return ExecuteMoveTo(botAI, destinationX, destinationY, destinationZ);
}

bool BotSmartStrategyMgr::ExecuteMoveFromGroup(PlayerbotAI* botAI, float distance) const
{
    Player* bot = botAI->GetBot();
    std::vector<Player*> members = GetGroupMembers(bot, true, false);
    if (members.size() <= 1)
        return ExecuteMoveAwayFromPosition(botAI, bot->GetPositionX() - 1.0f, bot->GetPositionY(), bot->GetPositionZ(),
                                           distance);

    float avgX = 0.0f;
    float avgY = 0.0f;
    float avgZ = 0.0f;
    uint32 count = 0;
    for (Player* member : members)
    {
        if (member == bot)
            continue;

        avgX += member->GetPositionX();
        avgY += member->GetPositionY();
        avgZ += member->GetPositionZ();
        ++count;
    }

    if (!count)
        return false;

    return ExecuteMoveAwayFromPosition(botAI, avgX / count, avgY / count, avgZ / count, distance);
}

bool BotSmartStrategyMgr::ExecuteAttack(PlayerbotAI* botAI, Unit* target) const
{
    Player* bot = botAI->GetBot();
    if (!bot || !target || !target->IsInWorld() || !target->IsAlive() || !bot->IsValidAttackTarget(target) ||
        IsAttackForbidden(bot, target))
        return false;

    botAI->GetAiObjectContext()->GetValue<GuidVector>("prioritized targets")->Set({target->GetGUID()});
    botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(target);
    bot->SetSelection(target->GetGUID());

    if (botAI->CanMove() && !bot->HasInArc(CAST_ANGLE_IN_FRONT, target))
        ServerFacade::instance().SetFacingTo(bot, target);

    botAI->ChangeEngine(BOT_STATE_COMBAT);
    bool shouldMelee = PlayerbotAI::IsMelee(bot) || bot->IsWithinMeleeRange(target);
    bot->Attack(target, shouldMelee);
    return true;
}

bool BotSmartStrategyMgr::ExecuteStopAttack(PlayerbotAI* botAI, uint32 durationMs) const
{
    Player* bot = botAI->GetBot();
    if (!bot)
        return false;

    bot->AttackStop();
    bot->SetTarget(ObjectGuid::Empty);
    bot->SetSelection(ObjectGuid::Empty);
    botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(nullptr);
    botAI->GetAiObjectContext()->GetValue<GuidVector>("prioritized targets")->Reset();
    if (durationMs)
        botAI->SetNextCheckDelay(durationMs);
    return true;
}

bool BotSmartStrategyMgr::ExecuteDispel(PlayerbotAI* botAI) const
{
    static std::vector<std::string> const actions = {
        "cleanse magic on party",
        "cleanse poison on party",
        "cleanse disease on party",
        "purify poison on party",
        "purify disease on party",
        "remove curse on party",
        "remove lesser curse on party",
        "cure poison on party",
        "abolish poison on party",
        "cleanse spirit poison on party",
        "cleanse spirit curse on party",
        "cleanse spirit disease on party",
        "cure toxins poison on party",
        "cure toxins disease on party",
        "devour magic cleanse"
    };

    for (std::string const& action : actions)
        if (botAI->DoSpecificAction(action, Event(), true))
            return true;

    return false;
}

bool BotSmartStrategyMgr::ExecuteTaunt(PlayerbotAI* botAI, Unit* target) const
{
    if (!target)
        return false;

    botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(target);
    botAI->GetBot()->SetSelection(target->GetGUID());
    return botAI->DoSpecificAction("taunt spell", Event(), true);
}

bool BotSmartStrategyMgr::ExecuteUseGameObject(PlayerbotAI* botAI, GameObject* go) const
{
    Player* bot = botAI->GetBot();
    if (!bot || !go)
        return false;

    WorldPacket packet(CMSG_GAMEOBJ_USE);
    packet << go->GetGUID();
    packet.rpos(0);
    bot->GetSession()->HandleGameObjectUseOpcode(packet);
    botAI->SetNextCheckDelay(sPlayerbotAIConfig.reactDelay);
    return true;
}

bool BotSmartStrategyMgr::ExecuteFocusHeal(PlayerbotAI* botAI, Player* target) const
{
    if (!target)
        return false;

    std::list<ObjectGuid> focusTargets;
    focusTargets.push_back(target->GetGUID());
    botAI->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("focus heal targets")->Set(focusTargets);
    botAI->ChangeStrategy("+focus heal targets", BOT_STATE_COMBAT);
    return true;
}

bool BotSmartStrategyMgr::ExecuteCastHeal(PlayerbotAI* botAI, Unit* target) const
{
    if (!target)
        return false;

    static std::map<uint8, std::vector<std::string>> const healsByClass = {
        {CLASS_PALADIN, {"holy shock", "flash of light", "holy light"}},
        {CLASS_PRIEST, {"penance", "flash heal", "greater heal", "heal"}},
        {CLASS_DRUID, {"swiftmend", "nourish", "regrowth", "healing touch", "rejuvenation"}},
        {CLASS_SHAMAN, {"riptide", "lesser healing wave", "healing wave"}}
    };

    Player* bot = botAI->GetBot();
    auto itr = healsByClass.find(bot->getClass());
    if (itr == healsByClass.end())
        return false;

    for (std::string const& spell : itr->second)
        if (botAI->CastSpell(spell, target))
            return true;

    return false;
}

Unit* BotSmartStrategyMgr::ResolveBoss(PlayerbotAI* botAI, BotSmartScript const& script) const
{
    Player* bot = botAI->GetBot();
    if (script.creatureId)
    {
        if (AiObjectContext* context = botAI->GetAiObjectContext())
        {
            if (Value<Unit*>* value = context->GetValue<Unit*>("current target"))
            {
                if (Unit* currentTarget = value->Get())
                {
                    if (currentTarget->GetEntry() == script.creatureId && currentTarget->IsInWorld() &&
                        currentTarget->GetMapId() == bot->GetMapId() && IsInCombatContext(bot, currentTarget))
                        return currentTarget;
                }
            }
        }

        if (Unit* victim = bot->GetVictim())
            if (victim->GetEntry() == script.creatureId && IsInCombatContext(bot, victim))
                return victim;

        for (Player* member : GetGroupMembers(bot, true, false))
        {
            if (Unit* victim = member->GetVictim())
                if (victim->GetEntry() == script.creatureId && IsInCombatContext(bot, victim))
                    return victim;
        }

        if (Creature* alive = FindNearestCreature(bot, script.creatureId, SMART_BOSS_SEARCH_RANGE, true))
            if (IsInCombatContext(bot, alive))
                return alive;

        if (Creature* dead = FindNearestCreature(bot, script.creatureId, SMART_BOSS_SEARCH_RANGE, false))
            if (IsInCombatContext(bot, dead))
                return dead;

        return nullptr;
    }

    if (AiObjectContext* context = botAI->GetAiObjectContext())
    {
        if (Value<Unit*>* value = context->GetValue<Unit*>("current target"))
            if (Unit* currentTarget = value->Get())
                if (currentTarget->IsInWorld() && currentTarget->GetMapId() == bot->GetMapId())
                    return currentTarget;
    }

    return bot->GetVictim();
}

bool BotSmartStrategyMgr::IsInCombatContext(Player* bot, Unit* unit) const
{
    if (!bot || !unit || !unit->IsInWorld() || unit->GetMapId() != bot->GetMapId())
        return false;

    if (bot->GetVictim() == unit || unit->GetVictim() == bot || bot->IsInCombatWith(unit) || unit->IsInCombatWith(bot))
        return true;

    for (Player* member : GetGroupMembers(bot, true, false))
    {
        if (!member || member->GetMapId() != bot->GetMapId())
            continue;

        if (member->GetVictim() == unit || unit->GetVictim() == member ||
            member->IsInCombatWith(unit) || unit->IsInCombatWith(member))
            return true;
    }

    return false;
}

Creature* BotSmartStrategyMgr::FindNearestCreature(Player* bot, uint32 entry, float range, bool alive) const
{
    if (!bot || !entry)
        return nullptr;

    std::list<Creature*> creatures;
    bot->GetCreatureListWithEntryInGrid(creatures, entry, range);

    Creature* nearest = nullptr;
    float nearestDistance = std::numeric_limits<float>::max();
    for (Creature* creature : creatures)
    {
        if (!creature || !creature->IsInWorld())
            continue;

        if (alive && !creature->IsAlive())
            continue;

        if (creature->GetMapId() != bot->GetMapId() || creature->GetInstanceId() != bot->GetInstanceId())
            continue;

        float distance = bot->GetExactDist(creature);
        if (distance < nearestDistance)
        {
            nearest = creature;
            nearestDistance = distance;
        }
    }

    return nearest;
}

GameObject* BotSmartStrategyMgr::FindNearestGameObject(Player* bot, uint32 entry, float range) const
{
    if (!bot || !entry)
        return nullptr;

    return bot->FindNearestGameObject(entry, range);
}

WorldObject* BotSmartStrategyMgr::FindNearestCreatureOrGameObject(Player* bot, uint32 entry, float range) const
{
    Creature* creature = FindNearestCreature(bot, entry, range, true);
    GameObject* gameObject = FindNearestGameObject(bot, entry, range);

    if (creature && gameObject)
        return bot->GetExactDist(creature) <= bot->GetExactDist(gameObject) ? static_cast<WorldObject*>(creature)
                                                                            : static_cast<WorldObject*>(gameObject);

    if (creature)
        return creature;

    return gameObject;
}

Unit* BotSmartStrategyMgr::FindPriorityAttackTarget(Player* bot, BotSmartScript const& script) const
{
    float range = AsRange(script.actionParams[3], SMART_DEFAULT_SEARCH_RANGE);
    std::array<uint32, 5> entries = {
        AsUInt(script.actionParams[0]),
        AsUInt(script.actionParams[4]),
        AsUInt(script.actionParams[5]),
        AsUInt(script.actionParams[6]),
        AsUInt(script.actionParams[7])
    };

    for (uint32 entry : entries)
    {
        if (!entry)
            continue;

        if (Creature* creature = FindNearestCreature(bot, entry, range, true))
            return creature;
    }

    return nullptr;
}

Unit* BotSmartStrategyMgr::FindNearestAttackTarget(Player* bot, BotSmartScript const& script) const
{
    float range = AsRange(script.actionParams[5], SMART_DEFAULT_SEARCH_RANGE);
    Unit* nearest = nullptr;
    float nearestDistance = std::numeric_limits<float>::max();

    for (uint8 i = 0; i < 5; ++i)
    {
        uint32 entry = AsUInt(script.actionParams[i]);
        if (!entry)
            continue;

        Creature* creature = FindNearestCreature(bot, entry, range, true);
        if (!creature || IsAttackForbidden(bot, creature))
            continue;

        float distance = bot->GetExactDist(creature);
        if (distance < nearestDistance)
        {
            nearest = creature;
            nearestDistance = distance;
        }
    }

    return nearest;
}

Unit* BotSmartStrategyMgr::FindTargetByCreatureOrDebuff(PlayerbotAI* botAI, BotSmartScript const& script,
                                                        bool includeMultiple) const
{
    Player* bot = botAI->GetBot();
    uint32 creatureId = AsUInt(script.actionParams[1]);
    uint32 debuffId = AsUInt(script.actionParams[2]);
    int32 stacks = script.actionParams[3] > 0 ? script.actionParams[3] : 1;
    uint32 excludeAura = AsUInt(script.actionParams[5]);

    if (creatureId)
        if (Creature* creature = FindNearestCreature(bot, creatureId, SMART_DEFAULT_SEARCH_RANGE, true))
            return creature;

    if (debuffId)
        return FindDebuffedGroupMember(bot, debuffId, stacks, excludeAura);

    if (includeMultiple && script.actionParams[4] > 0)
        return FindNearestCreature(bot, AsUInt(script.actionParams[4]), SMART_DEFAULT_SEARCH_RANGE, true);

    return nullptr;
}

Player* BotSmartStrategyMgr::FindDebuffedGroupMember(Player* bot, uint32 spellId, int32 stacks, uint32 excludeAura) const
{
    if (!bot || !spellId)
        return nullptr;

    Player* nearest = nullptr;
    float nearestDistance = std::numeric_limits<float>::max();
    for (Player* member : GetGroupMembers(bot, true, false))
    {
        if (excludeAura && member->HasAura(excludeAura))
            continue;

        Aura* aura = member->GetAura(spellId);
        if (!aura || (stacks > 0 && aura->GetStackAmount() < static_cast<uint8>(stacks)))
            continue;

        float distance = bot->GetExactDist(member);
        if (distance < nearestDistance)
        {
            nearest = member;
            nearestDistance = distance;
        }
    }

    return nearest;
}

Player* BotSmartStrategyMgr::FindRolePlayer(PlayerbotAI* botAI, int32 role) const
{
    Player* bot = botAI->GetBot();
    for (Player* member : GetGroupMembers(bot, true, false))
    {
        switch (role)
        {
            case 1:
                if (PlayerbotAI::IsMainTank(member))
                    return member;
                break;
            case 2:
                if (PlayerbotAI::IsAssistTankOfIndex(member, 0, true))
                    return member;
                break;
            case 3:
                if (PlayerbotAI::IsAssistTankOfIndex(member, 1, true))
                    return member;
                break;
            case 4:
                if (PlayerbotAI::IsAssistTankOfIndex(member, 2, true))
                    return member;
                break;
            case 5:
                if (PlayerbotAI::IsAssistTank(member))
                    return member;
                break;
            default:
                if (MatchesRole(member, role))
                    return member;
                break;
        }
    }

    return nullptr;
}

Player* BotSmartStrategyMgr::FindMasterOrLeader(PlayerbotAI* botAI) const
{
    if (!botAI)
        return nullptr;

    if (Player* master = botAI->GetMaster())
        if (master->IsInWorld() && master->GetMapId() == botAI->GetBot()->GetMapId())
            return master;

    return botAI->GetGroupLeader();
}

bool BotSmartStrategyMgr::AuraMatches(Unit const* unit, uint32 spellId, int32 stacks, int32 minDuration,
                                      int32 maxDuration) const
{
    if (!unit || !spellId)
        return false;

    Aura* aura = unit->GetAura(spellId);
    if (!aura)
        return false;

    if (stacks > 0 && aura->GetStackAmount() < static_cast<uint8>(stacks))
        return false;

    int32 duration = aura->GetDuration();
    if (duration >= 0)
    {
        if (minDuration > 0 && duration < minDuration)
            return false;

        if (maxDuration > 0 && duration > maxDuration)
            return false;
    }

    return true;
}

bool BotSmartStrategyMgr::AuraStackLess(Unit const* unit, uint32 spellId, int32 stacks) const
{
    if (!unit || !spellId || stacks <= 0)
        return false;

    Aura* aura = unit->GetAura(spellId);
    if (!aura)
        return true;

    return aura->GetStackAmount() < static_cast<uint8>(stacks);
}

bool BotSmartStrategyMgr::PowerInRange(Unit const* unit, int32 minPower, int32 maxPower) const
{
    if (!unit)
        return false;

    Powers powerType = unit->getPowerType();
    int32 power = unit->GetPower(powerType);
    if (power < minPower)
        return false;

    return maxPower <= 0 || power <= maxPower;
}

bool BotSmartStrategyMgr::PositionInRange(WorldObject const* object, float x, float y, float z, float radius) const
{
    if (!object)
        return false;

    return object->GetExactDist(x, y, z) <= radius;
}

BotSmartEncounterKey BotSmartStrategyMgr::BuildEncounterKey(Player* bot, BotSmartScript const& script, Unit* boss) const
{
    BotSmartEncounterKey key;
    key.mapId = bot ? bot->GetMapId() : script.mapId;
    key.instanceId = bot ? bot->GetInstanceId() : 0;
    key.creatureId = script.creatureId ? script.creatureId : (boss ? boss->GetEntry() : 0);
    return key;
}

BotSmartEncounterState& BotSmartStrategyMgr::GetEncounterState(Player* bot, BotSmartScript const& script, Unit* boss)
{
    return _encounters[BuildEncounterKey(bot, script, boss)];
}

void BotSmartStrategyMgr::UpdateEncounterState(Player* bot, BotSmartScript const& script, Unit* boss, uint32 now)
{
    BotSmartEncounterState& state = GetEncounterState(bot, script, boss);
    bool inCombat = bot->IsInCombat() || (boss && boss->IsInCombat());

    if (!inCombat)
    {
        state.combatStartedAt = 0;
        state.lastCastSpell = 0;
        state.castCounts.clear();
        state.phase = 0;
        return;
    }

    if (!state.combatStartedAt)
        state.combatStartedAt = now;

    uint32 currentCast = GetCurrentCastSpell(boss);
    if (currentCast && currentCast != state.lastCastSpell)
    {
        ++state.castCounts[currentCast];
        state.lastCastSpell = currentCast;
    }
    else if (!currentCast)
    {
        state.lastCastSpell = 0;
    }
}

uint32 BotSmartStrategyMgr::GetEncounterElapsed(Player* bot, BotSmartScript const& script, Unit* boss, uint32 now)
{
    BotSmartEncounterState& state = GetEncounterState(bot, script, boss);
    if (!state.combatStartedAt)
        return 0;

    return getMSTimeDiff(state.combatStartedAt, now);
}

uint32 BotSmartStrategyMgr::GetPhase(Player* bot, BotSmartScript const& script, Unit* boss)
{
    return GetEncounterState(bot, script, boss).phase;
}

void BotSmartStrategyMgr::SetPhase(Player* bot, uint32 creatureId, uint32 phase)
{
    if (!bot)
        return;

    BotSmartEncounterKey key;
    key.mapId = bot->GetMapId();
    key.instanceId = bot->GetInstanceId();
    key.creatureId = creatureId;
    _encounters[key].phase = phase;
}

uint32 BotSmartStrategyMgr::GetCurrentCastSpell(Unit* unit) const
{
    if (!unit)
        return 0;

    Spell* spell = unit->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    if (!spell)
        spell = unit->GetCurrentSpell(CURRENT_CHANNELED_SPELL);

    return spell && spell->GetSpellInfo() ? spell->GetSpellInfo()->Id : 0;
}

uint32 BotSmartStrategyMgr::GetCastCount(Player* bot, BotSmartScript const& script, Unit* boss, uint32 spellId)
{
    BotSmartEncounterState& state = GetEncounterState(bot, script, boss);
    auto itr = state.castCounts.find(spellId);
    return itr != state.castCounts.end() ? itr->second : 0;
}

std::vector<Player*> BotSmartStrategyMgr::GetGroupMembers(Player* bot, bool aliveOnly, bool botsOnly) const
{
    std::vector<Player*> members;
    if (!bot)
        return members;

    Group* group = bot->GetGroup();
    if (!group)
    {
        if ((!aliveOnly || bot->IsAlive()) && (!botsOnly || IsBot(bot)))
            members.push_back(bot);
        return members;
    }

    for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
    {
        Player* member = gref->GetSource();
        if (!member || member->GetMapId() != bot->GetMapId() || member->GetInstanceId() != bot->GetInstanceId())
            continue;

        if (aliveOnly && !member->IsAlive())
            continue;

        if (botsOnly && !IsBot(member))
            continue;

        members.push_back(member);
    }

    return members;
}

int32 BotSmartStrategyMgr::GetRoleIndex(PlayerbotAI* botAI, Player* player, int32 role) const
{
    if (!botAI || !player)
        return 0;

    int32 index = 0;
    for (Player* member : GetGroupMembers(botAI->GetBot(), true, true))
    {
        if (!MatchesDestinationRole(botAI, member, role))
            continue;

        ++index;
        if (member == player)
            return index;
    }

    return 0;
}

bool BotSmartStrategyMgr::MatchesDestinationRole(PlayerbotAI* /*botAI*/, Player* player, int32 role) const
{
    switch (role)
    {
        case 1:
            return PlayerbotAI::IsMelee(player) && PlayerbotAI::IsDps(player);
        case 2:
            return PlayerbotAI::IsRangedDps(player);
        case 3:
            return PlayerbotAI::IsHeal(player);
        case 4:
            return PlayerbotAI::IsTank(player);
        default:
            return false;
    }
}

BotSmartDestination const* BotSmartStrategyMgr::SelectDestination(PlayerbotAI* botAI, BotSmartScript const& script)
{
    auto smartItr = _destinationsBySmartId.find(script.id);
    if (smartItr == _destinationsBySmartId.end())
        return nullptr;

    Player* bot = botAI->GetBot();
    for (BotSmartDestination const& destination : smartItr->second)
    {
        if (!MatchesDestinationRole(botAI, bot, destination.role))
            continue;

        if (destination.allotIndex <= 0 || GetRoleIndex(botAI, bot, destination.role) == destination.allotIndex)
            return &destination;
    }

    return nullptr;
}

BotSmartWaypoint const* BotSmartStrategyMgr::SelectWaypoint(PlayerbotAI* botAI, BotSmartScript const& script,
                                                            BotSmartRuntimeState& state)
{
    auto smartItr = _waypointsBySmartId.find(script.id);
    if (smartItr == _waypointsBySmartId.end() || smartItr->second.empty())
        return nullptr;

    if (!state.waypointInitialized)
    {
        std::vector<uint32> groups;
        for (auto const& groupPair : smartItr->second)
            if (!groupPair.second.empty())
                groups.push_back(groupPair.first);

        if (groups.empty())
            return nullptr;

        if (script.actionParams[6] == 1)
            state.waypointGroup = groups[(GetBotLowGuid(botAI->GetBot()) + script.id) % groups.size()];
        else
            state.waypointGroup = groups[urand(0, groups.size() - 1)];

        state.waypointIndex = 0;
        state.waypointInitialized = true;
    }

    auto groupItr = smartItr->second.find(state.waypointGroup);
    if (groupItr == smartItr->second.end() || groupItr->second.empty())
        return nullptr;

    if (state.waypointIndex >= groupItr->second.size())
        state.waypointIndex = groupItr->second.size() - 1;

    return &groupItr->second[state.waypointIndex];
}

bool BotSmartStrategyMgr::IsAttackForbidden(Player* bot, Unit* target) const
{
    if (!bot || !target)
        return false;

    auto botItr = _forbiddenEntriesByBot.find(GetBotLowGuid(bot));
    if (botItr == _forbiddenEntriesByBot.end())
        return false;

    return botItr->second.find(target->GetEntry()) != botItr->second.end();
}

void BotSmartStrategyMgr::AddForbidden(Player* bot, std::array<int32, 8> const& entries, uint32 first, uint32 last)
{
    if (!bot)
        return;

    std::set<uint32>& forbidden = _forbiddenEntriesByBot[GetBotLowGuid(bot)];
    for (uint32 i = first; i <= last && i < entries.size(); ++i)
        if (entries[i] > 0)
            forbidden.insert(static_cast<uint32>(entries[i]));
}

void BotSmartStrategyMgr::RemoveForbidden(Player* bot, std::array<int32, 8> const& entries, uint32 first, uint32 last)
{
    if (!bot)
        return;

    auto botItr = _forbiddenEntriesByBot.find(GetBotLowGuid(bot));
    if (botItr == _forbiddenEntriesByBot.end())
        return;

    for (uint32 i = first; i <= last && i < entries.size(); ++i)
        if (entries[i] > 0)
            botItr->second.erase(static_cast<uint32>(entries[i]));
}
