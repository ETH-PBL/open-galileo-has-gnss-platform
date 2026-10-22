import os
import json
import folium
import branca.colormap as cm

# --- Configuration ---
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
LOG_DIR = os.path.join(SCRIPT_DIR, 'log_data')
INPUT_LOG = os.path.join(LOG_DIR, 'log_gnss_*.jsonl') # From serial_logger.py
OUTPUT_HTML = os.path.join(LOG_DIR, 'rotation_has.html')

def main():
    if not os.path.exists(INPUT_LOG):
        print(f"Input file not found: {INPUT_LOG}")
        print("Please run the serial_logger.py script first.")
        return
    
    print(f"Loading data from: {INPUT_LOG}")

    data_points = []
    
    with open(INPUT_LOG, 'r', encoding='utf-8') as file:
        for line in file:
            if not line.strip():
                continue
            try:
                data = json.loads(line)
                if data.get('sensor') == 'GNSS_PVT':
                    lon = data.get('lon', 0) / 1e7
                    lat = data.get('lat', 0) / 1e7
                    hacc = data.get('hAcc', 0) / 1000.0  # Convert mm to meters
                    
                    if lat != 0.0 and lon != 0.0:
                        data_points.append((lat, lon, hacc))
            except json.JSONDecodeError:
                continue

    if not data_points:
        print("No valid GNSS position data found in the log.")
        return

    # Calculate center of the map
    center_lat = sum(p[0] for p in data_points) / len(data_points)
    center_lon = sum(p[1] for p in data_points) / len(data_points)
    
    # Initialize the Map
    gnss_map = folium.Map(location=[center_lat, center_lon], zoom_start=18, max_zoom=100, control_scale=True)
    
    # Add Satellite View (Esri World Imagery)
    folium.TileLayer(
        tiles='https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}',
        attr='Esri',
        name='Satellite View',
        max_zoom=100
    ).add_to(gnss_map)

    # Determine accuracy bounds for the Colormap (lower hAcc means higher accuracy)
    hacc_values = [p[2] for p in data_points]
    min_acc, max_acc = min(hacc_values), max(hacc_values)
    
    # Prevent division by zero if all points have the exact same accuracy
    if min_acc == max_acc:
        min_acc -= 0.1
        max_acc += 0.1

    # Color scale: Green (lowest value = best accuracy) to Red (highest value = worst accuracy)
    colormap = cm.LinearColormap(colors=['green', 'yellow', 'red'], vmin=min_acc, vmax=max_acc)
    colormap.caption = 'Horizontal Accuracy Estimate (meters) - Green is better'
    
    # Add points to the map
    for lat, lon, hacc in data_points:
        folium.CircleMarker(
            location=[lat, lon], radius=4, color=colormap(hacc), fill=True, 
            fill_color=colormap(hacc), fill_opacity=0.8, popup=f"Accuracy: {hacc:.2f}m"
        ).add_to(gnss_map)
        
    gnss_map.add_child(colormap)
    
    # Add Layer Control to switch between map views
    folium.LayerControl().add_to(gnss_map)

    gnss_map.save(OUTPUT_HTML)
    print(f"Map successfully generated and saved to: {OUTPUT_HTML}")

if __name__ == "__main__":
    main()
