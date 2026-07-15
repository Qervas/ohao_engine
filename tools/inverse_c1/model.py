"""Small CNN: LDR FIT image → θ (scene parameters).

Color-aware: global mean RGB is injected so albedo does not collapse to mid-gray
under multi-light ambiguity (the main failure mode of pure physical FD).
"""

from __future__ import annotations

import torch
import torch.nn as nn
import torch.nn.functional as F


class ThetaPriorNet(nn.Module):
    """Compact conv encoder + color stats → MLP head."""

    def __init__(self, theta_dims: int = 12, in_ch: int = 3):
        super().__init__()
        self.theta_dims = theta_dims
        self.encoder = nn.Sequential(
            nn.Conv2d(in_ch, 32, 5, stride=2, padding=2),
            nn.GELU(),
            nn.Conv2d(32, 64, 3, stride=2, padding=1),
            nn.GELU(),
            nn.Conv2d(64, 128, 3, stride=2, padding=1),
            nn.GELU(),
            nn.Conv2d(128, 128, 3, stride=2, padding=1),
            nn.GELU(),
            nn.AdaptiveAvgPool2d((4, 4)),
        )
        # Global color stats: mean RGB + mean of max channel + luminance
        # → 3 + 1 + 1 = 5
        self.color_dim = 5
        feat = 128 * 4 * 4 + self.color_dim
        self.head = nn.Sequential(
            nn.Linear(feat, 256),
            nn.GELU(),
            nn.Dropout(0.12),
            nn.Linear(256, 128),
            nn.GELU(),
            nn.Linear(128, theta_dims),
        )
        nn.init.zeros_(self.head[-1].weight)
        nn.init.constant_(self.head[-1].bias, 0.35)
        # Strong identity-ish path for albedo: start near mean RGB
        if theta_dims >= 3:
            # Will be applied as residual in forward
            self.albedo_gain = nn.Parameter(torch.ones(3) * 0.85)
            self.albedo_bias = nn.Parameter(torch.zeros(3))
        else:
            self.albedo_gain = None
            self.albedo_bias = None

    @staticmethod
    def color_stats(x: torch.Tensor) -> torch.Tensor:
        """x: N,3,H,W → N,5  (mean RGB, mean-max, mean-luma)."""
        mean_rgb = x.mean(dim=(2, 3))  # N,3
        max_ch = x.amax(dim=1, keepdim=True)  # N,1,H,W
        mean_max = max_ch.mean(dim=(2, 3))  # N,1
        # Rec.709 luma
        w = mean_rgb.new_tensor([0.2126, 0.7152, 0.0722]).view(1, 3)
        luma = (mean_rgb * w).sum(dim=1, keepdim=True)
        return torch.cat([mean_rgb, mean_max, luma], dim=1)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        # x: N,3,H,W in [0,1]
        stats = self.color_stats(x)
        h = self.encoder(x).flatten(1)
        h = torch.cat([h, stats], dim=1)
        pred = self.head(h)
        # Residual albedo from image mean color (fixes "totally wrong color" basin)
        if self.albedo_gain is not None and pred.size(1) >= 3:
            mean_rgb = stats[:, :3]
            albedo = pred[:, :3] * 0.35 + mean_rgb * self.albedo_gain + self.albedo_bias
            albedo = albedo.clamp(0.0, 1.0)
            pred = torch.cat([albedo, pred[:, 3:]], dim=1)
        return pred
