// velo_controls.cpp — chrome, tabs, pages, sections, controls and the HUD.
// Mirrors the VelocityUI control set one call per control.

#include "velo.h"
#include "velo_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace velo
{
    using namespace detail;

    // ================================================================= helpers
    namespace detail
    {
        ImU32 A(ImU32 c) { return WithAlpha(c, g_state.alphaMul); }
        float RowH() { return theme().Scale(theme().RowHeight); }

        static ImVector<PageRec> g_pages;

        PageRec* GetPage(ImGuiID id)
        {
            for (int i = 0; i < g_pages.Size; i++)
                if (g_pages[i].id == id) return &g_pages[i];
            PageRec r;
            r.id = id;
            g_pages.push_back(r);
            return &g_pages.back();
        }

        ItemAnim::ItemAnim()
        {
            savedAlpha = g_state.alphaMul;
            PageCtx* p = g_state.page;
            if (!p) return;

            // A vertical slide is applied by moving the layout cursor down before
            // the item, which leaves the item's offset in the cursor afterwards.
            // Left alone, every item would start below the previous one's offset
            // and they would stack down the column (an auto-height section then
            // swells mid-transition). So each item first takes the previous
            // item's offset back out, folded into its own move. It can't be undone
            // *after* the item instead: a SetCursorPos with no item following it
            // is an ImGui error at the end of the window.
            float dy = 0.0f;
            const ImDrawList* win = ImGui::GetWindowDrawList();

            if (p->outPhase)
            {
                g_state.alphaMul = savedAlpha * p->outAlpha;
            }
            else
            {
                float delay = p->item * (p->opts.StaggerMs / 1000.0f);
                float local = p->localTime - delay;
                float dur = VMax(0.001f, p->opts.DurationMs / 1000.0f);
                float t = local <= 0.0f ? 0.0f : Clamp01(local / dur);
                float e = EaseF(p->opts.Curve, t);

                if (p->opts.Transition != PageTransition::Slide)
                    g_state.alphaMul = savedAlpha * e;

                if (p->opts.Transition != PageTransition::Fade && p->opts.SlideDistance > 0.0f)
                {
                    float d = p->opts.SlideDistance * (1.0f - e);
                    float dx = 0.0f;
                    switch (p->opts.SlideFrom)
                    {
                    case SlideOrigin::Left:   dx = -d; break;
                    case SlideOrigin::Right:  dx = d; break;
                    case SlideOrigin::Top:    dy = -d; break;
                    case SlideOrigin::Bottom: dy = d; break;
                    default:                  dx = (p->dir >= 0) ? d : -d; break;
                    }
                    if (dx != 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + dx);
                }
            }

            float undo = (g_state.slideWindow == win) ? g_state.slidePending : 0.0f;
            if (dy - undo != 0.0f) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + dy - undo);
            g_state.slidePending = dy;
            g_state.slideWindow = win;

            p->item++;

            // Stock ImGui widgets (InputText) honour this; our own drawing uses alphaMul.
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * (g_state.alphaMul / VMax(0.0001f, savedAlpha)));
            pushedStyle = true;
        }

        ItemAnim::~ItemAnim()
        {
            if (pushedStyle) ImGui::PopStyleVar();
            g_state.alphaMul = savedAlpha;
        }

        ImVec2 Anchor(ScreenAnchor a, ImVec2 size, ImVec2 offset, ImVec2 mn, ImVec2 mx)
        {
            float x, y;
            switch (a)
            {
            case ScreenAnchor::TopCenter: case ScreenAnchor::MiddleCenter: case ScreenAnchor::BottomCenter:
                x = mn.x + ((mx.x - mn.x) - size.x) * 0.5f; break;
            case ScreenAnchor::TopRight: case ScreenAnchor::MiddleRight: case ScreenAnchor::BottomRight:
                x = mx.x - size.x - offset.x; break;
            default: x = mn.x + offset.x; break;
            }
            switch (a)
            {
            case ScreenAnchor::MiddleLeft: case ScreenAnchor::MiddleCenter: case ScreenAnchor::MiddleRight:
                y = mn.y + ((mx.y - mn.y) - size.y) * 0.5f; break;
            case ScreenAnchor::BottomLeft: case ScreenAnchor::BottomCenter: case ScreenAnchor::BottomRight:
                y = mx.y - size.y - offset.y; break;
            default: y = mn.y + offset.y; break;
            }
            return ImVec2(x, y);
        }
    }

    // Reserve a rect on the current line and return it in screen space.
    static Area Reserve(float w, float h, const char* id, bool* hovered = nullptr, bool* held = nullptr, bool* clicked = nullptr)
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        if (w <= 0.0f) w = ImGui::GetContentRegionAvail().x;
        ImGui::InvisibleButton(id, ImVec2(VMax(1.0f, w), VMax(1.0f, h)));
        if (hovered) *hovered = ImGui::IsItemHovered();
        if (held)    *held = ImGui::IsItemActive();
        if (clicked) *clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        return Area(p, ImVec2(p.x + w, p.y + h));
    }

    // ================================================================= chrome
    void WindowFrame(bool doubleFrame, float thickness)
    {
        const Theme& t = theme();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 mn = ImGui::GetWindowPos();
        ImVec2 mx = ImVec2(mn.x + ImGui::GetWindowSize().x, mn.y + ImGui::GetWindowSize().y);
        draw::Border(dl, mn, mx, t.Outline, thickness);
        if (doubleFrame)
            draw::Border(dl, ImVec2(mn.x + thickness, mn.y + thickness),
                         ImVec2(mx.x - thickness, mx.y - thickness), t.Inline, 1.0f);
    }

    void WindowShadow(float size, float opacity)
    {
        if (size <= 0.0f || opacity <= 0.0f) return;
        const Theme& t = theme();
        // Background draw list so the shadow lands under the window itself rather
        // than over its own fill. Concentric rounded rects at falling alpha are a
        // decent gaussian stand-in and cost a handful of quads.
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        ImVec2 mn = ImGui::GetWindowPos();
        ImVec2 sz = ImGui::GetWindowSize();
        ImVec2 mx(mn.x + sz.x, mn.y + sz.y);

        const int steps = 14;
        for (int i = steps; i >= 1; i--)
        {
            float f = (float)i / (float)steps;      // 1 = outermost
            float grow = size * f;
            // Quadratic falloff: dense near the edge, nearly gone at the rim.
            float a = opacity * (1.0f - f) * (1.0f - f) * 0.5f;
            int   ai = (int)(a * 255.0f);
            if (ai <= 0) continue;
            dl->AddRectFilled(ImVec2(mn.x - grow, mn.y - grow + size * 0.25f),
                              ImVec2(mx.x + grow, mx.y + grow + size * 0.25f),
                              IM_COL32(0, 0, 0, ai), t.Radius + grow);
        }
    }

    Area TitleBar(const char* brand, const char* brand2, const char* subtitle, float height,
                  TextAlign align)
    {
        const Theme& t = theme();
        if (height <= 0.0f) height = t.Scale(28.0f);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wpos = ImGui::GetWindowPos();
        float w = ImGui::GetWindowSize().x;
        ImVec2 mn(wpos.x + 1.0f, wpos.y + 1.0f);
        ImVec2 mx(wpos.x + w - 1.0f, wpos.y + height);

        // Drag handle covering the whole bar, submitted *first* and flagged
        // allow-overlap so anything placed on top of it later (a tab strip, usually)
        // still gets the hover. Without this the tabs' own hit area covers the bar
        // and the window can't be dragged at all.
        ImGui::SetCursorScreenPos(mn);
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##velo_titlebar", ImVec2(mx.x - mn.x, mx.y - mn.y));
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            ImVec2 p = ImGui::GetWindowPos();
            ImVec2 d = ImGui::GetIO().MouseDelta;
            ImGui::SetWindowPos(ImVec2(p.x + d.x, p.y + d.y));
        }

        // Round the top corners to match the window, or a rounded window shows
        // square title-bar corners poking out past its edge.
        float wr = VMax(0.0f, ImGui::GetStyle().WindowRounding - 1.0f);
        dl->AddRectFilled(mn, mx, t.Tab, wr, ImDrawFlags_RoundCornersTop);
        dl->AddLine(ImVec2(mn.x, mx.y - 1.0f), ImVec2(mx.x, mx.y - 1.0f), t.Border, 1.0f);

        float lh = draw::LineHeight();
        float ty = mn.y + ((mx.y - mn.y) - lh) * 0.5f;
        float tx = mn.x + 10.0f;
        if (align != TextAlign::Left)
        {
            float bw = draw::TextWidth(brand) + draw::TextWidth(brand2);
            tx = (align == TextAlign::Center) ? mn.x + ((mx.x - mn.x) - bw) * 0.5f
                                              : mx.x - bw - 10.0f;
        }

        if (brand && *brand)
        {
            draw::Text(dl, ImVec2(tx, ty), t.Text, brand);
            tx += draw::TextWidth(brand);
        }
        if (brand2 && *brand2)
        {
            if (t.AccentGradient)
                draw::TextGradient(dl, ImVec2(tx, ty), t.Accent,
                                   t.Accent2 ? t.Accent2 : t.Accent, brand2);
            else
                draw::Text(dl, ImVec2(tx, ty), t.Accent, brand2);
        }

        if (subtitle && *subtitle)
        {
            float sw = draw::TextWidth(subtitle);
            draw::Text(dl, ImVec2(mx.x - sw - 10.0f, ty), t.TextInactive, subtitle);
        }

        // The InvisibleButton above already reserved the row, so content continues
        // underneath it.
        return Area(mn, mx);
    }

    // ================================================================= tabs
    bool TabStrip(const char* id, const char* const labels[], int count, int* selected,
                  const TabStripOpts& o, const Glyph icons[])
    {
        if (count <= 0 || !selected) return false;
        const Theme& t = theme();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        float h = o.Height > 0.0f ? o.Height : t.Scale(t.TabHeight);
        float availW = o.Width > 0.0f ? o.Width : ImGui::GetContentRegionAvail().x;

        ImGui::PushID(id);
        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##strip", ImVec2(VMax(1.0f, availW), h));
        bool stripHovered = ImGui::IsItemHovered();
        bool stripClicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        ImVec2 mouse = ImGui::GetIO().MousePos;

        float lh = draw::LineHeight();
        float iconSz = GlyphSizeFor(lh);
        float sepW = (o.Separator && *o.Separator) ? draw::TextWidth(o.Separator) + 6.0f : 0.0f;

        // ---- measure
        static ImVector<float> widths;
        widths.resize(count);
        float total = 0.0f;
        for (int i = 0; i < count; i++)
        {
            bool ico = o.ShowIcons && icons && icons[i] != Glyph::None;
            // IconOnly drops the label entirely and shrinks the tab to the glyph, which
            // is what a compact icon rail in a title bar wants. A tab with no icon still
            // falls back to its label so the strip never collapses to nothing.
            float w = (o.IconOnly && ico) ? iconSz + o.ItemPadding * 2.0f
                                          : draw::TextWidth(labels[i]) + o.ItemPadding * 2.0f;
            if (ico && !o.IconOnly) w += iconSz + o.IconSpacing;
            widths[i] = w;
            total += w;
        }
        total += sepW * (count - 1);

        // ---- lay out
        static ImVector<float> xs;
        xs.resize(count);
        if (o.Alignment == TabAlign::Fill)
        {
            float each = availW / count, x = 0.0f;
            for (int i = 0; i < count; i++) { xs[i] = x; widths[i] = each; x += each; }
            sepW = 0.0f;
        }
        else
        {
            float start = 0.0f;
            if (o.Alignment == TabAlign::Center) start = (availW - total) * 0.5f;
            else if (o.Alignment == TabAlign::Right) start = availW - total;
            if (start < 0.0f) start = 0.0f;
            float x = start;
            for (int i = 0; i < count; i++) { xs[i] = x; x += widths[i] + sepW; }
        }

        // ---- input
        bool changed = false;
        int hoverIndex = -1;
        if (stripHovered)
        {
            for (int i = 0; i < count; i++)
            {
                float x0 = origin.x + xs[i], x1 = x0 + widths[i];
                if (mouse.x >= x0 && mouse.x < x1) { hoverIndex = i; break; }
            }
        }
        if (stripClicked && hoverIndex >= 0 && hoverIndex != *selected)
        {
            *selected = hoverIndex;
            changed = true;
        }

        // ---- animated indicator
        ImGuiID posId = ImGui::GetID("##indpos");
        ImGuiID sizeId = ImGui::GetID("##indsize");
        float targetX = xs[*selected], targetW = widths[*selected];
        float indX = Anim(posId, targetX, 20.0f);
        float indW = Anim(sizeId, targetW, 20.0f);

        if (o.Treatment != TabTreatment::Text)
            draw::Rect(dl, origin, ImVec2(origin.x + availW, origin.y + h), t.Tab);
        if (o.Treatment == TabTreatment::Segment)
            draw::Border(dl, origin, ImVec2(origin.x + availW, origin.y + h), t.Border);

        // ---- tabs
        for (int i = 0; i < count; i++)
        {
            ImVec2 mn(origin.x + xs[i], origin.y);
            ImVec2 mx(mn.x + widths[i], origin.y + h);
            bool active = (i == *selected);

            char hid[32]; snprintf(hid, sizeof(hid), "##t%d", i);
            float hot = Anim(ImGui::GetID(hid), (i == hoverIndex) ? 1.0f : 0.0f, 18.0f);

            ImU32 textCol;
            if (active)
                textCol = (o.Treatment == TabTreatment::Boxed) ? t.AccentText : t.Text;
            else
                textCol = LerpCol(t.TextInactive, t.Text, hot);

            switch (o.Treatment)
            {
            case TabTreatment::Boxed:
                draw::Box(dl, ImVec2(mn.x + 2, mn.y + 2), ImVec2(mx.x - 2, mx.y - 2),
                          active ? t.Accent : LerpCol(t.Element, t.ElementHover, hot),
                          active ? t.Accent : t.Border);
                break;
            case TabTreatment::Segment:
                if (active) draw::Rect(dl, ImVec2(mn.x + 1, mn.y + 1), ImVec2(mx.x - 1, mx.y - 1), t.Accent);
                else if (hot > 0.01f)
                    draw::Rect(dl, ImVec2(mn.x + 1, mn.y + 1), ImVec2(mx.x - 1, mx.y - 1), SetAlpha(t.ElementHover, (int)(hot * 255)));
                if (i > 0) draw::Line(dl, ImVec2(mn.x, mn.y + 1), ImVec2(mn.x, mx.y - 1), t.Border);
                break;
            case TabTreatment::TopBar:
                draw::Rect(dl, mn, mx, active ? t.Section : LerpCol(t.Tab, t.ElementHover, hot * 0.7f));
                break;
            default:
                if (!active && hot > 0.01f)
                    draw::Rect(dl, mn, mx, SetAlpha(t.ElementHover, (int)(hot * 90)));
                break;
            }

            // content
            bool hasIcon = o.ShowIcons && icons && icons[i] != Glyph::None;
            bool iconOnly = o.IconOnly && hasIcon;
            float labelW = iconOnly ? 0.0f : draw::TextWidth(labels[i]);
            float contentW = labelW + (hasIcon ? iconSz + (iconOnly ? 0.0f : o.IconSpacing) : 0.0f);
            float cx = (iconOnly || o.Alignment == TabAlign::Fill || o.Treatment == TabTreatment::Boxed ||
                        o.Treatment == TabTreatment::Segment || o.Treatment == TabTreatment::TopBar)
                       ? mn.x + (widths[i] - contentW) * 0.5f
                       : mn.x + o.ItemPadding;
            float cy = mn.y + (h - lh) * 0.5f;

            if (hasIcon)
            {
                DrawGlyph(dl, icons[i], ImVec2(cx, mn.y + (h - iconSz) * 0.5f),
                          ImVec2(cx + iconSz, mn.y + (h + iconSz) * 0.5f), textCol);
                cx += iconSz + o.IconSpacing;
            }
            if (!iconOnly)
            {
                if (o.AccentActive)
                {
                    // Cross-fade the plain label into the accent one so the colour
                    // travels with the selection instead of snapping.
                    char aid[32]; snprintf(aid, sizeof(aid), "##act%d", i);
                    float act = Anim(ImGui::GetID(aid), active ? 1.0f : 0.0f, 16.0f);
                    ImU32 base = LerpCol(t.TextInactive, t.Text, hot);
                    if (act < 0.99f) draw::Text(dl, ImVec2(cx, cy), WithAlpha(base, 1.0f - act), labels[i]);
                    if (act > 0.01f)
                    {
                        if (t.AccentGradient)
                            draw::TextGradient(dl, ImVec2(cx, cy), WithAlpha(AccentAt(0.0f), act),
                                               WithAlpha(AccentAt(1.0f), act), labels[i]);
                        else
                            draw::Text(dl, ImVec2(cx, cy), WithAlpha(t.Accent, act), labels[i]);
                    }
                }
                else
                    draw::Text(dl, ImVec2(cx, cy), textCol, labels[i]);
            }

            if (sepW > 0.0f && i < count - 1)
                draw::Text(dl, ImVec2(mx.x + 3.0f, cy), SetAlpha(t.TextInactive, 150), o.Separator);
        }

        // ---- indicator
        if (o.IndicatorThickness > 0.0f)
        {
            float th = o.IndicatorThickness;
            switch (o.Treatment)
            {
            case TabTreatment::Underline:
                draw::AccentRect(dl, ImVec2(origin.x + indX, origin.y + h - th),
                                 ImVec2(origin.x + indX + indW, origin.y + h),
                                 origin.x, origin.x + availW);
                break;
            case TabTreatment::TopBar:
                draw::AccentRect(dl, ImVec2(origin.x + indX, origin.y),
                                 ImVec2(origin.x + indX + indW, origin.y + th),
                                 origin.x, origin.x + availW);
                break;
            case TabTreatment::Text:
            {
                float w = VMax(8.0f, indW - o.ItemPadding * 2.0f);
                float x = origin.x + indX + (indW - w) * 0.5f;
                // The ramp runs across the whole strip, so the indicator picks up a
                // different slice of it per tab as it slides.
                draw::AccentRect(dl, ImVec2(x, origin.y + h - th),
                                 ImVec2(x + w, origin.y + h), origin.x, origin.x + availW);
                break;
            }
            default: break;
            }
        }

        ImGui::PopID();
        return changed;
    }

    bool Sidebar(const char* id, const char* const labels[], int count, int* selected,
                 const SidebarOpts& o, const Glyph icons[], const char* const descriptions[])
    {
        if (count <= 0 || !selected) return false;
        const Theme& t = theme();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        float lh = draw::LineHeight();
        float rowH = o.ItemHeight;
        float headH = (o.Title && *o.Title) ? lh + 12.0f : 0.0f;
        float totalH = headH + rowH * count;

        ImGui::PushID(id);
        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##side", ImVec2(o.Width, totalH));
        bool hovered = ImGui::IsItemHovered();
        bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        ImVec2 mouse = ImGui::GetIO().MousePos;

        draw::Rect(dl, origin, ImVec2(origin.x + o.Width, origin.y + totalH), t.Tab);

        if (headH > 0.0f)
            draw::Text(dl, ImVec2(origin.x + o.Indent, origin.y + 6.0f), t.TextInactive, o.Title);

        int hoverIndex = -1;
        if (hovered)
        {
            float local = mouse.y - origin.y - headH;
            if (local >= 0.0f) { int i = (int)(local / rowH); if (i >= 0 && i < count) hoverIndex = i; }
        }
        bool changed = false;
        if (clicked && hoverIndex >= 0 && hoverIndex != *selected) { *selected = hoverIndex; changed = true; }

        ImGuiID mkId = ImGui::GetID("##mark");
        float markY = Anim(mkId, headH + (*selected) * rowH, 20.0f);

        float iconSz = GlyphSizeFor(lh);
        for (int i = 0; i < count; i++)
        {
            ImVec2 mn(origin.x, origin.y + headH + i * rowH);
            ImVec2 mx(origin.x + o.Width, mn.y + rowH);
            bool active = (i == *selected);

            char hid[32]; snprintf(hid, sizeof(hid), "##s%d", i);
            float hot = Anim(ImGui::GetID(hid), (i == hoverIndex) ? 1.0f : 0.0f, 18.0f);

            if (active) draw::Rect(dl, mn, mx, SetAlpha(t.Accent, 26));
            else if (hot > 0.01f) draw::Rect(dl, mn, mx, SetAlpha(t.ElementHover, (int)(hot * 120)));

            ImU32 col = active ? t.Text : LerpCol(t.TextInactive, t.Text, hot);
            float x = mn.x + o.Indent;
            bool hasIcon = o.ShowIcons && icons && icons[i] != Glyph::None;
            if (hasIcon)
            {
                DrawGlyph(dl, icons[i], ImVec2(x, mn.y + (rowH - iconSz) * 0.5f),
                          ImVec2(x + iconSz, mn.y + (rowH + iconSz) * 0.5f), active ? t.Accent : col);
                x += iconSz + 8.0f;
            }

            bool detailed = descriptions && descriptions[i] && *descriptions[i];
            if (detailed)
            {
                float top = mn.y + (rowH - lh * 2.0f - 2.0f) * 0.5f;
                draw::Text(dl, ImVec2(x, top), col, labels[i]);
                draw::Text(dl, ImVec2(x, top + lh + 2.0f), SetAlpha(t.TextInactive, 190), descriptions[i]);
            }
            else
            {
                draw::Text(dl, ImVec2(x, mn.y + (rowH - lh) * 0.5f), col, labels[i]);
            }
        }

        if (o.MarkerWidth > 0.0f)
            draw::Rect(dl, ImVec2(origin.x, origin.y + markY),
                       ImVec2(origin.x + o.MarkerWidth, origin.y + markY + rowH), t.Accent);

        ImGui::PopID();
        return changed;
    }

    // ================================================================= pages
    static PageCtx g_pageCtx;

    bool BeginPage(const char* id, int index, int selected, const PageOpts& o)
    {
        ImGuiID pid = ImGui::GetID(id);
        PageRec* rec = GetPage(pid);

        bool active = (selected == index);
        if (active != rec->wasActive)
        {
            rec->phase = active ? 1 : 2;
            rec->timer = 0.0f;
            rec->dir = (selected >= rec->lastSelected) ? 1 : -1;
            rec->wasActive = active;
        }
        else
        {
            rec->timer += g_state.dt;
        }
        rec->lastSelected = selected;

        if (!theme().Animations || o.Transition == PageTransition::None)
        {
            if (!active) return false;
            g_pageCtx = PageCtx();
            g_pageCtx.opts = o;
            g_pageCtx.localTime = 1e6f;      // fully settled
            g_state.page = &g_pageCtx;
            ImGui::PushID(id);
            return true;
        }

        float fadeOut = o.FadeOutMs / 1000.0f;

        if (rec->phase == 2)
        {
            // Fading out. The build code still runs so there is something to fade —
            // this is the closest immediate-mode equivalent of the WinForms snapshot.
            if (rec->timer >= fadeOut) { rec->phase = 0; return false; }
            g_pageCtx = PageCtx();
            g_pageCtx.opts = o;
            g_pageCtx.outPhase = true;
            g_pageCtx.outAlpha = 1.0f - EaseF(Ease::OutCubic, fadeOut <= 0.0f ? 1.0f : rec->timer / fadeOut);
            g_pageCtx.dir = rec->dir;
            g_state.page = &g_pageCtx;
            ImGui::PushID(id);
            return true;
        }

        if (!active) return false;

        // Incoming: wait out the old page's fade, then run the staggered entry.
        float local = rec->timer - fadeOut;
        if (local < 0.0f) return false;

        g_pageCtx = PageCtx();
        g_pageCtx.opts = o;
        g_pageCtx.localTime = local;
        g_pageCtx.dir = rec->dir;
        g_state.page = &g_pageCtx;
        ImGui::PushID(id);
        return true;
    }

    void EndPage()
    {
        ImGui::PopID();
        g_state.page = nullptr;
        g_state.alphaMul = 1.0f;
        g_state.slidePending = 0.0f;
        g_state.slideWindow = nullptr;
    }

    // Mirrors the alpha half of ItemAnim's constructor for the *next* item index,
    // without incrementing it.
    float PageFade()
    {
        PageCtx* p = g_state.page;
        if (!p) return 1.0f;
        if (p->outPhase) return p->outAlpha;
        if (p->opts.Transition == PageTransition::Slide) return 1.0f;
        float delay = p->item * (p->opts.StaggerMs / 1000.0f);
        float local = p->localTime - delay;
        float dur = VMax(0.001f, p->opts.DurationMs / 1000.0f);
        return EaseF(p->opts.Curve, local <= 0.0f ? 0.0f : Clamp01(local / dur));
    }

    // ================================================================= sections
    struct SectionFrame
    {
        ImVec2 mn, mx;
        SectionHeader header;
        float headH;
        bool  autoH;
        float fade;                 // page fade at BeginSection: the frame tracks its first row
        Glyph icon;
        char  title[64];            // display text, "##" suffix already stripped
    };

    static ImVector<SectionFrame> g_sections;

    // Body, border and header. Fixed-height sections paint this up front; auto-
    // height ones paint it in EndSection once the content has been measured. That
    // still lands underneath the content, because a child window's draw list is
    // rendered after its parent's.
    static void PaintSection(const SectionFrame& f)
    {
        // Sections draw outside any ItemAnim, so without this the frame would sit
        // at full alpha while its rows fade in, then vanish in one frame on the
        // way out. Restored by the guard on every return path.
        struct AlphaScope
        {
            float saved;
            explicit AlphaScope(float k) : saved(g_state.alphaMul) { g_state.alphaMul = saved * k; }
            ~AlphaScope() { g_state.alphaMul = saved; }
        } scope(f.fade);

        const Theme& t = theme();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float lh = draw::LineHeight();
        ImVec2 mn = f.mn, mx = f.mx;
        SectionHeader header = f.header;
        float headH = f.headH;
        const char* title = f.title;

        float bodyTop = (header == SectionHeader::Fieldset || header == SectionHeader::Centered)
                        ? mn.y + headH * 0.5f : mn.y;

        draw::Rect(dl, ImVec2(mn.x, bodyTop), mx, t.Section);

        float pad = t.InnerPadding;
        bool hasIcon = f.icon != Glyph::None;
        float iconSz = GlyphSizeFor(lh);

        if (header == SectionHeader::Strip)
        {
            // Header fill with only its top corners rounded, so it meets the body
            // square along the divider.
            ImU32 hc = A(t.Header ? t.Header : t.Element);
            dl->AddRectFilled(mn, ImVec2(mx.x, mn.y + headH), hc, t.Radius, ImDrawFlags_RoundCornersTop);
            draw::Line(dl, ImVec2(mn.x, mn.y + headH - 1.0f), ImVec2(mx.x, mn.y + headH - 1.0f), t.Border);
        }
        draw::Border(dl, ImVec2(mn.x, bodyTop), mx, t.Border);

        if (!title[0] || header == SectionHeader::None) return;

        float tw = draw::TextWidth(title) + (hasIcon ? iconSz + 5.0f : 0.0f);
        if (header == SectionHeader::Fieldset || header == SectionHeader::Centered)
        {
            float tx = (header == SectionHeader::Centered) ? mn.x + ((mx.x - mn.x) - tw) * 0.5f : mn.x + pad;
            // Punch a gap in the border so the title sits in the line.
            draw::Rect(dl, ImVec2(tx - 4.0f, bodyTop), ImVec2(tx + tw + 4.0f, bodyTop + 1.0f), t.Section);
            if (hasIcon)
            {
                DrawGlyph(dl, f.icon, ImVec2(tx, mn.y + (headH - iconSz) * 0.5f),
                          ImVec2(tx + iconSz, mn.y + (headH + iconSz) * 0.5f), t.Accent);
                tx += iconSz + 5.0f;
            }
            draw::Text(dl, ImVec2(tx, mn.y), t.Text, title);
            return;
        }

        if (header == SectionHeader::TopAccent)
            draw::AccentRect(dl, mn, ImVec2(mx.x, mn.y + 1.0f), mn.x, mx.x);
        float tx = mn.x + pad;
        if (hasIcon)
        {
            DrawGlyph(dl, f.icon, ImVec2(tx, mn.y + (headH - iconSz) * 0.5f),
                      ImVec2(tx + iconSz, mn.y + (headH + iconSz) * 0.5f), t.Accent);
            tx += iconSz + 5.0f;
        }
        draw::Text(dl, ImVec2(tx, mn.y + (headH - lh) * 0.5f), t.Text, title);
        if (header == SectionHeader::Underline)
            draw::Line(dl, ImVec2(mn.x + pad, mn.y + headH - 1.0f),
                       ImVec2(mx.x - pad, mn.y + headH - 1.0f), t.Border);
    }

    bool BeginSection(const char* title, ImVec2 size, SectionHeader header, Glyph icon)
    {
        // Namespace everything inside. Control IDs come from their labels, and the
        // same label ("enabled") legitimately appears in several sections on one
        // page — without this they'd collide and share hover/click state.
        //
        // An untitled section gets a per-frame index instead. Two untitled
        // sections sharing one ID also share one child window, and ImGui quietly
        // appends the second one's contents into the first.
        if (title && *title) ImGui::PushID(title);
        else                 ImGui::PushID(0x5EC7 + g_state.untitled++);

        const Theme& t = theme();
        float lh = draw::LineHeight();

        float headH = 0.0f;
        switch (header)
        {
        case SectionHeader::None: headH = 0.0f; break;
        case SectionHeader::Fieldset:
        case SectionHeader::Centered: headH = lh; break;
        case SectionHeader::TopAccent:
        case SectionHeader::Strip: headH = t.HeaderHeight > 0.0f ? t.Scale(t.HeaderHeight) : lh + 10.0f; break;
        default: headH = lh + 8.0f; break;
        }

        bool autoH = size.y < 0.0f;
        if (size.x <= 0.0f) size.x = ImGui::GetContentRegionAvail().x;
        if (size.y == 0.0f) size.y = ImGui::GetContentRegionAvail().y;

        ImVec2 origin = ImGui::GetCursorScreenPos();
        SectionFrame f;
        f.mn = origin;
        f.mx = ImVec2(origin.x + size.x, origin.y + (autoH ? headH : size.y));
        f.header = header; f.headH = headH; f.autoH = autoH; f.icon = icon;
        f.fade = PageFade();
        f.title[0] = 0;
        if (title && *title)
        {
            const char* end = strstr(title, "##");
            size_t n = end ? (size_t)(end - title) : strlen(title);
            if (n >= sizeof(f.title)) n = sizeof(f.title) - 1;
            memcpy(f.title, title, n); f.title[n] = 0;
        }
        if (!autoH) PaintSection(f);
        g_sections.push_back(f);

        // Content region: an ImGui child so clipping comes for free.
        float pad = t.InnerPadding;
        float contentTop = ((header == SectionHeader::Fieldset || header == SectionHeader::Centered)
                            ? headH * 0.5f : headH) + pad;
        ImGui::SetCursorScreenPos(ImVec2(f.mn.x + pad, f.mn.y + contentTop));

        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(t.ItemSpacing, t.ItemSpacing));
        const ImGuiWindowFlags wf = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground;
        if (autoH)
            ImGui::BeginChild("##sec", ImVec2(size.x - pad * 2.0f, 0.0f), ImGuiChildFlags_AutoResizeY, wf);
        else
            ImGui::BeginChild("##sec", ImVec2(size.x - pad * 2.0f, size.y - contentTop - pad), 0, wf);
        return true;
    }

    void EndSection()
    {
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        if (g_sections.Size > 0)
        {
            SectionFrame f = g_sections.back();
            g_sections.pop_back();

            if (f.autoH)
            {
                // The child just reported its fitted height as the last item.
                f.mx.y = ImGui::GetItemRectMax().y + theme().InnerPadding;
                PaintSection(f);
            }

            // Submit an item spanning the whole section rather than just moving the
            // cursor. ImGui asserts if SetCursorScreenPos pushes past the window's
            // content bounds with no item submitted afterwards to grow them — and
            // callers position sections absolutely, so this is the one place that
            // can account for the space they occupy. It also leaves the cursor
            // under the section, which is what lets auto-height sections stack.
            ImGui::SetCursorScreenPos(f.mn);
            ImGui::Dummy(ImVec2(f.mx.x - f.mn.x, f.mx.y - f.mn.y));
        }

        ImGui::PopID();     // matches the PushID at the top of BeginSection
    }

    // ================================================================= checkbox
    // A colour swatch. With GradientVertical it picks up the same top-lit shading
    // as the accent fills, derived from its own colour, so it sits in the look.
    static void DrawSwatch(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 col, ImU32 border)
    {
        const Theme& t = theme();
        if ((col & IM_COL32_A_MASK) != IM_COL32_A_MASK) draw::Checker(dl, mn, mx, 3.0f);
        if (t.GradientVertical) draw::RectGradient(dl, mn, mx, col, Darken(col, 0.42f), false);
        else                    draw::Rect(dl, mn, mx, col);
        draw::Border(dl, mn, mx, border);
    }

    // The colour popup shared by ColorPicker and the checkbox swatch.
    static bool ColorPopup(const char* popupId, ImU32* col, bool allowAlpha)
    {
        const Theme& t = theme();
        bool changed = false;
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(t.Element));
        if (ImGui::BeginPopup(popupId))
        {
            ImVec4 c = ImGui::ColorConvertU32ToFloat4(*col);
            ImGuiColorEditFlags flags = ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview;
            flags |= allowAlpha ? ImGuiColorEditFlags_AlphaBar : ImGuiColorEditFlags_NoAlpha;
            if (ImGui::ColorPicker4("##pick", (float*)&c, flags))
            {
                *col = ImGui::ColorConvertFloat4ToU32(c);
                changed = true;
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor();
        return changed;
    }

    bool Checkbox(const char* label, bool* value, CheckGlyph glyph, const char* accessory, ImU32* swatch)
    {
        ItemAnim anim;
        const Theme& t = theme();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        float lh = draw::LineHeight();
        float h = VMax(RowH(), lh + 2.0f);
        bool hovered = false, clicked = false;
        Area r = Reserve(0.0f, h, label, &hovered, nullptr, &clicked);

        bool right = t.CheckboxRight;
        bool sw = t.CheckboxSwitch && glyph != CheckGlyph::Radio;
        float box = t.CheckboxSize > 0.0f ? t.Scale(t.CheckboxSize) : VMax(9.0f, lh - 2.0f);
        box = VMin(box, h);
        // A switch is a pill: same height as the box would be, 1.75x as wide.
        float boxW = sw ? box * 1.75f : box;
        ImVec2 bmn(right ? r.Max.x - boxW : r.Min.x, r.Min.y + (h - box) * 0.5f);
        ImVec2 bmx(bmn.x + boxW, bmn.y + box);

        // Classic: a square swatch at the far right. Right-hand box: a pill that
        // sits just left of the box, the way minimal menus pair colour + toggle.
        float swH = right ? box : box + 2.0f;
        float swW = right ? VMax(24.0f, box * 1.75f) : swH;
        float swR = right ? bmn.x - 6.0f : r.Max.x - 1.0f;   // bmn already accounts for pill width
        ImVec2 smn(swR - swW, r.Min.y + (h - swH) * 0.5f);
        ImVec2 smx(swR, smn.y + swH);
        ImVec2 mouse = ImGui::GetIO().MousePos;
        bool overSwatch = swatch && hovered && mouse.x >= smn.x && mouse.x < smx.x;

        char popupId[96]; snprintf(popupId, sizeof(popupId), "##cbcol_%s", label);
        bool changed = false;
        if (clicked && overSwatch) ImGui::OpenPopup(popupId);
        else if (clicked) { *value = !*value; changed = true; }

        float hot = Anim(ImGui::GetID(label), hovered && !overSwatch ? 1.0f : 0.0f, 18.0f);
        char fid[96]; snprintf(fid, sizeof(fid), "%s##fill", label);
        float fill = Anim(ImGui::GetID(fid), *value ? 1.0f : 0.0f, 20.0f);

        ImU32 off = LerpCol(t.Element, t.ElementHover, hot * 0.8f);
        ImU32 f = LerpCol(off, t.Accent, fill);
        ImU32 bd = LerpCol(t.Border, t.AccentGradient ? AccentAt(1.0f) : t.Accent, fill);

        if (sw)
        {
            // Rounded track, and a knob that slides across it. The knob is drawn
            // from the eased fill, so it travels with the colour change.
            float rad = box * 0.5f;
            ImU32 track = LerpCol(off, t.AccentGradient ? AccentAt(1.0f) : t.Accent, fill);
            dl->AddRectFilled(bmn, bmx, A(track), rad);
            if (fill < 0.99f)
                dl->AddRect(bmn, bmx, A(SetAlpha(t.Border, (int)(255 * (1.0f - fill)))), rad, 0, 1.0f);

            float kr = rad - 2.0f;
            float kx = bmn.x + rad + (boxW - box) * EaseF(Ease::OutCubic, fill);
            ImU32 knob = LerpCol(t.TextInactive, IM_COL32(255, 255, 255, 255), VMax(fill, hot * 0.4f));
            dl->AddCircleFilled(ImVec2(kx, (bmn.y + bmx.y) * 0.5f), kr, A(knob));
        }
        else if (glyph == CheckGlyph::Radio)
        {
            ImVec2 mid((bmn.x + bmx.x) * 0.5f, (bmn.y + bmx.y) * 0.5f);
            dl->AddCircleFilled(mid, box * 0.5f, A(off));
            dl->AddCircle(mid, box * 0.5f, A(bd), 0, 1.0f);
            if (fill > 0.01f) dl->AddCircleFilled(mid, box * 0.28f, A(SetAlpha(t.Accent, (int)(255 * fill))));
        }
        else
        {
            // Check fills with the accent as well and stamps the tick on top of it.
            // Dot keeps the plain element fill so the inner square reads against it.
            bool filled = (glyph == CheckGlyph::Fill || glyph == CheckGlyph::Check);
            if (filled && t.AccentGradient)
            {
                draw::Rect(dl, bmn, bmx, off);
                draw::AccentRect(dl, bmn, bmx, bmn.x, bmx.x, (int)(255 * fill));
                draw::Border(dl, bmn, bmx, bd);
            }
            else
                draw::Box(dl, bmn, bmx, filled ? f : off, bd);

            if (fill > 0.01f)
            {
                if (glyph == CheckGlyph::Check)
                    draw::Check(dl, ImVec2(bmn.x + 1, bmn.y + 1), ImVec2(bmx.x - 1, bmx.y - 1),
                                SetAlpha(t.AccentText, (int)(255 * fill)));
                else if (glyph == CheckGlyph::Dot)
                {
                    float in = VMax(2.0f, box * 0.25f);
                    draw::AccentRect(dl, ImVec2(bmn.x + in, bmn.y + in), ImVec2(bmx.x - in, bmx.y - in),
                                     bmn.x, bmx.x, (int)(255 * fill));
                }
            }
        }

        float right_ = swatch ? smn.x - 6.0f : (right ? bmn.x - 6.0f : r.Max.x);
        if (accessory && *accessory)
        {
            float aw = draw::TextWidth(accessory);
            draw::Text(dl, ImVec2(right_ - aw, r.Min.y + (h - lh) * 0.5f), t.TextInactive, accessory);
            right_ -= aw + 8.0f;
        }

        float tx = right ? r.Min.x : bmx.x + 7.0f;
        ImU32 lc = t.DimOffLabels ? LerpCol(t.TextInactive, t.Text, fill) : t.Text;
        draw::TextClipped(dl, ImVec2(tx, r.Min.y + (h - lh) * 0.5f), right_ - tx,
                          LerpCol(lc, Lighten(lc, 0.25f), hot), label);

        if (swatch)
        {
            DrawSwatch(dl, smn, smx, *swatch, overSwatch ? t.Accent : t.Border);
            if (ColorPopup(popupId, swatch, true)) changed = true;
        }
        return changed;
    }

    // ================================================================= slider
    static bool SliderImpl(const char* caption, float* value, float mn_, float mx_,
                           const char* suffix, int decimals, SliderValue placement,
                           bool showKnob, float step)
    {
        ItemAnim anim;
        const Theme& t = theme();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        float lh = draw::LineHeight();
        // BoxRight is a one-line row: caption left, track in the middle, value in a
        // box at the right. Every other placement keeps the caption above the track.
        bool inlineRow = (placement == SliderValue::BoxRight);
        bool hasCaption = (caption && *caption) || placement == SliderValue::CaptionRight;
        float capH = (hasCaption && !inlineRow) ? lh + 3.0f : 0.0f;
        float trackH = t.SliderHeight > 0.0f ? t.Scale(t.SliderHeight) : VMax(4.0f, lh * 0.5f);
        float h = capH + trackH + 2.0f;
        if (placement == SliderValue::BoxRight)
            h = VMax(h, t.FieldHeight > 0.0f ? t.Scale(t.FieldHeight) + 2.0f : lh + 8.0f);

        bool hovered = false, held = false;
        Area r = Reserve(0.0f, h, caption, &hovered, &held);

        char valueText[64];
        char fmt[16]; snprintf(fmt, sizeof(fmt), "%%.%df%%s", decimals < 0 ? 0 : decimals);
        snprintf(valueText, sizeof(valueText), fmt, *value, suffix ? suffix : "");

        float boxW = 0.0f;
        if (placement == SliderValue::BoxRight)
            boxW = VMax(34.0f, draw::TextWidth(valueText) + 14.0f);
        float valueW = (placement == SliderValue::TrackRight) ? draw::TextWidth(valueText) + 8.0f
                     : (placement == SliderValue::BoxRight)   ? boxW + 10.0f : 0.0f;
        // Inline sliders share a label column (a third of the row, or the label if
        // longer), so every track in a card starts at the same x.
        float capW = (inlineRow && caption && *caption)
                     ? VMax(draw::TextWidth(caption) + 14.0f, (r.Max.x - r.Min.x) * 0.34f) : 0.0f;
        ImVec2 tmn(r.Min.x + capW, r.Min.y + capH + (inlineRow ? (h - trackH) * 0.5f : 0.0f));
        ImVec2 tmx(r.Max.x - valueW, tmn.y + trackH);

        bool changed = false;
        if (held && tmx.x > tmn.x)
        {
            float p = Clamp01((ImGui::GetIO().MousePos.x - tmn.x) / (tmx.x - tmn.x));
            float nv = mn_ + p * (mx_ - mn_);
            if (step > 0.0f) nv = mn_ + floorf((nv - mn_) / step + 0.5f) * step;
            if (nv != *value) { *value = nv; changed = true; }
        }

        float range = (mx_ - mn_);
        if (fabsf(range) < 1e-6f) range = 1.0f;

        char aid[96]; snprintf(aid, sizeof(aid), "%s##disp", caption);
        float disp = Anim(ImGui::GetID(aid), *value, held ? 40.0f : 20.0f);
        float norm = Clamp01((disp - mn_) / range);

        float hot = Anim(ImGui::GetID(caption), hovered ? 1.0f : 0.0f, 18.0f);

        if (inlineRow)
        {
            if (caption && *caption)
                draw::TextClipped(dl, ImVec2(r.Min.x, (r.Min.y + r.Max.y) * 0.5f - lh * 0.5f),
                                  capW - 8.0f, t.Text, caption);
        }
        else if (hasCaption)
        {
            if (caption && *caption)
            {
                float capW = (placement == SliderValue::CaptionRight)
                             ? r.Max.x - r.Min.x - draw::TextWidth(valueText) - 8.0f
                             : r.Max.x - r.Min.x;
                draw::TextClipped(dl, r.Min, capW, t.Text, caption);
            }
            if (placement == SliderValue::CaptionRight)
                draw::Text(dl, ImVec2(r.Max.x - draw::TextWidth(valueText), r.Min.y), t.TextInactive, valueText);
        }

        draw::Rect(dl, tmn, tmx, LerpCol(t.Element, t.ElementHover, hot * 0.6f));
        float fw = (tmx.x - tmn.x) * norm;
        if (fw > 0.0f) draw::AccentRect(dl, tmn, ImVec2(tmn.x + fw, tmx.y), tmn.x, tmx.x);
        draw::Border(dl, tmn, tmx, t.Border);

        if (showKnob && t.SliderKnobRound)
        {
            // A circle riding the end of the fill, inset so it never hangs past
            // either end of the track.
            float kr = VMax(4.0f, trackH * 1.1f);
            float kx = ClampF(tmn.x + fw, tmn.x + kr, tmx.x - kr);
            ImVec2 mid(kx, (tmn.y + tmx.y) * 0.5f);
            float grow = 1.0f + 0.12f * VMax(hot, held ? 1.0f : 0.0f);
            dl->AddCircleFilled(mid, kr * grow, A(IM_COL32(255, 255, 255, 255)));
        }
        else if (showKnob)
        {
            // A small raised block that rides the end of the fill and stands 2px
            // proud of the track at top and bottom.
            float kw = VMax(6.0f, trackH + 2.0f);
            float kx = ClampF(tmn.x + fw - kw * 0.5f, tmn.x, tmx.x - kw);
            ImVec2 kmn(kx, tmn.y - 2.0f), kmx(kx + kw, tmx.y + 2.0f);
            draw::Box(dl, kmn, kmx, LerpCol(t.ElementHover, Lighten(t.ElementHover, 0.08f), VMax(hot, held ? 1.0f : 0.0f)),
                      LerpCol(t.Border, Lighten(t.Border, 0.1f), hot));
        }

        if (placement == SliderValue::Overlay)
        {
            float vw = draw::TextWidth(valueText);
            draw::Text(dl, ImVec2((tmn.x + tmx.x - vw) * 0.5f, tmn.y + (trackH - lh) * 0.5f),
                       ReadableOn(norm > 0.5f ? t.Accent : t.Element), valueText);
        }
        else if (placement == SliderValue::TrackRight)
        {
            draw::Text(dl, ImVec2(tmx.x + 4.0f, tmn.y + (trackH - lh) * 0.5f), t.TextInactive, valueText);
        }
        else if (placement == SliderValue::BoxRight)
        {
            // The value in its own outlined box at the right, like a read-only field.
            float bh = VMax(lh + 6.0f, t.FieldHeight > 0.0f ? t.Scale(t.FieldHeight) : lh + 6.0f);
            ImVec2 vmn(r.Max.x - boxW, (tmn.y + tmx.y) * 0.5f - bh * 0.5f);
            ImVec2 vmx(r.Max.x, vmn.y + bh);
            draw::Border(dl, vmn, vmx, t.Element);
            float vw = draw::TextWidth(valueText);
            draw::Text(dl, ImVec2((vmn.x + vmx.x - vw) * 0.5f, (vmn.y + vmx.y) * 0.5f - lh * 0.5f),
                       t.Text, valueText);
        }
        return changed;
    }

    bool SliderFloat(const char* caption, float* value, float mn, float mx, const char* suffix,
                     int decimals, SliderValue placement, bool showKnob, float step)
    {
        return SliderImpl(caption, value, mn, mx, suffix, decimals, placement, showKnob, step);
    }

    bool SliderInt(const char* caption, int* value, int mn, int mx, const char* suffix,
                   SliderValue placement, bool showKnob)
    {
        float v = (float)*value;
        bool ch = SliderImpl(caption, &v, (float)mn, (float)mx, suffix, 0, placement, showKnob, 1.0f);
        if (ch) *value = (int)(v + 0.5f);
        return ch;
    }

    // ================================================================= dropdown
    // A chevron that rotates through flat as the list opens: t 0 = down, 1 = up.
    static void AnimChevron(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 col, float t,
                            float thickness = 1.4f)
    {
        if ((col & IM_COL32_A_MASK) == 0) return;
        float cx = (mn.x + mx.x) * 0.5f, cy = (mn.y + mx.y) * 0.5f;
        float w = VMin(mx.x - mn.x, mx.y - mn.y) * 0.30f;
        float dy = w * 0.6f * (1.0f - 2.0f * Clamp01(t));
        dl->PathLineTo(ImVec2(cx - w, cy - dy));
        dl->PathLineTo(ImVec2(cx, cy + dy));
        dl->PathLineTo(ImVec2(cx + w, cy - dy));
        dl->PathStroke(col, 0, thickness);
    }

    static bool DropdownImpl(const char* caption, int* current, bool checked[],
                             const char* const items[], int count,
                             bool multi, bool countBadge, const char* placeholder)
    {
        ItemAnim anim;
        const Theme& t = theme();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        float lh = draw::LineHeight();
        // "##id" captions are ID-only, as everywhere in ImGui: not drawn, no row.
        bool hasCap = (caption && *caption && !(caption[0] == '#' && caption[1] == '#'));
        bool inlineRow = t.InlineFields && hasCap;
        float capH = (hasCap && !inlineRow) ? lh + 3.0f : 0.0f;
        float boxH = t.FieldHeight > 0.0f ? t.Scale(t.FieldHeight) : lh + 8.0f;

        bool hovered = false, clicked = false;
        Area r = Reserve(0.0f, capH + boxH, caption ? caption : "##dd", &hovered, nullptr, &clicked);

        // Inline: the box takes the right FieldFraction of the row, caption the rest.
        float frac = t.FieldFraction > 0.0f ? t.FieldFraction : 0.45f;
        float boxLeft = inlineRow ? r.Max.x - (r.Max.x - r.Min.x) * frac : r.Min.x;
        ImVec2 bmn(boxLeft, r.Min.y + capH);
        ImVec2 bmx(r.Max.x, bmn.y + boxH);

        char popupId[96];
        snprintf(popupId, sizeof(popupId), "##pop_%s", caption ? caption : "dd");
        if (clicked && (!inlineRow || ImGui::GetIO().MousePos.x >= boxLeft)) ImGui::OpenPopup(popupId);

        if (capH > 0.0f) draw::Text(dl, r.Min, t.Text, caption);
        else if (inlineRow)
            draw::TextClipped(dl, ImVec2(r.Min.x, (r.Min.y + r.Max.y) * 0.5f - lh * 0.5f),
                              boxLeft - r.Min.x - 8.0f, t.Text, caption);

        float hot = Anim(ImGui::GetID(caption ? caption : "##dd"), hovered ? 1.0f : 0.0f, 18.0f);
        draw::Box(dl, bmn, bmx, LerpCol(t.Element, t.ElementHover, hot * 0.7f),
                  LerpCol(t.Border, t.Accent, hot * 0.4f));

        // header text
        char header[192];
        if (multi)
        {
            int n = 0; header[0] = 0;
            for (int i = 0; i < count; i++) if (checked[i]) n++;
            if (n == 0) snprintf(header, sizeof(header), "%s", placeholder);
            else
            {
                int shown = 0; size_t used = 0;
                for (int i = 0; i < count && shown < 2; i++)
                {
                    if (!checked[i]) continue;
                    used += snprintf(header + used, sizeof(header) - used, "%s%s", shown ? ", " : "", items[i]);
                    shown++;
                }
                if (n > shown) snprintf(header + used, sizeof(header) - used, ", +%d", n - shown);
            }
        }
        else
        {
            snprintf(header, sizeof(header), "%s",
                     (*current >= 0 && *current < count) ? items[*current] : placeholder);
        }

        float arrowW = t.DropdownListIcon ? 26.0f : 16.0f;
        float badgeW = 0.0f;
        char badge[24] = "";
        if (multi && countBadge)
        {
            int n = 0; for (int i = 0; i < count; i++) if (checked[i]) n++;
            snprintf(badge, sizeof(badge), "%d/%d", n, count);
            badgeW = draw::TextWidth(badge) + 10.0f;
        }

        draw::TextClipped(dl, ImVec2(bmn.x + 7.0f, bmn.y + (boxH - lh) * 0.5f),
                          (bmx.x - arrowW - badgeW) - (bmn.x + 7.0f),
                          t.MutedFieldText ? LerpCol(t.TextInactive, t.Text, hot * 0.6f) : t.Text, header);

        if (badgeW > 0.0f)
        {
            ImVec2 mn2(bmx.x - arrowW - badgeW, bmn.y + (boxH - lh - 2.0f) * 0.5f);
            ImVec2 mx2(mn2.x + badgeW - 4.0f, mn2.y + lh + 2.0f);
            draw::Box(dl, mn2, mx2, t.Content, t.Border);
            draw::Text(dl, ImVec2(mn2.x + 4.0f, mn2.y + 1.0f), t.Accent, badge);
        }

        // ---- popup list
        bool changed = false;
        bool isOpen = ImGui::IsPopupOpen(popupId);

        // Grows out of the header rather than snapping open. Driven from the parent
        // window's storage, because the popup's own storage doesn't exist yet on the
        // frame it opens.
        //
        // Keyed off the popup ID, not a fixed "##ddopen": that resolved to one ID
        // per section, so two dropdowns in a panel drove the same value in opposite
        // directions every frame and the open one stalled part-way, cutting its
        // list short.
        char oid[112]; snprintf(oid, sizeof(oid), "%s##open", popupId);
        float openT = Anim(ImGui::GetID(oid), isOpen ? 1.0f : 0.0f, 22.0f);

        if (t.DropdownListIcon)
        {
            float gs = GlyphSizeFor(lh);
            float gx = bmx.x - arrowW * 0.5f - 3.0f, gy = (bmn.y + bmx.y) * 0.5f;
            DrawGlyph(dl, Glyph::List, ImVec2(gx - gs * 0.5f, gy - gs * 0.5f), ImVec2(gx + gs * 0.5f, gy + gs * 0.5f),
                      LerpCol(LerpCol(t.Border, t.TextInactive, 0.25f), t.Accent, VMax(openT, hot * 0.5f)));
        }
        else
            AnimChevron(dl, ImVec2(bmx.x - arrowW, bmn.y), ImVec2(bmx.x, bmx.y),
                        A(t.TextInactive), EaseF(Ease::OutCubic, openT));

        float rowH = lh + 8.0f;
        float fullH = count * rowH + 2.0f;

        // A list that won't fit below the box opens upward if there's more room
        // there, and past ten rows (or the display edge) it scrolls instead of
        // running off the bottom of the window.
        float below = ImGui::GetIO().DisplaySize.y - bmx.y - 6.0f;
        float above = bmn.y - 6.0f;
        bool up = fullH > below && above > below;
        float room = up ? above : below;
        float maxH = VMax(rowH * 3.0f, VMin(room, rowH * 10.0f)) + 2.0f;
        bool scrolls = fullH > maxH;
        if (scrolls) fullH = floorf((maxH - 2.0f) / rowH) * rowH + 2.0f;
        float popH = VMax(1.0f, fullH * EaseF(Ease::OutCubic, openT));

        ImGui::SetNextWindowPos(ImVec2(bmn.x, up ? bmn.y - 1.0f - popH : bmx.y + 1.0f));
        ImGui::SetNextWindowSize(ImVec2(bmx.x - bmn.x, popH));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(t.Element));
        ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(t.Border));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImGui::ColorConvertU32ToFloat4(t.Border));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImGui::ColorConvertU32ToFloat4(t.TextInactive));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ImGui::ColorConvertU32ToFloat4(t.Accent));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(1, 1));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, t.Radius);

        // Rows are drawn by hand rather than with ImGui::Selectable, which brings its
        // own highlight colour, padding and text alignment and looks nothing like the
        // rest of the menu.
        ImGuiWindowFlags popFlags = scrolls ? 0 : (ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        if (ImGui::BeginPopup(popupId, popFlags))
        {
            ImDrawList* pdl = ImGui::GetWindowDrawList();
            // Open a scrolling list with the current choice in view.
            if (scrolls && ImGui::IsWindowAppearing() && !multi && *current >= 0)
                ImGui::SetScrollY(VMax(0.0f, (*current + 0.5f) * rowH - fullH * 0.5f));
            ImVec2 origin = ImGui::GetCursorScreenPos();
            float pw = ImGui::GetContentRegionAvail().x;
            float boxSz = VMax(9.0f, lh - 3.0f);

            for (int i = 0; i < count; i++)
            {
                ImGui::PushID(i);

                ImVec2 rmn(origin.x, origin.y + i * rowH);
                ImVec2 rmx(rmn.x + pw, rmn.y + rowH);

                ImGui::SetCursorScreenPos(rmn);
                ImGui::InvisibleButton("##row", ImVec2(pw, rowH));
                bool hov = ImGui::IsItemHovered();
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
                {
                    if (multi) { checked[i] = !checked[i]; changed = true; }
                    else { *current = i; changed = true; ImGui::CloseCurrentPopup(); }
                }

                bool sel = multi ? checked[i] : (i == *current);
                float hot = Anim(ImGui::GetID("##hot"), hov ? 1.0f : 0.0f, 20.0f);
                float on = Anim(ImGui::GetID("##sel"), sel ? 1.0f : 0.0f, 18.0f);

                // hover wash, then a faint accent tint for the current selection
                ImU32 fill = LerpCol(t.Element, t.ElementHover, hot);
                fill = LerpCol(fill, SetAlpha(t.Accent, 34), on * 0.9f);
                pdl->AddRectFilled(rmn, rmx, fill);

                // accent bar grows down the leading edge of the selected row
                if (on > 0.01f)
                {
                    float barH = rowH * EaseF(Ease::OutCubic, on);
                    float cy = (rmn.y + rmx.y) * 0.5f;
                    pdl->AddRectFilled(ImVec2(rmn.x, cy - barH * 0.5f),
                                       ImVec2(rmn.x + 2.0f, cy + barH * 0.5f), t.Accent);
                }

                float tx = rmn.x + 8.0f;
                if (multi)
                {
                    ImVec2 cmn(tx, rmn.y + (rowH - boxSz) * 0.5f);
                    ImVec2 cmx(cmn.x + boxSz, cmn.y + boxSz);
                    pdl->AddRectFilled(cmn, cmx, LerpCol(t.Content, t.Accent, on));
                    pdl->AddRect(cmn, cmx, LerpCol(t.Border, t.Accent, on), 0.0f, 0, 1.0f);
                    tx += boxSz + 7.0f;
                }

                ImU32 textCol = LerpCol(LerpCol(t.TextInactive, t.Text, hot), t.Text, on);
                float ty = rmn.y + (rowH - lh) * 0.5f;
                if (t.TextShadows)
                    pdl->AddText(ImVec2(tx + 1.0f, ty + 1.0f), t.TextShadow, items[i]);
                pdl->AddText(ImVec2(tx, ty), textCol, items[i]);

                ImGui::PopID();
            }

            ImGui::EndPopup();
        }
        ImGui::PopStyleVar(4);
        ImGui::PopStyleColor(6);
        return changed;
    }

    bool Dropdown(const char* caption, int* current, const char* const items[], int count, const char* placeholder)
    {
        return DropdownImpl(caption, current, nullptr, items, count, false, false, placeholder);
    }

    bool MultiDropdown(const char* caption, bool checked[], const char* const items[], int count,
                       bool countBadge, const char* placeholder)
    {
        int dummy = -1;
        return DropdownImpl(caption, &dummy, checked, items, count, true, countBadge, placeholder);
    }

    // ================================================================= keybind
    const char* KeybindName(int bind)
    {
        if (bind == 0) return "None";
        switch (bind)
        {
        case -1: return "LMB"; case -2: return "RMB"; case -3: return "MMB";
        case -4: return "MB4"; case -5: return "MB5";
        }
        const char* n = ImGui::GetKeyName((ImGuiKey)bind);
        return n ? n : "?";
    }

    bool KeybindActive(int bind, KeybindMode mode, bool* toggleState)
    {
        if (mode == KeybindMode::Always) return true;
        if (bind == 0) return false;

        bool down, pressed;
        if (bind < 0)
        {
            int btn = (-bind) - 1;
            down = ImGui::IsMouseDown((ImGuiMouseButton)btn);
            pressed = ImGui::IsMouseClicked((ImGuiMouseButton)btn);
        }
        else
        {
            down = ImGui::IsKeyDown((ImGuiKey)bind);
            pressed = ImGui::IsKeyPressed((ImGuiKey)bind, false);
        }

        if (mode == KeybindMode::Hold) return down;
        if (pressed && toggleState) *toggleState = !*toggleState;
        return toggleState ? *toggleState : false;
    }

    bool Keybind(const char* caption, int* bind, KeybindMode* mode)
    {
        ItemAnim anim;
        // Every keybind needs its own ID scope. Without this the "##capture" flag
        // below resolves to the same ID for every keybind in a section: clicking one
        // would put them all into capture, and whichever row the cursor was *not*
        // over would instantly bind left-mouse.
        ImGui::PushID(caption);

        const Theme& t = theme();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        float lh = draw::LineHeight();
        float h = VMax(RowH(), lh + 2.0f);

        bool hovered = false, clicked = false;
        Area r = Reserve(0.0f, h, caption, &hovered, nullptr, &clicked);

        ImGuiID capId = ImGui::GetID("##capture");
        ImGuiStorage* st = ImGui::GetStateStorage();
        bool capturing = st->GetBool(capId, false);

        const char* label = capturing ? "..." : KeybindName(*bind);
        float cw = VMax(28.0f, draw::TextWidth(label) + 14.0f);
        float ch = VMin(h, lh + 4.0f);
        ImVec2 cmn(r.Max.x - cw, r.Min.y + (h - ch) * 0.5f);
        ImVec2 cmx(r.Max.x, cmn.y + ch);

        bool overChip = hovered && ImGui::GetIO().MousePos.x >= cmn.x;
        bool changed = false;

        // The click that opens capture must not also be captured as the bind, so the
        // rest of this is skipped on the frame capture starts. From the next frame on,
        // any key or button lands — including left-mouse, which is a legitimate bind.
        bool justStarted = false;
        if (clicked && overChip && !capturing)
        {
            st->SetBool(capId, true);
            capturing = true;
            justStarted = true;
        }

        if (capturing && !justStarted)
        {
            // Any key or mouse button lands the bind; Escape/Delete clears it.
            for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++)
            {
                if (!ImGui::IsKeyPressed((ImGuiKey)k, false)) continue;
                if (k == ImGuiKey_Escape || k == ImGuiKey_Delete) *bind = 0;
                else *bind = k;
                st->SetBool(capId, false);
                capturing = false;
                changed = true;
                break;
            }
            if (capturing)
            {
                for (int b = 0; b < 5; b++)
                {
                    if (!ImGui::IsMouseClicked((ImGuiMouseButton)b)) continue;
                    *bind = -(b + 1);
                    st->SetBool(capId, false);
                    capturing = false;
                    changed = true;
                    break;
                }
            }
        }

        if (mode && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            *mode = (KeybindMode)(((int)(*mode) + 1) % 3);   // Hold -> Toggle -> Always
            changed = true;
        }

        draw::TextClipped(dl, ImVec2(r.Min.x, r.Min.y + (h - lh) * 0.5f), cmn.x - r.Min.x - 6.0f,
                          t.Text, caption);

        ImU32 fill, border, text;
        if (capturing)
        {
            float k = 0.45f + 0.55f * fabsf(sinf((float)Time() * 3.4f));
            fill = SetAlpha(t.Accent, (int)(70 * k));
            border = t.Accent; text = t.Accent;
        }
        else
        {
            float hot = Anim(ImGui::GetID(caption), overChip ? 1.0f : 0.0f, 18.0f);
            fill = LerpCol(t.Element, t.ElementHover, hot * 0.8f);
            border = LerpCol(t.Border, t.Accent, hot * 0.5f);
            text = (*bind == 0) ? t.TextInactive : t.Text;
        }

        draw::Box(dl, cmn, cmx, fill, border);
        float tw = draw::TextWidth(label);
        draw::Text(dl, ImVec2((cmn.x + cmx.x - tw) * 0.5f, cmn.y + (ch - lh) * 0.5f), text, label);

        if (mode && *mode != KeybindMode::Toggle && !capturing)
            draw::Rect(dl, ImVec2(cmn.x - 5.0f, cmn.y + ch * 0.5f - 1.0f), ImVec2(cmn.x - 3.0f, cmn.y + ch * 0.5f + 1.0f),
                       (*mode == KeybindMode::Always) ? t.Positive : t.Warning);

        ImGui::PopID();
        return changed;
    }

    // ================================================================= color picker
    bool ColorPicker(const char* caption, ImU32* col, SwatchShape shape, bool showHex, bool allowAlpha)
    {
        ItemAnim anim;
        const Theme& t = theme();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        float lh = draw::LineHeight();
        float h = VMax(RowH(), lh + 2.0f);
        bool hovered = false, clicked = false;
        Area r = Reserve(0.0f, h, caption, &hovered, nullptr, &clicked);

        float sh = VMin(h - 2.0f, lh + 2.0f);
        float sw = (shape == SwatchShape::Bar) ? 26.0f : sh;
        ImVec2 smn(r.Max.x - sw, r.Min.y + (h - sh) * 0.5f);
        ImVec2 smx(r.Max.x, smn.y + sh);

        char popupId[96]; snprintf(popupId, sizeof(popupId), "##col_%s", caption);
        if (clicked) ImGui::OpenPopup(popupId);

        float hexW = 0.0f;
        if (showHex)
        {
            char hex[16]; ToHex(*col, hex, sizeof(hex));
            hexW = draw::TextWidth(hex) + 8.0f;
            draw::Text(dl, ImVec2(smn.x - hexW, r.Min.y + (h - lh) * 0.5f), t.TextInactive, hex);
        }

        draw::TextClipped(dl, ImVec2(r.Min.x, r.Min.y + (h - lh) * 0.5f),
                          smn.x - hexW - r.Min.x - 4.0f, t.Text, caption);

        float hot = Anim(ImGui::GetID(caption), hovered ? 1.0f : 0.0f, 18.0f);
        ImU32 border = LerpCol(t.Border, t.Accent, hot);

        if (shape == SwatchShape::Dot)
        {
            ImVec2 mid((smn.x + smx.x) * 0.5f, (smn.y + smx.y) * 0.5f);
            dl->AddCircleFilled(mid, sh * 0.5f, A(*col));
            dl->AddCircle(mid, sh * 0.5f, A(border), 0, 1.0f);
        }
        else
            DrawSwatch(dl, smn, smx, *col, border);

        bool changed = false;
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(t.Element));
        if (ImGui::BeginPopup(popupId))
        {
            ImVec4 c = ImGui::ColorConvertU32ToFloat4(*col);
            ImGuiColorEditFlags flags = ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview;
            if (!allowAlpha) flags |= ImGuiColorEditFlags_NoAlpha;
            else flags |= ImGuiColorEditFlags_AlphaBar;
            if (ImGui::ColorPicker4("##pick", (float*)&c, flags))
            {
                *col = ImGui::ColorConvertFloat4ToU32(c);
                changed = true;
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor();
        return changed;
    }

    // ================================================================= textbox
    bool TextBox(const char* caption, char* buf, size_t bufSize, const char* placeholder,
                 TextBoxVariant variant, Glyph icon)
    {
        ItemAnim anim;
        const Theme& t = theme();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        float lh = draw::LineHeight();
        float capH = (caption && *caption && caption[0] != '#') ? lh + 3.0f : 0.0f;
        float h = t.FieldHeight > 0.0f ? t.Scale(t.FieldHeight) : lh + 8.0f;

        ImVec2 origin = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        ImVec2 mn(origin.x, origin.y + capH), mx(origin.x + w, origin.y + capH + h);

        float iconW = (icon != Glyph::None) ? GlyphSizeFor(lh) + 5.0f : 0.0f;

        // The focus ring reads *last* frame's active flag, because the frame has to
        // be painted before the input is submitted — both go to the same draw list,
        // and anything added afterwards covers the text ImGui just emitted. One
        // frame of lag on a fade nobody can see is the cheaper trade.
        char aid[96]; snprintf(aid, sizeof(aid), "%s##focus", caption ? caption : "tb");
        ImGuiID focusId = ImGui::GetID(aid);
        char sid[96]; snprintf(sid, sizeof(sid), "%s##active", caption ? caption : "tb");
        ImGuiID stateId = ImGui::GetID(sid);
        ImGuiStorage* store = ImGui::GetStateStorage();
        bool wasActive = store->GetBool(stateId, false);

        float focus = Anim(focusId, wasActive ? 1.0f : 0.0f, 18.0f);
        ImU32 border = LerpCol(t.Border, t.Accent, focus);

        if (capH > 0.0f) draw::Text(dl, origin, t.Text, caption);

        switch (variant)
        {
        case TextBoxVariant::Underline:
            draw::Line(dl, ImVec2(mn.x, mx.y - 1.0f), ImVec2(mx.x, mx.y - 1.0f), border);
            break;
        case TextBoxVariant::Filled:
            dl->AddRectFilled(mn, mx, A(t.Element), t.Radius);
            break;
        default:
            dl->AddRectFilled(mn, mx, A(t.Element), t.Radius);
            dl->AddRect(mn, mx, A(border), t.Radius, 0, 1.0f);
            break;
        }

        if (icon != Glyph::None)
        {
            float gs = GlyphSizeFor(lh);
            DrawGlyph(dl, icon, ImVec2(mn.x + 6.0f, mn.y + (h - gs) * 0.5f),
                      ImVec2(mn.x + 6.0f + gs, mn.y + (h + gs) * 0.5f), t.TextInactive);
        }

        ImGui::SetCursorScreenPos(mn);
        ImGui::PushID(caption ? caption : "##tb");
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(t.Text));
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImGui::ColorConvertU32ToFloat4(SetAlpha(t.TextInactive, 170)));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f + iconW, (h - lh) * 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);

        ImGui::SetNextItemWidth(w);
        bool changed = ImGui::InputTextWithHint("##in", placeholder ? placeholder : "", buf, bufSize);
        store->SetBool(stateId, ImGui::IsItemActive());

        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(3);
        ImGui::PopID();

        return changed;
    }

    // ================================================================= button
    bool Button(const char* label, ButtonVariant variant, ImVec2 size, Glyph icon, TextAlign align)
    {
        ItemAnim anim;
        const Theme& t = theme();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        float lh = draw::LineHeight();
        float h = size.y > 0.0f ? size.y : (t.FieldHeight > 0.0f ? t.Scale(t.FieldHeight) : lh + 8.0f);
        bool hovered = false, held = false, clicked = false;
        Area r = Reserve(size.x, h, label, &hovered, &held, &clicked);

        float hot = Anim(ImGui::GetID(label), hovered ? 1.0f : 0.0f, 16.0f);
        float press = held ? 1.0f : 0.0f;

        ImU32 baseFill, textCol;
        switch (variant)
        {
        case ButtonVariant::Primary: baseFill = t.Accent; textCol = t.AccentText; break;
        case ButtonVariant::Danger:  baseFill = t.Risky;  textCol = ReadableOn(t.Risky); break;
        case ButtonVariant::Outline:
        case ButtonVariant::Flat:    baseFill = 0;        textCol = t.Text; break;
        default:                     baseFill = t.Element; textCol = t.Text; break;
        }
        if (t.MutedFieldText && variant != ButtonVariant::Primary && variant != ButtonVariant::Danger)
            textCol = LerpCol(t.TextInactive, t.Text, hot);

        ImU32 fill;
        if (variant == ButtonVariant::Outline || variant == ButtonVariant::Flat)
            fill = SetAlpha(t.ElementHover, (int)(hot * 120));
        else
        {
            fill = LerpCol(baseFill, Lighten(baseFill, 0.14f), hot);
            fill = LerpCol(fill, Darken(fill, 0.18f), press);
        }
        if (variant == ButtonVariant::Primary && t.AccentGradient)
        {
            // The ramp replaces the flat fill, so hover and press shade on top of it
            // rather than being blended into the base colour.
            draw::AccentRect(dl, r.Min, r.Max, r.Min.x, r.Max.x);
            if (hot > 0.01f)   draw::Rect(dl, r.Min, r.Max, IM_COL32(255, 255, 255, (int)(30 * hot)));
            if (press > 0.01f) draw::Rect(dl, r.Min, r.Max, IM_COL32(0, 0, 0, (int)(45 * press)));
        }
        else draw::Rect(dl, r.Min, r.Max, fill);

        if (variant != ButtonVariant::Flat)
        {
            ImU32 bc = (variant == ButtonVariant::Primary) ? t.Accent
                     : (variant == ButtonVariant::Danger) ? t.Risky
                     : LerpCol(t.Border, t.Accent, hot * 0.55f);
            draw::Border(dl, r.Min, r.Max, bc);
        }

        // Accent wipe along the bottom edge, like the WinForms button.
        if (hot > 0.01f && (variant == ButtonVariant::Default || variant == ButtonVariant::Outline))
        {
            float w = (r.Max.x - r.Min.x) * EaseF(Ease::OutCubic, hot);
            float x = r.Min.x + ((r.Max.x - r.Min.x) - w) * 0.5f;
            draw::AccentRect(dl, ImVec2(x, r.Max.y - 1.0f), ImVec2(x + w, r.Max.y), r.Min.x, r.Max.x);
        }

        float iconSz = GlyphSizeFor(lh);
        bool hasIcon = icon != Glyph::None;
        float tw = draw::TextWidth(label);
        float totalW = tw + (hasIcon ? iconSz + 6.0f : 0.0f);
        float cx;
        switch (align)
        {
        case TextAlign::Left:  cx = r.Min.x + 8.0f; break;
        case TextAlign::Right: cx = r.Max.x - totalW - 8.0f; break;
        default:               cx = r.Min.x + ((r.Max.x - r.Min.x) - totalW) * 0.5f; break;
        }
        float cy = r.Min.y + (h - lh) * 0.5f + press;

        if (hasIcon)
        {
            DrawGlyph(dl, icon, ImVec2(cx, r.Min.y + (h - iconSz) * 0.5f + press),
                      ImVec2(cx + iconSz, r.Min.y + (h + iconSz) * 0.5f + press), textCol);
            cx += iconSz + 6.0f;
        }
        draw::Text(dl, ImVec2(cx, cy), textCol, label);
        return clicked;
    }

    // ================================================================= misc
    void RowDivider(float gap)
    {
        const Theme& t = theme();
        if (t.RowDivider <= 0.0f) return;
        ItemAnim anim;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        float y = p.y + gap;
        draw::Line(dl, ImVec2(p.x, y), ImVec2(p.x + w, y),
                   SetAlpha(t.Border, (int)(255 * Clamp01(t.RowDivider))));
        ImGui::Dummy(ImVec2(w, gap * 2.0f + 1.0f));
    }

    void Separator(const char* caption)
    {
        ItemAnim anim;
        const Theme& t = theme();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float lh = draw::LineHeight();
        float h = (caption && *caption) ? lh + 2.0f : 3.0f;
        Area r = Reserve(0.0f, h, caption ? caption : "##sep");
        float y = r.Min.y + h * 0.5f;

        if (!caption || !*caption)
        {
            draw::Line(dl, ImVec2(r.Min.x, y), ImVec2(r.Max.x, y), t.Border);
            return;
        }
        float cw = draw::TextWidth(caption) + 12.0f;
        draw::Line(dl, ImVec2(r.Min.x, y), ImVec2(r.Min.x + 6.0f, y), t.Border);
        draw::Text(dl, ImVec2(r.Min.x + 10.0f, r.Min.y), t.TextInactive, caption);
        draw::Line(dl, ImVec2(r.Min.x + cw, y), ImVec2(r.Max.x, y), t.Border);
    }

    void Label(const char* text, const char* text2)
    {
        ItemAnim anim;
        const Theme& t = theme();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float lh = draw::LineHeight();
        Area r = Reserve(0.0f, lh + 2.0f, text ? text : "##lbl");
        float x = r.Min.x;
        if (text && *text) { draw::Text(dl, ImVec2(x, r.Min.y), t.Text, text); x += draw::TextWidth(text); }
        if (text2 && *text2) draw::Text(dl, ImVec2(x, r.Min.y), t.Accent, text2);
    }

    void RainbowLabel(const char* text, float speed, bool wave, float amplitude)
    {
        ItemAnim anim;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float lh = draw::LineHeight();
        Area r = Reserve(0.0f, lh + 2.0f + (wave ? amplitude * 2.0f : 0.0f), text);
        float x = r.Min.x;
        for (int i = 0; text[i]; i++)
        {
            char ch[2] = { text[i], 0 };
            float y = r.Min.y + (wave ? sinf((float)Time() * 5.0f * speed + i * 0.55f) * amplitude : 0.0f);
            ImU32 col = FromHsv((float)fmod(Time() * 0.35 * speed + i * 0.055, 1.0), 0.62f, 1.0f);
            draw::Text(dl, ImVec2(x, y), col, ch);
            x += draw::TextWidth(ch);
        }
    }

    // ================================================================= HUD
    // Drawn on the foreground draw list — the ImGui equivalent of the WinForms
    // detached overlay windows. Never clipped by any ImGui window.
    static void HudBox(ImDrawList* dl, ImVec2 mn, ImVec2 mx, bool accentLine)
    {
        const Theme& t = theme();
        dl->AddRectFilled(mn, mx, t.Tab, t.Radius);
        dl->AddRect(mn, mx, t.Border, t.Radius, 0, 1.0f);
        if (accentLine) dl->AddRectFilled(mn, ImVec2(mx.x, mn.y + 1.0f), t.Accent);
    }

    static void HudText(ImDrawList* dl, ImVec2 p, ImU32 col, const char* s)
    {
        const Theme& t = theme();
        if (t.TextShadows) dl->AddText(ImVec2(p.x + 1, p.y + 1), t.TextShadow, s);
        dl->AddText(p, col, s);
    }

    void Watermark(const WatermarkSegment segments[], int count, ScreenAnchor anchor,
                   ImVec2 offset, const char* separator)
    {
        if (count <= 0) return;
        const Theme& t = theme();
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        float lh = draw::LineHeight();
        float padX = 8.0f, boxH = lh + 8.0f, gap = 4.0f;

        // measure
        float total = 0.0f, shared = 0.0f;
        bool inShared = false;
        for (int i = 0; i < count; i++)
        {
            if (segments[i].Boxed)
            {
                if (inShared) { total += shared + padX * 2 + gap; shared = 0; inShared = false; }
                total += draw::TextWidth(segments[i].Text) + padX * 2 + gap;
            }
            else
            {
                if (inShared) shared += draw::TextWidth(separator);
                shared += draw::TextWidth(segments[i].Text);
                inShared = true;
            }
        }
        if (inShared) total += shared + padX * 2 + gap;
        if (total > 0.0f) total -= gap;

        ImVec2 disp = ImGui::GetIO().DisplaySize;
        ImVec2 pos = Anchor(anchor, ImVec2(total, boxH), offset, ImVec2(0, 0), disp);

        float x = pos.x;
        int i = 0;
        while (i < count)
        {
            if (segments[i].Boxed)
            {
                float w = draw::TextWidth(segments[i].Text) + padX * 2;
                HudBox(dl, ImVec2(x, pos.y), ImVec2(x + w, pos.y + boxH), true);
                HudText(dl, ImVec2(x + padX, pos.y + (boxH - lh) * 0.5f),
                        segments[i].Accent ? t.Accent : t.Text, segments[i].Text);
                x += w + gap; i++;
                continue;
            }
            int start = i; float runW = 0.0f;
            while (i < count && !segments[i].Boxed)
            {
                if (i > start) runW += draw::TextWidth(separator);
                runW += draw::TextWidth(segments[i].Text);
                i++;
            }
            float boxW = runW + padX * 2;
            HudBox(dl, ImVec2(x, pos.y), ImVec2(x + boxW, pos.y + boxH), true);
            float tx = x + padX, ty = pos.y + (boxH - lh) * 0.5f;
            for (int k = start; k < i; k++)
            {
                if (k > start)
                {
                    HudText(dl, ImVec2(tx, ty), SetAlpha(t.TextInactive, 170), separator);
                    tx += draw::TextWidth(separator);
                }
                HudText(dl, ImVec2(tx, ty), segments[k].Accent ? t.Accent : t.Text, segments[k].Text);
                tx += draw::TextWidth(segments[k].Text);
            }
            x += boxW + gap;
        }
    }

    // ---- keybind panel animation state
    //
    // The panel is handed a fresh array every frame, so anything that animates has
    // to be remembered here and matched up by label. Rows keep their own text too,
    // because a row that has just been un-toggled is no longer in the caller's array
    // but still has to draw while it fades out.
    namespace
    {
        struct BindRow
        {
            unsigned int id = 0;
            char  label[64] = {};
            char  right[64] = {};
            float in = 0.0f;         // 0..1 presence
            float y = 0.0f;          // animated row index, so rows reflow smoothly
            float active = 0.0f;     // 0..1 highlight
            float targetY = 0.0f;
            bool  seen = false;
            bool  isActive = false;
        };

        struct BindPanel
        {
            unsigned int id = 0;
            ImVector<BindRow> rows;
            float w = 0.0f, h = 0.0f;    // animated panel size
            bool  sized = false;
        };

        ImVector<BindPanel> g_bindPanels;

        // Small FNV-1a. ImHashStr lives in imgui_internal.h, and ImGui::GetID needs a
        // current window — neither is available to a foreground-drawlist widget.
        unsigned int HashStr(const char* s)
        {
            unsigned int h = 2166136261u;
            for (; s && *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
            return h;
        }

        BindPanel* GetBindPanel(unsigned int id)
        {
            for (int i = 0; i < g_bindPanels.Size; i++)
                if (g_bindPanels[i].id == id) return &g_bindPanels[i];
            BindPanel p; p.id = id;
            g_bindPanels.push_back(p);
            return &g_bindPanels.back();
        }
    }

    void KeybindListPanel(const char* title, const KeybindEntry entries[], int count,
                          ScreenAnchor anchor, ImVec2 offset, bool showMode)
    {
        const Theme& t = theme();
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        float lh = draw::LineHeight();
        float rowH = lh + 6.0f;
        float headH = (title && *title) ? lh + 10.0f : 0.0f;

        float dt = detail::g_state.dt;
        BindPanel* panel = GetBindPanel(HashStr(title ? title : "##binds"));

        for (int i = 0; i < panel->rows.Size; i++) panel->rows[i].seen = false;

        // ---- reconcile this frame's entries against the remembered rows.
        // What gets listed, and it changes as you play:
        //   unbound        — never listed (unless the mode is Always)
        //   Toggle, off    — drops out of the list
        //   everything else— listed, highlighted while Active
        char right[64];
        float targetW = 130.0f;
        int visible = 0;

        for (int i = 0; i < count; i++)
        {
            const KeybindEntry& e = entries[i];
            if (!e.Label || !*e.Label) continue;
            if (e.Bind == 0 && e.Mode != KeybindMode::Always) continue;
            if (e.Mode == KeybindMode::Toggle && !e.Active) continue;

            const char* bn = (e.Mode == KeybindMode::Always) ? "ALWAYS" : KeybindName(e.Bind);
            if (showMode)
            {
                const char* m = e.Mode == KeybindMode::Hold ? "Hold"
                              : e.Mode == KeybindMode::Toggle ? "Toggle" : "Always";
                snprintf(right, sizeof(right), "[%s] %s", bn, m);
            }
            else snprintf(right, sizeof(right), "[%s]", bn);

            unsigned int id = HashStr(e.Label);
            BindRow* row = nullptr;
            for (int r = 0; r < panel->rows.Size; r++)
                if (panel->rows[r].id == id) { row = &panel->rows[r]; break; }

            if (!row)
            {
                BindRow nr;
                nr.id = id;
                nr.y = (float)visible;      // start in place, then fade/slide in
                panel->rows.push_back(nr);
                row = &panel->rows.back();
            }

            row->seen = true;
            row->isActive = e.Active;
            row->targetY = (float)visible++;
            snprintf(row->label, sizeof(row->label), "%s", e.Label);
            snprintf(row->right, sizeof(row->right), "%s", right);

            float need = draw::TextWidth(row->label) + draw::TextWidth(row->right) + 34.0f;
            if (need > targetW) targetW = need;
        }

        // ---- animate, and drop rows that have finished fading out
        for (int i = panel->rows.Size - 1; i >= 0; i--)
        {
            BindRow& r = panel->rows[i];
            float targetIn = r.seen ? 1.0f : 0.0f;
            r.in = Approach(r.in, targetIn, 14.0f, dt);
            r.y = Approach(r.y, r.targetY, 16.0f, dt);
            r.active = Approach(r.active, r.isActive ? 1.0f : 0.0f, 16.0f, dt);

            if (!r.seen && r.in < 0.01f) panel->rows.erase(panel->rows.begin() + i);
        }

        if (panel->rows.Size == 0) { panel->sized = false; return; }

        // ---- animate the panel box itself so it grows and shrinks smoothly
        float targetH = headH + visible * rowH + 6.0f;
        if (visible == 0) targetH = headH + rowH + 6.0f;
        if (!panel->sized) { panel->w = targetW; panel->h = targetH; panel->sized = true; }
        else
        {
            panel->w = Approach(panel->w, targetW, 16.0f, dt);
            panel->h = Approach(panel->h, targetH, 16.0f, dt);
        }
        float w = panel->w, h = panel->h;

        ImVec2 disp = ImGui::GetIO().DisplaySize;
        ImVec2 p = Anchor(anchor, ImVec2(w, h), offset, ImVec2(0, 0), disp);

        HudBox(dl, p, ImVec2(p.x + w, p.y + h), true);

        if (headH > 0.0f)
        {
            HudText(dl, ImVec2(p.x + 9.0f, p.y + 5.0f), t.Text, title);
            dl->AddLine(ImVec2(p.x + 6.0f, p.y + headH - 1.0f),
                        ImVec2(p.x + w - 6.0f, p.y + headH - 1.0f), t.Border);
        }

        // Rows are clipped to the body so one sliding out never spills past the frame.
        dl->PushClipRect(ImVec2(p.x + 1.0f, p.y + headH), ImVec2(p.x + w - 1.0f, p.y + h - 1.0f), true);

        for (int i = 0; i < panel->rows.Size; i++)
        {
            const BindRow& r = panel->rows[i];
            float eased = EaseF(Ease::OutCubic, r.in);
            int alpha = (int)(255.0f * eased);
            if (alpha <= 2) continue;

            // Slide in from the left as it appears, back out as it goes.
            float slide = (1.0f - eased) * 12.0f;
            float y = p.y + headH + 3.0f + r.y * rowH;

            ImU32 label = LerpCol(t.Text, t.Accent, r.active);
            ImU32 bind = LerpCol(t.TextInactive, t.Accent, r.active);

            HudText(dl, ImVec2(p.x + 9.0f - slide, y), SetAlpha(label, alpha), r.label);
            HudText(dl, ImVec2(p.x + w - 9.0f - draw::TextWidth(r.right) - slide, y),
                    SetAlpha(bind, alpha), r.right);

            // Accent tick on the leading edge, growing with the active amount.
            if (r.active > 0.01f)
            {
                float tick = (lh + 2.0f) * EaseF(Ease::OutCubic, r.active);
                float cy = y + lh * 0.5f;
                dl->AddRectFilled(ImVec2(p.x + 1.0f, cy - tick * 0.5f),
                                  ImVec2(p.x + 3.0f, cy + tick * 0.5f),
                                  SetAlpha(t.Accent, alpha));
            }
        }

        dl->PopClipRect();
    }

    // ---- notifications
    struct Toast
    {
        char  title[64];
        char  body[160];
        NotifyKind kind;
        float life;
        float in;
        bool  started;    // life is only seeded once; it legitimately goes negative
                          // while fading out, so a negative sentinel can't be used
    };
    static ImVector<Toast> g_toasts;

    void Notify(const char* title, const char* body, NotifyKind kind)
    {
        Toast t;
        snprintf(t.title, sizeof(t.title), "%s", title ? title : "");
        snprintf(t.body, sizeof(t.body), "%s", body ? body : "");
        t.kind = kind;
        t.life = 0.0f;
        t.started = false;   // seeded on first render, so Duration can change freely
        t.in = 0.0f;
        g_toasts.push_back(t);
        while (g_toasts.Size > 5) g_toasts.erase(g_toasts.begin());
    }

    void RenderNotifications(ScreenAnchor anchor, ImVec2 offset, float durationSec)
    {
        if (g_toasts.Size == 0) return;
        const Theme& t = theme();
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        float lh = draw::LineHeight();
        float boxH = lh + 12.0f, gap = 5.0f;

        // measure the stack
        float w = 0.0f;
        for (int i = 0; i < g_toasts.Size; i++)
        {
            float tw = draw::TextWidth(g_toasts[i].title) + (g_toasts[i].title[0] ? 4.0f : 0.0f)
                     + draw::TextWidth(g_toasts[i].body) + GlyphSizeFor(lh) + 33.0f;
            if (tw > w) w = tw;
        }
        w = ClampF(w, 110.0f, 320.0f);
        float h = g_toasts.Size * (boxH + gap) - gap;

        ImVec2 disp = ImGui::GetIO().DisplaySize;
        ImVec2 p = Anchor(anchor, ImVec2(w, h), offset, ImVec2(0, 0), disp);

        for (int i = 0; i < g_toasts.Size; )
        {
            Toast& to = g_toasts[i];
            if (!to.started) { to.started = true; to.life = durationSec; }
            to.in = Approach(to.in, 1.0f, 14.0f, g_state.dt);
            to.life -= g_state.dt;

            float alpha = EaseF(Ease::OutCubic, to.in);
            if (to.life <= 0.0f) alpha *= VMax(0.0f, 1.0f + to.life * 6.0f);   // fade out
            if (to.life <= -0.35f) { g_toasts.erase(g_toasts.begin() + i); continue; }

            int a = (int)(255 * Clamp01(alpha));
            float rise = (1.0f - EaseF(Ease::OutCubic, to.in)) * 10.0f;
            ImVec2 mn(p.x, p.y + i * (boxH + gap) + rise);
            ImVec2 mx(mn.x + w, mn.y + boxH);

            ImU32 accent = to.kind == NotifyKind::Success ? t.Positive
                         : to.kind == NotifyKind::Warning ? t.Warning
                         : to.kind == NotifyKind::Error ? t.Risky : t.Accent;

            dl->AddRectFilled(mn, mx, SetAlpha(t.Tab, a), t.Radius);
            dl->AddRect(mn, mx, SetAlpha(t.Border, a), t.Radius, 0, 1.0f);
            dl->AddRectFilled(mn, ImVec2(mn.x + 2.0f, mx.y), SetAlpha(accent, a));

            float s = GlyphSizeFor(lh);
            Glyph ic = to.kind == NotifyKind::Success ? Glyph::Check
                     : to.kind == NotifyKind::Warning ? Glyph::Warning
                     : to.kind == NotifyKind::Error ? Glyph::Cross : Glyph::Info;
            DrawGlyph(dl, ic, ImVec2(mn.x + 9.0f, mn.y + (boxH - s) * 0.5f),
                      ImVec2(mn.x + 9.0f + s, mn.y + (boxH + s) * 0.5f), SetAlpha(accent, a));

            float tx = mn.x + 9.0f + s + 7.0f, ty = mn.y + (boxH - lh) * 0.5f;
            if (to.title[0])
            {
                dl->AddText(ImVec2(tx, ty), SetAlpha(accent, a), to.title);
                tx += draw::TextWidth(to.title) + 4.0f;
            }
            dl->AddText(ImVec2(tx, ty), SetAlpha(t.Text, a), to.body);

            if (to.life > 0.0f)
            {
                float pr = Clamp01(to.life / VMax(0.001f, durationSec));
                dl->AddRectFilled(ImVec2(mn.x, mx.y - 1.0f), ImVec2(mn.x + w * pr, mx.y),
                                  SetAlpha(accent, a / 2));
            }
            i++;
        }
    }
} // namespace velo
