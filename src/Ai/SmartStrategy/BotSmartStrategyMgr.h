/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license,
 * you may redistribute it and/or modify it under version 3 of the License, or (at your option),
 * any later version.
 */

#ifndef _PLAYERBOT_BOTSMARTSTRATEGYMGR_H
#define _PLAYERBOT_BOTSMARTSTRATEGYMGR_H

#include "Define.h"

#include <array>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

class Aura;
class Creature;
class GameObject;
class Player;
class PlayerbotAI;
class Unit;
class WorldObject;

struct BotSmartEvent
{
    int32 type = 0;
    std::array<int32, 6> params = {};

    bool IsConfigured(bool primary) const;
};

struct BotSmartScript
{
    uint32 id = 0;
    uint32 mapId = 0;
    uint32 groupId = 0;
    uint32 creatureId = 0;
    int32 actionType = 0;
    std::array<int32, 8> actionParams = {};
    std::array<BotSmartEvent, 4> events = {};
    std::string remark;
    int32 extraType = 0;
    std::array<int32, 6> extraParams = {};
    int32 orderNum = 0;
    int32 targetType = 0;
    std::array<int32, 4> targetParams = {};
    int32 bossPhase = 0;
    int32 diff = 0;
};

struct BotSmartWaypoint
{
    uint32 id = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    uint32 smartId = 0;
    int32 type = 0;
    int32 orderNum = 0;
    int32 groupId = 0;
    std::array<int32, 3> params = {};
};

struct BotSmartDestination
{
    uint32 id = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    uint32 smartId = 0;
    int32 role = 0;
    int32 allotIndex = 0;
    int32 stayTime = 0;
    int32 type = 0;
    std::array<int32, 3> params = {};
};

struct BotSmartRuntimeKey
{
    uint64 botGuid = 0;
    uint32 scriptId = 0;

    bool operator<(BotSmartRuntimeKey const& other) const
    {
        if (botGuid != other.botGuid)
            return botGuid < other.botGuid;

        return scriptId < other.scriptId;
    }
};

struct BotSmartEncounterKey
{
    uint32 mapId = 0;
    uint32 instanceId = 0;
    uint32 creatureId = 0;

    bool operator<(BotSmartEncounterKey const& other) const
    {
        if (mapId != other.mapId)
            return mapId < other.mapId;

        if (instanceId != other.instanceId)
            return instanceId < other.instanceId;

        return creatureId < other.creatureId;
    }
};

struct BotSmartRuntimeState
{
    uint32 eventMatchedAt = 0;
    uint32 executeAt = 0;
    uint32 activeUntil = 0;
    uint32 nextActionAt = 0;
    uint32 waypointGroup = 0;
    uint32 waypointIndex = 0;
    bool extraOneShotConsumed = false;
    bool waypointInitialized = false;
};

struct BotSmartEncounterState
{
    uint32 combatStartedAt = 0;
    uint32 phase = 0;
    uint32 lastCastSpell = 0;
    std::map<uint32, uint32> castCounts;
};

class BotSmartStrategyMgr
{
public:
    static BotSmartStrategyMgr& Instance();

    bool Initialize();
    bool Reload();
    void Update(PlayerbotAI* botAI, uint32 diff);

    std::size_t GetScriptCount() const;
    std::size_t GetWaypointCount() const;
    std::size_t GetDestinationCount() const;

private:
    BotSmartStrategyMgr() = default;

    bool LoadFromDB();
    void Clear();

    bool ProcessScript(PlayerbotAI* botAI, BotSmartScript const& script, uint32 now);
    bool MatchesBasicFilters(Player* bot, BotSmartScript const& script) const;
    bool MatchesEvents(PlayerbotAI* botAI, BotSmartScript const& script, Unit* boss, uint32 now);
    bool MatchesEvent(PlayerbotAI* botAI, BotSmartScript const& script, BotSmartEvent const& event, Unit* boss,
                      uint32 now);
    bool MatchesTargetFilter(PlayerbotAI* botAI, BotSmartScript const& script) const;
    bool MatchesActionFilter(PlayerbotAI* botAI, BotSmartScript const& script) const;
    bool MatchesRole(Player* player, int32 role) const;
    bool MatchesClass(Player* player, int32 classId) const;
    bool MatchesActionRole(PlayerbotAI* botAI, BotSmartScript const& script, int32 role) const;
    bool IsAssigned(PlayerbotAI* botAI, BotSmartScript const& script, int32 role, int32 count,
                    int32 classId = 0) const;

    bool ApplyExtra(BotSmartScript const& script, BotSmartRuntimeState& state, uint32 now);
    uint32 GetActionCooldown(BotSmartScript const& script) const;

    bool ExecuteAction(PlayerbotAI* botAI, BotSmartScript const& script, Unit* boss, BotSmartRuntimeState& state,
                       uint32 now);

    bool ExecuteMoveTo(PlayerbotAI* botAI, float x, float y, float z, uint32 waitMs = 0) const;
    bool ExecuteMoveAwayFrom(PlayerbotAI* botAI, WorldObject* source, float safeDistance) const;
    bool ExecuteMoveAwayFromPosition(PlayerbotAI* botAI, float sourceX, float sourceY, float sourceZ,
                                     float safeDistance) const;
    bool ExecuteMoveFromGroup(PlayerbotAI* botAI, float distance) const;
    bool ExecuteAttack(PlayerbotAI* botAI, Unit* target) const;
    bool ExecuteStopAttack(PlayerbotAI* botAI, uint32 durationMs) const;
    bool ExecuteDispel(PlayerbotAI* botAI) const;
    bool ExecuteTaunt(PlayerbotAI* botAI, Unit* target) const;
    bool ExecuteUseGameObject(PlayerbotAI* botAI, GameObject* go) const;
    bool ExecuteFocusHeal(PlayerbotAI* botAI, Player* target) const;
    bool ExecuteCastHeal(PlayerbotAI* botAI, Unit* target) const;

    Unit* ResolveBoss(PlayerbotAI* botAI, BotSmartScript const& script) const;
    Creature* FindNearestCreature(Player* bot, uint32 entry, float range, bool alive = true) const;
    GameObject* FindNearestGameObject(Player* bot, uint32 entry, float range) const;
    WorldObject* FindNearestCreatureOrGameObject(Player* bot, uint32 entry, float range) const;
    Unit* FindPriorityAttackTarget(Player* bot, BotSmartScript const& script) const;
    Unit* FindNearestAttackTarget(Player* bot, BotSmartScript const& script) const;
    Unit* FindTargetByCreatureOrDebuff(PlayerbotAI* botAI, BotSmartScript const& script, bool includeMultiple) const;
    Player* FindDebuffedGroupMember(Player* bot, uint32 spellId, int32 stacks, uint32 excludeAura) const;
    Player* FindRolePlayer(PlayerbotAI* botAI, int32 role) const;
    Player* FindMasterOrLeader(PlayerbotAI* botAI) const;
    bool IsInCombatContext(Player* bot, Unit* unit) const;

    bool AuraMatches(Unit const* unit, uint32 spellId, int32 stacks, int32 minDuration, int32 maxDuration) const;
    bool AuraStackLess(Unit const* unit, uint32 spellId, int32 stacks) const;
    bool PowerInRange(Unit const* unit, int32 minPower, int32 maxPower) const;
    bool PositionInRange(WorldObject const* object, float x, float y, float z, float radius) const;

    BotSmartEncounterKey BuildEncounterKey(Player* bot, BotSmartScript const& script, Unit* boss) const;
    BotSmartEncounterState& GetEncounterState(Player* bot, BotSmartScript const& script, Unit* boss);
    void UpdateEncounterState(Player* bot, BotSmartScript const& script, Unit* boss, uint32 now);
    uint32 GetEncounterElapsed(Player* bot, BotSmartScript const& script, Unit* boss, uint32 now);
    uint32 GetPhase(Player* bot, BotSmartScript const& script, Unit* boss);
    void SetPhase(Player* bot, uint32 creatureId, uint32 phase);
    uint32 GetCurrentCastSpell(Unit* unit) const;
    uint32 GetCastCount(Player* bot, BotSmartScript const& script, Unit* boss, uint32 spellId);

    std::vector<Player*> GetGroupMembers(Player* bot, bool aliveOnly = true, bool botsOnly = false) const;
    int32 GetRoleIndex(PlayerbotAI* botAI, Player* player, int32 role) const;
    bool MatchesDestinationRole(PlayerbotAI* botAI, Player* player, int32 role) const;
    BotSmartDestination const* SelectDestination(PlayerbotAI* botAI, BotSmartScript const& script);
    BotSmartWaypoint const* SelectWaypoint(PlayerbotAI* botAI, BotSmartScript const& script,
                                           BotSmartRuntimeState& state);

    bool IsAttackForbidden(Player* bot, Unit* target) const;
    void AddForbidden(Player* bot, std::array<int32, 8> const& entries, uint32 first, uint32 last);
    void RemoveForbidden(Player* bot, std::array<int32, 8> const& entries, uint32 first, uint32 last);

private:
    mutable std::mutex _lock;
    std::unordered_map<uint32, std::vector<BotSmartScript>> _scriptsByMap;
    std::unordered_map<uint32, std::map<uint32, std::vector<BotSmartWaypoint>>> _waypointsBySmartId;
    std::unordered_map<uint32, std::vector<BotSmartDestination>> _destinationsBySmartId;

    std::map<BotSmartRuntimeKey, BotSmartRuntimeState> _runtime;
    std::map<BotSmartEncounterKey, BotSmartEncounterState> _encounters;
    std::map<uint64, std::set<uint32>> _forbiddenEntriesByBot;

    std::size_t _scriptCount = 0;
    std::size_t _waypointCount = 0;
    std::size_t _destinationCount = 0;
};

#define sBotSmartStrategyMgr BotSmartStrategyMgr::Instance()

#endif
