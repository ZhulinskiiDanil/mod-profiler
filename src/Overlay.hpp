#pragma once

// The table in the game: every mod's own time a frame, its share, and for the picked mod the hooks
// and functions it spends it in. Above every scene, shown and hidden with a key
namespace overlay
{
  // Call on the main thread once the game is loaded
  void setup();
  void toggle();
  void setVisible(bool visible);
  bool visible();
  // The last 10 seconds (or the game's start) to a file for speedscope.app, with a notification of how it went
  void saveProfile(bool startup = false);
} // namespace overlay
