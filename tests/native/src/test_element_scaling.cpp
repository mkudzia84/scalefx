// Tests for controllers/lib/sfx_board/element/element_scaling.h
//
// Locks in scaleDuty() math + the three scaling modes:
//   - Passthrough: duty = pct * maxDuty / 100, voltages ignored
//   - Linear:      duty = (Ve / Vp) * pct * maxDuty / 100   (resistive heaters)
//   - Quadratic:   duty = (Ve / Vp)^2 * pct * maxDuty / 100 (power-mode)
//
// Plus the defensive zero-input + over-rail-element + zero-port cases.
//
// Numeric checks use 1-unit tolerance because the function rounds
// to uint16_t at the end.  The HALF-UP rounding is what matters for
// the steady-state PWM driver, not bit-exact equality.

#include "doctest.h"
#include "element_scaling.h"

#include <cstdint>

using namespace sfx_core;

// ---- Off is always off -----------------------------------------------

TEST_CASE("requestedPct=0 returns 0 regardless of mode/voltage") {
    ElementConfig elem;
    elem.elementMv = 5000;
    elem.mode = ElementScalingMode::Linear;
    CHECK(scaleDuty(0, 4095, 8000, elem) == 0);

    elem.mode = ElementScalingMode::Quadratic;
    CHECK(scaleDuty(0, 4095, 8000, elem) == 0);

    elem.mode = ElementScalingMode::Passthrough;
    CHECK(scaleDuty(0, 4095, 8000, elem) == 0);
}

TEST_CASE("portMaxDuty=0 returns 0") {
    ElementConfig elem; elem.elementMv = 5000; elem.mode = ElementScalingMode::Linear;
    CHECK(scaleDuty(50, 0, 8000, elem) == 0);
    CHECK(scaleDuty(100, 0, 8000, elem) == 0);
}

// ---- Passthrough mode -------------------------------------------------

TEST_CASE("passthrough ignores voltage ratio") {
    ElementConfig elem;
    elem.mode = ElementScalingMode::Passthrough;
    elem.elementMv = 5000;
    // Even when element << port (5V on 8V rail), passthrough doesn't scale.
    CHECK(scaleDuty(100, 4095, 8000, elem) == 4095);
    CHECK(scaleDuty( 50, 4095, 8000, elem) == doctest::Approx(2048).epsilon(0.01));
    CHECK(scaleDuty( 25, 4095, 8000, elem) == doctest::Approx(1024).epsilon(0.01));
}

// ---- Linear mode ------------------------------------------------------

TEST_CASE("linear scaling 5V on 8V rail at 100 pct => 5/8 duty") {
    ElementConfig elem;
    elem.elementMv = 5000;
    elem.mode = ElementScalingMode::Linear;
    // 5/8 * 4095 = 2559.375 → rounds to 2559
    CHECK(scaleDuty(100, 4095, 8000, elem) == doctest::Approx(2559).epsilon(0.001));
}

TEST_CASE("linear scaling 5V on 8V rail at 50 pct => 5/8 * 50 pct") {
    ElementConfig elem; elem.elementMv = 5000; elem.mode = ElementScalingMode::Linear;
    // 5/8 * 0.5 * 4095 = 1280
    CHECK(scaleDuty(50, 4095, 8000, elem) == doctest::Approx(1280).epsilon(0.001));
}

TEST_CASE("linear with element rated at OR above rail is passthrough-equivalent") {
    // 8V heater on 8V rail: full rail delivers rated voltage, so duty
    // SHOULD just be the requested pct (no de-scale, no over-drive).
    ElementConfig elem; elem.elementMv = 8000; elem.mode = ElementScalingMode::Linear;
    CHECK(scaleDuty(100, 4095, 8000, elem) == 4095);
    CHECK(scaleDuty( 50, 4095, 8000, elem) == doctest::Approx(2048).epsilon(0.01));

    // 12V heater on 8V rail: rail can't reach rated voltage anyway,
    // so passthrough is the conservative choice (the operator opted
    // into an under-driven element).
    elem.elementMv = 12000;
    CHECK(scaleDuty(100, 4095, 8000, elem) == 4095);
}

// ---- Quadratic mode (power-mode) -------------------------------------

TEST_CASE("quadratic scaling 5V on 8V rail at 100 pct => (5/8)^2 duty") {
    ElementConfig elem; elem.elementMv = 5000; elem.mode = ElementScalingMode::Quadratic;
    // (5/8)^2 * 4095 = 0.390625 * 4095 ≈ 1600
    CHECK(scaleDuty(100, 4095, 8000, elem) == doctest::Approx(1600).epsilon(0.001));
}

TEST_CASE("quadratic vs linear: same Ve/Vp, quadratic delivers lower duty") {
    ElementConfig lin; lin.elementMv = 5000; lin.mode = ElementScalingMode::Linear;
    ElementConfig quad = lin; quad.mode = ElementScalingMode::Quadratic;

    const uint16_t dLin  = scaleDuty(100, 4095, 8000, lin);
    const uint16_t dQuad = scaleDuty(100, 4095, 8000, quad);
    // quad squares a ratio < 1 → smaller result
    CHECK(dQuad < dLin);
    CHECK(dLin > 0);
}

// ---- Defensive: requested pct > 100 clamps to 100 -------------------

TEST_CASE("requestedPct >100 clamps to 100") {
    ElementConfig elem; elem.elementMv = 8000; elem.mode = ElementScalingMode::Linear;
    CHECK(scaleDuty(150, 4095, 8000, elem) == scaleDuty(100, 4095, 8000, elem));
    CHECK(scaleDuty(255, 4095, 8000, elem) == scaleDuty(100, 4095, 8000, elem));
}

// ---- Zero/unknown voltage falls back to passthrough -----------------

TEST_CASE("elementMv=0 falls back to passthrough behaviour") {
    ElementConfig elem; elem.elementMv = 0; elem.mode = ElementScalingMode::Linear;
    CHECK(scaleDuty(100, 4095, 8000, elem) == 4095);
    CHECK(scaleDuty( 50, 4095, 8000, elem) == doctest::Approx(2048).epsilon(0.01));
}

TEST_CASE("portMv=0 falls back to passthrough behaviour") {
    ElementConfig elem; elem.elementMv = 5000; elem.mode = ElementScalingMode::Linear;
    CHECK(scaleDuty(100, 4095, 0, elem) == 4095);
}

// ---- Pessimistic edge cases ------------------------------------------

TEST_CASE("very small element / very large port still yields >0 at 100 pct") {
    // 3.3V LED on a 24V battery rail (worst-case extreme), linear mode.
    // 3300/24000 = 0.1375 → duty = 0.1375 * 4095 = 563
    ElementConfig elem; elem.elementMv = 3300; elem.mode = ElementScalingMode::Linear;
    CHECK(scaleDuty(100, 4095, 24000, elem) == doctest::Approx(563).epsilon(0.005));
    // At low pct, duty must still be > 0 — otherwise PWM never engages.
    CHECK(scaleDuty(10, 4095, 24000, elem) > 0);
}

TEST_CASE("8-bit PWM max duty (255) works just like 12-bit") {
    ElementConfig elem; elem.elementMv = 5000; elem.mode = ElementScalingMode::Linear;
    // 5/8 * 255 = 159.375 → 159
    CHECK(scaleDuty(100, 255, 8000, elem) == doctest::Approx(159).epsilon(0.005));
}

// ---- ElementConfig default values ------------------------------------

TEST_CASE("ElementConfig defaults are safe (passthrough until configured)") {
    ElementConfig elem;
    CHECK(elem.elementMv == 0);
    CHECK(elem.mode == ElementScalingMode::Linear);
    // With elementMv=0 we get passthrough fallback — 100 pct delivers
    // full duty without any scaling math kicking in.
    CHECK(scaleDuty(100, 4095, 8000, elem) == 4095);
}
