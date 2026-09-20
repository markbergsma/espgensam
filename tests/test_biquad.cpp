/// @file test_biquad.cpp
/// @brief Host-side test for design_biquad() against coefficients captured from GLM.
///
/// ===================================================================================
/// RUNNING
/// ===================================================================================
///   c++ -std=c++17 -Wall -o /tmp/test_biquad \
///       tests/test_biquad.cpp components/gensam/biquad.cpp && /tmp/test_biquad
///
/// No framework, no build system: biquad.cpp is pure arithmetic over floats and biquad.h
/// includes only <cstdint>, so the translation unit compiles on a host compiler unmodified.
///
/// ===================================================================================
/// WHAT THIS PINS DOWN
/// ===================================================================================
/// The speaker never sees frequency, gain or Q -- only five finished float32 coefficients --
/// so a silent error here is a silent error in the room, with nothing on the bus to show for
/// it. There is no specification to check against that can be trusted on this point: the
/// published one is wrong about the shelving filters (see biquad.h point 5). So the expected
/// vectors below are not derived from any formula. They are the bytes Genelec's own software
/// put on the wire, lifted from captures/glm_group_switch_capture.log, paired with the
/// frequency/gain/Q that produced them from captures/glm-config/Home Cinema.sam.
///
/// The capture was taken while switching a real 7350A + 2x 8330A system between three
/// calibrated groups, and contains 37 distinct non-bypass coefficient sets. All 37 reproduce
/// to within 2.4e-7 -- float32 epsilon near unity -- which is what justifies the constants in
/// biquad.h. The cases below are the subset that covers every distinct path:
///
///   - peaking at 12 kHz (subwoofer) and at 48 kHz (two-way), since the design rate is
///     device-class specific and a 4x error here is a plausible, stable, wrong filter;
///   - low shelf across three decades of gain, which is what discriminates the Q form the
///     hardware actually uses from the slope form the specification describes -- the two
///     agree only at 0 dB;
///   - high shelf;
///   - two bands with infinitesimal but non-zero gain, one peaking and one shelving, which
///     GLM designs honestly rather than bypassing. These pin the bypass rule to exact
///     equality with zero and would fail against any tolerance-based threshold.
///
/// test_feedback_is_sign_inverted() and test_design_rate_changes_the_result() are structural
/// rather than data-driven, because a golden vector cannot distinguish "correct" from
/// "consistently wrong in the same way the fixture was generated".
/// ===================================================================================

#include "../components/gensam/biquad.h"

#include <cmath>
#include <cstdio>

using esphome::gensam::BiquadCoeffs;
using esphome::gensam::design_biquad;
using esphome::gensam::PeqType;

namespace {

int failures = 0;

void check(bool ok, const char *what) {
  std::printf("  %s %s\n", ok ? "[ok]  " : "[FAIL]", what);
  if (!ok) {
    failures++;
  }
}

/// Agreement required against a captured coefficient.
///
/// float32 carries about 1.2e-7 of relative precision near 1.0, and these coefficients
/// cluster around +/-1 and +/-2, so two correct implementations that round from double at
/// different points cannot be expected to agree more closely than a few ULPs. Across all 37
/// captured sets the worst disagreement is 2.4e-7; 1e-6 sits just above that and still far
/// below any error a wrong formula, rate or slope would produce (those start around 1e-3).
constexpr float OEM_TOL = 1e-6f;

void check_oem(const char *what, PeqType type, float freq, float gain, float q, uint32_t rate,
               const BiquadCoeffs &oem) {
  const BiquadCoeffs got = design_biquad(type, freq, gain, q, rate);
  const bool ok = std::fabs(got.b0 - oem.b0) < OEM_TOL && std::fabs(got.b1 - oem.b1) < OEM_TOL &&
                  std::fabs(got.b2 - oem.b2) < OEM_TOL && std::fabs(got.a1 - oem.a1) < OEM_TOL &&
                  std::fabs(got.a2 - oem.a2) < OEM_TOL;
  check(ok, what);
  if (!ok) {
    std::printf("         GLM  {%.9g, %.9g, %.9g, %.9g, %.9g}\n", oem.b0, oem.b1, oem.b2, oem.a1, oem.a2);
    std::printf("         ours {%.9g, %.9g, %.9g, %.9g, %.9g}\n", got.b0, got.b1, got.b2, got.a1, got.a2);
  }
}

bool is_bypass(const BiquadCoeffs &c) {
  return c.b0 == 1.0f && c.b1 == 0.0f && c.b2 == 0.0f && c.a1 == 0.0f && c.a2 == 0.0f;
}

// --- Peaking, subwoofer, designed at 12 kHz --------------------------------------------

void test_peaking_subwoofer_12k() {
  check_oem("peaking 56.1739 Hz -6.05847 dB Q 4.68839 @12k", PeqType::PEAKING, 56.1739f, -6.05847f,
            4.68839f, 12000,
            {0.9977777f, -1.99028802f, 0.993371487f, 1.99028802f, -0.991149187f});

  check_oem("peaking 120.478 Hz -4.44345 dB Q 20 @12k", PeqType::PEAKING, 120.478f, -4.44345f, 20.0f,
            12000, {0.999186635f, -1.99196756f, 0.996750951f, 1.99196756f, -0.995937526f});

  check_oem("peaking 93.3897 Hz -7.88665 dB Q 16.6502 @12k", PeqType::PEAKING, 93.3897f, -7.88665f,
            16.6502f, 12000,
            {0.998624146f, -1.99300313f, 0.996764004f, 1.99300313f, -0.99538821f});
}

// --- Peaking, two-way monitor, designed at 48 kHz ---------------------------------------

void test_peaking_twoway_48k() {
  check_oem("peaking 198.371 Hz -4.55673 dB Q 3.49809 @48k", PeqType::PEAKING, 198.371f, -4.55673f,
            3.49809f, 48000,
            {0.998040259f, -1.98972702f, 0.99235785f, 1.98972702f, -0.99039793f});

  check_oem("peaking 153.156 Hz -3.71365 dB Q 3.8515 @48k", PeqType::PEAKING, 153.156f, -3.71365f,
            3.8515f, 48000,
            {0.998882413f, -1.99317479f, 0.994692862f, 1.99317479f, -0.993575215f});
}

// --- Shelving ----------------------------------------------------------------------------
// The gain spread here is the point. At 0 dB the Q form and the slope form of the RBJ
// shelving filter are identical, so only a band with real gain can tell them apart; the
// -4.25 dB case does, and rules out the S = 1.0 the specification claims.
//
// Q is deliberately passed as an absurd value to prove the shelving paths ignore it and use
// their own fixed SHELF_Q_LOW / SHELF_Q_HIGH.

void test_low_shelf_48k() {
  check_oem("low shelf 46.1344 Hz -4.25103 dB @48k", PeqType::LOW_SHELF, 46.1344f, -4.25103f, 999.0f,
            48000, {0.997554183f, -1.97746897f, 0.979942977f, 1.97746003f, -0.977506161f});

  check_oem("low shelf 118.711 Hz -0.177536 dB @48k", PeqType::LOW_SHELF, 118.711f, -0.177536f, 999.0f,
            48000, {0.999740839f, -1.9490248f, 0.949516833f, 1.94902229f, -0.949260116f});
}

void test_high_shelf_48k() {
  check_oem("high shelf 14999 Hz -0.0199986 dB @48k", PeqType::HIGH_SHELF, 14999.0f, -0.0199986f,
            999.0f, 48000,
            {0.999078035f, 0.397874445f, 0.0396125428f, -0.397135854f, -0.0394292213f});
}

// --- The bypass rule is exact equality with zero -----------------------------------------

void test_infinitesimal_gain_is_designed_not_bypassed() {
  // A notch at -3.9e-14 dB. GLM designs it: the result is an identity response (b == a) but
  // expressed with the band's real coefficients, not the structural bypass vector. Any
  // tolerance-based bypass threshold fails this outright.
  check_oem("peaking 138.768 Hz -3.93406e-14 dB Q 16.8776 @12k is designed, not bypassed",
            PeqType::PEAKING, 138.768f, -3.93406e-14f, 16.8776f, 12000,
            {1.0f, -1.99044228f, 0.995707929f, 1.99044228f, -0.995707929f});

  // Same for a shelf, three orders of magnitude further from zero and still not bypassed.
  check_oem("low shelf 743.8 Hz -3.12747e-05 dB @48k is designed, not bypassed",
            PeqType::LOW_SHELF, 743.8f, -3.12747e-05f, 999.0f, 48000,
            {0.999999821f, -1.71299541f, 0.72114712f, 1.71299541f, -0.721146941f});

  check(!is_bypass(design_biquad(PeqType::PEAKING, 138.768f, -3.93406e-14f, 16.8776f, 12000)),
        "infinitesimal gain does not take the bypass path");
}

void test_exactly_zero_gain_is_bypassed() {
  // Unused slots in a .sam are stored as Gain:0 at a default frequency -- 40 Hz on subwoofers,
  // 1000 Hz on two-ways. Every such slot in the capture was transmitted as the bypass vector.
  check(is_bypass(design_biquad(PeqType::PEAKING, 40.0f, 0.0f, 1.0f, 12000)),
        "unused subwoofer slot (40 Hz, exactly 0 dB) is bypassed");
  check(is_bypass(design_biquad(PeqType::PEAKING, 1000.0f, 0.0f, 1.0f, 48000)),
        "unused two-way slot (1000 Hz, exactly 0 dB) is bypassed");
  check(is_bypass(design_biquad(PeqType::LOW_SHELF, 100.0f, 0.0f, 0.0f, 48000)),
        "low shelf at exactly 0 dB is bypassed");
  check(is_bypass(design_biquad(PeqType::HIGH_SHELF, 6000.0f, 0.0f, 0.0f, 48000)),
        "high shelf at exactly 0 dB is bypassed");
}

void test_bypass_type() {
  check(is_bypass(design_biquad(PeqType::BYPASS, 1000.0f, -6.0f, 1.0f, 48000)),
        "PeqType::BYPASS returns the identity vector regardless of parameters");
}

// --- The design rate must actually be honoured -------------------------------------------

void test_design_rate_changes_the_result() {
  const BiquadCoeffs at_12k = design_biquad(PeqType::PEAKING, 56.1739f, -6.05847f, 4.68839f, 12000);
  const BiquadCoeffs at_48k = design_biquad(PeqType::PEAKING, 56.1739f, -6.05847f, 4.68839f, 48000);
  check(std::fabs(at_12k.b1 - at_48k.b1) > 1e-3f,
        "same band designed at 12k and 48k yields different coefficients");

  // A design depends on frequency only through w0 = 2*pi*f/fs, so scaling both must cancel
  // exactly. This is the assertion that catches a hardcoded rate surviving in the code.
  const BiquadCoeffs scaled = design_biquad(PeqType::PEAKING, 56.1739f * 4.0f, -6.05847f, 4.68839f, 48000);
  const bool same = std::fabs(at_12k.b0 - scaled.b0) < 1e-6f && std::fabs(at_12k.b1 - scaled.b1) < 1e-6f &&
                    std::fabs(at_12k.b2 - scaled.b2) < 1e-6f && std::fabs(at_12k.a1 - scaled.a1) < 1e-6f &&
                    std::fabs(at_12k.a2 - scaled.a2) < 1e-6f;
  check(same, "56.1739 Hz @12k == 224.6956 Hz @48k (rate enters only through w0)");
}

// --- Sign inversion, checked structurally --------------------------------------------------

void test_feedback_is_sign_inverted() {
  // Re-derive the peaking section here in the textbook orientation (feedback not negated) and
  // confirm the implementation returns its negation.
  const double f0 = 198.371, gain_db = -4.55673, q = 3.49809, fs = 48000.0;
  const double w0 = 2.0 * 3.14159265358979323846 * f0 / fs;
  const double alpha = std::sin(w0) / (2.0 * q);
  const double amp = std::pow(10.0, gain_db / 40.0);
  const double a0 = 1.0 + alpha / amp;
  const double a1 = -2.0 * std::cos(w0);
  const double a2 = 1.0 - alpha / amp;

  const BiquadCoeffs got = design_biquad(PeqType::PEAKING, (float) f0, (float) gain_db, (float) q, 48000);
  check(std::fabs(got.a1 - (float) (-(a1 / a0))) < OEM_TOL, "a1 is transmitted as -a1/a0");
  check(std::fabs(got.a2 - (float) (-(a2 / a0))) < OEM_TOL, "a2 is transmitted as -a2/a0");

  // For a cut at these parameters the textbook a1 is negative, so a missing negation shows up
  // as a sign flip rather than only as a magnitude error.
  check(got.a1 * (float) a1 < 0.0f, "a1 sign is opposite the textbook coefficient");
}

// --- Degenerate input -----------------------------------------------------------------------
// These fall back to bypass rather than failing, so one malformed band in a setup file cannot
// abort an entire group push; the slot simply passes audio through.

void test_degenerate_parameters_fall_back_to_bypass() {
  check(is_bypass(design_biquad(PeqType::PEAKING, 0.0f, -6.0f, 1.0f, 48000)),
        "zero frequency falls back to bypass");
  check(is_bypass(design_biquad(PeqType::PEAKING, -100.0f, -6.0f, 1.0f, 48000)),
        "negative frequency falls back to bypass");
  check(is_bypass(design_biquad(PeqType::PEAKING, 24000.0f, -6.0f, 1.0f, 48000)),
        "frequency at Nyquist falls back to bypass");
  check(is_bypass(design_biquad(PeqType::PEAKING, 7000.0f, -6.0f, 1.0f, 12000)),
        "frequency above Nyquist for a 12k subwoofer design falls back to bypass");
  check(is_bypass(design_biquad(PeqType::PEAKING, 1000.0f, -6.0f, 0.0f, 48000)),
        "zero Q on a peaking band falls back to bypass");
  check(is_bypass(design_biquad(PeqType::PEAKING, 1000.0f, -6.0f, 1.0f, 0)),
        "zero design rate falls back to bypass");
}

}  // namespace

int main() {
  std::printf("design_biquad()\n");
  test_peaking_subwoofer_12k();
  test_peaking_twoway_48k();
  test_low_shelf_48k();
  test_high_shelf_48k();
  test_infinitesimal_gain_is_designed_not_bypassed();
  test_exactly_zero_gain_is_bypassed();
  test_bypass_type();
  test_design_rate_changes_the_result();
  test_feedback_is_sign_inverted();
  test_degenerate_parameters_fall_back_to_bypass();

  if (failures != 0) {
    std::printf("\n%d check(s) FAILED\n", failures);
    return 1;
  }
  std::printf("\nall checks passed\n");
  return 0;
}
