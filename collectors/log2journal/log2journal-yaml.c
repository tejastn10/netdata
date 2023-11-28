// SPDX-License-Identifier: GPL-3.0-or-later

#include "log2journal.h"

// ----------------------------------------------------------------------------
// yaml configuration file

#ifdef HAVE_LIBYAML

static const char *yaml_event_name(yaml_event_type_t type) {
    switch (type) {
        case YAML_NO_EVENT:
            return "YAML_NO_EVENT";

        case YAML_SCALAR_EVENT:
            return "YAML_SCALAR_EVENT";

        case YAML_ALIAS_EVENT:
            return "YAML_ALIAS_EVENT";

        case YAML_MAPPING_START_EVENT:
            return "YAML_MAPPING_START_EVENT";

        case YAML_MAPPING_END_EVENT:
            return "YAML_MAPPING_END_EVENT";

        case YAML_SEQUENCE_START_EVENT:
            return "YAML_SEQUENCE_START_EVENT";

        case YAML_SEQUENCE_END_EVENT:
            return "YAML_SEQUENCE_END_EVENT";

        case YAML_STREAM_START_EVENT:
            return "YAML_STREAM_START_EVENT";

        case YAML_STREAM_END_EVENT:
            return "YAML_STREAM_END_EVENT";

        case YAML_DOCUMENT_START_EVENT:
            return "YAML_DOCUMENT_START_EVENT";

        case YAML_DOCUMENT_END_EVENT:
            return "YAML_DOCUMENT_END_EVENT";

        default:
            return "UNKNOWN";
    }
}

#define yaml_error(parser, event, fmt, args...) yaml_error_with_trace(parser, event, __LINE__, __FUNCTION__, __FILE__, fmt, ##args)
static void yaml_error_with_trace(yaml_parser_t *parser, yaml_event_t *event, size_t line, const char *function, const char *file, const char *format, ...) __attribute__ ((format(__printf__, 6, 7)));
static void yaml_error_with_trace(yaml_parser_t *parser, yaml_event_t *event, size_t line, const char *function, const char *file, const char *format, ...) {
    char buf[1024] = ""; // Initialize buf to an empty string
    const char *type = "";

    if(event) {
        type = yaml_event_name(event->type);

        switch (event->type) {
            case YAML_SCALAR_EVENT:
                copy_to_buffer(buf, sizeof(buf), (char *)event->data.scalar.value, event->data.scalar.length);
                break;

            case YAML_ALIAS_EVENT:
                snprintf(buf, sizeof(buf), "%s", event->data.alias.anchor);
                break;

            default:
                break;
        }
    }

    fprintf(stderr, "YAML %zu@%s, %s(): (line %d, column %d, %s%s%s): ",
            line, file, function,
            (int)(parser->mark.line + 1), (int)(parser->mark.column + 1),
            type, buf[0]? ", near ": "", buf);

    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, "\n");
}

#define yaml_parse(parser, event) yaml_parse_with_trace(parser, event, __LINE__, __FUNCTION__, __FILE__)
static bool yaml_parse_with_trace(yaml_parser_t *parser, yaml_event_t *event, size_t line, const char *function, const char *file) {
    if (!yaml_parser_parse(parser, event)) {
        yaml_error(parser, NULL, "YAML parser error %d", parser->error);
        return false;
    }

//    fprintf(stderr, ">>> %s >>> %.*s\n",
//            yaml_event_name(event->type),
//            event->type == YAML_SCALAR_EVENT ? event->data.scalar.length : 0,
//            event->type == YAML_SCALAR_EVENT ? (char *)event->data.scalar.value : "");

    return true;
}

#define yaml_parse_expect_event(parser, type) yaml_parse_expect_event_with_trace(parser, type, __LINE__, __FUNCTION__, __FILE__)
static bool yaml_parse_expect_event_with_trace(yaml_parser_t *parser, yaml_event_type_t type, size_t line, const char *function, const char *file) {
    yaml_event_t event;
    if (!yaml_parse(parser, &event))
        return false;

    bool ret = true;
    if(event.type != type) {
        yaml_error_with_trace(parser, &event, line, function, file, "unexpected event - expecting: %s", yaml_event_name(type));
        ret = false;
    }
//    else
//        fprintf(stderr, "OK (%zu@%s, %s()\n", line, file, function);

    yaml_event_delete(&event);
    return ret;
}

#define yaml_scalar_matches(event, s, len) yaml_scalar_matches_with_trace(event, s, len, __LINE__, __FUNCTION__, __FILE__)
static bool yaml_scalar_matches_with_trace(yaml_event_t *event, const char *s, size_t len, size_t line __maybe_unused, const char *function __maybe_unused, const char *file __maybe_unused) {
    if(event->type != YAML_SCALAR_EVENT)
        return false;

    if(len != event->data.scalar.length)
        return false;
//    else
//        fprintf(stderr, "OK (%zu@%s, %s()\n", line, file, function);

    return strcmp((char *)event->data.scalar.value, s) == 0;
}

// ----------------------------------------------------------------------------

static struct key_dup *yaml_parse_duplicate_key(struct log_job *jb, yaml_parser_t *parser) {
    yaml_event_t event;

    if (!yaml_parse(parser, &event))
        return false;

    struct key_dup *kd = NULL;
    if(event.type == YAML_SCALAR_EVENT) {
        kd = log_job_add_duplication_to_job(jb, (char *) event.data.scalar.value, event.data.scalar.length);
    }
    else
        yaml_error(parser, &event, "duplicate key must be a scalar.");

    yaml_event_delete(&event);
    return kd;
}

static size_t yaml_parse_duplicate_from(struct log_job *jb, yaml_parser_t *parser, struct key_dup *kd) {
    size_t errors = 0;
    yaml_event_t event;

    if (!yaml_parse(parser, &event))
        return 1;

    bool ret = true;
    if(event.type == YAML_SCALAR_EVENT)
        ret = log_job_add_key_to_duplication(kd, (char *) event.data.scalar.value, event.data.scalar.length);

    else if(event.type == YAML_SEQUENCE_START_EVENT) {
        bool finished = false;
        while(!errors && !finished) {
            yaml_event_t sub_event;
            if (!yaml_parse(parser, &sub_event))
                return errors++;
            else {
                if (sub_event.type == YAML_SCALAR_EVENT)
                    log_job_add_key_to_duplication(kd, (char *) sub_event.data.scalar.value
                                                   , sub_event.data.scalar.length
                                                  );

                else if (sub_event.type == YAML_SEQUENCE_END_EVENT)
                    finished = true;

                yaml_event_delete(&sub_event);
            }
        }
    }
    else
        yaml_error(parser, &event, "not expected event type");

    yaml_event_delete(&event);
    return errors;
}

static size_t yaml_parse_filename_injection(yaml_parser_t *parser, struct log_job *jb) {
    yaml_event_t event;
    size_t errors = 0;

    if(!yaml_parse_expect_event(parser, YAML_MAPPING_START_EVENT))
        return 1;

    if (!yaml_parse(parser, &event))
        return 1;

    if (yaml_scalar_matches(&event, "key", strlen("key"))) {
        yaml_event_t sub_event;
        if (!yaml_parse(parser, &sub_event))
            errors++;

        else {
            if (event.type == YAML_SCALAR_EVENT) {
                if(!log_job_add_filename_key(jb, (char *)sub_event.data.scalar.value, sub_event.data.scalar.length))
                    errors++;
            }

            else {
                yaml_error(parser, &sub_event, "expected the filename as %s", yaml_event_name(YAML_SCALAR_EVENT));
                errors++;
            }

            yaml_event_delete(&sub_event);
        }
    }

    if(!yaml_parse_expect_event(parser, YAML_MAPPING_END_EVENT))
        errors++;

    yaml_event_delete(&event);
    return errors;
}

static size_t yaml_parse_prefix(yaml_parser_t *parser, struct log_job *jb) {
    yaml_event_t event;
    size_t errors = 0;

    if (!yaml_parse(parser, &event))
        return 1;

    if (event.type == YAML_SCALAR_EVENT) {
        if(!log_job_add_key_prefix(jb, (char *)event.data.scalar.value, event.data.scalar.length))
            errors++;
    }

    yaml_event_delete(&event);
    return errors;
}

static size_t yaml_parse_duplicates_injection(yaml_parser_t *parser, struct log_job *jb) {
    if (!yaml_parse_expect_event(parser, YAML_SEQUENCE_START_EVENT))
        return 1;

    struct key_dup *kd = NULL;

    // Expecting a key-value pair for each duplicate
    bool finished;
    size_t errors = 0;
    while (!errors && !finished) {
        yaml_event_t event;
        if (!yaml_parse(parser, &event)) {
            errors++;
            break;
        }

        if(event.type == YAML_MAPPING_START_EVENT) {
            ;
        }
        if (event.type == YAML_SEQUENCE_END_EVENT) {
            finished = true;
        }
        else if(event.type == YAML_SCALAR_EVENT) {
            if (yaml_scalar_matches(&event, "key", strlen("key"))) {
                kd = yaml_parse_duplicate_key(jb, parser);
                if (!kd)
                    errors++;
                else {
                    while (!errors && kd) {
                        yaml_event_t sub_event;
                        if (!yaml_parse(parser, &sub_event)) {
                            errors++;
                            break;
                        }

                        if (sub_event.type == YAML_MAPPING_END_EVENT) {
                            kd = NULL;
                        } else if (sub_event.type == YAML_SCALAR_EVENT) {
                            if (yaml_scalar_matches(&sub_event, "values_of", strlen("values_of"))) {
                                if (!kd) {
                                    yaml_error(parser, &sub_event, "Found 'values_of' but the 'key' is not set.");
                                    errors++;
                                } else
                                    errors += yaml_parse_duplicate_from(jb, parser, kd);
                            } else {
                                yaml_error(parser, &sub_event, "unknown scalar");
                                errors++;
                            }
                        } else {
                            yaml_error(parser, &sub_event, "unexpected event type");
                            errors++;
                        }

                        // Delete the event after processing
                        yaml_event_delete(&event);
                    }
                }
            } else {
                yaml_error(parser, &event, "unknown scalar");
                errors++;
            }
        }

        yaml_event_delete(&event);
    }

    return errors;
}

static bool yaml_parse_constant_field_injection(yaml_parser_t *parser, struct log_job *jb, bool unmatched) {
    yaml_event_t event;
    if (!yaml_parse(parser, &event) || event.type != YAML_SCALAR_EVENT) {
        yaml_error(parser, &event, "Expected scalar for constant field injection key");
        yaml_event_delete(&event);
        return false;
    }

    char *key = strndupz((char *)event.data.scalar.value, event.data.scalar.length);
    char *value = NULL;
    bool ret = false;

    yaml_event_delete(&event);

    if (!yaml_parse(parser, &event) || event.type != YAML_SCALAR_EVENT) {
        yaml_error(parser, &event, "Expected scalar for constant field injection value");
        goto cleanup;
    }

    if(!yaml_scalar_matches(&event, "value", strlen("value"))) {
        yaml_error(parser, &event, "Expected scalar 'value'");
        goto cleanup;
    }

    if (!yaml_parse(parser, &event) || event.type != YAML_SCALAR_EVENT) {
        yaml_error(parser, &event, "Expected scalar for constant field injection value");
        goto cleanup;
    }

    value = strndupz((char *)event.data.scalar.value, event.data.scalar.length);

    if(!log_job_add_injection(jb, key, strlen(key), value, strlen(value), unmatched))
        ret = false;
    else
        ret = true;

    ret = true;

cleanup:
    yaml_event_delete(&event);
    freez(key);
    freez(value);
    return !ret ? 1 : 0;
}

static bool yaml_parse_injection_mapping(yaml_parser_t *parser, struct log_job *jb, bool unmatched) {
    yaml_event_t event;
    size_t errors = 0;
    bool finished = false;

    while (!errors && !finished) {
        if (!yaml_parse(parser, &event)) {
            errors++;
            continue;
        }

        switch (event.type) {
            case YAML_SCALAR_EVENT:
                if (yaml_scalar_matches(&event, "key", strlen("key"))) {
                    errors += yaml_parse_constant_field_injection(parser, jb, unmatched);
                } else {
                    yaml_error(parser, &event, "Unexpected scalar in injection mapping");
                    errors++;
                }
                break;

            case YAML_MAPPING_END_EVENT:
                finished = true;
                break;

            default:
                yaml_error(parser, &event, "Unexpected event in injection mapping");
                errors++;
                break;
        }

        yaml_event_delete(&event);
    }

    return errors == 0;
}

static size_t yaml_parse_injections(yaml_parser_t *parser, struct log_job *jb, bool unmatched) {
    yaml_event_t event;
    size_t errors = 0;
    bool finished = false;

    if (!yaml_parse_expect_event(parser, YAML_SEQUENCE_START_EVENT))
        return 1;

    while (!errors && !finished) {
        if (!yaml_parse(parser, &event)) {
            errors++;
            continue;
        }

        switch (event.type) {
            case YAML_MAPPING_START_EVENT:
                if (!yaml_parse_injection_mapping(parser, jb, unmatched))
                    errors++;
                break;

            case YAML_SEQUENCE_END_EVENT:
                finished = true;
                break;

            default:
                yaml_error(parser, &event, "Unexpected event in injections sequence");
                errors++;
                break;
        }

        yaml_event_delete(&event);
    }

    return errors;
}

static size_t yaml_parse_unmatched(yaml_parser_t *parser, struct log_job *jb) {
    size_t errors = 0;
    bool finished = false;

    if (!yaml_parse_expect_event(parser, YAML_MAPPING_START_EVENT))
        return 1;

    while (!errors && !finished) {
        yaml_event_t event;
        if (!yaml_parse(parser, &event)) {
            errors++;
            continue;
        }

        switch (event.type) {
            case YAML_SCALAR_EVENT:
                if (yaml_scalar_matches(&event, "key", strlen("key"))) {
                    yaml_event_t sub_event;
                    if (!yaml_parse(parser, &sub_event)) {
                        errors++;
                    } else {
                        if (sub_event.type == YAML_SCALAR_EVENT) {
                            jb->unmatched.key = strndupz((char *)sub_event.data.scalar.value, sub_event.data.scalar.length);
                        } else {
                            yaml_error(parser, &sub_event, "expected a scalar value for 'key'");
                            errors++;
                        }
                        yaml_event_delete(&sub_event);
                    }
                } else if (yaml_scalar_matches(&event, "inject", strlen("inject"))) {
                    errors += yaml_parse_injections(parser, jb, true);
                } else {
                    yaml_error(parser, &event, "Unexpected scalar in unmatched section");
                    errors++;
                }
                break;

            case YAML_MAPPING_END_EVENT:
                finished = true;
                break;

            default:
                yaml_error(parser, &event, "Unexpected event in unmatched section");
                errors++;
                break;
        }

        yaml_event_delete(&event);
    }

    return errors;
}

static size_t yaml_parse_rewrites(yaml_parser_t *parser, struct log_job *jb) {
    size_t errors = 0;

    if (!yaml_parse_expect_event(parser, YAML_SEQUENCE_START_EVENT))
        return 1;

    bool finished = false;
    while (!errors && !finished) {
        yaml_event_t event;
        if (!yaml_parse(parser, &event)) {
            errors++;
            continue;
        }

        switch (event.type) {
            case YAML_MAPPING_START_EVENT:
            {
                struct key_rewrite rw = {0};

                bool mapping_finished = false;
                while (!errors && !mapping_finished) {
                    yaml_event_t sub_event;
                    if (!yaml_parse(parser, &sub_event)) {
                        errors++;
                        continue;
                    }

                    switch (sub_event.type) {
                        case YAML_SCALAR_EVENT:
                            if (yaml_scalar_matches(&sub_event, "key", strlen("key"))) {
                                if (!yaml_parse(parser, &sub_event) || sub_event.type != YAML_SCALAR_EVENT) {
                                    yaml_error(parser, &sub_event, "Expected scalar for rewrite key");
                                    errors++;
                                } else {
                                    rw.key = strndupz((char *)sub_event.data.scalar.value, sub_event.data.scalar.length);
                                    yaml_event_delete(&sub_event);
                                }
                            } else if (yaml_scalar_matches(&sub_event, "search", strlen("search"))) {
                                if (!yaml_parse(parser, &sub_event) || sub_event.type != YAML_SCALAR_EVENT) {
                                    yaml_error(parser, &sub_event, "Expected scalar for rewrite search pattern");
                                    errors++;
                                } else {
                                    rw.search_pattern = strndupz((char *)sub_event.data.scalar.value, sub_event.data.scalar.length);
                                    yaml_event_delete(&sub_event);
                                }
                            } else if (yaml_scalar_matches(&sub_event, "replace", strlen("replace"))) {
                                if (!yaml_parse(parser, &sub_event) || sub_event.type != YAML_SCALAR_EVENT) {
                                    yaml_error(parser, &sub_event, "Expected scalar for rewrite replace pattern");
                                    errors++;
                                } else {
                                    rw.replace_pattern = strndupz((char *)sub_event.data.scalar.value, sub_event.data.scalar.length);
                                    yaml_event_delete(&sub_event);
                                }
                            } else {
                                yaml_error(parser, &sub_event, "Unexpected scalar in rewrite mapping");
                                errors++;
                            }
                            break;

                        case YAML_MAPPING_END_EVENT:
                            if(rw.key && rw.search_pattern && rw.replace_pattern) {
                                if (!log_job_add_rewrite(jb, rw.key, rw.search_pattern, rw.replace_pattern))
                                    errors++;
                            }
                            freez(rw.key);
                            freez(rw.search_pattern);
                            freez(rw.replace_pattern);
                            memset(&rw, 0, sizeof(rw));

                            mapping_finished = true;
                            break;

                        default:
                            yaml_error(parser, &sub_event, "Unexpected event in rewrite mapping");
                            errors++;
                            break;
                    }

                    yaml_event_delete(&sub_event);
                }
            }
                break;

            case YAML_SEQUENCE_END_EVENT:
                finished = true;
                break;

            default:
                yaml_error(parser, &event, "Unexpected event in rewrites sequence");
                errors++;
                break;
        }

        yaml_event_delete(&event);
    }

    return errors;
}

static size_t yaml_parse_renames(yaml_parser_t *parser, struct log_job *jb) {
    size_t errors = 0;

    if (!yaml_parse_expect_event(parser, YAML_SEQUENCE_START_EVENT))
        return 1;

    bool finished = false;
    while (!errors && !finished) {
        yaml_event_t event;
        if (!yaml_parse(parser, &event)) {
            errors++;
            continue;
        }

        switch (event.type) {
            case YAML_MAPPING_START_EVENT:
            {
                struct key_rename rn = {0};

                bool mapping_finished = false;
                while (!errors && !mapping_finished) {
                    yaml_event_t sub_event;
                    if (!yaml_parse(parser, &sub_event)) {
                        errors++;
                        continue;
                    }

                    switch (sub_event.type) {
                        case YAML_SCALAR_EVENT:
                            if (yaml_scalar_matches(&sub_event, "new_key", strlen("new_key"))) {
                                if (!yaml_parse(parser, &sub_event) || sub_event.type != YAML_SCALAR_EVENT) {
                                    yaml_error(parser, &sub_event, "Expected scalar for rename new_key");
                                    errors++;
                                } else {
                                    rn.new_key = strndupz((char *)sub_event.data.scalar.value, sub_event.data.scalar.length);
                                    yaml_event_delete(&sub_event);
                                }
                            } else if (yaml_scalar_matches(&sub_event, "old_key", strlen("old_key"))) {
                                if (!yaml_parse(parser, &sub_event) || sub_event.type != YAML_SCALAR_EVENT) {
                                    yaml_error(parser, &sub_event, "Expected scalar for rename old_key");
                                    errors++;
                                } else {
                                    rn.old_key = strndupz((char *)sub_event.data.scalar.value, sub_event.data.scalar.length);
                                    yaml_event_delete(&sub_event);
                                }
                            } else {
                                yaml_error(parser, &sub_event, "Unexpected scalar in rewrite mapping");
                                errors++;
                            }
                            break;

                        case YAML_MAPPING_END_EVENT:
                            if(rn.old_key && rn.new_key) {
                                if (!log_job_add_rename(jb, rn.new_key, strlen(rn.new_key), rn.old_key, strlen(rn.old_key)))
                                    errors++;
                            }
                            freez(rn.new_key);
                            freez(rn.old_key);
                            memset(&rn, 0, sizeof(rn));

                            mapping_finished = true;
                            break;

                        default:
                            yaml_error(parser, &sub_event, "Unexpected event in rewrite mapping");
                            errors++;
                            break;
                    }

                    yaml_event_delete(&sub_event);
                }
            }
                break;

            case YAML_SEQUENCE_END_EVENT:
                finished = true;
                break;

            default:
                yaml_error(parser, &event, "Unexpected event in rewrites sequence");
                errors++;
                break;
        }

        yaml_event_delete(&event);
    }

    return errors;
}

static size_t yaml_parse_pattern(yaml_parser_t *parser, struct log_job *jb) {
    yaml_event_t event;
    size_t errors = 0;

    if (!yaml_parse(parser, &event))
        return 1;

    if(event.type == YAML_SCALAR_EVENT)
        jb->pattern = strndupz((char *)event.data.scalar.value, event.data.scalar.length);
    else {
        yaml_error(parser, &event, "unexpected event type");
        errors++;
    }

    yaml_event_delete(&event);
    return errors;
}

static size_t yaml_parse_initialized(yaml_parser_t *parser, struct log_job *jb) {
    size_t errors = 0;

    if(!yaml_parse_expect_event(parser, YAML_STREAM_START_EVENT)) {
        errors++;
        goto cleanup;
    }

    if(!yaml_parse_expect_event(parser, YAML_DOCUMENT_START_EVENT)) {
        errors++;
        goto cleanup;
    }

    if(!yaml_parse_expect_event(parser, YAML_MAPPING_START_EVENT)) {
        errors++;
        goto cleanup;
    }

    bool finished = false;
    while (!errors && !finished) {
        yaml_event_t event;
        if(!yaml_parse(parser, &event)) {
            errors++;
            continue;
        }

        switch(event.type) {
            default:
                yaml_error(parser, &event, "unexpected type");
                errors++;
                break;

            case YAML_MAPPING_END_EVENT:
                finished = true;
                break;

            case YAML_SCALAR_EVENT:
                if (yaml_scalar_matches(&event, "pattern", strlen("pattern")))
                    errors += yaml_parse_pattern(parser, jb);

                else if (yaml_scalar_matches(&event, "prefix", strlen("prefix")))
                    errors += yaml_parse_prefix(parser, jb);

                else if (yaml_scalar_matches(&event, "filename", strlen("filename")))
                    errors += yaml_parse_filename_injection(parser, jb);

                else if (yaml_scalar_matches(&event, "duplicate", strlen("duplicate")))
                    errors += yaml_parse_duplicates_injection(parser, jb);

                else if (yaml_scalar_matches(&event, "inject", strlen("inject")))
                    errors += yaml_parse_injections(parser, jb, false);

                else if (yaml_scalar_matches(&event, "unmatched", strlen("unmatched")))
                    errors += yaml_parse_unmatched(parser, jb);

                else if (yaml_scalar_matches(&event, "rewrite", strlen("rewrite")))
                    errors += yaml_parse_rewrites(parser, jb);

                else if (yaml_scalar_matches(&event, "rename", strlen("rename")))
                    errors += yaml_parse_renames(parser, jb);

                else {
                    yaml_error(parser, &event, "unexpected scalar");
                    errors++;
                }
                break;
        }

        yaml_event_delete(&event);
    }

    if(!yaml_parse_expect_event(parser, YAML_DOCUMENT_END_EVENT)) {
        errors++;
        goto cleanup;
    }

    if(!yaml_parse_expect_event(parser, YAML_STREAM_END_EVENT)) {
        errors++;
        goto cleanup;
    }

cleanup:
    return errors;
}

bool yaml_parse_file(const char *config_file_path, struct log_job *jb) {
    if(!config_file_path || !*config_file_path) {
        log2stderr("yaml configuration filename cannot be empty.");
        return false;
    }

    FILE *fp = fopen(config_file_path, "r");
    if (!fp) {
        log2stderr("Error opening config file: %s", config_file_path);
        return false;
    }

    yaml_parser_t parser;
    yaml_parser_initialize(&parser);
    yaml_parser_set_input_file(&parser, fp);

    size_t errors = yaml_parse_initialized(&parser, jb);

    yaml_parser_delete(&parser);
    fclose(fp);
    return errors == 0;
}

bool yaml_parse_config(const char *config_name, struct log_job *jb) {
    char filename[FILENAME_MAX + 1];

    snprintf(filename, sizeof(filename), "%s/%s.yaml", LOG2JOURNAL_CONFIG_PATH, config_name);
    return yaml_parse_file(filename, jb);
}

#endif // HAVE_LIBYAML

// ----------------------------------------------------------------------------
// printing yaml

static void yaml_print_multiline_value(const char *s, size_t depth) {
    if (!s)
        s = "";

    do {
        const char* next = strchr(s, '\n');
        if(next) next++;

        size_t len = next ? (size_t)(next - s) : strlen(s);
        char buf[len + 1];
        strncpy(buf, s, len);
        buf[len] = '\0';

        fprintf(stderr, "%.*s%s%s",
                (int)(depth * 2), "                    ",
                buf, next ? "" : "\n");

        s = next;
    } while(s && *s);
}

static bool needs_quotes_in_yaml(const char *str) {
    // Lookup table for special YAML characters
    static bool special_chars[256] = { false };
    static bool table_initialized = false;

    if (!table_initialized) {
        // Initialize the lookup table
        const char *special_chars_str = ":{}[],&*!|>'\"%@`^";
        for (const char *c = special_chars_str; *c; ++c) {
            special_chars[(unsigned char)*c] = true;
        }
        table_initialized = true;
    }

    while (*str) {
        if (special_chars[(unsigned char)*str]) {
            return true;
        }
        str++;
    }
    return false;
}

static void yaml_print_node(const char *key, const char *value, size_t depth, bool dash) {
    if(depth > 10) depth = 10;
    const char *quote = "\"";

    const char *second_line = NULL;
    if(value && strchr(value, '\n')) {
        second_line = value;
        value = "|";
        quote = "";
    }
    else if(!value || !needs_quotes_in_yaml(value))
        quote = "";

    fprintf(stderr, "%.*s%s%s%s%s%s%s\n",
            (int)(depth * 2), "                    ", dash ? "- ": "",
            key ? key : "", key ? ": " : "",
            quote, value ? value : "", quote);

    if(second_line) {
        yaml_print_multiline_value(second_line, depth + 1);
    }
}

void log_job_to_yaml(struct log_job *jb) {
    if(jb->pattern)
        yaml_print_node("pattern", jb->pattern, 0, false);

    if(jb->prefix) {
        fprintf(stderr, "\n");
        yaml_print_node("prefix", jb->prefix, 0, false);
    }

    if(jb->filename.key) {
        fprintf(stderr, "\n");
        yaml_print_node("filename", NULL, 0, false);
        yaml_print_node("key", jb->filename.key, 1, false);
    }

    if(jb->dups.used) {
        fprintf(stderr, "\n");
        yaml_print_node("duplicate", NULL, 0, false);
        for(size_t i = 0; i < jb->dups.used ;i++) {
            struct key_dup *kd = &jb->dups.array[i];
            yaml_print_node("key", kd->target, 1, true);
            yaml_print_node("values_of", NULL, 2, false);

            for(size_t k = 0; k < kd->used ;k++)
                yaml_print_node(NULL, kd->keys[k], 3, true);
        }
    }

    if(jb->injections.used) {
        fprintf(stderr, "\n");
        yaml_print_node("inject", NULL, 0, false);

        for (size_t i = 0; i < jb->injections.used; i++) {
            yaml_print_node("key", jb->injections.keys[i].key, 1, true);
            yaml_print_node("value", jb->injections.keys[i].value.s, 2, false);
        }
    }

    if(jb->rewrites.used) {
        fprintf(stderr, "\n");
        yaml_print_node("rewrite", NULL, 0, false);

        for(size_t i = 0; i < jb->rewrites.used ;i++) {
            yaml_print_node("key", jb->rewrites.array[i].key, 1, true);
            yaml_print_node("search", jb->rewrites.array[i].search_pattern, 2, false);
            yaml_print_node("replace", jb->rewrites.array[i].replace_pattern, 2, false);
        }
    }

    if(jb->renames.used) {
        fprintf(stderr, "\n");
        yaml_print_node("rename", NULL, 0, false);

        for(size_t i = 0; i < jb->renames.used ;i++) {
            yaml_print_node("new_key", jb->renames.array[i].new_key, 1, true);
            yaml_print_node("old_key", jb->renames.array[i].old_key, 2, false);
        }
    }

    if(jb->unmatched.key || jb->unmatched.injections.used) {
        fprintf(stderr, "\n");
        yaml_print_node("unmatched", NULL, 0, false);

        if(jb->unmatched.key)
            yaml_print_node("key", jb->unmatched.key, 1, false);

        if(jb->unmatched.injections.used) {
            fprintf(stderr, "\n");
            yaml_print_node("inject", NULL, 1, false);

            for (size_t i = 0; i < jb->unmatched.injections.used; i++) {
                yaml_print_node("key", jb->unmatched.injections.keys[i].key, 2, true);
                yaml_print_node("value", jb->unmatched.injections.keys[i].value.s, 3, false);
            }
        }
    }
}
