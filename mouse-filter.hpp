// Platform-independent filtering logic shared by the Linux and Windows backends.
//
// Time is in microseconds on a monotonic clock. Scroll deltas are in "hi-res
// units" where one wheel notch is 120 (the Linux REL_WHEEL_HI_RES and Windows
// WHEEL_DELTA convention).
#pragma once

#include <cstdint>
#include <cstdlib>

namespace mhf {

using Micros = std::int64_t;

constexpr int kUnitsPerNotch = 120;
constexpr Micros kNoDeadline = INT64_MAX;

// Click chatter fix.
//
// Presses are forwarded immediately, so clicking has no added latency.
// Releases are held back for `window`; if the switch bounces back to "pressed"
// within that window, the release and the bounced press are both swallowed.
// This removes double clicks from chatter both when clicking and in the middle
// of a hold/drag, which a plain "ignore events for N ms" filter cannot do.
class ButtonDebouncer {
public:
    explicit ButtonDebouncer(Micros window = 0) : window_(window) {}

    void set_window(Micros window) { window_ = window; }

    // Returns true if the press should be forwarded.
    bool on_press(Micros) {
        if (release_deadline_ != kNoDeadline) {
            release_deadline_ = kNoDeadline;  // chatter: button never really went up
            ++suppressed_;
            return false;
        }
        if (down_) return false;  // duplicate press, nothing to report
        down_ = true;
        return true;
    }

    // Returns true if the release should be forwarded right now. Otherwise it
    // is either a stray release, or it has been deferred until deadline().
    bool on_release(Micros t) {
        if (!down_ || release_deadline_ != kNoDeadline) return false;
        if (window_ <= 0) {
            down_ = false;
            return true;
        }
        release_deadline_ = t + window_;
        return false;
    }

    // Returns true once when a deferred release is due and must be emitted.
    bool poll(Micros now) {
        if (release_deadline_ == kNoDeadline || now < release_deadline_) return false;
        release_deadline_ = kNoDeadline;
        down_ = false;
        return true;
    }

    // Forget everything; used after the backend lost track of device state.
    void reset(bool down) {
        down_ = down;
        release_deadline_ = kNoDeadline;
    }

    Micros deadline() const { return release_deadline_; }
    bool down() const { return down_; }
    unsigned long suppressed() const { return suppressed_; }

private:
    Micros window_;
    bool down_ = false;
    Micros release_deadline_ = kNoDeadline;
    unsigned long suppressed_ = 0;
};

// Scroll wheel bounce fix.
//
// A worn wheel encoder occasionally reports a step in the opposite direction
// while the wheel is being turned. While a scroll gesture is in progress, an
// opposite-direction step is treated as a bounce unless the wheel keeps going
// that way for `reversal_units` (default: 2 notches). A pause longer than
// `idle_reset` ends the gesture, so a deliberate reversal after a short pause
// always goes through immediately.
class ScrollFilter {
public:
    enum class Verdict {
        Pass,    // forward as-is
        Invert,  // bounce: forward with the sign flipped (keeps scrolling smooth)
        Drop,    // bounce: discard
    };

    struct Config {
        Micros idle_reset = 300000;
        int reversal_units = 2 * kUnitsPerNotch;
        bool invert = true;  // false: drop bounces instead of inverting them
    };

    ScrollFilter() = default;
    explicit ScrollFilter(const Config& cfg) : cfg_(cfg) {}

    void set_config(const Config& cfg) { cfg_ = cfg; }

    Verdict on_scroll(Micros t, int units) {
        if (units == 0) return Verdict::Pass;
        if (have_last_ && t - last_time_ > cfg_.idle_reset) {
            direction_ = 0;
            opposite_ = 0;
        }
        have_last_ = true;
        last_time_ = t;

        const int dir = units > 0 ? 1 : -1;
        if (direction_ == 0 || dir == direction_) {
            direction_ = dir;
            opposite_ = 0;
            return Verdict::Pass;
        }

        opposite_ += std::abs(units);
        if (opposite_ >= cfg_.reversal_units) {
            direction_ = dir;
            opposite_ = 0;
            return Verdict::Pass;
        }
        ++corrected_;
        return cfg_.invert ? Verdict::Invert : Verdict::Drop;
    }

    unsigned long corrected() const { return corrected_; }

private:
    Config cfg_;
    int direction_ = 0;
    int opposite_ = 0;
    bool have_last_ = false;
    Micros last_time_ = 0;
    unsigned long corrected_ = 0;
};

// Settings shared by both backends, filled from the command line.
struct Options {
    bool scroll_fix = true;
    bool click_fix = true;
    bool verbose = false;
    ScrollFilter::Config scroll;
    Micros click_window = 25000;
};

// Parses a duration in seconds (e.g. "0.025"). Returns false on bad input.
inline bool parse_seconds(const char* s, Micros& out) {
    char* end = nullptr;
    double v = std::strtod(s, &end);
    if (end == s || *end != '\0' || v < 0 || v > 10) return false;
    out = static_cast<Micros>(v * 1e6 + 0.5);
    return true;
}

// Parses a notch count (e.g. "2" or "1.5") into hi-res units.
inline bool parse_notches(const char* s, int& out) {
    char* end = nullptr;
    double v = std::strtod(s, &end);
    if (end == s || *end != '\0' || v <= 0 || v > 100) return false;
    out = static_cast<int>(v * kUnitsPerNotch + 0.5);
    return true;
}

}  // namespace mhf
