#include "BaselineFile.hpp"

#include "Profiler.hpp"

#include <Geode/Geode.hpp>

using namespace geode::prelude;

namespace
{
  Baseline s_baseline;

  std::filesystem::path path() { return Mod::get()->getSaveDir() / "baseline.json"; }
} // namespace

Baseline const& baseline_file::current() { return s_baseline; }

void baseline_file::load()
{
  auto text = file::readString(path());
  if (!text)
    return;
  auto json = matjson::parse(*text);
  if (!json)
  {
    log::warn("The saved baseline can't be read: {}", json.unwrapErr());
    return;
  }
  auto const& value = json.unwrap();
  Baseline baseline{
      .saved = value["saved"].asString().unwrapOr(""),
      .seconds = value["seconds"].asDouble().unwrapOr(0),
      .fps = value["fps"].asDouble().unwrapOr(0),
      .frameMillis = value["frame-ms"].asDouble().unwrapOr(0),
      .busyMillis = value["work-ms"].asDouble().unwrapOr(0),
  };
  if (auto mods = value["mods"]; mods.isObject())
    for (auto const& [id, mod] : mods)
      baseline.mods[id] = {
          .name = mod["name"].asString().unwrapOr(id),
          .ownMillis = mod["own-ms"].asDouble().unwrapOr(0),
          .inclusivePercent = mod["inclusive-percent"].asDouble().unwrapOr(0),
      };
  s_baseline = std::move(baseline);
  profiler::setBaseline(s_baseline);
}

void baseline_file::save(Baseline baseline)
{
  auto mods = matjson::Value::object();
  for (auto const& [id, mod] : baseline.mods)
    mods[id] = matjson::makeObject({
        {"name", mod.name},
        {"own-ms", mod.ownMillis},
        {"inclusive-percent", mod.inclusivePercent},
    });
  auto json = matjson::makeObject({
      {"saved", baseline.saved},
      {"seconds", baseline.seconds},
      {"fps", baseline.fps},
      {"frame-ms", baseline.frameMillis},
      {"work-ms", baseline.busyMillis},
      {"mods", mods},
  });
  if (auto result = file::writeString(path(), json.dump()); !result)
    log::warn("The baseline couldn't be saved: {}", result.unwrapErr());
  s_baseline = std::move(baseline);
  profiler::setBaseline(s_baseline);
}

void baseline_file::clear()
{
  std::error_code code;
  std::filesystem::remove(path(), code);
  s_baseline = {};
  profiler::setBaseline(s_baseline);
}
