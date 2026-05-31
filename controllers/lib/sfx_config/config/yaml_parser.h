/*
 * YAML Parser — Lightweight Embedded YAML Subset Parser
 *
 * Parses a YAML document into a queryable node tree.  Designed for
 * configuration files on embedded targets (ESP32-S3, RP2040/RP2350).
 *
 * Supported YAML subset:
 *   - Block mappings (key: value, key:\n  nested)
 *   - Block sequences (- item, - key: value for arrays of maps)
 *   - Flow collections, single-line, arbitrarily nested:
 *       maps  `{ kind: pwm, idx: 0 }`   sequences `[900, 1200]`
 *     Accepted as a value (`port: { kind: pwm, idx: 0 }`) or a whole
 *     sequence item (`- { id: 0, state: on }`).  Emitters still write
 *     block form (Rule 27); flow is an input convenience for hand-
 *     authored leaf objects.
 *   - Scalar types: strings (plain, single-quoted, double-quoted),
 *     integers, floats, booleans (true/false/yes/no)
 *   - Comments (# to end of line)
 *   - Indentation-based nesting (any consistent indent width)
 *
 * NOT supported (keeps parser small):
 *   - Multi-line flow collections (a `{`/`[` must close on the same line)
 *   - Multi-line scalars (|, >)
 *   - Anchors and aliases (&, *)
 *   - Tags (!!)
 *   - Multiple documents (---, ...)
 *
 * Memory model:
 *   Node pool and string pool are heap-allocated in parse(), freed in
 *   destructor or reset().  Pool sizes are compile-time template parameters
 *   via TPoolConfig.  On ESP32-S3 with PSRAM, use LargeYamlPool for
 *   bigger configs.
 *
 * Usage:
 *   YamlParser<> parser;
 *   if (parser.parse(yamlText, len)) {
 *       const char* name = parser.getString("engine_fx.type", "turbine");
 *       int rpm = parser.getInt("gun_fx.rates_of_fire[0].rpm", 200);
 *       bool enabled = parser.getBool("engine_fx.enabled", true);
 *
 *       // Sequence iteration
 *       int n = parser.sequenceLength("gun_fx.rates_of_fire");
 *       for (int i = 0; i < n; i++) {
 *           auto* item = parser.sequenceItem("gun_fx.rates_of_fire", i);
 *           int rpm = parser.getIntFrom(item, "rpm", 0);
 *       }
 *   }
 */

#ifndef YAML_PARSER_H
#define YAML_PARSER_H

#include <cstring>
#include <cstdlib>
#include <platform/sfx_platform.h>   // sfxPsramCalloc / sfxPsramFree

// ============================================================================
// Pool Configuration Presets
// ============================================================================

/// Default pool sizes — suitable for typical 2-4 KB config files
struct DefaultYamlPool {
    static constexpr size_t MAX_NODES       = 128;
    static constexpr size_t STRING_POOL_SIZE = 4096;
    static constexpr size_t MAX_DEPTH       = 10;
};

/// Large pool sizes — for complex configs on ESP32 with PSRAM
struct LargeYamlPool {
    static constexpr size_t MAX_NODES       = 512;
    static constexpr size_t STRING_POOL_SIZE = 16384;
    static constexpr size_t MAX_DEPTH       = 16;
};

// ============================================================================
// Scalar Type Conversion Traits
// ============================================================================

/// Forward declaration — specializations follow
template<typename T>
struct YamlScalarConvert;

template<>
struct YamlScalarConvert<const char*> {
    static const char* parse(const char* str, const char* def) {
        return (str && str[0] != '\0') ? str : def;
    }
};

template<>
struct YamlScalarConvert<int32_t> {
    static int32_t parse(const char* str, int32_t def) {
        if (!str) return def;
        char* end = nullptr;
        long val = strtol(str, &end, 10);
        return (end == str) ? def : (int32_t)val;
    }
};

template<>
struct YamlScalarConvert<uint32_t> {
    static uint32_t parse(const char* str, uint32_t def) {
        if (!str) return def;
        char* end = nullptr;
        unsigned long val = strtoul(str, &end, 10);
        return (end == str) ? def : (uint32_t)val;
    }
};

template<>
struct YamlScalarConvert<float> {
    static float parse(const char* str, float def) {
        if (!str) return def;
        char* end = nullptr;
        float val = strtof(str, &end);
        return (end == str) ? def : val;
    }
};

template<>
struct YamlScalarConvert<bool> {
    static bool parse(const char* str, bool def) {
        if (!str) return def;
        if (strcasecmp(str, "true") == 0 || strcasecmp(str, "yes") == 0 ||
            strcasecmp(str, "on") == 0) return true;
        if (strcasecmp(str, "false") == 0 || strcasecmp(str, "no") == 0 ||
            strcasecmp(str, "off") == 0) return false;
        return def;
    }
};

// ============================================================================
// YAML Node
// ============================================================================

/**
 * @brief A node in the parsed YAML tree
 *
 * Nodes form a tree via firstChild/nextSibling linked lists:
 *   Map node      → children are keyed entries (key != nullptr)
 *   Sequence node → children are items (key == nullptr)
 *   Scalar node   → leaf with scalarValue
 */
struct YamlNode {
    enum Type : uint8_t { Scalar = 0, Map = 1, Sequence = 2 };

    Type        type         = Scalar;
    const char* key          = nullptr;   ///< Node's key within parent Map (null for seq items, root)
    const char* scalarValue  = nullptr;   ///< Value string (Scalar nodes only)
    YamlNode*   firstChild   = nullptr;   ///< First child (Map/Sequence only)
    YamlNode*   nextSibling  = nullptr;   ///< Next sibling in same parent

    // ---- Typed scalar access ----

    /// Convert this scalar node's value to type T, or return default
    template<typename T>
    T as(T defaultVal) const {
        if (type != Scalar || !scalarValue) return defaultVal;
        return YamlScalarConvert<T>::parse(scalarValue, defaultVal);
    }

    // ---- Child navigation (works on Map/Sequence nodes) ----

    /// Find direct child by key name
    const YamlNode* child(const char* childKey) const {
        if (!childKey) return nullptr;
        const YamlNode* c = firstChild;
        while (c) {
            if (c->key && strcmp(c->key, childKey) == 0) return c;
            c = c->nextSibling;
        }
        return nullptr;
    }

    /// Get typed value from a named child: node->childAs<int>("rpm", 0)
    template<typename T>
    T childAs(const char* childKey, T defaultVal) const {
        const YamlNode* c = child(childKey);
        return c ? c->as<T>(defaultVal) : defaultVal;
    }

    /// Count direct children
    int childCount() const {
        int n = 0;
        const YamlNode* c = firstChild;
        while (c) { n++; c = c->nextSibling; }
        return n;
    }

    /// Get child by zero-based index
    const YamlNode* childAt(int index) const {
        if (index < 0) return nullptr;
        const YamlNode* c = firstChild;
        for (int i = 0; i < index && c; i++) c = c->nextSibling;
        return c;
    }
};

// ============================================================================
// YAML Parser
// ============================================================================

/**
 * @brief Lightweight YAML subset parser for embedded config files
 *
 * @tparam TPool Pool configuration (DefaultYamlPool or LargeYamlPool)
 */
template<typename TPool = DefaultYamlPool>
class YamlParser {
public:
    YamlParser() = default;
    ~YamlParser() { reset(); }

    // Non-copyable, movable
    YamlParser(const YamlParser&) = delete;
    YamlParser& operator=(const YamlParser&) = delete;

    // ========================================================================
    // Parsing
    // ========================================================================

    /**
     * @brief Parse YAML text into a node tree
     * @param yaml YAML content (null-terminated not required)
     * @param len  Content length in bytes
     * @return true if parsing succeeded
     */
    bool parse(const char* yaml, size_t len);

    /**
     * @brief Free all parser memory and reset state
     */
    void reset();

    /**
     * @brief Get the root node of the parsed tree
     * @return Root Map node, or nullptr if not parsed
     */
    const YamlNode* root() const { return _root; }

    /**
     * @brief Check if parsing succeeded and tree is available
     */
    bool isValid() const { return _root != nullptr; }

    /**
     * @brief Get parse error message (empty if no error)
     */
    const char* error() const { return _error; }

    // ========================================================================
    // Path-Based Queries (dot-separated keys)
    // ========================================================================

    /**
     * @brief Find a node by dotted path from root
     *
     * Path format: "section.subsection.key"
     * Array access not supported in path — use sequenceItem() instead.
     *
     * @param dotPath Dot-separated key path
     * @return Node at path, or nullptr if not found
     */
    const YamlNode* find(const char* dotPath) const;

    /**
     * @brief Generic typed query by dotted path
     *
     * Supported types: const char*, int32_t, uint32_t, float, bool.
     * Extend by specializing YamlScalarConvert<T>.
     *
     * @tparam T Value type
     * @param dotPath Dot-separated path
     * @param defaultVal Default if path not found or not parseable
     */
    template<typename T>
    T get(const char* dotPath, T defaultVal) const {
        const YamlNode* node = find(dotPath);
        return node ? node->as<T>(defaultVal) : defaultVal;
    }

    // ---- Named convenience wrappers (preserve familiar API) ----
    const char* getString(const char* p, const char* d = "")   const { return get<const char*>(p, d); }
    int32_t     getInt   (const char* p, int32_t     d = 0)    const { return get<int32_t>(p, d); }
    uint32_t    getUInt  (const char* p, uint32_t    d = 0)    const { return get<uint32_t>(p, d); }
    bool        getBool  (const char* p, bool        d = false)const { return get<bool>(p, d); }
    float       getFloat (const char* p, float       d = 0.0f) const { return get<float>(p, d); }

    // ========================================================================
    // Sequence Access
    // ========================================================================

    /**
     * @brief Get length of a sequence at path
     * @return Number of items, or 0 if not a sequence
     */
    int sequenceLength(const char* dotPath) const;

    /**
     * @brief Get a sequence item by index
     * @param dotPath Path to the sequence
     * @param index Zero-based index
     * @return Node at index, or nullptr
     */
    const YamlNode* sequenceItem(const char* dotPath, int index) const;

    // ========================================================================
    // Node-Relative Queries (for traversing sequence items)
    // ========================================================================

    /**
     * @brief Generic typed query on a child of a node
     *
     * Equivalent to node->childAs<T>(key, defaultVal) with null-safety.
     *
     * @tparam T Value type
     * @param node Starting node (typically a sequence item)
     * @param key  Child key to find
     * @param defaultVal Default if not found or not parseable
     */
    template<typename T>
    static T getFrom(const YamlNode* node, const char* key, T defaultVal) {
        return node ? node->childAs<T>(key, defaultVal) : defaultVal;
    }

    // ---- Named convenience wrappers (delegate to YamlNode members) ----
    static const YamlNode* findChild    (const YamlNode* n, const char* k)                            { return n ? n->child(k) : nullptr; }
    static const char*     getStringFrom(const YamlNode* n, const char* k, const char* d = "")        { return getFrom<const char*>(n, k, d); }
    static int32_t         getIntFrom   (const YamlNode* n, const char* k, int32_t     d = 0)         { return getFrom<int32_t>(n, k, d); }
    static uint32_t        getUIntFrom  (const YamlNode* n, const char* k, uint32_t    d = 0)         { return getFrom<uint32_t>(n, k, d); }
    static bool            getBoolFrom  (const YamlNode* n, const char* k, bool        d = false)     { return getFrom<bool>(n, k, d); }
    static float           getFloatFrom (const YamlNode* n, const char* k, float       d = 0.0f)      { return getFrom<float>(n, k, d); }
    static int             childCount   (const YamlNode* n)                                           { return n ? n->childCount() : 0; }
    static const YamlNode* childAt      (const YamlNode* n, int i)                                    { return n ? n->childAt(i) : nullptr; }

    // ========================================================================
    // Stats
    // ========================================================================

    size_t nodeCount() const { return _nodeCount; }
    size_t nodeCapacity() const { return TPool::MAX_NODES; }
    size_t stringPoolUsed() const { return _stringPos; }
    size_t stringPoolCapacity() const { return TPool::STRING_POOL_SIZE; }

private:
    // ---- Memory pools ----
    YamlNode* _nodePool   = nullptr;
    size_t    _nodeCount   = 0;
    char*     _stringPool  = nullptr;
    size_t    _stringPos   = 0;
    YamlNode* _root        = nullptr;
    char      _error[64]   = {};

    // ---- Parser context stack ----
    struct ContextEntry {
        int        indent;
        YamlNode*  node;
    };
    ContextEntry _contextStack[TPool::MAX_DEPTH];
    int          _contextTop = 0;

    // ---- Internal helpers ----
    bool allocatePools();
    YamlNode* allocNode(YamlNode::Type type = YamlNode::Scalar);
    const char* internString(const char* str, size_t len);

    void addChild(YamlNode* parent, YamlNode* child);
    void pushContext(int indent, YamlNode* node);
    void popToIndent(int indent);
    YamlNode* currentParent();

    bool parseLine(const char* line, size_t lineLen);
    bool parseScalarEntry(YamlNode* parent, const char* key, size_t keyLen,
                          const char* value, size_t valueLen);
    bool parseContainerEntry(YamlNode* parent, const char* key, size_t keyLen, int lineIndent);
    bool parseSequenceItem(YamlNode* parent, const char* content, size_t contentLen, int dashIndent);

    /// Parse a single-line flow collection / scalar starting at `*p`
    /// (which must point at `{`, `[`, or a bare scalar).  Advances `*p`
    /// past the parsed value.  Returns the freshly-allocated node (key
    /// unset — the caller assigns it) or nullptr on pool exhaustion /
    /// malformed input.  Recurses for nested flow collections.
    YamlNode* parseFlowNode(const char*& p, const char* end);
    /// True if the first non-blank char of [v, v+len) opens a flow
    /// collection (`{` or `[`).
    static bool isFlowStart(const char* v, size_t len);

    static int countIndent(const char* line, size_t len);
    static bool isComment(const char* line, size_t len);
    static bool isBlank(const char* line, size_t len);
    static const char* stripQuotes(const char* str, size_t len, size_t& outLen);
    static const YamlNode* findPath(const YamlNode* node, const char* dotPath);
};

// ============================================================================
// Template Implementation
// ============================================================================

// Implementation is in yaml_parser.cpp as explicit instantiations for
// DefaultYamlPool and LargeYamlPool.  The template body is included here
// so that custom pool configs also work.

#include "yaml_parser.ipp"

#endif // YAML_PARSER_H
