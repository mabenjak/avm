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

#include "tools/stream_mux.h"

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
            "[--no-ras] <input1> <input2> <output>\n",
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

  const int target_sframe = skip_switches + 1;
  int sframe_count = 0;
  int tu_index1 = 0;
  int tu_index2 = 0;
  int obus_from_stream1 = 0;
  int obus_from_stream2 = 0;

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
          printf("    TU #%d (stream1): S-frame #%d found, stopping stream1\n",
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
                           false, detect_switch, detect_ras, &obus_from_stream2);
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
                         false, detect_switch, detect_ras, &obus_from_stream2);
    }
    fwrite(input2.unit_buffer, 1, unit_size, fout);
    ++tu_index2;
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
