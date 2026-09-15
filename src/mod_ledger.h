#ifndef MOD_LEDGER_H
#define MOD_LEDGER_H

#include "Define.h"
#include <string>
#include <string_view>
#include <vector>

class Group;
class Player;

namespace Ledger
{
    struct Config
    {
        bool enable = true;
        bool recordAllBots = false;
        bool recordBotChat = false;     // every bot chat line (regard needs bot-to-bot talk)
        bool recordGuildBots = false;   // events and groups of guild members
        float listenerRadius = 25.0f;
        float yellListenerRadius = 100.0f;
        bool skipCommands = true;
        uint32 lootMinQuality = 3;
        bool recordAuctions = false;    // auction listings, sales and expiries (ledger_auction)
        std::vector<std::string> commandPrefixes;
    };

    Config const& Cfg();
    void LoadConfig();

    // Same test as mod-ollama-chat: the session flag is right from login, the
    // AI lookup covers bots attached some other way.
    bool IsBot(Player* player);
    bool IsReal(Player* player);
    bool GroupHasRealPlayer(Group* group);
    bool InGroupWithRealPlayer(Player* player);
    Player* NearestRealPlayer(Player* from, float radius);
    bool IsCommand(std::string const& msg);

    std::string JsonString(std::string_view text, size_t maxBytes = 64);

    // Callers have already applied the volume guard. listenerGuid / subjectGuid
    // of 0 are written as NULL; an empty detail is NULL.
    void WriteChat(Player* speaker, uint32 chatType, uint32 listenerGuid, std::string const& text,
        std::string const& channel = "");
    void WriteEvent(Player* actor, char const* eventType, uint32 subjectGuid, std::string const& detail);
    void WriteEvent(uint32 actorGuid, bool actorIsBot, uint32 zoneId, uint32 mapId,
        char const* eventType, uint32 subjectGuid, std::string const& detail);
}

#endif // MOD_LEDGER_H
