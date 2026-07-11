#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <cstring>
#include <cmath>
#include <string>
#include <getopt.h>

const double DEFAULT_IDLE_TIMEOUT_SEC = 0.300; 
const double DEFAULT_CLICK_DEBOUNCE_SEC = 0.015; 

struct ButtonState {
    int current_state = 0;
    double last_stable_press_time = 0.0;
    double last_stable_release_time = 0.0;
    bool ignore_next_release = false;
    bool ignore_next_press = false;
};

int main(int argc, char* argv[]) {
    double idle_timeout_sec = DEFAULT_IDLE_TIMEOUT_SEC;
    double click_debounce_sec = DEFAULT_CLICK_DEBOUNCE_SEC;
    bool enable_scroll_fix = true;
    bool enable_click_fix = true;
    std::string device_path = "";

    struct option long_options[] = {
        {"device", required_argument, 0, 'd'},
        {"disable-scroll", no_argument, 0, 's'},
        {"disable-click", no_argument, 0, 'c'},
        {"scroll-timeout", required_argument, 0, 'S'},
        {"click-timeout", required_argument, 0, 'C'},
        {0, 0, 0, 0}
    };

    int opt;
    int option_index = 0;
    while ((opt = getopt_long(argc, argv, "d:scS:C:", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'd': device_path = optarg; break;
            case 's': enable_scroll_fix = false; break;
            case 'c': enable_click_fix = false; break;
            case 'S': try { idle_timeout_sec = std::stod(optarg); } catch (...) {} break;
            case 'C': try { click_debounce_sec = std::stod(optarg); } catch (...) {} break;
            default:
                std::cerr << "Usage: " << argv[0] << " --device <path> [--disable-scroll] [--disable-click]" << std::endl;
                return 1;
        }
    }

    if (device_path.empty()) {
        std::cerr << "Error: You must specify a device using --device <path>" << std::endl;
        return 1;
    }

    if (!enable_scroll_fix && !enable_click_fix) {
        std::cerr << "Both fixes disabled. Nothing to do." << std::endl;
        return 1;
    }

    int fd = open(device_path.c_str(), O_RDONLY);
    if (fd < 0) {
        std::cerr << "Error opening device " << device_path << " (run with sudo): " << std::strerror(errno) << std::endl;
        return 1;
    }

    int uifd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (uifd < 0) {
        std::cerr << "Error opening /dev/uinput: " << std::strerror(errno) << std::endl;
        close(fd);
        return 1;
    }

    ioctl(uifd, UI_SET_EVBIT, EV_SYN);
    ioctl(uifd, UI_SET_EVBIT, EV_KEY);
    ioctl(uifd, UI_SET_EVBIT, EV_REL);

    // X/Y Movement and Scroll
    ioctl(uifd, UI_SET_RELBIT, REL_X);
    ioctl(uifd, UI_SET_RELBIT, REL_Y);
    ioctl(uifd, UI_SET_RELBIT, REL_WHEEL);
    ioctl(uifd, UI_SET_RELBIT, REL_WHEEL_HI_RES);

    // MB1 to MB5 (Including Forward/Back mappings)
    ioctl(uifd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(uifd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(uifd, UI_SET_KEYBIT, BTN_MIDDLE);
    ioctl(uifd, UI_SET_KEYBIT, BTN_SIDE);
    ioctl(uifd, UI_SET_KEYBIT, BTN_EXTRA);
    ioctl(uifd, UI_SET_KEYBIT, BTN_FORWARD);
    ioctl(uifd, UI_SET_KEYBIT, BTN_BACK);

    struct uinput_setup usetup;
    std::memset(&usetup, 0, sizeof(usetup));
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor  = 0x1234;
    usetup.id.product = 0x5678;
    std::strcpy(usetup.name, "Debounced Mouse");

    if (ioctl(uifd, UI_DEV_SETUP, &usetup) < 0) {
        std::cerr << "Error setting up uinput device: " << std::strerror(errno) << std::endl;
        close(fd);
        close(uifd);
        return 1;
    }

    if (ioctl(uifd, UI_DEV_CREATE) < 0) {
        std::cerr << "Error creating uinput device: " << std::strerror(errno) << std::endl;
        close(fd);
        close(uifd);
        return 1;
    }

    if (ioctl(fd, EVIOCGRAB, 1) < 0) {
        std::cerr << "Error grabbing device: " << std::strerror(errno) << std::endl;
        ioctl(uifd, UI_DEV_DESTROY);
        close(fd);
        close(uifd);
        return 1;
    }

    std::cout << "Filtering device: " << device_path << std::endl;
    std::cout << "Scroll Fix: " << (enable_scroll_fix ? "ON" : "OFF") 
              << " | Click Fix: " << (enable_click_fix ? "ON" : "OFF") << std::endl;

    struct input_event ev;
    int current_valid_direction = 0;
    int pending_direction = 0;
    int consecutive_count = 0;

    bool scroll_seen_in_current_batch = false;
    int current_batch_direction = 0;
    double current_batch_time = 0.0;
    double last_scroll_time = 0.0;

    ButtonState button_states[10]; 

    struct input_event batch[64];
    int batch_count = 0;

    while (read(fd, &ev, sizeof(struct input_event)) > 0) {
        bool discard_event = false;

        // Apply debounce only to MB1 through MB5
        if (enable_click_fix && ev.type == EV_KEY && ev.code >= BTN_LEFT && ev.code <= BTN_BACK) {
            int btn_idx = ev.code - BTN_LEFT;
            double now = ev.time.tv_sec + (ev.time.tv_usec / 1000000.0);

            if (ev.value == 1) { 
                if (button_states[btn_idx].ignore_next_press) {
                    discard_event = true;
                    button_states[btn_idx].ignore_next_press = false;
                } else if (now - button_states[btn_idx].last_stable_release_time < click_debounce_sec) {
                    discard_event = true;
                    button_states[btn_idx].ignore_next_release = true;
                } else {
                    button_states[btn_idx].current_state = 1;
                    button_states[btn_idx].last_stable_press_time = now;
                }
            } else if (ev.value == 0) { 
                if (button_states[btn_idx].ignore_next_release) {
                    discard_event = true;
                    button_states[btn_idx].ignore_next_release = false;
                } else if (now - button_states[btn_idx].last_stable_press_time < click_debounce_sec) {
                    discard_event = true;
                    button_states[btn_idx].ignore_next_press = true;
                } else {
                    button_states[btn_idx].current_state = 0;
                    button_states[btn_idx].last_stable_release_time = now;
                }
            }
        }

        // Allow only our explicit capabilities to pass into the batch
        bool is_valid_key = (ev.type == EV_KEY && ev.code >= BTN_LEFT && ev.code <= BTN_BACK);
        bool is_valid_rel = (ev.type == EV_REL && (ev.code == REL_X || ev.code == REL_Y || ev.code == REL_WHEEL || ev.code == REL_WHEEL_HI_RES));
        bool is_sync = (ev.type == EV_SYN);

        if (!discard_event && (is_valid_key || is_valid_rel || is_sync)) {
            batch[batch_count++] = ev;
        }

        if (ev.type == EV_REL && (ev.code == REL_WHEEL || ev.code == REL_WHEEL_HI_RES)) {
            scroll_seen_in_current_batch = true;
            current_batch_direction = (ev.value > 0) ? 1 : -1;
            current_batch_time = ev.time.tv_sec + (ev.time.tv_usec / 1000000.0);
        }

        if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            bool invert_scrolls = false;

            if (enable_scroll_fix && scroll_seen_in_current_batch) {
                if (last_scroll_time > 0.0 && (current_batch_time - last_scroll_time) > idle_timeout_sec) {
                    current_valid_direction = 0;
                    pending_direction = 0;
                    consecutive_count = 0;
                }

                last_scroll_time = current_batch_time;
                int dir = current_batch_direction;

                if (current_valid_direction == 0) {
                    current_valid_direction = dir;
                } else if (dir == current_valid_direction) {
                    consecutive_count = 0;
                } else {
                    if (dir == pending_direction) {
                        consecutive_count++;
                    } else {
                        pending_direction = dir;
                        consecutive_count = 1;
                    }

                    if (consecutive_count >= 2) {
                        current_valid_direction = dir;
                        consecutive_count = 0;
                    } else {
                        invert_scrolls = true;
                    }
                }
            }

            for (int i = 0; i < batch_count; ++i) {
                struct input_event& e = batch[i];
                bool is_scroll = (e.type == EV_REL && (e.code == REL_WHEEL || e.code == REL_WHEEL_HI_RES));

                if (is_scroll && invert_scrolls) {
                    e.value = std::abs(e.value) * current_valid_direction;
                }

                if (write(uifd, &e, sizeof(struct input_event)) < 0) {
                    std::cerr << "Error writing to virtual device" << std::endl;
                    break;
                }
            }

            batch_count = 0;
            scroll_seen_in_current_batch = false;
        }

        if (batch_count >= 64) {
            batch_count = 0; 
        }
    }

    ioctl(fd, EVIOCGRAB, 0);
    ioctl(uifd, UI_DEV_DESTROY);
    close(fd);
    close(uifd);
    return 0;
}
