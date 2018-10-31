#include <json-glib/json-glib.h>
#include "file-utils.h"
#include "glib-utils.h"
#include "fr-command-zip.h"
#include "fr-command-unarchiver.h"

/*
********* encoding判断逻辑(伪代码) *************
let encoding = match lsarConfidence {
  0 => “utf-8”,
  0.5 ... 1 if uriencoded_chars_count < 12 => lsarEncoding(), // 使用 lsar 的检测结果
  _ => unzip_detect_logic() // 使用 unzip 的检测逻辑
}

*********** 关于 lsar 可能认定编码出现偏差 ***********
lsar 可能会错误的认定某个文件的编码为 utf-8，但在这时，`lsar -j` 给出的文件名会使用 urlencode 编码无法正确显示/解析的文件名. 此处通过统计被
urlencode 过的字符的数量并当发现出现 **可能** 包含两个汉字的情况时，返回使用 unzip 自身的检测逻辑而不是信任 lsar 的结果. 这种策略依然可能推断
失败，但 unzip 本身也会试图解决编码问题故应该不会有太大问题。

*********** 已知逻辑 *****************
3. lsar 的编码检测认定为 utf-8 时，如果文件名出现明显被 uriencode 过的情况，那么此时很可能 lsar 的编码检测是错误的，此时应当转而使用 unzip
2. lsar的encoding分析存在不准确的情况 (360生成测试文件, gb18030编码, 短文件名)
1. unzip对中文编码检测支持较好, 但若编码为UTF8(lsar 100%确定)则unzip会失败 (MacOS下生成测试文件)
 */

typedef void (*DetectFunc)(JsonObject* root, void*);

/**
 * count_uriencoded_chars:
 * @str: 指向需要被统计被 uriencoded 过的字符数量的字符串指针
 *
 * 统计某个字符串中包含的被 urlencoded 过的可能是中文的字符数量。
 *
 * 仅当有两个相邻的字符被 uriencoded 才会被认定可能是中文（如 "%12%34" 会被认定为可能是中文而 "%12_%34" 不会）。统计会返回计数的字符数量，结果
 * 总是 3 的倍数（因为类如 "%12" 是三个字符构成的）。
 *
 * 该函数不会接管 (const gchar*)str 的所有权。
 */
static
int count_uriencoded_chars(const gchar* str)
{
  int prevIsPercent = 0, uriEncodedCharCnt = 0;
  int stringLen = strlen(str);
  for(int i = 0; i < stringLen; i++) {
    char chr = str[i];
    if (chr == '%') {
      if (prevIsPercent != 0) {
        uriEncodedCharCnt += 3;
      }
      prevIsPercent++;
      i += 2;
    } else {
      if (prevIsPercent) {
        i -= 2;
      }
      prevIsPercent = 0;
    }
  }
  // g_printf("xxxxxxxxxxxxxxxxx %d / %d\n", uriEncodedCharCnt, stringLen);
  return uriEncodedCharCnt;
}

/**
 * negative_inference_detected:
 * @root: 用于分析文件名的 Json Object 指针
 *
 * 统计 lsar -j 得到的 Json 中解析出的文件名，并返回对 lsar 编码是否存在问题的猜测。
 *
 * 当认定 lsar 给出的编码可能存在问题时返回 1 （表示否定 lsar 的猜测），否则返回 0。
 *
 * 该函数不会接管 (JsonObject*)root 的所有权。
 */
static
int negative_inference_detected(JsonObject* root)
{
  JsonArray * arr = json_object_get_array_member(root, "lsarContents");

  if (arr) {
    int elemCount = json_array_get_length(arr);
    for(int i = 0; i < elemCount; i++) {
      JsonObject * aNode = json_array_get_object_element(arr, 0);
      const gchar * aUrlEncodedFileName = json_object_get_string_member(aNode, "XADFileName");
      int charCount = count_uriencoded_chars(aUrlEncodedFileName);
      if (charCount >= 12) {
        return 1;
      }
    }
  }

  return 0;
}

static
void detect_by_lsar(const char* file, DetectFunc fn , void* data)
{
    if (!_g_program_is_in_path("lsar")) {
       return;
    }

    gchar* buf = 0;
    GError *err = 0;
    char* quoted_file = g_shell_quote(file);
    char* cmd  = g_strdup_printf("lsar -j %s", quoted_file);
    g_free(quoted_file);
    g_spawn_command_line_sync (cmd,	&buf, 0, 0, &err);
    g_free(cmd);

    if (err != 0) {
        g_warning("guess_encoding_by_lsar failed: %s\n", err->message);
        g_error_free(err);
        return;
    }

    JsonParser* parser = json_parser_new ();
    if (json_parser_load_from_data(parser, buf, -1, 0)) {
      JsonObject *root = json_node_get_object (json_parser_get_root (parser));
      fn(root, data);
    }
    g_free(buf);
}

static
void detect_encoding(JsonObject* root, void* data)
{
  double c = json_object_get_double_member (root, "lsarConfidence");
  char** v = data;

  if (c == 0) {
    *v = g_strdup("utf-8");
  } else if (c < 0.5) {
    // Do nothing
  } else if (c >= 0.5) {
    if (!negative_inference_detected(root)) {
      *v = g_strdup(json_object_get_string_member (root, "lsarEncoding"));
    }
    // else do nothing
  }
}

char* guess_encoding_by_lsar(const char* file)
{
    char* encoding = 0;
    detect_by_lsar(file, detect_encoding, &encoding);
    return encoding;
}

static
void should_use_unzip(JsonObject* root, void* ret)
{
  gboolean *v = ret;
  const char* fmt = json_object_get_string_member (root, "lsarFormatName");
  if (0 != g_strcmp0(fmt, "Zip")) {
    *v = FALSE;
    return;
  }

  JsonArray* contents = json_object_get_array_member(root, "lsarContents");
  for (int i=0; i<json_array_get_length(contents); i++) {
    JsonNode* node = json_array_get_element(contents, i);
    JsonObject* obj = json_node_get_object(node);
    gint64 m = json_object_get_int_member(obj, "ZipCompressionMethod");
    if (m != 0 && m != 8) {
      *v = FALSE;
      g_warning("/usr/bin/unzip can't handle compression method of %ld\n", m);
      return;
    }
  }
  *v = TRUE;
}

static
void should_use_unar_for_tar(JsonObject* root, void *ret)
{
  gboolean *v = ret;
  const char* encoding = json_object_get_string_member (root, "lsarEncoding");
  if (0 == g_strcmp0(encoding, "gb18030") ||
      0 != g_strstr_len(encoding, 100, "2312")) {
    const char* fmt = json_object_get_string_member (root, "lsarFormatName");
    if (0 == g_strcmp0(fmt, "Tar")) {
      *v = TRUE;
      return;
    }
  }
  *v = FALSE;
}

GType guess_archive_type_by_lsar(GType t, GFile *file)
{
  GType ret = t;

  char* filename = g_file_get_path(file);

  const char* ext = _g_filename_get_extension (filename);

  if (g_strcasecmp(ext, ".zip") == 0 || g_strcasecmp(ext, ".zipx") == 0) {
    gboolean v = FALSE;
    detect_by_lsar(filename, should_use_unzip, &v);
    if (v)  {
      ret =  FR_TYPE_COMMAND_ZIP;
    }
  } else if (g_strcasecmp(ext, ".tar") == 0) {
    gboolean v = FALSE;
    detect_by_lsar(filename, should_use_unar_for_tar, &v);
    if (v)  {
      ret =  FR_TYPE_COMMAND_UNARCHIVER;
    }
  }

  g_free(filename);
  return ret;
}
