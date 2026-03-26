/*
 * Copyright (c) 2024, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 3-Clause Clear License
 * and the Alliance for Open Media Patent License 1.0. If the BSD 3-Clause Clear
 * License was not distributed with this source code in the LICENSE file, you
 * can obtain it at aomedia.org/license/software-license/bsd-3-c-c/.  If the
 * Alliance for Open Media Patent License 1.0 was not distributed with this
 * source code in the PATENTS file, you can obtain it at
 * aomedia.org/license/patent-license/.
 */

// yuv_splitter: Split a concatenated raw YUV or Y4M file into separate output
// files, each with a potentially different resolution and frame count. Supports
// both raw YUV and Y4M output formats. Y4M input is auto-detected.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SPLITS 64
#define MAX_PATH_LEN 4096

typedef struct {
  int width;
  int height;
  int frame_count;
} SplitSpec;

typedef struct {
  const char *input_path;
  int bitdepth;
  int chroma;
  int num_splits;
  SplitSpec splits[MAX_SPLITS];
  const char *output_pattern;
  int output_y4m;
  int fps_num;
  int fps_den;
  int input_y4m;    // auto-detected from file magic
  int bitdepth_set; // 1 if user explicitly passed -b
  int chroma_set;   // 1 if user explicitly passed -c
  int fps_set;      // 1 if user explicitly passed --fps
} Config;

static void print_usage(const char *argv0) {
  fprintf(stderr,
          "Usage: %s [options] -s <W> <H> <N> -s <W> <H> <N> [-s ...] "
          "<output_pattern>\n"
          "\n"
          "Options:\n"
          "  -i, --input <path>        Input raw YUV or Y4M file (required, "
          "auto-detected)\n"
          "  -b, --bitdepth <N>        Bit depth: 8, 10, 12, 16 (default: 8)\n"
          "  -c, --chroma <fmt>        Chroma format: 420, 422, 444 "
          "(default: 420)\n"
          "  -s, --split <W> <H> <N>   Split spec: width, height, "
          "frame_count (min 2 splits)\n"
          "      --fps <num>/<den>      Frame rate for Y4M output "
          "(default: 30/1)\n"
          "  -h, --help                Show help\n"
          "\n"
          "  <output_pattern>          Last positional arg. Contains printf "
          "%%d for split\n"
          "                            index (0-based). Extension .y4m writes "
          "Y4M headers;\n"
          "                            .yuv writes raw data.\n"
          "\n"
          "Example:\n"
          "  %s -i input.yuv -b 10 -c 420 \\\n"
          "    -s 1920 1080 100 -s 1280 720 50 output_%%d.y4m\n",
          argv0, argv0);
}

static size_t frame_size_bytes(int w, int h, int bitdepth, int chroma) {
  int bps = (bitdepth > 8) ? 2 : 1;
  size_t luma = (size_t)w * h;
  size_t chroma_plane;
  if (chroma == 444)
    chroma_plane = luma;
  else if (chroma == 422)
    chroma_plane = (size_t)((w + 1) / 2) * h;
  else /* 420 */
    chroma_plane = (size_t)((w + 1) / 2) * ((h + 1) / 2);
  return (luma + 2 * chroma_plane) * bps;
}

static const char *y4m_chroma_tag(int bitdepth, int chroma) {
  if (bitdepth == 8) {
    if (chroma == 444) return "C444";
    if (chroma == 422) return "C422";
    return "C420";
  }
  // For high bit depth, append pN suffix.
  if (chroma == 444) {
    switch (bitdepth) {
      case 10: return "C444p10 XYSCSS=444P10";
      case 12: return "C444p12 XYSCSS=444P12";
      case 16: return "C444p16 XYSCSS=444P16";
    }
  } else if (chroma == 422) {
    switch (bitdepth) {
      case 10: return "C422p10 XYSCSS=422P10";
      case 12: return "C422p12 XYSCSS=422P12";
      case 16: return "C422p16 XYSCSS=422P16";
    }
  } else {
    switch (bitdepth) {
      case 10: return "C420p10 XYSCSS=420P10";
      case 12: return "C420p12 XYSCSS=420P12";
      case 16: return "C420p16 XYSCSS=420P16";
    }
  }
  return "C420";
}

// Returns 1 if the pattern contains exactly one printf-style %d specifier.
static int validate_pattern(const char *pattern) {
  int count = 0;
  for (const char *p = pattern; *p; ++p) {
    if (*p == '%') {
      ++p;
      if (*p == '%') continue;  // escaped %%
      // Skip optional flags, width, precision.
      while (*p == '-' || *p == '+' || *p == ' ' || *p == '0' || *p == '#')
        ++p;
      while (*p >= '0' && *p <= '9') ++p;
      if (*p == '.') {
        ++p;
        while (*p >= '0' && *p <= '9') ++p;
      }
      if (*p == 'd' || *p == 'i') {
        ++count;
      } else {
        return 0;  // unsupported format specifier
      }
    }
  }
  return count == 1;
}

static int ends_with(const char *str, const char *suffix) {
  size_t str_len = strlen(str);
  size_t suffix_len = strlen(suffix);
  if (suffix_len > str_len) return 0;
  return strcmp(str + str_len - suffix_len, suffix) == 0;
}

// Maps a Y4M C-tag (without the 'C' prefix) to bitdepth and chroma subsampling.
// Returns 0 on success, 1 on unknown tag.
static int parse_y4m_chroma_tag(const char *tag, int *bitdepth, int *chroma) {
  if (strcmp(tag, "420") == 0 || strcmp(tag, "420jpeg") == 0 ||
      strcmp(tag, "420mpeg2") == 0 || strcmp(tag, "420paldv") == 0) {
    *bitdepth = 8;
    *chroma = 420;
  } else if (strcmp(tag, "422") == 0) {
    *bitdepth = 8;
    *chroma = 422;
  } else if (strcmp(tag, "444") == 0) {
    *bitdepth = 8;
    *chroma = 444;
  } else if (strcmp(tag, "420p10") == 0) {
    *bitdepth = 10;
    *chroma = 420;
  } else if (strcmp(tag, "420p12") == 0) {
    *bitdepth = 12;
    *chroma = 420;
  } else if (strcmp(tag, "422p10") == 0) {
    *bitdepth = 10;
    *chroma = 422;
  } else if (strcmp(tag, "422p12") == 0) {
    *bitdepth = 12;
    *chroma = 422;
  } else if (strcmp(tag, "444p10") == 0) {
    *bitdepth = 10;
    *chroma = 444;
  } else if (strcmp(tag, "444p12") == 0) {
    *bitdepth = 12;
    *chroma = 444;
  } else {
    return 1;
  }
  return 0;
}

// Reads the Y4M file header line and sets cfg fields that the user didn't
// explicitly override. Leaves the file position at the first byte after the
// header's terminating newline.
static int parse_y4m_file_header(FILE *f, Config *cfg) {
  // Read the entire header line (up to newline).
  char header[4096];
  int len = 0;
  int c;
  while ((c = fgetc(f)) != EOF && c != '\n') {
    if (len < (int)sizeof(header) - 1) header[len++] = (char)c;
  }
  header[len] = '\0';
  if (c == EOF) {
    fprintf(stderr, "Error: Unexpected EOF in Y4M file header.\n");
    return 1;
  }

  // Verify the magic prefix.
  if (strncmp(header, "YUV4MPEG2", 9) != 0) {
    fprintf(stderr, "Error: Invalid Y4M header.\n");
    return 1;
  }

  // Parse space-separated tags after "YUV4MPEG2".
  char *p = header + 9;
  while (*p) {
    // Skip whitespace.
    while (*p == ' ') ++p;
    if (!*p) break;

    char tag_type = *p;
    const char *val = p + 1;

    // Find end of this tag (next space or end of string).
    char *end = p + 1;
    while (*end && *end != ' ') ++end;
    char saved = *end;
    *end = '\0';

    switch (tag_type) {
      case 'C': {
        if (!cfg->chroma_set || !cfg->bitdepth_set) {
          int bd, ch;
          if (parse_y4m_chroma_tag(val, &bd, &ch) == 0) {
            if (!cfg->bitdepth_set) cfg->bitdepth = bd;
            if (!cfg->chroma_set) cfg->chroma = ch;
          } else {
            fprintf(stderr, "Warning: Unknown Y4M chroma tag 'C%s', "
                            "using defaults.\n",
                    val);
          }
        }
        break;
      }
      case 'F': {
        if (!cfg->fps_set) {
          int num, den;
          if (sscanf(val, "%d:%d", &num, &den) == 2 && num > 0 && den > 0) {
            cfg->fps_num = num;
            cfg->fps_den = den;
          }
        }
        break;
      }
      case 'W':
      case 'H':
      case 'I':
      case 'A':
      case 'X':
        // Ignored.
        break;
      default: break;
    }

    *end = saved;
    p = end;
  }

  return 0;
}

// Reads and skips the Y4M "FRAME" marker line (including optional parameters)
// up to and including the terminating newline. Returns 0 on success, -1 on
// error or EOF.
static int skip_y4m_frame_header(FILE *f) {
  char marker[6];
  size_t n = fread(marker, 1, 5, f);
  if (n < 5) return -1;
  if (memcmp(marker, "FRAME", 5) != 0) {
    fprintf(stderr, "Error: Expected Y4M FRAME marker, got '%.5s'.\n", marker);
    return -1;
  }
  // Read until newline (skip optional frame parameters).
  int c;
  while ((c = fgetc(f)) != EOF && c != '\n')
    ;
  if (c == EOF) return -1;
  return 0;
}

static int parse_args(int argc, char **argv, Config *cfg) {
  cfg->input_path = NULL;
  cfg->bitdepth = 8;
  cfg->chroma = 420;
  cfg->num_splits = 0;
  cfg->output_pattern = NULL;
  cfg->output_y4m = 0;
  cfg->fps_num = 30;
  cfg->fps_den = 1;
  cfg->input_y4m = 0;
  cfg->bitdepth_set = 0;
  cfg->chroma_set = 0;
  cfg->fps_set = 0;

  int i = 1;
  while (i < argc) {
    if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      print_usage(argv[0]);
      exit(0);
    } else if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--input") == 0) {
      if (++i >= argc) {
        fprintf(stderr, "Error: -i requires a file path argument.\n");
        return 1;
      }
      cfg->input_path = argv[i];
    } else if (strcmp(argv[i], "-b") == 0 ||
               strcmp(argv[i], "--bitdepth") == 0) {
      if (++i >= argc) {
        fprintf(stderr, "Error: -b requires a bit depth argument.\n");
        return 1;
      }
      cfg->bitdepth = atoi(argv[i]);
      if (cfg->bitdepth != 8 && cfg->bitdepth != 10 && cfg->bitdepth != 12 &&
          cfg->bitdepth != 16) {
        fprintf(stderr,
                "Error: Invalid bit depth %d. Must be 8, 10, 12, or "
                "16.\n",
                cfg->bitdepth);
        return 1;
      }
      cfg->bitdepth_set = 1;
    } else if (strcmp(argv[i], "-c") == 0 ||
               strcmp(argv[i], "--chroma") == 0) {
      if (++i >= argc) {
        fprintf(stderr, "Error: -c requires a chroma format argument.\n");
        return 1;
      }
      cfg->chroma = atoi(argv[i]);
      if (cfg->chroma != 420 && cfg->chroma != 422 && cfg->chroma != 444) {
        fprintf(stderr,
                "Error: Invalid chroma format %d. Must be 420, 422, or "
                "444.\n",
                cfg->chroma);
        return 1;
      }
      cfg->chroma_set = 1;
    } else if (strcmp(argv[i], "-s") == 0 ||
               strcmp(argv[i], "--split") == 0) {
      if (i + 3 >= argc) {
        fprintf(stderr, "Error: -s requires three arguments: W H N.\n");
        return 1;
      }
      if (cfg->num_splits >= MAX_SPLITS) {
        fprintf(stderr, "Error: Too many splits (max %d).\n", MAX_SPLITS);
        return 1;
      }
      SplitSpec *s = &cfg->splits[cfg->num_splits];
      s->width = atoi(argv[++i]);
      s->height = atoi(argv[++i]);
      s->frame_count = atoi(argv[++i]);
      if (s->width <= 0 || s->height <= 0 || s->frame_count <= 0) {
        fprintf(stderr,
                "Error: Split spec values must be positive integers "
                "(got %d %d %d).\n",
                s->width, s->height, s->frame_count);
        return 1;
      }
      cfg->num_splits++;
    } else if (strcmp(argv[i], "--fps") == 0) {
      if (++i >= argc) {
        fprintf(stderr, "Error: --fps requires a num/den argument.\n");
        return 1;
      }
      if (sscanf(argv[i], "%d/%d", &cfg->fps_num, &cfg->fps_den) != 2 ||
          cfg->fps_num <= 0 || cfg->fps_den <= 0) {
        fprintf(stderr, "Error: Invalid fps format '%s'. Expected num/den.\n",
                argv[i]);
        return 1;
      }
      cfg->fps_set = 1;
    } else if (argv[i][0] == '-') {
      fprintf(stderr, "Error: Unknown option '%s'.\n", argv[i]);
      return 1;
    } else {
      // Positional argument: output pattern (must be last).
      cfg->output_pattern = argv[i];
    }
    ++i;
  }

  if (!cfg->input_path) {
    fprintf(stderr, "Error: Input file is required (-i).\n");
    return 1;
  }
  if (!cfg->output_pattern) {
    fprintf(stderr, "Error: Output pattern is required.\n");
    return 1;
  }
  if (cfg->num_splits < 2) {
    fprintf(stderr, "Error: At least 2 split specs are required.\n");
    return 1;
  }
  if (!validate_pattern(cfg->output_pattern)) {
    fprintf(stderr,
            "Error: Output pattern must contain exactly one %%d specifier.\n");
    return 1;
  }

  cfg->output_y4m = ends_with(cfg->output_pattern, ".y4m");
  return 0;
}

int main(int argc, char **argv) {
  Config cfg;
  if (argc < 2) {
    print_usage(argv[0]);
    return 1;
  }
  if (parse_args(argc, argv, &cfg)) return 1;

  FILE *fin = fopen(cfg.input_path, "rb");
  if (!fin) {
    fprintf(stderr, "Error: Cannot open input file '%s'.\n", cfg.input_path);
    return 1;
  }

  // Auto-detect Y4M input by checking file magic.
  {
    char magic[9];
    size_t n = fread(magic, 1, 9, fin);
    if (n >= 9 && memcmp(magic, "YUV4MPEG2", 9) == 0) {
      cfg.input_y4m = 1;
      fseek(fin, 0, SEEK_SET);
      if (parse_y4m_file_header(fin, &cfg)) {
        fclose(fin);
        return 1;
      }
      fprintf(stdout, "Detected Y4M input: %d-bit %d\n", cfg.bitdepth,
              cfg.chroma);
    } else {
      fseek(fin, 0, SEEK_SET);
    }
  }

  // Find max frame size for buffer allocation.
  size_t max_frame = 0;
  for (int i = 0; i < cfg.num_splits; ++i) {
    size_t fs = frame_size_bytes(cfg.splits[i].width, cfg.splits[i].height,
                                 cfg.bitdepth, cfg.chroma);
    if (fs > max_frame) max_frame = fs;
  }

  unsigned char *buf = (unsigned char *)malloc(max_frame);
  if (!buf) {
    fprintf(stderr, "Error: Failed to allocate %zu bytes.\n", max_frame);
    fclose(fin);
    return 1;
  }

  int ret = 0;
  for (int i = 0; i < cfg.num_splits; ++i) {
    const SplitSpec *s = &cfg.splits[i];
    size_t fs =
        frame_size_bytes(s->width, s->height, cfg.bitdepth, cfg.chroma);

    char outname[MAX_PATH_LEN];
    snprintf(outname, sizeof(outname), cfg.output_pattern, i);

    FILE *fout = fopen(outname, "wb");
    if (!fout) {
      fprintf(stderr, "Error: Cannot open output file '%s'.\n", outname);
      ret = 1;
      break;
    }

    // Write Y4M file header if applicable.
    if (cfg.output_y4m) {
      const char *ctag = y4m_chroma_tag(cfg.bitdepth, cfg.chroma);
      fprintf(fout, "YUV4MPEG2 W%d H%d F%d:%d Ip %s\n", s->width, s->height,
              cfg.fps_num, cfg.fps_den, ctag);
    }

    for (int f = 0; f < s->frame_count; ++f) {
      if (cfg.input_y4m) {
        if (skip_y4m_frame_header(fin) < 0) {
          fprintf(stderr,
                  "Error: Failed to read Y4M FRAME marker at split %d, "
                  "frame %d.\n",
                  i, f);
          fclose(fout);
          ret = 1;
          goto cleanup;
        }
      }
      size_t nread = fread(buf, 1, fs, fin);
      if (nread != fs) {
        fprintf(stderr,
                "Error: Short read at split %d, frame %d "
                "(expected %zu bytes, got %zu).\n",
                i, f, fs, nread);
        fclose(fout);
        ret = 1;
        goto cleanup;
      }
      if (cfg.output_y4m) {
        if (fwrite("FRAME\n", 1, 6, fout) != 6) {
          fprintf(stderr, "Error: Failed to write Y4M frame header.\n");
          fclose(fout);
          ret = 1;
          goto cleanup;
        }
      }
      if (fwrite(buf, 1, fs, fout) != fs) {
        fprintf(stderr, "Error: Failed to write frame data to '%s'.\n",
                outname);
        fclose(fout);
        ret = 1;
        goto cleanup;
      }
    }

    fclose(fout);
    fprintf(stdout, "Split %d: %dx%d, %d frames -> %s\n", i, s->width,
            s->height, s->frame_count, outname);
  }

  // Check for leftover bytes in input.
  if (ret == 0) {
    long pos = ftell(fin);
    fseek(fin, 0, SEEK_END);
    long end = ftell(fin);
    if (end > pos) {
      fprintf(stderr, "Warning: %ld leftover bytes in input file.\n",
              end - pos);
    }
  }

cleanup:
  free(buf);
  fclose(fin);
  return ret;
}
