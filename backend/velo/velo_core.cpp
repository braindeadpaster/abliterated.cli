// velo_core.cpp — theme, colour, easing, the shared draw layer and the icon set.
// Mirrors VelocityTheme.cs, ColorUtil.cs, Easing.cs, Draw.cs and Glyphs.cs.

#include "velo.h"
#include "velo_internal.h"
#include "velo_lucide.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace velo
{
    // ================================================================= state
    namespace detail
    {
        State g_state;
    }

    // ================================================================= colour
    ImU32 RGBA(int r, int g, int b, int a)
    {
        return IM_COL32(r & 0xFF, g & 0xFF, b & 0xFF, a & 0xFF);
    }

    ImU32 WithAlpha(ImU32 c, float mul)
    {
        int a = (int)(((c >> IM_COL32_A_SHIFT) & 0xFF) * Clamp01(mul));
        return (c & ~IM_COL32_A_MASK) | ((ImU32)a << IM_COL32_A_SHIFT);
    }

    ImU32 SetAlpha(ImU32 c, int a)
    {
        if (a < 0) a = 0; else if (a > 255) a = 255;
        return (c & ~IM_COL32_A_MASK) | ((ImU32)a << IM_COL32_A_SHIFT);
    }

    ImU32 LerpCol(ImU32 a, ImU32 b, float t)
    {
        t = Clamp01(t);
        int ar = (a >> IM_COL32_R_SHIFT) & 0xFF, ag = (a >> IM_COL32_G_SHIFT) & 0xFF;
        int ab = (a >> IM_COL32_B_SHIFT) & 0xFF, aa = (a >> IM_COL32_A_SHIFT) & 0xFF;
        int br = (b >> IM_COL32_R_SHIFT) & 0xFF, bg = (b >> IM_COL32_G_SHIFT) & 0xFF;
        int bb = (b >> IM_COL32_B_SHIFT) & 0xFF, ba = (b >> IM_COL32_A_SHIFT) & 0xFF;
        return IM_COL32((int)(ar + (br - ar) * t), (int)(ag + (bg - ag) * t),
                        (int)(ab + (bb - ab) * t), (int)(aa + (ba - aa) * t));
    }

    ImU32 Lighten(ImU32 c, float amount) { return LerpCol(c, IM_COL32_WHITE, amount); }
    ImU32 Darken(ImU32 c, float amount) { return LerpCol(c, IM_COL32_BLACK, amount); }

    ImU32 FromHsv(float h, float s, float v, int a)
    {
        h = h - floorf(h);
        s = Clamp01(s); v = Clamp01(v);
        float c = v * s;
        float x = c * (1.0f - fabsf(fmodf(h * 6.0f, 2.0f) - 1.0f));
        float m = v - c;
        float r, g, b;
        switch ((int)(h * 6.0f) % 6)
        {
        case 0: r = c; g = x; b = 0; break;
        case 1: r = x; g = c; b = 0; break;
        case 2: r = 0; g = c; b = x; break;
        case 3: r = 0; g = x; b = c; break;
        case 4: r = x; g = 0; b = c; break;
        default: r = c; g = 0; b = x; break;
        }
        return IM_COL32((int)((r + m) * 255.0f + 0.5f), (int)((g + m) * 255.0f + 0.5f),
                        (int)((b + m) * 255.0f + 0.5f), a);
    }

    void ToHsv(ImU32 col, float& h, float& s, float& v)
    {
        float r = ((col >> IM_COL32_R_SHIFT) & 0xFF) / 255.0f;
        float g = ((col >> IM_COL32_G_SHIFT) & 0xFF) / 255.0f;
        float b = ((col >> IM_COL32_B_SHIFT) & 0xFF) / 255.0f;
        float mx = VMax(r, VMax(g, b)), mn = VMin(r, VMin(g, b));
        float d = mx - mn;
        v = mx;
        s = mx <= 0.0f ? 0.0f : d / mx;
        if (d <= 0.0f) { h = 0.0f; return; }
        if (mx == r)      h = fmodf((g - b) / d, 6.0f) / 6.0f;
        else if (mx == g) h = ((b - r) / d + 2.0f) / 6.0f;
        else              h = ((r - g) / d + 4.0f) / 6.0f;
        if (h < 0.0f) h += 1.0f;
    }

    ImU32 Rainbow(double seconds, float speed, float sat, float val)
    {
        return FromHsv((float)fmod(seconds * speed, 1.0), sat, val);
    }

    float Luminance(ImU32 c)
    {
        float r = (float)((c >> IM_COL32_R_SHIFT) & 0xFF);
        float g = (float)((c >> IM_COL32_G_SHIFT) & 0xFF);
        float b = (float)((c >> IM_COL32_B_SHIFT) & 0xFF);
        return (0.299f * r + 0.587f * g + 0.114f * b) / 255.0f;
    }

    ImU32 ReadableOn(ImU32 bg)
    {
        return Luminance(bg) > 0.55f ? IM_COL32(16, 16, 18, 255) : IM_COL32(245, 245, 250, 255);
    }

    ImU32 AccentAt(float u)
    {
        const Theme& t = theme();
        if (!t.AccentGradient) return t.Accent;
        ImU32 end = t.Accent2 ? t.Accent2 : t.Accent;
        return LerpCol(t.Accent, end, Clamp01(u));
    }

    void ToHex(ImU32 c, char* out, size_t outSize)
    {
        int r = (c >> IM_COL32_R_SHIFT) & 0xFF, g = (c >> IM_COL32_G_SHIFT) & 0xFF;
        int b = (c >> IM_COL32_B_SHIFT) & 0xFF, a = (c >> IM_COL32_A_SHIFT) & 0xFF;
        if (a == 255) snprintf(out, outSize, "#%02X%02X%02X", r, g, b);
        else          snprintf(out, outSize, "#%02X%02X%02X%02X", a, r, g, b);
    }

    // ================================================================= easing
    float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
    float ClampF(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

    float Approach(float current, float target, float k, float dt)
    {
        return target + (current - target) * expf(-k * dt);
    }

    float EaseF(Ease kind, float t)
    {
        t = Clamp01(t);
        switch (kind)
        {
        case Ease::OutCubic: { float u = 1.0f - t; return 1.0f - u * u * u; }
        case Ease::OutQuint: { float u = 1.0f - t; return 1.0f - u * u * u * u * u; }
        case Ease::InOutCubic:
            return t < 0.5f ? 4.0f * t * t * t : 1.0f - powf(-2.0f * t + 2.0f, 3.0f) / 2.0f;
        case Ease::OutBack:
        {
            const float c1 = 1.70158f, c3 = c1 + 1.0f;
            float u = t - 1.0f;
            return 1.0f + c3 * u * u * u + c1 * u * u;
        }
        case Ease::OutExpo: return t >= 1.0f ? 1.0f : 1.0f - powf(2.0f, -10.0f * t);
        default: return t;
        }
    }

    float Anim(ImGuiID id, float target, float speed)
    {
        ImGuiStorage* st = ImGui::GetStateStorage();
        float cur = st->GetFloat(id, target);
        if (!theme().Animations)
        {
            st->SetFloat(id, target);
            return target;
        }
        cur = Approach(cur, target, speed, detail::g_state.dt);
        if (fabsf(cur - target) < 0.0008f) cur = target;
        st->SetFloat(id, cur);
        return cur;
    }

    // ================================================================= frame
    void NewFrame()
    {
        detail::State& s = detail::g_state;
        float dt = ImGui::GetIO().DeltaTime;
        if (dt <= 0.0f) dt = 1.0f / 60.0f;
        else if (dt > 0.1f) dt = 0.1f;         // clamp after a stall, like the C# loop
        s.dt = dt * theme().AnimationSpeed;
        s.time += s.dt;
        s.alphaMul = 1.0f;
        s.page = nullptr;
        s.untitled = 0;
        s.slidePending = 0.0f;
        s.slideWindow = nullptr;
        LoadIcons();                            // no-op once loaded, or while the atlas is locked
    }

    double Time() { return detail::g_state.time; }

    // ================================================================= theme
    static Theme MakeBase()
    {
        Theme t = {};
        t.Radius = 0.0f;
        t.BorderThickness = 1.0f;
        t.RowHeight = 18.0f;
        t.ItemSpacing = 4.0f;
        t.InnerPadding = 8.0f;
        t.TabHeight = 24.0f;
        t.Density = 1.0f;
        t.TextShadows = true;
        t.Animations = true;
        t.AnimationSpeed = 1.0f;
        return t;
    }

    static Theme MakeNavy()
    {
        Theme t = MakeBase();
        t.Accent = RGBA(82, 122, 190);   t.AccentText = RGBA(240, 244, 252);
        t.Text = RGBA(206, 214, 232);    t.TextInactive = RGBA(104, 116, 142);
        t.TextShadow = RGBA(6, 8, 14, 190);
        t.Outline = RGBA(8, 10, 16);     t.Inline = RGBA(52, 62, 86);
        t.Content = RGBA(24, 29, 42);    t.Tab = RGBA(20, 25, 36);
        t.Section = RGBA(28, 34, 48);    t.Element = RGBA(17, 21, 31);
        t.ElementHover = RGBA(35, 43, 60); t.Border = RGBA(46, 55, 76);
        t.Risky = RGBA(198, 72, 76);     t.Positive = RGBA(104, 176, 116);
        t.Warning = RGBA(206, 166, 76);
        return t;
    }

    static Theme MakeIndigo()
    {
        Theme t = MakeBase();
        t.Accent = RGBA(122, 108, 220);  t.AccentText = RGBA(244, 242, 255);
        t.Text = RGBA(212, 208, 228);    t.TextInactive = RGBA(108, 102, 132);
        t.TextShadow = RGBA(4, 3, 8, 190);
        t.Outline = RGBA(96, 84, 200);   t.Inline = RGBA(38, 34, 54);
        t.Content = RGBA(15, 14, 20);    t.Tab = RGBA(12, 11, 16);
        t.Section = RGBA(18, 17, 24);    t.Element = RGBA(11, 10, 15);
        t.ElementHover = RGBA(30, 27, 42); t.Border = RGBA(46, 42, 64);
        t.Risky = RGBA(206, 78, 92);     t.Positive = RGBA(110, 190, 130);
        t.Warning = RGBA(212, 176, 84);
        return t;
    }

    static Theme MakeCarbon()
    {
        Theme t = MakeBase();
        t.Accent = RGBA(198, 172, 76);   t.AccentText = RGBA(24, 21, 12);
        t.Text = RGBA(214, 214, 214);    t.TextInactive = RGBA(116, 116, 116);
        t.TextShadow = RGBA(0, 0, 0, 190);
        t.Outline = RGBA(10, 10, 10);    t.Inline = RGBA(64, 64, 64);
        t.Content = RGBA(32, 32, 32);    t.Tab = RGBA(26, 26, 26);
        t.Section = RGBA(38, 38, 38);    t.Element = RGBA(22, 22, 22);
        t.ElementHover = RGBA(48, 48, 48); t.Border = RGBA(58, 58, 58);
        t.Risky = RGBA(192, 76, 72);     t.Positive = RGBA(112, 172, 108);
        t.Warning = RGBA(206, 166, 72);
        return t;
    }

    static Theme MakeRose()
    {
        Theme t = MakeBase();
        t.Accent = RGBA(232, 96, 152);   t.AccentText = RGBA(28, 12, 20);
        t.Text = RGBA(226, 218, 224);    t.TextInactive = RGBA(122, 110, 120);
        t.TextShadow = RGBA(6, 3, 6, 190);
        t.Outline = RGBA(212, 84, 138);  t.Inline = RGBA(40, 30, 38);
        t.Content = RGBA(22, 18, 22);    t.Tab = RGBA(18, 15, 19);
        t.Section = RGBA(27, 22, 27);    t.Element = RGBA(16, 13, 17);
        t.ElementHover = RGBA(40, 32, 40); t.Border = RGBA(54, 44, 54);
        t.Risky = RGBA(216, 82, 88);     t.Positive = RGBA(116, 194, 138);
        t.Warning = RGBA(216, 176, 88);
        return t;
    }

    static Theme MakeAzure()
    {
        Theme t = MakeBase();
        t.Accent = RGBA(72, 120, 232);   t.AccentText = RGBA(240, 244, 255);
        t.Text = RGBA(216, 220, 230);    t.TextInactive = RGBA(112, 118, 132);
        t.TextShadow = RGBA(3, 5, 10, 190);
        t.Outline = RGBA(58, 104, 208);  t.Inline = RGBA(30, 34, 44);
        t.Content = RGBA(19, 21, 27);    t.Tab = RGBA(15, 17, 22);
        t.Section = RGBA(24, 27, 34);    t.Element = RGBA(13, 15, 20);
        t.ElementHover = RGBA(34, 39, 50); t.Border = RGBA(48, 54, 68);
        t.Risky = RGBA(204, 74, 78);     t.Positive = RGBA(104, 182, 122);
        t.Warning = RGBA(208, 170, 78);
        return t;
    }

    static Theme MakeCyan()
    {
        Theme t = MakeBase();
        t.Accent = RGBA(64, 178, 216);   t.AccentText = RGBA(10, 22, 28);
        t.Text = RGBA(212, 220, 226);    t.TextInactive = RGBA(104, 116, 124);
        t.TextShadow = RGBA(2, 6, 8, 190);
        t.Outline = RGBA(8, 12, 14);     t.Inline = RGBA(38, 48, 54);
        t.Content = RGBA(20, 24, 28);    t.Tab = RGBA(16, 20, 23);
        t.Section = RGBA(25, 30, 35);    t.Element = RGBA(14, 18, 21);
        t.ElementHover = RGBA(34, 42, 48); t.Border = RGBA(46, 56, 64);
        t.Risky = RGBA(200, 76, 76);     t.Positive = RGBA(100, 184, 124);
        t.Warning = RGBA(206, 170, 76);
        return t;
    }

    static Theme MakeOrchid()
    {
        Theme t = MakeBase();
        t.Accent = RGBA(233, 150, 199);  t.AccentText = RGBA(26, 14, 22);
        t.Text = RGBA(206, 210, 220);    t.TextInactive = RGBA(110, 116, 132);
        t.TextShadow = RGBA(3, 4, 8, 190);
        t.Outline = RGBA(6, 7, 11);      t.Inline = RGBA(34, 38, 50);
        t.Content = RGBA(16, 18, 26);    t.Tab = RGBA(13, 15, 22);
        t.Section = RGBA(16, 18, 26);    t.Element = RGBA(11, 13, 19);
        t.ElementHover = RGBA(28, 32, 44); t.Border = RGBA(38, 43, 56);
        t.Risky = RGBA(214, 92, 104);    t.Positive = RGBA(112, 194, 140);
        t.Warning = RGBA(212, 176, 92);
        t.RowHeight = 16.0f; t.ItemSpacing = 3.0f; t.InnerPadding = 9.0f; t.TabHeight = 22.0f;
        return t;
    }

    // Rounded, cyan-on-navy, with lighter header strips — the modern "store menu"
    // look rather than the flat CS:GO one. Radius is non-zero here, which is what
    // every rounded corner in the library keys off.
    static Theme MakeElectro()
    {
        Theme t = MakeBase();
        t.Accent = RGBA(34, 195, 230);   t.AccentText = RGBA(8, 18, 24);
        t.Text = RGBA(226, 233, 241);    t.TextInactive = RGBA(96, 112, 133);
        t.TextShadow = RGBA(0, 0, 0, 0);          // flat look — no text shadow
        t.Outline = RGBA(7, 12, 19);     t.Inline = RGBA(30, 41, 56);
        t.Content = RGBA(13, 20, 30);    t.Tab = RGBA(16, 24, 35);
        t.Section = RGBA(17, 25, 37);    t.Element = RGBA(25, 35, 49);
        t.ElementHover = RGBA(31, 43, 60); t.Border = RGBA(30, 41, 56);
        t.Risky = RGBA(238, 122, 106);   t.Positive = RGBA(92, 206, 160);
        t.Warning = RGBA(226, 180, 92);
        t.Radius = 4.0f;
        t.RowHeight = 20.0f; t.ItemSpacing = 6.0f; t.InnerPadding = 10.0f; t.TabHeight = 26.0f;
        t.TextShadows = false;
        return t;
    }

    // geekbarz.fun — the soft modern look: neutral greys sampled from the
    // reference (17 window, 21 panel, 25 header, 35 lines), right-hand checkboxes,
    // tall fields, and fills lit top-down with the violet ramp the project's
    // console banner uses: RGB(148,140,246) -> RGB(100,50,200).
    static Theme MakeGeek()
    {
        Theme t = MakeBase();
        t.Accent = RGBA(148, 140, 246);  t.Accent2 = RGBA(100, 50, 200);
        t.AccentGradient = true;         t.GradientVertical = true;
        t.AccentText = RGBA(250, 250, 255);
        t.Text = RGBA(239, 239, 239);    t.TextInactive = RGBA(161, 161, 161);
        t.TextShadow = RGBA(0, 0, 0, 0); t.TextShadows = false;
        t.Outline = RGBA(35, 35, 35);    t.Inline = RGBA(35, 35, 35);
        t.Content = RGBA(17, 17, 17);    t.Tab = RGBA(17, 17, 17);
        t.Section = RGBA(21, 21, 21);    t.Header = RGBA(25, 25, 25);
        t.Element = RGBA(17, 17, 17);    t.ElementHover = RGBA(29, 29, 29);
        t.Border = RGBA(35, 35, 35);
        t.Risky = RGBA(222, 96, 104);    t.Positive = RGBA(104, 196, 136);
        t.Warning = RGBA(222, 178, 92);
        t.Radius = 3.0f;
        // Compact: the reference's proportions at roughly 80%.
        t.RowHeight = 17.0f; t.ItemSpacing = 5.0f; t.InnerPadding = 8.0f; t.TabHeight = 28.0f;
        t.FieldHeight = 24.0f; t.SliderHeight = 8.0f; t.HeaderHeight = 24.0f; t.CheckboxSize = 12.0f;
        t.CheckboxRight = true; t.DimOffLabels = true;
        t.MutedFieldText = true; t.DropdownListIcon = true;
        return t;
    }

    // tensura.win — the modern dark "app" look: near-black chrome, soft rounded
    // cards, pill switches, and Rimuru's periwinkle blue as the accent ramp.
    // Greys are sampled from the reference: 13 window, 18 card, 34 field, 46 line.
    static Theme MakeTensura()
    {
        Theme t = MakeBase();
        t.Accent = RGBA(126, 170, 240);  t.Accent2 = RGBA(92, 132, 224);
        t.AccentGradient = true;         t.GradientVertical = true;
        t.AccentText = RGBA(10, 14, 22);
        t.Text = RGBA(212, 214, 221);    t.TextInactive = RGBA(128, 130, 140);
        t.TextShadow = RGBA(0, 0, 0, 0); t.TextShadows = false;
        t.Outline = RGBA(34, 36, 44);    t.Inline = RGBA(30, 32, 39);
        t.Content = RGBA(13, 14, 18);    t.Tab = RGBA(9, 11, 16);
        t.Section = RGBA(18, 19, 24);    t.Header = RGBA(21, 22, 27);
        t.Element = RGBA(34, 36, 44);    t.ElementHover = RGBA(44, 47, 56);
        t.Border = RGBA(40, 42, 50);
        t.Risky = RGBA(228, 100, 108);   t.Positive = RGBA(104, 200, 148);
        t.Warning = RGBA(226, 180, 96);
        t.Radius = 6.0f;
        t.RowHeight = 18.0f; t.ItemSpacing = 5.0f; t.InnerPadding = 10.0f; t.TabHeight = 26.0f;
        t.FieldHeight = 20.0f; t.SliderHeight = 4.0f; t.HeaderHeight = 24.0f; t.CheckboxSize = 13.0f;
        t.CheckboxRight = true; t.CheckboxSwitch = true; t.SliderKnobRound = true;
        t.InlineFields = true;  t.FieldFraction = 0.46f;
        t.RowDivider = 0.55f;
        return t;
    }

    // Mono — strict black and white, square. The accent *is* white, so primary
    // fills invert (white box, black text) instead of taking on a colour.
    static Theme MakeMono()
    {
        Theme t = MakeBase();
        t.Accent = RGBA(255, 255, 255);  t.Accent2 = RGBA(255, 255, 255);
        t.AccentText = RGBA(8, 8, 8);
        t.Text = RGBA(237, 237, 237);    t.TextInactive = RGBA(130, 130, 130);
        t.TextShadow = RGBA(0, 0, 0, 0); t.TextShadows = false;
        t.Outline = RGBA(40, 40, 40);    t.Inline = RGBA(28, 28, 28);
        t.Content = RGBA(9, 9, 9);       t.Tab = RGBA(9, 9, 9);
        t.Section = RGBA(15, 15, 15);    t.Header = RGBA(15, 15, 15);
        t.Element = RGBA(20, 20, 20);    t.ElementHover = RGBA(32, 32, 32);
        t.Border = RGBA(44, 44, 44);
        t.Risky = RGBA(232, 92, 92);     t.Positive = RGBA(230, 230, 230);
        t.Warning = RGBA(200, 200, 200);
        t.Radius = 0.0f;
        t.RowHeight = 20.0f; t.ItemSpacing = 6.0f; t.InnerPadding = 12.0f; t.TabHeight = 30.0f;
        t.FieldHeight = 30.0f; t.SliderHeight = 4.0f; t.HeaderHeight = 28.0f; t.CheckboxSize = 13.0f;
        t.CheckboxRight = true; t.CheckboxSwitch = true; t.SliderKnobRound = true;
        return t;
    }

    Theme& theme()
    {
        static Theme t = MakeNavy();
        return t;
    }

    void Apply(Preset p)
    {
        switch (p)
        {
        case Preset::Indigo: theme() = MakeIndigo(); break;
        case Preset::Carbon: theme() = MakeCarbon(); break;
        case Preset::Rose:   theme() = MakeRose();   break;
        case Preset::Azure:  theme() = MakeAzure();  break;
        case Preset::Cyan:   theme() = MakeCyan();   break;
        case Preset::Orchid: theme() = MakeOrchid(); break;
        case Preset::Electro: theme() = MakeElectro(); break;
        case Preset::Geek:    theme() = MakeGeek();    break;
        case Preset::Tensura: theme() = MakeTensura(); break;
        case Preset::Mono:    theme() = MakeMono();    break;
        default:             theme() = MakeNavy();   break;
        }
    }

    static ImVec4 V4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

    void ApplyToImGuiStyle()
    {
        const Theme& t = theme();
        ImGuiStyle& s = ImGui::GetStyle();

        s.WindowRounding = s.ChildRounding = s.FrameRounding = s.PopupRounding = t.Radius;
        s.ScrollbarRounding = s.GrabRounding = t.Radius;
        s.WindowBorderSize = s.ChildBorderSize = s.PopupBorderSize = s.FrameBorderSize = 1.0f;
        s.WindowPadding = ImVec2(t.InnerPadding, t.InnerPadding);
        s.ItemSpacing = ImVec2(t.ItemSpacing, t.ItemSpacing);
        s.FramePadding = ImVec2(4.0f, 3.0f);

        s.Colors[ImGuiCol_WindowBg] = V4(t.Content);
        s.Colors[ImGuiCol_ChildBg] = V4(t.Section);
        s.Colors[ImGuiCol_PopupBg] = V4(t.Element);
        s.Colors[ImGuiCol_Border] = V4(t.Border);
        s.Colors[ImGuiCol_FrameBg] = V4(t.Element);
        s.Colors[ImGuiCol_FrameBgHovered] = V4(t.ElementHover);
        s.Colors[ImGuiCol_FrameBgActive] = V4(t.ElementHover);
        s.Colors[ImGuiCol_TitleBg] = V4(t.Tab);
        s.Colors[ImGuiCol_TitleBgActive] = V4(t.Tab);
        s.Colors[ImGuiCol_Text] = V4(t.Text);
        s.Colors[ImGuiCol_TextDisabled] = V4(t.TextInactive);
        s.Colors[ImGuiCol_Button] = V4(t.Element);
        s.Colors[ImGuiCol_ButtonHovered] = V4(t.ElementHover);
        s.Colors[ImGuiCol_ButtonActive] = V4(t.Accent);
        s.Colors[ImGuiCol_CheckMark] = V4(t.Accent);
        s.Colors[ImGuiCol_SliderGrab] = V4(t.Accent);
        s.Colors[ImGuiCol_SliderGrabActive] = V4(Lighten(t.Accent, 0.15f));
        s.Colors[ImGuiCol_Header] = V4(t.ElementHover);
        s.Colors[ImGuiCol_HeaderHovered] = V4(t.ElementHover);
        s.Colors[ImGuiCol_HeaderActive] = V4(t.Accent);
        s.Colors[ImGuiCol_ScrollbarBg] = V4(t.Element);
        s.Colors[ImGuiCol_ScrollbarGrab] = V4(t.Border);
        s.Colors[ImGuiCol_ScrollbarGrabHovered] = V4(t.ElementHover);
        s.Colors[ImGuiCol_ScrollbarGrabActive] = V4(t.Accent);
        s.Colors[ImGuiCol_Separator] = V4(t.Border);
    }

    // ================================================================= draw
    namespace draw
    {
        // Everything routes through the page alpha so a transition fades custom
        // drawing along with the built-in controls.
        static ImU32 A(ImU32 c) { return WithAlpha(c, detail::g_state.alphaMul); }

        void Rect(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 col)
        {
            if (mx.x <= mn.x || mx.y <= mn.y) return;
            col = A(col);
            if ((col & IM_COL32_A_MASK) == 0) return;
            dl->AddRectFilled(mn, mx, col, theme().Radius);
        }

        void Border(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 col, float thickness)
        {
            if (mx.x <= mn.x || mx.y <= mn.y || thickness <= 0.0f) return;
            col = A(col);
            if ((col & IM_COL32_A_MASK) == 0) return;
            dl->AddRect(mn, mx, col, theme().Radius, 0, thickness);
        }

        void Box(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 fill, ImU32 border)
        {
            Rect(dl, mn, mx, fill);
            Border(dl, mn, mx, border);
        }

        void Line(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float thickness)
        {
            col = A(col);
            if ((col & IM_COL32_A_MASK) == 0) return;
            dl->AddLine(a, b, col, thickness);
        }

        void Text(ImDrawList* dl, ImVec2 pos, ImU32 col, const char* text, const char* end)
        {
            if (!text || !*text) return;
            const Theme& t = theme();
            ImU32 c = A(col);
            if ((c & IM_COL32_A_MASK) == 0) return;

            if (t.TextShadows && (t.TextShadow & IM_COL32_A_MASK) != 0)
            {
                // Scale the shadow by the text's own alpha so both fade together.
                float k = ((c >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
                dl->AddText(ImVec2(pos.x + 1.0f, pos.y + 1.0f), WithAlpha(A(t.TextShadow), k), text, end);
            }
            dl->AddText(pos, c, text, end);
        }

        void TextClipped(ImDrawList* dl, ImVec2 pos, float maxWidth, ImU32 col, const char* text)
        {
            char buf[256];
            Text(dl, pos, col, Ellipsize(text, maxWidth, buf, sizeof(buf)));
        }

        float TextWidth(const char* text)
        {
            if (!text || !*text) return 0.0f;
            return ImGui::CalcTextSize(text).x;
        }

        float LineHeight() { return ImGui::GetTextLineHeight(); }

        const char* Ellipsize(const char* text, float maxWidth, char* out, size_t outSize)
        {
            if (!text || !*text || outSize == 0) { if (outSize) out[0] = 0; return out; }
            if (TextWidth(text) <= maxWidth || maxWidth <= 0.0f)
            {
                snprintf(out, outSize, "%s", text);
                return out;
            }
            float dots = TextWidth("...");
            size_t len = strlen(text);
            for (size_t n = len; n > 0; n--)
            {
                if (n + 4 > outSize) continue;
                memcpy(out, text, n);
                out[n] = 0;
                if (TextWidth(out) + dots <= maxWidth)
                {
                    snprintf(out + n, outSize - n, "...");
                    return out;
                }
            }
            snprintf(out, outSize, "...");
            return out;
        }

        void Chevron(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 col, int dir, float thickness)
        {
            col = A(col);
            if ((col & IM_COL32_A_MASK) == 0) return;
            float cx = (mn.x + mx.x) * 0.5f, cy = (mn.y + mx.y) * 0.5f;
            float w = VMin(mx.x - mn.x, mx.y - mn.y) * 0.30f;
            ImVec2 a, b, c;
            switch (dir)
            {
            case 1: a = ImVec2(cx - w, cy + w * 0.6f); b = ImVec2(cx, cy - w * 0.6f); c = ImVec2(cx + w, cy + w * 0.6f); break;
            case 2: a = ImVec2(cx + w * 0.6f, cy - w); b = ImVec2(cx - w * 0.6f, cy); c = ImVec2(cx + w * 0.6f, cy + w); break;
            case 3: a = ImVec2(cx - w * 0.6f, cy - w); b = ImVec2(cx + w * 0.6f, cy); c = ImVec2(cx - w * 0.6f, cy + w); break;
            default: a = ImVec2(cx - w, cy - w * 0.6f); b = ImVec2(cx, cy + w * 0.6f); c = ImVec2(cx + w, cy - w * 0.6f); break;
            }
            dl->PathLineTo(a); dl->PathLineTo(b); dl->PathLineTo(c);
            dl->PathStroke(col, 0, thickness);
        }

        void Check(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 col, float thickness)
        {
            col = A(col);
            if ((col & IM_COL32_A_MASK) == 0) return;
            float x = mn.x, y = mn.y, w = mx.x - mn.x, h = mx.y - mn.y;
            dl->PathLineTo(ImVec2(x + w * 0.20f, y + h * 0.52f));
            dl->PathLineTo(ImVec2(x + w * 0.42f, y + h * 0.74f));
            dl->PathLineTo(ImVec2(x + w * 0.80f, y + h * 0.26f));
            dl->PathStroke(col, 0, thickness);
        }

        void RectGradient(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 a, ImU32 b, bool horizontal)
        {
            if (mx.x <= mn.x || mx.y <= mn.y) return;
            a = A(a); b = A(b);
            if (((a | b) & IM_COL32_A_MASK) == 0) return;

            float rounding = VMin(theme().Radius, VMin(mx.x - mn.x, mx.y - mn.y) * 0.5f);
            if (rounding <= 0.0f)
            {
                if (horizontal) dl->AddRectFilledMultiColor(mn, mx, a, b, b, a);
                else            dl->AddRectFilledMultiColor(mn, mx, a, a, b, b);
                return;
            }

            // ImDrawList has no rounded multi-colour rect. Draw the rounded shape in
            // opaque white, then recolour its vertices along the gradient axis —
            // keeping each vertex's own alpha, which is the anti-aliased fringe.
            // (ImGui's ShadeVertsLinearColorGradientKeepAlpha does the same, but it
            // lives in imgui_internal.h.)
            int v0 = dl->VtxBuffer.Size;
            dl->AddRectFilled(mn, mx, IM_COL32_WHITE, rounding);
            float span = horizontal ? (mx.x - mn.x) : (mx.y - mn.y);
            for (int i = v0; i < dl->VtxBuffer.Size; i++)
            {
                ImDrawVert& v = dl->VtxBuffer[i];
                float u = Clamp01(((horizontal ? v.pos.x - mn.x : v.pos.y - mn.y)) / span);
                ImU32 c = LerpCol(a, b, u);
                unsigned fringe = (v.col >> IM_COL32_A_SHIFT) & 0xFF;
                unsigned ca = (c >> IM_COL32_A_SHIFT) & 0xFF;
                v.col = (c & ~IM_COL32_A_MASK) | ((ImU32)(fringe * ca / 255) << IM_COL32_A_SHIFT);
            }
        }

        void AccentRect(ImDrawList* dl, ImVec2 mn, ImVec2 mx, float spanX0, float spanX1, int alpha)
        {
            if (mx.x <= mn.x || mx.y <= mn.y || alpha <= 0) return;
            const Theme& t = theme();
            if (!t.AccentGradient)
            {
                // Flat themes keep the rounded path; a multi-colour rect cannot round.
                Rect(dl, mn, mx, SetAlpha(t.Accent, alpha));
                return;
            }
            if (t.GradientVertical)
            {
                // Lit from above: the whole ramp inside the fill's own height.
                RectGradient(dl, mn, mx, SetAlpha(AccentAt(0.0f), alpha),
                             SetAlpha(AccentAt(1.0f), alpha), false);
                return;
            }
            float w = VMax(1.0f, spanX1 - spanX0);
            ImU32 a = SetAlpha(AccentAt((mn.x - spanX0) / w), alpha);
            ImU32 b = SetAlpha(AccentAt((mx.x - spanX0) / w), alpha);
            RectGradient(dl, mn, mx, a, b, true);
        }

        void TextGradient(ImDrawList* dl, ImVec2 pos, ImU32 a, ImU32 b, const char* text)
        {
            if (!text || !*text) return;
            const Theme& t = theme();
            int n = (int)strlen(text);
            float x = pos.x;
            for (int i = 0; i < n; i++)
            {
                char ch[2] = { text[i], 0 };
                ImU32 c = A(LerpCol(a, b, n <= 1 ? 0.0f : (float)i / (float)(n - 1)));
                if ((c & IM_COL32_A_MASK) != 0)
                {
                    if (t.TextShadows && (t.TextShadow & IM_COL32_A_MASK) != 0)
                    {
                        float k = ((c >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
                        dl->AddText(ImVec2(x + 1.0f, pos.y + 1.0f), WithAlpha(A(t.TextShadow), k), ch);
                    }
                    dl->AddText(ImVec2(x, pos.y), c, ch);
                }
                // Per-glyph advance. ImGui fonts carry no kerning, so stepping one
                // character at a time lands exactly where one AddText would.
                x += ImGui::CalcTextSize(ch).x;
            }
        }

        void HueBar(ImDrawList* dl, ImVec2 mn, ImVec2 mx, bool vertical)
        {
            // Six gradient bands beat a per-pixel loop and look identical.
            const int N = 6;
            static const ImU32 stops[N + 1] = {
                IM_COL32(255,0,0,255),   IM_COL32(255,255,0,255), IM_COL32(0,255,0,255),
                IM_COL32(0,255,255,255), IM_COL32(0,0,255,255),   IM_COL32(255,0,255,255),
                IM_COL32(255,0,0,255),
            };
            for (int i = 0; i < N; i++)
            {
                ImU32 a = A(stops[i]), b = A(stops[i + 1]);
                if (vertical)
                {
                    float y0 = mn.y + (mx.y - mn.y) * (i / (float)N);
                    float y1 = mn.y + (mx.y - mn.y) * ((i + 1) / (float)N);
                    dl->AddRectFilledMultiColor(ImVec2(mn.x, y0), ImVec2(mx.x, y1), a, a, b, b);
                }
                else
                {
                    float x0 = mn.x + (mx.x - mn.x) * (i / (float)N);
                    float x1 = mn.x + (mx.x - mn.x) * ((i + 1) / (float)N);
                    dl->AddRectFilledMultiColor(ImVec2(x0, mn.y), ImVec2(x1, mx.y), a, b, b, a);
                }
            }
        }

        void Checker(ImDrawList* dl, ImVec2 mn, ImVec2 mx, float cell)
        {
            ImU32 c1 = A(IM_COL32(108, 108, 112, 255));
            ImU32 c2 = A(IM_COL32(74, 74, 78, 255));
            dl->PushClipRect(mn, mx, true);
            int row = 0;
            for (float y = mn.y; y < mx.y; y += cell, row++)
            {
                int colI = 0;
                for (float x = mn.x; x < mx.x; x += cell, colI++)
                    dl->AddRectFilled(ImVec2(x, y), ImVec2(VMin(x + cell, mx.x), VMin(y + cell, mx.y)),
                                      ((row + colI) & 1) ? c2 : c1);
            }
            dl->PopClipRect();
        }
    } // namespace draw

    // ================================================================= glyphs
    float GlyphSizeFor(float lineHeight)
    {
        float s = lineHeight * 0.78f;
        return s < 8.0f ? 8.0f : s;
    }

    static ImFont* g_icons = nullptr;

    ImFont* LoadIcons()
    {
        if (g_icons) return g_icons;
        ImGuiIO& io = ImGui::GetIO();
        if (io.Fonts->Locked) return nullptr;

        // The first font added becomes ImGui's default. If the host hasn't added
        // one yet, give it ImGui's own first — otherwise every string in the app
        // would be looked up in an icon font that has no letters.
        if (io.Fonts->Fonts.Size == 0) io.Fonts->AddFontDefault();

        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;   // static array: the atlas must not free it
        cfg.RasterizerMultiply = 1.1f;      // a touch heavier; Lucide's 2/24 stroke is thin at 13px
        snprintf(cfg.Name, sizeof(cfg.Name), "Lucide (velo)");
        g_icons = io.Fonts->AddFontFromMemoryTTF((void*)lucide::kFont, (int)lucide::kFontSize, 16.0f, &cfg);
        return g_icons;
    }

    // UTF-8 for one BMP codepoint. All of Lucide's sit in the private-use area,
    // which is three bytes.
    static int Utf8(unsigned int cp, char out[5])
    {
        if (cp < 0x80)  { out[0] = (char)cp; out[1] = 0; return 1; }
        if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); out[2] = 0; return 2; }
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        out[3] = 0;
        return 3;
    }

    void DrawGlyph(ImDrawList* dl, Glyph g, ImVec2 mn, ImVec2 mx, ImU32 col, float thickness)
    {
        (void)thickness;
        if (g == Glyph::None || !dl) return;
        int idx = (int)g;
        if (idx <= 0 || idx >= (int)(sizeof(lucide::kCodepoints) / sizeof(lucide::kCodepoints[0]))) return;

        col = WithAlpha(col, detail::g_state.alphaMul);
        if ((col & IM_COL32_A_MASK) == 0) return;

        ImFont* font = g_icons ? g_icons : LoadIcons();
        if (!font) return;

        float size = VMin(mx.x - mn.x, mx.y - mn.y);
        if (size <= 2.0f) return;

        // Centre the glyph's actual ink box, not its line box: Lucide's vertical
        // metrics come from the icon font, not from the text font beside it.
        unsigned int cp = lucide::kCodepoints[idx];
        ImFontBaked* baked = font->GetFontBaked(size);
        const ImFontGlyph* gl = baked ? baked->FindGlyph((ImWchar)cp) : nullptr;
        if (!gl || !gl->Visible) return;

        ImVec2 c((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f);
        ImVec2 pos(floorf(c.x - (gl->X0 + gl->X1) * 0.5f + 0.5f),
                   floorf(c.y - (gl->Y0 + gl->Y1) * 0.5f + 0.5f));
        char utf8[5];
        int n = Utf8(cp, utf8);
        dl->AddText(font, size, pos, col, utf8, utf8 + n);
    }
} // namespace velo
