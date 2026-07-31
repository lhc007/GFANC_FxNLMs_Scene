"""
全局路径常量 - 模型、数据、输出的默认目录.
"""
from pathlib import Path

# 项目根目录：gfanc/ 的父目录
_PROJECT_ROOT = Path(__file__).resolve().parent.parent
# _PROJECT_ROOT = Path('D:/')
# 各类数据文件夹
MODELS_DIR        = _PROJECT_ROOT / 'models'
BANDPASS_FILTER_MAT = MODELS_DIR / 'bandpass_filter_20_1500Hz.mat'
NOISE_EXAMPLES_DIR = _PROJECT_ROOT / 'Noise Examples'
FIGURES_DIR       = _PROJECT_ROOT / 'figures'
SYNTHESIZED_DATASET_DIR = Path('D:/') / 'Dataset' / 'Synthetic_Noise_Dataset'
