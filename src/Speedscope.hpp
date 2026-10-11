#pragma once

// A profile file for https://www.speedscope.app, without Windows or Geode in it: its "sampled" format,
// one stack and one weight for every sample
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace speedscope
{
  struct Frame
  {
    std::string name;
    // The module, shown as the frame's file
    std::string file;
  };

  struct Profile
  {
    std::string name;
    std::vector<Frame> frames;
    // Indices into `frames`, from the root of the stack to where the thread was
    std::vector<std::vector<uint32_t>> stacks;
    // How long each stack stands for, in milliseconds
    std::vector<double> weights;
  };

  std::string escape(std::string_view text);
  std::string json(Profile const& profile);
} // namespace speedscope
