// tools/vmouse.c — a virtual mouse on /dev/uinput, for measuring pointer
// input to photon on the real screen (tools/photon.sh).
//
// An ABSOLUTE pointer (like a VM's USB tablet): libinput maps it onto the
// screen, so `m X Y` puts the pointer exactly at screen pixel X,Y -- no
// acceleration to undo. Events go through the kernel, libinput and the
// compositor like a real mouse's, and carry the kernel's timestamp, so the
// latency clock starts where a real one would.
//
//   cc -O2 -o vmouse tools/vmouse.c
//   printf 'm 100 200\nd\nm 150 200\nu\nw 3\ns 30\n' | ./vmouse 3440 1440
//
// Commands, one per line:  m X Y  (move)   d / u  (left down / up)
//                          w N    (wheel N notches, + = down)   s MS  (sleep)
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static int fd = -1;

static void emit(int type, int code, int value) {
    struct input_event e;
    memset(&e, 0, sizeof e);
    e.type = (unsigned short)type;
    e.code = (unsigned short)code;
    e.value = value;
    if (write(fd, &e, sizeof e) != (ssize_t)sizeof e) perror("vmouse: write");
}
static void syn(void) { emit(EV_SYN, SYN_REPORT, 0); }

static void sleep_ms(long ms) {
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&t, &t) != 0) {}
}

int main(int argc, char** argv) {
    const int w = argc > 1 ? atoi(argv[1]) : 1920, h = argc > 2 ? atoi(argv[2]) : 1080;
    fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { perror("vmouse: /dev/uinput"); return 1; }
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);
    ioctl(fd, UI_SET_EVBIT, EV_REL);
    ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_ABSBIT, ABS_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_Y);
    struct uinput_abs_setup ax = {.code = ABS_X, .absinfo = {.minimum = 0, .maximum = w - 1}};
    struct uinput_abs_setup ay = {.code = ABS_Y, .absinfo = {.minimum = 0, .maximum = h - 1}};
    ioctl(fd, UI_ABS_SETUP, &ax);
    ioctl(fd, UI_ABS_SETUP, &ay);
    struct uinput_setup us;
    memset(&us, 0, sizeof us);
    us.id.bustype = BUS_USB;
    us.id.vendor = 0x1234;
    us.id.product = 0x5678;
    snprintf(us.name, sizeof us.name, "hand photon vmouse");
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) { perror("vmouse: create"); return 1; }
    sleep_ms(400);   // let libinput and the compositor pick the device up

    char line[128];
    while (fgets(line, sizeof line, stdin)) {
        int a = 0, b = 0;
        switch (line[0]) {
            case 'm': if (sscanf(line + 1, "%d %d", &a, &b) == 2) { emit(EV_ABS, ABS_X, a); emit(EV_ABS, ABS_Y, b); syn(); } break;
            case 'd': emit(EV_KEY, BTN_LEFT, 1); syn(); break;
            case 'u': emit(EV_KEY, BTN_LEFT, 0); syn(); break;
            case 'w': if (sscanf(line + 1, "%d", &a) == 1) { emit(EV_REL, REL_WHEEL, -a); syn(); } break;
            case 's': if (sscanf(line + 1, "%d", &a) == 1) sleep_ms(a); break;
            default: break;
        }
    }
    sleep_ms(100);
    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
    return 0;
}
