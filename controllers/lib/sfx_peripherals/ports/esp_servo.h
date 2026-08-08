/*
 * esp_servo.h — native ESP-IDF MCPWM hobby-servo driver (no Arduino).
 *
 * Replaces the `ESP32Servo` Arduino library.  Drives up to 12 servo pulse
 * trains on the ESP32-S3 straight off the MCPWM peripheral — no LEDC channels
 * consumed (those stay free for LED brightness, see esp_native_gpio.h).
 *
 * Resource map (ESP32-S3, from soc_caps): 2 MCPWM groups × 3 operators ×
 * 2 generators = 12 outputs.  Each group runs ONE shared 50 Hz timer
 * (1 MHz tick → 20000-tick / 20 ms period); every servo gets its own
 * comparator so its pulse width is independent.  A generator goes HIGH on the
 * timer's empty (counter == 0) event and LOW on its comparator match, so the
 * commanded microsecond value maps 1:1 to compare ticks.
 *
 *   EspServo s;
 *   s.attach(gpio, 500, 2500);   // allocates one MCPWM generator
 *   s.writeMicroseconds(1500);   // 1.5 ms pulse → centre
 *
 * `EspServoPool` owns the singleton allocation across both groups; `EspServo`
 * is the per-port handle MicroservoPort holds (same surface as Arduino Servo:
 * attach / attached / writeMicroseconds).  No virtual, no RTTI.
 */

#ifndef SFX_ESP_SERVO_H
#define SFX_ESP_SERVO_H

#include <platform/sfx_platform.h>
#if SFX_PLATFORM_ESP32

#include <cstdint>
#include <driver/mcpwm_prelude.h>
#include <soc/soc_caps.h>

namespace sfx_peripherals {

// ============================================================================
// EspServoPool — MCPWM generator allocator (board-unique singleton, Rule 14)
// ============================================================================

class EspServoPool {
public:
    static EspServoPool& instance() {
        static EspServoPool pool;          // C++11 thread-safe static local
        return pool;
    }

    static constexpr int kGroups   = SOC_MCPWM_GROUPS;                   // 2
    static constexpr int kOpers    = SOC_MCPWM_OPERATORS_PER_GROUP;      // 3
    static constexpr int kGensOp   = SOC_MCPWM_GENERATORS_PER_OPERATOR;  // 2
    static constexpr int kPerGroup = kOpers * kGensOp;                   // 6
    static constexpr int kMaxServos = kGroups * kPerGroup;               // 12

    static constexpr uint32_t kResolutionHz = 1'000'000;  // 1 tick = 1 µs
    static constexpr uint32_t kPeriodTicks  = 20'000;     // 20 ms → 50 Hz

    /// Allocate one MCPWM generator on `gpio`.  Returns its comparator handle
    /// (used to set pulse width) or nullptr when the pool is exhausted /
    /// hardware setup fails.  `outGen` receives the generator handle (used by
    /// EspServo for the quiet-attach force-level control).
    ///
    /// THREADING CONTRACT: allocate() mutates pool state (`_count`, the timer/
    /// operator tables) WITHOUT a lock — it MUST be called only during
    /// single-threaded setup (servo attach happens at board bringup, before the
    /// servo-tick task runs).  Do not call it from a running task.
    mcpwm_cmpr_handle_t allocate(int gpio, uint16_t initialUs,
                                 mcpwm_gen_handle_t* outGen = nullptr) {
        if (gpio < 0 || _count >= kMaxServos) return nullptr;

        const int idx     = _count;
        const int group   = idx / kPerGroup;
        const int local   = idx % kPerGroup;
        const int operIdx = local / kGensOp;

        if (!ensureTimer(group))            return nullptr;
        if (!ensureOperator(group, operIdx)) return nullptr;

        mcpwm_oper_handle_t oper = _oper[group][operIdx];

        mcpwm_cmpr_handle_t cmp = nullptr;
        mcpwm_comparator_config_t cc = {};
        cc.flags.update_cmp_on_tez = true;   // latch new compare at counter zero
        if (mcpwm_new_comparator(oper, &cc, &cmp) != ESP_OK) return nullptr;

        mcpwm_gen_handle_t gen = nullptr;
        mcpwm_generator_config_t gc = {};
        gc.gen_gpio_num = gpio;
        if (mcpwm_new_generator(oper, &gc, &gen) != ESP_OK) return nullptr;

        // HIGH at counter empty, LOW at compare → pulse width == compare ticks.
        mcpwm_generator_set_action_on_timer_event(
            gen, MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP,
                                              MCPWM_TIMER_EVENT_EMPTY,
                                              MCPWM_GEN_ACTION_HIGH));
        mcpwm_generator_set_action_on_compare_event(
            gen, MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP,
                                                cmp, MCPWM_GEN_ACTION_LOW));

        mcpwm_comparator_set_compare_value(cmp, initialUs);
        ++_count;
        if (outGen) *outGen = gen;
        return cmp;
    }

private:
    EspServoPool() = default;

    bool ensureTimer(int group) {
        if (_timer[group]) return true;
        mcpwm_timer_config_t tc = {};
        tc.group_id      = group;
        tc.clk_src       = MCPWM_TIMER_CLK_SRC_DEFAULT;
        tc.resolution_hz = kResolutionHz;
        tc.count_mode    = MCPWM_TIMER_COUNT_MODE_UP;
        tc.period_ticks  = kPeriodTicks;
        if (mcpwm_new_timer(&tc, &_timer[group]) != ESP_OK) return false;
        mcpwm_timer_enable(_timer[group]);
        mcpwm_timer_start_stop(_timer[group], MCPWM_TIMER_START_NO_STOP);
        return true;
    }

    bool ensureOperator(int group, int operIdx) {
        if (_oper[group][operIdx]) return true;
        mcpwm_operator_config_t oc = {};
        oc.group_id = group;
        if (mcpwm_new_operator(&oc, &_oper[group][operIdx]) != ESP_OK) return false;
        mcpwm_operator_connect_timer(_oper[group][operIdx], _timer[group]);
        return true;
    }

    mcpwm_timer_handle_t    _timer[kGroups]        = {};
    mcpwm_oper_handle_t _oper [kGroups][kOpers] = {};
    int                     _count                  = 0;
};

// ============================================================================
// EspServo — one servo on one pin (Arduino-Servo-compatible subset)
// ============================================================================

class EspServo {
public:
    /// QUIET ATTACH (2.45.2): attach() allocates the generator but holds the
    /// pin FORCED LOW — no pulse train until the first writeMicroseconds().
    /// A hobby servo (or a black-box retract controller) with no pulse simply
    /// holds position; emitting the 1500 µs centre at attach actively DROVE
    /// every servo mid-travel on each boot — with the bench's repeated
    /// watchdog/brownout resets that read as "servos randomly open and shut".
    /// servo_port.h keys off kQuietAttach to skip its initial write too.
    static constexpr bool kQuietAttach = true;

    /// @param gpio  GPIO number.  @param minUs/maxUs  driver clamp (unused by
    /// the MCPWM math but kept for API parity — MicroservoPort clamps already).
    bool attach(int gpio, uint16_t /*minUs*/ = 500, uint16_t /*maxUs*/ = 2500,
                uint16_t initialUs = 1500) {
        _cmp = EspServoPool::instance().allocate(gpio, initialUs, &_gen);
        if (_cmp && _gen) {
            mcpwm_generator_set_force_level(_gen, 0, true);   // pin low, no pulses
            _forced = true;
        }
        return _cmp != nullptr;
    }

    bool attached() const { return _cmp != nullptr; }

    /// THREADING CONTRACT: each EspServo must have exactly ONE writer task.
    /// The underlying mcpwm_comparator_set_compare_value() is a single atomic
    /// register write (latched at counter-zero, see allocate()'s
    /// update_cmp_on_tez) so it cannot tear, but if a calibration handler runs
    /// on a task OTHER than the servo-tick task, route the write through the
    /// owning task — do not have two tasks drive the same _cmp.  The one-shot
    /// force release below shares that single-writer contract.
    void writeMicroseconds(uint16_t us) {
        if (!_cmp) return;
        mcpwm_comparator_set_compare_value(_cmp, us);
        if (_forced) {
            // First real command — release the force so the generator starts
            // pulsing at the freshly-set width (level -1 = remove force).
            mcpwm_generator_set_force_level(_gen, -1, true);
            _forced = false;
        }
    }

private:
    mcpwm_cmpr_handle_t _cmp    = nullptr;
    mcpwm_gen_handle_t  _gen    = nullptr;
    bool                _forced = false;
};

}  // namespace sfx_peripherals

#endif  // SFX_PLATFORM_ESP32
#endif  // SFX_ESP_SERVO_H
