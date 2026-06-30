// --- Hyprchroma Shader Plugin ---

#include <algorithm>
#include <any>
#include <chrono>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>

#define private public
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/Shader.hpp>
#include <hyprland/src/render/ShaderLoader.hpp>
#undef private

#include <hyprutils/memory/SharedPtr.hpp>

using CHyprOpenGLImpl = Render::GL::CHyprOpenGLImpl;
using CTexture = Render::ITexture;

static HANDLE pHandle = nullptr;

static CFunctionHook *g_getShaderVariantHook = nullptr;
static CFunctionHook *g_useShaderHook = nullptr;
static CFunctionHook *g_renderTextureInternalHook = nullptr;
static CFunctionHook *g_renderTextureWithBlurInternalHook = nullptr;

static CHyprSignalListener g_configListener;
static CHyprSignalListener g_destroyWindowListener;

static SP<SHyprCtlCommand> g_probeCommand;
static SP<SHyprCtlCommand> g_probeCommandCompat;

struct SConfig {
  float r = 0.20f;
  float g = 0.70f;
  float b = 1.00f;
  float strength = 0.04f;
  float protectBrights = 1.00f;
  float brightThreshold = 0.82f;
  float brightKnee = 0.12f;
  float protectSaturated = 0.85f;
  float saturationThreshold = 0.20f;
  float saturationKnee = 0.16f;
  int debugVisualize = 0;
  bool enableOnFullscreen = true;
  bool tintAllSurfaces = true;
};

struct SUniforms {
  GLint tintColor = -1;
  GLint tintStrength = -1;
  GLint protectBrights = -1;
  GLint brightThreshold = -1;
  GLint brightKnee = -1;
  GLint protectSaturated = -1;
  GLint saturationThreshold = -1;
  GLint saturationKnee = -1;
  GLint debugVisualize = -1;
};

struct SVariant {
  SP<CShader> shader;
  SUniforms uniforms;
  Render::ePreparedFragmentShader frag = Render::SH_FRAG_LAST;
  Render::ShaderFeatureFlags features = 0;
};

static SConfig g_config;
static bool g_globalShaded = true;
static std::set<void *> g_perWindowShaded;
static std::map<std::pair<Render::ePreparedFragmentShader, Render::ShaderFeatureFlags>, SVariant>
    g_variants;
static std::set<std::pair<Render::ePreparedFragmentShader, Render::ShaderFeatureFlags>>
    g_failedVariants;

static WP<CTexture> g_blurredBackground;
static bool g_ignoreTextureRender = true;
static bool g_loggedNativePath = false;
static bool g_loggedVariantFailure = false;
static bool g_loggedShaderHook = false;
static std::string g_lastReject = "not evaluated";
static size_t g_surfaceVariantHits = 0;
static size_t g_extVariantHits = 0;
static size_t g_uniformPasses = 0;

static auto &renderData() { return g_pHyprRenderer->m_renderData; }

static std::string replaceAll(std::string source, const std::string &from,
                              const std::string &to) {
  size_t pos = 0;
  while ((pos = source.find(from, pos)) != std::string::npos) {
    source.replace(pos, from.size(), to);
    pos += to.size();
  }
  return source;
}

static bool insertBefore(std::string &source, const std::string &needle,
                         const std::string &payload) {
  const auto pos = source.find(needle);
  if (pos == std::string::npos)
    return false;
  source.insert(pos, payload);
  return true;
}

static bool insertAfter(std::string &source, const std::string &needle,
                        const std::string &payload) {
  const auto pos = source.find(needle);
  if (pos == std::string::npos)
    return false;
  source.insert(pos + needle.size(), payload);
  return true;
}

static bool replaceOnce(std::string &source, const std::string &needle,
                        const std::string &payload) {
  const auto pos = source.find(needle);
  if (pos == std::string::npos)
    return false;
  source.replace(pos, needle.size(), payload);
  return true;
}

static const char *hyprchromaGLSL() {
  return R"glsl(
uniform vec3 hyprchromaTintColor;
uniform float hyprchromaTintStrength;
uniform float hyprchromaProtectBrights;
uniform float hyprchromaBrightThreshold;
uniform float hyprchromaBrightKnee;
uniform float hyprchromaProtectSaturated;
uniform float hyprchromaSaturationThreshold;
uniform float hyprchromaSaturationKnee;
uniform int hyprchromaDebugVisualize;

vec4 hyprchromaApply(vec4 basePremul, vec4 analysisColor) {
    float contentAlpha = clamp(analysisColor.a, 0.0, 1.0);
    if (basePremul.a <= 0.001 || contentAlpha <= 0.001)
        return basePremul;

    vec3 analysisRgb = analysisColor.rgb;
    float luminance = dot(analysisRgb, vec3(0.2126, 0.7152, 0.0722));
    float maxChannel = max(analysisRgb.r, max(analysisRgb.g, analysisRgb.b));
    float minChannel = min(analysisRgb.r, min(analysisRgb.g, analysisRgb.b));
    float saturation = maxChannel - minChannel;

    float brightProtect = hyprchromaProtectBrights * smoothstep(
        hyprchromaBrightThreshold - hyprchromaBrightKnee,
        hyprchromaBrightThreshold + hyprchromaBrightKnee,
        luminance
    );
    float saturatedProtect = hyprchromaProtectSaturated * smoothstep(
        hyprchromaSaturationThreshold - hyprchromaSaturationKnee,
        hyprchromaSaturationThreshold + hyprchromaSaturationKnee,
        saturation
    );
    float preserve = clamp(max(brightProtect, saturatedProtect), 0.0, 1.0);
    float darkness = 1.0 - pow(clamp(luminance, 0.0, 1.0), 1.35);
    float overlayAlpha = clamp(
        hyprchromaTintStrength * (1.0 - preserve) * darkness * basePremul.a,
        0.0,
        1.0
    );

    if (hyprchromaDebugVisualize == 1)
        return vec4(vec3(luminance), basePremul.a);
    if (hyprchromaDebugVisualize == 2)
        return vec4(vec3(saturation), basePremul.a);
    if (hyprchromaDebugVisualize == 3)
        return vec4(vec3(preserve), basePremul.a);
    if (hyprchromaDebugVisualize == 4)
        return vec4(vec3(overlayAlpha), basePremul.a);
    if (hyprchromaDebugVisualize == 5)
        return vec4(analysisRgb * basePremul.a, basePremul.a);

    return vec4(
        hyprchromaTintColor * overlayAlpha + basePremul.rgb * (1.0 - overlayAlpha),
        overlayAlpha + basePremul.a * (1.0 - overlayAlpha)
    );
}

)glsl";
}

static std::optional<std::string>
patchSurfaceShaderSource(std::string source) {
  if (!insertBefore(source, "layout(location = 0) out vec4 fragColor;",
                    hyprchromaGLSL()))
    return std::nullopt;

  bool capturedAnalysis = false;
  capturedAnalysis |=
      insertAfter(source, "vec4 pixColor = texture(tex, v_texcoord);",
                  "\n    vec4 hyprchromaAnalysisColor = pixColor;");
  capturedAnalysis |= insertAfter(
      source, "vec4 pixColor = vec4(texture(tex, v_texcoord).rgb, 1.0);",
      "\n    vec4 hyprchromaAnalysisColor = pixColor;");
  if (!capturedAnalysis)
    return std::nullopt;

  const std::string apply =
      "\n    pixColor = hyprchromaApply(pixColor, hyprchromaAnalysisColor);\n";
  if (!insertBefore(source, "#if USE_BLUR", apply) &&
      !insertBefore(source, "fragColor = pixColor;", apply))
    return std::nullopt;

  if (source.find("mirrorColor = pixColor;") != std::string::npos) {
    source = replaceAll(
        source, "mirrorColor = pixColor;",
        "mirrorColor = hyprchromaApply(pixColor, hyprchromaAnalysisColor);");
  }

  return source;
}

static std::optional<std::string> patchExtShaderSource(std::string source) {
  if (!insertBefore(source, "layout(location = 0) out vec4 fragColor;",
                    hyprchromaGLSL()))
    return std::nullopt;

  if (!insertAfter(source, "vec4 pixColor = texture(tex, v_texcoord);",
                   "\n    vec4 hyprchromaAnalysisColor = pixColor;"))
    return std::nullopt;

  if (!replaceOnce(
          source, "fragColor = pixColor * alpha;",
          "fragColor = hyprchromaApply(pixColor * alpha, hyprchromaAnalysisColor);"))
    return std::nullopt;

  return source;
}

static SUniforms queryUniforms(GLuint program) {
  SUniforms uniforms;
  uniforms.tintColor = glGetUniformLocation(program, "hyprchromaTintColor");
  uniforms.tintStrength =
      glGetUniformLocation(program, "hyprchromaTintStrength");
  uniforms.protectBrights =
      glGetUniformLocation(program, "hyprchromaProtectBrights");
  uniforms.brightThreshold =
      glGetUniformLocation(program, "hyprchromaBrightThreshold");
  uniforms.brightKnee = glGetUniformLocation(program, "hyprchromaBrightKnee");
  uniforms.protectSaturated =
      glGetUniformLocation(program, "hyprchromaProtectSaturated");
  uniforms.saturationThreshold =
      glGetUniformLocation(program, "hyprchromaSaturationThreshold");
  uniforms.saturationKnee =
      glGetUniformLocation(program, "hyprchromaSaturationKnee");
  uniforms.debugVisualize =
      glGetUniformLocation(program, "hyprchromaDebugVisualize");
  return uniforms;
}

static void passUniforms(const SUniforms &uniforms) {
  if (uniforms.tintColor != -1)
    glUniform3f(uniforms.tintColor, g_config.r, g_config.g, g_config.b);
  if (uniforms.tintStrength != -1)
    glUniform1f(uniforms.tintStrength, g_config.strength);
  if (uniforms.protectBrights != -1)
    glUniform1f(uniforms.protectBrights, g_config.protectBrights);
  if (uniforms.brightThreshold != -1)
    glUniform1f(uniforms.brightThreshold, g_config.brightThreshold);
  if (uniforms.brightKnee != -1)
    glUniform1f(uniforms.brightKnee, g_config.brightKnee);
  if (uniforms.protectSaturated != -1)
    glUniform1f(uniforms.protectSaturated, g_config.protectSaturated);
  if (uniforms.saturationThreshold != -1)
    glUniform1f(uniforms.saturationThreshold, g_config.saturationThreshold);
  if (uniforms.saturationKnee != -1)
    glUniform1f(uniforms.saturationKnee, g_config.saturationKnee);
  if (uniforms.debugVisualize != -1)
    glUniform1i(uniforms.debugVisualize, g_config.debugVisualize);
  ++g_uniformPasses;
}

static float getCfgFloat(const std::string &key, float fallback) {
  auto *cv = HyprlandAPI::getConfigValue(pHandle, key);
  if (!cv || !cv->dataPtr())
    return fallback;
  return *static_cast<Hyprlang::FLOAT *>(cv->dataPtr());
}

static int getCfgInt(const std::string &key, int fallback) {
  auto *cv = HyprlandAPI::getConfigValue(pHandle, key);
  if (!cv || !cv->dataPtr())
    return fallback;
  return *static_cast<Hyprlang::INT *>(cv->dataPtr());
}

static void updateConfig() {
  g_config.r = getCfgFloat("plugin:darkwindow:tint_r", 0.20f);
  g_config.g = getCfgFloat("plugin:darkwindow:tint_g", 0.70f);
  g_config.b = getCfgFloat("plugin:darkwindow:tint_b", 1.00f);
  g_config.strength =
      getCfgFloat("plugin:darkwindow:tint_strength", 0.040f);
  g_config.protectBrights =
      getCfgFloat("plugin:darkwindow:protect_brights", 1.00f);
  g_config.brightThreshold =
      getCfgFloat("plugin:darkwindow:bright_threshold", 0.82f);
  g_config.brightKnee =
      getCfgFloat("plugin:darkwindow:bright_knee", 0.12f);
  g_config.protectSaturated =
      getCfgFloat("plugin:darkwindow:protect_saturated", 0.85f);
  g_config.saturationThreshold =
      getCfgFloat("plugin:darkwindow:saturation_threshold", 0.20f);
  g_config.saturationKnee =
      getCfgFloat("plugin:darkwindow:saturation_knee", 0.16f);
  g_config.debugVisualize =
      getCfgInt("plugin:darkwindow:debug_visualize", 0);
  g_config.enableOnFullscreen =
      getCfgInt("plugin:darkwindow:enable_on_fullscreen", 1);
  g_config.tintAllSurfaces =
      getCfgInt("plugin:darkwindow:tint_all_surfaces", 1);
}

static bool isShaded(const PHLWINDOW &window) {
  if (!window)
    return false;
  if (g_perWindowShaded.contains((void *)window.get()))
    return true;
  return g_globalShaded;
}

static bool shouldShadeCurrentRender() {
  updateConfig();

  if (g_ignoreTextureRender) {
    g_lastReject = "ignored texture render";
    return false;
  }

  if (g_config.strength <= 0.0f) {
    g_lastReject = "zero tint strength";
    return false;
  }

  const auto window = renderData().currentWindow.lock();
  if (!window) {
    g_lastReject = "no current window";
    return false;
  }

  if (!isShaded(window)) {
    g_lastReject = "window not shaded";
    return false;
  }

  if (!g_config.enableOnFullscreen && window->isFullscreen()) {
    g_lastReject = "fullscreen blocked";
    return false;
  }

  if (window->isHidden()) {
    g_lastReject = "hidden window";
    return false;
  }

  if (!g_config.tintAllSurfaces) {
    const auto surface = renderData().surface.lock();
    if (surface && surface != window->resource()) {
      g_lastReject = "subsurface blocked";
      return false;
    }
  }

  g_lastReject = "accepted";
  return true;
}

static std::optional<SVariant>
compileVariant(CHyprOpenGLImpl *self, Render::ePreparedFragmentShader frag,
               Render::ShaderFeatureFlags features) {
  if (!self || !self->m_shaders || self->m_shaders->TEXVERTSRC.empty() ||
      !Render::g_pShaderLoader) {
    g_lastReject = "shader loader unavailable";
    return std::nullopt;
  }

  std::string original = Render::g_pShaderLoader->getVariantSource(frag, features);
  std::optional<std::string> patched;

  if (frag == Render::SH_FRAG_SURFACE)
    patched = patchSurfaceShaderSource(std::move(original));
  else if (frag == Render::SH_FRAG_EXT)
    patched = patchExtShaderSource(std::move(original));
  else
    return std::nullopt;

  if (!patched) {
    g_lastReject = "shader source patch failed";
    return std::nullopt;
  }

  auto shader = makeShared<CShader>();
  if (!shader->createProgram(self->m_shaders->TEXVERTSRC, *patched, true, true)) {
    g_lastReject = "shader compile failed";
    if (!g_loggedVariantFailure) {
      Log::logger->log(
          Log::ERR,
          "[Hyprchroma] Failed to compile native shader variant frag={} "
          "features={}",
          (int)frag, features);
      g_loggedVariantFailure = true;
    }
    return std::nullopt;
  }

  SVariant variant;
  variant.shader = shader;
  variant.uniforms = queryUniforms(shader->program());
  variant.frag = frag;
  variant.features = features;
  return variant;
}

static const SVariant *findVariant(const WP<CShader> &shader) {
  if (!shader)
    return nullptr;

  for (const auto &[_, variant] : g_variants) {
    if (variant.shader && shader == variant.shader)
      return &variant;
  }

  return nullptr;
}

using PGetShaderVariantFn =
    WP<CShader> (*)(CHyprOpenGLImpl *, Render::ePreparedFragmentShader,
                    Render::ShaderFeatureFlags);
using PUseShaderFn = WP<CShader> (*)(CHyprOpenGLImpl *, WP<CShader>);
using PRenderTextureInternalFn =
    void (*)(CHyprOpenGLImpl *, SP<CTexture>, const CBox &,
             const CHyprOpenGLImpl::STextureRenderData &);

static WP<CShader> hkGetShaderVariant(CHyprOpenGLImpl *self,
                                      Render::ePreparedFragmentShader frag,
                                      Render::ShaderFeatureFlags features) {
  const auto original =
      reinterpret_cast<PGetShaderVariantFn>(g_getShaderVariantHook->m_original);

  if (frag != Render::SH_FRAG_SURFACE && frag != Render::SH_FRAG_EXT)
    return original(self, frag, features);

  if (!shouldShadeCurrentRender())
    return original(self, frag, features);

  const auto key = std::make_pair(frag, features);
  if (const auto it = g_variants.find(key); it != g_variants.end()) {
    if (frag == Render::SH_FRAG_SURFACE)
      ++g_surfaceVariantHits;
    else
      ++g_extVariantHits;
    return it->second.shader;
  }

  if (g_failedVariants.contains(key))
    return original(self, frag, features);

  auto variant = compileVariant(self, frag, features);
  if (!variant) {
    g_failedVariants.insert(key);
    return original(self, frag, features);
  }

  const auto [inserted, _] = g_variants.emplace(key, std::move(*variant));
  if (frag == Render::SH_FRAG_SURFACE)
    ++g_surfaceVariantHits;
  else
    ++g_extVariantHits;

  if (!g_loggedNativePath) {
    Log::logger->log(Log::INFO,
                     "[Hyprchroma] Native surface shader path active "
                     "(getShaderVariant / SH_FRAG_SURFACE)");
    g_loggedNativePath = true;
  }

  return inserted->second.shader;
}

static WP<CShader> hkUseShader(CHyprOpenGLImpl *self, WP<CShader> shader) {
  const auto original =
      reinterpret_cast<PUseShaderFn>(g_useShaderHook->m_original);
  auto used = original(self, shader);

  if (const auto *variant = findVariant(used); variant && shouldShadeCurrentRender())
    passUniforms(variant->uniforms);

  return used;
}

static void hkRenderTextureInternal(
    CHyprOpenGLImpl *self, SP<CTexture> tex, const CBox &box,
    const CHyprOpenGLImpl::STextureRenderData &data) {
  const auto original = reinterpret_cast<PRenderTextureInternalFn>(
      g_renderTextureInternalHook->m_original);

  g_ignoreTextureRender = g_blurredBackground == tex;
  original(self, std::move(tex), box, data);
  g_ignoreTextureRender = true;
}

static void hkRenderTextureWithBlurInternal(
    CHyprOpenGLImpl *self, SP<CTexture> tex, const CBox &box,
    const CHyprOpenGLImpl::STextureRenderData &data) {
  const auto original = reinterpret_cast<PRenderTextureInternalFn>(
      g_renderTextureWithBlurInternalHook->m_original);

  g_blurredBackground = data.blurredBG;
  original(self, std::move(tex), box, data);
  g_blurredBackground.reset();
}

static bool installHook(const std::string &method, const std::string &prefix,
                        const void *destination, CFunctionHook *&slot) {
  auto matches = HyprlandAPI::findFunctionsByName(pHandle, method);
  auto found = std::find_if(matches.begin(), matches.end(),
                            [&](const SFunctionMatch &match) {
                              return match.demangled.starts_with(prefix);
                            });

  if (found == matches.end()) {
    Log::logger->log(Log::ERR, "[Hyprchroma] Failed to find hook target {}",
                     prefix);
    return false;
  }

  slot = HyprlandAPI::createFunctionHook(pHandle, found->address, destination);
  if (!slot || !slot->hook()) {
    Log::logger->log(Log::ERR, "[Hyprchroma] Failed to activate hook {}",
                     prefix);
    if (slot) {
      HyprlandAPI::removeFunctionHook(pHandle, slot);
      slot = nullptr;
    }
    return false;
  }

  return true;
}

static void redrawAll() {
  for (auto &monitor : g_pCompositor->m_monitors) {
    g_pHyprRenderer->damageMonitor(monitor);
    g_pCompositor->scheduleFrameForMonitor(monitor);
  }
}

static SDispatchResult shadeDispatcher(std::string args) {
  if (args.find("address:") != std::string::npos) {
    PHLWINDOW target = nullptr;
    const auto space = args.find(' ');
    std::string addr =
        args.substr(8, (space == std::string::npos ? args.length() : space) - 8);
    addr.erase(0, addr.find_first_not_of(" \t\n\r"));
    addr.erase(addr.find_last_not_of(" \t\n\r") + 1);

    for (auto &window : g_pCompositor->m_windows) {
      std::ostringstream full;
      full << "0x" << std::hex << (uintptr_t)window.get();
      std::ostringstream shortAddr;
      shortAddr << std::hex << (uintptr_t)window.get();
      if (full.str() == addr || shortAddr.str() == addr) {
        target = window;
        break;
      }
    }

    if (!target)
      return {false, false, "Window not found"};

    if (g_perWindowShaded.contains((void *)target.get()))
      g_perWindowShaded.erase((void *)target.get());
    else
      g_perWindowShaded.insert((void *)target.get());
  } else {
    g_perWindowShaded.clear();
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

static std::string probeText() {
  std::ostringstream out;
  out << "Hyprchroma native shader probe\n";
  out << "version: 4.0.0-v055-native\n";
  out << "getShaderVariant_hook: " << (g_getShaderVariantHook ? "yes" : "no")
      << "\n";
  out << "useShader_hook: " << (g_useShaderHook ? "yes" : "no") << "\n";
  out << "renderTextureInternal_hook: "
      << (g_renderTextureInternalHook ? "yes" : "no") << "\n";
  out << "renderTextureWithBlurInternal_hook: "
      << (g_renderTextureWithBlurInternalHook ? "yes" : "no") << "\n";
  out << "compiled_variants: " << g_variants.size() << "\n";
  out << "failed_variants: " << g_failedVariants.size() << "\n";
  out << "surface_variant_hits: " << g_surfaceVariantHits << "\n";
  out << "ext_variant_hits: " << g_extVariantHits << "\n";
  out << "uniform_passes: " << g_uniformPasses << "\n";
  out << "global_shaded: " << (g_globalShaded ? "yes" : "no") << "\n";
  out << "per_window_overrides: " << g_perWindowShaded.size() << "\n";
  out << "last_decision: " << g_lastReject << "\n";
  return out.str();
}

static std::string probeJson() {
  std::ostringstream out;
  out << "{";
  out << "\"version\":\"4.0.0-v055-native\",";
  out << "\"hooks\":{";
  out << "\"getShaderVariant\":" << (g_getShaderVariantHook ? "true" : "false")
      << ",";
  out << "\"useShader\":" << (g_useShaderHook ? "true" : "false") << ",";
  out << "\"renderTextureInternal\":"
      << (g_renderTextureInternalHook ? "true" : "false") << ",";
  out << "\"renderTextureWithBlurInternal\":"
      << (g_renderTextureWithBlurInternalHook ? "true" : "false");
  out << "},";
  out << "\"compiled_variants\":" << g_variants.size() << ",";
  out << "\"failed_variants\":" << g_failedVariants.size() << ",";
  out << "\"surface_variant_hits\":" << g_surfaceVariantHits << ",";
  out << "\"ext_variant_hits\":" << g_extVariantHits << ",";
  out << "\"uniform_passes\":" << g_uniformPasses << ",";
  out << "\"global_shaded\":" << (g_globalShaded ? "true" : "false") << ",";
  out << "\"per_window_overrides\":" << g_perWindowShaded.size() << ",";
  out << "\"last_decision\":\"" << g_lastReject << "\"";
  out << "}";
  return out.str();
}

APICALL EXPORT std::string pluginAPIVersion() { return HYPRLAND_API_VERSION; }

APICALL EXPORT PLUGIN_DESCRIPTION_INFO pluginInit(HANDLE handle) {
  pHandle = handle;

  const std::string compositorHash = __hyprland_api_get_hash();
  const std::string clientHash = __hyprland_api_get_client_hash();
  if (compositorHash != clientHash) {
    HyprlandAPI::addNotification(
        handle,
        "[Hyprchroma] Version mismatch between plugin headers and compositor",
        CHyprColor(1.f, 0.2f, 0.1f, 1.f), 8000);
    throw std::runtime_error(std::format(
        "[Hyprchroma] version mismatch, built against {}, running {}",
        clientHash, compositorHash));
  }

  HyprlandAPI::addConfigValue(handle, "plugin:darkwindow:tint_r",
                              Hyprlang::FLOAT{0.20f});
  HyprlandAPI::addConfigValue(handle, "plugin:darkwindow:tint_g",
                              Hyprlang::FLOAT{0.70f});
  HyprlandAPI::addConfigValue(handle, "plugin:darkwindow:tint_b",
                              Hyprlang::FLOAT{1.00f});
  HyprlandAPI::addConfigValue(handle, "plugin:darkwindow:tint_strength",
                              Hyprlang::FLOAT{0.040f});
  HyprlandAPI::addConfigValue(handle, "plugin:darkwindow:protect_brights",
                              Hyprlang::FLOAT{1.00f});
  HyprlandAPI::addConfigValue(handle, "plugin:darkwindow:bright_threshold",
                              Hyprlang::FLOAT{0.82f});
  HyprlandAPI::addConfigValue(handle, "plugin:darkwindow:bright_knee",
                              Hyprlang::FLOAT{0.12f});
  HyprlandAPI::addConfigValue(handle, "plugin:darkwindow:protect_saturated",
                              Hyprlang::FLOAT{0.85f});
  HyprlandAPI::addConfigValue(handle,
                              "plugin:darkwindow:saturation_threshold",
                              Hyprlang::FLOAT{0.20f});
  HyprlandAPI::addConfigValue(handle, "plugin:darkwindow:saturation_knee",
                              Hyprlang::FLOAT{0.16f});
  HyprlandAPI::addConfigValue(handle, "plugin:darkwindow:debug_visualize",
                              Hyprlang::INT{0});
  HyprlandAPI::addConfigValue(handle,
                              "plugin:darkwindow:enable_on_fullscreen",
                              Hyprlang::INT{1});
  HyprlandAPI::addConfigValue(handle, "plugin:darkwindow:tint_all_surfaces",
                              Hyprlang::INT{1});

  // Backward-compatible key for existing configs.
  HyprlandAPI::addConfigValue(handle,
                              "plugin:darkwindow:native_surface_shader_pass",
                              Hyprlang::INT{1});
  HyprlandAPI::addConfigValue(handle,
                              "plugin:darkwindow:unified_window_pass",
                              Hyprlang::INT{0});
  HyprlandAPI::addConfigValue(handle,
                              "plugin:darkwindow:cursor_invalidation_mode",
                              Hyprlang::INT{0});
  HyprlandAPI::addConfigValue(
      handle, "plugin:darkwindow:cursor_invalidation_throttle_ms",
      Hyprlang::INT{0});
  HyprlandAPI::addConfigValue(handle,
                              "plugin:darkwindow:cursor_invalidation_radius",
                              Hyprlang::INT{48});
  HyprlandAPI::addConfigValue(
      handle, "plugin:darkwindow:suspend_on_workspace_switch_ms",
      Hyprlang::INT{0});

  updateConfig();

  g_configListener =
      Event::bus()->m_events.config.reloaded.listen([]() { updateConfig(); });
  g_destroyWindowListener = Event::bus()->m_events.window.destroy.listen(
      [](PHLWINDOW window) { g_perWindowShaded.erase((void *)window.get()); });

  const bool hookedGetShaderVariant =
      installHook("getShaderVariant",
                  "Render::GL::CHyprOpenGLImpl::getShaderVariant(",
                  reinterpret_cast<void *>(hkGetShaderVariant),
                  g_getShaderVariantHook);
  const bool hookedUseShader =
      installHook("useShader", "Render::GL::CHyprOpenGLImpl::useShader(",
                  reinterpret_cast<void *>(hkUseShader), g_useShaderHook);
  const bool hookedRenderTextureInternal = installHook(
      "renderTextureInternal",
      "Render::GL::CHyprOpenGLImpl::renderTextureInternal(",
      reinterpret_cast<void *>(hkRenderTextureInternal),
      g_renderTextureInternalHook);
  const bool hookedRenderTextureWithBlurInternal = installHook(
      "renderTextureWithBlurInternal",
      "Render::GL::CHyprOpenGLImpl::renderTextureWithBlurInternal(",
      reinterpret_cast<void *>(hkRenderTextureWithBlurInternal),
      g_renderTextureWithBlurInternalHook);

  if (!hookedGetShaderVariant || !hookedUseShader ||
      !hookedRenderTextureInternal || !hookedRenderTextureWithBlurInternal)
    throw std::runtime_error("[Hyprchroma] failed to install native hooks");

  if (!g_loggedShaderHook) {
    Log::logger->log(Log::INFO,
                     "[Hyprchroma] Installed native Hyprland 0.55 shader hooks");
    g_loggedShaderHook = true;
  }

  HyprlandAPI::addDispatcherV2(handle, "togglechromakey",
                               [](std::string args) -> SDispatchResult {
                                 return shadeDispatcher(args);
                               });
  HyprlandAPI::addDispatcherV2(handle, "darkwindow:shade",
                               [](std::string args) -> SDispatchResult {
                                 return shadeDispatcher(args);
                               });

  g_probeCommand = HyprlandAPI::registerHyprCtlCommand(
      handle, {.name = "hyprchromaprobe",
               .exact = true,
               .fn =
                   [](eHyprCtlOutputFormat format, std::string) {
                     return format == FORMAT_JSON ? probeJson() : probeText();
                   }});
  g_probeCommandCompat = HyprlandAPI::registerHyprCtlCommand(
      handle, {.name = "darkwindowprobe2",
               .exact = true,
               .fn =
                   [](eHyprCtlOutputFormat format, std::string) {
                     return format == FORMAT_JSON ? probeJson() : probeText();
                   }});

  HyprlandAPI::addNotification(
      handle, "[Hyprchroma] Native surface shader path loaded",
      CHyprColor(0.15f, 0.9f, 0.25f, 1.f), 3000);

  return {"DarkWindow", "Native adaptive per-pixel surface tint", "tco",
          "4.0.0-v055-native"};
}

APICALL EXPORT void pluginExit() {
  if (g_probeCommand) {
    HyprlandAPI::unregisterHyprCtlCommand(pHandle, g_probeCommand);
    g_probeCommand.reset();
  }
  if (g_probeCommandCompat) {
    HyprlandAPI::unregisterHyprCtlCommand(pHandle, g_probeCommandCompat);
    g_probeCommandCompat.reset();
  }

  if (g_getShaderVariantHook) {
    HyprlandAPI::removeFunctionHook(pHandle, g_getShaderVariantHook);
    g_getShaderVariantHook = nullptr;
  }
  if (g_useShaderHook) {
    HyprlandAPI::removeFunctionHook(pHandle, g_useShaderHook);
    g_useShaderHook = nullptr;
  }
  if (g_renderTextureInternalHook) {
    HyprlandAPI::removeFunctionHook(pHandle, g_renderTextureInternalHook);
    g_renderTextureInternalHook = nullptr;
  }
  if (g_renderTextureWithBlurInternalHook) {
    HyprlandAPI::removeFunctionHook(pHandle,
                                    g_renderTextureWithBlurInternalHook);
    g_renderTextureWithBlurInternalHook = nullptr;
  }

  g_configListener.reset();
  g_destroyWindowListener.reset();
  g_variants.clear();
  g_failedVariants.clear();
  g_perWindowShaded.clear();
  pHandle = nullptr;
}
