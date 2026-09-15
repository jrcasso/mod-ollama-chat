#ifndef MOD_OLLAMA_CHAT_TRANSCRIPT_H
#define MOD_OLLAMA_CHAT_TRANSCRIPT_H

#include "mod-ollama-chat_handler.h"

#include <string>

class Player;

// Room transcript: what was actually said in one conversation space, in order,
// by everyone in it.
//
// The pairwise history in handler.cpp answers a different question -- "what
// have this bot and this player said to each other" -- and that shape is wrong
// for a chat channel in two ways. A bot could not see what anyone *else* in
// the room said, and bot-to-bot lines were never recorded at all
// (`recordHistory = !senderIsBot`), so any conversation with more than two
// participants had no thread for the model to follow. Bots answered the single
// line that triggered them and nothing else.
//
// Keyed by the governor's scope key, so it partitions exactly the way
// cooldowns and repetition history already do -- one channel, one guild, one
// party (see Governor_MakeScopeKey).
//
// Three properties this deliberately has:
//
//   * It stores strings and coordinates, never a Player*, Channel* or Group*.
//     Nothing here can dangle when someone logs out mid-conversation.
//   * It is bounded three ways: lines per room, age, and number of rooms.
//     Party scope keys embed a group counter that rises forever, so an
//     unbounded map here would be a slow leak on a long-running realm.
//   * Whisper is excluded. Whisper scope keys are "Whisper#z<zone>", which is
//     shared by every private conversation in the zone -- recording them would
//     leak one player's whispers into another's prompt. Whisper is 1:1 and the
//     pairwise history already covers it.
//
// World thread only, like the rest of the prompt-building path.

// Record one line. Position is the speaker's, used to filter say and yell down
// to what was actually audible; pass the bot's or player's own coordinates.
void Transcript_Note(const std::string& scopeKey, ChatChannelSourceLocal source,
                     const std::string& speaker, const std::string& text,
                     uint32_t mapId, float x, float y);

// The block that goes in the prompt, or "" when the room has nothing this bot
// could have heard. Caller falls back to the pairwise history in that case.
std::string Transcript_BuildPrompt(Player* bot, const std::string& scopeKey,
                                   ChatChannelSourceLocal source,
                                   const std::string& playerName,
                                   const std::string& playerMessage);

// Diagnostics for `.ollama status`.
size_t Transcript_RoomCount();
size_t Transcript_LineCount();

#endif // MOD_OLLAMA_CHAT_TRANSCRIPT_H
