#include "mod-ollama-chat_transcript.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat-utilities.h"

#include "Player.h"

#include <fmt/core.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace
{
    using Clock     = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    // WoW's own chat limit is 255 bytes. Storing more than this would only be
    // storing something no client could have sent.
    constexpr size_t kMaxStoredTextBytes = 255;

    struct Line
    {
        std::string speaker;
        std::string text;
        uint32_t    mapId        = 0;
        float       x            = 0.0f;
        float       y            = 0.0f;
        TimePoint   at{};
    };

    struct Room
    {
        std::deque<Line> lines;
        TimePoint        lastTouched{};
    };

    std::mutex                                  g_mutex;
    std::unordered_map<std::string, Room>       g_rooms;

    // Say and yell are keyed by zone, because that is how the governor keys
    // them. Two unrelated conversations in the same zone therefore share a
    // room, so for these sources a line is only shown to a bot that was close
    // enough to hear it. Without this a bot in Goldshire would answer a
    // conversation held in Northshire.
    bool IsLocalSource(ChatChannelSourceLocal source)
    {
        return source == SRC_SAY_LOCAL || source == SRC_YELL_LOCAL;
    }

    float AudibleRange(ChatChannelSourceLocal source)
    {
        return source == SRC_YELL_LOCAL ? g_YellDistance : g_SayDistance;
    }

    TimePoint g_lastPrune{};

    // Called with the lock held. Sweeping every room costs O(rooms), so it
    // runs on a timer rather than on every message: on a busy realm this path
    // is the world thread, and chat is not the thing that should be walking a
    // map per line.
    void PruneLocked(const TimePoint& now)
    {
        if (g_lastPrune != TimePoint{} &&
            now - g_lastPrune < std::chrono::seconds(30))
            return;

        g_lastPrune = now;

        const auto maxAge = std::chrono::seconds(g_RoomHistoryMaxAgeSeconds);

        for (auto it = g_rooms.begin(); it != g_rooms.end(); )
        {
            Room& room = it->second;

            while (!room.lines.empty() && now - room.lines.front().at > maxAge)
                room.lines.pop_front();

            if (room.lines.empty())
                it = g_rooms.erase(it);
            else
                ++it;
        }

        // A party scope key embeds a group counter that only ever rises, so
        // the number of distinct rooms is unbounded over a realm's lifetime
        // even though each one is small. Evict least-recently-used.
        // A configured 0 would erase every room on every sweep -- including
        // the one the caller just wrote to and still holds a reference to.
        const size_t maxRooms = std::max<uint32_t>(g_RoomHistoryMaxRooms, 1);

        while (g_rooms.size() > maxRooms)
        {
            auto oldest = g_rooms.begin();
            for (auto it = std::next(g_rooms.begin()); it != g_rooms.end(); ++it)
                if (it->second.lastTouched < oldest->second.lastTouched)
                    oldest = it;

            g_rooms.erase(oldest);
        }
    }
}

void Transcript_Note(const std::string& scopeKey, ChatChannelSourceLocal source,
                     const std::string& speaker, const std::string& text,
                     uint32_t mapId, float x, float y)
{
    if (!g_EnableRoomHistory || g_RoomHistoryMaxLines == 0)
        return;

    // See the header: whisper scope keys are shared by every private
    // conversation in a zone.
    if (source == SRC_WHISPER_LOCAL || scopeKey.empty())
        return;

    if (speaker.empty() ||
        text.find_first_not_of(" \t\r\n") == std::string::npos)
        return;

    const TimePoint now = Clock::now();

    Line line;
    line.speaker      = speaker;
    line.text         = text.size() > kMaxStoredTextBytes
                            ? text.substr(0, kMaxStoredTextBytes) : text;
    line.mapId        = mapId;
    line.x            = x;
    line.y            = y;
    line.at           = now;

    std::lock_guard<std::mutex> lock(g_mutex);

    {
        Room& room = g_rooms[scopeKey];

        // Exactly-once, cheaply. A delivered bot line is recorded by the
        // dispatcher and then usually arrives here a second time, because
        // ProcessBotChatMessage feeds it back through ProcessChat -- but not
        // always: that function bails on chain depth and on eligibility before
        // it gets there. Rather than trying to predict which lines make the
        // trip, both paths record and the duplicate is dropped here.
        //
        // The cost of being wrong is one lost line in the rare case where a
        // speaker repeats itself verbatim back to back, which the governor's
        // repetition brake already suppresses.
        if (!room.lines.empty() &&
            room.lines.back().speaker == speaker &&
            room.lines.back().text == line.text)
        {
            room.lastTouched = now;
            return;
        }

        room.lastTouched = now;
        room.lines.push_back(std::move(line));

        while (room.lines.size() > g_RoomHistoryMaxLines)
            room.lines.pop_front();
    }

    // `room` is deliberately out of scope here: PruneLocked erases from the
    // map it points into.
    PruneLocked(now);
}

std::string Transcript_BuildPrompt(Player* bot, const std::string& scopeKey,
                                   ChatChannelSourceLocal source,
                                   const std::string& playerName,
                                   const std::string& playerMessage)
{
    if (!g_EnableRoomHistory || !bot || scopeKey.empty())
        return "";

    if (source == SRC_WHISPER_LOCAL)
        return "";

    const bool  local = IsLocalSource(source);
    const float range = AudibleRange(source);

    std::vector<Line> audible;
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        auto it = g_rooms.find(scopeKey);
        if (it == g_rooms.end())
            return "";

        const auto maxAge = std::chrono::seconds(g_RoomHistoryMaxAgeSeconds);
        const auto now    = Clock::now();

        for (const Line& line : it->second.lines)
        {
            if (now - line.at > maxAge)
                continue;

            if (local && (line.mapId != bot->GetMapId() ||
                          bot->GetExactDist2d(line.x, line.y) > range))
                continue;

            audible.push_back(line);
        }
    }

    if (audible.empty())
        return "";

    // Oldest first, trimmed from the front to a character budget so a busy
    // channel cannot inflate every prompt (and with it every bot's inference
    // latency) without limit.
    size_t budget = 0;
    size_t first  = audible.size();
    while (first > 0)
    {
        const Line& line = audible[first - 1];
        const size_t cost = line.speaker.size() + line.text.size() + 4;
        if (budget + cost > g_RoomHistoryMaxChars)
            break;
        budget += cost;
        --first;
    }

    if (first == audible.size())   // even the newest line does not fit
        return "";

    const std::string botName = bot->GetName();

    std::string result = SafeFormat(g_RoomHistoryHeaderTemplate,
                                    fmt::arg("bot_name", botName));

    for (size_t i = first; i < audible.size(); ++i)
    {
        const Line& line = audible[i];

        // The speaker name and the text are passed as *arguments*, never as a
        // format string. A player typing "{bot_name}" must not be formatted.
        result += SafeFormat(g_RoomHistoryLineTemplate,
                             fmt::arg("speaker", line.speaker == botName
                                                     ? std::string("You")
                                                     : line.speaker),
                             fmt::arg("text", line.text));
    }

    // Reuse the existing footer so the "this is the line to answer" framing is
    // identical to the pairwise block this replaces.
    result += SafeFormat(g_ChatHistoryFooterTemplate,
                         fmt::arg("player_name", playerName),
                         fmt::arg("player_message", playerMessage));

    return result;
}

size_t Transcript_RoomCount()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_rooms.size();
}

size_t Transcript_LineCount()
{
    std::lock_guard<std::mutex> lock(g_mutex);

    size_t total = 0;
    for (const auto& [key, room] : g_rooms)
        total += room.lines.size();

    return total;
}
