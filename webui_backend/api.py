import logging
import os
import re
import time

from flask import Flask, jsonify, render_template, request

from .context import BackendContext


def create_app(base_dir=None):
    app_base = base_dir or os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    ctx = BackendContext(app_base)
    ctx.bootstrap()

    app = Flask(
        __name__,
        template_folder=os.path.join(app_base, "templates"),
        static_folder=os.path.join(app_base, "static"),
    )

    log = logging.getLogger("werkzeug")
    log.setLevel(logging.ERROR)

    @app.route("/")
    def index():
        return render_template("index.html")

    @app.route("/api/state")
    def api_state():
        with ctx.state_lock:
            ctx.send_params_to_cpp()
            return jsonify(ctx.get_state_dict())

    @app.route("/api/update", methods=["POST"])
    def api_update():
        payload = request.json or {}
        with ctx.state_lock:
            for k, v in payload.items():
                if k in ctx.DEPRECATED_PARAMS:
                    continue
                try:
                    ctx.params[k] = float(v)
                except Exception:
                    continue
            ctx.save_params()
            ctx.send_params_to_cpp()
        return jsonify({"ok": True})

    @app.route("/api/runtime", methods=["POST"])
    def api_runtime():
        payload = request.json or {}
        with ctx.state_lock:
            try:
                prev_model = ctx.runtime_cfg["selected_model_id"]
                prev_family = ctx.runtime_cfg["model_family"]
                prev_classes = int(ctx.runtime_cfg["num_classes"])
                prev_npu_mode = ctx.runtime_cfg.get("npu_mode", "latency")

                model_id = str(payload.get("selected_model_id", ctx.runtime_cfg["selected_model_id"]))
                family = str(payload.get("model_family", ctx.runtime_cfg["model_family"]))
                class_count = int(payload.get("num_classes", ctx.runtime_cfg["num_classes"]))
                npu_mode = str(payload.get("npu_mode", ctx.runtime_cfg.get("npu_mode", "latency")))
                class_count = max(1, min(100, class_count))

                model_ids = {m["id"] for m in ctx.build_model_list()}
                if model_id in model_ids:
                    ctx.runtime_cfg["selected_model_id"] = model_id
                if family in ctx.MODEL_FAMILY_MAP:
                    ctx.runtime_cfg["model_family"] = family
                if npu_mode in ("latency", "throughput"):
                    ctx.runtime_cfg["npu_mode"] = npu_mode
                ctx.runtime_cfg["num_classes"] = class_count

                ctx.ensure_class_params(class_count)
                ctx.ensure_class_names(class_count)
                ctx.save_runtime()
                ctx.save_params()
                ctx.send_params_to_cpp()

                if (
                    ctx.runtime_cfg["selected_model_id"] != prev_model
                    or ctx.runtime_cfg["model_family"] != prev_family
                    or int(ctx.runtime_cfg["num_classes"]) != prev_classes
                    or ctx.runtime_cfg.get("npu_mode", "latency") != prev_npu_mode
                ):
                    ctx.restart_engine_if_running()

                return jsonify({"ok": True, "state": ctx.get_state_dict()})
            except Exception as e:
                return jsonify({"ok": False, "msg": str(e)})

    @app.route("/api/class_name", methods=["POST"])
    def api_class_name():
        payload = request.json or {}
        with ctx.state_lock:
            try:
                idx = int(payload.get("index", -1))
                name = str(payload.get("name", "")).strip()[:32]
                if idx < 0 or idx >= int(ctx.runtime_cfg["num_classes"]):
                    return jsonify({"ok": False, "msg": "index out of range"})
                ctx.ensure_class_names(ctx.runtime_cfg["num_classes"])
                ctx.runtime_cfg["class_names"][idx] = name if name else ctx.default_class_name(idx)
                ctx.save_runtime()
                return jsonify({"ok": True})
            except Exception as e:
                return jsonify({"ok": False, "msg": str(e)})

    @app.route("/api/crosshair_preset/select", methods=["POST"])
    def api_crosshair_preset_select():
        payload = request.json or {}
        with ctx.state_lock:
            try:
                name = str(payload.get("name", "")).strip()
                ctx.ensure_xhair_presets()
                preset = next(
                    (p for p in ctx.runtime_cfg["xhair_presets"] if p["name"].lower() == name.lower()),
                    None,
                )
                if not preset:
                    return jsonify({"ok": False, "msg": "preset not found"})
                ctx.apply_xhair_preset_to_params(preset)
                ctx.save_runtime()
                ctx.save_params()
                ctx.send_params_to_cpp()
                return jsonify({"ok": True, "state": ctx.get_state_dict()})
            except Exception as e:
                return jsonify({"ok": False, "msg": str(e)})

    @app.route("/api/crosshair_preset/save", methods=["POST"])
    def api_crosshair_preset_save():
        payload = request.json or {}
        with ctx.state_lock:
            try:
                name = str(payload.get("name", "")).strip()[:32]
                if not name:
                    return jsonify({"ok": False, "msg": "preset name is empty"})
                preset = ctx.current_xhair_preset_from_params(name)
                clean = ctx.sanitize_xhair_preset(preset)
                if not clean:
                    return jsonify({"ok": False, "msg": "invalid preset"})

                ctx.ensure_xhair_presets()
                presets = ctx.runtime_cfg["xhair_presets"]
                replaced = False
                for i, old in enumerate(presets):
                    if old["name"].lower() == clean["name"].lower():
                        presets[i] = clean
                        replaced = True
                        break
                if not replaced:
                    presets.append(clean)
                ctx.runtime_cfg["xhair_selected_preset"] = clean["name"]
                ctx.save_runtime()
                ctx.save_params()
                ctx.send_params_to_cpp()
                return jsonify({"ok": True, "state": ctx.get_state_dict()})
            except Exception as e:
                return jsonify({"ok": False, "msg": str(e)})

    @app.route("/api/crosshair_preset/delete", methods=["POST"])
    def api_crosshair_preset_delete():
        payload = request.json or {}
        with ctx.state_lock:
            try:
                name = str(payload.get("name", "")).strip()
                ctx.ensure_xhair_presets()
                presets = [p for p in ctx.runtime_cfg["xhair_presets"] if p["name"].lower() != name.lower()]
                if len(presets) == len(ctx.runtime_cfg["xhair_presets"]):
                    return jsonify({"ok": False, "msg": "preset not found"})
                if not presets:
                    presets = [dict(ctx.DEFAULT_RUNTIME["xhair_presets"][0])]
                ctx.runtime_cfg["xhair_presets"] = presets
                if ctx.runtime_cfg.get("xhair_selected_preset", "").lower() == name.lower():
                    ctx.apply_xhair_preset_to_params(presets[0])
                ctx.ensure_xhair_presets()
                ctx.save_runtime()
                ctx.save_params()
                ctx.send_params_to_cpp()
                return jsonify({"ok": True, "state": ctx.get_state_dict()})
            except Exception as e:
                return jsonify({"ok": False, "msg": str(e)})

    @app.route("/api/config/create", methods=["POST"])
    def api_config_create():
        payload = request.json or {}
        with ctx.state_lock:
            try:
                name = ctx.normalize_profile_name(payload.get("config_name", ""))
                if not name:
                    return jsonify({"ok": False, "msg": "配置名不能为空"})
                p = ctx.profile_path(name)
                if p is None:
                    return jsonify({"ok": False, "msg": "配置名非法"})
                if os.path.exists(p):
                    return jsonify({"ok": False, "msg": "配置已存在"})

                ctx.runtime_cfg["active_config"] = name
                ctx.save_runtime()
                ctx.save_params()
                ctx.send_params_to_cpp()
                return jsonify({"ok": True, "state": ctx.get_state_dict()})
            except Exception as e:
                return jsonify({"ok": False, "msg": str(e)})

    @app.route("/api/config/switch", methods=["POST"])
    def api_config_switch():
        payload = request.json or {}
        with ctx.state_lock:
            try:
                name = ctx.normalize_profile_name(payload.get("config_name", ""))
                if not name:
                    return jsonify({"ok": False, "msg": "配置名不能为空"})
                data = ctx._load_profile(name)
                if data is None:
                    return jsonify({"ok": False, "msg": "配置不存在"})

                prev_model = ctx.runtime_cfg["selected_model_id"]
                prev_family = ctx.runtime_cfg["model_family"]
                prev_classes = int(ctx.runtime_cfg["num_classes"])
                prev_npu_mode = ctx.runtime_cfg.get("npu_mode", "latency")

                ctx._apply_profile_data(data)
                ctx.runtime_cfg["active_config"] = name
                ctx.save_runtime()
                ctx.save_params()
                ctx.send_params_to_cpp()

                if (
                    ctx.runtime_cfg["selected_model_id"] != prev_model
                    or ctx.runtime_cfg["model_family"] != prev_family
                    or int(ctx.runtime_cfg["num_classes"]) != prev_classes
                    or ctx.runtime_cfg.get("npu_mode", "latency") != prev_npu_mode
                ):
                    ctx.restart_engine_if_running()

                return jsonify({"ok": True, "state": ctx.get_state_dict()})
            except Exception as e:
                return jsonify({"ok": False, "msg": str(e)})

    @app.route("/api/config/save", methods=["POST"])
    def api_config_save():
        with ctx.state_lock:
            try:
                ctx.save_runtime()
                ctx.save_params()
                ctx.send_params_to_cpp()
                return jsonify({"ok": True, "state": ctx.get_state_dict()})
            except Exception as e:
                return jsonify({"ok": False, "msg": str(e)})

    @app.route("/api/upload_model", methods=["POST"])
    def api_upload_model():
        if "model_file" not in request.files:
            return jsonify({"ok": False, "msg": "缺少文件字段 model_file"})
        f = request.files["model_file"]
        if not f or not f.filename:
            return jsonify({"ok": False, "msg": "未选择文件"})

        filename = os.path.basename(f.filename)
        if not filename.lower().endswith(".rknn"):
            return jsonify({"ok": False, "msg": "仅支持 .rknn 文件"})

        safe_name = re.sub(r"[^A-Za-z0-9._-]", "_", filename)
        save_path = os.path.join(ctx.model_dir, safe_name)
        if os.path.exists(save_path):
            base, ext = os.path.splitext(safe_name)
            safe_name = f"{base}_{int(time.time())}{ext}"
            save_path = os.path.join(ctx.model_dir, safe_name)
        try:
            f.save(save_path)
            with ctx.state_lock:
                ctx.runtime_cfg["selected_model_id"] = f"models:{safe_name}"
                ctx.save_runtime()
                ctx.restart_engine_if_running()
            return jsonify({"ok": True, "filename": safe_name})
        except Exception as e:
            return jsonify({"ok": False, "msg": str(e)})

    @app.route("/api/toggle", methods=["POST"])
    def api_toggle():
        with ctx.state_lock:
            try:
                if ctx.aimbot_process is None or ctx.aimbot_process.poll() is not None:
                    ok, msg = ctx.start_engine()
                    return jsonify({"ok": ok, "msg": msg})
                ctx.stop_engine()
                return jsonify({"ok": True, "msg": "stopped"})
            except Exception as e:
                return jsonify({"ok": False, "msg": str(e)})

    @app.route("/api/status")
    def api_status():
        with ctx.state_lock:
            ctx._refresh_engine_state()
            return jsonify(
                {
                    "engine_running": ctx.aimbot_process is not None and ctx.aimbot_process.poll() is None,
                    "engine_error": ctx.engine_last_error,
                    "engine_exit_code": ctx.engine_last_exit_code,
                    "telemetry": ctx.telemetry_data,
                }
            )

    @app.route("/api/logs")
    def api_logs():
        try:
            tail = int(request.args.get("tail", 200))
        except Exception:
            tail = 200
        tail = max(20, min(2000, tail))
        text = ctx._tail_text(ctx.engine_log_file, max_lines=tail)
        return jsonify(
            {
                "ok": True,
                "exists": os.path.exists(ctx.engine_log_file),
                "path": ctx.engine_log_file,
                "log": text,
            }
        )

    @app.route("/api/streamer/start", methods=["POST"])
    def api_streamer_start():
        payload = request.json or {}
        with ctx.state_lock:
            try:
                cfg = ctx.sanitize_streamer_config(payload)
                ok, msg = ctx.start_streamer(cfg)
                return jsonify({"ok": ok, "msg": msg, "streamer": ctx.get_streamer_state()})
            except Exception as e:
                return jsonify({"ok": False, "msg": str(e), "streamer": ctx.get_streamer_state()})

    @app.route("/api/streamer/config", methods=["POST"])
    def api_streamer_config():
        payload = request.json or {}
        with ctx.state_lock:
            try:
                cfg = ctx.sanitize_streamer_config(payload)
                ctx.runtime_cfg["streamer"] = cfg
                ctx.save_runtime()
                return jsonify({"ok": True, "streamer": ctx.get_streamer_state()})
            except Exception as e:
                return jsonify({"ok": False, "msg": str(e), "streamer": ctx.get_streamer_state()})

    @app.route("/api/streamer/stop", methods=["POST"])
    def api_streamer_stop():
        with ctx.state_lock:
            try:
                ctx.stop_streamer()
                return jsonify({"ok": True, "streamer": ctx.get_streamer_state()})
            except Exception as e:
                return jsonify({"ok": False, "msg": str(e), "streamer": ctx.get_streamer_state()})

    @app.route("/api/streamer/status")
    def api_streamer_status():
        with ctx.state_lock:
            return jsonify({"ok": True, "streamer": ctx.get_streamer_state()})

    @app.route("/api/streamer/logs")
    def api_streamer_logs():
        try:
            tail = int(request.args.get("tail", 120))
        except Exception:
            tail = 120
        tail = max(20, min(1000, tail))
        text = ctx._tail_text(ctx.streamer_log_file, max_lines=tail)
        return jsonify(
            {
                "ok": True,
                "exists": os.path.exists(ctx.streamer_log_file),
                "path": ctx.streamer_log_file,
                "log": text,
            }
        )

    @app.route("/api/system/reboot", methods=["POST"])
    def api_system_reboot():
        ok, msg = ctx.run_system_command("reboot")
        return jsonify({"ok": ok, "msg": msg})

    @app.route("/api/system/shutdown", methods=["POST"])
    def api_system_shutdown():
        ok, msg = ctx.run_system_command("shutdown")
        return jsonify({"ok": ok, "msg": msg})

    @app.route("/api/system/edid", methods=["POST"])
    def api_system_edid():
        ok, msg = ctx.run_system_command("edid")
        return jsonify({"ok": ok, "msg": msg})

    return app, ctx
