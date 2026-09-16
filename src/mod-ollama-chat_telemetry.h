#ifndef MOD_OLLAMA_CHAT_TELEMETRY_H
#define MOD_OLLAMA_CHAT_TELEMETRY_H

#include <cstdint>
#include <string>

class Player;

// Conversation telemetry: what was said, what the bot understood by it, and
// what it did about it.
//
// Companion to mod-ollama-bot-buddy's telemetry, which covers decisions. They
// are separate modules with separate release cycles, so they write separate
// files into the same directory rather than sharing a writer -- a runtime
// dependency between two independently-versioned modules would be worse than
// a hundred duplicated lines. tools/telemetry.py merges them by timestamp.
//
// Only conversations involving a real player are recorded. Bots talk among
// themselves constantly and none of it is what anyone wants to revisit.
//
// Same two rules as the other writer: the world thread never touches the disk,
// and a full queue drops events rather than stalling the server.

void ChatTelemetry_Start();
void ChatTelemetry_Stop();
bool ChatTelemetry_Enabled();

// A message arrived. Returns a correlation id to pass to the events it causes,
// or 0 when this conversation is not being recorded.
uint64_t ChatTelemetry_NoteIncoming(Player* bot, Player* speaker,
                                std::string const& text, char const* source,
                                std::string const& scopeKey);

// The bot answered. `intent` is the command the reply asked for, or "".
void ChatTelemetry_NoteReply(uint64_t turn, std::string const& botName,
                         std::string const& text, std::string const& intent,
                         uint32_t emoteId, int64_t latencyMs, size_t promptChars);

// An intent reached the command layer, or was refused before it got there.
void ChatTelemetry_NoteIntent(Player* bot, Player* speaker,
                          std::string const& command, bool executed,
                          char const* refusedBecause);

#endif // MOD_OLLAMA_CHAT_TELEMETRY_H
