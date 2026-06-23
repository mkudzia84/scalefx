/*
 * YAML Parser — Template Implementation
 *
 * Included at the bottom of yaml_parser.h.
 * Contains the full parsing algorithm and query methods.
 */

#ifndef YAML_PARSER_IPP
#define YAML_PARSER_IPP

// ============================================================================
// Memory Management
// ============================================================================

template<typename TPool>
bool YamlParser<TPool>::allocatePools() {
    // Phase 4 polish (2026-05-27): pull the node + string pools from
    // PSRAM (ESP32) / heap (Pico) explicitly instead of relying on
    // `new[]` to land there by chance.  HubFX pool sizes are large
    // (LargeYamlPool ≈ 26 KB, HubFxYamlPool ≈ 15 KB, LightFxProgramYamlPool
    // ≈ 14 KB) and a malloc fallback into internal DRAM during boot
    // fragments the DMA-cap pool — that was the failure mode that
    // crashed audio I²S init on 2026-05-27.  YamlNode is trivially
    // constructible (POD-like) and every default member init is zero
    // (Scalar=0, nullptr=0), so calloc-zero matches default-init.
    _nodePool = static_cast<YamlNode*>(
        sfxPsramCalloc(TPool::MAX_NODES, sizeof(YamlNode)));
    _stringPool = static_cast<char*>(
        sfxPsramCalloc(TPool::STRING_POOL_SIZE, 1));
    if (!_nodePool || !_stringPool) {
        reset();
        snprintf(_error, sizeof(_error), "Pool allocation failed");
        return false;
    }
    _nodeCount = 0;
    _stringPos = 0;
    return true;
}

template<typename TPool>
void YamlParser<TPool>::reset() {
    // sfxPsramFree(nullptr) is a no-op (free(nullptr) is per the C
    // spec; heap_caps_free matches), so the unconditional pair is safe
    // whether allocatePools half-succeeded or never ran.
    sfxPsramFree(_nodePool);
    sfxPsramFree(_stringPool);
    _nodePool = nullptr;
    _stringPool = nullptr;
    _nodeCount = 0;
    _stringPos = 0;
    _root = nullptr;
    _contextTop = 0;
    _error[0] = '\0';
}

template<typename TPool>
YamlNode* YamlParser<TPool>::allocNode(YamlNode::Type type) {
    if (_nodeCount >= TPool::MAX_NODES) {
        snprintf(_error, sizeof(_error), "Node pool exhausted (%zu nodes)", TPool::MAX_NODES);
        return nullptr;
    }
    YamlNode* node = &_nodePool[_nodeCount++];
    node->type = type;
    node->key = nullptr;
    node->scalarValue = nullptr;
    node->firstChild = nullptr;
    node->nextSibling = nullptr;
    node->lastChild = nullptr;
    return node;
}

template<typename TPool>
const char* YamlParser<TPool>::internString(const char* str, size_t len) {
    if (!str || len == 0) return "";
    if (_stringPos + len + 1 > TPool::STRING_POOL_SIZE) {
        snprintf(_error, sizeof(_error), "String pool exhausted (%zu bytes)", TPool::STRING_POOL_SIZE);
        return nullptr;
    }
    char* dest = &_stringPool[_stringPos];
    memcpy(dest, str, len);
    dest[len] = '\0';
    _stringPos += len + 1;
    return dest;
}

// ============================================================================
// Tree Building Helpers
// ============================================================================

template<typename TPool>
void YamlParser<TPool>::addChild(YamlNode* parent, YamlNode* child) {
    if (!parent || !child) return;
    if (!parent->firstChild) {
        parent->firstChild = child;
    } else {
        // O(1) append via the cached tail.  `lastChild` is maintained
        // here as the sole mutation site for the child list, so it always
        // points at the current end (or is null for an empty list).
        parent->lastChild->nextSibling = child;
    }
    parent->lastChild = child;
}

template<typename TPool>
bool YamlParser<TPool>::pushContext(int indent, YamlNode* node) {
    if (_contextTop >= (int)TPool::MAX_DEPTH) {
        snprintf(_error, sizeof(_error), "Max nesting depth exceeded (%zu)",
                 TPool::MAX_DEPTH);
        return false;
    }
    _contextStack[_contextTop].indent = indent;
    _contextStack[_contextTop].node = node;
    _contextTop++;
    return true;
}

template<typename TPool>
void YamlParser<TPool>::popToIndent(int indent) {
    while (_contextTop > 1 && _contextStack[_contextTop - 1].indent >= indent) {
        _contextTop--;
    }
}

template<typename TPool>
YamlNode* YamlParser<TPool>::currentParent() {
    if (_contextTop <= 0) return nullptr;
    return _contextStack[_contextTop - 1].node;
}

// ============================================================================
// Line Utilities
// ============================================================================

template<typename TPool>
int YamlParser<TPool>::countIndent(const char* line, size_t len) {
    int indent = 0;
    for (size_t i = 0; i < len; i++) {
        if (line[i] == ' ') indent++;
        else if (line[i] == '\t') indent += 2;  // Treat tab as 2 spaces
        else break;
    }
    return indent;
}

template<typename TPool>
bool YamlParser<TPool>::isComment(const char* line, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (line[i] == ' ' || line[i] == '\t') continue;
        return line[i] == '#';
    }
    return false;
}

template<typename TPool>
bool YamlParser<TPool>::isBlank(const char* line, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (line[i] != ' ' && line[i] != '\t' && line[i] != '\r' && line[i] != '\n')
            return false;
    }
    return true;
}

template<typename TPool>
const char* YamlParser<TPool>::stripQuotes(const char* str, size_t len, size_t& outLen) {
    if (len >= 2) {
        if ((str[0] == '"' && str[len - 1] == '"') ||
            (str[0] == '\'' && str[len - 1] == '\'')) {
            outLen = len - 2;
            return str + 1;
        }
    }
    outLen = len;
    return str;
}

// ============================================================================
// Main Parse
// ============================================================================

template<typename TPool>
bool YamlParser<TPool>::parse(const char* yaml, size_t len) {
    reset();
    if (!yaml || len == 0) {
        snprintf(_error, sizeof(_error), "Empty input");
        return false;
    }

    if (!allocatePools()) return false;

    // Create root Map node
    _root = allocNode(YamlNode::Map);
    if (!_root) return false;

    // Initialize context stack with root
    _contextTop = 0;
    pushContext(-1, _root);

    // Parse line by line
    const char* pos = yaml;
    const char* end = yaml + len;
    int lineNum = 0;

    while (pos < end) {
        lineNum++;
        // Find end of line
        const char* lineEnd = pos;
        while (lineEnd < end && *lineEnd != '\n') lineEnd++;

        size_t lineLen = lineEnd - pos;
        // Strip trailing \r
        if (lineLen > 0 && pos[lineLen - 1] == '\r') lineLen--;

        // Process line
        if (lineLen > 0 && !isBlank(pos, lineLen) && !isComment(pos, lineLen)) {
            if (!parseLine(pos, lineLen)) {
                // Prepend line number to error
                char tmp[64];
                snprintf(tmp, sizeof(tmp), "Line %d: %s", lineNum, _error);
                strncpy(_error, tmp, sizeof(_error) - 1);
                _error[sizeof(_error) - 1] = '\0';
                return false;
            }
        }

        pos = lineEnd + 1;  // Skip \n
    }

    return true;
}

// ============================================================================
// Line Processing
// ============================================================================

template<typename TPool>
bool YamlParser<TPool>::parseLine(const char* line, size_t lineLen) {
    // `indent` is the LOGICAL nesting width (tab counts as 2) used for the
    // pop/push context comparison.  The byte advance to skip past the
    // leading whitespace is a SEPARATE quantity — a tab is one byte but two
    // logical columns, so reusing `indent` as a byte offset would overshoot
    // `content` and underflow `contentLen` on a tab-indented line.
    int indent = countIndent(line, lineLen);

    // Count leading-whitespace BYTES to advance `content` correctly.
    size_t indentBytes = 0;
    while (indentBytes < lineLen &&
           (line[indentBytes] == ' ' || line[indentBytes] == '\t')) {
        indentBytes++;
    }
    // Whitespace-only line (defensive: blank lines are filtered upstream).
    if (indentBytes >= lineLen) return true;

    // Skip to content (past whitespace)
    const char* content = line + indentBytes;
    size_t contentLen = lineLen - indentBytes;

    // Strip inline comment (not inside quotes)
    bool inSingleQuote = false, inDoubleQuote = false;
    for (size_t i = 0; i < contentLen; i++) {
        if (content[i] == '\'' && !inDoubleQuote) inSingleQuote = !inSingleQuote;
        if (content[i] == '"' && !inSingleQuote) inDoubleQuote = !inDoubleQuote;
        if (content[i] == '#' && !inSingleQuote && !inDoubleQuote) {
            // Ensure there's a space before # (YAML spec: # must be preceded by space)
            if (i > 0 && content[i - 1] == ' ') {
                contentLen = i;
                // Trim trailing spaces
                while (contentLen > 0 && content[contentLen - 1] == ' ') contentLen--;
                break;
            }
        }
    }

    if (contentLen == 0) return true;  // Became blank after comment strip

    // Pop context stack to find correct parent
    popToIndent(indent);
    YamlNode* parent = currentParent();
    if (!parent) {
        snprintf(_error, sizeof(_error), "No parent context");
        return false;
    }

    // Check for sequence item: starts with "- "
    if (contentLen >= 2 && content[0] == '-' && content[1] == ' ') {
        return parseSequenceItem(parent, content + 2, contentLen - 2, indent);
    }

    // Check for key: value pair
    // Find the first ": " (colon followed by space) or ":" at end of content
    const char* colonPos = nullptr;
    bool inSQ = false, inDQ = false;
    for (size_t i = 0; i < contentLen; i++) {
        if (content[i] == '\'' && !inDQ) inSQ = !inSQ;
        if (content[i] == '"' && !inSQ) inDQ = !inDQ;
        if (content[i] == ':' && !inSQ && !inDQ) {
            // ":" must be followed by space, end of string, or nothing (nested map)
            if (i + 1 >= contentLen || content[i + 1] == ' ') {
                colonPos = content + i;
                break;
            }
        }
    }

    if (colonPos) {
        size_t keyLen = colonPos - content;
        const char* valueStart = colonPos + 1;
        size_t valueLen = contentLen - keyLen - 1;

        // Trim leading space from value
        while (valueLen > 0 && *valueStart == ' ') {
            valueStart++;
            valueLen--;
        }

        // Trim trailing space from value
        while (valueLen > 0 && valueStart[valueLen - 1] == ' ') {
            valueLen--;
        }

        if (valueLen == 0) {
            // "key:" with no value → container (Map by default, may become Sequence)
            return parseContainerEntry(parent, content, keyLen, indent);
        } else {
            // "key: value" → scalar entry
            return parseScalarEntry(parent, content, keyLen, valueStart, valueLen);
        }
    }

    // Unknown line format — skip silently (tolerant parsing)
    return true;
}

template<typename TPool>
bool YamlParser<TPool>::isFlowStart(const char* v, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (v[i] == ' ' || v[i] == '\t') continue;
        return v[i] == '{' || v[i] == '[';
    }
    return false;
}

template<typename TPool>
YamlNode* YamlParser<TPool>::parseFlowNode(const char*& p, const char* end) {
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    if (p >= end) return nullptr;

    // ── Flow map: { key: value, key: value } ──────────────────────────
    if (*p == '{') {
        ++p;                                    // consume '{'
        YamlNode* map = allocNode(YamlNode::Map);
        if (!map) return nullptr;
        while (p < end) {
            while (p < end && (*p == ' ' || *p == '\t' || *p == ',')) p++;
            if (p < end && *p == '}') { ++p; break; }
            if (p >= end) break;
            // Read key up to an unquoted ':'.
            const char* keyStart = p;
            bool sq = false, dq = false;
            while (p < end) {
                const char c = *p;
                if (c == '\'' && !dq) sq = !sq;
                else if (c == '"' && !sq) dq = !dq;
                else if (c == ':' && !sq && !dq) break;
                ++p;
            }
            if (p >= end || *p != ':') {
                snprintf(_error, sizeof(_error), "flow map: missing ':'");
                return nullptr;
            }
            size_t keyLen = (size_t)(p - keyStart);
            while (keyLen > 0 && (keyStart[keyLen - 1] == ' ' || keyStart[keyLen - 1] == '\t')) keyLen--;
            size_t kStripLen;
            const char* kStrip = stripQuotes(keyStart, keyLen, kStripLen);
            const char* iKey = internString(kStrip, kStripLen);
            if (!iKey) return nullptr;
            ++p;                                // consume ':'
            YamlNode* val = parseFlowNode(p, end);
            if (!val) return nullptr;
            val->key = iKey;
            addChild(map, val);
        }
        return map;
    }

    // ── Flow sequence: [ a, b, c ] ────────────────────────────────────
    if (*p == '[') {
        ++p;                                    // consume '['
        YamlNode* seq = allocNode(YamlNode::Sequence);
        if (!seq) return nullptr;
        while (p < end) {
            while (p < end && (*p == ' ' || *p == '\t' || *p == ',')) p++;
            if (p < end && *p == ']') { ++p; break; }
            if (p >= end) break;
            YamlNode* item = parseFlowNode(p, end);
            if (!item) return nullptr;
            addChild(seq, item);
        }
        return seq;
    }

    // ── Scalar: read to the next unquoted ',' '}' ']' ─────────────────
    const char* s = p;
    bool sq = false, dq = false;
    while (p < end) {
        const char c = *p;
        if (c == '\'' && !dq) sq = !sq;
        else if (c == '"' && !sq) dq = !dq;
        else if (!sq && !dq && (c == ',' || c == '}' || c == ']')) break;
        ++p;
    }
    size_t len = (size_t)(p - s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) len--;
    size_t stripLen;
    const char* strip = stripQuotes(s, len, stripLen);
    const char* iVal = internString(strip, stripLen);
    if (!iVal) return nullptr;
    YamlNode* node = allocNode(YamlNode::Scalar);
    if (!node) return nullptr;
    node->scalarValue = iVal;
    return node;
}

template<typename TPool>
bool YamlParser<TPool>::parseScalarEntry(YamlNode* parent, const char* key, size_t keyLen,
                                          const char* value, size_t valueLen) {
    // Flow collection as a value: `key: { … }` or `key: [ … ]`.
    if (isFlowStart(value, valueLen)) {
        const char* p = value;
        YamlNode* node = parseFlowNode(p, value + valueLen);
        if (!node) {
            if (!_error[0]) snprintf(_error, sizeof(_error), "malformed flow collection");
            return false;
        }
        const char* iKey = internString(key, keyLen);
        if (!iKey) return false;
        node->key = iKey;
        addChild(parent, node);
        return true;
    }

    // Strip quotes from value
    size_t strippedLen;
    const char* stripped = stripQuotes(value, valueLen, strippedLen);

    const char* iKey = internString(key, keyLen);
    const char* iVal = internString(stripped, strippedLen);
    if (!iKey || !iVal) return false;

    YamlNode* node = allocNode(YamlNode::Scalar);
    if (!node) return false;

    node->key = iKey;
    node->scalarValue = iVal;
    addChild(parent, node);
    return true;
}

template<typename TPool>
bool YamlParser<TPool>::parseContainerEntry(YamlNode* parent, const char* key, size_t keyLen, int lineIndent) {
    const char* iKey = internString(key, keyLen);
    if (!iKey) return false;

    // Create Map node (may be converted to Sequence if "- " items follow)
    YamlNode* node = allocNode(YamlNode::Map);
    if (!node) return false;

    node->key = iKey;
    addChild(parent, node);

    // Push onto context at the actual document indent of this key.
    // Children will appear at deeper indent and will be associated with
    // this node.  Siblings at the same or lesser indent will pop this entry.
    // Reject (rather than silently drop) an over-deep document so children
    // never attach to the wrong parent.
    return pushContext(lineIndent, node);
}

template<typename TPool>
bool YamlParser<TPool>::parseSequenceItem(YamlNode* parent, const char* content, size_t contentLen, int dashIndent) {
    // If parent is currently a Map with no children, convert to Sequence.
    // This handles: "key:\n  - item" where "key:" created a Map.
    if (parent->type == YamlNode::Map && parent->firstChild == nullptr) {
        parent->type = YamlNode::Sequence;
    }

    // If parent is not a Sequence, we need to find or create one.
    // This can happen with block sequences indented at the same level as the
    // parent key ("retracts:\n- channel: 0"): after the first item's scope is
    // popped, the context returns to the parent Map — the sequence lives as
    // its last child.
    if (parent->type != YamlNode::Sequence) {
        YamlNode* last = parent->firstChild;
        if (last) {
            while (last->nextSibling) last = last->nextSibling;
            if (last->type == YamlNode::Sequence) {
                // Existing sequence: append to it.
                parent = last;
            } else if (last->type == YamlNode::Map && last->firstChild == nullptr) {
                // Empty Map (created by "key:"): reinterpret as Sequence.
                last->type = YamlNode::Sequence;
                parent = last;
            } else {
                snprintf(_error, sizeof(_error), "Sequence item without sequence parent");
                return false;
            }
        } else {
            snprintf(_error, sizeof(_error), "Sequence item in empty map");
            return false;
        }
    }

    // Trim leading/trailing spaces from content
    while (contentLen > 0 && *content == ' ') { content++; contentLen--; }
    while (contentLen > 0 && content[contentLen - 1] == ' ') contentLen--;

    if (contentLen == 0) {
        // Bare "- " with nothing → empty scalar item
        YamlNode* item = allocNode(YamlNode::Scalar);
        if (!item) return false;
        item->scalarValue = "";
        addChild(parent, item);
        return true;
    }

    // Flow collection as a whole sequence item: `- { … }` or `- [ … ]`.
    // Must run BEFORE the colon scan below, which would otherwise split
    // the item at the first `:` inside the braces.
    if (content[0] == '{' || content[0] == '[') {
        const char* p = content;
        YamlNode* node = parseFlowNode(p, content + contentLen);
        if (!node) {
            if (!_error[0]) snprintf(_error, sizeof(_error), "malformed flow collection");
            return false;
        }
        addChild(parent, node);
        return true;
    }

    // Check if content has "key: value" or just "key:" → map item
    const char* colonPos = nullptr;
    bool inSQ = false, inDQ = false;
    for (size_t i = 0; i < contentLen; i++) {
        if (content[i] == '\'' && !inDQ) inSQ = !inSQ;
        if (content[i] == '"' && !inSQ) inDQ = !inDQ;
        if (content[i] == ':' && !inSQ && !inDQ) {
            if (i + 1 >= contentLen || content[i + 1] == ' ') {
                colonPos = content + i;
                break;
            }
        }
    }

    if (colonPos) {
        // "- key: value" or "- key:" → sequence item is a Map
        YamlNode* itemMap = allocNode(YamlNode::Map);
        if (!itemMap) return false;
        addChild(parent, itemMap);

        size_t keyLen = colonPos - content;
        const char* valueStart = colonPos + 1;
        size_t valueLen = contentLen - keyLen - 1;

        // Trim leading space from value
        while (valueLen > 0 && *valueStart == ' ') { valueStart++; valueLen--; }
        while (valueLen > 0 && valueStart[valueLen - 1] == ' ') valueLen--;

        // Push item map onto context FIRST — parseContainerEntry may also
        // push (for nested "- key:\n  child"), so itemMap must be below.
        // Propagate an over-deep failure loudly rather than mis-nesting.
        if (!pushContext(dashIndent, itemMap)) return false;

        if (valueLen == 0) {
            // "- key:" → first entry is a container.  The key's effective
            // indent is dashIndent + 2 (past the "- " prefix).
            return parseContainerEntry(itemMap, content, keyLen, dashIndent + 2);
        } else {
            // "- key: value" → first entry is scalar
            return parseScalarEntry(itemMap, content, keyLen, valueStart, valueLen);
        }
    }

    // "- value" → scalar sequence item
    size_t strippedLen;
    const char* stripped = stripQuotes(content, contentLen, strippedLen);
    const char* iVal = internString(stripped, strippedLen);
    if (!iVal) return false;

    YamlNode* item = allocNode(YamlNode::Scalar);
    if (!item) return false;
    item->scalarValue = iVal;
    addChild(parent, item);
    return true;
}

// ============================================================================
// Path-Based Queries
// ============================================================================

template<typename TPool>
const YamlNode* YamlParser<TPool>::findPath(const YamlNode* node, const char* dotPath) {
    if (!node || !dotPath || dotPath[0] == '\0') return node;

    char pathBuf[128];
    size_t pathLen = strlen(dotPath);
    if (pathLen >= sizeof(pathBuf)) return nullptr;
    memcpy(pathBuf, dotPath, pathLen + 1);

    const YamlNode* current = node;
    char* saveptr = nullptr;
    char* token = strtok_r(pathBuf, ".", &saveptr);

    while (token) {
        if (current->type != YamlNode::Map && current->type != YamlNode::Sequence) {
            return nullptr;
        }

        // Find child with matching key
        const YamlNode* child = current->firstChild;
        bool found = false;
        while (child) {
            if (child->key && strcmp(child->key, token) == 0) {
                current = child;
                found = true;
                break;
            }
            child = child->nextSibling;
        }
        if (!found) return nullptr;

        token = strtok_r(nullptr, ".", &saveptr);
    }

    return current;
}

template<typename TPool>
const YamlNode* YamlParser<TPool>::find(const char* dotPath) const {
    return findPath(_root, dotPath);
}

// ============================================================================
// Sequence Access
// ============================================================================

template<typename TPool>
int YamlParser<TPool>::sequenceLength(const char* dotPath) const {
    const YamlNode* node = find(dotPath);
    if (!node || node->type != YamlNode::Sequence) return 0;
    return node->childCount();
}

template<typename TPool>
const YamlNode* YamlParser<TPool>::sequenceItem(const char* dotPath, int index) const {
    const YamlNode* node = find(dotPath);
    if (!node || node->type != YamlNode::Sequence) return nullptr;
    return node->childAt(index);
}

#endif // YAML_PARSER_IPP
