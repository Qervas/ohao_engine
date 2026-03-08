"""Engine bridge — TCP client to OHAO's GDScript OhaoServer (HTTP on port 9756)."""

import os
import httpx

DEFAULT_HOST = os.environ.get("OHAO_ENGINE_HOST", "127.0.0.1")
DEFAULT_PORT = int(os.environ.get("OHAO_ENGINE_PORT", "9756"))
TIMEOUT = 10.0


class EngineBridge:
    def __init__(self, host: str = DEFAULT_HOST, port: int = DEFAULT_PORT):
        self.base_url = f"http://{host}:{port}"

    def get(self, path: str) -> dict:
        """GET request to engine server. Returns parsed JSON."""
        with httpx.Client(timeout=TIMEOUT) as client:
            r = client.get(f"{self.base_url}{path}")
            r.raise_for_status()
            return r.json()

    def post(self, path: str, body: dict | None = None) -> dict:
        """POST request to engine server. Returns parsed JSON."""
        with httpx.Client(timeout=TIMEOUT) as client:
            r = client.post(f"{self.base_url}{path}", json=body or {})
            r.raise_for_status()
            return r.json()

    def delete(self, path: str) -> dict:
        """DELETE request to engine server."""
        with httpx.Client(timeout=TIMEOUT) as client:
            r = client.delete(f"{self.base_url}{path}")
            r.raise_for_status()
            return r.json()

    def capture_image_b64(self) -> tuple[str, int, int]:
        """Capture viewport screenshot. Returns (base64_png, width, height)."""
        data = self.post("/god/capture")
        return data["image_base64"], data["width"], data["height"]

    def is_connected(self) -> bool:
        """Check if engine is reachable."""
        try:
            self.get("/")
            return True
        except Exception:
            return False


# Module-level singleton
bridge = EngineBridge()
