#include "mod-ollama-chat_telemetry.h"
#include "mod-ollama-chat_config.h"

#include "Log.h"
#include "Player.h"
#include "Playerbots.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

namespace
{
    constexpr size_t kMaxQueued = 20000;

    std::mutex              g_mutex;
    std::condition_variable g_cv;
    std::deque<std::string> g_queue;

    std::thread       g_writer;
    std::atomic<bool> g_running{false};

    std::atomic<uint64_t> g_dropped{0};
    std::atomic<uint64_t> g_nextTurn{1};

    int64_t NowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
    }

    void Emit(nlohmann::json ev)
    {
        if (!g_running)
            return;

        ev["t"] = NowMs();
        std::string line = ev.dump();

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_queue.size() >= kMaxQueued)
            {
                ++g_dropped;
                return;
            }
            g_queue.push_back(std::move(line));
        }

        g_cv.notify_one();
    }

    void WriterLoop(std::string path)
    {
        std::ofstream out(path, std::ios::app);
        if (!out)
        {
            LOG_ERROR("module.ollamachat", "[Ollama Chat] telemetry: cannot open {}", path);
            g_running = false;
            return;
        }

        for (;;)
        {
            std::deque<std::string> batch;
            {
                std::unique_lock<std::mutex> lock(g_mutex);
                g_cv.wait_for(lock, std::chrono::milliseconds(500),
                              [] { return !g_queue.empty() || !g_running; });

                if (g_queue.empty() && !g_running)
                    break;

                batch.swap(g_queue);
            }

            for (std::string const& line : batch)
                out << line << '\n';

            out.flush();
        }

        out.flush();
    }

    // Recorded only when a real person is on one end of it.
    //
    // Named apart from mod-playerbots' global IsRealPlayer(Player*) in
    // PlayerbotAI.h, which this collided with.
    bool TelemetryIsRealPlayer(Player* p)
    {
        return p && !GET_PLAYERBOT_AI(p);
    }
}

void ChatTelemetry_Start()
{
    if (!g_EnableChatTelemetry || g_running)
        return;

    std::string dir = g_ChatTelemetryDir;
    if (dir.empty())
        dir = "/azerothcore/env/dist/logs/telemetry";
    // Create it rather than assuming it. A missing directory would otherwise
    // disable telemetry with only a log line to say so, which is exactly the
    // silent failure this whole system exists to stop.
    {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec)
            LOG_ERROR("server.loading",
                      "[Ollama Chat] telemetry: cannot create {}: {}", dir, ec.message());
    }

    if (dir.back() != '/')
        dir += '/';

    std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);

    std::string const path = dir + "chat-" + stamp + ".jsonl";

    g_running = true;
    g_writer = std::thread(WriterLoop, path);

    LOG_INFO("server.loading", "[Ollama Chat] telemetry -> {}", path);
}

void ChatTelemetry_Stop()
{
    if (!g_running)
        return;

    Emit({{"kind", "session_end"}, {"dropped", g_dropped.load()}});

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_running = false;
    }
    g_cv.notify_all();

    if (g_writer.joinable())
        g_writer.join();
}

bool ChatTelemetry_Enabled()
{
    return g_EnableChatTelemetry && g_running;
}

uint64_t ChatTelemetry_NoteIncoming(Player* bot, Player* speaker,
                                std::string const& text, char const* source,
                                std::string const& scopeKey)
{
    if (!g_running || !bot || !speaker)
        return 0;

    // Bot-to-bot chatter is most of the traffic and none of the interest.
    if (!TelemetryIsRealPlayer(speaker))
        return 0;

    uint64_t const turn = g_nextTurn++;

    Emit({
        {"kind",    "said_to_bot"},
        {"turn",    turn},
        {"bot",     bot->GetName()},
        {"player",  speaker->GetName()},
        {"source",  source},
        {"scope",   scopeKey},
        {"text",    text},
        {"map",     bot->GetMapId()},
        {"pos",     { bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ() }},
        {"distance", speaker->GetDistance(bot)},
    });

    return turn;
}

void ChatTelemetry_NoteReply(uint64_t turn, std::string const& botName,
                         std::string const& text, std::string const& intent,
                         uint32_t emoteId, int64_t latencyMs, size_t promptChars)
{
    if (!g_running || !turn)
        return;

    Emit({
        {"kind",         "bot_said"},
        {"turn",         turn},
        {"bot",          botName},
        {"text",         text},
        {"intent",       intent},
        {"emote",        emoteId},
        {"latency_ms",   latencyMs},
        {"prompt_chars", promptChars},
    });
}

void ChatTelemetry_NoteIntent(Player* bot, Player* speaker,
                          std::string const& command, bool executed,
                          char const* refusedBecause)
{
    if (!g_running || !bot || !speaker)
        return;

    Emit({
        {"kind",     "intent"},
        {"bot",      bot->GetName()},
        {"player",   speaker->GetName()},
        {"command",  command},
        {"executed", executed},
        {"refused",  refusedBecause ? refusedBecause : ""},
    });
}
