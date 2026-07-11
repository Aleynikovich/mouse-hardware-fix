#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <cstring>
#include <cmath>
#include <string>

const char* DEVICE_PATH = "/dev/input/by-id/usb-Logitech_USB_Receiver-if02-event-mouse";
const double DEFAULT_IDLE_TIMEOUT_SEC = 0.400; 

int main(int argc, char* argv[]) {
    double idle_timeout_sec = DEFAULT_IDLE_TIMEOUT_SEC;

    // Parse timeout argument if passed
    if (argc > 1) {
        try {
            idle_timeout_sec = std::stod(argv[1]);
            if (idle_timeout_sec <= 0.0) {
                std::cerr << "Timeout must be positive. Using default: " << DEFAULT_IDLE_TIMEOUT_SEC << "s" << std::endl;
                idle_timeout_sec = DEFAULT_IDLE_TIMEOUT_SEC;
            }
        } catch (const std::exception& e) {
            std::cerr << "Invalid argument format. Using default: " << DEFAULT_IDLE_TIMEOUT_SEC << "s" << std::endl;
            idle_timeout_sec = DEFAULT_IDLE_TIMEOUT_SEC;
        }
    }

    int fd = open(DEVICE_PATH, O_RDONLY);
    if (fd < 0) {
        std::cerr << "Error opening device (run with sudo): " << std::strerror(errno) << std::endl;
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

    ioctl(uifd, UI_SET_RELBIT, REL_X);
    ioctl(uifd, UI_SET_RELBIT, REL_Y);
    ioctl(uifd, UI_SET_RELBIT, REL_WHEEL);
    ioctl(uifd, UI_SET_RELBIT, REL_WHEEL_HI_RES);

    for (int btn = BTN_LEFT; btn <= BTN_TASK; ++btn) {
        ioctl(uifd, UI_SET_KEYBIT, btn);
    }

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

    std::cout << "Filtering scroll events. Active timeout: " << idle_timeout_sec << "s" << std::endl;

    struct input_event ev;
    int current_valid_direction = 0;
    int pending_direction = 0;
    int consecutive_count = 0;

    bool scroll_seen_in_current_batch = false;
    int current_batch_direction = 0;
    double current_batch_time = 0.0;
    double last_scroll_time = 0.0;

    struct input_event batch[64];
    int batch_count = 0;

    while (read(fd, &ev, sizeof(struct input_event)) > 0) {
        batch[batch_count++] = ev;

        if (ev.type == EV_REL && (ev.code == REL_WHEEL || ev.code == REL_WHEEL_HI_RES)) {
            scroll_seen_in_current_batch = true;
            current_batch_direction = (ev.value > 0) ? 1 : -1;
            current_batch_time = ev.time.tv_sec + (ev.time.tv_usec / 1000000.0);
        }

        if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            bool invert_scrolls = false;

            if (scroll_seen_in_current_batch) {
                if (last_scroll_time > 0.0 && (current_batch_time - last_scroll_time) > idle_timeout_sec) {
                    current_valid_direction = 0;
                    pending_direction = 0;
                    consecutive_count = 0;
                    std::cout << "Idle timeout reached. Lock reset." << std::endl;
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
