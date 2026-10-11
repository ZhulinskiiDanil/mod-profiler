#include "Overlay.hpp"

#include "Profiler.hpp"

#include <Geode/Geode.hpp>
#include <Geode/ui/OverlayManager.hpp>

using namespace geode::prelude;

namespace
{
  constexpr int ROWS = 10;
  constexpr int DETAIL_LINES = 9;
  constexpr float WIDTH = 205;
  constexpr float LINE = 6.5f;
  constexpr float MARGIN = 4;
  constexpr float FONT_SCALE = 0.3f;
  // Right edges of the number columns
  constexpr float OWN_MS_X = 135;
  constexpr float OWN_X = 167;
  constexpr float INCLUSIVE_X = WIDTH - MARGIN;

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
      auto height = MARGIN * 2 + LINE * (3 + ROWS + 1 + DETAIL_LINES);
      setContentSize({WIDTH, height});
      setAnchorPoint({0, 1});
      setID("mod-profiler-overlay"_spr);

      auto background = CCLayerColor::create({0, 0, 0, 175}, WIDTH, height);
      addChild(background);

      float y = height - MARGIN;
      m_title = label(this, MARGIN, y, {0, 1}, {255, 210, 90});
      y -= LINE;
      m_summary = label(this, MARGIN, y, {0, 1}, {190, 190, 200});
      y -= LINE;
      label(this, MARGIN, y, {0, 1}, {150, 150, 160})->setString("Mod");
      label(this, OWN_MS_X, y, {1, 1}, {150, 150, 160})->setString("own ms");
      label(this, OWN_X, y, {1, 1}, {150, 150, 160})->setString("own %");
      label(this, INCLUSIVE_X, y, {1, 1}, {150, 150, 160})->setString("incl %");
      y -= LINE;

      m_menu = CCMenu::create();
      m_menu->setPosition({0, 0});
      m_menu->setContentSize(getContentSize());
      addChild(m_menu);

      for (int i = 0; i < ROWS; i++)
      {
        Row row;
        row.name = label(this, MARGIN, y, {0, 1});
        row.ownMillis = label(this, OWN_MS_X, y, {1, 1}, {120, 220, 120});
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

      schedule(schedule_selector(Overlay::refresh), 0.5f);
      refresh(0);
      return true;
    }

    void refresh(float)
    {
      if (!isVisible())
        return;
      auto snapshot = profiler::snapshot();
      auto const& window = snapshot.recent;
      auto const& profile = window.profile;
      auto samples = profile.samples();

      auto fps = window.seconds > 0 ? window.frames / window.seconds : 0;
      auto frameMillis = window.frames ? window.seconds * 1000 / window.frames : 0;
      m_title->setString(fmt::format("Mod Profiler   {:.0f} fps, {:.2f} ms a frame", fps, frameMillis).c_str());
      if (samples == 0)
      {
        m_summary->setString("No samples yet");
        for (auto& row : m_rows)
          clearRow(row);
        m_detail->setString("");
        return;
      }
      auto between = 100.0 * profile.between() / samples;
      auto idle = 100.0 * profile.idle() / samples;
      m_summary->setString(fmt::format("in frames {:.0f}%, between {:.0f}%, idle {:.0f}%   {:.0f} samples/s, stop {:.0f} us",
                                       std::max(0.0, 100 - between - idle), between, idle, window.stats.samples / window.seconds,
                                       window.stats.suspendAverageMicros())
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
        row.name->setString(fit((picked ? "> " : "") + bucket.name, 34).c_str());
        row.name->setColor(picked ? ccColor3B{255, 210, 90} : ccColor3B{255, 255, 255});
        row.ownMillis->setString(fmt::format("{:.2f}", share.ownMillisPerFrame).c_str());
        row.own->setString(fmt::format("{:.1f}", share.ownPercent).c_str());
        row.inclusive->setString(fmt::format("{:.1f}", share.inclusivePercent).c_str());
        row.button->setEnabled(true);
      }
      // Until a row is clicked, the busiest mod is the picked one
      if ((m_picked.empty() || !m_clicked) && !m_shown.empty())
        m_picked = m_shown.front();

      m_detail->setString(detail(snapshot).c_str());
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

  private:
    struct Row
    {
      CCLabelBMFont* name = nullptr;
      CCLabelBMFont* ownMillis = nullptr;
      CCLabelBMFont* own = nullptr;
      CCLabelBMFont* inclusive = nullptr;
      CCMenuItem* button = nullptr;
    };

    void clearRow(Row& row)
    {
      row.name->setString("");
      row.ownMillis->setString("");
      row.own->setString("");
      row.inclusive->setString("");
      row.button->setEnabled(false);
    }

    // The picked mod's hooks and functions by their share of its own time
    std::string detail(profiler::Snapshot const& snapshot)
    {
      auto index = snapshot.buckets.find(m_picked);
      if (index < 0)
        return "";
      auto bucket = static_cast<uint32_t>(index);
      auto const& profile = snapshot.recent.profile;
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
          text += fmt::format("\n  {:>3.0f}%  {}", 100.0 * place.samples / own, fit(nameOf(place.address), 48));
          lines++;
        }
      };
      list("Under its hooks of", profile.hooks(), 3);
      list("In its functions", profile.functions(), static_cast<size_t>(std::max(1, DETAIL_LINES - lines - 1)));
      return text;
    }

    CCLabelBMFont* m_title = nullptr;
    CCLabelBMFont* m_summary = nullptr;
    CCLabelBMFont* m_detail = nullptr;
    CCMenu* m_menu = nullptr;
    std::vector<Row> m_rows;
    // The mods in the rows, by ID, and the picked one
    std::vector<std::string> m_shown;
    std::string m_picked;
    bool m_clicked = false;
  };

  Overlay* s_overlay = nullptr;

  void place()
  {
    if (!s_overlay)
      return;
    auto size = CCDirector::get()->getWinSize();
    s_overlay->setPosition({MARGIN, size.height - MARGIN});
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
  if (visible)
    s_overlay->refresh(0);
  Mod::get()->setSavedValue("overlay-visible", visible);
}

void overlay::toggle() { setVisible(!visible()); }

bool overlay::visible() { return s_overlay && s_overlay->isVisible(); }
