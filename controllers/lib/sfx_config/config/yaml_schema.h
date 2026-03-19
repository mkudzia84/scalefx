/*
 * YAML Schema — Declarative Schema Definition for Config Files
 *
 * Define your config→struct mapping declaratively using compile-time
 * field bindings.  The schema auto-generates populate() and validate()
 * functions, eliminating hand-written parsing boilerplate.
 *
 * Requires C++17 (auto NTTP, if constexpr, fold expressions, std::tuple).
 *
 * Three building blocks:
 *   sfx::prop<&S::field>("key", default)        — scalar/string field
 *   sfx::group<&S::sub>("key", children...)      — nested YAML map
 *   sfx::seq<&S::arr, &S::cnt>("key", children...) — YAML sequence → array
 *
 * Example:
 *
 *   struct MyConfig {
 *       struct GunFx {
 *           uint8_t  channel = 2;
 *           uint16_t rpm = 200;
 *           bool     enabled = true;
 *           char     soundFile[64] = "";
 *
 *           static constexpr int MAX_RATES = 4;
 *           struct Rate { char name[16] = {}; uint16_t rpm = 200; };
 *           Rate    rates[MAX_RATES] = {};
 *           uint8_t rateCount = 0;
 *       } gunFx;
 *   };
 *
 *   using namespace sfx;
 *   using GunFx = MyConfig::GunFx;
 *   using Rate   = GunFx::Rate;
 *
 *   inline const auto mySchema = schema<MyConfig>(
 *       group<&MyConfig::gunFx>("gun_fx",
 *           prop<&GunFx::channel>  ("channel",    uint8_t(2)).range(1, 10),
 *           prop<&GunFx::rpm>      ("rpm",        uint16_t(200)).range(50, 3000),
 *           prop<&GunFx::enabled>  ("enabled",    true),
 *           prop<&GunFx::soundFile>("sound_file", ""),
 *           seq<&GunFx::rates, &GunFx::rateCount>("rates_of_fire",
 *               prop<&Rate::name>("name", ""),
 *               prop<&Rate::rpm> ("rpm",  uint16_t(200)).range(50, 3000)
 *           )
 *       )
 *   );
 *
 *   // Adapt for ConfigStore:
 *   struct MySchema {
 *       using DataType = MyConfig;
 *       static bool populate(DataType& d, const YamlParser<>& p) {
 *           return mySchema.populate(d, p.root());
 *       }
 *       static bool validate(const DataType& d, char* e, size_t el) {
 *           return mySchema.validate(d, e, el);
 *       }
 *       static const char* defaultPath() { return "/config.yaml"; }
 *   };
 */

#ifndef YAML_SCHEMA_H
#define YAML_SCHEMA_H

#include <cstring>
#include <tuple>
#include <type_traits>
#include <climits>
#include "yaml_parser.h"

// ============================================================================
// Additional YamlScalarConvert Specializations (small integer types)
// ============================================================================
// Enable node->childAs<uint8_t>() etc. without manual casting.
// Core types (int32_t, uint32_t, float, bool, const char*) are in yaml_parser.h.

template<>
struct YamlScalarConvert<uint8_t> {
    static uint8_t parse(const char* s, uint8_t d) {
        return static_cast<uint8_t>(YamlScalarConvert<uint32_t>::parse(s, d));
    }
};

template<>
struct YamlScalarConvert<int8_t> {
    static int8_t parse(const char* s, int8_t d) {
        return static_cast<int8_t>(YamlScalarConvert<int32_t>::parse(s, d));
    }
};

template<>
struct YamlScalarConvert<uint16_t> {
    static uint16_t parse(const char* s, uint16_t d) {
        return static_cast<uint16_t>(YamlScalarConvert<uint32_t>::parse(s, d));
    }
};

template<>
struct YamlScalarConvert<int16_t> {
    static int16_t parse(const char* s, int16_t d) {
        return static_cast<int16_t>(YamlScalarConvert<int32_t>::parse(s, d));
    }
};

// ============================================================================
// Implementation Namespace
// ============================================================================

namespace sfx {

namespace detail {

/// Extract class and value types from a pointer-to-member
template<typename T> struct MemberTraits;
template<typename C, typename V>
struct MemberTraits<V C::*> {
    using ClassType = C;
    using ValueType = V;
};

} // namespace detail

// ============================================================================
// YamlBind — Scalar/String Field Binding
// ============================================================================

/**
 * @brief Binds a YAML key to a struct member via compile-time member pointer.
 *
 * Handles all common field types:
 *   - Arithmetic (uint8_t..uint32_t, int8_t..int32_t, float) — parsed + cast
 *   - Boolean — true/false/yes/no
 *   - char[N] arrays — strncpy with null-termination
 *
 * Optional range constraints for numeric fields via .range(min, max).
 *
 * @tparam MemberPtr  Pointer-to-member, e.g. &GunFx::channel
 */
template<auto MemberPtr>
struct YamlBind {
    using Traits    = detail::MemberTraits<decltype(MemberPtr)>;
    using Owner     = typename Traits::ClassType;
    using FieldType = typename Traits::ValueType;

    /// True if this field is a char[] (string) member
    static constexpr bool isCharArray =
        std::is_array_v<FieldType> &&
        std::is_same_v<std::remove_extent_t<FieldType>, char>;

    /// Storage type for default: const char* for strings, FieldType otherwise
    using DefaultType = std::conditional_t<isCharArray, const char*, FieldType>;

    const char*  yamlKey;
    DefaultType  defaultVal;
    int32_t      minVal = INT32_MIN;
    int32_t      maxVal = INT32_MAX;

    constexpr YamlBind(const char* key, DefaultType def)
        : yamlKey(key), defaultVal(def) {}

    constexpr YamlBind(const char* key, DefaultType def, int32_t lo, int32_t hi)
        : yamlKey(key), defaultVal(def), minVal(lo), maxVal(hi) {}

    /// Return a copy with range constraints (numeric fields only)
    constexpr YamlBind range(int32_t lo, int32_t hi) const {
        return {yamlKey, defaultVal, lo, hi};
    }

    /// Populate this field from a parent YAML node's child
    void populateFrom(Owner& data, const YamlNode* parentNode) const {
        if constexpr (isCharArray) {
            const char* val = parentNode
                ? parentNode->childAs<const char*>(yamlKey, defaultVal)
                : defaultVal;
            strncpy(data.*MemberPtr, val ? val : "", sizeof(data.*MemberPtr) - 1);
            (data.*MemberPtr)[sizeof(data.*MemberPtr) - 1] = '\0';
        } else {
            data.*MemberPtr = parentNode
                ? parentNode->childAs<FieldType>(yamlKey, defaultVal)
                : defaultVal;
        }
    }

    /// Validate this field against range constraints
    bool validate(const Owner& data, char* err, size_t errLen) const {
        if constexpr (!isCharArray && !std::is_same_v<FieldType, bool> &&
                      std::is_arithmetic_v<FieldType>) {
            if (minVal != INT32_MIN || maxVal != INT32_MAX) {
                auto val = data.*MemberPtr;
                if (val < static_cast<FieldType>(minVal) ||
                    val > static_cast<FieldType>(maxVal)) {
                    if constexpr (std::is_floating_point_v<FieldType>) {
                        snprintf(err, errLen, "%s: %.2f out of range [%ld, %ld]",
                                 yamlKey, (double)val, (long)minVal, (long)maxVal);
                    } else {
                        snprintf(err, errLen, "%s: %ld out of range [%ld, %ld]",
                                 yamlKey, (long)val, (long)minVal, (long)maxVal);
                    }
                    return false;
                }
            }
        }
        return true;
    }
};

// ============================================================================
// YamlGroup — Nested Map Node Binding
// ============================================================================

/**
 * @brief Binds a YAML map key to a nested sub-struct.
 *
 * Children can be any mix of YamlBind, YamlGroup, and YamlSequence.
 * Nesting depth is unlimited.
 *
 * @tparam GroupMemberPtr  Pointer-to-member for the sub-struct
 * @tparam Children        Child binding types (deduced)
 */
template<auto GroupMemberPtr, typename... Children>
struct YamlGroup {
    using Traits  = detail::MemberTraits<decltype(GroupMemberPtr)>;
    using Owner   = typename Traits::ClassType;
    using SubData = typename Traits::ValueType;

    const char*              yamlKey;
    std::tuple<Children...>  children;

    constexpr YamlGroup(const char* key, Children... ch)
        : yamlKey(key), children(ch...) {}

    /// Populate sub-struct fields from a YAML map node
    void populateFrom(Owner& data, const YamlNode* parentNode) const {
        const YamlNode* groupNode = parentNode
            ? parentNode->child(yamlKey) : nullptr;
        SubData& sub = data.*GroupMemberPtr;
        std::apply([&](const auto&... child) {
            (child.populateFrom(sub, groupNode), ...);
        }, children);
    }

    /// Validate all children within this group
    bool validate(const Owner& data, char* err, size_t errLen) const {
        const SubData& sub = data.*GroupMemberPtr;
        bool ok = true;
        std::apply([&](const auto&... child) {
            auto check = [&](const auto& ch) {
                if (ok) ok = ch.validate(sub, err, errLen);
            };
            (check(child), ...);
        }, children);
        return ok;
    }
};

// ============================================================================
// YamlSequence — Array/Sequence Binding
// ============================================================================

/**
 * @brief Binds a YAML sequence to a fixed-size C array + count member.
 *
 * Maximum capacity is deduced from the array extent (std::extent_v).
 * Children describe the fields of each sequence item (map items only).
 *
 * @tparam ArrayMemberPtr  Pointer-to-member for the array (e.g. &GunFx::rates)
 * @tparam CountMemberPtr  Pointer-to-member for the count (e.g. &GunFx::rateCount)
 * @tparam Children        Child binding types for each item
 */
template<auto ArrayMemberPtr, auto CountMemberPtr, typename... Children>
struct YamlSequence {
    using ArrayTraits = detail::MemberTraits<decltype(ArrayMemberPtr)>;
    using CountTraits = detail::MemberTraits<decltype(CountMemberPtr)>;
    using Owner       = typename ArrayTraits::ClassType;
    using ArrayType   = typename ArrayTraits::ValueType;
    using ItemType    = std::remove_extent_t<ArrayType>;
    using CountType   = typename CountTraits::ValueType;

    /// Maximum items (automatically deduced from array size)
    static constexpr size_t maxCapacity = std::extent_v<ArrayType>;

    const char*              yamlKey;
    std::tuple<Children...>  children;

    constexpr YamlSequence(const char* key, Children... ch)
        : yamlKey(key), children(ch...) {}

    /// Populate array from a YAML sequence node
    void populateFrom(Owner& data, const YamlNode* parentNode) const {
        const YamlNode* seqNode = parentNode
            ? parentNode->child(yamlKey) : nullptr;
        if (!seqNode || seqNode->type != YamlNode::Sequence) {
            data.*CountMemberPtr = 0;
            return;
        }
        int n = seqNode->childCount();
        if (n > (int)maxCapacity) n = (int)maxCapacity;
        data.*CountMemberPtr = static_cast<CountType>(n);

        for (int i = 0; i < n; i++) {
            const YamlNode* itemNode = seqNode->childAt(i);
            ItemType& item = (data.*ArrayMemberPtr)[i];
            std::apply([&](const auto&... child) {
                (child.populateFrom(item, itemNode), ...);
            }, children);
        }
    }

    /// Validate each item in the sequence
    bool validate(const Owner& data, char* err, size_t errLen) const {
        int n = static_cast<int>(data.*CountMemberPtr);
        for (int i = 0; i < n; i++) {
            const ItemType& item = (data.*ArrayMemberPtr)[i];
            bool ok = true;
            std::apply([&](const auto&... child) {
                auto check = [&](const auto& ch) {
                    if (ok) ok = ch.validate(item, err, errLen);
                };
                (check(child), ...);
            }, children);
            if (!ok) return false;
        }
        return true;
    }
};

// ============================================================================
// YamlSchema — Top-Level Schema Container
// ============================================================================

/**
 * @brief Top-level schema holding all group/field bindings.
 *
 * Provides populate() and validate() that auto-dispatch to all children.
 * Use with ConfigStore by wrapping in a thin adapter struct.
 *
 * @tparam TData     Root config data struct type
 * @tparam Children  Top-level binding types (groups, props)
 */
template<typename TData, typename... Children>
struct YamlSchema {
    using DataType = TData;

    std::tuple<Children...> children;

    constexpr YamlSchema(Children... ch) : children(ch...) {}

    /// Populate data struct from a parsed YAML root node
    bool populate(DataType& data, const YamlNode* root) const {
        std::apply([&](const auto&... child) {
            (child.populateFrom(data, root), ...);
        }, children);
        return true;
    }

    /// Validate data struct against all schema constraints
    bool validate(const DataType& data, char* err, size_t errLen) const {
        bool ok = true;
        std::apply([&](const auto&... child) {
            auto check = [&](const auto& ch) {
                if (ok) ok = ch.validate(data, err, errLen);
            };
            (check(child), ...);
        }, children);
        return ok;
    }

    /**
     * @brief Re-wrap this schema's children as a YamlGroup for embedding
     *        in a parent struct.
     *
     * This enables composing smaller, standalone schemas into a larger
     * parent schema without duplicating field definitions.
     *
     * @tparam GroupMemberPtr  Member pointer in the parent struct
     *                         whose type must equal DataType.
     * @param key  YAML key for the group node in the parent document.
     *
     * @code
     * // Standalone engine schema:
     * inline const auto engineFields = schema<EngineConfig>(...);
     *
     * // Embed in parent:
     * inline const auto hubFields = schema<HubConfig>(
     *     engineFields.asGroup<&HubConfig::engine>("engine_fx")
     * );
     * @endcode
     */
    template<auto GroupMemberPtr>
    constexpr auto asGroup(const char* key) const {
        using GMTraits = detail::MemberTraits<decltype(GroupMemberPtr)>;
        static_assert(std::is_same_v<typename GMTraits::ValueType, DataType>,
                      "asGroup: member type must match schema DataType");
        return std::apply([key](auto... ch) {
            return YamlGroup<GroupMemberPtr, decltype(ch)...>{key, ch...};
        }, children);
    }
};

// ============================================================================
// Factory Functions
// ============================================================================

/**
 * @brief Create a field binding.
 *
 * Maps a YAML key to a struct member.  Supports all scalar types
 * (integers, float, bool) and char[N] string arrays.
 *
 * @code
 * prop<&GunFx::channel>("channel", uint8_t(2))
 * prop<&GunFx::channel>("channel", uint8_t(2)).range(1, 10)
 * prop<&GunFx::sounds> ("sounds",  "default.wav")  // char[64]
 * @endcode
 */
template<auto MemberPtr>
constexpr auto prop(const char* key,
                    typename YamlBind<MemberPtr>::DefaultType defaultVal) {
    return YamlBind<MemberPtr>{key, defaultVal};
}

/**
 * @brief Create a group binding (nested YAML map → sub-struct).
 *
 * @code
 * group<&Config::gunFx>("gun_fx",
 *     prop<&GunFx::enabled>("enabled", true),
 *     prop<&GunFx::channel>("channel", uint8_t(2))
 * )
 * @endcode
 */
template<auto GroupMemberPtr, typename... Children>
constexpr auto group(const char* key, Children... children) {
    return YamlGroup<GroupMemberPtr, Children...>{key, children...};
}

/**
 * @brief Create a sequence binding (YAML sequence → fixed array + count).
 *
 * Maximum capacity is deduced from the array member's extent.
 * Children describe the fields of each map-typed sequence item.
 *
 * @code
 * seq<&GunFx::rates, &GunFx::rateCount>("rates_of_fire",
 *     prop<&Rate::name>("name", ""),
 *     prop<&Rate::rpm> ("rpm",  uint16_t(200)).range(50, 3000)
 * )
 * @endcode
 */
template<auto ArrayMemberPtr, auto CountMemberPtr, typename... Children>
constexpr auto seq(const char* key, Children... children) {
    return YamlSequence<ArrayMemberPtr, CountMemberPtr, Children...>{
        key, children...};
}

/**
 * @brief Create a top-level schema.
 *
 * @code
 * inline const auto mySchema = schema<MyConfig>(
 *     group<&MyConfig::gunFx>("gun_fx", ...),
 *     group<&MyConfig::engineFx>("engine_fx", ...)
 * );
 * @endcode
 */
template<typename TData, typename... Children>
constexpr auto schema(Children... children) {
    return YamlSchema<TData, Children...>{children...};
}

/**
 * @brief Embed an existing schema as a group in a parent schema.
 *
 * Equivalent to `schema.asGroup<MemberPtr>(key)` but reads
 * naturally alongside the other factory functions.
 *
 * @tparam MemberPtr  Member pointer in the parent struct
 * @param  key        YAML key for the group node
 * @param  schema     The sub-schema to embed
 *
 * @code
 * inline const auto hubFields = schema<HubConfig>(
 *     embed<&HubConfig::engine>("engine_fx", engineFields),
 *     embed<&HubConfig::gunFx> ("gun_fx",    gunFxFields)
 * );
 * @endcode
 */
template<auto MemberPtr, typename TData, typename... Ch>
constexpr auto embed(const char* key,
                     const YamlSchema<TData, Ch...>& schema) {
    return schema.template asGroup<MemberPtr>(key);
}

} // namespace sfx

#endif // YAML_SCHEMA_H
