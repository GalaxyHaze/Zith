#include "common/source-map.hpp"

#include "common/source-file.hpp"
#include "common/span.hpp"

#include <mio/mmap.hpp>

#include <mutex>
#include <string_view>
#include <system_error>
#include <utility>

namespace zith::memory {

SourceMap::SourceMap() : files(file_arena) {}

static bool is_valid_utf8(std::string_view text) noexcept {
    for (size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c <= 0x7F)
            continue;

        size_t follow = 0;
        if ((c & 0xE0) == 0xC0)
            follow = 1;
        else if ((c & 0xF0) == 0xE0)
            follow = 2;
        else if ((c & 0xF8) == 0xF0)
            follow = 3;
        else
            return false;

        if (i + follow >= text.size())
            return false;
        for (size_t j = 1; j <= follow; ++j) {
            if ((static_cast<unsigned char>(text[i + j]) & 0xC0) != 0x80)
                return false;
        }
        i += follow;
    }
    return true;
}

static auto build_loc(
    DynArray<SourceLoc> &files,
    std::string path,
    SourceLoc::FileVar file,
    FlatMap<std::string, FileId> &cache
) -> Result<FileId> {
    SourceLoc loc{std::move(file), std::move(path), files.arena()};
    const std::string_view content = loc.slice();
    if (!is_valid_utf8(content))
        return Error{"File is not valid UTF-8"};

    loc.buildLines();
    const FileId id = static_cast<FileId>(files.size());
    cache.insert(loc.path, id);
    files.emplace(std::move(loc));
    return id;
}

auto SourceMap::addFile(std::string_view path, std::string_view content) -> Result<FileId> {
    if (!is_valid_utf8(content))
        return Error{"File is not valid UTF-8"};

    std::unique_lock lock(rw_mutex);
    if (FileId *existing = cache.get(path)) {
        SourceLoc loc{std::string(content), std::string(path), file_arena};
        loc.buildLines();
        files[*existing] = std::move(loc);
        return *existing;
    }
    return build_loc(files, std::string(path), std::string(content), cache);
}

auto SourceMap::loadFile(std::string_view path, bool write) -> Result<FileId> {
    std::unique_lock lock(rw_mutex);
    if (FileId *existing = cache.get(path))
        return *existing;

    std::error_code error;
    if (write) {
        auto file = mio::make_mmap_sink(std::string(path), error);
        if (error)
            return Error{error.message()};
        return build_loc(files, std::string(path), std::move(file), cache);
    }

    auto file = mio::make_mmap_source(std::string(path), error);
    if (error)
        return Error{error.message()};
    return build_loc(files, std::string(path), std::move(file), cache);
}

auto SourceMap::exists(FileId id) const noexcept -> bool {
    std::shared_lock lock(rw_mutex);
    return id < files.size();
}

auto SourceMap::snippet(const Span &span) noexcept -> Result<std::string_view> {
    std::shared_lock lock(rw_mutex);
    if (span.file >= files.size())
        return Error{"Invalid File ID in Span"};
    return files[span.file].snippet(span);
}

auto SourceMap::get(FileId id) noexcept -> Optional<std::reference_wrapper<SourceLoc>> {
    std::shared_lock lock(rw_mutex);
    if (id < files.size())
        return std::ref(files[id]);
    return nullptr;
}

auto SourceMap::loc(const Span &span) const noexcept -> Loc {
    std::shared_lock lock(rw_mutex);
    if (span.file >= files.size())
        return Loc{0, 0};
    return files[span.file].loc(span.start);
}

} // namespace zith::memory
