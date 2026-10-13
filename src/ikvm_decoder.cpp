#include "ikvm_decoder.hpp"

#include <algorithm>
#include <cstring>

// Tables transcribed verbatim from com.ami.kvm.jviewer.video.JTables. The
// Tbl_*Y/Tbl_*UV quantization tables are genuinely signed bytes in the
// Java source (some entries exceed 127 and wrap negative) - set_quant_table
// relies on that sign-extension, so these stay int8_t, not uint8_t.
namespace jtables {

const uint8_t zigzag[64] = {0,  1,  5,  6,  14, 15, 27, 28, 2,  4,  7,  13, 16, 26, 29, 42,
                             3,  8,  12, 17, 25, 30, 41, 43, 9,  11, 18, 24, 31, 40, 44, 53,
                             10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38, 46, 51, 55, 60,
                             21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63};

// Oversized vs. the 64-entry zigzag table on purpose: a pathological
// zero-run-length sequence can index up to 77 (see process_huffman_data_unit's
// `k = run_pos + run`), and the Java source pads with trailing 63s for
// exactly that.
const uint8_t dezigzag[79] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12,
                               19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42,
                               49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52,
                               45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63, 63, 63, 63, 63,
                               63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63};

const uint8_t std_dc_luminance_nrcodes[17] = {0, 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
const int16_t std_dc_luminance_values[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
const uint8_t std_dc_chrominance_nrcodes[17] = {0, 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
const int16_t std_dc_chrominance_values[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

const uint8_t std_ac_luminance_nrcodes[17] = {0, 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 125};
const int16_t std_ac_luminance_values[162] = {
    1,   2,   3,   0,   4,   17,  5,   18,  33,  49,  65,  6,   19,  81,  97,  7,   34,  113, 20,  50,  129,
    145, 161, 8,   35,  66,  177, 193, 21,  82,  209, 240, 36,  51,  98,  114, 130, 9,   10,  22,  23,  24,
    25,  26,  37,  38,  39,  40,  41,  42,  52,  53,  54,  55,  56,  57,  58,  67,  68,  69,  70,  71,  72,
    73,  74,  83,  84,  85,  86,  87,  88,  89,  90,  99,  100, 101, 102, 103, 104, 105, 106, 115, 116, 117,
    118, 119, 120, 121, 122, 131, 132, 133, 134, 135, 136, 137, 138, 146, 147, 148, 149, 150, 151, 152, 153,
    154, 162, 163, 164, 165, 166, 167, 168, 169, 170, 178, 179, 180, 181, 182, 183, 184, 185, 186, 194, 195,
    196, 197, 198, 199, 200, 201, 202, 210, 211, 212, 213, 214, 215, 216, 217, 218, 225, 226, 227, 228, 229,
    230, 231, 232, 233, 234, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250};

const uint8_t std_ac_chrominance_nrcodes[17] = {0, 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 119};
const int16_t std_ac_chrominance_values[162] = {
    0,   1,   2,   3,   17,  4,   5,   33,  49,  6,   18,  65,  81,  7,   97,  113, 19,  34,  50,  129, 8,
    20,  66,  145, 161, 177, 193, 9,   35,  51,  82,  240, 21,  98,  114, 209, 10,  22,  36,  52,  225, 37,
    241, 23,  24,  25,  26,  38,  39,  40,  41,  42,  53,  54,  55,  56,  57,  58,  67,  68,  69,  70,  71,
    72,  73,  74,  83,  84,  85,  86,  87,  88,  89,  90,  99,  100, 101, 102, 103, 104, 105, 106, 115, 116,
    117, 118, 119, 120, 121, 122, 130, 131, 132, 133, 134, 135, 136, 137, 138, 146, 147, 148, 149, 150, 151,
    152, 153, 154, 162, 163, 164, 165, 166, 167, 168, 169, 170, 178, 179, 180, 181, 182, 183, 184, 185, 186,
    194, 195, 196, 197, 198, 199, 200, 201, 202, 210, 211, 212, 213, 214, 215, 216, 217, 218, 226, 227, 228,
    229, 230, 231, 232, 233, 234, 242, 243, 244, 245, 246, 247, 248, 249, 250};

// Flattened {threshold, code-length} pairs, see Decoder.load_huffman_table.
const int32_t dc_luminance_huffmancode[26] = {0,     0, 16384, 2, 24576, 3, 32768, 3, 40960, 3, 49152, 3, 57344,
                                               3,     61440, 4, 63488, 5, 64512, 6, 65024, 7, 65280, 8, 65535, 9};
const int32_t dc_chrominance_huffmancode[26] = {0,     0, 16384, 2, 32768, 2, 49152, 2, 57344, 3, 61440, 4,  63488,
                                                 5,     64512, 6, 65024, 7, 65280, 8, 65408, 9, 65472, 10, 65535, 11};
const int32_t ac_luminance_huffmancode[78] = {
    0,     0,  16384, 2,  32768, 2,  40960, 3,  45056, 4,  49152, 4,  53248, 4,  55296, 5,  57344, 5,  59392, 5,
    60416, 6,  61440, 6,  61952, 7,  62464, 7,  62976, 7,  63488, 7,  63744, 8,  64000, 8,  64256, 8,  64384, 9,
    64512, 9,  64640, 9,  64768, 9,  64896, 9,  64960, 10, 65024, 10, 65088, 10, 65152, 10, 65216, 10, 65248, 11,
    65280, 11, 65312, 11, 65344, 11, 65360, 12, 65376, 12, 65392, 12, 65408, 12, 65410, 15, 65535, 16};
const int32_t ac_chrominance_huffmancode[90] = {
    0,     0,  16384, 2,  32768, 2,  40960, 3,  45056, 4,  49152, 4,  51200, 5,  53248, 5,  55296, 5,  57344, 5,
    58368, 6,  59392, 6,  60416, 6,  61440, 6,  61952, 7,  62464, 7,  62976, 7,  63232, 8,  63488, 8,  63744, 8,
    64000, 8,  64128, 9,  64256, 9,  64384, 9,  64512, 9,  64640, 9,  64768, 9,  64896, 9,  64960, 10, 65024, 10,
    65088, 10, 65152, 10, 65216, 10, 65248, 11, 65280, 11, 65312, 11, 65344, 11, 65360, 12, 65376, 12, 65392, 12,
    65408, 12, 65412, 14, 65414, 15, 65416, 15, 65535, 16};

const int8_t Tbl_100Y[64] = {2, 1, 1, 2, 3,  5,  6,  7,  1, 1, 1, 2, 3,  7,  7,  6,  1, 1, 2, 3, 5,  7,  8,  7,
                              1, 2, 2, 3, 6,  10, 10, 7,  2, 2, 4, 7, 8,  13, 12, 9,  3, 4, 6, 8, 10, 13, 14, 11,
                              6, 8, 9, 10, 12, 15, 15, 12, 9, 11, 11, 12, 14, 12, 12, 12};
const int8_t Tbl_100UV[64] = {3,  3,  4,  8,  18, 18, 18, 18, 3,  3,  4,  12, 18, 18, 18, 18,
                               4,  4,  10, 18, 18, 18, 18, 18, 8,  12, 18, 18, 18, 18, 18, 18,
                               18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18,
                               18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18};
const int8_t Tbl_086Y[64] = {3, 2, 1, 3, 4,  7,  9,  11, 2, 2, 2, 3,  4,  10, 11, 10, 2,  2,  3,  4,  7,  10, 12, 10,
                              2, 3, 4, 5, 9,  16, 15, 11, 3, 4, 6, 10, 12, 20, 19, 14, 4,  6,  10, 12, 15, 19, 21, 17,
                              9, 12, 14, 16, 19, 22, 22, 18, 13, 17, 17, 18, 21, 18, 19, 18};
const int8_t Tbl_086UV[64] = {4,  5,  6,  13, 27, 27, 27, 27, 5,  5,  7,  18, 27, 27, 27, 27,
                               6,  7,  15, 27, 27, 27, 27, 27, 13, 18, 27, 27, 27, 27, 27, 27,
                               27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
                               27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27};
const int8_t Tbl_071Y[64] = {6,  4,  3,  6,  9,  15, 19, 22, 4,  4,  5,  7,  9,  21, 22, 20,
                              5,  4,  6,  9,  15, 21, 25, 21, 5,  6,  8,  10, 19, 32, 30, 23,
                              6,  8,  13, 21, 25, 40, 38, 28, 9,  13, 20, 24, 30, 39, 42, 34,
                              18, 24, 29, 32, 38, 45, 45, 37, 27, 34, 35, 36, 42, 37, 38, 37};
const int8_t Tbl_071UV[64] = {9,  10, 13, 26, 55, 55, 55, 55, 10, 11, 14, 37, 55, 55, 55, 55,
                               13, 14, 31, 55, 55, 55, 55, 55, 26, 37, 55, 55, 55, 55, 55, 55,
                               55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55,
                               55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55};
const int8_t Tbl_057Y[64] = {9,  6,  5,  9,  13, 22, 28, 34, 6,  6,  7,  10, 14, 32, 33, 30,
                              7,  7,  9,  13, 22, 32, 38, 31, 7,  9,  12, 16, 28, 48, 45, 34,
                              10, 12, 20, 31, 38, 61, 57, 43, 13, 19, 30, 36, 45, 58, 63, 51,
                              27, 36, 43, 48, 57, 68, 67, 56, 40, 51, 53, 55, 63, 56, 57, 55};
const int8_t Tbl_057UV[64] = {13, 14, 19, 38, 80, 80, 80, 80, 14, 17, 21, 53, 80, 80, 80, 80,
                               19, 21, 45, 80, 80, 80, 80, 80, 38, 53, 80, 80, 80, 80, 80, 80,
                               80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80,
                               80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80};
const int8_t Tbl_043Y[64] = {11, 7,  7,  11, 17, 28, 36, 43, 8,  8,  10, 13, 18, 41, 43, 39,
                              10, 9,  11, 17, 28, 40, 49, 40, 10, 12, 15, 20, 36, 62, 57, 44,
                              12, 15, 26, 40, 48, 78, 74, 55, 17, 25, 39, 46, 58, 74, 81, 66,
                              35, 46, 56, 62, 74, 86, 86, 72, 51, 66, 68, 70, 80, 71, 74, 71};
const int8_t Tbl_043UV[64] = {18,  19,  26,  51,  108, 108, 108, 108, 19,  22,  28,  72,  108, 108, 108, 108,
                               26,  28,  61,  108, 108, 108, 108, 108, 51,  72,  108, 108, 108, 108, 108, 108,
                               108, 108, 108, 108, 108, 108, 108, 108, 108, 108, 108, 108, 108, 108, 108, 108,
                               108, 108, 108, 108, 108, 108, 108, 108, 108, 108, 108, 108, 108, 108, 108, 108};
const int8_t Tbl_029Y[64] = {14, 9,  9,  14, 21, 36, 46, 55, 10, 10, 12, 17, 23, 52, 54, 49,
                              12, 11, 14, 21, 36, 51, 62, 50, 12, 15, 19, 26, 46, 78, 72, 56,
                              16, 19, 33, 50, 61, 98, 93, 69, 21, 31, 49, 58, 73, 94, 102, 83,
                              44, 58, 70, 78, 93, 109, 108, 91, 65, 83, 86, 88, 101, 90, 93, 89};
const int8_t Tbl_029UV[64] = {22,   24,   32,   63,   -123, -123, -123, -123, 24,   28,   34,   88,
                               -123, -123, -123, -123, 32,   34,   75,   -123, -123, -123, -123, -123,
                               63,   88,   -123, -123, -123, -123, -123, -123, -123, -123, -123, -123,
                               -123, -123, -123, -123, -123, -123, -123, -123, -123, -123, -123, -123,
                               -123, -123, -123, -123, -123, -123, -123, -123, -123, -123, -123, -123,
                               -123, -123, -123, -123};
const int8_t Tbl_014Y[64] = {17, 12, 10, 17, 26, 43,  55,  66,  13, 13, 15, 20, 28,  63,  65,  60,
                              15, 14, 17, 26, 43, 62,  75,  61,  15, 18, 24, 31, 55,  95,  87,  67,
                              19, 24, 40, 61, 74, 119, 112, 84,  26, 38, 60, 70, 88,  113, 123, 100,
                              53, 70, 85, 95, 112, -124, -125, 110, 78, 100, 103, 107, 122, 109, 112, 108};
const int8_t Tbl_014UV[64] = {27,  29,  39,  76,  -96, -96, -96, -96, 29,  34,  42,  107,
                               -96, -96, -96, -96, 39,  42,  91,  -96, -96, -96, -96, -96,
                               76,  107, -96, -96, -96, -96, -96, -96, -96, -96, -96, -96,
                               -96, -96, -96, -96, -96, -96, -96, -96, -96, -96, -96, -96,
                               -96, -96, -96, -96, -96, -96, -96, -96, -96, -96, -96, -96,
                               -96, -96, -96, -96};
const int8_t Tbl_000Y[64] = {20, 13, 12, 20, 30,  50,   63,   76,  15, 15, 17, 23, 32,   72,   75,  68,
                              17, 16, 20, 30, 50,  71,   86,   70,  17, 21, 27, 36, 63,   108,  100, 77,
                              22, 27, 46, 70, 85,  -120, -128, 96,  30, 43, 68, 80, 101,  -126, -115, 115,
                              61, 80, 97, 108, -128, -105, -106, 126, 90, 115, 118, 122, -116, 125, -128, 123};
const int8_t Tbl_000UV[64] = {31,  33,  45,  88,  -71, -71, -71, -71, 33,  39,  48,  123,
                               -71, -71, -71, -71, 45,  48,  105, -71, -71, -71, -71, -71,
                               88,  123, -71, -71, -71, -71, -71, -71, -71, -71, -71, -71,
                               -71, -71, -71, -71, -71, -71, -71, -71, -71, -71, -71, -71,
                               -71, -71, -71, -71, -71, -71, -71, -71, -71, -71, -71, -71,
                               -71, -71, -71, -71};

const int8_t *luminance_table(int selector) {
  switch (selector) {
    case 0:
      return Tbl_000Y;
    case 1:
      return Tbl_014Y;
    case 2:
      return Tbl_029Y;
    case 3:
      return Tbl_043Y;
    case 4:
      return Tbl_057Y;
    case 5:
      return Tbl_071Y;
    case 6:
      return Tbl_086Y;
    default:
      return Tbl_100Y;
  }
}

const int8_t *chrominance_table_y_mapped(int selector) {
  // JPEGYUVTableMapping == 1 reuses the luminance tables for chrominance.
  return luminance_table(selector);
}

const int8_t *chrominance_table(int selector) {
  switch (selector) {
    case 0:
      return Tbl_000UV;
    case 1:
      return Tbl_014UV;
    case 2:
      return Tbl_029UV;
    case 3:
      return Tbl_043UV;
    case 4:
      return Tbl_057UV;
    case 5:
      return Tbl_071UV;
    case 6:
      return Tbl_086UV;
    default:
      return Tbl_100UV;
  }
}

}  // namespace jtables

namespace {

// IVTPPktHdr's macroblock opcodes (top 4 bits of each macroblock's leading
// word), see Decoder.decode()'s dispatch.
constexpr int kVqNoSkip1Color = 5;
constexpr int kVqNoSkip2Color = 6;
constexpr int kVqNoSkip4Color = 7;
constexpr int kJpegNoSkip = 0;
constexpr int kLowJpegNoSkip = 4;
constexpr int kJpegSkip = 8;
constexpr int kLowJpegSkip = 12;
constexpr int kVqSkip1Color = 13;
constexpr int kVqSkip2Color = 14;
constexpr int kVqSkip4Color = 15;
constexpr int kFrameEnd = 9;

inline uint64_t zext(int32_t v) { return static_cast<uint32_t>(v); }

inline int32_t dequant(int32_t coeff, int64_t qt) {
  // Decoder.IDCT_transform does `(int)((long)coeff * qt) >> 16`: the cast
  // to int truncates the 64-bit product to 32 bits *before* the shift, so
  // this is not equivalent to shifting the full 64-bit product.
  int32_t truncated = static_cast<int32_t>(static_cast<int64_t>(coeff) * qt);
  return truncated >> 16;
}

inline int32_t multiply(int32_t a, int32_t b) { return (a * b) >> 8; }

inline int word_hi_lo(int hi_byte, int lo_byte) { return ((hi_byte & 0xFF) << 8) | (lo_byte & 0xFF); }

// Decoder.java has no bounds check on m_RecvBuffer[_index] in
// updatereadbuf() (only skipKbits() clamps) - an out-of-range read there
// throws ArrayIndexOutOfBoundsException, which decode()'s try/catch
// silently swallows, stopping that frame's decode. We hit the same
// condition near the end of every frame's bitstream, so it's not an edge
// case to harden against - it's how decoding is expected to end. This is
// the C++ equivalent signal, caught at the bottom of decode().
struct StopDecode {};

}  // namespace

IkvmDecoder::IkvmDecoder() {
  for (int n = 1; n < 17; ++n) {
    neg_pow2_[n] = static_cast<int16_t>(1 - (1 << n));
  }

  init_color_table();
  prepare_range_limit_table();

  htdc_[0] = load_huffman_table(jtables::std_dc_luminance_nrcodes, jtables::std_dc_luminance_values,
                                 jtables::dc_luminance_huffmancode);
  htac_[0] = load_huffman_table(jtables::std_ac_luminance_nrcodes, jtables::std_ac_luminance_values,
                                 jtables::ac_luminance_huffmancode);
  htdc_[1] = load_huffman_table(jtables::std_dc_chrominance_nrcodes, jtables::std_dc_chrominance_values,
                                 jtables::dc_chrominance_huffmancode);
  htac_[1] = load_huffman_table(jtables::std_ac_chrominance_nrcodes, jtables::std_ac_chrominance_values,
                                 jtables::ac_chrominance_huffmancode);
}

void IkvmDecoder::init_color_table() {
  auto fix_g = [](double d) { return static_cast<int>(d * 65536.0 + 0.5); };
  int n2 = 65536 >> 1;
  int n4 = -128;
  for (int n3 = 0; n3 < 256; ++n3, ++n4) {
    cr_to_r_[n3] = (fix_g(1.597656) * n4 + n2) >> 16;
    cb_to_b_[n3] = (fix_g(2.015625) * n4 + n2) >> 16;
    cr_to_g_[n3] = (-fix_g(0.8125) * n4 + n2) >> 16;
    cb_to_g_[n3] = (-fix_g(0.390625) * n4 + n2) >> 16;
  }
  n4 = -16;
  for (int n3 = 0; n3 < 256; ++n3, ++n4) {
    y_lut_[n3] = (fix_g(1.164) * n4 + n2) >> 16;
  }
}

void IkvmDecoder::prepare_range_limit_table() {
  std::fill(rlimit_table_, rlimit_table_ + 255, uint8_t{0});
  for (int n = 0; n < 256; ++n) {
    rlimit_table_[n + 256] = static_cast<uint8_t>(n);
    rlimit_table_short_[n + 256] = n;
  }
  std::fill(rlimit_table_ + 512, rlimit_table_ + 895, uint8_t{255});
  std::fill(rlimit_table_short_ + 512, rlimit_table_short_ + 895, 255);
  std::fill(rlimit_table_ + 896, rlimit_table_ + 1279, uint8_t{0});
  std::fill(rlimit_table_short_ + 896, rlimit_table_short_ + 1279, 0);
  for (int n = 1024; n < 1152; ++n) {
    rlimit_table_[n + 256] = static_cast<uint8_t>(n & 0xFF);
    rlimit_table_short_[n + 256] = n & 0xFF;
  }
}

IkvmDecoder::HuffmanTable IkvmDecoder::load_huffman_table(const uint8_t *nrcodes, const int16_t *values,
                                                           const int32_t *huffmancode) {
  HuffmanTable t;
  for (int len = 1; len <= 16; ++len) t.length[len] = nrcodes[len];

  int n = 0;
  for (int len = 1; len <= 16; ++len) {
    for (int k = 0; k < t.length[len]; ++k) {
      t.v[word_hi_lo(len, k)] = values[n];
      ++n;
    }
  }

  int code = 0;
  for (int len = 1; len <= 16; ++len) {
    t.minor_code[len] = code;
    for (int k = 1; k <= t.length[len]; ++k) ++code;
    t.major_code[len] = code - 1;
    code *= 2;
    if (t.length[len] == 0) {
      t.minor_code[len] = -1;
      t.major_code[len] = 0;
    }
  }

  // Direct length lookup: Len[prefix] gives the code length for the 16-bit
  // window value `prefix`, scanning the {threshold, length} pairs.
  t.len[0] = 2;
  n = 2;
  for (int i = 1; i < 65535; ++i) {
    if (i < huffmancode[n]) {
      t.len[i] = static_cast<uint8_t>(huffmancode[n + 1] & 0xFF);
    } else {
      n += 2;
      t.len[i] = static_cast<uint8_t>(huffmancode[n + 1] & 0xFF);
    }
  }
  return t;
}

void IkvmDecoder::set_quant_table(const int8_t *table, int scale, uint8_t *out) {
  for (int n = 0; n < 64; ++n) {
    int n2 = table[n] * 16 / scale;
    if (n2 <= 0) n2 = 1;
    if (n2 > 255) n2 = 255;
    out[jtables::zigzag[n]] = static_cast<uint8_t>(n2);
  }
}

namespace {
// AAN-style scale factors applied on top of the zigzag-ordered quant table,
// see Decoder.load_quant_table.
const float kAanScale[8] = {1.0f, 1.3870399f, 1.306563f, 1.1758755f, 1.0f, 0.78569496f, 0.5411961f, 0.27589938f};
}  // namespace

void IkvmDecoder::load_quant_table(int64_t *out) {
  uint8_t scaled[64];
  set_quant_table(jtables::luminance_table(selector_), scalefactor_, scaled);
  int64_t zz[64];
  for (int n = 0; n <= 63; ++n) zz[n] = scaled[jtables::zigzag[n]];
  int n = 0;
  for (int r = 0; r <= 7; ++r) {
    for (int c = 0; c <= 7; ++c) {
      int64_t v = static_cast<int64_t>(static_cast<float>(zz[n]) * (kAanScale[r] * kAanScale[c]));
      out[n] = v * 65536;
      ++n;
    }
  }
}

void IkvmDecoder::load_quant_table_cb(int64_t *out) {
  const int8_t *table =
      mapping_ == 1 ? jtables::chrominance_table_y_mapped(selector_) : jtables::chrominance_table(selector_);
  uint8_t scaled[64];
  set_quant_table(table, scalefactoruv_, scaled);
  int64_t zz[64];
  for (int n = 0; n <= 63; ++n) zz[n] = scaled[jtables::zigzag[n]];
  int n = 0;
  for (int r = 0; r <= 7; ++r) {
    for (int c = 0; c <= 7; ++c) {
      int64_t v = static_cast<int64_t>(static_cast<float>(zz[n]) * (kAanScale[r] * kAanScale[c]));
      out[n] = v * 65536;
      ++n;
    }
  }
}

void IkvmDecoder::load_advance_quant_table(int64_t *out) {
  uint8_t scaled[64];
  set_quant_table(jtables::luminance_table(advance_selector_), advancescalefactor_, scaled);
  int64_t zz[64];
  for (int n = 0; n <= 63; ++n) zz[n] = scaled[jtables::zigzag[n]];
  int n = 0;
  for (int r = 0; r <= 7; ++r) {
    for (int c = 0; c <= 7; ++c) {
      int64_t v = static_cast<int64_t>(static_cast<float>(zz[n]) * (kAanScale[r] * kAanScale[c]));
      out[n] = v * 65536;
      ++n;
    }
  }
}

void IkvmDecoder::load_advance_quant_table_cb(int64_t *out) {
  const int8_t *table = mapping_ == 1 ? jtables::chrominance_table_y_mapped(advance_selector_)
                                       : jtables::chrominance_table(advance_selector_);
  uint8_t scaled[64];
  set_quant_table(table, advancescalefactoruv_, scaled);
  int64_t zz[64];
  for (int n = 0; n <= 63; ++n) zz[n] = scaled[jtables::zigzag[n]];
  int n = 0;
  for (int r = 0; r <= 7; ++r) {
    for (int c = 0; c <= 7; ++c) {
      int64_t v = static_cast<int64_t>(static_cast<float>(zz[n]) * (kAanScale[r] * kAanScale[c]));
      out[n] = v * 65536;
      ++n;
    }
  }
}

void IkvmDecoder::init_jpg_decoding() {
  load_quant_table(qt_[0]);
  load_quant_table_cb(qt_[1]);
  load_advance_quant_table(qt_[2]);
  load_advance_quant_table_cb(qt_[3]);
}

int16_t IkvmDecoder::look_kbits(int bits) const {
  uint64_t v = zext(recv_buffer_[0]) >> (32 - bits);
  return static_cast<int16_t>(v);
}

void IkvmDecoder::skip_kbits(int bits) {
  if (newbits_ - bits <= 0) {
    if (index_ > static_cast<int>(recv_buffer_.size()) - 1) {
      index_ = static_cast<int>(recv_buffer_.size()) - 1;
    }
    uint64_t next = zext(recv_buffer_[index_]);
    recv_buffer_[0] =
        (recv_buffer_[0] << bits) | static_cast<int32_t>((zext(recv_buffer_[1]) | (next >> newbits_)) >> (32 - bits));
    recv_buffer_[1] = static_cast<int32_t>(next << (bits - newbits_));
    newbits_ = 32 + newbits_ - bits;
    ++index_;
  } else {
    recv_buffer_[0] = (recv_buffer_[0] << bits) | static_cast<int32_t>(zext(recv_buffer_[1]) >> (32 - bits));
    recv_buffer_[1] = recv_buffer_[1] << bits;
    newbits_ -= bits;
  }
}

int16_t IkvmDecoder::get_kbits(int bits) {
  int16_t v = look_kbits(bits);
  if (((1LL << (bits - 1)) & static_cast<int64_t>(v)) == 0) {
    v = static_cast<int16_t>(v + neg_pow2_[bits]);
  }
  skip_kbits(bits);
  return v;
}

void IkvmDecoder::update_readbuf(int bits) {
  if (newbits_ - bits <= 0) {
    if (index_ < 0 || index_ >= static_cast<int>(recv_buffer_.size())) throw StopDecode{};
    uint64_t next = zext(recv_buffer_[index_]);
    ++index_;
    recv_buffer_[0] =
        (recv_buffer_[0] << bits) | static_cast<int32_t>((zext(recv_buffer_[1]) | (next >> newbits_)) >> (32 - bits));
    recv_buffer_[1] = static_cast<int32_t>(next << (bits - newbits_));
    newbits_ = 32 + newbits_ - bits;
  } else {
    recv_buffer_[0] = (recv_buffer_[0] << bits) | static_cast<int32_t>(zext(recv_buffer_[1]) >> (32 - bits));
    recv_buffer_[1] = recv_buffer_[1] << bits;
    newbits_ -= bits;
  }
}

void IkvmDecoder::process_huffman_data_unit(int dc_table, int ac_table, int16_t *pred, int block_offset) {
  std::memset(dct_coeff_, 0, sizeof(dct_coeff_));

  const HuffmanTable &dc = htdc_[dc_table];
  int prefix = static_cast<int>(zext(recv_buffer_[0]) >> 16);
  int len = dc.len[prefix];
  int s2 = look_kbits(len);
  skip_kbits(len);
  int dc_symbol = dc.v[word_hi_lo(len, s2 - dc.minor_code[len])] & 0xFF;

  if (dc_symbol == 0) {
    dct_coeff_[block_offset + 0] = pred[0];
  } else {
    dct_coeff_[block_offset] = pred[0] + get_kbits(dc_symbol);
    pred[0] = static_cast<int16_t>(dct_coeff_[block_offset]);
  }

  const HuffmanTable &ac = htac_[ac_table];
  int run_pos = 1;
  do {
    prefix = static_cast<int>(zext(recv_buffer_[0]) >> 16);
    len = ac.len[prefix];
    s2 = look_kbits(len);
    skip_kbits(len);
    int ac_symbol = ac.v[word_hi_lo(len, s2 - ac.minor_code[len])] & 0xFF;
    int size = ac_symbol & 0xF;
    int run = (ac_symbol >> 4) & 0xF;
    if (size == 0) {
      if (run != 15) break;  // ZRL: skip 16 zero coefficients
      run_pos += 16;
      continue;
    }
    int k = run_pos + run;
    run_pos = k + 1;
    dct_coeff_[block_offset + jtables::dezigzag[k]] = get_kbits(size);
  } while (run_pos < 64);
}

void IkvmDecoder::idct_transform(int block_offset, int qt_index) {
  const int64_t *qt = qt_[qt_index];
  int pos = block_offset;
  int ws = 0;
  int qpos = 0;

  for (int row = 8; row > 0; --row) {
    if ((dct_coeff_[pos + 8] | dct_coeff_[pos + 16] | dct_coeff_[pos + 24] | dct_coeff_[pos + 32] |
         dct_coeff_[pos + 40] | dct_coeff_[pos + 48] | dct_coeff_[pos + 56]) == 0) {
      int32_t v = dequant(dct_coeff_[pos + 0], qt[qpos + 0]);
      workspace_[ws + 0] = v;
      workspace_[ws + 8] = v;
      workspace_[ws + 16] = v;
      workspace_[ws + 24] = v;
      workspace_[ws + 32] = v;
      workspace_[ws + 40] = v;
      workspace_[ws + 48] = v;
      workspace_[ws + 56] = v;
      ++pos;
      ++qpos;
      ++ws;
      continue;
    }

    int32_t t0 = dequant(dct_coeff_[pos + 0], qt[qpos + 0]);
    int32_t t1 = dequant(dct_coeff_[pos + 16], qt[qpos + 16]);
    int32_t t2 = dequant(dct_coeff_[pos + 32], qt[qpos + 32]);
    int32_t t3 = dequant(dct_coeff_[pos + 48], qt[qpos + 48]);
    int32_t a0 = t0 + t2;
    int32_t a1 = t0 - t2;
    int32_t a2 = t1 + t3;
    int32_t a3 = multiply(t1 - t3, 362) - a2;
    t0 = a0 + a2;
    t3 = a0 - a2;
    t1 = a1 + a3;
    t2 = a1 - a3;

    int32_t u0 = dequant(dct_coeff_[pos + 8], qt[qpos + 8]);
    int32_t u1 = dequant(dct_coeff_[pos + 24], qt[qpos + 24]);
    int32_t u2 = dequant(dct_coeff_[pos + 40], qt[qpos + 40]);
    int32_t u3 = dequant(dct_coeff_[pos + 56], qt[qpos + 56]);
    int32_t b0 = u2 + u1;
    int32_t b1 = u2 - u1;
    int32_t b2 = u0 + u3;
    int32_t b3 = u0 - u3;
    int32_t c0 = b2 + b0;
    int32_t c1 = multiply(b2 - b0, 362);
    int32_t c2 = multiply(b1 + b3, 473);
    int32_t c3 = multiply(b3, 277) - c2;
    int32_t c4 = multiply(b1, -669) + c2;
    int32_t d0 = c4 - c0;
    int32_t d1 = c1 - d0;
    int32_t d2 = c3 + d1;

    workspace_[ws + 0] = t0 + c0;
    workspace_[ws + 56] = t0 - c0;
    workspace_[ws + 8] = t1 + d0;
    workspace_[ws + 48] = t1 - d0;
    workspace_[ws + 16] = t2 + d1;
    workspace_[ws + 40] = t2 - d1;
    workspace_[ws + 32] = t3 + d2;
    workspace_[ws + 24] = t3 - d2;
    ++pos;
    ++qpos;
    ++ws;
  }

  ws = 0;
  for (int col = 0; col < 8; ++col) {
    int out_pos = block_offset + col * 8;
    int32_t a0 = workspace_[ws + 0] + workspace_[ws + 4];
    int32_t a1 = workspace_[ws + 0] - workspace_[ws + 4];
    int32_t a2 = workspace_[ws + 2] + workspace_[ws + 6];
    int32_t a3 = multiply(workspace_[ws + 2] - workspace_[ws + 6], 362) - a2;
    int32_t t0 = a0 + a2;
    int32_t t3 = a0 - a2;
    int32_t t1 = a1 + a3;
    int32_t t2 = a1 - a3;

    int32_t b0 = workspace_[ws + 5] + workspace_[ws + 3];
    int32_t b1 = workspace_[ws + 5] - workspace_[ws + 3];
    int32_t b2 = workspace_[ws + 1] + workspace_[ws + 7];
    int32_t b3 = workspace_[ws + 1] - workspace_[ws + 7];
    int32_t c0 = b2 + b0;
    int32_t c1 = multiply(b2 - b0, 362);
    int32_t c2 = multiply(b1 + b3, 473);
    int32_t c3 = multiply(b3, 277) - c2;
    int32_t c4 = multiply(b1, -669) + c2;
    int32_t d0 = c4 - c0;
    int32_t d1 = c1 - d0;
    int32_t d2 = c3 + d1;

    int idx;
    idx = 128 + (((t0 + c0) >> 3) & 0x3FF);
    tile_yuv_[out_pos + 0] = rlimit_table_short_[idx + 256];
    idx = 128 + (((t0 - c0) >> 3) & 0x3FF);
    tile_yuv_[out_pos + 7] = rlimit_table_short_[idx + 256];
    idx = 128 + (((t1 + d0) >> 3) & 0x3FF);
    tile_yuv_[out_pos + 1] = rlimit_table_short_[idx + 256];
    idx = 128 + (((t1 - d0) >> 3) & 0x3FF);
    tile_yuv_[out_pos + 6] = rlimit_table_short_[idx + 256];
    idx = 128 + (((t2 + d1) >> 3) & 0x3FF);
    tile_yuv_[out_pos + 2] = rlimit_table_short_[idx + 256];
    idx = 128 + (((t2 - d1) >> 3) & 0x3FF);
    tile_yuv_[out_pos + 5] = rlimit_table_short_[idx + 256];
    idx = 128 + (((t3 + d2) >> 3) & 0x3FF);
    tile_yuv_[out_pos + 4] = rlimit_table_short_[idx + 256];
    idx = 128 + (((t3 - d2) >> 3) & 0x3FF);
    tile_yuv_[out_pos + 3] = rlimit_table_short_[idx + 256];
    ws += 8;
  }
}

void IkvmDecoder::yuv_to_rgb(int txb, int tyb) {
  if (m_mode420_ == 0) {
    for (int i = 0; i < 64; ++i) {
      py_[i] = tile_yuv_[i];
      pcb_[i] = tile_yuv_[64 + i];
      pcr_[i] = tile_yuv_[64 + 64 + i];
    }
    int x0 = txb * 8;
    int y0 = tyb * 8;
    int row_base = y0 * real_width_ + x0;
    int cols = real_width_ - x0;
    if (cols == 0 || cols > 8) cols = 8;
    for (int r = 0; r < 8; ++r) {
      for (int c = 0; c < cols; ++c) {
        int s = (r << 3) + c;
        int y = py_[s];
        int cb = pcb_[s];
        int cr = pcr_[s];
        int out = (row_base + c) * 3;
        int rr = y_lut_[y] + cr_to_r_[cr];
        int gg = y_lut_[y] + cb_to_g_[cb] + cr_to_g_[cr];
        int bb = y_lut_[y] + cb_to_b_[cb];
        rr = rr >= 0 ? rr + 256 : 0;
        gg = gg >= 0 ? gg + 256 : 0;
        bb = bb >= 0 ? bb + 256 : 0;
        if (out >= real_width_ * real_height_ * 3) continue;
        out_buf_[out] = rlimit_table_[rr];
        out_buf_[out + 1] = rlimit_table_[gg];
        out_buf_[out + 2] = rlimit_table_[bb];
      }
      row_base += real_width_;
    }
  } else {
    int n3 = 0;
    for (int b = 0; b < 4; ++b) {
      for (int i = 0; i < 64; ++i) py420_[b][i] = tile_yuv_[n3++];
    }
    for (int i = 0; i < 64; ++i) {
      pcb_[i] = tile_yuv_[n3];
      pcr_[i] = tile_yuv_[n3 + 64];
      ++n3;
    }

    int x0 = txb * 16;
    int y0 = tyb * 16;
    int row_base = y0 * width_ + x0;
    int quadrant_counter[4] = {0, 0, 0, 0};
    int rows = 16;
    if (height_ == 608 && tyb == 37) rows = 8;

    for (int row = 0; row < rows; ++row) {
      int q_row = (row >> 3) * 2;
      int chroma_row_base = (row >> 1) << 3;
      for (int col = 0; col < 16; ++col) {
        int quadrant = q_row + (col >> 3);
        int sample_in_quadrant = quadrant_counter[quadrant]++;
        int y = py420_[quadrant][sample_in_quadrant];
        int chroma_idx = chroma_row_base + (col >> 1);
        int cb = pcb_[chroma_idx];
        int cr = pcr_[chroma_idx];
        int rr = y_lut_[y] + cr_to_r_[cr];
        int gg = y_lut_[y] + (cb_to_g_[cb] + cr_to_g_[cr]);
        int bb = y_lut_[y] + cb_to_b_[cb];
        int out = (row_base + col) * 3;
        out_buf_[out] = rr >= 0 ? rlimit_table_[rr + 256] : 0;
        out_buf_[out + 1] = gg >= 0 ? rlimit_table_[gg + 256] : 0;
        out_buf_[out + 2] = bb >= 0 ? rlimit_table_[bb + 256] : 0;
      }
      row_base += real_width_;
    }
  }
}

bool IkvmDecoder::decompress(int txb, int tyb, int qt_index) {
  process_huffman_data_unit(0, 0, dcy_, 0);
  idct_transform(0, qt_index);
  if (m_mode420_ == 1) {
    process_huffman_data_unit(0, 0, dcy_, 64);
    idct_transform(64, qt_index);
    process_huffman_data_unit(0, 0, dcy_, 128);
    idct_transform(128, qt_index);
    process_huffman_data_unit(0, 0, dcy_, 192);
    idct_transform(192, qt_index);
    process_huffman_data_unit(1, 1, dccb_, 256);
    idct_transform(256, qt_index + 1);
    process_huffman_data_unit(1, 1, dccr_, 320);
    idct_transform(320, qt_index + 1);
  } else {
    process_huffman_data_unit(1, 1, dccb_, 64);
    idct_transform(64, qt_index + 1);
    process_huffman_data_unit(1, 1, dccr_, 128);
    idct_transform(128, qt_index + 1);
  }
  yuv_to_rgb(txb, tyb);
  return true;
}

void IkvmDecoder::vq_decompress(int txb, int tyb) {
  int n3 = 0;
  if (vq_.bitmap_bits == 0) {
    for (int n4 = 0; n4 < 64; ++n4) {
      int64_t color = vq_.color[vq_.index[0]];
      tile_yuv_[n3 + 0] = static_cast<int>((color & 0xFF0000) >> 16);
      tile_yuv_[n3 + 64] = static_cast<int>((color & 0xFF00) >> 8);
      tile_yuv_[n3 + 128] = static_cast<int>(color & 0xFF);
      ++n3;
    }
  } else {
    for (int n4 = 0; n4 < 64; ++n4) {
      int s = look_kbits(vq_.bitmap_bits);
      int64_t color = vq_.color[vq_.index[s]];
      tile_yuv_[n3 + 0] = static_cast<int>((color & 0xFF0000) >> 16);
      tile_yuv_[n3 + 64] = static_cast<int>((color & 0xFF00) >> 8);
      tile_yuv_[n3 + 128] = static_cast<int>(color & 0xFF);
      ++n3;
      skip_kbits(vq_.bitmap_bits);
    }
  }
  yuv_to_rgb(txb, tyb);
}

void IkvmDecoder::move_block_index() {
  ++txb_;
  if (m_mode420_ == 0) {
    if (txb_ >= tmp_width_by16_ / 8) {
      ++tyb_;
      if (tyb_ >= tmp_height_by16_ / 8) tyb_ = 0;
      txb_ = 0;
    }
  } else if (txb_ >= tmp_width_by16_ / 16) {
    ++tyb_;
    if (tyb_ >= tmp_height_by16_ / 16) tyb_ = 0;
    txb_ = 0;
  }
}

void IkvmDecoder::decode_rc4_setup(const uint8_t *key) {
  rc4_state_.x = 0;
  rc4_state_.y = 0;
  for (int n = 0; n < 256; ++n) rc4_state_.m[n] = n;
  int n2 = 0;
  int n3 = 0;
  for (int n = 0; n < 256; ++n) {
    int32_t n4 = rc4_state_.m[n];
    n3 = (n3 + n4 + key[n2]) & 0xFF;
    rc4_state_.m[n] = rc4_state_.m[n3];
    rc4_state_.m[n3] = n4;
    ++n2;
  }
}

void IkvmDecoder::rc4_crypt(int32_t *data, int count) {
  int x = rc4_state_.x;
  int y = rc4_state_.y;
  int32_t *m = rc4_state_.m;
  for (int i = 0; i < count; ++i) {
    x = (x + 1) & 0xFF;
    int32_t n5 = m[x];
    y = (y + n5) & 0xFF;
    int32_t n4 = m[y];
    m[x] = n4;
    m[y] = n5;
    data[i] = data[i] ^ m[(n5 + n4) & 0xFF];
  }
  rc4_state_.x = x;
  rc4_state_.y = y;
}

std::vector<uint8_t> IkvmDecoder::decode(const IkvmFrameInfo &info, const uint8_t *compressed,
                                          size_t compressed_len) {
  size_t frame_size = static_cast<size_t>(info.dest_width) * info.dest_height * 3;
  if (frame_buf_.size() != frame_size) frame_buf_.assign(frame_size, 0);
  out_buf_ = frame_buf_.data();

  for (int n4 = 0; n4 < 4; ++n4) vq_.index[n4] = n4;
  vq_.color[0] = 32896;
  vq_.color[1] = 0xFF8080;
  vq_.color[2] = 0x808080;
  vq_.color[3] = 0xC08080;

  width_ = info.source_width;
  height_ = info.source_height;
  real_width_ = info.source_width;
  real_height_ = info.source_height;
  m_mode420_ = info.mode420;
  if (m_mode420_ == 1) {
    if (width_ % 16 != 0) width_ += 16 - width_ % 16;
    if (height_ % 16 != 0) height_ += 16 - height_ % 16;
  } else {
    if (width_ % 8 != 0) width_ += 8 - width_ % 8;
    if (height_ % 8 != 0) height_ += 8 - height_ % 8;
  }

  tmp_width_by16_ = info.dest_width;
  tmp_height_by16_ = info.dest_height;
  if (m_mode420_ == 1) {
    if (tmp_width_by16_ % 16 != 0) tmp_width_by16_ += 16 - tmp_width_by16_ % 16;
    if (tmp_height_by16_ % 16 != 0) tmp_height_by16_ += 16 - tmp_height_by16_ % 16;
  } else {
    if (tmp_width_by16_ % 8 != 0) tmp_width_by16_ += 8 - tmp_width_by16_ % 8;
    if (tmp_height_by16_ % 8 != 0) tmp_height_by16_ += 8 - tmp_height_by16_ % 8;
  }

  // Pack the compressed bytes into little-endian 32-bit words, zero-padding
  // to a multiple of 4 - matches JVVideo.MakeIntArray exactly.
  size_t padded_len = compressed_len;
  int pad = static_cast<int>(compressed_len % 4);
  if (pad != 0) padded_len += 4 - pad;
  std::vector<uint8_t> padded(padded_len, 0);
  std::memcpy(padded.data(), compressed, compressed_len);
  recv_buffer_.assign(padded_len / 4, 0);
  for (size_t i = 0; i < recv_buffer_.size(); ++i) {
    size_t b = i * 4;
    recv_buffer_[i] = static_cast<int32_t>(static_cast<uint32_t>(padded[b]) | static_cast<uint32_t>(padded[b + 1]) << 8 |
                                            static_cast<uint32_t>(padded[b + 2]) << 16 |
                                            static_cast<uint32_t>(padded[b + 3]) << 24);
  }

  int32_t word_count = static_cast<int32_t>(info.compress_size / 4);
  if (info.rc4_enable == 1) {
    // Decoder.java's Keys_Expansion(byte[]) tiles DecodeKeys ("fedcba...",
    // 16 bytes) up to 256 entries by writing into the SAME 16-byte array it
    // was given, which throws ArrayIndexOutOfBoundsException on the 17th
    // write - silently caught by decode()'s try/catch, aborting that
    // frame's decode outright. So there's no working original behavior to
    // copy here; this tiles the 16-byte key into a real 256-byte buffer
    // instead, which is the obviously intended behavior. Untested against
    // real encrypted video - our captured frame had RC4Enable=0.
    uint8_t key[256];
    const char *base = "fedcba9876543210";
    for (int n = 0; n < 256; ++n) key[n] = static_cast<uint8_t>(base[n % 16]);
    decode_rc4_setup(key);
    rc4_crypt(recv_buffer_.data(), word_count);
  }

  selector_ = info.jpeg_table_selector;
  advance_selector_ = info.advance_table_selector;
  mapping_ = info.jpeg_yuv_table_mapping;
  scalefactor_ = info.jpeg_scale_factor;
  scalefactoruv_ = info.jpeg_scale_factor;
  advancescalefactor_ = info.advance_scale_factor;
  advancescalefactoruv_ = info.advance_scale_factor;

  init_jpg_decoding();

  index_ = 2;
  tyb_ = 0;
  txb_ = 0;
  newbits_ = 32;
  dcy_[0] = 0;
  dccb_[0] = 0;
  dccr_[0] = 0;

  if (recv_buffer_.size() < 2) return frame_buf_;

  try {
  do {
    uint32_t opcode = (zext(recv_buffer_[0]) >> 28) & 0xFu;
    if (opcode == kJpegNoSkip) {
      update_readbuf(4);
      decompress(txb_, tyb_, 0);
      move_block_index();
    } else if (opcode == kFrameEnd) {
      break;
    } else if (opcode == kJpegSkip) {
      txb_ = static_cast<int>((zext(recv_buffer_[0]) & 0xFF00000) >> 20);
      tyb_ = static_cast<int>((zext(recv_buffer_[0]) & 0xFF000) >> 12);
      update_readbuf(20);
      decompress(txb_, tyb_, 0);
      move_block_index();
    } else if (opcode == kVqNoSkip1Color || opcode == kVqNoSkip2Color || opcode == kVqNoSkip4Color) {
      update_readbuf(4);
      int n_colors = opcode == kVqNoSkip1Color ? 1 : opcode == kVqNoSkip2Color ? 2 : 4;
      vq_.bitmap_bits = opcode == kVqNoSkip1Color ? 0 : opcode == kVqNoSkip2Color ? 1 : 2;
      for (int n4 = 0; n4 < n_colors; ++n4) {
        vq_.index[n4] = static_cast<int>((zext(recv_buffer_[0]) >> 29) & 3);
        if (((zext(recv_buffer_[0]) >> 31) & 1) == 0) {
          update_readbuf(3);
          continue;
        }
        vq_.color[vq_.index[n4]] = (zext(recv_buffer_[0]) >> 5) & 0xFFFFFF;
        update_readbuf(27);
      }
      vq_decompress(txb_, tyb_);
      move_block_index();
    } else if (opcode == kVqSkip1Color || opcode == kVqSkip2Color || opcode == kVqSkip4Color) {
      txb_ = static_cast<int>((zext(recv_buffer_[0]) & 0xFF00000) >> 20);
      tyb_ = static_cast<int>((zext(recv_buffer_[0]) & 0xFF000) >> 12);
      update_readbuf(20);
      int n_colors = opcode == kVqSkip1Color ? 1 : opcode == kVqSkip2Color ? 2 : 4;
      vq_.bitmap_bits = opcode == kVqSkip1Color ? 0 : opcode == kVqSkip2Color ? 1 : 2;
      for (int n4 = 0; n4 < n_colors; ++n4) {
        vq_.index[n4] = static_cast<int>((zext(recv_buffer_[0]) >> 29) & 3);
        if (((zext(recv_buffer_[0]) >> 31) & 1) == 0) {
          update_readbuf(3);
          continue;
        }
        vq_.color[vq_.index[n4]] = (zext(recv_buffer_[0]) >> 5) & 0xFFFFFF;
        update_readbuf(27);
      }
      vq_decompress(txb_, tyb_);
      move_block_index();
    } else if (opcode == kLowJpegNoSkip) {
      update_readbuf(4);
      decompress(txb_, tyb_, 2);
      move_block_index();
    } else if (opcode == kLowJpegSkip) {
      txb_ = static_cast<int>((zext(recv_buffer_[0]) & 0xFF00000) >> 20);
      tyb_ = static_cast<int>((zext(recv_buffer_[0]) & 0xFF000) >> 12);
      update_readbuf(20);
      decompress(txb_, tyb_, 2);
      move_block_index();
    }
    // Any other opcode (1,2,3,10,11) has no handler in the original either
    // - it's dead stream state that would spin forever, so we bail instead
    // of hanging.
    else {
      break;
    }
  } while (index_ <= word_count);
  } catch (const StopDecode &) {
    // Ran past the end of the bitstream mid-macroblock - same outcome as
    // Decoder.java's ArrayIndexOutOfBoundsException getting caught: stop
    // and keep whatever was decoded so far.
  }

  return frame_buf_;
}
