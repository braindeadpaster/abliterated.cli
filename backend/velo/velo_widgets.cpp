// velo_widgets.cpp — lists, config bar, setting rows, module cards, tooltips, and
// the domain widgets (ESP preview, bone picker, curve view, varying bar).
//
// Split out from velo_controls.cpp purely to keep both files readable; there is no
// architectural line between them.

#include "velo.h"
#include "velo_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace velo
{
    using namespace detail;

    // Reserve a rect on the current line and report its interaction state.
    static Area Hit(float w, float h, const char* id,
                    bool* hovered = nullptr, bool* held = nullptr, bool* clicked = nullptr)
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        if (w <= 0.0f) w = ImGui::GetContentRegionAvail().x;
        ImGui::InvisibleButton(id, ImVec2(VMax(1.0f, w), VMax(1.0f, h)));
        if (hovered) *hovered = ImGui::IsItemHovered();
        if (held)    *held = ImGui::IsItemActive();
        if (clicked) *clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        return Area(p, ImVec2(p.x + w, p.y + h));
    }

    // ===================================================================== lists
    // One scrolling list implementation behind both ListBox and PlayerList.
    struct ListRowDraw
    {
        const char* Left;
        const char* Right;        // status word, or null
        ImU32 RightColor;
    };

    static bool ListCore(const char* id, int count, int* selected, int visibleRows,
                         bool checked[], const ListRowDraw* (*fetch)(int, void*), void* user)
    {
        ItemAnim anim;
        const Theme& t = theme();
        float lh = draw::LineHeight();
        float rowH = lh + 7.0f;
        float h = visibleRows * rowH + 2.0f;

        ImGui::PushID(id);
        ImVec2 origin = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;

        ImDrawList* dl = ImGui::GetWindowDrawList();
        draw::Box(dl, origin, ImVec2(origin.x + w, origin.y + h), t.Element, t.Border);

        // A child gives clipping and a scrollbar for free.
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::BeginChild("##list", ImVec2(w - 2.0f, h - 2.0f), false,
                          ImGuiWindowFlags_NoBackground);

        ImDrawList* cdl = ImGui::GetWindowDrawList();
        bool changed = false;
        float cw = ImGui::GetContentRegionAvail().x;

        for (int i = 0; i < count; i++)
        {
            const ListRowDraw* row = fetch(i, user);
            if (!row) continue;

            ImGui::PushID(i);
            ImVec2 rmn = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##r", ImVec2(cw, rowH));
            bool hov = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
            {
                if (checked) { checked[i] = !checked[i]; changed = true; }
                else if (selected && *selected != i) { *selected = i; changed = true; }
            }
            ImVec2 rmx(rmn.x + cw, rmn.y + rowH);

            bool sel = checked ? checked[i] : (selected && *selected == i);
            float hot = Anim(ImGui::GetID("##h"), hov ? 1.0f : 0.0f, 20.0f);
            float on = Anim(ImGui::GetID("##s"), sel ? 1.0f : 0.0f, 18.0f);

            ImU32 fill = LerpCol(IM_COL32(0, 0, 0, 0), t.ElementHover, hot);
            fill = LerpCol(fill, SetAlpha(t.Accent, 34), on * 0.9f);
            if ((fill & IM_COL32_A_MASK) != 0) cdl->AddRectFilled(rmn, rmx, A(fill));

            if (on > 0.01f)
            {
                float barH = rowH * EaseF(Ease::OutCubic, on);
                float cy = (rmn.y + rmx.y) * 0.5f;
                cdl->AddRectFilled(ImVec2(rmn.x, cy - barH * 0.5f),
                                   ImVec2(rmn.x + 2.0f, cy + barH * 0.5f), A(t.Accent));
            }

            float tx = rmn.x + 8.0f;
            if (checked)
            {
                float bs = VMax(9.0f, lh - 3.0f);
                ImVec2 cmn(tx, rmn.y + (rowH - bs) * 0.5f);
                ImVec2 cmx(cmn.x + bs, cmn.y + bs);
                cdl->AddRectFilled(cmn, cmx, A(LerpCol(t.Content, t.Accent, on)));
                cdl->AddRect(cmn, cmx, A(LerpCol(t.Border, t.Accent, on)), 0.0f, 0, 1.0f);
                tx += bs + 7.0f;
            }

            float ty = rmn.y + (rowH - lh) * 0.5f;
            ImU32 leftCol = LerpCol(LerpCol(t.TextInactive, t.Text, hot), t.Text, on);
            draw::Text(cdl, ImVec2(tx, ty), leftCol, row->Left);

            if (row->Right && *row->Right)
            {
                float rw = draw::TextWidth(row->Right);
                draw::Text(cdl, ImVec2(rmx.x - rw - 8.0f, ty), row->RightColor, row->Right);
            }

            ImGui::PopID();
        }

        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        ImGui::PopID();
        return changed;
    }

    namespace
    {
        struct PlainList { const char* const* items; };
        const ListRowDraw* FetchPlain(int i, void* user)
        {
            static ListRowDraw row;
            PlainList* p = (PlainList*)user;
            row.Left = p->items[i];
            row.Right = nullptr;
            row.RightColor = 0;
            return &row;
        }

        struct PlayerListData { const PlayerRow* rows; };
        const ListRowDraw* FetchPlayer(int i, void* user)
        {
            static ListRowDraw row;
            PlayerListData* p = (PlayerListData*)user;
            row.Left = p->rows[i].Name;
            row.Right = p->rows[i].Status;
            row.RightColor = p->rows[i].StatusColor;
            return &row;
        }
    }

    bool ListBox(const char* id, const char* const items[], int count, int* selected,
                 int visibleRows, bool checked[])
    {
        PlainList data{ items };
        return ListCore(id, count, selected, visibleRows, checked, FetchPlain, &data);
    }

    bool PlayerList(const char* id, const PlayerRow rows[], int count, int* selected,
                    int visibleRows)
    {
        PlayerListData data{ rows };
        return ListCore(id, count, selected, visibleRows, nullptr, FetchPlayer, &data);
    }

    // ===================================================================== config bar
    ConfigAction ConfigBar(const char* id, const char* const configs[], int count, int* selected)
    {
        const Theme& t = theme();
        ConfigAction action = ConfigAction::None;

        ImGui::PushID(id);
        ListBox("##list", configs, count, selected, 5);

        // Word buttons rather than filled ones — this strip reads as a toolbar.
        static const char* words[] = { "load", "save", "create", "delete" };
        static const ConfigAction acts[] = { ConfigAction::Load, ConfigAction::Save,
                                             ConfigAction::Create, ConfigAction::Delete };

        ItemAnim anim;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float lh = draw::LineHeight();
        float h = lh + 8.0f;
        float w = ImGui::GetContentRegionAvail().x;
        ImVec2 origin = ImGui::GetCursorScreenPos();

        float each = w / 4.0f;
        for (int i = 0; i < 4; i++)
        {
            ImGui::PushID(i);
            ImVec2 mn(origin.x + i * each, origin.y);
            ImGui::SetCursorScreenPos(mn);
            ImGui::InvisibleButton("##w", ImVec2(each, h));
            bool hov = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) action = acts[i];

            float hot = Anim(ImGui::GetID("##h"), hov ? 1.0f : 0.0f, 18.0f);
            ImVec2 mx(mn.x + each, mn.y + h);

            ImU32 col = LerpCol(t.TextInactive,
                                acts[i] == ConfigAction::Delete ? t.Risky : t.Accent, hot);
            float tw = draw::TextWidth(words[i]);
            draw::Text(dl, ImVec2(mn.x + (each - tw) * 0.5f, mn.y + (h - lh) * 0.5f), col, words[i]);

            // underline wipes in from the centre on hover
            if (hot > 0.01f)
            {
                float uw = tw * EaseF(Ease::OutCubic, hot);
                float ux = mn.x + (each - uw) * 0.5f;
                draw::Rect(dl, ImVec2(ux, mx.y - 2.0f), ImVec2(ux + uw, mx.y - 1.0f), col);
            }
            ImGui::PopID();
        }

        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + h));
        ImGui::Dummy(ImVec2(w, 0.0f));
        ImGui::PopID();
        return action;
    }

    // ===================================================================== setting row
    bool SettingRow(const char* title, const char* description, RowItem items[], int count)
    {
        ItemAnim anim;
        const Theme& t = theme();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        float lh = draw::LineHeight();
        bool hasDesc = description && *description;
        float h = hasDesc ? lh * 2.0f + 10.0f : VMax(RowH(), lh + 6.0f);

        ImGui::PushID(title);
        bool rowHov = false;
        Area r = Hit(0.0f, h, "##row", &rowHov);

        float hot = Anim(ImGui::GetID("##h"), rowHov ? 1.0f : 0.0f, 18.0f);
        if (hot > 0.01f)
            draw::Rect(dl, r.Min, r.Max, SetAlpha(t.ElementHover, (int)(hot * 70)));

        // ---- accessories, right to left
        float x = r.Max.x - 6.0f;
        bool changed = false;
        float boxSz = VMax(9.0f, lh - 3.0f);

        for (int i = count - 1; i >= 0; i--)
        {
            RowItem& it = items[i];
            it.Clicked = false;
            ImGui::PushID(i);

            float aw = 0.0f;
            switch (it.Kind)
            {
            case RowKind::Toggle:   aw = boxSz * 2.0f + 2.0f; break;
            case RowKind::Badge:    aw = draw::TextWidth(it.Text ? it.Text : "") + 10.0f; break;
            case RowKind::Keybind:  aw = VMax(26.0f, draw::TextWidth(KeybindName(it.Bind ? *it.Bind : 0)) + 12.0f); break;
            default:                aw = boxSz + 2.0f; break;
            }

            ImVec2 amn(x - aw, r.Min.y + (h - boxSz) * 0.5f);
            ImVec2 amx(x, amn.y + boxSz);
            if (it.Kind == RowKind::Toggle) { amn.y = r.Min.y + (h - boxSz) * 0.5f; amx.y = amn.y + boxSz; }

            ImGui::SetCursorScreenPos(ImVec2(amn.x, r.Min.y));
            ImGui::InvisibleButton("##a", ImVec2(aw, h));
            bool aHov = ImGui::IsItemHovered();
            bool aClick = ImGui::IsItemClicked(ImGuiMouseButton_Left);

            if (aClick)
            {
                it.Clicked = true;
                changed = true;
                if ((it.Kind == RowKind::Checkbox || it.Kind == RowKind::Toggle ||
                     it.Kind == RowKind::Expander) && it.Value)
                    *it.Value = !*it.Value;
            }

            bool on = it.Value && *it.Value;
            float onT = Anim(ImGui::GetID("##on"), on ? 1.0f : 0.0f, 20.0f);
            float aHot = Anim(ImGui::GetID("##ah"), aHov ? 1.0f : 0.0f, 18.0f);

            switch (it.Kind)
            {
            case RowKind::Checkbox:
                draw::Box(dl, amn, amx, LerpCol(LerpCol(t.Element, t.ElementHover, aHot), t.Accent, onT),
                          LerpCol(t.Border, t.Accent, onT));
                break;

            case RowKind::Toggle:
            {
                ImVec2 tmn(x - aw, amn.y), tmx(x, amn.y + boxSz);
                draw::Rect(dl, tmn, tmx, LerpCol(t.Element, t.Accent, onT));
                draw::Border(dl, tmn, tmx, LerpCol(t.Border, t.Accent, onT));
                float knob = boxSz - 4.0f;
                float kx = tmn.x + 2.0f + onT * ((tmx.x - tmn.x) - knob - 4.0f);
                draw::Rect(dl, ImVec2(kx, tmn.y + 2.0f), ImVec2(kx + knob, tmx.y - 2.0f), t.Text);
                break;
            }

            case RowKind::Swatch:
                if (it.Color)
                {
                    draw::Rect(dl, amn, amx, *it.Color);
                    draw::Border(dl, amn, amx, aHot > 0.5f ? t.Accent : t.Border);
                }
                break;

            case RowKind::Badge:
            {
                ImVec2 bmn(x - aw, r.Min.y + (h - lh - 2.0f) * 0.5f);
                ImVec2 bmx(x, bmn.y + lh + 2.0f);
                draw::Box(dl, bmn, bmx, t.Element, t.Border);
                float tw = draw::TextWidth(it.Text ? it.Text : "");
                draw::Text(dl, ImVec2((bmn.x + bmx.x - tw) * 0.5f, bmn.y + 1.0f), t.Accent,
                           it.Text ? it.Text : "");
                break;
            }

            case RowKind::Gear:
                DrawGlyph(dl, Glyph::Gear, amn, amx, LerpCol(t.TextInactive, t.Accent, aHot));
                break;

            case RowKind::Keybind:
            {
                ImVec2 kmn(x - aw, r.Min.y + (h - lh - 4.0f) * 0.5f);
                ImVec2 kmx(x, kmn.y + lh + 4.0f);
                draw::Box(dl, kmn, kmx, LerpCol(t.Element, t.ElementHover, aHot), t.Border);
                const char* nm = KeybindName(it.Bind ? *it.Bind : 0);
                float tw = draw::TextWidth(nm);
                draw::Text(dl, ImVec2((kmn.x + kmx.x - tw) * 0.5f, kmn.y + 2.0f), t.Text, nm);
                break;
            }

            case RowKind::Expander:
                draw::Chevron(dl, amn, amx, LerpCol(t.TextInactive, t.Accent, aHot), on ? 1 : 0);
                break;
            }

            x -= aw + 6.0f;
            ImGui::PopID();
        }

        // ---- title and description
        float tx = r.Min.x + 2.0f;
        float avail = VMax(0.0f, x - tx);
        if (hasDesc)
        {
            float top = r.Min.y + (h - lh * 2.0f) * 0.5f;
            draw::TextClipped(dl, ImVec2(tx, top), avail, LerpCol(t.Text, Lighten(t.Text, 0.2f), hot), title);
            draw::TextClipped(dl, ImVec2(tx, top + lh), avail, t.TextInactive, description);
        }
        else
        {
            draw::TextClipped(dl, ImVec2(tx, r.Min.y + (h - lh) * 0.5f), avail,
                              LerpCol(t.Text, Lighten(t.Text, 0.2f), hot), title);
        }

        ImGui::SetCursorScreenPos(ImVec2(r.Min.x, r.Max.y));
        ImGui::Dummy(ImVec2(r.Max.x - r.Min.x, 0.0f));
        ImGui::PopID();
        return changed;
    }

    // ===================================================================== module card
    namespace
    {
        struct ModuleCardState { float savedAlpha; bool pushedAlpha; };
        ImVector<ModuleCardState> g_moduleCards;
    }

    bool BeginModuleCard(const char* title, bool* enabled, ImVec2 size, int* bind)
    {
        const Theme& t = theme();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float lh = draw::LineHeight();
        float headH = lh + 10.0f;

        if (size.x <= 0.0f) size.x = ImGui::GetContentRegionAvail().x;
        if (size.y <= 0.0f) size.y = headH + 100.0f;

        ImGui::PushID(title);
        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImVec2 mn = origin, mx(origin.x + size.x, origin.y + size.y);

        draw::Rect(dl, mn, mx, t.Section);
        draw::Border(dl, mn, mx, t.Border);

        // header doubles as the master toggle
        ImGui::SetCursorScreenPos(mn);
        ImGui::InvisibleButton("##head", ImVec2(size.x, headH));
        bool hov = ImGui::IsItemHovered();
        bool changed = false;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && enabled) { *enabled = !*enabled; changed = true; }

        bool on = enabled && *enabled;
        float onT = Anim(ImGui::GetID("##on"), on ? 1.0f : 0.0f, 18.0f);
        float hot = Anim(ImGui::GetID("##h"), hov ? 1.0f : 0.0f, 18.0f);

        ImVec2 hmx(mx.x, mn.y + headH);
        draw::Rect(dl, mn, hmx, LerpCol(t.Element, t.ElementHover, hot * 0.7f));
        draw::Rect(dl, mn, ImVec2(mn.x + 2.0f, hmx.y), LerpCol(t.Border, t.Accent, onT));
        draw::Line(dl, ImVec2(mn.x, hmx.y - 1.0f), ImVec2(mx.x, hmx.y - 1.0f), t.Border);

        float tx = mn.x + 10.0f;
        draw::Text(dl, ImVec2(tx, mn.y + (headH - lh) * 0.5f),
                   LerpCol(t.TextInactive, t.Text, onT), title);

        float rx = mx.x - 8.0f;
        float boxSz = VMax(9.0f, lh - 3.0f);
        ImVec2 bmn(rx - boxSz, mn.y + (headH - boxSz) * 0.5f);
        draw::Box(dl, bmn, ImVec2(rx, bmn.y + boxSz),
                  LerpCol(t.Element, t.Accent, onT), LerpCol(t.Border, t.Accent, onT));
        rx -= boxSz + 8.0f;

        if (bind)
        {
            const char* nm = KeybindName(*bind);
            float bw = draw::TextWidth(nm) + 12.0f;
            ImVec2 kmn(rx - bw, mn.y + (headH - lh - 4.0f) * 0.5f);
            ImVec2 kmx(rx, kmn.y + lh + 4.0f);
            draw::Box(dl, kmn, kmx, t.Content, t.Border);
            draw::Text(dl, ImVec2(kmn.x + 6.0f, kmn.y + 2.0f), t.TextInactive, nm);
        }

        // The body dims when the module is off — still readable, clearly inactive.
        ModuleCardState st;
        st.savedAlpha = g_state.alphaMul;
        g_state.alphaMul = st.savedAlpha * (0.4f + 0.6f * onT);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * (0.4f + 0.6f * onT));
        st.pushedAlpha = true;
        g_moduleCards.push_back(st);

        float pad = t.InnerPadding;
        ImGui::SetCursorScreenPos(ImVec2(mn.x + pad, mn.y + headH + pad));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
        ImGui::BeginChild("##body", ImVec2(size.x - pad * 2.0f, size.y - headH - pad * 2.0f),
                          false, ImGuiWindowFlags_NoBackground);
        (void)changed;
        return true;
    }

    void EndModuleCard()
    {
        ImGui::EndChild();
        ImGui::PopStyleColor();

        if (g_moduleCards.Size > 0)
        {
            ModuleCardState st = g_moduleCards.back();
            g_moduleCards.pop_back();
            if (st.pushedAlpha) ImGui::PopStyleVar();
            g_state.alphaMul = st.savedAlpha;
        }
        ImGui::PopID();
    }

    // ===================================================================== tooltip
    void Tooltip(const char* text, float delaySec)
    {
        if (!text || !*text) return;
        if (!ImGui::IsItemHovered()) return;

        const Theme& t = theme();
        // Dwell before showing, then fade in — a tooltip that snaps feels twitchy.
        ImGuiID id = ImGui::GetItemID();
        ImGuiStorage* st = ImGui::GetStateStorage();
        float dwell = st->GetFloat(id, 0.0f) + g_state.dt;
        st->SetFloat(id, dwell);
        if (dwell < delaySec) return;

        float k = Clamp01((dwell - delaySec) / 0.12f);
        int alpha = (int)(255 * EaseF(Ease::OutCubic, k));

        float lh = draw::LineHeight();
        float tw = draw::TextWidth(text);
        ImVec2 m = ImGui::GetIO().MousePos;
        ImVec2 mn(m.x + 14.0f, m.y + 16.0f);
        ImVec2 mx(mn.x + tw + 14.0f, mn.y + lh + 8.0f);

        ImVec2 disp = ImGui::GetIO().DisplaySize;
        if (mx.x > disp.x) { float d = mx.x - disp.x; mn.x -= d; mx.x -= d; }
        if (mx.y > disp.y) { float d = (mx.y - mn.y) + 24.0f; mn.y -= d; mx.y -= d; }

        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddRectFilled(mn, mx, SetAlpha(t.Tab, alpha), t.Radius);
        dl->AddRect(mn, mx, SetAlpha(t.Border, alpha), t.Radius, 0, 1.0f);
        dl->AddRectFilled(mn, ImVec2(mn.x + 2.0f, mx.y), SetAlpha(t.Accent, alpha));
        if (t.TextShadows)
            dl->AddText(ImVec2(mn.x + 9.0f, mn.y + 5.0f), SetAlpha(t.TextShadow, alpha), text);
        dl->AddText(ImVec2(mn.x + 8.0f, mn.y + 4.0f), SetAlpha(t.Text, alpha), text);
    }

    // ===================================================================== esp preview
    void EspPreview(const char* id, ImVec2 size, const EspPreviewStyle& s)
    {
        ItemAnim anim;
        const Theme& t = theme();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        if (size.x <= 0.0f) size.x = ImGui::GetContentRegionAvail().x;
        if (size.y <= 0.0f) size.y = 200.0f;

        ImGui::PushID(id);
        Area r = Hit(size.x, size.y, "##esp");
        draw::Box(dl, r.Min, r.Max, t.Content, t.Border);

        // Faint grid so the panel reads as a viewport rather than an empty box.
        for (float gx = r.Min.x + 24.0f; gx < r.Max.x; gx += 24.0f)
            dl->AddLine(ImVec2(gx, r.Min.y + 1.0f), ImVec2(gx, r.Max.y - 1.0f), A(SetAlpha(t.Border, 60)));
        for (float gy = r.Min.y + 24.0f; gy < r.Max.y; gy += 24.0f)
            dl->AddLine(ImVec2(r.Min.x + 1.0f, gy), ImVec2(r.Max.x - 1.0f, gy), A(SetAlpha(t.Border, 60)));

        float w = r.Max.x - r.Min.x, h = r.Max.y - r.Min.y;
        float inset = 14.0f;

        // ---- the subject: letterbox the render, or fall back to a silhouette
        ImVec2 imn, imx;
        if (s.Texture != 0 && s.TextureSize.x > 0.0f && s.TextureSize.y > 0.0f)
        {
            float availW = w - inset * 2.0f, availH = h - inset * 2.0f;
            float scale = VMin(availW / s.TextureSize.x, availH / s.TextureSize.y);
            float dw = s.TextureSize.x * scale, dh = s.TextureSize.y * scale;
            imn = ImVec2(r.Min.x + (w - dw) * 0.5f, r.Min.y + (h - dh) * 0.5f);
            imx = ImVec2(imn.x + dw, imn.y + dh);
            dl->AddImage(s.Texture, imn, imx, ImVec2(0, 0), ImVec2(1, 1),
                         A(IM_COL32_WHITE));
        }
        else
        {
            float bw = h * 0.30f, bh = h * 0.62f;
            imn = ImVec2(r.Min.x + (w - bw) * 0.5f, r.Min.y + h * 0.19f);
            imx = ImVec2(imn.x + bw, imn.y + bh);
            ImU32 body = A(SetAlpha(t.TextInactive, 60));
            dl->AddRectFilled(ImVec2(imn.x + bw * 0.28f, imn.y), ImVec2(imx.x - bw * 0.28f, imn.y + bh * 0.18f), body);
            dl->AddRectFilled(ImVec2(imn.x + bw * 0.12f, imn.y + bh * 0.20f), ImVec2(imx.x - bw * 0.12f, imn.y + bh * 0.62f), body);
            dl->AddRectFilled(ImVec2(imn.x + bw * 0.22f, imn.y + bh * 0.64f), ImVec2(imx.x - bw * 0.22f, imx.y), body);
        }

        // ---- the ESP box, inset to roughly hug the figure inside the image
        float iw = imx.x - imn.x, ih = imx.y - imn.y;
        ImVec2 bmn(imn.x + iw * s.BoxPadX, imn.y + ih * s.BoxPadY);
        ImVec2 bmx(imx.x - iw * s.BoxPadX, imx.y - ih * s.BoxPadY);

        if (s.Skeleton)
        {
            // Rough humanoid rig scaled into the box — enough to read as a skeleton.
            ImU32 sk = A(s.SkeletonColor);
            float bw2 = bmx.x - bmn.x, bh2 = bmx.y - bmn.y;
            float cx = (bmn.x + bmx.x) * 0.5f;
            float head = bmn.y + bh2 * 0.07f;
            float neck = bmn.y + bh2 * 0.14f;
            float hip = bmn.y + bh2 * 0.52f;
            dl->AddCircle(ImVec2(cx, head), bh2 * 0.045f, sk, 0, 1.0f);
            dl->AddLine(ImVec2(cx, neck), ImVec2(cx, hip), sk, 1.0f);
            dl->AddLine(ImVec2(cx - bw2 * 0.28f, neck + bh2 * 0.05f), ImVec2(cx + bw2 * 0.28f, neck + bh2 * 0.05f), sk, 1.0f);
            dl->AddLine(ImVec2(cx - bw2 * 0.28f, neck + bh2 * 0.05f), ImVec2(cx - bw2 * 0.34f, hip - bh2 * 0.04f), sk, 1.0f);
            dl->AddLine(ImVec2(cx + bw2 * 0.28f, neck + bh2 * 0.05f), ImVec2(cx + bw2 * 0.34f, hip - bh2 * 0.04f), sk, 1.0f);
            dl->AddLine(ImVec2(cx, hip), ImVec2(cx - bw2 * 0.20f, bmx.y - bh2 * 0.02f), sk, 1.0f);
            dl->AddLine(ImVec2(cx, hip), ImVec2(cx + bw2 * 0.20f, bmx.y - bh2 * 0.02f), sk, 1.0f);
        }

        if (s.Box)
        {
            ImU32 box = A(s.BoxColor);
            float corner = VMin(bmx.x - bmn.x, bmx.y - bmn.y) * 0.24f;
            if (s.BoxStyle == 0)
            {
                dl->AddRect(bmn, bmx, box, 0.0f, 0, 1.0f);
            }
            else if (s.BoxStyle == 1)
            {
                dl->AddLine(bmn, ImVec2(bmn.x + corner, bmn.y), box);
                dl->AddLine(bmn, ImVec2(bmn.x, bmn.y + corner), box);
                dl->AddLine(ImVec2(bmx.x, bmn.y), ImVec2(bmx.x - corner, bmn.y), box);
                dl->AddLine(ImVec2(bmx.x, bmn.y), ImVec2(bmx.x, bmn.y + corner), box);
                dl->AddLine(ImVec2(bmn.x, bmx.y), ImVec2(bmn.x + corner, bmx.y), box);
                dl->AddLine(ImVec2(bmn.x, bmx.y), ImVec2(bmn.x, bmx.y - corner), box);
                dl->AddLine(bmx, ImVec2(bmx.x - corner, bmx.y), box);
                dl->AddLine(bmx, ImVec2(bmx.x, bmx.y - corner), box);
            }
            else
            {
                dl->AddLine(bmn, ImVec2(bmn.x, bmx.y), box);
                dl->AddLine(ImVec2(bmx.x, bmn.y), bmx, box);
            }
        }

        float lh = draw::LineHeight();

        if (s.Health)
        {
            float hx = bmn.x - 6.0f;
            dl->AddRectFilled(ImVec2(hx - 3.0f, bmn.y), ImVec2(hx, bmx.y), A(IM_COL32(0, 0, 0, 170)));
            dl->AddRect(ImVec2(hx - 3.0f, bmn.y), ImVec2(hx, bmx.y), A(SetAlpha(IM_COL32_BLACK, 200)), 0.0f, 0, 1.0f);
            float frac = Clamp01(s.HealthFraction);
            float hh = (bmx.y - bmn.y) * frac;
            // green at full, red as it drops — the usual read
            ImU32 hc = LerpCol(t.Risky, s.HealthColor, frac);
            dl->AddRectFilled(ImVec2(hx - 3.0f, bmx.y - hh), ImVec2(hx, bmx.y), A(hc));
        }

        if (s.Name && s.NameText && *s.NameText)
        {
            float tw = draw::TextWidth(s.NameText);
            draw::Text(dl, ImVec2((bmn.x + bmx.x - tw) * 0.5f, bmn.y - lh - 3.0f),
                       s.NameColor, s.NameText);
        }

        if (s.Weapon && s.WeaponText && *s.WeaponText)
        {
            float tw = draw::TextWidth(s.WeaponText);
            draw::Text(dl, ImVec2((bmn.x + bmx.x - tw) * 0.5f, bmx.y + 3.0f),
                       t.TextInactive, s.WeaponText);
        }

        if (s.Distance && s.DistanceText && *s.DistanceText)
        {
            float tw = draw::TextWidth(s.DistanceText);
            float y = s.Weapon ? bmx.y + 3.0f + lh : bmx.y + 3.0f;
            draw::Text(dl, ImVec2((bmn.x + bmx.x - tw) * 0.5f, y), t.TextInactive, s.DistanceText);
        }

        ImGui::PopID();
    }

    // ===================================================================== bone picker
    bool BonePicker(const char* id, unsigned int* mask, ImVec2 size)
    {
        ItemAnim anim;
        const Theme& t = theme();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        if (size.x <= 0.0f) size.x = ImGui::GetContentRegionAvail().x;
        if (size.y <= 0.0f) size.y = 150.0f;

        ImGui::PushID(id);
        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##bp", size);
        bool hov = ImGui::IsItemHovered();
        bool click = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        ImVec2 m = ImGui::GetIO().MousePos;

        Area r(origin, ImVec2(origin.x + size.x, origin.y + size.y));
        draw::Box(dl, r.Min, r.Max, t.Content, t.Border);

        float w = size.x, h = size.y;
        float cx = r.Min.x + w * 0.5f;

        // Hit zones over a simple humanoid, in unit coords of the widget.
        struct Zone { unsigned int bit; float x, y, bw, bh; };
        const Zone zones[] = {
            { HitBone_Head,    0.50f, 0.10f, 0.13f, 0.11f },
            { HitBone_Neck,    0.50f, 0.21f, 0.07f, 0.05f },
            { HitBone_Chest,   0.50f, 0.33f, 0.24f, 0.14f },
            { HitBone_Stomach, 0.50f, 0.47f, 0.20f, 0.09f },
            { HitBone_Pelvis,  0.50f, 0.57f, 0.20f, 0.08f },
            { HitBone_Arms,    0.28f, 0.40f, 0.10f, 0.26f },
            { HitBone_Arms,    0.72f, 0.40f, 0.10f, 0.26f },
            { HitBone_Legs,    0.42f, 0.76f, 0.11f, 0.24f },
            { HitBone_Legs,    0.58f, 0.76f, 0.11f, 0.24f },
            { HitBone_Feet,    0.42f, 0.95f, 0.11f, 0.05f },
            { HitBone_Feet,    0.58f, 0.95f, 0.11f, 0.05f },
        };
        const int zoneCount = (int)(sizeof(zones) / sizeof(zones[0]));

        bool changed = false;
        unsigned int hoverBit = 0;

        for (int i = 0; i < zoneCount; i++)
        {
            const Zone& z = zones[i];
            ImVec2 zmn(r.Min.x + (z.x - z.bw * 0.5f) * w, r.Min.y + (z.y - z.bh * 0.5f) * h);
            ImVec2 zmx(r.Min.x + (z.x + z.bw * 0.5f) * w, r.Min.y + (z.y + z.bh * 0.5f) * h);
            if (hov && m.x >= zmn.x && m.x < zmx.x && m.y >= zmn.y && m.y < zmx.y)
                hoverBit = z.bit;
        }
        if (click && hoverBit && mask) { *mask ^= hoverBit; changed = true; }

        for (int i = 0; i < zoneCount; i++)
        {
            const Zone& z = zones[i];
            ImVec2 zmn(r.Min.x + (z.x - z.bw * 0.5f) * w, r.Min.y + (z.y - z.bh * 0.5f) * h);
            ImVec2 zmx(r.Min.x + (z.x + z.bw * 0.5f) * w, r.Min.y + (z.y + z.bh * 0.5f) * h);

            bool on = mask && (*mask & z.bit) != 0;
            char aid[32]; snprintf(aid, sizeof(aid), "##z%d", i);
            float onT = Anim(ImGui::GetID(aid), on ? 1.0f : 0.0f, 18.0f);
            char hid[32]; snprintf(hid, sizeof(hid), "##zh%d", i);
            float hotT = Anim(ImGui::GetID(hid), (hoverBit == z.bit) ? 1.0f : 0.0f, 18.0f);

            ImU32 fill = LerpCol(SetAlpha(t.TextInactive, 40), SetAlpha(t.Accent, 150), onT);
            fill = LerpCol(fill, SetAlpha(t.Accent, 90), hotT * (1.0f - onT));
            dl->AddRectFilled(zmn, zmx, A(fill));
            dl->AddRect(zmn, zmx, A(LerpCol(t.Border, t.Accent, VMax(onT, hotT * 0.6f))), 0.0f, 0, 1.0f);
        }

        (void)cx;
        ImGui::PopID();
        return changed;
    }

    // ===================================================================== curve view
    bool CurveView(const char* id, float points[], int count, ImVec2 size, bool editable)
    {
        ItemAnim anim;
        const Theme& t = theme();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        if (size.x <= 0.0f) size.x = ImGui::GetContentRegionAvail().x;
        if (size.y <= 0.0f) size.y = 110.0f;
        if (count < 2) return false;

        ImGui::PushID(id);
        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##cv", size);
        bool hov = ImGui::IsItemHovered();
        bool held = ImGui::IsItemActive();
        ImVec2 m = ImGui::GetIO().MousePos;

        Area r(origin, ImVec2(origin.x + size.x, origin.y + size.y));
        draw::Box(dl, r.Min, r.Max, t.Element, t.Border);

        // grid
        for (int i = 1; i < 4; i++)
        {
            float gx = r.Min.x + (r.Max.x - r.Min.x) * (i / 4.0f);
            float gy = r.Min.y + (r.Max.y - r.Min.y) * (i / 4.0f);
            dl->AddLine(ImVec2(gx, r.Min.y), ImVec2(gx, r.Max.y), A(SetAlpha(t.Border, 110)));
            dl->AddLine(ImVec2(r.Min.x, gy), ImVec2(r.Max.x, gy), A(SetAlpha(t.Border, 110)));
        }

        float step = (r.Max.x - r.Min.x) / (count - 1);
        bool changed = false;
        int nearest = -1;
        float nearestD = 1e9f;

        if (hov || held)
        {
            for (int i = 0; i < count; i++)
            {
                float px = r.Min.x + i * step;
                float d = fabsf(m.x - px);
                if (d < nearestD) { nearestD = d; nearest = i; }
            }
        }
        if (editable && held && nearest >= 0 && nearestD < step * 0.75f)
        {
            float v = 1.0f - Clamp01((m.y - r.Min.y) / VMax(1.0f, r.Max.y - r.Min.y));
            if (points[nearest] != v) { points[nearest] = v; changed = true; }
        }

        // filled area under the curve, then the line
        for (int i = 0; i < count; i++)
        {
            float px = r.Min.x + i * step;
            float py = r.Max.y - Clamp01(points[i]) * (r.Max.y - r.Min.y);
            dl->PathLineTo(ImVec2(px, py));
        }
        // copy the path before stroking, so the fill can close it along the baseline
        ImVector<ImVec2> line;
        line.resize(dl->_Path.Size);
        memcpy(line.Data, dl->_Path.Data, sizeof(ImVec2) * dl->_Path.Size);
        dl->PathStroke(A(t.Accent), 0, 1.6f);

        for (int i = 0; i < line.Size; i++) dl->PathLineTo(line[i]);
        dl->PathLineTo(ImVec2(r.Max.x, r.Max.y));
        dl->PathLineTo(ImVec2(r.Min.x, r.Max.y));
        dl->PathFillConvex(A(SetAlpha(t.Accent, 34)));

        for (int i = 0; i < count; i++)
        {
            float px = r.Min.x + i * step;
            float py = r.Max.y - Clamp01(points[i]) * (r.Max.y - r.Min.y);
            bool near_ = (i == nearest && nearestD < step * 0.75f);
            dl->AddCircleFilled(ImVec2(px, py), near_ ? 3.5f : 2.5f,
                                A(near_ ? t.Text : t.Accent));
        }

        ImGui::PopID();
        return changed;
    }

    // ===================================================================== varying bar
    float VaryingBar(const char* caption, BarMotion motion, const char* suffix, float speed)
    {
        ItemAnim anim;
        const Theme& t = theme();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        float lh = draw::LineHeight();
        float capH = (caption && *caption) ? lh + 3.0f : 0.0f;
        float trackH = VMax(4.0f, lh * 0.5f);

        double s = Time() * speed;
        float v = 0.0f;
        switch (motion)
        {
        case BarMotion::Triangle: v = fabsf(fmodf((float)s, 2.0f) - 1.0f); break;
        case BarMotion::Sawtooth: v = fmodf((float)s, 1.0f); break;
        case BarMotion::Pulse:    v = (fmodf((float)s, 1.0f) < 0.5f) ? 1.0f : 0.0f; break;
        default:                  v = 0.5f + 0.5f * sinf((float)s * 3.14159265f); break;
        }

        Area r = Hit(0.0f, capH + trackH + 2.0f, caption ? caption : "##vb");

        char value[32];
        snprintf(value, sizeof(value), "%d%s", (int)(v * 100.0f + 0.5f), suffix ? suffix : "");

        if (capH > 0.0f)
        {
            draw::Text(dl, r.Min, t.Text, caption);
            float vw = draw::TextWidth(value);
            draw::Text(dl, ImVec2(r.Max.x - vw, r.Min.y), t.TextInactive, value);
        }

        ImVec2 tmn(r.Min.x, r.Min.y + capH), tmx(r.Max.x, r.Min.y + capH + trackH);
        draw::Rect(dl, tmn, tmx, t.Element);
        draw::Rect(dl, tmn, ImVec2(tmn.x + (tmx.x - tmn.x) * v, tmx.y), t.Accent);
        draw::Border(dl, tmn, tmx, t.Border);
        return v;
    }
}
