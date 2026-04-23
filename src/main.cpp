// libhypr-darkwindow.so — Hyprchroma v3.4.0 for Hyprland v0.54.2
//
// Per-pixel luminance-based chromakey tint overlay.
// Samples window surface texture to vary tint alpha: strong on dark pixels,
// near-zero on bright pixels. Falls back to uniform CRectPassElement when
// surface texture is unavailable.

#include <algorithm>
#include <any>
#include <chrono>
#include <format>
#include <map>
#include <set>
#include <sstream>
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
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprlang.hpp>
#include <hyprutils/math/Box.hpp>
#include <hyprutils/math/Vector2D.hpp>

using namespace Desktop::View;

static HANDLE pHandle = nullptr;
static CHyprSignalListener g_renderListener;
static CHyprSignalListener g_configListener;
static CHyprSignalListener g_destroyWindowListener;
static CHyprSignalListener g_workspaceListener;
static CHyprSignalListener g_mouseMoveListener;
static SP<SHyprCtlCommand> g_runtimeProbeCommand;

// Glass state
static bool g_globalShaded = true;
static std::set<void *> g_perWindowShaded;

// Render-local guard (reset every frame)
static std::set<void *> g_renderedThisFrame;
static std::set<void *> g_nativeShadedThisFrame;

struct SConfig {
  float r, g, b, a;
  float protect_brights;
  float bright_threshold;
  float bright_knee;
  float protect_saturated;
  float saturation_threshold;
  float saturation_knee;
  int debug_visualize;
  bool enable_on_fullscreen;
  bool tint_all_surfaces;
  bool unified_window_pass;
  bool native_surface_shader_pass;
  int cursor_invalidation_mode;
  int cursor_invalidation_throttle_ms;
  int cursor_invalidation_radius;
  int suspend_on_workspace_switch_ms;
} g_config;

static std::chrono::steady_clock::time_point g_suspendUntil =
    std::chrono::steady_clock::time_point::min();
static std::chrono::steady_clock::time_point g_lastCursorInvalidation =
    std::chrono::steady_clock::time_point::min();
static bool g_wasSuspendedLastFrame = false;
static bool g_runtimeProbeSafeForLowerLevel = false;

static void redrawAll();

struct SRuntimeSymbolProbe {
  std::string query;
  std::string demangledFilter;
  std::vector<SFunctionMatch> matches;
};

struct SRuntimeProbeReport {
  SVersionInfo runtimeVersion;
  bool hashMatchesBuild = false;
  bool tagMatchesBuild = false;
  bool modernRenderAPIHeadersPresent = true;
  bool eventBusRenderStagePresent = true;
  bool preWindowStagePresent = true;
  bool postWindowStagePresent = true;
  bool currentWindowRenderDataPresent = true;
  bool legacyShaderSwapABIAvailable = false;
  SRuntimeSymbolProbe useShader;
  SRuntimeSymbolProbe getSurfaceShader;
  SRuntimeSymbolProbe renderTexture;
  SRuntimeSymbolProbe renderTextureInternalWithDamage;
  SRuntimeSymbolProbe decorationGetDataFor;
  bool supportsModernShaderInsertion = false;
  bool supportsDecorationHook = false;
  bool safeForLowerLevelPrototype = false;
  std::string recommendation;
};

struct SNativeShaderUniforms {
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

struct SNativeShaderVariant {
  SP<CShader> shader;
  SNativeShaderUniforms uniforms;
};

// ── v3 shader state ──

static GLuint g_chromaProgram = 0;
static GLuint g_chromaProgram_ext = 0;
static GLuint g_blitProgram = 0;
static GLuint g_blitProgram_ext = 0;
static GLuint g_chromaVAO = 0;
static GLuint g_chromaVBO = 0;
static bool g_shadersCompiled = false;
static bool g_loggedShaderInit = false;
static bool g_loggedShaderPath = false;
static bool g_loggedUnifiedPath = false;
static bool g_loggedNativeShaderPath = false;
static bool g_loggedNativeHooks = false;
static bool g_loggedNativeFlagBlocked = false;
static bool g_loggedFallbackNoSurface = false;
static bool g_loggedFallbackNoTexture = false;
static bool g_loggedFallbackNoExternalProgram = false;
static bool g_loggedUnifiedFallbackNoExternalProgram = false;
static bool g_loggedCursorInvalidationMode = false;
static bool g_notifiedShaderDebugPath = false;
static bool g_notifiedFallbackDebugPath = false;
static bool g_notifiedSurfaceDebugCount = false;
static CFunctionHook *g_getSurfaceShaderHook = nullptr;
static CFunctionHook *g_useShaderHook = nullptr;
static std::map<uint8_t, SNativeShaderVariant> g_nativeSurfaceShaders;
static std::set<uint8_t> g_nativeSurfaceShaderFailures;
static SNativeShaderVariant g_nativeExtShader;
static bool g_nativeExtShaderCompileAttempted = false;

// Uniform locations — sampler2D variant
static GLint g_loc_proj = -1;
static GLint g_loc_windowTex = -1;
static GLint g_loc_tintColor = -1;
static GLint g_loc_tintStrength = -1;
static GLint g_loc_windowAlpha = -1;
static GLint g_loc_topLeft = -1;
static GLint g_loc_fullSize = -1;
static GLint g_loc_radius = -1;
static GLint g_loc_roundingPower = -1;
static GLint g_loc_uvTopLeft = -1;
static GLint g_loc_uvBottomRight = -1;
static GLint g_loc_protectBrights = -1;
static GLint g_loc_brightThreshold = -1;
static GLint g_loc_brightKnee = -1;
static GLint g_loc_protectSaturated = -1;
static GLint g_loc_saturationThreshold = -1;
static GLint g_loc_saturationKnee = -1;
static GLint g_loc_debugVisualize = -1;

// Uniform locations — samplerExternalOES variant
static GLint g_loc_ext_proj = -1;
static GLint g_loc_ext_windowTex = -1;
static GLint g_loc_ext_tintColor = -1;
static GLint g_loc_ext_tintStrength = -1;
static GLint g_loc_ext_windowAlpha = -1;
static GLint g_loc_ext_topLeft = -1;
static GLint g_loc_ext_fullSize = -1;
static GLint g_loc_ext_radius = -1;
static GLint g_loc_ext_roundingPower = -1;
static GLint g_loc_ext_uvTopLeft = -1;
static GLint g_loc_ext_uvBottomRight = -1;
static GLint g_loc_ext_protectBrights = -1;
static GLint g_loc_ext_brightThreshold = -1;
static GLint g_loc_ext_brightKnee = -1;
static GLint g_loc_ext_protectSaturated = -1;
static GLint g_loc_ext_saturationThreshold = -1;
static GLint g_loc_ext_saturationKnee = -1;
static GLint g_loc_ext_debugVisualize = -1;

// Uniform locations — blit sampler2D variant
static GLint g_loc_blit_targetSize = -1;
static GLint g_loc_blit_quadTopLeft = -1;
static GLint g_loc_blit_quadSize = -1;
static GLint g_loc_blit_windowTex = -1;
static GLint g_loc_blit_uvTopLeft = -1;
static GLint g_loc_blit_uvBottomRight = -1;
static GLint g_loc_blit_opacity = -1;

// Uniform locations — blit samplerExternalOES variant
static GLint g_loc_blit_ext_targetSize = -1;
static GLint g_loc_blit_ext_quadTopLeft = -1;
static GLint g_loc_blit_ext_quadSize = -1;
static GLint g_loc_blit_ext_windowTex = -1;
static GLint g_loc_blit_ext_uvTopLeft = -1;
static GLint g_loc_blit_ext_uvBottomRight = -1;
static GLint g_loc_blit_ext_opacity = -1;

// ── GLSL shaders ──

static const char *CHROMA_VERT_SRC = R"(
#version 300 es
precision highp float;

uniform mat3 proj;

in vec2 pos;
in vec2 texcoord;

out vec2 v_texcoord;

void main() {
    gl_Position = vec4(proj * vec3(pos, 1.0), 1.0);
    v_texcoord = texcoord;
}
)";

static const char *CHROMA_FRAG_SRC = R"(
#version 300 es
precision highp float;

in vec2 v_texcoord;

uniform sampler2D windowTex;
uniform vec3 tintColor;
uniform float tintStrength;
uniform float windowAlpha;

uniform float radius;
uniform float roundingPower;
uniform vec2 topLeft;
uniform vec2 fullSize;
uniform vec2 uvTopLeft;
uniform vec2 uvBottomRight;
uniform float protectBrights;
uniform float brightThreshold;
uniform float brightKnee;
uniform float protectSaturated;
uniform float saturationThreshold;
uniform float saturationKnee;
uniform int debugVisualize;

layout(location = 0) out vec4 fragColor;

void main() {
    vec2 sampleUV = mix(uvTopLeft, uvBottomRight, v_texcoord);
    vec4 windowPixel = texture(windowTex, sampleUV);
    float contentAlpha = windowPixel.a;
    if (contentAlpha <= 0.001)
        discard;
    float luminance = dot(windowPixel.rgb, vec3(0.2126, 0.7152, 0.0722));
    float maxChannel = max(windowPixel.r, max(windowPixel.g, windowPixel.b));
    float minChannel = min(windowPixel.r, min(windowPixel.g, windowPixel.b));
    float saturation = maxChannel - minChannel;

    float brightProtect = protectBrights * smoothstep(
        brightThreshold - brightKnee,
        brightThreshold + brightKnee,
        luminance
    );
    float saturatedProtect = protectSaturated * smoothstep(
        saturationThreshold - saturationKnee,
        saturationThreshold + saturationKnee,
        saturation
    );
    float preserve = clamp(max(brightProtect, saturatedProtect), 0.0, 1.0);
    float alpha = tintStrength * (1.0 - preserve) * (1.0 - luminance) * windowAlpha * contentAlpha;
    alpha = clamp(alpha, 0.0, 1.0);

    if (debugVisualize == 1) {
        fragColor = vec4(vec3(luminance), 1.0);
        return;
    }
    if (debugVisualize == 2) {
        fragColor = vec4(vec3(saturation), 1.0);
        return;
    }
    if (debugVisualize == 3) {
        fragColor = vec4(vec3(preserve), 1.0);
        return;
    }
    if (debugVisualize == 4) {
        fragColor = vec4(vec3(alpha), 1.0);
        return;
    }
    if (debugVisualize == 5) {
        fragColor = vec4(windowPixel.rgb, 1.0);
        return;
    }

    // Rounding (superellipse distance, matches Hyprland's rounding.glsl)
    if (radius > 0.0) {
        vec2 pixCoord = vec2(gl_FragCoord);
        pixCoord -= topLeft + fullSize * 0.5;
        pixCoord *= vec2(lessThan(pixCoord, vec2(0.0))) * -2.0 + 1.0;
        pixCoord -= fullSize * 0.5 - radius;
        pixCoord += vec2(1.0, 1.0) / fullSize;

        if (pixCoord.x + pixCoord.y > radius) {
            float dist = pow(
                pow(pixCoord.x, roundingPower) + pow(pixCoord.y, roundingPower),
                1.0 / roundingPower
            );
            float smoothingConstant = 3.14159265 / 5.34665792551;
            if (dist > radius + smoothingConstant)
                discard;
            float normalized = 1.0 - smoothstep(
                0.0, 1.0,
                (dist - radius + smoothingConstant) / (smoothingConstant * 2.0)
            );
            alpha *= normalized;
        }
    }

    // Premultiplied alpha (Hyprland blend: GL_ONE, GL_ONE_MINUS_SRC_ALPHA)
    fragColor = vec4(tintColor * alpha, alpha);
}
)";

static const char *CHROMA_FRAG_EXT_SRC = R"(
#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision highp float;

in vec2 v_texcoord;

uniform samplerExternalOES windowTex;
uniform vec3 tintColor;
uniform float tintStrength;
uniform float windowAlpha;

uniform float radius;
uniform float roundingPower;
uniform vec2 topLeft;
uniform vec2 fullSize;
uniform vec2 uvTopLeft;
uniform vec2 uvBottomRight;
uniform float protectBrights;
uniform float brightThreshold;
uniform float brightKnee;
uniform float protectSaturated;
uniform float saturationThreshold;
uniform float saturationKnee;
uniform int debugVisualize;

layout(location = 0) out vec4 fragColor;

void main() {
    vec2 sampleUV = mix(uvTopLeft, uvBottomRight, v_texcoord);
    vec4 windowPixel = texture(windowTex, sampleUV);
    float contentAlpha = windowPixel.a;
    if (contentAlpha <= 0.001)
        discard;
    float luminance = dot(windowPixel.rgb, vec3(0.2126, 0.7152, 0.0722));
    float maxChannel = max(windowPixel.r, max(windowPixel.g, windowPixel.b));
    float minChannel = min(windowPixel.r, min(windowPixel.g, windowPixel.b));
    float saturation = maxChannel - minChannel;

    float brightProtect = protectBrights * smoothstep(
        brightThreshold - brightKnee,
        brightThreshold + brightKnee,
        luminance
    );
    float saturatedProtect = protectSaturated * smoothstep(
        saturationThreshold - saturationKnee,
        saturationThreshold + saturationKnee,
        saturation
    );
    float preserve = clamp(max(brightProtect, saturatedProtect), 0.0, 1.0);
    float alpha = tintStrength * (1.0 - preserve) * (1.0 - luminance) * windowAlpha * contentAlpha;
    alpha = clamp(alpha, 0.0, 1.0);

    if (debugVisualize == 1) {
        fragColor = vec4(vec3(luminance), 1.0);
        return;
    }
    if (debugVisualize == 2) {
        fragColor = vec4(vec3(saturation), 1.0);
        return;
    }
    if (debugVisualize == 3) {
        fragColor = vec4(vec3(preserve), 1.0);
        return;
    }
    if (debugVisualize == 4) {
        fragColor = vec4(vec3(alpha), 1.0);
        return;
    }
    if (debugVisualize == 5) {
        fragColor = vec4(windowPixel.rgb, 1.0);
        return;
    }

    if (radius > 0.0) {
        vec2 pixCoord = vec2(gl_FragCoord);
        pixCoord -= topLeft + fullSize * 0.5;
        pixCoord *= vec2(lessThan(pixCoord, vec2(0.0))) * -2.0 + 1.0;
        pixCoord -= fullSize * 0.5 - radius;
        pixCoord += vec2(1.0, 1.0) / fullSize;

        if (pixCoord.x + pixCoord.y > radius) {
            float dist = pow(
                pow(pixCoord.x, roundingPower) + pow(pixCoord.y, roundingPower),
                1.0 / roundingPower
            );
            float smoothingConstant = 3.14159265 / 5.34665792551;
            if (dist > radius + smoothingConstant)
                discard;
            float normalized = 1.0 - smoothstep(
                0.0, 1.0,
                (dist - radius + smoothingConstant) / (smoothingConstant * 2.0)
            );
            alpha *= normalized;
        }
    }

    fragColor = vec4(tintColor * alpha, alpha);
}
)";

static const char *BLIT_VERT_SRC = R"(
#version 300 es
precision highp float;

uniform vec2 targetSize;
uniform vec2 quadTopLeft;
uniform vec2 quadSize;

in vec2 pos;
in vec2 texcoord;

out vec2 v_texcoord;

void main() {
    vec2 pixel = quadTopLeft + pos * quadSize;
    vec2 ndc = vec2(
        (pixel.x / targetSize.x) * 2.0 - 1.0,
        1.0 - (pixel.y / targetSize.y) * 2.0
    );
    gl_Position = vec4(ndc, 0.0, 1.0);
    v_texcoord = texcoord;
}
)";

static const char *BLIT_FRAG_SRC = R"(
#version 300 es
precision highp float;

in vec2 v_texcoord;

uniform sampler2D windowTex;
uniform vec2 uvTopLeft;
uniform vec2 uvBottomRight;
uniform float opacity;

layout(location = 0) out vec4 fragColor;

void main() {
    vec2 sampleUV = mix(uvTopLeft, uvBottomRight, v_texcoord);
    vec4 windowPixel = texture(windowTex, sampleUV);
    fragColor = windowPixel * opacity;
}
)";

static const char *BLIT_FRAG_EXT_SRC = R"(
#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision highp float;

in vec2 v_texcoord;

uniform samplerExternalOES windowTex;
uniform vec2 uvTopLeft;
uniform vec2 uvBottomRight;
uniform float opacity;

layout(location = 0) out vec4 fragColor;

void main() {
    vec2 sampleUV = mix(uvTopLeft, uvBottomRight, v_texcoord);
    vec4 windowPixel = texture(windowTex, sampleUV);
    fragColor = windowPixel * opacity;
}
)";

static const char *NATIVE_SURFACE_FRAG_SRC = R"(
#version 300 es
#extension GL_ARB_shading_language_include : enable

precision highp float;
in vec2 v_texcoord;
uniform sampler2D tex;

uniform float alpha;

#include "discard.glsl"
#include "tint.glsl"
#include "rounding.glsl"
#include "surface_CM.glsl"

uniform vec3 darkwindowTintColor;
uniform float darkwindowTintStrength;
uniform float darkwindowProtectBrights;
uniform float darkwindowBrightThreshold;
uniform float darkwindowBrightKnee;
uniform float darkwindowProtectSaturated;
uniform float darkwindowSaturationThreshold;
uniform float darkwindowSaturationKnee;
uniform int darkwindowDebugVisualize;

layout(location = 0) out vec4 fragColor;

void main() {
    #include "get_rgb_pixel.glsl"

    vec4 analysisColor = pixColor;

    #include "do_discard.glsl"
    #include "do_CM.glsl"
    #include "do_tint.glsl"
    #include "do_rounding.glsl"

    float luminance = dot(analysisColor.rgb, vec3(0.2126, 0.7152, 0.0722));
    float maxChannel = max(analysisColor.r, max(analysisColor.g, analysisColor.b));
    float minChannel = min(analysisColor.r, min(analysisColor.g, analysisColor.b));
    float saturation = maxChannel - minChannel;

    float brightProtect = darkwindowProtectBrights * smoothstep(
        darkwindowBrightThreshold - darkwindowBrightKnee,
        darkwindowBrightThreshold + darkwindowBrightKnee,
        luminance
    );
    float saturatedProtect = darkwindowProtectSaturated * smoothstep(
        darkwindowSaturationThreshold - darkwindowSaturationKnee,
        darkwindowSaturationThreshold + darkwindowSaturationKnee,
        saturation
    );
    float preserve = clamp(max(brightProtect, saturatedProtect), 0.0, 1.0);

    vec4 basePremul = pixColor * alpha;
    float overlayAlpha = darkwindowTintStrength * (1.0 - preserve) * (1.0 - luminance) * basePremul.a;
    overlayAlpha = clamp(overlayAlpha, 0.0, 1.0);

    if (darkwindowDebugVisualize == 1) {
        fragColor = vec4(vec3(luminance), 1.0);
        return;
    }
    if (darkwindowDebugVisualize == 2) {
        fragColor = vec4(vec3(saturation), 1.0);
        return;
    }
    if (darkwindowDebugVisualize == 3) {
        fragColor = vec4(vec3(preserve), 1.0);
        return;
    }
    if (darkwindowDebugVisualize == 4) {
        fragColor = vec4(vec3(overlayAlpha), 1.0);
        return;
    }
    if (darkwindowDebugVisualize == 5) {
        fragColor = vec4(analysisColor.rgb, 1.0);
        return;
    }

    fragColor = vec4(
        darkwindowTintColor * overlayAlpha + basePremul.rgb * (1.0 - overlayAlpha),
        overlayAlpha + basePremul.a * (1.0 - overlayAlpha)
    );
}
)";

static const char *NATIVE_EXT_FRAG_SRC = R"(
#version 300 es
#extension GL_ARB_shading_language_include : enable
#extension GL_OES_EGL_image_external_essl3 : require

precision highp float;
in vec2 v_texcoord;
uniform samplerExternalOES tex;
uniform float alpha;

#include "rounding.glsl"

uniform int discardOpaque;
uniform int discardAlpha;
uniform float discardAlphaValue;

uniform int applyTint;
uniform vec3 tint;

uniform vec3 darkwindowTintColor;
uniform float darkwindowTintStrength;
uniform float darkwindowProtectBrights;
uniform float darkwindowBrightThreshold;
uniform float darkwindowBrightKnee;
uniform float darkwindowProtectSaturated;
uniform float darkwindowSaturationThreshold;
uniform float darkwindowSaturationKnee;
uniform int darkwindowDebugVisualize;

layout(location = 0) out vec4 fragColor;

void main() {
    vec4 pixColor = texture(tex, v_texcoord);
    vec4 analysisColor = pixColor;

    if (discardOpaque == 1 && pixColor.a * alpha == 1.0)
        discard;

    if (discardAlpha == 1 && pixColor.a <= discardAlphaValue)
        discard;

    if (applyTint == 1)
        pixColor.rgb *= tint;

    if (radius > 0.0)
        pixColor = rounding(pixColor);

    float luminance = dot(analysisColor.rgb, vec3(0.2126, 0.7152, 0.0722));
    float maxChannel = max(analysisColor.r, max(analysisColor.g, analysisColor.b));
    float minChannel = min(analysisColor.r, min(analysisColor.g, analysisColor.b));
    float saturation = maxChannel - minChannel;

    float brightProtect = darkwindowProtectBrights * smoothstep(
        darkwindowBrightThreshold - darkwindowBrightKnee,
        darkwindowBrightThreshold + darkwindowBrightKnee,
        luminance
    );
    float saturatedProtect = darkwindowProtectSaturated * smoothstep(
        darkwindowSaturationThreshold - darkwindowSaturationKnee,
        darkwindowSaturationThreshold + darkwindowSaturationKnee,
        saturation
    );
    float preserve = clamp(max(brightProtect, saturatedProtect), 0.0, 1.0);

    vec4 basePremul = pixColor * alpha;
    float overlayAlpha = darkwindowTintStrength * (1.0 - preserve) * (1.0 - luminance) * basePremul.a;
    overlayAlpha = clamp(overlayAlpha, 0.0, 1.0);

    if (darkwindowDebugVisualize == 1) {
        fragColor = vec4(vec3(luminance), 1.0);
        return;
    }
    if (darkwindowDebugVisualize == 2) {
        fragColor = vec4(vec3(saturation), 1.0);
        return;
    }
    if (darkwindowDebugVisualize == 3) {
        fragColor = vec4(vec3(preserve), 1.0);
        return;
    }
    if (darkwindowDebugVisualize == 4) {
        fragColor = vec4(vec3(overlayAlpha), 1.0);
        return;
    }
    if (darkwindowDebugVisualize == 5) {
        fragColor = vec4(analysisColor.rgb, 1.0);
        return;
    }

    fragColor = vec4(
        darkwindowTintColor * overlayAlpha + basePremul.rgb * (1.0 - overlayAlpha),
        overlayAlpha + basePremul.a * (1.0 - overlayAlpha)
    );
}
)";

// ── Shader compilation helpers ──

static void replaceAllInPlace(std::string &value, const std::string &needle,
                              const std::string &replacement) {
  if (needle.empty())
    return;

  std::size_t start = 0;
  while ((start = value.find(needle, start)) != std::string::npos) {
    value.replace(start, needle.length(), replacement);
    start += replacement.length();
  }
}

static std::string
preprocessShaderSource(std::string source,
                       const std::map<std::string, std::string> &includes,
                       const uint8_t includeDepth = 3) {
  for (uint8_t pass = 0; pass < includeDepth; ++pass) {
    for (const auto &[name, text] : includes)
      replaceAllInPlace(source, "#include \"" + name + "\"", text);
  }
  return source;
}

static std::map<std::string, std::string>
buildNativeSurfaceShaderIncludes(CHyprOpenGLImpl *self, const uint8_t features) {
  auto includes = self->m_includes;

  includes["get_rgb_pixel.glsl"] =
      includes[features & SH_FEAT_RGBA ? "get_rgba_pixel.glsl"
                                       : "get_rgbx_pixel.glsl"];

  if (!(features & SH_FEAT_DISCARD)) {
    includes["discard.glsl"] = "";
    includes["do_discard.glsl"] = "";
  }
  if (!(features & SH_FEAT_TINT)) {
    includes["tint.glsl"] = "";
    includes["do_tint.glsl"] = "";
  }
  if (!(features & SH_FEAT_ROUNDING)) {
    includes["rounding.glsl"] = "";
    includes["do_rounding.glsl"] = "";
  }
  if (!(features & SH_FEAT_CM)) {
    includes["surface_CM.glsl"] = "";
    includes["CM.glsl"] = "";
    includes["do_CM.glsl"] = "";
  }
  if (!(features & SH_FEAT_TONEMAP)) {
    includes["tonemap.glsl"] = "";
    includes["do_tonemap.glsl"] = "";
  }
  if (!(features & SH_FEAT_SDR_MOD)) {
    includes["sdr_mod.glsl"] = "";
    includes["do_sdr_mod.glsl"] = "";
  }
  if (!(features & SH_FEAT_TONEMAP || features & SH_FEAT_SDR_MOD))
    includes["primaries_xyz.glsl"] = includes["primaries_xyz_const.glsl"];

  return includes;
}

static void
queryNativeUniformLocations(const GLuint program, SNativeShaderUniforms &uniforms) {
  uniforms.tintColor = glGetUniformLocation(program, "darkwindowTintColor");
  uniforms.tintStrength =
      glGetUniformLocation(program, "darkwindowTintStrength");
  uniforms.protectBrights =
      glGetUniformLocation(program, "darkwindowProtectBrights");
  uniforms.brightThreshold =
      glGetUniformLocation(program, "darkwindowBrightThreshold");
  uniforms.brightKnee = glGetUniformLocation(program, "darkwindowBrightKnee");
  uniforms.protectSaturated =
      glGetUniformLocation(program, "darkwindowProtectSaturated");
  uniforms.saturationThreshold =
      glGetUniformLocation(program, "darkwindowSaturationThreshold");
  uniforms.saturationKnee =
      glGetUniformLocation(program, "darkwindowSaturationKnee");
  uniforms.debugVisualize =
      glGetUniformLocation(program, "darkwindowDebugVisualize");
}

static void passNativeUniforms(const SNativeShaderUniforms &uniforms) {
  if (uniforms.tintColor != -1)
    glUniform3f(uniforms.tintColor, g_config.r, g_config.g, g_config.b);
  if (uniforms.tintStrength != -1)
    glUniform1f(uniforms.tintStrength, g_config.a);
  if (uniforms.protectBrights != -1)
    glUniform1f(uniforms.protectBrights, g_config.protect_brights);
  if (uniforms.brightThreshold != -1)
    glUniform1f(uniforms.brightThreshold, g_config.bright_threshold);
  if (uniforms.brightKnee != -1)
    glUniform1f(uniforms.brightKnee, g_config.bright_knee);
  if (uniforms.protectSaturated != -1)
    glUniform1f(uniforms.protectSaturated, g_config.protect_saturated);
  if (uniforms.saturationThreshold != -1)
    glUniform1f(uniforms.saturationThreshold, g_config.saturation_threshold);
  if (uniforms.saturationKnee != -1)
    glUniform1f(uniforms.saturationKnee, g_config.saturation_knee);
  if (uniforms.debugVisualize != -1)
    glUniform1i(uniforms.debugVisualize, g_config.debug_visualize);
}

static bool isShaded(PHLWINDOW pWindow);

static PHLWINDOW currentWindowForNativePath(CHyprOpenGLImpl *self) {
  if (!self)
    return nullptr;
  return self->m_renderData.currentWindow.lock();
}

static bool shouldUseNativeSurfaceShader(CHyprOpenGLImpl *self,
                                         const PHLWINDOW &window) {
  if (!self || !window)
    return false;
  if (!g_config.native_surface_shader_pass)
    return false;
  if (!g_runtimeProbeSafeForLowerLevel)
    return false;
  if (g_config.a <= 0.0f)
    return false;
  if (g_config.debug_visualize == 6)
    return false;
  if (g_pCompositor->isWindowActive(window))
    return false;
  if (!isShaded(window))
    return false;
  if (!g_config.enable_on_fullscreen && window->isFullscreen())
    return false;
  if (window->isHidden())
    return false;

  const auto workspace = window->m_workspace;
  if (!workspace || !workspace->m_visible)
    return false;

  if (std::chrono::steady_clock::now() < g_suspendUntil)
    return false;

  if (!g_config.tint_all_surfaces) {
    const auto rootSurface = window->resource();
    if (!rootSurface || self->m_renderData.surface != rootSurface)
      return false;
  }

  return true;
}

static SNativeShaderVariant *
getNativeSurfaceShaderVariant(CHyprOpenGLImpl *self, const uint8_t features) {
  if (!self || !self->m_shaders || self->m_shaders->TEXVERTSRC.empty())
    return nullptr;

  if (g_nativeSurfaceShaderFailures.contains(features))
    return nullptr;

  if (auto it = g_nativeSurfaceShaders.find(features);
      it != g_nativeSurfaceShaders.end())
    return &it->second;

  auto shader = makeShared<CShader>();
  const auto includes = buildNativeSurfaceShaderIncludes(self, features);
  const auto fragment =
      preprocessShaderSource(NATIVE_SURFACE_FRAG_SRC, includes);

  if (!shader->createProgram(self->m_shaders->TEXVERTSRC, fragment, true, true)) {
    Log::logger->log(
        Log::WARN,
        "[Hyprchroma] Native surface shader compilation failed for feature "
        "set {}, falling back to post-window tint",
        features);
    g_nativeSurfaceShaderFailures.insert(features);
    return nullptr;
  }

  SNativeShaderVariant variant;
  variant.shader = shader;
  queryNativeUniformLocations(shader->program(), variant.uniforms);

  const auto [it, _] =
      g_nativeSurfaceShaders.emplace(features, std::move(variant));
  return &it->second;
}

static SNativeShaderVariant *getNativeExtShaderVariant(CHyprOpenGLImpl *self) {
  if (!self || !self->m_shaders || self->m_shaders->TEXVERTSRC.empty())
    return nullptr;

  if (g_nativeExtShader.shader)
    return &g_nativeExtShader;
  if (g_nativeExtShaderCompileAttempted)
    return nullptr;

  g_nativeExtShaderCompileAttempted = true;

  auto shader = makeShared<CShader>();
  const auto fragment =
      preprocessShaderSource(NATIVE_EXT_FRAG_SRC, self->m_includes);

  if (!shader->createProgram(self->m_shaders->TEXVERTSRC, fragment, true, true)) {
    Log::logger->log(
        Log::WARN,
        "[Hyprchroma] Native external-texture shader compilation failed, "
        "falling back to post-window tint for DMA-BUF surfaces");
    return nullptr;
  }

  g_nativeExtShader.shader = shader;
  queryNativeUniformLocations(shader->program(), g_nativeExtShader.uniforms);
  return &g_nativeExtShader;
}

static const SNativeShaderVariant *
findNativeShaderVariant(const WP<CShader> &shader) {
  if (!shader)
    return nullptr;

  for (const auto &[_, variant] : g_nativeSurfaceShaders) {
    if (variant.shader && shader == variant.shader)
      return &variant;
  }

  if (g_nativeExtShader.shader && shader == g_nativeExtShader.shader)
    return &g_nativeExtShader;

  return nullptr;
}

using PGetSurfaceShaderFn = WP<CShader> (*)(CHyprOpenGLImpl *, uint8_t);
using PUseShaderFn = WP<CShader> (*)(CHyprOpenGLImpl *, WP<CShader>);

static WP<CShader> hkGetSurfaceShader(CHyprOpenGLImpl *self, uint8_t features) {
  const auto original =
      reinterpret_cast<PGetSurfaceShaderFn>(g_getSurfaceShaderHook->m_original);

  const auto window = currentWindowForNativePath(self);
  if (!shouldUseNativeSurfaceShader(self, window))
    return original(self, features);

  if (auto *variant = getNativeSurfaceShaderVariant(self, features))
    return variant->shader;

  return original(self, features);
}

static WP<CShader> hkUseShader(CHyprOpenGLImpl *self, WP<CShader> shader) {
  const auto original =
      reinterpret_cast<PUseShaderFn>(g_useShaderHook->m_original);

  const auto window = currentWindowForNativePath(self);
  const bool allowNative = shouldUseNativeSurfaceShader(self, window);

  if (allowNative && self && self->m_shaders &&
      self->m_shaders->frag[SH_FRAG_EXT] &&
      shader == self->m_shaders->frag[SH_FRAG_EXT]) {
    if (auto *variant = getNativeExtShaderVariant(self))
      shader = variant->shader;
  }

  auto usedShader = original(self, shader);

  if (const auto *variant = findNativeShaderVariant(usedShader)) {
    passNativeUniforms(variant->uniforms);
    if (window)
      g_nativeShadedThisFrame.insert((void *)window.get());
    if (!g_loggedNativeShaderPath) {
      Log::logger->log(
          Log::INFO,
          "[Hyprchroma] Native surface shader path active "
          "(adaptive tint composed inside Hyprland surface shaders)");
      g_loggedNativeShaderPath = true;
    }
  }

  return usedShader;
}

static GLuint compileShaderRaw(GLenum type, const char *source) {
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);

  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512];
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    Log::logger->log(Log::ERR, "[Hyprchroma] Shader compile error: {}", log);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

static GLuint linkProgramRaw(GLuint vert, GLuint frag) {
  GLuint prog = glCreateProgram();
  glAttachShader(prog, vert);
  glAttachShader(prog, frag);

  // Force attribute locations so both programs share one VAO
  glBindAttribLocation(prog, 0, "pos");
  glBindAttribLocation(prog, 1, "texcoord");

  glLinkProgram(prog);

  GLint ok = 0;
  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[512];
    glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
    Log::logger->log(Log::ERR, "[Hyprchroma] Program link error: {}", log);
    glDeleteProgram(prog);
    return 0;
  }
  return prog;
}

static void queryUniformLocations(
    GLuint prog, GLint &proj, GLint &windowTex, GLint &tintColor,
    GLint &tintStrength, GLint &windowAlpha, GLint &topLeft, GLint &fullSize,
    GLint &radius, GLint &roundingPower, GLint &uvTopLeft, GLint &uvBottomRight,
    GLint &protectBrights, GLint &brightThreshold, GLint &brightKnee,
    GLint &protectSaturated, GLint &saturationThreshold, GLint &saturationKnee,
    GLint &debugVisualize) {
  proj = glGetUniformLocation(prog, "proj");
  windowTex = glGetUniformLocation(prog, "windowTex");
  tintColor = glGetUniformLocation(prog, "tintColor");
  tintStrength = glGetUniformLocation(prog, "tintStrength");
  windowAlpha = glGetUniformLocation(prog, "windowAlpha");
  topLeft = glGetUniformLocation(prog, "topLeft");
  fullSize = glGetUniformLocation(prog, "fullSize");
  radius = glGetUniformLocation(prog, "radius");
  roundingPower = glGetUniformLocation(prog, "roundingPower");
  uvTopLeft = glGetUniformLocation(prog, "uvTopLeft");
  uvBottomRight = glGetUniformLocation(prog, "uvBottomRight");
  protectBrights = glGetUniformLocation(prog, "protectBrights");
  brightThreshold = glGetUniformLocation(prog, "brightThreshold");
  brightKnee = glGetUniformLocation(prog, "brightKnee");
  protectSaturated = glGetUniformLocation(prog, "protectSaturated");
  saturationThreshold = glGetUniformLocation(prog, "saturationThreshold");
  saturationKnee = glGetUniformLocation(prog, "saturationKnee");
  debugVisualize = glGetUniformLocation(prog, "debugVisualize");
}

static void queryBlitUniformLocations(
    GLuint prog, GLint &targetSize, GLint &quadTopLeft, GLint &quadSize,
    GLint &windowTex, GLint &uvTopLeft, GLint &uvBottomRight,
    GLint &opacity) {
  targetSize = glGetUniformLocation(prog, "targetSize");
  quadTopLeft = glGetUniformLocation(prog, "quadTopLeft");
  quadSize = glGetUniformLocation(prog, "quadSize");
  windowTex = glGetUniformLocation(prog, "windowTex");
  uvTopLeft = glGetUniformLocation(prog, "uvTopLeft");
  uvBottomRight = glGetUniformLocation(prog, "uvBottomRight");
  opacity = glGetUniformLocation(prog, "opacity");
}

static bool compileChromaShaders() {
  GLuint vert = compileShaderRaw(GL_VERTEX_SHADER, CHROMA_VERT_SRC);
  if (!vert)
    return false;

  // sampler2D variant
  GLuint frag = compileShaderRaw(GL_FRAGMENT_SHADER, CHROMA_FRAG_SRC);
  if (!frag) {
    glDeleteShader(vert);
    return false;
  }

  g_chromaProgram = linkProgramRaw(vert, frag);
  glDeleteShader(frag);

  if (g_chromaProgram) {
    queryUniformLocations(
        g_chromaProgram, g_loc_proj, g_loc_windowTex, g_loc_tintColor,
        g_loc_tintStrength, g_loc_windowAlpha, g_loc_topLeft, g_loc_fullSize,
        g_loc_radius, g_loc_roundingPower, g_loc_uvTopLeft, g_loc_uvBottomRight,
        g_loc_protectBrights, g_loc_brightThreshold, g_loc_brightKnee,
        g_loc_protectSaturated, g_loc_saturationThreshold, g_loc_saturationKnee,
        g_loc_debugVisualize);
  }

  // samplerExternalOES variant
  GLuint fragExt = compileShaderRaw(GL_FRAGMENT_SHADER, CHROMA_FRAG_EXT_SRC);
  if (fragExt) {
    g_chromaProgram_ext = linkProgramRaw(vert, fragExt);
    glDeleteShader(fragExt);
    if (g_chromaProgram_ext) {
      queryUniformLocations(
          g_chromaProgram_ext, g_loc_ext_proj, g_loc_ext_windowTex,
          g_loc_ext_tintColor, g_loc_ext_tintStrength, g_loc_ext_windowAlpha,
          g_loc_ext_topLeft, g_loc_ext_fullSize, g_loc_ext_radius,
          g_loc_ext_roundingPower, g_loc_ext_uvTopLeft, g_loc_ext_uvBottomRight,
          g_loc_ext_protectBrights, g_loc_ext_brightThreshold,
          g_loc_ext_brightKnee, g_loc_ext_protectSaturated,
          g_loc_ext_saturationThreshold, g_loc_ext_saturationKnee,
          g_loc_ext_debugVisualize);
    }
  } else {
    Log::logger->log(Log::WARN,
                     "[Hyprchroma] OES_EGL_image_external_essl3 not available, "
                     "DMA-BUF windows will use uniform tint fallback");
  }

  glDeleteShader(vert);

  GLuint blitVert = compileShaderRaw(GL_VERTEX_SHADER, BLIT_VERT_SRC);
  if (blitVert) {
    GLuint blitFrag = compileShaderRaw(GL_FRAGMENT_SHADER, BLIT_FRAG_SRC);
    if (blitFrag) {
      g_blitProgram = linkProgramRaw(blitVert, blitFrag);
      glDeleteShader(blitFrag);
      if (g_blitProgram) {
        queryBlitUniformLocations(
            g_blitProgram, g_loc_blit_targetSize, g_loc_blit_quadTopLeft,
            g_loc_blit_quadSize, g_loc_blit_windowTex, g_loc_blit_uvTopLeft,
            g_loc_blit_uvBottomRight, g_loc_blit_opacity);
      }
    }

    GLuint blitFragExt = compileShaderRaw(GL_FRAGMENT_SHADER, BLIT_FRAG_EXT_SRC);
    if (blitFragExt) {
      g_blitProgram_ext = linkProgramRaw(blitVert, blitFragExt);
      glDeleteShader(blitFragExt);
      if (g_blitProgram_ext) {
        queryBlitUniformLocations(
            g_blitProgram_ext, g_loc_blit_ext_targetSize,
            g_loc_blit_ext_quadTopLeft, g_loc_blit_ext_quadSize,
            g_loc_blit_ext_windowTex, g_loc_blit_ext_uvTopLeft,
            g_loc_blit_ext_uvBottomRight, g_loc_blit_ext_opacity);
      }
    }

    glDeleteShader(blitVert);
  }

  // Create shared VAO/VBO
  glGenVertexArrays(1, &g_chromaVAO);
  glGenBuffers(1, &g_chromaVBO);

  glBindVertexArray(g_chromaVAO);
  glBindBuffer(GL_ARRAY_BUFFER, g_chromaVBO);
  glBufferData(GL_ARRAY_BUFFER, sizeof(fullVerts), fullVerts.data(),
               GL_STATIC_DRAW);

  // pos at location 0: offset 0
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(SVertex),
                        (const void *)offsetof(SVertex, x));
  // texcoord at location 1: offset 8
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(SVertex),
                        (const void *)offsetof(SVertex, u));

  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  if (!g_blitProgram) {
    Log::logger->log(
        Log::WARN,
        "[Hyprchroma] Unified window pass blit shader unavailable, "
        "falling back to grouped per-surface shading");
  } else if (!g_blitProgram_ext) {
    Log::logger->log(
        Log::WARN,
        "[Hyprchroma] Unified window pass has no external-texture blit shader, "
        "DMA-BUF surfaces will fall back to grouped per-surface shading");
  }

  return g_chromaProgram != 0;
}

// ── CChromaPassElement ──

class CChromaPassElement : public IPassElement {
public:
  struct SSurfaceData {
    SP<CTexture> windowTex;
    CBox box;
    Vector2D uvTopLeft = Vector2D(0.F, 0.F);
    Vector2D uvBottomRight = Vector2D(1.F, 1.F);
    float alpha = 1.0f;
    bool isRootSurface = false;
  };

  struct SChromaData {
    CBox box;
    CBox clipBox;
    float tintR, tintG, tintB;
    float tintStrength;
    float windowAlpha;
    int round = 0;
    float roundingPower = 2.0f;
    bool useNearestNeighbor = false;
    Vector2D uvTopLeft = Vector2D(0.F, 0.F);
    Vector2D uvBottomRight = Vector2D(1.F, 1.F);
    float protectBrights = 1.0f;
    float brightThreshold = 0.82f;
    float brightKnee = 0.12f;
    float protectSaturated = 0.85f;
    float saturationThreshold = 0.20f;
    float saturationKnee = 0.16f;
    int debugVisualize = 0;
    bool unifiedWindowPass = false;
    std::vector<SSurfaceData> surfaces;
  };

  CChromaPassElement(const SChromaData &data) : m_data(data) {}
  virtual ~CChromaPassElement() = default;

  virtual void draw(const CRegion &damage) override;
  virtual bool needsLiveBlur() override { return false; }
  virtual bool needsPrecomputeBlur() override { return false; }
  virtual const char *passName() override { return "CChromaPassElement"; }
  virtual std::optional<CBox> boundingBox() override { return m_data.box; }

private:
  SChromaData m_data;
  CFramebuffer m_unifiedFB;
};

static bool composeUnifiedSurface(
    const CChromaPassElement::SChromaData &data, CFramebuffer &fb,
    CChromaPassElement::SSurfaceData &outSurface) {
  if (data.surfaces.empty() || data.box.w <= 0 || data.box.h <= 0 ||
      g_blitProgram == 0)
    return false;

  if (!fb.isAllocated() || (int)fb.m_size.x != data.box.w ||
      (int)fb.m_size.y != data.box.h) {
    fb.release();
    if (!fb.alloc(data.box.w, data.box.h)) {
      Log::logger->log(
          Log::ERR,
          "[Hyprchroma] Failed to allocate unified window framebuffer {}x{}",
          data.box.w, data.box.h);
      return false;
    }
  }

  GLint prevProgram = 0;
  glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
  GLint prevActiveTexture = 0;
  glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTexture);
  GLint prevTex2D = 0;
  GLint prevTexExternal = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex2D);
  glGetIntegerv(GL_TEXTURE_BINDING_EXTERNAL_OES, &prevTexExternal);
  GLint prevVAO = 0;
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVAO);
  GLint prevFB = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFB);
  GLint prevViewport[4] = {0, 0, 0, 0};
  glGetIntegerv(GL_VIEWPORT, prevViewport);
  GLboolean prevBlendEnabled = glIsEnabled(GL_BLEND);
  GLint prevBlendSrcRGB = 0;
  GLint prevBlendDstRGB = 0;
  GLint prevBlendSrcAlpha = 0;
  GLint prevBlendDstAlpha = 0;
  GLint prevBlendEqRGB = 0;
  GLint prevBlendEqAlpha = 0;
  glGetIntegerv(GL_BLEND_SRC_RGB, &prevBlendSrcRGB);
  glGetIntegerv(GL_BLEND_DST_RGB, &prevBlendDstRGB);
  glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevBlendSrcAlpha);
  glGetIntegerv(GL_BLEND_DST_ALPHA, &prevBlendDstAlpha);
  glGetIntegerv(GL_BLEND_EQUATION_RGB, &prevBlendEqRGB);
  glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &prevBlendEqAlpha);
  GLboolean prevScissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
  GLint prevScissorBox[4] = {0, 0, 0, 0};
  glGetIntegerv(GL_SCISSOR_BOX, prevScissorBox);
  GLboolean prevStencilEnabled = glIsEnabled(GL_STENCIL_TEST);
  GLint prevStencilFunc = 0;
  GLint prevStencilRef = 0;
  GLint prevStencilValueMask = 0;
  GLint prevStencilWriteMask = 0;
  GLint prevStencilFail = 0;
  GLint prevStencilPassDepthFail = 0;
  GLint prevStencilPassDepthPass = 0;
  GLint prevStencilClearValue = 0;
  glGetIntegerv(GL_STENCIL_FUNC, &prevStencilFunc);
  glGetIntegerv(GL_STENCIL_REF, &prevStencilRef);
  glGetIntegerv(GL_STENCIL_VALUE_MASK, &prevStencilValueMask);
  glGetIntegerv(GL_STENCIL_WRITEMASK, &prevStencilWriteMask);
  glGetIntegerv(GL_STENCIL_FAIL, &prevStencilFail);
  glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, &prevStencilPassDepthFail);
  glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &prevStencilPassDepthPass);
  glGetIntegerv(GL_STENCIL_CLEAR_VALUE, &prevStencilClearValue);
  GLfloat prevClearColor[4] = {0.F, 0.F, 0.F, 0.F};
  glGetFloatv(GL_COLOR_CLEAR_VALUE, prevClearColor);

  fb.bind();
  glViewport(0, 0, data.box.w, data.box.h);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_STENCIL_TEST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glBlendEquation(GL_FUNC_ADD);
  glClearColor(0.F, 0.F, 0.F, 0.F);
  glClear(GL_COLOR_BUFFER_BIT);
  glBindVertexArray(g_chromaVAO);

  bool success = true;
  for (const auto &surf : data.surfaces) {
    const bool isExternal =
        (surf.windowTex->m_target == GL_TEXTURE_EXTERNAL_OES);
    const GLuint prog = isExternal ? g_blitProgram_ext : g_blitProgram;
    if (prog == 0) {
      if (isExternal && !g_loggedUnifiedFallbackNoExternalProgram) {
        Log::logger->log(
            Log::WARN,
            "[Hyprchroma] Unified window pass encountered an external texture "
            "without external blit shader support, falling back");
        g_loggedUnifiedFallbackNoExternalProgram = true;
      }
      success = false;
      break;
    }

    const GLint locTargetSize =
        isExternal ? g_loc_blit_ext_targetSize : g_loc_blit_targetSize;
    const GLint locQuadTopLeft =
        isExternal ? g_loc_blit_ext_quadTopLeft : g_loc_blit_quadTopLeft;
    const GLint locQuadSize =
        isExternal ? g_loc_blit_ext_quadSize : g_loc_blit_quadSize;
    const GLint locWindowTex =
        isExternal ? g_loc_blit_ext_windowTex : g_loc_blit_windowTex;
    const GLint locUvTopLeft =
        isExternal ? g_loc_blit_ext_uvTopLeft : g_loc_blit_uvTopLeft;
    const GLint locUvBottomRight =
        isExternal ? g_loc_blit_ext_uvBottomRight : g_loc_blit_uvBottomRight;
    const GLint locOpacity =
        isExternal ? g_loc_blit_ext_opacity : g_loc_blit_opacity;

    glUseProgram(prog);
    glUniform2f(locTargetSize, (float)data.box.w, (float)data.box.h);
    glUniform2f(locQuadTopLeft, (float)(surf.box.x - data.box.x),
                (float)(surf.box.y - data.box.y));
    glUniform2f(locQuadSize, (float)surf.box.w, (float)surf.box.h);
    glUniform2f(locUvTopLeft, surf.uvTopLeft.x, surf.uvTopLeft.y);
    glUniform2f(locUvBottomRight, surf.uvBottomRight.x, surf.uvBottomRight.y);
    glUniform1f(locOpacity, surf.alpha);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(surf.windowTex->m_target, surf.windowTex->m_texID);
    glTexParameteri(surf.windowTex->m_target, GL_TEXTURE_WRAP_S,
                    GL_CLAMP_TO_EDGE);
    glTexParameteri(surf.windowTex->m_target, GL_TEXTURE_WRAP_T,
                    GL_CLAMP_TO_EDGE);
    glTexParameteri(surf.windowTex->m_target, GL_TEXTURE_MIN_FILTER,
                    data.useNearestNeighbor ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(surf.windowTex->m_target, GL_TEXTURE_MAG_FILTER,
                    data.useNearestNeighbor ? GL_NEAREST : GL_LINEAR);
    glUniform1i(locWindowTex, 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }

  glBindVertexArray(prevVAO);
  glActiveTexture(prevActiveTexture);
  glBindTexture(GL_TEXTURE_2D, prevTex2D);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, prevTexExternal);
  glUseProgram(prevProgram);
  glBindFramebuffer(GL_FRAMEBUFFER, prevFB);
  glViewport(prevViewport[0], prevViewport[1], prevViewport[2],
             prevViewport[3]);
  if (prevBlendEnabled)
    glEnable(GL_BLEND);
  else
    glDisable(GL_BLEND);
  glBlendFuncSeparate(prevBlendSrcRGB, prevBlendDstRGB, prevBlendSrcAlpha,
                      prevBlendDstAlpha);
  glBlendEquationSeparate(prevBlendEqRGB, prevBlendEqAlpha);
  if (prevScissorEnabled)
    glEnable(GL_SCISSOR_TEST);
  else
    glDisable(GL_SCISSOR_TEST);
  glScissor(prevScissorBox[0], prevScissorBox[1], prevScissorBox[2],
            prevScissorBox[3]);
  if (prevStencilEnabled)
    glEnable(GL_STENCIL_TEST);
  else
    glDisable(GL_STENCIL_TEST);
  glStencilFunc(prevStencilFunc, prevStencilRef, prevStencilValueMask);
  glStencilOp(prevStencilFail, prevStencilPassDepthFail,
              prevStencilPassDepthPass);
  glStencilMask(prevStencilWriteMask);
  glClearStencil(prevStencilClearValue);
  glClearColor(prevClearColor[0], prevClearColor[1], prevClearColor[2],
               prevClearColor[3]);

  if (!success)
    return false;

  outSurface.windowTex = fb.getTexture();
  if (!outSurface.windowTex || outSurface.windowTex->m_texID == 0)
    return false;

  outSurface.box = data.box;
  // FBO color attachments are sampled upside-down relative to the direct
  // surface path, so flip Y here or the preserve-mask appears mirrored and
  // drifts opposite to scroll.
  outSurface.uvTopLeft = Vector2D(0.F, 1.F);
  outSurface.uvBottomRight = Vector2D(1.F, 0.F);
  outSurface.alpha = 1.0f;
  outSurface.isRootSurface = true;
  return true;
}

void CChromaPassElement::draw(const CRegion &damage) {
  auto pMonitor = g_pHyprOpenGL->m_renderData.pMonitor.lock();
  if (!pMonitor)
    return;

  GLint composePrevProgram = 0;
  glGetIntegerv(GL_CURRENT_PROGRAM, &composePrevProgram);
  GLint composePrevActiveTexture = 0;
  glGetIntegerv(GL_ACTIVE_TEXTURE, &composePrevActiveTexture);
  GLint composePrevTex2D = 0;
  GLint composePrevTexExternal = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &composePrevTex2D);
  glGetIntegerv(GL_TEXTURE_BINDING_EXTERNAL_OES, &composePrevTexExternal);
  GLint composePrevVAO = 0;
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &composePrevVAO);
  GLint composePrevFB = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &composePrevFB);
  GLint composePrevViewport[4] = {0, 0, 0, 0};
  glGetIntegerv(GL_VIEWPORT, composePrevViewport);
  GLboolean composePrevBlendEnabled = glIsEnabled(GL_BLEND);
  GLint composePrevBlendSrcRGB = 0;
  GLint composePrevBlendDstRGB = 0;
  GLint composePrevBlendSrcAlpha = 0;
  GLint composePrevBlendDstAlpha = 0;
  GLint composePrevBlendEqRGB = 0;
  GLint composePrevBlendEqAlpha = 0;
  glGetIntegerv(GL_BLEND_SRC_RGB, &composePrevBlendSrcRGB);
  glGetIntegerv(GL_BLEND_DST_RGB, &composePrevBlendDstRGB);
  glGetIntegerv(GL_BLEND_SRC_ALPHA, &composePrevBlendSrcAlpha);
  glGetIntegerv(GL_BLEND_DST_ALPHA, &composePrevBlendDstAlpha);
  glGetIntegerv(GL_BLEND_EQUATION_RGB, &composePrevBlendEqRGB);
  glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &composePrevBlendEqAlpha);
  GLboolean composePrevScissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
  GLint composePrevScissorBox[4] = {0, 0, 0, 0};
  glGetIntegerv(GL_SCISSOR_BOX, composePrevScissorBox);
  GLboolean composePrevStencilEnabled = glIsEnabled(GL_STENCIL_TEST);
  GLint composePrevStencilFunc = 0;
  GLint composePrevStencilRef = 0;
  GLint composePrevStencilValueMask = 0;
  GLint composePrevStencilWriteMask = 0;
  GLint composePrevStencilFail = 0;
  GLint composePrevStencilPassDepthFail = 0;
  GLint composePrevStencilPassDepthPass = 0;
  GLint composePrevStencilClearValue = 0;
  glGetIntegerv(GL_STENCIL_FUNC, &composePrevStencilFunc);
  glGetIntegerv(GL_STENCIL_REF, &composePrevStencilRef);
  glGetIntegerv(GL_STENCIL_VALUE_MASK, &composePrevStencilValueMask);
  glGetIntegerv(GL_STENCIL_WRITEMASK, &composePrevStencilWriteMask);
  glGetIntegerv(GL_STENCIL_FAIL, &composePrevStencilFail);
  glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL,
                &composePrevStencilPassDepthFail);
  glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS,
                &composePrevStencilPassDepthPass);
  glGetIntegerv(GL_STENCIL_CLEAR_VALUE, &composePrevStencilClearValue);
  GLfloat composePrevClearColor[4] = {0.F, 0.F, 0.F, 0.F};
  glGetFloatv(GL_COLOR_CLEAR_VALUE, composePrevClearColor);

  std::vector<SSurfaceData> unifiedSurfaceStorage;
  const auto *surfacesToDraw = &m_data.surfaces;

  if (m_data.unifiedWindowPass) {
    SSurfaceData unifiedSurface;
    if (composeUnifiedSurface(m_data, m_unifiedFB, unifiedSurface)) {
      unifiedSurfaceStorage.push_back(std::move(unifiedSurface));
      surfacesToDraw = &unifiedSurfaceStorage;
    }
  }

  glBindVertexArray(composePrevVAO);
  glActiveTexture(composePrevActiveTexture);
  glBindTexture(GL_TEXTURE_2D, composePrevTex2D);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, composePrevTexExternal);
  glUseProgram(composePrevProgram);
  glBindFramebuffer(GL_FRAMEBUFFER, composePrevFB);
  glViewport(composePrevViewport[0], composePrevViewport[1],
             composePrevViewport[2], composePrevViewport[3]);
  if (composePrevBlendEnabled)
    glEnable(GL_BLEND);
  else
    glDisable(GL_BLEND);
  glBlendFuncSeparate(composePrevBlendSrcRGB, composePrevBlendDstRGB,
                      composePrevBlendSrcAlpha, composePrevBlendDstAlpha);
  glBlendEquationSeparate(composePrevBlendEqRGB, composePrevBlendEqAlpha);
  if (composePrevScissorEnabled)
    glEnable(GL_SCISSOR_TEST);
  else
    glDisable(GL_SCISSOR_TEST);
  glScissor(composePrevScissorBox[0], composePrevScissorBox[1],
            composePrevScissorBox[2], composePrevScissorBox[3]);
  if (composePrevStencilEnabled)
    glEnable(GL_STENCIL_TEST);
  else
    glDisable(GL_STENCIL_TEST);
  glStencilFunc(composePrevStencilFunc, composePrevStencilRef,
                composePrevStencilValueMask);
  glStencilOp(composePrevStencilFail, composePrevStencilPassDepthFail,
              composePrevStencilPassDepthPass);
  glStencilMask(composePrevStencilWriteMask);
  glClearStencil(composePrevStencilClearValue);
  glClearColor(composePrevClearColor[0], composePrevClearColor[1],
               composePrevClearColor[2], composePrevClearColor[3]);

  if (surfacesToDraw->empty())
    return;

  static constexpr double DAMAGE_PAD = 1.0;
  const bool useFullWindowDamage =
      m_data.unifiedWindowPass && surfacesToDraw != &m_data.surfaces;
  const CRegion effectiveDamage =
      useFullWindowDamage
          ? CRegion(m_data.box.copy().round().expand(DAMAGE_PAD))
          : damage.copy().expand(DAMAGE_PAD);
  // Save GL state
  GLint prevProgram = 0;
  glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
  GLint prevActiveTexture = 0;
  glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTexture);
  GLint prevTex2D = 0;
  GLint prevTexExternal = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex2D);
  glGetIntegerv(GL_TEXTURE_BINDING_EXTERNAL_OES, &prevTexExternal);
  GLint prevVAO = 0;
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVAO);
  GLboolean prevStencilEnabled = glIsEnabled(GL_STENCIL_TEST);
  GLint prevStencilFunc = 0;
  GLint prevStencilRef = 0;
  GLint prevStencilValueMask = 0;
  GLint prevStencilWriteMask = 0;
  GLint prevStencilFail = 0;
  GLint prevStencilPassDepthFail = 0;
  GLint prevStencilPassDepthPass = 0;
  GLint prevStencilClearValue = 0;
  glGetIntegerv(GL_STENCIL_FUNC, &prevStencilFunc);
  glGetIntegerv(GL_STENCIL_REF, &prevStencilRef);
  glGetIntegerv(GL_STENCIL_VALUE_MASK, &prevStencilValueMask);
  glGetIntegerv(GL_STENCIL_WRITEMASK, &prevStencilWriteMask);
  glGetIntegerv(GL_STENCIL_FAIL, &prevStencilFail);
  glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, &prevStencilPassDepthFail);
  glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &prevStencilPassDepthPass);
  glGetIntegerv(GL_STENCIL_CLEAR_VALUE, &prevStencilClearValue);
  GLboolean prevBlendEnabled = glIsEnabled(GL_BLEND);
  GLint prevBlendSrcRGB = 0;
  GLint prevBlendDstRGB = 0;
  GLint prevBlendSrcAlpha = 0;
  GLint prevBlendDstAlpha = 0;
  GLint prevBlendEqRGB = 0;
  GLint prevBlendEqAlpha = 0;
  glGetIntegerv(GL_BLEND_SRC_RGB, &prevBlendSrcRGB);
  glGetIntegerv(GL_BLEND_DST_RGB, &prevBlendDstRGB);
  glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevBlendSrcAlpha);
  glGetIntegerv(GL_BLEND_DST_ALPHA, &prevBlendDstAlpha);
  glGetIntegerv(GL_BLEND_EQUATION_RGB, &prevBlendEqRGB);
  glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &prevBlendEqAlpha);

  glBindVertexArray(g_chromaVAO);
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glBlendEquation(GL_FUNC_ADD);
  glEnable(GL_STENCIL_TEST);
  glStencilMask(0x80);
  glStencilFunc(GL_NOTEQUAL, 0x80, 0x80);
  glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);

  CRegion clearRegion = effectiveDamage.copy();
  clearRegion.intersect(m_data.box.copy().round().expand(DAMAGE_PAD));
  if (m_data.clipBox.w != 0 && m_data.clipBox.h != 0)
    clearRegion.intersect(m_data.clipBox.copy().round().expand(DAMAGE_PAD));

  // Clear our stencil bit before drawing so stale claims from previous frames
  // cannot punch holes in the current frame.
  glStencilMask(0x80);
  glClearStencil(0);
  for (auto &rect : clearRegion.getRects()) {
    g_pHyprOpenGL->scissor(&rect);
    glClear(GL_STENCIL_BUFFER_BIT);
  }

  for (const auto &surf : *surfacesToDraw) {
    const bool isExternal =
        (surf.windowTex->m_target == GL_TEXTURE_EXTERNAL_OES);
    const GLuint prog = isExternal ? g_chromaProgram_ext : g_chromaProgram;
    if (prog == 0)
      continue;

    const GLint locProj = isExternal ? g_loc_ext_proj : g_loc_proj;
    const GLint locWindowTex =
        isExternal ? g_loc_ext_windowTex : g_loc_windowTex;
    const GLint locTintColor =
        isExternal ? g_loc_ext_tintColor : g_loc_tintColor;
    const GLint locTintStrength =
        isExternal ? g_loc_ext_tintStrength : g_loc_tintStrength;
    const GLint locWindowAlpha =
        isExternal ? g_loc_ext_windowAlpha : g_loc_windowAlpha;
    const GLint locTopLeft = isExternal ? g_loc_ext_topLeft : g_loc_topLeft;
    const GLint locFullSize = isExternal ? g_loc_ext_fullSize : g_loc_fullSize;
    const GLint locRadius = isExternal ? g_loc_ext_radius : g_loc_radius;
    const GLint locRoundingPower =
        isExternal ? g_loc_ext_roundingPower : g_loc_roundingPower;
    const GLint locUvTopLeft =
        isExternal ? g_loc_ext_uvTopLeft : g_loc_uvTopLeft;
    const GLint locUvBottomRight =
        isExternal ? g_loc_ext_uvBottomRight : g_loc_uvBottomRight;
    const GLint locProtectBrights =
        isExternal ? g_loc_ext_protectBrights : g_loc_protectBrights;
    const GLint locBrightThreshold =
        isExternal ? g_loc_ext_brightThreshold : g_loc_brightThreshold;
    const GLint locBrightKnee =
        isExternal ? g_loc_ext_brightKnee : g_loc_brightKnee;
    const GLint locProtectSaturated =
        isExternal ? g_loc_ext_protectSaturated : g_loc_protectSaturated;
    const GLint locSaturationThreshold =
        isExternal ? g_loc_ext_saturationThreshold : g_loc_saturationThreshold;
    const GLint locSaturationKnee =
        isExternal ? g_loc_ext_saturationKnee : g_loc_saturationKnee;
    const GLint locDebugVisualize =
        isExternal ? g_loc_ext_debugVisualize : g_loc_debugVisualize;

    const auto matrix =
        g_pHyprOpenGL->m_renderData.monitorProjection.projectBox(
            surf.box, HYPRUTILS_TRANSFORM_NORMAL);
    const auto glMatrix =
        g_pHyprOpenGL->m_renderData.projection.copy().multiply(matrix);
    const auto matData = glMatrix.getMatrix();

    glUseProgram(prog);
    glUniformMatrix3fv(locProj, 1, GL_TRUE, matData.data());
    glUniform3f(locTintColor, m_data.tintR, m_data.tintG, m_data.tintB);
    glUniform1f(locTintStrength, m_data.tintStrength);
    glUniform1f(locWindowAlpha, m_data.windowAlpha * surf.alpha);

    CBox transformedBox = surf.box;
    transformedBox.transform(Math::wlTransformToHyprutils(
                                 Math::invertTransform(pMonitor->m_transform)),
                             pMonitor->m_transformedSize.x,
                             pMonitor->m_transformedSize.y);

    glUniform1f(locRadius, surf.isRootSurface ? (float)m_data.round : 0.0f);
    glUniform1f(locRoundingPower, m_data.roundingPower);
    glUniform2f(locTopLeft, transformedBox.x, transformedBox.y);
    glUniform2f(locFullSize, transformedBox.w, transformedBox.h);
    glUniform2f(locUvTopLeft, surf.uvTopLeft.x, surf.uvTopLeft.y);
    glUniform2f(locUvBottomRight, surf.uvBottomRight.x, surf.uvBottomRight.y);
    glUniform1f(locProtectBrights, m_data.protectBrights);
    glUniform1f(locBrightThreshold, m_data.brightThreshold);
    glUniform1f(locBrightKnee, m_data.brightKnee);
    glUniform1f(locProtectSaturated, m_data.protectSaturated);
    glUniform1f(locSaturationThreshold, m_data.saturationThreshold);
    glUniform1f(locSaturationKnee, m_data.saturationKnee);
    glUniform1i(locDebugVisualize, m_data.debugVisualize);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(surf.windowTex->m_target, surf.windowTex->m_texID);
    glTexParameteri(surf.windowTex->m_target, GL_TEXTURE_WRAP_S,
                    GL_CLAMP_TO_EDGE);
    glTexParameteri(surf.windowTex->m_target, GL_TEXTURE_WRAP_T,
                    GL_CLAMP_TO_EDGE);
    glTexParameteri(surf.windowTex->m_target, GL_TEXTURE_MIN_FILTER,
                    m_data.useNearestNeighbor ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(surf.windowTex->m_target, GL_TEXTURE_MAG_FILTER,
                    m_data.useNearestNeighbor ? GL_NEAREST : GL_LINEAR);
    glUniform1i(locWindowTex, 0);

    CRegion surfDamage = effectiveDamage.copy();
    surfDamage.intersect(surf.box.copy().round().expand(DAMAGE_PAD));
    if (m_data.clipBox.w != 0 && m_data.clipBox.h != 0)
      surfDamage.intersect(m_data.clipBox.copy().round().expand(DAMAGE_PAD));

    for (auto &rect : surfDamage.getRects()) {
      g_pHyprOpenGL->scissor(&rect);
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
  }

  // Clear again after drawing so later passes start from a clean stencil state.
  glStencilMask(0x80);
  glClearStencil(0);
  for (auto &rect : clearRegion.getRects()) {
    g_pHyprOpenGL->scissor(&rect);
    glClear(GL_STENCIL_BUFFER_BIT);
  }

  // Restore GL state
  g_pHyprOpenGL->scissor((const pixman_box32 *)nullptr);
  glBindVertexArray(prevVAO);
  glActiveTexture(prevActiveTexture);
  glBindTexture(GL_TEXTURE_2D, prevTex2D);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, prevTexExternal);
  glUseProgram(prevProgram);
  if (prevBlendEnabled)
    glEnable(GL_BLEND);
  else
    glDisable(GL_BLEND);
  glBlendFuncSeparate(prevBlendSrcRGB, prevBlendDstRGB, prevBlendSrcAlpha,
                      prevBlendDstAlpha);
  glBlendEquationSeparate(prevBlendEqRGB, prevBlendEqAlpha);
  if (prevStencilEnabled)
    glEnable(GL_STENCIL_TEST);
  else
    glDisable(GL_STENCIL_TEST);
  glStencilFunc(prevStencilFunc, prevStencilRef, prevStencilValueMask);
  glStencilOp(prevStencilFail, prevStencilPassDepthFail,
              prevStencilPassDepthPass);
  glStencilMask(prevStencilWriteMask);
  glClearStencil(prevStencilClearValue);
}

// ── Config helpers ──

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
  g_config.protect_brights =
      getCfgFloat("plugin:darkwindow:protect_brights", 1.00f);
  g_config.bright_threshold =
      getCfgFloat("plugin:darkwindow:bright_threshold", 0.82f);
  g_config.bright_knee = getCfgFloat("plugin:darkwindow:bright_knee", 0.12f);
  g_config.protect_saturated =
      getCfgFloat("plugin:darkwindow:protect_saturated", 0.85f);
  g_config.saturation_threshold =
      getCfgFloat("plugin:darkwindow:saturation_threshold", 0.20f);
  g_config.saturation_knee =
      getCfgFloat("plugin:darkwindow:saturation_knee", 0.16f);
  g_config.debug_visualize = getCfgInt("plugin:darkwindow:debug_visualize", 0);
  g_config.enable_on_fullscreen =
      getCfgInt("plugin:darkwindow:enable_on_fullscreen", 1);
  g_config.tint_all_surfaces =
      getCfgInt("plugin:darkwindow:tint_all_surfaces", 1);
  g_config.unified_window_pass =
      getCfgInt("plugin:darkwindow:unified_window_pass", 0);
  g_config.native_surface_shader_pass =
      getCfgInt("plugin:darkwindow:native_surface_shader_pass", 0);
  g_config.cursor_invalidation_mode =
      std::max(0, getCfgInt("plugin:darkwindow:cursor_invalidation_mode", 0));
  g_config.cursor_invalidation_throttle_ms =
      std::max(0, getCfgInt("plugin:darkwindow:cursor_invalidation_throttle_ms",
                            0));
  g_config.cursor_invalidation_radius =
      std::max(0, getCfgInt("plugin:darkwindow:cursor_invalidation_radius",
                            48));
  g_config.suspend_on_workspace_switch_ms =
      std::max(0, getCfgInt("plugin:darkwindow:suspend_on_workspace_switch_ms",
                            150));

  if (g_config.cursor_invalidation_mode > 0 && !g_loggedCursorInvalidationMode) {
    Log::logger->log(
        Log::INFO,
        "[Hyprchroma] Cursor invalidation mode {} active "
        "(throttle={}ms radius={}px)",
        g_config.cursor_invalidation_mode,
        g_config.cursor_invalidation_throttle_ms,
        g_config.cursor_invalidation_radius);
    g_loggedCursorInvalidationMode = true;
  } else if (g_config.cursor_invalidation_mode <= 0) {
    g_loggedCursorInvalidationMode = false;
  }

  if (g_config.native_surface_shader_pass && !g_runtimeProbeSafeForLowerLevel &&
      !g_loggedNativeFlagBlocked) {
    Log::logger->log(
        Log::WARN,
        "[Hyprchroma] native_surface_shader_pass requested but runtime probe "
        "does not consider lower-level hooks safe on this Hyprland build");
    g_loggedNativeFlagBlocked = true;
  } else if (!g_config.native_surface_shader_pass ||
             g_runtimeProbeSafeForLowerLevel) {
    g_loggedNativeFlagBlocked = false;
  }
}

static bool isShaded(PHLWINDOW pWindow) {
  if (!pWindow)
    return false;
  if (g_perWindowShaded.contains((void *)pWindow.get()))
    return true;
  return g_globalShaded;
}

static bool pointInExpandedBox(const Vector2D &point, const CBox &box,
                               const double expand) {
  return point.x >= box.x - expand && point.x <= box.x + box.w + expand &&
         point.y >= box.y - expand && point.y <= box.y + box.h + expand;
}

static void invalidateFromCursorMotion(const Vector2D &, Event::SCallbackInfo &) {
  if (g_config.cursor_invalidation_mode <= 0 || !g_pInputManager)
    return;

  const auto now = std::chrono::steady_clock::now();
  if (g_config.cursor_invalidation_throttle_ms > 0 &&
      g_lastCursorInvalidation != std::chrono::steady_clock::time_point::min() &&
      now - g_lastCursorInvalidation <
          std::chrono::milliseconds(g_config.cursor_invalidation_throttle_ms))
    return;

  if (g_config.cursor_invalidation_mode == 3) {
    redrawAll();
    g_lastCursorInvalidation = now;
    return;
  }

  const auto cursor = g_pInputManager->getMouseCoordsInternal();
  const auto radius = static_cast<double>(g_config.cursor_invalidation_radius);
  bool invalidatedAnything = false;

  for (auto &window : g_pCompositor->m_windows) {
    if (!window || !isShaded(window) || window->isHidden())
      continue;

    auto workspace = window->m_workspace;
    if (!workspace || !workspace->m_visible)
      continue;

    const auto box = window->getWindowMainSurfaceBox();
    const bool shouldInvalidate =
        g_config.cursor_invalidation_mode == 2 ||
        pointInExpandedBox(cursor, box, radius);

    if (!shouldInvalidate)
      continue;

    g_pHyprRenderer->damageWindow(window);
    if (auto monitor = window->m_monitor.lock())
      g_pCompositor->scheduleFrameForMonitor(monitor);
    invalidatedAnything = true;
  }

  if (invalidatedAnything)
    g_lastCursorInvalidation = now;
}

// ── Render hook ──

static void onRenderStage(eRenderStage stage) {
  if (stage == RENDER_BEGIN) {
    g_renderedThisFrame.clear();
    g_nativeShadedThisFrame.clear();

    const bool suspendedNow =
        std::chrono::steady_clock::now() < g_suspendUntil;
    if (g_wasSuspendedLastFrame && !suspendedNow)
      redrawAll();
    g_wasSuspendedLastFrame = suspendedNow;

    // Lazy shader compilation (GL context guaranteed active here)
    if (!g_shadersCompiled) {
      g_shadersCompiled = compileChromaShaders();
      if (g_shadersCompiled && !g_loggedShaderInit) {
        Log::logger->log(Log::INFO,
                         "[Hyprchroma] Shader path initialized successfully");
        g_loggedShaderInit = true;
      } else if (!g_shadersCompiled)
        Log::logger->log(Log::ERR, "[Hyprchroma] Shader compilation failed, "
                                   "falling back to uniform tint");
    }
    return;
  }

  if (stage != RENDER_POST_WINDOW)
    return;

  if (g_config.a <= 0.0f)
    return;

  if (std::chrono::steady_clock::now() < g_suspendUntil)
    return;

  auto window = g_pHyprOpenGL->m_renderData.currentWindow.lock();
  if (!window || !isShaded(window))
    return;

  if (g_nativeShadedThisFrame.contains((void *)window.get()))
    return;

  if (!g_config.enable_on_fullscreen && window->isFullscreen())
    return;

  auto monitor = g_pHyprOpenGL->m_renderData.pMonitor.lock();
  if (!monitor)
    return;

  if (window->isHidden())
    return;

  auto wksp = window->m_workspace;
  if (!wksp || !wksp->m_visible)
    return;

  if (g_renderedThisFrame.contains((void *)window.get()))
    return;
  g_renderedThisFrame.insert((void *)window.get());

  const float scale = monitor->m_scale;
  const auto logBox = window->getWindowMainSurfaceBox();
  auto renderOffset = wksp->m_renderOffset->value();

  float windowAlpha =
      window->m_alpha->value() * window->m_activeInactiveAlpha->value();

  CBox overlayBox((logBox.x + renderOffset.x - monitor->m_position.x) * scale,
                  (logBox.y + renderOffset.y - monitor->m_position.y) * scale,
                  logBox.w * scale, logBox.h * scale);

  int round = static_cast<int>(window->rounding() * scale);
  float rPower = window->roundingPower();

  // Try v4 path: per-surface luminance tint
  auto clipBox = g_pHyprOpenGL->m_renderData.clipBox;
  bool useNearestNeighbor = g_pHyprOpenGL->m_renderData.useNearestNeighbor;
  CChromaPassElement::SChromaData chromaData;
  chromaData.box = overlayBox;
  chromaData.clipBox = clipBox;
  chromaData.tintR = g_config.r;
  chromaData.tintG = g_config.g;
  chromaData.tintB = g_config.b;
  chromaData.tintStrength = g_config.a;
  chromaData.windowAlpha = windowAlpha;
  chromaData.round = round;
  chromaData.roundingPower = rPower;
  chromaData.useNearestNeighbor = useNearestNeighbor;
  chromaData.protectBrights = g_config.protect_brights;
  chromaData.brightThreshold = g_config.bright_threshold;
  chromaData.brightKnee = g_config.bright_knee;
  chromaData.protectSaturated = g_config.protect_saturated;
  chromaData.saturationThreshold = g_config.saturation_threshold;
  chromaData.saturationKnee = g_config.saturation_knee;
  chromaData.debugVisualize = g_config.debug_visualize;
  chromaData.unifiedWindowPass = g_config.unified_window_pass;
  if (g_shadersCompiled) {
    auto rootSurface = window->resource();
    if (!rootSurface) {
      if (!g_loggedFallbackNoSurface) {
        Log::logger->log(Log::WARN,
                         "[Hyprchroma] Window has no root surface at "
                         "RENDER_POST_WINDOW, using uniform fallback");
        g_loggedFallbackNoSurface = true;
      }
    } else {
      rootSurface->breadthfirst(
          [&](SP<CWLSurfaceResource> surface, const Vector2D &offset, void *) {
            if (!surface || !surface->m_current.texture)
              return;

            auto tex = surface->m_current.texture;
            if (tex->m_texID == 0)
              return;

            if (tex->m_target == GL_TEXTURE_EXTERNAL_OES &&
                g_chromaProgram_ext == 0) {
              if (!g_loggedFallbackNoExternalProgram) {
                Log::logger->log(
                    Log::WARN,
                    "[Hyprchroma] External texture encountered without "
                    "external shader support, skipping that surface");
                g_loggedFallbackNoExternalProgram = true;
              }
              return;
            }

            auto logicalSize = surface->m_current.size;
            const bool hasLogicalSize =
                (logicalSize.x > 1.1f && logicalSize.y > 1.1f);
            const bool isRootSurface = surface == rootSurface;
            if (!g_config.tint_all_surfaces && !isRootSurface)
              return;

            Vector2D projectedSize;
            if (surface->m_current.viewport.hasDestination)
              projectedSize =
                  (surface->m_current.viewport.destination * scale).round();
            else if (surface->m_current.viewport.hasSource)
              projectedSize =
                  (surface->m_current.viewport.source.size() * scale).round();
            else if (hasLogicalSize)
              projectedSize = (logicalSize * scale).round();
            else
              projectedSize = surface->m_current.bufferSize;

            CChromaPassElement::SSurfaceData sdata;
            sdata.windowTex = tex;
            sdata.box = CBox(
                (logBox.x + offset.x + renderOffset.x - monitor->m_position.x) *
                    scale,
                (logBox.y + offset.y + renderOffset.y - monitor->m_position.y) *
                    scale,
                projectedSize.x, projectedSize.y);
            sdata.isRootSurface = isRootSurface;

            if (isRootSurface &&
                g_pHyprOpenGL->m_renderData.primarySurfaceUVTopLeft !=
                    Vector2D(-1, -1) &&
                g_pHyprOpenGL->m_renderData.primarySurfaceUVBottomRight !=
                    Vector2D(-1, -1)) {
              sdata.uvTopLeft =
                  g_pHyprOpenGL->m_renderData.primarySurfaceUVTopLeft;
              sdata.uvBottomRight =
                  g_pHyprOpenGL->m_renderData.primarySurfaceUVBottomRight;
            } else if (surface->m_current.viewport.hasSource &&
                       surface->m_current.bufferSize.x > 0.F &&
                       surface->m_current.bufferSize.y > 0.F) {
              const auto &bufferSize = surface->m_current.bufferSize;
              const auto &bufferSource = surface->m_current.viewport.source;
              sdata.uvTopLeft = Vector2D(bufferSource.x / bufferSize.x,
                                         bufferSource.y / bufferSize.y);
              sdata.uvBottomRight =
                  Vector2D((bufferSource.x + bufferSource.width) /
                               bufferSize.x,
                           (bufferSource.y + bufferSource.height) /
                               bufferSize.y);

              if (sdata.uvBottomRight.x < 0.01f ||
                  sdata.uvBottomRight.y < 0.01f) {
                sdata.uvTopLeft = Vector2D(0.F, 0.F);
                sdata.uvBottomRight = Vector2D(1.F, 1.F);
              }
            }

            chromaData.surfaces.push_back(std::move(sdata));
          },
          nullptr);

      if (chromaData.surfaces.empty() && !g_loggedFallbackNoTexture) {
        Log::logger->log(Log::WARN,
                         "[Hyprchroma] No usable surface textures collected "
                         "for window, using uniform fallback");
        g_loggedFallbackNoTexture = true;
      } else if (chromaData.surfaces.size() > 1 &&
                 !g_config.unified_window_pass) {
        // Hyprland queues surfaces in breadthfirst compositor order and later
        // surfaces visually appear on top. Our stencil path is "first claim
        // wins", so we must tint in reverse render order to let the topmost
        // surface own the pixel before background/root surfaces can claim it.
        std::reverse(chromaData.surfaces.begin(), chromaData.surfaces.end());
      }
    }
  }

  if (!chromaData.surfaces.empty()) {
    if (g_config.debug_visualize > 0 && !g_notifiedShaderDebugPath) {
      HyprlandAPI::addNotification(pHandle,
                                   "[DarkWindow] Debug: shader path active",
                                   CHyprColor(0.2f, 1.0f, 0.2f, 1.0f), 2500);
      g_notifiedShaderDebugPath = true;
    }
    if (g_config.debug_visualize == 6 && !g_notifiedSurfaceDebugCount) {
      HyprlandAPI::addNotification(
          pHandle,
          std::format("[DarkWindow] Debug: {} surface(s) traced",
                      chromaData.surfaces.size()),
          CHyprColor(0.9f, 0.9f, 0.2f, 1.0f), 3500);
      g_notifiedSurfaceDebugCount = true;
    }
    if (!g_loggedShaderPath) {
      Log::logger->log(Log::INFO,
                       "[Hyprchroma] Using grouped shader tint path "
                       "(surfaces={}, clip={}x{})",
                       chromaData.surfaces.size(), clipBox.w, clipBox.h);
      g_loggedShaderPath = true;
    }
    if (g_config.unified_window_pass && !g_loggedUnifiedPath) {
      Log::logger->log(
          Log::INFO,
          "[Hyprchroma] Unified window pass enabled "
          "(compose full window into FBO before adaptive tint)");
      g_loggedUnifiedPath = true;
    }
    g_pHyprRenderer->m_renderPass.add(
        makeUnique<CChromaPassElement>(chromaData));
    if (g_config.debug_visualize == 6) {
      for (const auto &surf : chromaData.surfaces) {
        CRectPassElement::SRectData debugRect;
        debugRect.box = surf.box;
        debugRect.color = surf.isRootSurface
                              ? CHyprColor(0.15f, 1.0f, 0.2f, 0.12f)
                              : CHyprColor(1.0f, 0.55f, 0.05f, 0.12f);
        debugRect.round = surf.isRootSurface ? round : 0;
        debugRect.roundingPower = rPower;
        g_pHyprRenderer->m_renderPass.add(
            makeUnique<CRectPassElement>(debugRect));

        CBorderPassElement::SBorderData border;
        border.box = surf.box;
        border.grad1 = CGradientValueData(
            surf.isRootSurface ? CHyprColor(0.2f, 1.0f, 0.25f, 1.0f)
                               : CHyprColor(1.0f, 0.65f, 0.15f, 1.0f));
        border.a = 1.0f;
        border.round = surf.isRootSurface ? round : 0;
        border.borderSize = std::max(4, (int)std::round(6.F * scale));
        border.roundingPower = rPower;
        g_pHyprRenderer->m_renderPass.add(
            makeUnique<CBorderPassElement>(border));
      }
    }
  } else {
    if (g_config.debug_visualize > 0 && !g_notifiedFallbackDebugPath) {
      HyprlandAPI::addNotification(
          pHandle, "[DarkWindow] Debug: uniform fallback path active",
          CHyprColor(1.0f, 0.2f, 0.5f, 1.0f), 3500);
      g_notifiedFallbackDebugPath = true;
    }
    // v2 fallback: uniform color rect overlay
    CRectPassElement::SRectData data;
    data.box = overlayBox;
    data.color = (g_config.debug_visualize > 0 && g_config.debug_visualize != 6)
                     ? CHyprColor(1.0f, 0.0f, 0.5f, 0.35f * windowAlpha)
                     : CHyprColor(g_config.r, g_config.g, g_config.b,
                                  g_config.a * windowAlpha);
    data.round = round;
    data.roundingPower = rPower;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(data));
    if (g_config.debug_visualize == 6) {
      CRectPassElement::SRectData debugRect;
      debugRect.box = overlayBox;
      debugRect.color = CHyprColor(1.0f, 0.0f, 0.5f, 0.10f);
      debugRect.round = round;
      debugRect.roundingPower = rPower;
      g_pHyprRenderer->m_renderPass.add(
          makeUnique<CRectPassElement>(debugRect));

      CBorderPassElement::SBorderData border;
      border.box = overlayBox;
      border.grad1 = CGradientValueData(CHyprColor(1.0f, 0.0f, 0.5f, 1.0f));
      border.a = 1.0f;
      border.round = round;
      border.borderSize = std::max(4, (int)std::round(6.F * scale));
      border.roundingPower = rPower;
      g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(border));
    }
  }
}

static void redrawAll() {
  for (auto &m : g_pCompositor->m_monitors) {
    g_pHyprRenderer->damageMonitor(m);
    g_pCompositor->scheduleFrameForMonitor(m);
  }
}

static std::vector<SFunctionMatch>
findFunctionMatches(const std::string &query,
                    const std::string &demangledFilter = "") {
  auto matches = HyprlandAPI::findFunctionsByName(pHandle, query);

  if (demangledFilter.empty())
    return matches;

  std::erase_if(matches, [&](const SFunctionMatch &match) {
    return match.demangled.find(demangledFilter) == std::string::npos;
  });

  return matches;
}

static SRuntimeSymbolProbe
probeFunctionSymbol(const std::string &query,
                    const std::string &demangledFilter = "") {
  return {query, demangledFilter, findFunctionMatches(query, demangledFilter)};
}

static std::string boolWord(const bool value) {
  return value ? "yes" : "no";
}

static std::string jsonEscape(const std::string &value) {
  std::string escaped;
  escaped.reserve(value.size() + 16);

  for (const char c : value) {
    switch (c) {
    case '\\':
      escaped += "\\\\";
      break;
    case '"':
      escaped += "\\\"";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
      escaped += c;
      break;
    }
  }

  return escaped;
}

static std::string
formatSymbolProbeSummary(const SRuntimeSymbolProbe &probe,
                         const std::size_t maxMatches = 4) {
  std::ostringstream out;
  out << probe.query;
  if (!probe.demangledFilter.empty())
    out << " [filter: " << probe.demangledFilter << "]";
  out << " -> " << probe.matches.size() << " match(es)";

  const auto shown = std::min(maxMatches, probe.matches.size());
  for (std::size_t index = 0; index < shown; ++index)
    out << "\n  - " << probe.matches[index].demangled;

  if (probe.matches.size() > shown)
    out << "\n  - ... +" << (probe.matches.size() - shown) << " more";

  return out.str();
}

static SRuntimeProbeReport collectRuntimeProbeReport() {
  SRuntimeProbeReport report;
  report.runtimeVersion = HyprlandAPI::getHyprlandVersion(pHandle);
  report.hashMatchesBuild = report.runtimeVersion.hash == GIT_COMMIT_HASH;
  report.tagMatchesBuild = report.runtimeVersion.tag == GIT_TAG;

  report.useShader =
      probeFunctionSymbol("useShader", "CHyprOpenGLImpl::useShader");
  report.getSurfaceShader = probeFunctionSymbol(
      "getSurfaceShader", "CHyprOpenGLImpl::getSurfaceShader");
  report.renderTexture =
      probeFunctionSymbol("renderTexture", "CHyprOpenGLImpl::renderTexture");
  report.renderTextureInternalWithDamage =
      probeFunctionSymbol("renderTextureInternalWithDamage",
                          "CHyprOpenGLImpl::renderTextureInternalWithDamage");
  report.decorationGetDataFor =
      probeFunctionSymbol("getDataFor", "CDecorationPositioner::getDataFor");

  report.supportsModernShaderInsertion =
      !report.useShader.matches.empty() &&
      !report.getSurfaceShader.matches.empty();
  report.supportsDecorationHook = !report.decorationGetDataFor.matches.empty();

  report.safeForLowerLevelPrototype =
      report.hashMatchesBuild && report.modernRenderAPIHeadersPresent &&
      report.eventBusRenderStagePresent && report.preWindowStagePresent &&
      report.postWindowStagePresent && report.currentWindowRenderDataPresent &&
      report.supportsModernShaderInsertion;

  if (!report.hashMatchesBuild) {
    report.recommendation =
        "keep the safe post-window path: runtime hash differs from the build "
        "hash";
  } else if (report.safeForLowerLevelPrototype) {
    report.recommendation =
        "candidate for a guarded lower-level prototype via "
        "useShader/getSurfaceShader";
  } else if (report.supportsDecorationHook) {
    report.recommendation =
        "decoration hook is present, but no modern shader insertion point is "
        "confirmed; keep the safe post-window path";
  } else {
    report.recommendation =
        "no confirmed lower-level insertion point on this runtime; keep the "
        "safe post-window path";
  }

  g_runtimeProbeSafeForLowerLevel = report.safeForLowerLevelPrototype;
  return report;
}

static std::string
buildRuntimeProbeTextReport(const SRuntimeProbeReport &report) {
  std::ostringstream out;
  out << "Hyprchroma runtime probe\n";
  out << "runtime.tag: " << report.runtimeVersion.tag << "\n";
  out << "runtime.hash: " << report.runtimeVersion.hash << "\n";
  out << "runtime.branch: " << report.runtimeVersion.branch << "\n";
  out << "runtime.dirty: " << boolWord(report.runtimeVersion.dirty) << "\n";
  out << "build.tag: " << GIT_TAG << "\n";
  out << "build.hash: " << GIT_COMMIT_HASH << "\n";
  out << "hash_match: " << boolWord(report.hashMatchesBuild) << "\n";
  out << "tag_match: " << boolWord(report.tagMatchesBuild) << "\n";
  out << "headers.modern_render_api: "
      << boolWord(report.modernRenderAPIHeadersPresent) << "\n";
  out << "headers.event_bus_render_stage: "
      << boolWord(report.eventBusRenderStagePresent) << "\n";
  out << "headers.render_pre_window: " << boolWord(report.preWindowStagePresent)
      << "\n";
  out << "headers.render_post_window: "
      << boolWord(report.postWindowStagePresent) << "\n";
  out << "headers.current_window_render_data: "
      << boolWord(report.currentWindowRenderDataPresent) << "\n";
  out << "legacy_upstream_shader_swap_abi: "
      << boolWord(report.legacyShaderSwapABIAvailable) << "\n";
  out << "supports_modern_shader_insertion: "
      << boolWord(report.supportsModernShaderInsertion) << "\n";
  out << "supports_decoration_hook: "
      << boolWord(report.supportsDecorationHook) << "\n";
  out << "safe_for_lower_level_prototype: "
      << boolWord(report.safeForLowerLevelPrototype) << "\n";
  out << "recommendation: " << report.recommendation << "\n";
  out << "\n"
      << formatSymbolProbeSummary(report.useShader) << "\n";
  out << "\n"
      << formatSymbolProbeSummary(report.getSurfaceShader) << "\n";
  out << "\n"
      << formatSymbolProbeSummary(report.renderTexture) << "\n";
  out << "\n"
      << formatSymbolProbeSummary(report.renderTextureInternalWithDamage)
      << "\n";
  out << "\n"
      << formatSymbolProbeSummary(report.decorationGetDataFor);

  return out.str();
}

static std::string
buildRuntimeProbeJsonReport(const SRuntimeProbeReport &report) {
  const auto dumpSymbolProbe = [&](const SRuntimeSymbolProbe &probe) {
    std::ostringstream out;
    out << "{";
    out << "\"query\":\"" << jsonEscape(probe.query) << "\",";
    out << "\"filter\":\"" << jsonEscape(probe.demangledFilter) << "\",";
    out << "\"count\":" << probe.matches.size() << ",";
    out << "\"matches\":[";
    for (std::size_t index = 0; index < probe.matches.size(); ++index) {
      if (index != 0)
        out << ",";
      out << "{";
      out << "\"signature\":\"" << jsonEscape(probe.matches[index].signature)
          << "\",";
      out << "\"demangled\":\"" << jsonEscape(probe.matches[index].demangled)
          << "\"";
      out << "}";
    }
    out << "]";
    out << "}";
    return out.str();
  };

  std::ostringstream out;
  out << "{";
  out << "\"runtime\":{";
  out << "\"tag\":\"" << jsonEscape(report.runtimeVersion.tag) << "\",";
  out << "\"hash\":\"" << jsonEscape(report.runtimeVersion.hash) << "\",";
  out << "\"branch\":\"" << jsonEscape(report.runtimeVersion.branch) << "\",";
  out << "\"dirty\":" << (report.runtimeVersion.dirty ? "true" : "false");
  out << "},";
  out << "\"build\":{";
  out << "\"tag\":\"" << jsonEscape(GIT_TAG) << "\",";
  out << "\"hash\":\"" << jsonEscape(GIT_COMMIT_HASH) << "\"";
  out << "},";
  out << "\"capabilities\":{";
  out << "\"hash_match\":" << (report.hashMatchesBuild ? "true" : "false")
      << ",";
  out << "\"tag_match\":" << (report.tagMatchesBuild ? "true" : "false")
      << ",";
  out << "\"modern_render_api_headers_present\":"
      << (report.modernRenderAPIHeadersPresent ? "true" : "false") << ",";
  out << "\"event_bus_render_stage_present\":"
      << (report.eventBusRenderStagePresent ? "true" : "false") << ",";
  out << "\"render_pre_window_present\":"
      << (report.preWindowStagePresent ? "true" : "false") << ",";
  out << "\"render_post_window_present\":"
      << (report.postWindowStagePresent ? "true" : "false") << ",";
  out << "\"current_window_render_data_present\":"
      << (report.currentWindowRenderDataPresent ? "true" : "false") << ",";
  out << "\"legacy_upstream_shader_swap_abi_available\":"
      << (report.legacyShaderSwapABIAvailable ? "true" : "false") << ",";
  out << "\"supports_modern_shader_insertion\":"
      << (report.supportsModernShaderInsertion ? "true" : "false") << ",";
  out << "\"supports_decoration_hook\":"
      << (report.supportsDecorationHook ? "true" : "false") << ",";
  out << "\"safe_for_lower_level_prototype\":"
      << (report.safeForLowerLevelPrototype ? "true" : "false");
  out << "},";
  out << "\"symbols\":{";
  out << "\"useShader\":" << dumpSymbolProbe(report.useShader) << ",";
  out << "\"getSurfaceShader\":" << dumpSymbolProbe(report.getSurfaceShader)
      << ",";
  out << "\"renderTexture\":" << dumpSymbolProbe(report.renderTexture) << ",";
  out << "\"renderTextureInternalWithDamage\":"
      << dumpSymbolProbe(report.renderTextureInternalWithDamage) << ",";
  out << "\"decorationGetDataFor\":"
      << dumpSymbolProbe(report.decorationGetDataFor);
  out << "},";
  out << "\"recommendation\":\"" << jsonEscape(report.recommendation) << "\"";
  out << "}";
  return out.str();
}

static void logRuntimeProbeReport(const SRuntimeProbeReport &report,
                                  const bool verbose) {
  const auto text = buildRuntimeProbeTextReport(report);
  std::istringstream stream(text);
  std::string line;
  int lineNumber = 0;

  while (std::getline(stream, line)) {
    if (!verbose && lineNumber >= 16)
      break;
    Log::logger->log(Log::INFO, "[Hyprchroma] {}", line);
    ++lineNumber;
  }
}

static void notifyRuntimeProbeResult(const SRuntimeProbeReport &report) {
  const auto message = report.safeForLowerLevelPrototype
                           ? "[Hyprchroma] Runtime probe: guarded lower-level "
                             "prototype looks viable"
                           : "[Hyprchroma] Runtime probe: keep the safe "
                             "post-window path";
  const auto color = report.safeForLowerLevelPrototype
                         ? CHyprColor(0.15f, 0.9f, 0.25f, 1.0f)
                         : CHyprColor(0.95f, 0.75f, 0.15f, 1.0f);
  HyprlandAPI::addNotification(pHandle, message, color, 3500);
}

// ── Dispatchers ──

static SDispatchResult shadeDispatcher(std::string args) {
  if (args.find("address:") != std::string::npos) {
    PHLWINDOW target = nullptr;
    size_t space = args.find(' ');
    std::string addrStr = args.substr(
        8, (space == std::string::npos ? args.length() : space) - 8);

    addrStr.erase(0, addrStr.find_first_not_of(" \t\n\r"));
    addrStr.erase(addrStr.find_last_not_of(" \t\n\r") + 1);

    for (auto &w : g_pCompositor->m_windows) {
      std::ostringstream ss;
      ss << "0x" << std::hex << (uintptr_t)w.get();
      std::string currentHex = ss.str();
      std::ostringstream ss2;
      ss2 << std::hex << (uintptr_t)w.get();
      std::string currentHexShort = ss2.str();

      if (currentHex == addrStr || currentHexShort == addrStr) {
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
    const auto clearedOverrides = g_perWindowShaded.size();
    g_perWindowShaded.clear();

    if (args.find("on") != std::string::npos)
      g_globalShaded = true;
    else if (args.find("off") != std::string::npos)
      g_globalShaded = false;
    else
      g_globalShaded = !g_globalShaded;

    HyprlandAPI::addNotification(
        pHandle,
        std::format("[DarkWindow] Global shade {}{}",
                    g_globalShaded ? "ON" : "OFF",
                    clearedOverrides
                        ? std::format(" (cleared {} window override(s))",
                                      clearedOverrides)
                        : ""),
        CHyprColor(0.f, 1.f, 1.f, 1.f), 1500);
  }

  redrawAll();
  return {};
}

// ── Plugin entry points ──

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
  HyprlandAPI::addConfigValue(handle, "plugin:darkwindow:protect_brights",
                              Hyprlang::FLOAT{1.00f});
  HyprlandAPI::addConfigValue(handle, "plugin:darkwindow:bright_threshold",
                              Hyprlang::FLOAT{0.82f});
  HyprlandAPI::addConfigValue(handle, "plugin:darkwindow:bright_knee",
                              Hyprlang::FLOAT{0.12f});
  HyprlandAPI::addConfigValue(handle, "plugin:darkwindow:protect_saturated",
                              Hyprlang::FLOAT{0.85f});
  HyprlandAPI::addConfigValue(handle, "plugin:darkwindow:saturation_threshold",
                              Hyprlang::FLOAT{0.20f});
  HyprlandAPI::addConfigValue(handle, "plugin:darkwindow:saturation_knee",
                              Hyprlang::FLOAT{0.16f});
  HyprlandAPI::addConfigValue(handle, "plugin:darkwindow:debug_visualize",
                              Hyprlang::INT{0});
  HyprlandAPI::addConfigValue(handle, "plugin:darkwindow:enable_on_fullscreen",
                              Hyprlang::INT{1});
  HyprlandAPI::addConfigValue(handle, "plugin:darkwindow:tint_all_surfaces",
                              Hyprlang::INT{1});
  HyprlandAPI::addConfigValue(handle, "plugin:darkwindow:unified_window_pass",
                              Hyprlang::INT{0});
  HyprlandAPI::addConfigValue(handle,
                              "plugin:darkwindow:native_surface_shader_pass",
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
      Hyprlang::INT{150});

  updateConfig();

  g_renderListener = Event::bus()->m_events.render.stage.listen(
      [](eRenderStage stage) { onRenderStage(stage); });

  g_configListener =
      Event::bus()->m_events.config.reloaded.listen([]() { updateConfig(); });

  g_destroyWindowListener = Event::bus()->m_events.window.destroy.listen(
      [](PHLWINDOW w) { g_perWindowShaded.erase((void *)w.get()); });

  g_workspaceListener = Event::bus()->m_events.workspace.active.listen(
      [](const PHLWORKSPACE &) {
        if (g_config.suspend_on_workspace_switch_ms <= 0)
          return;
        g_suspendUntil = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds(
                             g_config.suspend_on_workspace_switch_ms);
        redrawAll();
      });

  g_mouseMoveListener = Event::bus()->m_events.input.mouse.move.listen(
      [](const Vector2D &delta, Event::SCallbackInfo &info) {
        invalidateFromCursorMotion(delta, info);
      });

  if (!HyprlandAPI::addDispatcherV2(handle, "togglechromakey",
                                    [](std::string args) -> SDispatchResult {
                                      return shadeDispatcher(args);
                                    })) {
    Log::logger->log(Log::WARN,
                     "[Hyprchroma] Failed to register dispatcher "
                     "togglechromakey");
  }

  if (!HyprlandAPI::addDispatcherV2(handle, "darkwindow:shade",
                                    [](std::string args) -> SDispatchResult {
                                      return shadeDispatcher(args);
                                    })) {
    Log::logger->log(Log::WARN,
                     "[Hyprchroma] Failed to register dispatcher "
                     "darkwindow:shade");
  }

  const auto initialProbe = collectRuntimeProbeReport();

  if (initialProbe.safeForLowerLevelPrototype) {
    const auto installHook = [&](const std::string &query,
                                 const std::string &filter,
                                 const void *destination,
                                 CFunctionHook *&slot) {
      auto matches = findFunctionMatches(query, filter);
      if (matches.size() != 1) {
        Log::logger->log(
            Log::WARN,
            "[Hyprchroma] Expected exactly one match for {} [{}], got {}",
            query, filter, matches.size());
        return false;
      }

      slot = HyprlandAPI::createFunctionHook(handle, matches.front().address,
                                             destination);
      if (!slot) {
        Log::logger->log(Log::WARN,
                         "[Hyprchroma] Failed to allocate hook for {}", filter);
        return false;
      }

      if (!slot->hook()) {
        Log::logger->log(Log::WARN,
                         "[Hyprchroma] Failed to activate hook for {}", filter);
        HyprlandAPI::removeFunctionHook(handle, slot);
        slot = nullptr;
        return false;
      }

      return true;
    };

    const bool hookedGetSurfaceShader = installHook(
        "getSurfaceShader", "CHyprOpenGLImpl::getSurfaceShader",
        reinterpret_cast<void *>(hkGetSurfaceShader), g_getSurfaceShaderHook);
    const bool hookedUseShader = installHook(
        "useShader", "CHyprOpenGLImpl::useShader",
        reinterpret_cast<void *>(hkUseShader), g_useShaderHook);

    if (hookedGetSurfaceShader && hookedUseShader && !g_loggedNativeHooks) {
      Log::logger->log(
          Log::INFO,
          "[Hyprchroma] Installed guarded lower-level hooks "
          "(getSurfaceShader/useShader)");
      g_loggedNativeHooks = true;
    }
  }

  g_runtimeProbeCommand = HyprlandAPI::registerHyprCtlCommand(
      handle, {.name = "darkwindowprobe",
               .exact = true,
               .fn =
                   [](eHyprCtlOutputFormat format, std::string) {
                     const auto report = collectRuntimeProbeReport();
                     return format == FORMAT_JSON
                                ? buildRuntimeProbeJsonReport(report)
                                : buildRuntimeProbeTextReport(report);
                   }});

  if (!g_runtimeProbeCommand)
    Log::logger->log(
        Log::WARN,
        "[Hyprchroma] Failed to register hyprctl command darkwindowprobe");
  else
    Log::logger->log(Log::INFO,
                    "[Hyprchroma] hyprctl command registration ok for "
                     "darkwindowprobe");

  HyprlandAPI::addNotification(handle,
                               "[DarkWindow] Registered v3.4.0 "
                               "(Grouped adaptive chromakey tint + guarded "
                               "native surface shader path)",
                               CHyprColor(0.f, 1.f, 0.f, 1.f), 3000);

  logRuntimeProbeReport(initialProbe, false);

  return {"DarkWindow", "Grouped adaptive per-pixel chromakey tint", "tco",
          "3.4.0"};
}

APICALL EXPORT void pluginExit() {
  g_renderListener.reset();
  g_configListener.reset();
  g_destroyWindowListener.reset();
  g_workspaceListener.reset();
  g_mouseMoveListener.reset();
  if (g_runtimeProbeCommand) {
    HyprlandAPI::unregisterHyprCtlCommand(pHandle, g_runtimeProbeCommand);
    g_runtimeProbeCommand.reset();
  }
  if (g_useShaderHook) {
    HyprlandAPI::removeFunctionHook(pHandle, g_useShaderHook);
    g_useShaderHook = nullptr;
  }
  if (g_getSurfaceShaderHook) {
    HyprlandAPI::removeFunctionHook(pHandle, g_getSurfaceShaderHook);
    g_getSurfaceShaderHook = nullptr;
  }
  g_perWindowShaded.clear();
  g_nativeShadedThisFrame.clear();
  g_nativeSurfaceShaders.clear();
  g_nativeSurfaceShaderFailures.clear();
  g_nativeExtShader = {};
  g_nativeExtShaderCompileAttempted = false;

  if (g_chromaProgram) {
    glDeleteProgram(g_chromaProgram);
    g_chromaProgram = 0;
  }
  if (g_chromaProgram_ext) {
    glDeleteProgram(g_chromaProgram_ext);
    g_chromaProgram_ext = 0;
  }
  if (g_blitProgram) {
    glDeleteProgram(g_blitProgram);
    g_blitProgram = 0;
  }
  if (g_blitProgram_ext) {
    glDeleteProgram(g_blitProgram_ext);
    g_blitProgram_ext = 0;
  }
  if (g_chromaVBO) {
    glDeleteBuffers(1, &g_chromaVBO);
    g_chromaVBO = 0;
  }
  if (g_chromaVAO) {
    glDeleteVertexArrays(1, &g_chromaVAO);
    g_chromaVAO = 0;
  }
  g_shadersCompiled = false;
  g_notifiedShaderDebugPath = false;
  g_notifiedFallbackDebugPath = false;
  g_notifiedSurfaceDebugCount = false;
  g_loggedUnifiedPath = false;
  g_loggedNativeShaderPath = false;
  g_loggedNativeHooks = false;
  g_loggedUnifiedFallbackNoExternalProgram = false;

  redrawAll();
}
