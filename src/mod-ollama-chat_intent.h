#ifndef MOD_OLLAMA_CHAT_INTENT_H
#define MOD_OLLAMA_CHAT_INTENT_H

#include <string>

class Player;

// Turning what a player says into something the bot does.
//
// mod-playerbots already has ~120 chat commands -- invite, trade, sell, roll,
// join lfg -- but they only fire on the exact command word. "invite" works;
// "I'm down to join a party" does nothing. Meanwhile mod-ollama-chat replies in
// natural language and never acts. The two halves were mutually exclusive by
// design: a message starting with a command word skips the LLM entirely
// (g_BlacklistCommands), and everything else skips the command layer.
//
// This is the bridge, and it is deliberately thin:
//
//   * The LLM decides only WHAT WAS ASKED. That is open-ended language
//     understanding, which is the one thing it is better at than code.
//   * Whether the bot is willing is NOT an LLM decision. PlayerbotSecurity
//     already answers it -- DENY_ALL / TALK / INVITE / ALLOW_ALL with a
//     DenyReason for level, gearscore, faction, full group, being in a BG.
//   * Doing it is not new code either. The canonical command string goes to
//     PlayerbotAI::HandleCommand, the same entry point a typed command uses,
//     so the security check, the queueing onto the world tick and the bot's
//     own refusal messages are all existing behaviour.
//
// Costs no extra inference: the tag rides along in the reply the bot was
// generating anyway, in the same way [emote:name] already does, which also
// means the words and the action cannot contradict each other -- a bot that
// says "sure, inviting you" is the one that sends the invite.

// The instruction appended to the chat prompt. Empty when the speaker is not a
// real player, or when nothing in the whitelist applies. World thread.
std::string Intent_BuildPrompt(Player* bot, Player* speaker);

// Pull a [do:...] tag out of a reply, strip it, and return the canonical
// command it maps to, or "" for none. Runs on a dispatcher worker, so it reads
// no config strings and touches no world state.
std::string Intent_Extract(std::string& response);

// Run the command as though the speaker had typed it. World thread only.
void Intent_Execute(Player* bot, Player* speaker, std::string const& command);

#endif // MOD_OLLAMA_CHAT_INTENT_H
