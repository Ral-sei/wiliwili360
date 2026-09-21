// Real std::filesystem implementations for the Xbox 360 XDK CRT.
//
// The OXDK libc++ archive ships no <filesystem> compiled half. The M3 linker
// shims only provided placeholder bodies: __status always reported
// "not found", __create_directories did nothing, and directory_iterator never
// yielded an entry. That broke i18n locale discovery
// ("directory game:\resources\i18n\... doesn't exist") and the first config
// save ("Cannot write config to: game:\config\wiliwili\..."), because the
// config directory was never actually created.
//
// This file replaces those placeholders with implementations backed by the
// XDK Win32 file APIs (GetFileAttributesA / CreateDirectoryA /
// FindFirstFileA) — the same primitives the XDK CRT's own fopen() uses for
// game:\ paths.
//
// Only the symbols the wiliwili/borealis code actually references are
// provided; anything else still fails at link time, which keeps the
// supported surface explicit.

// libc++ headers first: the XDK Windows headers define macros (far, near,
// interface, ...) that would poison the templates if included earlier.
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#include <windows.h>

namespace std {
namespace __1 {
namespace __fs {
namespace filesystem {

namespace {

// std::system_category()/std::generic_category() are not part of the OXDK
// libc++ archive, so report Win32 errors through a private category instead.
class Xbox360FsErrorCategory final : public error_category {
public:
    const char* name() const noexcept override { return "xbox360-fs"; }

    string message(int ev) const override {
        // std::to_string lives in a sibling shim; keep this self-contained.
        char digits[16];
        int n = 0;
        unsigned int v = static_cast<unsigned int>(ev);
        do {
            digits[n++] = static_cast<char>('0' + (v % 10));
            v /= 10;
        } while (v != 0 && n < static_cast<int>(sizeof(digits)));
        string msg("Win32 error ");
        while (n > 0)
            msg += digits[--n];
        return msg;
    }
};

error_code makeFsError(unsigned long win32Error) {
    static Xbox360FsErrorCategory category;
    return error_code(static_cast<int>(win32Error), category);
}

// The XDK headers omit INVALID_FILE_ATTRIBUTES; 0xFFFFFFFF is the documented
// GetFileAttributesA failure value (also used by bili360-main).
constexpr unsigned long kInvalidFileAttributes = 0xFFFFFFFFUL;

bool isDotEntry(const char* name) {
    return name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

} // namespace

// ---------------------------------------------------------------------------
// path::__filename — everything after the last path separator, as a view
// into the path's own storage (replace_filename() relies on that aliasing).
// Both separators are accepted: borealis/wiliwili may hand over paths that
// still contain forward slashes.
// ---------------------------------------------------------------------------
path::__string_view path::__filename() const {
    // value_type is wchar_t here: with _WIN32 defined, libc++'s path stores
    // wide strings and narrows through our __wide_to_char on demand. A plain
    // char literal promotes cleanly for the separator comparison.
    const value_type* data = __pn_.data();
    const size_t size      = __pn_.size();
    size_t start      = size;
    while (start > 0 && data[start - 1] != '\\' && data[start - 1] != '/')
        --start;
    return __string_view(data + start, size - start);
}

// ---------------------------------------------------------------------------
// __status — one GetFileAttributesA call. A missing file is not an error,
// matching libc++ semantics for status()/exists().
// ---------------------------------------------------------------------------
file_status __status(const path& p, error_code* ec) {
    const string native = p.string(); // wide -> narrow via __wide_to_char
    const DWORD attrs   = GetFileAttributesA(native.c_str());
    if (attrs == kInvalidFileAttributes) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            if (ec)
                ec->clear();
        } else if (ec) {
            *ec = makeFsError(err);
        }
        return file_status(file_type::not_found);
    }
    if (ec)
        ec->clear();
    if (attrs & FILE_ATTRIBUTE_DIRECTORY)
        return file_status(file_type::directory);
    return file_status(file_type::regular);
}

// ---------------------------------------------------------------------------
// __create_directories — create each missing component in turn.
// Returns true only if at least one directory was actually created.
// ---------------------------------------------------------------------------
bool __create_directories(const path& p, error_code* ec) {
    if (ec)
        ec->clear();

    const string native = p.string();
    if (native.empty())
        return false;

    const DWORD attrs = GetFileAttributesA(native.c_str());
    if (attrs != kInvalidFileAttributes) {
        if (attrs & FILE_ATTRIBUTE_DIRECTORY)
            return false; // already exists, nothing created
        if (ec)
            *ec = makeFsError(ERROR_ALREADY_EXISTS); // a file blocks the path
        return false;
    }

    bool created = false;
    string current;

    // Keep a drive/root prefix like "game:" intact; CreateDirectoryA on the
    // root itself would just fail.
    size_t start = 0;
    const size_t rootSep = native.find('\\');
    if (rootSep != string::npos) {
        current = native.substr(0, rootSep);
        start   = rootSep + 1;
    }

    while (start <= native.size()) {
        const size_t next      = native.find_first_of("\\/", start); // tolerate stray '/'
        const string component = native.substr(start, next == string::npos ? next : next - start);
        if (!component.empty()) {
            if (!current.empty())
                current += '\\';
            current += component;
            if (CreateDirectoryA(current.c_str(), nullptr)) {
                created = true;
            } else {
                const DWORD err = GetLastError();
                if (err != ERROR_ALREADY_EXISTS) {
                    if (ec)
                        *ec = makeFsError(err);
                    return created;
                }
            }
        }
        if (next == string::npos)
            break;
        start = next + 1;
    }
    return created;
}


// ---------------------------------------------------------------------------
// directory_iterator — the header only forward-declares __dir_stream; the
// layout below is private to this translation unit, exactly like libc++'s
// own compiled version. directory_iterator is a friend of directory_entry,
// so the member functions here may fill the entry cache directly.
// ---------------------------------------------------------------------------
class __dir_stream {
public:
    __dir_stream() = default;

    ~__dir_stream() {
        if (handle != INVALID_HANDLE_VALUE)
            FindClose(handle);
    }

    __dir_stream(const __dir_stream&)            = delete;
    __dir_stream& operator=(const __dir_stream&) = delete;

    // __dir_stream is a friend of directory_entry, so it can fill the entry
    // cache directly instead of paying for a second status() syscall.
    void captureCurrentEntry() {
        const file_type ft = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? file_type::directory
                                                                                : file_type::regular;
        const uintmax_t size = (static_cast<uintmax_t>(data.nFileSizeHigh) << 32) |
                               static_cast<uintmax_t>(data.nFileSizeLow);
        entry.__assign_iter_entry(path(base + "\\" + data.cFileName),
                                  directory_entry::__create_iter_cached_result(ft, size, perms::unknown,
                                                                               file_time_type::min()));
    }

    HANDLE handle = INVALID_HANDLE_VALUE;
    WIN32_FIND_DATAA data {};
    string base; // directory path without a trailing separator
    directory_entry entry;
};

directory_iterator::directory_iterator(const path& p, error_code* ec, directory_options opts) {
    (void)opts; // no symlink or permission semantics to honor on the XDK CRT
    if (ec)
        ec->clear();

    string base = p.string();
    while (!base.empty() && (base.back() == '\\' || base.back() == '/'))
        base.pop_back();

    const string pattern = base + "\\*";

    WIN32_FIND_DATAA first {};
    const HANDLE handle = FindFirstFileA(pattern.c_str(), &first);
    if (handle == INVALID_HANDLE_VALUE) {
        // A missing or empty directory is reported as the end iterator,
        // matching how callers use directory_iterator for best-effort
        // enumeration.
        if (ec)
            *ec = makeFsError(GetLastError());
        return;
    }

    auto stream    = make_shared<__dir_stream>();
    stream->handle = handle;
    stream->base   = std::move(base);
    stream->data   = first;
    __imp_         = std::move(stream);

    // Position on the first real entry (skip "." and "..").
    for (;;) {
        if (!isDotEntry(__imp_->data.cFileName)) {
            __imp_->captureCurrentEntry();
            return;
        }
        if (!FindNextFileA(__imp_->handle, &__imp_->data)) {
            __imp_.reset(); // empty directory == end iterator
            return;
        }
    }
}

directory_iterator& directory_iterator::__increment(error_code* ec) {
    if (ec)
        ec->clear();
    if (!__imp_)
        return *this;

    for (;;) {
        if (!FindNextFileA(__imp_->handle, &__imp_->data)) {
            const DWORD err = GetLastError();
            if (err != ERROR_NO_MORE_FILES && ec)
                *ec = makeFsError(err);
            __imp_.reset(); // become the end iterator
            return *this;
        }
        if (isDotEntry(__imp_->data.cFileName))
            continue;
        __imp_->captureCurrentEntry();
        return *this;
    }
}

const directory_entry& directory_iterator::__dereference() const {
    return __imp_->entry;
}


// ---------------------------------------------------------------------------
// Wide/narrow path conversion. The Xbox 360 file system is ASCII-only, so a
// truncating cast is the honest conversion.
// ---------------------------------------------------------------------------
size_t __wide_to_char(const basic_string<wchar_t>& in, char* out, size_t N) {
    size_t n = in.size() < N ? in.size() : N;
    for (size_t i = 0; i < n; ++i)
        out[i] = (char)in[i];
    if (n < N)
        out[n] = '\0';
    return in.size();
}

size_t __char_to_wide(const basic_string<char>& in, wchar_t* out, size_t N) {
    size_t n = in.size() < N ? in.size() : N;
    for (size_t i = 0; i < n; ++i)
        out[i] = (wchar_t)(unsigned char)in[i];
    if (n < N)
        out[n] = L'\0';
    return in.size();
}

} // namespace filesystem
} // namespace __fs
} // namespace __1
} // namespace std

