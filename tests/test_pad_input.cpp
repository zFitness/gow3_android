// gow3: the pad snapshot the overlay dialogs read (gpu/shim/gport_pad_input.h).
#include <cassert>
#include <cstdio>
#include <atomic>
#include "gport_pad_input.h"

namespace P = GportPad;

int main() {
    // A press fires once; holding the key does not repeat it.
    std::atomic<std::uint32_t> seen{0};
    assert(P::Edges(P::kCross, seen) == P::kCross);
    assert(P::Edges(P::kCross, seen) == 0);
    assert(P::Edges(P::kCross | P::kUp, seen) == P::kUp);
    // Releasing and pressing again fires again.
    assert(P::Edges(0, seen) == 0);
    assert(P::Edges(P::kCross, seen) == P::kCross);
    // Two keys released and pressed together come out together...
    assert(P::Edges(0, seen) == 0);
    assert(P::Edges(P::kCross | P::kCircle, seen) == (P::kCross | P::kCircle));
    // ...and holding both afterwards fires neither again.
    assert(P::Edges(P::kCross | P::kCircle, seen) == 0);
    // Only the newly pressed key is an edge while the other stays down.
    assert(P::Edges(P::kCross | P::kCircle | P::kUp, seen) == P::kUp);
    assert(P::Edges(0, seen) == 0);

    // Opening a dialog forgets what was already held, so it is not a press on the new list.
    std::atomic<std::uint32_t> reopened{0};
    P::Forget(reopened, P::kCross);
    assert(P::Edges(P::kCross, reopened) == 0);
    assert(P::Edges(P::kCross | P::kDown, reopened) == P::kDown);
    // No pad file at all (desktop): nothing is ever reported.
    assert(P::Edges(0, reopened) == 0);

    // Directions, accept and cancel, and nothing else.
    int move = 0, finish = 0;
    P::Map(P::kUp, move, finish);      assert(move == -1 && finish == 0);
    P::Map(P::kDown, move, finish);    assert(move == 1 && finish == 0);
    P::Map(P::kCross, move, finish);   assert(move == 0 && finish == 1);
    P::Map(P::kCircle, move, finish);  assert(move == 0 && finish == 2);
    P::Map(0, move, finish);           assert(move == 0 && finish == 0);
    // A direction held together with accept still moves and accepts.
    P::Map(P::kDown | P::kCross, move, finish); assert(move == 1 && finish == 1);
    // Buttons the list does not use (shoulders, sticks) never act.
    P::Map(0x400 | 0x800 | 0x2 | 0x4, move, finish); assert(move == 0 && finish == 0);

    puts("PASS: pad snapshot edges, dialog reset, choice key mapping");
}
