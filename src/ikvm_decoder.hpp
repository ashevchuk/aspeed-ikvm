#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Parsed subset of ASP2000ImgHdr (see ikvm_session.hpp's frame layout
// comment) that AMI JViewer's Decoder.decode(VideoEngineInfo, int[]) reads.
// Field names match VideoEngineInfo.java's nested structs.
struct IkvmFrameInfo {
  int source_width = 0;
  int source_height = 0;
  int dest_width = 0;
  int dest_height = 0;
  int mode420 = 1;
  int rc4_enable = 0;
  uint32_t compress_size = 0;
  int jpeg_scale_factor = 0;
  uint32_t number_of_mb = 0;
  int advance_scale_factor = 0;
  int jpeg_table_selector = 0;
  int advance_table_selector = 0;
  int jpeg_yuv_table_mapping = 0;
};

// Faithful port of AMI JViewer's com.ami.kvm.jviewer.video.Decoder: the
// ASPEED AST2000/ASP-2000 macroblock codec used by its KVM video channel.
// It reuses JPEG's DCT/quantization/Huffman math but frames it with its own
// per-macroblock opcodes (plain JPEG block, "low quality" JPEG block, or a
// VQ-coded flat-color fill, each either at the current position or at an
// explicit skip-to position) - not a standard JPEG/JFIF bitstream, so it
// can't be handed to a normal JPEG decoder.
//
// This is a line-by-line translation, not a redesign: variable names and
// control flow deliberately track Decoder.java (see the decompiled source
// this was ported from) so a decode mismatch can be diffed against it
// directly instead of re-derived from scratch.
class IkvmDecoder {
 public:
  IkvmDecoder();

  // Decodes one frame's compressed payload - the bytes after the combined
  // VideoHdr+ASP2000ImgHdr header, i.e. frame[125:] from
  // IkvmSession::read_video_frame() - into an RGB24 buffer of
  // info.dest_width * info.dest_height * 3 bytes, row-major, no padding.
  std::vector<uint8_t> decode(const IkvmFrameInfo &info, const uint8_t *compressed, size_t compressed_len);

 private:
  struct HuffmanTable {
    int length[17] = {};
    int minor_code[17] = {};
    int major_code[17] = {};
    std::vector<int16_t> v = std::vector<int16_t>(65536, 0);
    std::vector<uint8_t> len = std::vector<uint8_t>(65536, 0);
  };

  // --- built once, frame-invariant (Decoder's constructor + init_jpg_table) ---
  void init_color_table();
  void prepare_range_limit_table();
  HuffmanTable load_huffman_table(const uint8_t *nrcodes, const int16_t *values, const int32_t *huffmancode);

  // --- rebuilt every decode() call, depends on this frame's header fields ---
  void set_quant_table(const int8_t *table, int scale, uint8_t *out);
  void load_quant_table(int64_t *out);
  void load_quant_table_cb(int64_t *out);
  void load_advance_quant_table(int64_t *out);
  void load_advance_quant_table_cb(int64_t *out);
  void init_jpg_decoding();

  // --- bitstream + macroblock decode, operating on recv_buffer_/index_/newbits_ ---
  int16_t look_kbits(int bits) const;
  void skip_kbits(int bits);
  int16_t get_kbits(int bits);
  void update_readbuf(int bits);
  void process_huffman_data_unit(int dc_table, int ac_table, int16_t *pred, int block_offset);
  void idct_transform(int block_offset, int qt_index);
  void yuv_to_rgb(int txb, int tyb);
  bool decompress(int txb, int tyb, int qt_index);
  void vq_decompress(int txb, int tyb);
  void move_block_index();

  void rc4_crypt(int32_t *data, int count);
  void decode_rc4_setup(const uint8_t *key);

  // --- frame-invariant tables ---
  HuffmanTable htdc_[4];
  HuffmanTable htac_[4];
  uint8_t rlimit_table_[1408] = {};
  int rlimit_table_short_[1408] = {};
  int cr_to_r_[256] = {};
  int cb_to_b_[256] = {};
  int cr_to_g_[256] = {};
  int cb_to_g_[256] = {};
  int y_lut_[256] = {};
  int16_t neg_pow2_[17] = {};

  // --- per-decode-call state (reset at the top of decode()) ---
  int64_t qt_[4][64] = {};
  int selector_ = 0;
  int advance_selector_ = 0;
  int mapping_ = 0;
  int scalefactor_ = 0;
  int scalefactoruv_ = 0;
  int advancescalefactor_ = 0;
  int advancescalefactoruv_ = 0;
  int m_mode420_ = 1;
  int width_ = 0;
  int height_ = 0;
  int real_width_ = 0;
  int real_height_ = 0;
  int tmp_width_by16_ = 0;
  int tmp_height_by16_ = 0;
  int txb_ = 0;
  int tyb_ = 0;
  int16_t dcy_[1] = {};
  int16_t dccb_[1] = {};
  int16_t dccr_[1] = {};
  int32_t dct_coeff_[384] = {};
  int tile_yuv_[768] = {};
  int workspace_[64] = {};
  int py_[768] = {};
  int py420_[4][64] = {};
  int pcb_[64] = {};
  int pcr_[64] = {};
  struct {
    int64_t color[4] = {};
    int index[4] = {};
    int bitmap_bits = 0;
  } vq_;
  struct {
    int x = 0;
    int y = 0;
    int32_t m[256] = {};
  } rc4_state_;

  std::vector<int32_t> recv_buffer_;
  int index_ = 0;
  int newbits_ = 0;

  uint8_t *out_buf_ = nullptr;

  // Persists across decode() calls: the protocol has per-macroblock opcodes
  // that explicitly skip ahead to a new (txb,tyb) position (see
  // move_block_index()'s callers), leaving the macroblocks in between
  // untouched on the wire because the BMC expects the viewer to already
  // have their last-decoded content on screen. A fresh buffer each call
  // would render those skipped blocks as black instead. Only cleared when
  // the resolution changes.
  std::vector<uint8_t> frame_buf_;
};
