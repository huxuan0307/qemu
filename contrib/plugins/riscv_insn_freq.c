/*
 * RISC-V instruction frequency plugin
 *
 * Count executed instructions by mnemonic and write CSV output.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <glib.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

typedef struct {
    char *mnemonic;
    qemu_plugin_u64 count;
} InsnStat;

static GHashTable *stats;
static GMutex stats_lock;
static bool do_inline = true;
static bool do_sort = true;
static guint64 topn;
static char *outfile;

static guint stat_hash(gconstpointer key)
{
    return g_str_hash(key);
}

static gboolean stat_equal(gconstpointer a, gconstpointer b)
{
    return g_str_equal(a, b);
}

static gint cmp_stat_by_count_desc(gconstpointer a, gconstpointer b, gpointer d)
{
    const InsnStat *sa = a;
    const InsnStat *sb = b;
    uint64_t ca = qemu_plugin_u64_sum(sa->count);
    uint64_t cb = qemu_plugin_u64_sum(sb->count);

    if (ca == cb) {
        return strcmp(sa->mnemonic, sb->mnemonic);
    }
    return ca > cb ? -1 : 1;
}

static InsnStat *get_or_create_stat(const char *mnemonic)
{
    InsnStat *stat;

    g_mutex_lock(&stats_lock);
    stat = g_hash_table_lookup(stats, mnemonic);
    if (!stat) {
        stat = g_new0(InsnStat, 1);
        stat->mnemonic = g_strdup(mnemonic);
        stat->count = qemu_plugin_scoreboard_u64(
            qemu_plugin_scoreboard_new(sizeof(uint64_t)));
        g_hash_table_insert(stats, stat->mnemonic, stat);
    }
    g_mutex_unlock(&stats_lock);

    return stat;
}

static char *extract_mnemonic(const char *disas)
{
    const char *p = disas;
    const char *end;

    while (*p == ' ' || *p == '\t') {
        p++;
    }
    end = p;
    while (*end != '\0' && *end != ' ' && *end != '\t') {
        end++;
    }

    if (end == p) {
        return NULL;
    }

    return g_strndup(p, end - p);
}

static void vcpu_insn_exec(unsigned int cpu_index, void *udata)
{
    InsnStat *stat = udata;
    qemu_plugin_u64_add(stat->count, cpu_index, 1);
}

static void vcpu_tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    size_t n = qemu_plugin_tb_n_insns(tb);

    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        g_autofree char *disas = qemu_plugin_insn_disas(insn);
        g_autofree char *mnemonic = NULL;
        InsnStat *stat;

        if (!disas) {
            continue;
        }

        mnemonic = extract_mnemonic(disas);
        if (!mnemonic) {
            continue;
        }

        stat = get_or_create_stat(mnemonic);
        if (do_inline) {
            qemu_plugin_register_vcpu_insn_exec_inline_per_vcpu(
                insn, QEMU_PLUGIN_INLINE_ADD_U64, stat->count, 1);
        } else {
            qemu_plugin_register_vcpu_insn_exec_cb(
                insn, vcpu_insn_exec, QEMU_PLUGIN_CB_NO_REGS, stat);
        }
    }
}

static void emit_stats_to_plugin_out(GList *items)
{
    guint64 emitted = 0;

    qemu_plugin_outs("mnemonic,count\n");
    for (GList *it = items; it; it = it->next) {
        InsnStat *s = it->data;
        uint64_t count = qemu_plugin_u64_sum(s->count);
        g_autofree char *line = NULL;

        if (topn != 0 && emitted >= topn) {
            break;
        }

        line = g_strdup_printf("%s,%" PRIu64 "\n", s->mnemonic, count);
        qemu_plugin_outs(line);
        emitted++;
    }
}

static bool emit_stats_to_file(FILE *fp, GList *items)
{
    guint64 emitted = 0;

    if (fprintf(fp, "mnemonic,count\n") < 0) {
        return false;
    }

    for (GList *it = items; it; it = it->next) {
        InsnStat *s = it->data;
        uint64_t count = qemu_plugin_u64_sum(s->count);

        if (topn != 0 && emitted >= topn) {
            break;
        }

        if (fprintf(fp, "%s,%" PRIu64 "\n", s->mnemonic, count) < 0) {
            return false;
        }
        emitted++;
    }

    return fflush(fp) == 0;
}

static void free_stat_entry(gpointer key, gpointer value, gpointer user_data)
{
    InsnStat *s = value;

    qemu_plugin_scoreboard_free(s->count.score);
    g_free(s->mnemonic);
    g_free(s);
}

static void plugin_exit(qemu_plugin_id_t id, void *p)
{
    GList *items = g_hash_table_get_values(stats);

    if (do_sort) {
        items = g_list_sort_with_data(items, cmp_stat_by_count_desc, NULL);
    }

    if (outfile) {
        FILE *fp = fopen(outfile, "w");

        if (!fp) {
            g_autofree char *msg = g_strdup_printf(
                "riscv_insn_freq: cannot open output file '%s'\n", outfile);
            qemu_plugin_outs(msg);
            emit_stats_to_plugin_out(items);
        } else {
            if (!emit_stats_to_file(fp, items)) {
                qemu_plugin_outs("riscv_insn_freq: failed while writing CSV, fallback to plugin output\n");
                emit_stats_to_plugin_out(items);
            }
            fclose(fp);
        }
    } else {
        emit_stats_to_plugin_out(items);
    }

    g_list_free(items);
    g_hash_table_foreach(stats, free_stat_entry, NULL);
    g_hash_table_destroy(stats);
    g_free(outfile);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        char *opt = argv[i];
        g_auto(GStrv) tokens = g_strsplit(opt, "=", 2);

        if (g_strcmp0(tokens[0], "outfile") == 0) {
            if (!tokens[1] || tokens[1][0] == '\0') {
                fprintf(stderr, "argument parsing failed: %s\n", opt);
                return -1;
            }
            g_free(outfile);
            outfile = g_strdup(tokens[1]);
        } else if (g_strcmp0(tokens[0], "inline") == 0) {
            if (!qemu_plugin_bool_parse(tokens[0], tokens[1], &do_inline)) {
                fprintf(stderr, "boolean argument parsing failed: %s\n", opt);
                return -1;
            }
        } else if (g_strcmp0(tokens[0], "sort") == 0) {
            if (!qemu_plugin_bool_parse(tokens[0], tokens[1], &do_sort)) {
                fprintf(stderr, "boolean argument parsing failed: %s\n", opt);
                return -1;
            }
        } else if (g_strcmp0(tokens[0], "topn") == 0) {
            char *endptr = NULL;

            if (!tokens[1] || tokens[1][0] == '\0') {
                fprintf(stderr, "unsigned integer parsing failed: %s\n", opt);
                return -1;
            }

            topn = g_ascii_strtoull(tokens[1], &endptr, 10);
            if (endptr == tokens[1] || *endptr != '\0') {
                fprintf(stderr, "unsigned integer parsing failed: %s\n", opt);
                return -1;
            }
        } else {
            fprintf(stderr, "option parsing failed: %s\n", opt);
            return -1;
        }
    }

    if (info && info->target_name &&
        !g_str_has_prefix(info->target_name, "riscv")) {
        fprintf(stderr,
                "riscv_insn_freq is intended for riscv targets, got: %s\n",
                info->target_name);
        return -1;
    }

    stats = g_hash_table_new(stat_hash, stat_equal);

    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
    return 0;
}
