#include "HubClient.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Logging.h>
#include <WiFi.h>
#include <WiFiClient.h>

#include <cstdio>

#include "CrossPointSettings.h"

namespace {
constexpr int TIMEOUT_MS = 8000;

// The bridge answers in a few kilobytes. A larger body means something is
// wrong on the far end, and parsing it would cost heap the reader needs.
constexpr int MAX_BODY_BYTES = 24 * 1024;

constexpr size_t MAX_TILES = 40;
constexpr size_t MAX_TODO = 40;
constexpr size_t MAX_EVENTS = 40;
constexpr size_t MAX_NOTES = 25;
constexpr size_t MAX_BLOCKS = 60;

std::string baseUrl() {
  std::string url = SETTINGS.hubUrl;
  while (!url.empty() && url.back() == '/') url.pop_back();
  if (!url.empty() && url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
    url.insert(0, "http://");
  }
  return url;
}

/** Percent-encodes a path segment so an entity id or Notion id survives. */
std::string escapeSegment(const std::string& raw) {
  std::string out;
  out.reserve(raw.size() + 8);
  for (const unsigned char c : raw) {
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                            c == '_' || c == '.' || c == '~';
    if (unreserved) {
      out.push_back(static_cast<char>(c));
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", c);
      out.append(buf);
    }
  }
  return out;
}

HubClient::Error statusToError(const int status) {
  if (status == 401 || status == 403) return HubClient::AUTH_FAILED;
  if (status == 404) return HubClient::NOT_FOUND;
  if (status > 0) return HubClient::SERVER_ERROR;
  return HubClient::NETWORK_ERROR;
}

/**
 * Runs one request and hands back the parsed body. Errors from the bridge
 * carry a short sentence in `e`, which is what the screen shows, so a
 * misconfigured Home Assistant reads as itself rather than as a status code.
 */
HubClient::Error request(const char* method, const std::string& path, JsonDocument& doc, std::string& message) {
  if (!HubClient::isConfigured()) return HubClient::NOT_CONFIGURED;
  if (WiFi.status() != WL_CONNECTED) return HubClient::NETWORK_ERROR;

  const std::string url = baseUrl() + path;

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(TIMEOUT_MS);
  http.setConnectTimeout(TIMEOUT_MS);
  if (!http.begin(client, url.c_str())) {
    LOG_ERR("Hub", "begin failed for %s", url.c_str());
    return HubClient::NETWORK_ERROR;
  }

  const std::string auth = std::string("Bearer ") + SETTINGS.hubToken;
  http.addHeader("Authorization", auth.c_str());
  http.addHeader("Accept", "application/json");

  const int status = http.sendRequest(method, static_cast<uint8_t*>(nullptr), 0);
  if (status <= 0) {
    LOG_ERR("Hub", "%s %s failed: %s", method, path.c_str(), HTTPClient::errorToString(status).c_str());
    http.end();
    return HubClient::NETWORK_ERROR;
  }

  const int length = http.getSize();
  if (length > MAX_BODY_BYTES) {
    LOG_ERR("Hub", "%s returned %d bytes, over the %d cap", path.c_str(), length, MAX_BODY_BYTES);
    http.end();
    return HubClient::SERVER_ERROR;
  }

  const String body = http.getString();
  http.end();

  const DeserializationError parseError = deserializeJson(doc, body.c_str());
  if (parseError) {
    LOG_ERR("Hub", "%s parse failed: %s", path.c_str(), parseError.c_str());
    return status == 200 ? HubClient::JSON_ERROR : statusToError(status);
  }

  if (doc["e"].is<const char*>()) message = doc["e"].as<const char*>();

  if (status != 200) return statusToError(status);
  return HubClient::OK;
}
}  // namespace

bool HubClient::isConfigured() { return SETTINGS.hubUrl[0] != '\0' && SETTINGS.hubToken[0] != '\0'; }

HubClient::Error HubClient::fetchTiles(std::vector<HubTile>& out, std::string& message) {
  JsonDocument doc;
  const Error error = request("GET", "/v1/tiles", doc, message);
  if (error != OK) return error;

  out.clear();
  for (JsonObjectConst row : doc["tiles"].as<JsonArrayConst>()) {
    if (out.size() >= MAX_TILES) break;
    HubTile tile;
    tile.id = row["i"].as<const char*>() ? row["i"].as<const char*>() : "";
    tile.name = row["n"].as<const char*>() ? row["n"].as<const char*>() : tile.id;
    tile.state = static_cast<int8_t>(row["s"] | -1);
    if (!tile.id.empty()) out.push_back(std::move(tile));
  }
  return OK;
}

HubClient::Error HubClient::toggleTile(const std::string& id, HubTile& out, std::string& message) {
  JsonDocument doc;
  const std::string path = "/v1/tiles/" + escapeSegment(id) + "/toggle";
  const Error error = request("POST", path, doc, message);
  if (error != OK) return error;

  out.id = doc["i"].as<const char*>() ? doc["i"].as<const char*>() : id;
  out.name = doc["n"].as<const char*>() ? doc["n"].as<const char*>() : out.id;
  out.state = static_cast<int8_t>(doc["s"] | -1);
  return OK;
}

HubClient::Error HubClient::fetchTodo(std::vector<HubTodo>& out, std::string& message) {
  JsonDocument doc;
  const Error error = request("GET", "/v1/todo", doc, message);
  if (error != OK) return error;

  out.clear();
  for (JsonObjectConst row : doc["items"].as<JsonArrayConst>()) {
    if (out.size() >= MAX_TODO) break;
    HubTodo item;
    item.id = row["i"].as<const char*>() ? row["i"].as<const char*>() : "";
    item.name = row["n"].as<const char*>() ? row["n"].as<const char*>() : "";
    if (row["d"].is<const char*>()) item.due = row["d"].as<const char*>();
    if (!item.id.empty() && !item.name.empty()) out.push_back(std::move(item));
  }
  return OK;
}

HubClient::Error HubClient::completeTodo(const std::string& id, std::string& message) {
  JsonDocument doc;
  const std::string path = "/v1/todo/" + escapeSegment(id) + "/done";
  return request("POST", path, doc, message);
}

HubClient::Error HubClient::fetchCalendar(std::vector<HubEvent>& out, std::string& message) {
  JsonDocument doc;
  const Error error = request("GET", "/v1/calendar", doc, message);
  if (error != OK) return error;

  out.clear();
  for (JsonObjectConst row : doc["events"].as<JsonArrayConst>()) {
    if (out.size() >= MAX_EVENTS) break;
    HubEvent event;
    event.id = row["i"].as<const char*>() ? row["i"].as<const char*>() : "";
    event.name = row["n"].as<const char*>() ? row["n"].as<const char*>() : "";
    event.start = row["s"].as<const char*>() ? row["s"].as<const char*>() : "";
    event.allDay = (row["a"] | 0) == 1;
    if (!event.name.empty()) out.push_back(std::move(event));
  }
  return OK;
}

HubClient::Error HubClient::fetchNotes(std::vector<HubNote>& out, std::string& message) {
  JsonDocument doc;
  const Error error = request("GET", "/v1/notes", doc, message);
  if (error != OK) return error;

  out.clear();
  for (JsonObjectConst row : doc["notes"].as<JsonArrayConst>()) {
    if (out.size() >= MAX_NOTES) break;
    HubNote note;
    note.id = row["i"].as<const char*>() ? row["i"].as<const char*>() : "";
    note.name = row["n"].as<const char*>() ? row["n"].as<const char*>() : "";
    if (!note.id.empty()) out.push_back(std::move(note));
  }
  return OK;
}

HubClient::Error HubClient::fetchNote(const std::string& id, const int from, std::string& title,
                                      std::vector<HubBlock>& blocks, int& next, std::string& message) {
  JsonDocument doc;
  char query[32];
  snprintf(query, sizeof(query), "?from=%d&max=2048", from < 0 ? 0 : from);
  const std::string path = "/v1/notes/" + escapeSegment(id) + query;
  const Error error = request("GET", path, doc, message);
  if (error != OK) return error;

  if (doc["n"].is<const char*>()) title = doc["n"].as<const char*>();
  // The bridge sends null at the end of a note; JSON has no negative sentinel.
  next = doc["next"].isNull() ? -1 : (doc["next"] | -1);

  blocks.clear();
  for (JsonObjectConst row : doc["blocks"].as<JsonArrayConst>()) {
    if (blocks.size() >= MAX_BLOCKS) break;
    const char* kind = row["t"].as<const char*>();
    if (kind == nullptr) continue;

    HubBlock block;
    block.checked = -1;
    if (strcmp(kind, "h1") == 0 || strcmp(kind, "h2") == 0 || strcmp(kind, "h3") == 0) {
      block.kind = HubBlock::Heading;
    } else if (strcmp(kind, "li") == 0) {
      block.kind = HubBlock::ListItem;
    } else if (strcmp(kind, "todo") == 0) {
      block.kind = HubBlock::Todo;
      block.checked = static_cast<int8_t>((row["c"] | 0) == 1 ? 1 : 0);
    } else if (strcmp(kind, "quote") == 0) {
      block.kind = HubBlock::Quote;
    } else if (strcmp(kind, "code") == 0) {
      block.kind = HubBlock::Code;
    } else if (strcmp(kind, "hr") == 0) {
      block.kind = HubBlock::Rule;
    } else {
      block.kind = HubBlock::Paragraph;
    }

    block.text = row["x"].as<const char*>() ? row["x"].as<const char*>() : "";
    if (block.kind == HubBlock::Rule || !block.text.empty()) blocks.push_back(std::move(block));
  }
  return OK;
}
