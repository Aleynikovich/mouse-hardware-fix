// Unit tests for the platform-independent filters. No dependencies:
//   g++ -std=c++17 -I. tests/filter-test.cpp -o filter-test && ./filter-test

#include "mouse-filter.hpp"

#include <cstdio>

using mhf::ButtonDebouncer;
using mhf::ScrollFilter;
using V = ScrollFilter::Verdict;

static int failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

constexpr mhf::Micros ms = 1000;

static void clean_click() {
    ButtonDebouncer b(25 * ms);
    CHECK(b.on_press(0));               // press goes out immediately
    CHECK(!b.on_release(80 * ms));      // release is held back...
    CHECK(!b.poll(100 * ms));
    CHECK(b.poll(105 * ms));            // ...and emitted after the window
    CHECK(!b.down());
}

static void chatter_on_press() {
    ButtonDebouncer b(25 * ms);
    CHECK(b.on_press(0));
    CHECK(!b.on_release(2 * ms));
    CHECK(!b.on_press(4 * ms));         // bounce cancels the pending release
    CHECK(!b.poll(50 * ms));            // so the button stays down
    CHECK(b.down());
    CHECK(!b.on_release(90 * ms));
    CHECK(b.poll(115 * ms));
}

static void chatter_during_drag() {
    ButtonDebouncer b(25 * ms);
    CHECK(b.on_press(0));
    CHECK(!b.on_release(500 * ms));     // switch opens briefly mid-drag
    CHECK(!b.on_press(508 * ms));
    CHECK(!b.poll(600 * ms));
    CHECK(b.down());                    // drag never interrupted
}

static void chatter_after_release() {
    ButtonDebouncer b(25 * ms);
    CHECK(b.on_press(0));
    CHECK(!b.on_release(80 * ms));
    CHECK(!b.on_press(85 * ms));        // ghost click right after letting go
    CHECK(!b.on_release(88 * ms));
    CHECK(!b.poll(110 * ms));
    CHECK(b.poll(113 * ms));            // exactly one release, one click total
    CHECK(b.suppressed() == 1);
}

static void double_click_survives() {
    ButtonDebouncer b(25 * ms);
    CHECK(b.on_press(0));
    CHECK(!b.on_release(70 * ms));
    CHECK(b.poll(95 * ms));
    CHECK(b.on_press(150 * ms));        // a real double click is untouched
}

static void zero_window_passthrough() {
    ButtonDebouncer b(0);
    CHECK(b.on_press(0));
    CHECK(b.on_release(1));
    CHECK(!b.on_release(2));            // stray release
}

static void scroll_bounce_inverted() {
    ScrollFilter f;
    CHECK(f.on_scroll(0, -120) == V::Pass);
    CHECK(f.on_scroll(30 * ms, -120) == V::Pass);
    CHECK(f.on_scroll(60 * ms, 120) == V::Invert);  // single opposite step
    CHECK(f.on_scroll(90 * ms, -120) == V::Pass);
    CHECK(f.corrected() == 1);
}

static void scroll_reversal_mid_gesture() {
    ScrollFilter f;
    CHECK(f.on_scroll(0, -120) == V::Pass);
    CHECK(f.on_scroll(30 * ms, 120) == V::Invert);
    CHECK(f.on_scroll(60 * ms, 120) == V::Pass);    // second notch confirms it
    CHECK(f.on_scroll(90 * ms, 120) == V::Pass);
}

static void scroll_reversal_after_pause() {
    ScrollFilter f;
    CHECK(f.on_scroll(0, -120) == V::Pass);
    CHECK(f.on_scroll(400 * ms, 120) == V::Pass);   // pause > idle reset
}

static void scroll_hires_counts_units() {
    ScrollFilter f;
    CHECK(f.on_scroll(0, -15) == V::Pass);
    for (int i = 1; i < 16; ++i)                    // 15 * 15 = 225 < 240
        CHECK(f.on_scroll(i * ms, 15) == V::Invert);
    CHECK(f.on_scroll(16 * ms, 15) == V::Pass);     // 240 = 2 notches
}

static void scroll_drop_mode() {
    ScrollFilter::Config cfg;
    cfg.invert = false;
    ScrollFilter f(cfg);
    CHECK(f.on_scroll(0, 120) == V::Pass);
    CHECK(f.on_scroll(10 * ms, -120) == V::Drop);
}

static void parsing() {
    mhf::Micros t = 0;
    CHECK(mhf::parse_seconds("0.025", t) && t == 25000);
    CHECK(!mhf::parse_seconds("abc", t));
    CHECK(!mhf::parse_seconds("-1", t));
    int u = 0;
    CHECK(mhf::parse_notches("1.5", u) && u == 180);
    CHECK(!mhf::parse_notches("0", u));
}

int main() {
    clean_click();
    chatter_on_press();
    chatter_during_drag();
    chatter_after_release();
    double_click_survives();
    zero_window_passthrough();
    scroll_bounce_inverted();
    scroll_reversal_mid_gesture();
    scroll_reversal_after_pause();
    scroll_hires_counts_units();
    scroll_drop_mode();
    parsing();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all tests passed");
    return 0;
}
