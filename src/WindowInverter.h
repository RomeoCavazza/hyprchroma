#pragma once

#include <vector>
#include <hyprland/src/Compositor.hpp>

#include "Helpers.h"


class WindowInverter
{
public:
    void Init();
    void Unload();

    void SetBackground(GLfloat r, GLfloat g, GLfloat b);

    void InvertIfMatches(CWindow* window);
    void ToggleInvert(CWindow* window);
    void SetRules(std::vector<SWindowRule>&& rules);
    void Reload();

    void OnRenderWindowPre();
    void OnRenderWindowPost();
    void OnWindowClose(CWindow* window);

private:
    std::vector<SWindowRule> m_InvertWindowRules;
    std::vector<CWindow*> m_InvertedWindows;
    std::vector<CWindow*> m_ManuallyInvertedWindows;

    ShaderHolder m_Shaders;
    bool m_ShadersSwapped = false;

    GLfloat bkgR = 0.0f;
    GLfloat bkgG = 0.0f;
    GLfloat bkgB = 0.0f;
};