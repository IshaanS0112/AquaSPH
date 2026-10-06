#pragma once
#include <string>
#include <vector>

namespace aquasph {

// Writes rendered frames to disk as PNG.
class FrameCapture {
public:
    // `directory` is created if it does not exist.
    explicit FrameCapture(const std::string& directory);

    // `rgb` must be width*height*3 bytes, top row first.
    bool write(const std::vector<unsigned char>& rgb, int width, int height);

    int framesWritten() const { return frameIndex_; }
    const std::string& directory() const { return directory_; }
    const std::string& lastError() const { return lastError_; }

private:
    std::string directory_;
    std::string lastError_;
    int frameIndex_ = 0;
};

} // namespace aquasph
