import time
import json
import sys
import ctypes
import threading
import numpy as np
import cv2
import mss
import win32api
import win32con
import serial.tools.list_ports

# Fix Windows DPI Scaling (so 1920x1080 isn't squeezed if you have 125% scaling in Windows)
try:
    ctypes.windll.user32.SetProcessDPIAware()
except Exception:
    pass

# Suppress '>>> ' serial prompt spammed by the makcu background reader thread
class FilterMakcuConsoleOutput:
    def __init__(self, stream):
        self.stream = stream

    def write(self, text):
        if text == ">>> " or text == ">>>" or text == ">>> \n":
            return
        if ">>> " in text:
            text = text.replace(">>> ", "")
            if not text:
                return
        self.stream.write(text)

    def flush(self):
        try:
            self.stream.flush()
        except Exception:
            pass

sys.stdout = FilterMakcuConsoleOutput(sys.stdout)

from makcu import create_controller, MouseButton

CONFIG_FILE = "config.json"

def load_config(config_path=CONFIG_FILE):
    with open(config_path, "r") as f:
        return json.load(f)
        
def save_config(config, config_path=CONFIG_FILE):
    with open(config_path, "w") as f:
        json.dump(config, f, indent=2)

def list_available_com_ports():
    ports = serial.tools.list_ports.comports()
    if not ports:
        return "No active COM ports detected."
    return "\n".join([f"  - {p.device}: {p.description}" for p in ports])

class MakcuColorBot:
    def __init__(self, config_path="config.json"):
        self.config = load_config(config_path)
        
        self.screen_w = win32api.GetSystemMetrics(0)
        self.screen_h = win32api.GetSystemMetrics(1)
        self.center_x = self.screen_w // 2
        self.center_y = self.screen_h // 2
        
        self.kernel = np.ones((3, 3), np.uint8)
        
        # Track sticky memory
        self.locked_target = None
        self.max_sticky_dist = 80
        
        # State tracking for preview window
        self.preview_window_active = False
        self.running = True
        
        self.update_settings(self.config)

        makcu_kwargs = {
            "debug": self.config.get("makcu_debug", False),
            "auto_reconnect": self.config.get("makcu_auto_reconnect", True)
        }
        
        custom_port = self.config.get("makcu_port", "")
        if custom_port:
            makcu_kwargs["fallback_com_port"] = custom_port
            if self.config.get("makcu_override_port", False):
                makcu_kwargs["override_port"] = True

        print(f"[+] Initializing Makcu Hardware Controller (https://pypi.org/project/makcu/)...")
        try:
            self.makcu = create_controller(**makcu_kwargs)
            print("[✓] Makcu Controller successfully connected!")
        except Exception as e:
            print("\n" + "="*65)
            print(f"[!] Critical Error: Failed to initialize Makcu device!")
            print(f"[!] Details: {e}")
            print("="*65)
            print("\n[!] HOW TO FIX IT:")
            print("    1. Unplug your Makcu USB device, wait 3 seconds, and plug it back in.")
            print("    2. Close any open Python scripts, Arduino IDE, or Serial Monitor windows.")
            print("    3. Specify a different port in config.json under 'makcu_port' (e.g., \"COM3\").")
            print("\n[+] Currently Available COM Ports on System:")
            print(list_available_com_ports())
            print("="*65 + "\n")
            sys.exit(1)

    def update_settings(self, config):
        self.config = config
        self.preset_key = self.config.get("active_preset", "purple")
        self.preset = self.config["presets"][self.preset_key]
        
        self.fov_radius = int(self.config.get("fov_radius", 120))
        self.smooth = float(self.config.get("smooth_factor", 0.45))
        self.min_area = float(self.config.get("min_contour_area", 4.0))
        self.offset_x = int(self.config.get("offset_x", 2))
        self.offset_y = int(self.config.get("offset_y", 3))
        self.filter_orbs = self.config.get("filter_orbs", True)
        self.aim_key_code = int(self.config.get("aim_key", "0x05"), 16)
        
        self.grab_w = self.fov_radius * 2
        self.grab_h = self.fov_radius * 2
        self.grab_top = self.center_y - self.fov_radius
        self.grab_left = self.center_x - self.fov_radius
        
        self.grab_region = {
            "top": int(self.grab_top),
            "left": int(self.grab_left),
            "width": int(self.grab_w),
            "height": int(self.grab_h)
        }
        
        self.local_cx = self.fov_radius
        self.local_cy = self.fov_radius
        
        if self.preset_key == "red":
            self.hsv_lower1 = np.array(self.preset["hsv_lower_1"], dtype=np.uint8)
            self.hsv_upper1 = np.array(self.preset["hsv_upper_1"], dtype=np.uint8)
            self.hsv_lower2 = np.array(self.preset["hsv_lower_2"], dtype=np.uint8)
            self.hsv_upper2 = np.array(self.preset["hsv_upper_2"], dtype=np.uint8)
        else:
            self.hsv_lower = np.array(self.preset["hsv_lower"], dtype=np.uint8)
            self.hsv_upper = np.array(self.preset["hsv_upper"], dtype=np.uint8)

    def is_aim_key_pressed(self):
        return (win32api.GetAsyncKeyState(self.aim_key_code) & 0x8000) != 0

    def get_color_mask(self, hsv_frame):
        if self.preset_key == "red":
            mask1 = cv2.inRange(hsv_frame, self.hsv_lower1, self.hsv_upper1)
            mask2 = cv2.inRange(hsv_frame, self.hsv_lower2, self.hsv_upper2)
            mask = cv2.bitwise_or(mask1, mask2)
        else:
            mask = cv2.inRange(hsv_frame, self.hsv_lower, self.hsv_upper)
            
        mask = cv2.morphologyEx(mask, cv2.MORPH_CLOSE, self.kernel)
        mask = cv2.dilate(mask, self.kernel, iterations=1)
        return mask

    def find_target(self, mask):
        h, w = mask.shape
        y_indices, x_indices = np.ogrid[:h, :w]
        dist_from_center = np.hypot(x_indices - self.local_cx, y_indices - self.local_cy)
        fov_mask = np.zeros_like(mask)
        fov_mask[dist_from_center <= self.fov_radius] = mask[dist_from_center <= self.fov_radius]

        contours, _ = cv2.findContours(fov_mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
        if not contours:
            return None, []

        player_blobs = []
        for c in contours:
            area = cv2.contourArea(c)
            if area < self.min_area or area > 2000:
                continue

            x, y, w, h = cv2.boundingRect(c)
            if w > 120 or h > 180:
                continue

            aspect_ratio = float(h) / float(w) if w > 0 else 0
            solidity = float(area) / (w * h) if (w * h) > 0 else 0
            
            if aspect_ratio < 0.25 and w > 20:
                continue
            if 0.70 <= aspect_ratio <= 1.25 and solidity > 0.65 and w > 15:
                continue

            merged = False
            for blob in player_blobs:
                bx, by, bw, bh = blob["bbox"]
                if not (x + w < bx - 12 or x > bx + bw + 12):
                    blob["contours"].append(c)
                    nx = min(x, bx)
                    ny = min(y, by)
                    nw = max(x + w, bx + bw) - nx
                    nh = max(y + h, by + bh) - ny
                    blob["bbox"] = (nx, ny, nw, nh)
                    merged = True
                    break

            if not merged:
                player_blobs.append({"bbox": (x, y, w, h), "contours": [c]})

        if not player_blobs:
            return None, []

        valid_contours = [c for blob in player_blobs for c in blob["contours"]]
        player_heads = []

        for blob in player_blobs:
            blob_mask = np.zeros_like(fov_mask)
            cv2.drawContours(blob_mask, blob["contours"], -1, 255, -1)
            pts = cv2.findNonZero(blob_mask)
            if pts is None:
                continue
            
            coords = pts.reshape(-1, 2)
            c_min_y = np.min(coords[:, 1])
            c_top_pixels = coords[coords[:, 1] <= c_min_y + 3]
            
            head_x = int(np.mean(c_top_pixels[:, 0]) + self.offset_x)
            head_y = int(c_min_y + self.offset_y)
            
            dist_to_crosshair = np.hypot(head_x - self.local_cx, head_y - self.local_cy)
            player_heads.append((head_x, head_y, dist_to_crosshair))

        if not player_heads:
            return None, valid_contours

        player_heads.sort(key=lambda p: p[2])
        best_head_x, best_head_y, _ = player_heads[0]

        return (best_head_x, best_head_y), valid_contours

    def run(self):
        print(f"[+] ColorBot Running headless mode...")
        print(f"[+] Reading settings directly from config.json...")
        
        with mss.mss() as sct:
            while self.running:
                img = np.array(sct.grab(self.grab_region))
                bgr = cv2.cvtColor(img, cv2.COLOR_BGRA2BGR)
                hsv = cv2.cvtColor(bgr, cv2.COLOR_BGR2HSV)
                
                mask = self.get_color_mask(hsv)
                key_pressed = self.is_aim_key_pressed()
                target, contours = self.find_target(mask)

                if key_pressed and target is not None:
                    tx, ty = target
                    dx = int((tx - self.local_cx) * self.smooth)
                    dy = int((ty - self.local_cy) * self.smooth)

                    if dx != 0 or dy != 0:
                        try:
                            self.makcu.move(dx, dy)
                        except Exception as err:
                            print(f"[!] Makcu send error: {err}")

    def stop(self):
        self.running = False


if __name__ == "__main__":
    bot = MakcuColorBot()
    try:
        bot.run()
    except KeyboardInterrupt:
        print("\n[+] Exiting...")
        bot.stop()
