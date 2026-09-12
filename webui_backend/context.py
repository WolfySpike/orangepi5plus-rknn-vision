import json
import os
import re
import shutil
import shlex
import signal
import socket
import subprocess
import threading
import time
import copy
from urllib.parse import quote


class BackendContext:
    MODEL_FAMILY_MAP = {"yolov8": 8, "yolov11": 11, "yolov26": 26}
    DEFAULT_CLASS_LABELS = [
        "Body",
        "Head",
        "Thermal",
        "Teammate",
        "Bot",
        "Knocked",
        "D_Body",
        "D_Head",
        "Neg",
    ]
    DEPRECATED_PARAMS = {"ki_x", "ki_y", "i_limit"}
    DEFAULT_PARAMS = {
        "kp_x": 0.25,
        "kd_x": 0.10,
        "kp_y": 0.25,
        "kd_y": 0.10,
        "recoil": 2.0,
        "max_step": 10.0,
        "deadzone": 3.0,
        "aim_slow_radius": 45.0,
        "aim_slow_min_scale": 0.35,
        "pred_en": 1,
        "pred_lead_ms": 28.0,
        "pred_vel_smooth": 0.35,
        "pred_pos_smooth": 0.55,
        "pred_max_speed": 550.0,
        "conf_thres": 0.20,
        "nms_iou": 0.30,
        "aim_mask": 3,
        "trigger_mask": 4,
        "aim_fov": 180.0,
        "trigger_radius": 8.0,
        "trigger_delay_ms": 0.0,
        "preview_en": 0,
        "preview_fps": 20.0,
        "xhair_en": 0,
        "xhair_c": 0,
        "xhair_h_min": 170,
        "xhair_h_max": 10,
        "xhair_s_min": 120,
        "xhair_s_max": 255,
        "xhair_v_min": 120,
        "xhair_v_max": 255,
    }
    DEFAULT_RUNTIME = {
        "model_family": "yolov8",
        "num_classes": 9,
        "selected_model_id": "root:sjzdawan.rknn",
        "class_names": DEFAULT_CLASS_LABELS[:],
        "active_config": "default",
        "npu_mode": "latency",
        "xhair_selected_preset": "Red",
        "xhair_presets": [
            {"name": "Red", "h_min": 170, "h_max": 10, "s_min": 120, "s_max": 255, "v_min": 120, "v_max": 255},
            {"name": "Green", "h_min": 35, "h_max": 85, "s_min": 120, "s_max": 255, "v_min": 120, "v_max": 255},
            {"name": "Blue", "h_min": 85, "h_max": 130, "s_min": 120, "s_max": 255, "v_min": 120, "v_max": 255},
        ],
        "streamer": {
            "host": "192.168.3.113",
            "port": 9999,
            "protocol": "udp",
            "width": 416,
            "height": 416,
            "fps": 240,
            "quality": 85,
            "pkt_size": 1316,
            "src_width": 1920,
            "src_height": 1080,
            "src_format": "BGR",
            "idct_method": 1,
        },
    }

    def __init__(self, base_dir):
        self.base_dir = os.path.abspath(base_dir)
        self.build_dir = os.path.join(self.base_dir, "build")
        self.lib_dir = os.path.join(self.base_dir, "lib")
        self.model_dir = os.path.join(self.base_dir, "models")
        self.configs_dir = os.path.join(self.base_dir, "configs")
        self.config_file = os.path.join(self.base_dir, "pid_config.json")
        self.runtime_file = os.path.join(self.base_dir, "runtime_config.json")
        self.log_dir = os.path.join(self.base_dir, "logs")
        self.engine_log_file = os.path.join(self.log_dir, "engine.log")
        self.streamer_log_file = os.path.join(self.log_dir, "streamer.log")

        self.udp_ip = "127.0.0.1"
        self.udp_port_cmd = 9999
        self.udp_port_telemetry = 9998
        self.sock_cmd = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

        os.makedirs(self.model_dir, exist_ok=True)
        os.makedirs(self.configs_dir, exist_ok=True)
        os.makedirs(self.log_dir, exist_ok=True)

        self.params = dict(self.DEFAULT_PARAMS)
        self.runtime_cfg = copy.deepcopy(self.DEFAULT_RUNTIME)
        self.telemetry_data = {
            "fps": 0,
            "infer_time": 0.0,
            "core": 0,
            "is_aiming": 0,
            "cls": -1,
            "conf": 0.0,
        }
        self.system_stats = {"cpu": 0.0, "mem": 0.0, "disk": 0.0, "npu": -1.0}

        self.aimbot_process = None
        self.engine_last_error = ""
        self.engine_last_exit_code = None
        self.streamer_process = None
        self.streamer_last_error = ""
        self.streamer_last_exit_code = None
        self.streamer_stats = {"fps": 0.0, "frame": 0, "started_at": 0.0, "last_update": 0.0}

        self.state_lock = threading.Lock()
        self._threads_started = False

    def build_model_list(self):
        models = []
        for name in sorted(os.listdir(self.base_dir)):
            if name.lower().endswith(".rknn"):
                models.append(
                    {
                        "id": f"root:{name}",
                        "name": f"{name} (项目目录)",
                        "path": os.path.join(self.base_dir, name),
                    }
                )
        for name in sorted(os.listdir(self.model_dir)):
            if name.lower().endswith(".rknn"):
                models.append(
                    {
                        "id": f"models:{name}",
                        "name": f"{name} (上传模型)",
                        "path": os.path.join(self.model_dir, name),
                    }
                )
        return models

    def resolve_model_path(self, model_id):
        if model_id.startswith("root:"):
            return os.path.join(self.base_dir, model_id.split(":", 1)[1])
        if model_id.startswith("models:"):
            return os.path.join(self.model_dir, model_id.split(":", 1)[1])
        return os.path.join(self.base_dir, "sjzdawan.rknn")

    def default_class_name(self, idx):
        if idx < len(self.DEFAULT_CLASS_LABELS):
            return self.DEFAULT_CLASS_LABELS[idx]
        return f"Class_{idx}"

    def _sanitize_hsv_int(self, value, lo, hi, default):
        try:
            return max(lo, min(hi, int(round(float(value)))))
        except Exception:
            return default

    def sanitize_xhair_preset(self, preset):
        if not isinstance(preset, dict):
            return None
        name = str(preset.get("name", "")).strip()[:32]
        if not name:
            return None
        return {
            "name": name,
            "h_min": self._sanitize_hsv_int(preset.get("h_min"), 0, 180, 170),
            "h_max": self._sanitize_hsv_int(preset.get("h_max"), 0, 180, 10),
            "s_min": self._sanitize_hsv_int(preset.get("s_min"), 0, 255, 120),
            "s_max": self._sanitize_hsv_int(preset.get("s_max"), 0, 255, 255),
            "v_min": self._sanitize_hsv_int(preset.get("v_min"), 0, 255, 120),
            "v_max": self._sanitize_hsv_int(preset.get("v_max"), 0, 255, 255),
        }

    def ensure_xhair_presets(self):
        raw = self.runtime_cfg.get("xhair_presets", [])
        if not isinstance(raw, list):
            raw = []
        merged = []
        seen = set()
        for p in raw + self.DEFAULT_RUNTIME["xhair_presets"]:
            clean = self.sanitize_xhair_preset(p)
            if not clean:
                continue
            key = clean["name"].lower()
            if key in seen:
                continue
            seen.add(key)
            merged.append(clean)
        if not merged:
            merged = [dict(self.DEFAULT_RUNTIME["xhair_presets"][0])]
        selected = str(self.runtime_cfg.get("xhair_selected_preset", merged[0]["name"])).strip()
        if selected.lower() not in {p["name"].lower() for p in merged}:
            selected = merged[0]["name"]
        self.runtime_cfg["xhair_presets"] = merged
        self.runtime_cfg["xhair_selected_preset"] = selected

    def current_xhair_preset_from_params(self, name):
        return {
            "name": str(name or "").strip()[:32],
            "h_min": self._sanitize_hsv_int(self.params.get("xhair_h_min"), 0, 180, 170),
            "h_max": self._sanitize_hsv_int(self.params.get("xhair_h_max"), 0, 180, 10),
            "s_min": self._sanitize_hsv_int(self.params.get("xhair_s_min"), 0, 255, 120),
            "s_max": self._sanitize_hsv_int(self.params.get("xhair_s_max"), 0, 255, 255),
            "v_min": self._sanitize_hsv_int(self.params.get("xhair_v_min"), 0, 255, 120),
            "v_max": self._sanitize_hsv_int(self.params.get("xhair_v_max"), 0, 255, 255),
        }

    def apply_xhair_preset_to_params(self, preset):
        clean = self.sanitize_xhair_preset(preset)
        if not clean:
            return False
        for key in ("h_min", "h_max", "s_min", "s_max", "v_min", "v_max"):
            self.params[f"xhair_{key}"] = float(clean[key])
        self.runtime_cfg["xhair_selected_preset"] = clean["name"]
        return True

    def sanitize_streamer_config(self, raw=None):
        defaults = copy.deepcopy(self.DEFAULT_RUNTIME["streamer"])
        cfg = dict(defaults)
        if isinstance(raw, dict):
            cfg.update(raw)

        def clamp_int(key, lo, hi):
            try:
                value = int(round(float(cfg.get(key, defaults[key]))))
            except Exception:
                value = defaults[key]
            cfg[key] = max(lo, min(hi, value))

        host = str(cfg.get("host", defaults["host"])).strip()
        if not re.fullmatch(r"[A-Za-z0-9_.:-]{1,128}", host):
            host = defaults["host"]
        cfg["host"] = host

        protocol = str(cfg.get("protocol", defaults["protocol"])).strip().lower()
        cfg["protocol"] = protocol if protocol in {"udp", "tcp"} else defaults["protocol"]

        src_format = str(cfg.get("src_format", defaults["src_format"])).strip().upper()
        cfg["src_format"] = src_format if src_format in {"BGR", "RGB", "BGR3"} else defaults["src_format"]

        clamp_int("port", 1, 65535)
        clamp_int("width", 64, 1920)
        clamp_int("height", 64, 1080)
        clamp_int("fps", 1, 240)
        clamp_int("quality", 1, 100)
        clamp_int("pkt_size", 256, 65507)
        clamp_int("src_width", 320, 7680)
        clamp_int("src_height", 240, 4320)
        clamp_int("idct_method", 0, 2)
        return cfg

    def normalize_profile_name(self, name):
        raw = str(name or "").strip()
        if raw.lower().endswith(".json"):
            raw = raw[:-5]
        # \w is Unicode-aware in Python 3, so Chinese profile names are kept
        # without relying on a fragile literal Chinese character range.
        raw = re.sub(r"[^\w.\-]", "_", raw, flags=re.UNICODE)
        raw = raw.strip(" ._-")
        if not raw:
            return None
        return raw[:64]

    def profile_path(self, profile_name):
        safe = self.normalize_profile_name(profile_name)
        if not safe:
            return None
        p = os.path.abspath(os.path.join(self.configs_dir, f"{safe}.json"))
        root = os.path.abspath(self.configs_dir)
        try:
            if os.path.commonpath([root, p]) != root:
                return None
        except ValueError:
            return None
        return p

    def list_profiles(self):
        names = []
        for fn in sorted(os.listdir(self.configs_dir)):
            if fn.lower().endswith(".json"):
                names.append(fn[:-5])
        if "default" not in names:
            names.insert(0, "default")
        return sorted(set(names), key=lambda x: (x != "default", x.lower()))

    def _write_json(self, path, data):
        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)

    def _profile_payload(self):
        return {
            "params": dict(self.params),
            "runtime": {
                "model_family": self.runtime_cfg.get("model_family", "yolov8"),
                "num_classes": int(self.runtime_cfg.get("num_classes", 9)),
                "selected_model_id": self.runtime_cfg.get("selected_model_id", "root:sjzdawan.rknn"),
                "class_names": list(self.runtime_cfg.get("class_names", [])),
                "npu_mode": self.runtime_cfg.get("npu_mode", "latency"),
                "xhair_selected_preset": self.runtime_cfg.get("xhair_selected_preset", "Red"),
                "xhair_presets": list(self.runtime_cfg.get("xhair_presets", [])),
                "streamer": self.sanitize_streamer_config(self.runtime_cfg.get("streamer", {})),
            },
        }

    def _save_profile_only(self, profile_name):
        p = self.profile_path(profile_name)
        if not p:
            return False
        self._write_json(p, self._profile_payload())
        return True

    def _load_profile(self, profile_name):
        p = self.profile_path(profile_name)
        if not p or not os.path.exists(p):
            return None
        with open(p, "r", encoding="utf-8-sig") as f:
            data = json.load(f)
        if isinstance(data, dict):
            return data
        return None

    def _valid_model_ids(self):
        return {m["id"] for m in self.build_model_list()}

    def _apply_profile_data(self, profile_data):
        loaded_params = profile_data.get("params", {})
        if isinstance(loaded_params, dict):
            self.params.clear()
            self.params.update(self.DEFAULT_PARAMS)
            for k, v in loaded_params.items():
                if k in self.DEPRECATED_PARAMS:
                    continue
                try:
                    self.params[k] = float(v)
                except Exception:
                    pass

        loaded_runtime = profile_data.get("runtime", {})
        if isinstance(loaded_runtime, dict):
            self.runtime_cfg["model_family"] = str(
                loaded_runtime.get("model_family", self.runtime_cfg.get("model_family", "yolov8"))
            )
            self.runtime_cfg["num_classes"] = int(
                loaded_runtime.get("num_classes", self.runtime_cfg.get("num_classes", 9))
            )
            self.runtime_cfg["selected_model_id"] = str(
                loaded_runtime.get(
                    "selected_model_id",
                    self.runtime_cfg.get("selected_model_id", "root:sjzdawan.rknn"),
                )
            )
            self.runtime_cfg["class_names"] = loaded_runtime.get(
                "class_names", self.runtime_cfg.get("class_names", [])
            )
            self.runtime_cfg["npu_mode"] = str(
                loaded_runtime.get("npu_mode", self.runtime_cfg.get("npu_mode", "latency"))
            )
            self.runtime_cfg["xhair_selected_preset"] = str(
                loaded_runtime.get(
                    "xhair_selected_preset",
                    self.runtime_cfg.get("xhair_selected_preset", "Red"),
                )
            )
            self.runtime_cfg["xhair_presets"] = loaded_runtime.get(
                "xhair_presets", self.runtime_cfg.get("xhair_presets", [])
            )
            self.runtime_cfg["streamer"] = self.sanitize_streamer_config(
                loaded_runtime.get("streamer", self.runtime_cfg.get("streamer", {}))
            )

        self.runtime_cfg["num_classes"] = max(1, int(self.runtime_cfg.get("num_classes", 9)))
        if self.runtime_cfg.get("model_family") not in self.MODEL_FAMILY_MAP:
            self.runtime_cfg["model_family"] = "yolov8"
        if self.runtime_cfg.get("npu_mode") not in ("latency", "throughput"):
            self.runtime_cfg["npu_mode"] = "latency"
        if self.runtime_cfg.get("selected_model_id") not in self._valid_model_ids():
            self.runtime_cfg["selected_model_id"] = "root:sjzdawan.rknn"
        self.ensure_class_params(self.runtime_cfg["num_classes"])
        self.ensure_class_names(self.runtime_cfg["num_classes"])
        self.ensure_xhair_presets()
        self.runtime_cfg["streamer"] = self.sanitize_streamer_config(self.runtime_cfg.get("streamer", {}))

    def ensure_class_params(self, class_count):
        class_count = max(1, int(class_count))
        for i in range(class_count):
            self.params.setdefault(f"c{i}_en", 1)
            self.params.setdefault(f"c{i}_hr", 0.70 if i in (1, 7) else 0.30)
            self.params.setdefault(f"c{i}_py", 0)
            self.params.setdefault(f"c{i}_pri", 0 if i in (1, 7) else 10)

        p = re.compile(r"^c(\d+)_(en|hr|py|pri)$")
        for key in list(self.params.keys()):
            m = p.match(key)
            if m and int(m.group(1)) >= class_count:
                del self.params[key]

    def ensure_class_names(self, class_count):
        class_count = max(1, int(class_count))
        existing = self.runtime_cfg.get("class_names", [])
        if not isinstance(existing, list):
            existing = []
        out = []
        for i in range(class_count):
            if i < len(existing) and isinstance(existing[i], str) and existing[i].strip():
                out.append(existing[i].strip()[:32])
            else:
                out.append(self.default_class_name(i))
        self.runtime_cfg["class_names"] = out

    def drop_deprecated_params(self):
        for key in self.DEPRECATED_PARAMS:
            self.params.pop(key, None)

    def save_params(self):
        self.drop_deprecated_params()
        try:
            self._write_json(self.config_file, self.params)
        except Exception:
            pass
        self._save_profile_only(self.runtime_cfg.get("active_config", "default"))

    def save_runtime(self):
        try:
            self._write_json(self.runtime_file, self.runtime_cfg)
        except Exception:
            pass
        self._save_profile_only(self.runtime_cfg.get("active_config", "default"))

    def load_state_files(self):
        if os.path.exists(self.config_file):
            try:
                with open(self.config_file, "r", encoding="utf-8-sig") as f:
                    loaded = json.load(f)
                    if isinstance(loaded, dict):
                        for k, v in loaded.items():
                            if k not in self.DEPRECATED_PARAMS:
                                self.params[k] = v
            except Exception:
                pass

        if os.path.exists(self.runtime_file):
            try:
                with open(self.runtime_file, "r", encoding="utf-8-sig") as f:
                    loaded = json.load(f)
                    if isinstance(loaded, dict):
                        self.runtime_cfg.update(loaded)
            except Exception:
                pass

        self.runtime_cfg["num_classes"] = max(1, int(self.runtime_cfg.get("num_classes", 9)))
        if self.runtime_cfg.get("model_family") not in self.MODEL_FAMILY_MAP:
            self.runtime_cfg["model_family"] = "yolov8"
        if self.runtime_cfg.get("npu_mode") not in ("latency", "throughput"):
            self.runtime_cfg["npu_mode"] = "latency"
        if self.runtime_cfg.get("selected_model_id") not in self._valid_model_ids():
            self.runtime_cfg["selected_model_id"] = "root:sjzdawan.rknn"

        self.runtime_cfg["active_config"] = (
            self.normalize_profile_name(self.runtime_cfg.get("active_config", "default")) or "default"
        )
        self.ensure_class_params(self.runtime_cfg["num_classes"])
        self.ensure_class_names(self.runtime_cfg["num_classes"])
        self.ensure_xhair_presets()
        self.runtime_cfg["streamer"] = self.sanitize_streamer_config(self.runtime_cfg.get("streamer", {}))
        self.drop_deprecated_params()

        default_file = self.profile_path("default")
        if default_file and not os.path.exists(default_file):
            self._save_profile_only("default")

        active = self.runtime_cfg.get("active_config", "default")
        loaded_profile = self._load_profile(active)
        if loaded_profile is None and active != "default":
            self.runtime_cfg["active_config"] = "default"
            loaded_profile = self._load_profile("default")
        if loaded_profile is not None:
            self._apply_profile_data(loaded_profile)

        self.runtime_cfg["active_config"] = (
            self.normalize_profile_name(self.runtime_cfg.get("active_config", "default")) or "default"
        )
        if self.runtime_cfg["active_config"] not in self.list_profiles():
            self.runtime_cfg["active_config"] = "default"
        self.ensure_class_params(self.runtime_cfg["num_classes"])
        self.ensure_class_names(self.runtime_cfg["num_classes"])
        self.ensure_xhair_presets()
        self.runtime_cfg["streamer"] = self.sanitize_streamer_config(self.runtime_cfg.get("streamer", {}))
        self.drop_deprecated_params()

    def build_udp_payload(self):
        payload = dict(self.params)
        payload["num_classes"] = int(self.runtime_cfg["num_classes"])
        payload["model_family"] = self.MODEL_FAMILY_MAP.get(self.runtime_cfg["model_family"], 8)
        parts = [f"{k}={v}" for k, v in payload.items()]
        class_names = self.runtime_cfg.get("class_names", [])
        class_count = int(self.runtime_cfg["num_classes"])
        for i in range(class_count):
            name = ""
            if isinstance(class_names, list) and i < len(class_names):
                raw_name = str(class_names[i]).strip()
                if raw_name and raw_name != self.default_class_name(i):
                    name = raw_name
            parts.append(f"label{i}={quote(name, safe='')}")
        return ";".join(parts) + ";"

    def send_params_to_cpp(self):
        try:
            self.sock_cmd.sendto(self.build_udp_payload().encode(), (self.udp_ip, self.udp_port_cmd))
        except Exception:
            pass

    def _tail_text(self, path, max_lines=30):
        try:
            with open(path, "r", encoding="utf-8", errors="ignore") as f:
                lines = f.readlines()
            if not lines:
                return ""
            return "".join(lines[-max_lines:]).strip()
        except Exception:
            return ""

    def _refresh_engine_state(self):
        if self.aimbot_process is None:
            return
        code = self.aimbot_process.poll()
        if code is None:
            return
        self.engine_last_exit_code = code
        if not self.engine_last_error:
            self.engine_last_error = f"引擎进程已退出，退出码: {code}"

    def start_engine(self):
        if self.aimbot_process is not None and self.aimbot_process.poll() is None:
            return True, "already running"

        model_path = self.resolve_model_path(self.runtime_cfg["selected_model_id"])
        if not os.path.exists(model_path):
            model_path = os.path.join(self.base_dir, "sjzdawan.rknn")
        if not os.path.exists(model_path):
            self.engine_last_error = f"模型文件不存在: {model_path}"
            return False, self.engine_last_error

        bin_path = os.path.join(self.build_dir, "aimbot_main")
        if not os.path.exists(bin_path):
            self.engine_last_error = f"找不到引擎可执行文件: {bin_path}"
            return False, self.engine_last_error

        family_code = self.MODEL_FAMILY_MAP.get(self.runtime_cfg["model_family"], 8)
        class_count = int(self.runtime_cfg["num_classes"])
        npu_mode = self.runtime_cfg.get("npu_mode", "latency")
        npu_cores = 1 if npu_mode == "latency" else 3

        env = os.environ.copy()
        prev_ld = env.get("LD_LIBRARY_PATH", "")
        env["LD_LIBRARY_PATH"] = f"{self.lib_dir}:{prev_ld}" if prev_ld else self.lib_dir
        self.engine_last_error = ""
        self.engine_last_exit_code = None
        cmd = [
            "./aimbot_main",
            "--model",
            model_path,
            "--classes",
            str(class_count),
            "--family",
            str(family_code),
            "--npu-cores",
            str(npu_cores),
        ]

        try:
            with open(self.engine_log_file, "a", encoding="utf-8") as logf:
                logf.write(
                    f"\n\n===== START {time.strftime('%Y-%m-%d %H:%M:%S')} =====\n"
                    f"cwd={self.build_dir}\ncmd={' '.join(cmd)}\nmodel={model_path}\n"
                )
                logf.flush()
                self.aimbot_process = subprocess.Popen(
                    cmd,
                    cwd=self.build_dir,
                    env=env,
                    stdout=logf,
                    stderr=logf,
                )
            time.sleep(0.2)
            code = self.aimbot_process.poll()
            if code is not None:
                self.engine_last_exit_code = code
                tail = self._tail_text(self.engine_log_file)
                self.engine_last_error = f"引擎启动后立刻退出，退出码: {code}"
                if tail:
                    self.engine_last_error += f"\n日志末尾:\n{tail}"
                return False, self.engine_last_error
        except Exception as e:
            self.engine_last_error = str(e)
            return False, self.engine_last_error

        threading.Timer(0.5, self.send_params_to_cpp).start()
        return True, "ok"

    def stop_engine(self):
        if self.aimbot_process is not None and self.aimbot_process.poll() is None:
            self.aimbot_process.terminate()
            try:
                self.aimbot_process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.aimbot_process.kill()
                self.aimbot_process.wait(timeout=2)
        self.telemetry_data["fps"] = 0

    def _refresh_streamer_state(self):
        if self.streamer_process is None:
            return
        code = self.streamer_process.poll()
        if code is None:
            return
        self.streamer_last_exit_code = code
        if code != 0 and not self.streamer_last_error:
            self.streamer_last_error = f"发射器进程已退出，退出码: {code}"

    def build_streamer_command(self, cfg):
        width = int(cfg["width"])
        height = int(cfg["height"])
        src_width = int(cfg["src_width"])
        src_height = int(cfg["src_height"])
        crop_left = max(0, (src_width - width) // 2)
        crop_right = max(0, src_width - width - crop_left)
        crop_top = max(0, (src_height - height) // 2)
        crop_bottom = max(0, src_height - height - crop_top)
        src_format = str(cfg["src_format"])
        if src_format == "BGR3":
            src_format = "BGR"
        if cfg.get("protocol") == "tcp":
            host = shlex.quote(str(cfg["host"]))
            port = int(cfg["port"])
            fps = int(cfg["fps"])
            quality = int(cfg["quality"])
            gst_command = " ".join(
                [
                    "gst-launch-1.0",
                    'v4l2src device="$VIDEO_DEV" io-mode=2 !',
                    f"video/x-raw,format={shlex.quote(src_format)},width={src_width},height={src_height},framerate={fps}/1 !",
                    f"videocrop top={crop_top} bottom={crop_bottom} left={crop_left} right={crop_right} !",
                    "queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 leaky=downstream !",
                    f"jpegenc quality={quality} !",
                    f"tcpclientsink host={host} port={port} sync=false",
                ]
            )
            script = [
                "export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
                "sysctl -w net.ipv4.tcp_notsent_lowat=16384 >/dev/null 2>&1 || true",
                "sysctl -w net.ipv4.tcp_slow_start_after_idle=0 >/dev/null 2>&1 || true",
                'NET_INTERFACE="$(ip route 2>/dev/null | awk \'/default/ {print $5; exit}\')"',
                'if [ -n "$NET_INTERFACE" ]; then',
                '  ip link set dev "$NET_INTERFACE" txqueuelen 10000 >/dev/null 2>&1 || { command -v ifconfig >/dev/null 2>&1 && ifconfig "$NET_INTERFACE" txqueuelen 10000 >/dev/null 2>&1; } || true',
                "fi",
                "VIDEO_DEV=\"$(v4l2-ctl --list-devices 2>/dev/null | awk 'BEGIN{hit=0} /hdmirx|rk_hdmirx/{hit=1; next} hit && /\\/dev\\/video/{print $1; exit}')\"",
                '[ -n "$VIDEO_DEV" ] || VIDEO_DEV="/dev/video0"',
                f'echo "[streamer] TCP MJPEG target {cfg["host"]}:{port}, crop={width}x{height}, fps={fps}, quality={quality}"',
                'while [ ! -e "$VIDEO_DEV" ]; do',
                '  echo "[streamer] waiting for HDMI-RX device: $VIDEO_DEV"',
                "  sleep 2",
                "done",
                'if command -v chrt >/dev/null 2>&1 && chrt -f 99 true >/dev/null 2>&1; then',
                '  CHRT_CMD="chrt -f 99"',
                '  echo "[streamer] realtime scheduler: chrt -f 99"',
                "else",
                '  CHRT_CMD=""',
                '  echo "[streamer] warning: chrt -f 99 unavailable; start WebUI with sudo for lowest latency"',
                "fi",
                "while true; do",
                '  v4l2-ctl -d "$VIDEO_DEV" --set-dv-bt-timings query >/dev/null 2>&1 || true',
                f"  $CHRT_CMD {gst_command}",
                '  echo "[streamer] TCP connection ended; retrying in 2 seconds..."',
                "  sleep 2",
                "done",
            ]
            return "\n".join(script)

        url = f"udp://{cfg['host']}:{int(cfg['port'])}?pkt_size={int(cfg['pkt_size'])}"
        parts = [
            "gst-launch-1.0 -q",
            f"v4l2src device={shlex.quote('/dev/video0')} !",
            f"video/x-raw,format={shlex.quote(src_format)},width={src_width},height={src_height} !",
            f"videocrop top={crop_top} left={crop_left} right={crop_right} bottom={crop_bottom} !",
            f"jpegenc idct-method={int(cfg['idct_method'])} quality={int(cfg['quality'])} !",
            "fdsink fd=1 sync=false |",
            "ffmpeg -nostdin -stats -loglevel error -fflags nobuffer -flags low_delay -y",
            f"-f mjpeg -framerate {int(cfg['fps'])} -i pipe:0",
            "-c copy -flush_packets 1",
            f"-f mjpeg {shlex.quote(url)}",
        ]
        return " ".join(parts)

    def _streamer_monitor(self, proc):
        buf = ""
        fps_re = re.compile(r"fps=\s*([0-9.]+)")
        frame_re = re.compile(r"frame=\s*(\d+)")
        try:
            with open(self.streamer_log_file, "a", encoding="utf-8", errors="ignore") as logf:
                while proc.stderr:
                    ch = proc.stderr.read(1)
                    if not ch:
                        break
                    if ch in "\r\n":
                        line = buf.strip()
                        buf = ""
                        if not line:
                            continue
                        logf.write(line + "\n")
                        logf.flush()
                        fm = fps_re.search(line)
                        frm = frame_re.search(line)
                        with self.state_lock:
                            if fm:
                                try:
                                    self.streamer_stats["fps"] = float(fm.group(1))
                                except Exception:
                                    pass
                            if frm:
                                try:
                                    self.streamer_stats["frame"] = int(frm.group(1))
                                except Exception:
                                    pass
                            self.streamer_stats["last_update"] = time.time()
                    else:
                        buf += ch
        except Exception as e:
            with self.state_lock:
                if not self.streamer_last_error:
                    self.streamer_last_error = str(e)

    def start_streamer(self, cfg=None):
        self._refresh_streamer_state()
        if self.streamer_process is not None and self.streamer_process.poll() is None:
            return True, "already running"
        clean = self.sanitize_streamer_config(cfg or self.runtime_cfg.get("streamer", {}))
        self.runtime_cfg["streamer"] = clean
        self.save_runtime()
        if shutil.which("gst-launch-1.0") is None:
            self.streamer_last_error = "找不到 gst-launch-1.0"
            return False, self.streamer_last_error
        if clean.get("protocol") == "udp" and shutil.which("ffmpeg") is None:
            self.streamer_last_error = "找不到 ffmpeg"
            return False, self.streamer_last_error
        cmd = self.build_streamer_command(clean)
        self.streamer_last_error = ""
        self.streamer_last_exit_code = None
        self.streamer_stats = {"fps": 0.0, "frame": 0, "started_at": time.time(), "last_update": 0.0}
        try:
            with open(self.streamer_log_file, "a", encoding="utf-8", errors="ignore") as logf:
                logf.write(f"\n\n===== STREAM START {time.strftime('%Y-%m-%d %H:%M:%S')} =====\ncmd={cmd}\n")
                logf.flush()
                kwargs = {
                    "cwd": self.base_dir,
                    "stdout": logf,
                    "stderr": subprocess.PIPE,
                    "text": True,
                    "bufsize": 0,
                    "shell": True,
                    "executable": "/bin/bash",
                }
                if os.name == "posix":
                    kwargs["preexec_fn"] = os.setsid
                self.streamer_process = subprocess.Popen(cmd, **kwargs)
            threading.Thread(target=self._streamer_monitor, args=(self.streamer_process,), daemon=True).start()
            time.sleep(0.2)
            code = self.streamer_process.poll()
            if code is not None:
                self.streamer_last_exit_code = code
                tail = self._tail_text(self.streamer_log_file, max_lines=30)
                self.streamer_last_error = f"发射器启动后立即退出，退出码: {code}"
                if tail:
                    self.streamer_last_error += f"\n日志末尾:\n{tail}"
                return False, self.streamer_last_error
            return True, "ok"
        except PermissionError as e:
            self.streamer_last_error = (
                f"无法写入发射器日志: {self.streamer_log_file}\n"
                f"{e}\n"
                "请修复 logs 目录权限，或使用 sudo -E 启动 WebUI。"
            )
            return False, self.streamer_last_error
        except Exception as e:
            self.streamer_last_error = str(e)
            return False, self.streamer_last_error

    def stop_streamer(self):
        if self.streamer_process is not None and self.streamer_process.poll() is None:
            try:
                if os.name == "posix":
                    os.killpg(os.getpgid(self.streamer_process.pid), signal.SIGTERM)
                else:
                    self.streamer_process.terminate()
                self.streamer_process.wait(timeout=3)
            except Exception:
                try:
                    if os.name == "posix":
                        os.killpg(os.getpgid(self.streamer_process.pid), signal.SIGKILL)
                    else:
                        self.streamer_process.kill()
                    self.streamer_process.wait(timeout=2)
                except Exception:
                    pass
        self.streamer_stats["fps"] = 0.0

    def get_streamer_state(self):
        self._refresh_streamer_state()
        running = self.streamer_process is not None and self.streamer_process.poll() is None
        return {
            "running": running,
            "config": self.sanitize_streamer_config(self.runtime_cfg.get("streamer", {})),
            "fps": self.streamer_stats.get("fps", 0.0) if running else 0.0,
            "frame": self.streamer_stats.get("frame", 0),
            "started_at": self.streamer_stats.get("started_at", 0.0),
            "last_update": self.streamer_stats.get("last_update", 0.0),
            "error": self.streamer_last_error,
            "exit_code": self.streamer_last_exit_code,
            "log_path": self.streamer_log_file,
        }

    def restart_engine_if_running(self):
        if self.aimbot_process is not None and self.aimbot_process.poll() is None:
            self.stop_engine()
            time.sleep(0.2)
            self.start_engine()

    def run_system_command(self, action):
        cmd_map = {
            "shutdown": ["sudo", "-n", "shutdown", "-h", "now"],
            "reboot": ["sudo", "-n", "reboot"],
            "edid": ["sudo", "-n", "/usr/local/sbin/apply-edid-240hz.sh"],
        }
        cmd = cmd_map.get(action)
        if cmd is None:
            return False, "unsupported action"
        try:
            subprocess.run(cmd, capture_output=True, text=True, check=True)
            return True, "ok"
        except subprocess.CalledProcessError as e:
            msg = (e.stderr or e.stdout or "").strip()
            return False, msg if msg else "sudo command failed"
        except Exception as e:
            return False, str(e)

    def telemetry_listener(self):
        sock_tele = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock_tele.bind(("127.0.0.1", self.udp_port_telemetry))
        while True:
            try:
                data, _ = sock_tele.recvfrom(256)
                parts = data.decode("utf-8").split(",")
                if len(parts) == 6:
                    self.telemetry_data["fps"] = int(parts[0])
                    self.telemetry_data["infer_time"] = float(parts[1])
                    self.telemetry_data["core"] = int(parts[2])
                    self.telemetry_data["is_aiming"] = int(parts[3])
                    self.telemetry_data["cls"] = int(parts[4])
                    self.telemetry_data["conf"] = float(parts[5])
            except Exception:
                pass

    def _read_cpu_times(self):
        try:
            with open("/proc/stat", "r", encoding="utf-8", errors="ignore") as f:
                line = f.readline().strip()
            parts = line.split()
            if len(parts) < 5 or parts[0] != "cpu":
                return None, None
            vals = [int(x) for x in parts[1:8]]
            idle = vals[3] + vals[4]
            total = sum(vals)
            return total, idle
        except Exception:
            return None, None

    def _read_mem_usage(self):
        try:
            total = 0
            avail = 0
            with open("/proc/meminfo", "r", encoding="utf-8", errors="ignore") as f:
                for line in f:
                    if line.startswith("MemTotal:"):
                        total = int(line.split()[1])
                    elif line.startswith("MemAvailable:"):
                        avail = int(line.split()[1])
                    if total > 0 and avail > 0:
                        break
            if total <= 0:
                return 0.0
            used = max(0, total - avail)
            return used * 100.0 / total
        except Exception:
            return 0.0

    def _read_disk_usage(self):
        try:
            total, used, _free = shutil.disk_usage(self.base_dir)
            if total <= 0:
                return 0.0
            return used * 100.0 / total
        except Exception:
            return 0.0

    def _read_npu_usage(self):
        paths = [
            "/sys/kernel/debug/rknpu/load",
            "/sys/class/devfreq/fdab0000.npu/load",
            "/sys/class/devfreq/fde40000.npu/load",
            "/sys/class/devfreq/rknpu/load",
            "/sys/devices/platform/fdab0000.npu/devfreq/fdab0000.npu/load",
        ]
        for p in paths:
            try:
                if not os.path.exists(p):
                    continue
                with open(p, "r", encoding="utf-8", errors="ignore") as f:
                    txt = f.read().strip()
                # common formats:
                # 1) "37@1000000000Hz"
                # 2) "37%"
                # 3) "370" (permille -> 37.0%)
                m = re.search(r"(\d+(?:\.\d+)?)\s*@", txt)
                if m:
                    val = float(m.group(1))
                    if 100.0 < val <= 1000.0:
                        val = val / 10.0
                    if 0.0 <= val <= 100.0:
                        return val

                m = re.search(r"(\d+(?:\.\d+)?)\s*%", txt)
                if m:
                    val = float(m.group(1))
                    if 0.0 <= val <= 100.0:
                        return val

                m = re.fullmatch(r"\s*(\d+(?:\.\d+)?)\s*", txt)
                if m:
                    val = float(m.group(1))
                    if 100.0 < val <= 1000.0:
                        val = val / 10.0
                    if 0.0 <= val <= 100.0:
                        return val
            except Exception:
                continue
        return -1.0

    def system_stats_worker(self):
        prev_total, prev_idle = None, None
        while True:
            try:
                total, idle = self._read_cpu_times()
                cpu_use = self.system_stats["cpu"]
                if total is not None and idle is not None and prev_total is not None and prev_idle is not None:
                    dt = total - prev_total
                    di = idle - prev_idle
                    if dt > 0:
                        cpu_use = (dt - di) * 100.0 / dt
                prev_total, prev_idle = total, idle

                mem_use = self._read_mem_usage()
                disk_use = self._read_disk_usage()
                engine_running = self.aimbot_process is not None and self.aimbot_process.poll() is None
                npu_use = self._read_npu_usage() if engine_running else 0.0

                self.system_stats["cpu"] = round(max(0.0, min(100.0, cpu_use)), 1)
                self.system_stats["mem"] = round(max(0.0, min(100.0, mem_use)), 1)
                self.system_stats["disk"] = round(max(0.0, min(100.0, disk_use)), 1)
                self.system_stats["npu"] = round(npu_use, 1) if npu_use >= 0 else -1.0
            except Exception:
                pass
            time.sleep(1.0)

    def get_state_dict(self):
        self._refresh_engine_state()
        self._refresh_streamer_state()
        models = self.build_model_list()
        model_map = {m["id"]: m for m in models}
        current = model_map.get(self.runtime_cfg["selected_model_id"])
        return {
            "engine_running": self.aimbot_process is not None and self.aimbot_process.poll() is None,
            "engine_error": self.engine_last_error,
            "engine_exit_code": self.engine_last_exit_code,
            "telemetry": self.telemetry_data,
            "system": self.system_stats,
            "streamer": self.get_streamer_state(),
            "params": self.params,
            "runtime": self.runtime_cfg,
            "models": [{"id": m["id"], "name": m["name"]} for m in models],
            "profiles": self.list_profiles(),
            "current_model_name": current["name"] if current else "未找到模型",
        }

    def start_background_threads(self):
        if self._threads_started:
            return
        self._threads_started = True
        threading.Thread(target=self.telemetry_listener, daemon=True).start()
        threading.Thread(target=self.system_stats_worker, daemon=True).start()

    def bootstrap(self):
        self.load_state_files()
        self.save_params()
        self.save_runtime()
        self.start_background_threads()
