// libhypr-darkwindow.so — Hyprchroma port for Hyprland v0.54.2
//
// Version 1.11:
//  - Frame-synchronized rendering (RENDER_POST_WINDOW)
//  - Render-local guard to prevent double-exposure
//  - Improved floating window tracking

#define WLR_USE_UNSTABLE

#include <algorithm>
#include <any>
#include <format>
#include <set>
#include <string>
#include <vector>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/SharedDefs.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/defines.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>
#include <hyprland/src/helpers/signal/Signal.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprlang.hpp>
#include <hyprutils/math/Box.hpp>
#include <hyprutils/math/Vector2D.hpp>

using namespace Desktop::View;

static HANDLE pHandle = nullptr;
static CHyprSignalListener g_renderListener;
static CHyprSignalListener g_configListener;
static CHyprSignalListener g_destroyWindowListener;

// Glass state
static bool g_globalShaded = true;
static std::set<void *> g_perWindowShaded;

// Render-local guard (reset every frame)
static std::set<void *> g_renderedThisFrame;

struct SConfig {
  float r, g, b, a;
  bool enable_on_fullscreen;
} g_config;

static float getCfgFloat(const std::string &key, float fallback) {
  auto *cv = HyprlandAPI::getConfigValue(pHandle, key);
  if (!cv)
    return fallback;
  return std::any_cast<Hyprlang::FLOAT>(cv->getValue());
}

static int getCfgInt(const std::string &key, int fallback) {
  auto *cv = HyprlandAPI::getConfigValue(pHandle, key);
  if (!cv)
    return fallback;
  return std::any_cast<Hyprlang::INT>(cv->getValue());
}

static void updateConfig() {
  g_config.r = getCfgFloat("plugin:darkwindow:tint_r", 0.20f);
  g_config.g = getCfgFloat("plugin:darkwindow:tint_g", 0.70f);
  g_config.b = getCfgFloat("plugin:darkwindow:tint_b", 1.00f);
  g_config.a = getCfgFloat("plugin:darkwindow:tint_strength", 0.040f);
  g_config.enable_on_fullscreen =
      getCfgInt("plugin:darkwindow:enable_on_fullscreen", 1);
}

static bool isShaded(PHLWINDOW pWindow) {
  if (!pWindow)
    return false;
  if (g_perWindowShaded.contains((void *)pWindow.get()))
    return true;
  return g_globalShaded;
}

static void onRenderStage(eRenderStage stage) {
  if (stage == RENDER_BEGIN) {
    g_renderedThisFrame.clear();
    return;
  }

  // Use POST_WINDOW for tight synchronization with the window's own render pass
  if (stage != RENDER_POST_WINDOW)
    return;

  if (g_config.a <= 0.0f)
    return;

  auto window = g_pHyprOpenGL->m_renderData.currentWindow.lock();
  if (!window || !isShaded(window))
    return;

  if (!g_config.enable_on_fullscreen && window->isFullscreen())
    return;

  // ── Ghost-layer fix: skip windows not on a visible workspace ──
  auto monitor = g_pHyprOpenGL->m_renderData.pMonitor.lock();
  if (!monitor)
    return;

  if (window->isHidden())
    return;

  auto wksp = window->m_workspace;
  if (!wksp || !wksp->m_visible)
    return;

  // Render-local guard: only once per window per frame (prevents
  // double-exposure on active win)
  if (g_renderedThisFrame.contains((void *)window.get()))
    return;
  g_renderedThisFrame.insert((void *)window.get());

  const float scale = monitor->m_scale;
  const auto logBox = window->getWindowMainSurfaceBox();

  // Apply workspace animation offset so overlay slides with the window
  auto renderOffset = wksp->m_renderOffset->value();

  CRectPassElement::SRectData data;
  data.box = CBox((logBox.x + renderOffset.x - monitor->m_position.x) * scale,
                  (logBox.y + renderOffset.y - monitor->m_position.y) * scale,
                  logBox.w * scale, logBox.h * scale);

  // Sync alpha with window opacity for smooth animations (fade-in/out)
  float windowAlpha =
      window->m_alpha->value() * window->m_activeInactiveAlpha->value();
  data.color =
      CHyprColor(g_config.r, g_config.g, g_config.b, g_config.a * windowAlpha);
  data.round = static_cast<int>(window->rounding() * scale);
  data.roundingPower = window->roundingPower();

  g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(data));
}

static void redrawAll() {
  for (auto &m : g_pCompositor->m_monitors) {
    g_pHyprRenderer->damageMonitor(m);
    g_pCompositor->scheduleFrameForMonitor(m);
  }
}

static SDispatchResult shadeDispatcher(std::string args) {
  if (args.find("address:") != std::string::npos) {
    PHLWINDOW target = nullptr;
    size_t space = args.find(' ');
    std::string addrStr = args.substr(
        8, (space == std::string::npos ? args.length() : space) - 8);

    addrStr.erase(0, addrStr.find_first_not_of(" \t\n\r"));
    addrStr.erase(addrStr.find_last_not_of(" \t\n\r") + 1);

    for (auto &w : g_pCompositor->m_windows) {
      if (std::format("0x{:x}", (uintptr_t)w.get()) == addrStr ||
          std::format("{:x}", (uintptr_t)w.get()) == addrStr) {
        target = w;
        break;
      }
    }

    if (!target) {
      HyprlandAPI::addNotification(pHandle,
                                   "[DarkWindow] Error: Window not found",
                                   CHyprColor(1.f, 0.f, 0.f, 1.f), 3000);
      return {false, false, "Window not found"};
    }

    if (g_perWindowShaded.contains((void *)target.get()))
      g_perWindowShaded.erase((void *)target.get());
    else
      g_perWindowShaded.insert((void *)target.get());

    HyprlandAPI::addNotification(pHandle,
                                 "[DarkWindow] Per-window shade toggled",
                                 CHyprColor(0.f, 1.f, 1.f, 1.f), 1000);
  } else {
    if (args.find("on") != std::string::npos)
      g_globalShaded = true;
    else if (args.find("off") != std::string::npos)
      g_globalShaded = false;
    else
      g_globalShaded = !g_globalShaded;
  }

  redrawAll();
  return {};
}

APICALL EXPORT std::string pluginAPIVersion() { return HYPRLAND_API_VERSION; }

APICALL EXPORT PLUGIN_DESCRIPTION_INFO pluginInit(HANDLE handle) {
  pHandle = handle;

  HyprlandAPI::addConfigValue(handle, "plugin:darkwindow:tint_r",
                              Hyprlang::FLOAT{0.20f});
  HyprlandAPI::addConfigValue(handle, "plugin:darkwindow:tint_g",
                              Hyprlang::FLOAT{0.70f});
  HyprlandAPI::addConfigValue(handle, "plugin:darkwindow:tint_b",
                              Hyprlang::FLOAT{1.00f});
  HyprlandAPI::addConfigValue(handle, "plugin:darkwindow:tint_strength",
                              Hyprlang::FLOAT{0.040f});
  HyprlandAPI::addConfigValue(handle, "plugin:darkwindow:enable_on_fullscreen",
                              Hyprlang::INT{1});

  updateConfig();

  g_renderListener = Event::bus()->m_events.render.stage.listen(
      [](eRenderStage stage) { onRenderStage(stage); });

  g_configListener =
      Event::bus()->m_events.config.reloaded.listen([]() { updateConfig(); });

  g_destroyWindowListener = Event::bus()->m_events.window.destroy.listen(
      [](PHLWINDOW w) { g_perWindowShaded.erase((void *)w.get()); });

  HyprlandAPI::addDispatcherV2(handle, "togglechromakey",
                               [](std::string args) -> SDispatchResult {
                                 return shadeDispatcher(args);
                               });

  HyprlandAPI::addDispatcherV2(handle, "darkwindow:shade",
                               [](std::string args) -> SDispatchResult {
                                 return shadeDispatcher(args);
                               });

  HyprlandAPI::addNotification(handle,
                               "[DarkWindow] Registered v1.11 (Floating Fix)",
                               CHyprColor(0.f, 1.f, 0.f, 1.f), 3000);

  return {"DarkWindow", "Per-window glass tint overlay", "tco", "1.11"};
}

APICALL EXPORT void pluginExit() {
  g_renderListener.reset();
  g_configListener.reset();
  g_destroyWindowListener.reset();
  g_perWindowShaded.clear();
  redrawAll();
}
