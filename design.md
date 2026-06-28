# System Architecture Design Document: MobTurret

This document details the system architecture, component communication, data models, and network protocols for **MobTurret** — an intelligent, dual-core serverless IoT Nerf Turret system.

---

## 1. System Architecture Overview

The MobTurret system is composed of three primary pillars:
1. **Edge Robot (FRDM-iMX93):** An asymmetric dual-core SoC running Linux on Cortex-A55 for heavy computing (Computer Vision) and Zephyr RTOS on Cortex-M33 for real-time motor and actuator control.
2. **Cloud Infrastructure (AWS Serverless & IoT Core):** A fully managed backend utilizing AWS IoT Core for control signaling, API Gateway/Lambda for business logic, and DynamoDB for persistent storage.
3. **Mobile Client (Flutter App):** A cross-platform mobile application for manual/autonomous operation, video streaming, and system monitoring.

### 1.1 High-Level Architecture Diagram
```
+-----------------------------------------------------------------------------------+
|                                   MOBILE CLIENT                                   |
|                                   (Flutter App)                                   |
+--------------------------+-----------------------+---------------------+----------+
                           |                       |                     |
              HTTPS / JWT  |                       | RTSP / TCP          | MQTT
                           v                       v                     v
+--------------------------+----+  +---------------+---------------+  +--+----------+
|       AWS API GATEWAY        |  |        VIDEO STREAM           |  |  AWS IoT     |
+--------------------------+----+  |  (Direct WebRTC/RTSP Bridge)  |  |   CORE       |
                           |       +---------------+---------------+  +--+----------+
                    Lambda |                       ^                     ^
                           v                       |                     |
+--------------------------+----+                  |                     |
|         DYNAMODB         |                  |                     |
+-------------------------------+                  |                     |
                                                   |                     | MQTT / TLS
                                                   |                     | (Cert-based)
+--------------------------------------------------+---------------------+----------+
|                                    EDGE ROBOT                                     |
|                                  (FRDM-iMX93)                                     |
+-----------------------------------------------------------------------------------+
```

---

## 2. Component Specifications & Interfaces

### 2.1 Edge Robot Architecture (FRDM-iMX93)
The NXP i.MX93 utilizes heterogeneous asymmetric multiprocessing (AMP). The task separation guarantees safety and split-second responsiveness for hardware control while keeping heavy AI threads isolated.

#### 2.1.1 Cortex-A55 Core (Linux OS)
* **Responsibilities:**
  * Ingest RGB-D USB Camera stream.
  * Run **YOLOv8** object detection model targeting human detection.
  * Extrapolate bounding box coordinates and cross-reference with the **Depth Map** array to compute precise 3D spatial vectors ($x, y, z$) relative to the turret muzzle.
  * Handle network communication upstream to AWS IoT Core (MQTT) and downstream to the Flutter Client via direct TCP Sockets (Manual mode).
  * Stream live video over RTSP protocol.
* **AI Engine & Frame Rate:** YOLOv8-nano/small optimized via TensorRT/NPU toolkit targeting a deterministic **30 FPS**.

#### 2.1.2 Cortex-M33 Core (Zephyr RTOS)
* **Responsibilities:**
  * Consume real-time kinematics and angular target data.
  * Control **SC15 Serial Bus Servos** via `UART3` interface.
  * Control peripheral firing mechanisms (Relay/MOSFET switching for Nerf flywheel acceleration and pusher actuation).
  * Maintain strict deterministic real-time loops for safe hardware acceleration limits.

#### 2.1.3 Inter-Core Communication (IPC)
* **Protocol:** **RPMsg (Remote Processor Messaging)** over OpenAMP framework.
* **Mechanism:** Shared memory ring buffers with hardware mailbox interrupts.
  * *A55 to M33 Payload:* Target pitch angle, target yaw angle, fire command bit, system operating state (Auto/Manual).
  * *M33 to A55 Payload:* Current servo position feedback, thermal telemetry, low-voltage battery status flag.

```
+-----------------------+                     +-----------------------+
|  Cortex-A55 (Linux)   |    Shared Memory    |  Cortex-M33 (Zephyr)  |
|  - YOLOv8 Inference   |======[ RPMsg ]=====>|  - SC15 Servo Drivers |
|  - Depth Vector Calc  |<===== Buffer ======='  - Trigger Actuator   |
+-----------------------+                     +-----------------------+
```

---

## 3. Communication Protocols & Networking

### 3.1 MQTT Over TLS (Control & Telemetry)
* **Broker:** AWS IoT Core.
* **Authentication:**
  * **Robot:** Mutual TLS (mTLS) with unique X.509 Device Certificates burned securely into the edge hardware or managed via hardware security modules if applicable.
  * **Flutter Client:** Token-based WebSocket connection using Custom JWT generated via AWS API Gateway upon user login.

#### 3.1.4 MQTT Topic Hierarchy
| Topic Pattern | Publisher | Subscriber | Purpose |
| :--- | :--- | :--- | :--- |
| `turret/{robot_id}/telemetry` | Robot (A55) | AWS IoT / Flutter | Periodic battery, state, and temperature health reports. |
| `turret/{robot_id}/evt/alert` | Robot (A55) | AWS IoT Core | Fired when a human is detected in Auto Mode. Triggers notification pipeline. |
| `turret/{robot_id}/cmd/mode` | Flutter App | Robot (A55) | Switches operational mode between `AUTO` and `MANUAL`. |
| `turret/{robot_id}/config` | Flutter App | Robot (A55) | Updates dynamic configurations (e.g., enable/disable Short Video Capture upon alert). |

### 3.2 Low-Latency Manual Mode Connection
* **Protocol:** Direct **TCP/IP Socket / WebSockets** connection.
* **Architecture Justification:** Bypassing cloud infrastructures during Manual Mode ensures ultra-low latency (< 50ms local networks) for reactive pan, tilt, and fire commands compared to the overhead of cloud roundtrips.
* **Fallback Strategy:** If clients are outside the local network subnet, a peer-to-peer connection or STUN/TURN traversal is initialized.

### 3.3 Video Streaming Pipeline
* **Protocol:** **RTSP (Real-Time Streaming Protocol)** over RTP/UDP.
* **Video Encoding:** H.264/H.265 compressed stream generated via hardware-accelerated VPU wrappers on the A55 core to keep CPU overhead negligible.

---

## 4. Cloud Infrastructure & Serverless Backend

### 4.1 Cloud Infrastructure Diagram
```
                     +-------------------+
                     |  AWS API Gateway  | <--- JWT Auth (Flutter App)
                     +---------+---------+
                               |
                               v
                     +-------------------+
                     |    AWS Lambda     |
                     +---------+---------+
                               |
            +------------------+------------------+
            |                                     |
            v                                     v
    +---------------+                     +---------------+
    | AWS DynamoDB  |                     |  AWS S3 Buckets|
    +---------------+                     +---------------+

 
                     +-------------------+
                     |  AWS IoT Core     | <--- mTLS (Robot Alert / Config)
                     +---------+---------+
                               |
                               v
                     +-------------------+
                     | AWS IoT Rule Engine|
                     +---------+---------+
                               |
                               v
                     +-------------------+
                     |    AWS Lambda     | (Process Notification Event)
                     +---------+---------+
                               |
                               v
                     +-------------------+
                     |      AWS SNS      |
                     +---------+---------+
                               |
                               v
                     +-------------------+
                     |   Firebase Cloud  | / APNs
                     |     Messaging     |
                     +-------------------+
```

### 4.2 Push Notification & Video Storage Workflow
1. **Detection Event:** A55 core identifies a human target via YOLOv8, extracts depth info, logs a positive match.
2. **Alert Dispatch:** Robot publishes a structured JSON payload to `turret/{robot_id}/evt/alert`.
3. **Cloud Routing:** AWS IoT Rule Engine parses the payload and invokes the Notification processing Lambda function.
4. **Conditional Video S3 Storage:** * The Lambda reads the target turret's configuration flag. 
   * If `video_capture_enabled == true`, the A55 core buffers the last 10 seconds of H.264 footage, pushes the `.mp4` file via an authenticated pre-signed URL to an **AWS S3 Bucket**, and writes the record path to DynamoDB under the user's asset list.
5. **Notification Push:** The processing Lambda dispatches an alert message to **AWS SNS**, which forwards the message payload to **Firebase Cloud Messaging (FCM)** for Android and **Apple Push Notification service (APNs)** for iOS devices.

---

## 5. Database Architecture (Amazon DynamoDB)

A single-table or optimized multi-table schema layout is used to maximize query throughput with serverless indexing patterns.

### 5.1 DynamoDB Tables

#### 5.1.1 `Users` Table
* **Primary Key:** `user_id` (String - UUID)
* **Attributes:** `email` (String), `created_at` (Timestamp), `jwt_issued_at` (Timestamp), `fcm_token` (String).

#### 5.1.2 `Turrets` Table
* **Primary Key:** `robot_id` (String - MAC Address / Hardware Unique ID)
* **Attributes:** `owner_id` (String - Reference to `user_id`), `status` (String: `ONLINE`, `OFFLINE`), `firmware_version` (String), `video_capture_enabled` (Boolean).

#### 5.1.3 `UsageLogs` Table
* **Partition Key (PK):** `robot_id` (String)
* **Sort Key (SK):** `session_start` (Timestamp)
* **Attributes:** `session_end` (Timestamp), `total_duration_sec` (Number), `shots_fired` (Number), `mode_used` (String: `AUTO`, `MANUAL`).

#### 5.1.4 `SavedMedia` Table
* **Partition Key (PK):** `user_id` (String)
* **Sort Key (SK):** `timestamp` (Timestamp)
* **Attributes:** `robot_id` (String), `s3_uri` (String), `media_type` (String: `SHORT_VIDEO`), `file_size_bytes` (Number).

---

## 6. Mobile Application Architecture (Flutter)

The mobile application acts as the command center for the physical turret system.

### 6.1 State Management & Architecture Pattern
* **Pattern:** BLoC (Business Logic Component) or Riverpod for predictable, immutable state transitions across async events.
* **Network Drivers:** * `dio` for structured RESTful JSON integration over the AWS API Gateway.
  * `mqtt_client` with WebSocket capabilities wrapping Secure WebSockets (`wss://`) for control channels.
  * `flutter_vlc_player` or native media pipelines for low-latency RTSP decoding.

### 6.2 Key UI Workflows & Views
* **Authentication View:** Username/password handling via JWT validation middleware. Holds token securely within device Keychain/Keystore.
* **Dashboard / Discovery:** Scans and lists registered robot endpoints assigned to the `owner_id`. Displays current real-time telemetry updates.
* **Control Center (Manual Mode):**
  * Spawns an RTSP live video frame overlay.
  * Captures dual-axis virtual joystick inputs translating positions directly to target vectors.
  * Pushes immediate packet outputs over the established direct TCP socket channel.
* **Media Gallery View:** Queries the `SavedMedia` endpoints via API Gateway to fetch and playback alert videos hosted on AWS S3 using authenticated temporal pre-signed download signatures.
