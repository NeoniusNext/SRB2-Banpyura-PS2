#!/usr/bin/env python3
"""Isolated, reversible dec_level score selection; never edits production."""
from pathlib import Path
import argparse
import hashlib
import json

ROOT = Path(__file__).resolve().parents[2]
PATH = Path("ps2/hw/ps2_hw_tex.inc")

OLD = '''			// the sample whose luminance is farthest from the mean (the first of equals); holes are not candidates
			da = a == 255 ? 0 : (la > mean ? la - mean : mean - la);
			db = b == 255 ? 0 : (lb > mean ? lb - mean : mean - lb);
			dc = c == 255 ? 0 : (lc > mean ? lc - mean : mean - lc);
			dd = d == 255 ? 0 : (ld > mean ? ld - mean : mean - ld);
			best = 256; // none yet
			bestd = 0;
			if (a != 255)
			{
				best = a;
				bestd = da;
			}
			if (b != 255 && (best == 256 || db > bestd))
			{
				best = b;
				bestd = db;
			}
			if (c != 255 && (best == 256 || dc > bestd))
			{
				best = c;
				bestd = dc;
			}
			if (d != 255 && (best == 256 || dd > bestd))
			{
				best = d;
				bestd = dd;
			}
'''

NEW = '''			// Unsigned absolute differences: luminance and mean are bytes, so the
			// subtraction's top bit is exactly the sign of their difference.
			da = la - mean;
			db = lb - mean;
			dc = lc - mean;
			dd = ld - mean;
			da = (da ^ (0u - (da >> 31))) + (da >> 31);
			db = (db ^ (0u - (db >> 31))) + (db >> 31);
			dc = (dc ^ (0u - (dc >> 31))) + (dc >> 31);
			dd = (dd ^ (0u - (dd >> 31))) + (dd >> 31);
			// A valid sample has score distance + 1; a hole has score zero.
			// At most one hole reached this point. Strict comparisons retain the
			// first valid sample even when every luminance equals the mean.
			da = a == 255 ? 0 : da + 1;
			db = b == 255 ? 0 : db + 1;
			dc = c == 255 ? 0 : dc + 1;
			dd = d == 255 ? 0 : dd + 1;
			best = a;
			bestd = da;
			if (db > bestd)
			{
				best = b;
				bestd = db;
			}
			if (dc > bestd)
			{
				best = c;
				bestd = dc;
			}
			if (dd > bestd)
			{
				best = d;
				bestd = dd;
			}
'''


def rewrite(text):
    if text.count(OLD) != 1 or NEW in text:
        raise RuntimeError("Exact isolated dec_level baseline anchor changed")
    return text.replace(OLD, NEW, 1)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--base-src", type=Path, default=ROOT / "build/nov15-next12/final/source/src")
    p.add_argument("--out", type=Path, required=True)
    args = p.parse_args()
    if args.out.exists():
        raise RuntimeError("Fresh immutable variant destination required")
    args.out.mkdir(parents=True)
    original = args.base_src / PATH
    target = args.out / PATH
    target.parent.mkdir(parents=True)
    target.write_text(rewrite(original.read_text()))
    (args.out / "variant.json").write_text(json.dumps({
        "baseline_src": str(args.base_src.resolve()),
        "baseline_texture_sha256": sha(original),
        "candidate_texture_sha256": sha(target),
        "only_changed_region": "dec_level stable score selection",
    }, indent=2) + "\n")


if __name__ == "__main__":
    main()
