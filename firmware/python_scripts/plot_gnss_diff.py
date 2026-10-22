"""
plot_gnss_diff.py — distance difference between simultaneous GNSS recordings

Matches epochs by iTOW (GPS Time of Week), then plots:
  1. Horizontal distance between the two chips over time
  2. North / East offset over time  (shows directional bias)
  3. hAcc estimate from both chips over time  (reference)
  4. N/E scatter plot  (shows spatial spread and bias)

Usage:
    python plot_gnss_diff.py                          # interactive file picker
    python plot_gnss_diff.py file_a.jsonl file_b.jsonl [file_c.jsonl ...]
"""

import os
import sys
import json
import math
import statistics
import itertools
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import matplotlib.gridspec as gridspec

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
LOG_DIR    = os.path.join(SCRIPT_DIR, 'log_data')

# ── Predefined batch ──────────────────────────────────────────────────

PREDEFINED_BATCH_PAIRS = [
    # Example: [('file_a.jsonl', 0.0), ('file_b.jsonl', 0.0)],
    # Add your predefined pairs here, with optional antenna offsets in cm
]

# ── Data loading ──────────────────────────────────────────────────────

def load_gnss(filepath):
    """Return dict {iTOW: record} for every GNSS_PVT entry in a JSONL file."""
    points = {}
    with open(filepath, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                d = json.loads(line)
                if d.get('sensor') != 'GNSS_PVT':
                    continue
                itow = d.get('iTOW')
                if itow is None:
                    continue
                lat = d.get('lat', 0) / 1e7
                lon = d.get('lon', 0) / 1e7
                if lat == 0.0 and lon == 0.0:
                    continue
                points[itow] = {
                    'lat':        lat,
                    'lon':        lon,
                    'hacc':       d.get('hAcc', 0) / 1000.0,   # mm → m
                    'vacc':       d.get('vAcc', 0) / 1000.0,
                    'epoch_us':   int(d.get('epoch_us', 0)),
                    'correction': d.get('correction_source', ''),
                    'numSV':      d.get('numSV', 0),
                    'fixType':    d.get('fixType', 0),
                    'heading':    d.get('heading', 0) / 1e5,    # → degrees
                }
            except (json.JSONDecodeError, ValueError):
                continue
    return points


# ── Geometry ──────────────────────────────────────────────────────────

def haversine_m(lat1, lon1, lat2, lon2):
    """Haversine distance in metres between two WGS-84 lat/lon points."""
    R = 6_371_000.0
    phi1, phi2 = math.radians(lat1), math.radians(lat2)
    a = (math.sin(math.radians(lat2 - lat1) / 2) ** 2
         + math.cos(phi1) * math.cos(phi2)
         * math.sin(math.radians(lon2 - lon1) / 2) ** 2)
    return 2 * R * math.asin(math.sqrt(a))


def apply_heading_offset(lat, lon, heading_deg, offset_m):
    """Shift (lat, lon) by offset_m metres along heading_deg (clockwise from North)."""
    theta   = math.radians(heading_deg)
    cos_lat = math.cos(math.radians(lat))
    dlat = offset_m * math.cos(theta) / 111_111.0
    dlon = offset_m * math.sin(theta) / (111_111.0 * cos_lat)
    return lat + dlat, lon + dlon


def north_east_offset_m(lat_ref, lon_ref, lat, lon):
    """
    Flat-Earth approximation: offset of (lat, lon) from (lat_ref, lon_ref)
    in metres, split into North (+) / South (-) and East (+) / West (-).
    Accurate to <0.1% for offsets under ~1 km.
    """
    dn = (lat - lat_ref) * 111_111.0
    de = (lon - lon_ref) * 111_111.0 * math.cos(math.radians(lat_ref))
    return dn, de


# ── File picker ───────────────────────────────────────────────────────

def pick_files():
    files = sorted(f for f in os.listdir(LOG_DIR) if f.endswith('.jsonl'))
    if not files:
        print(f"No .jsonl files found in {LOG_DIR}")
        return None
    if len(files) < 2:
        print(f"Need at least two .jsonl files in {LOG_DIR} to compare.")
        return None

    print("\nAvailable log files:")
    for i, f in enumerate(files):
        print(f"  [{i}] {f}")
    print("\n  [batch] Run all predefined pairs")

    while True:
        choice = input("Select two or three files separated by space (e.g. '0 1 2'), or 'batch': ").strip()
        if not choice:
            continue
        if choice.lower() == 'batch':
            return ['batch']

        tokens = choice.split()
        if len(tokens) not in (2, 3):
            print("  Please select exactly two or three files.")
            continue

        selected = []
        invalid = False
        for c in tokens:
            if c.isdigit() and 0 <= int(c) < len(files):
                selected.append(os.path.join(LOG_DIR, files[int(c)]))
            else:
                invalid = True
                break

        if not invalid:
            return selected
        print("  Invalid choice, try again.")


# ── Main ──────────────────────────────────────────────────────────────

def process_datasets(paths, is_batch=False, predef_offsets=None):
    labels = [os.path.splitext(os.path.basename(p))[0] for p in paths]
    abc = [chr(ord('A') + i) for i in range(len(paths))]

    if is_batch:
        print(f"\n{'='*60}")
        print(f"Processing: {' vs '.join(labels)}")
        print(f"{'='*60}")

    for p in paths:
        if not os.path.exists(p):
            print(f"  [not found] {p}")
            return

    datas = []
    for p, l, a in zip(paths, labels, abc):
        print(f"\nLoading  {a}: {l}")
        d = load_gnss(p)
        print(f"  {len(d)} GNSS_PVT records")
        datas.append(d)

    common = set(datas[0].keys())
    for d in datas[1:]:
        common &= set(d.keys())
    common = sorted(common)

    print(f"Matched epochs (iTOW): {len(common)}")

    if not common:
        print("\nNo matching iTOW values — are the files from simultaneous recordings?")
        return

    # ── Antenna offset input ──────────────────────────────────────────
    offsets_cm = [0.0] * len(paths)

    if predef_offsets is not None:
        for i, off in enumerate(predef_offsets):
            if off is not None:
                offsets_cm[i] = float(off)

    if not is_batch and (predef_offsets is None or all(o is None for o in predef_offsets)):
        print("\nAntenna offset correction  (positive = forward along heading, 0 = none)")
        for i, (l, a) in enumerate(zip(labels, abc)):
            try:
                val = input(f"  Offset {a}  [{l}]  cm: ")
                if val:
                    offsets_cm[i] = float(val)
            except ValueError:
                pass

    offsets_m = [off / 100.0 for off in offsets_cm]

    # Build time series
    t0 = datas[0][common[0]]['epoch_us']
    
    comp_indices = list(itertools.combinations(range(len(paths)), 2))

    times_s   = []
    dist_m    = [[] for _ in comp_indices]
    delta_n   = [[] for _ in comp_indices]
    delta_e   = [[] for _ in comp_indices]
    haccs     = [[] for _ in paths]

    for itow in common:
        # Only consider points where all chips have a valid fix (fixType > 0)
        valid = True
        for d in datas:
            if d[itow]['fixType'] == 0:
                valid = False
                break
        if not valid:
            continue

        t  = (datas[0][itow]['epoch_us'] - t0) / 1e6
        times_s.append(t)

        lat_lons = []
        for i, d in enumerate(datas):
            lat, lon = apply_heading_offset(d[itow]['lat'], d[itow]['lon'], d[itow]['heading'], offsets_m[i])
            lat_lons.append((lat, lon))

        for i, (idx1, idx2) in enumerate(comp_indices):
            lat1, lon1 = lat_lons[idx1]
            lat2, lon2 = lat_lons[idx2]

            d_val = haversine_m(lat1, lon1, lat2, lon2)
            lat_mid = (lat1 + lat2) / 2
            dn_val   = (lat1 - lat2) * 111_111.0
            de_val   = (lon1 - lon2) * 111_111.0 * math.cos(math.radians(lat_mid))

            dist_m[i].append(d_val)
            delta_n[i].append(dn_val)
            delta_e[i].append(de_val)

        for i, d in enumerate(datas):
            haccs[i].append(d[itow]['hacc'])

    if not dist_m:
        print("\nNo valid simultaneous GNSS fixes found in the matched epochs.")
        return

    # Stats
    means_d   = [statistics.mean(dm) for dm in dist_m]
    medians_d = [statistics.median(dm) for dm in dist_m]
    stdevs_d  = [statistics.stdev(dm) if len(dm) > 1 else 0.0 for dm in dist_m]
    means_dn  = [statistics.mean(dn) for dn in delta_n]
    means_de  = [statistics.mean(de) for de in delta_e]

    for i, (idx1, idx2) in enumerate(comp_indices):
        a1, a2 = abc[idx1], abc[idx2]
        l1, l2 = labels[idx1], labels[idx2]
        print(f"\nDistance  {a1}={l1}  vs  {a2}={l2}")
        print(f"  Mean:     {means_d[i]*100:.1f} cm")
        print(f"  Median:   {medians_d[i]*100:.1f} cm")
        print(f"  Std dev:  {stdevs_d[i]*100:.1f} cm")
        print(f"  Min:      {min(dist_m[i])*100:.1f} cm")
        print(f"  Max:      {max(dist_m[i])*100:.1f} cm")
        print(f"\nMean offset ({a1} − {a2})")
        dir_n = f"{a1} is north" if means_dn[i] > 0 else f"{a1} is south"
        dir_e = f"{a1} is east"  if means_de[i] > 0 else f"{a1} is west"
        print(f"  North:    {means_dn[i]*100:+.1f} cm  ({dir_n} of {a2})")
        print(f"  East:     {means_de[i]*100:+.1f} cm  ({dir_e}  of {a2})")

    # ── Plot layout: 2×2 grid ────────────────────────────────────────
    fig = plt.figure(figsize=(14, 9))

    gs = gridspec.GridSpec(
        2, 2,
        figure=fig,
        width_ratios=[2.5, 1],
        hspace=0.38,
        wspace=0.30,
    )

    ax_dist    = fig.add_subplot(gs[0, 0])
    ax_scatter = fig.add_subplot(gs[0, 1])
    ax_ne      = fig.add_subplot(gs[1, 0], sharex=ax_dist)
    ax_hacc    = fig.add_subplot(gs[1, 1])

    comp_colors = [
        {'dist': 'steelblue',  'mean': 'red',         'median': 'darkorange',   'dn': 'tab:green', 'de': 'tab:red',    'scat': 'steelblue',  'scat_mean': 'red'},
        {'dist': 'tab:purple', 'mean': 'darkmagenta', 'median': 'mediumorchid', 'dn': 'tab:olive', 'de': 'tab:orange', 'scat': 'tab:purple', 'scat_mean': 'darkviolet'},
        {'dist': 'tab:brown',  'mean': 'saddlebrown', 'median': 'peru',         'dn': 'tab:cyan',  'de': 'tab:pink',   'scat': 'tab:brown',  'scat_mean': 'saddlebrown'},
        {'dist': 'tab:gray',   'mean': 'black',       'median': 'darkgray',     'dn': 'black',     'de': 'gray',       'scat': 'tab:gray',   'scat_mean': 'black'},
    ]

    if len(paths) == 2:
        scat_title = 'N/E scatter  (A − B)'
        ne_title = 'North / East offset  (A − B)'
    else:
        scat_title = 'N/E scatter'
        ne_title = 'North / East offset'

    for i, (idx1, idx2) in enumerate(comp_indices):
        c = comp_colors[i % len(comp_colors)]
        a1, a2 = abc[idx1], abc[idx2]

        if len(paths) == 2:
            dist_label = 'Distance'
            mean_label = f'Mean {means_d[i]*100:.1f} cm'
            median_label = f'Median {medians_d[i]*100:.1f} cm'
            dn_label = 'ΔNorth (A−B)'
            de_label = 'ΔEast (A−B)'
            scat_mean_label = f'Mean ({means_de[i]*100:+.1f},{means_dn[i]*100:+.1f}) cm'
        else:
            dist_label = f'Dist {a1}-{a2}'
            mean_label = f'Mean {a1}-{a2} {means_d[i]*100:.0f}cm'
            median_label = f'Med {a1}-{a2} {medians_d[i]*100:.0f}cm'
            dn_label = f'ΔN ({a1}-{a2})'
            de_label = f'ΔE ({a1}-{a2})'
            scat_mean_label = f'Mean {a1}-{a2} ({means_de[i]*100:+.0f},{means_dn[i]*100:+.0f})'

        # ── (0,0) Horizontal distance over time ─────────────────────────
        ax_dist.plot(times_s, [d * 100 for d in dist_m[i]],
                     color=c['dist'], linewidth=0.7, alpha=0.7, label=dist_label)
        ax_dist.axhline(means_d[i] * 100, color=c['mean'], linestyle='--',
                        linewidth=1.3, label=mean_label)
        ax_dist.axhline(medians_d[i] * 100, color=c['median'], linestyle=':',
                        linewidth=1.3, label=median_label)

        # ── (1,0) North / East offset over time ─────────────────────────
        ax_ne.plot(times_s, [v * 100 for v in delta_n[i]],
                   color=c['dn'], linewidth=0.7, alpha=0.8, label=dn_label)
        ax_ne.plot(times_s, [v * 100 for v in delta_e[i]],
                   color=c['de'],   linewidth=0.7, alpha=0.8, label=de_label)
        ax_ne.axhline(means_dn[i] * 100, color=c['dn'], linestyle='--',
                      linewidth=1.0, alpha=0.7)
        ax_ne.axhline(means_de[i] * 100, color=c['de'],   linestyle='--',
                      linewidth=1.0, alpha=0.7)

        # ── (0,1) N/E scatter plot ───────────────────────────────────────
        ax_scatter.scatter(
            [v * 100 for v in delta_e[i]],
            [v * 100 for v in delta_n[i]],
            s=4, alpha=0.4, color=c['scat'], linewidths=0,
        )
        ax_scatter.scatter(
            [means_de[i] * 100], [means_dn[i] * 100],
            s=60, color=c['scat_mean'], zorder=5, label=scat_mean_label
        )

    # ── Configurations ────────────────────────────────────────────────
    ax_dist.set_yscale('log')
    ax_dist.set_xlabel('Time (s)')
    ax_dist.set_ylabel('Horizontal distance (cm)')
    ax_dist.set_title('Distance between chips')
    ax_dist.legend(fontsize=8, loc='upper right')
    ax_dist.grid(True, which='both', alpha=0.3)

    if len(paths) == 2:
        stats_text = (
            f"n = {len(dist_m[0])}\n"
            f"mean   = {means_d[0]*100:.1f} cm\n"
            f"median = {medians_d[0]*100:.1f} cm\n"
            f"std    = {stdevs_d[0]*100:.1f} cm\n"
            f"min    = {min(dist_m[0])*100:.1f} cm\n"
            f"max    = {max(dist_m[0])*100:.1f} cm"
        )
    else:
        stats_text = f"n = {len(dist_m[0])}\n"
        for i, (idx1, idx2) in enumerate(comp_indices):
            stats_text += f"{abc[idx1]}-{abc[idx2]} mean = {means_d[i]*100:.1f} cm\n"
        stats_text = stats_text.strip()

    ax_dist.text(
        0.01, 0.98, stats_text,
        transform=ax_dist.transAxes,
        verticalalignment='top', fontsize=7.5, family='monospace',
        bbox=dict(boxstyle='round,pad=0.4', facecolor='white', alpha=0.85),
    )

    ax_ne.axhline(0, color='black', linewidth=0.6, linestyle='-')
    ax_ne.set_yscale('symlog', linthresh=1.0)
    ax_ne.set_xlabel('Time (s)')
    ax_ne.set_ylabel('Offset (cm)')
    ax_ne.set_title(ne_title)
    ax_ne.legend(fontsize=8, loc='upper right')
    ax_ne.grid(True, which='both', alpha=0.3)

    if len(paths) == 2:
        ne_text = f"mean N = {means_dn[0]*100:+.1f} cm\nmean E = {means_de[0]*100:+.1f} cm"
    else:
        ne_text = ""
        for i, (idx1, idx2) in enumerate(comp_indices):
            ne_text += f"{abc[idx1]}-{abc[idx2]} mN={means_dn[i]*100:+.1f} mE={means_de[i]*100:+.1f}\n"
        ne_text = ne_text.strip()

    ax_ne.text(
        0.02, 0.02, ne_text,
        transform=ax_ne.transAxes,
        verticalalignment='bottom', fontsize=7.5, family='monospace',
        bbox=dict(boxstyle='round,pad=0.4', facecolor='white', alpha=0.85),
    )

    ax_scatter.axhline(0, color='black', linewidth=0.6)
    ax_scatter.axvline(0, color='black', linewidth=0.6)
    ax_scatter.set_xscale('symlog', linthresh=1.0)
    ax_scatter.set_yscale('symlog', linthresh=1.0)
    ax_scatter.set_xlabel('ΔEast (cm)')
    ax_scatter.set_ylabel('ΔNorth (cm)')
    ax_scatter.set_title(scat_title)
    ax_scatter.legend(fontsize=8)
    ax_scatter.grid(True, which='both', alpha=0.3)

    # ── (1,1) hAcc both chips ───────────────────────────────────────
    def get_friendly_name(lbl):
        l = lbl.lower()
        if 'has' in l: return 'Galileo HAS'
        if 'spartn' in l or 'spartan' in l: return 'PointPerfect Flex'
        if 'nocorr' in l: return 'No Correction'
        return lbl

    hacc_colors = ['tab:blue', 'tab:orange', 'tab:green', 'tab:red', 'tab:purple']
    for i, d in enumerate(datas):
        ax_hacc.plot(times_s, [v * 100 for v in haccs[i]],
                     color=hacc_colors[i % len(hacc_colors)], linewidth=0.7, alpha=0.8,
                     label=get_friendly_name(labels[i]))

    ax_hacc.set_yscale('log')
    ax_hacc.set_xlabel('Time (s)')
    ax_hacc.set_ylabel('hAcc estimate (cm)')
    ax_hacc.set_title('hAcc (chip self-estimate)')
    ax_hacc.legend(fontsize=8)
    ax_hacc.grid(True, which='both', alpha=0.3)

    output_path = os.path.join(
        LOG_DIR, f"diff_{'_vs_'.join(labels)}.png"
    )
    fig.savefig(output_path, dpi=500, bbox_inches='tight')
    print(f"Saved: {output_path}")

    if not is_batch:
        plt.show()
    else:
        plt.close(fig)


def main():
    if len(sys.argv) >= 3:
        item = []
        for arg in sys.argv[1:]:
            item.extend([arg, None])
        batch_sets = [tuple(item)]
    elif len(sys.argv) == 1:
        ans = pick_files()
        if ans is None:
            return
        if ans == ['batch']:
            batch_sets = PREDEFINED_BATCH_PAIRS
        else:
            item = []
            for f in ans:
                item.extend([f, None])
            batch_sets = [tuple(item)]
    else:
        print("Usage: python plot_gnss_diff.py [file_a.jsonl file_b.jsonl ...]")
        return

    is_batch = len(batch_sets) > 1

    for item in batch_sets:
        paths = []
        offsets = []
        for i in range(0, len(item), 2):
            p = item[i]
            paths.append(os.path.join(LOG_DIR, p) if not os.path.isabs(p) else p)
            offsets.append(item[i+1])
        process_datasets(paths, is_batch, offsets)


if __name__ == '__main__':
    main()
