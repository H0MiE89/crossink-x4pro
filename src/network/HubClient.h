#pragma once
#include <cstdint>
#include <string>
#include <vector>

/**
 * Client for xthub, a small service the owner runs on their own network that
 * fronts Home Assistant and Notion.
 *
 * The reader never speaks to those services directly. It has no memory
 * protection and a removable SD card, so it should not hold their credentials,
 * and it cannot afford to parse Notion's block JSON. The bridge holds the
 * secrets, flattens everything, and answers in a few kilobytes.
 *
 * Every call is synchronous and expects Wi-Fi to already be up. Screens fetch
 * once when opened so the radio is never running while reading.
 */

struct HubTile {
  std::string id;
  std::string name;
  int8_t state;  // 1 on, 0 off, -1 unknown or unavailable
};

struct HubTodo {
  std::string id;
  std::string name;
  std::string due;  // "YYYY-MM-DD", empty when the item has no date
};

struct HubEvent {
  std::string id;
  std::string name;
  std::string start;  // "YYYY-MM-DD" when allDay, otherwise an ISO instant
  bool allDay;
};

struct HubNote {
  std::string id;
  std::string name;
};

struct HubBlock {
  enum Kind : uint8_t { Paragraph, Heading, ListItem, Todo, Quote, Code, Rule };
  Kind kind;
  std::string text;
  int8_t checked;  // 1 or 0 for a Todo block, -1 otherwise
};

class HubClient {
 public:
  enum Error : uint8_t {
    OK = 0,
    NOT_CONFIGURED,  // no bridge address saved in settings
    NETWORK_ERROR,   // bridge unreachable or the reply was truncated
    AUTH_FAILED,     // the saved token was rejected
    NOT_FOUND,       // the bridge has no such screen configured
    SERVER_ERROR,    // the bridge reached Home Assistant or Notion and failed
    JSON_ERROR
  };

  static bool isConfigured();
  // The transport is a plain WiFiClient, so an https address would speak
  // cleartext at a TLS listener and fail with a misleading message.
  static bool isPlainHttp();

  // `message` receives a short sentence fit to show on screen. On OK it is the
  // bridge's own wording where there is one, otherwise it is left untouched.
  static Error fetchTiles(std::vector<HubTile>& out, std::string& message);
  static Error toggleTile(const std::string& id, HubTile& out, std::string& message);
  static Error fetchTodo(std::vector<HubTodo>& out, std::string& message);
  static Error completeTodo(const std::string& id, std::string& message);
  static Error fetchCalendar(std::vector<HubEvent>& out, std::string& message);
  static Error fetchNotes(std::vector<HubNote>& out, std::string& message);

  // Notes arrive in chunks. Pass the `next` from the previous call as `from`;
  // `next` comes back negative at the end of the note.
  static Error fetchNote(const std::string& id, int from, std::string& title, std::vector<HubBlock>& blocks, int& next,
                         std::string& message);
};
