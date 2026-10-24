// velo.h — the Dear ImGui twin of VelocityUI.
//
// Every VelocityUI control has a same-named function here with the same defaults,
// the same theme slots and the same animation feel, so a WinForms menu built with
// VelocityUI ports to ImGui one call per control:
//
//     C#:   new VelocityCheckbox { Text = "enabled", Checked = cfg.Enabled }
//     C++:  velo::Checkbox("enabled", &cfg.enabled);
//
// Usage, once at startup:
//     velo::Apply(velo::Preset::Orchid);
// Then every frame, before building the menu:
//     velo::NewFrame();            // advances animation clocks
//     velo::ApplyToImGuiStyle();   // only needed if you also use stock ImGui widgets
//
// Requires Dear ImGui 1.89+ (developed against 1.92). Only imgui.h is needed —
// no imgui_internal.h, so this drops into any project unchanged.

#pragma once
#include "imgui.h"
#include <stddef.h>

namespace velo
{
    // ImMin/ImMax live in imgui_internal.h, and this header is meant to need
    // nothing but imgui.h — so the twin carries its own.
    inline float VMin(float a, float b) { return a < b ? a : b; }
    inline float VMax(float a, float b) { return a > b ? a : b; }
    inline int   VMin(int a, int b) { return a < b ? a : b; }
    inline int   VMax(int a, int b) { return a > b ? a : b; }

    // A plain rectangle. Deliberately not ImRect — that lives in imgui_internal.h,
    // and this header is meant to need nothing but imgui.h.
    struct Area
    {
        ImVec2 Min, Max;
        Area() : Min(0, 0), Max(0, 0) {}
        Area(ImVec2 mn, ImVec2 mx) : Min(mn), Max(mx) {}
        float Width()  const { return Max.x - Min.x; }
        float Height() const { return Max.y - Min.y; }
        bool  Contains(ImVec2 p) const
        { return p.x >= Min.x && p.x < Max.x && p.y >= Min.y && p.y < Max.y; }
    };

    // ================================================================= color
    ImU32 RGBA(int r, int g, int b, int a = 255);
    ImU32 WithAlpha(ImU32 c, float mul);            // scales existing alpha
    ImU32 SetAlpha(ImU32 c, int a);                 // replaces alpha, 0..255
    ImU32 LerpCol(ImU32 a, ImU32 b, float t);
    ImU32 Lighten(ImU32 c, float amount);
    ImU32 Darken(ImU32 c, float amount);
    ImU32 FromHsv(float h, float s, float v, int a = 255);
    void  ToHsv(ImU32 c, float& h, float& s, float& v);
    ImU32 Rainbow(double seconds, float speed = 0.35f, float sat = 0.65f, float val = 1.0f);
    ImU32 ReadableOn(ImU32 background);
    // A point on the accent ramp: Accent at 0, Accent2 at 1. Returns Accent flat
    // whenever AccentGradient is off, so callers never need to branch.
    ImU32 AccentAt(float u);
    float Luminance(ImU32 c);
    void  ToHex(ImU32 c, char* out, size_t outSize);  // "#RRGGBB" / "#AARRGGBB"

    // ================================================================= theme
    // Mirrors VelocityTheme's presets value-for-value.
    enum class Preset { Navy, Indigo, Carbon, Rose, Azure, Cyan, Orchid, Electro, Geek, Tensura, Mono };

    // Mirrors VelocityTheme 1:1. Colors are packed ImU32 (what ImDrawList wants);
    // metrics are in pixels.
    struct Theme
    {
        ImU32 Accent, Accent2, AccentText,
              Text, TextInactive, TextShadow,
              Outline, Inline, Content, Tab, Section, Element, ElementHover, Border,
              Risky, Positive, Warning;
        ImU32 Header;            // section header strip; 0 = Element

        float Radius;            // 0 = square, the CS:GO default
        float BorderThickness;
        float RowHeight;
        float ItemSpacing;
        float InnerPadding;
        float TabHeight;
        float Density;

        // ---- widget shape. Every field defaults to 0 / false, which keeps the
        // original CS:GO look; presets opt in to the rest.
        float FieldHeight;       // dropdown, text box and default button height; 0 = line + 8
        float SliderHeight;      // slider track height; 0 = half a line
        float HeaderHeight;      // section header height; 0 = derived from the line
        float CheckboxSize;      // checkbox edge; 0 = line - 2
        bool  GradientVertical;  // accent ramp runs top->bottom inside each fill — a lit,
                                 // glossy bar — instead of left->right across the control.
                                 // Swatches pick up the same shading from their own colour.
        bool  CheckboxRight;     // box on the right edge, label on the left
        bool  DimOffLabels;      // unchecked checkboxes draw their label as TextInactive
        bool  MutedFieldText;    // dropdown values and button labels in TextInactive
        bool  DropdownListIcon;  // a list glyph instead of the chevron
        bool  CheckboxSwitch;    // draw the checkbox as a sliding pill switch
        bool  SliderKnobRound;   // circular knob riding the fill, not a raised block
        float RowDivider;        // alpha 0..1 of a hairline under each row; 0 = none
        // Dropdowns put their caption to the LEFT on the same row instead of above
        // it, with the box taking FieldFraction of the row width. SliderValue
        // ::BoxRight lays sliders out the same way regardless of this flag.
        bool  InlineFields;
        float FieldFraction;     // 0 = 0.45

        bool  TextShadows;       // the 1px shadow under every string
        // Ramp accent fills from Accent to Accent2 instead of painting them flat.
        // Off by default, and Accent2 == 0 falls back to Accent, so a theme that
        // never sets either is unaffected.
        bool  AccentGradient;
        bool  Animations;        // global motion kill switch
        float AnimationSpeed;    // global rate multiplier

        float Scale(float px) const { return px * Density; }
    };

    Theme& theme();                       // the live theme; mutate freely
    void   Apply(Preset p);
    void   ApplyToImGuiStyle();           // pushes the theme into ImGuiStyle

    // Call once per frame before building the menu. Advances the shared clock the
    // animations run off; without it nothing moves.
    void NewFrame();
    double Time();                        // seconds since the first NewFrame

    // ================================================================= easing
    // Mirrors Easing.cs. Continuous state uses Approach (frame-rate independent
    // exponential approach); one-shot state uses a 0..1 progress through EaseF.
    enum class Ease { Linear, OutCubic, OutQuint, InOutCubic, OutBack, OutExpo };

    float Clamp01(float v);
    float ClampF(float v, float lo, float hi);
    float Approach(float current, float target, float k, float dt);
    float EaseF(Ease kind, float t);

    // Per-widget animation state, stored in the current window's ImGuiStorage.
    // This is the ImGui equivalent of a control's private float field.
    float Anim(ImGuiID id, float target, float speed = 16.0f);

    // ================================================================= glyphs
    // The same 48 icons as Glyphs.cs, drawn from Lucide (lucide.dev, ISC). The
    // icon font is subset to exactly these glyphs and compiled into velo as a
    // byte array (velo_lucide.h), so there is still nothing to ship beside it.
    enum class Glyph
    {
        None = 0,
        Crosshair, Target, Skull, Shield, Sword, Bone,
        Eye, EyeOff, Monitor, Layers, Cube, Map,
        Gear, Sliders, Wrench, Terminal, Code, Bug,
        User, Users, Person,
        Keyboard, Mouse, Power, Zap,
        Folder, Save, Trash, Search, Filter,
        Bell, Info, Warning, Check, Cross, Plus, Minus,
        Home, Globe, Lock, Star, Heart, Palette, Flask, Chart, Link, Clock,
        List,
    };

    // Centres the icon's ink in mn..mx at the box's size. `thickness` is kept for
    // source compatibility and ignored: Lucide's stroke weight is part of the font.
    void  DrawGlyph(ImDrawList* dl, Glyph g, ImVec2 mn, ImVec2 mx, ImU32 col, float thickness = 1.35f);
    float GlyphSizeFor(float lineHeight);

    // Adds the embedded Lucide font to the atlas. Call once after creating the
    // ImGui context and adding your own fonts (the first font added becomes
    // ImGui's default, and it must not be this one). velo::NewFrame() also calls
    // it lazily if you don't, whenever the atlas isn't locked. Safe to call twice.
    ImFont* LoadIcons();

    // ================================================================= draw
    // Thin wrappers over ImDrawList that respect the current page transition's
    // alpha. Use these rather than raw AddRectFilled inside a page, or your
    // custom drawing won't fade with everything else.
    namespace draw
    {
        void Rect(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 col);
        void Border(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 col, float thickness = 1.0f);
        void Box(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 fill, ImU32 border);
        void Line(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float thickness = 1.0f);
        void Text(ImDrawList* dl, ImVec2 pos, ImU32 col, const char* text, const char* end = nullptr);
        void TextClipped(ImDrawList* dl, ImVec2 pos, float maxWidth, ImU32 col, const char* text);
        void Chevron(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 col, int dir = 0, float thickness = 1.4f);
        void Check(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 col, float thickness = 1.6f);
        void HueBar(ImDrawList* dl, ImVec2 mn, ImVec2 mx, bool vertical = true);

        // Two-stop gradient fill. Square only — ImDrawList has no rounded
        // multi-colour rect — which is what the flat CS:GO look wants anyway.
        void RectGradient(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 a, ImU32 b,
                          bool horizontal = true);

        // Fill mn..mx from the accent ramp, measured across [spanX0, spanX1]. A
        // half-filled slider therefore shows the left half of the ramp rather than
        // squeezing the whole thing into the filled part.
        void AccentRect(ImDrawList* dl, ImVec2 mn, ImVec2 mx,
                        float spanX0, float spanX1, int alpha = 255);

        // Per-character colour ramp — the same trick a console logger uses to
        // gradient a banner. Honours the theme's text shadow.
        void TextGradient(ImDrawList* dl, ImVec2 pos, ImU32 a, ImU32 b, const char* text);
        void Checker(ImDrawList* dl, ImVec2 mn, ImVec2 mx, float cell = 4.0f);

        float TextWidth(const char* text);
        float LineHeight();
        // Truncate with "..." to fit a pixel width. Writes into `out`.
        const char* Ellipsize(const char* text, float maxWidth, char* out, size_t outSize);
    }

    // ================================================================= enums
    // These mirror the C# enums value-for-value.
    enum class TabTreatment  { Text, Underline, Boxed, Segment, TopBar };
    enum class TabAlign      { Left, Center, Right, Fill };
    enum class SectionHeader { Fieldset, Underline, Centered, TopAccent, Strip, None };
    enum class CheckGlyph    { Check, Fill, Dot, Radio };
    enum class ButtonVariant { Default, Primary, Outline, Flat, Danger };
    enum class SliderValue   { CaptionRight, Overlay, TrackRight, None, BoxRight };
    enum class TextBoxVariant{ Boxed, Underline, Filled };
    enum class SwatchShape   { Square, Bar, Dot };
    enum class KeybindMode   { Hold, Toggle, Always };
    enum class TextAlign     { Left, Center, Right };
    enum class ScreenAnchor
    {
        TopLeft, TopCenter, TopRight,
        MiddleLeft, MiddleCenter, MiddleRight,
        BottomLeft, BottomCenter, BottomRight,
    };
    enum class NotifyKind     { Info, Success, Warning, Error };
    enum class PageTransition { None, Fade, Slide, FadeSlide };
    enum class SlideOrigin    { Auto, Left, Right, Top, Bottom };

    // ================================================================= chrome
    // Paints the CS:GO double frame over the current window. Call right after
    // ImGui::Begin(). Mirrors VelocityForm's Outline/Inline frame.
    void WindowFrame(bool doubleFrame = true, float thickness = 1.0f);

    // Soft drop shadow behind the current window, drawn on the background draw list.
    // GPU-cheap approximation: concentric rounded rects at falling alpha. Worth
    // having on a transparent overlay, where the window edge would otherwise sit
    // hard against the desktop.
    void WindowShadow(float size = 18.0f, float opacity = 0.55f);

    // Title bar with two-tone brand on the left and muted subtitle on the right.
    // Reserves the row and returns its rect so tabs can be laid into it.
    // `align` places the brand; Center puts it mid-bar, the way minimal menus do.
    Area TitleBar(const char* brand, const char* brand2 = nullptr,
                  const char* subtitle = nullptr, float height = 0.0f,
                  TextAlign align = TextAlign::Left);

    // ================================================================= tabs
    struct TabStripOpts
    {
        TabTreatment Treatment = TabTreatment::Underline;
        TabAlign     Alignment = TabAlign::Fill;
        const char*  Separator = "";        // e.g. "/" or "|", Text treatment only
        float ItemPadding = 12.0f;
        float IconSpacing = 6.0f;
        float IndicatorThickness = 2.0f;    // 0 = colour alone marks the active tab
        float Width  = 0.0f;                // 0 = fill available
        float Height = 0.0f;                // 0 = theme TabHeight
        bool  ShowIcons = true;
        bool  UpperCase = false;
        // Draw the icon only and drop the label. Tabs shrink to the icon, which is
        // what a compact icon-rail title bar wants.
        bool  IconOnly = false;
        // Colour the active label with the accent (the ramp, if the theme has one)
        // instead of the text colour. Pairs with IndicatorThickness = 0.
        bool  AccentActive = false;
    };

    // Returns true on the frame the selection changes.
    bool TabStrip(const char* id, const char* const labels[], int count, int* selected,
                  const TabStripOpts& opts = TabStripOpts(), const Glyph icons[] = nullptr);

    struct SidebarOpts
    {
        float ItemHeight = 30.0f;
        float Indent = 12.0f;
        float MarkerWidth = 2.0f;
        float Width = 140.0f;
        bool  ShowIcons = true;
        bool  UpperCase = false;
        const char* Title = nullptr;
    };

    bool Sidebar(const char* id, const char* const labels[], int count, int* selected,
                 const SidebarOpts& opts = SidebarOpts(), const Glyph icons[] = nullptr,
                 const char* const descriptions[] = nullptr);

    // ================================================================= pages
    struct PageOpts
    {
        PageTransition Transition = PageTransition::FadeSlide;
        SlideOrigin    SlideFrom  = SlideOrigin::Auto;
        float SlideDistance = 16.0f;
        float DurationMs    = 200.0f;
        float FadeOutMs     = 110.0f;
        float StaggerMs     = 26.0f;   // 0 = the page moves as one block
        Ease  Curve         = Ease::OutQuint;
    };

    // A page bound to one tab index. Returns false when this page isn't showing
    // (and has finished fading out), in which case skip its contents entirely.
    //
    // Unlike the WinForms version there are no retained child controls to snapshot,
    // so the transition is applied per widget: every velo:: control inside a page
    // picks up its own alpha and slide offset automatically, staggered in call
    // order. That reproduces the same look without a bitmap.
    bool BeginPage(const char* id, int index, int selected, const PageOpts& opts = PageOpts());
    void EndPage();

    // The current page's fade, 0..1, for whatever is drawn next — what the next
    // control would get, without using up its stagger slot. 1 outside a page.
    // Custom drawing that isn't a velo control (a label you place by hand) should
    // multiply its colour by this, or it pops in while the rest fades.
    float PageFade();

    // ================================================================= sections
    // size.y > 0 fixes the height, 0 fills what is left, and AutoHeight (any
    // negative value) fits the section to its contents so sections can stack down
    // a column. Text after "##" in the title is an ID suffix and is not drawn, so
    // two sections can both read "General".
    constexpr float AutoHeight = -1.0f;
    bool BeginSection(const char* title, ImVec2 size,
                      SectionHeader header = SectionHeader::Fieldset,
                      Glyph icon = Glyph::None);
    void EndSection();

    // ================================================================= controls
    bool Checkbox(const char* label, bool* value,
                  CheckGlyph glyph = CheckGlyph::Fill,
                  const char* accessory = nullptr,
                  ImU32* swatch = nullptr);          // non-null adds a trailing swatch

    bool SliderFloat(const char* caption, float* value, float mn, float mx,
                     const char* suffix = "", int decimals = 0,
                     SliderValue placement = SliderValue::CaptionRight,
                     bool showKnob = false, float step = 0.0f);

    bool SliderInt(const char* caption, int* value, int mn, int mx,
                   const char* suffix = "",
                   SliderValue placement = SliderValue::CaptionRight,
                   bool showKnob = false);

    bool Dropdown(const char* caption, int* current, const char* const items[], int count,
                  const char* placeholder = "select...");

    bool MultiDropdown(const char* caption, bool checked[], const char* const items[], int count,
                       bool countBadge = true, const char* placeholder = "none");

    // *bind: >0 = ImGuiKey, <0 = mouse (-1 LMB .. -5 MB5), 0 = unbound.
    bool Keybind(const char* caption, int* bind, KeybindMode* mode = nullptr);
    const char* KeybindName(int bind);
    // True while a bind is active, honouring its Hold/Toggle/Always mode.
    bool KeybindActive(int bind, KeybindMode mode, bool* toggleState);

    bool ColorPicker(const char* caption, ImU32* col,
                     SwatchShape shape = SwatchShape::Square,
                     bool showHex = false, bool allowAlpha = true);

    bool TextBox(const char* caption, char* buf, size_t bufSize,
                 const char* placeholder = "",
                 TextBoxVariant variant = TextBoxVariant::Boxed,
                 Glyph icon = Glyph::None);

    bool Button(const char* label, ButtonVariant variant = ButtonVariant::Default,
                ImVec2 size = ImVec2(0, 0), Glyph icon = Glyph::None,
                TextAlign align = TextAlign::Center);

    void Separator(const char* caption = nullptr);
    // A hairline across the current content width, with no vertical space of its
    // own beyond `gap` above and below. Card rows use this, not Separator.
    void RowDivider(float gap = 0.0f);
    void Label(const char* text, const char* text2 = nullptr);
    void RainbowLabel(const char* text, float speed = 1.0f, bool wave = false, float amplitude = 3.0f);

    // ================================================================= lists
    // Scrolling list. Pass `checked` to get a tick box per row (multi-select);
    // leave it null for single-select. Returns true when the selection changes.
    bool ListBox(const char* id, const char* const items[], int count, int* selected,
                 int visibleRows = 6, bool checked[] = nullptr);

    // A player row list: name plus a status word, coloured by status index into
    // your own palette. Returns true when the selection changes.
    struct PlayerRow { const char* Name; const char* Status; ImU32 StatusColor; };
    bool PlayerList(const char* id, const PlayerRow rows[], int count, int* selected,
                    int visibleRows = 8);

    // ================================================================= config bar
    enum class ConfigAction { None, Load, Save, Create, Delete };

    // Config selector with load / save / create / delete word buttons underneath —
    // the strip every menu has. Returns which action fired this frame.
    ConfigAction ConfigBar(const char* id, const char* const configs[], int count, int* selected);

    // ================================================================= setting row
    // A titled row with a stack of trailing accessories, the densest way to present
    // a feature that has more than one knob.
    enum class RowKind { Checkbox, Toggle, Swatch, Badge, Gear, Keybind, Expander };

    struct RowItem
    {
        RowKind Kind = RowKind::Checkbox;
        bool*   Value = nullptr;        // Checkbox / Toggle / Expander
        ImU32*  Color = nullptr;        // Swatch
        const char* Text = nullptr;     // Badge
        int*    Bind = nullptr;         // Keybind chip
        bool    Clicked = false;        // written back: true the frame it was hit
    };

    // Returns true if any accessory changed. Inspect RowItem::Clicked for which.
    bool SettingRow(const char* title, const char* description, RowItem items[], int count);

    // ================================================================= module card
    // Header row acts as the master toggle; the body dims when it is off.
    bool BeginModuleCard(const char* title, bool* enabled, ImVec2 size, int* bind = nullptr);
    void EndModuleCard();

    // ================================================================= tooltip
    // Themed tooltip for the item submitted immediately before it. Fades in after a
    // short dwell rather than snapping, like the rest of the library.
    void Tooltip(const char* text, float delaySec = 0.35f);

    // ================================================================= specialty
    // The domain widgets: previews and pickers that only make sense in this kind of
    // menu, and that are tedious to rebuild every time.

    // ESP box preview — shows what the current visuals settings look like on a
    // target, so the user can see a colour change without alt-tabbing into a game.
    struct EspPreviewStyle
    {
        ImU32 BoxColor = IM_COL32(233, 150, 199, 255);
        ImU32 NameColor = IM_COL32(255, 255, 255, 255);
        ImU32 HealthColor = IM_COL32(112, 194, 140, 255);
        ImU32 SkeletonColor = IM_COL32(150, 170, 220, 255);
        int   BoxStyle = 0;             // 0 full, 1 corner, 2 edges
        bool  Box = true, Name = true, Health = true, Weapon = false,
              Skeleton = false, Distance = false;
        float HealthFraction = 0.72f;
        const char* NameText = "player";
        const char* WeaponText = "ak47";
        const char* DistanceText = "12m";

        // Optional player render. Supply a texture and its pixel size and it is
        // letterboxed into the widget and used as the subject; leave Texture at 0
        // and a plain silhouette is drawn instead, so the preview still works with
        // no art shipped.
        ImTextureID Texture = 0;
        ImVec2 TextureSize = ImVec2(0, 0);

        // How far the ESP box insets from the image rect, as a fraction of it.
        // A full-body render usually has empty margins either side; these trim to
        // roughly the figure.
        float BoxPadX = 0.20f;
        float BoxPadY = 0.01f;
    };
    void EspPreview(const char* id, ImVec2 size, const EspPreviewStyle& style);

    // Humanoid hitbox picker. `mask` is a bitfield of HitBone_ values.
    enum HitBone_
    {
        HitBone_None = 0,
        HitBone_Head = 1 << 0,
        HitBone_Neck = 1 << 1,
        HitBone_Chest = 1 << 2,
        HitBone_Stomach = 1 << 3,
        HitBone_Pelvis = 1 << 4,
        HitBone_Arms = 1 << 5,
        HitBone_Legs = 1 << 6,
        HitBone_Feet = 1 << 7,
    };
    bool BonePicker(const char* id, unsigned int* mask, ImVec2 size = ImVec2(0, 0));

    // Progression graph with a grid. Drag a point to reshape it when editable.
    bool CurveView(const char* id, float points[], int count, ImVec2 size, bool editable = true);

    // Auto-oscillating preview bar — shows what a varying value looks like in
    // motion. Returns the current 0..1 value so a label can track it.
    enum class BarMotion { Sine, Triangle, Sawtooth, Pulse };
    float VaryingBar(const char* caption, BarMotion motion = BarMotion::Sine,
                     const char* suffix = "%", float speed = 1.0f);

    // ================================================================= HUD
    // These draw on the foreground draw list, which is the ImGui equivalent of the
    // WinForms detached overlay windows: they sit over everything and are not
    // clipped by any window.
    struct WatermarkSegment { const char* Text; bool Accent; bool Boxed; };

    void Watermark(const WatermarkSegment segments[], int count,
                   ScreenAnchor anchor = ScreenAnchor::TopCenter,
                   ImVec2 offset = ImVec2(10, 10), const char* separator = " | ");

    // Active is the bind's live state — feed it from KeybindActive(). The panel is
    // dynamic, matching how these read in a real menu:
    //   Hold   — always listed; highlighted while the key is down
    //   Toggle — listed only while switched on, so it disappears when toggled off
    //   Always — always listed and highlighted
    struct KeybindEntry { const char* Label; int Bind; KeybindMode Mode; bool Active; };

    void KeybindListPanel(const char* title, const KeybindEntry entries[], int count,
                          ScreenAnchor anchor = ScreenAnchor::MiddleLeft,
                          ImVec2 offset = ImVec2(10, 10), bool showMode = true);

    // Queue a toast. Safe to call from anywhere during the frame.
    void Notify(const char* title, const char* body, NotifyKind kind = NotifyKind::Info);
    // Draw and age the queued toasts. Call once per frame.
    void RenderNotifications(ScreenAnchor anchor = ScreenAnchor::TopRight,
                             ImVec2 offset = ImVec2(10, 10), float durationSec = 3.2f);
}
