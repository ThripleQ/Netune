#include "ui/components/cover_overlay.h"

#include <ftxui/screen/box.hpp>
#include <ftxui/screen/screen.hpp>

#include "core/cover.h"
#include "core/cover_cache.h"

using namespace ftxui;

namespace cover_overlay {

namespace {

/* ── Slot table ────────────────────────────────────────
   Keyed by song index, persistent across frames. SetBox() overwrites the
   entry for its row each time the layout runs; a frame where FTXUI skips
   re-layout leaves the previous values in place, so the overlay is stable
   (no flicker from delete-all/re-place-all). */
std::vector<Slot> g_slots;

/* Monotonic layout-generation counter (see gen() in the header). */
uint64_t g_gen = 0;

/* ── Cover placeholder node ────────────────────────────
   A leaf that reserves the square cover column and records its box. It
   paints nothing — the space is left blank and the real image is placed
   over it by update_list_covers() in the frame hook. */
class CoverNode : public Node {
 public:
  CoverNode(int song_idx, const char* url)
      : song_idx_(song_idx), url_(url ? url : "") {}

  void ComputeRequirement() override {
    /* The placeholder occupies only the FIRST of the song's two rows. The
       image is placed over BOTH rows (see update_list_covers), but the
       layout box must not stretch the title row — a min_y of 2 here would
       make the hbox row 2 tall, pushing title and artist a blank row
       apart (the title sat on the first line, the second stayed empty). */
    requirement_.min_x = cols();
    requirement_.min_y = 1;
    requirement_.flex_grow_x = 0;
    requirement_.flex_shrink_x = 0;
  }

  void SetBox(Box box) override {
    Node::SetBox(box);
    while ((int)g_slots.size() <= song_idx_)
      g_slots.emplace_back();
    Slot& slot = g_slots[song_idx_];
    slot.song_idx = song_idx_;
    slot.x = box.x_min;
    slot.y = box.y_min;
    slot.w = box.x_max - box.x_min + 1;
    slot.h = box.y_max - box.y_min + 1;
    slot.id = 0;
    slot.gen = ++g_gen;  /* every layout pass stamps a fresh gen */
    if (!url_.empty())
      slot.id = cover_cache_request(url_.c_str());
  }

  void Render(Screen& screen) override {
    /* paint nothing — keep the space blank */
    (void)screen;
  }

 private:
  int         song_idx_;
  std::string url_;
};

}  // namespace

int cols(void) {
  int c = rows() * cover_cell_height() / cover_cell_width();
  if (c < 4) c = 4;
  return c;
}

int rows(void) { return 2; }

std::vector<Slot>& slots() { return g_slots; }

uint64_t gen(void) { return g_gen; }

Element coverPlaceholder(int song_idx, const char* url) {
  return std::make_shared<CoverNode>(song_idx, url);
}

}  // namespace cover_overlay
