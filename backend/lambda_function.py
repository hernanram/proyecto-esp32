"""
AWS Lambda Function — ESP32 UNSAAC Monitor
Python 3.12

Endpoints (via API Gateway proxy integration):
  GET  /data        → lee ítem DynamoDB, retorna estado actual
  POST /adjust      → actualiza manual_offset atómicamente y publica MQTT
  POST /iot-ingest  → recibe JSON del ESP32 (desde IoT Rule) y almacena en DynamoDB
  OPTIONS *         → preflight CORS
"""

import json
import boto3
import os
from decimal import Decimal

# ── Configuración ──────────────────────────────────────────────────────────────
TABLE_NAME   = os.environ.get("DYNAMO_TABLE", "ESP32Scan")
IOT_ENDPOINT = os.environ.get("IOT_ENDPOINT", "TU_ENDPOINT-ats.iot.us-east-1.amazonaws.com")
DEVICE_ID    = "ESP32-UNSAAC-Sniffer"
MQTT_TOPIC   = "unsaac/esp32/adjust"

# Clientes (se reutilizan entre invocaciones en el mismo contenedor)
dynamodb = boto3.resource("dynamodb")
iot_data = boto3.client("iot-data", endpoint_url=f"https://{IOT_ENDPOINT}")
table    = dynamodb.Table(TABLE_NAME)

# ── Headers CORS ───────────────────────────────────────────────────────────────
CORS_HEADERS = {
    "Content-Type": "application/json",
    "Access-Control-Allow-Origin": "*",
    "Access-Control-Allow-Methods": "GET,POST,OPTIONS",
    "Access-Control-Allow-Headers": "Content-Type",
}

# ── Helpers ────────────────────────────────────────────────────────────────────

def _response(status_code: int, body: dict) -> dict:
    """Construye una respuesta HTTP compatible con API Gateway proxy."""
    return {
        "statusCode": status_code,
        "headers": CORS_HEADERS,
        "body": json.dumps(body, default=_decimal_serializer),
    }


def _decimal_serializer(obj):
    """json.dumps no sabe serializar Decimal; lo convertimos a int o float."""
    if isinstance(obj, Decimal):
        return int(obj) if obj == obj.to_integral_value() else float(obj)
    raise TypeError(f"Object of type {type(obj)} is not JSON serializable")


def _default_item() -> dict:
    """Estructura vacía/default devuelta cuando el ítem no existe aún."""
    return {
        "device_id": DEVICE_ID,
        "timestamp": 0,
        "poi": 0,
        "wifi": {"count": 0, "devices": []},
        "ble":  {"count": 0, "devices": []},
        "people": {
            "auto_estimate": 0,
            "manual_offset": 0,
            "total": 0,
        },
        "free_heap": 0,
        "phase": "UNKNOWN",
        "channel": 0,
    }


# ── Handler principal ──────────────────────────────────────────────────────────

def lambda_handler(event, context):
    # ── Ingesta directa desde Regla IoT Core (MQTT) ───────────────────────────
    if "device_id" in event or "httpMethod" not in event:
        print("[INFO] Ingesta directa desde AWS IoT Core Rule")
        return _handle_iot_ingest(event)

    http_method  = event.get("httpMethod", "")
    path         = event.get("path", "")
    body_raw     = event.get("body") or "{}"

    # ── CORS preflight ─────────────────────────────────────────────────────────
    if http_method == "OPTIONS":
        return _response(200, {"message": "OK"})

    # ── GET /data ──────────────────────────────────────────────────────────────
    if http_method == "GET" and path.rstrip("/") == "/data":
        return _handle_get_data()

    # ── POST /adjust ───────────────────────────────────────────────────────────
    if http_method == "POST" and path.rstrip("/") == "/adjust":
        try:
            body = json.loads(body_raw)
        except json.JSONDecodeError:
            return _response(400, {"error": "Body JSON inválido"})
        return _handle_post_adjust(body)

    # ── POST /iot-ingest ───────────────────────────────────────────────────────
    if http_method == "POST" and path.rstrip("/") == "/iot-ingest":
        try:
            body = json.loads(body_raw)
        except json.JSONDecodeError:
            return _response(400, {"error": "Body JSON inválido"})
        return _handle_iot_ingest(body)

    # ── Ruta no encontrada ─────────────────────────────────────────────────────
    return _response(404, {"error": f"Ruta no encontrada: {http_method} {path}"})


# ── Handlers por endpoint ──────────────────────────────────────────────────────

def _handle_get_data() -> dict:
    """
    GET /data
    Lee el ítem del device en DynamoDB y lo retorna.
    Si no existe, retorna estructura default.
    """
    try:
        resp = table.get_item(Key={"device_id": DEVICE_ID})
        item = resp.get("Item")
        if not item:
            return _response(200, _default_item())
        return _response(200, item)
    except Exception as e:
        print(f"[ERROR] _handle_get_data: {e}")
        return _response(500, {"error": "Error al leer DynamoDB", "detail": str(e)})


def _handle_post_adjust(body: dict) -> dict:
    """
    POST /adjust  — body: {"delta": 1} o {"delta": -1}

    Actualiza manual_offset de forma atómica (ADD) en DynamoDB,
    luego publica el nuevo valor vía MQTT.
    """
    delta = body.get("delta")
    if delta not in (1, -1):
        return _response(400, {"error": "delta debe ser 1 o -1"})

    try:
        # ADD es atómico sobre el ajuste persistido en el nivel superior.
        resp = table.update_item(
            Key={"device_id": DEVICE_ID},
            UpdateExpression="ADD manual_offset :delta",
            ExpressionAttributeValues={
                ":delta": Decimal(delta),
            },
            ReturnValues="ALL_NEW",
        )
        updated = resp.get("Attributes", {})
        new_offset = int(updated.get("manual_offset", 0))

        # Mantener también el objeto people actualizado para los dashboards
        # que lean el valor dentro de esa estructura.
        people = updated.get("people")
        if not isinstance(people, dict):
            people = {}
        auto_estimate = int(people.get("auto_estimate", 0))
        table.update_item(
            Key={"device_id": DEVICE_ID},
            UpdateExpression="SET people.manual_offset = :offset, people.total = :total",
            ExpressionAttributeValues={
                ":offset": Decimal(new_offset),
                ":total": Decimal(max(0, auto_estimate + new_offset)),
            },
        )

        # Publicar MQTT
        mqtt_payload = json.dumps({
            "action":     "adjust",
            "delta":      delta,
            "new_offset": new_offset,
        })
        iot_data.publish(
            topic=MQTT_TOPIC,
            qos=1,
            payload=mqtt_payload.encode("utf-8"),
        )

        return _response(200, {
            "message":    "Ajuste aplicado",
            "delta":      delta,
            "new_offset": new_offset,
            "device":     DEVICE_ID,
        })

    except Exception as e:
        print(f"[ERROR] _handle_post_adjust: {e}")
        return _response(500, {"error": "Error al ajustar offset", "detail": str(e)})


def _handle_iot_ingest(body: dict) -> dict:
    """
    POST /iot-ingest
    Recibe el JSON que el ESP32 publica en 'unsaac/esp32/scan'
    (enviado por una Regla de IoT Core) y actualiza el ítem en DynamoDB.

    El cuerpo puede llegar tal como viene de la regla IoT
    o envuelto por API Gateway; se acepta ambos.
    """
    try:
        # Asegurar que haya device_id en el ítem que vamos a guardar
        body["device_id"] = DEVICE_ID

        # Conserva el ajuste manual aunque el ESP32 publique una
        # instantánea nueva que no contiene manual_offset en la raíz.
        existing = table.get_item(
            Key={"device_id": DEVICE_ID},
            ProjectionExpression="manual_offset",
        ).get("Item", {})
        saved_offset = int(existing.get("manual_offset", 0) or 0)
        body["manual_offset"] = saved_offset

        people = body.get("people")
        if not isinstance(people, dict):
            people = {}
        auto_estimate = int(people.get("auto_estimate", 0) or 0)
        people["manual_offset"] = saved_offset
        people["total"] = max(0, auto_estimate + saved_offset)
        body["people"] = people

        # Convertir todos los números a Decimal para DynamoDB
        item = _convert_floats(body)

        table.put_item(Item=item)

        return _response(200, {"message": "Datos almacenados", "device": DEVICE_ID})

    except Exception as e:
        print(f"[ERROR] _handle_iot_ingest: {e}")
        return _response(500, {"error": "Error al guardar en DynamoDB", "detail": str(e)})


def _convert_floats(obj):
    """
    Recorre recursivamente un objeto Python y convierte float → Decimal
    para que DynamoDB lo acepte.
    """
    if isinstance(obj, float):
        return Decimal(str(obj))
    if isinstance(obj, int):
        return Decimal(obj)
    if isinstance(obj, dict):
        return {k: _convert_floats(v) for k, v in obj.items()}
    if isinstance(obj, list):
        return [_convert_floats(i) for i in obj]
    return obj
