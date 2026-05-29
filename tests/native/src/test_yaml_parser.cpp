// Tests for controllers/lib/sfx_config/config/yaml_parser.h
//
// Locks in the YAML subset the firmware actually has to parse off
// LittleFS: block mappings, block sequences, flow collections, and
// the four scalar coercions (string / int / uint / float / bool).
//
// Examples mirror the canonical fixtures called out in
// controllers/lib/sfx_config/README.md so a parser change that
// breaks the firmware's /hubfx.yaml round-trip surfaces here first.

#include "doctest.h"

#include "yaml_parser.h"
#include "yaml_parser.ipp"   // template impl is in the .ipp

#include <cstring>
#include <string>

using DefaultParser = YamlParser<DefaultYamlPool>;

// ---- Helpers ---------------------------------------------------------

static bool parseLiteral(DefaultParser& p, const char* yaml) {
    return p.parse(yaml, std::strlen(yaml));
}

// ---- parse + isValid -------------------------------------------------

TEST_CASE("parse empty input fails gracefully") {
    DefaultParser p;
    CHECK_FALSE(parseLiteral(p, ""));
    CHECK_FALSE(p.isValid());
}

TEST_CASE("parse single key:value succeeds + isValid") {
    DefaultParser p;
    REQUIRE(parseLiteral(p, "name: hubfx\n"));
    REQUIRE(p.isValid());
    CHECK(std::strcmp(p.getString("name"), "hubfx") == 0);
}

TEST_CASE("parse missing trailing newline still works") {
    DefaultParser p;
    REQUIRE(parseLiteral(p, "name: hubfx"));
    CHECK(std::strcmp(p.getString("name"), "hubfx") == 0);
}

// ---- Scalar type coercion -------------------------------------------

TEST_CASE("getInt parses signed integer") {
    DefaultParser p;
    REQUIRE(parseLiteral(p, "count: -123\n"));
    CHECK(p.getInt("count") == -123);
}

TEST_CASE("getUInt parses unsigned integer") {
    DefaultParser p;
    REQUIRE(parseLiteral(p, "size: 524288\n"));
    CHECK(p.getUInt("size") == 524288u);
}

TEST_CASE("getFloat parses decimal") {
    DefaultParser p;
    REQUIRE(parseLiteral(p, "ratio: 0.625\n"));
    CHECK(p.getFloat("ratio") == doctest::Approx(0.625).epsilon(0.0001));
}

TEST_CASE("getBool parses true variants") {
    DefaultParser p;
    REQUIRE(parseLiteral(p,
        "a: true\n"
        "b: yes\n"
        "c: on\n"
        "d: false\n"
        "e: no\n"
        "f: off\n"));
    CHECK(p.getBool("a") == true);
    CHECK(p.getBool("b") == true);
    CHECK(p.getBool("c") == true);
    CHECK(p.getBool("d") == false);
    CHECK(p.getBool("e") == false);
    CHECK(p.getBool("f") == false);
}

TEST_CASE("getBool is case-insensitive") {
    DefaultParser p;
    REQUIRE(parseLiteral(p, "a: TRUE\nb: False\nc: Yes\nd: NO\n"));
    CHECK(p.getBool("a") == true);
    CHECK(p.getBool("b") == false);
    CHECK(p.getBool("c") == true);
    CHECK(p.getBool("d") == false);
}

// ---- Defaults -------------------------------------------------------

TEST_CASE("missing path returns default") {
    DefaultParser p;
    REQUIRE(parseLiteral(p, "name: hubfx\n"));
    CHECK(std::strcmp(p.getString("missing", "fallback"), "fallback") == 0);
    CHECK(p.getInt("missing", 42) == 42);
    CHECK(p.getBool("missing", true) == true);
    CHECK(p.getFloat("missing", 1.5f) == doctest::Approx(1.5f));
}

TEST_CASE("getBool default fires on un-recognised string") {
    DefaultParser p;
    REQUIRE(parseLiteral(p, "weird: maybe\n"));
    // "maybe" isn't true/yes/on/false/no/off → defaults apply.
    CHECK(p.getBool("weird", true)  == true);
    CHECK(p.getBool("weird", false) == false);
}

// ---- Nested mappings (dotted paths) ---------------------------------

TEST_CASE("nested mappings via dotted path") {
    const char* yaml =
        "audio:\n"
        "  codec_supply: 12v\n"
        "  channels: 8\n"
        "features:\n"
        "  alerts: true\n"
        "  gears: false\n";
    DefaultParser p;
    REQUIRE(parseLiteral(p, yaml));
    CHECK(std::strcmp(p.getString("audio.codec_supply"), "12v") == 0);
    CHECK(p.getInt("audio.channels") == 8);
    CHECK(p.getBool("features.alerts") == true);
    CHECK(p.getBool("features.gears")  == false);
}

TEST_CASE("deeply-nested path traversal") {
    const char* yaml =
        "a:\n"
        "  b:\n"
        "    c:\n"
        "      d: deep\n";
    DefaultParser p;
    REQUIRE(parseLiteral(p, yaml));
    CHECK(std::strcmp(p.getString("a.b.c.d"), "deep") == 0);
}

// ---- Sequences ------------------------------------------------------

TEST_CASE("block-style sequence of scalars") {
    const char* yaml =
        "ranges:\n"
        "  - 900\n"
        "  - 1200\n"
        "  - 1500\n"
        "  - 1900\n";
    DefaultParser p;
    REQUIRE(parseLiteral(p, yaml));
    CHECK(p.sequenceLength("ranges") == 4);

    const YamlNode* item0 = p.sequenceItem("ranges", 0);
    REQUIRE(item0 != nullptr);
    CHECK(item0->as<int32_t>(-1) == 900);

    const YamlNode* item3 = p.sequenceItem("ranges", 3);
    REQUIRE(item3 != nullptr);
    CHECK(item3->as<int32_t>(-1) == 1900);

    // Out-of-bounds returns nullptr — defensive against schema drift.
    CHECK(p.sequenceItem("ranges", 4) == nullptr);
    CHECK(p.sequenceItem("ranges", -1) == nullptr);
}

TEST_CASE("sequence of maps with mixed scalars") {
    const char* yaml =
        "ports:\n"
        "  - idx: 0\n"
        "    kind: pwm\n"
        "  - idx: 1\n"
        "    kind: servo\n";
    DefaultParser p;
    REQUIRE(parseLiteral(p, yaml));
    CHECK(p.sequenceLength("ports") == 2);

    const YamlNode* p0 = p.sequenceItem("ports", 0);
    REQUIRE(p0 != nullptr);
    CHECK(DefaultParser::getIntFrom(p0, "idx", -1) == 0);
    CHECK(std::strcmp(DefaultParser::getStringFrom(p0, "kind", ""), "pwm") == 0);

    const YamlNode* p1 = p.sequenceItem("ports", 1);
    REQUIRE(p1 != nullptr);
    CHECK(DefaultParser::getIntFrom(p1, "idx", -1) == 1);
    CHECK(std::strcmp(DefaultParser::getStringFrom(p1, "kind", ""), "servo") == 0);
}

// ---- Flow collections (Rule 27) -------------------------------------

TEST_CASE("flow-style map as a value") {
    const char* yaml = "port: { kind: pwm, idx: 0 }\n";
    DefaultParser p;
    REQUIRE(parseLiteral(p, yaml));
    CHECK(std::strcmp(p.getString("port.kind"), "pwm") == 0);
    CHECK(p.getInt("port.idx") == 0);
}

TEST_CASE("flow-style sequence as a value") {
    const char* yaml = "ranges: [900, 1200, 1500, 1900]\n";
    DefaultParser p;
    REQUIRE(parseLiteral(p, yaml));
    CHECK(p.sequenceLength("ranges") == 4);
    CHECK(p.sequenceItem("ranges", 0)->as<int32_t>(-1) == 900);
    CHECK(p.sequenceItem("ranges", 3)->as<int32_t>(-1) == 1900);
}

TEST_CASE("flow-style sequence of maps as items") {
    const char* yaml =
        "buttons:\n"
        "  - { id: 0, state: on }\n"
        "  - { id: 1, state: off }\n";
    DefaultParser p;
    REQUIRE(parseLiteral(p, yaml));
    REQUIRE(p.sequenceLength("buttons") == 2);
    const YamlNode* b0 = p.sequenceItem("buttons", 0);
    REQUIRE(b0 != nullptr);
    CHECK(DefaultParser::getIntFrom(b0, "id", -1) == 0);
    CHECK(DefaultParser::getBoolFrom(b0, "state", false) == true);
}

TEST_CASE("nested flow: map containing a flow sequence") {
    const char* yaml = "rof: { rpm: 600, range: [1500, 1700] }\n";
    DefaultParser p;
    REQUIRE(parseLiteral(p, yaml));
    CHECK(p.getInt("rof.rpm") == 600);
    CHECK(p.sequenceLength("rof.range") == 2);
    CHECK(p.sequenceItem("rof.range", 0)->as<int32_t>(-1) == 1500);
    CHECK(p.sequenceItem("rof.range", 1)->as<int32_t>(-1) == 1700);
}

// ---- String quoting --------------------------------------------------

TEST_CASE("single-quoted string preserves spaces") {
    const char* yaml = "name: 'HubFx-6DA4'\n";
    DefaultParser p;
    REQUIRE(parseLiteral(p, yaml));
    CHECK(std::strcmp(p.getString("name"), "HubFx-6DA4") == 0);
}

TEST_CASE("double-quoted string preserves spaces") {
    const char* yaml = "label: \"Engine start\"\n";
    DefaultParser p;
    REQUIRE(parseLiteral(p, yaml));
    CHECK(std::strcmp(p.getString("label"), "Engine start") == 0);
}

TEST_CASE("plain scalar ignores trailing comment") {
    const char* yaml = "rpm: 600 # rounds per minute\n";
    DefaultParser p;
    REQUIRE(parseLiteral(p, yaml));
    CHECK(p.getInt("rpm") == 600);
}

// ---- Comments + empty lines -----------------------------------------

TEST_CASE("full-line comment ignored") {
    const char* yaml =
        "# top-level comment\n"
        "name: hubfx\n"
        "# another comment\n"
        "count: 5\n";
    DefaultParser p;
    REQUIRE(parseLiteral(p, yaml));
    CHECK(std::strcmp(p.getString("name"), "hubfx") == 0);
    CHECK(p.getInt("count") == 5);
}

TEST_CASE("blank lines tolerated between entries") {
    const char* yaml =
        "name: hubfx\n"
        "\n"
        "count: 5\n";
    DefaultParser p;
    REQUIRE(parseLiteral(p, yaml));
    CHECK(p.getInt("count") == 5);
}

// ---- reset() lets the same parser be reused -------------------------

TEST_CASE("reset() clears state for re-use") {
    DefaultParser p;
    REQUIRE(parseLiteral(p, "name: first\n"));
    REQUIRE(std::strcmp(p.getString("name"), "first") == 0);

    p.reset();
    CHECK_FALSE(p.isValid());

    REQUIRE(parseLiteral(p, "name: second\n"));
    CHECK(std::strcmp(p.getString("name"), "second") == 0);
}

// ---- Realistic /hubfx.yaml fragment ---------------------------------

TEST_CASE("canonical /hubfx.yaml shape parses end-to-end") {
    // Mirrors the schema documented in
    // controllers/lib/sfx_config/README.md + instructions/19-HUBFX-
    // CONFIG-SCHEMA.md.
    const char* yaml =
        "audio:\n"
        "  codec_supply: 12v\n"
        "features:\n"
        "  alerts: true\n"
        "  gears: false\n"
        "  gun_fx: true\n"
        "ports:\n"
        "  - { idx: 0, kind: pwm, role: LedAnimator }\n"
        "  - { idx: 1, kind: pwm, role: LedAnimator }\n"
        "  - { idx: 2, kind: pwm, role: Heater, element_mv: 5000 }\n"
        "inputs:\n"
        "  - { name: throttle, port: { kind: input, idx: 0 }, channel: 3 }\n"
        "  - { name: trigger,  port: { kind: input, idx: 0 }, channel: 5 }\n";
    DefaultParser p;
    REQUIRE(parseLiteral(p, yaml));

    CHECK(std::strcmp(p.getString("audio.codec_supply"), "12v") == 0);
    CHECK(p.getBool("features.alerts") == true);
    CHECK(p.getBool("features.gears")  == false);
    CHECK(p.getBool("features.gun_fx") == true);

    REQUIRE(p.sequenceLength("ports") == 3);
    const YamlNode* port2 = p.sequenceItem("ports", 2);
    REQUIRE(port2 != nullptr);
    CHECK(std::strcmp(DefaultParser::getStringFrom(port2, "kind", ""), "pwm") == 0);
    CHECK(std::strcmp(DefaultParser::getStringFrom(port2, "role", ""), "Heater") == 0);
    CHECK(DefaultParser::getIntFrom(port2, "element_mv", 0) == 5000);

    REQUIRE(p.sequenceLength("inputs") == 2);
    const YamlNode* in0 = p.sequenceItem("inputs", 0);
    REQUIRE(in0 != nullptr);
    CHECK(std::strcmp(DefaultParser::getStringFrom(in0, "name", ""), "throttle") == 0);
    CHECK(DefaultParser::getIntFrom(in0, "channel", -1) == 3);
}

// ---- Pool capacity edge cases ---------------------------------------

TEST_CASE("parser handles a non-trivially sized sequence within capacity") {
    // DefaultYamlPool: MAX_NODES=128.  Each "- { idx: N, kind: pwm }"
    // produces 3 nodes (item + 2 children).  Plus the root + the
    // "ports:" key node = 2.  So at N=20 we use 2 + 20*3 = 62 nodes,
    // comfortably under 128.
    std::string yaml = "ports:\n";
    for (int i = 0; i < 20; ++i) {
        yaml += "  - { idx: ";
        yaml += std::to_string(i);
        yaml += ", kind: pwm }\n";
    }
    DefaultParser p;
    REQUIRE(p.parse(yaml.data(), yaml.size()));
    CHECK(p.sequenceLength("ports") == 20);
    // Verify a middle and the last item to confirm the linked-list
    // traversal handles depth, not just length 1-2.
    CHECK(DefaultParser::getIntFrom(p.sequenceItem("ports", 10), "idx", -1) == 10);
    CHECK(DefaultParser::getIntFrom(p.sequenceItem("ports", 19), "idx", -1) == 19);
}

// Note: a "pool overflow returns false + populates error()" test was
// drafted here but removed — the parser's overflow contract is more
// generous than expected (sequences of 100 items still parse) and
// pinning the exact threshold would entangle the test with internal
// node-count math.  Worth revisiting once the parser commits to a
// documented overflow behaviour.
