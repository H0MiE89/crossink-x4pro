#pragma once
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "activities/ScreenTransitionRefresh.h"
#include "components/UiAppHost.h"
#include "network/HubClient.h"
#include "util/ButtonNavigator.h"

/**
 * Everything the reader shows that is not a book: house lights, tasks, an
 * agenda, and Notion notes, all read from the owner's own xthub service.
 *
 * One activity holds every page rather than one activity per screen. Wi-Fi is
 * expensive to bring up and expensive to leave running, so it comes up once on
 * entry, stays up while moving between pages, and goes down on exit. Each page
 * fetches when it opens and then sits still, which is what an e-ink panel
 * wants anyway.
 */
class HubActivity final : public Activity {
  using UiHost = UiAppHost<24, 4>;
  using UiApp = UiHost::App;

 public:
  static constexpr const char* NAME = "Hub";

  explicit HubActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return busy; }

 private:
  enum class Page : uint8_t { Menu, Lights, Tasks, Calendar, Notes, Note };

  // Menu rows, in screen order.
  static constexpr int MENU_COUNT = 4;
  static constexpr Page MENU_PAGES[MENU_COUNT] = {Page::Lights, Page::Tasks, Page::Calendar, Page::Notes};

  UiHost ui;
  ButtonNavigator buttonNavigator;
  ScreenTransitionRefresh screenTransitionRefresh;
  freeink::ui::ListNav listNav;

  Page page = Page::Menu;
  int selectedIndex = 0;
  int topIndex = 0;
  int visibleRows = 1;

  bool wifiActivated = false;
  bool busy = false;           // a request is in flight; blocks auto-sleep
  bool needsFetch = false;     // the page opened and has not loaded yet
  std::string statusMessage;   // shown in place of the list when it is set
  bool statusIsError = false;

  // Backing store for row strings the list does not own. ListItem holds bare
  // pointers, so anything computed per render has to outlive the render; it is
  // reserved once per build so growth cannot invalidate the pointers handed out.
  std::vector<std::string> rowText;

  std::vector<HubTile> tiles;
  std::vector<HubTodo> tasks;
  std::vector<HubEvent> events;
  std::vector<HubNote> notes;

  // The open note, paged through the bridge a chunk at a time.
  std::string noteId;
  std::string noteTitle;
  std::vector<HubBlock> noteBlocks;
  int noteNext = -1;
  int noteFrom = 0;

  static void listScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);

  void buildScreen(UiApp::ScreenType& screen);
  void buildMenuRows(std::vector<freeink::ui::ListItem>& items);
  void buildContentRows(std::vector<freeink::ui::ListItem>& items);

  int rowCount() const;
  const char* pageTitle() const;
  void openPage(Page next);
  void goBack();
  void activateRow(int index);
  void runFetch();
  void applyError(HubClient::Error error, const std::string& message);
  void connectWifi();
  void onWifiReady(bool connected);
  void moveSelection(int delta);
};
