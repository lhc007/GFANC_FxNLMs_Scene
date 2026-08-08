"""生成合成直接权重检查点 (冒烟用) — m5_scene(K=30, dropout=0.3).

用途: 管道验证 (export_bin.py → main.exe) 在真实训练产出前能跑通:
  * CNN 输出 30 维 (S×C = 2 扬声器 × 15 子带增益), 而非场景分类的 K 维概率.
  * 权重为固定种子 xavier 初始化的随机值 — 不代表真实声学, 仅验证 C 运行时
    (scene_ctrl_process → tanh 增益 → Wc 构造) 与导出链路无维度/格式错误.

真实训练后重跑 Train_validate.py 会覆盖此文件.

用法: python export/make_synthetic_dw_ckpt.py
"""
import os, sys, torch
from pathlib import Path

SCRIPT_DIR = Path(os.path.dirname(os.path.abspath(__file__)))
PY_PROJ = SCRIPT_DIR.parent / 'GFANC_Scene'

# 固定种子 → 可复现的合成权重 (不影响管道验证; 真实模型会覆盖)
torch.manual_seed(20260807)
torch.backends.cudnn.deterministic = True

sys.path.insert(0, str(PY_PROJ))
from gfanc.Network import m5_scene   # noqa: E402

K = 30   # S×C = 2×15 直接权重输出维 (与 scene_controller.h SC_DW_MAX 一致)
model = m5_scene(K=K, dropout=0.3)

# xavier 均匀初始化所有 Conv/Linear 权重 (默认 PyTorch 也是类似分布, 显式化保证可复现)
def xavier_init(m):
    if isinstance(m, torch.nn.Conv1d) or isinstance(m, torch.nn.Linear):
        torch.nn.init.xavier_uniform_(m.weight)
        if m.bias is not None:
            torch.nn.init.zeros_(m.bias)
    elif isinstance(m, torch.nn.BatchNorm1d):
        torch.nn.init.ones_(m.weight)
        torch.nn.init.zeros_(m.bias)

model.apply(xavier_init)
model.eval()

out_path = PY_PROJ / 'models' / 'MIMO_M5_DirectWeight_Real.pth'
torch.save(model.state_dict(), str(out_path))

n_out = model.state_dict()['linear.weight'].shape[0]
assert n_out == K, f'linear out {n_out} != {K}'
n_param = sum(p.numel() for p in model.parameters())
print(f'Saved synthetic direct-weight checkpoint: {out_path}')
print(f'  linear out = {n_out} (=S×C={K}), params = {n_param/1e3:.1f}K, seed=20260807')
print('  NOTE: 权重为随机初始化 (冒烟用) — 真实训练 (Train_validate.py) 产出会覆盖此文件.')
