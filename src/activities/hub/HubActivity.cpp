#include "AppCapabilities.h"
#if CROSSINK_APP_CAP_HUB
#include "HubActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "network/WifiUtils.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_ROW = 1;

constexpr StrId MENU_LABELS[] = {StrId::STR_HUB_LIGHTS, StrId::STR_HUB_TASKS, StrId::STR_HUB_CALENDAR,
                                 StrId::STR_HUB_NOTES};
constexpr UIIcon MENU_ICONS[] = {UIIcon::Hotspot, UIIcon::BookmarkIcon, UIIcon::Recent, UIIcon::Text};


void wifiOff() {
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

/**
 * Turns a bridge timestamp into something short enough for a list subtitle.
 * The strings are ISO already, so this slices rather than parses: a date-only
 * value stays a date, and an instant keeps its day and hour.
 */
std::string formatWhen(const std::string& iso, const bool allDay) {
  if (iso.size() < 10) return iso;
  std::string out = iso.substr(0, 10);
  if (allDay) {
    out += "  ";
    out += tr(STR_HUB_ALL_DAY);
    return out;
  }
  if (iso.size() >= 16 && iso[10] == 'T') {
    out += "  ";
    out += iso.substr(11, 5);
  }
  return out;
}

const char* bulletFor(const HubBlock& block) {
  switch (block.kind) {
    case HubBlock::ListItem:
      return "- ";
    case HubBlock::Todo:
      return block.checked == 1 ? "[x] " : "[ ] ";
    case HubBlock::Quote:
      return "| ";
    default:
      return "";
  }
}
}  // namespace

HubActivity::HubActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity(NAME, renderer, mappedInput), ui(renderer) {}

void HubActivity::onEnter() {
  Activity::onEnter();

  page = Page::Menu;
  selectedIndex = 0;
  topIndex = 0;
  visibleRows = 1;
  statusMessage.clear();
  statusIsError = false;

  ui.closeRouting();
  listNav.reset(0);
  ui.reset();
  ui.app.on(ACTION_ROW, &HubActivity::onRowEvent, this);
  ui.app.setScreen(&HubActivity::listScreen, this);

  if (!HubClient::isConfigured()) {
    statusMessage = tr(STR_HUB_NOT_SET);
    statusIsError = true;
    requestUpdate();
    return;
  }

  connectWifi();
}

void HubActivity::onExit() {
  Activity::onExit();
  if (wifiActivated) {
    wifiOff();
    wifiActivated = false;
  }
}

void HubActivity::connectWifi() {
  wifiActivated = true;
  if (hasActiveStationWifiConnection()) {
    onWifiReady(true);
    return;
  }
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, true, true),
                         [this](const ActivityResult& result) { onWifiReady(!result.isCancelled); });
}

void HubActivity::onWifiReady(const bool connected) {
  {
    RenderLock lock(*this);
    if (connected) {
      // The menu itself needs no data; each page fetches when it opens.
      statusMessage.clear();
      statusIsError = false;
    } else {
      statusMessage = tr(STR_HUB_UNREACHABLE);
      statusIsError = true;
    }
  }
  requestUpdate();
}

const char* HubActivity::pageTitle() const {
  switch (page) {
    case Page::Lights:
      return tr(STR_HUB_LIGHTS);
    case Page::Tasks:
      return tr(STR_HUB_TASKS);
    case Page::Calendar:
      return tr(STR_HUB_CALENDAR);
    case Page::Notes:
      return tr(STR_HUB_NOTES);
    case Page::Note:
      return noteTitle.empty() ? tr(STR_HUB_NOTES) : noteTitle.c_str();
    case Page::Menu:
    default:
      return tr(STR_HUB);
  }
}

int HubActivity::rowCount() const {
  switch (page) {
    case Page::Menu:
      return MENU_COUNT;
    case Page::Lights:
      return static_cast<int>(tiles.size());
    case Page::Tasks:
      return static_cast<int>(tasks.size());
    case Page::Calendar:
      return static_cast<int>(events.size());
    case Page::Notes:
      return static_cast<int>(notes.size());
    case Page::Note:
      // Plus a trailing row to pull the next chunk, when there is one.
      return static_cast<int>(noteBlocks.size()) + (noteNext >= 0 ? 1 : 0);
    default:
      return 0;
  }
}

void HubActivity::openPage(const Page next) {
  {
    // Page and selection are read by the render task, so they change together
    // rather than leaving it a page index that disagrees with the rows.
    RenderLock lock(*this);
    page = next;
    selectedIndex = 0;
    topIndex = 0;
    listNav.reset(0);
    statusMessage.clear();
    statusIsError = false;
  }
  needsFetch = next != Page::Menu;
  requestUpdate();
}

void HubActivity::goBack() {
  if (page == Page::Menu) {
    finishAfterBackPress();
    return;
  }
  if (page == Page::Note) {
    {
      RenderLock lock(*this);
      noteBlocks.clear();
      noteBlocks.shrink_to_fit();
      noteNext = -1;
    }
    // The note list is still in memory and cannot have changed while a note was
    // open, so go back to it without paying for another round trip.
    {
      RenderLock lock(*this);
      page = Page::Notes;
      selectedIndex = 0;
      topIndex = 0;
      listNav.reset(0);
      statusMessage = notes.empty() ? tr(STR_HUB_NOTHING) : "";
      statusIsError = false;
    }
    needsFetch = false;
    requestUpdate();
    return;
  }
  {
    RenderLock lock(*this);
    page = Page::Menu;
    selectedIndex = 0;
    topIndex = 0;
    listNav.reset(0);
    statusMessage.clear();
    statusIsError = false;
  }
  needsFetch = false;
  requestUpdate();
}

std::string HubActivity::errorText(const HubClient::Error error, const std::string& message) const {
  // The bridge's own sentence is more useful than a generic one, so prefer it
  // and fall back to a translated line only when it did not send one.
  if (!message.empty()) return message;
  switch (error) {
    case HubClient::NOT_CONFIGURED:
      return tr(STR_HUB_NOT_SET);
    case HubClient::AUTH_FAILED:
      return tr(STR_HUB_REJECTED);
    case HubClient::NOT_FOUND:
      return tr(STR_HUB_NOT_AVAILABLE);
    case HubClient::NETWORK_ERROR:
      return tr(STR_HUB_UNREACHABLE);
    default:
      return tr(STR_HUB_FAILED);
  }
}

void HubActivity::runFetch() {
  needsFetch = false;
  busy = true;

  // The request blocks the main loop, so without this the panel shows an empty
  // frame for the whole round trip and reads as broken.
  if (rowCount() == 0) {
    {
      RenderLock lock(*this);
      statusMessage = tr(STR_LOADING);
      statusIsError = false;
    }
    requestUpdateAndWait();
  }

  // The request runs against locals and only the swap below is locked. Holding
  // RenderLock across an eight-second HTTP call would stall the render task;
  // mutating the live vectors without it would let that task walk a list while
  // it reallocates, which is a crash rather than a glitch.
  std::vector<HubTile> nextTiles;
  std::vector<HubTodo> nextTasks;
  std::vector<HubEvent> nextEvents;
  std::vector<HubNote> nextNotes;
  std::vector<HubBlock> nextBlocks;
  std::string nextTitle = noteTitle;
  int nextNoteNext = -1;

  std::string message;
  HubClient::Error error = HubClient::OK;
  const Page fetching = page;

  switch (fetching) {
    case Page::Lights:
      error = HubClient::fetchTiles(nextTiles, message);
      break;
    case Page::Tasks:
      error = HubClient::fetchTodo(nextTasks, message);
      break;
    case Page::Calendar:
      error = HubClient::fetchCalendar(nextEvents, message);
      break;
    case Page::Notes:
      error = HubClient::fetchNotes(nextNotes, message);
      break;
    case Page::Note:
      error = HubClient::fetchNote(noteId, noteFrom, nextTitle, nextBlocks, nextNoteNext, message);
      break;
    default:
      break;
  }

  busy = false;

  // Defensive: the fetch is synchronous today, so this cannot fire. It is here
  // so that making it asynchronous later does not silently paint one page's
  // answer over another.
  if (page != fetching) return;

  {
    RenderLock lock(*this);
    int count = 0;
    switch (fetching) {
      case Page::Lights:
        if (error == HubClient::OK) tiles = std::move(nextTiles);
        count = static_cast<int>(tiles.size());
        break;
      case Page::Tasks:
        if (error == HubClient::OK) tasks = std::move(nextTasks);
        count = static_cast<int>(tasks.size());
        break;
      case Page::Calendar:
        if (error == HubClient::OK) events = std::move(nextEvents);
        count = static_cast<int>(events.size());
        break;
      case Page::Notes:
        if (error == HubClient::OK) notes = std::move(nextNotes);
        count = static_cast<int>(notes.size());
        break;
      case Page::Note:
        if (error == HubClient::OK) {
          noteBlocks = std::move(nextBlocks);
          noteTitle = std::move(nextTitle);
          noteNext = nextNoteNext;
        }
        count = static_cast<int>(noteBlocks.size());
        break;
      default:
        break;
    }

    if (error != HubClient::OK) {
      statusMessage = errorText(error, message);
      statusIsError = true;
    } else if (count == 0) {
      statusMessage = tr(STR_HUB_NOTHING);
      statusIsError = false;
    } else {
      statusMessage.clear();
      statusIsError = false;
    }

    selectedIndex = 0;
    topIndex = 0;
    listNav.reset(0);
  }
  requestUpdate();
}

void HubActivity::activateRow(const int index) {
  if (index < 0 || index >= rowCount()) return;

  std::string message;
  switch (page) {
    case Page::Menu:
      openPage(MENU_PAGES[index]);
      return;

    case Page::Lights: {
      // Touch skips disabled rows; the button path has to skip them too.
      if (tiles[index].state < 0) return;
      // Copy the id before the call: the vector is only read here, but the id
      // is what the answer is matched against and the row may move meanwhile.
      const std::string entityId = tiles[index].id;
      HubTile updated;
      busy = true;
      const auto error = HubClient::toggleTile(entityId, updated, message);
      busy = false;
      {
        RenderLock lock(*this);
        if (error != HubClient::OK) {
          statusMessage = errorText(error, message);
          statusIsError = true;
        } else {
          // Repaint one row from the bridge's answer instead of refetching the
          // whole list, which would cost a second round trip per tap.
          for (auto& tile : tiles) {
            if (tile.id != entityId) continue;
            tile.state = updated.state;
            if (!updated.name.empty()) tile.name = updated.name;
            break;
          }
          statusMessage.clear();
          statusIsError = false;
        }
      }
      requestUpdate();
      return;
    }

    case Page::Tasks: {
      const std::string uid = tasks[index].id;
      busy = true;
      const auto error = HubClient::completeTodo(uid, message);
      busy = false;
      {
        RenderLock lock(*this);
        if (error != HubClient::OK) {
          statusMessage = errorText(error, message);
          statusIsError = true;
        } else {
          for (size_t i = 0; i < tasks.size(); i++) {
            if (tasks[i].id != uid) continue;
            tasks.erase(tasks.begin() + static_cast<long>(i));
            break;
          }
          if (selectedIndex >= static_cast<int>(tasks.size())) {
            selectedIndex = tasks.empty() ? 0 : static_cast<int>(tasks.size()) - 1;
          }
          statusMessage = tasks.empty() ? tr(STR_HUB_NOTHING) : "";
          statusIsError = false;
          listNav.selected = selectedIndex;
        }
      }
      requestUpdate();
      return;
    }

    case Page::Notes: {
      RenderLock lock(*this);
      noteId = notes[index].id;
      noteTitle = notes[index].name;
      noteBlocks.clear();
      noteNext = 0;
      noteFrom = 0;
      lock.unlock();
      openPage(Page::Note);
      return;
    }

    case Page::Note:
      // Only the trailing row pulls the next chunk. Tapping a line of prose
      // should do nothing, not jump the reader forward.
      if (noteNext >= 0 && index == static_cast<int>(noteBlocks.size())) {
        noteFrom = noteNext;
        needsFetch = true;
        requestUpdate();
      }
      return;

    default:
      return;
  }
}

void HubActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<HubActivity*>(user);
  if (event.value < 0 || event.value >= self->rowCount()) return;
  self->selectedIndex = event.value;
  self->ui.app.clearTapFlash();
  self->activateRow(event.value);
}

void HubActivity::moveSelection(const int delta) {
  const int count = rowCount();
  if (count <= 0) return;
  {
    RenderLock lock(*this);
    selectedIndex = delta > 0 ? ButtonNavigator::nextIndex(selectedIndex, count)
                              : ButtonNavigator::previousIndex(selectedIndex, count);
    listNav.selected = selectedIndex;
    listNav.top = topIndex;
    listNav.visibleRows = visibleRows;
    listNav.follow(count);
    topIndex = listNav.top;
  }
  requestUpdate();
}

void HubActivity::loop() {
  if (needsFetch && !busy) {
    runFetch();
    return;
  }

  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    goBack();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    goBack();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    mappedInput.suppressNextConfirmRelease();
    activateRow(selectedIndex);
    return;
  }

  if (ui.routingReady()) {
    fui::ActionEvent event{};
    if (ui.routeTouch(mappedInput, event)) {
      if (ui.app.invalidated()) requestUpdate();
      if (event) return;
    }
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    const int count = rowCount();
    if (count <= 0) return;
    bool moved = false;
    {
      RenderLock lock(*this);
      listNav.selected = selectedIndex;
      listNav.top = topIndex;
      listNav.visibleRows = visibleRows;
      const int pageRows = listNav.pageRowsFor(count);
      moved = listNav.scrollBy(swipe == MappedInputManager::SwipeDir::Up ? pageRows : -pageRows, count);
      topIndex = listNav.top;
    }
    if (moved) requestUpdate();
    return;
  }

  buttonNavigator.onNext([this] { moveSelection(1); });
  buttonNavigator.onPrevious([this] { moveSelection(-1); });
}

void HubActivity::buildMenuRows(std::vector<fui::ListItem>& items) {
  // These arrays live here while MENU_COUNT lives in the header, so a fifth
  // page would read them out of bounds with no diagnostic.
  static_assert(sizeof(MENU_LABELS) / sizeof(MENU_LABELS[0]) == MENU_COUNT,
                "MENU_LABELS must have one entry per menu page");
  static_assert(sizeof(MENU_ICONS) / sizeof(MENU_ICONS[0]) == MENU_COUNT,
                "MENU_ICONS must have one entry per menu page");
  for (int i = 0; i < MENU_COUNT; i++) {
    fui::ListItem item;
    item.label = I18N.get(MENU_LABELS[i]);
    item.icon = listIconFor(MENU_ICONS[i], 32);
    item.actionValue = static_cast<int16_t>(i);
    items.push_back(item);
  }
}

void HubActivity::buildContentRows(std::vector<fui::ListItem>& items) {
  // Rows point into rowText, so it is sized before anything takes a pointer:
  // one computed string per row at most, and no growth while pointers are live.
  rowText.clear();
  rowText.reserve(static_cast<size_t>(rowCount()));

  switch (page) {
    case Page::Lights:
      for (size_t i = 0; i < tiles.size(); i++) {
        fui::ListItem item;
        item.label = tiles[i].name.c_str();
        // A real switch reads at a glance on e-ink; an unavailable entity is
        // dimmed and says so instead of showing a switch that lies.
        // The list ORs in StateSelected and StateDisabled itself, so state is
        // left alone; toggleChecked is what draws the knob.
        item.toggle = tiles[i].state >= 0;
        item.toggleChecked = tiles[i].state == 1;
        if (tiles[i].state < 0) {
          item.enabled = false;
          item.subtitle = tr(STR_HUB_NOT_AVAILABLE);
        }
        item.actionValue = static_cast<int16_t>(i);
        items.push_back(item);
      }
      break;

    case Page::Tasks:
      for (size_t i = 0; i < tasks.size(); i++) {
        fui::ListItem item;
        item.label = tasks[i].name.c_str();
        if (!tasks[i].due.empty()) item.subtitle = tasks[i].due.c_str();
        item.actionValue = static_cast<int16_t>(i);
        items.push_back(item);
      }
      break;

    case Page::Calendar:
      for (size_t i = 0; i < events.size(); i++) {
        fui::ListItem item;
        item.label = events[i].name.c_str();
        rowText.push_back(formatWhen(events[i].start, events[i].allDay));
        item.subtitle = rowText.back().c_str();
        item.actionValue = static_cast<int16_t>(i);
        items.push_back(item);
      }
      break;

    case Page::Notes:
      for (size_t i = 0; i < notes.size(); i++) {
        fui::ListItem item;
        item.label = notes[i].name.c_str();
        item.actionValue = static_cast<int16_t>(i);
        items.push_back(item);
      }
      break;

    case Page::Note:
      for (size_t i = 0; i < noteBlocks.size(); i++) {
        const auto& block = noteBlocks[i];
        fui::ListItem item;
        if (block.kind == HubBlock::Rule) {
          rowText.emplace_back("----");
        } else {
          rowText.push_back(std::string(bulletFor(block)) + block.text);
        }
        item.label = rowText.back().c_str();
        item.isHeader = block.kind == HubBlock::Heading;
        item.actionValue = static_cast<int16_t>(i);
        items.push_back(item);
      }
      if (noteNext >= 0) {
        fui::ListItem more;
        more.label = tr(STR_MORE);
        more.actionValue = static_cast<int16_t>(noteBlocks.size());
        items.push_back(more);
      }
      break;

    default:
      break;
  }
}

void HubActivity::listScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<HubActivity*>(user)->buildScreen(screen);
}

void HubActivity::buildScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput)), 0,
                  static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // A status message replaces the list rather than sitting above it, so a
  // failed fetch never leaves stale rows on screen looking current.
  if (!statusMessage.empty() && rowCount() == 0) return;

  std::vector<fui::ListItem> items;
  items.reserve(static_cast<size_t>(rowCount()));
  if (page == Page::Menu) {
    buildMenuRows(items);
  } else {
    buildContentRows(items);
  }
  if (items.empty()) return;

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex = static_cast<int16_t>(selectedIndex);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.labelText = screen.theme().bodyText;
  props.labelText.bold = page == Page::Menu;
  props.subtitleText = screen.theme().smallText;
  props.subtitleText.maxLines = 1;
  props.headerText = screen.theme().bodyText;
  props.headerText.bold = true;
  props.rowGap = 10;

  // A note is prose, so its rows wrap over several lines; every other page is
  // a label with at most one line of detail under it.
  if (page == Page::Note) props.labelText.maxLines = 4;

  const auto rows = configureUiList(props, screen.theme(), screen.body(), UiListRowType::WithSubtitle);
  visibleRows = rows > 0 ? rows : 1;
  listNav.selected = selectedIndex;
  listNav.top = topIndex;
  listNav.visibleRows = visibleRows;
  listNav.syncToProps(screen.body(), props.rowHeight, props.rowGap, static_cast<int>(items.size()), props);
  topIndex = listNav.top;
  screen.list(props);
  topIndex = listNav.top;
}

void HubActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, ui.target, header, pageTitle(), false);
  } else {
    GUI.drawHeader(renderer, header, pageTitle());
  }

  ui.closeRouting();
  for (int pass = 0; pass < 8; ++pass) {
    ui.render();
    if (!listNav.consumeRebuildNeeded()) break;
  }

  if (!statusMessage.empty()) {
    const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
    if (rowCount() == 0) {
      UITheme::drawCenteredText(renderer, safe, UI_10_FONT_ID, safe.y + safe.height / 2 - 20, statusMessage.c_str(),
                                true, statusIsError ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    } else {
      // An action can fail while the list still holds good rows. Without this
      // the tap would read as having done nothing at all.
      const int y = safe.y + safe.height - metrics.buttonHintsHeight - 28;
      UITheme::drawCenteredText(renderer, safe, UI_10_FONT_ID, y, statusMessage.c_str(), true, EpdFontFamily::BOLD);
    }
  }

  const char* confirmLabel = tr(STR_SELECT);
  if (page == Page::Lights) {
    confirmLabel = tr(STR_TOGGLE);
  } else if (page == Page::Tasks) {
    confirmLabel = tr(STR_DONE);
  }

  const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)),
                                            rowCount() > 0 ? confirmLabel : "", tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Mixing the chunk in makes each page of a note a transition; a light toggle
  // keeps the same key and stays fast.
  const uint8_t refreshKey =
      page == Page::Note ? static_cast<uint8_t>(0x80 ^ (noteFrom & 0x7F)) : static_cast<uint8_t>(page);
  renderer.displayBuffer(screenTransitionRefresh.modeFor(refreshKey));
}

#endif  // CROSSINK_APP_CAP_HUB
