"""ASCOM Alpaca client (ITelescopeV4) — GET/PUT with ErrorNumber validation.

Three kinds of failure are separated:
  - TransportError: could not talk to the device (network/timeout).
  - DeviceError: the device answered with ErrorNumber != 0.
  - AssertionError: the case assertion failed.

Each response keeps the raw body and the standard fields (Value, ErrorNumber,
ErrorMessage, ClientTransactionID, ServerTransactionID) for evidence and
traceability.
"""
import json
import urllib.error
import urllib.parse
import urllib.request


class TransportError(RuntimeError):
    pass


class DeviceError(RuntimeError):
    def __init__(self, number, message, raw=""):
        super().__init__(f"Alpaca ErrorNumber={number}: {message}")
        self.number = number
        self.message = message
        self.raw = raw


class Response:
    __slots__ = ("raw", "value", "error_number", "error_message",
                 "client_transaction_id", "server_transaction_id", "http_status",
                 "headers")

    def __init__(self, raw, obj, http_status, headers=None):
        self.raw = raw
        self.http_status = http_status
        self.headers = headers or {}
        self.value = obj.get("Value")
        self.error_number = obj.get("ErrorNumber", 0)
        self.error_message = obj.get("ErrorMessage", "")
        self.client_transaction_id = obj.get("ClientTransactionID")
        self.server_transaction_id = obj.get("ServerTransactionID")

    def snapshot(self):
        """Parse the X-NRM-Snapshot header into {ra, dec, st, tr, g, ras, decs, t}.

        Returns None when the header is absent (production firmware)."""
        raw = self.headers.get("X-NRM-Snapshot")
        if not raw:
            return None
        snap = {}
        for part in raw.split(";"):
            key, _, value = part.partition("=")
            snap[key] = int(value) if key in ("ra", "dec", "st", "tr", "g", "t") else float(value)
        return snap

    @property
    def ok(self):
        return self.error_number == 0

    def __repr__(self):
        return (f"<AlpacaResponse http={self.http_status} err={self.error_number} "
                f"value={self.value!r} ctid={self.client_transaction_id}>")


class AlpacaClient:
    """Cliente de un dispositivo Telescope sobre el puerto Alpaca."""

    def __init__(self, base, device_number=0, timeout=8):
        self.base = base.rstrip("/")
        self.telescope = f"{self.base}/api/v1/telescope/{device_number}"
        self.timeout = timeout
        self._client_id = 1
        self._txn = 0

    # ── transaction ──────────────────────────────────────────────
    def next_txn(self):
        self._txn += 1
        return self._txn

    # ── HTTP primitivo ───────────────────────────────────────────
    def _request(self, method, path, params=None, form=None, timeout=None,
                 check_alpaca=True):
        url = path
        qs = urllib.parse.urlencode(params or {})
        if qs:
            url += ("&" if "?" in url else "?") + qs

        data = None
        headers = {}
        if method == "PUT" and form is not None:
            data = urllib.parse.urlencode(form).encode()
            headers["Content-Type"] = "application/x-www-form-urlencoded"

        req = urllib.request.Request(url, data=data, method=method, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=timeout or self.timeout) as r:
                body = r.read().decode()
                status = r.status
                resp_headers = dict(r.headers)
        except urllib.error.HTTPError as e:
            body = e.read().decode()
            status = e.code
            resp_headers = dict(e.headers)
        except Exception as e:
            raise TransportError(f"{method} {path}: {type(e).__name__}: {e}")

        # A 5xx is a server failure regardless of the body — never a success.
        if status >= 500:
            raise TransportError(f"{method} {path}: HTTP {status}: {body[:200]!r}")

        try:
            obj = json.loads(body)
        except Exception:
            raise TransportError(f"{method} {path}: respuesta no JSON: {body[:200]!r}")

        # Telescope responses must carry ErrorNumber; its absence is a malformed
        # (or crashed) reply, never a success.  Management endpoints are exempt.
        if check_alpaca and "ErrorNumber" not in obj:
            raise TransportError(f"{method} {path}: falta ErrorNumber: {body[:200]!r}")

        return Response(body, obj, status, resp_headers)

    def _std(self, params):
        """Fill ClientID/ClientTransactionID only if not already provided."""
        p = dict(params)
        p.setdefault("ClientID", self._client_id)
        p.setdefault("ClientTransactionID", self.next_txn())
        return p

    # ── GET ──────────────────────────────────────────────────────
    def get(self, name, **params):
        """GET de una propiedad; devuelve Response y no lanza salvo transporte."""
        path = f"{self.telescope}/{name}"
        return self._request("GET", path, params=self._std(params))

    def get_value(self, name, **params):
        """GET devolviendo el Value crudo (None si ErrorNumber != 0)."""
        r = self.get(name, **params)
        if r.error_number != 0:
            raise DeviceError(r.error_number, r.error_message, r.raw)
        return r.value

    # ── PUT (action or setter) ────────────────────────────────────
    def put(self, name, form=None, **params):
        path = f"{self.telescope}/{name}"
        return self._request("PUT", path, params=self._std(params), form=form or {})

    def put_ok(self, name, form=None, **params):
        """PUT that expects success (ErrorNumber == 0); raises DeviceError otherwise."""
        r = self.put(name, form=form, **params)
        if r.error_number != 0:
            raise DeviceError(r.error_number, r.error_message, r.raw)
        return r

    # ── management ───────────────────────────────────────────────
    def management(self, name):
        return self._request("GET", f"{self.base}/management/{name}",
                             check_alpaca=False)
