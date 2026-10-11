#include "Overlay.hpp"

#include "BaselineFile.hpp"
#include "Profiler.hpp"
#include "ZonesApi.hpp"

#include <Geode/Geode.hpp>
#include <Geode/ui/OverlayManager.hpp>

using namespace geode::prelude;

namespace
{
  constexpr int ROWS = 10;
  constexpr int DETAIL_LINES = 9;
  constexpr int SLOW_LINES = 4;
  constexpr float WIDTH = 230;
  constexpr float LINE = 6.5f;
  constexpr float MARGIN = 4;
  constexpr float FONT_SCALE = 0.3f;
  // Right edges of the number columns
  constexpr float OWN_MS_X = 128;
  constexpr float BASE_X = 158;
  constexpr float OWN_X = 190;
  constexpr float INCLUSIVE_X = WIDTH - MARGIN;
  // A session shorter than this is too short to compare with
  constexpr double MIN_BASELINE_SECONDS = 5;

  ccColor3B const GREY = {150, 150, 160};
  ccColor3B const GOLD = {255, 210, 90};
  ccColor3B const GREEN = {120, 220, 120};
  ccColor3B const RED = {255, 140, 120};
  ccColor3B const BUTTON = {140, 200, 255};

  std::string fit(std::string text, size_t length)
  {
    if (text.size() > length)
      text = text.substr(0, length - 1) + ".";
    return text;
  }

  CCLabelBMFont* label(CCNode* parent, float x, float y, CCPoint anchor, ccColor3B color = {255, 255, 255})
  {
    auto text = CCLabelBMFont::create("", "chatFont.fnt");
    text->setScale(FONT_SCALE);
    text->setAnchorPoint(anchor);
    text->setPosition({x, y});
    text->setColor(color);
    parent->addChild(text);
    return text;
  }

  class Overlay : public CCNode
  {
  public:
    static Overlay* create()
    {
      auto node = new Overlay();
      if (node->init())
      {
        node->autorelease();
        return node;
      }
      delete node;
      return nullptr;
    }

    bool init() override
    {
      if (!CCNode::init())
        return false;
      auto height = MARGIN * 2 + LINE * (5 + ROWS + 1 + DETAIL_LINES + 1 + SLOW_LINES);
      setContentSize({WIDTH, height});
      setAnchorPoint({0, 1});
      setID("mod-profiler-overlay"_spr);

      addChild(CCLayerColor::create({0, 0, 0, 175}, WIDTH, height));
      m_menu = CCMenu::create();
      m_menu->setPosition({0, 0});
      m_menu->setContentSize(getContentSize());
      addChild(m_menu);

      float y = height - MARGIN;
      m_title = label(this, MARGIN, y, {0, 1}, GOLD);
      y -= LINE;
      m_summary = label(this, MARGIN, y, {0, 1}, {190, 190, 200});
      y -= LINE;
      m_compare = label(this, MARGIN, y, {0, 1}, {190, 190, 200});
      y -= LINE;

      // The buttons: what the table shows, starting the session again, the baseline, saving a profile
      float x = MARGIN;
      m_viewButton = button(x, y, "[last 5 s]", menu_selector(Overlay::onView));
      button(x, y, "[reset]", menu_selector(Overlay::onReset));
      m_baselineButton = button(x, y, "[new baseline]", menu_selector(Overlay::onBaseline));
      m_clearButton = button(x, y, "[forget baseline]", menu_selector(Overlay::onForget));
      button(x, y, "[save profile]", menu_selector(Overlay::onSave));
      y -= LINE * 1.2f;

      label(this, MARGIN, y, {0, 1}, GREY)->setString("Mod");
      m_ownHeader = label(this, OWN_MS_X, y, {1, 1}, GREY);
      m_baseHeader = label(this, BASE_X, y, {1, 1}, GREY);
      label(this, OWN_X, y, {1, 1}, GREY)->setString("own %");
      label(this, INCLUSIVE_X, y, {1, 1}, GREY)->setString("incl %");
      y -= LINE;

      for (int i = 0; i < ROWS; i++)
      {
        Row row;
        row.name = label(this, MARGIN, y, {0, 1});
        row.ownMillis = label(this, OWN_MS_X, y, {1, 1}, GREEN);
        row.base = label(this, BASE_X, y, {1, 1}, GREY);
        row.own = label(this, OWN_X, y, {1, 1});
        row.inclusive = label(this, INCLUSIVE_X, y, {1, 1}, {170, 170, 180});
        // A click on the row picks the mod
        row.button = CCMenuItem::create(this, menu_selector(Overlay::onRow));
        row.button->setContentSize({WIDTH, LINE});
        row.button->setAnchorPoint({0, 1});
        row.button->setPosition({0, y});
        row.button->setTag(i);
        m_menu->addChild(row.button);
        m_rows.push_back(row);
        y -= LINE;
      }

      y -= LINE / 2;
      m_detail = label(this, MARGIN, y, {0, 1}, {210, 210, 220});
      y -= LINE * (DETAIL_LINES + 0.5f);
      m_slow = label(this, MARGIN, y, {0, 1}, {255, 170, 150});

      schedule(schedule_selector(Overlay::refresh), 0.5f);
      refresh(0);
      return true;
    }

    void refresh(float)
    {
      if (!isVisible())
        return;
      auto shared = profiler::snapshot();
      auto const& snapshot = *shared;
      // The start isn't counted until a few seconds after the game loaded: until then the session shows
      auto view = m_view == View::Startup && !snapshot.startupReady ? View::Session : m_view;
      auto const& window = view == View::Recent    ? snapshot.recent
                           : view == View::Session ? snapshot.session
                           : view == View::Scene   ? snapshot.scene
                                                   : snapshot.startup;
      bool startup = view == View::Startup;
      auto const& baseline = baseline_file::current();
      auto const& profile = window.profile;
      auto samples = profile.samples();

      m_viewButton->setString(m_view == View::Recent    ? "[last 5 s]"
                              : m_view == View::Session ? "[session]"
                              : m_view == View::Scene   ? "[this scene]"
                                                        : "[startup]");
      m_ownHeader->setString(startup ? "own s" : "own ms");
      m_baselineButton->setString(baseline.empty() ? "[set baseline]" : "[new baseline]");
      m_clearButton->setVisible(!baseline.empty());
      m_clearItem->setEnabled(!baseline.empty());
      m_baseHeader->setString(baseline.empty() || m_view == View::Startup ? "" : "base ms");

      auto fps = window.seconds > 0 ? window.frames / window.seconds : 0;
      auto frameMillis = window.frames ? window.seconds * 1000 / window.frames : 0;
      if (startup)
        m_title->setString(fmt::format("Mod Profiler   the game's start: {:.1f} s until loaded", window.seconds).c_str());
      else
        m_title->setString(fmt::format("Mod Profiler   {:.0f} fps, {:.2f} ms a frame   {}", fps, frameMillis,
                                       view == View::Session ? fmt::format("session of {:.0f} s", window.seconds)
                                       : view == View::Scene ? fmt::format("{} for {:.0f} s", snapshot.sceneName, window.seconds)
                                                             : std::string("last 5 s"))
                               .c_str());
      if (samples == 0)
      {
        m_summary->setString("No samples yet");
        m_compare->setString("");
        for (auto& row : m_rows)
          clearRow(row);
        m_detail->setString("");
        m_slow->setString("");
        return;
      }
      auto between = 100.0 * profile.between() / samples;
      auto idle = 100.0 * profile.idle() / samples;
      auto cost = costOf(window);
      if (startup)
        m_summary->setString(fmt::format("working {:.0f}%, waiting {:.0f}%   {} samples, [save profile] saves the start",
                                         std::max(0.0, 100 - between - idle), between + idle, window.stats.samples)
                                 .c_str());
      else
        m_summary->setString(fmt::format("in frames {:.0f}% ({:.2f} ms), between {:.0f}%, idle {:.0f}%   stopped {:.1f} ms/s",
                                       std::max(0.0, 100 - between - idle), busyMillis(window), between, idle,
                                       cost.stoppedMillisPerSecond)
                               .c_str());
      if (baseline.empty())
        m_compare->setString("No baseline: [set baseline] keeps this session to compare the next ones with");
      else
        m_compare->setString(fmt::format("Baseline of {} ({:.0f} s): frame {:.2f} ms, work {:.2f} ms", baseline.saved,
                                         baseline.seconds, baseline.frameMillis, baseline.busyMillis)
                                 .c_str());

      auto top = topMods(window, ROWS, Order::Own);
      m_shown.clear();
      for (size_t i = 0; i < m_rows.size(); i++)
      {
        auto& row = m_rows[i];
        if (i >= top.size())
        {
          clearRow(row);
          continue;
        }
        auto const& share = top[i];
        auto const& bucket = snapshot.buckets[share.bucket];
        m_shown.push_back(bucket.id);
        bool picked = bucket.id == m_picked;
        row.name->setString(fit((picked ? "> " : "") + bucket.name, 30).c_str());
        row.name->setColor(picked ? GOLD : ccColor3B{255, 255, 255});
        if (startup)
          row.ownMillis->setString(fmt::format("{:.2f}", share.ownPercent / 100 * window.seconds).c_str());
        else
          row.ownMillis->setString(fmt::format("{:.2f}", share.ownMillisPerFrame).c_str());
        row.own->setString(fmt::format("{:.1f}", share.ownPercent).c_str());
        row.inclusive->setString(fmt::format("{:.1f}", share.inclusivePercent).c_str());
        row.button->setEnabled(true);

        // Against the baseline: red when the mod takes more than it did
        auto base = baseline.mods.find(bucket.id);
        if (baseline.empty() || startup)
          row.base->setString("");
        else if (base == baseline.mods.end())
        {
          row.base->setString("new");
          row.base->setColor(RED);
        }
        else
        {
          auto change = share.ownMillisPerFrame - base->second.ownMillis;
          row.base->setString(fmt::format("{:.2f}", base->second.ownMillis).c_str());
          row.base->setColor(change > 0.01 ? RED : change < -0.01 ? GREEN : GREY);
        }
      }
      // Until a row is clicked, the busiest mod is the picked one
      if ((m_picked.empty() || !m_clicked) && !m_shown.empty())
        m_picked = m_shown.front();

      m_detail->setString(detail(snapshot, window).c_str());
      m_slow->setString(slowFrames(snapshot).c_str());
    }

  private:
    struct Row
    {
      CCLabelBMFont* name = nullptr;
      CCLabelBMFont* ownMillis = nullptr;
      CCLabelBMFont* base = nullptr;
      CCLabelBMFont* own = nullptr;
      CCLabelBMFont* inclusive = nullptr;
      CCMenuItem* button = nullptr;
    };

    // A text button at x, which moves on past it. The item is scaled, not the label: the item's size
    // is what a click hits
    CCLabelBMFont* button(float& x, float y, char const* text, SEL_MenuHandler handler)
    {
      auto label = CCLabelBMFont::create(text, "chatFont.fnt");
      label->setColor(BUTTON);
      auto item = CCMenuItemLabel::create(label, this, handler);
      item->setScale(FONT_SCALE);
      item->setAnchorPoint({0, 1});
      item->setPosition({x, y});
      m_menu->addChild(item);
      if (handler == menu_selector(Overlay::onForget))
        m_clearItem = item;
      x += item->getContentSize().width * FONT_SCALE + 4;
      return label;
    }

    void onRow(CCObject* sender)
    {
      auto index = static_cast<size_t>(static_cast<CCNode*>(sender)->getTag());
      if (index < m_shown.size())
      {
        m_picked = m_shown[index];
        m_clicked = true;
      }
      refresh(0);
    }

    void onView(CCObject*)
    {
      m_view = m_view == View::Recent    ? View::Scene
               : m_view == View::Scene   ? View::Session
               : m_view == View::Session ? View::Startup
                                         : View::Recent;
      refresh(0);
    }

    void onReset(CCObject*)
    {
      profiler::resetSession();
      Notification::create("The session starts again", NotificationIcon::Info, 1.5f)->show();
    }

    void onBaseline(CCObject*)
    {
      auto shared = profiler::snapshot();
      auto const& snapshot = *shared;
      if (snapshot.session.seconds < MIN_BASELINE_SECONDS)
      {
        Notification::create("Play a few seconds more first", NotificationIcon::Warning)->show();
        return;
      }
      auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
      std::tm local{};
      localtime_s(&local, &now);
      char saved[32];
      std::strftime(saved, sizeof(saved), "%d.%m %H:%M", &local);
      baseline_file::save(makeBaseline(snapshot.session, snapshot.buckets, saved));
      log::info("Baseline saved: {}", summaryLine(snapshot.session, snapshot.buckets, 5));
      Notification::create("The session is the baseline now: [reset] and compare", NotificationIcon::Success, 3.f)->show();
      refresh(0);
    }

    void onForget(CCObject*)
    {
      baseline_file::clear();
      refresh(0);
    }

    // In the start's view, the start is saved
    void onSave(CCObject*) { overlay::saveProfile(m_view == View::Startup); }

    void clearRow(Row& row)
    {
      row.name->setString("");
      row.ownMillis->setString("");
      row.base->setString("");
      row.own->setString("");
      row.inclusive->setString("");
      row.button->setEnabled(false);
    }

    // The latest slow frames and whose they were
    std::string slowFrames(profiler::Snapshot const& snapshot)
    {
      auto const& frames = snapshot.slowFrames;
      if (frames.empty())
        return "No slow frames";
      std::string text = fmt::format("Slow frames: {}", frames.size());
      auto shown = std::min<size_t>(frames.size(), SLOW_LINES - 1);
      for (size_t i = frames.size() - shown; i < frames.size(); i++)
      {
        auto const& frame = frames[i];
        auto who = !frame.culprit.empty()   ? fmt::format("{} {:.0f}%", fit(frame.culprit, 22), frame.culpritPercent)
                   : !frame.waitingIn.empty() ? fmt::format("waiting {:.0f}% in {}", frame.idlePercent, fit(frame.waitingIn, 22))
                                              : fmt::format("the game {:.0f}%", frame.gamePercent);
        if (!frame.function.empty())
          who += "  " + fit(shortName(frame.function), 40);
        text += fmt::format("\n  {}  {:>5.0f} ms  {}", frame.when, frame.millis, who);
      }
      return text;
    }

    // The picked mod's hooks and functions by their share of its own time
    std::string detail(profiler::Snapshot const& snapshot, Window const& window)
    {
      auto index = snapshot.buckets.find(m_picked);
      if (index < 0)
        return "";
      auto bucket = static_cast<uint32_t>(index);
      auto const& profile = window.profile;
      auto own = profile.at(bucket).own;
      if (own == 0)
        return fmt::format("{}: no own time", snapshot.buckets[bucket].name);

      auto nameOf = [&](uintptr_t address)
      {
        auto name = snapshot.names.find(address);
        return name != snapshot.names.end() ? name->second : fmt::format("{:#x}", address);
      };
      std::string text = fmt::format("{}: {} samples of its own", fit(snapshot.buckets[bucket].name, 30), own);
      int lines = 1;
      auto list = [&](char const* title, Profile::Places const& places, size_t limit)
      {
        auto top = topPlaces(places, bucket, limit);
        if (top.empty())
          return;
        text += fmt::format("\n{}", title);
        lines++;
        for (auto const& place : top)
        {
          text += fmt::format("\n  {:>3.0f}%  {}", 100.0 * place.samples / own, fit(shortName(nameOf(place.address)), 52));
          lines++;
        }
      };
      // Its zones first (ModProfiler.hpp): what the mod named itself, in ms a frame
      auto const& mod = snapshot.buckets[bucket].id;
      std::vector<std::pair<uint64_t, std::string>> zoned;
      for (auto const& [id, samples] : profile.zones())
      {
        auto name = zones::registry().nameOf(id);
        if (name.mod == mod)
          zoned.push_back({samples, name.name});
      }
      if (!zoned.empty())
      {
        std::sort(zoned.begin(), zoned.end(), std::greater<>());
        text += "\nIn its zones";
        lines++;
        for (size_t i = 0; i < zoned.size() && i < 3; i++)
        {
          auto millis = window.frames ? 1000.0 * zoned[i].first / profile.samples() * window.seconds / window.frames : 0;
          text += fmt::format("\n  {:.2f} ms  {}", millis, fit(zoned[i].second, 48));
          lines++;
        }
      }
      list("Under its hooks of", profile.hooks(), 3);
      list("In its functions", profile.functions(), static_cast<size_t>(std::max(1, DETAIL_LINES - lines - 1)));
      return text;
    }

    CCLabelBMFont* m_title = nullptr;
    CCLabelBMFont* m_summary = nullptr;
    CCLabelBMFont* m_compare = nullptr;
    CCLabelBMFont* m_detail = nullptr;
    CCLabelBMFont* m_slow = nullptr;
    CCLabelBMFont* m_baseHeader = nullptr;
    CCLabelBMFont* m_viewButton = nullptr;
    CCLabelBMFont* m_baselineButton = nullptr;
    CCLabelBMFont* m_clearButton = nullptr;
    CCMenuItemLabel* m_clearItem = nullptr;
    CCMenu* m_menu = nullptr;
    std::vector<Row> m_rows;
    // The mods in the rows, by ID, and the picked one
    std::vector<std::string> m_shown;
    std::string m_picked;
    bool m_clicked = false;
    // The last seconds, the whole session, or the game's start
    enum class View
    {
      Recent,
      Scene,
      Session,
      Startup,
    };
    View m_view = View::Recent;
    CCLabelBMFont* m_ownHeader = nullptr;
  };

  Overlay* s_overlay = nullptr;

  // In the corner and at the size the settings say
  void place()
  {
    if (!s_overlay)
      return;
    auto mod = Mod::get();
    auto corner = mod->getSettingValue<std::string>("table-corner");
    bool right = corner.ends_with("right");
    bool bottom = corner.starts_with("Bottom");
    auto size = CCDirector::get()->getWinSize();
    s_overlay->setScale(static_cast<float>(mod->getSettingValue<double>("table-scale")));
    s_overlay->setAnchorPoint({right ? 1.f : 0.f, bottom ? 0.f : 1.f});
    s_overlay->setPosition({right ? size.width - MARGIN : MARGIN, bottom ? MARGIN : size.height - MARGIN});
  }
} // namespace

void overlay::setup()
{
  if (s_overlay)
    return;
  s_overlay = Overlay::create();
  s_overlay->retain();
  s_overlay->setZOrder(10000);
  s_overlay->setVisible(false);
  OverlayManager::get()->addChild(s_overlay);
  place();
}

void overlay::setVisible(bool visible)
{
  if (!s_overlay)
    return;
  place();
  s_overlay->setVisible(visible);
  profiler::setNamesWanted(visible);
  if (visible)
    s_overlay->refresh(0);
  Mod::get()->setSavedValue("overlay-visible", visible);
}

void overlay::toggle() { setVisible(!visible()); }

bool overlay::visible() { return s_overlay && s_overlay->isVisible(); }

void overlay::applySettings() { place(); }

void overlay::saveProfile(bool startup)
{
  Notification::create(startup ? "Saving the game's start..." : "Saving the last 10 seconds...", NotificationIcon::Loading, 1.5f)->show();
  profiler::requestExport(startup, [](std::string const& file, std::string const& error)
                          {
                            if (!error.empty())
                            {
                              Notification::create("Not saved: " + error, NotificationIcon::Error)->show();
                              return;
                            }
                            log::info("Saved the profile to {}, open it at https://www.speedscope.app", file);
                            Notification::create("Saved: open it at speedscope.app (the folder is in the log)", NotificationIcon::Success, 4.f)->show(); });
}
