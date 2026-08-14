"""生成 CNN 输入带通 FIR (.mat) — 与部署 bandpass_fir.bin 同源.

背景:
  `GFANC_Scene/models/bandpass_fir.mat` 是 CNN 输入带通的唯一来源 (过去是来路不明的
  静态 .mat, 2026-08-14 补上生成脚本). 训练/导出脚本只「读」它 (fir_bandpass_coeff),
  从不「写」它. 本脚本补上来源, 使其可复现. 2026-08-14 整套降噪范围折中为 50-1500Hz.

用法:
    python export/gen_bandpass_fir.py [--f-low 50 --f-high 1500 --taps 1024]

输出:
    GFANC_Scene/models/bandpass_fir.mat   (固定文件名, 与频率无关 — 改范围不连带改路径)
    键: fir_bandpass_coeff(1,taps,float64) + fs/f_low/f_high/n_taps(标量 int)

与 C 端 / 训练端一致性:
    - 1024 tap = main.c/main_realtime.c BP_LEN (CNN 分类需频率分辨率)
    - 由 export_bin.py 读 fir_bandpass_coeff → data/bandpass_fir.bin
    - 由训练脚本 (Train_validate/verify_discrimination/...) 读同键做输入带通
"""
import argparse
import numpy as np
from pathlib import Path
from scipy import signal
from scipy.io import savemat

MODELS_DIR = Path(__file__).resolve().parent.parent / 'GFANC_Scene' / 'models'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--f-low', type=float, default=50.0)
    ap.add_argument('--f-high', type=float, default=1500.0)
    ap.add_argument('--taps', type=int, default=1024)
    ap.add_argument('--fs', type=int, default=16000)
    args = ap.parse_args()

    coeff = signal.firwin(args.taps, [args.f_low, args.f_high],
                          pass_zero='bandpass', window='hamming', fs=args.fs)
    out = MODELS_DIR / 'bandpass_fir.mat'
    savemat(str(out), {
        'fir_bandpass_coeff': coeff.astype(np.float64).reshape(1, -1),
        'fs':      np.int64(args.fs),
        'f_low':   np.int64(args.f_low),
        'f_high':  np.int64(args.f_high),
        'n_taps':  np.int64(args.taps),
    })
    print(f'OK: {out}')
    print(f'  passband {args.f_low:.0f}-{args.f_high:.0f} Hz, {args.taps} taps, '
          f'fs={args.fs}, gd={(args.taps-1)/(2*args.fs)*1000:.1f}ms')


if __name__ == '__main__':
    main()
