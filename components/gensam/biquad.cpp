/// @file biquad.cpp
/// @brief Implementation of the RBJ biquad designer. See biquad.h for the design rationale,
/// the sign-inversion convention, and why the sample rate is a parameter.
///
/// Ported from the independent HLM project's generate_peq_48_payload()
/// (https://github.com/robcazzaro/hlm, src/HLM/glm_library.c, GPLv3), with the design rate
/// lifted out of a compile-time constant into a parameter so subwoofers can be designed at
/// their own 12 kHz rate. The formulas are those of Appendix A.3 of the GLM protocol
/// specification, themselves the Robert Bristow-Johnson Audio EQ Cookbook derivations.

#include "biquad.h"

#include <cmath>

namespace esphome {
namespace gensam {

namespace {

/// Below this, a0 is too close to zero for the normalised coefficients to mean anything.
constexpr double A0_EPSILON = 1e-9;

/// Spelled out rather than taken from M_PI, which is POSIX and not guaranteed by <cmath>.
constexpr double PI = 3.14159265358979323846;

/// @brief Unnormalised second-order section, in the textbook orientation.
struct RawSection {
  double b0, b1, b2, a0, a1, a2;
};

}  // namespace

BiquadCoeffs design_biquad(PeqType type, float frequency_hz, float gain_db, float q,
                           uint32_t design_rate_hz) {
  if (type == PeqType::BYPASS) {
    return BIQUAD_BYPASS;
  }

  // Exactly zero, not a tolerance: GLM designs bands with gains as small as -2.3e-05 dB and
  // reserves the bypass vector for slots whose stored gain is precisely 0. See biquad.h.
  if (gain_db == 0.0f) {
    return BIQUAD_BYPASS;
  }

  // Guard the whole parameter domain up front: everything past here divides, and a filter
  // designed at or above Nyquist is not a filter. Falling back to bypass keeps one bad band
  // from aborting a group push; see the @return note in biquad.h.
  if (design_rate_hz == 0 || frequency_hz <= 0.0f ||
      frequency_hz >= static_cast<float>(design_rate_hz) / 2.0f) {
    return BIQUAD_BYPASS;
  }
  if (type == PeqType::PEAKING && q <= 0.0f) {
    return BIQUAD_BYPASS;
  }

  // Intermediates are double throughout: the coefficients cluster hard around +/-1 and +/-2
  // (b1 = -1.99028802 for a 56 Hz band at 12 kHz), where float32 has ~1e-7 of resolution, so
  // computing in float would spend most of it on rounding before the value ever reaches the
  // wire. Only the five normalised results are narrowed.
  const double w0 = 2.0 * PI * static_cast<double>(frequency_hz) / static_cast<double>(design_rate_hz);
  const double cos_w0 = std::cos(w0);
  const double sin_w0 = std::sin(w0);
  const double a = std::pow(10.0, static_cast<double>(gain_db) / 40.0);

  RawSection s{};
  switch (type) {
    case PeqType::PEAKING: {
      const double alpha = sin_w0 / (2.0 * static_cast<double>(q));
      s.b0 = 1.0 + alpha * a;
      s.b1 = -2.0 * cos_w0;
      s.b2 = 1.0 - alpha * a;
      s.a0 = 1.0 + alpha / a;
      s.a1 = -2.0 * cos_w0;
      s.a2 = 1.0 - alpha / a;
      break;
    }

    // Both shelves use the cookbook's Q form, alpha = sin(w0)/(2Q), with a fixed Q per type
    // rather than the slope (S) form the protocol specification describes. The caller's `q`
    // is ignored: GLM exposes no control for it. See biquad.h point 5 for the evidence.
    case PeqType::LOW_SHELF: {
      const double alpha = sin_w0 / (2.0 * static_cast<double>(SHELF_Q_LOW));
      const double two_sqrt_a_alpha = 2.0 * std::sqrt(a) * alpha;
      s.b0 = a * ((a + 1.0) - (a - 1.0) * cos_w0 + two_sqrt_a_alpha);
      s.b1 = 2.0 * a * ((a - 1.0) - (a + 1.0) * cos_w0);
      s.b2 = a * ((a + 1.0) - (a - 1.0) * cos_w0 - two_sqrt_a_alpha);
      s.a0 = (a + 1.0) + (a - 1.0) * cos_w0 + two_sqrt_a_alpha;
      s.a1 = -2.0 * ((a - 1.0) + (a + 1.0) * cos_w0);
      s.a2 = (a + 1.0) + (a - 1.0) * cos_w0 - two_sqrt_a_alpha;
      break;
    }

    case PeqType::HIGH_SHELF: {
      const double alpha = sin_w0 / (2.0 * static_cast<double>(SHELF_Q_HIGH));
      const double two_sqrt_a_alpha = 2.0 * std::sqrt(a) * alpha;
      s.b0 = a * ((a + 1.0) + (a - 1.0) * cos_w0 + two_sqrt_a_alpha);
      s.b1 = -2.0 * a * ((a - 1.0) + (a + 1.0) * cos_w0);
      s.b2 = a * ((a + 1.0) + (a - 1.0) * cos_w0 - two_sqrt_a_alpha);
      s.a0 = (a + 1.0) - (a - 1.0) * cos_w0 + two_sqrt_a_alpha;
      s.a1 = 2.0 * ((a - 1.0) - (a + 1.0) * cos_w0);
      s.a2 = (a + 1.0) - (a - 1.0) * cos_w0 - two_sqrt_a_alpha;
      break;
    }

    case PeqType::BYPASS:
    default:
      return BIQUAD_BYPASS;
  }

  if (std::fabs(s.a0) < A0_EPSILON) {
    return BIQUAD_BYPASS;
  }

  // Normalise by a0, and negate the feedback pair: the speaker's MAC unit accumulates all
  // five terms rather than subtracting the feedback, so the wire carries -a1/a0 and -a2/a0.
  BiquadCoeffs out;
  out.b0 = static_cast<float>(s.b0 / s.a0);
  out.b1 = static_cast<float>(s.b1 / s.a0);
  out.b2 = static_cast<float>(s.b2 / s.a0);
  out.a1 = static_cast<float>(-(s.a1 / s.a0));
  out.a2 = static_cast<float>(-(s.a2 / s.a0));
  return out;
}

}  // namespace gensam
}  // namespace esphome
