"""Stages Somnabulate's footstep sounds into git-ignored assets/local/audio/footsteps, for local listening
only (Frank's ruling, sub-project 6 step 4): they are never committed (the repo is MIT and public), and
the window host prefers that folder over assets/audio/footsteps (runtime/footsteps.hpp).

    python tools/audio/stage_somnabulate_footsteps.py [SRC_DIR]

SRC_DIR is Somnabulate's hub/audio/events folder (read only: nothing there is written). Its
move.footstep.<surface>.walk_NN.wav files are copied as <ours>_<n>.wav: wood as wood, tile as
concrete (the blockout's floor). Surfaces with no Somnabulate sound (metal) keep OHAO's own.
"""
import os
import shutil
import sys

SRC = r"C:\Users\djmax\Desktop\games\somnabulate\assets\hub\audio\events"
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "assets", "local", "audio", "footsteps")
MAP = {"wood": "wood", "tile": "concrete"}   # Somnabulate surface -> ours


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else SRC
    if not os.path.isdir(src):
        print(f"no Somnabulate audio at {src}: nothing staged")
        return 1
    os.makedirs(OUT, exist_ok=True)
    staged = 0
    for theirs, ours in MAP.items():
        for n in range(1, 100):
            path = os.path.join(src, f"move.footstep.{theirs}.walk_{n:02d}.wav")
            if not os.path.exists(path):
                break
            shutil.copyfile(path, os.path.join(OUT, f"{ours}_{n}.wav"))
            staged += 1
    print(f"staged {staged} footsteps into {OUT} (git-ignored; never commit them)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
