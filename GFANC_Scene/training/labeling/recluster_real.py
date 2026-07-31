"""
真实噪声 K-means 场景聚类 — 在 label_real_noise.py 的 LMS 标注之后运行.

用法:
    python training/labeling/recluster_real.py

流程:
    1. 加载 label_real_noise.py 保存的 LMS 增益 (Gains_real_*.npy)
    2. K-means 扫描 K=[2..12], 表征度肘部法 + 去重护栏自动选最优 K
    3. 生成 scene_definitions_real.json (自动命名)
    4. 生成 SoftLabels (余弦相似度 → softmax)
    5. 更新 Index CSV 的 scene_id + centroid 列
    6. t-SNE 可视化

输入:
    D:\Dataset\Real_world_Dataset\Gains_real_*.npy
    D:\Dataset\Real_world_Dataset\Index_real_*.csv

输出:
    models/scene_definitions_real.json  (新场景定义)
    D:\Dataset\Real_world_Dataset\SoftLabels_real_*.npy
    D:\Dataset\Real_world_Dataset\Index_real_*.csv  (更新 scene_id + centroid)
    D:\Dataset\Real_world_Dataset\tsne_clustering_real.png
"""
import os, sys, json
import numpy as np
import pandas as pd
from sklearn.cluster import KMeans
from sklearn.metrics import silhouette_score
from collections import Counter, defaultdict
from pathlib import Path

_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
_PROJECT_ROOT = Path(os.path.abspath(os.path.join(_SCRIPT_DIR, '..', '..')))
sys.path.insert(0, str(_PROJECT_ROOT))

# ═══════════════════════════════════════════════════════════════
# 配置
# ═══════════════════════════════════════════════════════════════
DATA_DIR = r'D:\Dataset\Real_world_Dataset'
K_RANGE = [2, 3, 4, 5, 6, 7, 8, 10, 12]
# None=自动选择 (表征度肘部法 + 质心去重护栏, 见下方第 3 节注释).
# 2026-07-26 在当前数据上自动选择结果为 K=4.
# 仅在明确知道自动结果不合理时才手动指定整数覆盖.
FORCE_K = None
N_INIT = 50
RANDOM_STATE = 42
THRESHOLD = 0.5  # centroid → binary config 的相对阈值 (≥ max×THRESHOLD, 仅用于命名/展示)
OUTPUT_JSON = str(_PROJECT_ROOT / 'models' / 'scene_definitions_real.json')

# ═══════════════════════════════════════════════════════════════

# 1. 加载原始 LMS 增益
print('=' * 60)
print('  加载 LMS 增益...')
print('=' * 60)

gains_train = np.load(os.path.join(DATA_DIR, 'Gains_real_Training_data.npy'))
gains_val   = np.load(os.path.join(DATA_DIR, 'Gains_real_Validate_data.npy'))
gains_test  = np.load(os.path.join(DATA_DIR, 'Gains_real_Testing_data.npy'))

# L2 归一化: 消除响度差异, 按频谱模式聚类 (与 label_real_noise.py 一致;
# C端 construct_wc 对 centroid 做 max归一化, 只使用相对模式, 空间必须对齐)
from sklearn.preprocessing import normalize
gains_train = normalize(gains_train, norm='l2')
gains_val   = normalize(gains_val, norm='l2')
gains_test  = normalize(gains_test, norm='l2')

all_gains = np.vstack([gains_train, gains_val, gains_test])
n_samples, n_dims = all_gains.shape
S = 2  # 扬声器数
C = n_dims // S  # 每扬声器子带数 = 15

print(f'  Train: {gains_train.shape[0]}, Val: {gains_val.shape[0]}, Test: {gains_test.shape[0]}')
print(f'  总计: {n_samples} 样本, {n_dims} 维 (S={S}, C={C})')
print(f'  聚类空间: L2归一化, 仅在 Train 上拟合 (避免 val/test 泄漏)')

# 2. K-means 分析
print(f'\n{"=" * 60}')
print(f'  K-means 聚类 (K={K_RANGE})')
print(f'{"=" * 60}')

inertias = []
silhouettes = []
all_models = {}
all_labels = {}

for K in K_RANGE:
    # 仅在 Train 上拟合, val/test 用 predict (避免聚类阶段的泄漏)
    km = KMeans(n_clusters=K, n_init=N_INIT, random_state=RANDOM_STATE, max_iter=500)
    km.fit(gains_train)
    labels = np.concatenate([
        km.labels_,
        km.predict(gains_val),
        km.predict(gains_test),
    ])
    all_models[K] = km
    all_labels[K] = labels

    inertias.append(km.inertia_)

    # silhouette (子样本估计, 大数据集加速)
    if n_samples > 20000:
        rng = np.random.RandomState(RANDOM_STATE)
        idx = rng.choice(n_samples, 20000, replace=False)
        sil = silhouette_score(all_gains[idx], labels[idx])
    else:
        sil = silhouette_score(all_gains, labels)
    silhouettes.append(sil)

    counts = Counter(labels)
    top3_pct = sum(c for _, c in counts.most_common(3)) / n_samples * 100
    print(f'  K={K:2d}: inertia={km.inertia_:.1f}  silhouette={sil:.4f}  '
          f'classes={len(counts)}  top3={top3_pct:.0f}%  min={min(counts.values())}')

# 3. 选最优 K — 表征度肘部法 + 质心去重护栏
#    表征度 = 每样本与所属质心的余弦相似度均值, 衡量质心对"每样本最优增益
#    模式"的近似程度, 是降噪量(NR)的廉价代理. 2026-07-26 实测验证:
#      NR(质心固定增益, 320测试样本): K4=3.54 / K6=3.55 / K8=3.55 dB (饱和)
#      表征度边际增益: +0.0107(K3) → +0.0061(K4) → +0.0037(K5) → +0.0021(K6)
#    两条曲线在同一处拐弯 → 肘部即应用意义上的最优 K.
#    (旧 inertia 肘部法在本数据上误选 K=6 — 质心 cos_max=0.977 近重复, 已弃用)
#    护栏: 若肘部 K 的质心最大成对余弦 > 0.95 (存在近重复场景), 逐级降 K.
def _centroid_cos_max(km):
    cn = km.cluster_centers_ / (np.linalg.norm(km.cluster_centers_, axis=1, keepdims=True) + 1e-10)
    cos = cn @ cn.T
    return float(cos[np.triu_indices(cos.shape[0], k=1)].max())

reprs = []
for K in K_RANGE:
    km = all_models[K]
    cn = km.cluster_centers_ / (np.linalg.norm(km.cluster_centers_, axis=1, keepdims=True) + 1e-10)
    reprs.append(float((gains_train * cn[km.labels_]).sum(1).mean()))

n_K = len(K_RANGE)
x = np.arange(n_K)
y = np.array(reprs)
x_norm = x / (n_K - 1)
y_norm = (y - y.min()) / (y.max() - y.min() + 1e-10)
dx = x_norm[-1] - x_norm[0]
dy = y_norm[-1] - y_norm[0]
line_len = np.sqrt(dx**2 + dy**2) + 1e-10
ux, uy = dx / line_len, dy / line_len
distances = np.abs((x_norm - x_norm[0]) * uy - (y_norm - y_norm[0]) * ux)
elbow_idx = int(np.argmax(distances))

print(f'\n  Representativeness (cos sample-centroid, train):')
prev = 0.0
for i, K in enumerate(K_RANGE):
    print(f'    K={K:2d}: repr={reprs[i]:.4f} (delta +{reprs[i]-prev:.4f})  '
          f'centroid_cos_max={_centroid_cos_max(all_models[K]):.3f}  elbow_dist={distances[i]:.3f}')
    prev = reprs[i]

# 护栏: 肘部 K 存在近重复质心 (cos>0.95) 则逐级降 K
COS_DUP_GUARD = 0.95
while elbow_idx > 0 and _centroid_cos_max(all_models[K_RANGE[elbow_idx]]) > COS_DUP_GUARD:
    print(f'  ⚠ K={K_RANGE[elbow_idx]} 质心cos_max={_centroid_cos_max(all_models[K_RANGE[elbow_idx]]):.3f} '
          f'> {COS_DUP_GUARD} (近重复场景), 降 K')
    elbow_idx -= 1

best_K = K_RANGE[elbow_idx]
print(f'  自动选择 K={best_K} (表征度肘部 + 去重护栏)')

if FORCE_K is not None:
    print(f'  ⚡ 手动覆盖: K={best_K} → K={FORCE_K}')
    best_K = FORCE_K

# 4. 生成新场景定义
print(f'\n{"=" * 60}')
print(f'  生成 scene_definitions_real.json (K={best_K})')
print(f'{"=" * 60}')

km = all_models[best_K]
centroids = km.cluster_centers_
labels = all_labels[best_K]

# ── 质心间余弦相似度分析 ──
centroid_norms = np.linalg.norm(centroids, axis=1, keepdims=True)
centroid_cos = (centroids / (centroid_norms + 1e-10)) @ (centroids / (centroid_norms + 1e-10)).T
n_c = centroid_cos.shape[0]
triu_idx = np.triu_indices(n_c, k=1)
pairwise_sim = centroid_cos[triu_idx]
print(f'\n  质心间成对余弦相似度: mean={pairwise_sim.mean():.4f}, '
      f'min={pairwise_sim.min():.4f}, max={pairwise_sim.max():.4f}')
if pairwise_sim.max() > 0.7:
    print(f'  ⚠ 存在高度相似质心对 (cos>0.7), 场景可能冗余')

# ── 软标签峰值分布 ──
cos_sim_all = (all_gains / (np.linalg.norm(all_gains, axis=1, keepdims=True) + 1e-10)) @ \
              (centroids / (centroid_norms + 1e-10)).T
SOFT_TEMP = 0.2
soft_labels = np.exp(cos_sim_all / SOFT_TEMP) / np.exp(cos_sim_all / SOFT_TEMP).sum(axis=1, keepdims=True)
top1_probs = soft_labels.max(axis=1)
weak_threshold = 1.5 / best_K  # top-1 ≤ 1.5×均匀分布 = 峰值弱
print(f'  Soft-label top-1 概率: mean={top1_probs.mean():.3f}, median={np.median(top1_probs):.3f}, '
      f'P10={np.percentile(top1_probs, 10):.3f}, P90={np.percentile(top1_probs, 90):.3f}')
weak_count = int((top1_probs <= weak_threshold).sum())
print(f'  峰值弱 (≤{weak_threshold:.3f}) 占比: {weak_count}/{len(top1_probs)} ({100*weak_count/len(top1_probs):.1f}%)')

scenes = {}
for k in range(best_K):
    mask = labels == k
    sample_count = mask.sum()
    centroid = centroids[k].tolist()
    # 相对阈值: L2 归一化空间的质心幅值较小, 用 max 的 50% 判定活跃 band
    config = (np.array(centroid) >= THRESHOLD * np.max(centroid)).astype(int).tolist()

    active_bands = sorted(
        [(i, centroid[i]) for i in range(len(centroid)) if config[i] == 1],
        key=lambda x: x[1], reverse=True)

    # 自动命名: per-speaker per-band
    top_bands = [b for b, _ in active_bands[:5]]
    name_parts = []
    for b in top_bands[:3]:
        spk = b // C
        band = b % C
        name_parts.append(f'S{spk}B{band}')
    auto_name = '_'.join(name_parts) if name_parts else f'scene_{k:02d}'

    gain_mean = np.mean(all_gains[mask], axis=0).reshape(S, C)
    scenes[str(k)] = {
        'name': f'scene_{k:02d}_{auto_name}',
        'centroid': centroid,
        'config': config,
        'sample_count': int(sample_count),
        'top_active_bands': top_bands,
        'gain_mean': gain_mean.tolist(),
    }
    print(f'  Scene {k:2d}: {sample_count:6d} 样本 ({100*sample_count/n_samples:4.1f}%)  '
          f'{len([b for b,_ in active_bands])} 活跃band  {auto_name}')

doc = {
    'n_scenes': best_K,
    'n_bands': C,
    'n_speakers': S,
    'creation_date': pd.Timestamp.now().strftime('%Y-%m-%d %H:%M:%S'),
    'source': 'kmeans_on_real_noise',
    'threshold': THRESHOLD,
    'random_state': RANDOM_STATE,
    'scenes': scenes,
}

os.makedirs(os.path.dirname(OUTPUT_JSON), exist_ok=True)
with open(OUTPUT_JSON, 'w', encoding='utf-8') as f:
    json.dump(doc, f, indent=2, ensure_ascii=False)
print(f'\n  已保存: {OUTPUT_JSON}')

# 5. 更新 CSV 的 scene_id (用新聚类结果)
print(f'\n{"=" * 60}')
print(f'  更新 CSV scene_id...')
print(f'{"=" * 60}')

for split_name in ['Training_data', 'Validate_data', 'Testing_data']:
    csv_path = os.path.join(DATA_DIR, f'Index_real_{split_name}.csv')
    if not os.path.exists(csv_path):
        continue

    df = pd.read_csv(csv_path)
    gain_cols = [c for c in df.columns if c.startswith('gain_')]
    gains = df[gain_cols].values.astype(np.float32)

    # 用新 K-means 标签 (与拟合时一致的 L2 归一化空间)
    new_labels = km.predict(normalize(gains, norm='l2'))
    df['scene_id'] = new_labels

    # 同时更新 centroid 列 (band_)
    for b in range(n_dims):
        df[f'band_{b}'] = df['scene_id'].apply(lambda sid: float(centroids[sid][b]))

    df.to_csv(csv_path, index=False)

    # 生成并保存软标签 (余弦相似度 → softmax)
    from sklearn.preprocessing import normalize
    gains_norm = normalize(gains, norm='l2')
    cos_sim = gains_norm @ (centroids / (centroid_norms + 1e-10)).T
    soft = np.exp(cos_sim / SOFT_TEMP) / np.exp(cos_sim / SOFT_TEMP).sum(axis=1, keepdims=True)
    soft_path = os.path.join(DATA_DIR, f'SoftLabels_real_{split_name}.npy')
    np.save(soft_path, soft.astype(np.float32))

    print(f'  {split_name}: {len(df)} 样本, 分布: {dict(sorted(Counter(new_labels).items()))}')

print(f'\n  完成! 旧 scene 体系已替换.')
print(f'  新定义: {OUTPUT_JSON}')

# ── 类别→场景映射 (质量抽查, 此时 CSV 已更新为新 scene_id) ──
print(f'\n  类别→场景映射 (Top-2 场景):')
try:
    df_train = pd.read_csv(os.path.join(DATA_DIR, 'Index_real_Training_data.csv'))
    if 'category' in df_train.columns:
        cat_scene = defaultdict(Counter)
        for _, row in df_train.iterrows():
            cat_scene[row['category']][row['scene_id']] += 1
        for cat in sorted(cat_scene.keys()):
            top2 = cat_scene[cat].most_common(2)
            s0, c0 = top2[0]; pct0 = 100 * c0 / sum(cat_scene[cat].values())
            s1, c1 = top2[1] if len(top2) > 1 else ('-', 0)
            pct1 = 100 * c1 / sum(cat_scene[cat].values()) if len(top2) > 1 else 0
            flag = '⚠' if pct0 < 30 else '✓'
            print(f'  {flag} {cat:15s}: scene_{s0}({pct0:5.1f}%)  scene_{s1}({pct1:5.1f}%)  共{len(cat_scene[cat])}场景')
except Exception as e:
    print(f'  [跳过] {e}')

print(f'\n  下一步: 用新 scene 定义 + 新 CSV 训练 CNN')

# ── t-SNE 可视化 (验证聚类结构) ──
print(f'\n{"=" * 60}')
print(f'  t-SNE 可视化')
print(f'{"=" * 60}')
try:
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    from sklearn.manifold import TSNE

    N_VIZ = min(5000, n_samples)
    rng_viz = np.random.RandomState(RANDOM_STATE)
    viz_idx = rng_viz.choice(n_samples, N_VIZ, replace=False)
    viz_gains = all_gains[viz_idx]
    viz_labels = labels[viz_idx]

    # 尝试加载类别标签用于左图着色
    cat_labels = None
    try:
        df_train = pd.read_csv(os.path.join(DATA_DIR, 'Index_real_Training_data.csv'))
        df_val = pd.read_csv(os.path.join(DATA_DIR, 'Index_real_Validate_data.csv'))
        df_test = pd.read_csv(os.path.join(DATA_DIR, 'Index_real_Testing_data.csv'))
        df_all = pd.concat([df_train, df_val, df_test], ignore_index=True)
        if 'category' in df_all.columns:
            cat_labels = df_all['category'].values[viz_idx]
    except Exception:
        pass

    print(f'  计算 t-SNE ({N_VIZ} 样本, 约 30~60 秒)...')
    tsne = TSNE(n_components=2, random_state=RANDOM_STATE, perplexity=30, max_iter=1000)
    viz_2d = tsne.fit_transform(viz_gains)

    if cat_labels is not None:
        fig, axes = plt.subplots(1, 2, figsize=(16, 6))
        cat_list = sorted(set(cat_labels))
        colors = plt.cm.tab10(np.linspace(0, 1, len(cat_list)))
        ax = axes[0]
        for i, cat in enumerate(cat_list):
            mask = cat_labels == cat
            ax.scatter(viz_2d[mask, 0], viz_2d[mask, 1], c=[colors[i]], label=cat,
                       alpha=0.5, s=4)
        ax.set_title('t-SNE colored by Noise Category')
        ax.legend(markerscale=4, fontsize=8)
        ax = axes[1]
    else:
        fig, ax = plt.subplots(1, 1, figsize=(8, 6))

    for k in range(best_K):
        mask = viz_labels == k
        ax.scatter(viz_2d[mask, 0], viz_2d[mask, 1], label=f'scene_{k}',
                   alpha=0.5, s=4)
    ax.set_title(f't-SNE colored by K-means Cluster (K={best_K})')
    ax.legend(markerscale=4, fontsize=8)

    viz_path = os.path.join(DATA_DIR, 'tsne_clustering_real.png')
    plt.tight_layout()
    plt.savefig(viz_path, dpi=150)
    plt.close()
    print(f'  已保存: {viz_path}')
except ImportError as e:
    print(f'  [跳过] 缺少依赖 (pip install matplotlib scikit-learn): {e}')
except Exception as e:
    print(f'  [失败] t-SNE 可视化: {e}')
