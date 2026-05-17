/*
 * BoardIdentifier — runtime-assigned, flash-persisted slave identifier.
 *
 * Each slave reports two identity tokens in IDENT_GET_RESP:
 *   - boardType:    compile-time SlaveType enum value (e.g. GunFX, LightFX)
 *   - identifier:   user-assigned UTF-8 string, persisted in /board.yaml
 *                   on the slave's filesystem.  Empty on a fresh board.
 *
 * Master-side (or a test fixture connected directly via PC serial) can
 * write a new identifier with IDENT_SET; the slave persists it before
 * ACKing.  On next boot the identifier is reloaded automatically.
 *
 * YAML schema (single-key file):
 *
 *     board:
 *       identifier: "left-wing"
 *
 * Constraints:
 *   - Max length 32 bytes (BoardIdentifier::MAX_LEN).  Keeps the
 *     IDENT_GET_RESP payload bounded so it fits a single packet.
 *   - Printable ASCII only (0x20..0x7E).  Avoids escaping headaches in
 *     the YAML emitter and YAML parser; full UTF-8 can be added later.
 *
 * The class is platform-agnostic; the storage backend is injected via
 * read/write callbacks so it works on any filesystem (LittleFS, SDFS,
 * mock-RAM for tests).
 */

#ifndef SFX_BOARD_IDENTIFIER_H
#define SFX_BOARD_IDENTIFIER_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>

namespace sfx_core {

class BoardIdentifier {
public:
    static constexpr size_t MAX_LEN = 32;

    /// Read-callback contract: read up to `bufSize` bytes from `path` into
    /// `buf`, return bytes-read (0 if file missing).  Negative → I/O error.
    using ReadFile  = std::function<int (const char* path, char* buf, size_t bufSize)>;

    /// Write-callback contract: write `len` bytes of `buf` to `path`,
    /// returning true on success.
    using WriteFile = std::function<bool(const char* path, const char* buf, size_t len)>;

    BoardIdentifier() { _ident[0] = 0; }

    /// Set the compile-time slave-type byte the firmware will report
    /// in IDENT_GET_RESP.  Numeric value matches the master-side
    /// `sfx_usb::SlaveType` enum (1=GunFX, 2=LightFX, 3=GearControl,
    /// 0=Unknown / dev board).  Boards call this once from setup()
    /// — the BoardIdentifier itself doesn't know which compile target
    /// it's part of.
    void setBoardType(uint8_t boardType) { _boardType = boardType; }
    uint8_t boardType() const            { return _boardType; }

    /// Default path for the identifier YAML file.
    static const char* defaultPath() { return "/board.yaml"; }

    /// Load identifier from the YAML file.  Returns true if a value was
    /// read; false if the file is absent / empty / invalid (identifier is
    /// then left as ""; that's the canonical "unassigned" state).
    bool load(const ReadFile& read, const char* path = defaultPath()) {
        char buf[256] = {0};
        int n = read(path, buf, sizeof buf - 1);
        if (n <= 0) { _ident[0] = 0; return false; }
        buf[n] = 0;

        // Single-purpose mini-parser — looks for a line starting with
        // `identifier:` (optionally indented) and grabs the quoted or
        // bare scalar value.  Avoids dragging in the full YAML parser
        // for one field; if/when this file grows, switch to YamlParser.
        const char* p = buf;
        while (*p) {
            // skip leading whitespace
            while (*p == ' ' || *p == '\t') p++;
            // match the key
            static constexpr const char* KEY = "identifier:";
            size_t klen = strlen(KEY);
            if (strncmp(p, KEY, klen) == 0) {
                p += klen;
                while (*p == ' ' || *p == '\t') p++;
                // strip optional surrounding quotes
                bool quoted = (*p == '"' || *p == '\'');
                char q = quoted ? *p++ : 0;
                size_t i = 0;
                while (*p && i < MAX_LEN) {
                    if (quoted ? (*p == q) : (*p == '\n' || *p == '\r' || *p == '#')) break;
                    _ident[i++] = *p++;
                }
                // trim trailing spaces on the unquoted form
                while (i > 0 && (_ident[i - 1] == ' ' || _ident[i - 1] == '\t')) i--;
                _ident[i] = 0;
                return i > 0;
            }
            // advance to next line
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
        }
        _ident[0] = 0;
        return false;
    }

    /// Persist the current identifier to the YAML file.  Emits the
    /// canonical two-line form (parser accepts indented OR compact form
    /// per Rule 27, but emitters always write the indented form).
    bool save(const WriteFile& write, const char* path = defaultPath()) const {
        char buf[80];
        // Conservative quoting: emit a double-quoted scalar regardless
        // of contents.  Identifier is restricted to printable ASCII so
        // there's nothing to escape.
        int n = snprintf(buf, sizeof buf, "board:\n  identifier: \"%s\"\n", _ident);
        if (n <= 0 || (size_t)n >= sizeof buf) return false;
        return write(path, buf, (size_t)n);
    }

    /// Set the in-memory identifier.  Returns false on validation error
    /// (length / character set).  Caller invokes save() to persist.
    bool set(const char* s) {
        if (!s) return false;
        size_t len = strlen(s);
        if (len > MAX_LEN) return false;
        for (size_t i = 0; i < len; i++) {
            uint8_t c = (uint8_t)s[i];
            if (c < 0x20 || c > 0x7E || c == '"') return false;
        }
        memcpy(_ident, s, len);
        _ident[len] = 0;
        return true;
    }

    void clear() { _ident[0] = 0; }
    bool        isAssigned() const { return _ident[0] != 0; }
    const char* get()        const { return _ident; }
    size_t      length()     const { return strlen(_ident); }

private:
    char    _ident[MAX_LEN + 1];
    uint8_t _boardType = 0;        ///< compile-time slave-type byte (0 = Unknown)
};

}  // namespace sfx_core

#endif  // SFX_BOARD_IDENTIFIER_H
