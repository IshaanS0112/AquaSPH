#include "FrameCapture.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <cstdio>
#include <filesystem>
#include <iostream>

namespace aquasph {

FrameCapture::FrameCapture(const std::string& directory) : directory_(directory) {
    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);
    if (ec) {
        lastError_ = "could not create '" + directory_ + "': " + ec.message();
        std::cerr << "[FrameCapture] " << lastError_ << "\n";
    }
}

bool FrameCapture::write(const std::vector<unsigned char>& rgb, int width, int height) {
    if (static_cast<size_t>(width) * height * 3 != rgb.size()) {
        lastError_ = "pixel buffer size does not match the requested dimensions";
        return false;
    }
    char name[64];
    std::snprintf(name, sizeof(name), "frame_%05d.png", frameIndex_);
    const std::string path = (std::filesystem::path(directory_) / name).string();

    // Stride is passed explicitly rather than left to stb's default so a
    // future switch to a padded readback cannot silently skew every row.
    const int ok = stbi_write_png(path.c_str(), width, height, 3, rgb.data(), width * 3);
    if (!ok) {
        lastError_ = "stbi_write_png failed for '" + path + "'";
        std::cerr << "[FrameCapture] " << lastError_ << "\n";
        return false;
    }
    ++frameIndex_;
    return true;
}

} // namespace aquasph
