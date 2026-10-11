#include "Speedscope.hpp"

#include <cstdio>

std::string speedscope::escape(std::string_view text)
{
  std::string out;
  out.reserve(text.size() + 2);
  for (unsigned char c : text)
  {
    switch (c)
    {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (c < 0x20)
      {
        char code[8];
        std::snprintf(code, sizeof(code), "\\u%04x", c);
        out += code;
      }
      else
        out += static_cast<char>(c);
    }
  }
  return out;
}

std::string speedscope::json(Profile const& profile)
{
  std::string out = R"({"$schema":"https://www.speedscope.app/file-format-schema.json","exporter":"Mod Profiler",)";
  out += "\"name\":\"" + escape(profile.name) + "\",\"activeProfileIndex\":0,\"shared\":{\"frames\":[";
  for (size_t i = 0; i < profile.frames.size(); i++)
  {
    auto const& frame = profile.frames[i];
    out += i ? "," : "";
    out += "{\"name\":\"" + escape(frame.name) + "\"";
    if (!frame.file.empty())
      out += ",\"file\":\"" + escape(frame.file) + "\"";
    out += "}";
  }

  double total = 0;
  for (auto weight : profile.weights)
    total += weight;
  char number[32];
  std::snprintf(number, sizeof(number), "%.3f", total);
  out += "]},\"profiles\":[{\"type\":\"sampled\",\"name\":\"" + escape(profile.name) +
         "\",\"unit\":\"milliseconds\",\"startValue\":0,\"endValue\":" + number + ",\"samples\":[";
  for (size_t i = 0; i < profile.stacks.size(); i++)
  {
    out += i ? ",[" : "[";
    auto const& stack = profile.stacks[i];
    for (size_t j = 0; j < stack.size(); j++)
    {
      out += j ? "," : "";
      out += std::to_string(stack[j]);
    }
    out += "]";
  }
  out += "],\"weights\":[";
  for (size_t i = 0; i < profile.weights.size(); i++)
  {
    std::snprintf(number, sizeof(number), "%s%.3f", i ? "," : "", profile.weights[i]);
    out += number;
  }
  out += "]}]}";
  return out;
}
