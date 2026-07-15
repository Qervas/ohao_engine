"""Color-aware CNN: LDR FIT image → θ (scene parameters).

Larger capacity + mean-RGB residual on albedo for correct color under multi-light.
"""

from __future__ import annotations

import torch
import torch.nn as nn


class ThetaPriorNet(nn.Module):
    """Conv encoder + color stats → MLP head (~0.5M params at 128×72 / 12D)."""

    def __init__(self, theta_dims: int = 12, in_ch: int = 3):
        super().__init__()
        self.theta_dims = theta_dims
        self.encoder = nn.Sequential(
            nn.Conv2d(in_ch, 48, 5, stride=2, padding=2),
            nn.GELU(),
            nn.Conv2d(48, 96, 3, stride=2, padding=1),
            nn.GELU(),
            nn.Conv2d(96, 160, 3, stride=2, padding=1),
            nn.GELU(),
            nn.Conv2d(160, 192, 3, stride=2, padding=1),
            nn.GELU(),
            nn.AdaptiveAvgPool2d((4, 4)),
        )
        # mean RGB + mean-max + luma + chroma (R-G, B-Y proxy) = 3+1+1+2 = 7
        self.color_dim = 7
        feat = 192 * 4 * 4 + self.color_dim
        self.head = nn.Sequential(
            nn.Linear(feat, 384),
            nn.GELU(),
            nn.Dropout(0.12),
            nn.Linear(384, 192),
            nn.GELU(),
            nn.Dropout(0.08),
            nn.Linear(192, theta_dims),
        )
        nn.init.zeros_(self.head[-1].weight)
        nn.init.constant_(self.head[-1].bias, 0.35)
        if theta_dims >= 3:
            self.albedo_gain = nn.Parameter(torch.ones(3) * 0.90)
            self.albedo_bias = nn.Parameter(torch.zeros(3))
        else:
            self.albedo_gain = None
            self.albedo_bias = None

    @staticmethod
    def color_stats(x: torch.Tensor) -> torch.Tensor:
        """x: N,3,H,W → N,7."""
        mean_rgb = x.mean(dim=(2, 3))
        max_ch = x.amax(dim=1, keepdim=True)
        mean_max = max_ch.mean(dim=(2, 3))
        w = mean_rgb.new_tensor([0.2126, 0.7152, 0.0722]).view(1, 3)
        luma = (mean_rgb * w).sum(dim=1, keepdim=True)
        # Chromaticity-ish: R-G, B - mean
        rg = (mean_rgb[:, 0:1] - mean_rgb[:, 1:2])
        bm = mean_rgb[:, 2:3] - mean_rgb.mean(dim=1, keepdim=True)
        return torch.cat([mean_rgb, mean_max, luma, rg, bm], dim=1)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        stats = self.color_stats(x)
        h = self.encoder(x).flatten(1)
        h = torch.cat([h, stats], dim=1)
        pred = self.head(h)
        if self.albedo_gain is not None and pred.size(1) >= 3:
            mean_rgb = stats[:, :3]
            albedo = pred[:, :3] * 0.30 + mean_rgb * self.albedo_gain + self.albedo_bias
            albedo = albedo.clamp(0.0, 1.0)
            pred = torch.cat([albedo, pred[:, 3:]], dim=1)
        return pred
