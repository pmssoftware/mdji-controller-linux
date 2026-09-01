#include <linux/input.h>
#include <linux/uinput.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace {

volatile std::sig_atomic_t running = 1;
void stop(int) { running = 0; }

constexpr std::array<std::uint8_t, 26> kInit{
    0x55, 0xaa, 0x55, 0xaa, 0x1e, 0x00, 0x01, 0x00, 0x00,
    0x01, 0x01, 0x00, 0x80, 0x00, 0x04, 0x04, 0x74, 0x94,
    0x35, 0x00, 0xd8, 0xc0, 0x41, 0x00, 0x30, 0xf6};

constexpr std::array<std::uint8_t, 26> kPing{
    0x55, 0x0d, 0x04, 0x33, 0x0a, 0x03, 0x04, 0x00, 0x40,
    0x00, 0x0e, 0xd0, 0xe3, 0x55, 0x0d, 0x04, 0x33, 0x0a,
    0x0e, 0x05, 0x00, 0x40, 0x06, 0x27, 0x58, 0x35};

class FileDescriptor {
public:
    explicit FileDescriptor(int fd = -1) : fd_(fd) {}
    ~FileDescriptor() { if (fd_ >= 0) ::close(fd_); }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) ::close(fd_);
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }
    int get() const { return fd_; }
private:
    int fd_;
};

void writeAll(int fd, const std::uint8_t* data, std::size_t size) {
    while (size > 0) {
        const ssize_t sent = ::write(fd, data, size);
        if (sent > 0) {
            data += sent;
            size -= static_cast<std::size_t>(sent);
        } else if (sent < 0 && errno == EINTR) {
            continue;
        } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd descriptor{fd, POLLOUT, 0};
            if (poll(&descriptor, 1, 500) >= 0) continue;
            throw std::runtime_error("poll before write failed: " + std::string(std::strerror(errno)));
        } else {
            throw std::runtime_error("write failed: " + std::string(std::strerror(errno)));
        }
    }
}

FileDescriptor openSerial(const std::string& path) {
    FileDescriptor fd(::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK));
    if (fd.get() < 0) {
        throw std::runtime_error("cannot open " + path + ": " + std::strerror(errno));
    }

    termios tty{};
    if (tcgetattr(fd.get(), &tty) != 0) {
        throw std::runtime_error("tcgetattr failed: " + std::string(std::strerror(errno)));
    }
    cfmakeraw(&tty);
    cfsetispeed(&tty, B115200);
    cfsetospeed(&tty, B115200);
    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8 | CLOCAL | CREAD;
    tty.c_cflag &= ~(PARENB | CSTOPB | CRTSCTS);
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 1;
    if (tcsetattr(fd.get(), TCSANOW, &tty) != 0) {
        throw std::runtime_error("tcsetattr failed: " + std::string(std::strerror(errno)));
    }
    tcflush(fd.get(), TCIOFLUSH);
    return fd;
}

std::optional<std::string> autoDetectSerial() {
    const std::array<const char*, 2> prefixes{"ttyACM", "ttyUSB"};
    for (const char* prefix : prefixes) {
        for (const auto& entry : std::filesystem::directory_iterator("/dev")) {
            const std::string name = entry.path().filename().string();
            if (name.rfind(prefix, 0) == 0) return entry.path().string();
        }
    }
    return std::nullopt;
}

class VirtualJoystick {
public:
    VirtualJoystick() : fd_(::open("/dev/uinput", O_WRONLY | O_NONBLOCK)) {
        if (fd_.get() < 0) {
            throw std::runtime_error("cannot open /dev/uinput: " + std::string(std::strerror(errno)));
        }
        checkedIoctl(UI_SET_EVBIT, EV_KEY);
        for (int key = BTN_JOYSTICK; key < BTN_JOYSTICK + 9; ++key) checkedIoctl(UI_SET_KEYBIT, key);
        checkedIoctl(UI_SET_EVBIT, EV_ABS);

        setupAxis(ABS_X, 0, 32767);
        setupAxis(ABS_Y, 0, 32767);
        setupAxis(ABS_Z, 0, 32767);
        setupAxis(ABS_RX, 0, 32767);
        setupAxis(ABS_RY, 0, 32767);
        setupAxis(ABS_RZ, 0, 32767);

        uinput_setup setup{};
        std::snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "DJI Phantom 3 Remote Controller");
        setup.id.bustype = BUS_USB;
        setup.id.vendor = 0x2ca3;
        setup.id.product = 0x001f;
        setup.id.version = 1;
        if (ioctl(fd_.get(), UI_DEV_SETUP, &setup) < 0 || ioctl(fd_.get(), UI_DEV_CREATE) < 0) {
            throw std::runtime_error("creating uinput device failed: " + std::string(std::strerror(errno)));
        }
        created_ = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    ~VirtualJoystick() { if (created_) ioctl(fd_.get(), UI_DEV_DESTROY); }

    void update(const std::array<int, 6>& axes, const std::array<bool, 9>& buttons) {
        constexpr std::array<int, 6> codes{ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ};
        for (std::size_t i = 0; i < axes.size(); ++i) emit(EV_ABS, codes[i], axes[i]);
        for (std::size_t i = 0; i < buttons.size(); ++i) emit(EV_KEY, BTN_JOYSTICK + static_cast<int>(i), buttons[i]);
        emit(EV_SYN, SYN_REPORT, 0);
    }

private:
    void checkedIoctl(unsigned long request, int value) {
        if (ioctl(fd_.get(), request, value) < 0) {
            throw std::runtime_error("uinput ioctl failed: " + std::string(std::strerror(errno)));
        }
    }
    void setupAxis(int code, int minimum, int maximum) {
        checkedIoctl(UI_SET_ABSBIT, code);
        uinput_abs_setup axis{};
        axis.code = static_cast<__u16>(code);
        axis.absinfo.minimum = minimum;
        axis.absinfo.maximum = maximum;
        axis.absinfo.flat = 128;
        axis.absinfo.fuzz = 4;
        if (ioctl(fd_.get(), UI_ABS_SETUP, &axis) < 0) {
            throw std::runtime_error("axis setup failed: " + std::string(std::strerror(errno)));
        }
    }
    void emit(int type, int code, int value) {
        input_event event{};
        event.type = static_cast<__u16>(type);
        event.code = static_cast<__u16>(code);
        event.value = value;
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&event);
        writeAll(fd_.get(), bytes, sizeof(event));
    }
    FileDescriptor fd_;
    bool created_ = false;
};

std::int16_t littleEndian16(const std::uint8_t* p) {
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(p[0]) |
                                     (static_cast<std::uint16_t>(p[1]) << 8));
}

int stickAxis(std::int16_t raw) { return std::clamp((static_cast<int>(raw) - 750) * 12, 0, 32767); }

struct DecoderState {
    std::uint8_t previousWheel = 0;
    bool haveWheel = false;
};

void decodeReport(const std::uint8_t* p, VirtualJoystick& joystick, DecoderState& state, bool verbose) {
    const int rightVertical = littleEndian16(p + 16);
    const int rightHorizontal = littleEndian16(p + 18);
    const int leftVertical = littleEndian16(p + 20);
    const int leftHorizontal = littleEndian16(p + 22);
    const int camera = littleEndian16(p + 24);
    const std::uint8_t wheel = p[26];
    const std::uint8_t flight = p[28];
    const std::uint8_t bits = p[29];

    const int flightMode = flight >= 32 ? 2 : flight >= 16 ? 1 : 0;
    const int flightModeAxis = flightMode == 2 ? 32767 : flightMode * 16384;
    std::array<int, 6> axes{
        stickAxis(leftHorizontal), stickAxis(leftVertical), stickAxis(rightHorizontal),
        stickAxis(rightVertical), std::clamp(camera * 8, 0, 32767), flightModeAxis};
    std::array<bool, 9> buttons{};
    for (int i = 0; i < 7; ++i) buttons[i] = (bits & (1u << (i + 1))) != 0;
    if (state.haveWheel) {
        const std::int8_t delta = static_cast<std::int8_t>(wheel - state.previousWheel);
        buttons[7] = delta > 0;
        buttons[8] = delta < 0;
    }
    state.previousWheel = wheel;
    state.haveWheel = true;
    joystick.update(axes, buttons);

    if (verbose) {
        std::cout << "axes " << axes[0] << ' ' << axes[1] << ' ' << axes[2] << ' '
                  << axes[3] << ' ' << axes[4] << ' ' << axes[5] << " buttons ";
        for (bool button : buttons) std::cout << button;
        std::cout << '\r' << std::flush;
    }
}

void consumeFrames(std::vector<std::uint8_t>& buffer, VirtualJoystick& joystick,
                   DecoderState& state, bool verbose) {
    while (true) {
        const auto marker = std::find(buffer.begin(), buffer.end(), 0x55);
        buffer.erase(buffer.begin(), marker);
        if (buffer.size() < 3) return;

        // DJI DUML packets encode their total length in the low ten bits of bytes 1-2.
        const std::size_t length = static_cast<std::size_t>(buffer[1]) |
                                   ((static_cast<std::size_t>(buffer[2]) & 0x03) << 8);
        if (length < 13 || length > 1023) {
            buffer.erase(buffer.begin());
            continue;
        }
        if (buffer.size() < length) return;
        if (length == 58) decodeReport(buffer.data(), joystick, state, verbose);
        buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(length));
    }
}

void usage(const char* program) {
    std::cout << "Usage: " << program << " [--device /dev/ttyACM0] [--verbose]\n";
}

} // namespace

int main(int argc, char** argv) try {
    std::optional<std::string> device;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--device" && i + 1 < argc) device = argv[++i];
        else if (arg == "--verbose") verbose = true;
        else if (arg == "--help") { usage(argv[0]); return 0; }
        else { usage(argv[0]); return 2; }
    }
    if (!device) device = autoDetectSerial();
    if (!device) throw std::runtime_error("no serial controller found; pass --device /dev/ttyACM0");

    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    auto serial = openSerial(*device);
    VirtualJoystick joystick;
    std::cout << "Connected to " << *device << ". Press Ctrl+C to stop.\n";
    writeAll(serial.get(), kInit.data(), kInit.size());

    std::vector<std::uint8_t> buffer;
    buffer.reserve(2048);
    DecoderState state;
    auto nextPing = std::chrono::steady_clock::now();
    while (running) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextPing) {
            writeAll(serial.get(), kPing.data(), kPing.size());
            nextPing = now + std::chrono::milliseconds(10);
        }
        pollfd descriptor{serial.get(), POLLIN, 0};
        const int result = poll(&descriptor, 1, 5);
        if (result < 0 && errno != EINTR) throw std::runtime_error("serial poll failed");
        if (result > 0 && (descriptor.revents & POLLIN)) {
            std::array<std::uint8_t, 512> chunk{};
            const ssize_t count = ::read(serial.get(), chunk.data(), chunk.size());
            if (count > 0) {
                buffer.insert(buffer.end(), chunk.begin(), chunk.begin() + count);
                consumeFrames(buffer, joystick, state, verbose);
            }
        }
    }
    if (verbose) std::cout << '\n';
    return 0;
} catch (const std::exception& error) {
    std::cerr << "mdji-controller: " << error.what() << '\n';
    return 1;
}
