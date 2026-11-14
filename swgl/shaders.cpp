// Shader recognition: map the game's GLSL sources to the C++ stages swgl implements.
// cocos2d-x 2.x built-in shaders, matched on their defining expressions (whitespace-insensitive).
#include "swgl_internal.hpp"
#include <algorithm>

namespace swgl {
namespace {
std::string squash(const std::string& s) {   // drop whitespace and // comments
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/') { while (i < s.size() && s[i] != '\n') i++; continue; }
        if (!isspace((unsigned char)s[i])) out += s[i];
    }
    return out;
}
bool has(const std::string& sq, const char* needle) { return sq.find(squash(needle)) != std::string::npos; }
}  // namespace

VertexShape classify_vertex(const std::string& src) {
    std::string sq = squash(src);
    VertexShape v;
    if (has(sq, "attribute vec2 a_texCoord") || has(sq, "attribute mediump vec2 a_texCoord")) { v.uv_attr = "a_texCoord"; v.has_uv = true; }
    if (has(sq, "attribute vec2 a_texcoord") || has(sq, "attribute mediump vec2 a_texcoord")) { v.uv_attr = "a_texcoord"; v.has_uv = true; }
    if (has(sq, "attribute vec4 a_color") || has(sq, "attribute mediump vec4 a_color")) { v.color_attr = "a_color"; v.has_color = true; }
    if (has(sq, "v_fragmentColor = u_color")) { v.color_from_uniform = true; v.has_color = true; }
    if (has(sq, "vec4(a_color.rgb * a_color.a, a_color.a)")) v.premultiply_color = true;
    return v;
}

FKind classify_fragment(const std::string& src) {
    std::string sq = squash(src);
    if (has(sq, "texture2D(CC_Texture0, v_texCoord) * u_color")) return FKind::TexUColor;
    if (has(sq, "u_mask")) return FKind::TexMask;
    if (has(sq, "CC_alpha_value")) return FKind::TexAlphaTest;
    if (has(sq, "v_fragmentColor.a * texture2D(CC_Texture0, v_texCoord).a")) return FKind::TexAlphaOnly;
    if (has(sq, "v_color*step(0.0, 1.0 - length(v_texcoord))")) return FKind::Circle;
    if (has(sq, "gl_FragColor = v_fragmentColor * texture2D(CC_Texture0, v_texCoord)")) return FKind::TexColor;
    if (has(sq, "gl_FragColor = texture2D(CC_Texture0, v_texCoord);")) return FKind::Tex;
    if (has(sq, "gl_FragColor = v_fragmentColor;")) return FKind::Color;
    return FKind::Unknown;
}

const char* fkind_name(FKind k) {
    switch (k) {
    case FKind::Color: return "color"; case FKind::Tex: return "texture"; case FKind::TexColor: return "texture*color";
    case FKind::TexUColor: return "texture*u_color"; case FKind::TexAlphaOnly: return "color with texture alpha";
    case FKind::TexAlphaTest: return "texture*color + alpha test"; case FKind::TexMask: return "texture with mask";
    case FKind::Circle: return "circle"; default: return "UNKNOWN";
    }
}

}  // namespace swgl
