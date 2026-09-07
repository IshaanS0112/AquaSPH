#pragma once
#include <string>
#include <vector>

namespace aquasph {

// Writes rendered frames to disk as PNG.
//
// DEPENDENCY NOTE. PNG encoding comes from stb_image_write.h (public
// domain, single header, ~1,700 lines), pulled in only when
// AQUASPH_BUILD_VISUALIZATION is on.
//
//  * What it provides: one function, stbi_write_png, which is a
//    zlib-compatible deflate plus the PNG chunk framing.
//  * What was rejected: a general image library (OpenCV, FreeImage,
//    ImageMagick) -- tens of megabytes and a system package requirement
//    for one call. Also rejected: hand-rolling a PNG writer using
//    uncompressed deflate blocks, which needs no dependency at all but
//    produces ~7.7 MB per 1600x1600 frame, so a 300-frame capture would
//    be 2.3 GB of intermediate files before ffmpeg ever sees them.
//  * Video encoding is NOT bundled. scripts/make_video.sh shells out to
//    ffmpeg, which is documented as an external prerequisite. Linking a
//    multimedia framework to turn a PNG sequence into an MP4 would be a
//    far larger dependency than the task justifies.
class FrameCapture {
public:
    // `directory` is created if it does not exist. Frames are written as
    // <directory>/frame_%05d.png, zero-padded so a lexical sort is a
    // temporal sort -- which is what ffmpeg's image2 demuxer assumes.
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
