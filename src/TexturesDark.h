#pragma once

#include <string>
#include <format>

#include <hyprland/src/render/shaders/Textures.hpp>


inline static const std::string DARK_MODE_FUNC = R"glsl(
uniform bool doInvert;
uniform vec3 bkg;

void invert(inout vec4 color) {
    if (doInvert) {
        // Original shader by ikz87

        // Apply opacity changes to pixels similar to one color
        // vec3 color_rgb = vec3(0,0,255); // Color to replace, in rgb format
        float similarity = 0.1; // How many similar colors should be affected.

        float amount = 1.4; // How much similar colors should be changed.
        float target_opacity = 0.83;
        // Change any of the above values to get the result you want

        // Set values to a 0 - 1 range
        vec3 chroma = vec3(bkg[0]/255.0, bkg[1]/255.0, bkg[2]/255.0);

        if (color.x >=chroma.x - similarity && color.x <=chroma.x + similarity &&
                color.y >=chroma.y - similarity && color.y <=chroma.y + similarity &&
                color.z >=chroma.z - similarity && color.z <=chroma.z + similarity &&
                color.w >= 0.99)
        {
            // Calculate error between matched pixel and color_rgb values
                vec3 error = vec3(abs(chroma.x - color.x), abs(chroma.y - color.y), abs(chroma.z - color.z));
            float avg_error = (error.x + error.y + error.z) / 3.0;
                color.w = target_opacity + (1.0 - target_opacity)*avg_error*amount/similarity;

            // color.rgba = vec4(0, 0, 1, 0.5);
        }
    }
}
)glsl";


inline const std::string TEXFRAGSRCRGBA_DARK = R"glsl(
precision mediump float;
varying vec2 v_texcoord; // is in 0-1
uniform sampler2D tex;
uniform float alpha;

uniform vec2 topLeft;
uniform vec2 fullSize;
uniform float radius;

uniform int discardOpaque;
uniform int discardAlpha;
uniform float discardAlphaValue;

uniform int applyTint;
uniform vec3 tint;

)glsl" + DARK_MODE_FUNC + R"glsl(

void main() {

    vec4 pixColor = texture2D(tex, v_texcoord);

    if (discardOpaque == 1 && pixColor[3] * alpha == 1.0)
	    discard;

    if (discardAlpha == 1 && pixColor[3] <= discardAlphaValue)
        discard;

    if (applyTint == 2) {
	    pixColor[0] = pixColor[0] * tint[0];
	    pixColor[1] = pixColor[1] * tint[1];
	    pixColor[2] = pixColor[2] * tint[2];
    }

    invert(pixColor);

    if (radius > 0.0) {
    )glsl" +
    ROUNDED_SHADER_FUNC("pixColor") + R"glsl(
    }

    gl_FragColor = pixColor * alpha;
})glsl";

inline const std::string TEXFRAGSRCRGBX_DARK = R"glsl(
precision mediump float;
varying vec2 v_texcoord;
uniform sampler2D tex;
uniform float alpha;

uniform vec2 topLeft;
uniform vec2 fullSize;
uniform float radius;

uniform int discardOpaque;
uniform int discardAlpha;
uniform int discardAlphaValue;

uniform int applyTint;
uniform vec3 tint;

)glsl" + DARK_MODE_FUNC + R"glsl(

void main() {

    if (discardOpaque == 1 && alpha == 1.0)
	discard;

    vec4 pixColor = vec4(texture2D(tex, v_texcoord).rgb, 1.0);

    if (applyTint == 2) {
        pixColor[0] = pixColor[0] * tint[0];
        pixColor[1] = pixColor[1] * tint[1];
        pixColor[2] = pixColor[2] * tint[2];
    }

    invert(pixColor);

    if (radius > 0.0) {
    )glsl" +
    ROUNDED_SHADER_FUNC("pixColor") + R"glsl(
    }

    gl_FragColor = pixColor * alpha;
})glsl";

inline const std::string TEXFRAGSRCEXT_DARK = R"glsl(
#extension GL_OES_EGL_image_external : require

precision mediump float;
varying vec2 v_texcoord;
uniform samplerExternalOES texture0;
uniform float alpha;

uniform vec2 topLeft;
uniform vec2 fullSize;
uniform float radius;

uniform int discardOpaque;
uniform int discardAlpha;
uniform int discardAlphaValue;

uniform int applyTint;
uniform vec3 tint;

)glsl" + DARK_MODE_FUNC + R"glsl(

void main() {

    vec4 pixColor = texture2D(texture0, v_texcoord);

    if (discardOpaque == 1 && pixColor[3] * alpha == 1.0)
        discard;

    if (applyTint == 2) {
        pixColor[0] = pixColor[0] * tint[0];
        pixColor[1] = pixColor[1] * tint[1];
        pixColor[2] = pixColor[2] * tint[2];
    }

    invert(pixColor);

    if (radius > 0.0) {
    )glsl" +
    ROUNDED_SHADER_FUNC("pixColor") + R"glsl(
    }

    gl_FragColor = pixColor * alpha;
}
)glsl";
