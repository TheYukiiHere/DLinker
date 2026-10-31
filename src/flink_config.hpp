#pragma once

#include <fstream>
#include <string>

namespace flink {

struct Config {
    std::string ip = "192.168.50.2";
    int port = 5002;
    int screenWidth = 0;
    int screenHeight = 0;
    int fps = 15;
    bool audio = true;
    bool input = true;
    bool physicalKeyboard = true;
};

inline Config loadConfig(const std::string& path)
{
    Config config;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        const auto separator = line.find('=');
        if (separator == std::string::npos) continue;
        const auto key = line.substr(0, separator);
        const auto value = line.substr(separator + 1);
        try {
            if (key == "ip") config.ip = value;
            else if (key == "port") config.port = std::stoi(value);
            else if (key == "screen_width") config.screenWidth = std::stoi(value);
            else if (key == "screen_height") config.screenHeight = std::stoi(value);
            else if (key == "fps") config.fps = std::stoi(value);
            else if (key == "audio") config.audio = value == "true";
            else if (key == "input") config.input = value == "true";
            else if (key == "physical_keyboard") config.physicalKeyboard = value == "true";
        } catch (...) {
        }

    }
    if (config.fps < 1) config.fps = 1;
    if (config.fps > 60) config.fps = 60;
    return config;
}

inline bool hasConfig(const std::string& path)
{
    return static_cast<bool>(std::ifstream(path));
}

} // namespace flink
