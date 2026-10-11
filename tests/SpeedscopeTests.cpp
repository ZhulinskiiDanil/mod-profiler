// Tests for src/Speedscope.cpp: the profile file for speedscope.app
#include "../src/Speedscope.hpp"
#include "Check.hpp"

static bool contains(std::string const& text, char const* part) { return text.find(part) != std::string::npos; }

static void testEscape()
{
  CHECK(speedscope::escape("plain") == "plain");
  CHECK(speedscope::escape("a\"b") == "a\\\"b");
  CHECK(speedscope::escape("C:\\path") == "C:\\\\path");
  CHECK(speedscope::escape("line\nnext\t") == "line\\nnext\\t");
  CHECK(speedscope::escape(std::string("\x01", 1)) == "\\u0001");
  // UTF-8 goes through as it is
  CHECK(speedscope::escape("привет") == "привет");
  CHECK(speedscope::escape("`anonymous namespace'::f") == "`anonymous namespace'::f");
}

static void testJson()
{
  speedscope::Profile profile;
  profile.name = "Main thread";
  profile.frames = {{"main", "GeometryDash.exe"}, {"Hair::update", "zhulis.icon-mayhem.dll"}, {"hook handler", ""}};
  profile.stacks = {{0, 1}, {0}, {0, 2, 1}};
  profile.weights = {1.0, 1.5, 0.5};
  auto json = speedscope::json(profile);
  CHECK(contains(json, R"("$schema":"https://www.speedscope.app/file-format-schema.json")"));
  CHECK(contains(json, R"("frames":[{"name":"main","file":"GeometryDash.exe"},{"name":"Hair::update","file":"zhulis.icon-mayhem.dll"},{"name":"hook handler"}])"));
  CHECK(contains(json, R"("type":"sampled","name":"Main thread","unit":"milliseconds","startValue":0,"endValue":3.000)"));
  CHECK(contains(json, R"("samples":[[0,1],[0],[0,2,1]])"));
  CHECK(contains(json, R"("weights":[1.000,1.500,0.500])"));
  // Balanced, the way a parser needs it
  int depth = 0;
  for (char c : json)
    depth += c == '{' || c == '[' ? 1 : c == '}' || c == ']' ? -1 : 0;
  CHECK(depth == 0);
}

static void testEmpty()
{
  auto json = speedscope::json({});
  CHECK(contains(json, R"("samples":[])"));
  CHECK(contains(json, R"("weights":[])"));
  CHECK(contains(json, R"("endValue":0.000)"));
}

int main()
{
  testEscape();
  testJson();
  testEmpty();
  return finish("speedscope");
}
