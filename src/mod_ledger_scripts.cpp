#include "mod_ledger.h"

#include "Channel.h"
#include "CharacterCache.h"
#include "Creature.h"
#include "Group.h"
#include "Guild.h"
#include "Item.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <fmt/format.h>
#include <functional>
#include <mutex>
#include <unordered_map>

using namespace Ledger;

namespace
{
    // Per-character last zone. OnPlayerUpdateZone also fires for area changes
    // and on the periodic zone refresh, so only a different zone is a change.
    std::mutex s_zoneLock;
    std::unordered_map<uint32, uint32> s_lastZone;

    // A group of two disbands straight after OnRemoveMember, and OnDisband
    // still lists the member who left. Remember them so they get one row.
    std::mutex s_groupLock;
    Group const* s_lastRemovedGroup = nullptr;
    ObjectGuid s_lastRemovedGuid;

    bool IsLoggedChat(uint32 type, uint32 lang)
    {
        return lang != LANG_ADDON && type != CHAT_MSG_AFK && type != CHAT_MSG_DND;
    }

    bool IsNormalGroup(Group* group)
    {
        return group && !group->isBGGroup() && !group->isBFGroup();
    }

    bool InGuild(Player* player)
    {
        return player && player->GetGuildId() != 0;
    }

    bool GroupHasGuildMember(Group* group)
    {
        if (!group)
            return false;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (InGuild(ref->GetSource()))
                return true;

        return false;
    }

    // Group rows are kept when a real player is in the group, or (RecordGuildBots)
    // when a guild member is: regard and guild rivalries need bot company too.
    bool GroupGuard(Group* group)
    {
        return Cfg().recordAllBots || GroupHasRealPlayer(group) || (Cfg().recordGuildBots && GroupHasGuildMember(group));
    }

    // Everything a chat hook shares after the volume guard picks a listener.
    void RecordChat(Player* speaker, uint32 type, uint32 lang, std::string const& msg, Player* listener, bool involvesReal,
        std::string const& channel = "")
    {
        if (!Cfg().enable || !speaker || msg.empty() || !IsLoggedChat(type, lang))
            return;

        bool speakerReal = IsReal(speaker);
        if (speakerReal && Cfg().skipCommands && IsCommand(msg))
            return;

        if (!Cfg().recordAllBots && !Cfg().recordBotChat && !speakerReal && !involvesReal && !InGroupWithRealPlayer(speaker))
            return;

        WriteChat(speaker, type, listener ? listener->GetGUID().GetCounter() : 0, msg, channel);
    }

    bool RealPlayerOnline(std::function<bool(Player*)> const& match)
    {
        for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
            if (player && player->IsInWorld() && IsReal(player) && match(player))
                return true;

        return false;
    }

    // guildBots = false for high-volume rows nobody downstream reads for bots
    // (zone changes).
    bool EventGuard(Player* actor, Player* subject = nullptr, bool guildBots = true)
    {
        if (!Cfg().enable || !actor)
            return false;

        return Cfg().recordAllBots || IsReal(actor) || IsReal(subject) || InGroupWithRealPlayer(actor)
            || (guildBots && Cfg().recordGuildBots && (InGuild(actor) || InGuild(subject)));
    }

    void RecordKill(Player* killer, Creature* killed, bool byPet)
    {
        if (!killed || !EventGuard(killer))
            return;

        // rank: 0 normal, 1 elite, 2 rare elite, 3 boss, 4 rare. boss: a dungeon
        // encounter boss (regard's "slain together").
        CreatureTemplate const* proto = killed->GetCreatureTemplate();
        WriteEvent(killer, "kill", 0, fmt::format("{{\"entry\":{},\"name\":{},\"level\":{},\"rank\":{}{}{}}}",
            killed->GetEntry(), JsonString(killed->GetName()), killed->GetLevel(), proto ? proto->rank : 0,
            killed->IsDungeonBoss() ? ",\"boss\":1" : "", byPet ? ",\"pet\":1" : ""));
    }
}

class LedgerWorldScript : public WorldScript
{
public:
    LedgerWorldScript() : WorldScript("LedgerWorldScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        LoadConfig();
    }
};

class LedgerPlayerScript : public PlayerScript
{
public:
    LedgerPlayerScript() : PlayerScript("LedgerPlayerScript", {
        PLAYERHOOK_CAN_PLAYER_USE_CHAT,
        PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT,
        PLAYERHOOK_CAN_PLAYER_USE_GROUP_CHAT,
        PLAYERHOOK_CAN_PLAYER_USE_GUILD_CHAT,
        PLAYERHOOK_CAN_PLAYER_USE_CHANNEL_CHAT,
        PLAYERHOOK_ON_CREATURE_KILL,
        PLAYERHOOK_ON_CREATURE_KILLED_BY_PET,
        PLAYERHOOK_ON_PVP_KILL,
        PLAYERHOOK_ON_LEVEL_CHANGED,
        PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST,
        PLAYERHOOK_ON_LOOT_ITEM,
        PLAYERHOOK_ON_DUEL_END,
        PLAYERHOOK_ON_UPDATE_ZONE,
        PLAYERHOOK_ON_LOGOUT
    }) { }

    // Chat hooks are "can use" gates: every one of them must return true or
    // the message is blocked. The ledger only observes.

    // Say, yell, text emote (Player::Say/Yell/TextEmote, bots included).
    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg) override
    {
        if (Cfg().enable && (type == CHAT_MSG_SAY || type == CHAT_MSG_YELL || type == CHAT_MSG_EMOTE))
        {
            float radius = type == CHAT_MSG_YELL ? Cfg().yellListenerRadius : Cfg().listenerRadius;
            Player* listener = NearestRealPlayer(player, radius);
            RecordChat(player, type, lang, msg, listener, listener != nullptr);
        }
        return true;
    }

    // Whisper (Player::Whisper, bots included).
    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Player* receiver) override
    {
        RecordChat(player, type, lang, msg, receiver, IsReal(receiver));
        return true;
    }

    // Party / raid / battleground from a client. Bot party chat is sent as raw
    // packets by mod-playerbots and never reaches this hook.
    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Group* group) override
    {
        RecordChat(player, type, lang, msg, nullptr, GroupHasRealPlayer(group));
        return true;
    }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Guild* /*guild*/) override
    {
        RecordChat(player, type, lang, msg, nullptr, false);
        return true;
    }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Channel* channel) override
    {
        RecordChat(player, type, lang, msg, nullptr, false, channel ? channel->GetName() : "");
        return true;
    }

    void OnPlayerCreatureKill(Player* killer, Creature* killed) override
    {
        RecordKill(killer, killed, false);
    }

    void OnPlayerCreatureKilledByPet(Player* owner, Creature* killed) override
    {
        RecordKill(owner, killed, true);
    }

    void OnPlayerPVPKill(Player* killer, Player* killed) override
    {
        // Unit::Kill reports self-inflicted deaths (falling, drowning) as a
        // kill of yourself.
        if (!killed || killer == killed || !EventGuard(killer, killed))
            return;

        WriteEvent(killer, "pvp_kill", killed->GetGUID().GetCounter(), "");
    }

    void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override
    {
        if (!player || player->GetLevel() <= oldLevel || !EventGuard(player))
            return;

        WriteEvent(player, "level_up", 0, fmt::format("{{\"old\":{},\"new\":{}}}", oldLevel, player->GetLevel()));
    }

    // Fires from Player::RewardQuest, i.e. on turn-in.
    void OnPlayerCompleteQuest(Player* player, Quest const* quest) override
    {
        if (!quest || !EventGuard(player))
            return;

        WriteEvent(player, "quest_complete", 0, fmt::format("{{\"quest\":{},\"title\":{}}}",
            quest->GetQuestId(), JsonString(quest->GetTitle())));
    }

    void OnPlayerLootItem(Player* player, Item* item, uint32 count, ObjectGuid /*lootGuid*/) override
    {
        if (!item || !Cfg().enable)
            return;

        ItemTemplate const* proto = item->GetTemplate();
        if (!proto || proto->Quality < Cfg().lootMinQuality || !EventGuard(player))
            return;

        WriteEvent(player, "loot_item", 0, fmt::format("{{\"item\":{},\"quality\":{},\"count\":{},\"name\":{}}}",
            proto->ItemId, proto->Quality, count, JsonString(proto->Name1)));
    }

    // Called as (winner, loser).
    void OnPlayerDuelEnd(Player* winner, Player* loser, DuelCompleteType type) override
    {
        if (type == DUEL_INTERRUPTED || !winner || !loser || !EventGuard(winner, loser))
            return;

        std::string detail = type == DUEL_FLED ? "{\"fled\":1}" : "";
        WriteEvent(winner, "duel_won", loser->GetGUID().GetCounter(), detail);
        WriteEvent(loser, "duel_lost", winner->GetGUID().GetCounter(), detail);
    }

    void OnPlayerUpdateZone(Player* player, uint32 newZone, uint32 /*newArea*/) override
    {
        if (!player)
            return;

        uint32 oldZone = 0;
        {
            std::lock_guard<std::mutex> lock(s_zoneLock);
            auto [itr, inserted] = s_lastZone.try_emplace(player->GetGUID().GetCounter(), newZone);
            if (inserted || itr->second == newZone)
                return;

            oldZone = itr->second;
            itr->second = newZone;
        }

        if (!EventGuard(player, nullptr, false))
            return;

        WriteEvent(player, "zone_change", 0, fmt::format("{{\"from\":{},\"to\":{}}}", oldZone, newZone));
    }

    void OnPlayerLogout(Player* player) override
    {
        if (!player)
            return;

        std::lock_guard<std::mutex> lock(s_zoneLock);
        s_lastZone.erase(player->GetGUID().GetCounter());
    }
};

// One hook for every death: PvP, creature, pet and environmental. The killer
// comes straight from Unit::Kill, so nothing has to be carried between hooks.
class LedgerUnitScript : public UnitScript
{
public:
    LedgerUnitScript() : UnitScript("LedgerUnitScript", true, { UNITHOOK_ON_UNIT_DEATH }) { }

    void OnUnitDeath(Unit* unit, Unit* killer) override
    {
        if (!Cfg().enable || !unit || !unit->IsPlayer())
            return;

        Player* victim = unit->ToPlayer();
        Player* killerPlayer = killer ? killer->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
        if (killerPlayer == victim)
            killerPlayer = nullptr;

        if (!EventGuard(victim, killerPlayer))
            return;

        std::string detail;
        if (!killer || killer == victim)
            detail = "{\"cause\":\"environment\"}";
        else if (!killerPlayer && killer->IsCreature())
            detail = fmt::format("{{\"entry\":{},\"name\":{}}}", killer->GetEntry(), JsonString(killer->GetName()));
        else if (killerPlayer && killer != killerPlayer)
            detail = "{\"by_pet\":1}";

        WriteEvent(victim, "death", killerPlayer ? killerPlayer->GetGUID().GetCounter() : 0, detail);
    }
};

class LedgerGroupScript : public GroupScript
{
public:
    LedgerGroupScript() : GroupScript("LedgerGroupScript", {
        GROUPHOOK_ON_ADD_MEMBER,
        GROUPHOOK_ON_REMOVE_MEMBER,
        GROUPHOOK_ON_DISBAND
    }) { }

    // Also fires for the leader inside Group::Create.
    void OnAddMember(Group* group, ObjectGuid guid) override
    {
        if (!Cfg().enable || !IsNormalGroup(group))
            return;

        Player* member = ObjectAccessor::FindConnectedPlayer(guid);
        if (!member || !GroupGuard(group))
            return;

        WriteEvent(member, "group_join", group->GetLeaderGUID().GetCounter(), "");
    }

    void OnRemoveMember(Group* group, ObjectGuid guid, RemoveMethod method, ObjectGuid kicker, char const* /*reason*/) override
    {
        if (!Cfg().enable || !IsNormalGroup(group))
            return;

        {
            std::lock_guard<std::mutex> lock(s_groupLock);
            s_lastRemovedGroup = group;
            s_lastRemovedGuid = guid;
        }

        Player* member = ObjectAccessor::FindConnectedPlayer(guid);
        bool memberReal = member ? IsReal(member) : true; // offline members are never random bots
        if (!(memberReal || GroupGuard(group) || (Cfg().recordGuildBots && InGuild(member))))
            return;

        std::string detail = fmt::format("{{\"method\":{}{}}}", static_cast<uint32>(method),
            kicker ? fmt::format(",\"kicker\":{}", kicker.GetCounter()) : "");
        uint32 leader = group->GetLeaderGUID().GetCounter();

        if (member)
            WriteEvent(member, "group_leave", leader == guid.GetCounter() ? 0 : leader, detail);
        else
            WriteEvent(guid.GetCounter(), false, 0, 0, "group_leave", leader == guid.GetCounter() ? 0 : leader, detail);
    }

    // Members still in the group when it dissolves leave too.
    void OnDisband(Group* group) override
    {
        if (!Cfg().enable || !IsNormalGroup(group))
            return;

        ObjectGuid alreadyWritten;
        {
            std::lock_guard<std::mutex> lock(s_groupLock);
            if (s_lastRemovedGroup == group)
                alreadyWritten = s_lastRemovedGuid;
            s_lastRemovedGroup = nullptr;
            s_lastRemovedGuid.Clear();
        }

        if (!GroupGuard(group))
            return;

        uint32 leader = group->GetLeaderGUID().GetCounter();
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member->GetGUID() == alreadyWritten)
                continue;

            uint32 subject = member->GetGUID().GetCounter() == leader ? 0 : leader;
            WriteEvent(member, "group_leave", subject, "{\"disband\":1}");
        }
    }
};

// Bot party, raid, guild and channel lines are sent as raw packets or through
// Channel::Say and never reach OnPlayerCanUseChat. mod-playerbots and
// mod-ollama-chat call this at those send sites through a weak declaration, so
// they still link without mod-ledger. Keep the signature in sync with them.
// Defined in this file so it lands in the same object as the script loader.
void LedgerRecordBotChat(Player* bot, uint32 type, std::string const& msg, Channel* channel)
{
    if (!Cfg().enable || !bot || msg.empty())
        return;

    bool involvesReal = false;
    switch (type)
    {
        case CHAT_MSG_PARTY:
        case CHAT_MSG_RAID:
            involvesReal = GroupHasRealPlayer(bot->GetGroup());
            break;
        case CHAT_MSG_GUILD:
        {
            uint32 guildId = bot->GetGuildId();
            involvesReal = guildId && RealPlayerOnline([guildId](Player* p) { return p->GetGuildId() == guildId; });
            break;
        }
        case CHAT_MSG_CHANNEL:
            involvesReal = channel && RealPlayerOnline([channel](Player* p) { return p->IsInChannel(channel); });
            break;
        default:
            break;
    }

    RecordChat(bot, type, LANG_UNIVERSAL, msg, nullptr, involvesReal, channel ? channel->GetName() : "");
}

namespace
{
    // Guild events name characters by guid: members are often offline when they are removed or promoted.
    void WriteGuildEvent(ObjectGuid::LowType actor, char const* eventType, ObjectGuid::LowType subject,
        std::string const& detail)
    {
        if (!Cfg().enable || !actor)
            return;

        ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(actor);
        if (Player* player = ObjectAccessor::FindConnectedPlayer(guid))
            WriteEvent(player, eventType, subject, detail);
        else
            WriteEvent(actor, sPlayerbotAIConfig.IsInRandomAccountList(sCharacterCache->GetCharacterAccountIdByGuid(guid)),
                0, 0, eventType, subject, detail);
    }
}

// Invites, joins, rank changes, removals, departures, founding and disbanding (custom wow plans/18, step P1).
// Always recorded: membership changes are rare. Guild::_LogEvent is where the core reports the member events
// with guids; GM commands that skip it (.guild uninvite, .guild rank) and leader hand-overs are not seen.
class LedgerGuildScript : public GuildScript
{
public:
    LedgerGuildScript() : GuildScript("LedgerGuildScript", {
        GUILDHOOK_ON_CREATE,
        GUILDHOOK_ON_DISBAND,
        GUILDHOOK_ON_EVENT
    }) { }

    // Guild::Create has already added the founder (a guild_join row comes first); charter signers join after.
    void OnCreate(Guild* guild, Player* leader, std::string const& name) override
    {
        if (!Cfg().enable || !guild || !leader)
            return;

        WriteEvent(leader, "guild_found", 0, fmt::format("{{\"guild\":{},\"name\":{}}}", guild->GetId(), JsonString(name)));
    }

    // Runs before the members are removed; they get no guild_leave rows of their own.
    void OnDisband(Guild* guild) override
    {
        if (!guild)
            return;

        WriteGuildEvent(guild->GetLeaderGUID().GetCounter(), "guild_disband", 0,
            fmt::format("{{\"guild\":{},\"name\":{}}}", guild->GetId(), JsonString(guild->GetName())));
    }

    void OnEvent(Guild* guild, uint8 eventType, ObjectGuid::LowType guid1, ObjectGuid::LowType guid2, uint8 newRank) override
    {
        if (!guild)
            return;

        uint32 const id = guild->GetId();
        switch (eventType)
        {
            case GUILD_EVENT_LOG_INVITE_PLAYER:     // guid1 invited guid2
                WriteGuildEvent(guid1, "guild_invite", guid2, fmt::format("{{\"guild\":{}}}", id));
                break;
            case GUILD_EVENT_LOG_JOIN_GUILD:        // guid1 joined
            {
                Guild::Member const* member = guild->GetMember(ObjectGuid::Create<HighGuid::Player>(guid1));
                WriteGuildEvent(guid1, "guild_join", 0,
                    fmt::format("{{\"guild\":{},\"rank\":{}}}", id, member ? uint32(member->GetRankId()) : 0));
                break;
            }
            case GUILD_EVENT_LOG_PROMOTE_PLAYER:    // guid1 changed guid2's rank
            case GUILD_EVENT_LOG_DEMOTE_PLAYER:
                WriteGuildEvent(guid2, "guild_rank", guid1, fmt::format("{{\"guild\":{},\"rank\":{},\"promote\":{}}}",
                    id, uint32(newRank), eventType == GUILD_EVENT_LOG_PROMOTE_PLAYER ? 1 : 0));
                break;
            case GUILD_EVENT_LOG_UNINVITE_PLAYER:   // guid1 removed guid2
                WriteGuildEvent(guid2, "guild_leave", guid1, fmt::format("{{\"guild\":{},\"removed\":1}}", id));
                break;
            case GUILD_EVENT_LOG_LEAVE_GUILD:       // guid1 left
                WriteGuildEvent(guid1, "guild_leave", 0, fmt::format("{{\"guild\":{}}}", id));
                break;
            default:
                break;
        }
    }
};

// A real player turning down a guild invite at the dialog (custom wow plans/18, step P4 gap). The core has no guild
// hook for it, so the decline packet is read on its way in, while the invite is still on the player.
class LedgerServerScript : public ServerScript
{
public:
    LedgerServerScript() : ServerScript("LedgerServerScript", { SERVERHOOK_CAN_PACKET_RECEIVE }) { }

    bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
    {
        if (packet.GetOpcode() != CMSG_GUILD_DECLINE || !Cfg().enable || !session)
            return true;

        Player* player = session->GetPlayer();
        if (player && !player->GetGuildId() && player->GetGuildIdInvited())
            WriteEvent(player, "guild_decline", 0, fmt::format("{{\"guild\":{}}}", player->GetGuildIdInvited()));
        return true;
    }
};

void Addmod_ledgerScripts()
{
    new LedgerWorldScript();
    new LedgerPlayerScript();
    new LedgerUnitScript();
    new LedgerGroupScript();
    new LedgerGuildScript();
    new LedgerServerScript();
}
