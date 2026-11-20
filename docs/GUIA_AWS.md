# 🚀 Guía de Configuración AWS — ESP32 UNSAAC Monitor

> Guía completa paso a paso para desplegar el backend en AWS desde cero.  
> Tiempo estimado: 30–45 minutos.

---

## Tabla de contenidos

1. [AWS IoT Core — Thing, Certificados y Policy](#1-aws-iot-core)
2. [DynamoDB — Tabla ESP32Scan](#2-dynamodb)
3. [IoT Rule — Reenviar datos a Lambda](#3-iot-rule)
4. [Lambda — Función Python 3.12](#4-lambda)
5. [API Gateway — REST API](#5-api-gateway)
6. [S3 — Hosting del Dashboard (opcional)](#6-s3-opcional)
7. [Tabla de Placeholders → Valores Reales](#7-placeholders)
8. [Librerías Arduino a Instalar](#8-librerias-arduino)
9. [Cómo Cargar Certificados en el ESP32](#9-certificados-esp32)

---

## 1. AWS IoT Core

### 1.1 Crear el Thing

1. Abre la consola AWS → **IoT Core** → **Manage → Things → Create things**.
2. Selecciona **Create single thing** → **Next**.
3. Nombre: `esp32-unsaac-01` → **Next**.
4. En *Device certificate*: elige **Auto-generate a new certificate** → **Next**.

### 1.2 Crear la Policy

Antes de adjuntar el certificado crea la política:

1. Ve a **IoT Core → Security → Policies → Create policy**.
2. Nombre: `ESP32UNSAACPolicy`.
3. En el editor JSON pega:

```json
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Action": "iot:Connect",
      "Resource": "arn:aws:iot:us-east-1:*:client/esp32-unsaac-01"
    },
    {
      "Effect": "Allow",
      "Action": "iot:Publish",
      "Resource": "arn:aws:iot:us-east-1:*:topic/unsaac/esp32/scan"
    },
    {
      "Effect": "Allow",
      "Action": "iot:Subscribe",
      "Resource": "arn:aws:iot:us-east-1:*:topicfilter/unsaac/esp32/adjust"
    },
    {
      "Effect": "Allow",
      "Action": "iot:Receive",
      "Resource": "arn:aws:iot:us-east-1:*:topic/unsaac/esp32/adjust"
    }
  ]
}
```

4. Haz clic en **Create**.

### 1.3 Adjuntar Policy al Certificado

1. Vuelve al flujo de creación del Thing.
2. Selecciona la policy `ESP32UNSAACPolicy` → **Create thing**.
3. **⚠ Descarga AHORA los siguientes archivos** (solo se ofrecen una vez):
   - `xxxxx-certificate.pem.crt` → renombra a `cert.pem`
   - `xxxxx-private.pem.key` → renombra a `private.pem`
   - `AmazonRootCA1.pem` → renombra a `ca.pem`

> [!WARNING]
> Si no descargas los certificados ahora, tendrás que revocar y crear nuevos.
> Guarda los 3 archivos en una carpeta segura en tu PC.

### 1.4 Obtener el Endpoint ATS

1. Ve a **IoT Core → Settings** (esquina inferior izquierda).
2. Copia el valor de **Device data endpoint**. Tiene esta forma:
   ```
   abcdefghijkl-ats.iot.us-east-1.amazonaws.com
   ```
3. Guárdalo; lo necesitarás para la Lambda y el ESP32.

---

## 2. DynamoDB

### 2.1 Crear la Tabla

1. Abre **DynamoDB → Tables → Create table**.
2. Configuración:

| Campo | Valor |
|---|---|
| Table name | `ESP32Scan` |
| Partition key | `device_id` (tipo **String**) |
| Sort key | *(dejar vacío)* |
| Table settings | **Customize settings** |
| Billing mode | **On-demand** (paga por uso, ideal para dev) |

3. Haz clic en **Create table**.

> [!TIP]
> Con el modo *On-demand* no pagas nada si no hay tráfico. Para producción con tráfico constante, considera *Provisioned* con Auto Scaling.

---

## 3. IoT Rule

La regla escucha el tópico MQTT `unsaac/esp32/scan` y reenvía los datos a Lambda.

1. Ve a **IoT Core → Message routing → Rules → Create rule**.
2. Nombre: `ESP32ScanToLambda`.
3. SQL:
   ```sql
   SELECT * FROM 'unsaac/esp32/scan'
   ```
4. En **Rule actions** → **Add action** → selecciona **Lambda** → **Send a message to a Lambda function**.
5. Elige la función `esp32-unsaac-lambda` (créala primero en el paso 4, luego vuelve aquí).
6. Haz clic en **Create rule**.

> [!NOTE]
> La regla IoT llamará a tu Lambda con el body del mensaje como JSON. El endpoint `/iot-ingest` en la Lambda lo procesa.

---

## 4. Lambda

### 4.1 Crear la Función

1. Ve a **Lambda → Functions → Create function**.
2. Configuración:

| Campo | Valor |
|---|---|
| Function name | `esp32-unsaac-lambda` |
| Runtime | **Python 3.12** |
| Architecture | x86_64 |
| Execution role | Create a new role with basic Lambda permissions |

3. Haz clic en **Create function**.

### 4.2 Pegar el Código

1. En la pestaña **Code**, borra el contenido de `lambda_function.py`.
2. Pega todo el contenido del archivo `lambda_function.py` que está en tu proyecto.
3. Haz clic en **Deploy**.

### 4.3 Variables de Entorno

1. Ve a la pestaña **Configuration → Environment variables → Edit**.
2. Agrega:

| Key | Value |
|---|---|
| `DYNAMO_TABLE` | `ESP32Scan` |
| `IOT_ENDPOINT` | `abcdefghijkl-ats.iot.us-east-1.amazonaws.com` *(tu endpoint real)* |

3. Haz clic en **Save**.

### 4.4 Política IAM para Lambda

La función necesita permisos para DynamoDB e IoT. Sigue estos pasos:

1. Ve a **Configuration → Permissions** → haz clic en el nombre del rol (ej. `esp32-unsaac-lambda-role-xxxx`).
2. Se abre **IAM → Roles**.
3. Haz clic en **Add permissions → Attach policies**.
4. Busca y adjunta:
   - `AmazonDynamoDBFullAccess`
   - `AWSIoTDataAccess`

> [!WARNING]
> `AmazonDynamoDBFullAccess` es amplia. Para producción, crea una política personalizada que solo permita `dynamodb:GetItem`, `dynamodb:PutItem` y `dynamodb:UpdateItem` sobre la tabla `ESP32Scan`.

### 4.5 Ajustar el Timeout

1. Ve a **Configuration → General configuration → Edit**.
2. Cambia **Timeout** a `10` segundos (el default de 3 s puede ser insuficiente).
3. Guarda.

---

## 5. API Gateway

### 5.1 Crear la API

1. Ve a **API Gateway → Create API**.
2. Selecciona **REST API** (no HTTP API, necesitamos más control de CORS) → **Build**.
3. Configuración:

| Campo | Valor |
|---|---|
| API name | `ESP32UNSAACApi` |
| Endpoint type | Regional |

4. Haz clic en **Create API**.

### 5.2 Crear los Recursos y Métodos

#### Recurso `/data` — GET

1. En el árbol de recursos, selecciona `/` → **Create resource**.
2. Resource name: `data` → **Create resource**.
3. Selecciona `/data` → **Create method** → **GET** → ✓.
4. Integration type: **Lambda Function**.
5. Activa **Lambda Proxy integration** ✓.
6. Lambda function: `esp32-unsaac-lambda` → **Save**.

#### Recurso `/adjust` — POST

1. Selecciona `/` → **Create resource**.
2. Resource name: `adjust` → **Create resource**.
3. Selecciona `/adjust` → **Create method** → **POST** → ✓.
4. Integration type: **Lambda Function**, **Lambda Proxy** ✓.
5. Lambda function: `esp32-unsaac-lambda` → **Save**.

#### Recurso `/iot-ingest` — POST

1. Selecciona `/` → **Create resource**.
2. Resource name: `iot-ingest` → **Create resource**.
3. Selecciona `/iot-ingest` → **Create method** → **POST** → ✓.
4. Integration type: **Lambda Function**, **Lambda Proxy** ✓.
5. Lambda function: `esp32-unsaac-lambda` → **Save**.

### 5.3 Habilitar CORS

Para cada recurso (`/data`, `/adjust`, `/iot-ingest`):

1. Selecciona el recurso → **Actions → Enable CORS**.
2. Deja los valores default (origin `*`) → **Enable CORS and replace existing CORS headers**.
3. Confirma.

> [!TIP]
> La Lambda ya envía los headers CORS en cada respuesta. Este paso agrega el método OPTIONS automáticamente en API Gateway, necesario para el preflight del browser.

### 5.4 Deploy

1. **Actions → Deploy API**.
2. Stage: **[New Stage]** → Stage name: `prod`.
3. Haz clic en **Deploy**.
4. Copia la **Invoke URL**:
   ```
   https://TU_API_ID.execute-api.us-east-1.amazonaws.com/prod
   ```

> [!IMPORTANT]
> Esta URL es la que debes pegar en `index.html` reemplazando `TU_API_ID`. Guárdala.

---

## 6. S3 (Opcional)

Sirve el `index.html` como sitio web estático accesible desde cualquier celular.

### 6.1 Crear el Bucket

1. Ve a **S3 → Create bucket**.
2. Nombre único global (ej. `esp32-unsaac-dashboard-2026`).
3. Región: `us-east-1`.
4. **Desactiva** "Block all public access" → confirma el aviso.
5. Haz clic en **Create bucket**.

### 6.2 Habilitar Static Website Hosting

1. Entra al bucket → **Properties** → scroll hasta **Static website hosting** → **Edit**.
2. Enable → Index document: `index.html`.
3. Guarda. Copia la **Bucket website endpoint** que aparece.

### 6.3 Bucket Policy (acceso público de lectura)

1. Ve a **Permissions → Bucket policy** → **Edit**.
2. Pega (reemplaza `NOMBRE_BUCKET`):

```json
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Sid": "PublicReadGetObject",
      "Effect": "Allow",
      "Principal": "*",
      "Action": "s3:GetObject",
      "Resource": "arn:aws:s3:::NOMBRE_BUCKET/*"
    }
  ]
}
```

3. Guarda.

### 6.4 Subir el Dashboard

1. Ve a **Objects → Upload**.
2. Sube `index.html`.
3. Abre la URL del sitio web estático desde tu celular.

---

## 7. Tabla de Placeholders

Usa esta tabla para saber exactamente qué reemplazar y dónde:

| Placeholder | Valor real | Dónde pegarlo |
|---|---|---|
| `TU_ENDPOINT-ats.iot.us-east-1.amazonaws.com` | Endpoint ATS de IoT Core (sección 1.4) | Variable de entorno Lambda `IOT_ENDPOINT`, y en el código del ESP32 |
| `TU_API_ID` | ID de tu API Gateway (sección 5.4) | `index.html` → constante `API_BASE` |
| `NOMBRE_BUCKET` | Nombre del bucket S3 (sección 6.1) | Bucket policy de S3 |
| `ESP32Scan` | *(ya es el default)* | Variable de entorno Lambda `DYNAMO_TABLE` |
| `us-east-1` | Región AWS elegida | Todo (si usas otra región, cámbiala en todos lados) |

> [!WARNING]
> Si cambias de región (ej. a `sa-east-1` para menor latencia desde Perú), debes actualizar la región en la policy de IoT, en los ARN de IAM, en el cliente boto3 y en el código del ESP32.

---

## 8. Librerías Arduino a Instalar

Abre **Arduino IDE → Sketch → Include Library → Manage Libraries** e instala:

| Librería | Autor | Versión | Para qué sirve |
|---|---|---|---|
| **PubSubClient** | Nick O'Leary | **2.8** | Comunicación MQTT con AWS IoT Core |
| **ArduinoJson** | Benoit Blanchon | **6.x** | Serializar/deserializar JSON (documentos del ESP32) |

> [!TIP]
> PubSubClient v2.8 es la versión estable más compatible con ESP32. No instales v3.x (aún en desarrollo). ArduinoJson v6 (no v5) usa la API moderna con `JsonDocument`.

También necesitarás (ya incluidas en el soporte ESP32 para Arduino IDE):
- `WiFi.h`
- `WiFiClientSecure.h`
- `esp_wifi.h` (para modo promiscuo)

---

## 9. Cómo Cargar Certificados en el ESP32

Los certificados se incrustan directamente en el código fuente como strings de C.

### 9.1 Formato

Abre cada archivo `.pem` con un editor de texto (Notepad, VSCode) y copia su contenido completo.

### 9.2 Pegar en el Código

En tu sketch de Arduino, define las constantes así:

```cpp
// ── Certificado del dispositivo (cert.pem) ─────────────────────────
static const char DEVICE_CERT[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIIDWTCCAkGgAwIBAgIUXXXXXXXXXXXXXXXXXXXXXXXXXXXwDQYJKoZIhvcNAQEL
... (tu contenido completo) ...
-----END CERTIFICATE-----
)EOF";

// ── Clave privada (private.pem) ────────────────────────────────────
static const char PRIVATE_KEY[] PROGMEM = R"EOF(
-----BEGIN RSA PRIVATE KEY-----
MIIEpAIBAAKCAQEAXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
... (tu contenido completo) ...
-----END RSA PRIVATE KEY-----
)EOF";

// ── Certificado CA raíz de Amazon (ca.pem) ─────────────────────────
static const char CA_CERT[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIIDQTCCAimgAwIBAgITBmyfz5m/jAo54vB4ikPmljZbyjANBgkqhkiG9w0BAQsF
... (tu contenido completo) ...
-----END CERTIFICATE-----
)EOF";
```

### 9.3 Configurar WiFiClientSecure

```cpp
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

const char* IOT_ENDPOINT = "abcdefghijkl-ats.iot.us-east-1.amazonaws.com";
const int   MQTT_PORT    = 8883;

WiFiClientSecure net;
PubSubClient     mqtt(net);

void setupMQTT() {
  net.setCACert(CA_CERT);
  net.setCertificate(DEVICE_CERT);
  net.setPrivateKey(PRIVATE_KEY);

  mqtt.setServer(IOT_ENDPOINT, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(2048); // importante para payloads grandes
}
```

> [!WARNING]
> Nunca subas los archivos `.pem` ni el sketch con claves privadas a un repositorio público (GitHub, etc.). Agrega `*.pem` y `*.key` a tu `.gitignore`.

> [!TIP]
> Si el ESP32 no conecta con `MQTT_PORT 8883`, verifica que el router/ISP no bloquee ese puerto. Alternativa: usa el puerto `443` con ALPN `x-amzn-mqtt-ca`, que muy pocos routers bloquean.

---

## Verificación Final

Una vez completados todos los pasos, prueba el flujo:

1. **ESP32 enciende** → conecta a WiFi → conecta MQTT → empieza a publicar en `unsaac/esp32/scan`.
2. **IoT Rule** captura el mensaje → llama a **Lambda** (`/iot-ingest`) → guarda en **DynamoDB**.
3. **Dashboard** (`index.html`) hace `GET /data` cada 10 s → muestra los datos actualizados.
4. **Botones +/-** del dashboard → `POST /adjust` → Lambda actualiza DynamoDB y publica en `unsaac/esp32/adjust` → ESP32 recibe el ajuste.

```
ESP32  ──publish──▶  IoT Core  ──rule──▶  Lambda  ──put──▶  DynamoDB
                                                               │
                                                            GET/data
                                                               │
Dashboard  ◀──────────────────────────────────────────────────┘
   │
   └──POST /adjust──▶  Lambda  ──ADD──▶  DynamoDB
                           │
                           └──publish──▶  IoT Core  ──▶  ESP32
```

---

*UNSAAC — Escuela Profesional de Ingeniería Informática y de Sistemas — IoT 2026*
