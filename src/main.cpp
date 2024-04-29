#include "WindowInverter.h"

#include <hyprland/src/SharedDefs.hpp>

inline HANDLE PHANDLE = nullptr;

inline WindowInverter g_WindowInverter;
inline std::mutex g_InverterMutex;

inline CFunctionHook* g_SetConfigValueHook;
inline std::vector<SWindowRule> g_WindowRulesBuildup;

Hyprlang::CParseResult onInvertKeyword(const char* COMMAND, const char* VALUE)
{
    Hyprlang::CParseResult res;
    try
    {
        g_WindowRulesBuildup.push_back(ParseRule(VALUE));
    }
    catch (const std::exception& ex)
    {
        res.setError(ex.what());
    }
    return res;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle)
{
    PHANDLE = handle;

    {
        std::lock_guard<std::mutex> lock(g_InverterMutex);
        g_WindowInverter.Init();
        g_pConfigManager->m_bForceReload = true;
    }

    using PCONFIGHANDLERFUNC = Hyprlang::CParseResult(*)(const char* COMMAND, const char* VALUE);

    HyprlandAPI::addConfigKeyword(
        handle, "chromakey_enable",
        onInvertKeyword,
        { .allowFlags = false }
    );

    HyprlandAPI::addConfigKeyword(
        handle, "chromakey_background",
        [](const char* cmd, const char* val) -> Hyprlang::CParseResult {
            // Parse val as "r,g,b" into 3 GLfloats
            std::vector<std::string> result;
            std::stringstream ss (val);
            std::string component;

            getline(ss, component, ',');
            GLfloat r = std::stof(component);
            getline(ss, component, ',');
            GLfloat g = std::stof(component);
            getline(ss, component, ',');
            GLfloat b = std::stof(component);

            g_WindowInverter.SetBackground(r, g, b);

            return Hyprlang::CParseResult(); // return a default CParseResult
        },
        { .allowFlags = false }
    );

    HyprlandAPI::registerCallbackDynamic(
        PHANDLE, "render",
        [&](void* self, SCallbackInfo&, std::any data) {
        std::lock_guard<std::mutex> lock(g_InverterMutex);
        eRenderStage renderStage = std::any_cast<eRenderStage>(data);

        if (renderStage == eRenderStage::RENDER_PRE_WINDOW)
            g_WindowInverter.OnRenderWindowPre();
        if (renderStage == eRenderStage::RENDER_POST_WINDOW)
            g_WindowInverter.OnRenderWindowPost();
    }
    );
    HyprlandAPI::registerCallbackDynamic(
        PHANDLE, "configReloaded",
        [&](void* self, SCallbackInfo&, std::any data) {
        std::lock_guard<std::mutex> lock(g_InverterMutex);
        g_WindowInverter.SetRules(std::move(g_WindowRulesBuildup));
        g_WindowRulesBuildup = {};
    }
    );
    HyprlandAPI::registerCallbackDynamic(
        PHANDLE, "closeWindow",
        [&](void* self, SCallbackInfo&, std::any data) {
        std::lock_guard<std::mutex> lock(g_InverterMutex);
        g_WindowInverter.OnWindowClose(std::any_cast<CWindow*>(data));
    }
    );

    HyprlandAPI::registerCallbackDynamic(
        PHANDLE, "moveWindow",
        [&](void* self, SCallbackInfo&, std::any data) {
        std::lock_guard<std::mutex> lock(g_InverterMutex);
        g_WindowInverter.InvertIfMatches(std::any_cast<CWindow*>(std::any_cast<std::vector<std::any>>(data)[0]));
    }
    );

    for (const auto& event : { "openWindow", "fullscreen", "changeFloatingMode", "windowtitle" })
    {
        HyprlandAPI::registerCallbackDynamic(
            PHANDLE, event,
            [&](void* self, SCallbackInfo&, std::any data) {
            std::lock_guard<std::mutex> lock(g_InverterMutex);
            g_WindowInverter.InvertIfMatches(std::any_cast<CWindow*>(data));
        }
        );
    }


    HyprlandAPI::addDispatcher(PHANDLE, "invertwindow", [&](std::string args) {
        std::lock_guard<std::mutex> lock(g_InverterMutex);
        g_WindowInverter.ToggleInvert(g_pCompositor->getWindowByRegex(args));
    });
    HyprlandAPI::addDispatcher(PHANDLE, "invertactivewindow", [&](std::string args) {
        // Debug::log(INFO, "Something something woo");
        std::lock_guard<std::mutex> lock(g_InverterMutex);
        // Debug::log(INFO, "Something something yay");
        g_WindowInverter.ToggleInvert(g_pCompositor->m_pLastWindow);
        // Debug::log(INFO, "Something something nice");
    });

    return {
        "hyprchroma",
        "Applies ChromaKey algorithm to windows for transparency effect",
        "alexhulbert",
        "1.0.0"
    };
}

APICALL EXPORT void PLUGIN_EXIT()
{
    std::lock_guard<std::mutex> lock(g_InverterMutex);
    g_WindowInverter.Unload();
}

APICALL EXPORT std::string PLUGIN_API_VERSION()
{
    return HYPRLAND_API_VERSION;
}
