#include "flink_protocol.hpp"
#include "flink_sync.hpp"
#include "flink_config.hpp"

#ifndef _WIN32
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <functional>
#include <thread>
#include <chrono>
#include <atomic>
#ifdef FLINK_HAS_X11
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#ifdef FLINK_HAS_XTEST
#include <X11/extensions/XTest.h>
#endif
#endif
#ifdef FLINK_HAS_ALSA
#include <alsa/asoundlib.h>
#endif

namespace {

constexpr const char* kCyan = "\033[36m";
constexpr const char* kGreen = "\033[32m";
constexpr const char* kYellow = "\033[33m";
constexpr const char* kRed = "\033[31m";
constexpr const char* kReset = "\033[0m";

bool receiveAll(int socketHandle, void* data, std::size_t size)
{
    auto* bytes = static_cast<char*>(data);
    while (size > 0) {
        const ssize_t received = recv(socketHandle, bytes, size, 0);
        if (received <= 0) return false;
        bytes += received;
        size -= static_cast<std::size_t>(received);
    }
    return true;
}

bool sendAll(int socketHandle, const void* data, std::size_t size)
{
    const auto* bytes = static_cast<const char*>(data);
    while (size > 0) {
        const ssize_t sent = send(socketHandle, bytes, size, MSG_NOSIGNAL);
        if (sent <= 0) return false;
        bytes += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

bool sendPacket(int socketHandle, flink::PacketType type, std::uint32_t sequence,
                const std::vector<std::uint8_t>& payload = {})
{
    const flink::PacketHeader header{
        flink::kMagic, flink::kVersion, static_cast<std::uint16_t>(type),
        static_cast<std::uint32_t>(payload.size()), sequence
    };
    return sendAll(socketHandle, &header, sizeof(header)) &&
           (payload.empty() || sendAll(socketHandle, payload.data(), payload.size()));
}

#ifdef FLINK_HAS_X11
bool sendVideoFrame(int socketHandle, std::uint32_t sequence, Display* display,
                    Window rootWindow, int screenWidth, int screenHeight,
                    const flink::Config& config)
{
    const int width = config.screenWidth > 0 ? std::min(config.screenWidth, screenWidth) : screenWidth;
    const int height = config.screenHeight > 0 ? std::min(config.screenHeight, screenHeight) : screenHeight;
    XImage* image = XGetImage(display, rootWindow, 0, 0, width, height, AllPlanes, ZPixmap);
    if (image == nullptr) return false;
    std::vector<std::uint8_t> payload(sizeof(flink::VideoFrameInfo) +
                                      static_cast<std::size_t>(width) * height * 4);
    const flink::VideoFrameInfo info{
        static_cast<std::uint16_t>(width), static_cast<std::uint16_t>(height), 4, 0
    };
    std::memcpy(payload.data(), &info, sizeof(info));
    auto* pixels = payload.data() + sizeof(info);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto pixel = XGetPixel(image, x, y);
            const auto offset = (static_cast<std::size_t>(y) * width + x) * 4;
            pixels[offset + 0] = static_cast<std::uint8_t>(pixel & 0xff);
            pixels[offset + 1] = static_cast<std::uint8_t>((pixel >> 8) & 0xff);
            pixels[offset + 2] = static_cast<std::uint8_t>((pixel >> 16) & 0xff);
            pixels[offset + 3] = 255;
        }
    }
    XDestroyImage(image);
    return sendPacket(socketHandle, flink::PacketType::VideoFrame, sequence, payload);
}
#endif

#if defined(FLINK_HAS_X11) && defined(FLINK_HAS_XTEST)
void injectInput(Display* display, const flink::InputInfo& input)
{
    if (input.mouseDx != 0 || input.mouseDy != 0) {
        Window root = DefaultRootWindow(display);
        Window current = root;
        int rootX = 0, rootY = 0, winX = 0, winY = 0;
        unsigned int mask = 0;
        XQueryPointer(display, root, &root, &current, &rootX, &rootY, &winX, &winY, &mask);
        XTestFakeMotionEvent(display, -1, rootX + input.mouseDx, rootY + input.mouseDy, CurrentTime);
    }
    if (input.key != 0) {
        XTestFakeKeyEvent(display, input.key, input.keyDown != 0, CurrentTime);
    }
    if (input.mouseButton != 0) {
        XTestFakeButtonEvent(display, input.mouseButton, input.buttonDown != 0, CurrentTime);
    }
    XFlush(display);
}
#endif

#ifdef FLINK_HAS_ALSA
bool sendAudioFrame(int socketHandle, std::uint32_t sequence, snd_pcm_t* pcm)
{
    constexpr int frames = 960;
    std::vector<std::int16_t> samples(frames * 2);
    const auto count = snd_pcm_readi(pcm, samples.data(), frames);
    if (count <= 0) {
        snd_pcm_prepare(pcm);
        return true;
    }
    std::vector<std::uint8_t> payload(sizeof(flink::AudioFrameInfo) +
                                      static_cast<std::size_t>(count) * 2 * sizeof(std::int16_t));
    const flink::AudioFrameInfo info{48000, 2, 2};
    std::memcpy(payload.data(), &info, sizeof(info));
    std::memcpy(payload.data() + sizeof(info), samples.data(),
                static_cast<std::size_t>(count) * 2 * sizeof(std::int16_t));
    return sendPacket(socketHandle, flink::PacketType::AudioFrame, sequence, payload);
}
#endif

bool receivePacket(int socketHandle, flink::PacketHeader& header,
                   std::vector<std::uint8_t>& payload)
{
    if (!receiveAll(socketHandle, &header, sizeof(header)) ||
        header.magic != flink::kMagic || header.version != flink::kVersion ||
        header.payloadSize > flink::kMaxFileSize) {
        return false;
    }
    payload.resize(header.payloadSize);
    return payload.empty() || receiveAll(socketHandle, payload.data(), payload.size());
}

bool receiveExpected(int socketHandle, flink::PacketHeader& header,
                     std::vector<std::uint8_t>& payload, flink::PacketType expected,
                     const std::function<void(const std::string&)>& report)
{
    if (!receivePacket(socketHandle, header, payload)) {
        report("FLink: paquet illisible ou version incompatible");
        return false;
    }
    const auto expectedType = static_cast<std::uint16_t>(expected);
    if (header.type != expectedType) {
        report("FLink: type inattendu, recu " + std::to_string(header.type) +
               ", attendu " + std::to_string(expectedType) +
               ", version " + std::to_string(header.version) +
               ", taille " + std::to_string(header.payloadSize));
        return false;
    }
    return true;
}

void append(std::vector<std::uint8_t>& data, const void* value, std::size_t size)
{
    const auto* bytes = static_cast<const std::uint8_t*>(value);
    data.insert(data.end(), bytes, bytes + size);
}

template <typename T>
void appendValue(std::vector<std::uint8_t>& data, T value) { append(data, &value, sizeof(value)); }

std::vector<std::uint8_t> encodeManifest(const std::vector<flink::FileEntry>& entries)
{
    std::vector<std::uint8_t> data;
    appendValue(data, static_cast<std::uint32_t>(entries.size()));
    for (const auto& entry : entries) {
        const auto length = static_cast<std::uint16_t>(entry.path.size());
        appendValue(data, length);
        appendValue(data, entry.size);
        appendValue(data, entry.modified);
        append(data, entry.path.data(), length);
    }
    return data;
}

bool decodeManifest(const std::vector<std::uint8_t>& data, std::vector<flink::FileEntry>& entries)
{
    std::size_t offset = 0;
    auto read = [&](void* target, std::size_t size) {
        if (offset + size > data.size()) return false;
        std::memcpy(target, data.data() + offset, size);
        offset += size;
        return true;
    };
    std::uint32_t count = 0;
    if (!read(&count, sizeof(count)) || count > 100000) return false;
    entries.clear();
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint16_t length = 0;
        flink::FileEntry entry{};
        if (!read(&length, sizeof(length)) || length > 4096 ||
            !read(&entry.size, sizeof(entry.size)) ||
            !read(&entry.modified, sizeof(entry.modified))) return false;
        entry.path.resize(length);
        if (!read(entry.path.data(), length) || !flink::safeRelativePath(entry.path)) return false;
        entries.push_back(std::move(entry));
    }
    return offset == data.size();
}

std::vector<std::uint8_t> encodeFile(const std::string& path, const std::vector<std::uint8_t>& content)
{
    std::vector<std::uint8_t> data;
    const auto length = static_cast<std::uint16_t>(path.size());
    appendValue(data, length);
    append(data, path.data(), length);
    append(data, content.data(), content.size());
    return data;
}

bool decodeFile(const std::vector<std::uint8_t>& data, std::string& path,
                std::vector<std::uint8_t>& content)
{
    if (data.size() < sizeof(std::uint16_t)) return false;
    std::uint16_t length = 0;
    std::memcpy(&length, data.data(), sizeof(length));
    if (length == 0 || data.size() < sizeof(length) + length) return false;
    path.assign(reinterpret_cast<const char*>(data.data() + sizeof(length)), length);
    if (!flink::safeRelativePath(path)) return false;
    content.assign(data.begin() + sizeof(length) + length, data.end());
    return true;
}

const flink::FileEntry* findEntry(const std::vector<flink::FileEntry>& entries,
                                  const std::string& path)
{
    const auto found = std::find_if(entries.begin(), entries.end(),
        [&](const auto& entry) { return entry.path == path; });
    return found == entries.end() ? nullptr : &*found;
}

} // namespace

int main(int argc, char** argv)
{
    const std::filesystem::path root =
        argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::current_path() / "FLink";
    const auto config = flink::loadConfig((root / "flink.conf.txt").string());
    std::filesystem::create_directories(root);
    const int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) return 1;
    int reuse = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(flink::kPort);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
        listen(server, 1) < 0) {
        close(server);
        return 1;
    }

    std::ofstream log(root / "agent.log", std::ios::app);
    const auto report = [&](const std::string& message) {
        const char* color = kCyan;
        if (message.find("echec") != std::string::npos ||
            message.find("indisponible") != std::string::npos) color = kRed;
        else if (message.find("capture") != std::string::npos ||
                 message.find("envoie") != std::string::npos ||
                 message.find("recu") != std::string::npos) color = kGreen;
        else if (message.find("attente") != std::string::npos) color = kYellow;
        std::cout << color << "[FLink] " << message << kReset << std::endl;
        if (log) log << message << '\n';
    };
    report("FLink agent actif sur le port " + std::to_string(flink::kPort) +
           ", dossier " + root.string());
    report("FLink: config " + (flink::hasConfig((root / "flink.conf.txt").string())
        ? root.string() + "/flink.conf.txt"
        : "defaut") + ", resolution demandee " +
        std::to_string(config.screenWidth) + "x" + std::to_string(config.screenHeight) +
        ", FPS " + std::to_string(config.fps));
#ifndef FLINK_HAS_X11
    report("FLink: capture X11 indisponible (installez libx11-dev)");
#endif
#ifndef FLINK_HAS_ALSA
    report("FLink: capture audio indisponible (installez libasound2-dev)");
#endif
#ifndef FLINK_HAS_XTEST
    report("FLink: injection clavier/souris indisponible (installez libxtst-dev)");
#endif
    while (true) {
        const int client = accept(server, nullptr, nullptr);
        if (client < 0) continue;
        report("FLink: client connecte");
        flink::PacketHeader request{};
        std::vector<std::uint8_t> payload;
        if (!receiveExpected(client, request, payload, flink::PacketType::Hello, report) ||
            !receiveExpected(client, request, payload, flink::PacketType::Ping, report) ||
            !sendPacket(client, flink::PacketType::Pong, request.sequence) ||
            !receiveExpected(client, request, payload, flink::PacketType::Manifest, report)) {
            report("FLink: negociation invalide");
            close(client);
            continue;
        }
        report("FLink: HELLO/PING/manifest recus");

        std::vector<flink::FileEntry> remoteEntries;
        if (!decodeManifest(payload, remoteEntries)) {
            report("FLink: manifeste Windows invalide");
            close(client);
            continue;
        }
        const auto localEntries = flink::manifest(root);
        report("FLink: " + std::to_string(localEntries.size()) +
               " fichier(s) local(aux), " + std::to_string(remoteEntries.size()) +
               " distant(s)");
        if (!sendPacket(client, flink::PacketType::Manifest, 3, encodeManifest(localEntries))) {
            close(client);
            continue;
        }

        std::uint32_t sequence = 4;
        for (const auto& local : localEntries) {
            const auto* remote = findEntry(remoteEntries, local.path);
            if (remote != nullptr && remote->modified >= local.modified) continue;
            std::vector<std::uint8_t> content;
            if (flink::readFile(root, local.path, content) &&
                !sendPacket(client, flink::PacketType::FilePut, sequence++,
                            encodeFile(local.path, content))) {
                report("FLink: echec envoi " + local.path);
                break;
            }
            report("FLink: envoie " + local.path);
        }
        sendPacket(client, flink::PacketType::SyncReady, sequence++);

        while (receivePacket(client, request, payload)) {
            if (request.type == static_cast<std::uint16_t>(flink::PacketType::SyncDone)) break;
            if (request.type != static_cast<std::uint16_t>(flink::PacketType::FilePut)) break;
            std::string path;
            std::vector<std::uint8_t> content;
            if (!decodeFile(payload, path, content) || !flink::writeFile(root, path, content)) {
                report("FLink: echec ecriture " + path);
                break;
            }
            report("FLink: recu " + path);
        }
        report("FLink: synchronisation terminee, demarrage du flux");
#ifdef FLINK_HAS_X11
        Display* display = XOpenDisplay(nullptr);
        Window rootWindow = 0;
        int screenWidth = 0;
        int screenHeight = 0;
        if (display != nullptr) {
            rootWindow = DefaultRootWindow(display);
            const auto screen = DefaultScreen(display);
            screenWidth = DisplayWidth(display, screen);
            screenHeight = DisplayHeight(display, screen);
            report("FLink: X11 capture " + std::to_string(screenWidth) + "x" +
                   std::to_string(screenHeight));
            const int captureWidth = config.screenWidth > 0
                ? std::min(config.screenWidth, screenWidth) : screenWidth;
            const int captureHeight = config.screenHeight > 0
                ? std::min(config.screenHeight, screenHeight) : screenHeight;
            report("FLink: resolution envoyee " + std::to_string(captureWidth) + "x" +
                   std::to_string(captureHeight));
        } else {
            report("FLink: XOpenDisplay a echoue");
        }
#endif
#ifdef FLINK_HAS_ALSA
        snd_pcm_t* pcm = nullptr;
        const auto audioResult = snd_pcm_open(&pcm, "default", SND_PCM_STREAM_CAPTURE,
                                              SND_PCM_NONBLOCK);
        if (audioResult == 0) {
            snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                               2, 48000, 1, 20000);
            report("FLink: capture audio ALSA active");
        } else {
            report("FLink: ALSA indisponible: " + std::string(snd_strerror(audioResult)));
        }
#endif
        std::atomic<bool> streaming{true};
        std::thread streamThread([&] {
        std::uint32_t streamSequence = 1000;
        const auto frameDelay = std::chrono::milliseconds(1000 / config.fps);
        for (int frame = 0; streaming; ++frame) {
#ifdef FLINK_HAS_X11
            if (display != nullptr &&
                !sendVideoFrame(client, streamSequence++, display, rootWindow,
                                screenWidth, screenHeight, config)) break;
#endif
#ifdef FLINK_HAS_ALSA
            if (pcm != nullptr && !sendAudioFrame(client, streamSequence++, pcm)) break;
#endif
            std::this_thread::sleep_for(frameDelay);
        }
        streaming = false;
        });
        while (streaming && receivePacket(client, request, payload)) {
            if (request.type != static_cast<std::uint16_t>(flink::PacketType::Input) ||
                payload.size() != sizeof(flink::InputInfo)) continue;
            flink::InputInfo input{};
            std::memcpy(&input, payload.data(), sizeof(input));
#if defined(FLINK_HAS_X11) && defined(FLINK_HAS_XTEST)
            if (display != nullptr && config.input) injectInput(display, input);
#endif
            report("FLink: input recu");
        }
        streaming = false;
        shutdown(client, SHUT_RDWR);
        streamThread.join();
#ifdef FLINK_HAS_ALSA
        if (pcm != nullptr) snd_pcm_close(pcm);
#endif
#ifdef FLINK_HAS_X11
        if (display != nullptr) XCloseDisplay(display);
#endif
        close(client);
    }
}
#else
int main() { return 0; }
#endif
