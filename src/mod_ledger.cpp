#include "mod_ledger.h"

#include "CellImpl.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Log.h"
#include "Player.h"
#include "WorldSession.h"

#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <fmt/format.h>
#include <list>
#include <sstream>

namespace
{
    Ledger::Config s_config;

    std::string TruncateUtf8(std::string_view text, size_t maxBytes)
    {
        if (text.size() <= maxBytes)
            return std::string(text);

        // Never cut inside a multi-byte sequence: strict mode rejects the row.
        size_t n = maxBytes;
        while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80)
            --n;

        return std::string(text.substr(0, n));
    }

    std::string Trim(std::string const& s)
    {
        size_t start = s.find_first_not_of(" \t");
        if (start == std::string::npos)
            return "";

        size_t end = s.find_last_not_of(" \t");
        return s.substr(start, end - start + 1);
    }

    std::string NullableGuid(uint32 guid)
    {
        return guid ? std::to_string(guid) : "NULL";
    }
}

namespace Ledger
{
    Config const& Cfg()
    {
        return s_config;
    }

    void LoadConfig()
    {
        Config cfg;
        cfg.enable             = sConfigMgr->GetOption<bool>("Ledger.Enable", true);
        cfg.recordAllBots      = sConfigMgr->GetOption<bool>("Ledger.RecordAllBots", false);
        cfg.recordBotChat      = sConfigMgr->GetOption<bool>("Ledger.RecordBotChat", false);
        cfg.recordGuildBots    = sConfigMgr->GetOption<bool>("Ledger.RecordGuildBots", false);
        cfg.listenerRadius     = sConfigMgr->GetOption<float>("Ledger.ListenerRadius", 25.0f);
        cfg.yellListenerRadius = sConfigMgr->GetOption<float>("Ledger.YellListenerRadius", 100.0f);
        cfg.skipCommands       = sConfigMgr->GetOption<bool>("Ledger.SkipCommands", true);
        cfg.lootMinQuality     = sConfigMgr->GetOption<uint32>("Ledger.LootMinQuality", 3);

        // mod-ollama-chat's built-in prefixes plus its configured list, trimmed
        // the same way it trims them.
        cfg.commandPrefixes = { ".playerbots", "playerbot" };
        std::stringstream ss(sConfigMgr->GetOption<std::string>("OllamaChat.BlacklistCommands", "", false));
        std::string token;
        while (std::getline(ss, token, ','))
        {
            token = Trim(token);
            if (!token.empty())
                cfg.commandPrefixes.push_back(token);
        }

        s_config = std::move(cfg);

        LOG_INFO("module", "[Ledger] Enable={} RecordAllBots={} RecordBotChat={} RecordGuildBots={} ListenerRadius={} YellListenerRadius={} SkipCommands={} ({} prefixes) LootMinQuality={}",
            s_config.enable, s_config.recordAllBots, s_config.recordBotChat, s_config.recordGuildBots,
            s_config.listenerRadius, s_config.yellListenerRadius,
            s_config.skipCommands, s_config.commandPrefixes.size(), s_config.lootMinQuality);
    }

    bool IsBot(Player* player)
    {
        if (!player)
            return false;

        if (WorldSession* session = player->GetSession())
            if (session->IsBot())
                return true;

        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(player);
        return ai && ai->IsBotAI();
    }

    bool IsReal(Player* player)
    {
        return player && !IsBot(player);
    }

    bool GroupHasRealPlayer(Group* group)
    {
        if (!group)
            return false;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (IsReal(ref->GetSource()))
                return true;

        return false;
    }

    bool InGroupWithRealPlayer(Player* player)
    {
        return player && GroupHasRealPlayer(player->GetGroup());
    }

    Player* NearestRealPlayer(Player* from, float radius)
    {
        if (!from || !from->IsInWorld() || radius <= 0.0f)
            return nullptr;

        std::list<Player*> players;
        Acore::AnyPlayerInObjectRangeCheck checker(from, radius, false);
        Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(from, players, checker);
        Cell::VisitObjects(from, searcher, radius);

        Player* nearest = nullptr;
        float nearestDist = 0.0f;
        for (Player* player : players)
        {
            if (player == from || !IsReal(player))
                continue;

            float dist = from->GetExactDistSq(player);
            if (!nearest || dist < nearestDist)
            {
                nearest = player;
                nearestDist = dist;
            }
        }

        return nearest;
    }

    bool IsCommand(std::string const& msg)
    {
        std::string text = msg;
        size_t end = text.find_last_not_of(" \t\r\n");
        text.erase(end == std::string::npos ? 0 : end + 1);

        // Whole-word prefix match, case-sensitive, as mod-ollama-chat does it.
        for (std::string const& prefix : s_config.commandPrefixes)
        {
            if (text.size() < prefix.size() || text.compare(0, prefix.size(), prefix) != 0)
                continue;

            if (text.size() == prefix.size() || !std::isalnum(static_cast<unsigned char>(text[prefix.size()])))
                return true;
        }

        return false;
    }

    std::string JsonString(std::string_view text, size_t maxBytes)
    {
        std::string clipped = TruncateUtf8(text, maxBytes);
        std::string out = "\"";
        for (char c : clipped)
        {
            switch (c)
            {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                        out += fmt::format("\\u{:04x}", static_cast<unsigned char>(c));
                    else
                        out += c;
            }
        }
        out += '"';
        return out;
    }

    void WriteChat(Player* speaker, uint32 chatType, uint32 listenerGuid, std::string const& text,
        std::string const& channel)
    {
        std::string name = TruncateUtf8(speaker->GetName(), 24);
        std::string body = TruncateUtf8(text, 512);
        CharacterDatabase.EscapeString(name);
        CharacterDatabase.EscapeString(body);

        std::string channelSql = "NULL";
        if (!channel.empty())
        {
            std::string escaped = TruncateUtf8(channel, 32);
            CharacterDatabase.EscapeString(escaped);
            channelSql = "'" + escaped + "'";
        }

        CharacterDatabase.Execute(
            "INSERT INTO ledger_chat (speaker_guid, speaker_name, speaker_is_bot, listener_guid, chat_type, channel, zone_id, map_id, text) "
            "VALUES ({}, '{}', {}, {}, {}, {}, {}, {}, '{}')",
            speaker->GetGUID().GetCounter(), name, IsBot(speaker) ? 1 : 0, NullableGuid(listenerGuid),
            chatType, channelSql, speaker->GetZoneId(), speaker->GetMapId(), body);
    }

    void WriteEvent(Player* actor, char const* eventType, uint32 subjectGuid, std::string const& detail)
    {
        WriteEvent(actor->GetGUID().GetCounter(), IsBot(actor), actor->GetZoneId(), actor->GetMapId(),
            eventType, subjectGuid, detail);
    }

    void WriteEvent(uint32 actorGuid, bool actorIsBot, uint32 zoneId, uint32 mapId,
        char const* eventType, uint32 subjectGuid, std::string const& detail)
    {
        std::string detailSql = "NULL";
        if (!detail.empty())
        {
            std::string escaped = TruncateUtf8(detail, 255);
            CharacterDatabase.EscapeString(escaped);
            detailSql = "'" + escaped + "'";
        }

        CharacterDatabase.Execute(
            "INSERT INTO ledger_event (actor_guid, actor_is_bot, event_type, subject_guid, zone_id, map_id, detail) "
            "VALUES ({}, {}, '{}', {}, {}, {}, {})",
            actorGuid, actorIsBot ? 1 : 0, eventType, NullableGuid(subjectGuid), zoneId, mapId, detailSql);
    }
}
