#include "core/io/posix_file_system.hpp"

#include <cerrno>
#include <cstdio>  // rename
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mc::io {
namespace {

// Suffix for the temporary an atomic write goes through. It sits next to the
// target so the rename stays within one filesystem.
constexpr char kTempSuffix[] = ".tmp";

// EINTR is not a failure, just an interruption; every raw descriptor loop has
// to retry rather than treat it as an error.
bool readExactly(int fd, u8* dst, usize count)
{
    usize done = 0;
    while (done < count) {
        const ssize_t n = ::read(fd, dst + done, count - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;  // shorter than fstat claimed
        }
        done += usize(n);
    }
    return true;
}

bool writeExactly(int fd, const u8* src, usize count)
{
    usize done = 0;
    while (done < count) {
        const ssize_t n = ::write(fd, src + done, count - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        done += usize(n);
    }
    return true;
}

bool buildTempPath(const char* path, char* out, usize outSize)
{
    const usize length = std::strlen(path);
    if (length + sizeof(kTempSuffix) > outSize) {
        return false;
    }
    std::memcpy(out, path, length);
    std::memcpy(out + length, kTempSuffix, sizeof(kTempSuffix));
    return true;
}

}  // namespace

bool PosixFileSystem::readFile(const char* path, std::vector<u8>* out, usize maxSize)
{
    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) {
        return false;
    }

    struct stat info;
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size < 0 || usize(info.st_size) > maxSize) {
        ::close(fd);
        return false;
    }

    const usize size = usize(info.st_size);
    const usize base = out->size();
    out->resize(base + size);
    const bool ok = size == 0 || readExactly(fd, out->data() + base, size);
    ::close(fd);

    if (!ok) {
        out->resize(base);
    }
    return ok;
}

bool PosixFileSystem::writeFileAtomic(const char* path, ConstByteSpan data)
{
    char temp[1024];
    if (!buildTempPath(path, temp, sizeof(temp))) {
        return false;
    }

    const int fd = ::open(temp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        return false;
    }

    bool ok = data.empty() || writeExactly(fd, data.data(), data.size());
    // Without this the rename can land before the data does, which on a
    // console that loses power mid-save produces an empty file with a valid
    // name -- worse than either outcome the rename is supposed to guarantee.
    if (ok && ::fsync(fd) != 0) {
        ok = false;
    }
    if (::close(fd) != 0) {
        ok = false;
    }

    if (ok && std::rename(temp, path) != 0) {
        // POSIX rename replaces atomically. FAT does not, and devkitARM's
        // implementation refuses when the target exists, so fall back to
        // remove-then-rename there. The window this opens is real but only on
        // the platform that never had an atomic replace to begin with.
        ok = (::unlink(path) == 0 || errno == ENOENT) && std::rename(temp, path) == 0;
    }

    if (!ok) {
        ::unlink(temp);
    }
    return ok;
}

bool PosixFileSystem::exists(const char* path)
{
    struct stat info;
    return ::stat(path, &info) == 0;
}

bool PosixFileSystem::isDirectory(const char* path)
{
    struct stat info;
    return ::stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

bool PosixFileSystem::makeDirectories(const char* path)
{
    char buffer[1024];
    const usize length = std::strlen(path);
    if (length + 1 > sizeof(buffer)) {
        return false;
    }
    std::memcpy(buffer, path, length + 1);

    // Walk the components, creating each in turn. Starting at 1 skips a leading
    // slash, whose "parent" is the root and always exists.
    for (usize i = 1; i <= length; ++i) {
        if (buffer[i] != '/' && buffer[i] != '\0') {
            continue;
        }
        const char saved = buffer[i];
        buffer[i] = '\0';
        if (::mkdir(buffer, 0777) != 0 && errno != EEXIST) {
            return false;
        }
        buffer[i] = saved;
    }
    return true;
}

bool PosixFileSystem::removeFile(const char* path)
{
    return ::unlink(path) == 0 || errno == ENOENT;
}

bool PosixFileSystem::removeDirectory(const char* path)
{
    // ENOENT is success for the same reason it is in removeFile: the caller
    // asked for the directory to be gone, and it is.
    return ::rmdir(path) == 0 || errno == ENOENT;
}

bool PosixFileSystem::listDirectory(const char* path, void* context, DirVisitor visit)
{
    DIR* dir = ::opendir(path);
    if (dir == nullptr) {
        return false;
    }

    char child[1024];
    const usize prefixLength = std::strlen(path);

    while (const struct dirent* entry = ::readdir(dir)) {
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        DirEntry out;
        const usize nameLength = std::strlen(entry->d_name);
        if (nameLength + 1 > sizeof(out.name)) {
            continue;  // longer than FAT can name; not ours
        }
        std::memcpy(out.name, entry->d_name, nameLength + 1);

        if (entry->d_type == DT_DIR) {
            out.isDirectory = true;
        } else if (entry->d_type == DT_UNKNOWN) {
            // Some filesystems do not fill d_type in. Paying for a stat is the
            // only way to find out, so it happens here and not for every entry.
            out.isDirectory = false;
            if (prefixLength + 1 + nameLength + 1 <= sizeof(child)) {
                std::memcpy(child, path, prefixLength);
                child[prefixLength] = '/';
                std::memcpy(child + prefixLength + 1, entry->d_name, nameLength + 1);
                out.isDirectory = isDirectory(child);
            }
        } else {
            out.isDirectory = false;
        }

        if (!visit(context, out)) {
            break;
        }
    }

    ::closedir(dir);
    return true;
}

}  // namespace mc::io
