#pragma once

// With Improved Logger installed: the numbers pinned in its console, and its markers (Ctrl+Shift+M
// in the game) cut the session into spans, each one summed up in the log. Without it, nothing
namespace logger_link
{
  // Call on the main thread once the game is loaded
  void setup();
  void setWatching(bool watching);
} // namespace logger_link
