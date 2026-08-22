/** SceneBank — SFANC 式硬选滤波器库 I/O (Phase 1).
 *
 *  库格式: 单文件 data/wc_bank.bin
 *    头 16B: magic "GFNC" (4B) + version u32 + n_slots u32 + slot_len u32
 *    数据:   n_slots × (slot_len float32)  // slot_len = S*L (本系统 2*1024=2048)
 *
 *  C 端 N (库槽数) 由 (n_floats)/(S*L) 推导, 无需 JSON 解析.
 *  data/wc_bank_info.json 仅人/工具可读 (类名/增益记录), C 端不依赖.
 *
 *  对齐校验: 加载期必须满足 n_slots == CNN K (分类输出维), 不符 FATAL —
 *  硬选库的类顺序 = CNN 标签 = C 名表三处必须一致.
 *
 *  绝对增益 (Phase 3 标定定稿): 库槽 = 就地 FxLMS 收敛的成品 Wc 原样写入,
 *  含完整绝对振幅+相位, 不做 RMS 归一化 — 这是"开环可降噪"与现状
 *  "~0.01 几乎无声" 的根本区别 (研究结论 §1).
 */
#ifndef SCENE_BANK_H
#define SCENE_BANK_H

#include <stddef.h>
#include <stdint.h>

#define SCENE_BANK_MAGIC  0x434E4647u   /* 磁盘字节 "GFNC" (LE 存储: 0x47'G'|0x46'F'|0x4E'N'|0x43'C') */
#define SCENE_BANK_VERSION 1u
#define SCENE_BANK_HEADER_SIZE 16       /* magic4 + version4 + n_slots4 + slot_len4 */

typedef struct {
    uint32_t n_slots;       /* 库槽数 N */
    uint32_t slot_len;      /* 每槽 float 数 = S*L */
    float   *data;          /* [n_slots * slot_len] 调用方 free */
} scene_bank_t;

/* 从文件加载整个库. S/L 用于校验 slot_len. 返回 0=成功, -1=文件缺失/头不符/尺寸不符. */
int  scene_bank_load(const char *path, int S, int L, scene_bank_t *bank);

/* 单槽保存 (标定写槽 k): 追加/覆盖 — 若文件已存在且 slot_len 一致, 只重写槽 k 段.
 * 文件不存在则先写头 + 全零填充. 返回 0=成功, -1=I/O 错/尺寸不符. */
int  scene_bank_save_slot(const char *path, int S, int L, int k, const float *wc);

/* 整个库落盘 (全量写, 槽数可增). 返回 0=成功, -1=I/O 错. */
int  scene_bank_write_all(const char *path, int S, int L, int n_slots, const float *data);

/* 释放 data (NULL 安全). */
void scene_bank_free(scene_bank_t *bank);

/* 槽 k 指针 (越界返回 NULL). */
const float *scene_bank_slot(const scene_bank_t *bank, int k);

/* ── 类名表 (Phase 3, 诊断用) ──
 * 槽序 == CNN 标签 == 此表三处必须与 SceneZone_Scene/models/
 * scene_definitions_bank.json 的 classes 一致 (硬选库对齐校验 #6).
 * 越界返回 "?". */
#define SCENE_BANK_CLASS_COUNT 4
const char *scene_bank_class_name(int k);

#endif
