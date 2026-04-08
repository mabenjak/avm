/*
 * Copyright (c) 2026, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 3-Clause Clear License
 * and the Alliance for Open Media Patent License 1.0. If the BSD 3-Clause Clear
 * License was not distributed with this source code in the LICENSE file, you
 * can obtain it at aomedia.org/license/software-license/bsd-3-c-c/.  If the
 * Alliance for Open Media Patent License 1.0 was not distributed with this
 * source code in the PATENTS file, you can obtain it at
 * aomedia.org/license/patent-license/.
 */

// Stream switch muxer tool
// ========================
//
// Splices two single-layer AV2 bitstreams at an S-frame boundary.
// Outputs TUs from stream 1 up to the switch point, then outputs TUs
// from stream 2 from the corresponding S-frame onward.

#include <setjmp.h>

#include "avm/internal/avm_codec_internal.h"
#include "av2/common/av2_common_int.h"
#include "av2/decoder/decodeframe.h"
#include "tools/stream_mux.h"

// ---------------------------------------------------------------------------
// Standalone sequence header parser (no AV2Decoder needed)
// ---------------------------------------------------------------------------

static void standalone_error_handler(void *data, avm_codec_err_t error,
                                     const char *detail) {
  struct avm_internal_error_info *info = (struct avm_internal_error_info *)data;
  avm_internal_error(info, error, "%s", detail);
}

// Reimplemented from obu.c (static there, not linkable).
static int read_bitstream_level(AV2_LEVEL *seq_level_idx,
                                struct avm_read_bit_buffer *rb) {
  *seq_level_idx = avm_rb_read_literal(rb, LEVEL_BITS);
  if (!is_valid_seq_level_idx(*seq_level_idx)) return 0;
  return 1;
}

static void read_tlayer_dependency_info(SequenceHeader *const seq,
                                        struct avm_read_bit_buffer *rb) {
  const int max_mlayer_id = seq->max_mlayer_id;
  const int max_tlayer_id = seq->max_tlayer_id;
  const int multi_tlayer_flag = seq->multi_tlayer_dependency_map_present_flag;
  for (int curr_mlayer_id = 0; curr_mlayer_id <= max_mlayer_id;
       curr_mlayer_id++) {
    for (int curr_tlayer_id = 1; curr_tlayer_id <= max_tlayer_id;
         curr_tlayer_id++) {
      for (int ref_tlayer_id = curr_tlayer_id; ref_tlayer_id >= 0;
           ref_tlayer_id--) {
        if (multi_tlayer_flag > 0 || curr_mlayer_id == 0) {
          seq->tlayer_dependency_map[curr_mlayer_id][curr_tlayer_id]
                                    [ref_tlayer_id] = avm_rb_read_bit(rb);
        } else {
          seq->tlayer_dependency_map[curr_mlayer_id][curr_tlayer_id]
                                    [ref_tlayer_id] =
              seq->tlayer_dependency_map[0][curr_tlayer_id][ref_tlayer_id];
        }
      }
    }
  }
}

static void read_mlayer_dependency_info(SequenceHeader *const seq,
                                        struct avm_read_bit_buffer *rb) {
  const int max_mlayer_id = seq->max_mlayer_id;
  for (int curr_mlayer_id = 1; curr_mlayer_id <= max_mlayer_id;
       curr_mlayer_id++) {
    for (int ref_mlayer_id = curr_mlayer_id; ref_mlayer_id >= 0;
         ref_mlayer_id--) {
      seq->mlayer_dependency_map[curr_mlayer_id][ref_mlayer_id] =
          avm_rb_read_bit(rb);
    }
  }
}

// Parse a sequence header OBU payload into a SequenceHeader struct.
// Returns 0 on success, -1 on parse error.
static int parse_seq_header_obu_payload(const uint8_t *payload,
                                        size_t payload_size,
                                        SequenceHeader *seq_params) {
  struct avm_internal_error_info error_info;
  memset(&error_info, 0, sizeof(error_info));
  error_info.setjmp = 1;
  if (setjmp(error_info.jmp)) {
    return -1;
  }

  struct avm_read_bit_buffer rb_storage;
  memset(&rb_storage, 0, sizeof(rb_storage));
  struct avm_read_bit_buffer *rb = &rb_storage;
  rb->bit_offset = 0;
  rb->bit_buffer = payload;
  rb->bit_buffer_end = payload + payload_size;
  rb->error_handler = standalone_error_handler;
  rb->error_handler_data = &error_info;

  memset(seq_params, 0, sizeof(*seq_params));

  const uint32_t saved_bit_offset = rb->bit_offset;

  uint32_t seq_header_id = avm_rb_read_uvlc(rb);
  if (seq_header_id >= MAX_SEQ_NUM) return -1;
  seq_params->seq_header_id = seq_header_id;

  seq_params->seq_profile_idc = av2_read_profile(rb);
  if (seq_params->seq_profile_idc >= MAX_PROFILES) return -1;

  seq_params->single_picture_header_flag = avm_rb_read_bit(rb);
  if (!read_bitstream_level(&seq_params->seq_max_level_idx, rb)) return -1;

  if (seq_params->seq_max_level_idx >= SEQ_LEVEL_4_0 &&
      !seq_params->single_picture_header_flag)
    seq_params->seq_tier = avm_rb_read_bit(rb);
  else
    seq_params->seq_tier = 0;

  av2_read_chroma_format_bitdepth(rb, seq_params, &error_info);

  if (seq_params->single_picture_header_flag) {
    seq_params->seq_lcr_id = LCR_ID_UNSPECIFIED;
    seq_params->still_picture = 1;
    seq_params->max_tlayer_id = 0;
    seq_params->max_mlayer_id = 0;
    seq_params->seq_max_mlayer_cnt = 1;
  } else {
    int seq_lcr_id = avm_rb_read_literal(rb, 3);
    if (seq_lcr_id > MAX_NUM_SEQ_LCR_ID) return -1;
    seq_params->seq_lcr_id = seq_lcr_id;
    seq_params->still_picture = avm_rb_read_bit(rb);
    seq_params->max_tlayer_id = avm_rb_read_literal(rb, TLAYER_BITS);
    seq_params->max_mlayer_id = avm_rb_read_literal(rb, MLAYER_BITS);
    if (seq_params->max_mlayer_id > 0) {
      int n = avm_ceil_log2(seq_params->max_mlayer_id + 1);
      int seq_max_mlayer_cnt_minus_1 = avm_rb_read_literal(rb, n);
      if (seq_max_mlayer_cnt_minus_1 > seq_params->max_mlayer_id) return -1;
      seq_params->seq_max_mlayer_cnt = seq_max_mlayer_cnt_minus_1 + 1;
    } else {
      seq_params->seq_max_mlayer_cnt = 1;
    }
  }

  const int num_bits_width = avm_rb_read_literal(rb, 4) + 1;
  const int num_bits_height = avm_rb_read_literal(rb, 4) + 1;
  const int max_frame_width = avm_rb_read_literal(rb, num_bits_width) + 1;
  const int max_frame_height = avm_rb_read_literal(rb, num_bits_height) + 1;
  seq_params->num_bits_width = num_bits_width;
  seq_params->num_bits_height = num_bits_height;
  seq_params->max_frame_width = max_frame_width;
  seq_params->max_frame_height = max_frame_height;

  av2_read_conformance_window(rb, seq_params);

  if (seq_params->single_picture_header_flag) {
    seq_params->decoder_model_info_present_flag = 0;
    seq_params->display_model_info_present_flag = 0;
  } else {
    seq_params->seq_max_display_model_info_present_flag = avm_rb_read_bit(rb);
    seq_params->seq_max_initial_display_delay_minus_1 =
        BUFFER_POOL_MAX_SIZE - 1;
    if (seq_params->seq_max_display_model_info_present_flag)
      seq_params->seq_max_initial_display_delay_minus_1 =
          avm_rb_read_literal(rb, 4);
    seq_params->decoder_model_info_present_flag = avm_rb_read_bit(rb);
    if (seq_params->decoder_model_info_present_flag) {
      seq_params->decoder_model_info.num_units_in_decoding_tick =
          avm_rb_read_unsigned_literal(rb, 32);
      seq_params->seq_max_decoder_model_present_flag = avm_rb_read_bit(rb);
      if (seq_params->seq_max_decoder_model_present_flag) {
        seq_params->seq_max_decoder_buffer_delay = avm_rb_read_uvlc(rb);
        seq_params->seq_max_encoder_buffer_delay = avm_rb_read_uvlc(rb);
        seq_params->seq_max_low_delay_mode_flag = avm_rb_read_bit(rb);
      } else {
        seq_params->seq_max_decoder_buffer_delay = 70000;
        seq_params->seq_max_encoder_buffer_delay = 20000;
        seq_params->seq_max_low_delay_mode_flag = 0;
      }
    } else {
      seq_params->decoder_model_info.num_units_in_decoding_tick = 1;
      seq_params->seq_max_decoder_buffer_delay = 70000;
      seq_params->seq_max_encoder_buffer_delay = 20000;
      seq_params->seq_max_low_delay_mode_flag = 0;
    }
    // Skip the bitrate validation that the real decoder does — not needed
    // for field comparison.
  }

  setup_default_embedded_layer_dependency_structure(seq_params);
  setup_default_temporal_layer_dependency_structure(seq_params);

  seq_params->mlayer_dependency_present_flag = 0;
  if (seq_params->max_mlayer_id > 0) {
    seq_params->mlayer_dependency_present_flag = avm_rb_read_bit(rb);
    if (seq_params->mlayer_dependency_present_flag) {
      read_mlayer_dependency_info(seq_params, rb);
    }
  }

  seq_params->tlayer_dependency_present_flag = 0;
  seq_params->multi_tlayer_dependency_map_present_flag = 0;
  if (seq_params->max_tlayer_id > 0) {
    seq_params->tlayer_dependency_present_flag = avm_rb_read_bit(rb);
    if (seq_params->tlayer_dependency_present_flag) {
      if (seq_params->max_mlayer_id > 0) {
        seq_params->multi_tlayer_dependency_map_present_flag =
            avm_rb_read_bit(rb);
      }
      read_tlayer_dependency_info(seq_params, rb);
    }
  }

  av2_read_sequence_header(rb, seq_params);
  seq_params->film_grain_params_present = avm_rb_read_bit(rb);

  size_t bits_before_ext = rb->bit_offset - saved_bit_offset;
  seq_params->seq_extension_present_flag = avm_rb_read_bit(rb);
  if (seq_params->seq_extension_present_flag) {
    int extension_bits = read_obu_extension_bits(
        rb->bit_buffer, rb->bit_buffer_end - rb->bit_buffer, bits_before_ext,
        &error_info);
    if (extension_bits > 0) {
      rb->bit_offset += extension_bits;
    }
  }

  return 0;
}

// Extract OBU payload pointer and size from raw OBU bytes
// (strips ULEB size prefix + OBU header byte(s)).
static bool get_obu_payload(const std::vector<uint8_t> &obu_bytes,
                            const uint8_t **payload, size_t *payload_size) {
  if (obu_bytes.empty()) return false;
  const uint8_t *data = obu_bytes.data();
  size_t total = obu_bytes.size();

  // Read ULEB size prefix
  size_t length_field_size = 0;
  uint64_t obu_total_size = 0;
  if (avm_uleb_decode(data, total, &obu_total_size, &length_field_size) != 0)
    return false;

  const uint8_t *obu_start = data + length_field_size;
  size_t obu_size = static_cast<size_t>(obu_total_size);

  // OBU header byte
  if (obu_size < 1) return false;
  const uint8_t obu_header_byte = obu_start[0];
  int header_size = 1;
  bool has_extension =
      (obu_header_byte >> kObuExtensionFlagBitShift) & kObuExtensionFlagBitMask;
  if (has_extension) header_size = 2;

  if ((size_t)header_size >= obu_size) return false;
  *payload = obu_start + header_size;
  *payload_size = obu_size - header_size;
  return true;
}

// Print field-by-field diff of two parsed SequenceHeader structs.
// Compares fields up to op_params (matching are_seq_headers_consistent scope).
static void print_seq_header_diff(const SequenceHeader &s1,
                                  const SequenceHeader &s2) {
  int diff_count = 0;

// clang-format off
#define CMP_FIELD(field)                                            \
  if (s1.field != s2.field) {                                       \
    fprintf(stderr, "  %-50s: %d vs %d\n", #field,                 \
            (int)s1.field, (int)s2.field);                          \
    ++diff_count;                                                   \
  }
// clang-format on

  CMP_FIELD(seq_header_id);
  CMP_FIELD(seq_lcr_id);
  CMP_FIELD(num_bits_width);
  CMP_FIELD(num_bits_height);
  CMP_FIELD(max_frame_width);
  CMP_FIELD(max_frame_height);
  CMP_FIELD(sb_size);
  CMP_FIELD(mib_size);
  CMP_FIELD(mib_size_log2);
  CMP_FIELD(enable_explicit_ref_frame_map);
  CMP_FIELD(def_max_drl_bits);
  CMP_FIELD(allow_frame_max_drl_bits);
  CMP_FIELD(def_max_bvp_drl_bits);
  CMP_FIELD(allow_frame_max_bvp_drl_bits);
  CMP_FIELD(num_same_ref_compound);
  CMP_FIELD(ref_frames);
  CMP_FIELD(ref_frames_log2);

  // OrderHintInfo
  CMP_FIELD(order_hint_info.order_hint_bits_minus_1);
  CMP_FIELD(order_hint_info.enable_ref_frame_mvs);
  CMP_FIELD(order_hint_info.reduced_ref_frame_mvs_mode);

  CMP_FIELD(force_screen_content_tools);
  CMP_FIELD(still_picture);
  CMP_FIELD(single_picture_header_flag);
  CMP_FIELD(force_integer_mv);
  CMP_FIELD(enable_tcq);
  CMP_FIELD(enable_sdp);
  CMP_FIELD(enable_extended_sdp);
  CMP_FIELD(enable_mrls);
  CMP_FIELD(enable_tip);
  CMP_FIELD(enable_tip_hole_fill);
  CMP_FIELD(enable_tip_refinemv);
  CMP_FIELD(enable_tip_explicit_qp);
  CMP_FIELD(enable_mv_traj);
  CMP_FIELD(enable_bawp);
  CMP_FIELD(enable_cwp);
  CMP_FIELD(enable_imp_msk_bld);
  CMP_FIELD(enable_fsc);
  CMP_FIELD(enable_idtx_intra);
  CMP_FIELD(enable_intra_dip);
  CMP_FIELD(enable_intra_edge_filter);
  CMP_FIELD(enable_ist);
  CMP_FIELD(enable_inter_ist);
  CMP_FIELD(enable_chroma_dctonly);
  CMP_FIELD(enable_cfl_intra);
  CMP_FIELD(enable_mhccp);
  CMP_FIELD(enable_inter_ddt);
  CMP_FIELD(reduced_tx_part_set);
  CMP_FIELD(enable_cctx);
  CMP_FIELD(enable_ibp);
  CMP_FIELD(enable_adaptive_mvd);
  CMP_FIELD(enable_flex_mvres);
  CMP_FIELD(cfl_ds_filter_index);
  CMP_FIELD(enable_joint_mvd);
  CMP_FIELD(enable_refinemv);
  CMP_FIELD(enable_mvd_sign_derive);
  CMP_FIELD(seq_enabled_motion_modes);
  CMP_FIELD(seq_frame_motion_modes_present_flag);
  CMP_FIELD(enable_six_param_warp_delta);
  CMP_FIELD(enable_masked_compound);
  CMP_FIELD(enable_opfl_refine);
  CMP_FIELD(disable_loopfilters_across_tiles);
  CMP_FIELD(enable_cdef);
  CMP_FIELD(enable_gdf);
  CMP_FIELD(gdf_unit_matches_sb_size);
  CMP_FIELD(enable_restoration);
  CMP_FIELD(enable_ccso);
  CMP_FIELD(ccso_unit_matches_sb_size);
  CMP_FIELD(enable_lf_sub_pu);
  CMP_FIELD(enable_refmvbank);
  CMP_FIELD(enable_bru);
  CMP_FIELD(enable_drl_reorder);
  CMP_FIELD(enable_cdef_on_skip_txfm);
  CMP_FIELD(enable_avg_cdf);
  CMP_FIELD(avg_cdf_type);
  CMP_FIELD(lr_tools_disable_mask[0]);
  CMP_FIELD(lr_tools_disable_mask[1]);
  CMP_FIELD(enable_parity_hiding);
  CMP_FIELD(enable_ext_partitions);
  CMP_FIELD(enable_uneven_4way_partitions);
  CMP_FIELD(max_pb_aspect_ratio_log2_m1);
  CMP_FIELD(enable_global_motion);
  CMP_FIELD(enable_short_refresh_frame_flags);
  CMP_FIELD(number_of_bits_for_lt_frame_id);
  CMP_FIELD(enable_ext_seg);

  CMP_FIELD(seq_max_level_idx);
  CMP_FIELD(seq_tier);
  CMP_FIELD(seq_max_display_model_info_present_flag);
  CMP_FIELD(seq_max_initial_display_delay_minus_1);
  CMP_FIELD(seq_max_decoder_model_present_flag);
  CMP_FIELD(seq_max_decoder_buffer_delay);
  CMP_FIELD(seq_max_encoder_buffer_delay);
  CMP_FIELD(seq_max_low_delay_mode_flag);
  CMP_FIELD(seq_profile_idc);
  CMP_FIELD(seq_max_mlayer_cnt);

  // Color config
  CMP_FIELD(bit_depth);
  CMP_FIELD(monochrome);
  CMP_FIELD(subsampling_x);
  CMP_FIELD(subsampling_y);
  CMP_FIELD(equal_ac_dc_q);
  CMP_FIELD(separate_uv_delta_q);
  CMP_FIELD(base_y_dc_delta_q);
  CMP_FIELD(base_uv_dc_delta_q);
  CMP_FIELD(base_uv_ac_delta_q);
  CMP_FIELD(y_dc_delta_q_enabled);
  CMP_FIELD(uv_dc_delta_q_enabled);
  CMP_FIELD(uv_ac_delta_q_enabled);
  CMP_FIELD(film_grain_params_present);

  // Tile params
  CMP_FIELD(seq_tile_info_present_flag);
  CMP_FIELD(tile_params.allow_tile_info_change);

  // Operating points
  CMP_FIELD(operating_points_cnt_minus_1);
  for (int i = 0; i <= AVMMAX(s1.operating_points_cnt_minus_1,
                               s2.operating_points_cnt_minus_1);
       ++i) {
    if (s1.operating_point_idc[i] != s2.operating_point_idc[i]) {
      fprintf(stderr, "  operating_point_idc[%d]%*s: %d vs %d\n", i,
              (i < 10) ? 35 : 34, "", s1.operating_point_idc[i],
              s2.operating_point_idc[i]);
      ++diff_count;
    }
  }
  CMP_FIELD(decoder_model_info_present_flag);
  CMP_FIELD(decoder_model_info.num_units_in_decoding_tick);
  CMP_FIELD(display_model_info_present_flag);

  // Layer dependency
  CMP_FIELD(max_tlayer_id);
  CMP_FIELD(max_mlayer_id);
  CMP_FIELD(tlayer_dependency_present_flag);
  CMP_FIELD(mlayer_dependency_present_flag);
  CMP_FIELD(multi_tlayer_dependency_map_present_flag);

  if (memcmp(s1.tlayer_dependency_map, s2.tlayer_dependency_map,
             sizeof(s1.tlayer_dependency_map)) != 0) {
    fprintf(stderr, "  %-50s: differs\n", "tlayer_dependency_map[][][]");
    ++diff_count;
  }
  if (memcmp(s1.mlayer_dependency_map, s2.mlayer_dependency_map,
             sizeof(s1.mlayer_dependency_map)) != 0) {
    fprintf(stderr, "  %-50s: differs\n", "mlayer_dependency_map[][]");
    ++diff_count;
  }

  CMP_FIELD(df_par_bits_minus2);

  // CropWindow
  CMP_FIELD(conf.conf_win_enabled_flag);
  CMP_FIELD(conf.conf_win_left_offset);
  CMP_FIELD(conf.conf_win_right_offset);
  CMP_FIELD(conf.conf_win_top_offset);
  CMP_FIELD(conf.conf_win_bottom_offset);

  // Segmentation
  CMP_FIELD(seq_seg_info_present_flag);
  CMP_FIELD(seg_params.allow_seg_info_change);
  CMP_FIELD(seg_params.last_active_segid);
  CMP_FIELD(seg_params.segid_preskip);
  CMP_FIELD(seg_params.enable_ext_seg);
  CMP_FIELD(allow_seg_info_change);
  CMP_FIELD(seq_extension_present_flag);

#undef CMP_FIELD

  if (diff_count == 0)
    fprintf(stderr, "  (no field differences detected by parser)\n");
  else
    fprintf(stderr, "  Total: %d field(s) differ\n", diff_count);
}

static std::vector<uint8_t> extract_seq_header_obu(const uint8_t *data,
                                                    int length) {
  const int kObuHeaderSizeBytes = 1;
  const int kMinimumBytesRequired = 1 + kObuHeaderSizeBytes;
  int consumed = 0;

  while (consumed < length) {
    const int remaining = length - consumed;
    if (remaining < kMinimumBytesRequired) break;

    size_t length_field_size = 0;
    uint64_t obu_total_size = 0;

    if (avm_uleb_decode(data + consumed, remaining, &obu_total_size,
                        &length_field_size) != 0) {
      break;
    }

    const uint8_t obu_header_byte = *(data + consumed + length_field_size);
    ObuHeader obu_header;
    memset(&obu_header, 0, sizeof(obu_header));
    if (!ParseAV2ObuHeader(obu_header_byte, &obu_header)) break;

    const int obu_with_prefix =
        static_cast<int>(obu_total_size) + static_cast<int>(length_field_size);

    if (obu_header.type == OBU_SEQUENCE_HEADER) {
      return std::vector<uint8_t>(data + consumed,
                                  data + consumed + obu_with_prefix);
    }

    consumed += obu_with_prefix;
  }

  return std::vector<uint8_t>();
}

static bool tu_contains_sframe(const uint8_t *data, int length, bool verbose,
                                bool detect_switch, bool detect_ras,
                                int *obu_count) {
  const int kObuHeaderSizeBytes = 1;
  const int kMinimumBytesRequired = 1 + kObuHeaderSizeBytes;
  int consumed = 0;
  bool found = false;
  int count = 0;

  while (consumed < length) {
    const int remaining = length - consumed;
    if (remaining < kMinimumBytesRequired) break;

    size_t length_field_size = 0;
    uint64_t obu_total_size = 0;

    if (avm_uleb_decode(data + consumed, remaining, &obu_total_size,
                        &length_field_size) != 0) {
      fprintf(stderr, "OBU size parsing failed at offset %d.\n", consumed);
      break;
    }

    const uint8_t obu_header_byte = *(data + consumed + length_field_size);
    ObuHeader obu_header;
    memset(&obu_header, 0, sizeof(obu_header));
    if (!ParseAV2ObuHeader(obu_header_byte, &obu_header)) {
      fprintf(stderr, "OBU parsing failed at offset %d.\n",
              consumed + static_cast<int>(length_field_size));
      break;
    }

    if (obu_header.obu_header_extension_flag) {
      const uint8_t obu_header_ext_byte =
          *(data + consumed + length_field_size + kObuHeaderSizeBytes);
      ParseAV2ObuHeaderExtension(obu_header_ext_byte, &obu_header);
    }

    ++count;

    if (verbose) {
      printf("        OBU: type=%s size=%llu tlayer_id=%d",
             avm_obu_type_to_string(static_cast<OBU_TYPE>(obu_header.type)),
             static_cast<unsigned long long>(obu_total_size),
             obu_header.obu_tlayer_id);
      if (obu_header.obu_header_extension_flag) {
        printf(" mlayer_id=%d xlayer_id=%d", obu_header.obu_mlayer_id,
               obu_header.obu_xlayer_id);
      }
      printf("\n");
    }

    if ((detect_switch && obu_header.type == OBU_SWITCH) ||
        (detect_ras && obu_header.type == OBU_RAS_FRAME)) {
      found = true;
    }

    consumed +=
        static_cast<int>(obu_total_size) + static_cast<int>(length_field_size);
  }

  if (obu_count) *obu_count += count;
  return found;
}

int main(int argc, const char *argv[]) {
  int skip_switches = 0;
  bool verbose = false;
  bool detect_switch = true;
  bool detect_ras = true;
  bool check_seq_params = true;
  int align_tu = -1;  // -1=disabled, 0=use stream2 S-frame TU index, >0=explicit
  int arg_idx = 1;

  // Parse optional flags
  while (arg_idx < argc && argv[arg_idx][0] == '-') {
    if (strcmp(argv[arg_idx], "--skip-switches") == 0) {
      if (arg_idx + 1 >= argc) {
        fprintf(stderr, "Error: --skip-switches requires a value.\n");
        return EXIT_FAILURE;
      }
      skip_switches = atoi(argv[++arg_idx]);
      if (skip_switches < 0) {
        fprintf(stderr, "Error: --skip-switches must be non-negative.\n");
        return EXIT_FAILURE;
      }
    } else if (strcmp(argv[arg_idx], "--verbose") == 0) {
      verbose = true;
    } else if (strcmp(argv[arg_idx], "--no-switch") == 0) {
      detect_switch = false;
    } else if (strcmp(argv[arg_idx], "--no-ras") == 0) {
      detect_ras = false;
    } else if (strcmp(argv[arg_idx], "--align-tu") == 0) {
      align_tu = 0;
      // Check if the next argument is a numeric value (optional parameter)
      if (arg_idx + 1 < argc && argv[arg_idx + 1][0] != '-') {
        char *endptr = nullptr;
        long val = strtol(argv[arg_idx + 1], &endptr, 10);
        if (endptr != argv[arg_idx + 1] && *endptr == '\0' && val > 0) {
          align_tu = static_cast<int>(val);
          ++arg_idx;
        }
      }
    } else if (strcmp(argv[arg_idx], "--check-seq-params") == 0) {
      if (arg_idx + 1 >= argc) {
        fprintf(stderr, "Error: --check-seq-params requires a value.\n");
        return EXIT_FAILURE;
      }
      check_seq_params = atoi(argv[++arg_idx]) != 0;
    } else {
      fprintf(stderr, "Error: unknown option '%s'.\n", argv[arg_idx]);
      return EXIT_FAILURE;
    }
    ++arg_idx;
  }

  if (!detect_switch && !detect_ras) {
    fprintf(stderr,
            "Error: --no-switch and --no-ras cannot both be set.\n");
    return EXIT_FAILURE;
  }

  if (argc - arg_idx != 3) {
    fprintf(stderr,
            "Usage: %s [--skip-switches N] [--verbose] [--no-switch] "
            "[--no-ras] [--check-seq-params 0|1] [--align-tu [N]] "
            "<input1> <input2> <output>\n",
            argv[0]);
    return EXIT_FAILURE;
  }

  const char *input1_path = argv[arg_idx];
  const char *input2_path = argv[arg_idx + 1];
  const char *output_path = argv[arg_idx + 2];

  // Open input 1
  InputContext input1;
  AvxInputContext avx_ctx1;
  ObuDecInputContext obu_ctx1;
#if CONFIG_WEBM_IO
  WebmInputContext webm_ctx1;
  input1.webm_ctx = &webm_ctx1;
#endif
  input1.avx_ctx = &avx_ctx1;
  input1.obu_ctx = &obu_ctx1;
  input1.Init();

  FILE *fin1 = fopen(input1_path, "rb");
  if (!fin1) {
    fprintf(stderr, "Error: failed to open input file: %s\n", input1_path);
    return EXIT_FAILURE;
  }
  avx_ctx1.file = fin1;
  avx_ctx1.file_type = GetFileType(&input1);
  input1.unit_buffer =
      reinterpret_cast<uint8_t *>(calloc(kInitialBufferSize, 1));
  input1.unit_buffer_size = kInitialBufferSize;

  // Open input 2
  InputContext input2;
  AvxInputContext avx_ctx2;
  ObuDecInputContext obu_ctx2;
#if CONFIG_WEBM_IO
  WebmInputContext webm_ctx2;
  input2.webm_ctx = &webm_ctx2;
#endif
  input2.avx_ctx = &avx_ctx2;
  input2.obu_ctx = &obu_ctx2;
  input2.Init();

  FILE *fin2 = fopen(input2_path, "rb");
  if (!fin2) {
    fprintf(stderr, "Error: failed to open input file: %s\n", input2_path);
    fclose(fin1);
    return EXIT_FAILURE;
  }
  avx_ctx2.file = fin2;
  avx_ctx2.file_type = GetFileType(&input2);
  input2.unit_buffer =
      reinterpret_cast<uint8_t *>(calloc(kInitialBufferSize, 1));
  input2.unit_buffer_size = kInitialBufferSize;

  // Open output
  FILE *fout = fopen(output_path, "wb");
  if (!fout) {
    fprintf(stderr, "Error: failed to open output file: %s\n", output_path);
    fclose(fin1);
    fclose(fin2);
    return EXIT_FAILURE;
  }

  // Check sequence header compatibility
  if (check_seq_params) {
    std::vector<uint8_t> seq_hdr1, seq_hdr2;

    // Scan stream 1 for a sequence header
    while (seq_hdr1.empty()) {
      size_t unit_size = 0;
      if (!ReadTemporalUnit(&input1, &unit_size)) break;
      seq_hdr1 = extract_seq_header_obu(input1.unit_buffer,
                                         static_cast<int>(unit_size));
    }

    // Scan stream 2 for a sequence header
    while (seq_hdr2.empty()) {
      size_t unit_size = 0;
      if (!ReadTemporalUnit(&input2, &unit_size)) break;
      seq_hdr2 = extract_seq_header_obu(input2.unit_buffer,
                                         static_cast<int>(unit_size));
    }

    if (seq_hdr1.empty() || seq_hdr2.empty()) {
      fprintf(stderr,
              "Warning: could not find sequence header in %s, "
              "skipping sequence parameter check.\n",
              seq_hdr1.empty() ? "stream1" : "stream2");
    } else if (seq_hdr1 != seq_hdr2) {
      fprintf(stderr,
              "Error: sequence headers differ between stream1 (%zu bytes) "
              "and stream2 (%zu bytes). Streams are not compatible.\n",
              seq_hdr1.size(), seq_hdr2.size());

      // Attempt to parse both headers and print a field-by-field diff.
      const uint8_t *payload1 = nullptr, *payload2 = nullptr;
      size_t payload1_size = 0, payload2_size = 0;
      bool ok1 = get_obu_payload(seq_hdr1, &payload1, &payload1_size);
      bool ok2 = get_obu_payload(seq_hdr2, &payload2, &payload2_size);
      if (ok1 && ok2) {
        SequenceHeader sp1, sp2;
        int rc1 = parse_seq_header_obu_payload(payload1, payload1_size, &sp1);
        int rc2 = parse_seq_header_obu_payload(payload2, payload2_size, &sp2);
        if (rc1 == 0 && rc2 == 0) {
          fprintf(stderr, "Field-by-field diff:\n");
          print_seq_header_diff(sp1, sp2);
        } else {
          fprintf(stderr,
                  "Warning: could not parse sequence header%s for diff "
                  "(parse error).\n",
                  (rc1 != 0 && rc2 != 0)  ? "s"
                  : (rc1 != 0)             ? " from stream1"
                                           : " from stream2");
        }
      } else {
        fprintf(stderr,
                "Warning: could not extract OBU payload for diff.\n");
      }

      fclose(fin1);
      fclose(fin2);
      fclose(fout);
      return EXIT_FAILURE;
    } else {
      printf("Sequence header check: OK (%zu bytes match)\n", seq_hdr1.size());
    }

    // Rewind both inputs and re-initialize contexts
    fseek(fin1, 0, SEEK_SET);
    input1.Init();
    avx_ctx1.file = fin1;
    avx_ctx1.file_type = GetFileType(&input1);

    fseek(fin2, 0, SEEK_SET);
    input2.Init();
    avx_ctx2.file = fin2;
    avx_ctx2.file_type = GetFileType(&input2);
  }

  const int target_sframe = skip_switches + 1;
  int sframe_count = 0;
  int tu_index1 = 0;
  int tu_index2 = 0;
  int obus_from_stream1 = 0;
  int obus_from_stream2 = 0;

  if (align_tu >= 0) {
    // --align-tu mode: find the Nth S-frame in stream 2, then cut stream 1
    // at the same TU index (or a user-specified count) with no S-frame
    // detection needed on stream 1.

    // Phase 1: Scan stream 2 to find the Nth S-frame and record its TU index
    if (verbose) printf("--- Phase 1: scanning stream2 for S-frame ---\n");
    int switch_tu_index = -1;
    size_t switch_tu_size = 0;
    while (true) {
      size_t unit_size = 0;
      if (!ReadTemporalUnit(&input2, &unit_size)) {
        fprintf(stderr,
                "Error: stream2 ended before finding S-frame #%d.\n",
                target_sframe);
        fclose(fin1);
        fclose(fin2);
        fclose(fout);
        return EXIT_FAILURE;
      }

      bool is_sframe = tu_contains_sframe(
          input2.unit_buffer, static_cast<int>(unit_size), verbose,
          detect_switch, detect_ras, NULL);

      if (is_sframe) {
        ++sframe_count;
        if (sframe_count >= target_sframe) {
          switch_tu_index = tu_index2;
          switch_tu_size = unit_size;
          if (verbose) {
            printf(
                "    TU #%d (stream2): S-frame #%d found, switch_tu_index=%d\n",
                tu_index2, sframe_count, switch_tu_index);
          }
          break;
        }
      }

      if (verbose) {
        printf("    TU #%d (stream2): skipped%s\n", tu_index2,
               is_sframe ? " [S-frame]" : "");
      }
      ++tu_index2;
    }

    // Save the S-frame TU data from stream 2 (the buffer will be overwritten
    // when we read subsequent TUs from stream 2, but we need it after reading
    // stream 1).
    std::vector<uint8_t> switch_tu_data(input2.unit_buffer,
                                        input2.unit_buffer + switch_tu_size);

    // If the user specified an explicit TU count, use that instead of
    // the stream 2 S-frame TU index.
    int output_tu_count = (align_tu > 0) ? align_tu : switch_tu_index;

    // Phase 2: Output output_tu_count TUs from stream 1
    if (verbose)
      printf("--- Phase 2: outputting %d TUs from stream1 ---\n",
             output_tu_count);
    for (int i = 0; i < output_tu_count; ++i) {
      size_t unit_size = 0;
      if (!ReadTemporalUnit(&input1, &unit_size)) {
        fprintf(stderr,
                "Error: stream1 ended at TU #%d, needed %d TUs to reach "
                "switch point.\n",
                i, output_tu_count);
        fclose(fin1);
        fclose(fin2);
        fclose(fout);
        return EXIT_FAILURE;
      }

      int tu_obus = 0;
      if (verbose) {
        tu_contains_sframe(input1.unit_buffer, static_cast<int>(unit_size),
                           true, detect_switch, detect_ras, &tu_obus);
        printf("    TU #%d (stream1): output\n", tu_index1);
      } else {
        tu_contains_sframe(input1.unit_buffer, static_cast<int>(unit_size),
                           false, detect_switch, detect_ras, &tu_obus);
      }
      obus_from_stream1 += tu_obus;
      fwrite(input1.unit_buffer, 1, unit_size, fout);
      ++tu_index1;
    }

    // Phase 3: Output the S-frame TU from stream 2, then the rest of stream 2
    if (verbose) {
      printf("=== SWITCH at TU index %d, stream1 TU #%d -> stream2 TU #%d "
             "===\n",
             switch_tu_index, tu_index1, switch_tu_index);
      printf("--- Phase 3: outputting stream2 from S-frame onward ---\n");
    }

    // Output the saved S-frame TU
    tu_contains_sframe(switch_tu_data.data(),
                       static_cast<int>(switch_tu_data.size()), false,
                       detect_switch, detect_ras, &obus_from_stream2);
    fwrite(switch_tu_data.data(), 1, switch_tu_data.size(), fout);
    if (verbose) {
      printf("    TU #%d (stream2): output [S-frame, switch point]\n",
             tu_index2);
    }
    ++tu_index2;

    // Output remaining TUs from stream 2
    while (true) {
      size_t unit_size = 0;
      if (!ReadTemporalUnit(&input2, &unit_size)) break;

      if (verbose) {
        bool is_sframe = tu_contains_sframe(
            input2.unit_buffer, static_cast<int>(unit_size), true,
            detect_switch, detect_ras, &obus_from_stream2);
        printf("    TU #%d (stream2): output%s\n", tu_index2,
               is_sframe ? " [S-frame]" : "");
      } else {
        tu_contains_sframe(input2.unit_buffer, static_cast<int>(unit_size),
                           false, detect_switch, detect_ras,
                           &obus_from_stream2);
      }
      fwrite(input2.unit_buffer, 1, unit_size, fout);
      ++tu_index2;
    }
  } else {
    // Default mode: find Nth S-frame in both streams independently.

    // Phase 1: Output TUs from stream 1 until the (N+1)-th S-frame
    if (verbose) printf("--- Phase 1: reading from stream1 ---\n");
    while (true) {
      size_t unit_size = 0;
      if (!ReadTemporalUnit(&input1, &unit_size)) {
        fprintf(stderr,
                "Error: stream1 ended before finding S-frame #%d.\n",
                target_sframe);
        fclose(fin1);
        fclose(fin2);
        fclose(fout);
        return EXIT_FAILURE;
      }

      int tu_obus = 0;
      bool is_sframe = tu_contains_sframe(
          input1.unit_buffer, static_cast<int>(unit_size), verbose,
          detect_switch, detect_ras, &tu_obus);

      if (is_sframe) {
        ++sframe_count;
        if (sframe_count >= target_sframe) {
          if (verbose) {
            printf(
                "    TU #%d (stream1): S-frame #%d found, stopping stream1\n",
                tu_index1, sframe_count);
          }
          break;
        }
      }

      obus_from_stream1 += tu_obus;
      fwrite(input1.unit_buffer, 1, unit_size, fout);
      if (verbose) {
        printf("    TU #%d (stream1): output%s\n", tu_index1,
               is_sframe ? " [S-frame]" : "");
      }
      ++tu_index1;
    }

    // Phase 2: Advance stream 2 to the (N+1)-th S-frame
    if (verbose) printf("--- Phase 2: advancing stream2 ---\n");
    sframe_count = 0;
    while (true) {
      size_t unit_size = 0;
      if (!ReadTemporalUnit(&input2, &unit_size)) {
        fprintf(stderr,
                "Error: stream2 ended before finding S-frame #%d.\n",
                target_sframe);
        fclose(fin1);
        fclose(fin2);
        fclose(fout);
        return EXIT_FAILURE;
      }

      bool is_sframe = tu_contains_sframe(
          input2.unit_buffer, static_cast<int>(unit_size), verbose,
          detect_switch, detect_ras, NULL);

      if (is_sframe) {
        ++sframe_count;
        if (sframe_count >= target_sframe) {
          // Phase 2b: Output the switch TU from stream 2
          tu_contains_sframe(input2.unit_buffer, static_cast<int>(unit_size),
                             false, detect_switch, detect_ras,
                             &obus_from_stream2);
          if (verbose) {
            printf(
                "=== SWITCH at S-frame #%d, stream1 TU #%d -> stream2 TU #%d "
                "===\n",
                target_sframe, tu_index1, tu_index2);
            printf("    TU #%d (stream2): output [S-frame, switch point]\n",
                   tu_index2);
          }
          fwrite(input2.unit_buffer, 1, unit_size, fout);
          ++tu_index2;
          break;
        }
      }

      if (verbose) {
        printf("    TU #%d (stream2): discarded%s\n", tu_index2,
               is_sframe ? " [S-frame]" : "");
      }
      ++tu_index2;
    }

    // Phase 3: Output remaining TUs from stream 2
    if (verbose) printf("--- Phase 3: reading remainder of stream2 ---\n");
    while (true) {
      size_t unit_size = 0;
      if (!ReadTemporalUnit(&input2, &unit_size)) break;

      if (verbose) {
        bool is_sframe = tu_contains_sframe(
            input2.unit_buffer, static_cast<int>(unit_size), true,
            detect_switch, detect_ras, &obus_from_stream2);
        printf("    TU #%d (stream2): output%s\n", tu_index2,
               is_sframe ? " [S-frame]" : "");
      } else {
        tu_contains_sframe(input2.unit_buffer, static_cast<int>(unit_size),
                           false, detect_switch, detect_ras,
                           &obus_from_stream2);
      }
      fwrite(input2.unit_buffer, 1, unit_size, fout);
      ++tu_index2;
    }
  }

  int total_tus = tu_index1 + tu_index2;
  int total_obus = obus_from_stream1 + obus_from_stream2;
  printf("Stream switch summary:\n");
  printf("  stream1: %d TUs, %d OBUs\n", tu_index1, obus_from_stream1);
  printf("  stream2: %d TUs, %d OBUs\n", tu_index2, obus_from_stream2);
  printf("  output:  %d TUs, %d OBUs\n", total_tus, total_obus);

  fclose(fin1);
  fclose(fin2);
  fclose(fout);
  return EXIT_SUCCESS;
}
