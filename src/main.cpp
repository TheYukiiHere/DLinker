#include "flink_protocol.hpp"
#include "flink_sync.hpp"
#include "flink_config.hpp"

#include <SDL3/SDL.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using Socket = int;
constexpr Socket kInvalidSocket = -1;
#endif

#include <filesystem>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr const char* kBlue = "\x1b[36m";
constexpr const char* kGreen = "\x1b[32m";
constexpr const char* kYellow = "\x1b[33m";
constexpr const char* kRed = "\x1b[31m";
constexpr const char* kReset = "\x1b[0m";

void enableColors()
{
#ifdef _WIN32
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (output != INVALID_HANDLE_VALUE && GetConsoleMode(output, &mode)) {
        SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
#endif
}

void debugMessage(const char* color, const std::string& message)
{
    std::cout << color << "[FLink] " << message << kReset << '\n';
}

void closeSocket(Socket socket)
{
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

bool sendAll(Socket socket, const void* data, std::size_t size)
{
    const auto* bytes = static_cast<const char*>(data);
    while (size > 0) {
#ifdef _WIN32
        const int sent = send(socket, bytes, static_cast<int>(size), 0);
#else
        const ssize_t sent = send(socket, bytes, size, 0);
#endif
        if (sent <= 0) {
            return false;
        }
        bytes += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

bool receiveAll(Socket socket, void* data, std::size_t size)
{
    auto* bytes = static_cast<char*>(data);
    while (size > 0) {
#ifdef _WIN32
        const int received = recv(socket, bytes, static_cast<int>(size), 0);
#else
        const ssize_t received = recv(socket, bytes, size, 0);
#endif
        if (received <= 0) {
            return false;
        }
        bytes += received;
        size -= static_cast<std::size_t>(received);
    }
    return true;
}

bool sendPacket(Socket socket, flink::PacketType type, std::uint32_t sequence,
                const std::vector<std::uint8_t>& payload = {})
{
    const flink::PacketHeader header{
        flink::kMagic, flink::kVersion, static_cast<std::uint16_t>(type),
        static_cast<std::uint32_t>(payload.size()), sequence
    };
    return sendAll(socket, &header, sizeof(header)) &&
           (payload.empty() || sendAll(socket, payload.data(), payload.size()));
}

bool receivePacket(Socket socket, flink::PacketHeader& header,
                   std::vector<std::uint8_t>& payload)
{
    if (!receiveAll(socket, &header, sizeof(header)) ||
        header.payloadSize > flink::kMaxFileSize) return false;
    payload.resize(header.payloadSize);
    return (payload.empty() || receiveAll(socket, payload.data(), payload.size())) &&
           header.magic == flink::kMagic &&
           header.version == flink::kVersion &&
           header.payloadSize == payload.size();
}

bool streamPacketReady(Socket socket)
{
#ifdef _WIN32
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(socket, &readSet);
    timeval timeout{0, 0};
    return select(0, &readSet, nullptr, nullptr, &timeout) > 0;
#else
    return false;
#endif
}

Socket connectToLinux(const flink::Config& config)
{
    Socket socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socketHandle == kInvalidSocket) {
        return kInvalidSocket;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(config.port));
    if (inet_pton(AF_INET, config.ip.c_str(), &address.sin_addr) != 1 ||
        connect(socketHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        closeSocket(socketHandle);
        return kInvalidSocket;
    }

    return socketHandle;
}

bool sendInput(Socket socket, const flink::InputInfo& input, std::uint32_t sequence)
{
    const auto* begin = reinterpret_cast<const std::uint8_t*>(&input);
    const std::vector<std::uint8_t> payload(begin, begin + sizeof(input));
    return sendPacket(socket, flink::PacketType::Input, sequence, payload);
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

bool synchronizeFlink(Socket socketHandle, const std::filesystem::path& root)
{
    std::ofstream log(root / "sync.log", std::ios::app);
    const auto report = [&](const std::string& message) {
        if (log) log << message << '\n';
        std::cout << message << '\n';
    };
    report("FLink: dossier local " + std::filesystem::absolute(root).string());
    const auto localEntries = flink::manifest(root);
    report("FLink: " + std::to_string(localEntries.size()) + " fichier(s) local(aux)");
    std::vector<std::uint8_t> payload;
    flink::PacketHeader response{};
    if (!sendPacket(socketHandle, flink::PacketType::Hello, 1)) {
        report("FLink: echec envoi HELLO");
        return false;
    }
    if (!sendPacket(socketHandle, flink::PacketType::Ping, 2)) {
        report("FLink: echec envoi PING");
        return false;
    }
    if (!receivePacket(socketHandle, response, payload)) {
        report("FLink: aucune reponse PONG");
        return false;
    }
    if (response.type != static_cast<std::uint16_t>(flink::PacketType::Pong)) {
        report("FLink: reponse inattendue au PING: type " + std::to_string(response.type));
        return false;
    }
    report("FLink: PONG recu");
    if (!sendPacket(socketHandle, flink::PacketType::Manifest, 3,
                    encodeManifest(localEntries))) {
        report("FLink: echec envoi manifeste");
        return false;
    }
    if (!receivePacket(socketHandle, response, payload)) {
        report("FLink: aucun manifeste Lubuntu recu");
        return false;
    }
    if (response.type != static_cast<std::uint16_t>(flink::PacketType::Manifest)) {
        report("FLink: reponse inattendue au manifeste: type " +
               std::to_string(response.type));
        return false;
    }

    std::vector<flink::FileEntry> remoteEntries;
    if (!decodeManifest(payload, remoteEntries)) {
        report("FLink: manifeste distant invalide");
        return false;
    }
    report("FLink: " + std::to_string(remoteEntries.size()) + " fichier(s) distant(s)");
    while (receivePacket(socketHandle, response, payload)) {
        if (response.type == static_cast<std::uint16_t>(flink::PacketType::SyncReady)) break;
        if (response.type != static_cast<std::uint16_t>(flink::PacketType::FilePut)) {
            report("FLink: paquet fichier inattendu");
            return false;
        }
        std::string path;
        std::vector<std::uint8_t> content;
        if (!decodeFile(payload, path, content) || !flink::writeFile(root, path, content)) {
            report("FLink: echec d'ecriture distante vers " + path);
            return false;
        }
        report("FLink: recu " + path);
    }

    std::uint32_t sequence = 10;
    for (const auto& local : flink::manifest(root)) {
        const auto* remote = findEntry(remoteEntries, local.path);
        if (remote != nullptr && remote->modified >= local.modified) continue;
        std::vector<std::uint8_t> content;
        if (!flink::readFile(root, local.path, content) ||
            !sendPacket(socketHandle, flink::PacketType::FilePut, sequence++,
                        encodeFile(local.path, content))) {
            report("FLink: echec d'envoi de " + local.path);
            return false;
        }
        report("FLink: envoye " + local.path);
    }
    return sendPacket(socketHandle, flink::PacketType::SyncDone, sequence);
}

} // namespace

int main(int argc, char** argv)
{
    enableColors();
#ifdef _WIN32
    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup a echoue.\n";
        return 1;
    }
#endif

    const std::filesystem::path root =
        argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::current_path() / "FLink";
    std::filesystem::create_directories(root);
    const std::string requestedConfig = argc > 2 ? argv[2] : "";
    const auto configPath = flink::resolveConfigPath(requestedConfig, root.string());
    const auto config = flink::loadConfig(configPath);
    debugMessage(kBlue, "configuration " + (configPath.empty()
        ? "defaut" : std::filesystem::absolute(configPath).string()) +
        ", cible " + config.ip + ":" + std::to_string(config.port) +
        ", protocole v" + std::to_string(flink::kVersion));
    Socket socketHandle = connectToLinux(config);
    bool connected = socketHandle != kInvalidSocket;
    std::string status = connected ? "Connecte a Lubuntu" : "Lubuntu non joignable";

    if (connected) {
        connected = synchronizeFlink(socketHandle, root);
        status = connected ? "FLink : PONG recu de Lubuntu" : "FLink : echec du protocole";
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "SDL_Init a echoue: " << SDL_GetError() << '\n';
        if (socketHandle != kInvalidSocket) {
            closeSocket(socketHandle);
        }
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("DLinker - FLink", 1280, 720,
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_FULLSCREEN);
    SDL_Renderer* renderer = window == nullptr ? nullptr : SDL_CreateRenderer(window, nullptr);
    if (window == nullptr || renderer == nullptr) {
        std::cerr << "Creation de la fenetre SDL3 impossible: " << SDL_GetError() << '\n';
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        if (socketHandle != kInvalidSocket) {
            closeSocket(socketHandle);
        }
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }
    const auto title = "DLinker - " + status + " - " + std::filesystem::absolute(root).string();
    SDL_SetWindowTitle(window, title.c_str());

    SDL_Texture* videoTexture = nullptr;
    int videoWidth = 0;
    int videoHeight = 0;
    SDL_AudioDeviceID audioDevice = 0;
    SDL_AudioStream* audioStream = nullptr;
    SDL_AudioSpec audioSpec{};
    audioSpec.freq = 48000;
    audioSpec.format = SDL_AUDIO_S16;
    audioSpec.channels = 2;
    audioDevice = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &audioSpec);
    if (audioDevice != 0) {
        audioStream = SDL_CreateAudioStream(&audioSpec, &audioSpec);
        if (audioStream == nullptr || !SDL_BindAudioStream(audioDevice, audioStream)) {
            std::cerr << "FLink: liaison audio SDL impossible: " << SDL_GetError() << '\n';
            SDL_DestroyAudioStream(audioStream);
            audioStream = nullptr;
        }
        SDL_ResumeAudioDevice(audioDevice);
        debugMessage(kGreen, "sortie audio SDL active");
    } else {
        debugMessage(kYellow, "sortie audio SDL indisponible: " + std::string(SDL_GetError()));
    }
    bool running = true;
    std::uint32_t inputSequence = 2000;
    const auto pointerPosition = [&](float x, float y) {
        flink::InputInfo input{};
        int windowWidth = 0;
        int windowHeight = 0;
        SDL_GetWindowSize(window, &windowWidth, &windowHeight);
        const float scale = std::min(
            static_cast<float>(windowWidth) / std::max(videoWidth, 1),
            static_cast<float>(windowHeight) / std::max(videoHeight, 1));
        const float renderedWidth = videoWidth * scale;
        const float renderedHeight = videoHeight * scale;
        const float left = (windowWidth - renderedWidth) * 0.5f;
        const float top = (windowHeight - renderedHeight) * 0.5f;
        input.absoluteX = static_cast<std::int32_t>(
            std::clamp((x - left) / std::max(scale, 0.001f), 0.0f,
                       static_cast<float>(std::max(videoWidth - 1, 0))));
        input.absoluteY = static_cast<std::int32_t>(
            std::clamp((y - top) / std::max(scale, 0.001f), 0.0f,
                       static_cast<float>(std::max(videoHeight - 1, 0))));
        input.hasAbsolutePosition = videoWidth > 0 && videoHeight > 0 ? 1 : 0;
        return input;
    };
    while (running) {
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN &&
                       event.key.key == SDLK_D &&
                       (event.key.mod & SDL_KMOD_CTRL) != 0 &&
                          (event.key.mod & SDL_KMOD_SHIFT) != 0) {
                SDL_MinimizeWindow(window);
            } else if (connected && config.input &&
                       (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP)) {
                const flink::InputInfo input{
                    0, 0, 0, static_cast<std::uint8_t>(
                        event.type == SDL_EVENT_KEY_DOWN ? 1 : 0),
                    static_cast<std::uint16_t>(event.key.key),
                    0, 0, 0, 0, static_cast<std::uint8_t>(
                        (event.key.mod & SDL_KMOD_SHIFT) != 0)
                };
                sendInput(socketHandle, input, inputSequence++);
            } else if (connected && config.input && event.type == SDL_EVENT_MOUSE_MOTION) {
                auto input = pointerPosition(event.motion.x, event.motion.y);
                sendInput(socketHandle, input, inputSequence++);
            } else if (connected && config.input &&
                       (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                        event.type == SDL_EVENT_MOUSE_BUTTON_UP)) {
                auto input = pointerPosition(event.button.x, event.button.y);
                input.mouseButton = static_cast<std::uint8_t>(event.button.button);
                input.buttonDown = static_cast<std::uint8_t>(
                        event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? 1 : 0);
                sendInput(socketHandle, input, inputSequence++);
            }
        }

        if (connected && streamPacketReady(socketHandle)) {
            flink::PacketHeader streamHeader{};
            std::vector<std::uint8_t> streamPayload;
            if (!receivePacket(socketHandle, streamHeader, streamPayload)) {
                connected = false;
                std::cerr << "FLink: flux distant interrompu\n";
            } else if (streamHeader.type == static_cast<std::uint16_t>(flink::PacketType::VideoFrame) &&
                       streamPayload.size() >= sizeof(flink::VideoFrameInfo)) {
                flink::VideoFrameInfo info{};
                std::memcpy(&info, streamPayload.data(), sizeof(info));
                const auto expected = sizeof(info) +
                    static_cast<std::size_t>(info.width) * info.height * info.bytesPerPixel;
                if (info.bytesPerPixel == 4 && expected == streamPayload.size()) {
                    if (videoTexture == nullptr || videoWidth != info.width ||
                        videoHeight != info.height) {
                        SDL_DestroyTexture(videoTexture);
                        videoTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_BGRA32,
                                                         SDL_TEXTUREACCESS_STREAMING,
                                                         info.width, info.height);
                        videoWidth = info.width;
                        videoHeight = info.height;
                        debugMessage(kBlue, "texture video " + std::to_string(videoWidth) +
                                             "x" + std::to_string(videoHeight));
                    }
                    if (videoTexture != nullptr) {
                        SDL_UpdateTexture(videoTexture, nullptr,
                            streamPayload.data() + sizeof(info), info.width * 4);
                        debugMessage(kGreen, "image recue " + std::to_string(info.width) +
                                             "x" + std::to_string(info.height) +
                                             " -> affichage plein cadre");
                    }
                }
            } else if (streamHeader.type == static_cast<std::uint16_t>(flink::PacketType::AudioFrame) &&
                       streamPayload.size() >= sizeof(flink::AudioFrameInfo) &&
                       audioStream != nullptr) {
                const auto* audio = streamPayload.data() + sizeof(flink::AudioFrameInfo);
                SDL_PutAudioStreamData(audioStream, audio,
                                       static_cast<int>(streamPayload.size() -
                                                        sizeof(flink::AudioFrameInfo)));
                debugMessage(kBlue, "audio recu (" +
                                    std::to_string(streamPayload.size() -
                                                   sizeof(flink::AudioFrameInfo)) +
                                    " octets)");
            }
        }

        SDL_SetRenderDrawColor(renderer, connected ? 25 : 70, connected ? 100 : 35, 55, 255);
        SDL_RenderClear(renderer);
        if (videoTexture != nullptr) {
            int windowWidth = 0;
            int windowHeight = 0;
            SDL_GetWindowSize(window, &windowWidth, &windowHeight);
            const float scale = std::min(
                static_cast<float>(windowWidth) / videoWidth,
                static_cast<float>(windowHeight) / videoHeight);
            const SDL_FRect destination{
                (windowWidth - videoWidth * scale) * 0.5f,
                (windowHeight - videoHeight * scale) * 0.5f,
                videoWidth * scale,
                videoHeight * scale
            };
            SDL_RenderTexture(renderer, videoTexture, nullptr, &destination);
        }
        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    if (audioDevice != 0) {
        SDL_CloseAudioDevice(audioDevice);
    }
    SDL_DestroyAudioStream(audioStream);
    SDL_DestroyTexture(videoTexture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    if (socketHandle != kInvalidSocket) {
        closeSocket(socketHandle);
    }
#ifdef _WIN32
    WSACleanup();
#endif
    std::cout << status << '\n';
    return 0;
}
