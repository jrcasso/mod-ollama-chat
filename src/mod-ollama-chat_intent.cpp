#include "mod-ollama-chat_intent.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat-utilities.h"
#include "mod-ollama-chat_telemetry.h"

#include "Group.h"
#include "Player.h"

#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "PlayerbotSecurity.h"
#include "Playerbots.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <unordered_map>

namespace
{
    using Clock = std::chrono::steady_clock;

    // What a bot may be talked into. Every entry is an existing mod-playerbots
    // chat command, so nothing here executes anything new.
    //
    // `tag` is what the model writes; `command` is what playerbots receives.
    // They are separate because the tag should read as an intention ("join")
    // while the command is whatever the command layer happens to call it.
    struct Intent
    {
        char const* tag;
        char const* command;
        char const* when;      // shown to the model so it knows when to use it
        bool        needsGroup;
    };

    constexpr Intent kIntents[] =
    {
        // Party and group.
        { "invite",     "invite",      "they want to join your group, or asked you to invite them", false },
        { "leave",      "leave",       "they asked you to leave the group",                          true  },
        { "follow",     "follow",      "they asked you to come with them or follow",                 false },
        { "stay",       "stay",        "they asked you to wait here or hold position",               false },
        { "giveleader", "give leader", "they asked to lead, or for you to hand over leadership",     true  },

        // Trade.
        { "trade",      "trade",       "they want to trade with you, or to hand you something",      false },

        // Vendor.
        { "sell",       "sell gray",   "they told you to sell your junk or grey items",              false },
        { "repair",     "repair",      "they told you to repair your gear",                          false },

        // Loot.
        { "roll",       "roll",        "they told you to roll on the loot",                          true  },

        // LFG.
        { "lfg",        "join lfg",    "they asked you to queue for a dungeon or use the finder",    false },
    };

    // One action per bot per speaker per cooldown. The model is perfectly
    // capable of emitting [do:invite] on three replies in a row; the command
    // layer would then send three invites.
    std::mutex g_cooldownMutex;
    std::unordered_map<uint64_t, std::pair<std::string, Clock::time_point>> g_lastIntent;

    bool OnCooldown(Player* bot, std::string const& command)
    {
        const auto now = Clock::now();
        std::lock_guard<std::mutex> lock(g_cooldownMutex);

        auto& entry = g_lastIntent[bot->GetGUID().GetRawValue()];
        if (entry.first == command &&
            now - entry.second < std::chrono::seconds(g_IntentCooldownSeconds))
            return true;

        entry = { command, now };
        return false;
    }
}

std::string Intent_BuildPrompt(Player* bot, Player* speaker)
{
    if (!g_EnableChatIntents || !bot || !speaker)
        return "";

    // Never from another bot. Bots talk to each other constantly, and letting
    // that drive commands would have them inviting, trading and queueing each
    // other in loops no one asked for.
    if (GET_PLAYERBOT_AI(speaker))
        return "";

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return "";

    // Ask the security model up front rather than offering the bot actions it
    // will refuse. A bot that says "sure!" and then silently does nothing is
    // worse than one that never offered.
    if (!botAI->GetSecurity()->CheckLevelFor(PLAYERBOT_SECURITY_INVITE, true, speaker))
        return "";

    const bool inGroup = bot->GetGroup() != nullptr;

    std::string list;
    for (Intent const& intent : kIntents)
    {
        if (intent.needsGroup && !inGroup)
            continue;
        if (!std::string(intent.tag).compare("invite") && inGroup && bot->GetGroup()->IsFull())
            continue;

        if (!list.empty())
            list += ", ";

        list += "[do:";
        list += intent.tag;
        list += "] if ";
        list += intent.when;
    }

    if (list.empty())
        return "";

    return " If -- and only if -- they are actually asking you to do one of these,"
           " end your reply with exactly one tag: " + list +
           ". Say yes in your own words first, then the tag. Add no tag when they are"
           " only talking, or when you would rather not.";
}

std::string Intent_Extract(std::string& response)
{
    static const std::string open = "[do:";

    std::string command;

    for (;;)
    {
        size_t start = response.find(open);
        if (start == std::string::npos)
            break;

        size_t close = response.find(']', start);
        if (close == std::string::npos)
        {
            // Truncated tag. Drop the remainder so it cannot be spoken.
            response.erase(start);
            break;
        }

        const size_t tagStart = start + open.size();
        std::string tag = response.substr(tagStart, close - tagStart);
        response.erase(start, close - start + 1);

        // Normalise: models will write [do: Invite ].
        tag.erase(0, tag.find_first_not_of(" \t"));
        const size_t end = tag.find_last_not_of(" \t");
        tag = (end == std::string::npos) ? std::string() : tag.substr(0, end + 1);
        std::transform(tag.begin(), tag.end(), tag.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        // Strict whitelist. Anything invented is dropped, not guessed at: this
        // string is about to be handed to the command parser.
        if (command.empty())
            for (Intent const& intent : kIntents)
                if (tag == intent.tag)
                {
                    command = intent.command;
                    break;
                }
    }

    return command;
}

void Intent_Execute(Player* bot, Player* speaker, std::string const& command)
{
    if (!g_EnableChatIntents || command.empty() || !bot || !speaker)
        return;

    if (GET_PLAYERBOT_AI(speaker))
        return;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return;

    if (OnCooldown(bot, command))
    {
        Telemetry_NoteIntent(bot, speaker, command, false, "cooldown");

        if (g_DebugEnabled)
            LOG_INFO("module.ollamachat",
                     "[Ollama Chat] {} suppressed repeat intent '{}' for {}",
                     bot->GetName(), command, speaker->GetName());
        return;
    }

    // Straight to the command layer the typed commands use -- the PUBLIC
    // overload, which takes Player*; the Player& one is private. It re-checks
    // PlayerbotSecurity against this speaker, applies IsAllowedCommand, and
    // queues a ChatCommandHolder for the world tick to run, so refusals and
    // their messages are existing behaviour.
    //
    // CHAT_MSG_WHISPER matters: the security check is stricter for non-whisper
    // sources, and a whisper is the closest match to "this person asked me
    // directly", which is the only case that gets here.
    botAI->HandleCommand(CHAT_MSG_WHISPER, command, speaker);

    // "executed" means handed over, not that it succeeded: HandleCommand
    // applies its own security check and may still refuse. The bot-buddy
    // outcome events and the bot's own refusal message cover what happened
    // next; this records that the intent got that far.
    Telemetry_NoteIntent(bot, speaker, command, true, nullptr);

    if (g_DebugEnabled)
        LOG_INFO("module.ollamachat", "[Ollama Chat] {} acting on '{}' from {}",
                 bot->GetName(), command, speaker->GetName());
}
