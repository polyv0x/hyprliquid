#pragma once

#include <GLES3/gl32.h>
#include <array>
#include <cstddef>

// Host pointers passed to GL are only valid for tightly packed data. Inherited
// row offsets or a bound pixel buffer would change their meaning and bounds.
template <bool Unpack>
class CPixelTransferGuard {
  public:
    CPixelTransferGuard() {
        glGetIntegerv(Unpack ? GL_PIXEL_UNPACK_BUFFER_BINDING : GL_PIXEL_PACK_BUFFER_BINDING, &m_buffer);
        glBindBuffer(TARGET, 0);
        for (size_t i = 0; i < FIELDS.size(); ++i) {
            glGetIntegerv(FIELDS[i], &m_values[i]);
            glPixelStorei(FIELDS[i], i == 0 ? 1 : 0);
        }
    }
    ~CPixelTransferGuard() {
        for (size_t i = 0; i < FIELDS.size(); ++i)
            glPixelStorei(FIELDS[i], m_values[i]);
        glBindBuffer(TARGET, m_buffer);
    }
    CPixelTransferGuard(const CPixelTransferGuard&) = delete;
    CPixelTransferGuard& operator=(const CPixelTransferGuard&) = delete;

  private:
    static constexpr GLenum TARGET = Unpack ? GL_PIXEL_UNPACK_BUFFER : GL_PIXEL_PACK_BUFFER;
    static constexpr std::array<GLenum, 4> FIELDS = Unpack
        ? std::array<GLenum, 4>{GL_UNPACK_ALIGNMENT, GL_UNPACK_ROW_LENGTH, GL_UNPACK_SKIP_PIXELS, GL_UNPACK_SKIP_ROWS}
        : std::array<GLenum, 4>{GL_PACK_ALIGNMENT, GL_PACK_ROW_LENGTH, GL_PACK_SKIP_PIXELS, GL_PACK_SKIP_ROWS};
    GLint m_buffer = 0;
    std::array<GLint, 4> m_values{};
};
