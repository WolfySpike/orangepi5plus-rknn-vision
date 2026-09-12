import os

from webui_backend import create_app


BASE_DIR = os.path.dirname(os.path.abspath(__file__))
app, ctx = create_app(BASE_DIR)


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000)
