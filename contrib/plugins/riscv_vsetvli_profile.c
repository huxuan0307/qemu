/*
 * RISC-V vsetvli/vsetivli profiling plugin
 *
 * Statistics format:
 *   pc,category,rs1_value,count
 *
 * Categories:
 *   - rs1_ne_x0
 *   - rs1_eq_x0_rd_ne_x0
 *   - rs1_eq_x0_rd_eq_x0
 *   - vsetivli
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <glib.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

typedef enum {
    CAT_RS1_NE_X0 = 0,
    CAT_RS1_EQ_X0_RD_NE_X0 = 1,
    CAT_RS1_EQ_X0_RD_EQ_X0 = 2,
    CAT_VSETIVLI = 3,
} VsetCategory;

typedef struct {
    uint64_t pc;
    uint8_t category;
    uint8_t rs1;
    bool is_vsetivli;
} InsnMeta;

typedef struct {
    uint64_t pc;
    uint8_t category;
    bool has_rs1_value;
    uint64_t rs1_value;
} StatKey;

typedef struct {
    struct qemu_plugin_register *x[32];
} VcpuRegs;

static GHashTable *meta_pool;
static GHashTable *stats;
static GMutex meta_lock;
static GMutex stats_lock;
static GMutex vcpu_lock;
static GArray *vcpu_regs;

static char *outfile;
static bool do_sort = true;

static guint meta_hash(gconstpointer key)
{
    const InsnMeta *m = key;
    guint h = g_int64_hash(&m->pc);
    h = (h * 131u) ^ m->category;
    h = (h * 131u) ^ m->rs1;
    h = (h * 131u) ^ (m->is_vsetivli ? 1u : 0u);
    return h;
}

static gboolean meta_equal(gconstpointer a, gconstpointer b)
{
    const InsnMeta *ma = a;
    const InsnMeta *mb = b;

    return ma->pc == mb->pc &&
           ma->category == mb->category &&
           ma->rs1 == mb->rs1 &&
           ma->is_vsetivli == mb->is_vsetivli;
}

static guint stat_hash(gconstpointer key)
{
    const StatKey *k = key;
    guint h = g_int64_hash(&k->pc);
    h = (h * 131u) ^ k->category;
    h = (h * 131u) ^ (k->has_rs1_value ? 1u : 0u);
    h = (h * 131u) ^ g_int64_hash(&k->rs1_value);
    return h;
}

static gboolean stat_equal(gconstpointer a, gconstpointer b)
{
    const StatKey *ka = a;
    const StatKey *kb = b;

    return ka->pc == kb->pc &&
           ka->category == kb->category &&
           ka->has_rs1_value == kb->has_rs1_value &&
           ka->rs1_value == kb->rs1_value;
}

static const char *category_to_str(uint8_t category)
{
    switch ((VsetCategory)category) {
    case CAT_RS1_NE_X0:
        return "rs1_ne_x0";
    case CAT_RS1_EQ_X0_RD_NE_X0:
        return "rs1_eq_x0_rd_ne_x0";
    case CAT_RS1_EQ_X0_RD_EQ_X0:
        return "rs1_eq_x0_rd_eq_x0";
    case CAT_VSETIVLI:
        return "vsetivli";
    default:
        return "unknown";
    }
}

static guint8 decode_category_from_rd_rs1(uint8_t rd, uint8_t rs1)
{
    if (rs1 != 0) {
        return CAT_RS1_NE_X0;
    }
    if (rd != 0) {
        return CAT_RS1_EQ_X0_RD_NE_X0;
    }
    return CAT_RS1_EQ_X0_RD_EQ_X0;
}

static inline bool opcode_base_is_vset(uint32_t op)
{
    return (op & 0x7fu) == 0x57u && ((op >> 12) & 0x7u) == 0x7u;
}

static inline bool opcode_is_vsetvli(uint32_t op)
{
    return opcode_base_is_vset(op) && ((op >> 31) & 0x1u) == 0;
}

static inline bool opcode_is_vsetivli(uint32_t op)
{
    return opcode_base_is_vset(op) && ((op >> 30) & 0x3u) == 0x3u;
}

static inline uint8_t opcode_rd(uint32_t op)
{
    return (op >> 7) & 0x1fu;
}

static inline uint8_t opcode_rs1(uint32_t op)
{
    return (op >> 15) & 0x1fu;
}

static InsnMeta *get_or_create_meta(uint64_t pc, bool is_vsetivli,
                                    uint8_t category, uint8_t rs1)
{
    InsnMeta needle = {
        .pc = pc,
        .category = category,
        .rs1 = rs1,
        .is_vsetivli = is_vsetivli,
    };
    InsnMeta *m;

    g_mutex_lock(&meta_lock);
    m = g_hash_table_lookup(meta_pool, &needle);
    if (!m) {
        m = g_new(InsnMeta, 1);
        *m = needle;
        g_hash_table_insert(meta_pool, m, m);
    }
    g_mutex_unlock(&meta_lock);

    return m;
}

static void ensure_vcpu_slot(unsigned int cpu_index)
{
    g_mutex_lock(&vcpu_lock);
    if (cpu_index >= vcpu_regs->len) {
        g_array_set_size(vcpu_regs, cpu_index + 1);
    }
    g_mutex_unlock(&vcpu_lock);
}

static int parse_x_register_index(const char *name)
{
    const char *p;
    char *endptr = NULL;
    unsigned long v;

    if (!name || name[0] != 'x') {
        return -1;
    }

    p = name + 1;
    if (*p == '\0') {
        return -1;
    }

    v = g_ascii_strtoull(p, &endptr, 10);
    if (*endptr != '\0' || v > 31) {
        return -1;
    }

    return (int)v;
}

static int parse_riscv_gpr_alias(const char *name)
{
    static const struct {
        const char *name;
        int idx;
    } aliases[] = {
        { "zero", 0 },
        { "ra", 1 },
        { "sp", 2 },
        { "gp", 3 },
        { "tp", 4 },
        { "t0", 5 },
        { "t1", 6 },
        { "t2", 7 },
        { "s0", 8 },
        { "fp", 8 },
        { "s1", 9 },
        { "a0", 10 },
        { "a1", 11 },
        { "a2", 12 },
        { "a3", 13 },
        { "a4", 14 },
        { "a5", 15 },
        { "a6", 16 },
        { "a7", 17 },
        { "s2", 18 },
        { "s3", 19 },
        { "s4", 20 },
        { "s5", 21 },
        { "s6", 22 },
        { "s7", 23 },
        { "s8", 24 },
        { "s9", 25 },
        { "s10", 26 },
        { "s11", 27 },
        { "t3", 28 },
        { "t4", 29 },
        { "t5", 30 },
        { "t6", 31 },
    };

    for (size_t i = 0; i < G_N_ELEMENTS(aliases); i++) {
        if (g_strcmp0(name, aliases[i].name) == 0) {
            return aliases[i].idx;
        }
    }

    return -1;
}

static int parse_riscv_gpr_name(const char *name)
{
    g_auto(GStrv) parts = NULL;

    if (!name || name[0] == '\0') {
        return -1;
    }

    parts = g_strsplit(name, "/", -1);
    if (!parts) {
        return -1;
    }

    for (int i = 0; parts[i] != NULL; i++) {
        g_autofree char *lower = g_utf8_strdown(parts[i], -1);
        int idx = parse_x_register_index(lower);
        if (idx >= 0) {
            return idx;
        }

        idx = parse_riscv_gpr_alias(lower);
        if (idx >= 0) {
            return idx;
        }
    }

    return -1;
}

static uint64_t byte_array_to_u64_le(const GByteArray *buf)
{
    uint64_t v = 0;
    guint n = MIN(buf->len, (guint)sizeof(v));

    for (guint i = 0; i < n; i++) {
        v |= ((uint64_t)buf->data[i]) << (i * 8);
    }

    return v;
}

static bool read_x_reg_value(unsigned int cpu_index, uint8_t xidx,
                             uint64_t *out)
{
    struct qemu_plugin_register *reg;
    g_autoptr(GByteArray) buf = g_byte_array_new();

    if (xidx >= 32) {
        return false;
    }

    g_mutex_lock(&vcpu_lock);
    if (cpu_index >= vcpu_regs->len) {
        g_mutex_unlock(&vcpu_lock);
        return false;
    }
    reg = g_array_index(vcpu_regs, VcpuRegs, cpu_index).x[xidx];
    g_mutex_unlock(&vcpu_lock);

    if (!reg) {
        return false;
    }

    if (!qemu_plugin_read_register(reg, buf)) {
        return false;
    }

    *out = byte_array_to_u64_le(buf);
    return true;
}

static void stats_increment(uint64_t pc, uint8_t category,
                            bool has_rs1_value, uint64_t rs1_value)
{
    StatKey needle = {
        .pc = pc,
        .category = category,
        .has_rs1_value = has_rs1_value,
        .rs1_value = rs1_value,
    };
    StatKey *key;
    uint64_t *cnt;

    g_mutex_lock(&stats_lock);
    cnt = g_hash_table_lookup(stats, &needle);
    if (!cnt) {
        key = g_new(StatKey, 1);
        *key = needle;
        cnt = g_new0(uint64_t, 1);
        g_hash_table_insert(stats, key, cnt);
    }
    (*cnt)++;
    g_mutex_unlock(&stats_lock);
}

static void on_vsetvli_exec(unsigned int cpu_index, void *udata)
{
    const InsnMeta *m = udata;

    if (m->category == CAT_RS1_NE_X0) {
        uint64_t rs1_value;

        if (read_x_reg_value(cpu_index, m->rs1, &rs1_value)) {
            stats_increment(m->pc, m->category, true, rs1_value);
        }
    } else {
        stats_increment(m->pc, m->category, false, 0);
    }
}

static void on_vsetivli_exec(unsigned int cpu_index, void *udata)
{
    const InsnMeta *m = udata;

    (void)cpu_index;
    stats_increment(m->pc, m->category, false, 0);
}

static void vcpu_init(qemu_plugin_id_t id, unsigned int cpu_index)
{
    g_autoptr(GArray) regs = qemu_plugin_get_registers();
    VcpuRegs local = {0};

    (void)id;

    if (regs) {
        for (guint i = 0; i < regs->len; i++) {
            qemu_plugin_reg_descriptor *rd =
                &g_array_index(regs, qemu_plugin_reg_descriptor, i);
            int idx = parse_riscv_gpr_name(rd->name);
            if (idx >= 0) {
                local.x[idx] = rd->handle;
            }
        }
    }

    ensure_vcpu_slot(cpu_index);

    g_mutex_lock(&vcpu_lock);
    g_array_index(vcpu_regs, VcpuRegs, cpu_index) = local;
    g_mutex_unlock(&vcpu_lock);
}

static void vcpu_tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    size_t n = qemu_plugin_tb_n_insns(tb);

    (void)id;

    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        uint64_t pc = qemu_plugin_insn_vaddr(insn);
        uint32_t op = 0;
        size_t copied;

        copied = qemu_plugin_insn_data(insn, &op, sizeof(op));
        if (copied < sizeof(op)) {
            continue;
        }

        if (opcode_is_vsetvli(op)) {
            uint8_t rd = opcode_rd(op);
            uint8_t rs1 = opcode_rs1(op);
            uint8_t category = decode_category_from_rd_rs1(rd, rs1);
            InsnMeta *meta = get_or_create_meta(pc, false, category, rs1);

            qemu_plugin_register_vcpu_insn_exec_cb(
                insn, on_vsetvli_exec, QEMU_PLUGIN_CB_R_REGS, meta);
        } else if (opcode_is_vsetivli(op)) {
            InsnMeta *meta = get_or_create_meta(pc, true, CAT_VSETIVLI, 0);

            qemu_plugin_register_vcpu_insn_exec_cb(
                insn, on_vsetivli_exec, QEMU_PLUGIN_CB_NO_REGS, meta);
        }
    }
}

static gint cmp_stats(gconstpointer a, gconstpointer b)
{
    const StatKey *ka = a;
    const StatKey *kb = b;

    if (ka->pc != kb->pc) {
        return ka->pc < kb->pc ? -1 : 1;
    }
    if (ka->category != kb->category) {
        return ka->category < kb->category ? -1 : 1;
    }
    if (ka->has_rs1_value != kb->has_rs1_value) {
        return ka->has_rs1_value ? -1 : 1;
    }
    if (ka->rs1_value != kb->rs1_value) {
        return ka->rs1_value < kb->rs1_value ? -1 : 1;
    }
    return 0;
}

static bool emit_csv(FILE *fp)
{
    GList *keys;

    if (fprintf(fp, "pc,category,rs1_value,count\n") < 0) {
        return false;
    }

    g_mutex_lock(&stats_lock);
    keys = g_hash_table_get_keys(stats);
    g_mutex_unlock(&stats_lock);

    if (do_sort) {
        keys = g_list_sort(keys, cmp_stats);
    }

    for (GList *it = keys; it; it = it->next) {
        const StatKey *k = it->data;
        const uint64_t *cnt;

        g_mutex_lock(&stats_lock);
        cnt = g_hash_table_lookup(stats, k);
        g_mutex_unlock(&stats_lock);

        if (!cnt) {
            continue;
        }

        if (k->has_rs1_value) {
            if (fprintf(fp, "0x%" PRIx64 ",%s,0x%" PRIx64 ",%" PRIu64 "\n",
                        k->pc, category_to_str(k->category),
                        k->rs1_value, *cnt) < 0) {
                g_list_free(keys);
                return false;
            }
        } else {
            if (fprintf(fp, "0x%" PRIx64 ",%s,NA,%" PRIu64 "\n",
                        k->pc, category_to_str(k->category), *cnt) < 0) {
                g_list_free(keys);
                return false;
            }
        }
    }

    g_list_free(keys);
    return fflush(fp) == 0;
}

static void plugin_exit(qemu_plugin_id_t id, void *p)
{
    FILE *fp = NULL;
    bool dump_to_plugin_out = (outfile == NULL);

    (void)id;
    (void)p;

    if (outfile) {
        fp = fopen(outfile, "w");
        if (!fp) {
            g_autofree char *msg = g_strdup_printf(
                "riscv_vsetvli_profile: cannot open output file '%s'\n",
                outfile);
            qemu_plugin_outs(msg);
            dump_to_plugin_out = true;
        }
    }

    if (dump_to_plugin_out && !fp) {
        fp = tmpfile();
    }

    if (fp) {
        bool ok = emit_csv(fp);
        if (!ok) {
            qemu_plugin_outs("riscv_vsetvli_profile: failed to emit CSV\n");
        } else if (dump_to_plugin_out) {
            long len;
            rewind(fp);
            fseek(fp, 0, SEEK_END);
            len = ftell(fp);
            rewind(fp);
            if (len > 0) {
                g_autofree char *buf = g_malloc((size_t)len + 1);
                size_t got = fread(buf, 1, (size_t)len, fp);
                buf[got] = '\0';
                qemu_plugin_outs(buf);
            }
        }
        fclose(fp);
    }

    g_hash_table_destroy(stats);
    g_hash_table_destroy(meta_pool);
    g_array_free(vcpu_regs, true);
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
        } else if (g_strcmp0(tokens[0], "sort") == 0) {
            if (!qemu_plugin_bool_parse(tokens[0], tokens[1], &do_sort)) {
                fprintf(stderr, "boolean argument parsing failed: %s\n", opt);
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
                "riscv_vsetvli_profile is intended for riscv targets, got: %s\n",
                info->target_name);
        return -1;
    }

    stats = g_hash_table_new_full(stat_hash, stat_equal, g_free, g_free);
    meta_pool = g_hash_table_new_full(meta_hash, meta_equal, g_free, NULL);
    vcpu_regs = g_array_new(false, true, sizeof(VcpuRegs));

    qemu_plugin_register_vcpu_init_cb(id, vcpu_init);
    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);

    return 0;
}
