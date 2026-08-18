"""
CNN 模型定义 — 1D 残差卷积, 场景分类.

提供 CNN / CNNRes 基础类 + 场景分类预定义变体:
  - m5_scene:  轻量场景分类 (~100K params, 推荐)
  - m13_scene: 较大场景分类 (kernel=256, 7 ResBlock)
  - m3_scene:  极简场景分类 (~70K params, 快速实验)

输出维度: K (场景数), 无内置激活函数 (CrossEntropyLoss 内置 softmax).
"""
import torch


# ------------------------------------------------------------------------
# 类: CNN (基础卷积网络)
# ------------------------------------------------------------------------
class CNN(torch.nn.Module):
    def __init__(self, channels, conv_kernels, conv_strides, conv_padding,
                 pool_padding, num_classes=60):
        assert len(conv_kernels) == len(channels) == len(conv_strides) == len(conv_padding)
        super(CNN, self).__init__()

        self.conv_blocks = torch.nn.ModuleList()
        prev_channel = 1
        for i in range(len(channels)):
            block = []
            for j, conv_channel in enumerate(channels[i]):
                block.append(torch.nn.Conv1d(prev_channel, conv_channel,
                                             kernel_size=conv_kernels[i],
                                             stride=conv_strides[i],
                                             padding=conv_padding[i]))
                prev_channel = conv_channel
                block.append(torch.nn.BatchNorm1d(prev_channel))
                block.append(torch.nn.ReLU())
            self.conv_blocks.append(torch.nn.Sequential(*block))

        self.pool_blocks = torch.nn.ModuleList()
        for i in range(len(pool_padding)):
            self.pool_blocks.append(torch.nn.MaxPool1d(4, stride=4, padding=pool_padding[i]))

        self.global_pool = torch.nn.AdaptiveAvgPool1d(1)
        self.linear = torch.nn.Linear(prev_channel, num_classes)

    def forward(self, inwav):
        for i in range(len(self.conv_blocks)):
            inwav = self.conv_blocks[i](inwav)
            if i < len(self.pool_blocks):
                inwav = self.pool_blocks[i](inwav)
        out = self.global_pool(inwav).squeeze(-1)
        out = self.linear(out)
        return out


# ------------------------------------------------------------------------
# 类: ResBlock
# ------------------------------------------------------------------------
class ResBlock(torch.nn.Module):
    """残差块, 使用 1x1 Conv 投影匹配通道数."""

    def __init__(self, prev_channel, channel, conv_kernel, conv_stride, conv_pad):
        super(ResBlock, self).__init__()
        self.res = torch.nn.Sequential(
            torch.nn.Conv1d(in_channels=prev_channel, out_channels=channel,
                            kernel_size=conv_kernel, stride=conv_stride, padding=conv_pad),
            torch.nn.BatchNorm1d(channel),
            torch.nn.ReLU(),
            torch.nn.Conv1d(in_channels=channel, out_channels=channel,
                            kernel_size=conv_kernel, stride=conv_stride, padding=conv_pad),
            torch.nn.BatchNorm1d(channel),
        )
        if prev_channel != channel:
            self.proj = torch.nn.Conv1d(prev_channel, channel, kernel_size=1, bias=False)
        else:
            self.proj = torch.nn.Identity()
        self.relu = torch.nn.ReLU()

    def forward(self, x):
        identity = self.proj(x)
        x = self.res(x)
        x = x + identity
        x = self.relu(x)
        return x


# ------------------------------------------------------------------------
# 类: CNNRes (残差卷积网络)
# ------------------------------------------------------------------------
class CNNRes(torch.nn.Module):
    def __init__(self, channels, conv_kernels, conv_strides, conv_padding,
                 pool_padding, num_classes=60, dropout=0.3):
        assert len(conv_kernels) == len(channels) == len(conv_strides) == len(conv_padding)
        super(CNNRes, self).__init__()

        prev_channel = 1
        self.conv_block = torch.nn.Sequential(
            torch.nn.Conv1d(prev_channel, channels[0][0], conv_kernels[0],
                            conv_strides[0], conv_padding[0]),
            torch.nn.BatchNorm1d(channels[0][0]),
            torch.nn.ReLU(),
            torch.nn.MaxPool1d(4, stride=8, padding=pool_padding[0]),
        )

        prev_channel = channels[0][0]
        self.res_blocks = torch.nn.ModuleList()
        for i in range(1, len(channels)):
            block = []
            for j, conv_channel in enumerate(channels[i]):
                block.append(ResBlock(prev_channel, conv_channel, conv_kernels[i],
                                      conv_strides[i], conv_padding[i]))
                prev_channel = conv_channel
            self.res_blocks.append(torch.nn.Sequential(*block))

        self.pool_blocks = torch.nn.ModuleList()
        for i in range(1, len(pool_padding)):
            self.pool_blocks.append(torch.nn.MaxPool1d(4, stride=4, padding=pool_padding[i]))

        self.global_pool = torch.nn.AdaptiveAvgPool1d(1)
        self.dropout = torch.nn.Dropout(dropout)
        self.linear = torch.nn.Linear(prev_channel, num_classes)

    def forward(self, inwav):
        inwav = self.conv_block(inwav)
        for i in range(len(self.res_blocks)):
            inwav = self.res_blocks[i](inwav)
            if i < len(self.pool_blocks):
                inwav = self.pool_blocks[i](inwav)
        out = self.global_pool(inwav).squeeze(-1)
        out = self.dropout(out)
        out = self.linear(out)
        return out


# ------------------------------------------------------------------------
# 场景分类模型
# ------------------------------------------------------------------------

def m5_scene(K=50, dropout=0.3):
    """轻量场景分类 (~100K params, 推荐)."""
    return CNNRes(
        channels=[[64], [64]*2, [64]*2],
        conv_kernels=[80, 3, 3],
        conv_strides=[4, 1, 1],
        conv_padding=[38, 1, 1],
        pool_padding=[0, 0, 0],
        num_classes=K,
        dropout=dropout,
    )


def m13_scene(K=50, dropout=0.3):
    """较大场景分类 (kernel=256, 7 ResBlock)."""
    return CNNRes(
        channels=[[64], [64]*2, [128]*3, [256]*2],
        conv_kernels=[256, 3, 3, 3],
        conv_strides=[4, 1, 1, 1],
        conv_padding=[126, 1, 1, 1],
        pool_padding=[0, 0, 0, 0],
        num_classes=K,
        dropout=dropout,
    )


def m3_scene(K=50, dropout=0.3):
    """极简场景分类 (~70K params, 快速实验)."""
    return CNNRes(
        channels=[[64], [64]*2],
        conv_kernels=[80, 3],
        conv_strides=[4, 1],
        conv_padding=[38, 1],
        pool_padding=[0, 0],
        num_classes=K,
        dropout=dropout,
    )
