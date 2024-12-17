#pragma once

#include <vector>
#include <hyprland/src/Compositor.hpp>

#include "Helpers.h"


class WindowInverter
{
public:
    void Init(HANDLE pluginHandle);
    void Unload();

    void SetBackground(GLfloat r, GLfloat g, GLfloat b);

    void InvertIfMatches(PHLWINDOW window);
    void ToggleInvert(PHLWINDOW window);
    void SoftToggle(bool invert);
    void Reload();

    void OnRenderWindowPre();
    void OnRenderWindowPost();
    void OnWindowClose(PHLWINDOW window);

    void NoIgnoreDecorations()
    {
        m_IgnoreDecorations = {};
    }

private:
    HANDLE m_PluginHandle;

    std::vector<CWindowRule> m_InvertWindowRules;
    std::vector<PHLWINDOW> m_InvertedWindows;
    std::vector<PHLWINDOW> m_ManuallyInvertedWindows;

    std::optional<bool> m_IgnoreDecorations = true;
    bool m_DecorationsWrapped = false;   

    ShaderHolder m_Shaders;
    bool m_ShadersSwapped = false;

    GLfloat bkgR = 0.0f;
    GLfloat bkgG = 0.0f;
    GLfloat bkgB = 0.0f;
};