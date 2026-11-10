"""Reading the linear-radiance PFM images `ohao run --hdr` writes (the renderer harnesses' shared reader)."""
import numpy as np


def read_pfm(path):
    with open(path, "rb") as f:
        if f.readline().strip() != b"PF":
            raise ValueError("%s is not an RGB PFM" % path)
        w, h = map(int, f.readline().split())
        scale = float(f.readline())
        data = np.frombuffer(f.read(), dtype="<f4" if scale < 0 else ">f4")
    return data.reshape(h, w, 3)[::-1].astype(np.float64)
