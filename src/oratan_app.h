// oratan - Virtual-On Oratorio Tangram (XBLA) static recompilation
//
// ReXApp subclass: boot defaults and window setup.

#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/rex_app.h>
#include <rex/system/xmemory.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>

#include <cstdlib>
#include <string>

#include "branding.h"
#include "debug_tools.h"
#include "dual_monitor.h"
#include "fullscreen_toggle.h"
#include "game_files.h"
#include "options_menu.h"
#include "relaunch.h"

class OratanApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  // The app name is the folder in Documents that holds settings and saves,
  // and the title of error dialogs.
  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<OratanApp>(new OratanApp(ctx, oratan::kAppTitle,
        PPCImageConfig));
  }

  // Boot defaults. These run before the user's config file is loaded, so
  // anything set there still wins.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    // No --game_data_root: find the game files (extracted, or the raw XBLA
    // package, which is unpacked on first launch) so double-clicking works.
    if (paths.game_data_root.empty()) {
      std::string error;
      paths.game_data_root =
          oratan::LocateGameFiles(rex::filesystem::GetExecutableFolder(), &error);
      if (paths.game_data_root.empty()) {
        oratan::ShowError(error);
        std::exit(1);
      }
    }
    config_path_ = paths.config_path;
    // XBLA titles run as the trial unless the full-game license bit is set.
    rex::cvar::SetFlagByName("license_mask", "1");
    // Xbox LIVE modes on the LAN: the game's rich presence is 0 on the title
    // screen, 7 in the main and Multiplayer menus, 2 in System Link, 4 from
    // the Player Match menu on (its menu and every online lobby) and 3 once in
    // an online match (it stays 3 on the results and back in the Player Match
    // menu). The LAN party is the players at 3 or 4; a party invite brings in
    // a player in the Player Match menu or a lobby of their own, and the last
    // party match's players from anywhere in the online modes.
    rex::cvar::SetFlagByName("lan_party_presence", "3,4");
    rex::cvar::SetFlagByName("lan_invite_presence", "4");
  }

  // Use the Xenos GPU emulation plugin (rexgpu-xenos.dll) unless the user
  // picked something else with --gpu_plugin.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (config.gpu_plugin.empty()) config.gpu_plugin = "xenos";
  }

  void OnPostLoadXexImage() override {
    oratan::DumpGuestImageIfRequested(runtime());
    oratan::InstallCrashReport();
  }

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    if (!window()) return;
    window()->SetTitle(oratan::kWindowTitle);
    fullscreen_toggle_.Attach(window(), config_path_);
    oratan::RememberStartupSettings();
    oratan::AttachDualMonitorWindow(window());
    if (rex::cvar::Query<bool>("split_screen") && rex::cvar::Query<int>("split_display") == 2) {
      oratan::ApplyDualMonitorWindow(true);
    }
    // Developer: F9 saves a memory snapshot next to the exe (snapshots/).
    rex::ui::RegisterBind("bind_snapshot", "F9", "Save a memory snapshot (developer)", [this] {
      if (!runtime() || !runtime()->memory()) return;
      const auto dir = rex::filesystem::GetExecutableFolder() / "snapshots";
      std::filesystem::create_directories(dir);
      oratan::SaveSnapshot(runtime()->memory()->TranslateVirtual(0), dir.string());
    });
    rex::ui::RegisterBind("bind_options", "F1", "Toggle the Options window", [this, drawer] {
      if (options_) {
        options_.reset();
      } else {
        options_ = std::make_unique<oratan::OptionsDialog>(
            drawer, config_path_, [this](const oratan::RelaunchArgs& args) {
              // Settings are already saved; start a fresh copy, then close this
              // one the same way the window's X button does (a direct app quit
              // leaves the process hung while the game is running).
              if (oratan::LaunchNewInstance(args) && window()) window()->RequestClose();
            });
      }
    });
  }

  void OnShutdown() override {
    rex::ui::UnregisterBind("bind_options");
    rex::ui::UnregisterBind("bind_snapshot");
    options_.reset();
    oratan::AttachDualMonitorWindow(nullptr);
    fullscreen_toggle_.Detach();
  }

 private:
  std::filesystem::path config_path_;
  oratan::FullscreenToggle fullscreen_toggle_;
  std::unique_ptr<oratan::OptionsDialog> options_;
};
