// Copyright (c) 2026 Jozsef Ottucsak / Parvati.
//
// ModMatrixHighlight — a tiny editor-scoped highlight BUS that links the Mod
// Matrix rows (ModMatrixView) and the destination knobs (ParamControl), which
// live in different parts of the component tree and cannot message each other
// directly.
//
// Three independent signals flow over the bus:
//   * DEST highlight — fired by mousing over a knob OR a matrix row, AND while a
//     drag is hovered over a destination knob. Every knob whose
//     ModulationDestination matches glows its modulation ring, and every matrix
//     row whose dest matches emphasises itself. -1 clears.
//   * SLOT selection — fired by double-clicking a knob's modulation ring. The
//     matrix scrolls that slot's row into view and transiently emphasises it.
//     -1 clears the selection.
//   * ASSIGN request — fired by dropping a dragged mod source onto a destination
//     knob. The registered handler(s) consume the next free slot for
//     (source -> dest) and return true, or false if the matrix is full. Both the
//     synth ModMatrixView and the FX FxMatrixView register here (each ignores the
//     other's dest domain); the bus fans the request out so the drop needs no ref.
//
// CAVEAT (multi-editor): the bus is a static singleton, so it is SHARED across
// simultaneous plugin windows. A highlight set in one window would glow the
// matching knob/row in every open window of the same plugin. This is a purely
// cosmetic side-effect (a momentary highlight) and is acceptable for the hover
// feature; if it ever matters, the bus can be moved onto the processor and
// fetched via processor_ from both sides. Every subscriber callback is guarded
// by a SafePointer AND explicitly unsubscribed at teardown, so a cross-window
// notification is always a safe no-op on a component that has been destroyed.

#pragma once

#include <functional>
#include <utility>
#include <vector>

namespace parvati
{

class ModMatrixHighlight
{
public:
    // Editor-scoped singleton (see the multi-editor caveat above).
    static ModMatrixHighlight& instance()
    {
        static ModMatrixHighlight bus;
        return bus;
    }

    //--------------------------------------------------------------------
    // DEST highlight
    //--------------------------------------------------------------------
    // Set the highlighted ModulationDestination (-1 clears). Idempotent: a
    // no-op when the dest is unchanged. Notifies every dest subscriber with
    // the new dest (or -1 on clear).
    void setHighlightedDest (int modDst)
    {
        if (modDst == highlightedDest_)
            return;
        highlightedDest_ = modDst;
        notify (destSubs_, highlightedDest_);
    }

    int highlightedDest() const noexcept { return highlightedDest_; }

    // Register a dest-highlight observer. Returns a subscription id for
    // unsubscribe(). The callback receives the new dest, or -1 on clear.
    int onDestHighlighted (std::function<void (int)> cb)
    {
        const int id = nextId_++;
        destSubs_.push_back ({ id, std::move (cb) });
        return id;
    }

    //--------------------------------------------------------------------
    // SLOT selection
    //--------------------------------------------------------------------
    // Select a slot (0..13) so the matrix scrolls to + emphasises its row;
    // -1 clears the selection. Always notifies (a re-select re-triggers the
    // scroll/emphasis so a second double-click re-centres the row).
    void selectSlot (int slotIndex)
    {
        selectedSlot_ = slotIndex;
        notify (slotSubs_, selectedSlot_);
    }

    int selectedSlot() const noexcept { return selectedSlot_; }

    // Register a slot-selection observer. Returns a subscription id for
    // unsubscribe(). The callback receives the slot (0..13) or -1 on clear.
    int onSlotSelected (std::function<void (int)> cb)
    {
        const int id = nextId_++;
        slotSubs_.push_back ({ id, std::move (cb) });
        return id;
    }

    //--------------------------------------------------------------------
    // Teardown
    //--------------------------------------------------------------------
    // Remove a subscription (dest / slot / assign) by id. Safe to call with an
    // unknown / already-removed id (no-op).
    void unsubscribe (int id)
    {
        eraseById (destSubs_, id);
        eraseById (slotSubs_, id);
        eraseById (assignSubs_, id);
    }

    //--------------------------------------------------------------------
    // ASSIGN request (drag-and-drop)
    //--------------------------------------------------------------------
    // Register a handler that consumes a (source -> dest) assignment into the
    // next free matrix slot. Returns a subscription id for unsubscribe(). Both
    // the synth ModMatrixView and the FX FxMatrixView register here; each guards
    // its own dest domain (synth 0..MOD_DST_LAST-1, FX FX_DST+kFxModDstOffset)
    // so only the matching handler consumes a drop (the bus still snapshots the
    // list so a mid-dispatch unsubscribe is safe — see the caveat below).
    int onAssignRequest (std::function<bool (int, int)> cb)
    {
        const int id = nextId_++;
        assignSubs_.push_back ({ id, std::move (cb) });
        return id;
    }

    // Dispatch an assign request to every registered handler. Returns true if
    // ANY handler consumed a slot. The callback list is snapshotted first so a
    // handler that unsubscribes mid-dispatch cannot invalidate the iteration.
    bool requestAssign (int source, int dest)
    {
        std::vector<std::function<bool (int, int)>> snapshot;
        snapshot.reserve (assignSubs_.size());
        for (auto& s : assignSubs_)
            snapshot.push_back (s.cb);
        bool any = false;
        for (auto& cb : snapshot)
            if (cb && cb (source, dest))
                any = true;
        return any;
    }

private:
    ModMatrixHighlight() = default;

    struct Sub
    {
        int id;
        std::function<void (int)> cb;
    };

    // An assign-request observer: source/dest -> did a slot get consumed?
    struct AssignSub
    {
        int id;
        std::function<bool (int, int)> cb;
    };

    // Invoke every callback. The function list is snapshotted first so a
    // callback that unsubscribes (directly or transitively, e.g. via component
    // teardown) cannot invalidate the iteration.
    static void notify (std::vector<Sub>& subs, int value)
    {
        std::vector<std::function<void (int)>> snapshot;
        snapshot.reserve (subs.size());
        for (auto& s : subs)
            snapshot.push_back (s.cb);
        for (auto& cb : snapshot)
            if (cb)
                cb (value);
    }

    // Remove a subscription of any kind by id. Templated so it works for both
    // Sub (dest/slot) and AssignSub vectors. Safe with an unknown id (no-op).
    template <typename Entry>
    static void eraseById (std::vector<Entry>& subs, int id)
    {
        for (size_t i = 0; i < subs.size(); ++i)
            if (subs[i].id == id)
            {
                subs.erase (subs.begin() + static_cast<std::ptrdiff_t> (i));
                return;
            }
    }

    int nextId_ = 0;
    int highlightedDest_ = -1;
    int selectedSlot_ = -1;
    std::vector<Sub> destSubs_;
    std::vector<Sub> slotSubs_;
    std::vector<AssignSub> assignSubs_;
};

}  // namespace parvati
