"""
plot_gnss_advanced.py — multi-dataset GNSS comparison map

Marker shapes encode RTK fix quality (decoded from the flags field, bits 7-6):
  ●  Circle   — carrSoln = 2  RTK Fixed
  ■  Square   — carrSoln = 1  RTK Float
  ▲  Triangle — carrSoln = 0  No RTK (standard 3D fix)

Each dataset gets two toggleable layers in the legend:
  - Points layer  (shapes + colour per dataset)
  - Headings layer  (direction arrows, one every HEADING_STEP points)

Performance: markers are rendered via Leaflet's built-in HTML5 Canvas API, extended
with a custom _updateCircle to support native canvas square and triangle drawing.
This avoids DOM bloat completely; thousands of elements are drawn instantly as pixels.
Arrows use a single MultiLineString GeoJSON also natively mapped to the Canvas.

Usage:
    python plot_gnss_advanced.py                          # loads every .jsonl in log_data/
    python plot_gnss_advanced.py file1.jsonl file2.jsonl  # explicit files
"""

import os
import sys
import json
import math
import statistics
from datetime import datetime, timezone
import folium
from folium.plugins import MeasureControl
from branca.element import MacroElement
from jinja2 import Template

SCRIPT_DIR  = os.path.dirname(os.path.abspath(__file__))
LOG_DIR     = os.path.join(SCRIPT_DIR, 'log_data')
OUTPUT_HTML = os.path.join(LOG_DIR, 'map_comparison3.html')

# One arrow every N points (10 Hz data → 10 = one arrow per second)
HEADING_STEP   = 10
ARROW_LENGTH_M = 5   # shaft length in metres
MARKER_RADIUS  = 5   # SVG marker radius in pixels

FALLBACK_COLORS = ['#d62728', '#9467bd', '#8c564b', '#e377c2', '#bcbd22']

def get_dataset_color(label, pool_idx):
    lbl = label.lower()
    if 'has' in lbl:
        return '#ff7f0e', pool_idx  # Orange
    if 'spartn' in lbl or 'spartan' in lbl or 'pointperfect' in lbl:
        return '#1f77b4', pool_idx  # Blue
    if 'nocorr' in lbl:
        return '#2ca02c', pool_idx  # Green
    return FALLBACK_COLORS[pool_idx % len(FALLBACK_COLORS)], pool_idx + 1


# ── Helpers ───────────────────────────────────────────────────────────

def get_carr_soln(flags):
    """Extract carrSoln from bits 7-6 of the flags byte."""
    return (flags >> 6) & 0x3


# ── Canvas-based point layer ──────────────────────────────────────────
#
# Extends Leaflet's built-in Canvas renderer with square and triangle path
# drawing. By globally setting prefer_canvas=True on the map, this logic
# completely circumvents SVG and DOM appending, allowing thousands of elements
# to be rendered via direct GPU-accelerated pixel calls on zoom/pan.

class _PointLayer(MacroElement):
    """
    Renders all markers via L.geoJSON + Canvas pointToLayer.
    Shapes are Canvas drawing instructions.
    """
    _template = Template("""
        {% macro script(this, kwargs) %}
        (function () {
            /* extend Canvas renderer once for square and triangle shapes */
            if (!L.Canvas.prototype._updateCircleExtended) {
                var originalUpdateCircle = L.Canvas.prototype._updateCircle;
                L.Canvas.include({
                    _updateCircleExtended: true,
                    _updateCircle: function (layer) {
                        var options = layer.options;
                        if (options.shape === 'square' || options.shape === 'triangle') {
                            if (!this._drawing || layer._empty()) { return; }
                            var p = layer._point,
                                ctx = this._ctx,
                                r = Math.max(Math.round(layer._radius), 1);
                            ctx.beginPath();
                            if (options.shape === 'square') {
                                ctx.rect(p.x - r, p.y - r, r * 2, r * 2);
                            } else {
                                ctx.moveTo(p.x, p.y - r);
                                ctx.lineTo(p.x + r, p.y + r);
                                ctx.lineTo(p.x - r, p.y + r);
                                ctx.closePath();
                            }
                            this._fillStroke(ctx, layer);
                        } else {
                            /* Fallback to native Leaflet canvas circle implementation */
                            originalUpdateCircle.call(this, layer);
                        }
                    }
                });
            }

            /* register this dataset in the global offset store */
            window._gnssDatasets = window._gnssDatasets || [];
            var _dsIdx = window._gnssDatasets.length;
            window._gnssDatasets.push({
                label:   "{{ this.label }}",
                color:   "{{ this.color }}",
                markers: []   /* {marker, origLat, origLon, heading} */
            });

            var r = {{ this.radius }};

            L.geoJSON({{ this.data }}, {
                pointToLayer: function (feature, latlng) {
                    var p    = feature.properties;
                    var shapeType = 'triangle';
                    if (p.cs === 2) shapeType = 'circle';
                    else if (p.cs === 1) shapeType = 'square';

                    var opts = {
                        shape:       shapeType,
                        radius:      r,
                        fillColor:   p.col,
                        color:       'rgba(0,0,0,0.4)',
                        weight:      1,
                        fillOpacity: 0.9,
                        opacity:     0.9
                    };
                    var m = L.circleMarker(latlng, opts);
                    m.bindTooltip(p.tt, { sticky: true });
                    window._gnssDatasets[_dsIdx].markers.push({
                        marker:  m,
                        origLat: latlng.lat,
                        origLon: latlng.lng,
                        heading: p.hdg
                    });
                    return m;
                }
            }).addTo({{ this._parent.get_name() }});
        })();
        {% endmacro %}
    """)

    def __init__(self, geojson_data, radius, label, color):
        super().__init__()
        self.data   = json.dumps(geojson_data, separators=(',', ':'))
        self.radius = radius
        self.label  = label
        self.color  = color


# ── Arrow layer ───────────────────────────────────────────────────────

class _ArrowLayer(MacroElement):
    """
    Renders heading arrows as a single MultiLineString GeoJSON.
    Because prefer_canvas=True is active globally, Leaflet automatically
    renders this efficiently on the canvas without massive DOM bloat.
    """
    _template = Template("""
        {% macro script(this, kwargs) %}
        (function () {
            L.geoJSON({{ this.data }}, {
                style: function (f) {
                    return { color: f.properties.col, weight: 2.2, opacity: 0.9 };
                }
            }).addTo({{ this._parent.get_name() }});
        })();
        {% endmacro %}
    """)

    def __init__(self, geojson_data):
        super().__init__()
        self.data = json.dumps(geojson_data, separators=(',', ':'))


# ── GeoJSON builders ──────────────────────────────────────────────────

_CARR_LABEL = {2: '● Fixed', 1: '■ Float', 0: '▲ NoRTK'}


def _build_points_geojson(points, color, label):
    features = []
    for p in points:
        carr = p['carr_soln']
        ts = datetime.fromtimestamp(p['epoch_us'] / 1e6, tz=timezone.utc).strftime('%H:%M:%S.%f')[:-3]
        tt = (
            f"<b>{label}</b><br>"
            f"Time (UTC): {ts}<br>"
            f"Fix: {_CARR_LABEL.get(carr, carr)} &nbsp;(fixType={p['fix']})<br>"
            f"hAcc: <b>{p['hacc']:.3f} m</b><br>"
            f"vAcc: {p['vacc']:.3f} m<br>"
            f"Correction: {p['correction']}<br>"
            f"Satellites: {p['numSV']}<br>"
            f"Heading: {p['heading']:.1f}° (±{p['heading_acc']:.1f}°)"
        )
        features.append({
            'type': 'Feature',
            'geometry': {'type': 'Point', 'coordinates': [p['lon'], p['lat']]},
            'properties': {'cs': carr, 'col': color, 'tt': tt, 'hdg': p['heading']},
        })
    return {'type': 'FeatureCollection', 'features': features}


def _arrow_segments(lat, lon, heading_deg, length_m=ARROW_LENGTH_M):
    """Returns (shaft_coords, head_coords) in GeoJSON [lon, lat] order."""
    theta   = math.radians(heading_deg)
    cos_lat = math.cos(math.radians(lat))

    def off(from_lat, from_lon, dist_m, angle_rad):
        dlat = dist_m * math.cos(angle_rad) / 111_111.0
        dlon = dist_m * math.sin(angle_rad) / (111_111.0 * cos_lat)
        return [from_lon + dlon, from_lat + dlat]

    tip = off(lat, lon, length_m, theta)
    tip_lat, tip_lon = tip[1], tip[0]

    # Wings computed FROM the tip, going backward ±35° — proper arrowhead at tip
    wing_m = length_m * 0.55
    left  = off(tip_lat, tip_lon, wing_m, theta + math.pi - math.radians(35))
    right = off(tip_lat, tip_lon, wing_m, theta + math.pi + math.radians(35))

    return [[lon, lat], tip], [left, tip, right]


def _build_arrow_geojson(points, color):
    lines = []
    for i, p in enumerate(points):
        if i % HEADING_STEP != 0:
            continue
        shaft, head = _arrow_segments(p['lat'], p['lon'], p['heading'])
        lines.append(shaft)
        lines.append(head)
    if not lines:
        return None
    return {
        'type': 'Feature',
        'properties': {'col': color},
        'geometry': {'type': 'MultiLineString', 'coordinates': lines},
    }


# ── Data loading ──────────────────────────────────────────────────────

def load_gnss(filepath):
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
                lat = d.get('lat', 0) / 1e7
                lon = d.get('lon', 0) / 1e7
                if lat == 0.0 and lon == 0.0:
                    continue
                flags = d.get('flags', 0)
                points.append({
                    'lat':         lat,
                    'lon':         lon,
                    'hacc':        d.get('hAcc', 0) / 1000.0,
                    'vacc':        d.get('vAcc', 0) / 1000.0,
                    'fix':         d.get('fixType', 0),
                    'flags':       flags,
                    'carr_soln':   get_carr_soln(flags),
                    'numSV':       d.get('numSV', 0),
                    'correction':  d.get('correction_source', ''),
                    'epoch_us':    int(d.get('epoch_us', 0)),
                    'heading':     d.get('heading', 0) / 1e5,
                    'heading_acc': d.get('headingAcc', 0) / 1e5,
                })
            except (json.JSONDecodeError, ValueError):
                continue
    return points


def compute_stats(points):
    if not points:
        return None
    haccs = [p['hacc'] for p in points]
    carr_counts = {0: 0, 1: 0, 2: 0}
    for p in points:
        carr_counts[p['carr_soln']] = carr_counts.get(p['carr_soln'], 0) + 1
    return {
        'n':           len(points),
        'mean_hacc':   statistics.mean(haccs),
        'median_hacc': statistics.median(haccs),
        'stdev_hacc':  statistics.stdev(haccs) if len(haccs) > 1 else 0.0,
        'min_hacc':    min(haccs),
        'max_hacc':    max(haccs),
        'corrections': list({p['correction'] for p in points}),
        'carr_counts': carr_counts,
    }


def print_comparison_table(datasets):
    header = (
        f"{'Dataset':<28} {'N':>6} {'Mean(m)':>9} "
        f"{'Median(m)':>10} {'StdDev':>8} {'Min(m)':>8} {'Max(m)':>8}  "
        f"{'Fixed':>6} {'Float':>6} {'NoRTK':>6}"
    )
    print("\n" + "=" * len(header))
    print(header)
    print("-" * len(header))
    for label, _, s in datasets:
        cc = s['carr_counts']
        print(
            f"{label:<28} {s['n']:>6} {s['mean_hacc']:>9.3f} "
            f"{s['median_hacc']:>10.3f} {s['stdev_hacc']:>8.3f} "
            f"{s['min_hacc']:>8.3f} {s['max_hacc']:>8.3f}  "
            f"{cc.get(2,0):>6} {cc.get(1,0):>6} {cc.get(0,0):>6}"
            f"   [{', '.join(s['corrections'])}]"
        )
    print("=" * len(header) + "\n")


# ── Map legend HTML ───────────────────────────────────────────────────

SHAPE_LEGEND_HTML = """
<div style="
    position: fixed;
    bottom: 30px; right: 10px; z-index: 1000;
    background: white; padding: 8px 12px;
    border: 1px solid #aaa; border-radius: 6px;
    font-family: sans-serif; font-size: 12px; line-height: 1.8;
    box-shadow: 2px 2px 4px rgba(0,0,0,0.2);
">
  <b>Fix quality</b><br>
  <span style="font-size:15px;">&#9679;</span> &nbsp;RTK Fixed (carrSoln=2)<br>
  <span style="font-size:12px;">&#9632;</span> &nbsp;RTK Float (carrSoln=1)<br>
  <span style="font-size:13px;">&#9650;</span> &nbsp;No RTK &nbsp;(carrSoln=0)
</div>
"""


def _build_offset_panel_html(datasets):
    """
    Returns an HTML string that injects:
      - A fixed-position panel with one number input (cm) per dataset.
      - JavaScript functions gnssApplyOffset(idx) and gnssApplyAll() that
        shift each marker along its per-point heading by the given offset.
    """
    rows = []
    for idx, (label, color) in enumerate(datasets):
        rows.append(
            f'<div style="display:flex;align-items:center;gap:6px;margin-top:6px;">'
            f'  <span style="color:{color};font-size:16px;line-height:1;">&#9632;</span>'
            f'  <span style="flex:1;overflow:hidden;text-overflow:ellipsis;'
            f'         white-space:nowrap;max-width:200px;font-weight:bold;" title="{label}">{label}</span>'
            f'</div>'
            f'<div style="display:flex;align-items:center;gap:8px;margin-bottom:5px;padding-left:14px;font-size:11px;">'
            f'  <span><label>Hdg:</label> <input type="number" id="gnss-off-fwd-{idx}" value="0" step="1"'
            f'         style="width:45px;text-align:right;padding:2px 4px;"'
            f'         onkeydown="if(event.key===\'Enter\')gnssApplyOffset({idx})"></span>'
            f'  <span><label>N:</label> <input type="number" id="gnss-off-n-{idx}" value="0" step="1"'
            f'         style="width:45px;text-align:right;padding:2px 4px;"'
            f'         onkeydown="if(event.key===\'Enter\')gnssApplyOffset({idx})"></span>'
            f'  <span><label>E:</label> <input type="number" id="gnss-off-e-{idx}" value="0" step="1"'
            f'         style="width:45px;text-align:right;padding:2px 4px;"'
            f'         onkeydown="if(event.key===\'Enter\')gnssApplyOffset({idx})">'
            f'  <span>cm</span>'
            f'</div>'
        )

    rows_html = '\n'.join(rows)

    return f"""
<div id="gnss-offset-panel" style="
    position: fixed; top: 70px; left: 10px; z-index: 1000;
    background: white; padding: 10px 14px;
    border: 1px solid #aaa; border-radius: 6px;
    font-family: sans-serif; font-size: 12px;
    box-shadow: 2px 2px 6px rgba(0,0,0,0.2);
    min-width: 250px;
">
  <div style="font-weight:bold;margin-bottom:8px;">Antenna offset (cm)</div>
  {rows_html}
  <div style="margin-top:8px;display:flex;gap:6px;justify-content:flex-end;">
    <button onclick="gnssResetAll()"
        style="padding:3px 8px;cursor:pointer;font-size:12px;">Reset</button>
    <button onclick="gnssApplyAll()"
        style="padding:3px 10px;cursor:pointer;font-size:12px;font-weight:bold;">Apply all</button>
  </div>
</div>
<script>
function gnssApplyOffset(idx) {{
    var ds = (window._gnssDatasets || [])[idx];
    if (!ds) return;
    var fwdCm = parseFloat(document.getElementById('gnss-off-fwd-' + idx).value) || 0;
    var nCm   = parseFloat(document.getElementById('gnss-off-n-' + idx).value) || 0;
    var eCm   = parseFloat(document.getElementById('gnss-off-e-' + idx).value) || 0;
    
    var fwdM = fwdCm / 100.0;
    var nM   = nCm / 100.0;
    var eM   = eCm / 100.0;

    ds.markers.forEach(function(item) {{
        var theta  = item.heading * Math.PI / 180;
        var cosLat = Math.cos(item.origLat * Math.PI / 180);
        
        var dlatFwd = fwdM * Math.cos(theta) / 111111;
        var dlonFwd = fwdM * Math.sin(theta) / (111111 * cosLat);
        
        var dlatN = nM / 111111;
        var dlonE = eM / (111111 * cosLat);

        item.marker.setLatLng([
            item.origLat + dlatFwd + dlatN, 
            item.origLon + dlonFwd + dlonE
        ]);
    }});
}}
function gnssApplyAll() {{
    for (var i = 0; i < (window._gnssDatasets || []).length; i++) {{
        gnssApplyOffset(i);
    }}
}}
function gnssResetAll() {{
    (window._gnssDatasets || []).forEach(function(ds, i) {{
        document.getElementById('gnss-off-fwd-' + i).value = 0;
        document.getElementById('gnss-off-n-' + i).value = 0;
        document.getElementById('gnss-off-e-' + i).value = 0;
        ds.markers.forEach(function(item) {{
            item.marker.setLatLng([item.origLat, item.origLon]);
        }});
    }});
}}
</script>
"""


# ── Main ─────────────────────────────────────────────────────────────

def main():
    if len(sys.argv) > 1:
        file_pairs = [
            (p, os.path.splitext(os.path.basename(p))[0])
            for p in sys.argv[1:]
        ]
    else:
        file_pairs = sorted(
            [
                (os.path.join(LOG_DIR, f), os.path.splitext(f)[0])
                for f in os.listdir(LOG_DIR)
                if f.endswith('.jsonl')
            ],
            key=lambda x: x[1],
        )

    if not file_pairs:
        print("No .jsonl files found.")
        return

    datasets = []
    all_lats, all_lons = [], []
    print("\nLoading files:")
    for filepath, label in file_pairs:
        if not os.path.exists(filepath):
            print(f"  [not found] {filepath}")
            continue
        points = load_gnss(filepath)
        if not points:
            print(f"  [empty]     {label}")
            continue
        s = compute_stats(points)
        datasets.append((label, points, s))
        all_lats.extend(p['lat'] for p in points)
        all_lons.extend(p['lon'] for p in points)
        print(f"  [ok]        {label}  ({s['n']} points)")

    if not datasets:
        print("No valid GNSS data found.")
        return

    print_comparison_table(datasets)

    center_lat = sum(all_lats) / len(all_lats)
    center_lon = sum(all_lons) / len(all_lons)

    gnss_map = folium.Map(
        location=[center_lat, center_lon],
        zoom_start=19,
        max_zoom=25,
        control_scale=True,
        prefer_canvas=True,
    )

    folium.TileLayer(
        tiles='https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}',
        attr='Esri',
        name='Satellite',
        max_zoom=25,
        max_native_zoom=21,
    ).add_to(gnss_map)

    pool_idx = 0
    dataset_colors = []

    for i, (label, points, s) in enumerate(datasets):
        color, pool_idx = get_dataset_color(label, pool_idx)
        dataset_colors.append((label, color))

        # ── Points layer ─────────────────────────────────────────────
        points_name = (
            f'<span style="color:{color};font-weight:bold">&#9632;</span> '
            f'{label} &nbsp;'
            f'<small>(n={s["n"]}, hAcc avg={s["mean_hacc"]:.2f} m)</small>'
        )
        points_group = folium.FeatureGroup(name=points_name, show=True)
        _PointLayer(
            _build_points_geojson(points, color, label),
            MARKER_RADIUS,
            label,
            color,
        ).add_to(points_group)
        points_group.add_to(gnss_map)

        # ── Headings layer ───────────────────────────────────────────
        headings_name = (
            f'<span style="color:{color}">&#10148;</span> '
            f'{label} — headings'
        )
        headings_group = folium.FeatureGroup(name=headings_name, show=False)
        arrow_geojson = _build_arrow_geojson(points, color)
        if arrow_geojson:
            _ArrowLayer(arrow_geojson).add_to(headings_group)
        headings_group.add_to(gnss_map)

    gnss_map.get_root().html.add_child(folium.Element(SHAPE_LEGEND_HTML))

    offset_panel = _build_offset_panel_html(
        dataset_colors
    )
    gnss_map.get_root().html.add_child(folium.Element(offset_panel))

    folium.LayerControl(collapsed=False).add_to(gnss_map)
    MeasureControl(primary_length_unit='meters').add_to(gnss_map)

    gnss_map.save(OUTPUT_HTML)
    print(f"Saved: {OUTPUT_HTML}")


if __name__ == '__main__':
    main()
