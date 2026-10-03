// Linux backend: grabs one evdev mouse node and re-emits its events through a
// uinput clone with click chatter and wheel bounce filtered out.
//
// The clone copies the original device's name, IDs and capabilities, so
// libinput, hwdb entries and desktop per-device settings keep applying, and
// every event the filters do not touch is forwarded unchanged.

#include "mouse-filter.hpp"

#include <fcntl.h>
#include <getopt.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {

constexpr int kFirstButton = BTN_LEFT;  // BTN_LEFT .. BTN_TASK
constexpr int kButtonCount = BTN_TASK - BTN_LEFT + 1;

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

#define BITS_LONGS(n) (((n) + 8 * sizeof(long) - 1) / (8 * sizeof(long)))

bool test_bit(const unsigned long* bits, int bit) {
    return (bits[bit / (8 * sizeof(long))] >> (bit % (8 * sizeof(long)))) & 1UL;
}

mhf::Micros now_us() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<mhf::Micros>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

mhf::Micros event_us(const input_event& ev) {
    return static_cast<mhf::Micros>(ev.input_event_sec) * 1000000 + ev.input_event_usec;
}

bool is_button(const input_event& ev) {
    return ev.type == EV_KEY && ev.code >= kFirstButton && ev.code < kFirstButton + kButtonCount;
}

bool is_wheel(const input_event& ev) {
    return ev.type == EV_REL && (ev.code == REL_WHEEL || ev.code == REL_WHEEL_HI_RES);
}

struct Match {
    unsigned vendor, product;
};

void usage(const char* argv0) {
    std::fprintf(stderr,
        "Usage: %s --device /dev/input/eventN [options]\n"
        "  --disable-scroll          do not filter wheel bounce\n"
        "  --disable-click           do not filter button chatter\n"
        "  --scroll-timeout <sec>    pause that ends a scroll gesture (default 0.300)\n"
        "  --scroll-reversal <n>     notches needed to accept a mid-gesture reversal (default 2)\n"
        "  --scroll-drop             drop bounced wheel steps instead of inverting them\n"
        "  --click-timeout <sec>     chatter window for buttons (default 0.050)\n"
        "  --only <vid:pid>          only handle this device (hex, repeatable)\n"
        "  --verbose                 log every corrected event\n",
        argv0);
}

class Daemon {
public:
    explicit Daemon(const mhf::Options& opt) : opt_(opt), scroll_(opt.scroll) {
        for (auto& b : buttons_) b.set_window(opt.click_fix ? opt.click_window : 0);
    }

    ~Daemon() {
        if (out_ >= 0) {
            flush_pending_releases();
            ioctl(out_, UI_DEV_DESTROY);
            close(out_);
        }
        if (in_ >= 0) {
            ioctl(in_, EVIOCGRAB, 0);
            close(in_);
        }
    }

    // Returns 0 to run, otherwise the exit code: 2 = not a device we handle.
    int open_device(const std::string& path, const std::vector<Match>& only) {
        in_ = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (in_ < 0) {
            std::fprintf(stderr, "Cannot open %s: %s\n", path.c_str(), std::strerror(errno));
            return 1;
        }

        char name[UINPUT_MAX_NAME_SIZE] = {};
        char phys[64] = {};
        ioctl(in_, EVIOCGNAME(sizeof(name) - 1), name);
        ioctl(in_, EVIOCGPHYS(sizeof(phys) - 1), phys);
        ioctl(in_, EVIOCGID, &id_);

        if (!only.empty()) {
            bool found = false;
            for (const auto& m : only) found |= (m.vendor == id_.vendor && m.product == id_.product);
            if (!found) {
                std::fprintf(stderr, "Skipping %s (%04x:%04x): not in --only list\n", name, id_.vendor, id_.product);
                return 2;
            }
        }

        unsigned long ev_bits[BITS_LONGS(EV_CNT)] = {};
        unsigned long rel_bits[BITS_LONGS(REL_CNT)] = {};
        unsigned long key_bits[BITS_LONGS(KEY_CNT)] = {};
        ioctl(in_, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits);
        ioctl(in_, EVIOCGBIT(EV_REL, sizeof(rel_bits)), rel_bits);
        ioctl(in_, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits);

        // Only plain relative mice. Touchpads, tablets and anything with LEDs or
        // force feedback would lose functionality behind a uinput clone.
        bool is_mouse = test_bit(ev_bits, EV_REL) && test_bit(rel_bits, REL_X) &&
                        test_bit(rel_bits, REL_Y) && test_bit(key_bits, BTN_LEFT);
        bool unsupported = test_bit(ev_bits, EV_ABS) || test_bit(ev_bits, EV_LED) || test_bit(ev_bits, EV_FF);
        if (!is_mouse || unsupported) {
            std::fprintf(stderr, "Skipping %s: not a plain relative mouse\n", name);
            return 2;
        }

        // Event timestamps on the monotonic clock, so they can be compared with
        // deadlines and are immune to wall-clock jumps.
        int clk = CLOCK_MONOTONIC;
        ioctl(in_, EVIOCSCLOCKID, &clk);

        out_ = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (out_ < 0) {
            std::fprintf(stderr, "Cannot open /dev/uinput: %s\n", std::strerror(errno));
            return 1;
        }

        for (int type : {EV_SYN, EV_KEY, EV_REL, EV_MSC, EV_REP}) {
            if (test_bit(ev_bits, type)) ioctl(out_, UI_SET_EVBIT, type);
        }
        copy_bits(EV_KEY, KEY_CNT, UI_SET_KEYBIT, ev_bits);
        copy_bits(EV_REL, REL_CNT, UI_SET_RELBIT, ev_bits);
        copy_bits(EV_MSC, MSC_CNT, UI_SET_MSCBIT, ev_bits);

        unsigned long prop_bits[BITS_LONGS(INPUT_PROP_CNT)] = {};
        if (ioctl(in_, EVIOCGPROP(sizeof(prop_bits)), prop_bits) >= 0) {
            for (int p = 0; p < INPUT_PROP_CNT; ++p)
                if (test_bit(prop_bits, p)) ioctl(out_, UI_SET_PROPBIT, p);
        }

        std::string out_phys = std::string("mouse-hardware-fix/") + phys;
        ioctl(out_, UI_SET_PHYS, out_phys.c_str());

        uinput_setup setup = {};
        setup.id = id_;
        std::snprintf(setup.name, sizeof(setup.name), "%s", name);
        if (ioctl(out_, UI_DEV_SETUP, &setup) < 0 || ioctl(out_, UI_DEV_CREATE) < 0) {
            std::fprintf(stderr, "Cannot create uinput device: %s\n", std::strerror(errno));
            close(out_);
            out_ = -1;
            return 1;
        }

        if (ioctl(in_, EVIOCGRAB, 1) < 0) {
            std::fprintf(stderr, "Cannot grab %s: %s\n", path.c_str(), std::strerror(errno));
            return 1;
        }

        // Start from the real button state in case a button is held right now.
        sync_buttons(false);

        std::fprintf(stderr, "Filtering %s (%04x:%04x) on %s | scroll fix: %s | click fix: %s\n",
                     name, id_.vendor, id_.product, path.c_str(),
                     opt_.scroll_fix ? "on" : "off", opt_.click_fix ? "on" : "off");
        return 0;
    }

    int run() {
        input_event buf[64];
        while (!g_stop) {
            pollfd pfd = {in_, POLLIN, 0};
            int r = poll(&pfd, 1, poll_timeout_ms());
            if (r < 0) {
                if (errno == EINTR) continue;
                std::fprintf(stderr, "poll: %s\n", std::strerror(errno));
                return 1;
            }

            fire_due_releases(now_us());

            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                std::fprintf(stderr, "Device disconnected\n");
                return 0;
            }
            if (!(pfd.revents & POLLIN)) continue;

            ssize_t n = read(in_, buf, sizeof(buf));
            if (n < 0) {
                if (errno == EAGAIN || errno == EINTR) continue;
                if (errno == ENODEV) {
                    std::fprintf(stderr, "Device disconnected\n");
                    return 0;
                }
                std::fprintf(stderr, "read: %s\n", std::strerror(errno));
                return 1;
            }
            for (size_t i = 0; i < static_cast<size_t>(n) / sizeof(input_event); ++i) handle(buf[i]);
        }
        return 0;
    }

private:
    void copy_bits(int type, int count, unsigned long request, const unsigned long* ev_bits) {
        if (!test_bit(ev_bits, type)) return;
        std::vector<unsigned long> bits(BITS_LONGS(count));
        if (ioctl(in_, EVIOCGBIT(type, bits.size() * sizeof(long)), bits.data()) < 0) return;
        for (int c = 0; c < count; ++c)
            if (test_bit(bits.data(), c)) ioctl(out_, request, c);
    }

    void handle(const input_event& ev) {
        if (ev.type == EV_SYN && ev.code == SYN_DROPPED) {
            // The kernel buffer overflowed; the partial frame is unreliable.
            frame_.clear();
            resyncing_ = true;
            return;
        }
        if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            if (resyncing_) {
                frame_.clear();
                resyncing_ = false;
                sync_buttons(true);
            } else {
                flush_frame();
            }
            return;
        }
        if (!resyncing_) frame_.push_back(ev);
    }

    void flush_frame() {
        // One scroll verdict per frame, so REL_WHEEL and REL_WHEEL_HI_RES from
        // the same report are always treated identically.
        auto verdict = mhf::ScrollFilter::Verdict::Pass;
        if (opt_.scroll_fix) {
            int wheel = 0, hires = 0;
            bool has_hires = false;
            mhf::Micros t = 0;
            for (const auto& e : frame_) {
                if (e.type != EV_REL) continue;
                if (e.code == REL_WHEEL) {
                    wheel += e.value;
                } else if (e.code == REL_WHEEL_HI_RES) {
                    hires += e.value;
                    has_hires = true;
                } else {
                    continue;
                }
                t = event_us(e);
            }
            int units = has_hires ? hires : wheel * mhf::kUnitsPerNotch;
            if (units != 0) {
                verdict = scroll_.on_scroll(t, units);
                if (opt_.verbose && verdict != mhf::ScrollFilter::Verdict::Pass)
                    std::fprintf(stderr, "scroll bounce %+d %s\n", units,
                                 verdict == mhf::ScrollFilter::Verdict::Invert ? "inverted" : "dropped");
            }
        }

        std::vector<input_event> out;
        out.reserve(frame_.size() + 1);
        bool meaningful = false;
        for (auto e : frame_) {
            if (is_wheel(e) && verdict != mhf::ScrollFilter::Verdict::Pass) {
                if (verdict == mhf::ScrollFilter::Verdict::Drop) continue;
                e.value = -e.value;
            }
            if (opt_.click_fix && is_button(e) && !filter_button(e)) continue;
            meaningful |= (e.type != EV_MSC);
            out.push_back(e);
        }
        frame_.clear();

        // A frame whose only remaining content is MSC_SCAN belonged to a
        // swallowed button event; forwarding it would be noise.
        if (!meaningful) return;
        write_frame(out);
    }

    // Returns true if the button event should be forwarded.
    bool filter_button(const input_event& e) {
        auto& b = buttons_[e.code - kFirstButton];
        if (e.value == 1) {
            unsigned long before = b.suppressed();
            bool pass = b.on_press(event_us(e));
            if (opt_.verbose && b.suppressed() != before)
                std::fprintf(stderr, "button 0x%x chatter suppressed\n", e.code);
            return pass;
        }
        if (e.value == 0) return b.on_release(event_us(e));
        return true;  // autorepeat (value 2), not expected from mice
    }

    void write_frame(std::vector<input_event>& events) {
        input_event syn = {};
        syn.type = EV_SYN;
        syn.code = SYN_REPORT;
        events.push_back(syn);
        ssize_t len = static_cast<ssize_t>(events.size() * sizeof(input_event));
        if (write(out_, events.data(), len) != len)
            std::fprintf(stderr, "write to uinput failed: %s\n", std::strerror(errno));
    }

    void emit_button(int code, int value) {
        input_event e = {};
        e.type = EV_KEY;
        e.code = static_cast<unsigned short>(code);
        e.value = value;
        std::vector<input_event> events{e};
        write_frame(events);
    }

    void fire_due_releases(mhf::Micros now) {
        for (int i = 0; i < kButtonCount; ++i)
            if (buttons_[i].poll(now)) emit_button(kFirstButton + i, 0);
    }

    // On shutdown, make sure no button is left stuck down on the clone.
    void flush_pending_releases() {
        for (int i = 0; i < kButtonCount; ++i) {
            if (!buttons_[i].down()) continue;
            buttons_[i].reset(false);
            emit_button(kFirstButton + i, 0);
        }
    }

    // Reconciles our idea of the buttons with the kernel's after a gap.
    void sync_buttons(bool emit) {
        unsigned long keys[BITS_LONGS(KEY_CNT)] = {};
        if (ioctl(in_, EVIOCGKEY(sizeof(keys)), keys) < 0) return;
        for (int i = 0; i < kButtonCount; ++i) {
            bool real = test_bit(keys, kFirstButton + i);
            bool shown = buttons_[i].down();
            buttons_[i].reset(real);
            if (emit && real != shown) emit_button(kFirstButton + i, real ? 1 : 0);
        }
    }

    int poll_timeout_ms() const {
        mhf::Micros next = mhf::kNoDeadline;
        for (const auto& b : buttons_)
            if (b.deadline() < next) next = b.deadline();
        if (next == mhf::kNoDeadline) return -1;
        mhf::Micros wait = next - now_us();
        return wait <= 0 ? 0 : static_cast<int>((wait + 999) / 1000);
    }

    mhf::Options opt_;
    mhf::ScrollFilter scroll_;
    mhf::ButtonDebouncer buttons_[kButtonCount];
    std::vector<input_event> frame_;
    bool resyncing_ = false;
    int in_ = -1;
    int out_ = -1;
    input_id id_ = {};
};

}  // namespace

int main(int argc, char* argv[]) {
    mhf::Options opt;
    std::string device;
    std::vector<Match> only;

    enum { OPT_REVERSAL = 1000, OPT_DROP, OPT_ONLY, OPT_VERBOSE };
    const option long_options[] = {
        {"device", required_argument, nullptr, 'd'},
        {"disable-scroll", no_argument, nullptr, 's'},
        {"disable-click", no_argument, nullptr, 'c'},
        {"scroll-timeout", required_argument, nullptr, 'S'},
        {"click-timeout", required_argument, nullptr, 'C'},
        {"scroll-reversal", required_argument, nullptr, OPT_REVERSAL},
        {"scroll-drop", no_argument, nullptr, OPT_DROP},
        {"only", required_argument, nullptr, OPT_ONLY},
        {"verbose", no_argument, nullptr, OPT_VERBOSE},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };

    int c;
    while ((c = getopt_long(argc, argv, "d:scS:C:h", long_options, nullptr)) != -1) {
        bool ok = true;
        switch (c) {
            case 'd': device = optarg; break;
            case 's': opt.scroll_fix = false; break;
            case 'c': opt.click_fix = false; break;
            case 'S': ok = mhf::parse_seconds(optarg, opt.scroll.idle_reset); break;
            case 'C': ok = mhf::parse_seconds(optarg, opt.click_window); break;
            case OPT_REVERSAL: ok = mhf::parse_notches(optarg, opt.scroll.reversal_units); break;
            case OPT_DROP: opt.scroll.invert = false; break;
            case OPT_VERBOSE: opt.verbose = true; break;
            case OPT_ONLY: {
                Match m;
                ok = std::sscanf(optarg, "%x:%x", &m.vendor, &m.product) == 2;
                if (ok) only.push_back(m);
                break;
            }
            case 'h': usage(argv[0]); return 0;
            default: usage(argv[0]); return 1;
        }
        if (!ok) {
            for (const auto& o : long_options)
                if (o.name && o.val == c) std::fprintf(stderr, "Invalid value for --%s: %s\n", o.name, optarg);
            return 1;
        }
    }

    if (device.empty()) {
        usage(argv[0]);
        return 1;
    }
    if (!opt.scroll_fix && !opt.click_fix) {
        std::fprintf(stderr, "Both fixes disabled, nothing to do.\n");
        return 0;
    }

    struct sigaction sa = {};
    sa.sa_handler = on_signal;  // no SA_RESTART: poll() must wake up
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);

    Daemon d(opt);
    int r = d.open_device(device, only);
    if (r == 2) return 0;  // not ours: exit cleanly so systemd does not restart us
    if (r != 0) return r;
    return d.run();
}
