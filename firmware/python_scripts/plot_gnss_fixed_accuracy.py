"""
plot_gnss_fixed_accuracy.py — accuracy relative to a fixed ground-truth position

Detects the cold start (first fixType >0 → 0 drop), drops all pre-cold-start data,
and plots only the post-reacquisition convergence.  t=0 on the time axis is the
cold-start moment; TTFF is where data begins.

Layout:
  Top-left    Horizontal error to ground truth + hAcc self-estimate + rolling min (log)
  Top-right   N/E scatter coloured by fix type with CEP50/CEP95 circles
  Bottom-left Height error vs vAcc self-estimate (symlog)
  Bottom-right ΔNorth / ΔEast offset over time (symlog)

Set GROUND_TRUTH_* before running.

Usage:
    python plot_gnss_fixed_accuracy.py                     # interactive file picker
    python plot_gnss_fixed_accuracy.py f_a.jsonl f_b.jsonl # explicit files
"""

import os
import sys
import json
import math
import statistics
import datetime
import requests
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import matplotlib.gridspec as gridspec
import matplotlib.patches as patches
from matplotlib.lines import Line2D

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
LOG_DIR    = os.path.join(SCRIPT_DIR, 'log_data')

# ── Predefined batch ──────────────────────────────────────────────────

PREDEFINED_BATCH_SETS = [
    # Example: ["file_a.jsonl", "file_b.jsonl", "file_c.jsonl"],
    # Add your predefined sets here
]

# ── Ground truth input mode ───────────────────────────────────────────
# Set USE_LV95 = True to enter coordinates in the Swiss LV95 / LN02 system.
# Set USE_LV95 = False to enter WGS-84 directly.
USE_LV95 = False

# LV95 (CH1903+) input — only used when USE_LV95 = True.
# E = Easting [m], N = Northing [m], H_LN02 = orthometric height in LN02 [m].
LV95_E      = 2_674_851.920
LV95_N      = 1_243_483.850
LV95_H_LN02 =       590.116 + 0.123 # added distance from reference point to antenna phase center (123 mm up)

# WGS-84 fallback — used when USE_LV95 = False.
# hMSL = height above MSL (EGM96) in metres.
GROUND_TRUTH_LAT  =  47.3016092
GROUND_TRUTH_LON  =   8.4462493
GROUND_TRUTH_HMSL =     514.572

# Reference point from 11/06/2026 survey with PointPerfect Flex.
# lon": 84462493, "lat": 473016092, "height": 561902, "hMSL": 514572,

# ── Display toggles ───────────────────────────────────────────────────
# Set to False to hide the on-plot CEP/median stats text boxes.
SHOW_STATS_LABELS = False
# Set to False to hide median/CEP50 reference lines and circles.
SHOW_MEAN_LINES = False

# carrSoln display config  (bits 7-6 of the flags byte)
_CARR_COLOR = {2: '#2ca02c', 1: '#ff7f0e', 0: '#aaaaaa'}
_CARR_LABEL = {2: 'RTK Fixed', 1: 'Float', 0: 'No RTK'}
_TR_COLOR   = {2: '#2ca02c', 1: '#ff7f0e', 0: '#888888'}
_TR_LABEL   = {2: '→ RTK Fixed', 1: '→ Float', 0: '→ No RTK'}


# ── LV95 → WGS-84 conversion ─────────────────────────────────────────

def lv95_to_etrs89(easting, northing, height_ln02):
    """
    Step 1: Convert LV95 (Local) to ETRS89 (European Plate-Fixed)
    """
    # 1a. Convert LN02 physical height to Bessel ellipsoidal height
    url_height = "https://geodesy.geo.admin.ch/reframe/ln02tobessel"
    params_height = {
        'easting': easting,
        'northing': northing,
        'altitude': height_ln02,
        'format': 'json'
    }
    
    response_height = requests.get(url_height, params=params_height)
    response_height.raise_for_status()
    bessel_height = float(response_height.json()['altitude'])
    
    # 1b. Convert LV95 coordinates + Bessel height to WGS84/ETRS89
    url_geom = "https://geodesy.geo.admin.ch/reframe/lv95towgs84"
    params_geom = {
        'easting': easting,
        'northing': northing,
        'altitude': bessel_height, # Pass the corrected Bessel height here
        'format': 'json'
    }
    
    response_geom = requests.get(url_geom, params=params_geom)
    response_geom.raise_for_status()
    data = response_geom.json()
    
    lon_etrs89 = float(data['easting'])
    lat_etrs89 = float(data['northing'])
    h_ellps = float(data['altitude'])
    
    return lon_etrs89, lat_etrs89, h_ellps

def get_decimal_year(date: datetime.date):
    """Helper function to convert a standard date into a decimal year epoch."""
    start_of_year = datetime.date(date.year, 1, 1)
    end_of_year = datetime.date(date.year + 1, 1, 1)
    year_duration = end_of_year - start_of_year
    elapsed = date - start_of_year
    return date.year + (elapsed.days / year_duration.days)

def etrs89_to_itrf2020(lon, lat, h_ellps, target_epoch):
    """
    Step 2: Convert ETRS89 (Static) to ITRF2020 (Dynamic)
    Uses pyproj to apply the 14-parameter time-dependent Helmert transformation.
    """
    from pyproj import CRS, Transformer
    # EPSG:4937 = ETRS89 (3D Geographic: Lon, Lat, Ellipsoidal Height)
    # EPSG:9989 = ITRF2020 (3D Geographic: Lon, Lat, Ellipsoidal Height)
    crs_static = CRS.from_epsg(4937)
    crs_dynamic = CRS.from_epsg(9989)
    
    # Create the transformation pipeline
    # always_xy=True ensures axis order is strictly Longitude, Latitude
    transformer = Transformer.from_crs(crs_static, crs_dynamic, always_xy=True)
    
    # Perform the transformation. The 'tt' parameter is the observation epoch (time),
    # which calculates the exact tectonic drift up to that decimal year.
    # pyproj returns 4 values (lon, lat, h, t) when 'tt' is provided.
    res = transformer.transform(
        xx=lon, 
        yy=lat, 
        zz=h_ellps, 
        tt=target_epoch
    )
    
    lon_itrf, lat_itrf, h_itrf = res[0], res[1], res[2]
    return lon_itrf, lat_itrf, h_itrf


def lv95_to_wgs84(e, n, h_ln02, measurement_date):
    """
    Convert LV95 E/N + LN02 height to ITRF2020 (WGS84 compatible) lat/lon + ellipsoidal height.
    Returns (lat_deg, lon_deg, h_ellipsoidal_m, method_str).
    """
    lon_e, lat_e, h_e = lv95_to_etrs89(e, n, h_ln02)
    measurement_epoch = get_decimal_year(measurement_date)
    lon_i, lat_i, h_i = etrs89_to_itrf2020(lon_e, lat_e, h_e, measurement_epoch)
    return lat_i, lon_i, h_i, 'Swisstopo API + pyproj ITRF2020'


# ── Data loading ──────────────────────────────────────────────────────

def get_carr_soln(flags):
    return (flags >> 6) & 0x3


def load_gnss(filepath):
    """Load ALL GNSS_PVT records including pre-fix ones (lat/lon may be 0)."""
    points = []
    with open(filepath, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                d = json.loads(line)
                if d.get('sensor') != 'GNSS_PVT':
                    continue
                flags = d.get('flags', 0)
                points.append({
                    'lat':        d.get('lat', 0) / 1e7,
                    'lon':        d.get('lon', 0) / 1e7,
                    'hmsl':       d.get('hMSL', 0) / 1000.0,      # mm → m  (EGM96 MSL)
                    'height_ell': d.get('height', 0) / 1000.0,    # mm → m  (WGS84 ellipsoidal)
                    'hacc':       d.get('hAcc', 0) / 1000.0,      # mm → m
                    'vacc':       d.get('vAcc', 0) / 1000.0,      # mm → m
                    'epoch_us':  int(d.get('epoch_us', 0)),
                    'year':      int(d.get('year', 2024)),
                    'month':     int(d.get('month', 1)),
                    'day':       int(d.get('day', 1)),
                    'flags':     flags,
                    'carr_soln': get_carr_soln(flags),
                    'fixType':   d.get('fixType', 0),
                    'numSV':     d.get('numSV', 0),
                    'correction': d.get('correction_source', ''),
                })
            except (json.JSONDecodeError, ValueError):
                continue
    return points


# ── Geometry ──────────────────────────────────────────────────────────

def north_east_offset_m(lat_ref, lon_ref, lat, lon):
    dn = (lat - lat_ref) * 111_111.0
    de = (lon - lon_ref) * 111_111.0 * math.cos(math.radians(lat_ref))
    return dn, de


def percentile(data, pct):
    s = sorted(data)
    k = (len(s) - 1) * pct / 100.0
    lo, hi = int(k), min(int(k) + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


# ── File picker ───────────────────────────────────────────────────────

def pick_files():
    files = sorted(f for f in os.listdir(LOG_DIR) if f.endswith('.jsonl'))
    if not files:
        print(f"No .jsonl files found in {LOG_DIR}")
        return []
    print("\nAvailable log files:")
    for i, f in enumerate(files):
        print(f"  [{i}] {f}")
    print("\n  [all]   Run all files in directory")
    print("  [batch] Run predefined batch of files")
    while True:
        choice = input(f"Select files separated by space (e.g. '0 1 2'), 'all', or 'batch': ").strip()
        if not choice:
            continue
        if choice.lower() == 'batch':
            return ['batch']
        if choice.lower() == 'all':
            return [os.path.join(LOG_DIR, f) for f in files]
        
        selected = []
        invalid = False
        for c in choice.split():
            if c.isdigit() and 0 <= int(c) < len(files):
                selected.append(os.path.join(LOG_DIR, files[int(c)]))
            else:
                invalid = True
                break
                
        if not invalid and selected:
            return selected
        print("  Invalid choice, try again.")


# ── Vertical-line helper ──────────────────────────────────────────────

def draw_vlines(axes, x, color, linestyle='-', linewidth=0.9, alpha=0.65):
    for ax in axes:
        ax.axvline(x, color=color, linestyle=linestyle, linewidth=linewidth,
                   alpha=alpha, zorder=4)


# ── Main ──────────────────────────────────────────────────────────────

def get_friendly_name(lbl):
    l = lbl.lower()
    if 'has' in l: return 'Galileo HAS'
    if 'spartn' in l or 'spartan' in l: return 'PointPerfect Flex'
    if 'nocorr' in l: return 'No Correction'
    return lbl


def process_datasets(paths, is_batch=False):
    labels = [os.path.splitext(os.path.basename(p))[0] for p in paths]
    print(f"\n{'='*50}\nProcessing: {' vs '.join(labels)}\n{'='*50}")

    datasets = []
    for filepath, label in zip(paths, labels):
        all_points = load_gnss(filepath)
        if not all_points:
            print(f"[{label}] No GNSS_PVT records found.")
            continue

        # ── Cold-start detection ──────────────────────────────────────────
        cold_start_idx = None
        for i in range(1, len(all_points)):
            if all_points[i - 1]['fixType'] > 0 and all_points[i]['fixType'] == 0:
                cold_start_idx = i - 1
                break

        if cold_start_idx is not None:
            cold_start_us = all_points[cold_start_idx]['epoch_us']
            first_fix_idx = next(
                (i for i in range(cold_start_idx + 1, len(all_points))
                 if all_points[i]['fixType'] > 0),
                None,
            )
        else:
            cold_start_us = all_points[0]['epoch_us']
            first_fix_idx = next(
                (i for i, p in enumerate(all_points) if p['fixType'] > 0), None
            )

        if first_fix_idx is None:
            print(f"[{label}] No fix achieved in this recording — skipping.")
            continue

        # ── Resolve ground-truth coordinates ──────────────────────────────
        first_fix = all_points[first_fix_idx]
        try:
            file_date = datetime.date(first_fix['year'], first_fix['month'], first_fix['day'])
        except ValueError:
            file_date = datetime.date.today()
            print(f"[{label}] Warning: Invalid date in log file. Using today's date for tectonic drift.")

        if USE_LV95:
            gt_lat, gt_lon, gt_h, method = lv95_to_wgs84(LV95_E, LV95_N, LV95_H_LN02, file_date)
            gt_h_label = 'WGS84 ellipsoidal'
            h_field    = 'height_ell'
        else:
            gt_lat, gt_lon, gt_h = GROUND_TRUTH_LAT, GROUND_TRUTH_LON, GROUND_TRUTH_HMSL
            gt_h_label = 'hMSL (EGM96)'
            h_field    = 'hmsl'
            if gt_lat == 0.0 and gt_lon == 0.0:
                print("WARNING: GROUND_TRUTH_LAT / GROUND_TRUTH_LON are both 0.0 — "
                      "set them at the top of the script.")

        cold_start = cold_start_idx is not None
        first_fix_us = all_points[first_fix_idx]['epoch_us']

        if cold_start_us > 0 and first_fix_us > cold_start_us:
            ttff_s = (first_fix_us - cold_start_us) / 1e6
        else:
            ttff_s = (first_fix_idx - (cold_start_idx or 0)) * 0.1

        first_float_idx = next(
            (i for i in range(first_fix_idx, len(all_points)) if all_points[i]['carr_soln'] == 1),
            None,
        )
        first_fixed_idx = next(
            (i for i in range(first_fix_idx, len(all_points)) if all_points[i]['carr_soln'] == 2),
            None,
        )

        tt_float_s = -1.0
        if first_float_idx is not None:
            tt_float_us = all_points[first_float_idx]['epoch_us']
            if cold_start_us > 0 and tt_float_us > cold_start_us:
                tt_float_s = (tt_float_us - cold_start_us) / 1e6
            else:
                tt_float_s = (first_float_idx - (cold_start_idx or 0)) * 0.1

        tt_fixed_s = -1.0
        if first_fixed_idx is not None:
            tt_fixed_us = all_points[first_fixed_idx]['epoch_us']
            if cold_start_us > 0 and tt_fixed_us > cold_start_us:
                tt_fixed_s = (tt_fixed_us - cold_start_us) / 1e6
            else:
                tt_fixed_s = (first_fixed_idx - (cold_start_idx or 0)) * 0.1

        time_ref_us = cold_start_us

        points = [p for p in all_points[first_fix_idx:]
                  if not (p['lat'] == 0.0 and p['lon'] == 0.0)]

        if not points:
            print(f"[{label}] No valid post-fix positions found.")
            continue

        # ── Time series ───────────────────────────────────────────────────
        times_s, dist_m, delta_n, delta_e, hacc_m, vacc_cm, height_m, carr = [], [], [], [], [], [], [], []
        for p in points:
            dn, de = north_east_offset_m(gt_lat, gt_lon, p['lat'], p['lon'])
            times_s.append((p['epoch_us'] - time_ref_us) / 1e6)
            dist_m.append(math.hypot(dn, de))
            delta_n.append(dn)
            delta_e.append(de)
            hacc_m.append(p['hacc'])
            vacc_cm.append(p['vacc'] * 100)
            height_m.append(p[h_field])
            carr.append(p['carr_soln'])

        dist_cm   = [d * 100 for d in dist_m]
        hacc_cm   = [v * 100 for v in hacc_m]
        h_err_cm  = [(h - gt_h) * 100 for h in height_m]

        # ── Statistics ────────────────────────────────────────────────────
        cep50       = percentile(dist_m, 50)
        cep95       = percentile(dist_m, 95)
        stdev_d     = statistics.stdev(dist_m) if len(dist_m) > 1 else 0.0
        median_dn     = statistics.median(delta_n)
        median_de     = statistics.median(delta_e)
        median_h_err  = statistics.median(h_err_cm) if h_err_cm else 0.0
        carr_counts = {0: carr.count(0), 1: carr.count(1), 2: carr.count(2)}

        print(f"\n[{label}]")
        print(f"  {first_fix_idx} pre-fix records dropped, {len(points)} post-fix records used")
        print(f"  TTFF: {ttff_s:.1f} s")
        if tt_float_s >= 0: print(f"  Time to Float: {tt_float_s:.1f} s")
        if tt_fixed_s >= 0: print(f"  Time to Fixed: {tt_fixed_s:.1f} s")
        print(f"  Horizontal CEP50={cep50*100:.1f}cm, CEP95={cep95*100:.1f}cm, StdDev={stdev_d*100:.1f}cm")
        print(f"  Median offset: N={median_dn*100:+.1f}cm, E={median_de*100:+.1f}cm, H={median_h_err:+.1f}cm")
        print(f"  Fixes: Fixed={carr_counts[2]} Float={carr_counts[1]} NoRTK={carr_counts[0]}")

        transitions = []
        for i in range(1, len(carr)):
            if carr[i] != carr[i - 1]:
                transitions.append((times_s[i], carr[i]))

        datasets.append({
            'label': label,
            'friendly_name': get_friendly_name(label),
            'times_s': times_s,
            'dist_m': dist_m,
            'dist_cm': dist_cm,
            'hacc_cm': hacc_cm,
            'h_err_cm': h_err_cm,
            'vacc_cm': vacc_cm,
            'delta_n': delta_n,
            'delta_e': delta_e,
            'carr': carr,
            'cep50': cep50,
            'cep95': cep95,
            'stdev_d': stdev_d,
            'median_dn': median_dn,
            'median_de': median_de,
            'median_h_err': median_h_err,
            'ttff_s': ttff_s,
            'tt_float_s': tt_float_s,
            'tt_fixed_s': tt_fixed_s,
            'transitions': transitions,
            'gt_h_label': gt_h_label
        })

    if not datasets:
        return

    # ── Plot layout ───────────────────────────────────────────────────
    fig = plt.figure(figsize=(14, 9))

    gs = gridspec.GridSpec(
        2, 2,
        figure=fig,
        width_ratios=[2.5, 1],
        hspace=0.40,
        wspace=0.30,
    )

    ax_dist    = fig.add_subplot(gs[0, 0])                       # top-left
    ax_scatter = fig.add_subplot(gs[0, 1])                       # top-right
    ax_height  = fig.add_subplot(gs[1, 0], sharex=ax_dist)      # bottom-left
    ax_ne      = fig.add_subplot(gs[1, 1], sharex=ax_dist)      # bottom-right

    time_axes = (ax_dist, ax_height, ax_ne)

    plot_limit = 0.0
    comp_colors = ['tab:blue', 'tab:orange', 'tab:green', 'tab:red', 'tab:purple', 'tab:brown']
    comp_colors = ['tab:red', 'tab:purple', 'tab:brown', 'tab:pink', 'tab:gray', 'tab:olive', 'tab:cyan']
    used_correction_colors = set()

    def get_mpl_color(label, pool_idx):
        lbl = label.lower()
        if 'has' in lbl:
            color = 'tab:orange'
        elif 'spartn' in lbl or 'spartan' in lbl or 'pointperfect' in lbl:
            color = 'tab:blue'
        elif 'nocorr' in lbl:
            color = 'tab:green'
        else:
            color = None

        if color is not None and color not in used_correction_colors:
            used_correction_colors.add(color)
            return color, pool_idx
        return comp_colors[pool_idx % len(comp_colors)], pool_idx + 1

    if len(datasets) == 1:
        ds = datasets[0]
        
        # ── Top-left: horizontal error + hAcc + rolling min (log) ────────
        ax_dist.plot(ds['times_s'], ds['dist_cm'], color='steelblue', linewidth=0.7,
                     alpha=0.7, label='Horiz. error', zorder=2)
        ax_dist.plot(ds['times_s'], ds['hacc_cm'], color='darkorange', linewidth=0.7,
                     alpha=0.8, label='hAcc (chip)', zorder=2)

        if SHOW_STATS_LABELS:
            stats_text = (
                f"n = {len(ds['dist_m'])}\n"
                f"CEP50 = {ds['cep50']*100:.1f} cm\n"
                f"std   = {ds['stdev_d']*100:.1f} cm\n"
                f"min   = {min(ds['dist_m'])*100:.1f} cm"
            )
            ax_dist.text(
                0.01, 0.02, stats_text,
                transform=ax_dist.transAxes,
                verticalalignment='bottom', fontsize=7.5, family='monospace',
                bbox=dict(boxstyle='round,pad=0.4', facecolor='white', alpha=0.85),
            )

        # ── Bottom-left: height error vs vAcc (symlog) ───────────────────────
        ax_height.plot(ds['times_s'], ds['h_err_cm'], color='mediumpurple', linewidth=0.7,
                       alpha=0.7, label=f'Δh actual', zorder=2)
        if SHOW_MEAN_LINES:
            ax_height.axhline(ds['median_h_err'], color='mediumpurple', linestyle='--',
                              linewidth=1.2, alpha=0.8, label=f"Median Δh {ds['median_h_err']:+.1f} cm", zorder=3)
        ax_height.plot(ds['times_s'], ds['vacc_cm'], color='darkorange', linewidth=0.7,
                       alpha=0.8, label='±vAcc (chip)', zorder=2)
        ax_height.plot(ds['times_s'], [-v for v in ds['vacc_cm']], color='darkorange', linewidth=0.7,
                       alpha=0.8, zorder=2)
        ax_height.axhline(0, color='black', linewidth=0.6, zorder=1)
        ax_height.set_title(f"Height to ground truth")

        # ── Bottom-right: N/E offset (symlog, signed) ─────────────────────
        ax_ne.plot(ds['times_s'], [v * 100 for v in ds['delta_n']],
                   color='tab:green', linewidth=0.7, alpha=0.8, label='ΔNorth', zorder=2)
        ax_ne.plot(ds['times_s'], [v * 100 for v in ds['delta_e']],
                   color='tab:red',   linewidth=0.7, alpha=0.8, label='ΔEast',  zorder=2)
        ax_ne.axhline(0, color='black', linewidth=0.6)
        if SHOW_MEAN_LINES:
            ax_ne.axhline(ds['median_dn'] * 100, color='tab:green', linestyle='--', linewidth=1.0, alpha=0.7)
            ax_ne.axhline(ds['median_de'] * 100, color='tab:red',   linestyle='--', linewidth=1.0, alpha=0.7)

        if SHOW_STATS_LABELS:
            ax_ne.text(
                0.02, 0.02,
                f"median N = {ds['median_dn']*100:+.1f} cm\nmedian E = {ds['median_de']*100:+.1f} cm",
                transform=ax_ne.transAxes,
                verticalalignment='bottom', fontsize=7.5, family='monospace',
                bbox=dict(boxstyle='round,pad=0.4', facecolor='white', alpha=0.85),
            )

        # ── State transition vertical lines (solid) ───────────────────────
        drawn_cs = set()
        for t_tr, new_cs in ds['transitions']:
            draw_vlines(time_axes, t_tr, _TR_COLOR[new_cs], linestyle='-',
                        linewidth=0.9, alpha=0.65)
            drawn_cs.add(new_cs)

        # TTFF vertical line
        if ds['ttff_s'] > 0:
            draw_vlines(time_axes, ds['ttff_s'], color='royalblue',
                        linestyle='-', linewidth=1.3, alpha=0.85)

        # ── Legend for ax_dist ────────────────────────────────────────────
        existing_handles, _ = ax_dist.get_legend_handles_labels()
        extra_handles = []

        if ds['ttff_s'] > 0:
            extra_handles.append(
                Line2D([0], [0], color='royalblue', linewidth=1.3, linestyle='-',
                       label=f"First fix  (TTFF={ds['ttff_s']:.1f} s)")
            )
        for cs in sorted(drawn_cs):
            lbl = _TR_LABEL[cs]
            if cs == 1 and ds['tt_float_s'] >= 0:
                lbl += f"  ({ds['tt_float_s']:.1f} s)"
            elif cs == 2 and ds['tt_fixed_s'] >= 0:
                lbl += f"  ({ds['tt_fixed_s']:.1f} s)"

            extra_handles.append(
                Line2D([0], [0], color=_TR_COLOR[cs], linewidth=0.9, linestyle='-',
                       label=lbl)
            )

        ax_dist.legend(handles=existing_handles + extra_handles,
                       fontsize=8, loc='upper right')

        # ── N/E scatter coloured by carrSoln ─────────────────────────────
        for cs in (0, 1, 2):
            idxs = [i for i, c in enumerate(ds['carr']) if c == cs]
            if not idxs:
                continue
            ax_scatter.scatter(
                [ds['delta_e'][i] * 100 for i in idxs],
                [ds['delta_n'][i] * 100 for i in idxs],
                s=4, alpha=0.5, color=_CARR_COLOR[cs],
                linewidths=0, label=_CARR_LABEL[cs], zorder=2,
            )

        if SHOW_MEAN_LINES:
            ax_scatter.scatter([ds['median_de'] * 100], [ds['median_dn'] * 100], s=60, color='red',
                               zorder=5,
                               label=f"Median ({ds['median_de']*100:+.1f},{ds['median_dn']*100:+.1f}) cm")

            for radius, ls, lbl in [(ds['cep50'] * 100, '--', f"CEP50 {ds['cep50']*100:.1f} cm"),
                                     (ds['cep95'] * 100, ':',  f"CEP95 {ds['cep95']*100:.1f} cm")]:
                ax_scatter.add_patch(patches.Circle(
                    (0, 0), radius, fill=False, linestyle=ls,
                    edgecolor='black', linewidth=1.0, zorder=3, label=lbl,
                ))

        plot_limit = ds['cep95'] * 100 * 1.2
    else:
        # ── MULTIPLE DATASETS ──────────────────────────────────────────────
        stats_texts = []
        max_cep50 = 0
        pool_idx = 0

        for i, ds in enumerate(datasets):
            c = comp_colors[i % len(comp_colors)]
            c, pool_idx = get_mpl_color(ds['label'], pool_idx)
            fn = ds['friendly_name']

            # Top-left Dist
            ax_dist.plot(ds['times_s'], ds['dist_cm'], color=c, linewidth=0.7, alpha=0.8, label=f"{fn}")
            if SHOW_MEAN_LINES:
                ax_dist.axhline(ds['cep50'] * 100, color=c, linestyle='--', linewidth=1.0, alpha=0.6)

            # Bottom-left Height
            ax_height.plot(ds['times_s'], ds['h_err_cm'], color=c, linewidth=0.7, alpha=0.8, label=f"{fn} (medΔh: {ds['median_h_err']:+.1f}cm)")
            if SHOW_MEAN_LINES:
                ax_height.axhline(ds['median_h_err'], color=c, linestyle='--', linewidth=1.2, alpha=0.7)

            # Bottom-right hAcc
            ax_ne.plot(ds['times_s'], ds['hacc_cm'], color=c, linewidth=0.7, alpha=0.8, linestyle='-', label=f"{fn}")

            # Top-right Scatter
            ax_scatter.scatter([v * 100 for v in ds['delta_e']], [v * 100 for v in ds['delta_n']], s=4, alpha=0.5, color=c, linewidths=0, label=f"{fn}", zorder=2)
            if SHOW_MEAN_LINES:
                ax_scatter.add_patch(patches.Circle((0, 0), ds['cep50'] * 100, fill=False, linestyle=':', edgecolor=c, linewidth=1.2, zorder=3, label=f"{fn} CEP50: {ds['cep50']*100:.1f} cm"))

            if SHOW_STATS_LABELS:
                stats_texts.append(f"{fn}: CEP50={ds['cep50']*100:.1f}cm")

            if ds['cep50'] > max_cep50:
                max_cep50 = ds['cep50']

        ax_height.axhline(0, color='black', linewidth=0.6, zorder=1)

        if SHOW_STATS_LABELS and stats_texts:
            ax_dist.text(0.01, 0.02, "\n".join(stats_texts), transform=ax_dist.transAxes, verticalalignment='bottom', fontsize=7.5, family='monospace', bbox=dict(boxstyle='round,pad=0.4', facecolor='white', alpha=0.85))

        plot_limit = max_cep50 * 100 * 2.0
        
        ax_dist.legend(fontsize=8, loc='upper right')
        ax_height.set_title("Height to ground truth")

    # ── Common Configurations ─────────────────────────────────────────
    for ax in time_axes:
        ax.set_xlim(left=0)

    ax_dist.set_yscale('log')
    ax_dist.set_xlabel('Time (s)')
    ax_dist.set_ylabel('Horizontal error (cm)')
    ax_dist.set_title('Distance to ground truth')
    ax_dist.grid(True, which='both', alpha=0.25)
    ax_dist.grid(True, which='major', alpha=0.4)

    ax_height.set_yscale('symlog', linthresh=1.0)
    ax_height.set_xlabel('Time (s)')
    ax_height.set_ylabel('Height error (cm)')
    ax_height.legend(fontsize=8, loc='upper right')
    ax_height.grid(True, which='both', alpha=0.25)
    ax_height.grid(True, which='major', alpha=0.4)

    if len(datasets) == 1:
        ax_ne.set_yscale('symlog', linthresh=1.0)
        ax_ne.set_xlabel('Time (s)')
        ax_ne.set_ylabel('Offset (cm)')
        ax_ne.set_title('N/E offset from truth')
    else:
        ax_ne.set_yscale('log')
        ax_ne.set_xlabel('Time (s)')
        ax_ne.set_ylabel('hAcc estimate (cm)')
        ax_ne.set_title('hAcc (chip self-estimate)')

    ax_ne.legend(fontsize=8, loc='upper right')
    ax_ne.grid(True, which='both', alpha=0.25)
    ax_ne.grid(True, which='major', alpha=0.4)

    ax_scatter.scatter([0], [0], s=80, color='black', marker='+', zorder=5,
                       linewidths=1.5, label='Ground truth')
    ax_scatter.axhline(0, color='black', linewidth=0.5)
    ax_scatter.axvline(0, color='black', linewidth=0.5)
    ax_scatter.set_xlabel('ΔEast (cm)')
    ax_scatter.set_ylabel('ΔNorth (cm)')
    ax_scatter.set_title('N/E scatter (truth = origin)')
    ax_scatter.legend(fontsize=6, loc='best')
    ax_scatter.grid(True, alpha=0.3)
    ax_scatter.set_aspect('equal')

    if plot_limit > 0:
        plot_limit = max(plot_limit, 1.0)
        ax_scatter.set_xlim(-plot_limit, plot_limit)
        ax_scatter.set_ylim(-plot_limit, plot_limit)

    # ── Save ──────────────────────────────────────────────────────────
    if len(datasets) == 1:
        out_name = f"accuracy_{datasets[0]['label']}.png"
    else:
        out_name = f"accuracy_{'_vs_'.join([d['label'] for d in datasets])}.png"
        
    output_path = os.path.join(LOG_DIR, out_name)
    fig.savefig(output_path, dpi=500, bbox_inches='tight')
    print(f"\nSaved: {output_path}")

    if is_batch:
        plt.close(fig)


def main():
    batch_sets = []
    if len(sys.argv) > 1:
        filepaths = []
        for arg in sys.argv[1:]:
            if not os.path.isabs(arg):
                filepaths.append(os.path.join(LOG_DIR, arg))
            else:
                filepaths.append(arg)
        batch_sets.append(filepaths)
        is_batch = False
    else:
        ans = pick_files()
        if not ans:
            return
        if ans == ['batch']:
            for item in PREDEFINED_BATCH_SETS:
                batch_sets.append([os.path.join(LOG_DIR, f) for f in item])
            is_batch = True
        elif ans == ['all']:
            files = sorted(f for f in os.listdir(LOG_DIR) if f.endswith('.jsonl'))
            batch_sets = [[os.path.join(LOG_DIR, f)] for f in files]
            is_batch = False
        else:
            batch_sets.append(ans)
            is_batch = False

    for paths in batch_sets:
        process_datasets(paths, is_batch)
        
    if not is_batch:
        plt.show()


if __name__ == '__main__':
    main()
