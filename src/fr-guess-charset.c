#include <json-glib/json-glib.h>
#include "file-utils.h"
#include "glib-utils.h"
#include "fr-command-zip.h"
#include "fr-command-unarchiver.h"

/*
********* encoding判断逻辑 *************

case lsarConfidence值 of
  0 -> return utf-8
  (0, 0.5) -> return 0 (直接使用unzip自身的检测逻辑)
  [0.5, 1] -> return lsarEncoding
*/

/*
*********** 已知逻辑 *****************
2. lsar的encoding分析存在不准确的情况 (360生成测试文件, gb18030编码, 短文件名)
1. unzip对中文编码检测支持较好, 但若编码为UTF8(lsar 100%确定)则unzip会失败 (MacOS下生成测试文件)
 */

typedef void (*DetectFunc)(JsonObject* root, void*);

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
  JsonArray * arr = json_object_get_array_member(root, "lsarContents");
  int stringLen = 0;
  int prevIsPercent = 0, uriEncodedCharCnt = 0;

  // blumia: lsar 可能会错误的认定某个文件的编码为 utf-8，但在这时，`lsar -j` 给出的文件名会使用 urlencode 编码无法正确显示/解析
  //         的文件名. 此处通过统计被 urlencode 过的字符的数量（相当于 uriEncodedCharCnt / 3） 并当发现出现可能包含两个汉字的情况
  //         时，返回使用 unzip 自身的检测逻辑而不是信任 lsar 的结果. 这种策略依然可能推断失败，但 unzip 本身也会试图解决编码问题故
  //         应该不会有太大问题。
  // blumia: 注意，为了节省开销，这里只读取了一个文件名（第一个文件的文件名）以进行分析。
  if (arr) {
    JsonObject * aNode = json_array_get_object_element(arr, 0);
    const gchar * aUrlEncodedFileName = json_object_get_string_member(aNode, "XADFileName");
    stringLen = strlen(aUrlEncodedFileName);
    for(int i = 0; i < stringLen; i++) {
      char chr = aUrlEncodedFileName[i];
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
  }

  if (c == 0) {
    *v = g_strdup("utf-8");
  } else if (c < 0.5) {
    // Do nothing
  } else if (c >= 0.5) {
    if (uriEncodedCharCnt < 12) { // 一个字符会被 urlencode 成三个字符（%ab），一个汉字在国标码为两个字符构成
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
