#include "settings_internal.h"

#include <glib/gstdio.h>
#include <sys/stat.h>

#define WHISPER_MODEL_URL_BASE \
    "https://huggingface.co/ggerganov/whisper.cpp/resolve/main"

static const ModelInfo whisper_models[] = {
    { "tiny",   "Tiny",   "75 MB",   75 * 1024 * 1024LL,
      WHISPER_MODEL_URL_BASE "/ggml-tiny.bin", NULL, NULL },
    { "base",   "Base",   "142 MB",  142 * 1024 * 1024LL,
      WHISPER_MODEL_URL_BASE "/ggml-base.bin", NULL, NULL },
    { "small",  "Small",  "466 MB",  466 * 1024 * 1024LL,
      WHISPER_MODEL_URL_BASE "/ggml-small.bin", NULL, NULL },
    { "medium", "Medium", "1.5 GB",  1536 * 1024 * 1024LL,
      WHISPER_MODEL_URL_BASE "/ggml-medium.bin", NULL, NULL },
    { "large",  "Large",  "2.9 GB",  2952 * 1024 * 1024LL,
      WHISPER_MODEL_URL_BASE "/ggml-large.bin", NULL, NULL },
};

#define SHERPA_MODEL_URL_BASE \
    "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models"

static const ModelInfo sherpa_models[] = {
    { "sense-voice",
      "SenseVoice (zh/en/ja/ko/yue)", "~230 MB",
      1048 * 1024 * 1024LL,
      SHERPA_MODEL_URL_BASE
          "/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17.tar.bz2",
      "sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17.tar.bz2",
      "sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17" },
    { "whisper-tiny",
      "Whisper Tiny (multilingual)", "~110 MB",
      116 * 1024 * 1024LL,
      SHERPA_MODEL_URL_BASE "/sherpa-onnx-whisper-tiny.tar.bz2",
      "sherpa-onnx-whisper-tiny.tar.bz2",
      "sherpa-onnx-whisper-tiny" },
    { "whisper-base",
      "Whisper Base (multilingual)", "~197 MB",
      208 * 1024 * 1024LL,
      SHERPA_MODEL_URL_BASE "/sherpa-onnx-whisper-base.tar.bz2",
      "sherpa-onnx-whisper-base.tar.bz2",
      "sherpa-onnx-whisper-base" },
    { "whisper-small",
      "Whisper Small (multilingual)", "~609 MB",
      639 * 1024 * 1024LL,
      SHERPA_MODEL_URL_BASE "/sherpa-onnx-whisper-small.tar.bz2",
      "sherpa-onnx-whisper-small.tar.bz2",
      "sherpa-onnx-whisper-small" },
};

const char *settings_voice_backend_id(TypioSettings *settings, guint idx) {
    if (!settings || !settings->voice_backend_ids || idx >= settings->voice_backend_ids->len) {
        return NULL;
    }
    return g_ptr_array_index(settings->voice_backend_ids, idx);
}

guint settings_voice_backend_index(TypioSettings *settings, const char *id) {
    if (!settings || !settings->voice_backend_ids || !id) {
        return (guint)-1;
    }

    for (guint i = 0; i < settings->voice_backend_ids->len; i++) {
        const char *value = g_ptr_array_index(settings->voice_backend_ids, i);
        if (value && g_strcmp0(value, id) == 0) {
            return i;
        }
    }

    return (guint)-1;
}

gboolean is_voice_backend_name(const char *name) {
    return name &&
           (g_strcmp0(name, "whisper") == 0 ||
            g_strcmp0(name, "sherpa-onnx") == 0);
}

void voice_update_model_sections(TypioSettings *settings) {
    if (!settings) {
        return;
    }
    /* In the flux-ui port, section visibility is handled during build. */
}

void settings_refresh_voice_models(TypioSettings *settings) {
    if (!settings || !settings->voice_model_names) {
        return;
    }

    g_ptr_array_set_size(settings->voice_model_names, 0);

    const char *backend_name = settings_voice_backend_id(settings, settings->voice_backend_selected);
    const char *scan_dir;

    if (!backend_name) {
        return;
    }

    scan_dir = g_strcmp0(backend_name, "sherpa-onnx") == 0
        ? settings->sherpa_dir
        : settings->whisper_dir;

    if (!scan_dir) {
        return;
    }

    GDir *dir = g_dir_open(scan_dir, 0, nullptr);
    if (!dir) {
        return;
    }

    const char *entry;
    while ((entry = g_dir_read_name(dir)) != NULL) {
        if (g_strcmp0(backend_name, "sherpa-onnx") == 0) {
            char *full = g_build_filename(scan_dir, entry, nullptr);
            if (g_file_test(full, G_FILE_TEST_IS_DIR)) {
                g_ptr_array_add(settings->voice_model_names, g_strdup(entry));
            }
            g_free(full);
            continue;
        }

        if (g_str_has_prefix(entry, "ggml-") && g_str_has_suffix(entry, ".bin")) {
            size_t prefix_len = 5;
            size_t suffix_len = 4;
            size_t name_len = strlen(entry) - prefix_len - suffix_len;
            char *name = g_strndup(entry + prefix_len, name_len);
            g_ptr_array_add(settings->voice_model_names, name);
        }
    }

    g_dir_close(dir);
}

static gboolean model_installed(const ModelRow *row) {
    return g_file_test(row->installed_path,
                       G_FILE_TEST_EXISTS | G_FILE_TEST_IS_DIR) ||
           g_file_test(row->installed_path, G_FILE_TEST_EXISTS);
}

static void model_update_row_state(ModelRow *row) {
    g_free(row->status_text);
    row->status_text = NULL;
    g_free(row->progress_text);
    row->progress_text = NULL;
    row->download_progress = 0.0f;

    if (row->download_proc) {
        row->downloading = true;
        row->status_text = g_strdup("Downloading...");
        return;
    }

    row->downloading = false;

    if (model_installed(row)) {
        row->status_text = g_strdup("Installed");
    } else {
        row->status_text = g_strdup(row->info->size_label);
    }

    if (row->settings) {
        settings_refresh_voice_models_from_stage(row->settings);
    }
}

static gboolean model_progress_tick(gpointer user_data) {
    ModelRow *row = user_data;
    struct stat st;

    if (!row->download_proc || !row->tmp_path) {
        return G_SOURCE_REMOVE;
    }

    if (stat(row->tmp_path, &st) == 0 && row->info->expected_size > 0) {
        double fraction = (double)st.st_size / (double)row->info->expected_size;
        if (fraction > 1.0) {
            fraction = 1.0;
        }
        row->download_progress = (float)fraction;
        g_free(row->progress_text);
        row->progress_text = g_strdup_printf("%.0f%%", fraction * 100.0);
    }

    return G_SOURCE_CONTINUE;
}

static void model_simple_download_finished(GObject *source,
                                           GAsyncResult *result,
                                           gpointer user_data) {
    ModelRow *row = user_data;
    GError *error = nullptr;
    gboolean ok = g_subprocess_wait_check_finish(G_SUBPROCESS(source), result, &error);

    if (row->progress_timer) {
        g_source_remove(row->progress_timer);
        row->progress_timer = 0;
    }

    g_clear_object(&row->download_proc);

    if (ok && row->tmp_path && row->installed_path) {
        g_rename(row->tmp_path, row->installed_path);
    } else if (row->tmp_path) {
        g_unlink(row->tmp_path);
    }

    g_clear_error(&error);
    g_free(row->tmp_path);
    row->tmp_path = nullptr;
    model_update_row_state(row);
}

static void model_archive_download_finished(GObject *source,
                                            GAsyncResult *result,
                                            gpointer user_data) {
    ModelRow *row = user_data;
    GError *error = nullptr;
    gboolean ok = g_subprocess_wait_check_finish(G_SUBPROCESS(source), result, &error);

    if (row->progress_timer) {
        g_source_remove(row->progress_timer);
        row->progress_timer = 0;
    }

    g_clear_object(&row->download_proc);
    g_clear_error(&error);

    if (ok && row->tmp_path && row->base_dir) {
        GError *ext_error = nullptr;
        GSubprocess *extract = g_subprocess_new(
            G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
            &ext_error,
            "tar", "xjf", row->tmp_path, "-C", row->base_dir, nullptr);

        if (extract) {
            g_subprocess_wait_check(extract, nullptr, &ext_error);
            g_object_unref(extract);
        }
        if (ext_error) {
            g_free(row->status_text);
            row->status_text = g_strdup("Extract failed");
            g_clear_error(&ext_error);
        }
        g_unlink(row->tmp_path);
    } else if (row->tmp_path) {
        g_unlink(row->tmp_path);
    }

    g_free(row->tmp_path);
    row->tmp_path = nullptr;
    model_update_row_state(row);
}

static void model_start_download(ModelRow *row) {
    GError *error = nullptr;
    gboolean is_archive;

    if (row->download_proc) {
        return;
    }

    g_mkdir_with_parents(row->base_dir, 0755);
    is_archive = row->info->filename != NULL;

    if (is_archive) {
        row->tmp_path = g_build_filename(row->base_dir, row->info->filename, nullptr);
    } else {
        char *dir = g_path_get_dirname(row->installed_path);
        char *basename = g_path_get_basename(row->installed_path);
        char *part_name = g_strdup_printf(".%s.part", basename);
        row->tmp_path = g_build_filename(dir, part_name, nullptr);
        g_free(dir);
        g_free(basename);
        g_free(part_name);
    }

    row->download_proc = g_subprocess_new(
        G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
        &error,
        "curl", "-fSL", "--connect-timeout", "10",
        "-o", row->tmp_path, row->info->url, nullptr);

    if (!row->download_proc) {
        g_free(row->status_text);
        row->status_text = g_strdup(error ? error->message : "Failed to start curl");
        g_clear_error(&error);
        g_free(row->tmp_path);
        row->tmp_path = nullptr;
        return;
    }

    model_update_row_state(row);
    row->progress_timer = g_timeout_add(500, model_progress_tick, row);

    g_subprocess_wait_check_async(row->download_proc, nullptr,
                                  is_archive ? model_archive_download_finished
                                             : model_simple_download_finished,
                                  row);
}

static void model_cancel_download(ModelRow *row) {
    if (row->download_proc) {
        g_subprocess_force_exit(row->download_proc);
    }
}

static void remove_directory_recursive(const char *path) {
    GDir *dir = g_dir_open(path, 0, nullptr);
    if (!dir) {
        g_unlink(path);
        return;
    }

    const char *entry;
    while ((entry = g_dir_read_name(dir)) != NULL) {
        char *child = g_build_filename(path, entry, nullptr);
        if (g_file_test(child, G_FILE_TEST_IS_DIR)) {
            remove_directory_recursive(child);
        } else {
            g_unlink(child);
        }
        g_free(child);
    }

    g_dir_close(dir);
    g_rmdir(path);
}

static void on_model_action_clicked(ModelRow *row) {
    if (row->download_proc) {
        model_cancel_download(row);
        return;
    }

    if (model_installed(row)) {
        if (g_file_test(row->installed_path, G_FILE_TEST_IS_DIR)) {
            remove_directory_recursive(row->installed_path);
        } else {
            g_unlink(row->installed_path);
        }
        model_update_row_state(row);
        return;
    }

    model_start_download(row);
}

static void model_row_cleanup(ModelRow *row) {
    if (row->download_proc) {
        g_subprocess_force_exit(row->download_proc);
        g_clear_object(&row->download_proc);
    }
    if (row->progress_timer) {
        g_source_remove(row->progress_timer);
        row->progress_timer = 0;
    }
    if (row->tmp_path) {
        g_unlink(row->tmp_path);
        g_free(row->tmp_path);
    }
    g_free(row->installed_path);
    g_free(row->status_text);
    g_free(row->progress_text);
}

void settings_build_whisper_model_section(fx_ui *ui, TypioSettings *settings) {
    if (!ui || !settings) {
        return;
    }

    fx_label(ui, "Whisper models");
    fx_label_ex(ui, "Local `whisper.cpp` models available for download and removal.", 12.0f);

    for (size_t i = 0; i < WHISPER_MODEL_COUNT; i++) {
        ModelRow *row = &settings->whisper_rows[i];

        if (!row->info) {
            row->settings = settings;
            row->info = &whisper_models[i];
            row->base_dir = g_strdup(settings->whisper_dir);
            char *filename = g_strdup_printf("ggml-%s.bin", row->info->name);
            row->installed_path = g_build_filename(settings->whisper_dir, filename, nullptr);
            g_free(filename);
            model_update_row_state(row);
        }

        fx_push_id(ui, row->info->name);
        fx_row_ex(ui, (fx_layout_opts){ .gap = 12.0f, .cross = FX_CENTER });
        fx_flex(ui, 1.0f);
        fx_column(ui);
        fx_label(ui, row->info->display_name);
        fx_label_ex(ui, row->info->size_label, 12.0f);
        fx_end(ui);

        if (row->downloading) {
            fx_label(ui, row->status_text ? row->status_text : "Downloading...");
            fx_progress(ui, row->progress_text ? row->progress_text : "", row->download_progress);
            if (fx_button(ui, "Cancel")) {
                on_model_action_clicked(row);
            }
        } else if (model_installed(row)) {
            fx_label(ui, row->status_text ? row->status_text : "Installed");
            if (fx_button(ui, "Delete")) {
                on_model_action_clicked(row);
            }
        } else {
            fx_label(ui, row->status_text ? row->status_text : row->info->size_label);
            if (fx_button(ui, "Download")) {
                on_model_action_clicked(row);
            }
        }

        fx_end(ui);
        fx_separator(ui);
        fx_pop_id(ui);
    }
}

void settings_build_sherpa_model_section(fx_ui *ui, TypioSettings *settings) {
    if (!ui || !settings) {
        return;
    }

    fx_label(ui, "Sherpa-ONNX models");
    fx_label_ex(ui, "Packaged speech models extracted into the local Typio data directory.", 12.0f);

    for (size_t i = 0; i < SHERPA_MODEL_COUNT; i++) {
        ModelRow *row = &settings->sherpa_rows[i];

        if (!row->info) {
            row->settings = settings;
            row->info = &sherpa_models[i];
            row->base_dir = g_strdup(settings->sherpa_dir);
            row->installed_path = g_build_filename(settings->sherpa_dir,
                                                   row->info->extract_dir,
                                                   nullptr);
            model_update_row_state(row);
        }

        fx_push_id(ui, row->info->name);
        fx_row_ex(ui, (fx_layout_opts){ .gap = 12.0f, .cross = FX_CENTER });
        fx_flex(ui, 1.0f);
        fx_column(ui);
        fx_label(ui, row->info->display_name);
        fx_label_ex(ui, row->info->size_label, 12.0f);
        fx_end(ui);

        if (row->downloading) {
            fx_label(ui, row->status_text ? row->status_text : "Downloading...");
            fx_progress(ui, row->progress_text ? row->progress_text : "", row->download_progress);
            if (fx_button(ui, "Cancel")) {
                on_model_action_clicked(row);
            }
        } else if (model_installed(row)) {
            fx_label(ui, row->status_text ? row->status_text : "Installed");
            if (fx_button(ui, "Delete")) {
                on_model_action_clicked(row);
            }
        } else {
            fx_label(ui, row->status_text ? row->status_text : row->info->size_label);
            if (fx_button(ui, "Download")) {
                on_model_action_clicked(row);
            }
        }

        fx_end(ui);
        fx_separator(ui);
        fx_pop_id(ui);
    }
}

void settings_models_cleanup(TypioSettings *settings) {
    for (size_t i = 0; i < WHISPER_MODEL_COUNT; i++) {
        model_row_cleanup(&settings->whisper_rows[i]);
        g_free(settings->whisper_rows[i].base_dir);
    }
    g_free(settings->whisper_dir);

    for (size_t i = 0; i < SHERPA_MODEL_COUNT; i++) {
        model_row_cleanup(&settings->sherpa_rows[i]);
        g_free(settings->sherpa_rows[i].base_dir);
    }
    g_free(settings->sherpa_dir);
}
