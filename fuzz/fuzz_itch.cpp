// libFuzzer harness: arbitrary bytes -> ITCH framing/decoding -> book reconstruction.
// ASan/UBSan catch memory errors; validate() catches logical corruption of the book
// (broken links, wrong level totals, ladder out of order, leaked orders).
#include <cstdint>
#include <cstdlib>
#include <memory>

#include "lob/book_builder.hpp"
#include "lob/itch.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static auto* b = new lob::BookBuilder<lob::Fast>();  // reused: state carries across inputs, like a live feed
    lob::itch::walk(data, size, *b);
    if (!b->validate()) std::abort();
    if (b->live_orders() > 200000) { delete b; b = new lob::BookBuilder<lob::Fast>(); }
    return 0;
}
