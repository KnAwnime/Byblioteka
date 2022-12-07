# Owner(s): ["oncall: distributed"]

import torch
import torch.nn as nn


class UnitModule(nn.Module):
    def __init__(self):
        super().__init__()
        self.l1 = nn.Linear(100, 100)
        self.seq = nn.Sequential(
            nn.ReLU(),
            nn.Linear(100, 100),
            nn.ReLU(),
        )
        self.l2 = nn.Linear(100, 100)

    def forward(self, x):
        return self.l2(self.seq(self.l1(x)))


class CompositeModel(nn.Module):
    def __init__(self):
        super().__init__()
        self.l1 = nn.Linear(100, 100)
        self.u1 = UnitModule()
        self.u2 = UnitModule()
        self.l2 = nn.Linear(100, 100)

    def forward(self, x):
        return self.l2(self.u2(self.u1(self.l1(x))))


class UnitParamModule(nn.Module):
    def __init__(self):
        super().__init__()
        self.l = nn.Linear(100, 100)
        self.seq = nn.Sequential(
            nn.ReLU(),
            nn.Linear(100, 100),
            nn.ReLU(),
        )
        self.p = nn.Parameter(torch.randn(100, 100))

    def forward(self, x):
        return torch.mm(self.seq(self.l(x)), self.p)


class CompositeParamModel(nn.Module):
    def __init__(self):
        super().__init__()
        self.l = nn.Linear(100, 100)
        self.u1 = UnitModule()
        self.u2 = UnitModule()
        self.p = nn.Parameter(torch.randn(100, 100))

    def forward(self, x):
        a = self.u2(self.u1(self.l(x)))
        b = self.p
        return torch.mm(a, b)
