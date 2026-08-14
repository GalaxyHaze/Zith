#pragma once

#include "common/memory/result.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace common::memory {

// Stable source-file facade. The implementation may memory-map the file when
// ZCT_HAS_MIO is enabled, or fall back to a buffered read otherwise.
class FileSource {
public:
    FileSource();
    ~FileSource();

    FileSource(const FileSource &) = delete;
    FileSource &operator=(const FileSource &) = delete;
    FileSource(FileSource &&other) noexcept;
    FileSource &operator=(FileSource &&other) noexcept;

    [[nodiscard]] static auto mmap(std::string path, bool writable = false)
        -> Result<FileSource>;
    [[nodiscard]] static auto readAll(std::string path) -> Result<FileSource>;

    [[nodiscard]] const char *data() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::string_view slice() const noexcept;

private:
    struct MappedFile;
    struct Owned;
    std::shared_ptr<MappedFile> mapped_;
    std::shared_ptr<Owned> owned_;

    explicit FileSource(std::shared_ptr<Owned> owned);
    explicit FileSource(std::shared_ptr<MappedFile> mapped);
};

} // namespace common::memory
