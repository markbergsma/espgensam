#pragma once

/// @file biquad.h
/// @brief RBJ biquad designer for the Genelec SAM 20-band parametric EQ.
///
/// ===================================================================================
/// ARCHITECTURE & DESIGN RATIONALE
/// ===================================================================================
/// SAM monitors do not accept frequency, gain and Q.  Their DSP is a cascade of 20
/// second-order sections, and the GLM host is expected to run the filter design itself and
/// transmit finished coefficients (CMD_DSP 0x10, sub-command 0x0E).  This file is that
/// designer.  It is deliberately free of any bus, frame or ESPHome dependency: it is pure
/// arithmetic over floats, so it compiles and is tested on the host.
///
/// 1. Why Design On The Device At All:
///    The coefficients could equally be precomputed by the ESPHome code generator and shipped
///    as a flash table.  Designing on device instead keeps the YAML expressed in the same
///    terms the GLM user interface and the .sam setup file use - frequency, gain, Q - which
///    stays readable and reviewable, and leaves the door open to exposing live EQ controls to
///    Home Assistant later.  The cost is ~1 kB of code and a handful of transcendentals per
///    band, run once per group switch on a core with an FPU.
///
/// 2. Design Sample Rate Is A Parameter, Not A Constant:
///    A biquad is only meaningful relative to the rate it was designed at, and Genelec does
///    not use one rate across the range: two-way monitors design at 48 kHz, subwoofers at
///    12 kHz (both confirmed, the latter from an OEM .sam file, where every SubwooferGen2
///    band carries SR:12000).  Evaluating a 12 kHz design at 48 kHz moves every filter up by
///    4x - a 40 Hz notch lands at 160 Hz - so the rate travels with the call rather than
///    living in a #define.  The 83x1 series is reported to run a 96 kHz DSP path and is
///    unresolved; that is why callers can override the rate per monitor.
///
/// 3. Sign Inversion Is Part Of The Encoding, Not Of The Filter:
///    The speaker's MAC unit accumulates rather than subtracts, so it expects the feedback
///    path pre-negated: y[n] = b0.x[n] + b1.x[n-1] + b2.x[n-2] + a1.y[n-1] + a2.y[n-2].
///    design_biquad() therefore returns a1 and a2 already divided by a0 *and* negated.  The
///    values it returns are exactly the five floats that go on the wire, in wire order, so no
///    later stage has to remember to flip a sign.
///
/// 4. The Bypass Vector:
///    An unused band is transmitted as {1, 0, 0, 0, 0} rather than as computed coefficients,
///    letting the DSP skip the section entirely.  The threshold is exact equality with zero,
///    not a tolerance: a capture of GLM applying three calibrated groups shows every band
///    whose stored gain is exactly 0 dB sent as the bypass vector, and every band with any
///    non-zero gain designed honestly - including a peaking band at -2.3e-05 dB and a shelf
///    at -4.3e-14 dB, both of which produce a near-identity response but are still computed.
///    An earlier revision used a 1e-4 dB tolerance, copied from the HLM reference; that would
///    have bypassed bands GLM designs, so it was removed.
///
/// 5. Shelving Slope, And Where The Published Specification Is Wrong:
///    Appendix A.3 of the GLM protocol specification states that shelving bands use the
///    cookbook slope form with S fixed at 1.0.  They do not.  Matching designed coefficients
///    against captured ones shows the shelves use the *Q* form, alpha = sin(w0)/(2Q), with a
///    different fixed Q per shelf type - see SHELF_Q_LOW and SHELF_Q_HIGH.
///
///    It is tempting to think a near-flat shelf cannot distinguish the two forms, since at
///    0 dB both produce an identity *response*.  That is true of the response and false of the
///    coefficients: the gain cancels out of H(z) but alpha remains in every normalised
///    coefficient, and the two forms put alpha a factor of sqrt(2) apart at any gain.  So even
///    a 0.02 dB shelf pins Q tightly, which is why both constants below are firm.
/// ===================================================================================

#include <cstdint>

namespace esphome {
namespace gensam {

/// @brief Filter shape occupying one of the 20 PEQ slots.
///
/// The GLM user interface calls PEAKING bands "notches", but the DSP computes a standard
/// peaking/bell section that takes boost as readily as cut.  Shelving bands ignore @c q: the
/// interface hides the parameter and the hardware uses a fixed Q per shelf type (point 5).
enum class PeqType : uint8_t {
  BYPASS = 0,      ///< Unused slot; always transmitted as the identity vector.
  LOW_SHELF = 1,   ///< Low-frequency shelving filter (slots 0-1 on two-way monitors).
  HIGH_SHELF = 2,  ///< High-frequency shelving filter (slots 2-3 on two-way monitors).
  PEAKING = 3,     ///< Peaking / bell filter ("Notch" in GLM).
};

/// @brief One second-order section, normalised and encoded ready for the wire.
///
/// Members are in transmission order.  @c a1 and @c a2 are already divided by a0 and
/// negated, per the sign-inversion convention described at the top of this file.
struct BiquadCoeffs {
  float b0{1.0f};
  float b1{0.0f};
  float b2{0.0f};
  float a1{0.0f};
  float a2{0.0f};
};

/// The identity section the OEM transmits for a bypassed or flat band.
static constexpr BiquadCoeffs BIQUAD_BYPASS{1.0f, 0.0f, 0.0f, 0.0f, 0.0f};

/// @name Fixed shelving Q
///
/// GLM exposes no slope or Q control for the shelving bands, and the values it uses are not
/// the ones the protocol specification documents (see point 5 above). These were recovered by
/// fitting designed coefficients to a capture of GLM applying three calibrated groups. Both
/// land on round numbers, which is the first sign they are the real constants rather than a
/// curve fit that happens to be close; holding frequency and gain at the values from the setup
/// file and sweeping Q alone is the second. That fit reproduces a -4.25 dB low shelf at
/// 6e-08, and pins the high shelf to 0.5 within +/-2e-07 - the float32 noise floor - so
/// neither constant is a guess.
///@{
static constexpr float SHELF_Q_LOW = 0.3f;   ///< Low shelf; fitted over gains to -4.25 dB.
static constexpr float SHELF_Q_HIGH = 0.5f;  ///< High shelf; fitted to +/-2e-07.
///@}

/// @brief Design one second-order section from its user-facing parameters.
///
/// Intermediate arithmetic is done in double to keep rounding drift out of the float32 values
/// that reach the wire; only the final normalised coefficients are narrowed.
///
/// @param type Filter shape; BYPASS returns the identity vector without computing anything.
/// @param frequency_hz Centre frequency (peaking) or corner frequency (shelving), in Hz.
/// @param gain_db Gain in decibels; negative cuts. Exactly 0 yields the bypass vector.
/// @param q Quality factor, used by PEAKING only. The shelving types ignore it and use their
///          own fixed SHELF_Q_LOW / SHELF_Q_HIGH, matching GLM.
/// @param design_rate_hz Sample rate the filter is designed at (48000 two-way, 12000 sub).
/// @return The five normalised, sign-inverted coefficients.  Returns the bypass vector for a
///         flat band, and for any parameter that cannot yield a stable section: a
///         non-positive or above-Nyquist frequency, a non-positive Q on a peaking band, or a
///         vanishing a0.  Falling back rather than failing keeps one bad band in a setup file
///         from aborting a whole group push; the band simply passes audio through.
BiquadCoeffs design_biquad(PeqType type, float frequency_hz, float gain_db, float q,
                           uint32_t design_rate_hz);

}  // namespace gensam
}  // namespace esphome
