/*
 * Copyright 2023 Henri Verbeet for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <stdbool.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/stat.h>

#include "vkd3d_common.h"
#include "vkd3d_shader.h"
#include "vkd3d_shader_utils.h"
#ifdef HAVE_NCURSES
#include <term.h>
#endif

static bool array_reserve(void **elements, size_t *capacity, size_t element_count, size_t element_size)
{
    size_t new_capacity, max_capacity;
    void *new_elements;

    if (element_count <= *capacity)
        return true;

    max_capacity = ~(size_t)0 / element_size;
    if (max_capacity < element_count)
        return false;

    new_capacity = max(*capacity, 4);
    while (new_capacity < element_count && new_capacity <= max_capacity / 2)
        new_capacity *= 2;

    if (new_capacity < element_count)
        new_capacity = element_count;

    if (!(new_elements = realloc(*elements, new_capacity * element_size)))
        return false;

    *elements = new_elements;
    *capacity = new_capacity;

    return true;
}

enum
{
    OPTION_COLOUR = CHAR_MAX + 1,
    OPTION_EMIT,
    OPTION_EXTRACT,
    OPTION_HELP,
    OPTION_IGNORE_CHECKSUM,
    OPTION_INPUT,
    OPTION_LIST,
    OPTION_LIST_DATA,
    OPTION_NO_COLOUR,
    OPTION_OUTPUT,
    OPTION_UPDATE,
    OPTION_VERSION,
};

struct action
{
    enum action_type
    {
        ACTION_TYPE_EMIT,
        ACTION_TYPE_EXTRACT,
        ACTION_TYPE_UPDATE,
    } type;
    union
    {
        struct emit
        {
            const char *output_filename;
        } emit;
        struct extract
        {
            const char *section;
            const char *output_filename;
        } extract;
        struct update
        {
            const char *section;
            const struct vkd3d_shader_code *input;
        } update;
    } u;
};

struct options
{
    const struct vkd3d_shader_code *input;
    const char *output_filename;
    bool print_help;
    bool list;
    bool list_data;
    bool print_version;
    bool ignore_checksum;

    struct vkd3d_shader_code *inputs;
    size_t input_count, input_capacity;

    struct action *actions;
    size_t action_count, action_capacity;

    struct colours
    {
        const char *reset;
        const char *index;
        const char *offset;
        const char *label;
    } colours;
};

static struct action *action_push(struct options *options, enum action_type type)
{
    struct action *action;

    if (!array_reserve((void **)&options->actions, &options->action_capacity,
            options->action_count + 1, sizeof(*options->actions)))
    {
        fprintf(stderr, "Out of memory.\n");
        exit(1);
    }

    action = &options->actions[options->action_count++];
    action->type = type;

    return action;
}

static bool has_colour(void)
{
#ifdef HAVE_NCURSES
    bool supported;
    int ret;

    if (!isatty(fileno(stdout)))
        return false;
    setupterm(NULL, fileno(stdout), &ret);
    if (ret != 1)
        return false;
    supported = !!tigetstr((char *)"setaf");
    del_curterm(cur_term);

    return supported;
#else
    return false;
#endif
}

static bool action_push_emit(struct options *options)
{
    struct action *action;

    action = action_push(options, ACTION_TYPE_EMIT);
    if (!options->output_filename && isatty(fileno(stdout)))
    {
        fprintf(stderr, "Output is a tty and output format is binary, exiting.\n"
                "If this is really what you intended, specify the output explicitly.\n");
        return false;
    }
    action->u.emit.output_filename = options->output_filename;

    return true;
}

static FILE *open_input(const char *filename, bool *close)
{
    FILE *f;

    *close = false;

    if (!filename || !strcmp(filename, "-"))
        return stdin;

    if (!(f = fopen(filename, "rb")))
    {
        fprintf(stderr, "Unable to open '%s' for reading.\n", filename);
        return NULL;
    }

    *close = true;
    return f;
}

static struct vkd3d_shader_code *input_push(struct options *options, const char *filename)
{
    struct vkd3d_shader_code *input;
    bool close_input;
    FILE *f;
    int ret;

    if (!(f = open_input(filename, &close_input)))
        return NULL;

    if (!array_reserve((void **)&options->inputs, &options->input_capacity,
            options->input_count + 1, sizeof(*options->inputs)))
    {
        fprintf(stderr, "Out of memory.\n");
        input = NULL;
        goto done;
    }

    input = &options->inputs[options->input_count];
    if ((ret = vkd3d_shader_code_from_file(input, f)) < 0)
    {
        fprintf(stderr, "Failed to read input blob, ret %d.\n", ret);
        input = NULL;
        goto done;
    }
    ++options->input_count;

done:
    if (close_input)
        fclose(f);

    return input;
}

static bool parse_command_line(int argc, char **argv, struct options *options)
{
    struct action *action;
    bool emit = false;
    int option;

    static struct option long_options[] =
    {
        {"colour",          no_argument,       NULL, OPTION_COLOUR},
        {"emit",            no_argument,       NULL, OPTION_EMIT},
        {"extract",         required_argument, NULL, OPTION_EXTRACT},
        {"help",            no_argument,       NULL, OPTION_HELP},
        {"ignore-checksum", no_argument,       NULL, OPTION_IGNORE_CHECKSUM},
        {"input",           required_argument, NULL, OPTION_INPUT},
        {"list",            no_argument,       NULL, OPTION_LIST},
        {"list-data",       no_argument,       NULL, OPTION_LIST_DATA},
        {"no-colour",       no_argument,       NULL, OPTION_NO_COLOUR},
        {"output",          required_argument, NULL, OPTION_OUTPUT},
        {"update",          required_argument, NULL, OPTION_UPDATE},
        {"version",         no_argument,       NULL, OPTION_VERSION},
        {NULL,              0,                 NULL, 0},
    };

    static const struct colours colours =
    {
        .reset = "\x1b[m",
        .index = "\x1b[92m",
        .offset = "\x1b[36m",
        .label = "\x1b[93m",
    };

    static const struct colours no_colours =
    {
        .reset = "",
        .index = "",
        .offset = "",
        .label = "",
    };

    memset(options, 0, sizeof(*options));
    if (!getenv("NO_COLOUR") && !getenv("NO_COLOR") && has_colour())
        options->colours = colours;
    else
        options->colours = no_colours;

    for (;;)
    {
        if ((option = getopt_long(argc, argv, "ex:hi:to:u:V", long_options, NULL)) == -1)
            break;

        switch (option)
        {
            case OPTION_COLOUR:
                options->colours = colours;
                break;

            case 'e':
            case OPTION_EMIT:
                if (!action_push_emit(options))
                    return false;
                emit = false;
                break;

            case 'x':
            case OPTION_EXTRACT:
                if (!(action = action_push(options, ACTION_TYPE_EXTRACT)))
                    return false;
                action->u.extract.section = optarg;
                action->u.extract.output_filename = options->output_filename;
                break;

            case 'h':
            case OPTION_HELP:
                options->print_help = true;
                return true;

            case 'i':
            case OPTION_INPUT:
                if (!(options->input = input_push(options, optarg)))
                    return false;
                break;

            case OPTION_IGNORE_CHECKSUM:
                options->ignore_checksum = true;
                break;

            case 't':
            case OPTION_LIST:
                options->list = true;
                break;

            case OPTION_LIST_DATA:
                options->list_data = true;
                break;

            case OPTION_NO_COLOUR:
                options->colours = no_colours;
                break;

            case 'o':
            case OPTION_OUTPUT:
                options->output_filename = optarg;
                break;

            case 'u':
            case OPTION_UPDATE:
                if (!options->input && !(options->input = input_push(options, NULL)))
                    return false;
                if (!(action = action_push(options, ACTION_TYPE_UPDATE)))
                    return false;
                action->u.update.section = optarg;
                action->u.update.input = options->input;
                emit = true;
                break;

            case 'V':
            case OPTION_VERSION:
                options->print_version = true;
                return true;

            default:
                return false;
        }
    }

    if (emit && !action_push_emit(options))
        return false;

    if (!(options->input = input_push(options, optind < argc ? argv[argc - 1] : NULL)))
        return false;

    return true;
}

static void print_usage(const char *program_name)
{
    static const char usage[] =
        "[options...] [file]\n"
        "Options:\n"
        "  --colour                 Enable colour, even when not supported by the output.\n"
        "  -e, --emit               Emit the content of the DXBC blob.\n"
        "  -x, --extract=<section>  Extract the data contained in <section>.\n"
        "                           <section> may either be of the form 'i:<index>' in\n"
        "                           order to specify a section by its index or of the\n"
        "                           form 't:<tag>' in order to specify a section by its\n"
        "                           tag. <tag> may be either a four-character string or a\n"
        "                           32-bit hexadecimal integer. For example: 'i:2',\n"
        "                           't:SHDR', 't:52444853'. If multiple sections match\n"
        "                           the same tag, the first matching section is used.\n"
        "  -h, --help               Display this information and exit.\n"
        "  --ignore-checksum        Do not validate the checksum when parsing the DXBC\n"
        "                           blob.\n"
        "  -i, --input=<file>       Set the input filename for --update.\n"
        "  -t, --list               List the contents of the DXBC blob.\n"
        "  --list-data              List the data contained in the DXBC sections.\n"
        "  --no-colour              Disable colour, even when supported by the output.\n"
        "  -o, --output=<file>      Set the output filename for --emit or --extract.\n"
        "  -u, --update=<section>   Update the data contained in <section>. The format of\n"
        "                           <section> is decribed in the description for\n"
        "                           '--extract'.\n"
        "  -V, --version            Display version information and exit.\n"
        "  --                       Stop option processing. Any subsequent argument is\n"
        "                           interpreted as a filename.\n"
        "\n"
        "If the input file is '-' or not specified, input will be read from standard\n"
        "input. Similarly, if the output file is '-' or not specified at the point\n"
        "--emit or --extract is found, output will be written to the standard output.\n"
        "\n"
        "Currently this tool can only re-emit the same DXBC blob it loaded. However, it\n"
        "will recompute the checksum while doing so, so --emit can be useful together\n"
        "with --ignore-checksum to fix the checksum for a blob.\n"
        "\n"
        "Options --emit, --extract, and --output can be specified more than once. The\n"
        "DXBC blob will be emitted once for each --emit occurrence, each time using the\n"
        "closest previous --output occurrence as output filename.\n";

    fprintf(stderr, "Usage: %s %s", program_name, usage);
}

static FILE *open_output(const char *filename, bool *close)
{
    FILE *f;

    *close = false;

    if (!filename || !strcmp(filename, "-"))
        return stdout;

    if (!(f = fopen(filename, "wb")))
    {
        fprintf(stderr, "Unable to open '%s' for writing.\n", filename);
        return NULL;
    }

    *close = true;
    return f;
}

static bool write_output(FILE *f, const struct vkd3d_shader_code *dxbc)
{
    const char *code = dxbc->code;
    size_t pos = 0, ret;

    while (pos != dxbc->size)
    {
        if (!(ret = fwrite(&code[pos], 1, dxbc->size - pos, f)))
            return false;
        pos += ret;
    }

    return true;
}

static const char *dump_tag(char *out, uint32_t tag)
{
    unsigned int i;

    memcpy(out, &tag, sizeof(tag));
    for (i = 0; i < sizeof(tag); ++i)
    {
        if (!isprint(out[i]))
            out[i] = '.';
    }

    return out;
}

static bool parse_tag(uint32_t *t, const char *s)
{
    char *end;

    if (strlen(s) == 4)
    {
        *t = VKD3D_MAKE_TAG(s[0], s[1], s[2], s[3]);
        return true;
    }

    if (strlen(s) == 8)
    {
        *t = strtoul(s, &end, 16);
        if (!*end)
            return true;
    }

    return false;
}

static void dump_line(const struct colours *colours, const char *prefix,
        const uint8_t *data, size_t offset, size_t count)
{
    const uint8_t *ptr = &data[offset];
    size_t i;

    if (!count)
        return;

    printf("%s%s%08zx%s  ", prefix, colours->offset, offset, colours->reset);
    for (i = 0; i < count; ++i)
    {
        printf("%02x ", ptr[i]);
        if (i == 7)
            printf(" ");
    }

    while (i < 16)
    {
        printf("   ");
        if (i == 7)
            printf(" ");
        ++i;
    }
    printf(" %s|%s", colours->offset, colours->reset);

    for (i = 0; i < count; ++i)
    {
        printf("%c", isprint(ptr[i]) ? ptr[i] : '.');
    }
    printf("%s|%s\n", colours->offset, colours->reset);
}

static void dump_data(const struct colours *colours, const char *prefix, const struct vkd3d_shader_code *data)
{
    size_t line_count, remainder, i;

    line_count = data->size / 16;
    remainder = data->size % 16;

    for (i = 0; i < line_count; ++i)
    {
        dump_line(colours, prefix, data->code, i * 16, 16);
    }
    if (remainder)
    {
        dump_line(colours, prefix, data->code, i * 16, remainder);
    }
    printf("%s%s%08zx%s\n", prefix, colours->offset, data->size, colours->reset);
}

static void dump_dxbc(const struct vkd3d_shader_code *dxbc, const struct vkd3d_shader_dxbc_desc *dxbc_desc,
        const struct options *options)
{
    const struct colours *colours = &options->colours;
    struct vkd3d_shader_dxbc_section_desc *section;
    char tag[4];
    size_t i;

    printf("         %stag%s: %08x (%.4s)\n",
            colours->label, colours->reset,
            dxbc_desc->tag, dump_tag(tag, dxbc_desc->tag));
    printf("    %schecksum%s: %08x %08x %08x %08x\n",
            colours->label, colours->reset,
            dxbc_desc->checksum[0], dxbc_desc->checksum[1],
            dxbc_desc->checksum[2], dxbc_desc->checksum[3]);
    printf("     %sversion%s: %u\n", colours->label, colours->reset, dxbc_desc->version);
    printf("        %ssize%s: %#zx (%zu) bytes\n", colours->label, colours->reset, dxbc_desc->size, dxbc_desc->size);
    printf("    %ssections%s: %zu\n\n", colours->label, colours->reset, dxbc_desc->section_count);

    printf(" %s#%s  %stag%s              %ssize (bytes)%s             %soffset (bytes)%s\n",
            colours->label, colours->reset,
            colours->label, colours->reset,
            colours->label, colours->reset,
            colours->label, colours->reset);
    for (i = 0; i < dxbc_desc->section_count; ++i)
    {
        char dec_size[32];
        size_t offset;

        section = &dxbc_desc->sections[i];
        sprintf(dec_size, "(%zu)", section->data.size);
        offset = (char *)section->data.code - (char *)dxbc->code;
        printf("%s%2zu%s  %08x (%.4s)  0x%08zx %-13s 0x%08zx (%zu)\n",
                colours->index, i, colours->reset,
                section->tag, dump_tag(tag, section->tag),
                section->data.size, dec_size,
                offset, offset);

        if (!options->list_data)
            continue;
        printf("\n");
        dump_data(colours, "  ", &section->data);
        printf("\n");
    }
}

static bool emit_dxbc(const struct vkd3d_shader_dxbc_desc *dxbc_desc, const struct emit *emit)
{
    struct vkd3d_shader_code dxbc;
    bool close_output;
    char *messages;
    FILE *output;
    bool success;
    int ret;

    ret = vkd3d_shader_serialize_dxbc(dxbc_desc->section_count, dxbc_desc->sections, &dxbc, &messages);
    if (messages)
        fputs(messages, stderr);
    vkd3d_shader_free_messages(messages);
    if (ret < 0)
        return false;

    if (!(output = open_output(emit->output_filename, &close_output)))
    {
        vkd3d_shader_free_shader_code(&dxbc);
        return false;
    }

    if (!(success = write_output(output, &dxbc)))
        fprintf(stderr, "Failed to write output blob.\n");

    if (close_output)
        fclose(output);
    vkd3d_shader_free_shader_code(&dxbc);
    return success;
}

static struct vkd3d_shader_dxbc_section_desc *get_section(size_t section_count,
        struct vkd3d_shader_dxbc_section_desc *sections, const char *s)
{
    uint32_t tag;
    size_t idx;
    char *end;

    if (!strncmp("i:", s, 2))
    {
        s += 2;
        idx = strtoul(s, &end, 10);
        if (*end || idx >= section_count)
        {
            fprintf(stderr, "Invalid section index '%s' specified.\n", s);
            return NULL;
        }
        return &sections[idx];
    }

    if (!strncmp("t:", s, 2))
    {
        s += 2;
        if (!parse_tag(&tag, s))
        {
            fprintf(stderr, "Invalid section tag '%s' specified.\n", s);
            return NULL;
        }
        for (idx = 0; idx < section_count; ++idx)
        {
            if (sections[idx].tag == tag)
                return &sections[idx];
        }
        fprintf(stderr, "Section '%s' not found.\n", s);
        return NULL;
    }

    fprintf(stderr, "Invalid section specifier '%s' specified.\n", s);
    return NULL;
}

static bool extract_section(struct vkd3d_shader_dxbc_desc *dxbc_desc, const struct extract *extract)
{
    const struct vkd3d_shader_dxbc_section_desc *section;
    bool close_output, success = true;
    FILE *output;

    if (!(section = get_section(dxbc_desc->section_count, dxbc_desc->sections, extract->section)))
        return false;

    if (!(output = open_output(extract->output_filename, &close_output)))
        return false;

    if (!extract->output_filename && isatty(fileno(output)))
        fprintf(stderr, "Output is a tty, skipping section '%s'.\n"
                "If this is really what you intended, specify the output explicitly.\n",
                extract->section);
    else if (!(success = write_output(output, &section->data)))
        fprintf(stderr, "Failed to write output.\n");

    if (close_output)
        fclose(output);

    return success;
}

static bool update_section(struct vkd3d_shader_dxbc_desc *dxbc_desc, const struct update *update)
{
    struct vkd3d_shader_dxbc_section_desc *section;

    if (!(section = get_section(dxbc_desc->section_count, dxbc_desc->sections, update->section)))
        return false;
    section->data = *update->input;

    return true;
}

static bool apply_actions(struct vkd3d_shader_dxbc_desc *dxbc_desc,
        size_t action_count, const struct action *actions)
{
    for (size_t i = 0; i < action_count; ++i)
    {
        const struct action *action = &actions[i];

        switch (action->type)
        {
            case ACTION_TYPE_EMIT:
                if (!emit_dxbc(dxbc_desc, &action->u.emit))
                    return false;
                break;

            case ACTION_TYPE_EXTRACT:
                if (!extract_section(dxbc_desc, &action->u.extract))
                    return false;
                break;

            case ACTION_TYPE_UPDATE:
                if (!update_section(dxbc_desc, &action->u.update))
                    return false;
                break;
        }
    }

    return true;
}

int main(int argc, char **argv)
{
    struct vkd3d_shader_dxbc_desc dxbc_desc;
    struct options options;
    char *messages;
    uint32_t flags;
    int fail = 1;
    size_t i;
    int ret;

    if (!parse_command_line(argc, argv, &options))
    {
        print_usage(argv[0]);
        goto done;
    }

    if (options.print_help || (argc < 2 && isatty(fileno(stdin))))
    {
        print_usage(argv[0]);
        fail = 0;
        goto done;
    }

    if (options.print_version)
    {
        const char *version = vkd3d_shader_get_version(NULL, NULL);

        fprintf(stdout, "vkd3d-dxbc version " PACKAGE_VERSION " using %s\n", version);
        fail = 0;
        goto done;
    }

    flags = 0;
    if (options.ignore_checksum)
        flags |= VKD3D_SHADER_PARSE_DXBC_IGNORE_CHECKSUM;

    ret = vkd3d_shader_parse_dxbc(options.input, flags, &dxbc_desc, &messages);
    if (messages)
        fputs(messages, stderr);
    vkd3d_shader_free_messages(messages);
    if (ret < 0)
        goto done;

    if (options.list || options.list_data)
        dump_dxbc(options.input, &dxbc_desc, &options);

    fail = !apply_actions(&dxbc_desc, options.action_count, options.actions);

    vkd3d_shader_free_dxbc(&dxbc_desc);
done:
    for (i = 0; i < options.input_count; ++i)
    {
        vkd3d_shader_free_shader_code(&options.inputs[i]);
    }
    free(options.actions);

    return fail;
}
