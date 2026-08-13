#include "common/memory/file-source.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#if defined(ZITH_HAS_MIO)
#include <mio/mmap.hpp>
#endif

namespace common::memory {

namespace {

std::string readAllImpl(const std::string &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    return std::string{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

} // namespace

struct FileSource::Owned {
    std::string bytes;
};

struct FileSource::MappedFile {
#if defined(ZITH_HAS_MIO)
    mio::mmap_source source;
    mio::mmap_sink sink;
#else
    std::string bytes;
#endif

    [[nodiscard]] const char *data() const noexcept {
#if defined(ZITH_HAS_MIO)
        if (source.is_mapped())
            return reinterpret_cast<const char *>(source.data());
        return reinterpret_cast<const char *>(sink.data());
#else
        return bytes.data();
#endif
    }

    [[nodiscard]] std::size_t size() const noexcept {
#if defined(ZITH_HAS_MIO)
        if (source.is_mapped())
            return source.size();
        return sink.size();
#else
        return bytes.size();
#endif
    }
};

FileSource::FileSource() = default;
FileSource::~FileSource() = default;

FileSource::FileSource(FileSource &&other) noexcept
    : mapped_(std::move(other.mapped_)), owned_(std::move(other.owned_)) {}

FileSource &FileSource::operator=(FileSource &&other) noexcept {
    if (this != &other) {
        mapped_ = std::move(other.mapped_);
        owned_ = std::move(other.owned_);
    }
    return *this;
}

FileSource::FileSource(std::shared_ptr<Owned> owned) : owned_(std::move(owned)) {}
FileSource::FileSource(std::shared_ptr<MappedFile> mapped) : mapped_(std::move(mapped)) {}

auto FileSource::mmap(std::string path, bool writable) -> Result<FileSource> {
#if defined(ZITH_HAS_MIO)
    auto mapped = std::make_shared<MappedFile>();
    if (writable) {
        std::error_code error;
        mapped->sink = mio::make_mmap_sink(path, error);
        if (error)
            return Error{error.message()};
    } else {
        std::error_code error;
        mapped->source = mio::make_mmap_source(path, error);
        if (error)
            return Error{error.message()};
    }
    return FileSource(std::move(mapped));
#else
    (void)writable;
    const std::string bytes = readAllImpl(path);
    if (bytes.empty() && !std::filesystem::exists(path))
        return Error{"failed to read file: " + path};
    auto owned = std::make_shared<Owned>();
    owned->bytes = std::move(bytes);
    return FileSource(std::move(owned));
#endif
}

auto FileSource::readAll(std::string path) -> Result<FileSource> {
    std::string bytes = readAllImpl(path);
    if (bytes.empty() && !std::filesystem::exists(path))
        return Error{"failed to read file: " + path};
    auto owned = std::make_shared<Owned>();
    owned->bytes = std::move(bytes);
    return FileSource(std::move(owned));
}

const char *FileSource::data() const noexcept {
    if (mapped_)
        return mapped_->data();
    if (owned_)
        return owned_->bytes.data();
    return nullptr;
}

std::size_t FileSource::size() const noexcept {
    if (mapped_)
        return mapped_->size();
    if (owned_)
        return owned_->bytes.size();
    return 0;
}

std::string_view FileSource::slice() const noexcept {
    const char *ptr = data();
    return ptr == nullptr ? std::string_view{} : std::string_view(ptr, size());
}

} // namespace common::memory
