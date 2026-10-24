// velo_internal.h — shared state between the twin's translation units.
// Not part of the public API; include velo.h instead.

#pragma once
#include "velo.h"

namespace velo
{
    namespace detail
    {
        // ---------------------------------------------------------- pages
        // One record per page, persisted across frames. Because ImGui has no
        // retained children, a page's "content" only exists while its build code
        // runs — so the transition is driven entirely from these timers and applied
        // per widget as the page is rebuilt.
        struct PageRec
        {
            ImGuiID id = 0;
            bool    wasActive = false;
            int     lastSelected = -1;
            int     phase = 0;         // 0 hidden, 1 animating in, 2 animating out
            float   timer = 0.0f;      // seconds since the phase started
            int     dir = 1;           // +1 forward, -1 backward
        };

        // The page currently being built, if any.
        struct PageCtx
        {
            PageOpts opts;
            bool  outPhase = false;
            float outAlpha = 1.0f;
            float localTime = 0.0f;    // seconds into the in-phase
            int   item = 0;            // incremented per control, drives the stagger
            int   dir = 1;
        };

        struct State
        {
            float  dt = 1.0f / 60.0f;
            double time = 0.0;
            float  alphaMul = 1.0f;    // every draw call multiplies by this
            PageCtx* page = nullptr;
            int    untitled = 0;       // untitled sections seen this frame, for unique IDs
            // The previous item's vertical slide, still baked into the layout
            // cursor, and the window (by draw list) it was applied in. The next
            // item in the same window takes it back out — see ItemAnim().
            float  slidePending = 0.0f;
            const ImDrawList* slideWindow = nullptr;
        };

        extern State g_state;

        PageRec* GetPage(ImGuiID id);

        // ---------------------------------------------------------- per-item scope
        // Wraps one control so it picks up the current page's alpha and slide
        // offset. Controls construct one at the top of their body; the destructor
        // restores everything. This is what makes stagger automatic — the page
        // counts items in call order, exactly like the WinForms version sorts
        // children in reading order.
        struct ItemAnim
        {
            float savedAlpha;
            bool  pushedStyle = false;

            ItemAnim();
            ~ItemAnim();
        };

        // Shared helpers used across the control implementations.
        ImU32 A(ImU32 c);                       // apply the page alpha
        float RowH();                           // theme row height, scaled
        ImVec2 Anchor(ScreenAnchor a, ImVec2 size, ImVec2 offset, ImVec2 areaMin, ImVec2 areaMax);
    }
}
