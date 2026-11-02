// Winner stays in the LAN Xbox LIVE Party Lobby (with the SDK's lan_live).
//
// The game's Party Lobby host always fights: the first player to join its
// lobby is the opponent, the others watch, and after the match only the host
// can open the next lobby. So the host role moves instead: when a match ends,
// every console reports who won (game::MatchWinner, from its own side,
// OnlineInput::local_side) to the SDK, which says what this console does next
// (lan_sessions::PartyRole), and this file walks the menus for it by pressing
// buttons on the active controller:
//   - the host won: Return to lobby; its new lobby invites the next challenger
//   - the host lost: Return to lobby, then Back out of it to the Player Match
//     menu, where it takes invites like everyone else
//   - the opponent won: Return to Xbox LIVE VS. Play menu, Xbox LIVE Party
//     Lobby, the old host's settings (announced by the SDK), OK: its lobby
//     invites the party
//   - the opponent lost: Return to Xbox LIVE VS. Play menu, to be invited again
// Observers go back to the menu by themselves. The SDK picks each challenger:
// whoever fought longest ago, so the loser queues behind the ones waiting.
//
// Every step reads the menu screen (game::kMenuScreen) and the cursors first,
// and presses again until the screen changes, so a press lost to a transition
// or a dialog is made again.

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/lan_sessions.h>

#include "game_memory.h"
#include "tick_hooks.h"

#include <cstdint>
#include <string>

REXCVAR_DEFINE_STRING(party_rotation, "winner", "Oratan/Netplay",
                      "LAN Xbox LIVE Party Lobby, after each match: winner (the winner stays "
                      "and hosts the next match, the loser queues), host (the host always "
                      "fights) or off (everyone leaves the results by hand)");

namespace oratan::party_rotation {
namespace {

namespace lan = rex::system::lan_sessions;
using namespace game;

enum class Goal {
  kNone,
  kBackToLobby,  // host stays: Return to lobby
  kLeaveLobby,   // host steps down: Return to lobby, Back to the Player Match menu
  kToMenu,       // opponent lost: Return to Xbox LIVE VS. Play menu
  kHostLobby,    // opponent won: to the menu, then a Party Lobby of its own
};

constexpr uint32_t kHoldTicks = 4;           // a press: held this long, then released
constexpr uint32_t kResultsDelay = 6 * 60;   // let the players see the results first
constexpr uint32_t kSettle = 30;             // after a screen change, before pressing
constexpr uint32_t kBetweenPresses = 20;
constexpr uint32_t kAfterConfirm = 60;       // after A: the screen may still be changing
constexpr uint32_t kGiveUpAfter = 45 * 60;

bool g_fighting = false;  // a party fight is under way; its end not yet reported
Goal g_goal = Goal::kNone;
uint32_t g_goal_tick = 0;
uint32_t g_screen = 0;
uint32_t g_screen_tick = 0;
uint32_t g_next_press = 0;
uint8_t g_rules[lan::kPartyRulesSize] = {};
bool g_have_rules = false;
uint32_t g_rules_written_tick = 0;  // 0 = not yet on this settings screen

// The press being made (OnControllerRead applies it).
uint32_t g_button = 0;
uint32_t g_press_phase = 0;

const char* GoalName(Goal goal) {
  switch (goal) {
    case Goal::kBackToLobby: return "back to the lobby";
    case Goal::kLeaveLobby: return "leaving the host role";
    case Goal::kToMenu: return "back to the menu";
    case Goal::kHostLobby: return "hosting the next match";
    default: return "nothing";
  }
}

uint32_t Load32(const uint8_t* base, uint32_t address) {
  return *GuestPtr<be<uint32_t>>(base, address);
}

void Press(uint32_t button, uint32_t wait_after) {
  REXLOG_INFO("party rotation: pressing {:04X} on menu screen {}", button, g_screen);
  g_button = button;
  g_press_phase = 0;
  g_next_press = GameTick() + kHoldTicks + wait_after;
}

void Finish(const char* how) {
  REXLOG_INFO("party rotation: {} ({})", how, GoalName(g_goal));
  g_goal = Goal::kNone;
}

// The match ended: ask the SDK what this console does next.
void MatchOver(uint8_t* base, const Game& g) {
  const std::string mode = REXCVAR_GET(party_rotation);
  if (mode == "off") return;
  const int side = GuestPtr<OnlineInput>(base, kOnlineInput)->local_side;
  const int winner = MatchWinner(g);
  const lan::PartyRole role = lan::PartyMatchOver(side, mode == "winner" ? winner : 0);
  switch (role) {
    case lan::PartyRole::kHost: g_goal = Goal::kBackToLobby; break;
    case lan::PartyRole::kStepDown: g_goal = Goal::kLeaveLobby; break;
    case lan::PartyRole::kRejoin: g_goal = Goal::kToMenu; break;
    case lan::PartyRole::kBecomeHost:
      g_goal = Goal::kHostLobby;
      g_have_rules = lan::LastHostRules(g_rules);
      if (!g_have_rules) REXLOG_WARN("party rotation: the last host's settings aren't known");
      break;
    default: return;
  }
  g_goal_tick = GameTick();
  g_next_press = GameTick() + kResultsDelay;
  g_rules_written_tick = 0;
  REXLOG_INFO("party rotation: side {} won, this console is side {}: {}", winner + 1, side + 1,
              GoalName(g_goal));
}

// Out of the results: the host's menu opens on Return to lobby, an opponent's
// is moved to Return to Xbox LIVE VS. Play menu.
void LeaveResults(const uint8_t* base, bool host) {
  if (host || Load32(base, kResultsCursor) == kResultsReturnToMenu) {
    Press(pad::kA, kAfterConfirm);
  } else {
    Press(pad::kDown, kBetweenPresses);
  }
}

// On the Party Lobby settings: the old host's values, then OK.
void ConfirmRules(uint8_t* base) {
  auto* rules = GuestPtr<PartyRules>(base, kPartyRules);
  const uint32_t now = GameTick();
  if (!g_rules_written_tick) {
    if (g_have_rules) {
      rules->time_limit = g_rules[0];
      rules->round_count = g_rules[1];
      rules->observers = g_rules[2];
      REXLOG_INFO("party rotation: settings time {} rounds {} observers {}", g_rules[0],
                  g_rules[1] + 1, g_rules[2]);
    }
    g_rules_written_tick = now;
    g_next_press = now + 10;
    return;
  }
  // The cursor opens on OK; anywhere else, Down reaches it.
  Press(rules->help == 0 ? pad::kA : pad::kDown, rules->help == 0 ? kAfterConfirm : kBetweenPresses);
}

void Drive(uint8_t* base) {
  const uint32_t now = GameTick();
  if (now - g_goal_tick > kGiveUpAfter) {
    Finish("gave up: the menus didn't get there");
    return;
  }
  if (g_button || now < g_next_press || now - g_screen_tick < kSettle) return;
  switch (g_screen) {
    case menu_screen::kOnlineMatch:
      LeaveResults(base, g_goal == Goal::kBackToLobby || g_goal == Goal::kLeaveLobby);
      return;
    case menu_screen::kLobby:
      if (g_goal == Goal::kLeaveLobby) {
        Press(pad::kB, kAfterConfirm);
      } else if (g_goal == Goal::kBackToLobby || g_goal == Goal::kHostLobby) {
        Finish("done");
      }
      return;
    case menu_screen::kPartyRules:
      if (g_goal == Goal::kLeaveLobby) {
        Press(pad::kB, kAfterConfirm);
      } else if (g_goal == Goal::kHostLobby) {
        ConfirmRules(base);
      }
      return;
    case menu_screen::kPlayerMatch:
      if (g_goal != Goal::kHostLobby) {
        if (g_goal == Goal::kLeaveLobby || g_goal == Goal::kToMenu) Finish("done");
        return;
      }
      {
        const uint32_t cursor = Load32(base, kPlayerMatchCursor);
        if (cursor == 2) {
          g_rules_written_tick = 0;
          Press(pad::kA, kAfterConfirm);
        } else {
          Press(cursor < 2 ? pad::kDown : pad::kUp, kBetweenPresses);
        }
      }
      return;
    case menu_screen::kSelectVr:
      Press(pad::kA, kAfterConfirm);  // keep the robot
      return;
    default:
      return;  // on the way somewhere: wait
  }
}

}  // namespace

void OnTickEnd(uint8_t* base) {
  const uint32_t screen = Load32(base, kMenuScreen);
  if (screen != g_screen) {
    g_screen = screen;
    g_screen_tick = GameTick();
  }
  // The settings of a lobby this console set up, for whoever hosts after it.
  if (screen == menu_screen::kLobby || screen == menu_screen::kPartyRules) {
    const auto* rules = GuestPtr<PartyRules>(base, kPartyRules);
    const uint8_t values[lan::kPartyRulesSize] = {uint8_t(rules->time_limit),
                                                  uint8_t(rules->round_count),
                                                  uint8_t(rules->observers), 0};
    lan::SetPartyRules(values);
  }

  const Game* g = GetGame(base);
  const bool online = *GuestPtr<be<int32_t>>(base, kPlayMode) == 3;
  if (g && online && IsBattleModule(CurrentModule(base)) && g->scene == scene_id::kBattle) {
    if (g->scene_step == battle_step::kFight) {
      g_fighting = true;
    } else if (g_fighting && g->scene_step >= battle_step::kDecided && MatchWinner(*g) >= 0) {
      g_fighting = false;
      MatchOver(base, *g);
    }
  }
  if (g_goal != Goal::kNone) Drive(base);
}

void OnControllerRead(uint8_t* base) {
  if (!g_button) return;
  uint32_t controller = Load32(base, kActiveController);
  if (controller > 3) controller = 0;
  auto& state = GuestPtr<ControllerState>(base, kControllerStates)[controller];
  // Each field written outright: a read that skips an unchanged packet leaves
  // the last tick's values in place.
  // Menu cursors move on `repeated`, which also starts with the press.
  const uint32_t bit = g_button;
  const bool first = g_press_phase == 0;
  if (g_press_phase < kHoldTicks) {
    state.held = state.held | bit;
    state.pressed = first ? (state.pressed | bit) : (state.pressed & ~bit);
    state.repeated = first ? (state.repeated | bit) : (state.repeated & ~bit);
    state.released = state.released & ~bit;
  } else {
    state.held = state.held & ~bit;
    state.pressed = state.pressed & ~bit;
    state.repeated = state.repeated & ~bit;
    state.released = g_press_phase == kHoldTicks ? (state.released | bit) : (state.released & ~bit);
  }
  if (++g_press_phase > kHoldTicks + 1) g_button = 0;
}

}  // namespace oratan::party_rotation
