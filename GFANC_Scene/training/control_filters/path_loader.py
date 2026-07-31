"""
声学路径加载工具.

load_multichannel_paths_with_variable_names(): 从 .npy 文件加载主/次级路径,
  自动适配 SISO / MIMO / 多变量名等格式.
"""
# path_loader.py
import os 
import pandas as pd
import numpy as np
import matplotlib
import matplotlib.pyplot as plt
from scipy import signal, misc
import scipy.io as sio


def loading_paths(folder="Duct_path", Pri_path_file_name = "Primary Path.csv", Sec_path_file_name="Secondary Path.csv"):
    """
    从 CSV 文件加载单通道初级路径和次级路径。
    返回两个一维 numpy 数组，分别表示初级路径和次级路径的冲激响应。
    """
    Primay_path_file, Secondary_path_file = os.path.join(folder,Pri_path_file_name), os.path.join(folder,Sec_path_file_name)
    Pri_dfs, Secon_dfs   = pd.read_csv(Primay_path_file), pd.read_csv(Secondary_path_file)
    Pri_path, Secon_path = np.array(Pri_dfs['Amplitude - Plot 0']), np.array(Secon_dfs['Amplitude - Plot 0'])
    return Pri_path, Secon_path

# Pz1.mat 文件存储初级路径（Primary Path）的数据，Sz.mat 文件存储次级路径（Secondary Path）的数据。
def loading_paths_from_MAT(folder, subfolder, Pri_path_file_name, Sec_path_file_name):
    """
    从 .mat 文件加载单通道初级路径和次级路径。
    .mat 文件中应包含变量 'Pz1'（初级路径）和 'S'（次级路径），均为一维向量。
    返回两个一维 numpy 数组。
    """
    Primay_path_file, Secondary_path_file = os.path.join(folder, subfolder, Pri_path_file_name), os.path.join(folder,subfolder, Sec_path_file_name)
    Pri_dfs, Secon_dfs = sio.loadmat(Primay_path_file), sio.loadmat(Secondary_path_file)
    Pri_path, Secon_path = Pri_dfs['Pz1'].squeeze(), Secon_dfs['S'].squeeze()
    return Pri_path, Secon_path

#-----------------------------------------------------------------------------------
# 以下函数为加载多通道初级路径和次级路径
#-----------------------------------------------------------------------------------
def load_multichannel_paths_from_MAT_or_npy(folder, subfolder, Pri_path_file_name, Sec_path_file_name):
    """
    从 .mat 或 .npy 文件加载多通道初级路径和次级路径。
    
    .mat 文件中的变量约定：
    - 初级路径：变量名为 'Pz_multich'，形状为 (E, R, L) 的三维数组，
      其中 E 为误差传感器数量，R 为参考传感器数量，L 为路径冲激响应长度。
    - 次级路径：变量名为 'S_multich'，形状为 (E, S, L) 的三维数组，
      其中 S 为次级声源数量。
    
    返回：
    - Pri_path: 形状 (E, R, L) 的 numpy 数组
    - Sec_path: 形状 (E, S, L) 的 numpy 数组
    """
    Primary_path_file = os.path.join(folder, subfolder, Pri_path_file_name)
    Secondary_path_file = os.path.join(folder, subfolder, Sec_path_file_name)
    
    if Sec_path_file_name.lower().endswith('.npy'):
        Pri_path = np.load(Primary_path_file)
        Sec_path = np.load(Secondary_path_file)
        print(f"次级路径从 .npy 文件加载: {Secondary_path_file}")
    else:
        Pri_data = sio.loadmat(Primary_path_file)
        Sec_data = sio.loadmat(Secondary_path_file)
        Pri_path = Pri_data['Pz_multich']   # 形状 (E, R, L)     
        Sec_path = Sec_data['S_multich']   # 形状 (E, S, L)
        print(f"次级路径从 .mat 文件加载: {Secondary_path_file}")
    
    # 确保是三维数组，若原存储为二维则可能需调整，这里要求数据本身即为三维
    if Pri_path.ndim != 3 or Sec_path.ndim != 3:
        raise ValueError("多通道路径数据必须是三维数组，请检查 .mat 文件内容。")
    
    print("已加载多通道路径：")
    print(f"初级路径形状 (误差通道数 E, 参考通道数 R, 路径长度 L) = {Pri_path.shape}")
    print(f"次级路径形状 (误差通道数 E, 次级源数 S, 路径长度 L) = {Sec_path.shape}")
    
    return Pri_path, Sec_path

# 若需兼容多种命名方式，可增加可选参数指定变量名
def load_multichannel_paths_with_variable_names(folder, subfolder, 
                                           Pri_path_file_name, Sec_path_file_name,
                                           Pri_var_name='Pz_multich', Sec_var_name='S_multich'):
    """
    加载多通道路径（可指定文件中的变量名）。
    
    参数：
    - folder, subfolder: 文件夹路径
    - Pri_path_file_name, Sec_path_file_name: 文件名
    - Pri_var_name: 初级路径在文件中的变量名，默认为 'Pz_multich'
    - Sec_var_name: 次级路径在文件中的变量名，默认为 'S_multich'
    
    返回：
    - Pri_path: 形状 (E, R, L) 的 numpy 数组
    - Sec_path: 形状 (E, S, L) 的 numpy 数组
    """
    Primary_path_file = os.path.join(folder, subfolder, Pri_path_file_name)
    Secondary_path_file = os.path.join(folder, subfolder, Sec_path_file_name)
    
    if Sec_path_file_name.lower().endswith('.npy'):
        Pri_path = np.load(Primary_path_file)
        Sec_path = np.load(Secondary_path_file)
        print(f"次级路径从 .npy 文件加载: {Secondary_path_file}")
    else:
        Pri_data = sio.loadmat(Primary_path_file)
        Sec_data = sio.loadmat(Secondary_path_file)
        Pri_path = Pri_data[Pri_var_name]   # 形状 (E, R, L)     
        Sec_path = Sec_data[Sec_var_name]   # 形状 (E, S, L)
        print(f"次级路径从 .mat 文件加载: {Secondary_path_file}")

    
    if Pri_path.ndim != 3 or Sec_path.ndim != 3:
        raise ValueError("多通道路径数据必须是三维数组，请检查 .mat 文件内容。")
    
    print("已加载多通道路径：")
    print(f"初级路径形状 (误差通道数 E, 参考通道数 R, 路径长度 L) = {Pri_path.shape}")
    print(f"次级路径形状 (误差通道数 E, 次级源数 S, 路径长度 L) = {Sec_path.shape}")
    
    return Pri_path, Sec_path
