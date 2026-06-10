#!/usr/bin/env python3
"""
Plot selected channels directly from a Calvision EUDAQ .raw file.

Edit the CONFIG block below and run:
    python3 plot_raw_channels.py

It produces:
- the first N DRS waveforms for each selected DRS board/group/channel
- the HG and LG spectra for each selected FERS board/channel

Implementation note:
- the Python script compiles a tiny C++ helper on demand
- that helper walks the .raw file directly with the EUDAQ reader and uses the
  same Calvision unpackers as the monitor code
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Dict, Iterable, List, Sequence, Tuple


if "MPLCONFIGDIR" not in os.environ:
    os.environ["MPLCONFIGDIR"] = tempfile.mkdtemp(prefix="mplcfg_", dir="/tmp")

if not os.environ.get("DISPLAY"):
    import matplotlib

    matplotlib.use("Agg")

import matplotlib.pyplot as plt
import numpy as np


REPO_ROOT = Path(__file__).resolve().parents[3]
OFFLINE_DIR = Path(__file__).resolve().parent
HELPER_SRC = OFFLINE_DIR / "raw_dump_helper.cpp"
HELPER_BIN = OFFLINE_DIR / "raw_dump_helper"
ROOTSYS_CANDIDATE = Path("/home/softwares/root")


# ---------------------------------------------------------------------------
# CONFIG: edit these values instead of typing them at runtime
# ---------------------------------------------------------------------------

# Use an explicit .raw file path, or leave as None to auto-pick the newest
# /hdd/euDAQ_staging/run*/run*.raw file.
RAW_FILE: str | None = "/hdd/euDAQ_staging/run063/run063.raw"

# Output directory for PNG plots.
# If relative, it is created under the directory that contains the .raw file.
# If absolute, it is used as-is.
OUTPUT_DIR = "plots"

# First N DRS events to save per selected DRS channel.
# One event = one PNG.
DRS_EVENT_LIMIT = 20

# FERS spectrum histogram bin size, in ADC counts.
FERS_BIN_SIZE = 1

# DRS selection.
DRS_BOARD = 0
DRS_GROUP = 0
DRS_CHANNELS: Sequence[int] = [0, 2, 4, 6]

# FERS selection.
FERS_BOARD = 0
FERS_CHANNELS: Sequence[int] = [0,1,2,3]


def prepare_runtime_env() -> Dict[str, str]:
    env = os.environ.copy()
    ld_parts: List[str] = []

    if ROOTSYS_CANDIDATE.exists():
        env["ROOTSYS"] = str(ROOTSYS_CANDIDATE)
        env["PATH"] = f"{ROOTSYS_CANDIDATE / 'bin'}:{env.get('PATH', '')}"
        ld_parts.append(str(ROOTSYS_CANDIDATE / "lib"))

    ld_parts.append(str(Path.home() / "local_install" / "lib"))
    ld_parts.append(str(REPO_ROOT / "lib"))
    if env.get("LD_LIBRARY_PATH"):
        ld_parts.append(env["LD_LIBRARY_PATH"])
    env["LD_LIBRARY_PATH"] = ":".join(part for part in ld_parts if part)
    return env


def normalize_channel_list(channels: Sequence[int] | int, label: str) -> List[int]:
    if isinstance(channels, int):
        normalized = [channels]
    else:
        normalized = [int(ch) for ch in channels]

    if not normalized:
        raise ValueError(f"{label} is empty.")

    deduped: List[int] = []
    for channel in normalized:
        if channel not in deduped:
            deduped.append(channel)
    return deduped


def find_latest_raw_file() -> Path:
    staging_root = Path("/hdd/euDAQ_staging")
    candidates = sorted(staging_root.glob("run*/run*.raw"), key=lambda path: path.stat().st_mtime)
    if not candidates:
        raise FileNotFoundError(
            "Could not find any .raw files under /hdd/euDAQ_staging/run*/run*.raw. "
            "Set RAW_FILE at the top of the script."
        )
    return candidates[-1].resolve()


def resolve_raw_file() -> Path:
    if RAW_FILE is None:
        return find_latest_raw_file()

    raw_path = Path(RAW_FILE).expanduser().resolve()
    if not raw_path.exists():
        raise FileNotFoundError(f"Configured RAW_FILE does not exist: {raw_path}")
    if not raw_path.is_file():
        raise FileNotFoundError(f"Configured RAW_FILE is not a file: {raw_path}")
    if raw_path.suffix.lower() != ".raw":
        raise ValueError(f"Configured RAW_FILE is not a .raw file: {raw_path}")
    return raw_path


def resolve_output_dir(raw_path: Path) -> Path:
    output_dir = Path(OUTPUT_DIR).expanduser()
    if output_dir.is_absolute():
        return output_dir.resolve()
    return (raw_path.parent / output_dir).resolve()


def run_command(cmd: List[str], env: Dict[str, str], cwd: Path | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(
        cmd,
        cwd=str(cwd or REPO_ROOT),
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )


def ensure_build_tools() -> str:
    compiler = shutil.which("g++") or shutil.which("c++")
    if not compiler:
        raise FileNotFoundError("Could not find g++/c++ in PATH.")
    if not HELPER_SRC.exists():
        raise FileNotFoundError(f"Missing helper source: {HELPER_SRC}")
    return compiler


def helper_needs_rebuild() -> bool:
    if not HELPER_BIN.exists():
        return True

    dependencies = [
        HELPER_SRC,
        REPO_ROOT / "user" / "calvision" / "module" / "src" / "FERS_EUDAQ.cc",
        REPO_ROOT / "user" / "calvision" / "module" / "src" / "DRS_EUDAQ.cc",
        REPO_ROOT / "main" / "lib" / "core" / "include" / "eudaq" / "Event.hh",
        REPO_ROOT / "lib" / "libeudaq_core.so",
    ]
    helper_mtime = HELPER_BIN.stat().st_mtime
    return any(dep.exists() and dep.stat().st_mtime > helper_mtime for dep in dependencies)


def build_helper(compiler: str, env: Dict[str, str]) -> None:
    include_dirs = [
        REPO_ROOT / "main" / "lib" / "core" / "include",
        REPO_ROOT / "main" / "lib" / "core" / "include" / "eudaq",
        REPO_ROOT / "user" / "calvision" / "module" / "src",
        REPO_ROOT / "user" / "calvision" / "hardware" / "caenferslib-1.3.0" / "include",
        REPO_ROOT / "user" / "calvision" / "hardware" / "compat" / "include",
    ]

    cmd = [
        compiler,
        "-std=c++17",
        "-O2",
        "-Wall",
        "-Wextra",
        "-o",
        str(HELPER_BIN),
        str(HELPER_SRC),
        str(REPO_ROOT / "user" / "calvision" / "module" / "src" / "FERS_EUDAQ.cc"),
        str(REPO_ROOT / "user" / "calvision" / "module" / "src" / "DRS_EUDAQ.cc"),
        "-L",
        str(REPO_ROOT / "lib"),
        "-leudaq_core",
        "-Wl,-rpath," + str(REPO_ROOT / "lib"),
    ]
    for include_dir in include_dirs:
        cmd.extend(["-I", str(include_dir)])

    result = run_command(cmd, env=env)
    if result.returncode != 0:
        raise RuntimeError(f"Failed to build raw_dump_helper:\n{result.stdout}")
    HELPER_BIN.chmod(0o755)


def run_helper(
    raw_path: Path,
    drs_dump: Path,
    fers_dump: Path,
    drs_board: int,
    drs_group: int,
    drs_channel: int,
    fers_board: int,
    fers_channel: int,
    env: Dict[str, str],
    drs_limit: int = 20,
) -> None:
    cmd = [
        str(HELPER_BIN),
        str(raw_path),
        str(drs_dump),
        str(fers_dump),
        str(drs_board),
        str(drs_group),
        str(drs_channel),
        str(fers_board),
        str(fers_channel),
        str(drs_limit),
    ]
    result = run_command(cmd, env=env)
    if result.returncode != 0:
        raise RuntimeError(f"raw_dump_helper failed:\n{result.stdout}")
    if not drs_dump.exists():
        raise RuntimeError("DRS dump file was not created.")
    if not fers_dump.exists():
        raise RuntimeError("FERS dump file was not created.")


def load_drs_waveforms(drs_dump_path: Path) -> Tuple[np.ndarray, np.ndarray]:
    rows: List[np.ndarray] = []
    event_ids: List[int] = []
    with drs_dump_path.open() as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            parts = line.split("\t")
            event_ids.append(int(parts[0]))
            sample_count = int(parts[2])
            samples = np.array([float(value) for value in parts[3 : 3 + sample_count]], dtype=float)
            rows.append(samples)

    if not rows:
        raise RuntimeError("No DRS waveforms were dumped for the selected channel.")
    return np.array(event_ids, dtype=int), np.vstack(rows)


def load_fers_channel(fers_dump_path: Path) -> Tuple[np.ndarray, np.ndarray]:
    hg_values: List[float] = []
    lg_values: List[float] = []
    with fers_dump_path.open() as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            parts = line.split("\t")
            hg_values.append(float(parts[2]))
            lg_values.append(float(parts[3]))

    if not hg_values:
        raise RuntimeError("No FERS events were dumped for the selected channel.")
    return np.array(hg_values, dtype=float), np.array(lg_values, dtype=float)


def plot_single_drs_waveform(event_id: int, waveform: np.ndarray, output_path: Path, title_suffix: str) -> None:
    fig, ax = plt.subplots(figsize=(12, 7))
    x = np.arange(waveform.shape[0], dtype=int)
    ax.plot(x, waveform, linewidth=1.2, color="#1f77b4")

    ax.set_title(f"DRS Waveform: {title_suffix}, event {event_id}")
    ax.set_xlabel("Sample")
    ax.set_ylabel("ADC")
    ax.grid(True, alpha=0.25)
    fig.tight_layout()
    fig.savefig(output_path, dpi=160)
    plt.close(fig)


def plot_fers_spectra(hg_values: np.ndarray, lg_values: np.ndarray, output_path: Path, title_suffix: str) -> None:
    def integer_bin_edges(values: np.ndarray, bin_size: int) -> np.ndarray:
        min_edge = int(np.floor(np.min(values))) - 0.5
        max_edge = int(np.ceil(np.max(values))) + 0.5
        edges = np.arange(min_edge, max_edge + bin_size, bin_size, dtype=float)
        if edges.size < 2:
            edges = np.array([min_edge, min_edge + bin_size], dtype=float)
        return edges

    bins_hg = integer_bin_edges(hg_values, FERS_BIN_SIZE)
    bins_lg = integer_bin_edges(lg_values, FERS_BIN_SIZE)

    fig, axes = plt.subplots(2, 1, figsize=(11, 8), sharex=False)

    axes[0].hist(hg_values, bins=bins_hg, histtype="stepfilled", alpha=0.75, color="#1f77b4")
    axes[0].set_title(f"FERS HG Spectrum: {title_suffix}")
    axes[0].set_xlabel("HG ADC")
    axes[0].set_ylabel("Counts")
    axes[0].grid(True, alpha=0.25)

    axes[1].hist(lg_values, bins=bins_lg, histtype="stepfilled", alpha=0.75, color="#ff7f0e")
    axes[1].set_title(f"FERS LG Spectrum: {title_suffix}")
    axes[1].set_xlabel("LG ADC")
    axes[1].set_ylabel("Counts")
    axes[1].grid(True, alpha=0.25)

    fig.tight_layout()
    fig.savefig(output_path, dpi=160)
    plt.close(fig)


def maybe_show_plots(paths: List[Path]) -> None:
    backend_name = plt.get_backend().lower()
    if not os.environ.get("DISPLAY") or "agg" in backend_name:
        return

    fig, axes = plt.subplots(len(paths), 1, figsize=(12, 5 * len(paths)))
    if len(paths) == 1:
        axes = [axes]
    for ax, path in zip(axes, paths):
        image = plt.imread(path)
        ax.imshow(image)
        ax.set_title(path.name)
        ax.axis("off")
    plt.tight_layout()
    plt.show()


def main() -> int:
    compiler = ensure_build_tools()
    env = prepare_runtime_env()
    raw_path = resolve_raw_file()
    output_dir = resolve_output_dir(raw_path)
    try:
        output_dir.mkdir(parents=True, exist_ok=True)
    except OSError as exc:
        raise OSError(
            f"Could not create output directory under the raw file location: {output_dir}. "
            "If that directory is not writable, set OUTPUT_DIR to a different absolute path."
        ) from exc
    drs_channels = normalize_channel_list(DRS_CHANNELS, "DRS_CHANNELS")
    fers_channels = normalize_channel_list(FERS_CHANNELS, "FERS_CHANNELS")

    if helper_needs_rebuild():
        print("\nBuilding raw_dump_helper...")
        build_helper(compiler, env)

    stem = raw_path.stem
    generated_plots: List[Path] = []

    print("\nUsing configuration:")
    print(f"  RAW_FILE       : {raw_path}")
    print(f"  OUTPUT_DIR     : {output_dir}")
    print(f"  DRS board/group: {DRS_BOARD}/{DRS_GROUP}")
    print(f"  DRS channels   : {drs_channels}")
    print(f"  FERS board     : {FERS_BOARD}")
    print(f"  FERS channels  : {fers_channels}")

    with tempfile.TemporaryDirectory(prefix="calvision_raw_", dir="/tmp") as tmpdir:
        tmpdir_path = Path(tmpdir)
        print("\nDecoding and plotting DRS channels...")
        for drs_channel in drs_channels:
            drs_dump = tmpdir_path / f"drs_ch{drs_channel}.tsv"
            fers_dummy = tmpdir_path / f"fers_dummy_for_drs{drs_channel}.tsv"
            run_helper(
                raw_path=raw_path,
                drs_dump=drs_dump,
                fers_dump=fers_dummy,
                drs_board=DRS_BOARD,
                drs_group=DRS_GROUP,
                drs_channel=drs_channel,
                fers_board=FERS_BOARD,
                fers_channel=-1,
                env=env,
                drs_limit=DRS_EVENT_LIMIT,
            )
            drs_event_ids, drs_waveforms = load_drs_waveforms(drs_dump)
            drs_channel_dir = output_dir / "DRS" / f"board{DRS_BOARD}_group{DRS_GROUP}_ch{drs_channel}"
            drs_channel_dir.mkdir(parents=True, exist_ok=True)
            for event_id, waveform in zip(drs_event_ids, drs_waveforms):
                drs_png = drs_channel_dir / f"event_{event_id:06d}.png"
                plot_single_drs_waveform(
                    int(event_id),
                    waveform,
                    drs_png,
                    f"board {DRS_BOARD}, group {DRS_GROUP}, channel {drs_channel}",
                )
                generated_plots.append(drs_png)

        print("Decoding and plotting FERS channels...")
        for fers_channel in fers_channels:
            drs_dummy = tmpdir_path / f"drs_dummy_for_fers{fers_channel}.tsv"
            fers_dump = tmpdir_path / f"fers_ch{fers_channel}.tsv"
            run_helper(
                raw_path=raw_path,
                drs_dump=drs_dummy,
                fers_dump=fers_dump,
                drs_board=DRS_BOARD,
                drs_group=DRS_GROUP,
                drs_channel=-1,
                fers_board=FERS_BOARD,
                fers_channel=fers_channel,
                env=env,
                drs_limit=DRS_EVENT_LIMIT,
            )
            hg_values, lg_values = load_fers_channel(fers_dump)
            fers_channel_dir = output_dir / "FERS" / f"board{FERS_BOARD}_ch{fers_channel}"
            fers_channel_dir.mkdir(parents=True, exist_ok=True)
            fers_png = fers_channel_dir / "spectrum_hg_lg.png"
            plot_fers_spectra(
                hg_values,
                lg_values,
                fers_png,
                f"board {FERS_BOARD}, channel {fers_channel}",
            )
            generated_plots.append(fers_png)

    print("\nDone.")
    for plot_path in generated_plots:
        print(f"  {plot_path}")

    maybe_show_plots(generated_plots)
    return 0


if __name__ == "__main__":
    sys.exit(main())
