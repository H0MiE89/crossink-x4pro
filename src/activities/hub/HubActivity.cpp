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
  if (!connected) {
    statusMessage = tr(STR_HUB_UNREACHABLE);
    statusIsError = true;
    requestUpdate();
    return;
  }
  // The menu itself needs no data; each page fetches when it opens.
  statusMessage.clear();
  statusIsError = false;
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
  page = next;
  selectedIndex = 0;
  topIndex = 0;
  listNav.reset(0);
  statusMessage.clear();
  statusIsError = false;
  needsFetch = next != Page::Menu;
  requestUpdate();
}

void HubActivity::goBack() {
  if (page == Page::Menu) {
    finishAfterBackPress();
    return;
  }
  if (page == Page::Note) {
    noteBlocks.clear();
    noteBlocks.shrink_to_fit();
    openPage(Page::Notes);
    return;
  }
  page = Page::Menu;
  selectedIndex = 0;
  topIndex = 0;
  listNav.reset(0);
  statusMessage.clear();
  statusIsError = false;
  needsFetch = false;
  requestUpdate();
}

void HubActivity::applyError(const HubClient::Error error, const std::string& message) {
  statusIsError = true;
  // The bridge's own sentence is more useful than a generic one, so prefer it
  // and fall back to a translated line only when it did not send one.
  if (!message.empty()) {
    statusMessage = message;
    return;
  }
  switch (error) {
    case HubClient::NOT_CONFIGURED:
      statusMessage = tr(STR_HUB_NOT_SET);
      break;
    case HubClient::AUTH_FAILED:
      statusMessage = tr(STR_HUB_REJECTED);
      break;
    case HubClient::NOT_FOUND:
      statusMessage = tr(STR_HUB_NOT_AVAILABLE);
      break;
    case HubClient::NETWORK_ERROR:
      statusMessage = tr(STR_HUB_UNREACHABLE);
      break;
    default:
      statusMessage = tr(STR_HUB_FAILED);
      break;
  }
}

void HubActivity::runFetch() {
  needsFetch = false;
  busy = true;
  statusMessage.clear();
  statusIsError = false;

  std::string message;
  HubClient::Error error = HubClient::OK;

  switch (page) {
    case Page::Lights:
      error = HubClient::fetchTiles(tiles, message);
      break;
    case Page::Tasks:
      error = HubClient::fetchTodo(tasks, message);
      break;
    case Page::Calendar:
      error = HubClient::fetchCalendar(events, message);
      break;
    case Page::Notes:
      error = HubClient::fetchNotes(notes, message);
      break;
    case Page::Note:
      noteFrom = noteNext < 0 ? 0 : noteNext;
      error = HubClient::fetchNote(noteId, noteFrom, noteTitle, noteBlocks, noteNext, message);
      break;
    default:
      break;
  }

  busy = false;

  if (error != HubClient::OK) {
    applyError(error, message);
  } else if (rowCount() == 0) {
    statusMessage = tr(STR_HUB_NOTHING);
    statusIsError = false;
  }

  selectedIndex = 0;
  topIndex = 0;
  listNav.reset(0);
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
      HubTile updated;
      busy = true;
      const auto error = HubClient::toggleTile(tiles[index].id, updated, message);
      busy = false;
      if (error != HubClient::OK) {
        applyError(error, message);
      } else {
        // Repaint one row from the bridge's answer instead of refetching the
        // whole list, which would cost a second round trip per tap.
        tiles[index].state = updated.state;
        if (!updated.name.empty()) tiles[index].name = updated.name;
        statusMessage.clear();
        statusIsError = false;
      }
      requestUpdate();
      return;
    }

    case Page::Tasks: {
      busy = true;
      const auto error = HubClient::completeTodo(tasks[index].id, message);
      busy = false;
      if (error != HubClient::OK) {
        applyError(error, message);
      } else {
        statusMessage.clear();
        statusIsError = false;
        tasks.erase(tasks.begin() + index);
        if (selectedIndex >= static_cast<int>(tasks.size())) {
          selectedIndex = tasks.empty() ? 0 : static_cast<int>(tasks.size()) - 1;
        }
        if (tasks.empty()) statusMessage = tr(STR_HUB_NOTHING);
      }
      requestUpdate();
      return;
    }

    case Page::Notes:
      noteId = notes[index].id;
      noteTitle = notes[index].name;
      noteBlocks.clear();
      noteNext = 0;
      noteFrom = 0;
      openPage(Page::Note);
      return;

    case Page::Note:
      // Only the trailing row pulls the next chunk. Tapping a line of prose
      // should do nothing, not jump the reader forward.
      if (noteNext >= 0 && index == static_cast<int>(noteBlocks.size())) {
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
        item.toggle = tiles[i].state >= 0;
        item.state = tiles[i].state == 1 ? fui::StateChecked : fui::StateNormal;
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

  renderer.displayBuffer(screenTransitionRefresh.modeFor(static_cast<uint8_t>(page)));
}
