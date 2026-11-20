# Sistema integrado de detección y aforo UNSAAC

Proyecto académico basado en un ESP32 que combina:

- detección pasiva de dispositivos Wi-Fi;
- escaneo Bluetooth Low Energy;
- estimación aproximada de distancia RSSI;
- conteo aproximado de personas;
- ajuste manual remoto;
- MQTT sobre TLS con AWS IoT Core;
- almacenamiento en DynamoDB;
- API REST con API Gateway;
- dashboard web futurista.

## Archivos principales

| Ruta | Contenido |
|---|---|
| `firmware/proyecto_esp32_v3.ino` | Firmware Wi-Fi, BLE, Kalman, MQTT y conteo |
| `backend/lambda_function.py` | Ingesta IoT, DynamoDB y ajuste MQTT |
| `dashboard/index.html` | Dashboard, radar y tablas |
| `docs/GUIA_AWS.md` | Configuración AWS |
| `informes/Informe_I_Deteccion_Dispositivos_Electronicos.pdf` | Informe de detección |
| `informes/Informe_I_Deteccion_Dispositivos_Electronicos.tex` | Fuente LaTeX del informe I |
| `informes/Informe_II_Conteo_Aforo_Personas.pdf` | Informe de conteo y aforo |
| `informes/Informe_II_Conteo_Aforo_Personas.tex` | Fuente LaTeX del informe II |

## Arquitectura

```text
ESP32
  | Wi-Fi promiscuo + BLE
  v
AWS IoT Core (MQTT/TLS)
  |
  v
IoT Rule
  |
  v
Lambda
  |
  v
DynamoDB
  |
  v
API Gateway
  |
  v
Dashboard web
```

## Configuración antes de ejecutar

En el firmware se deben reemplazar:

- `TU_WIFI_SSID`;
- `TU_WIFI_PASSWORD`;
- `TU_ENDPOINT-ats.iot.REGION.amazonaws.com`;
- `PEGA_AQUI_AMAZON_ROOT_CA`;
- `PEGA_AQUI_CERTIFICADO_DEL_DISPOSITIVO`;
- `PEGA_AQUI_CLAVE_PRIVADA_DEL_DISPOSITIVO`.

No subir certificados reales, claves privadas ni contraseñas.

En Lambda se deben configurar:

- `DYNAMO_TABLE`;
- `IOT_ENDPOINT`.

En el dashboard se debe configurar `API_BASE` con la URL de API Gateway.

## Tópicos MQTT

- Publicación: `unsaac/esp32/scan`
- Ajuste manual: `unsaac/esp32/adjust`

## Ejecución del firmware

Librerías Arduino requeridas:

- PubSubClient;
- ArduinoJson v6.

Esquema recomendado:

- partición `Huge APP (3 MB No OTA/1 MB SPIFFS)` o equivalente.

## Ejecución local del dashboard

```powershell
cd dashboard
python -m http.server 8080
```

Abrir `http://localhost:8080`.

## Limitaciones

- RSSI no proporciona una distancia exacta.
- Las MAC aleatorias pueden fragmentar la identificación.
- OUI no garantiza modelo exacto.
- La relación de 1.5 dispositivos por persona es una referencia inicial.
- El conteo debe presentarse como aforo estimado.

## Licencia y privacidad

Usar únicamente en entornos autorizados. Informar a las personas presentes y no publicar MAC completas, ubicaciones personales ni datos sensibles.
